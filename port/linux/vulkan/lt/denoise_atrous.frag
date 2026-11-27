// W3's spatial stage (xgpu_vulkan_denoise.c): one pass of a variance-guided a-trous filter (SVGF, Schied 2017), the
// 5x5 B3 kernel at a step of control.z pixels (1, 2, 4, 8 over the passes), its weights stopped at edges: another
// owner, a pixel not drawn, the depth off this pixel's plane, the shaded normal's turn, and a luminance difference
// beyond its estimate's deviation (prefiltered 3x3; under 4 frames, a spatial guess, and steps 1 and 2 alone). The weights
// are normalized, so a
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

// a pixel's shaded normal (the guides'; its geometric one where they have none)
vec3 shaded_normal(ivec2 q, uvec4 words)
{
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 s = unfold(unpackSnorm2x16(texelFetch(guides, q, 0).x));
    return dot(s, s) < 0.5 || dot(s, n) < 0.0 ? n : s;
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
    float count = unpackHalf2x16(texelFetch(history, pixel, 0).w).x;
    // (step 0, checking: the temporal stage's mean alone; a pixel under 4 frames filtered by the first two passes
    // alone, steps 1 and 2: its noise is guessed from its neighbours, and a guess wrong at an edge stays small)
    if (pass.control.z == 0 || (pass.control.z > 2 && count < 4.0))
    {
        result = centre;
        return;
    }
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 shaded = shaded_normal(pixel, words);
    float distance = uintBitsToFloat(words.z);
    vec3 p = world_point(pixel, distance);
    float lp = luminance(centre.rgb);
    // (the variance prefiltered 3x3, Gaussian, over the same surface; in the first pass, under 4 frames, where the
    // count has too few samples for its own, a spatial guess instead: the least of the luminance's variances over the
    // four 2x2 quadrants about the pixel (Kuwahara's), among pixels of nearly the same normals, so that an edge in
    // the light, which one quadrant at least is clear of, is not taken for noise; it then stands for each young
    // pixel's in the propagation)
    bool young = (pass.control.y & FIRST) != 0 && count < 4.0;
    float v = 0.0, vw = 0.0;
    vec3 quadrants[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            uvec4 other = texelFetch(inputs, q, 0);
            if (!inside(q) || other.w != words.w || (other.y & INPUT_PRESENT) == 0u)
                continue;
            float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            vec4 c = fetch(q);
            v += w * c.a;
            vw += w;
            if (young && dot(shaded_normal(q, other), shaded) >= 0.9994 &&
                dot(unfold(unpackSnorm2x16(other.x)), n) >= 0.99)
            {
                float l = luminance(c.rgb);
                for (int k = 0; k < 4; k++)
                    if ((x == 0 || x == ((k & 1) != 0 ? 1 : -1)) && (y == 0 || y == ((k & 2) != 0 ? 1 : -1)))
                        quadrants[k] += vec3(1.0, l, l * l);
            }
        }
    vw = max(vw, 1.0e-6);
    float spatial = 1.0e30;
    for (int k = 0; k < 4; k++)
        if (quadrants[k].x >= 3.0)
            spatial = min(spatial, max(quadrants[k].z / quadrants[k].x - pow(quadrants[k].y / quadrants[k].x, 2.0), 0.0));
    if (spatial > 1.0e29)
        spatial = 0.0;
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
                vec3 pq = world_point(q, uintBitsToFloat(other.z));
                // (the shaded normal's turn, strictly: the bump map's detail is in the light, and kept)
                w *= exp(-(1.0 - dot(shaded, shaded_normal(q, other))) / pass.tuning.w);
                // (and the geometric one's, more loosely: which side faces the light)
                w *= exp(-(1.0 - dot(n, unfold(unpackSnorm2x16(other.x)))) / (10.0 * pass.tuning.w));
                w *= exp(-abs(dot(n, pq - p)) / (pass.tuning.z * distance + 1.0e-4));
                w *= exp(-abs(luminance(c.rgb) - lp) / sigma_l);
            }
            sum += w * c.rgb;
            variance += w * w * c.a;
            weights += w;
        }
    result = vec4(sum / weights, variance / (weights * weights));
}
