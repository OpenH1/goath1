/*
XGPU_VULKAN_CONTACT_SHADOWS.C

display.contact_shadows in the Vulkan backend (docs/dynamic-shadows-plan.md,
stage 2; tools/vulkan_post_shaders.py's CONTACT_SHADOWS): the small, sharp
shadows where an object meets the level (under its feet, against a wall),
for GPUs without ray queries, and where display.shadows is not
"ray_traced". One triangle over a window's 3D view after its lightmap pass:
each pixel's point from the depth, and for each caster whose volume holds
it (the ray-traced shadows' casters: render_objects.c's, with the classic
shadow's direction and darkness), a march of 12 steps over half a world
unit toward the caster's light, each step projected into the depth; the
first step behind what the depth holds there, by less than a guessed
thickness, shadows the pixel, more the nearer it is. The level's pixels
alone (the stencil's first bit is the models': the classic receivers'
test), as the ray-traced pass's are. Drawn after the classic shadows'
receivers, it writes the lesser of what they left and the light taken
before them (xgpu_vulkan_stream.c's contact_shadows_light) times its own
shadow: a pixel in both is darkened by the darker alone, not twice.

It fades out between 8 and 20 world units from the camera. Its steps are the
same for every pixel, with no noise (#1290: nothing after it averages noise
away under SMAA, so it stayed a dotted fringe around bodies that crawled as
the view moved); where the march went behind the depth is found between two
steps, and the shadow fades to nothing at the march's end. A pixel's normal
is from the nearer neighbour on each side, never across an object's outline.
Its pipeline is made as a map is prepared (a record of no casters), for
each of the map's targets. Off screen nothing casts, and a caster's
thickness is guessed: the plan's limits of a screen-space pass.
*/

#include "xgpu_vulkan_contact_shadows.h"
#include "host.h"
#include "xgpu_backend.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan_post_shaders.h"

#include <math.h>
#include <string.h>

_Static_assert(XGPU_VULKAN_REVERSED_DEPTH == 1, "the pass reads the depth reversed (CONTACT_SHADOWS' REVERSED_DEPTH)");
_Static_assert(sizeof(struct xgpu_stream_rt_shadow_caster) == 32, "a caster is two vec4s of the block");

/* the march: its length and the thickness a step may be behind the depth
(world units), its start off the surface along its normal, and the depth's
bias, a share of the camera's distance; the fade's start and end from the
camera; the strength */
#define CONTACT_LENGTH 0.5f
#define CONTACT_THICKNESS 0.25f
#define CONTACT_START 0.01f
#define CONTACT_BIAS 0.002f
#define CONTACT_FADE_NEAR 8.0f
#define CONTACT_FADE_FAR 20.0f
#define CONTACT_STRENGTH 1.0f

/* the block, as CONTACT_SHADOWS reads it (std140) */
struct contact_block
{
	float unproject[4][4];
	float project[4][4];
	float camera[4];
	float march[4];
	float view[4];
	float bounds[4];
	float reserved[4][4];
	struct xgpu_stream_rt_shadow_caster casters[XGPU_VULKAN_CONTACT_SHADOWS_CASTERS];
};

_Static_assert(sizeof(struct contact_block) == XGPU_VULKAN_CONTACT_SHADOWS_BLOCK, "the pass's block");

static struct
{
	VkShaderModule vertex, pixel;
	VkSampler sampler;
	int failed;
} contact;

static int contact_shaders(void)
{
	if (contact.failed)
		return 0;
	if (contact.vertex && contact.pixel && contact.sampler)
		return 1;
	if (!contact.vertex)
		contact.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!contact.pixel)
		contact.pixel = xgpu_vulkan_shader(xgpu_vulkan_post_contact_shadows.words,
			xgpu_vulkan_post_contact_shadows.count);
	if (!contact.sampler)
	{
		struct xgpu_vulkan_sampler_description sampler;

		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
		sampler.mip = XGPU_VULKAN_MIP_NONE;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
		sampler.anisotropy = 1;
		contact.sampler = xgpu_vulkan_sampler(&sampler);
	}
	if (!contact.vertex || !contact.pixel || !contact.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.contact_shadows' pass cannot be made: no contact shadows drawn");
		contact.failed = 1;
		return 0;
	}
	return 1;
}

