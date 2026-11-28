/*
XGPU_VULKAN_LT_INDIRECT.C

The light-transport pass's indirect light and reflections from the dynamic
lights (xgpu_vulkan_lt_indirect.h; ~/dev/rt2-realtime-design.md sections
1.3 and 1.4), lt/e1.frag's kernel built with LT_INDIRECT (lt/indirect.glsl:
its modes there), drawn after the direct light (xgpu_vulkan_lt.c) each frame:

1. the bounce, at half resolution: a texel a 2x2 block, its pixel this
   frame's phase's (the four in turn), one cosine ray; at the hit, its albedo
   (the material record) times one shadow-rayed light there plus the radiance
   cache's indirect light (only dynamic light: the lightmap has the static
   bounce);
2. the reflections, at the guides' specular pixels: one GGX ray, its hit
   Halo's lightmap x albedo plus the same dynamic light, its distance kept;
3. the radiance cache (our own world-space grid about the eye: cascades by
   distance, cells by place and normal octant, lt/indirect_cache.glsl): each
   slot gathers the bounce's samples of its cell where the cell is seen and
   merges them by count (1/n up to 256, never an exponential average), a slot
   started again where its cell or its lights changed; the hits read the last
   frame's (the bounce's later bounces, a frame each). A gather alone: the
   backend's draws scatter nothing (no storage writes, no vertex-stage
   textures), so its pipeline layout is the game's own;
4. the bounce upsampled on each pixel's surface and added to the direct
   light's sample: the one diffuse signal the denoiser then takes ("denoise",
   its gradients still the direct light's alone: their previous samples are
   the direct's); or counted in its own history and summed with the direct
   mean (E2, accumulate);
5. the reflections' hits' share counted (16 frames), and, after the
   diffuse's, the reflections through the denoiser's specular channel (or
   their own count: E2, accumulate); with the share, the right half of the
   light the surfaces read (xgpu_spirv.c's lt_reflection_texel), which their
   reflection stage takes in place of its cube map's texel where the rays
   met the scene.

Its passes are "LIGHT TRANSPORT" in debug.pass_times, as the pass's.
Render thread alone.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_denoise.h"
#include "xgpu_vulkan_lt.h"
#include "xgpu_vulkan_lt_indirect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INDIRECT_KIND 0xfc
/* (lt/e1.frag's flags, and lt/indirect.glsl's modes) */
#define INDIRECT_VISIBILITY 1
#define INDIRECT_RESET 2
#define INDIRECT_BOUNCE 4096
#define INDIRECT_SPECULAR_RAY 8192
#define INDIRECT_COMBINE 16384
#define INDIRECT_SUM 32768
#define INDIRECT_SPEC_HISTORY 65536
#define INDIRECT_CACHE_RESOLVE 131072
#define INDIRECT_SPEC_FINAL 262144
#define INDIRECT_ACCUMULATE 524288
#define INDIRECT_ONLY 1048576
#define INDIRECT_COVERAGE 2097152
#define INDIRECT_CACHE_ON 4194304
#define INDIRECT_BOUNCE_HISTORY 8388608
/* (with SPECULAR_RAY, HALO_LT_SHINE_VIEW=1, checking: the lights' shine
alone, no ray: lt/indirect.glsl's SHINE_ONLY) */
#define INDIRECT_SHINE_ONLY 16777216
/* (with CACHE_RESOLVE: the slots' means this pass's samples alone, the
cells' earlier light let go: the cache's warm-up) */
#define INDIRECT_CACHE_REPLACE 33554432
/* a level lit live: the passes of bounce and cache resolve drawn where the
cache starts again (a map's start, a cut, a hold's reset), each slot's mean
the last pass's alone, so each pass adds one bounce of the sky's light
(lt/indirect.glsl's bounce reads the cache the pass before left): the
first frame sees the sky's light bounced INDIRECT_WARM times, not once, and
the cache's later frames only refine it (no slow climb to the converged
light: the held frame's swelling) */
#define INDIRECT_WARM 6
/* where its parameters are in the uniforms (lt/e1.frag's indirect[4]) */
#define INDIRECT_PARAMS_OFFSET 1536
/* the cache's slots (a texel each, RGBA32F: 24 MB; lt/indirect_cache.glsl's
CELLS^2 by CELLS x 8 octants x CASCADES) */
#define INDIRECT_CACHE_WIDTH (32 * 32)
#define INDIRECT_CACHE_HEIGHT (32 * 8 * 6)
/* the least reflection a pixel must show for its ray (lt/indirect.glsl's
reflection_seen: linear light, about 1/255 of the display's white at its
darkest code; HALO_LT_REFLECTION_THRESHOLD another, checking) */
#define INDIRECT_REFLECTION_THRESHOLD 0.004f
/* the hits' share's history (frames); the bounce's own ahead of the
denoiser ("denoise": its noise counted down before it joins the direct
light's sample, which the denoiser's gradients then follow) */
#define INDIRECT_COVERAGE_CAP 16
#define INDIRECT_BOUNCE_CAP 32
/* a level lit live: the bounce is the sky's light, which stays: a longer
history (a light that comes or goes starts it again by the light-id hash
all the same, lt/indirect.glsl) */
#define INDIRECT_BOUNCE_CAP_LIVE 128

