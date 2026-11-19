/*
XGPU_METAL_PIPELINE.M

What a draw's render pipeline state is made of, and its Metal descriptor
(xgpu_metal_pipeline.h). No device: the tool that builds the app's
pipelines ahead of time runs this too.
*/

#import <Metal/Metal.h>

#include "xgpu_metal_pipeline.h"
#include "xgpu_hdr.h"
#include "xgpu_stream.h"

#include <string.h>

/* ---------- formats */

#define SWIZZLE(r, g, b, a) { MTLTextureSwizzle##r, MTLTextureSwizzle##g, MTLTextureSwizzle##b, MTLTextureSwizzle##a }

static const struct xgpu_metal_format_information formats[XGPU_METAL_FORMAT_COUNT] = {
	[XGPU_METAL_FORMAT_NONE] = { MTLPixelFormatInvalid, 0, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_BGRA8] = { MTLPixelFormatBGRA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_BGRX8] = { MTLPixelFormatBGRA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, One), 1 },
	/* (Metal names packed formats from the low bits up: B5G6R5 has red in
	the top 5 bits, as D3D's R5G6B5) */
	[XGPU_METAL_FORMAT_R5G6B5] = { MTLPixelFormatB5G6R5Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_A1R5G5B5] = { MTLPixelFormatBGR5A1Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_X1R5G5B5] = { MTLPixelFormatBGR5A1Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, One), 1 },
	/* A4R4G4B4 (alpha in the top 4 bits) is read through ABGR4, whose red
	is the top 4 bits: D3D's alpha, red, green, blue are its red, green,
	blue, alpha */
	[XGPU_METAL_FORMAT_A4R4G4B4] = { MTLPixelFormatABGR4Unorm, 2, 0, 0, 0, SWIZZLE(Green, Blue, Alpha, Red), 1 },
	[XGPU_METAL_FORMAT_L8] = { MTLPixelFormatR8Unorm, 1, 0, 0, 0, SWIZZLE(Red, Red, Red, One), 1 },
	[XGPU_METAL_FORMAT_A8] = { MTLPixelFormatA8Unorm, 1, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	/* A8L8: luminance in the low byte (red), alpha in the high (green) */
	[XGPU_METAL_FORMAT_A8L8] = { MTLPixelFormatRG8Unorm, 2, 0, 0, 0, SWIZZLE(Red, Red, Red, Green), 1 },
	[XGPU_METAL_FORMAT_DXT1] = { MTLPixelFormatBC1_RGBA, 8, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_DXT3] = { MTLPixelFormatBC2_RGBA, 16, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_DXT5] = { MTLPixelFormatBC3_RGBA, 16, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_D24S8] = { MTLPixelFormatDepth32Float_Stencil8, 8, 0, 1, 1, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_D16] = { MTLPixelFormatDepth16Unorm, 2, 0, 1, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RGBA8] = { MTLPixelFormatRGBA8Unorm, 4, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_COVERAGE] = { MTLPixelFormatR8Unorm, 1, 0, 0, 0, SWIZZLE(One, One, One, Red), 1 },
	[XGPU_METAL_FORMAT_RGBA16F] = { MTLPixelFormatRGBA16Float, 8, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_ASTC] = { MTLPixelFormatASTC_4x4_LDR, 16, 1, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_R8_SNORM] = { MTLPixelFormatR8Snorm, 1, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_R8] = { MTLPixelFormatR8Unorm, 1, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RG8] = { MTLPixelFormatRG8Unorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RGBA32UI] = { MTLPixelFormatRGBA32Uint, 16, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RGBA32F] = { MTLPixelFormatRGBA32Float, 16, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RG8_SNORM] = { MTLPixelFormatRG8Snorm, 2, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
	[XGPU_METAL_FORMAT_RGBA16] = { MTLPixelFormatRGBA16Unorm, 8, 0, 0, 0, SWIZZLE(Red, Green, Blue, Alpha), 0 },
};

const struct xgpu_metal_format_information *xgpu_metal_format_information(uint32_t format)
{
	return format < XGPU_METAL_FORMAT_COUNT ? &formats[format] : &formats[XGPU_METAL_FORMAT_NONE];
}

/* ---------- the interface's values in Metal's */

static const MTLCompareFunction compare_functions[8] = {
	MTLCompareFunctionNever, MTLCompareFunctionLess, MTLCompareFunctionEqual, MTLCompareFunctionLessEqual,
	MTLCompareFunctionGreater, MTLCompareFunctionNotEqual, MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways,
};

static const MTLBlendFactor blend_factors[15] = {
	MTLBlendFactorZero, MTLBlendFactorOne, MTLBlendFactorSourceColor, MTLBlendFactorOneMinusSourceColor,
	MTLBlendFactorSourceAlpha, MTLBlendFactorOneMinusSourceAlpha, MTLBlendFactorDestinationAlpha,
	MTLBlendFactorOneMinusDestinationAlpha, MTLBlendFactorDestinationColor, MTLBlendFactorOneMinusDestinationColor,
	MTLBlendFactorSourceAlphaSaturated, MTLBlendFactorBlendColor, MTLBlendFactorOneMinusBlendColor,
	MTLBlendFactorBlendAlpha, MTLBlendFactorOneMinusBlendAlpha,
};

static const MTLBlendOperation blend_operations[5] = {
	MTLBlendOperationAdd, MTLBlendOperationSubtract, MTLBlendOperationReverseSubtract, MTLBlendOperationMin,
	MTLBlendOperationMax,
};

static const MTLStencilOperation stencil_operations[8] = {
	MTLStencilOperationKeep, MTLStencilOperationZero, MTLStencilOperationReplace,
	MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp, MTLStencilOperationInvert,
	MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap,
};

MTLCompareFunction xgpu_metal_compare_function(uint32_t value)
{
	return value < 8 ? compare_functions[value] : MTLCompareFunctionAlways;
}

MTLBlendFactor xgpu_metal_blend_factor(uint32_t value)
{
	return value < 15 ? blend_factors[value] : MTLBlendFactorOne;
}

MTLBlendOperation xgpu_metal_blend_operation(uint32_t value)
{
	return value < 5 ? blend_operations[value] : MTLBlendOperationAdd;
}

MTLStencilOperation xgpu_metal_stencil_operation(uint32_t value)
{
	return value < 8 ? stencil_operations[value] : MTLStencilOperationKeep;
}

MTLVertexFormat xgpu_metal_vertex_format(uint32_t format)
{
	static const MTLVertexFormat vertex_formats[] = {
		[XGPU_METAL_ATTRIBUTE_FLOAT1] = MTLVertexFormatFloat,
		[XGPU_METAL_ATTRIBUTE_FLOAT2] = MTLVertexFormatFloat2,
		[XGPU_METAL_ATTRIBUTE_FLOAT3] = MTLVertexFormatFloat3,
		[XGPU_METAL_ATTRIBUTE_FLOAT4] = MTLVertexFormatFloat4,
		[XGPU_METAL_ATTRIBUTE_D3DCOLOR] = MTLVertexFormatUChar4Normalized_BGRA,
		[XGPU_METAL_ATTRIBUTE_SHORT1] = MTLVertexFormatShort,
		[XGPU_METAL_ATTRIBUTE_SHORT2] = MTLVertexFormatShort2,
		[XGPU_METAL_ATTRIBUTE_SHORT3] = MTLVertexFormatShort3,
		[XGPU_METAL_ATTRIBUTE_SHORT4] = MTLVertexFormatShort4,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT1] = MTLVertexFormatShortNormalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT2] = MTLVertexFormatShort2Normalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT3] = MTLVertexFormatShort3Normalized,
		[XGPU_METAL_ATTRIBUTE_NORMSHORT4] = MTLVertexFormatShort4Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE1] = MTLVertexFormatUCharNormalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE2] = MTLVertexFormatUChar2Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE3] = MTLVertexFormatUChar3Normalized,
		[XGPU_METAL_ATTRIBUTE_PBYTE4] = MTLVertexFormatUChar4Normalized,
		[XGPU_METAL_ATTRIBUTE_FLOAT2H] = MTLVertexFormatFloat3,
		[XGPU_METAL_ATTRIBUTE_PACKED] = MTLVertexFormatUInt,
	};

	return format < sizeof(vertex_formats) / sizeof(vertex_formats[0]) ? vertex_formats[format] : MTLVertexFormatInvalid;
}

