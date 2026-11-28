/*
XGPU_VULKAN_DENOISE.C

The light-transport pass's one denoising and temporal stage (W3,
xgpu_vulkan_denoise.h; ~/dev/rt2-realtime-design.md section 1.5): a few rays
a pixel made a stable, correct picture in motion. A frame, after E1's 1-spp
sample (HALO_LT_E1=denoise, xgpu_vulkan_lt.c):

1. the re-shade (xgpu_vulkan_lt_reshade): E1's kernel at the previous frame's
   gradient pixels (one a 3x3 stratum, lt/gradient_pixel.glsl), with the
   previous frame's surfaces and seed but this frame's light and scene;
2. the gradients (lt/denoise_gradient.frag): a texel a previous stratum, the
   change of the light at its gradient pixel where this frame still sees that
   surface (A-SVGF, Schied 2018: the same sample shaded twice, so nothing but
   a real change);
3. the temporal stage (lt/denoise_temporal.frag): the history where it is the
   same surface (owner, distance within 3%, normal within 25 degrees;
   reprojected by the guides' motion, each pixel's own, where the twins write
   it, else by the cameras), its
   count cut by the share of the light that changed, the sample added by count
   (1/n, n up to 64: never an exponential average, never clamped to the
   neighbourhood);
4. the spatial stage: each pixel's guides in one texel (lt/denoise_guide.frag),
   then 3 passes of the variance-guided a-trous filter (lt/denoise_atrous.frag,
   steps 1, 2, 4; half floats), its weights normalized, stopped at owners,
   depth, the shaded normal and luminance;
5. this frame's inputs, guides and sample kept for the next.

The last pass's image is the denoised irradiance the surfaces read (C3). Its
passes are "DENOISE" in debug.pass_times. HALO_LT_DENOISE_CAMERA=1 reprojects
by the cameras even where the guides carry motion (checking);
HALO_LT_DENOISE_NO_GRADIENTS=1 leaves the anti-lag out,
HALO_LT_DENOISE_SPATIAL=0 the spatial stage, and HALO_LT_DENOISE_EMA=1 makes
the history an exponential average (1/64) from the start (checking: the
negative controls). HALO_LT_DENOISE_MOTION_VIEW=1 (checking) makes the
result each pixel's guides' motion less the cameras' reprojection (rg, in
pixels; b 1 where the guides carry motion, 2 where not, 0 nothing drawn),
the spatial stage left out; =2 the guides' motion itself (pixels) and, in b, the pixel's distance negated.
HALO_LT_DENOISE_DUMP ("1-5,30,120": the denoiser's draws counted from its
first) writes the result at those draws into HALO_LT_DENOISE_DUMP_DIR (the
current folder by default) as denoise_<draw>.rgba16f, E1's dumps' form (a
16-byte header "LTE1", width, height, draw; then RGBA half rows), and logs
each draw (its frame number and whether it had history).
Render thread alone.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_denoise.h"
#include "xgpu_vulkan_lt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DENOISE_OWNER "DENOISE"
#define DENOISE_KIND 0xfb
/* the diffuse history's cap (frames) */
#define DENOISE_CAP 64
/* a level lit live: the diffuse's history longer, as the bounce's (xgpu_vulkan_lt_indirect.c's
INDIRECT_BOUNCE_CAP_LIVE): its light the sky's, which stays; the gradients still cut it where a light changes */
#define DENOISE_CAP_LIVE 128
/* the a-trous passes (steps 1, 2, 4) */
#define DENOISE_PASSES 3
/* the luminance's edge-stop (standard deviations of the estimate), the
depth's (a share of the distance off the pixel's plane), the shaded normal's
(1 - cos: about 2 degrees halves a weight, 5 leave 2%) */
#define DENOISE_SIGMA_LUMINANCE 4.0f
#define DENOISE_SIGMA_DEPTH 0.01f
#define DENOISE_SIGMA_NORMAL 0.001f
/* where the parameters start in the draw set's uniforms (lt/denoise_common.glsl's) */
#define DENOISE_PARAMS_OFFSET 1024

_Static_assert(sizeof(struct xgpu_world_ao_pass) <= DENOISE_PARAMS_OFFSET, "the pass's uniforms before the parameters");
_Static_assert(XGPU_WORLD_AO_INPUT_PRESENT == 0x400u && XGPU_SPIRV_GUIDES_MOTION_NONE == 0u &&
	XGPU_SPIRV_GUIDES_MOTION_FLIP == 0x80008000u, "lt/denoise_common.glsl's constants");