_Static_assert(INDIRECT_PARAMS_OFFSET + 64 <= 192 * 16, "the parameters within the uniforms");

static struct
{
	int settings_read, indirect, reflections, cache, only, made, failed, said, shine_only;
	unsigned generation;
	VkShaderModule vertex, fragment;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description wide, half, bounce_pass;
	/* the bounce (half resolution), the diffuse signal (the direct and the
	indirect), the indirect's histories (E2, accumulate), the reflections'
	samples, their hits' shares' histories, their radiance's (E2,
	accumulate); the cache in turn; the dump's */
	struct xgpu_vulkan_image *bounce, *total, *histories[2], *specular_raw, *coverage[2], *specular_histories[2];
	struct xgpu_vulkan_image *caches[2], *dump;
	/* (the fold's) the bounce's half-resolution histories in turn, the
	bounce pass's second output: the one the denoiser reads this frame */
	struct xgpu_vulkan_image *bounce_histories[2], *bounce_history;
	int bounce_history_valid;
	int current, cache_current, cache_valid;
	unsigned long frames;
	/* this frame's bounce's phase, and whether it was drawn (the fold's) */
	int phase, bounced;
	/* this frame's reflections' radiance (their samples, their count); the
	light the surfaces read (HALO_LT_SPECULAR_DUMP=final's) */
	struct xgpu_vulkan_image *specular_light, *final;
	/* (a 1x1 RGBA32Uint of zeros: the uint slots' stand-in, lt/e1.frag's
	inputs, previous_inputs and guides, where a mode binds none: a float
	image there is an unsigned sampler's undefined read) */
	struct xgpu_vulkan_image *uint_stand_in;
} indirect;

/* the stand-in for an unsigned slot left empty (NULL if it cannot be made) */
static struct xgpu_vulkan_image *indirect_uint_stand_in(void)
{
	static const uint32_t zeros[4] = { 0u, 0u, 0u, 0u };
	struct xgpu_vulkan_image_description description;

	if (indirect.uint_stand_in)
		return indirect.uint_stand_in;
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = XGPU_VULKAN_FORMAT_RGBA32_UINT;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_COPIED;
	description.width = 1;
	description.height = 1;
	description.depth_or_layers = 1;
	description.levels = 1;
	indirect.uint_stand_in = xgpu_vulkan_image_create(&description);
	if (indirect.uint_stand_in)
		xgpu_vulkan_image_upload(indirect.uint_stand_in, 0, 0, 0, 0, 0, 1, 1, 1, zeros, 16, 16);
	return indirect.uint_stand_in;
}

