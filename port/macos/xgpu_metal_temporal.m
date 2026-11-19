/*
XGPU_METAL_TEMPORAL.M

display.temporal_aa on the Metal backend (xgpu_metal_temporal.h,
docs/temporal-aa.md), as the Vulkan backend has it (xgpu_vulkan_stream.c's
motion, xgpu_vulkan_fsr.c):

- The window's motion, resolved over its corners from the objects' motion
  (drawn as their second colour attachment: xgpu_msl.c's motion) where
  their depth is still the pixel's, else from the depth by the camera's
  change (tools/vulkan_post_shaders.py's MOTION_RESOLVE_SOURCE, here as
  MSL); debug.motion_vectors' colours of it over the view.
- "metalfx": MetalFX's temporal scaler (MTLFXTemporalScaler) upscales the
  window's colour (before its tone map), reversed depth, motion (a fraction
  of the view, previous less current: scaled to the source's pixels, which
  is MetalFX's own sense), jitter and reset into an output texture of the
  destination window's size, copied into the destination (the back buffer
  at the display's scale, where the tone map, the Panini pass and the HUD
  follow). The exposure is fixed at 1 (a 1x1 R16Float of 1), as FSR's. Each
  window (split screen's four) has a scaler and history of its own, made
  again as its sizes or formats change: off the render thread (a scaler
  takes 100 to 300 ms to make on an M1 Max, over a second the first time
  after a system update), the window's view stretched into the destination
  (linear filtering) until it is made, its history begun with it. A
  window not at its target's top
  left corner, or smaller than it, is copied there first (MetalFX reads its
  inputs from their origin).
- The reactive mask: FSR's generated one (xgpu_vulkan_fsr.c's), from the
  window's colour before its transparent draws (xgpu_metal_metalfx_opaque)
  and after: where they changed it (tone mapped, the largest channel), 0.9
  past 0.2, else 0, an R8 texture MetalFX reads (macOS 14.4, iOS 17.4).

The passes are the backend's (xgpu_metal.h), their functions MSL compiled on
the backend's queues; MetalFX's work goes in the command buffer between the
passes (xgpu_metal_outside_passes). HALO_METALFX_TIMES=<n>: MetalFX's GPU
time (its own command buffer), its mean over each n upscales, logged, and
each window's first upscale's time on the render thread.
*/

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#if __has_include(<MetalFX/MetalFX.h>) && !TARGET_OS_SIMULATOR
#import <MetalFX/MetalFX.h>
#include <os/lock.h>
#define XGPU_METAL_METALFX 1
#endif

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "xgpu_metal.h"
#include "xgpu_metal_internal.h"
#include "xgpu_metal_pipeline.h"
#include "xgpu_metal_temporal.h"

#include <stdlib.h>
#include <string.h>

/* (the uniforms' place: the backend's draw uniforms) */
#define XGPU_METAL_TEMPORAL_UNIFORMS 17