/* (XGPU_SPIRV_GUIDES_MOTION_RANGE, 0.25 in lt/denoise_common.glsl too, is a
float, and a comparison of floats is not an integer constant expression for
every compiler (CI's clang refuses it): the two are kept alike by hand) */

/* lt/denoise_common.glsl's, std140 */
struct denoise_params
{
	float size[4];
	int32_t control[4];
	float tuning[4];
	/* (the guide's fold: the bounce's size in blocks, its phase, its
	flags: DENOISE_BOUNCE_*) */
	float bounce[4];
	/* (the final pass: the suns, each one's forward and power) */
	int32_t suns[4];
	float sun[16][4];
	/* (the gradients' deterministic half: the light table's rows this
	frame and the frame before, the frame before's first row, the
	flashlight's cookie's first texel: xgpu_vulkan_lt_table_halves) */
	int32_t lights[4];
};
/* the final pass's control.w (lt/denoise_pair.glsl's RIGHT): the right
half's draw */
#define DENOISE_RIGHT 1
enum
{
	DENOISE_BOUNCE_ON = 1,
	DENOISE_BOUNCE_ONLY = 2,
};
enum
{
	DENOISE_HISTORY = 1,
	DENOISE_GRADIENTS = 2,
	DENOISE_FIRST = 4,
	DENOISE_CAMERA_ONLY = 8,
	DENOISE_EMA = 16,
	DENOISE_SPECULAR = 32,
	DENOISE_MOTION_VIEW = 128,
	DENOISE_MOTION_RAW = 256,
};
/* the specular channel's cap where the guides have no roughness (its
temporal stage's own runs from 16 to 32 by it: lt/denoise_temporal.frag) */
#define DENOISE_SPECULAR_CAP 32
#define DENOISE_SPECULAR_PASSES 2

/* a channel's images and history (the diffuse's; the specular's,
xgpu_vulkan_denoise_specular_draw): the histories and filter guides in
turn (this frame's, the last), the a-trous passes' in turn, the result, the
re-shade, the gradients, the last frame's guides */
struct denoise_channel
{
	struct xgpu_vulkan_image *histories[2], *filter_guides[2], *filtered[2], *result, *reshaded, *gradients;
	/* (the diffuse's, with the bounce folded in: this frame's one diffuse
	sample, the direct light's and the bounce's, which the temporal stage
	counts) */
	struct xgpu_vulkan_image *combined;
	struct xgpu_vulkan_image *previous_guides;
	int current, previous_valid;
	struct xgpu_world_ao_pass previous_pass;
	/* HALO_LT_DENOISE_DUMP's: the draws counted */
	unsigned long draws;
};

static struct
{
	int made, failed, said;
	unsigned generation;
	VkShaderModule vertex, gradient, temporal, guide, guide_fold, atrous, atrous_pair, atrous_final;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description gradient_pass, temporal_pass, guide_pass, guide_fold_pass, atrous_pass;
	/* the spatial stage's shared passes (lt/denoise_pair.glsl): the two
	channels at once, and the last into the light the surfaces read */
	struct xgpu_vulkan_pipeline_description atrous_pair_pass, atrous_final_pass;
	/* the diffuse channel, the specular */
	struct denoise_channel channels[2];
	/* checking (HALO_LT_DENOISE_*) */
	int camera_only, no_gradients, spatial, ema, motion_view, cap;
	/* (the settings' generation read: xgpu_vulkan_lt_settings_generation) */
	unsigned settings_read;
	/* this frame a level lit live (the final's real): DENOISE_CAP_LIVE */
	int live;
	/* HALO_LT_DENOISE_DUMP's: those dumped */
	const char *dumps;
} denoise = { .spatial = -1 };

static VkShaderModule denoise_kernel(const char *source)
{
	VkShaderModule module = xgpu_vulkan_lt_kernel(source);

	if (!module)
		host_logf(HOST_LOG_WARN, "vulkan: denoise: no kernel %s", source);
	return module;
}

