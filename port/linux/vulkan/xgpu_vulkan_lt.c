/*
XGPU_VULKAN_LT.C

E1 and E2 of the one light-transport pass (xgpu_vulkan_lt.h;
~/dev/rt2-realtime-design.md, section 7): the view's shadowed flashlight's
direct light, traced at the pre-pass's end (xgpu_vulkan_stream.c, where the
light shadows and the bounce are), from the pre-pass's inputs (each pixel's
distance, normal and owner).

The light is a physical spot: a disk at the lamp on the weapon in hand,
facing the beam, its gel and falloff as rt/tonight's decoding of the tags has
them (the cells' XGPU_DYNAMIC_LIGHTS_FLASHLIGHT texels: the gel projected
from the game's light, as its gel pass projects it, its falloff from there),
taken from the game's gamma to linear light (^2.2), times the cosine at the
receiver and one shadow ray a pixel a frame to a point of the disk (the R2
sequence over the frames from a start of the pixel's own: stratified), into
an RGBA32F image whose rgb is the mean and alpha the count. The mean is
count-based (1/n up to the mode's cap), its history the previous frame's
image at the reprojected point where the previous frame's pre-pass found the
same surface there (distance within 3%, normal within 25 degrees, the same
owner); otherwise it starts again (a disocclusion). The count starts again
everywhere when the light comes or goes or jumps.

The pass is a triangle over the window drawn as the light-shadow passes
are (xgpu_vulkan_light_shadows.c: the backend's draw sets, the occlusion's
with the ray-traced scene, the hit set), its kernels GLSL compiled offline
(lt/e1.vert, lt/e1.frag; tools/vulkan_lt_shaders.py). Its image is then
the one the surfaces read at display.rt_bounce's binding, which they add,
in this mode, after the game's own lights, linearly (xgpu_spirv.c's
lt_e1_add).

HALO_LT_E1_DUMP ("1-5,30,100-120": frames counted from the count's last
start) writes the mean at those frames as half floats into HALO_LT_E1_DUMP_DIR
(the current folder by default) as lt_e1_<frame>.rgba16f: a 16-byte header
("LTE1", width, height, frame as uint32) and RGBA rows from the first.
HALO_LT_E1_UNSHADOWED=1 (checking) casts no shadow ray. Render thread alone.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_lt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lt/xgpu_vulkan_lt_shaders.inc"

/* (debug.pass_times' name of the pass; HALO_VULKAN_DRAW_TIMES' kind) */
#define LT_OWNER "LIGHT TRANSPORT"
#define LT_KIND 0xfa
/* the counts' caps: E2's (the design's diffuse history), the accumulation's */
#define LT_E2_CAP 64
#define LT_ACCUMULATE_CAP (1 << 20)
/* the disk's radius (world units: a lamp's lens, about 3 cm) */
#define LT_DISK_RADIUS 0.01f
/* the flashlight's linear power over the tag's decoding taken to linear, set once so that the beam's centre on the
c20 corner's wall at about 3 m shows as bright as the original's gamma add, falling off physically from there
(~/dev/rt2-calibration.json's flashlight_power_scale and its derivation, with the physical falloff of lt/e1.frag; the truth renderer uses the same) */
#define LT_POWER_SCALE 1.009f
/* the lamp moved this far in a frame (world units): a jump, the count started again */
#define LT_JUMP 0.25f
/* where the parameters start in the draw set's uniforms (lt/e1.frag's) */
#define LT_PARAMS_OFFSET 1024
#define LT_DUMPS 512

_Static_assert(sizeof(struct xgpu_world_ao_pass) <= LT_PARAMS_OFFSET, "the pass's uniforms before the parameters");
_Static_assert(XGPU_DYNAMIC_LIGHTS_GRID_Z == 16 && XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE == 80 &&
	XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF == 32 && XGPU_WORLD_CASCADES == 4 && XGPU_WORLD_AO_INPUT_PRESENT == 0x400u &&
	XGPU_SPIRV_RT_MASK_AO == 0x03u && XGPU_SPIRV_RT_HIT_TEXTURES == 512u, "lt/e1.frag's constants");

