// W3's A-SVGF gradients (Schied 2018; xgpu_vulkan_denoise.c): one texel a 3x3 stratum of the PREVIOUS frame. Its
// gradient pixel (lt/gradient_pixel.glsl, by that frame's number) was re-shaded (xgpu_vulkan_lt_reshade: its surface
// and seed, this frame's light and scene); where that surface is still seen, the same surface, in this frame (the
// point carried forward by the cameras), the light's change there is the re-shade less the previous frame's own 1-spp
// sample: the same sample, so nothing but the real change. Out (RGBA32Uint), in colour, as half floats: the previous
// sample's rgb and the re-shaded one's (x: r, g before; y: b before, r now; z: g, b now) and w 1, or 0 where there is
// none. The temporal stage reads them about the point its history comes from: in colour, so that a light of one hue
// that goes (a plasma bolt's) takes its hue out of the history, not only its brightness.
//
// The right half (strata from size.z on): the same strata's DETERMINISTIC gradients, the light's change without its
// sampling: at the same surface point, every light's unshadowed light (lt/e1.frag's formula at each emitter's middle,
// as the one light model's forward receivers have it: xgpu_spirv.c's lt_forward_light) by the frame before's light
// table and by this frame's (xgpu_vulkan_lt_table_halves: both in one image). A 1-spp sample sees a light among many
// only now and then, so the stochastic pair alone saw a light that went in a few strata of the one frame it went and
// never again: its light stayed in the history, fading by 1/64 a frame (a needle's pink wedges on the ground long
// after it burst, a grenade's glow on a rock). The deterministic pair sees every change of every light, every frame,
// and is exactly 0 where none changed (a held frame, the same table), so it never stirs a still picture. The same
// layout as the left half: x, y, z the before and now rgb as halves, w 1.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"
#include "gradient_pixel.glsl"

#define GRID_Z 16
#define COOKIE_SIZE 80
#define POINT 0
#define SPOT 1
#define PANEL 2
// (xgpu_spirv.h's XGPU_SPIRV_LT_FLASHLIGHT_ROW: the view's flashlight's row, past the 128 rows and 8 suns)
#define FLASHLIGHT_ROW 136

layout(set = 0, binding = 3) uniform usampler3D cells;
layout(set = 1, binding = 0) uniform usampler2D guide;
layout(set = 1, binding = 1) uniform usampler2D previous_guide;
layout(set = 1, binding = 2) uniform sampler2D reshaded;
layout(set = 1, binding = 3) uniform sampler2D previous_raw;
// (the light table: this frame's rows, then the frame before's from lights.z; lt/e1.frag's layout)
layout(set = 1, binding = 4) uniform sampler2D light_table;

layout(location = 0) out uvec4 result;

uvec4 cell(int index)
{
    return texelFetch(cells, ivec3(index & 31, index >> 5, GRID_Z), 0);
}

float cookie_byte(int x, int y)
{
    int k = y * COOKIE_SIZE + x;
    uint word = cell(pass.lights.w + (k >> 4))[(k >> 2) & 3];
    return float((word >> uint((k & 3) * 8)) & 255u) / 255.0;
}

// the gel's cookie, as lt/e1.frag's
float cookie(float a, float b)
{
    vec2 xy = (vec2(sign(a) * sqrt(abs(a)), sign(b) * sqrt(abs(b))) * 0.5 + 0.5) * float(COOKIE_SIZE) - 0.5;
    vec2 f = floor(xy), t = xy - f;
    ivec2 i0 = clamp(ivec2(f), ivec2(0), ivec2(COOKIE_SIZE - 1)), i1 = clamp(ivec2(f) + 1, ivec2(0),
        ivec2(COOKIE_SIZE - 1));
    return mix(mix(cookie_byte(i0.x, i0.y), cookie_byte(i1.x, i0.y), t.x),
        mix(cookie_byte(i0.x, i1.y), cookie_byte(i1.x, i1.y), t.x), t.y);
}

