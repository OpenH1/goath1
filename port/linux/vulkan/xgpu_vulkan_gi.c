/*
XGPU_VULKAN_GI.C

display.rt_bounce's bounced light in the Vulkan backend (docs/rt-plan/3-gi.md,
stage 3; xgpu_spirv.h's xgpu_spirv_rt_bounce_pass). The game hands over each
window's lights that bounce (the stream's RT_BOUNCE, before the window's
pre-pass: port/linux/game/dynamic_lights_bounce.c); at the pre-pass's end,
after the dynamic lights' shadows and before the surfaces are drawn again:

- the trace: one pixel of each XGPU_RT_BOUNCE_TILE square (another each
  frame) casts one ray through the ray-traced scene; what it meets, lit by
  the lights alone (one chosen by its share, its shadow ray), times its base
  map, is that square's sample;
- the resolve: the window's cache, a grid of cells about its camera kept in
  place in the world (its cells at their coordinates modulo the grid, so it
  scrolls without copying: a cell that comes into the grid begins again),
  six faces a cell, each blended with the samples seen in it that face its
  way, its history kept by RT_BOUNCE_KEEP a 60th of a second (at most
  RT_BOUNCE_MOST samples), faded by RT_BOUNCE_FADE where nothing is seen
  of it; two images in turn;
- the gather: each pixel's light from its eight cells, into an image of
  the window's size, which the occlusion's sets name
  (xgpu_vulkan_rt_bounce_set) until the next pre-pass begins.

The cache is the denoiser: a world-space average of every sample of a cell
over the frames, so the view's rays may move each frame without shimmer and
the camera may move without losing it. No pass is drawn and the surfaces read
the stand-in (none) without ray queries, the pre-pass, the window's lights
since RT_BOUNCE_IDLE frames, or before the pipelines are made. Render
thread alone, as the core.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_lt.h"
#include "xgpu_vulkan_lt_indirect.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* (debug.pass_times' name of the passes; HALO_VULKAN_DRAW_TIMES' kind) */
#define RT_BOUNCE_OWNER "RT BOUNCE"
#define RT_BOUNCE_KIND 0xfb
#define RT_BOUNCE_WINDOWS 4
/* a cell's history: its share kept each 60th of a second, its most (in
samples), its fading where unseen; frames with no lights before the passes
stop */
#define RT_BOUNCE_KEEP 0.9f
#define RT_BOUNCE_MOST 64.0f
#define RT_BOUNCE_FADE 0.98f
#define RT_BOUNCE_IDLE 120u

struct rt_bounce_window
{
	struct xgpu_vulkan_image *cache[2];
	int current, valid;
	float origin[3];
	uint32_t idle;
};

static struct
{
	int made, failed, drawn_said, lights_said;
	VkShaderModule vertex, trace, resolve, gather;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description passes[3];
	struct xgpu_vulkan_image *samples, *gathered;
	/* the last window's lights (the stream's RT_BOUNCE), had since its
	pre-pass began */
	struct xgpu_stream_rt_bounce lights;
	int have_lights;
	struct rt_bounce_window windows[RT_BOUNCE_WINDOWS];
	float strength, cell;
} rt_bounce;

static VkShaderModule rt_bounce_shader(int which, BOOL vertex)
{
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	VkShaderModule shader;
	BOOL made;

	memset(&options, 0, sizeof(options));
	options.reversed_depth = XGPU_VULKAN_REVERSED_DEPTH;
	options.ray_queries = TRUE;
	made = vertex ? xgpu_spirv_world_ao_pass(XGPU_SPIRV_WORLD_AO_VERTEX, &options, &module) :
		xgpu_spirv_rt_bounce_pass(which, &options, &module);
	if (!made)
		return VK_NULL_HANDLE;
	shader = xgpu_vulkan_shader(module.words, module.count);
	free(module.words);
	return shader;
}