static NSString *const temporal_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct motion_uniforms { float4 metrics; float4 window; float4 reproject[4]; float4 motion; };\n"
	"struct temporal_out { float4 position [[position]]; };\n"
	"vertex temporal_out xgpu_temporal_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\ttemporal_out out;\n"
	"\tout.position = float4(float((id & 1) << 2) - 1.0, float((id & 2) << 1) - 1.0, 0.0, 1.0);\n"
	"\treturn out;\n"
	"}\n"
	/* (the window's motion, as MOTION_RESOLVE_SOURCE) */
	"fragment float4 xgpu_temporal_resolve(temporal_out in [[stage_in]], constant motion_uniforms &u [[buffer(17)]],\n"
	"\ttexture2d<float> objects [[texture(0)]], depth2d<float> depth_texture [[texture(1)]])\n"
	"{\n"
	"\tuint2 texel = uint2(in.position.xy);\n"
	"\tfloat4 object = objects.read(texel);\n"
	"\tfloat depth = depth_texture.read(texel);\n"
	"\tif (object.w > 0.5 && abs(object.z - depth) <= max(1.5e-7, depth * 1.0e-6))\n"
	"\t\treturn float4(object.xy, 0.0, 1.0);\n"
	"\tif (u.motion.w == 0.0)\n"
	"\t\treturn float4(0.0);\n"
	"\tfloat2 place = (in.position.xy - u.window.xy) / u.window.zw;\n"
	"\tfloat2 ndc = float2(place.x * 2.0 - 1.0, 1.0 - place.y * 2.0) - u.motion.xy;\n"
	"\tfloat4 current = float4(ndc, u.motion.z != 0.0 ? 1.0 - depth : depth, 1.0);\n"
	"\tfloat4 previous = float4(dot(u.reproject[0], current), dot(u.reproject[1], current), dot(u.reproject[2], current),\n"
	"\t\tdot(u.reproject[3], current));\n"
	"\tfloat2 before = previous.w > 0.0 ? previous.xy / previous.w : ndc;\n"
	"\treturn float4((before - ndc) * float2(0.5, -0.5), 0.0, 0.0);\n"
	"}\n"
	/* (debug.motion_vectors: grey still, red to the right, green downward,
	full at 8 pixels a frame, as MOTION_VIEW_SOURCE) */
	"fragment float4 xgpu_temporal_view(temporal_out in [[stage_in]], constant motion_uniforms &u [[buffer(17)]],\n"
	"\ttexture2d<float> motion [[texture(0)]])\n"
	"{\n"
	"\tfloat2 moved = motion.read(uint2(in.position.xy)).xy * u.window.zw;\n"
	"\tfloat2 shown = clamp(moved / 8.0, -1.0, 1.0);\n"
	"\treturn float4(0.5 + 0.5 * shown.x, 0.5 + 0.5 * shown.y, 0.5 - 0.25 * (shown.x + shown.y), 1.0);\n"
	"}\n"
	/* (the reactive mask, FSR's generated one: tone mapped, the largest
	channel's change, 0.9 past 0.2) */
	"static float3 reactive_tone_map(float3 c) { return c / (max(c.r, max(c.g, c.b)) + 1.0); }\n"
	"fragment float4 xgpu_temporal_reactive(temporal_out in [[stage_in]], texture2d<float> opaque [[texture(0)]],\n"
	"\ttexture2d<float> color [[texture(1)]])\n"
	"{\n"
	"\tuint2 texel = uint2(in.position.xy);\n"
	"\tfloat3 change = abs(reactive_tone_map(max(color.read(texel).rgb, 0.0)) -\n"
	"\t\treactive_tone_map(max(opaque.read(texel).rgb, 0.0)));\n"
	"\treturn float4(max(change.r, max(change.g, change.b)) < 0.2 ? 0.0 : 0.9);\n"
	"}\n"
	/* (the view stretched while its scaler is made: the source's rectangle
	(metrics: x0, y0, width, height) over the destination's (window)) */
	"fragment float4 xgpu_temporal_stretch(temporal_out in [[stage_in]], constant motion_uniforms &u [[buffer(17)]],\n"
	"\ttexture2d<float> color [[texture(0)]])\n"
	"{\n"
	"\tconstexpr sampler linear(coord::pixel, filter::linear, address::clamp_to_edge);\n"
	"\treturn color.sample(linear, u.metrics.xy + (in.position.xy - u.window.xy) / u.window.zw * u.metrics.zw);\n"
	"}\n";

enum
{
	FUNCTION_VERTEX,
	FUNCTION_RESOLVE,
	FUNCTION_VIEW,
	FUNCTION_REACTIVE,
	FUNCTION_STRETCH,
	FUNCTIONS
};

static const char *const function_names[FUNCTIONS] =
{
	"xgpu_temporal_vertex", "xgpu_temporal_resolve", "xgpu_temporal_view", "xgpu_temporal_reactive",
	"xgpu_temporal_stretch",
};