/* ---------- render pipeline states */

void xgpu_metal_pipeline_state_key_make(const struct xgpu_metal_pipeline *pipeline,
	struct xgpu_metal_pipeline_state_key *key)
{
	int index;

	memset(key, 0, sizeof(*key));
	key->vertex_shader = pipeline->vertex_shader;
	key->pixel_shader = pipeline->pixel_shader;
	memcpy(key->attributes, pipeline->attributes, sizeof(key->attributes));
	for (index = 0; index < 16; index++)
	{
		if (key->attributes[index].format != XGPU_METAL_ATTRIBUTE_NONE &&
			key->attributes[index].stream < XGPU_METAL_STREAM_COUNT)
			key->strides[key->attributes[index].stream] = pipeline->strides[key->attributes[index].stream];
	}
	key->blend = pipeline->blend;
	if (key->blend)
	{
		key->blend_source = pipeline->blend_source;
		key->blend_destination = pipeline->blend_destination;
		key->blend_operation = pipeline->blend_operation;
	}
	key->color_mask = pipeline->color_mask;
	key->color_format = pipeline->color_format;
	key->depth_format = pipeline->depth_format;
	key->samples = pipeline->samples > 1 ? pipeline->samples : 1;
	key->motion_format = pipeline->motion_format;
	key->motion_mask = pipeline->motion_format ? pipeline->motion_mask : 0;
}

