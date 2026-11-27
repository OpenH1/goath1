// W3's temporal stage (xgpu_vulkan_denoise.c): the history found again where it is the same surface (owner, distance
// within 3%, normal within 25 degrees; a bilinear gather of the four texels about the reprojected point, each kept
// only where it is; Catmull-Rom's 4x4 for the mean where all 16 are; none kept, a disocclusion: the count starts
// again). The A-SVGF gradients then relight it (their ratio, where the light changed alike) and cut its count (where
// it changed otherwise, and at the pixels that moved beyond their spread there); in motion its cap comes down to 8.
// This frame's sample is added by count (1/n, n up to the cap: the mean, never an exponential average; the history is
// never clamped to the neighbourhood). Out (RGBA32Uint): the mean's rgb as floats, then packHalf2x16(count, the
// samples' relative variance: the second moment of the luminance over the mean's squared, less 1).
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform sampler2D raw;
layout(set = 1, binding = 1) uniform usampler2D inputs;
layout(set = 1, binding = 2) uniform usampler2D guides;
layout(set = 1, binding = 3) uniform usampler2D previous_guide;
layout(set = 1, binding = 4) uniform usampler2D history;
layout(set = 1, binding = 5) uniform usampler2D gradients;

layout(location = 0) out uvec4 result;

// the gradient's gain: the share of the history cut for a change the relighting does not explain, 16 times over
// (A-SVGF's history weight goes up with the normalized gradient)
const float GAIN = 16.0;
// the bounce's own history's cap (xgpu_vulkan_lt_indirect.c's INDIRECT_BOUNCE_CAP): a count under it has started again
const float BOUNCE_CAP = 32.0;

// the light's change about at, where the history comes from (the previous frame's 3x3 strata's gradient samples about
// it, its own 8 times over: a moving shadow's edge cut where it is): relight, each channel's ratio of the re-shaded
// samples' sum to the previous ones' (a change alike over the strata: the lamp nearer, a light fading in, a coloured
// light gone, whose hue leaves with it; a channel with no light before takes the luminance's ratio), which scales the
// history; the result the share to cut from its count, by the gain: what of the change the ratio does not explain,
// each sample against the ratio's prediction (a shadow or the beam's edge moving; all of it where there was no light)
float changed(vec2 at, out vec3 relight)
{
    relight = vec3(1.0);
    if ((pass.control.y & GRADIENTS) == 0)
        return 0.0;
    ivec2 stratum = ivec2(floor(at)) / 3, top = ivec2(pass.size.zw) - 1;
    vec3 before_rgb[9], now_rgb[9];
    float weight[9];
    vec3 before_sum = vec3(0.0), now_sum = vec3(0.0);
    float weights = 0.0;
    for (int k = 0; k < 9; k++)
    {
        uvec4 g = texelFetch(gradients, clamp(stratum + ivec2(k % 3 - 1, k / 3 - 1), ivec2(0), top), 0);
        vec2 x = unpackHalf2x16(g.x), y = unpackHalf2x16(g.y), z = unpackHalf2x16(g.z);
        before_rgb[k] = vec3(x, y.x);
        now_rgb[k] = vec3(y.y, z);
        weight[k] = float(g.w) * (k == 4 ? 8.0 : 1.0);
        before_sum += weight[k] * before_rgb[k];
        now_sum += weight[k] * now_rgb[k];
        weights += weight[k];
    }
    float before = luminance(before_sum), now = luminance(now_sum), larger = max(before, now);
    if (weights <= 0.0 || larger <= 1.0e-8)
        return 0.0;
    if (before <= 1.0e-3 * larger)
        return 1.0;
    float ratio = now / before, off = 0.0;
    for (int c = 0; c < 3; c++)
        relight[c] = before_sum[c] > 1.0e-3 * before ? now_sum[c] / before_sum[c] : ratio;
    for (int k = 0; k < 9; k++)
        off += weight[k] * abs(luminance(now_rgb[k]) - luminance(relight * before_rgb[k]));
    return clamp(GAIN * off / larger, 0.0, 1.0);
}