/* the passes' shaders and pipelines' descriptions, asked for (made apart
from the draws); 0 if they cannot be made */
static int rt_bounce_shaders(void)
{
	struct xgpu_vulkan_capabilities capabilities;
	struct xgpu_vulkan_sampler_description sampler;
	int index;

	if (rt_bounce.made)
		return 1;
	if (rt_bounce.failed)
		return 0;
	xgpu_vulkan_capabilities(&capabilities);
	if (!capabilities.ray_queries)
		return 0;
	rt_bounce.vertex = rt_bounce_shader(0, TRUE);
	rt_bounce.trace = rt_bounce_shader(XGPU_SPIRV_RT_BOUNCE_TRACE, FALSE);
	rt_bounce.resolve = rt_bounce_shader(XGPU_SPIRV_RT_BOUNCE_RESOLVE, FALSE);
	rt_bounce.gather = rt_bounce_shader(XGPU_SPIRV_RT_BOUNCE_GATHER, FALSE);
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	rt_bounce.sampler = xgpu_vulkan_sampler(&sampler);
	if (!rt_bounce.vertex || !rt_bounce.trace || !rt_bounce.resolve || !rt_bounce.gather || !rt_bounce.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.rt_bounce: its passes' shaders could not be made");
		rt_bounce.failed = 1;
		return 0;
	}
	for (index = 0; index < 3; index++)
	{
		struct xgpu_vulkan_pipeline_description *pass = &rt_bounce.passes[index];

		memset(pass, 0, sizeof(*pass));
		pass->vertex_shader = rt_bounce.vertex;
		pass->pixel_shader = index == XGPU_SPIRV_RT_BOUNCE_TRACE ? rt_bounce.trace :
			index == XGPU_SPIRV_RT_BOUNCE_RESOLVE ? rt_bounce.resolve : rt_bounce.gather;
		pass->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		pass->color_mask = 15;
		pass->color_format = XGPU_VULKAN_FORMAT_RGBA16F;
		pass->samples = 1;
		(void)xgpu_vulkan_pipeline_prepare(pass);
	}
	{
		const char *strength = getenv("HALO_RT_BOUNCE_STRENGTH");
		const char *cell = getenv("HALO_RT_BOUNCE_CELL");

		rt_bounce.strength = strength ? (float)atof(strength) : XGPU_RT_BOUNCE_STRENGTH;
		rt_bounce.cell = cell && atof(cell) > 0.01 ? (float)atof(cell) : XGPU_RT_BOUNCE_CELL;
	}
	rt_bounce.made = 1;
	return 1;
}

static struct xgpu_vulkan_image *rt_bounce_image(struct xgpu_vulkan_image *image, uint32_t width, uint32_t height)
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
	description.format = XGPU_VULKAN_FORMAT_RGBA16F;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* a pass's triangle over a rectangle of into: the pre-pass's inputs, the
history's image, the samples; the pass's uniforms, and the block at the
constants */
static void rt_bounce_pass_draw(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const float (*block)[4], int which, struct xgpu_vulkan_image *inputs,
	struct xgpu_vulkan_image *history, struct xgpu_vulkan_image *samples)
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	void *bytes;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(&rt_bounce.passes[which]);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = rt_bounce.sampler;
	}
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_INPUTS].image = inputs;
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_HISTORY].image = history;
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_CASCADES].image = xgpu_vulkan_world_ao_cascades_image();
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_CASCADES].kind = XGPU_VULKAN_IMAGE_3D;
	draw.textures[XGPU_SPIRV_RT_BOUNCE_SAMPLES].image = samples;
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, uniforms, sizeof(*uniforms));
	/* (the constants' range: 384 registers) */
	bytes = xgpu_vulkan_upload(384 * 16, 256, &draw.constants);
	if (!bytes)
		return;
	memset(bytes, 0, 384 * 16);
	memcpy(bytes, block, XGPU_RT_BOUNCE_BLOCK * 16);
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
	draw.kind[0] = RT_BOUNCE_KIND;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