/* a colour attachment's write mask (XGPU_WRITE_*) */
static MTLColorWriteMask write_mask(uint8_t mask)
{
	return ((mask & XGPU_WRITE_RED) ? MTLColorWriteMaskRed : 0) | ((mask & XGPU_WRITE_GREEN) ? MTLColorWriteMaskGreen : 0) |
		((mask & XGPU_WRITE_BLUE) ? MTLColorWriteMaskBlue : 0) | ((mask & XGPU_WRITE_ALPHA) ? MTLColorWriteMaskAlpha : 0);
}

MTLRenderPipelineDescriptor *xgpu_metal_pipeline_descriptor(const struct xgpu_metal_pipeline_state_key *key)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
	MTLVertexDescriptor *vertex = [MTLVertexDescriptor vertexDescriptor];
	int index;

	for (index = 0; index < 16; index++)
	{
		const struct xgpu_metal_vertex_attribute *attribute = &key->attributes[index];

		if (attribute->format == XGPU_METAL_ATTRIBUTE_NONE)
			continue;
		if (attribute->stream == XGPU_METAL_STREAM_CONSTANT)
		{
			/* (its register's value, the same for every vertex) */
			vertex.attributes[index].format = MTLVertexFormatFloat4;
			vertex.attributes[index].offset = (NSUInteger)index * 16;
			vertex.attributes[index].bufferIndex = XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stride = 16 * 16;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stepFunction = MTLVertexStepFunctionConstant;
			vertex.layouts[XGPU_METAL_BUFFER_CONSTANT_ATTRIBUTES].stepRate = 0;
			continue;
		}
		if (attribute->stream >= XGPU_METAL_STREAM_COUNT)
			continue;
		vertex.attributes[index].format = xgpu_metal_vertex_format(attribute->format);
		vertex.attributes[index].offset = attribute->offset;
		vertex.attributes[index].bufferIndex = XGPU_METAL_BUFFER_STREAM(attribute->stream);
		vertex.layouts[XGPU_METAL_BUFFER_STREAM(attribute->stream)].stride =
			key->strides[attribute->stream] ? key->strides[attribute->stream] : 4;
	}
	descriptor.vertexDescriptor = vertex;
	descriptor.rasterSampleCount = key->samples;
	if (key->color_format)
	{
		MTLRenderPipelineColorAttachmentDescriptor *attachment = descriptor.colorAttachments[0];

		attachment.pixelFormat = xgpu_metal_format_information(key->color_format)->pixel_format;
		attachment.writeMask = ((key->color_mask & XGPU_WRITE_RED) ? MTLColorWriteMaskRed : 0) |
			((key->color_mask & XGPU_WRITE_GREEN) ? MTLColorWriteMaskGreen : 0) |
			((key->color_mask & XGPU_WRITE_BLUE) ? MTLColorWriteMaskBlue : 0) |
			((key->color_mask & XGPU_WRITE_ALPHA) ? MTLColorWriteMaskAlpha : 0);
		attachment.blendingEnabled = key->blend != 0;
		attachment.sourceRGBBlendFactor = attachment.sourceAlphaBlendFactor = xgpu_metal_blend_factor(key->blend_source);
		attachment.destinationRGBBlendFactor = attachment.destinationAlphaBlendFactor =
			xgpu_metal_blend_factor(key->blend_destination);
		attachment.rgbBlendOperation = attachment.alphaBlendOperation = xgpu_metal_blend_operation(key->blend_operation);
	}
	/* (display.temporal_aa: the motion, written by the objects' opaque
	draws, the others drawn through it) */
	if (key->motion_format)
	{
		descriptor.colorAttachments[1].pixelFormat = xgpu_metal_format_information(key->motion_format)->pixel_format;
		descriptor.colorAttachments[1].writeMask = write_mask(key->motion_mask);
	}
	if (key->depth_format)
	{
		descriptor.depthAttachmentPixelFormat = xgpu_metal_format_information(key->depth_format)->pixel_format;
		if (xgpu_metal_format_information(key->depth_format)->stencil)
			descriptor.stencilAttachmentPixelFormat = descriptor.depthAttachmentPixelFormat;
	}
	return descriptor;
}

