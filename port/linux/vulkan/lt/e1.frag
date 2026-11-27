// LT E1/E2 (docs: ~/dev/rt2-realtime-design.md, section 7): the flashlight's direct light alone, as a physical spot
// with a disk emitter, one stratified shadow ray a pixel a frame through the ray-traced scene, accumulated by count
// (1/n) where the reprojected history is the same surface. Linear light (irradiance, the enhanced lights' units),
// which the surfaces add after the game's own lights (xgpu_spirv.c's lt_e1_add). Compiled offline by
// tools/vulkan_lt_shaders.py into xgpu_vulkan_lt_shaders.inc; drawn by xgpu_vulkan_lt.c as the light-shadow passes
// are (one triangle over the window, the backend's sets as the surfaces have them).
#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require

// (xgpu.h, xgpu_spirv.h)
#define INPUT_PRESENT 0x400u
#define HIT_MESHES 4096u
#define HIT_MATERIALS (4096u * 4u)
#define HIT_TEXTURES 512u
#define HIT_LEVEL 1.0
#define MASK_AO 0x03u
#define COOKIE_SIZE 80
#define GRID_Z 16
#define FALLOFF 32

// control.w's flags
#define VISIBILITY 1
#define RESET 2
#define COPY 4
// (with COPY: the pass's inputs written in place of the mean, for checking: the geometric normal, the shaded one,
// whether the pre-pass drew the pixel)
#define COPY_GEOMETRIC 8
#define COPY_SHADED 16
#define COPY_PRESENT 32
// (the twin's inputs: the shaded normal's bit, xgpu_spirv.c's lt_e1_twin_normal)
#define INPUT_SHADED 0x1000u

// the draw set's uniforms: struct xgpu_world_ao_pass, then (at 1024) xgpu_vulkan_lt.c's struct lt_params
layout(set = 0, binding = 1, std140) uniform Pass
{
    vec4 camera[4];
    ivec4 corners;
    vec4 cascades[4];
    vec4 previous[4];
    ivec4 previous_corners;
    vec4 history;
    // the gel's origin (the game's light, where its gel pass projects from) and 1 / radius
    layout(offset = 1024) vec4 light;
    // the light's colour (as the game adds it, gamma) and the gel's half width (tan)
    vec4 color;
    // the beam (the gel frame's forward) and the disk's radius; up and left (the gel's rows and columns)
    vec4 forward;
    vec4 up;
    vec4 left;
    // the disk's middle (the lamp on the weapon in hand); w the power's scale (xgpu_vulkan_lt.c's LT_POWER_SCALE)
    vec4 lamp;
    // x the count's cap (1: none kept), y a light this frame, z the cookie's first texel, w the flags
    ivec4 control;
    // x the light's owner (its wearer: the rays leave it out)
    uvec4 owners;
    // the falloff (the game's distance attenuation, gamma, 0 to the radius)
    vec4 falloff[FALLOFF / 4];
} pass;

layout(set = 0, binding = 3) uniform usampler3D cells;
layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform sampler2D history_image;
layout(set = 1, binding = 2) uniform usampler2D previous_inputs;
layout(set = 3, binding = 8) uniform accelerationStructureEXT scene;
layout(set = 4, binding = 0, std430) readonly buffer HitTable
{
    uint words[];
} table;
layout(set = 4, binding = 1) uniform sampler2D hit_textures[HIT_TEXTURES];

layout(location = 0) out vec4 result;

vec3 unfold(vec2 e)
{
    vec2 a = abs(e);
    float z = 1.0 - a.x - a.y;
    float t = max(-z, 0.0);
    vec2 f = e + vec2(e.x >= 0.0 ? -t : t, e.y >= 0.0 ? -t : t);
    return normalize(vec3(f, z));
}

uvec4 cell(int index)
{
    return texelFetch(cells, ivec3(index & 31, index >> 5, GRID_Z), 0);
}

float cookie_byte(int x, int y)
{
    int k = y * COOKIE_SIZE + x;
    uint word = cell(pass.control.z + (k >> 4))[(k >> 2) & 3];
    return float((word >> uint((k & 3) * 8)) & 255u) / 255.0;
}

// the gel's cookie, bilinear, at a, b in -1 to 1 (each axis warped: u = sign(a) sqrt(|a|), xgpu.h)
float cookie(float a, float b)
{
    vec2 xy = (vec2(sign(a) * sqrt(abs(a)), sign(b) * sqrt(abs(b))) * 0.5 + 0.5) * float(COOKIE_SIZE) - 0.5;
    vec2 f = floor(xy), t = xy - f;
    ivec2 i0 = clamp(ivec2(f), ivec2(0), ivec2(COOKIE_SIZE - 1)), i1 = clamp(ivec2(f) + 1, ivec2(0),
        ivec2(COOKIE_SIZE - 1));
    return mix(mix(cookie_byte(i0.x, i0.y), cookie_byte(i1.x, i0.y), t.x),
        mix(cookie_byte(i0.x, i1.y), cookie_byte(i1.x, i1.y), t.x), t.y);
}

