// W3's A-SVGF gradients (Schied 2018; xgpu_vulkan_denoise.c): one texel a 3x3 stratum of the PREVIOUS frame. Its
// gradient pixel (lt/gradient_pixel.glsl, by that frame's number) was re-shaded (xgpu_vulkan_lt_reshade: its surface
// and seed, this frame's light and scene); where that surface is still seen, the same surface, in this frame (the
// point carried forward by the cameras), the light's change there is the re-shade less the previous frame's own 1-spp
// sample: the same sample, so nothing but the real change. Out (RGBA32Uint), in colour, as half floats: the previous
// sample's rgb and the re-shaded one's (x: r, g before; y: b before, r now; z: g, b now) and w 1, or 0 where there is
// none. The temporal stage reads them about the point its history comes from: in colour, so that a light of one hue
// that goes (a plasma bolt's) takes its hue out of the history, not only its brightness.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"
#include "gradient_pixel.glsl"

layout(set = 1, binding = 0) uniform usampler2D guide;
layout(set = 1, binding = 1) uniform usampler2D previous_guide;
layout(set = 1, binding = 2) uniform sampler2D reshaded;
layout(set = 1, binding = 3) uniform sampler2D previous_raw;

layout(location = 0) out uvec4 result;

void main()
{
    ivec2 stratum = ivec2(gl_FragCoord.xy);
    result = uvec4(0u);
    ivec2 q = lt_gradient_pixel_in(stratum, pass.tuning.x);
    if (any(lessThan(q, pass.previous_corners.xy)) || any(greaterThanEqual(q, pass.previous_corners.zw)))
        return;
    // (the re-shade: a texel a stratum)
    vec4 now = texelFetch(reshaded, stratum, 0);
    uvec2 before = texelFetch(previous_guide, q, 0).xy;
    if (now.a <= 0.0 || (before.y & 0xFFFFu) == 0u)
        return;
    // (its point, from the previous camera, carried into this frame's pixels)
    vec3 p = previous_point(q, unpackHalf2x16(before.y).x);
    vec2 at;
    if (!project(p, at))
        return;
    ivec2 c = ivec2(floor(at));
    uvec2 here = texelFetch(guide, c, 0).xy;
    float distance = length(p - pass.camera[0].xyz);
    if ((here.y & 0xFFFFu) == 0u || (here.y >> 16) != (before.y >> 16) ||
        abs(unpackHalf2x16(here.y).x - distance) > DEPTH_SHARE * distance ||
        dot(unfold(unpackSnorm4x8(here.x).zw), unfold(unpackSnorm4x8(before.x).zw)) < COS_25)
        return;
    vec3 a = clamp(now.rgb, vec3(0.0), vec3(60000.0));
    vec3 b = clamp(texelFetch(previous_raw, q, 0).rgb, vec3(0.0), vec3(60000.0));
    result = uvec4(packHalf2x16(b.rg), packHalf2x16(vec2(b.b, a.r)), packHalf2x16(a.gb), 1u);
}