int xgpu_metal_pipeline_archivable(const struct xgpu_metal_pipeline_state_key *key)
{
	return key->color_format && key->color_mask;
}

MTLRenderPipelineDescriptor *xgpu_metal_clear_descriptor(const struct xgpu_metal_clear_key *key)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];

	descriptor.rasterSampleCount = key->samples;
	if (key->color_format)
	{
		descriptor.colorAttachments[0].pixelFormat = xgpu_metal_format_information(key->color_format)->pixel_format;
		descriptor.colorAttachments[0].writeMask = ((key->parts & XGPU_METAL_CLEAR_RED) ? MTLColorWriteMaskRed : 0) |
			((key->parts & XGPU_METAL_CLEAR_GREEN) ? MTLColorWriteMaskGreen : 0) |
			((key->parts & XGPU_METAL_CLEAR_BLUE) ? MTLColorWriteMaskBlue : 0) |
			((key->parts & XGPU_METAL_CLEAR_ALPHA) ? MTLColorWriteMaskAlpha : 0);
	}
	if (key->motion_format)
	{
		descriptor.colorAttachments[1].pixelFormat = xgpu_metal_format_information(key->motion_format)->pixel_format;
		descriptor.colorAttachments[1].writeMask = MTLColorWriteMaskNone;
	}
	if (key->depth_format)
	{
		descriptor.depthAttachmentPixelFormat = xgpu_metal_format_information(key->depth_format)->pixel_format;
		if (xgpu_metal_format_information(key->depth_format)->stencil)
			descriptor.stencilAttachmentPixelFormat = descriptor.depthAttachmentPixelFormat;
	}
	return descriptor;
}

MTLRenderPipelineDescriptor *xgpu_metal_present_descriptor(void)
{
	MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];

	descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	return descriptor;
}