static void indirect_settings(void)
{
	const char *given;

	if (indirect.settings_read == (int)xgpu_vulkan_lt_settings_generation())
		return;
	indirect.settings_read = (int)xgpu_vulkan_lt_settings_generation();
	given = getenv("HALO_LT_INDIRECT");
	indirect.indirect = !given || strcmp(given, "0");
	indirect.only = given && !strcmp(given, "only");
	indirect.cache = !given || strcmp(given, "nocache");
	given = getenv("HALO_LT_SPECULAR");
	indirect.reflections = indirect.indirect && (!given || strcmp(given, "0"));
	indirect.shine_only = getenv("HALO_LT_SHINE_VIEW") && atoi(getenv("HALO_LT_SHINE_VIEW")) == 1;
	if (getenv("HALO_LT_INDIRECT") || getenv("HALO_LT_SPECULAR"))
		host_logf(HOST_LOG_WARN, "vulkan: light transport: indirect %d (only %d, cache %d), reflections %d (checking)",
			indirect.indirect, indirect.only, indirect.cache, indirect.reflections);
}

int xgpu_vulkan_lt_indirect_on(void)
{
	indirect_settings();
	return indirect.indirect;
}

static int indirect_shaders(void)
{
	struct xgpu_vulkan_sampler_description sampler;
	unsigned generation = xgpu_vulkan_lt_kernels_generation();

	if (indirect.generation != generation)
	{
		/* (a reload: the kernels made again, the histories and the cache
		started again) */
		indirect.made = indirect.failed = indirect.cache_valid = indirect.bounce_history_valid = 0;
		indirect.current = indirect.cache_current = 0;
		/* (the bounce's phase counted from the start again) */
		indirect.frames = 0;
		indirect.generation = generation;
	}
	if (indirect.made)
		return 1;
	if (indirect.failed)
		return 0;
	indirect.vertex = xgpu_vulkan_lt_kernel("e1.vert");
	indirect.fragment = xgpu_vulkan_lt_kernel("e1_indirect.frag");
	if (!indirect.sampler)
	{
		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
		sampler.mip = XGPU_VULKAN_MIP_NONE;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
		sampler.anisotropy = 1;
		indirect.sampler = xgpu_vulkan_sampler(&sampler);
	}
	if (!indirect.vertex || !indirect.fragment || !indirect.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: light transport: the indirect light's passes cannot be made");
		indirect.failed = 1;
		return 0;
	}
	memset(&indirect.wide, 0, sizeof(indirect.wide));
	indirect.wide.vertex_shader = indirect.vertex;
	indirect.wide.pixel_shader = indirect.fragment;
	indirect.wide.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	indirect.wide.color_mask = 15;
	indirect.wide.color_format = XGPU_VULKAN_FORMAT_RGBA32F;
	indirect.wide.samples = 1;
	indirect.half = indirect.wide;
	indirect.half.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	/* (the fold's bounce: its history the second output) */
	indirect.bounce_pass = indirect.wide;
	indirect.bounce_pass.motion_format = XGPU_VULKAN_FORMAT_RGBA32F;
	indirect.bounce_pass.motion_mask = 15;
	(void)xgpu_vulkan_pipeline_prepare(&indirect.bounce_pass);
	(void)xgpu_vulkan_pipeline_prepare(&indirect.wide);
	(void)xgpu_vulkan_pipeline_prepare(&indirect.half);
	indirect.made = 1;
	return 1;
}