// a row's unshadowed light at p (n its geometric normal, shaded its shaded one): lt/e1.frag's light_sample at the
// emitter's middle, without its ray
vec3 row_light(int row, vec3 p, vec3 n, vec3 shaded)
{
    vec4 t4 = texelFetch(light_table, ivec2(4, row), 0);
    if (t4.w < 0.0 || t4.w > 2.5)
        return vec3(0.0);
    vec4 t0 = texelFetch(light_table, ivec2(0, row), 0), t1 = texelFetch(light_table, ivec2(1, row), 0);
    vec4 t2 = texelFetch(light_table, ivec2(2, row), 0), t3 = texelFetch(light_table, ivec2(3, row), 0);
    vec4 t5 = texelFetch(light_table, ivec2(5, row), 0), t6 = texelFetch(light_table, ivec2(6, row), 0);
    int kind = int(t4.w);
    vec3 to = t0.xyz - p;
    float d2 = max(dot(to, to), 1.0e-8), d = sqrt(d2);
    vec3 l = to / d;
    float x = d * t1.w, window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    window *= window;
    if (dot(n, l) <= 0.0 || window <= 0.0)
        return vec3(0.0);
    float r2 = t0.w * t0.w, e = dot(t1.xyz, -l), profile;
    if (kind == SPOT)
    {
        if (e <= 0.0)
            return vec3(0.0);
        float inner = t2.w, outer = t3.w;
        float ramp = outer <= -1.0 ? 1.0 : inner <= outer ? (e > outer ? 1.0 : 0.0) :
            clamp((e - outer) / (inner - outer), 0.0, 1.0);
        profile = ramp * e / max(d2, 1.0e-4);
        if (t5.y >= 0.0 && t5.z > 0.0)
        {
            vec3 v = p - (t6.w > 0.5 ? t6.xyz : t0.xyz);
            float z = dot(v, t1.xyz);
            if (z <= 1.0e-4)
                return vec3(0.0);
            float ga = dot(v, t3.xyz) / (z * t5.z), gb = dot(v, t2.xyz) / (z * t5.z);
            if (abs(ga) >= 1.0 || abs(gb) >= 1.0)
                return vec3(0.0);
            profile *= pow(max(cookie(ga, gb), 0.0), 2.2);
        }
    }
    else if (kind == PANEL)
        profile = e > 0.0 ? e * r2 / (r2 + d2) : 0.0;
    else
        profile = 1.0 / max(d2, r2);
    return t4.rgb * (profile * max(dot(shaded, l), 0.0) * window);
}

// every light of a table's half at p: its block's rows (rows of them from first), and the view's flashlight's row
vec3 table_light(int first, int rows, vec3 p, vec3 n, vec3 shaded)
{
    vec3 sum = vec3(0.0);
    for (int row = 0; row < rows; row++)
        sum += row_light(first + row, p, n, shaded);
    return sum + row_light(first + FLASHLIGHT_ROW, p, n, shaded);
}

void main()
{
    ivec2 stratum = ivec2(gl_FragCoord.xy);
    result = uvec4(0u);
    bool deterministic = stratum.x >= int(pass.size.z);
    if (deterministic)
    {
        stratum.x -= int(pass.size.z);
        if (pass.lights.z <= 0)
            return;
    }
    ivec2 q = lt_gradient_pixel_in(stratum, pass.tuning.x);
    if (any(lessThan(q, pass.previous_corners.xy)) || any(greaterThanEqual(q, pass.previous_corners.zw)))
        return;
    // (the re-shade: a texel a stratum)
    vec4 now = texelFetch(reshaded, stratum, 0);
    uvec2 before = texelFetch(previous_guide, q, 0).xy;
    if ((!deterministic && now.a <= 0.0) || (before.y & 0xFFFFu) == 0u)
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
    vec3 a, b;
    if (deterministic)
    {
        // (the guide's normals: x, y the shaded one, z, w the geometric one)
        vec4 normals = unpackSnorm4x8(before.x);
        vec3 n = unfold(normals.zw), shaded = unfold(normals.xy);
        if (dot(shaded, n) < 0.0)
            shaded = n;
        a = table_light(0, pass.lights.x, p, n, shaded);
        b = table_light(pass.lights.z, pass.lights.y, p, n, shaded);
    }
    else
    {
        a = now.rgb;
        b = texelFetch(previous_raw, q, 0).rgb;
    }
    a = clamp(a, vec3(0.0), vec3(60000.0));
    b = clamp(b, vec3(0.0), vec3(60000.0));
    result = uvec4(packHalf2x16(b.rg), packHalf2x16(vec2(b.b, a.r)), packHalf2x16(a.gb), 1u);
}
