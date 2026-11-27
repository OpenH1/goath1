// W3's spatial stage (xgpu_vulkan_denoise.c): one pass of a variance-guided a-trous filter (SVGF, Schied 2017), the
// 5x5 B3 kernel at a step of control.z pixels (1, 2, 4, 8 over the passes), its weights stopped at edges: another
// owner, a pixel not drawn, the depth off this pixel's plane, the shaded normal's turn, and a luminance difference
// beyond its estimate's deviation (prefiltered 3x3; under 4 frames, the spatial deviation there). The weights are normalized, so a
// flat region's mean is kept. The first pass reads the temporal stage's mean (its variance: the samples' over the
// count); each passes on (rgb, the estimate's variance, propagated as the sum of w^2 var over the weights' squared).
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 1) uniform usampler2D history;
layout(set = 1, binding = 2) uniform usampler2D inputs;
layout(set = 1, binding = 3) uniform usampler2D guides;

layout(location = 0) out vec4 result;

// a pixel's (rgb, the estimate's variance)
vec4 fetch(ivec2 q)
{
    if ((pass.control.y & FIRST) == 0)
        return texelFetch(source, q, 0);
    uvec4 h = texelFetch(history, q, 0);
    vec3 m = vec3(uintBitsToFloat(h.x), uintBitsToFloat(h.y), uintBitsToFloat(h.z));
    vec2 cv = unpackHalf2x16(h.w);
    float l = luminance(m);
    return vec4(m, cv.y * l * l / max(cv.x, 1.0));
}

bool inside(ivec2 q)
{
    return all(greaterThanEqual(q, pass.corners.xy)) && all(lessThan(q, pass.corners.zw));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = vec4(0.0);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec4 centre = fetch(pixel);
    // (step 0, checking: the temporal stage's mean alone)
    if (pass.control.z == 0)
    {
        result = centre;
        return;
    }
    float count = unpackHalf2x16(texelFetch(history, pixel, 0).w).x;
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 shaded = unfold(unpackSnorm2x16(texelFetch(guides, pixel, 0).x));
    if (dot(shaded, shaded) < 0.5 || dot(shaded, n) < 0.0)
        shaded = n;
    float distance = uintBitsToFloat(words.z);
    vec3 p = world_point(pixel, distance);
    float lp = luminance(centre.rgb);
    // (the variance prefiltered 3x3, Gaussian, over the same surface; in the first pass, under 4 frames, where the
    // count has too few samples for its own, the luminance's spatial variance there instead, SVGF's: it then stands
    // for each young pixel's in the propagation)
    bool young = (pass.control.y & FIRST) != 0 && count < 4.0;
    float v = 0.0, vw = 0.0, m1 = 0.0, m2 = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            uvec4 other = texelFetch(inputs, q, 0);
            if (!inside(q) || other.w != words.w || (other.y & INPUT_PRESENT) == 0u)
                continue;
            float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            vec4 c = fetch(q);
            float l = luminance(c.rgb);
            v += w * c.a;
            m1 += w * l;
            m2 += w * l * l;
            vw += w;
        }
    vw = max(vw, 1.0e-6);
    float spatial = max(m2 / vw - (m1 / vw) * (m1 / vw), 0.0);
    float deviation = sqrt(young ? spatial : max(v / vw, 0.0));
    const float kernel[3] = float[3](0.375, 0.25, 0.0625);
    vec3 sum = vec3(0.0);
    float weights = 0.0, variance = 0.0;
    int step = max(pass.control.z, 1);
    float sigma_l = pass.tuning.y * deviation + 1.0e-3 * lp + 1.0e-7;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
        {
            ivec2 q = pixel + ivec2(x, y) * step;
            if (!inside(q))
                continue;
            uvec4 other = texelFetch(inputs, q, 0);
            if ((other.y & INPUT_PRESENT) == 0u || other.w != words.w)
                continue;
            vec4 c = fetch(q);
            if (young && unpackHalf2x16(texelFetch(history, q, 0).w).x < 4.0)
                c.a = spatial;
            float w = kernel[abs(x)] * kernel[abs(y)];
            if (x != 0 || y != 0)
            {
                vec3 ns = unfold(unpackSnorm2x16(texelFetch(guides, q, 0).x));
                vec3 nq = unfold(unpackSnorm2x16(other.x));
                if (dot(ns, ns) < 0.5 || dot(ns, nq) < 0.0)
                    ns = nq;
                vec3 pq = world_point(q, uintBitsToFloat(other.z));
                w *= pow(max(dot(shaded, ns), 0.0), pass.tuning.w);
                w *= exp(-abs(dot(n, pq - p)) / (pass.tuning.z * distance + 1.0e-4));
                w *= exp(-abs(luminance(c.rgb) - lp) / sigma_l);
            }
            sum += w * c.rgb;
            variance += w * w * c.a;
            weights += w;
        }
    result = vec4(sum / weights, variance / (weights * weights));
}
