/*
XGPU_VULKAN_LIGHT_SHADOWS.C

display.shadows "ray_traced"'s shadows of the dynamic lights in the Vulkan
backend (docs/dynamic-shadows-plan.md, stage 1; xgpu.h's
XGPU_DYNAMIC_LIGHTS_SHADOWED). The game chooses the few lights that cast
them (port/linux/game/dynamic_lights.c) and names them in the enhanced
lights' cells; where display.ambient_occlusion "world" finds the window
once a pixel, its pre-pass has each pixel's distance, normal and owner
(the stream's WORLD_AO_PREPASS), and at its end, after the occlusion's
pass and before the surfaces are drawn again:

- the trace (xgpu_spirv_light_shadows_pass's _TRACE): a full-screen pass
  of ray queries through the ray-traced scene (xgpu_vulkan_rt.c), a few
  rays from each pixel to points on each shadowed light's disk, the pixel's
  own owner and the light's left out, into a mask of four bytes, a channel
  a light (the share of the disk seen);
- where no temporal technique (FSR, DLSS) accumulates the view, the filter
  (_FILTER): the mask averaged over the 3x3 pixels of the same surface, its
  rays the same each frame (no shimmer); with one, its rays are turned each
  frame and the technique's accumulation is the denoiser.

The mask is what the occlusion's sets name (xgpu_vulkan_light_shadows_set)
until the next pre-pass begins; the surfaces found once a pixel multiply
each shadowed light by its channel (xgpu_spirv.c's light_shadows_*). With
the setting off, no lights named, no ray queries or no pre-pass, no pass
is drawn and the surfaces read the stand-in: every light seen, the same
pixels.

The pipelines are made as a map's are, where the setting is on (the
stream's RT_SHADOWS while preparing: xgpu_vulkan_light_shadows_prepare); a
pass not yet made is not drawn. Render thread alone, as the core.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_shader.h"
#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

#include <stdlib.h>
#include <string.h>

/* (debug.pass_times' name of the passes; HALO_VULKAN_DRAW_TIMES' kind) */
#define LIGHT_SHADOWS_OWNER "LIGHT SHADOWS"
#define LIGHT_SHADOWS_KIND 0xfc

static struct
{
	int made, failed, drawn_said;
	VkShaderModule vertex, trace, filter, flashlight;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description trace_pass, filter_pass, flashlight_pass;
	int flashlight_prepared, flashlight_said;
	/* the trace's mask and the filter's */
	struct xgpu_vulkan_image *traced, *filtered;
} light_shadows;

static VkShaderModule light_shadows_shader(int which, BOOL vertex, BOOL ray_queries)
{
	struct xgpu_spirv_options options;
	struct xgpu_spirv_module module;
	VkShaderModule shader;
	BOOL made;

	memset(&options, 0, sizeof(options));
	options.reversed_depth = XGPU_VULKAN_REVERSED_DEPTH;
	options.ray_queries = ray_queries;
	made = vertex ? xgpu_spirv_world_ao_pass(XGPU_SPIRV_WORLD_AO_VERTEX, &options, &module) :
		xgpu_spirv_light_shadows_pass(which, &options, &module);
	if (!made)
		return VK_NULL_HANDLE;
	shader = xgpu_vulkan_shader(module.words, module.count);
	free(module.words);
	return shader;
}