/* (the clears: a rectangle in clip space, a colour, a depth; the
presentation: the frame's picture over the drawable, opaque) */
NSString *const xgpu_metal_fixed_source =
	@"#include <metal_stdlib>\n"
	"using namespace metal;\n"
	"struct clear_parameters { float4 rectangle; float4 color; float depth; };\n"
	"struct clear_vertex { float4 position [[position]]; };\n"
	"vertex clear_vertex xgpu_clear_vertex(uint id [[vertex_id]], constant clear_parameters &p [[buffer(0)]])\n"
	"{\n"
	"\tclear_vertex out;\n"
	"\tfloat2 corner = float2((id & 1) ? p.rectangle.z : p.rectangle.x, (id & 2) ? p.rectangle.w : p.rectangle.y);\n"
	"\tout.position = float4(corner, p.depth, 1.0);\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 xgpu_clear_fragment(constant clear_parameters &p [[buffer(0)]])\n"
	"{\n"
	"\treturn p.color;\n"
	"}\n"
	"struct present_vertex { float4 position [[position]]; float2 coordinates; };\n"
	"vertex present_vertex halo_present_vertex(uint id [[vertex_id]])\n"
	"{\n"
	"\tpresent_vertex out;\n"
	"\tfloat2 corner = float2(id & 1, id >> 1);\n"
	"\tout.position = float4(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);\n"
	"\tout.coordinates = corner;\n"
	"\treturn out;\n"
	"}\n"
	"fragment float4 halo_present_fragment(present_vertex in [[stage_in]], texture2d<float> picture [[texture(0)]],\n"
	"\tsampler linear [[sampler(0)]])\n"
	"{\n"
	"\treturn float4(picture.sample(linear, in.coordinates).rgb, 1.0);\n"
	"}\n";

/* display.temporal_aa's passes (xgpu_metal_temporal.m): the uniforms at
the backend's draw uniforms' place, 17 */
NSString *const xgpu_metal_temporal_source =
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
	"}\n"
	/* FSR's RCAS (port/third_party/fidelityfx-fsr3/gpu/fsr1/ffx_fsr1.h's
	FsrRcasF, as ffx_fsr3upscaler_rcas.h builds it: 32-bit, FSR_RCAS_DENOISE,
	the exposure 1), ported to MSL as a pass over the destination's rectangle
	(window.xy its corner) reading MetalFX's output (its pixels clamped at its
	edges); metrics.x FsrRcasCon's con[0], exp2(-(2 - 2 * sharpness)); the
	alpha passed through. Where a ring's channel is 0 or 1 throughout, its
	limits' reciprocals are kept finite (the SDK's are infinite there and its
	GPUs' max takes the other side of the NaN), the result the same.

	This part is ported from the AMD FidelityFX SDK:

	Copyright (C) 2024 Advanced Micro Devices, Inc.

	Permission is hereby granted, free of charge, to any person obtaining a copy
	of this software and associated documentation files(the "Software"), to deal
	in the Software without restriction, including without limitation the rights
	to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
	copies of the Software, and to permit persons to whom the Software is
	furnished to do so, subject to the following conditions :

	The above copyright notice and this permission notice shall be included in
	all copies or substantial portions of the Software.

	THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
	IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
	FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
	AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
	LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
	OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
	THE SOFTWARE. */
	"static float rcas_reciprocal_medium(float a)\n"
	"{\n"
	"\tfloat b = as_type<float>(0x7ef19fffu - as_type<uint>(a));\n"
	"\treturn b * (-b * a + 2.0);\n"
	"}\n"
	"static float rcas_luma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }\n"
	"fragment float4 xgpu_temporal_rcas(temporal_out in [[stage_in]], constant motion_uniforms &u [[buffer(17)]],\n"
	"\ttexture2d<float> color [[texture(0)]])\n"
	"{\n"
	"\tint2 last = int2(color.get_width(), color.get_height()) - 1;\n"
	"\tint2 sp = int2(in.position.xy - u.window.xy);\n"
	"\tfloat3 b = color.read(uint2(clamp(sp + int2(0, -1), int2(0), last))).rgb;\n"
	"\tfloat3 d = color.read(uint2(clamp(sp + int2(-1, 0), int2(0), last))).rgb;\n"
	"\tfloat4 ee = color.read(uint2(clamp(sp, int2(0), last)));\n"
	"\tfloat3 e = ee.rgb;\n"
	"\tfloat3 f = color.read(uint2(clamp(sp + int2(1, 0), int2(0), last))).rgb;\n"
	"\tfloat3 h = color.read(uint2(clamp(sp + int2(0, 1), int2(0), last))).rgb;\n"
	"\tfloat bL = rcas_luma(b), dL = rcas_luma(d), eL = rcas_luma(e), fL = rcas_luma(f), hL = rcas_luma(h);\n"
	"\tfloat nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;\n"
	"\tnz = saturate(abs(nz) * rcas_reciprocal_medium(max(max(max(bL, dL), eL), max(fL, hL)) -\n"
	"\t\tmin(min(min(bL, dL), eL), min(fL, hL))));\n"
	"\tnz = -0.5 * nz + 1.0;\n"
	"\tfloat3 mn4 = min(min(min(b, d), f), h);\n"
	"\tfloat3 mx4 = max(max(max(b, d), f), h);\n"
	"\tfloat3 hit_min = mn4 * (1.0 / max(4.0 * mx4, 1.0e-30));\n"
	"\tfloat3 hit_max = (1.0 - mx4) * (1.0 / min(4.0 * mn4 - 4.0, -1.0e-30));\n"
	"\tfloat3 lobe3 = max(-hit_min, hit_max);\n"
	"\tfloat lobe = max(-(0.25 - 1.0 / 16.0), min(max(max(lobe3.r, lobe3.g), lobe3.b), 0.0)) * u.metrics.x;\n"
	"\tlobe *= nz;\n"
	"\tfloat rcpL = rcas_reciprocal_medium(4.0 * lobe + 1.0);\n"
	"\treturn float4((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, ee.a);\n"
	"}\n";

