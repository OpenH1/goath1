/*
XGPU_VULKAN_POST.C

The post-process passes of display.anti_aliasing (xgpu_vulkan_post.h): FXAA
and SMAA, run on a render target in place, over the rectangle of a window's
3D view, their GLSL compiled to SPIR-V (xgpu_vulkan_post_shaders.c, by
tools/vulkan_post_shaders.py). Each pass draws one triangle covering the
rectangle (its corners from the vertex's index, with no vertex data) and
finds its texels from the fragment's coordinates; the target's rows are
the picture's from the top, as the passes' own images' are, so no
coordinate is turned over. No pass reads what it writes. Over all of a
target of one layer (xgpu_vulkan_post_anti_alias's moved, the caller's
handle of the target), the last pass draws the view into an image of the
target's own description, the spare, which then takes the target's place,
the target's image becoming the spare: the view drawn from one image into
the other, never copied. Over part of a target (a split screen's window),
the target's pixels in the rectangle are copied first and the last pass
draws them back into the target: drawn whole into another image, the view
would cost more than the copy of the rectangle. FXAA and SMAA's blend write
every pixel, with its own alpha (the game keeps values of its own in
destination alpha), so the spare holds all of the target's pixels: none
discards one (FXAA's pixel with no edge writes its own texel back).

Each pass is a pass of the core's (xgpu_vulkan.c) of its own: the barrier
that ends one makes what it drew visible to the next one's samples, and
the copy's barrier its pixels to the first. The spare is cleared whole as
the last pass begins (a clear of the whole attachment loads none of it),
then drawn over all of it. SMAA's edges and weights are
emptied whole before their passes, as the pass begins (a clear of the
whole attachment loads none of it), so that no edge of another window's
pass is found: they write only where there is an edge. The intermediate
images are the target's size (the shaders' coordinates are the
target's).

As SMAA's own design has it, the edges' pass marks its pixels in a
stencil (emptied with the edges; the edge shader discards a pixel with no
edge, which then writes neither), and the weights' pass runs only where
it is marked: elsewhere the shader would write zero over the zero the
clear left, so the weights are the same, without the searches' cost over
every pixel. The blend runs over every pixel still: where its weights
(its own, its right and its lower neighbor's) are zero it writes the
copy's texel back unchanged.

The present step's turn (xgpu_vulkan_post_rotate) is a pass of the same
kind: the frame drawn turned by quarter turns into an image of its own, for
a swapchain made for the display's orientation while the phone is held
another way (xgpu_vulkan.c, present). Its pipeline is made at the start,
where the window's surface can be turned (xgpu_vulkan_post_rotate_prepare).

The pipelines and SMAA's lookup textures are made as the setting is chosen
(xgpu_vulkan_post_prepare), or queued as a map is prepared, not in the
middle of a frame. The lookup textures are RGBA8 images of SMAA's one and
two channels (the rest zero), sampled as an R8 and an RG8 texture would be.
FXAA's and SMAA's blend into a target of its own format (display.hdr's
half floats too); the copy is the target's format, the spare the target's
description, SMAA's edges and weights 8 bits.

display.hdr's tone map (xgpu_vulkan_post_tone_map) is no pass of its own:
a draw in the pass that drew the view, reading each pixel where it is drawn
as the core's local reads let a draw (as Metal's reads tile memory), each
sample's own of a multisampled target, writing its colour (the game keeps
values of its own in alpha). Its pipeline is made as HDR is chosen, or
queued as a map is prepared. The frame's last pass
(xgpu_vulkan_post_encode, #1108) is a pass like the turn's: the frame drawn
at the picture's size in the swapchain (scaled up sharply in the pass, as
present_sharp's blits would), encoded for the swapchain (HDR's encoding, or
display.gamma's curve over an SDR frame) and dithered to its levels, into
an image of the swapchain's format, which the core then copies 1:1 into
the swapchain (a blit has no arithmetic, and would scale the dither's
pattern). Its pipelines are made at the start
(xgpu_vulkan_post_encode_prepare).
*/

#include "xgpu_vulkan_post.h"
#include "host.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_post_shaders.h"
#include "../src/xgpu_hdr.h"
#include "../src/xgpu_stream.h"

#include <stdlib.h>
#include <string.h>

/* the draw set's constants' range (xgpu_vulkan.c draw_set_make), which the
uniforms' (2048 bytes) fits in: one range of the ring for both */
#define UNIFORMS_RANGE (192 * 16)