int xgpu_vulkan_contact_shadows_pipeline(uint8_t color_format, uint8_t depth_format, uint8_t samples,
	struct xgpu_vulkan_pipeline_description *pipeline)
{
	if (!contact_shaders())
		return 0;
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex_shader = contact.vertex;
	pipeline->pixel_shader = contact.pixel;
	pipeline->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	/* (the lesser of the pixel and the light before the classic receivers
	times the contact shadow, its alpha the game's) */
	pipeline->blend = 1;
	pipeline->blend_source = XGPU_BLEND_ONE;
	pipeline->blend_destination = XGPU_BLEND_ONE;
	pipeline->blend_operation = XGPU_BLEND_MIN;
	pipeline->color_mask = 7;
	/* (the level's pixels alone: the models' have the stencil's first bit) */
	pipeline->stencil_test = 1;
	pipeline->stencil_function = XGPU_COMPARE_EQUAL;
	pipeline->stencil_fail = pipeline->stencil_depth_fail = pipeline->stencil_pass = XGPU_STENCIL_KEEP;
	pipeline->stencil_read_mask = 1;
	pipeline->stencil_write_mask = 0;
	pipeline->color_format = color_format;
	pipeline->depth_format = depth_format;
	pipeline->samples = samples ? samples : 1;
	return 1;
}

VkSampler xgpu_vulkan_contact_shadows_sampler(void)
{
	return contact_shaders() ? contact.sampler : VK_NULL_HANDLE;
}

int xgpu_vulkan_contact_shadows_block(const struct xgpu_stream_rt_shadows *record,
	const struct xgpu_vulkan_dynamic_state *dynamic, void *bytes)
{
	struct contact_block *block = bytes;
	const float (*c)[4] = (const float (*)[4])record->projection;
	double forward;
	int count;

	forward = sqrt((double)c[3][0] * c[3][0] + (double)c[3][1] * c[3][1] + (double)c[3][2] * c[3][2]);
	if (!(forward > 0.0))
		return 0;
	memset(block, 0, sizeof(*block));
	/* (as the game's programs place a point, and back: the ray-traced
	shadows' pass's own, xgpu_vulkan_stream.c) */
	if (!xgpu_vulkan_shadows_window(record, dynamic, block->project, block->unproject))
		return 0;
	count = record->count < 0 ? 0 : record->count > XGPU_VULKAN_CONTACT_SHADOWS_CASTERS ?
		XGPU_VULKAN_CONTACT_SHADOWS_CASTERS : record->count;
	memcpy(block->camera, record->camera, sizeof(record->camera));
	block->camera[3] = (float)count;
	block->march[0] = CONTACT_LENGTH;
	block->march[1] = CONTACT_THICKNESS;
	block->march[2] = CONTACT_START;
	block->march[3] = CONTACT_BIAS;
	block->view[0] = CONTACT_FADE_NEAR;
	block->view[1] = 1.0f / (CONTACT_FADE_FAR - CONTACT_FADE_NEAR);
	block->view[2] = (float)(1.0 / forward);
	block->view[3] = CONTACT_STRENGTH;
	block->bounds[0] = (float)dynamic->viewport[0];
	block->bounds[1] = (float)dynamic->viewport[1];
	block->bounds[2] = (float)(dynamic->viewport[0] + dynamic->viewport[2]);
	block->bounds[3] = (float)(dynamic->viewport[1] + dynamic->viewport[3]);
	memcpy(block->casters, record->casters, (size_t)count * sizeof(block->casters[0]));
	return 1;
}