// Catmull-Rom's four weights at t in [0, 1) between the middle two (they sum to 1)
vec4 catmull_rom(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return vec4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = uvec4(0u);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec4 raw_texel = texelFetch(raw, pixel, 0);
    vec3 sample_rgb = max(raw_texel.rgb, vec3(0.0));
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 p = world_point(pixel, uintBitsToFloat(words.z));
    // (checking: the guides' motion less the cameras' reprojection, pixels; z 1 where the guides carry motion, 2
    // where not)
    if ((pass.control.y & MOTION_VIEW) != 0)
    {
        uint motion = texelFetch(guides, pixel, 0).w;
        vec2 camera;
        bool seen = reproject_camera(p, camera);
        // (MOTION_RAW: the motion itself, and z the pixel's distance)
        vec3 off = motion == MOTION_NONE || !seen ? vec3(0.0, 0.0, 2.0) : (pass.control.y & MOTION_RAW) != 0 ?
            vec3(motion_fraction(motion) * vec2(pass.corners.zw - pass.corners.xy), -uintBitsToFloat(words.z)) :
            vec3(reproject_motion(pixel, motion) - camera, 1.0);
        result = uvec4(floatBitsToUint(off.x), floatBitsToUint(off.y), floatBitsToUint(off.z), packHalf2x16(vec2(1.0, 0.0)));
        return;
    }
    vec3 mean = vec3(0.0);
    float count = 0.0, moment = 0.0, cap = float(pass.control.x);
    vec2 at;
    // (the specular channel: nothing where the pixel has no specular; the history from where the reflection's virtual
    // point (behind the mirror by the hit's distance) was, by the cameras; the cap from 16 frames (smooth) to 32
    // (rough: its noise is wider), the guides' roughness, or 0.1 where they have none)
    bool specular = (pass.control.y & SPECULAR) != 0;
    vec3 reprojected = p;
    if (specular)
    {
        if (raw_texel.a < 0.0)
            return;
        uint byte = (texelFetch(guides, pixel, 0).z >> 16) & 255u;
        cap = mix(16.0, 32.0, clamp((byte == 255u ? 0.1 : float(byte) / 255.0) * 2.0, 0.0, 1.0));
        if (raw_texel.a > 0.0)
            reprojected = p + normalize(p - pass.camera[0].xyz) * raw_texel.a;
    }
    if ((pass.control.y & HISTORY) != 0 && pass.history.x != 0.0 &&
        reproject(reprojected, pixel, specular ? MOTION_NONE : texelFetch(guides, pixel, 0).w, at))
    {
        // (in motion, the history resampled each frame: its cap from 64 frames still down to 8 at a pixel's move a
        // frame, so that the resampling's blur stays a few frames' and the surfaces' detail stays sharp)
        cap = mix(cap, min(cap, 8.0), clamp(length(at - (vec2(pixel) + 0.5)), 0.0, 1.0));
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
                !(specular ? same_plane(texelFetch(previous_guide, q, 0).xy, q, p, n, words.w) :
                same_surface(texelFetch(previous_guide, q, 0).xy, p, n, words.w)))
                continue;
            uvec4 h = texelFetch(history, q, 0);
            vec2 cv = unpackHalf2x16(h.w);
            if (specular && cv.x <= 0.0)
                continue;
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
        // (in motion, the mean, where all of the 4x4 about the point is the same surface, by Catmull-Rom's weights
        // instead: a bilinear history, resampled every frame, would smooth away the bump map's detail in the light)
        if (weights > 0.999 && cap < float(pass.control.x))
        {
            vec4 wx = catmull_rom(t.x), wy = catmull_rom(t.y);
            vec3 sharp = vec3(0.0);
            bool whole = true;
            for (int j = 0; j < 4 && whole; j++)
                for (int i = 0; i < 4; i++)
                {
                    ivec2 q = base + ivec2(i - 1, j - 1);
                    if (any(lessThan(q, pass.previous_corners.xy)) || any(greaterThanEqual(q, pass.previous_corners.zw)) ||
                        !(specular ? same_plane(texelFetch(previous_guide, q, 0).xy, q, p, n, words.w) &&
                        unpackHalf2x16(texelFetch(history, q, 0).w).x > 0.0 :
                        same_surface(texelFetch(previous_guide, q, 0).xy, p, n, words.w)))
                    {
                        whole = false;
                        break;
                    }
                    uvec4 h = texelFetch(history, q, 0);
                    sharp += wx[i] * wy[j] * vec3(uintBitsToFloat(h.x), uintBitsToFloat(h.y), uintBitsToFloat(h.z));
                }
            if (whole)
                mean = max(sharp, vec3(0.0));
        }
    }
    // (anti-lag: the history relit by the change's ratio, its count cut by the share the ratio does not explain)
    vec3 relight = vec3(1.0);
    float cut = count > 0.0 ? changed(at, relight) : 0.0;
    float relit = luminance(mean) > 1.0e-12 ? luminance(mean * relight) / luminance(mean) : luminance(relight);
    mean *= relight;
    moment *= relit * relit;
    count *= 1.0 - cut;
    // (where the gradients say the light changed, which pixels it changed at: the sample beyond 3 deviations of the
    // relit history's samples (and 2% of its mean) cuts that pixel's count too, more the further it is. Never where
    // the light stayed: there a rare sample of a noisy pixel keeps its history, so a still view never shimmers)
    if (count > 0.0 && (any(greaterThan(abs(relight - 1.0), vec3(1.0e-3))) || cut > 0.0))
    {
        float lm = luminance(mean), ls = luminance(sample_rgb);
        float spread = sqrt(max(moment - lm * lm, 0.0)) + 0.02 * lm + 1.0e-7;
        float z = abs(ls - lm) / spread;
        if (z > 3.0)
            count *= (3.0 / z) * (3.0 / z);
    }
    // (the bounce folded in (lt/denoise_guide.frag's FOLD): no more history than the bounce's own has counted since
    // the lights about the pixel last changed, so a light that went takes its bounce out within a few frames)
    if (!specular && (int(pass.bounce.w) & 1) != 0 && raw_texel.a >= 0.0 && raw_texel.a < BOUNCE_CAP)
        count = min(count, raw_texel.a);
    count = min(count + 1.0, cap);
    if ((pass.control.y & EMA) != 0)
        count = cap;
    mean += (sample_rgb - mean) / count;
    float l = luminance(sample_rgb);
    moment += (l * l - moment) / count;
    float lm = luminance(mean);
    float relative = lm > 1.0e-12 ? max(moment / (lm * lm) - 1.0, 0.0) : 0.0;
    result = uvec4(floatBitsToUint(mean.r), floatBitsToUint(mean.g), floatBitsToUint(mean.b),
        packHalf2x16(vec2(count, min(relative, 60000.0))));
}