#ifdef XGPU_METAL_METALFX
/* what a scaler is made for: its inputs' and output's formats and sizes,
and whether it reads a reactive mask */
struct metalfx_fit
{
	MTLPixelFormat color, depth, output;
	NSUInteger input_width, input_height, output_width, output_height;
	BOOL reactive;
};

/* a window's scaler, what it was made for, its output, and its inputs'
copies (made where the window is not at its targets' origin); the scaler
being made off the render thread (making: for asked; made, under
temporal.lock: its result, handed over), and whether the next upscale
begins the history (the scaler new) */
struct metalfx_window
{
	id<MTLFXTemporalScaler> scaler;
	struct metalfx_fit fit, asked;
	BOOL making, made_done, fresh;
	id<MTLFXTemporalScaler> made;
	id<MTLTexture> output, inputs[4];
	/* the colour before the transparent draws (the backend's texture: 0
	none) and whether this frame's is in it; the reactive mask, of the
	target's size */
	uint32_t opaque, reactive;
	BOOL opaque_now;
};
#endif

static struct
{
	uint32_t functions[FUNCTIONS];
	BOOL failed, metalfx_failed;
#ifdef XGPU_METAL_METALFX
	struct metalfx_window windows[4];
	id<MTLTexture> exposure;
	/* the scalers made in turn, at the user's priority (below the render
	thread's); the lock over the windows' made and made_done */
	dispatch_queue_t maker;
	os_unfair_lock lock;
	/* HALO_METALFX_TIMES: each so many upscales, their GPU time's mean */
	int times_every, timed;
	BOOL first_timed[4];
	double timed_seconds;
#endif
} temporal;

/* ---------- the passes */

static int functions_make(void)
{
	NSData *source;
	int function;

	if (temporal.failed)
		return 0;
	source = [temporal_source dataUsingEncoding:NSUTF8StringEncoding];
	for (function = 0; function < FUNCTIONS; function++)
	{
		if (!temporal.functions[function])
			temporal.functions[function] = xgpu_metal_shader_create(source.bytes, source.length, function_names[function]);
		if (!temporal.functions[function])
		{
			host_logf(HOST_LOG_WARN, "metal: display.temporal_aa's passes cannot be made: no motion is found");
			temporal.failed = YES;
			return 0;
		}
	}
	return 1;
}

static void pipeline_describe(int function, uint8_t format, uint8_t mask, struct xgpu_metal_pipeline *pipeline)
{
	memset(pipeline, 0, sizeof(*pipeline));
	pipeline->vertex_shader = temporal.functions[FUNCTION_VERTEX];
	pipeline->pixel_shader = temporal.functions[function];
	pipeline->color_mask = mask;
	pipeline->color_format = format;
	pipeline->samples = 1;
}

#define WRITE_ALL (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE | XGPU_WRITE_ALPHA)
#define WRITE_COLOR (XGPU_WRITE_RED | XGPU_WRITE_GREEN | XGPU_WRITE_BLUE)

int xgpu_metal_motion_prepare(uint8_t view_format)
{
	struct xgpu_metal_pipeline pipeline;

	if (!functions_make())
		return 0;
	pipeline_describe(FUNCTION_RESOLVE, XGPU_METAL_FORMAT_RGBA16F, WRITE_ALL, &pipeline);
	xgpu_metal_prepare(&pipeline);
	if (view_format)
	{
		pipeline_describe(FUNCTION_VIEW, view_format, WRITE_COLOR, &pipeline);
		xgpu_metal_prepare(&pipeline);
	}
	if (xgpu_metal_metalfx_supported())
	{
		/* (and the stretch, into each of the back buffer's formats) */
		static const uint8_t formats[] = { XGPU_METAL_FORMAT_BGRA8, XGPU_METAL_FORMAT_RGBA16F, XGPU_METAL_FORMAT_RGBA16 };
		size_t index;

		pipeline_describe(FUNCTION_REACTIVE, XGPU_METAL_FORMAT_R8, WRITE_ALL, &pipeline);
		xgpu_metal_prepare(&pipeline);
		for (index = 0; index < sizeof(formats); index++)
		{
			pipeline_describe(FUNCTION_STRETCH, formats[index], WRITE_ALL, &pipeline);
			xgpu_metal_prepare(&pipeline);
		}
	}
	return 1;
}