static int denoise_shaders(void)
{
	struct xgpu_vulkan_sampler_description sampler;
	unsigned generation = xgpu_vulkan_lt_kernels_generation();

	if (denoise.generation != generation)
	{
		/* (a reload: the kernels made again, the history started again) */
		denoise.made = denoise.failed = denoise.channels[0].previous_valid = denoise.channels[1].previous_valid = 0;
		denoise.generation = generation;
	}
	if (denoise.made)
		return 1;
	if (denoise.failed)
		return 0;
	if (denoise.spatial < 0 || denoise.settings_read != xgpu_vulkan_lt_settings_generation())
	{
		const char *spatial = getenv("HALO_LT_DENOISE_SPATIAL");

		denoise.settings_read = xgpu_vulkan_lt_settings_generation();

		denoise.spatial = !spatial || strcmp(spatial, "0");
		denoise.camera_only = getenv("HALO_LT_DENOISE_CAMERA") && strcmp(getenv("HALO_LT_DENOISE_CAMERA"), "0");
		denoise.no_gradients = getenv("HALO_LT_DENOISE_NO_GRADIENTS") &&
			strcmp(getenv("HALO_LT_DENOISE_NO_GRADIENTS"), "0");
		denoise.dumps = getenv("HALO_LT_DENOISE_DUMP");
		denoise.ema = getenv("HALO_LT_DENOISE_EMA") && strcmp(getenv("HALO_LT_DENOISE_EMA"), "0");
		denoise.motion_view = getenv("HALO_LT_DENOISE_MOTION_VIEW") ? atoi(getenv("HALO_LT_DENOISE_MOTION_VIEW")) : 0;
		/* (HALO_LT_DENOISE_CAP: the diffuse history's cap, checking) */
		denoise.cap = getenv("HALO_LT_DENOISE_CAP") ? atoi(getenv("HALO_LT_DENOISE_CAP")) : 0;
		if (denoise.motion_view)
			denoise.spatial = 0;
	}
	denoise.vertex = denoise_kernel("e1.vert");
	denoise.gradient = denoise_kernel("denoise_gradient.frag");
	denoise.temporal = denoise_kernel("denoise_temporal.frag");
	denoise.guide = denoise_kernel("denoise_guide.frag");
	denoise.guide_fold = denoise_kernel("denoise_guide_fold.frag");
	denoise.atrous = denoise_kernel("denoise_atrous.frag");
	denoise.atrous_pair = denoise_kernel("denoise_atrous_pair.frag");
	denoise.atrous_final = denoise_kernel("denoise_atrous_final.frag");
	if (!denoise.sampler)
	{
		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
		sampler.mip = XGPU_VULKAN_MIP_NONE;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
		sampler.anisotropy = 1;
		denoise.sampler = xgpu_vulkan_sampler(&sampler);
	}
	if (!denoise.vertex || !denoise.gradient || !denoise.temporal || !denoise.guide || !denoise.guide_fold || !denoise.atrous || !denoise.atrous_pair ||
		!denoise.atrous_final || !denoise.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: denoise: its passes cannot be made");
		denoise.failed = 1;
		return 0;
	}
	memset(&denoise.gradient_pass, 0, sizeof(denoise.gradient_pass));
	denoise.gradient_pass.vertex_shader = denoise.vertex;
	denoise.gradient_pass.pixel_shader = denoise.gradient;
	denoise.gradient_pass.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	denoise.gradient_pass.color_mask = 15;
	denoise.gradient_pass.color_format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	denoise.gradient_pass.samples = 1;
	denoise.temporal_pass = denoise.gradient_pass;
	denoise.temporal_pass.pixel_shader = denoise.temporal;
	denoise.temporal_pass.color_format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	denoise.guide_pass = denoise.temporal_pass;
	denoise.guide_pass.pixel_shader = denoise.guide;
	denoise.guide_pass.color_format = XGPU_VULKAN_FORMAT_RG32_UINT;
	/* (the fold: the one diffuse sample its second output) */
	denoise.guide_fold_pass = denoise.guide_pass;
	denoise.guide_fold_pass.pixel_shader = denoise.guide_fold;
	denoise.guide_fold_pass.motion_format = XGPU_VULKAN_FORMAT_RGBA16F;
	denoise.guide_fold_pass.motion_mask = 15;
	/* (the a-trous passes in half floats: rgb and the standard deviation) */
	denoise.atrous_pass = denoise.gradient_pass;
	denoise.atrous_pass.pixel_shader = denoise.atrous;
	denoise.atrous_pass.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	denoise.atrous_pair_pass = denoise.atrous_pass;
	denoise.atrous_pair_pass.pixel_shader = denoise.atrous_pair;
	denoise.atrous_pair_pass.motion_format = XGPU_VULKAN_FORMAT_RGBA16F;
	denoise.atrous_pair_pass.motion_mask = 15;
	/* (the light the surfaces read: xgpu_vulkan_lt.c's lt.final) */
	denoise.atrous_final_pass = denoise.atrous_pass;
	denoise.atrous_final_pass.pixel_shader = denoise.atrous_final;
	denoise.atrous_final_pass.color_format = XGPU_VULKAN_FORMAT_RGBA32F;
	(void)xgpu_vulkan_pipeline_prepare(&denoise.gradient_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.temporal_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.guide_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.guide_fold_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.atrous_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.atrous_pair_pass);
	(void)xgpu_vulkan_pipeline_prepare(&denoise.atrous_final_pass);
	denoise.made = 1;
	return 1;
}