static struct
{
	VkShaderModule vertex, pixel[XGPU_VULKAN_POST_PROGRAM_COUNT];
	struct xgpu_vulkan_pipeline_description pipelines[XGPU_VULKAN_POST_PROGRAM_COUNT];
	/* FXAA's and SMAA's blend into display.hdr's half floats, and into
	display.output_depth's 16-bit channels */
	struct xgpu_vulkan_pipeline_description half_float[XGPU_VULKAN_POST_PROGRAM_COUNT];
	struct xgpu_vulkan_pipeline_description deep[XGPU_VULKAN_POST_PROGRAM_COUNT];
	/* FXAA's, and SMAA's, pipelines or textures could not be made */
	int failed[2];
	/* the size and format of the images below */
	uint32_t width, height;
	uint8_t format;
	/* the target's pixels before the pass, and SMAA's edges and blending
	weights, and the stencil marking the edges' pixels (a stencil alone,
	XGPU_VULKAN_FORMAT_S8) */
	struct xgpu_vulkan_image *color, *edges, *weights, *marks;
	/* the image a pass over all of a target draws the view into, of the
	target's description, given the target's place (its image then kept
	here) */
	struct xgpu_vulkan_image *spare;
	/* SMAA's lookup textures */
	struct xgpu_vulkan_image *area, *search;
	/* linear and clamped, as FXAA and SMAA sample */
	VkSampler sampler;
	/* the present step's turn: its pipeline (made at the start, never
	while a map is prepared), whether it cannot be, and the turned frame */
	VkPipeline rotate;
	int rotate_failed;
	struct xgpu_vulkan_image *turned;
	/* display.hdr: the tone map's pipeline could not be made (logged
	once); the last pass's pipelines (ENCODE_FORMATS'), and its image */
	int tone_map_failed;
	VkPipeline encode[3];
	struct xgpu_vulkan_image *encoded;
	/* display.panini's pipelines could not be made (logged once) */
	int panini_failed;
	/* the loading screen's sharp frame's pipelines (made at the start) */
	VkPipeline loading[3];
} post;

/* ---------- images */