float falloff_at(float x)
{
    float t = clamp(x, 0.0, 1.0) * float(FALLOFF - 1), f = floor(t);
    int low = clamp(int(f), 0, FALLOFF - 1), high = min(low + 1, FALLOFF - 1);
    return mix(pass.falloff[low >> 2][low & 3], pass.falloff[high >> 2][high & 3], t - f);
}

// an alpha-tested candidate taken where its alpha map passes (xgpu_spirv.c's rt_candidate_commits)
bool alpha_passes(rayQueryEXT q)
{
    if (rayQueryGetIntersectionGeometryIndexEXT(q, false) != 1)
        return true;
    uint mesh = uint(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(q, false)) & (HIT_MESHES - 1u);
    uint record = mesh << 2;
    uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, false)) + table.words[record + 3u];
    uint corners = table.words[record] + primitive * 3u, attributes = table.words[record + 1u];
    uint material_word = table.words[table.words[record + 2u] + (primitive >> 1)];
    uint material = (material_word >> ((primitive & 1u) << 4)) & 0xFFFFu;
    uint at = HIT_MATERIALS + (material << 4);
    vec2 uv[3];
    for (int corner = 0; corner < 3; corner++)
    {
        uint first = attributes + table.words[corners + uint(corner)] * 6u;
        uv[corner] = vec2(uintBitsToFloat(table.words[first]), uintBitsToFloat(table.words[first + 1u]));
    }
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, false);
    vec2 st = (uv[0] + (uv[1] - uv[0]) * b.x + (uv[2] - uv[0]) * b.y) *
        vec2(uintBitsToFloat(table.words[at + 5u]), uintBitsToFloat(table.words[at + 6u]));
    uint slot = min(table.words[at + 4u], HIT_TEXTURES - 1u);
    return textureLod(hit_textures[nonuniformEXT(slot)], st, HIT_LEVEL).a > uintBitsToFloat(table.words[at + 3u]);
}