/* a function's triangle over the rectangle into a texture, reading two */
static void pass_draw(int function, uint32_t into, uint8_t format, uint8_t mask, const int32_t rectangle[4],
	uint32_t texture0, uint32_t texture1, const struct xgpu_metal_motion_uniforms *uniforms)
{
	struct xgpu_metal_pipeline pipeline;
	struct xgpu_metal_dynamic_state dynamic;
	struct xgpu_metal_sampler sampler;
	uint32_t stage, kind;

	if (!functions_make())
		return;
	kind = xgpu_metal_pass_kind(XGPU_METAL_PASS_TEMPORAL);
	xgpu_metal_set_targets(into, 0, 0, 0, 0);
	pipeline_describe(function, format, mask, &pipeline);
	xgpu_metal_set_pipeline(&pipeline);
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	xgpu_metal_set_dynamic_state(&dynamic);
	memset(&sampler, 0, sizeof(sampler));
	sampler.mip_filter = XGPU_METAL_MIP_NONE;
	sampler.address_u = sampler.address_v = sampler.address_w = XGPU_METAL_ADDRESS_CLAMP;
	sampler.max_anisotropy = 1;
	for (stage = 0; stage < 4; stage++)
		xgpu_metal_set_texture(stage, stage == 0 ? texture0 : stage == 1 ? texture1 : 0, &sampler);
	if (uniforms)
		xgpu_metal_set_draw_uniforms(uniforms, sizeof(*uniforms));
	xgpu_metal_draw(XGPU_METAL_PRIMITIVE_TRIANGLES, 3, 0, 0, (struct xgpu_metal_source){ XGPU_METAL_SOURCE_NONE, 0, 0, 0 });
	xgpu_metal_pass_kind(kind);
}

void xgpu_metal_motion_resolve(uint32_t into, uint32_t objects, uint32_t depth, const int32_t rectangle[4],
	const struct xgpu_metal_motion_uniforms *uniforms)
{
	pass_draw(FUNCTION_RESOLVE, into, XGPU_METAL_FORMAT_RGBA16F, WRITE_ALL, rectangle, objects, depth, uniforms);
}

void xgpu_metal_motion_view(uint32_t into, uint8_t format, uint32_t motion, const int32_t rectangle[4],
	const struct xgpu_metal_motion_uniforms *uniforms)
{
	pass_draw(FUNCTION_VIEW, into, format, WRITE_COLOR, rectangle, motion, 0, uniforms);
}

/* ---------- MetalFX */

int xgpu_metal_metalfx_supported(void)
{
#ifdef XGPU_METAL_METALFX
	static int supported = -1;

	if (supported < 0)
	{
		id<MTLDevice> device = xgpu_metal_device();

		supported = device && [MTLFXTemporalScalerDescriptor supportsDevice:device];
	}
	return supported;
#else
	return 0;
#endif
}

#ifdef XGPU_METAL_METALFX

static id<MTLTexture> texture_make(MTLPixelFormat format, NSUInteger width, NSUInteger height, MTLTextureUsage usage)
{
	MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width
		height:height mipmapped:NO];

	descriptor.usage = usage;
	descriptor.storageMode = MTLStorageModePrivate;
	return [xgpu_metal_device() newTextureWithDescriptor:descriptor];
}