static struct xgpu_vulkan_image *image_make(uint8_t format, uint8_t usage, uint32_t width, uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = usage;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* a lookup texture of SMAA's, its channels' bytes (four to a word) each
made a texel of four */
static struct xgpu_vulkan_image *lookup_texture(const uint32_t *words, uint32_t width, uint32_t height,
	uint32_t channels)
{
	struct xgpu_vulkan_image *image = image_make(XGPU_VULKAN_FORMAT_RGBA8, XGPU_VULKAN_USAGE_SAMPLED, width, height);
	unsigned char *texels = malloc(width * height * 4);
	uint32_t texel, channel, byte;

	if (!image || !texels)
	{
		free(texels);
		xgpu_vulkan_image_destroy(image);
		return NULL;
	}
	for (texel = 0; texel < width * height; texel++)
	{
		for (channel = 0; channel < 4; channel++)
		{
			byte = texel * channels + channel;
			texels[texel * 4 + channel] = channel < channels ?
				(unsigned char)(words[byte / 4] >> (byte % 4 * 8)) : 0;
		}
	}
	xgpu_vulkan_image_upload(image, 0, 0, 0, 0, 0, width, height, 1, texels, width * 4, width * height * 4);
	free(texels);
	return image;
}

/* SMAA's images let go (FXAA chosen instead) */
static void smaa_images_release(void)
{
	xgpu_vulkan_image_destroy(post.edges);
	xgpu_vulkan_image_destroy(post.weights);
	xgpu_vulkan_image_destroy(post.marks);
	post.edges = post.weights = post.marks = NULL;
}

/* the passes' images, of the target's size (SMAA's only for SMAA, the
copy only where it is made); color is the target's format, so that the copy
is exact */
static int images_fit(int smaa, int copy, uint8_t format, uint32_t width, uint32_t height)
{
	if (post.width != width || post.height != height || post.format != format)
	{
		xgpu_vulkan_image_destroy(post.color);
		xgpu_vulkan_image_destroy(post.edges);
		xgpu_vulkan_image_destroy(post.weights);
		xgpu_vulkan_image_destroy(post.marks);
		post.color = post.edges = post.weights = post.marks = NULL;
		post.width = width;
		post.height = height;
		post.format = format;
	}
	if (copy && !post.color)
		post.color = image_make(format, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET, width, height);
	if (smaa && !post.edges)
	{
		post.edges = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
		post.weights = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
		post.marks = image_make(XGPU_VULKAN_FORMAT_S8, XGPU_VULKAN_USAGE_TARGET, width, height);
	}
	return (!copy || post.color) && (!smaa || (post.edges && post.weights && post.marks));
}

/* the spare a pass over the rectangle of a layer of a target draws the
view into: where moved is the caller's handle of the target and the
rectangle all of a target of one layer and one sample, the spare (of the
target's description, made again where it is not); else NULL, the target
copied and drawn into */
static struct xgpu_vulkan_image *spare_for(struct xgpu_vulkan_image *target, uint32_t layer,
	const int32_t rectangle[4], struct xgpu_vulkan_image **moved)
{
	const struct xgpu_vulkan_image_description *description = xgpu_vulkan_image_description(target);

	if (!moved || *moved != target || layer || description->depth_or_layers > 1 || description->samples > 1 ||
		description->type == XGPU_VULKAN_IMAGE_3D || description->type == XGPU_VULKAN_IMAGE_CUBE ||
		rectangle[0] || rectangle[1] || rectangle[2] != (int32_t)description->width ||
		rectangle[3] != (int32_t)description->height)
		return NULL;
	if (post.spare && memcmp(xgpu_vulkan_image_description(post.spare), description, sizeof(*description)))
	{
		xgpu_vulkan_image_destroy(post.spare);
		post.spare = NULL;
	}
	if (!post.spare)
		post.spare = xgpu_vulkan_image_create(description);
	return post.spare;
}

/* the spare emptied whole as the pass drawing over all of it begins (none
of it loaded) */
static void spare_clear(void)
{
	static const float transparent[4];
	const struct xgpu_vulkan_image_description *description = xgpu_vulkan_image_description(post.spare);
	int32_t whole[1][4];

	whole[0][0] = whole[0][1] = 0;
	whole[0][2] = (int32_t)description->width;
	whole[0][3] = (int32_t)description->height;
	xgpu_vulkan_set_targets(post.spare, NULL, 0);
	xgpu_vulkan_clear((const int32_t (*)[4])whole, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS, transparent,
		1.0f, 0);
}

/* the view drawn into the spare: the spare in the caller's handle, the
target's image the spare */
static void spare_take_place(struct xgpu_vulkan_image **moved)
{
	struct xgpu_vulkan_image *view = post.spare;

	post.spare = *moved;
	*moved = view;
}

/* ---------- pipelines */

/* the passes' sampler, made the first time: 1 if it is */
static int sampler_make(void)
{
	struct xgpu_vulkan_sampler_description sampler;

	if (post.sampler)
		return 1;
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	post.sampler = xgpu_vulkan_sampler(&sampler);
	return post.sampler != VK_NULL_HANDLE;
}

static int prepare_failed(int smaa, const char *what)
{
	host_logf(HOST_LOG_WARN, "vulkan: %s's %s cannot be made, so the 3D view is not antialiased",
		smaa ? "SMAA" : "FXAA", what);
	post.failed[smaa] = 1;
	return 0;
}

/* a pass's pipeline for a target of a format: FXAA's and SMAA's blend into
the target's (display.hdr's half floats, display.output_depth's 16 bits, or
8 bits), the rest into their own 8-bit images */
static struct xgpu_vulkan_pipeline_description *pipeline_of(int program, uint8_t format)
{
	int into_target = program == XGPU_VULKAN_POST_FXAA || program == XGPU_VULKAN_POST_SMAA_BLEND ||
		program == XGPU_VULKAN_POST_PANINI;

	return into_target && format == XGPU_VULKAN_FORMAT_RGBA16F ? &post.half_float[program] :
		into_target && format == XGPU_VULKAN_FORMAT_RGBA16 ? &post.deep[program] : &post.pipelines[program];
}

/* a target's format the passes draw into: its own, BGRA8 for any other */
static uint8_t target_format(uint8_t format)
{
	return format == XGPU_VULKAN_FORMAT_RGBA16F || format == XGPU_VULKAN_FORMAT_RGBA16 ? format :
		XGPU_VULKAN_FORMAT_BGRA8;
}

static int target_format_drawn(uint8_t format)
{
	return format == XGPU_VULKAN_FORMAT_BGRA8 || format == XGPU_VULKAN_FORMAT_RGBA16F ||
		format == XGPU_VULKAN_FORMAT_RGBA16;
}

/* the core's comparisons and stencil operations (xgpu_backend.h's
XGPU_COMPARE_* and XGPU_STENCIL_*: xgpu_vulkan.c compare_op, stencil_op) */
enum { COMPARE_EQUAL = 2, COMPARE_ALWAYS = 7, STENCIL_REPLACE = 2 };

/* SMAA's edges' pass marks its pixels in the stencil (the shader discards
a pixel with no edge); the weights' pass runs where they are marked. No
depth is tested or written */
static void marks_describe(int program, struct xgpu_vulkan_pipeline_description *description)
{
	if (program != XGPU_VULKAN_POST_SMAA_EDGES && program != XGPU_VULKAN_POST_SMAA_WEIGHTS)
		return;
	description->depth_format = XGPU_VULKAN_FORMAT_S8;
	description->stencil_test = 1;
	description->stencil_read_mask = 0xff;
	if (program == XGPU_VULKAN_POST_SMAA_EDGES)
	{
		description->stencil_function = COMPARE_ALWAYS;
		description->stencil_pass = STENCIL_REPLACE;
		description->stencil_write_mask = 0xff;
	}
	else
		description->stencil_function = COMPARE_EQUAL;
}

int xgpu_vulkan_post_prepare(int smaa, uint8_t format, int preparing)
{
	int first = smaa ? XGPU_VULKAN_POST_SMAA_EDGES : XGPU_VULKAN_POST_FXAA;
	int last = smaa ? XGPU_VULKAN_POST_SMAA_BLEND : XGPU_VULKAN_POST_FXAA;
	int program;

	smaa = smaa != 0;
	format = target_format(format);
	if (post.failed[smaa])
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	for (program = first; program <= last; program++)
	{
		struct xgpu_vulkan_pipeline_description *description = pipeline_of(program, format);
		int into_target = program == XGPU_VULKAN_POST_FXAA || program == XGPU_VULKAN_POST_SMAA_BLEND;

		if (!post.pixel[program])
			post.pixel[program] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[program].words,
				xgpu_vulkan_post_programs[program].count);
		if (!post.vertex || !post.pixel[program])
			return prepare_failed(smaa, "shaders");
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[program];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		/* (every channel: into the target, each pixel's own alpha) */
		description->color_mask = 15;
		description->color_format = into_target ? format : XGPU_VULKAN_FORMAT_BGRA8;
		description->samples = 1;
		marks_describe(program, description);
		if (preparing ? !xgpu_vulkan_pipeline_prepare(description) : !xgpu_vulkan_pipeline(description))
			return prepare_failed(smaa, "pipelines");
	}
	if (smaa && !post.area)
	{
		post.area = lookup_texture(xgpu_vulkan_post_area_texture, XGPU_VULKAN_POST_AREA_WIDTH,
			XGPU_VULKAN_POST_AREA_HEIGHT, 2);
		post.search = lookup_texture(xgpu_vulkan_post_search_texture, XGPU_VULKAN_POST_SEARCH_WIDTH,
			XGPU_VULKAN_POST_SEARCH_HEIGHT, 1);
		if (!post.area || !post.search)
			return prepare_failed(smaa, "lookup textures");
	}
	if (!sampler_make())
		return prepare_failed(smaa, "sampler");
	return 1;
}