/* lt/e1.frag's, std140 */
struct lt_params
{
	float light[4], color[4], forward[4], up[4], left[4], lamp[4];
	int32_t control[4];
	uint32_t owners[4];
	float falloff[XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF];
};
_Static_assert(LT_PARAMS_OFFSET + sizeof(struct lt_params) <= 192 * 16, "the parameters within the uniforms");
enum
{
	LT_VISIBILITY = 1,
	LT_RESET = 2,
	LT_COPY = 4,
	LT_COPY_GEOMETRIC = 8,
	LT_COPY_SHADED = 16,
	LT_COPY_PRESENT = 32,
};

static struct
{
	int mode, made, failed, said, prepared;
	VkShaderModule vertex, fragment;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description pass, copy;
	/* the means in turn (this frame's, the last), the last frame's inputs,
	the dump's halves */
	struct xgpu_vulkan_image *means[2], *previous, *dump;
	int current, previous_valid;
	/* the light last frame (its lamp), for the count's starts */
	int had_light;
	float last_lamp[3];
	unsigned long since_start;
	unsigned char dumps[LT_DUMPS];
	int dumps_read;
} lt = { -1 };

int xgpu_vulkan_lt_mode(void)
{
	if (lt.mode < 0)
	{
		const char *given = getenv("HALO_LT_E1");

		lt.mode = XGPU_VULKAN_LT_OFF;
		if (given && *given && strcmp(given, "off") && strcmp(given, "0") && strcmp(given, "false"))
			lt.mode = !strcmp(given, "raw") ? XGPU_VULKAN_LT_RAW : !strcmp(given, "accumulate") ?
				XGPU_VULKAN_LT_ACCUMULATE : XGPU_VULKAN_LT_E2;
		if (lt.mode)
			host_logf(HOST_LOG_INFO, "vulkan: light transport E1: \"%s\" (the flashlight's direct light traced, %s)",
				given, lt.mode == XGPU_VULKAN_LT_RAW ? "1 spp, no history" : lt.mode == XGPU_VULKAN_LT_E2 ?
				"1 spp, counted up to 64 frames" : "counted up to 2^20 frames");
	}
	return lt.mode;
}

/* HALO_LT_E1_DUMP's frames, read once */
static void lt_dumps_read(void)
{
	const char *given = getenv("HALO_LT_E1_DUMP");

	lt.dumps_read = 1;
	while (given && *given)
	{
		char *end;
		long low = strtol(given, &end, 10), high = low, frame;

		if (end == given)
			break;
		given = end;
		if (*given == '-')
		{
			high = strtol(given + 1, &end, 10);
			given = end;
		}
		for (frame = low; frame <= high && frame < LT_DUMPS; frame++)
			if (frame >= 0)
				lt.dumps[frame] = 1;
		while (*given == ',' || *given == ' ')
			given++;
	}
}

static int lt_shaders(void)
{
	struct xgpu_vulkan_sampler_description sampler;

	if (lt.made)
		return 1;
	if (lt.failed)
		return 0;
	lt.vertex = xgpu_vulkan_shader(lt_e1_vertex_spirv, sizeof(lt_e1_vertex_spirv) / sizeof(uint32_t));
	lt.fragment = xgpu_vulkan_shader(lt_e1_spirv, sizeof(lt_e1_spirv) / sizeof(uint32_t));
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	lt.sampler = xgpu_vulkan_sampler(&sampler);
	if (!lt.vertex || !lt.fragment || !lt.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: light transport E1: its pass cannot be made");
		lt.failed = 1;
		return 0;
	}
	memset(&lt.pass, 0, sizeof(lt.pass));
	lt.pass.vertex_shader = lt.vertex;
	lt.pass.pixel_shader = lt.fragment;
	lt.pass.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	lt.pass.color_mask = 15;
	lt.pass.color_format = XGPU_VULKAN_FORMAT_RGBA32F;
	lt.pass.samples = 1;
	lt.copy = lt.pass;
	lt.copy.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	lt.made = 1;
	return 1;
}