/* the backend's texture of a format and size for a window (made again as
they change) */
static uint32_t handle_fit(uint32_t texture, uint8_t format, uint32_t width, uint32_t height)
{
	struct xgpu_metal_texture_description description;
	id<MTLTexture> made = texture ? xgpu_metal_texture(texture) : nil;

	if (made && made.width == width && made.height == height &&
		made.pixelFormat == xgpu_metal_format_information(format)->pixel_format)
	{
		return texture;
	}
	xgpu_metal_texture_destroy(texture);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_METAL_TEXTURE_2D;
	description.format = format;
	description.usage = XGPU_METAL_USAGE_SAMPLED | XGPU_METAL_USAGE_TARGET;
	description.width = (uint16_t)width;
	description.height = (uint16_t)height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_metal_texture_create(&description);
}

/* the XGPU_METAL_FORMAT_* of a target's texture (its colour: BGRA8,
RGBA16F, RGBA16) */
static uint8_t backend_format(MTLPixelFormat format)
{
	return format == MTLPixelFormatRGBA16Float ? XGPU_METAL_FORMAT_RGBA16F :
		format == MTLPixelFormatRGBA16Unorm ? XGPU_METAL_FORMAT_RGBA16 : XGPU_METAL_FORMAT_BGRA8;
}

#endif

void xgpu_metal_metalfx_opaque(int window, uint32_t color)
{
#ifdef XGPU_METAL_METALFX
	struct metalfx_window *entry = &temporal.windows[window & 3];
	id<MTLTexture> texture = xgpu_metal_texture(color);

	entry->opaque_now = NO;
	if (!texture || !xgpu_metal_metalfx_supported())
		return;
	entry->opaque = handle_fit(entry->opaque, backend_format(texture.pixelFormat), (uint32_t)texture.width,
		(uint32_t)texture.height);
	if (!entry->opaque || xgpu_metal_texture(entry->opaque).pixelFormat != texture.pixelFormat)
		return;
	xgpu_metal_texture_copy(color, entry->opaque);
	entry->opaque_now = YES;
#else
	(void)window;
	(void)color;
#endif
}

#ifdef XGPU_METAL_METALFX

static BOOL fit_same(const struct metalfx_fit *a, const struct metalfx_fit *b)
{
	return a->color == b->color && a->depth == b->depth && a->output == b->output &&
		a->input_width == b->input_width && a->input_height == b->input_height &&
		a->output_width == b->output_width && a->output_height == b->output_height && a->reactive == b->reactive;
}

/* a scaler made for a fit, off the render thread, handed to the window's
made */
static void scaler_make(struct metalfx_window *entry, struct metalfx_fit fit)
{
	if (!temporal.maker)
		temporal.maker = dispatch_queue_create("metalfx scalers", dispatch_queue_attr_make_with_qos_class(
			DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0));
	entry->asked = fit;
	entry->making = YES;
	dispatch_async(temporal.maker, ^{
		MTLFXTemporalScalerDescriptor *descriptor = [MTLFXTemporalScalerDescriptor new];
		id<MTLFXTemporalScaler> scaler;
		double started = CFAbsoluteTimeGetCurrent();

		descriptor.colorTextureFormat = fit.color;
		descriptor.depthTextureFormat = fit.depth;
		descriptor.motionTextureFormat = MTLPixelFormatRGBA16Float;
		descriptor.outputTextureFormat = fit.output;
		descriptor.inputWidth = fit.input_width;
		descriptor.inputHeight = fit.input_height;
		descriptor.outputWidth = fit.output_width;
		descriptor.outputHeight = fit.output_height;
		descriptor.autoExposureEnabled = NO;
		if (fit.reactive)
		{
			descriptor.reactiveMaskTextureEnabled = YES;
			descriptor.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
		}
		scaler = [descriptor newTemporalScalerWithDevice:xgpu_metal_device()];
		if (scaler)
			host_logf(HOST_LOG_INFO, "metal: MetalFX's temporal scaler (%lux%lu to %lux%lu) made in %.0f ms",
				(unsigned long)fit.input_width, (unsigned long)fit.input_height, (unsigned long)fit.output_width,
				(unsigned long)fit.output_height, (CFAbsoluteTimeGetCurrent() - started) * 1000.0);
		os_unfair_lock_lock(&temporal.lock);
		entry->made = scaler;
		entry->made_done = YES;
		os_unfair_lock_unlock(&temporal.lock);
	});
}