static struct xgpu_vulkan_image *denoise_image(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
	uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	/* (never one the sets still name) */
	if (xgpu_vulkan_rt_bounce_bound(image))
		xgpu_vulkan_rt_bounce_set(NULL);
	xgpu_vulkan_image_destroy(image);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* whether HALO_LT_DENOISE_DUMP names draw */
static int denoise_dumped(unsigned long draw)
{
	const char *given = denoise.dumps;

	while (given && *given)
	{
		char *end;
		unsigned long low = strtoul(given, &end, 10), high = low;

		if (end == given)
			break;
		given = end;
		if (*given == '-')
		{
			high = strtoul(given + 1, &end, 10);
			given = end;
		}
		if (draw >= low && draw <= high)
			return 1;
		while (*given == ',' || *given == ' ')
			given++;
	}
	return 0;
}

static void denoise_dump(struct xgpu_vulkan_image *image, unsigned long draw, const char *what)
{
	const struct xgpu_vulkan_image_description *size = xgpu_vulkan_image_description(image);
	const char *folder = getenv("HALO_LT_DENOISE_DUMP_DIR");
	uint16_t *halves = NULL;
	uint32_t header[4];
	char path[1024];
	FILE *file;

	if (xgpu_vulkan_read_halves(image, 0, &halves) != 1 || !halves)
		return;
	snprintf(path, sizeof(path), "%s/denoise%s_%lu.rgba16f", folder && *folder ? folder : ".", what, draw);
	file = fopen(path, "wb");
	if (file)
	{
		memcpy(header, "LTE1", 4);
		header[1] = size->width;
		header[2] = size->height;
		header[3] = (uint32_t)draw;
		fwrite(header, sizeof(header), 1, file);
		fwrite(halves, (size_t)size->width * size->height * 8, 1, file);
		fclose(file);
	}
	free(halves);
}

/* a pass's triangle over rectangle of into, reading textures (up to
XGPU_VULKAN_TEXTURES; a NULL slot the first's) */
static void denoise_pass(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const struct denoise_params *params,
	const struct xgpu_vulkan_pipeline_description *pipeline, struct xgpu_vulkan_image *const *textures, int count)
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	unsigned char *bytes;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(pipeline);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < XGPU_VULKAN_TEXTURES && stage < count; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = denoise.sampler;
		draw.textures[stage].image = textures[stage] ? textures[stage] : textures[0];
	}
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, uniforms, sizeof(*uniforms));
	memcpy(bytes + DENOISE_PARAMS_OFFSET, params, sizeof(*params));
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	draw.ambient_occlusion = 1;
	draw.dynamic_lights = 1;
	draw.kind[0] = DENOISE_KIND;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

void xgpu_vulkan_denoise_prepare(void)
{
	(void)denoise_shaders();
}

