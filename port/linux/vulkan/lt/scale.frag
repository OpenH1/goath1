// The light transport at a reduced lighting resolution (xgpu_vulkan_lt.c's lt_scaled): its cost follows the
// lighting's pixels, not the output's.
// DOWN (lt_scale_down.frag): the pre-pass's inputs and guides at the lighting's pixels, a texel each, from the output
// pixel under the texel's middle (the same one every frame, so the light's histories stay on one surface), its
// distance measured again along the lighting pixel's own ray to that pixel's plane (lt/e1.frag and the denoiser find
// a pixel's point from its centre's ray: the point then lies on the surface, not off it by a part of an output pixel).
// UP (lt_scale_up.frag): the light the surfaces read (lt.final: its left half the diffuse, its right the reflections,
// where wide) at each output pixel, a joint bilateral upsample: the lighting texels about the pixel weighted by the
// bilinear tent (then a wider Gaussian where the tent's texels are of other surfaces), kept only where the texel is
// of the pixel's owner, its point on the pixel's plane and its normals (geometric and shaded) alike; none found, the
// most alike texel of the pixel's owner about it; none, the nearest drawn.
#version 460

layout(set = 0, binding = 1, std140) uniform Pass
{
    vec4 camera[4];
    ivec4 corners;
    vec4 cascades[4];
    vec4 previous[4];
    ivec4 previous_corners;
    vec4 history;
    // (at 1024) the other resolution's window corners (DOWN: the output's; UP: the lighting's)
    layout(offset = 1024) ivec4 other_corners;
    // x the output's width (UP: the wide image's half), y the lighting's width, z 1 where the target is wide
    ivec4 sizes;
} pass;

#define INPUT_PRESENT 0x400u

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform usampler2D guides;
#ifdef UP
layout(set = 1, binding = 2) uniform usampler2D low_inputs;
layout(set = 1, binding = 3) uniform usampler2D low_guides;
layout(set = 1, binding = 4) uniform sampler2D low_light;
layout(location = 0) out vec4 result;
#else
layout(location = 0) out uvec4 result;
layout(location = 1) out uvec4 result_guides;
#endif

vec3 unfold(vec2 e)
{
    vec2 a = abs(e);
    float z = 1.0 - a.x - a.y;
    float t = max(-z, 0.0);
    vec2 f = e + vec2(e.x >= 0.0 ? -t : t, e.y >= 0.0 ? -t : t);
    return normalize(vec3(f, z));
}

// a continuous position's screen (-1 to 1) in a window's corners, and back
vec2 to_screen(vec2 at, ivec4 c)
{
    return (at - vec2(c.xy)) * 2.0 / vec2(c.zw - c.xy) - 1.0;
}

vec2 from_screen(vec2 screen, ivec4 c)
{
    return vec2(c.xy) + (screen + 1.0) * 0.5 * vec2(c.zw - c.xy);
}

vec3 ray(vec2 screen)
{
    return normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
}

#ifndef UP
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    vec2 screen = to_screen(vec2(pixel) + 0.5, pass.corners);
    ivec4 full = pass.other_corners;
    ivec2 at = clamp(ivec2(floor(from_screen(screen, full))), full.xy, full.zw - 1);
    uvec4 words = texelFetch(inputs, at, 0);
    result = words;
    result_guides = texelFetch(guides, at, 0);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    // (the output pixel's point, on its own ray; then where this pixel's ray meets its plane, where that is near)
    float distance = uintBitsToFloat(words.z);
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 p = ray(to_screen(vec2(at) + 0.5, full)) * distance, r = ray(screen);
    float facing = dot(r, n);
    if (abs(facing) > 1.0e-4)
    {
        float t = dot(p, n) / facing;
        if (t > 0.9 * distance && t < 1.1 * distance)
            result.z = floatBitsToUint(t);
    }
}
#else
// the pixel's, for its taps
ivec2 g_size;
ivec4 g_low;
int g_right;
uint g_owner;
vec3 g_n, g_s, g_p;
float g_distance;