/* the window's scaler for its inputs' and output's formats and sizes: NO
while it is made (or cannot be) */
static BOOL scaler_fit(struct metalfx_window *entry, const struct metalfx_fit *fit)
{
	id<MTLFXTemporalScaler> made = nil;
	BOOL done;
	int which;

	if (entry->scaler && fit_same(&entry->fit, fit))
		return YES;
	if (!entry->making)
	{
		/* (one that cannot be made is not asked for again) */
		if (!temporal.metalfx_failed || !fit_same(&entry->asked, fit))
			scaler_make(entry, *fit);
		return NO;
	}
	os_unfair_lock_lock(&temporal.lock);
	done = entry->made_done;
	if (done)
	{
		made = entry->made;
		entry->made = nil;
		entry->made_done = NO;
	}
	os_unfair_lock_unlock(&temporal.lock);
	if (!done)
		return NO;
	entry->making = NO;
	if (!made)
	{
		if (!temporal.metalfx_failed)
			host_logf(HOST_LOG_WARN, "metal: MetalFX's temporal scaler cannot be made (%lux%lu to %lux%lu): the view is "
				"stretched, not upscaled", (unsigned long)entry->asked.input_width, (unsigned long)entry->asked.input_height,
				(unsigned long)entry->asked.output_width, (unsigned long)entry->asked.output_height);
		temporal.metalfx_failed = YES;
		return NO;
	}
	/* (made for another fit, the sizes changed meanwhile: made again) */
	if (!fit_same(&entry->asked, fit))
	{
		scaler_make(entry, *fit);
		return NO;
	}
	entry->scaler = made;
	entry->fit = *fit;
	entry->fresh = YES;
	entry->output = texture_make(fit->output, fit->output_width, fit->output_height, made.outputTextureUsage);
	for (which = 0; which < 4; which++)
		entry->inputs[which] = nil;
	if (!temporal.exposure)
	{
		MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR16Float
			width:1 height:1 mipmapped:NO];
		/* (1.0 as a half float) */
		const uint16_t one = 0x3c00;

		descriptor.usage = MTLTextureUsageShaderRead;
		descriptor.storageMode = MTLStorageModeShared;
		temporal.exposure = [xgpu_metal_device() newTextureWithDescriptor:descriptor];
		[temporal.exposure replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&one bytesPerRow:2];
	}
	return entry->output != nil;
}