/* a channel's frame (xgpu_vulkan_denoise_draw's; the specular's without
the gradients, flagged DENOISE_SPECULAR) */
static struct xgpu_vulkan_image *denoise_channel_draw(struct denoise_channel *channel, int specular,
	struct xgpu_vulkan_image *raw, struct xgpu_vulkan_image *previous_raw, struct xgpu_vulkan_image *inputs,
	struct xgpu_vulkan_image *previous_inputs, struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass,
	const int32_t rectangle[4], const struct xgpu_vulkan_denoise_bounce *bounce, int defer)
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_world_ao_pass uniforms;
	struct denoise_params params;
	struct xgpu_vulkan_image *history, *guide, *previous_guide, *output = NULL;
	uint32_t width, height, gradient_width, gradient_height;
	int32_t gradient_rectangle[4];
	int history_valid, index, passes;

	if (!raw || !inputs || !guides || !pass || !denoise_shaders())
		return NULL;
	if (!xgpu_vulkan_pipeline_ready(&denoise.gradient_pass) || !xgpu_vulkan_pipeline_ready(&denoise.temporal_pass) ||
		!xgpu_vulkan_pipeline_ready(&denoise.guide_pass) || !xgpu_vulkan_pipeline_ready(&denoise.atrous_pass) ||
		(bounce && bounce->image && !xgpu_vulkan_pipeline_ready(&denoise.guide_fold_pass)))
		return NULL;
	size = xgpu_vulkan_image_description(inputs);
	width = size->width;
	height = size->height;
	gradient_width = (width + 2) / 3;
	gradient_height = (height + 2) / 3;
	if (!channel->result || xgpu_vulkan_image_description(channel->result)->width != width ||
		xgpu_vulkan_image_description(channel->result)->height != height)
		channel->previous_valid = 0;
	for (index = 0; index < 2; index++)
	{
		channel->histories[index] = denoise_image(channel->histories[index], XGPU_VULKAN_FORMAT_RGBA32_UINT, width,
			height);
		channel->filtered[index] = denoise_image(channel->filtered[index], XGPU_VULKAN_FORMAT_RGBA16F, width, height);
		channel->filter_guides[index] = denoise_image(channel->filter_guides[index], XGPU_VULKAN_FORMAT_RG32_UINT, width,
			height);
	}
	channel->result = denoise_image(channel->result, XGPU_VULKAN_FORMAT_RGBA16F, width, height);
	if (bounce && bounce->image)
	{
		channel->combined = denoise_image(channel->combined, XGPU_VULKAN_FORMAT_RGBA16F, width, height);
		if (!channel->combined)
			return NULL;
	}
	channel->reshaded = denoise_image(channel->reshaded, XGPU_VULKAN_FORMAT_RGBA32F, gradient_width, gradient_height);
	/* (twice as wide: the right half the deterministic gradients,
	lt/denoise_gradient.frag) */
	channel->gradients = denoise_image(channel->gradients, XGPU_VULKAN_FORMAT_RGBA32_UINT, 2 * gradient_width,
		gradient_height);
	/* (the specular's: no re-shade, none kept) */
	channel->previous_guides = denoise_image(channel->previous_guides, XGPU_VULKAN_FORMAT_RGBA32_UINT,
		specular ? 1 : width, specular ? 1 : height);
	if (!channel->histories[0] || !channel->histories[1] || !channel->filtered[0] || !channel->filtered[1] ||
		!channel->filter_guides[0] || !channel->filter_guides[1] || !channel->result || !channel->reshaded ||
		!channel->gradients || !channel->previous_guides)
		return NULL;
	history_valid = channel->previous_valid && pass->history[0] != 0.0f;
	uniforms = *pass;
	memset(&params, 0, sizeof(params));
	/* (a level lit live: lt/denoise_pair.glsl's live) */
	params.suns[1] = denoise.live;
	params.size[0] = (float)width;
	params.size[1] = (float)height;
	params.size[2] = (float)gradient_width;
	params.size[3] = (float)gradient_height;
	params.control[0] = specular ? DENOISE_SPECULAR_CAP : denoise.cap > 0 ? denoise.cap : denoise.live ? DENOISE_CAP_LIVE :
		DENOISE_CAP;
	params.control[1] = (history_valid ? DENOISE_HISTORY : 0) | (denoise.camera_only ? DENOISE_CAMERA_ONLY : 0) |
		(denoise.ema ? DENOISE_EMA : 0) | (specular ? DENOISE_SPECULAR : 0) |
		(denoise.motion_view ? DENOISE_MOTION_VIEW : 0) | (denoise.motion_view == 2 ? DENOISE_MOTION_RAW : 0);
	params.tuning[0] = channel->previous_pass.history[1];
	params.tuning[1] = DENOISE_SIGMA_LUMINANCE;
	params.tuning[2] = DENOISE_SIGMA_DEPTH;
	params.tuning[3] = DENOISE_SIGMA_NORMAL;
	xgpu_vulkan_pass_owner(DENOISE_OWNER);
	/* 1: each pixel's guides in one texel (this frame's; the last frame's
	kept, in turn, for the history's tests) */
	previous_guide = channel->filter_guides[channel->current];
	channel->current ^= 1;
	guide = channel->filter_guides[channel->current];
	/* (the specular's: the diffuse channel's, drawn this frame from the
	same pixels, where it has them) */
	if (specular && denoise.channels[0].result && denoise.channels[0].previous_valid &&
		xgpu_vulkan_image_description(denoise.channels[0].result)->width == width &&
		xgpu_vulkan_image_description(denoise.channels[0].result)->height == height)
	{
		guide = denoise.channels[0].filter_guides[denoise.channels[0].current];
		previous_guide = denoise.channels[0].filter_guides[denoise.channels[0].current ^ 1];
	}
	else if (bounce && bounce->image)
	{
		/* (the fold: the bounce upsampled and added to the direct light's
		sample, the guide's second output, which the temporal stage then
		counts as the one diffuse sample) */
		const struct xgpu_vulkan_image_description *blocks = xgpu_vulkan_image_description(bounce->image);
		struct xgpu_vulkan_image *textures[4] = { inputs, guides, raw, bounce->image };

		params.bounce[0] = (float)blocks->width;
		params.bounce[1] = (float)blocks->height;
		params.bounce[2] = (float)bounce->phase;
		params.bounce[3] = (float)(DENOISE_BOUNCE_ON | (bounce->only ? DENOISE_BOUNCE_ONLY : 0));
		xgpu_vulkan_set_motion_target(channel->combined, guide);
		denoise_pass(guide, rectangle, &uniforms, &params, &denoise.guide_fold_pass, textures, 4);
		xgpu_vulkan_set_motion_target(NULL, NULL);
		raw = channel->combined;
	}
	else
	{
		struct xgpu_vulkan_image *textures[2] = { inputs, guides };

		denoise_pass(guide, rectangle, &uniforms, &params, &denoise.guide_pass, textures, 2);
	}
	/* 2: the gradients (the previous frame's surfaces re-shaded by this
	frame's light, against its own samples) */
	if (history_valid && previous_raw && previous_inputs && !denoise.no_gradients)
	{
		struct xgpu_vulkan_image *textures[5] = { guide, previous_guide, channel->reshaded, previous_raw, NULL };

		xgpu_vulkan_lt_reshade(channel->reshaded, &channel->previous_pass, previous_inputs, channel->previous_guides);
		/* (and the light table, both frames', for the deterministic half;
		none: the half left without, the stochastic alone) */
		textures[4] = xgpu_vulkan_lt_table_halves(params.lights);
		if (!textures[4])
		{
			/* (a float image in its float slot, never read: lights.z 0) */
			textures[4] = previous_raw;
			memset(params.lights, 0, sizeof(params.lights));
		}
		gradient_rectangle[0] = 0;
		gradient_rectangle[1] = 0;
		gradient_rectangle[2] = 2 * (int32_t)gradient_width;
		gradient_rectangle[3] = (int32_t)gradient_height;
		denoise_pass(channel->gradients, gradient_rectangle, &uniforms, &params, &denoise.gradient_pass, textures, 5);
		params.control[1] |= DENOISE_GRADIENTS;
	}
	/* 3: the temporal stage */
	history = channel->histories[channel->current ^ 1];
	{
		struct xgpu_vulkan_image *textures[6] = { raw, inputs, guides, previous_guide, history, channel->gradients };

		denoise_pass(channel->histories[channel->current], rectangle, &uniforms, &params, &denoise.temporal_pass,
			textures, 6);
	}
	/* 4: the spatial stage (the first pass from the history; the stage left
	out, checking: one pass of step 0, the mean alone) */
	/* (the specular's two, steps 1 and 2: its lobes are narrow, its count
	high where it is sharp) */
	passes = denoise.spatial ? (specular ? DENOISE_SPECULAR_PASSES : DENOISE_PASSES) : 1;
	/* (deferred: the shared passes after both channels' temporal stages,
	denoise_shared) */
	if (defer)
	{
		passes = 0;
		output = channel->histories[channel->current];
	}
	for (index = 0; index < passes; index++)
	{
		int last = index == passes - 1;
		struct xgpu_vulkan_image *textures[3] = { index ? output : channel->filtered[1],
			channel->histories[channel->current], guide };
		struct xgpu_vulkan_image *into = last ? channel->result : channel->filtered[index & 1];

		params.control[1] = (params.control[1] & ~DENOISE_FIRST) | (index ? 0 : DENOISE_FIRST);
		params.control[2] = denoise.spatial ? 1 << index : 0;
		denoise_pass(into, rectangle, &uniforms, &params, &denoise.atrous_pass, textures, 3);
		output = into;
	}
	/* 5: this frame's guides kept for the next's re-shade (its inputs and
	sample are the light transport's own, kept there) */
	if (!denoise.no_gradients && !specular)
		xgpu_vulkan_image_copy(guides, 0, 0, channel->previous_guides, 0, 0);
	channel->previous_pass = *pass;
	channel->previous_valid = 1;
	xgpu_vulkan_pass_owner(NULL);
	channel->draws++;
	if (denoise.dumps && *denoise.dumps && !defer)
	{
		host_logf(HOST_LOG_WARN, "vulkan: denoise: draw %lu, frame %.0f, history %d", channel->draws, pass->history[1],
			history_valid);
		if (denoise_dumped(channel->draws))
			denoise_dump(output, channel->draws, specular ? "_specular" : "");
	}
	if (!specular && !denoise.said)
	{
		denoise.said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: denoise: the light transport's irradiance denoised (%ux%u: count up to %d, "
			"A-SVGF gradients%s, %d a-trous passes)", width, height, DENOISE_CAP, denoise.no_gradients ? " off" : "",
			denoise.spatial ? DENOISE_PASSES : 0);
	}
	return output;
}

