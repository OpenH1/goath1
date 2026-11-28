// W3's spatial stage (xgpu_vulkan_denoise.c): one pass of a variance-guided a-trous filter (SVGF, Schied 2017), the
// 5x5 B3 kernel at a step of control.z pixels (1, 2, 4 over the passes), its weights stopped at edges: another
// owner, a pixel not drawn, the depth off this pixel's plane, the shaded normal's turn, and a luminance difference
// beyond its estimate's deviation (prefiltered 3x3; under 4 frames a spatial guess, and steps 1 and 2 alone). The weights are normalized, so a flat region's mean is kept. The first pass reads the temporal stage's mean
// (its deviation: the samples' over the root of the count); each passes on (rgb, the estimate's standard deviation,
// propagated as the root of the sum of w^2 var over the weights' sum), in half floats. Each pixel's guides are read
// from the filter guide (lt/denoise_guide.frag): one 8-byte texel.
// PAIR (denoise_atrous_pair.frag) and FINAL (denoise_atrous_final.frag): lt/denoise_pair.glsl, the two channels
// filtered together and the last pass into the light the surfaces read.
#version 460
#extension GL_GOOGLE_include_directive : require
#if defined(PAIR)
#extension GL_KHR_shader_subgroup_vote : require
#endif

#include "denoise_common.glsl"

#if !defined(PAIR) && !defined(FINAL)
layout(set = 1, binding = 0) uniform sampler2D source;
layout(set = 1, binding = 1) uniform usampler2D history;
layout(set = 1, binding = 2) uniform usampler2D filter_guide;

layout(location = 0) out vec4 result;
#endif

#if !defined(PAIR) && !defined(FINAL)
// a pixel's (rgb, the estimate's variance)
vec4 fetch(ivec2 q)
{
    if ((pass.control.y & FIRST) == 0)
    {
        vec4 c = texelFetch(source, q, 0);
        return vec4(c.rgb, c.a * c.a);
    }
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

void single_main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec2 g = texelFetch(filter_guide, pixel, 0).xy;
    result = vec4(0.0);
    if ((g.y & 0xFFFFu) == 0u)
        return;
    vec4 centre = fetch(pixel);
    float count = unpackHalf2x16(texelFetch(history, pixel, 0).w).x;
    // (the specular channel: nothing where the pixel has none, and its neighbours without one left out)
    bool specular = (pass.control.y & SPECULAR) != 0;
    if (specular && count <= 0.0)
        return;
    // (step 0, checking: the temporal stage's mean alone; a pixel under 4 frames filtered by the first two passes
    // alone, steps 1 and 2: its noise is guessed from its neighbours, and a guess wrong at an edge stays small)
    if (pass.control.z == 0 || (pass.control.z > 2 && count < 4.0))
    {
        result = vec4(centre.rgb, sqrt(max(centre.a, 0.0)));
        return;
    }
    vec4 normals = unpackSnorm4x8(g.x);
    vec3 shaded = unfold(normals.xy), n = unfold(normals.zw);
    float distance = unpackHalf2x16(g.y).x;
    vec3 p = world_point(pixel, distance);
    float lp = luminance(centre.rgb);
    // (the variance prefiltered 3x3, Gaussian, over the same surface; in the first pass, under 4 frames, where the
    // count has too few samples for its own, a spatial guess instead: the least of the luminance's variances over the
    // four 2x2 quadrants about the pixel (Kuwahara's), among pixels of nearly the same normals, so that an edge in
    // the light, which one quadrant at least is clear of, is not taken for noise; it then stands for each young
    // pixel's in the propagation)
    // (a level lit live: the diffuse's first frame already many rays, its own variance; the pair's rule,
    // lt/denoise_pair.glsl's, so the diffuse is filtered the same with the reflections off as with them on)
    bool young = (pass.control.y & FIRST) != 0 && count < 4.0 && (specular || pass.suns.y == 0);
    float v = 0.0, vw = 0.0;
    vec3 quadrants[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            uvec2 other = texelFetch(filter_guide, q, 0).xy;
            if (!inside(q) || (other.y >> 16) != (g.y >> 16) || (other.y & 0xFFFFu) == 0u ||
                (specular && unpackHalf2x16(texelFetch(history, q, 0).w).x <= 0.0))
                continue;
            float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            vec4 c = fetch(q);
            v += w * c.a;
            vw += w;
            vec4 on = unpackSnorm4x8(other.x);
            if (young && dot(unfold(on.xy), shaded) >= 0.9994 && dot(unfold(on.zw), n) >= 0.99)
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
    float sigma_z = pass.tuning.z * distance + 1.0e-4;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
        {
            ivec2 q = pixel + ivec2(x, y) * step;
            if (!inside(q))
                continue;
            uvec2 other = texelFetch(filter_guide, q, 0).xy;
            if ((other.y & 0xFFFFu) == 0u || (other.y >> 16) != (g.y >> 16) ||
                (specular && unpackHalf2x16(texelFetch(history, q, 0).w).x <= 0.0))
                continue;
            vec4 c = fetch(q);
            if (young && unpackHalf2x16(texelFetch(history, q, 0).w).x < 4.0)
                c.a = spatial;
            float w = kernel[abs(x)] * kernel[abs(y)];
            if (x != 0 || y != 0)
            {
                vec3 pq = world_point(q, unpackHalf2x16(other.y).x);
                vec4 on = unpackSnorm4x8(other.x);
                // (the shaded normal's turn, strictly: the bump map's detail is in the light, and kept; the depth off
                // this pixel's plane; the luminance)
                float e = (1.0 - dot(shaded, unfold(on.xy))) / pass.tuning.w + abs(dot(n, pq - p)) / sigma_z +
                    abs(luminance(c.rgb) - lp) / sigma_l;
                w *= exp(-e);
            }
            sum += w * c.rgb;
            variance += w * w * c.a;
            weights += w;
        }
    result = vec4(sum / weights, sqrt(variance) / weights);
}

void main()
{
    single_main();
}
#else
#include "denoise_pair.glsl"
#endif