/* the passes' shaders and pipelines' descriptions; 0 if they cannot be
made (none without ray queries) */
static int light_shadows_shaders(int ray_queries)
{
	struct xgpu_vulkan_sampler_description sampler;

	if (light_shadows.made)
		return 1;
	if (light_shadows.failed || !ray_queries)
		return 0;
	if (!light_shadows.vertex)
		light_shadows.vertex = light_shadows_shader(0, TRUE, FALSE);
	if (!light_shadows.trace)
		light_shadows.trace = light_shadows_shader(XGPU_SPIRV_LIGHT_SHADOWS_TRACE, FALSE, TRUE);
	if (!light_shadows.filter)
		light_shadows.filter = light_shadows_shader(XGPU_SPIRV_LIGHT_SHADOWS_FILTER, FALSE, FALSE);
	if (!light_shadows.flashlight)
		light_shadows.flashlight = light_shadows_shader(XGPU_SPIRV_LIGHT_SHADOWS_FLASHLIGHT, FALSE, TRUE);
	if (!light_shadows.sampler)
	{
		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
		sampler.mip = XGPU_VULKAN_MIP_NONE;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
		sampler.anisotropy = 1;
		light_shadows.sampler = xgpu_vulkan_sampler(&sampler);
	}
	if (!light_shadows.vertex || !light_shadows.trace || !light_shadows.filter || !light_shadows.flashlight ||
		!light_shadows.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.shadows \"ray_traced\": the dynamic lights' shadows' passes cannot "
			"be made: their lights unshadowed");
		light_shadows.failed = 1;
		return 0;
	}
	memset(&light_shadows.trace_pass, 0, sizeof(light_shadows.trace_pass));
	light_shadows.trace_pass.vertex_shader = light_shadows.vertex;
	light_shadows.trace_pass.pixel_shader = light_shadows.trace;
	light_shadows.trace_pass.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	light_shadows.trace_pass.color_mask = 15;
	light_shadows.trace_pass.color_format = XGPU_VULKAN_FORMAT_RGBA8;
	light_shadows.trace_pass.samples = 1;
	light_shadows.filter_pass = light_shadows.trace_pass;
	light_shadows.filter_pass.pixel_shader = light_shadows.filter;
	light_shadows.flashlight_pass = light_shadows.trace_pass;
	light_shadows.flashlight_pass.pixel_shader = light_shadows.flashlight;
	light_shadows.flashlight_pass.color_mask = 1;
	light_shadows.flashlight_pass.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	light_shadows.made = 1;
	return 1;
}

void xgpu_vulkan_light_shadows_prepare(int ray_queries)
{
	if (!light_shadows_shaders(ray_queries))
		return;
	(void)xgpu_vulkan_pipeline_prepare(&light_shadows.trace_pass);
	(void)xgpu_vulkan_pipeline_prepare(&light_shadows.filter_pass);
	(void)xgpu_vulkan_pipeline_prepare(&light_shadows.flashlight_pass);
	light_shadows.flashlight_prepared = 1;
}

/* a mask of the window's target's size */
static struct xgpu_vulkan_image *light_shadows_image(struct xgpu_vulkan_image *image, uint32_t width, uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	/* (never one the sets still name) */
	if (xgpu_vulkan_light_shadows_bound(image))
		xgpu_vulkan_light_shadows_set(NULL);
	xgpu_vulkan_image_destroy(image);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = XGPU_VULKAN_FORMAT_RGBA8;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* a pass's triangle over the window's rectangle of a mask: the pre-pass's
inputs, the trace's mask for the filter (at the history's binding), the
pass's uniforms; the lights' block and cells, the occlusion's set */
static void light_shadows_pass_draw_kind(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const struct xgpu_vulkan_pipeline_description *pass,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *traced, int kind)
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	void *bytes;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(pass);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = light_shadows.sampler;
	}
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_INPUTS].image = inputs;
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_HISTORY].image = traced;
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_CASCADES].image = xgpu_vulkan_world_ao_cascades_image();
	draw.textures[XGPU_SPIRV_WORLD_AO_PASS_CASCADES].kind = XGPU_VULKAN_IMAGE_3D;
	/* (the draw set's uniforms' range, which the constants' share) */
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, uniforms, sizeof(*uniforms));
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
	draw.kind[0] = (uint8_t)kind;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

static void light_shadows_pass_draw(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const struct xgpu_vulkan_pipeline_description *pass,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *traced)
{
	light_shadows_pass_draw_kind(into, rectangle, uniforms, pass, inputs, traced, LIGHT_SHADOWS_KIND);
}