/* the inverse of the 3x3 of columns a, b, c (rows into rows); 0 if it has
none */
static int rt_bounce_invert(const float a[3], const float b[3], const float c[3], float rows[3][4])
{
	double m[3][3], det;
	int i;

	for (i = 0; i < 3; i++)
	{
		m[i][0] = a[i];
		m[i][1] = b[i];
		m[i][2] = c[i];
	}
	det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
		m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
	if (fabs(det) < 1.0e-12)
		return 0;
	rows[0][0] = (float)((m[1][1] * m[2][2] - m[1][2] * m[2][1]) / det);
	rows[0][1] = (float)((m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det);
	rows[0][2] = (float)((m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det);
	rows[1][0] = (float)((m[1][2] * m[2][0] - m[1][0] * m[2][2]) / det);
	rows[1][1] = (float)((m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det);
	rows[1][2] = (float)((m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det);
	rows[2][0] = (float)((m[1][0] * m[2][1] - m[1][1] * m[2][0]) / det);
	rows[2][1] = (float)((m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det);
	rows[2][2] = (float)((m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det);
	rows[0][3] = rows[1][3] = rows[2][3] = 0.0f;
	return 1;
}

void xgpu_vulkan_rt_bounce_lights(const struct xgpu_stream_rt_bounce *record)
{
	rt_bounce.lights = *record;
	rt_bounce.have_lights = 1;
	if (!rt_bounce.lights_said)
	{
		rt_bounce.lights_said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: display.rt_bounce: the bounced light asked for (%d lights)", (int)record->count);
	}
}

void xgpu_vulkan_rt_bounce_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass)
{
	static const float dims[3] = { (float)XGPU_RT_BOUNCE_GRID_X, (float)XGPU_RT_BOUNCE_GRID_Y,
		(float)XGPU_RT_BOUNCE_GRID_Z };
	const struct xgpu_vulkan_image_description *size;
	struct rt_bounce_window *window;
	float block[XGPU_RT_BOUNCE_BLOCK][4], seconds, steps;
	int32_t rectangle[4], tiles[4], whole[4];
	uint32_t tiles_wide, tiles_high;
	int axis, index, count, next;

	xgpu_vulkan_rt_bounce_set(NULL);
	if (!rt_bounce.have_lights || !inputs || !pass)
		return;
	rt_bounce.have_lights = 0;
	if (!rt_bounce_shaders())
		return;
	index = rt_bounce.lights.window;
	window = &rt_bounce.windows[index >= 0 && index < RT_BOUNCE_WINDOWS ? index : 0];
	count = rt_bounce.lights.count;
	/* (no lights for a while: the cache let go of, nothing drawn) */
	window->idle = count > 0 ? 0 : window->idle + 1;
	if (window->idle > RT_BOUNCE_IDLE)
	{
		window->valid = 0;
		return;
	}
	for (index = 0; index < 3; index++)
	{
		if (!xgpu_vulkan_pipeline_ready(&rt_bounce.passes[index]))
			return;
	}
	if (!xgpu_vulkan_rt_build())
		return;
	size = xgpu_vulkan_image_description(inputs);
	for (axis = 0; axis < 2; axis++)
	{
		int32_t extent = (int32_t)(axis ? size->height : size->width);
		int32_t low = pass->corners[axis] < 0 ? 0 : pass->corners[axis] > extent ? extent : pass->corners[axis];
		int32_t high = pass->corners[axis + 2] < low ? low : pass->corners[axis + 2] > extent ? extent :
			pass->corners[axis + 2];

		rectangle[axis] = low;
		rectangle[axis + 2] = high;
		tiles[axis] = low / XGPU_RT_BOUNCE_TILE;
		tiles[axis + 2] = (high + XGPU_RT_BOUNCE_TILE - 1) / XGPU_RT_BOUNCE_TILE;
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	tiles_wide = (size->width + XGPU_RT_BOUNCE_TILE - 1) / XGPU_RT_BOUNCE_TILE;
	tiles_high = (size->height + XGPU_RT_BOUNCE_TILE - 1) / XGPU_RT_BOUNCE_TILE;
	rt_bounce.samples = rt_bounce_image(rt_bounce.samples, tiles_wide, tiles_high);
	rt_bounce.gathered = rt_bounce_image(rt_bounce.gathered, size->width, size->height);
	next = !window->current;
	window->cache[0] = rt_bounce_image(window->cache[0], XGPU_RT_BOUNCE_CACHE_WIDTH, XGPU_RT_BOUNCE_CACHE_HEIGHT);
	window->cache[1] = rt_bounce_image(window->cache[1], XGPU_RT_BOUNCE_CACHE_WIDTH, XGPU_RT_BOUNCE_CACHE_HEIGHT);
	if (!rt_bounce.samples || !rt_bounce.gathered || !window->cache[0] || !window->cache[1])
		return;
	/* the block */
	memset(block, 0, sizeof(block));
	for (axis = 0; axis < 3; axis++)
	{
		float origin = floorf(pass->camera[0][axis] / rt_bounce.cell) - floorf(dims[axis] * 0.5f);

		block[XGPU_RT_BOUNCE_BLOCK_ORIGIN][axis] = origin;
		block[XGPU_RT_BOUNCE_BLOCK_PREVIOUS][axis] = window->origin[axis];
		window->origin[axis] = origin;
		block[XGPU_RT_BOUNCE_BLOCK_EYE][axis] = pass->camera[0][axis];
	}
	block[XGPU_RT_BOUNCE_BLOCK_ORIGIN][3] = rt_bounce.cell;
	block[XGPU_RT_BOUNCE_BLOCK_PREVIOUS][3] = window->valid ? 1.0f : 0.0f;
	seconds = rt_bounce.lights.seconds > 0.0f && rt_bounce.lights.seconds < 0.25f ? rt_bounce.lights.seconds : 1.0f / 60.0f;
	steps = seconds * 60.0f;
	block[XGPU_RT_BOUNCE_BLOCK_TIMES][0] = pass->history[1];
	block[XGPU_RT_BOUNCE_BLOCK_TIMES][1] = powf(RT_BOUNCE_KEEP, steps);
	block[XGPU_RT_BOUNCE_BLOCK_TIMES][2] = RT_BOUNCE_MOST;
	block[XGPU_RT_BOUNCE_BLOCK_TIMES][3] = powf(RT_BOUNCE_FADE, steps);
	if (!rt_bounce_invert(pass->camera[1], pass->camera[2], pass->camera[3],
		(float (*)[4])block[XGPU_RT_BOUNCE_BLOCK_UNPROJECT]))
		return;
	block[XGPU_RT_BOUNCE_BLOCK_EYE][3] = (float)count;
	block[XGPU_RT_BOUNCE_BLOCK_SIZES][0] = (float)XGPU_RT_BOUNCE_TILE;
	block[XGPU_RT_BOUNCE_BLOCK_SIZES][1] = (float)tiles_wide;
	block[XGPU_RT_BOUNCE_BLOCK_SIZES][2] = (float)tiles_high;
	block[XGPU_RT_BOUNCE_BLOCK_SIZES][3] = rt_bounce.strength;
	for (index = 0; index < count && index < XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST; index++)
	{
		const struct xgpu_stream_rt_bounce_light *light = &rt_bounce.lights.lights[index];
		float (*out)[4] = &block[XGPU_RT_BOUNCE_BLOCK_LIGHTS + 3 * index];

		memcpy(out[0], light->position, 3 * sizeof(float));
		out[0][3] = light->radius;
		memcpy(out[1], light->color, 3 * sizeof(float));
		out[1][3] = light->cone_offset;
		memcpy(out[2], light->axis, 3 * sizeof(float));
		out[2][3] = light->cone_scale;
	}
	/* (HALO_RT_BOUNCE_LOG, checking: the lights every 60th window drawn) */
	{
		static int logging = -1;
		static unsigned long drawn;

		if (logging < 0)
			logging = getenv("HALO_RT_BOUNCE_LOG") != NULL;
		if (logging && drawn++ % 60 == 0)
			host_logf(HOST_LOG_INFO, "vulkan: display.rt_bounce: %d lights, the first at %.2f %.2f %.2f reach %.2f color "
				"%.3f %.3f %.3f; the grid from %.0f %.0f %.0f", count, (double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS][0],
				(double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS][1], (double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS][2],
				(double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS][3], (double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS + 1][0],
				(double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS + 1][1], (double)block[XGPU_RT_BOUNCE_BLOCK_LIGHTS + 1][2],
				(double)block[XGPU_RT_BOUNCE_BLOCK_ORIGIN][0], (double)block[XGPU_RT_BOUNCE_BLOCK_ORIGIN][1],
				(double)block[XGPU_RT_BOUNCE_BLOCK_ORIGIN][2]);
	}
	xgpu_vulkan_pass_owner(RT_BOUNCE_OWNER);
	rt_bounce_pass_draw(rt_bounce.samples, tiles, pass, (const float (*)[4])block, XGPU_SPIRV_RT_BOUNCE_TRACE, inputs,
		NULL, NULL);
	whole[0] = whole[1] = 0;
	whole[2] = XGPU_RT_BOUNCE_CACHE_WIDTH;
	whole[3] = XGPU_RT_BOUNCE_CACHE_HEIGHT;
	rt_bounce_pass_draw(window->cache[next], whole, pass, (const float (*)[4])block, XGPU_SPIRV_RT_BOUNCE_RESOLVE, inputs,
		window->cache[window->current], rt_bounce.samples);
	window->current = next;
	window->valid = 1;
	rt_bounce_pass_draw(rt_bounce.gathered, rectangle, pass, (const float (*)[4])block, XGPU_SPIRV_RT_BOUNCE_GATHER, inputs,
		window->cache[next], rt_bounce.samples);
	xgpu_vulkan_pass_owner(NULL);
	xgpu_vulkan_rt_bounce_set(rt_bounce.gathered);
	if (!rt_bounce.drawn_said)
	{
		rt_bounce.drawn_said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: display.rt_bounce: the bounced light traced (%ux%u samples, a cache of "
			"%dx%dx%d cells of %.3f world units)", tiles_wide, tiles_high, XGPU_RT_BOUNCE_GRID_X, XGPU_RT_BOUNCE_GRID_Y,
			XGPU_RT_BOUNCE_GRID_Z, (double)rt_bounce.cell);
	}
}

void xgpu_vulkan_rt_bounce_end(void)
{
	/* (HALO_LT_E1: the light transport's light kept bound past the window's
	end, until its next draw (xgpu_vulkan_lt_draw unbinds it first): the
	structure's reflection passes, drawn after the window, read its right
	half, xgpu_vulkan_lt_indirect.c) */
	if (xgpu_vulkan_lt_mode() && xgpu_vulkan_lt_indirect_on())
		return;
	xgpu_vulkan_rt_bounce_set(NULL);
}