static struct xgpu_vulkan_image *indirect_image(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
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

/* one of the kernel's draws over rectangle of into: its mode's flags (the
direct light's shadow setting kept), the count's cap, its six textures (a
NULL slot the light table's), the indirect's parameters */
static void indirect_pass(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_vulkan_lt_indirect_frame *frame, int flags, int cap, struct xgpu_vulkan_image *const textures[6],
	const struct xgpu_vulkan_pipeline_description *pipeline, const float parameters[4][4])
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	unsigned char *bytes;
	int32_t control[4];
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(pipeline);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 6; stage++)
	{
		/* (an empty slot: lt/e1.frag's unsigned ones (0 inputs, 2
		previous_inputs, 3 guides) a zero uint image, the float ones the
		table) */
		int unsigned_slot = stage == 0 || stage == 2 || stage == 3;

		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = indirect.sampler;
		draw.textures[stage].image = textures[stage] ? textures[stage] : unsigned_slot && indirect_uint_stand_in() ?
			indirect_uint_stand_in() : frame->table;
	}
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, frame->pass, sizeof(*frame->pass));
	memcpy(bytes + 1024, frame->params, frame->params_size);
	memcpy(control, bytes + 1024 + frame->control_offset, sizeof(control));
	control[0] = cap;
	control[3] = (control[3] & INDIRECT_VISIBILITY) | flags | (indirect.cache && indirect.cache_valid ?
		INDIRECT_CACHE_ON : 0);
	memcpy(bytes + 1024 + frame->control_offset, control, sizeof(control));
	memcpy(bytes + INDIRECT_PARAMS_OFFSET, parameters, sizeof(float) * 16);
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
	draw.kind[0] = INDIRECT_KIND;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

static void indirect_parameters(const struct xgpu_vulkan_lt_indirect_frame *frame, float parameters[4][4])
{
	const struct xgpu_vulkan_image_description *size = xgpu_vulkan_image_description(frame->inputs);

	memset(parameters, 0, sizeof(float) * 16);
	parameters[0][0] = (float)((size->width + 1) / 2);
	parameters[0][1] = (float)((size->height + 1) / 2);
	parameters[0][2] = (float)INDIRECT_CACHE_WIDTH;
	parameters[0][3] = (float)INDIRECT_CACHE_HEIGHT;
	parameters[1][0] = (float)size->width;
	parameters[1][1] = (float)(indirect.frames & 3);
	parameters[1][3] = (float)INDIRECT_COVERAGE_CAP;
	{
		static float threshold = -1.0f;

		if (threshold < 0.0f)
			threshold = getenv("HALO_LT_REFLECTION_THRESHOLD") ? (float)atof(getenv("HALO_LT_REFLECTION_THRESHOLD")) :
				INDIRECT_REFLECTION_THRESHOLD;
		parameters[2][2] = threshold;
	}
}

struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_draw(const struct xgpu_vulkan_lt_indirect_frame *frame)
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *before;
	uint32_t width, height, half_width, half_height;
	int32_t half[4], whole[4] = { 0, 0, INDIRECT_CACHE_WIDTH, INDIRECT_CACHE_HEIGHT };
	float parameters[4][4];
	int reset, accumulate, counted, axis, index;

	indirect.specular_light = NULL;
	indirect.bounced = 0;
	if (!xgpu_vulkan_lt_indirect_on() || !frame->inputs || !frame->direct || !frame->table || !frame->guides ||
		!indirect_shaders())
		return NULL;
	if (!xgpu_vulkan_pipeline_ready(&indirect.wide) || !xgpu_vulkan_pipeline_ready(&indirect.half) ||
		(frame->fold && !xgpu_vulkan_pipeline_ready(&indirect.bounce_pass)))
		return NULL;
	size = xgpu_vulkan_image_description(frame->inputs);
	width = size->width;
	height = size->height;
	half_width = (width + 1) / 2;
	half_height = (height + 1) / 2;
	accumulate = frame->mode == XGPU_VULKAN_LT_E2 || frame->mode == XGPU_VULKAN_LT_ACCUMULATE;
	/* (folded: the denoiser counts the bounce in its one temporal stage) */
	counted = accumulate || (frame->mode == XGPU_VULKAN_LT_DENOISE && !frame->fold);
	/* (the total's format follows the mode: E2's and accumulate's means in
	full floats) */
	if (indirect.total && (xgpu_vulkan_image_description(indirect.total)->format != (accumulate ?
		XGPU_VULKAN_FORMAT_RGBA32F : XGPU_VULKAN_FORMAT_RGBA16F) ||
		xgpu_vulkan_image_description(indirect.total)->width != width ||
		xgpu_vulkan_image_description(indirect.total)->height != height))
	{
		if (xgpu_vulkan_rt_bounce_bound(indirect.total))
			xgpu_vulkan_rt_bounce_set(NULL);
		xgpu_vulkan_image_destroy(indirect.total);
		indirect.total = NULL;
		indirect.cache_valid = 0;
	}
	indirect.bounce = indirect_image(indirect.bounce, XGPU_VULKAN_FORMAT_RGBA32F, half_width, half_height);
	if (frame->fold)
	{
		for (index = 0; index < 2; index++)
		{
			if (!indirect.bounce_histories[index] ||
				xgpu_vulkan_image_description(indirect.bounce_histories[index])->width != half_width ||
				xgpu_vulkan_image_description(indirect.bounce_histories[index])->height != half_height)
				indirect.bounce_history_valid = 0;
			indirect.bounce_histories[index] = indirect_image(indirect.bounce_histories[index],
				XGPU_VULKAN_FORMAT_RGBA32F, half_width, half_height);
			if (!indirect.bounce_histories[index])
				return NULL;
		}
	}
	else
		indirect.bounce_history_valid = 0;
	if (!frame->fold)
		indirect.total = indirect_image(indirect.total, accumulate ? XGPU_VULKAN_FORMAT_RGBA32F :
			XGPU_VULKAN_FORMAT_RGBA16F, width, height);
	for (index = 0; index < 2; index++)
	{
		indirect.caches[index] = indirect_image(indirect.caches[index], XGPU_VULKAN_FORMAT_RGBA32F,
			INDIRECT_CACHE_WIDTH, INDIRECT_CACHE_HEIGHT);
		if (counted)
			indirect.histories[index] = indirect_image(indirect.histories[index], XGPU_VULKAN_FORMAT_RGBA32F, width,
				height);
		if (indirect.reflections)
		{
			indirect.coverage[index] = indirect_image(indirect.coverage[index], XGPU_VULKAN_FORMAT_RGBA16F, width,
				height);
			if (accumulate)
				indirect.specular_histories[index] = indirect_image(indirect.specular_histories[index],
					XGPU_VULKAN_FORMAT_RGBA32F, width, height);
		}
	}
	if (indirect.reflections)
		indirect.specular_raw = indirect_image(indirect.specular_raw, XGPU_VULKAN_FORMAT_RGBA16F, width, height);
	if (!indirect.bounce || (!frame->fold && !indirect.total) || !indirect.caches[0] || !indirect.caches[1] || (counted && (!indirect.histories[0] || !indirect.histories[1])) ||
		(indirect.reflections && (!indirect.specular_raw || !indirect.coverage[0] || !indirect.coverage[1] ||
		(accumulate && (!indirect.specular_histories[0] || !indirect.specular_histories[1])))))
		return NULL;
	reset = frame->start || !frame->previous_valid;
	for (axis = 0; axis < 2; axis++)
	{
		half[axis] = frame->rectangle[axis] / 2;
		half[axis + 2] = (frame->rectangle[axis + 2] + 1) / 2;
	}
	indirect_parameters(frame, parameters);
	indirect.phase = (int)(indirect.frames & 3);
	xgpu_vulkan_pass_owner("LIGHT TRANSPORT");
	/* 0: a live level's cache warmed where it starts again */
	if (frame->live && indirect.cache && (reset || !indirect.cache_valid))
	{
		int pass;

		for (pass = 0; pass < INDIRECT_WARM; pass++)
		{
			struct xgpu_vulkan_image *bounce[6] = { frame->inputs, indirect.caches[indirect.cache_current],
				frame->previous_inputs, frame->guides, frame->table, NULL };
			struct xgpu_vulkan_image *resolve[6] = { frame->inputs, indirect.caches[indirect.cache_current], NULL,
				frame->guides, NULL, indirect.bounce };

			indirect_pass(indirect.bounce, half, frame, INDIRECT_BOUNCE | INDIRECT_RESET, frame->cap, bounce,
				&indirect.wide, parameters);
			resolve[1] = indirect.caches[indirect.cache_current];
			indirect_pass(indirect.caches[indirect.cache_current ^ 1], whole, frame, INDIRECT_CACHE_RESOLVE |
				INDIRECT_CACHE_REPLACE, frame->cap, resolve, &indirect.wide, parameters);
			indirect.cache_current ^= 1;
			indirect.cache_valid = 1;
		}
	}
	/* 1, 2: the bounce and the reflections' rays (the last frame's cache) */
	{
		struct xgpu_vulkan_image *textures[6] = { frame->inputs, indirect.caches[indirect.cache_current],
			frame->previous_inputs, frame->guides, frame->table, NULL };

		if (frame->fold)
		{
			/* (the fold: the bounce's half-resolution history counted by the
			same pass, its second output, from the last frame's) */
			float counted[4][4];
			int history_reset = reset || !indirect.bounce_history_valid;

			memcpy(counted, parameters, sizeof(counted));
			counted[3][0] = (float)(frame->live ? INDIRECT_BOUNCE_CAP_LIVE : INDIRECT_BOUNCE_CAP);
			textures[5] = indirect.bounce_histories[indirect.current];
			indirect.bounce_history = indirect.bounce_histories[indirect.current ^ 1];
			xgpu_vulkan_set_motion_target(indirect.bounce_history, indirect.bounce);
			indirect_pass(indirect.bounce, half, frame, INDIRECT_BOUNCE | INDIRECT_BOUNCE_HISTORY |
				(history_reset ? INDIRECT_RESET : 0), frame->cap, textures, &indirect.bounce_pass, counted);
			xgpu_vulkan_set_motion_target(NULL, NULL);
			textures[5] = NULL;
			indirect.bounce_history_valid = 1;
		}
		else
			indirect_pass(indirect.bounce, half, frame, INDIRECT_BOUNCE | (reset ? INDIRECT_RESET : 0), frame->cap,
				textures, &indirect.wide, parameters);
		indirect.bounced = 1;
		/* (the reflections: the direct light this frame for the guess of
		what they would show, reflection_seen) */
		textures[5] = frame->direct;
		if (indirect.reflections)
			indirect_pass(indirect.specular_raw, frame->rectangle, frame, INDIRECT_SPECULAR_RAY |
				(reset ? INDIRECT_RESET : 0) | (indirect.shine_only ? INDIRECT_SHINE_ONLY : 0), frame->cap, textures,
				&indirect.half, parameters);
	}
	if (indirect.cache)
	{
		/* 3: the cache: each slot's cell's samples gathered, into the other */
		struct xgpu_vulkan_image *resolve[6] = { frame->inputs, indirect.caches[indirect.cache_current], NULL,
			frame->guides, NULL, indirect.bounce };

		indirect_pass(indirect.caches[indirect.cache_current ^ 1], whole, frame, INDIRECT_CACHE_RESOLVE |
			(indirect.cache_valid ? 0 : INDIRECT_RESET), frame->cap, resolve, &indirect.wide, parameters);
		indirect.cache_current ^= 1;
		indirect.cache_valid = 1;
	}
	/* 4: the diffuse signal: the bounce upsampled, counted in its own
	history (E2's and accumulate's cap; "denoise" INDIRECT_BOUNCE_CAP) and
	summed with the direct light (its mean, or the sample the denoiser takes),
	or (raw) added to the direct sample as it is */
	before = counted ? indirect.histories[indirect.current] : NULL;
	if (frame->fold)
		;
	else if (counted)
	{
		struct xgpu_vulkan_image *now = indirect.histories[indirect.current ^ 1];
		struct xgpu_vulkan_image *textures[6] = { frame->inputs, frame->direct, frame->previous_inputs, frame->guides,
			before, indirect.bounce };
		struct xgpu_vulkan_image *sum[6] = { frame->inputs, frame->direct, NULL, NULL, NULL, now };

		indirect_pass(now, frame->rectangle, frame, INDIRECT_COMBINE | INDIRECT_ACCUMULATE |
			(reset ? INDIRECT_RESET : 0), accumulate ? frame->cap : INDIRECT_BOUNCE_CAP, textures, &indirect.wide,
			parameters);
		indirect_pass(indirect.total, frame->rectangle, frame, INDIRECT_SUM | (indirect.only ? INDIRECT_ONLY : 0),
			frame->cap, sum, accumulate ? &indirect.wide : &indirect.half, parameters);
	}
	else
	{
		struct xgpu_vulkan_image *textures[6] = { frame->inputs, frame->direct, frame->previous_inputs, frame->guides,
			NULL, indirect.bounce };

		indirect_pass(indirect.total, frame->rectangle, frame, INDIRECT_COMBINE | (indirect.only ? INDIRECT_ONLY : 0),
			frame->cap, textures, &indirect.half, parameters);
	}
	/* 5: the reflections' hits' share counted; their radiance counted (E2,
	accumulate) or as sampled (raw; "denoise": the denoiser's, after the
	diffuse's: xgpu_vulkan_lt_indirect_final) */
	if (indirect.reflections)
	{
		struct xgpu_vulkan_image *textures[6] = { frame->inputs, indirect.coverage[indirect.current],
			frame->previous_inputs, frame->guides, NULL, indirect.specular_raw };

		indirect_pass(indirect.coverage[indirect.current ^ 1], frame->rectangle, frame, INDIRECT_SPEC_HISTORY |
			INDIRECT_COVERAGE | (reset ? INDIRECT_RESET : 0), frame->cap, textures, &indirect.half, parameters);
		indirect.specular_light = indirect.specular_raw;
		if (accumulate)
		{
			float counted[4][4];

			memcpy(counted, parameters, sizeof(counted));
			counted[1][3] = (float)frame->cap;
			textures[1] = indirect.specular_histories[indirect.current];
			indirect_pass(indirect.specular_histories[indirect.current ^ 1], frame->rectangle, frame,
				INDIRECT_SPEC_HISTORY | (reset ? INDIRECT_RESET : 0), frame->cap, textures, &indirect.wide, counted);
			indirect.specular_light = indirect.specular_histories[indirect.current ^ 1];
		}
	}
	indirect.current ^= 1;
	xgpu_vulkan_pass_owner(NULL);
	indirect.frames++;
	if (!indirect.said)
	{
		indirect.said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: light transport: the dynamic lights' bounce (half resolution, the radiance "
			"cache %s) and reflections %s", indirect.cache ? "on" : "off", indirect.reflections ? "on" : "off");
	}
	return frame->fold ? frame->direct : indirect.total;
}

struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_specular(struct xgpu_vulkan_image **coverage)
{
	/* (this frame's hits' share: the history drawn last, in turn) */
	*coverage = indirect.coverage[indirect.current];
	return indirect.reflections && indirect.specular_light ? indirect.specular_raw : NULL;
}

void xgpu_vulkan_lt_indirect_finished(struct xgpu_vulkan_image *final, struct xgpu_vulkan_image *specular)
{
	indirect.final = final;
	if (specular)
		indirect.specular_light = specular;
}

struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_bounce(int *phase, int *only)
{
	*phase = indirect.phase;
	*only = indirect.only;
	return indirect.bounced && indirect.bounce_history_valid ? indirect.bounce_history : NULL;
}

void xgpu_vulkan_lt_indirect_final(const struct xgpu_vulkan_lt_indirect_frame *frame, struct xgpu_vulkan_image *final)
{
	struct xgpu_vulkan_image *light = indirect.specular_light;
	float parameters[4][4];
	int32_t right[4];

	indirect.final = final;
	if (!final || !indirect.made || !frame->inputs)
		return;
	indirect_parameters(frame, parameters);
	right[0] = frame->rectangle[0] + (int32_t)parameters[1][0];
	right[1] = frame->rectangle[1];
	right[2] = frame->rectangle[2] + (int32_t)parameters[1][0];
	right[3] = frame->rectangle[3];
	if (light && frame->mode == XGPU_VULKAN_LT_DENOISE)
	{
		struct xgpu_vulkan_image *denoised = xgpu_vulkan_denoise_specular_draw(light, frame->inputs, frame->guides,
			frame->pass, frame->rectangle);

		if (denoised)
			light = denoised;
	}
	xgpu_vulkan_pass_owner("LIGHT TRANSPORT");
	if (light)
	{
		/* (this frame's hits' share: the history drawn last, in turn) */
		struct xgpu_vulkan_image *textures[6] = { frame->inputs, light, NULL, NULL, NULL,
			indirect.coverage[indirect.current] };

		indirect.specular_light = light;
		indirect_pass(final, right, frame, INDIRECT_SPEC_FINAL, frame->cap, textures, &indirect.wide, parameters);
	}
	else
	{
		/* (no reflections: the right half nothing, the cube maps kept) */
		static const float none[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		int32_t rectangle[1][4] = { { right[0], right[1], right[2] - right[0], right[3] - right[1] } };

		xgpu_vulkan_set_target_views(final, NULL, 0, 1);
		xgpu_vulkan_clear((const int32_t (*)[4])rectangle, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS, none,
			1.0f, 0);
	}
	xgpu_vulkan_pass_owner(NULL);
}

void xgpu_vulkan_lt_indirect_dump(const struct xgpu_vulkan_lt_indirect_frame *frame, unsigned long since)
{
	const struct xgpu_vulkan_image_description *size;
	const char *folder = getenv("HALO_LT_E1_DUMP_DIR");
	struct xgpu_vulkan_image *textures[6];
	uint16_t *halves = NULL;
	float parameters[4][4];
	uint32_t header[4];
	char path[1024];
	FILE *file;

	if (!getenv("HALO_LT_SPECULAR_DUMP") || !indirect.specular_light || !frame->inputs)
		return;
	/* ("raw": the reflections' rays as traced: a less than 0 where none was) */
	if (!strcmp(getenv("HALO_LT_SPECULAR_DUMP"), "raw"))
	{
		struct xgpu_vulkan_image *raw = indirect.specular_raw;

		size = xgpu_vulkan_image_description(raw);
		if (xgpu_vulkan_read_halves(raw, 0, &halves) != 1 || !halves)
			return;
		snprintf(path, sizeof(path), "%s/lt_spec_raw_%lu.rgba16f", folder && *folder ? folder : ".", since);
		goto write;
	}
	size = xgpu_vulkan_image_description(frame->inputs);
	indirect.dump = indirect_image(indirect.dump, XGPU_VULKAN_FORMAT_RGBA16F, size->width, size->height);
	if (!indirect.dump)
		return;
	indirect_parameters(frame, parameters);
	parameters[1][0] = 0.0f;
	memset(textures, 0, sizeof(textures));
	textures[0] = frame->inputs;
	textures[1] = indirect.specular_light;
	textures[5] = indirect.coverage[indirect.current];
	/* ("final": the right half of the light the surfaces read, its rgb, its
	red again in alpha) */
	if (!strcmp(getenv("HALO_LT_SPECULAR_DUMP"), "final") && indirect.final)
	{
		parameters[1][0] = -(float)size->width;
		textures[1] = textures[5] = indirect.final;
	}
	indirect_pass(indirect.dump, frame->rectangle, frame, INDIRECT_SPEC_FINAL, frame->cap, textures, &indirect.half,
		parameters);
	if (xgpu_vulkan_read_halves(indirect.dump, 0, &halves) != 1 || !halves)
		return;
	snprintf(path, sizeof(path), "%s/lt_spec_%lu.rgba16f", folder && *folder ? folder : ".", since);
write:
	file = fopen(path, "wb");
	if (file)
	{
		memcpy(header, "LTE1", 4);
		header[1] = size->width;
		header[2] = size->height;
		header[3] = (uint32_t)since;
		fwrite(header, sizeof(header), 1, file);
		fwrite(halves, (size_t)size->width * size->height * 8, 1, file);
		fclose(file);
		host_logf(HOST_LOG_INFO, "vulkan: light transport: frame %lu's reflections written (%s)", since, path);
	}
	free(halves);
}