void xgpu_vulkan_light_shadows_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const uint32_t (*shadowed)[4])
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *mask;
	int32_t rectangle[4];
	int axis, temporal;

	/* (what the passes read is not what they draw into) */
	xgpu_vulkan_light_shadows_set(NULL);
	if (!shadowed || !shadowed[0][0] || !inputs || !pass || !light_shadows.made)
		return;
	temporal = shadowed[3][1] != 0;
	if (!xgpu_vulkan_pipeline_ready(&light_shadows.trace_pass) ||
		(!temporal && !xgpu_vulkan_pipeline_ready(&light_shadows.filter_pass)) || !xgpu_vulkan_rt_build())
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
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	light_shadows.traced = light_shadows_image(light_shadows.traced, size->width, size->height);
	if (!light_shadows.traced)
		return;
	if (!temporal)
	{
		light_shadows.filtered = light_shadows_image(light_shadows.filtered, size->width, size->height);
		if (!light_shadows.filtered)
			return;
	}
	xgpu_vulkan_pass_owner(LIGHT_SHADOWS_OWNER);
	light_shadows_pass_draw(light_shadows.traced, rectangle, pass, &light_shadows.trace_pass, inputs, NULL);
	mask = light_shadows.traced;
	if (!temporal)
	{
		light_shadows_pass_draw(light_shadows.filtered, rectangle, pass, &light_shadows.filter_pass, inputs,
			light_shadows.traced);
		mask = light_shadows.filtered;
	}
	xgpu_vulkan_pass_owner(NULL);
	xgpu_vulkan_light_shadows_set(mask);
	if (!light_shadows.drawn_said)
	{
		light_shadows.drawn_said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: display.shadows \"ray_traced\": the dynamic lights' shadows traced (%s)",
			temporal ? "turned each frame, the temporal technique's accumulation" : "filtered");
	}
}

void xgpu_vulkan_light_shadows_end(void)
{
	xgpu_vulkan_light_shadows_set(NULL);
}

/* ---------- the enhanced flashlight's shadow map (xgpu.h's
XGPU_DYNAMIC_LIGHTS_FLASHLIGHT; xgpu_spirv.c's flashlight_map_main)

A ray a texel from the shadowed slot's light into the ray-traced scene,
drawn as its frame's lights come (before any surface reads them: the
lights are given before the window's structure is drawn), wherever the
device has ray queries, whatever display.shadows and
display.ambient_occlusion are. Its pipeline is made the first time (one
of a fixed target, no map's draws'); until it is, nothing is drawn and
the map holds what it last held (as far as it goes, at first). */
_Static_assert(XGPU_VULKAN_BINDING_FLASHLIGHT_SHADOW == XGPU_SPIRV_BINDING_FLASHLIGHT_SHADOW &&
	XGPU_VULKAN_FLASHLIGHT_SHADOW_SIZE == XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE, "the flashlight's shadow map");

#define FLASHLIGHT_OWNER "FLASHLIGHT SHADOW"
#define FLASHLIGHT_KIND 0xfb

int xgpu_vulkan_flashlight_draw(const unsigned char *cells)
{
	enum
	{
		HEAD = ((XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y * XGPU_DYNAMIC_LIGHTS_GRID_Z) +
			XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1) * 16,
	};
	static const struct xgpu_world_ao_pass none;
	struct xgpu_vulkan_image *map = xgpu_vulkan_flashlight_shadow_image();
	const int32_t rectangle[4] = { 0, 0, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE,
		XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE };
	uint32_t slot;

	if (!cells || !map || xgpu_vulkan_image_description(map)->width != XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE)
		return 0;
	memcpy(&slot, cells + HEAD, sizeof(slot));
	if (!slot || slot > XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS || !light_shadows_shaders(1))
		return 0;
	if (!light_shadows.flashlight_prepared)
	{
		(void)xgpu_vulkan_pipeline_prepare(&light_shadows.flashlight_pass);
		light_shadows.flashlight_prepared = 1;
	}
	if (!xgpu_vulkan_pipeline_ready(&light_shadows.flashlight_pass) || !xgpu_vulkan_rt_build())
		return 0;
	xgpu_vulkan_pass_owner(FLASHLIGHT_OWNER);
	light_shadows_pass_draw_kind(map, rectangle, &none, &light_shadows.flashlight_pass, NULL, NULL, FLASHLIGHT_KIND);
	xgpu_vulkan_pass_owner(NULL);
	if (!light_shadows.flashlight_said)
	{
		light_shadows.flashlight_said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: the enhanced flashlight's shadow map traced (%dx%d)",
			XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE);
	}
	return 1;
}