/* ---------- the command stream's pipelines */

/* the stream's target formats (XGPU_FORMAT_*: BGRA8 1, D24S8 2, RGBA16F 3,
RGBA16 5) */
static uint8_t target_format(uint8_t format)
{
	return format == 1 ? XGPU_METAL_FORMAT_BGRA8 : format == 2 ? XGPU_METAL_FORMAT_D24S8 :
		format == 3 ? XGPU_METAL_FORMAT_RGBA16F : format == 5 ? XGPU_METAL_FORMAT_RGBA16 :
		format == XGPU_STREAM_FORMAT_WORLD_AO_INPUTS ? XGPU_METAL_FORMAT_RGBA32UI : XGPU_METAL_FORMAT_NONE;
}

void xgpu_metal_pipeline_from_key(const struct xgpu_stream_pipeline_key *key, struct xgpu_metal_pipeline *pipeline)
{
	memset(pipeline, 0, sizeof(*pipeline));
	/* (display.hdr: a draw whose pixel shader blends it itself, reading the
	pixel it draws over, has no blending of Metal's: xgpu_hdr.h) */
	pipeline->blend = XGPU_HDR_SHADER_BLENDS(key->pixel.hdr) ? 0 : key->blend;
	pipeline->blend_source = key->blend_source;
	pipeline->blend_destination = key->blend_destination;
	pipeline->blend_operation = key->blend_operation;
	pipeline->color_mask = key->color_mask;
	pipeline->depth_test = key->depth_test;
	pipeline->depth_function = key->depth_function;
	pipeline->depth_write = key->depth_write;
	pipeline->stencil_test = key->stencil_test;
	pipeline->stencil_function = key->stencil_function;
	pipeline->stencil_fail = key->stencil_fail;
	pipeline->stencil_depth_fail = key->stencil_depth_fail;
	pipeline->stencil_pass = key->stencil_pass;
	pipeline->stencil_read_mask = (uint8_t)key->stencil_read_mask;
	pipeline->stencil_write_mask = (uint8_t)key->stencil_write_mask;
	pipeline->cull = key->cull;
	pipeline->front_counter_clockwise = key->front_counter_clockwise;
	pipeline->cull_front = key->cull_front;
	pipeline->fill = key->fill;
	pipeline->depth_bias = key->depth_bias;
	pipeline->color_format = target_format(key->color_format);
	pipeline->depth_format = target_format(key->depth_format);
	pipeline->samples = key->samples > 1 ? key->samples : 1;
}

/* XGPU_VERTEX_*: XGPU_METAL_ATTRIBUTE_* are the same */
static uint8_t attribute_format(uint8_t format)
{
	return format < XGPU_METAL_ATTRIBUTE_COUNT ? format : XGPU_METAL_ATTRIBUTE_NONE;
}