int xgpu_vulkan_post_rotate_prepare(void)
{
	struct xgpu_vulkan_pipeline_description *description = &post.pipelines[XGPU_VULKAN_POST_ROTATE];

	if (post.rotate)
		return 1;
	if (post.rotate_failed)
		return 0;
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_ROTATE])
		post.pixel[XGPU_VULKAN_POST_ROTATE] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[XGPU_VULKAN_POST_ROTATE].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_ROTATE].count);
	if (post.vertex && post.pixel[XGPU_VULKAN_POST_ROTATE] && sampler_make())
	{
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[XGPU_VULKAN_POST_ROTATE];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		description->color_mask = 15;
		description->color_format = XGPU_VULKAN_FORMAT_BGRA8;
		description->samples = 1;
		post.rotate = xgpu_vulkan_pipeline(description);
	}
	if (!post.rotate)
	{
		host_logf(HOST_LOG_WARN, "vulkan: the frame cannot be turned for the display: the compositor turns it");
		post.rotate_failed = 1;
	}
	return post.rotate != VK_NULL_HANDLE;
}

/* ---------- the passes */

/* a program's triangle drawn into a layer of an image over the rectangle,
sampling images (NULL: none) */
static void pass_draw_with(int program, struct xgpu_vulkan_image *into, uint32_t layer, const int32_t rectangle[4],
	struct xgpu_vulkan_image *image0, struct xgpu_vulkan_image *image1, struct xgpu_vulkan_image *image2,
	const float bounds[4], const float window[4], const float panini[8])
{
	/* (SMAA's edges and weights with the stencil of their marks) */
	struct xgpu_vulkan_image *marks = program == XGPU_VULKAN_POST_SMAA_EDGES ||
		program == XGPU_VULKAN_POST_SMAA_WEIGHTS ? post.marks : NULL;
	uint8_t format = xgpu_vulkan_image_description(into)->format;
	uint32_t into_samples = xgpu_vulkan_image_description(into)->samples;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_image *images[3];
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage;

	memset(&draw, 0, sizeof(draw));
	/* (of the samples of the image drawn into: a multisampled attachment's
	every sample given its pixel's value) */
	pipeline = *pipeline_of(program, format);
	pipeline.samples = (uint8_t)(into_samples > 1 ? into_samples : 1);
	draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
	if (!draw.pipeline)
		return;
	images[0] = image0;
	images[1] = image1;
	images[2] = image2;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage < 3 ? images[stage] : NULL;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)post.width;
	uniforms->metrics[1] = 1.0f / (float)post.height;
	uniforms->metrics[2] = (float)post.width;
	uniforms->metrics[3] = (float)post.height;
	if (bounds)
		memcpy(uniforms->bounds, bounds, sizeof(uniforms->bounds));
	if (window)
		memcpy(uniforms->window, window, sizeof(uniforms->window));
	if (panini)
	{
		memcpy(uniforms->panini, panini, sizeof(uniforms->panini));
		memcpy(uniforms->tangents, panini + 4, sizeof(uniforms->tangents));
	}
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	dynamic.stencil_reference = 1;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(into, marks, layer, 1);
	xgpu_vulkan_draw(&draw);
}

static void pass_draw(int program, struct xgpu_vulkan_image *into, uint32_t layer, const int32_t rectangle[4],
	struct xgpu_vulkan_image *image0, struct xgpu_vulkan_image *image1, struct xgpu_vulkan_image *image2,
	const float bounds[4])
{
	pass_draw_with(program, into, layer, rectangle, image0, image1, image2, bounds, NULL, NULL);
}

/* an intermediate emptied, all of it, so that no edge of another window's
pass is found: SMAA's passes write only where there is an edge; with the
edges, the stencil of their marks; with the weights, the marks bound as
their pass will have them, and kept. The marks' depth (where the device
has no stencil alone), which nothing reads, cleared with each (so that
none of it is loaded or kept) */
static void intermediate_clear(struct xgpu_vulkan_image *image, int marks)
{
	static const float transparent[4];
	int32_t whole[1][4];

	whole[0][0] = whole[0][1] = 0;
	whole[0][2] = (int32_t)post.width;
	whole[0][3] = (int32_t)post.height;
	xgpu_vulkan_set_targets(image, post.marks, 0);
	xgpu_vulkan_clear((const int32_t (*)[4])whole, 1, XGPU_VULKAN_CLEAR_COLOR | XGPU_VULKAN_CLEAR_CHANNELS |
		XGPU_VULKAN_CLEAR_DEPTH | (marks ? XGPU_VULKAN_CLEAR_STENCIL : 0), transparent, 1.0f, 0);
}