/* an input as MetalFX reads it: the texture itself where the window is at
its origin and of its size, else the window copied into one of that size */
static id<MTLTexture> input_place(struct metalfx_window *entry, int which, id<MTLTexture> texture, const int32_t source[4],
	id<MTLBlitCommandEncoder> *blit, id<MTLCommandBuffer> buffer)
{
	NSUInteger width = (NSUInteger)(source[2] - source[0]), height = (NSUInteger)(source[3] - source[1]);

	if (!source[0] && !source[1] && texture.width == width && texture.height == height)
		return texture;
	if (!entry->inputs[which] || entry->inputs[which].pixelFormat != texture.pixelFormat)
		entry->inputs[which] = texture_make(texture.pixelFormat, width, height, MTLTextureUsageShaderRead);
	if (!entry->inputs[which])
		return nil;
	if (!*blit)
		*blit = [buffer blitCommandEncoder];
	[*blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake((NSUInteger)source[0],
		(NSUInteger)source[1], 0) sourceSize:MTLSizeMake(width, height, 1) toTexture:entry->inputs[which]
		destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
	return entry->inputs[which];
}

#endif

#ifdef XGPU_METAL_METALFX
static int upscale(const struct xgpu_metal_metalfx_frame *frame)
{
	struct metalfx_window *entry = &temporal.windows[frame->window & 3];
	id<MTLTexture> color = xgpu_metal_texture(frame->color), depth = xgpu_metal_texture(frame->depth);
	id<MTLTexture> motion = xgpu_metal_texture(frame->motion), destination = xgpu_metal_texture(frame->destination);
	id<MTLTexture> reactive = nil, inputs[4];
	id<MTLCommandBuffer> buffer;
	id<MTLBlitCommandEncoder> blit = nil;
	const int32_t *source = frame->source, *place = frame->destination_rectangle;
	NSUInteger input_width = (NSUInteger)(source[2] - source[0]), input_height = (NSUInteger)(source[3] - source[1]);
	NSUInteger output_width = (NSUInteger)(place[2] - place[0]), output_height = (NSUInteger)(place[3] - place[1]);
	BOOL use_reactive = entry->opaque_now;
	struct metalfx_fit fit;
	uint32_t kind;

	entry->opaque_now = NO;
	if (!xgpu_metal_metalfx_supported() || !color || !depth || !motion || !destination || !input_width || !input_height ||
		!output_width || !output_height)
		return 0;
	/* (the reactive mask: where this frame's opaque view was kept, of the
	colour's size) */
	if (use_reactive && entry->opaque && xgpu_metal_texture(entry->opaque).width == color.width &&
		xgpu_metal_texture(entry->opaque).height == color.height && functions_make())
	{
		entry->reactive = handle_fit(entry->reactive, XGPU_METAL_FORMAT_R8, (uint32_t)color.width, (uint32_t)color.height);
		if (entry->reactive)
		{
			pass_draw(FUNCTION_REACTIVE, entry->reactive, XGPU_METAL_FORMAT_R8, WRITE_ALL, source, entry->opaque,
				frame->color, NULL);
			reactive = xgpu_metal_texture(entry->reactive);
		}
	}
	fit.color = color.pixelFormat;
	fit.depth = depth.pixelFormat;
	fit.output = destination.pixelFormat;
	fit.input_width = input_width;
	fit.input_height = input_height;
	fit.output_width = output_width;
	fit.output_height = output_height;
	fit.reactive = reactive != nil;
	if (!scaler_fit(entry, &fit))
	{
		/* (while it is made: the view stretched over the destination's
		window) */
		struct xgpu_metal_motion_uniforms uniforms;

		memset(&uniforms, 0, sizeof(uniforms));
		uniforms.metrics[0] = (float)source[0];
		uniforms.metrics[1] = (float)source[1];
		uniforms.metrics[2] = (float)input_width;
		uniforms.metrics[3] = (float)input_height;
		uniforms.window[0] = (float)place[0];
		uniforms.window[1] = (float)place[1];
		uniforms.window[2] = (float)output_width;
		uniforms.window[3] = (float)output_height;
		pass_draw(FUNCTION_STRETCH, frame->destination, backend_format(destination.pixelFormat), WRITE_ALL, place,
			frame->color, 0, &uniforms);
		return 1;
	}
	kind = xgpu_metal_pass_kind(XGPU_METAL_PASS_TEMPORAL);
	/* (HALO_METALFX_TIMES: the work so far committed, MetalFX's alone in
	the next command buffer) */
	if (temporal.times_every > 0)
		xgpu_metal_fence();
	buffer = xgpu_metal_outside_passes();
	xgpu_metal_texture_encoded(frame->color, 0);
	xgpu_metal_texture_encoded(frame->depth, 0);
	xgpu_metal_texture_encoded(frame->motion, 0);
	if (reactive)
		xgpu_metal_texture_encoded(entry->reactive, 0);
	xgpu_metal_texture_encoded(frame->destination, 1);
	inputs[0] = input_place(entry, 0, color, source, &blit, buffer);
	inputs[1] = input_place(entry, 1, depth, source, &blit, buffer);
	inputs[2] = input_place(entry, 2, motion, source, &blit, buffer);
	inputs[3] = reactive ? input_place(entry, 3, reactive, source, &blit, buffer) : nil;
	if (blit)
		[blit endEncoding];
	blit = nil;
	if (!inputs[0] || !inputs[1] || !inputs[2] || (reactive && !inputs[3]))
	{
		xgpu_metal_pass_kind(kind);
		return 0;
	}
	entry->scaler.colorTexture = inputs[0];
	entry->scaler.depthTexture = inputs[1];
	entry->scaler.motionTexture = inputs[2];
	entry->scaler.reactiveMaskTexture = inputs[3];
	entry->scaler.outputTexture = entry->output;
	entry->scaler.exposureTexture = temporal.exposure;
	entry->scaler.preExposure = 1.0f;
	entry->scaler.inputContentWidth = input_width;
	entry->scaler.inputContentHeight = input_height;
	/* (the jitter as the draws were moved by it, in the source's pixels,
	x right, y down: MetalFX's sense, as FSR's; turned about, a still view
	is drawn in steps of the source's pixels) */
	entry->scaler.jitterOffsetX = frame->jitter[0];
	entry->scaler.jitterOffsetY = frame->jitter[1];
	/* (a fraction of the view, previous less current, x right, y down: in
	the source's pixels, where the pixel was the frame before, MetalFX's) */
	entry->scaler.motionVectorScaleX = (float)input_width;
	entry->scaler.motionVectorScaleY = (float)input_height;
	/* (a new scaler's history begun) */
	entry->scaler.reset = frame->reset != 0 || entry->fresh;
	entry->fresh = NO;
	entry->scaler.depthReversed = YES;
	if (temporal.times_every > 0)
	{
		[buffer addCompletedHandler:^(id<MTLCommandBuffer> done) {
			double seconds = done.GPUEndTime - done.GPUStartTime;

			/* (the completion handlers' thread, one at a time) */
			temporal.timed_seconds += seconds;
			if (++temporal.timed >= temporal.times_every)
			{
				host_logf(HOST_LOG_INFO, "metal: MetalFX %lux%lu to %lux%lu: %.3f ms GPU a frame over %d frames",
					(unsigned long)input_width, (unsigned long)input_height, (unsigned long)output_width,
					(unsigned long)output_height, temporal.timed_seconds * 1000.0 / temporal.timed, temporal.timed);
				temporal.timed = 0;
				temporal.timed_seconds = 0.0;
			}
		}];
	}
	[entry->scaler encodeToCommandBuffer:buffer];
	if (temporal.times_every > 0)
	{
		xgpu_metal_fence();
		buffer = xgpu_metal_outside_passes();
	}
	/* (the output into the destination's window) */
	blit = [buffer blitCommandEncoder];
	[blit copyFromTexture:entry->output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
		sourceSize:MTLSizeMake(output_width, output_height, 1) toTexture:destination destinationSlice:0
		destinationLevel:0 destinationOrigin:MTLOriginMake((NSUInteger)place[0], (NSUInteger)place[1], 0)];
	[blit endEncoding];
	xgpu_metal_pass_kind(kind);
	return 1;
}
#endif

int xgpu_metal_metalfx_upscale(const struct xgpu_metal_metalfx_frame *frame)
{
#ifdef XGPU_METAL_METALFX
	double started = CFAbsoluteTimeGetCurrent();
	int drawn;

	if (!temporal.times_every)
		temporal.times_every = getenv("HALO_METALFX_TIMES") ? atoi(getenv("HALO_METALFX_TIMES")) : -1;
	drawn = upscale(frame);
	/* (HALO_METALFX_TIMES: each window's first upscale's time on the render
	thread) */
	if (temporal.times_every > 0 && !temporal.first_timed[frame->window & 3])
	{
		temporal.first_timed[frame->window & 3] = YES;
		host_logf(HOST_LOG_INFO, "metal: MetalFX's first upscale of window %d: %.1f ms of the render thread",
			frame->window & 3, (CFAbsoluteTimeGetCurrent() - started) * 1000.0);
	}
	return drawn;
#else
	(void)frame;
	return 0;
#endif
}
