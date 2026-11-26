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
thickness, shadows the pixel, more the nearer it is. The colour is
multiplied into the pixel, the level's alone (the stencil's first bit is
the models': the classic receivers' test), as the ray-traced pass's is.

It fades out between 8 and 20 world units from the camera, and its steps
start at the same noise every frame (no history, no shimmer while still).
Its pipeline is made as a map is prepared (a record of no casters), for
each of the map's targets. Off screen nothing casts, and a caster's
thickness is guessed: the plan's limits of a screen-space pass.
*/

#include "xgpu_vulkan_contact_shadows.h"
#include "xgpu_vulkan_post_shaders.h"
#include "host.h"
#include "../src/xgpu_stream.h"

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
	/* (multiplied into the pixel, its alpha the game's) */
	pipeline->blend = 1;
	pipeline->blend_source = XGPU_BLEND_ZERO;
	pipeline->blend_destination = XGPU_BLEND_SOURCE_COLOR;
	pipeline->blend_operation = XGPU_BLEND_ADD;
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

/* the 4x4 inverse of a (rows), into inverse (rows); 0 if it has none */
static int contact_invert(const double a[4][4], double inverse[4][4])
{
	double m[4][8];
	int row, column, pivot, other;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			m[row][column] = a[row][column];
			m[row][column + 4] = row == column ? 1.0 : 0.0;
		}
	}
	for (column = 0; column < 4; column++)
	{
		double scale;

		pivot = column;
		for (row = column + 1; row < 4; row++)
		{
			if (fabs(m[row][column]) > fabs(m[pivot][column]))
				pivot = row;
		}
		if (fabs(m[pivot][column]) < 1.0e-300)
			return 0;
		for (other = 0; other < 8 && pivot != column; other++)
		{
			double swap = m[column][other];

			m[column][other] = m[pivot][other];
			m[pivot][other] = swap;
		}
		scale = 1.0 / m[column][column];
		for (other = 0; other < 8; other++)
			m[column][other] *= scale;
		for (row = 0; row < 4; row++)
		{
			double factor = m[row][column];

			if (row == column || factor == 0.0)
				continue;
			for (other = 0; other < 8; other++)
				m[row][other] -= factor * m[column][other];
		}
	}
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
			inverse[row][column] = m[row][column + 4];
	}
	return 1;
}

int xgpu_vulkan_contact_shadows_block(const struct xgpu_stream_rt_shadows *record,
	const struct xgpu_vulkan_dynamic_state *dynamic, void *bytes)
{
	struct contact_block *block = bytes;
	/* (the world's projection, the screen conversion and the viewport's
	scale and offset, as the game's programs place a point: the glows'
	vertex shader, and the ray-traced shadows' pass) */
	const float (*c)[4] = (const float (*)[4])record->projection;
	const float offsets[3] = { 0.5f + record->screen_offset, 0.5f, 0.0f };
	double ndc[3][4], window[4][4], inverse[4][4];
	double near_z = dynamic->depth_range[0], far_z = dynamic->depth_range[1];
	double forward;
	int axis, column, count;

	for (axis = 0; axis < 3; axis++)
	{
		double scale = record->viewport_scale[axis] != 0.0f ? record->viewport_scale[axis] : 1.0f;
		double shift = (double)c[5][axis] + offsets[axis] - record->viewport_offset[axis];

		for (column = 0; column < 4; column++)
			ndc[axis][column] = ((double)c[4][axis] * c[axis][column] + shift * c[3][column]) / scale;
	}
	for (column = 0; column < 4; column++)
	{
		double w = c[3][column];

		window[0][column] = dynamic->viewport[0] * w + (ndc[0][column] + w) * 0.5 * dynamic->viewport[2];
		/* (y down: the programs' y negated) */
		window[1][column] = dynamic->viewport[1] * w + (w - ndc[1][column]) * 0.5 * dynamic->viewport[3];
		window[2][column] = near_z * w + ndc[2][column] * (far_z - near_z);
		window[3][column] = w;
	}
	if (!contact_invert((const double (*)[4])window, inverse))
		return 0;
	forward = sqrt((double)c[3][0] * c[3][0] + (double)c[3][1] * c[3][1] + (double)c[3][2] * c[3][2]);
	if (!(forward > 0.0))
		return 0;
	memset(block, 0, sizeof(*block));
	for (axis = 0; axis < 4; axis++)
	{
		for (column = 0; column < 4; column++)
		{
			block->unproject[axis][column] = (float)inverse[axis][column];
			block->project[axis][column] = (float)window[axis][column];
		}
	}
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