int xgpu_vulkan_post_anti_alias(int smaa, struct xgpu_vulkan_image *target, uint32_t layer, const int32_t corners[4],
	struct xgpu_vulkan_image **moved)
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_image *source, *into;
	uint32_t into_layer;
	int32_t rectangle[4];
	int index;

	smaa = smaa != 0;
	if (!target)
		return 0;
	description = xgpu_vulkan_image_description(target);
	if (!target_format_drawn(description->format) || description->samples > 1)
		return 0;
	/* (made already, as the setting was chosen) */
	if (!xgpu_vulkan_post_prepare(smaa, description->format, 0))
		return 0;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	/* (SMAA's images kept only while SMAA is chosen) */
	if (!smaa && post.edges)
		smaa_images_release();
	into = spare_for(target, layer, rectangle, moved);
	if (!images_fit(smaa, !into, description->format, description->width, description->height))
		return 0;

	/* the view drawn from the target into the spare; or the target's
	pixels copied, read while the target is drawn into */
	source = target;
	into_layer = 0;
	if (!into)
	{
		xgpu_vulkan_image_copy_rectangle(target, layer, post.color, rectangle);
		source = post.color;
		into = target;
		into_layer = layer;
	}
	if (!smaa)
	{
		float bounds[4];

		/* (split screen: no texel of the next window is read) */
		bounds[0] = ((float)rectangle[0] + 0.5f) / (float)post.width;
		bounds[1] = ((float)rectangle[1] + 0.5f) / (float)post.height;
		bounds[2] = ((float)rectangle[2] - 0.5f) / (float)post.width;
		bounds[3] = ((float)rectangle[3] - 0.5f) / (float)post.height;
		if (into == post.spare)
			spare_clear();
		pass_draw(XGPU_VULKAN_POST_FXAA, into, into_layer, rectangle, source, NULL, NULL, bounds);
	}
	else
	{
		intermediate_clear(post.edges, 1);
		pass_draw(XGPU_VULKAN_POST_SMAA_EDGES, post.edges, 0, rectangle, source, NULL, NULL, NULL);

		intermediate_clear(post.weights, 0);
		pass_draw(XGPU_VULKAN_POST_SMAA_WEIGHTS, post.weights, 0, rectangle, post.edges, post.area, post.search, NULL);

		if (into == post.spare)
			spare_clear();
		pass_draw(XGPU_VULKAN_POST_SMAA_BLEND, into, into_layer, rectangle, source, post.weights, NULL, NULL);
	}
	if (into == post.spare)
		spare_take_place(moved);
	return 1;
}

/* ---------- display.panini (port/linux/game/panini.c)

The window's 3D view drawn again through the Panini projection, a pass like
FXAA's: each pixel of the rectangle drawn from the view where the
rectilinear view saw what the projection puts there
(tools/vulkan_post_shaders.py's PANINI_SOURCE), over all of a target into
the spare, which takes the target's place, else from a copy of the
rectangle into the target. All four channels: the
game's own values in destination alpha belong to the pixels they are moved
with. With multisampling the copy is of the resolved pixels and the pass
draws into the multisampled attachment, which the HUD is drawn into after
and which is resolved over the target's pixels at the frame's end: drawn
into the resolved pixels alone, the pass would be lost to that resolve. */

int xgpu_vulkan_post_panini_prepare(uint8_t format, uint32_t samples, int preparing)
{
	struct xgpu_vulkan_pipeline_description *description, multisampled;

	format = target_format(format);
	if (post.panini_failed)
		return 0;
	description = pipeline_of(XGPU_VULKAN_POST_PANINI, format);
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[XGPU_VULKAN_POST_PANINI])
		post.pixel[XGPU_VULKAN_POST_PANINI] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PANINI].words,
			xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PANINI].count);
	if (post.vertex && post.pixel[XGPU_VULKAN_POST_PANINI] && sampler_make())
	{
		memset(description, 0, sizeof(*description));
		description->vertex_shader = post.vertex;
		description->pixel_shader = post.pixel[XGPU_VULKAN_POST_PANINI];
		description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
		description->color_mask = 15;
		description->color_format = format;
		description->samples = 1;
		/* (the pipeline of the target's samples: pass_draw_with's) */
		multisampled = *description;
		multisampled.samples = (uint8_t)(samples > 1 ? samples : 1);
		if (preparing ? xgpu_vulkan_pipeline_prepare(&multisampled) :
			xgpu_vulkan_pipeline(&multisampled) != VK_NULL_HANDLE)
			return 1;
	}
	host_logf(HOST_LOG_WARN, "vulkan: the Panini projection's pass cannot be made, so the 3D view is drawn flat");
	post.panini_failed = 1;
	return 0;
}