// whether the segment from origin along d for t_max is clear (the light's wearer and the receiver's own object left
// out; the structure never)
float visible(vec3 origin, vec3 d, float t_max, int receiver)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, 0u, MASK_AO, origin, 0.0, d, t_max);
    while (rayQueryProceedEXT(q))
    {
        int candidate = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
        bool taken = candidate == 0 || (uint(candidate) != pass.owners.x && candidate != receiver);
        if (taken && alpha_passes(q))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

// a point of the unit disk for xi in [0, 1)^2 (Shirley and Chiu's concentric map: strata stay strata)
vec2 concentric(vec2 xi)
{
    vec2 o = xi * 2.0 - 1.0;
    if (o.x == 0.0 && o.y == 0.0)
        return vec2(0.0);
    float r, theta;
    if (abs(o.x) > abs(o.y))
    {
        r = o.x;
        theta = 0.78539816 * (o.y / o.x);
    }
    else
    {
        r = o.y;
        theta = 1.57079633 - 0.78539816 * (o.x / o.y);
    }
    return r * vec2(cos(theta), sin(theta));
}

// the flashlight's irradiance at p (normal n: the geometric one; shaded: the bump-mapped one): the gel and falloff as the game's decoding has them (gamma, so
// raised to 2.2 here), the cosine at the receiver and one shadow ray to a point of the disk, stratified over the
// frames by the R2 sequence from a per-pixel start
vec3 flashlight(vec3 p, vec3 n, vec3 shaded, ivec2 pixel, int receiver)
{
    vec3 f = pass.forward.xyz, u = pass.up.xyz, s = pass.left.xyz;
    vec3 v = p - pass.light.xyz;
    float z = dot(v, f), width = max(pass.color.w, 1.0e-4);
    if (z <= 1.0e-4)
        return vec3(0.0);
    float a = dot(v, s) / (z * width), b = dot(v, u) / (z * width);
    if (abs(a) >= 1.0 || abs(b) >= 1.0)
        return vec3(0.0);
    float gel = cookie(a, b);
    if (gel <= 0.0)
        return vec3(0.0);
    // (the disk's point for this pixel and frame)
    const vec2 r2 = vec2(0.75487766624669276, 0.56984029099805327);
    vec2 start = vec2(fract(0.5 + float(pixel.x) * r2.x + float(pixel.y) * r2.y),
        fract(52.9829189 * fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y))));
    vec2 xi = fract(start + pass.history.y * r2);
    vec2 disk = concentric(xi) * pass.forward.w;
    vec3 emitter = pass.lamp.xyz + normalize(s) * disk.x + normalize(u) * disk.y;
    vec3 to = emitter - p;
    float d = length(to);
    vec3 l = to / max(d, 1.0e-6);
    float facing = min(dot(n, l), dot(shaded, l)), emitting = dot(normalize(f), -l);
    // (the range window: (1 - (d / r)^4)^2, smooth, 0 only at the tag's radius)
    float x = d * abs(pass.light.w), window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    window *= window;
    if (facing <= 0.0 || emitting <= 0.0 || window <= 0.0)
        return vec3(0.0);
    float seen = 1.0;
    if ((pass.control.w & VISIBILITY) != 0)
    {
        // (off the surface by a little more far away, where the distance's float has less room)
        vec3 origin = p + n * (0.004 + 0.0005 * length(p - pass.camera[0].xyz));
        seen = visible(origin, normalize(emitter - origin), max(length(emitter - origin) - 0.01, 0.0), receiver);
    }
    // (a disk emitter's irradiance: its intensity (the gel's angular profile, linear, by the power's scale) by
    // the cosines at the receiver and the emitter over the distance squared, in the range window)
    // (the receiver's cosine by its shaded, bump-mapped normal; the geometric one decides the lit side and where the
    // shadow rays start)
    return pow(max(pass.color.rgb * gel, vec3(0.0)), vec3(2.2)) *
        (pass.lamp.w * dot(shaded, l) * emitting * window * seen / max(d * d, 1.0e-4));
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 shaded = (words.y & INPUT_SHADED) != 0u ? unfold(unpackSnorm4x8(words.y >> 16).xy) : n;
    if ((pass.control.w & COPY) != 0)
    {
        bool present = (words.y & INPUT_PRESENT) != 0u;
        result = (pass.control.w & COPY_GEOMETRIC) != 0 ? vec4(present ? n : vec3(0.0), 1.0) :
            (pass.control.w & COPY_SHADED) != 0 ? vec4(present ? shaded : vec3(0.0), 1.0) :
            (pass.control.w & COPY_PRESENT) != 0 ? vec4(present ? 1.0 : 0.0) : texelFetch(history_image, pixel, 0);
        return;
    }
    result = vec4(0.0);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    float distance = uintBitsToFloat(words.z);
    int receiver = int(words.w);
    vec2 low = vec2(pass.corners.xy), size = vec2(pass.corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / size - 1.0;
    vec3 ray = normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
    vec3 p = pass.camera[0].xyz + ray * distance;
    vec3 lit = pass.control.y != 0 ? flashlight(p, n, shaded, pixel, receiver) : vec3(0.0);
    // (the history at the same point where it is the same surface: the previous frame's guides within 3% in
    // distance, 25 degrees in normal, the same owner; else none, a disocclusion)
    vec3 mean = vec3(0.0);
    float count = 0.0;
    if (pass.control.x > 1 && (pass.control.w & RESET) == 0 && pass.history.x != 0.0)
    {
        vec3 w = p - pass.previous[0].xyz, pf = pass.previous[1].xyz, pr = pass.previous[2].xyz;
        vec3 pu = pass.previous[3].xyz;
        float along = dot(w, pf) / dot(pf, pf);
        vec2 was = vec2(dot(w, pr) / dot(pr, pr), dot(w, pu) / dot(pu, pu)) / along;
        vec2 plow = vec2(pass.previous_corners.xy), phigh = vec2(pass.previous_corners.zw);
        vec2 at = plow + (was + 1.0) * 0.5 * (phigh - plow);
        if (along > 0.0 && all(greaterThanEqual(at, plow)) && all(lessThan(at, phigh)))
        {
            ivec2 q = ivec2(floor(at));
            uvec4 before = texelFetch(previous_inputs, q, 0);
            float was_distance = uintBitsToFloat(before.z), now_distance = length(w);
            if ((before.y & INPUT_PRESENT) != 0u && abs(was_distance - now_distance) <= 0.03 * now_distance &&
                dot(unfold(unpackSnorm2x16(before.x)), n) >= 0.906 && before.w == words.w)
            {
                vec4 old = texelFetch(history_image, q, 0);
                mean = old.rgb;
                count = old.a;
            }
        }
    }
    count = min(count + 1.0, float(pass.control.x));
    mean += (lit - mean) / count;
    result = vec4(mean, count);
}