struct xgpu_vulkan_image *xgpu_vulkan_denoise_draw(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *previous_raw, struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *previous_inputs,
	struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass, const int32_t rectangle[4],
	const struct xgpu_vulkan_denoise_bounce *bounce)
{
	return denoise_channel_draw(&denoise.channels[0], 0, raw, previous_raw, inputs, previous_inputs, guides, pass,
		rectangle, bounce, 0);
}

/* the spatial stage of both channels at once and the light the surfaces
read (xgpu_vulkan_denoise_frame's), after their temporal stages: the pairs'
passes (steps 1 and 2), then the diffuse's last (step 4) into final's left
half, the sun's share off, and the specular's into its right; the diffuse's
denoised irradiance (its step 4 again, into its result) where a dump wants
it */
static struct xgpu_vulkan_image *denoise_shared(struct xgpu_vulkan_denoise_final *out,
	const struct xgpu_world_ao_pass *pass, const int32_t rectangle[4], struct xgpu_vulkan_image *inputs, int specular)
{
	struct denoise_channel *d = &denoise.channels[0], *s = &denoise.channels[1];
	struct xgpu_vulkan_image *guide = d->filter_guides[d->current];
	struct xgpu_vulkan_image *dh = d->histories[d->current], *sh = specular ? s->histories[s->current] : NULL;
	const struct xgpu_vulkan_image_description *size = xgpu_vulkan_image_description(inputs);
	struct xgpu_world_ao_pass uniforms = *pass;
	struct denoise_params params;
	int index, dumped = denoise.dumps && *denoise.dumps && denoise_dumped(d->draws);

	memset(&params, 0, sizeof(params));
	/* (a level lit live: lt/denoise_pair.glsl's live) */
	params.suns[1] = denoise.live;
	params.size[0] = (float)size->width;
	params.size[1] = (float)size->height;
	params.tuning[1] = DENOISE_SIGMA_LUMINANCE;
	params.tuning[2] = DENOISE_SIGMA_DEPTH;
	params.tuning[3] = DENOISE_SIGMA_NORMAL;
	xgpu_vulkan_pass_owner(DENOISE_OWNER);
	/* the pairs (or the diffuse alone): steps 1 and 2 */
	for (index = 0; index < 2; index++)
	{
		params.control[1] = index ? 0 : DENOISE_FIRST;
		params.control[2] = 1 << index;
		if (specular)
		{
			struct xgpu_vulkan_image *textures[5] = { d->filtered[index ^ 1], dh, guide, s->filtered[index ^ 1], sh };

			xgpu_vulkan_set_motion_target(s->filtered[index], d->filtered[index]);
			denoise_pass(d->filtered[index], rectangle, &uniforms, &params, &denoise.atrous_pair_pass, textures, 5);
			xgpu_vulkan_set_motion_target(NULL, NULL);
		}
		else
		{
			struct xgpu_vulkan_image *textures[3] = { d->filtered[index ^ 1], dh, guide };

			denoise_pass(d->filtered[index], rectangle, &uniforms, &params, &denoise.atrous_pass, textures, 3);
		}
	}
	/* the last: the diffuse's step 4 into final's left half; the
	specular's into its right (none: cleared, the cube maps kept) */
	params.control[1] = 0;
	params.control[2] = 4;
	params.suns[0] = out->suns;
	params.suns[1] = out->real;
	memcpy(params.sun, out->sun, sizeof(params.sun));
	{
		struct xgpu_vulkan_image *textures[6] = { d->filtered[1], dh, guide, inputs,
			specular ? s->filtered[1] : d->filtered[1], specular && out->coverage ? out->coverage : d->filtered[1] };

		denoise_pass(out->final, rectangle, &uniforms, &params, &denoise.atrous_final_pass, textures, 6);
		if (out->wide)
		{
			int32_t right[4] = { rectangle[0] + (int32_t)size->width, rectangle[1],
				rectangle[2] + (int32_t)size->width, rectangle[3] };

			if (specular)
			{
				params.control[3] = DENOISE_RIGHT;
				denoise_pass(out->final, right, &uniforms, &params, &denoise.atrous_final_pass, textures, 6);
			}
			else
			{
				static const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				int32_t clear[1][4] = { { right[0], right[1], right[2] - right[0], right[3] - right[1] } };

				xgpu_vulkan_set_target_views(out->final, NULL, 0, 1);
				xgpu_vulkan_clear((const int32_t (*)[4])clear, 1, XGPU_VULKAN_CLEAR_COLOR |
					XGPU_VULKAN_CLEAR_CHANNELS, none, 1.0f, 0);
			}
		}
	}
	out->drawn = 1;
	out->specular_denoised = specular ? s->filtered[1] : NULL;
	/* (a dump's: the diffuse's step 4 into its result, as the single
	channel's passes have it) */
	if (out->result || dumped)
	{
		struct xgpu_vulkan_image *textures[3] = { d->filtered[1], dh, guide };

		params.control[3] = 0;
		denoise_pass(d->result, rectangle, &uniforms, &params, &denoise.atrous_pass, textures, 3);
	}
	xgpu_vulkan_pass_owner(NULL);
	if (dumped)
	{
		denoise_dump(d->result, d->draws, "");
		if (specular)
			denoise_dump(s->filtered[1], d->draws, "_specular");
	}
	return out->result || dumped ? d->result : NULL;
}