int xgpu_vulkan_post_panini(struct xgpu_vulkan_image *source, struct xgpu_vulkan_image *into, uint32_t layer,
	const int32_t corners[4], const float projection[8], struct xgpu_vulkan_image **moved)
{
	struct xgpu_vulkan_image *spare, *read;
	const struct xgpu_vulkan_image_description *description, *into_description;
	int32_t rectangle[4];
	float bounds[4], window[4];
	int index;

	if (!source || !into)
		return 0;
	description = xgpu_vulkan_image_description(source);
	into_description = xgpu_vulkan_image_description(into);
	if (!target_format_drawn(description->format) ||
		description->samples > 1 || into_description->format != description->format ||
		into_description->width != description->width || into_description->height != description->height)
		return 0;
	/* (made already, as the map was prepared, or now) */
	if (!xgpu_vulkan_post_panini_prepare(description->format, into_description->samples, 0))
		return 0;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	/* (the spare, where the pass draws into the target itself; else the
	copy, FXAA's) */
	spare = into == source ? spare_for(source, layer, rectangle, moved) : NULL;
	if (!images_fit(0, !spare, description->format, description->width, description->height))
		return 0;
	read = source;
	if (spare)
	{
		into = spare;
		spare_clear();
	}
	else
	{
		xgpu_vulkan_image_copy_rectangle(source, layer, post.color, rectangle);
		read = post.color;
	}
	/* (split screen: no texel of the next window is read) */
	bounds[0] = ((float)rectangle[0] + 0.5f) / (float)post.width;
	bounds[1] = ((float)rectangle[1] + 0.5f) / (float)post.height;
	bounds[2] = ((float)rectangle[2] - 0.5f) / (float)post.width;
	bounds[3] = ((float)rectangle[3] - 0.5f) / (float)post.height;
	/* (the view's own corners, which the tangents are at) */
	window[0] = (float)corners[0];
	window[1] = (float)corners[1];
	window[2] = (float)(corners[2] - corners[0]);
	window[3] = (float)(corners[3] - corners[1]);
	pass_draw_with(XGPU_VULKAN_POST_PANINI, into, layer, rectangle, read, NULL, NULL, bounds, window,
		projection);
	if (spare)
		spare_take_place(moved);
	return 1;
}

/* the turn's matrix for each count of quarter turns clockwise: rows from
the turned image's coordinates (about its centre) to the frame's */
static const float rotate_rows[4][4] =
{
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, -1.0f, 0.0f },
	{ -1.0f, 0.0f, 0.0f, -1.0f },
	{ 0.0f, -1.0f, 1.0f, 0.0f },
};

struct xgpu_vulkan_image *xgpu_vulkan_post_rotate(struct xgpu_vulkan_image *image, int quarter_turns)
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	uint32_t width, height;
	int stage;

	if (!image || !post.rotate || quarter_turns < 1 || quarter_turns > 3)
		return NULL;
	description = xgpu_vulkan_image_description(image);
	width = quarter_turns & 1 ? description->height : description->width;
	height = quarter_turns & 1 ? description->width : description->height;
	if (post.turned && (xgpu_vulkan_image_description(post.turned)->width != width ||
		xgpu_vulkan_image_description(post.turned)->height != height))
	{
		xgpu_vulkan_image_destroy(post.turned);
		post.turned = NULL;
	}
	if (!post.turned)
		post.turned = image_make(XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET,
			width, height);
	if (!post.turned)
		return NULL;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = post.rotate;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : image;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return NULL;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)width;
	uniforms->metrics[1] = 1.0f / (float)height;
	uniforms->metrics[2] = (float)width;
	uniforms->metrics[3] = (float)height;
	memcpy(uniforms->bounds, rotate_rows[quarter_turns], sizeof(uniforms->bounds));
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.scissor[2] = (int32_t)width;
	dynamic.viewport[3] = dynamic.scissor[3] = (int32_t)height;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(post.turned, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return post.turned;
}

/* ---------- display.hdr (xgpu_hdr.c's shaders) */

/* the shader modules of a program and the passes' vertex shader: 1 if
they are made */
static int shaders_make(int program)
{
	if (!post.vertex)
		post.vertex = xgpu_vulkan_shader(xgpu_vulkan_post_vertex.words, xgpu_vulkan_post_vertex.count);
	if (!post.pixel[program])
		post.pixel[program] = xgpu_vulkan_shader(xgpu_vulkan_post_programs[program].words,
			xgpu_vulkan_post_programs[program].count);
	return post.vertex && post.pixel[program];
}

/* the tone map's pipeline: into an RGBA16F target of its samples, with its
depth attachment's format (none tested or written), its colour alone,
reading the pixel it draws over */
static void tone_map_description(uint8_t depth_format, uint32_t samples,
	struct xgpu_vulkan_pipeline_description *description)
{
	int program = samples > 1 ? XGPU_VULKAN_POST_HDR_TONE_MAP_MS : XGPU_VULKAN_POST_HDR_TONE_MAP;