// one lighting texel at o from base (f the pixel's place in the tent): its light and weights where it is of the
// pixel's surface; with all, the wider Gaussian's, the most alike's and the nearest's too
void tap(ivec2 base, ivec2 o, vec2 f, bool all, inout vec4 sum, inout float weights, inout vec4 wide,
    inout float wide_weights, inout float best, inout vec4 best_light, inout float nearest, inout vec4 nearest_light)
{
    ivec2 texel = base + o;
    if (any(lessThan(texel, ivec2(0))) || any(greaterThanEqual(texel, g_size)))
        return;
    uvec4 w = texelFetch(low_inputs, texel, 0);
    if ((w.y & INPUT_PRESENT) == 0u)
        return;
    vec2 d = vec2(o) - f;
    float d2 = dot(d, d);
    if (all && d2 < nearest)
    {
        nearest = d2;
        nearest_light = texelFetch(low_light, texel + ivec2(g_right * pass.sizes.y, 0), 0);
    }
    if (w.w != g_owner)
        return;
    vec3 ln = unfold(unpackSnorm2x16(w.x)), ls = unfold(unpackSnorm2x16(texelFetch(low_guides, texel, 0).x));
    if (dot(ls, ls) < 0.5 || dot(ls, ln) < 0.0)
        ls = ln;
    float cn = dot(g_n, ln), cs = dot(g_s, ls);
    vec3 lp = ray(to_screen(vec2(texel) + 0.5, g_low)) * uintBitsToFloat(w.z);
    float plane = abs(dot(lp - g_p, g_n)) / (0.01 * g_distance + 1.0e-4);
    // (the most alike of the owner's: normals first, then the plane)
    float alike = cn + cs - 0.1 * min(plane, 10.0);
    bool kept = cn >= 0.9 && plane <= 3.0;
    if (!kept && !(all && alike > best))
        return;
    vec4 light = texelFetch(low_light, texel + ivec2(g_right * pass.sizes.y, 0), 0);
    if (all && alike > best)
    {
        best = alike;
        best_light = light;
    }
    if (!kept)
        return;
    float g = exp(-plane * plane) * pow(max(cs, 0.0), 16.0);
    float tent = max(0.0, 1.0 - abs(d.x)) * max(0.0, 1.0 - abs(d.y));
    sum += tent * g * light;
    weights += tent * g;
    float gauss = exp(-d2 * 0.5) * g;
    wide += gauss * light;
    wide_weights += gauss;
}

void main()
{
    ivec2 target = ivec2(gl_FragCoord.xy);
    g_right = target.x >= pass.sizes.x ? 1 : 0;
    ivec2 pixel = target - ivec2(g_right * pass.sizes.x, 0);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = vec4(0.0);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    g_low = pass.other_corners;
    g_size = textureSize(low_inputs, 0);
    g_owner = words.w;
    vec2 screen = to_screen(vec2(pixel) + 0.5, pass.corners);
    g_distance = uintBitsToFloat(words.z);
    g_n = unfold(unpackSnorm2x16(words.x));
    g_s = unfold(unpackSnorm2x16(texelFetch(guides, pixel, 0).x));
    if (dot(g_s, g_s) < 0.5 || dot(g_s, g_n) < 0.0)
        g_s = g_n;
    g_p = ray(screen) * g_distance;
    // (the lighting's continuous position of this pixel)
    vec2 at = from_screen(screen, g_low) - 0.5;
    ivec2 base = ivec2(floor(at));
    vec2 f = at - vec2(base);
    vec4 sum = vec4(0.0), wide = vec4(0.0), best_light = vec4(0.0), nearest_light = vec4(0.0);
    float weights = 0.0, wide_weights = 0.0, best = -1.0e9, nearest = 1.0e9;
    // (the tent's four first: most pixels have their surface there)
    for (int k = 0; k < 4; k++)
        tap(base, ivec2(k & 1, k >> 1), f, false, sum, weights, wide, wide_weights, best, best_light, nearest,
            nearest_light);
    if (weights > 1.0e-3)
    {
        result = sum / weights;
        return;
    }
    sum = wide = vec4(0.0);
    weights = wide_weights = 0.0;
    for (int k = 0; k < 16; k++)
        tap(base, ivec2(k % 4 - 1, k / 4 - 1), f, true, sum, weights, wide, wide_weights, best, best_light, nearest,
            nearest_light);
    result = weights > 1.0e-3 ? sum / weights : wide_weights > 1.0e-4 ? wide / wide_weights :
        best > -1.0e8 ? best_light : nearest_light;
}
#endif