static struct xgpu_vulkan_image *lt_image(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
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

/* a cells' texel of the clusters' slice (xgpu.h) */
static const uint32_t *lt_texel(const unsigned char *cells, int index)
{
	return (const uint32_t *)(const void *)(cells + ((XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y *
		XGPU_DYNAMIC_LIGHTS_GRID_Z) + index) * 16);
}

/* the view's shadowed flashlight into the parameters: 0 where there is none */
static int lt_light(const unsigned char *cells, const struct xgpu_dynamic_lights_block *block, struct lt_params *params)
{
	const uint32_t *head = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1);
	const uint32_t *slots = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT);
	int slot, light, count = (int)block->grid[0][3], index;

	if (!head[0] || head[0] > XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS)
		return 0;
	slot = (int)head[0] - 1;
	light = (int)slots[slot] - 1;
	if (light < 0 || light >= count || light >= XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		return 0;
	memcpy(params->light, block->position[light], sizeof(params->light));
	memcpy(params->color, block->color[light], sizeof(params->color));
	memcpy(&params->color[3], &head[1], sizeof(float));
	memcpy(params->forward, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 2 + 3 * slot), sizeof(float) * 3);
	memcpy(params->up, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 3 + 3 * slot), sizeof(float) * 3);
	memcpy(params->left, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 4 + 3 * slot), sizeof(float) * 3);
	params->forward[3] = LT_DISK_RADIUS;
	memcpy(params->lamp, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 23), sizeof(float) * 3);
	params->lamp[3] = LT_POWER_SCALE;
	params->owners[0] = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 14)[3];
	for (index = 0; index < XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF / 4; index++)
		memcpy(&params->falloff[index * 4], lt_texel(cells, XGPU_DYNAMIC_LIGHTS_ATTENUATION + index), 16);
	return params->light[3] != 0.0f;
}

/* a pass's triangle over rectangle of into: the pre-pass's inputs, the last
mean, the last frame's inputs; the uniforms, the lights, the occlusion's and
the hits' sets */
static void lt_pass_draw(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const struct lt_params *params,
	const struct xgpu_vulkan_pipeline_description *pass, struct xgpu_vulkan_image *inputs,
	struct xgpu_vulkan_image *history, struct xgpu_vulkan_image *previous)
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	unsigned char *bytes;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(pass);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = lt.sampler;
	}
	draw.textures[0].image = inputs;
	draw.textures[1].image = history;
	draw.textures[2].image = previous;
	/* (the draw set's uniforms' range, which the constants' share) */
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, uniforms, sizeof(*uniforms));
	memcpy(bytes + LT_PARAMS_OFFSET, params, sizeof(*params));
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
	draw.kind[0] = LT_KIND;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

/* HALO_LT_E1_DUMP: the mean as half floats, written as its frame comes */
static void lt_dump_one(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs, int what, const char *suffix);

/* HALO_LT_E1_DUMP: the mean, and the pass's inputs (the geometric and shaded
normals, the pixels the pre-pass drew) */
static void lt_dump(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs)
{
	lt_dump_one(rectangle, uniforms, params, mean, inputs, 0, "");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_GEOMETRIC, "_normal_geometric");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_SHADED, "_normal_shaded");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_PRESENT, "_present");
}

static void lt_dump_one(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs, int what, const char *suffix)
{
	const struct xgpu_vulkan_image_description *size = xgpu_vulkan_image_description(mean);
	const char *folder = getenv("HALO_LT_E1_DUMP_DIR");
	uint16_t *halves = NULL;
	uint32_t header[4];
	char path[1024];
	FILE *file;

	if (!xgpu_vulkan_pipeline_ready(&lt.copy))
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.copy);
		return;
	}
	lt.dump = lt_image(lt.dump, XGPU_VULKAN_FORMAT_RGBA16F, size->width, size->height);
	if (!lt.dump)
		return;
	params->control[3] = LT_COPY | what;
	lt_pass_draw(lt.dump, rectangle, uniforms, params, &lt.copy, inputs, mean, lt.previous);
	if (xgpu_vulkan_read_halves(lt.dump, 0, &halves) != 1 || !halves)
		return;
	snprintf(path, sizeof(path), "%s/lt_e1_%lu%s.rgba16f", folder && *folder ? folder : ".", lt.since_start, suffix);
	file = fopen(path, "wb");
	if (file)
	{
		memcpy(header, "LTE1", 4);
		header[1] = size->width;
		header[2] = size->height;
		header[3] = (uint32_t)lt.since_start;
		fwrite(header, sizeof(header), 1, file);
		fwrite(halves, (size_t)size->width * size->height * 8, 1, file);
		fclose(file);
		host_logf(HOST_LOG_INFO, "vulkan: light transport E1: frame %lu's mean written (%s)", lt.since_start, path);
	}
	free(halves);
}

