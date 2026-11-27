// W3's A-SVGF gradients (Schied 2018; xgpu_vulkan_denoise.c): one texel a 3x3 stratum of the PREVIOUS frame. Its
// gradient pixel (lt/gradient_pixel.glsl, by that frame's number) was re-shaded (xgpu_vulkan_lt_reshade: its surface
// and seed, this frame's light and scene); where that surface is still seen, the same surface, in this frame (the
// point carried forward by the cameras), the light's change there is the re-shade less the previous frame's own 1-spp
// sample: the same sample, so nothing but the real change. Out, in luminance: (the previous sample, the re-shaded one,
// |change|, 1), or 0 where there is none. The temporal stage reads them about the point its history comes from.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"
#include "gradient_pixel.glsl"

layout(set = 1, binding = 0) uniform usampler2D guide;
layout(set = 1, binding = 1) uniform usampler2D previous_guide;
layout(set = 1, binding = 2) uniform sampler2D reshaded;
layout(set = 1, binding = 3) uniform sampler2D previous_raw;

layout(location = 0) out vec4 result;

void main()
{
    ivec2 stratum = ivec2(gl_FragCoord.xy);
    result = vec4(0.0);
    ivec2 q = ivec2(-1);
    for (int k = 0; k < 9 && q.x < 0; k++)
        if (lt_gradient_pixel(stratum * 3 + ivec2(k % 3, k / 3), pass.tuning.x))
            q = stratum * 3 + ivec2(k % 3, k / 3);
    if (q.x < 0 || any(lessThan(q, pass.previous_corners.xy)) || any(greaterThanEqual(q, pass.previous_corners.zw)))
        return;
    vec4 now = texelFetch(reshaded, q, 0);
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
    float a = luminance(max(now.rgb, vec3(0.0))), b = luminance(max(texelFetch(previous_raw, q, 0).rgb, vec3(0.0)));
    result = vec4(b, a, abs(a - b), 1.0);
}
