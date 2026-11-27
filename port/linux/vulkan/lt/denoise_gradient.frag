// W3's A-SVGF gradients (Schied 2018; xgpu_vulkan_denoise.c): one texel a 3x3 stratum of this frame. Each of the
// stratum's pixels found again in the frame before (the same surface there); where that is one of its gradient pixels
// (lt/gradient_pixel.glsl, by its frame's number), the light's change there is the re-shade's (the previous frame's
// surface and seed, this frame's light and scene: xgpu_vulkan_lt_reshade) less the previous frame's own 1-spp sample:
// the same sample, so nothing but the real change. Out, in luminance: (sum of the previous samples, sum of the
// re-shaded ones, sum of |change|, samples).
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"
#include "gradient_pixel.glsl"

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform usampler2D previous_inputs;
layout(set = 1, binding = 2) uniform sampler2D reshaded;
layout(set = 1, binding = 3) uniform sampler2D previous_raw;
layout(set = 1, binding = 4) uniform usampler2D guides;

layout(location = 0) out vec4 result;

void main()
{
    ivec2 stratum = ivec2(gl_FragCoord.xy);
    vec4 sums = vec4(0.0);
    for (int k = 0; k < 9; k++)
    {
        ivec2 pixel = stratum * 3 + ivec2(k % 3, k / 3);
        if (any(lessThan(pixel, pass.corners.xy)) || any(greaterThanEqual(pixel, pass.corners.zw)))
            continue;
        uvec4 words = texelFetch(inputs, pixel, 0);
        if ((words.y & INPUT_PRESENT) == 0u)
            continue;
        vec3 n = unfold(unpackSnorm2x16(words.x));
        vec3 p = world_point(pixel, uintBitsToFloat(words.z));
        vec2 at;
        if (!reproject(p, pixel, texelFetch(guides, pixel, 0).w, at))
            continue;
        ivec2 q = ivec2(floor(at));
        if (!lt_gradient_pixel(q, pass.tuning.x) || !same_surface(texelFetch(previous_inputs, q, 0), p, n, words.w))
            continue;
        vec4 now = texelFetch(reshaded, q, 0);
        if (now.a <= 0.0)
            continue;
        float a = luminance(max(now.rgb, vec3(0.0))), b = luminance(max(texelFetch(previous_raw, q, 0).rgb, vec3(0.0)));
        sums += vec4(b, a, abs(a - b), 1.0);
    }
    result = sums;
}