int xgpu_vulkan_lt_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const unsigned char *cells, const struct xgpu_dynamic_lights_block *block)
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *history;
	struct lt_params params;
	int32_t rectangle[4];
	int axis, light, start;

	if (!xgpu_vulkan_lt_mode())
		return 0;
	/* (what the pass reads is not what it draws into) */
	xgpu_vulkan_rt_bounce_set(NULL);
	if (!inputs || !pass || !cells || !block || !lt_shaders())
		return 1;
	if (!lt.prepared)
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.pass);
		lt.prepared = 1;
	}
	if (!lt.dumps_read)
		lt_dumps_read();
	if (!xgpu_vulkan_pipeline_ready(&lt.pass) || !xgpu_vulkan_rt_build())
		return 1;
	size = xgpu_vulkan_image_description(inputs);
	for (axis = 0; axis < 2; axis++)
	{
		int32_t extent = (int32_t)(axis ? size->height : size->width);
		int32_t low = pass->corners[axis] < 0 ? 0 : pass->corners[axis] > extent ? extent : pass->corners[axis];
		int32_t high = pass->corners[axis + 2] < low ? low : pass->corners[axis + 2] > extent ? extent :
			pass->corners[axis + 2];

		rectangle[axis] = low;
		rectangle[axis + 2] = high;
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	lt.means[0] = lt_image(lt.means[0], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
	lt.means[1] = lt_image(lt.means[1], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
	if (!lt.previous || xgpu_vulkan_image_description(lt.previous)->width != size->width ||
		xgpu_vulkan_image_description(lt.previous)->height != size->height)
		lt.previous_valid = 0;
	lt.previous = lt_image(lt.previous, XGPU_VULKAN_FORMAT_RGBA32_UINT, size->width, size->height);
	if (!lt.means[0] || !lt.means[1] || !lt.previous)
		return 1;
	memset(&params, 0, sizeof(params));
	light = lt_light(cells, block, &params);
	/* (the count started again as the light comes, goes or jumps, or the
	last frame's inputs are not had) */
	start = light != lt.had_light || !lt.previous_valid;
	if (light && lt.had_light)
	{
		float dx = params.lamp[0] - lt.last_lamp[0], dy = params.lamp[1] - lt.last_lamp[1];
		float dz = params.lamp[2] - lt.last_lamp[2];

		start |= dx * dx + dy * dy + dz * dz > LT_JUMP * LT_JUMP;
	}
	lt.had_light = light;
	memcpy(lt.last_lamp, params.lamp, sizeof(lt.last_lamp));
	lt.since_start = start ? 1 : lt.since_start + 1;
	{
		static int unshadowed = -1;

		if (unshadowed < 0)
			unshadowed = getenv("HALO_LT_E1_UNSHADOWED") && strcmp(getenv("HALO_LT_E1_UNSHADOWED"), "0");
		params.control[0] = lt.mode == XGPU_VULKAN_LT_RAW ? 1 : lt.mode == XGPU_VULKAN_LT_E2 ? LT_E2_CAP :
			LT_ACCUMULATE_CAP;
		params.control[1] = light;
		params.control[2] = XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE;
		params.control[3] = (unshadowed ? 0 : LT_VISIBILITY) | (start ? LT_RESET : 0);
	}
	history = lt.means[lt.current];
	lt.current ^= 1;
	xgpu_vulkan_pass_owner(LT_OWNER);
	lt_pass_draw(lt.means[lt.current], rectangle, pass, &params, &lt.pass, inputs, history, lt.previous);
	/* (this frame's inputs kept for the next's reprojection) */
	xgpu_vulkan_image_copy(inputs, 0, 0, lt.previous, 0, 0);
	lt.previous_valid = 1;
	xgpu_vulkan_pass_owner(NULL);
	{
		/* (HALO_LT_E1_DUMP_EVERY: every so many frames too, a held frame's) */
		static long every = -1;

		if (every < 0)
			every = getenv("HALO_LT_E1_DUMP_EVERY") ? atol(getenv("HALO_LT_E1_DUMP_EVERY")) : 0;
		if ((lt.since_start < LT_DUMPS && lt.dumps[lt.since_start]) || (every > 0 && !(lt.since_start % every)))
			lt_dump(rectangle, pass, &params, lt.means[lt.current], inputs);
	}
	xgpu_vulkan_rt_bounce_set(lt.means[lt.current]);
	if (!lt.said)
	{
		lt.said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: light transport E1: the flashlight's direct light traced (%ux%u)",
			size->width, size->height);
	}
	return 1;
}