	memset(description, 0, sizeof(*description));
	description->vertex_shader = post.vertex;
	description->pixel_shader = post.pixel[program];
	description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	description->color_mask = 7;
	description->color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	description->depth_format = depth_format == XGPU_VULKAN_FORMAT_D24S8 ? XGPU_VULKAN_FORMAT_D24S8 :
		XGPU_VULKAN_FORMAT_NONE;
	description->samples = (uint8_t)(samples > 1 ? samples : 1);
	description->color_read = 1;
}

int xgpu_vulkan_post_prepare_hdr(uint8_t depth_format, uint32_t samples, int preparing)
{
	struct xgpu_vulkan_pipeline_description description;
	int program = samples > 1 ? XGPU_VULKAN_POST_HDR_TONE_MAP_MS : XGPU_VULKAN_POST_HDR_TONE_MAP;
	int made;

	if (post.tone_map_failed)
		return 0;
	made = shaders_make(program);
	if (made)
	{
		tone_map_description(depth_format, samples, &description);
		made = preparing ? xgpu_vulkan_pipeline_prepare(&description) :
			xgpu_vulkan_pipeline(&description) != VK_NULL_HANDLE;
	}
	if (!made)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.hdr's tone map cannot be made: highlights past the display's peak clip");
		post.tone_map_failed = 1;
	}
	return made;
}