struct xgpu_vulkan_image *xgpu_vulkan_denoise_frame(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *previous_raw, struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *previous_inputs,
	struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass, const int32_t rectangle[4],
	const struct xgpu_vulkan_denoise_bounce *bounce, struct xgpu_vulkan_denoise_final *out)
{
	int specular;

	out->drawn = 0;
	out->specular_denoised = NULL;
	denoise.live = out->real;
	if (!denoise_shaders())
		return NULL;
	/* (the checking modes, or a pipeline not made yet: the channels' own
	passes, the caller's final passes) */
	if (!out->final || !denoise.spatial || denoise.motion_view ||
		!xgpu_vulkan_pipeline_ready(&denoise.atrous_pair_pass) || !xgpu_vulkan_pipeline_ready(&denoise.atrous_final_pass))
		return xgpu_vulkan_denoise_draw(raw, previous_raw, inputs, previous_inputs, guides, pass, rectangle, bounce);
	if (!denoise_channel_draw(&denoise.channels[0], 0, raw, previous_raw, inputs, previous_inputs, guides, pass,
		rectangle, bounce, 1))
		return NULL;
	specular = out->specular && denoise_channel_draw(&denoise.channels[1], 1, out->specular, NULL, inputs, NULL, guides,
		pass, rectangle, NULL, 1);
	return denoise_shared(out, pass, rectangle, inputs, specular);
}

struct xgpu_vulkan_image *xgpu_vulkan_denoise_specular_draw(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass,
	const int32_t rectangle[4])
{
	return denoise_channel_draw(&denoise.channels[1], 1, raw, NULL, inputs, NULL, guides, pass, rectangle, NULL, 0);
}