uint16_t xgpu_metal_layout_attributes(const struct xgpu_stream_vertex_element *elements, uint32_t count,
	struct xgpu_metal_vertex_attribute attributes[16])
{
	uint32_t fed = 0, integers = 0, index;

	memset(attributes, 0, 16 * sizeof(attributes[0]));
	for (index = 0; index < count; index++)
	{
		const struct xgpu_stream_vertex_element *element = &elements[index];

		if (element->reg >= 16 || element->stream >= XGPU_METAL_STREAM_COUNT)
			continue;
		attributes[element->reg].format = attribute_format(element->format);
		attributes[element->reg].stream = element->stream;
		attributes[element->reg].offset = element->offset;
		if (attributes[element->reg].format != XGPU_METAL_ATTRIBUTE_NONE)
			fed |= 1u << element->reg;
		/* (unnormalized shorts are read as integers, which the shader
		converts) */
		if (element->format >= XGPU_METAL_ATTRIBUTE_SHORT1 && element->format <= XGPU_METAL_ATTRIBUTE_SHORT4)
			integers |= 1u << element->reg;
	}
	/* (every other input, from its value) */
	for (index = 0; index < 16; index++)
	{
		if (!(fed & (1u << index)))
		{
			attributes[index].format = XGPU_METAL_ATTRIBUTE_FLOAT4;
			attributes[index].stream = XGPU_METAL_STREAM_CONSTANT;
		}
	}
	return (uint16_t)integers;
}

uint32_t xgpu_metal_layout_immediate(uint32_t written, struct xgpu_metal_vertex_attribute attributes[16],
	uint16_t strides[XGPU_METAL_STREAM_COUNT])
{
	uint32_t index, count = 0;

	memset(attributes, 0, 16 * sizeof(attributes[0]));
	memset(strides, 0, XGPU_METAL_STREAM_COUNT * sizeof(strides[0]));
	for (index = 0; index < 16; index++)
	{
		attributes[index].format = XGPU_METAL_ATTRIBUTE_FLOAT4;
		if (written & (1u << index))
		{
			attributes[index].stream = 0;
			attributes[index].offset = (uint16_t)(count++ * 16);
		}
		else
		{
			attributes[index].stream = XGPU_METAL_STREAM_CONSTANT;
		}
	}
	strides[0] = (uint16_t)(count * 16);
	return count;
}

/* XGPU_SOURCE_NONE; _xgpu_sampler_3d, _cube, _2d_layered (xgpu.h) */
enum
{
	_source_none = 0,
	_sampler_3d = 2,
	_sampler_cube = 3,
	_sampler_2d_layered = 4,
};

_Static_assert(XGPU_METAL_ADDRESS_REGION == 5, "XGPU_ADDRESS_REGION's number");

uint8_t xgpu_metal_border_axes(uint8_t source, uint8_t sampler_type, const struct xgpu_stream_sampler *sampler)
{
	uint8_t axes = 0;
	int axis;

	/* (the shader draws the border of a 2D or 3D texture's sampler, whatever
	its colour, the uniforms': the pipeline is the same for every colour. A
	draw's border colour is the texture stage's as the draw before left it
	where the game's code sets none, so that a pipeline told apart by it,
	Metal's own three colours drawn by the sampler and the others by the
	shader, was made as the map was prepared for one and missed in play for
	the other: a10's screen effects, prepared with 0x05050505 and drawn
	with 0) */
	if (source == _source_none || sampler_type == _sampler_cube || sampler_type == _sampler_2d_layered)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		if (sampler->address[axis] == XGPU_METAL_ADDRESS_BORDER)
			axes |= (uint8_t)(1u << axis);
	}
	/* (a region of a 2D texture: its border on u and v, the shader's) */
	if (sampler->address[0] == XGPU_METAL_ADDRESS_REGION && sampler_type != _sampler_3d)
		axes = 3 | XGPU_METAL_BORDER_REGION;
	/* (the high-res text's distance fields, a 2D texture's: the shader's) */
	if (sampler->address[2] == XGPU_METAL_ADDRESS_MSDF && sampler_type != _sampler_3d)
		axes |= XGPU_METAL_BORDER_MSDF;
	return axes;
}