void xgpu_vulkan_post_tone_map(struct xgpu_vulkan_image *target, struct xgpu_vulkan_image *depth,
	const int32_t corners[4], const float hdr[4], const float camera[4])
{
	const struct xgpu_vulkan_image_description *description;
	struct xgpu_vulkan_pipeline_description pipeline;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	uint8_t depth_format = depth ? XGPU_VULKAN_FORMAT_D24S8 : XGPU_VULKAN_FORMAT_NONE;
	int32_t rectangle[4];
	int index, stage;

	if (!target)
		return;
	description = xgpu_vulkan_image_description(target);
	if (description->format != XGPU_VULKAN_FORMAT_RGBA16F ||
		!xgpu_vulkan_post_prepare_hdr(depth_format, description->samples, 0))
		return;
	/* (inside the target) */
	for (index = 0; index < 4; index++)
	{
		int32_t most = (int32_t)(index & 1 ? description->height : description->width);

		rectangle[index] = corners[index] < 0 ? 0 : corners[index] > most ? most : corners[index];
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	tone_map_description(depth_format, description->samples, &pipeline);
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline(&pipeline);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return;
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[0] = 1.0f / (float)description->width;
	uniforms->metrics[1] = 1.0f / (float)description->height;
	uniforms->metrics[2] = (float)description->width;
	uniforms->metrics[3] = (float)description->height;
	memcpy(uniforms->hdr, hdr, sizeof(uniforms->hdr));
	/* (the camera's, XGPU_HDR_MODE_CAMERA: its contrast, its exposure) */
	if (camera)
		memcpy(uniforms->window, camera, sizeof(uniforms->window));
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
	draw.reads_color = 1;
	/* (the pass that drew the view, where these are its targets) */
	xgpu_vulkan_set_target_views(target, depth, 0, 1);
	xgpu_vulkan_draw(&draw);
}

/* the encoding's pipeline for a swapchain's format: the frame sampled,
every channel written (the swapchain's alpha is opaque) */
static void encode_description(uint8_t format, struct xgpu_vulkan_pipeline_description *description)
{
	memset(description, 0, sizeof(*description));
	description->vertex_shader = post.vertex;
	description->pixel_shader = post.pixel[XGPU_VULKAN_POST_HDR_ENCODE];
	description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	description->color_mask = 15;
	description->color_format = format;
	description->samples = 1;
}

/* the last pass's formats (the swapchains': scRGB's; HDR10's and a 10-bit
SDR one's; SDR's), its pipelines' order */
static const uint8_t ENCODE_FORMATS[3] =
{
	XGPU_VULKAN_FORMAT_RGBA16F, XGPU_VULKAN_FORMAT_RGB10A2, XGPU_VULKAN_FORMAT_BGRA8,
};

int xgpu_vulkan_post_encode_prepare(void)
{
	struct xgpu_vulkan_pipeline_description description;
	int index;

	if (post.encode[0] && post.encode[1] && post.encode[2])
		return 1;
	if (shaders_make(XGPU_VULKAN_POST_HDR_ENCODE) && sampler_make())
	{
		for (index = 0; index < 3; index++)
		{
			encode_description(ENCODE_FORMATS[index], &description);
			post.encode[index] = xgpu_vulkan_pipeline(&description);
		}
	}
	if (!post.encode[0] || !post.encode[1] || !post.encode[2])
	{
		host_logf(HOST_LOG_WARN, "vulkan: the frame's last pass cannot be made: the window is SDR, without "
			"display.gamma or dither");
		return 0;
	}
	return 1;
}

struct xgpu_vulkan_image *xgpu_vulkan_post_encode(struct xgpu_vulkan_image *image, const struct xgpu_hdr_frame *frame,
	uint8_t format, const struct xgpu_vulkan_output *output)
{
	const struct xgpu_vulkan_image_description *description, *encoded;
	struct xgpu_vulkan_post_uniforms *uniforms;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	VkPipeline pipeline = VK_NULL_HANDLE;
	int stage, index;

	if (!image || !output || !output->width || !output->height)
		return NULL;
	for (index = 0; index < 3; index++)
	{
		if (format == ENCODE_FORMATS[index])
			pipeline = post.encode[index];
	}
	if (!pipeline)
		return NULL;
	description = xgpu_vulkan_image_description(image);
	encoded = post.encoded ? xgpu_vulkan_image_description(post.encoded) : NULL;
	if (encoded && (encoded->width != output->width || encoded->height != output->height ||
		encoded->format != format))
	{
		xgpu_vulkan_image_destroy(post.encoded);
		post.encoded = NULL;
	}
	if (!post.encoded)
		post.encoded = image_make(format, XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET, output->width,
			output->height);
	if (!post.encoded)
		return NULL;
	memset(&draw, 0, sizeof(draw));
	draw.pipeline = pipeline;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = stage ? NULL : image;
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	uniforms = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!uniforms)
		return NULL;
	/* (the viewport's corner, and its size's reciprocals: the frame over
	all of it; the frame's size and the sharp factors; the encoding; the
	SDR curve, the dither's levels, display.gamma's power, the display's
	black) */
	memset(uniforms, 0, sizeof(*uniforms));
	uniforms->metrics[2] = 1.0f / (float)output->width;
	uniforms->metrics[3] = 1.0f / (float)output->height;
	uniforms->bounds[0] = (float)description->width;
	uniforms->bounds[1] = (float)description->height;
	uniforms->bounds[2] = (float)(output->factors[0] ? output->factors[0] : 1);
	uniforms->bounds[3] = (float)(output->factors[1] ? output->factors[1] : 1);
	uniforms->hdr[0] = frame->paper_white;
	uniforms->hdr[1] = frame->peak;
	uniforms->hdr[2] = (float)frame->output;
	uniforms->hdr[3] = frame->ui;
	uniforms->window[0] = (float)frame->sdr_curve;
	uniforms->window[1] = output->levels;
	uniforms->window[2] = output->power > 0.0f ? output->power : 1.0f;
	uniforms->window[3] = frame->black;
	uniforms->gamut[0] = frame->gamut ? 1.0f : 0.0f;
	uniforms->gamut[1] = frame->gamut_target;
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[2] = dynamic.scissor[2] = (int32_t)output->width;
	dynamic.viewport[3] = dynamic.scissor[3] = (int32_t)output->height;
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	xgpu_vulkan_set_target_views(post.encoded, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
	return post.encoded;
}

/* ---------- the loading screen's sharp frame (loading_art.h's "drawn by the GPU")

Its programs draw into BGRA8 images (the loading image, and the frame under
a caption), the picture's into the still layers' RGBA8 one. Made at the
start, as the loading screen presents while a map is prepared and may not
ask for a pipeline. */

static const uint8_t LOADING_FORMATS[3] = { XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_FORMAT_BGRA8, XGPU_VULKAN_FORMAT_RGBA8 };

static void loading_description(int index, struct xgpu_vulkan_pipeline_description *description)
{
	memset(description, 0, sizeof(*description));
	description->vertex_shader = post.vertex;
	description->pixel_shader = post.pixel[XGPU_VULKAN_POST_LOADING + index];
	description->primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	description->color_mask = 15;
	description->color_format = LOADING_FORMATS[index];
	description->samples = 1;
}

int xgpu_vulkan_post_loading_prepare(void)
{
	struct xgpu_vulkan_pipeline_description description;
	int index;

	if (post.loading[0] && post.loading[1] && post.loading[2])
		return 1;
	for (index = 0; index < 3; index++)
	{
		if (!shaders_make(XGPU_VULKAN_POST_LOADING + index) || !sampler_make())
			break;
		loading_description(index, &description);
		post.loading[index] = xgpu_vulkan_pipeline(&description);
		if (!post.loading[index])
			break;
	}
	if (index < 3)
	{
		host_logf(HOST_LOG_WARN, "vulkan: the loading screen's sharp frame's pipelines cannot be made: "
			"the processor draws it");
		return 0;
	}
	return 1;
}

void xgpu_vulkan_post_loading(int program, struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	struct xgpu_vulkan_image *const images[4], const float uniforms[5][4])
{
	struct xgpu_vulkan_pipeline_description description;
	struct xgpu_vulkan_post_uniforms *block;
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	int stage, index = program - XGPU_VULKAN_POST_LOADING;

	if (index < 0 || index > 2 || !post.loading[index])
		return;
	memset(&draw, 0, sizeof(draw));
	loading_description(index, &description);
	draw.pipeline = xgpu_vulkan_pipeline(&description);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].image = images[stage];
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = post.sampler;
	}
	block = xgpu_vulkan_upload(UNIFORMS_RANGE, 256, &draw.uniforms);
	if (!block)
		return;
	memset(block, 0, sizeof(*block));
	memcpy(block->metrics, uniforms[0], sizeof(block->metrics));
	memcpy(block->bounds, uniforms[1], sizeof(block->bounds));
	memcpy(block->hdr, uniforms[2], sizeof(block->hdr));
	memcpy(block->window, uniforms[3], sizeof(block->window));
	memcpy(block->panini, uniforms[4], sizeof(block->panini));
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
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}
