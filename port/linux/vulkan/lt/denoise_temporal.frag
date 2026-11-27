// W3's temporal stage (xgpu_vulkan_denoise.c): the history found again where it is the same surface (a bilinear
// gather of the previous frame's four texels about the reprojected point, each kept only where it is the same surface:
// owner, distance within 3%, normal within 25 degrees; none kept, a disocclusion: the count starts again), its count
// cut by the A-SVGF gradient (the share of the light that changed: only where it did), then this frame's sample
// added by count (1/n, n up to the cap: the mean, never an exponential average; the history is never clamped to the
// neighbourhood). Out (RGBA32Uint): the mean's rgb as floats, then packHalf2x16(count, the samples' relative
// variance: the second moment of the luminance over the mean's squared, less 1).
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D raw;
layout(set = 1, binding = 1) uniform usampler2D inputs;
layout(set = 1, binding = 2) uniform usampler2D guides;
layout(set = 1, binding = 3) uniform usampler2D previous_inputs;
layout(set = 1, binding = 4) uniform usampler2D history;
layout(set = 1, binding = 5) uniform sampler2D gradients;

layout(location = 0) out uvec4 result;

// the gradient's gain: a change's share cut from the history four times over (A-SVGF's history weight goes up with the
// normalized gradient; here, so that a light coming on over a few frames is followed within them, while a lamp
// drifting a percent a frame keeps most of a 64-frame history)
const float GAIN = 4.0;

// the share of the light that changed about pixel's stratum (its 3x3 strata: their sums' ratio, by the gain)
float changed(ivec2 pixel)
{
    if ((pass.control.y & GRADIENTS) == 0)
        return 0.0;
    ivec2 stratum = pixel / 3, top = ivec2(pass.size.zw) - 1;
    vec2 sums = vec2(0.0);
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
            sums += texelFetch(gradients, clamp(stratum + ivec2(x, y), ivec2(0), top), 0).xy;
    return sums.y > 1.0e-8 ? clamp(GAIN * sums.x / sums.y, 0.0, 1.0) : 0.0;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = uvec4(0u);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec3 sample_rgb = max(texelFetch(raw, pixel, 0).rgb, vec3(0.0));
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 p = world_point(pixel, uintBitsToFloat(words.z));
    vec3 mean = vec3(0.0);
    float count = 0.0, moment = 0.0;
    vec2 at;
    if ((pass.control.y & HISTORY) != 0 && pass.history.x != 0.0 &&
        reproject(p, pixel, texelFetch(guides, pixel, 0).w, at))
    {
        vec2 f = at - 0.5;
        ivec2 base = ivec2(floor(f));
        vec2 t = f - vec2(base);
        float weights = 0.0;
        vec3 rgb = vec3(0.0);
        float counts = 0.0, moments = 0.0;
        for (int k = 0; k < 4; k++)
        {
            ivec2 q = base + ivec2(k & 1, k >> 1);
            float w = ((k & 1) != 0 ? t.x : 1.0 - t.x) * ((k >> 1) != 0 ? t.y : 1.0 - t.y);
            if (w <= 0.0 || any(lessThan(q, pass.previous_corners.xy)) ||
                any(greaterThanEqual(q, pass.previous_corners.zw)) ||
                !same_surface(texelFetch(previous_inputs, q, 0), p, n, words.w))
                continue;
            uvec4 h = texelFetch(history, q, 0);
            vec2 cv = unpackHalf2x16(h.w);
            vec3 m = vec3(uintBitsToFloat(h.x), uintBitsToFloat(h.y), uintBitsToFloat(h.z));
            float l = luminance(m);
            rgb += w * m;
            counts += w * cv.x;
            moments += w * (cv.y + 1.0) * l * l;
            weights += w;
        }
        // (a little of the footprint kept is enough: its weights normalized)
        if (weights > 0.01)
        {
            mean = rgb / weights;
            count = counts / weights;
            moment = moments / weights;
        }
    }
    // (anti-lag: the count cut by the share that changed)
    count *= 1.0 - changed(pixel);
    count = min(count + 1.0, float(pass.control.x));
    if ((pass.control.y & EMA) != 0)
        count = float(pass.control.x);
    mean += (sample_rgb - mean) / count;
    float l = luminance(sample_rgb);
    moment += (l * l - moment) / count;
    float lm = luminance(mean);
    float relative = lm > 1.0e-12 ? max(moment / (lm * lm) - 1.0, 0.0) : 0.0;
    result = uvec4(floatBitsToUint(mean.r), floatBitsToUint(mean.g), floatBitsToUint(mean.b),
        packHalf2x16(vec2(count, min(relative, 60000.0))));
}
