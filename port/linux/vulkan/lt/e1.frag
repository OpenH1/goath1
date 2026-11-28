// LT E1/E2 and W2 (docs: ~/dev/rt2-realtime-design.md, sections 1.2 and 7): the direct light of every dynamic light
// (the C4 light list's, xgpu_vulkan_lt.c's light table) as area emitters: the flashlight as E1 had it (a physical
// spot with a disk emitter); the 2 strongest other lights of the pixel's cell (the dynamic lights' clusters' grid) one
// stratified shadow ray each (the hero lights); the rest by ReSTIR DI (8 RIS candidates, temporal reuse keyed by the
// light, M at most 20, one spatial pass of 4 neighbours, one visibility ray; RESERVOIR draws the reservoirs, the
// main draw reuses them spatially); accumulated by count (1/n) where the reprojected history is the same surface.
// The suns (W2): on the structure, the light is less the suns' direct share where a dynamic object blocks the sun and
// the structure does not (never more: the lightmap holds the sun). As the light's history and the denoiser keep a
// light of 0 or more, the light carries the suns' whole direct share less that (cmax - share, 0 or more) and FINAL,
// after them, takes the whole share off (cmax, found exactly: no rays), so one temporal stage averages the shadow. Linear light (irradiance, the enhanced lights' units),
// which the surfaces add after the game's own lights (xgpu_spirv.c's lt_e1_add). Compiled offline by
// tools/vulkan_lt_shaders.py into xgpu_vulkan_lt_shaders.inc; drawn by xgpu_vulkan_lt.c as the light-shadow passes
// are (one triangle over the window, the backend's sets as the surfaces have them).
#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require

#include "gradient_pixel.glsl"

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
// (with COPY: the pass's inputs written in place of the mean, for checking: the geometric normal, the shaded one (the
// guides'), whether the pre-pass drew the pixel)
#define COPY_GEOMETRIC 8
#define COPY_SHADED 16
#define COPY_PRESENT 32
// the denoiser's re-shade (xgpu_vulkan_lt_reshade: the previous frame's uniforms, inputs and guides, this frame's
// light): drawn a texel a 3x3 stratum, each its stratum's gradient pixel shaded
#define GRADIENT 128
// W2: the suns' share subtracted from history_image's light (the draw after the denoiser); the reservoirs drawn (RIS
// and the temporal reuse) in place of the light; COPY with the reach (d / range) of the light table's row
// lights.w, for the calibration's band
#define RESERVOIR 512
#define COPY_REACH 1024
// the light the surfaces read: history_image's light less the suns' whole direct share (cmax): less than 0 where a
// dynamic object shadows the sun
#define FINAL 2048
// the suns' rays a sun where the pixel has no history (a reset, a cut, a disocclusion), so that the first frame's
// shadow is as good as the later ones' average
#define SUN_FIRST 12
// how far the suns' rays go (world units): past any BSP (a Halo BSP is well under 1 km across), so a dynamic object
// at any height between a surface and the sun shadows it (a Pelican high overhead)
#define SUN_REACH 4096.0
// lights.z's
#define LIGHTS_FLASHLIGHT 1
#define LIGHTS_HEROES 2
#define LIGHTS_RESTIR 4
#define LIGHTS_REUSE 8
// a level lit live (lt_lights.c's lt_lights_real_sky, ~/dev/rt2-realtime-design.md section 9): the suns' rows are the
// sky's whole light, not the lightmap's share; a row of angular diameter past a half turn is the sky's dome (the
// bounce's: a ray that leaves the scene returns it, lt/indirect.glsl)
#define LIGHTS_REAL 16
#define DOME_DIAMETER 3.2
#define RT_MASK_SUN 0x0Bu
// the light table (xgpu_vulkan_lt.c's lt_table): a row a light, TABLE_TEXELS texels; the block's lights' rows
// first (by their index in the block), the suns' from SUN_ROW
#define SUN_ROW 128
#define POINT 0
#define SPOT 1
#define PANEL 2
#define DIRECTIONAL 3
#define RT_MASK_STRUCTURE 0x01u
#define RT_MASK_DYNAMIC 0x0Au
#define M_CAP 20.0

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
    // W2: the dynamic lights' grid (the block's grid[0], [1]: its middle, each axis's cells a doubling, 1 / cell)
    vec4 grid[2];
    // x the block's lights in the table, y the suns, z LIGHTS_*, w COPY_REACH's row
    ivec4 lights;
#ifdef LT_INDIRECT
    // (the indirect light's and reflections' kernel, lt/indirect.glsl: xgpu_vulkan_lt_indirect.c's struct
    // lt_indirect_params)
    layout(offset = 1536) vec4 indirect[4];
#endif
} pass;

// how far a ray starts off its receiver: the receiver's point is found on
// the pixel's middle ray, at the drawn point's distance, and the drawn point
// lies anywhere in the pixel (the camera's jitter, the pass's own scale), so
// up to a pixel's width off its surface: 1.5 pixels' width at its distance,
// over a floor (a fixed fraction of the distance fell short on b30's beach,
// whole triangles shadowed by themselves)
float ray_lift(vec3 p)
{
    float pixel_width = 2.0 * length(pass.camera[2].xyz) /
        (length(pass.camera[1].xyz) * max(float(pass.corners.z - pass.corners.x), 1.0));
    return 0.004 + 1.5 * pixel_width * length(p - pass.camera[0].xyz);
}

layout(set = 0, binding = 3) uniform usampler3D cells;
layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform sampler2D history_image;
layout(set = 1, binding = 2) uniform usampler2D previous_inputs;
// (the pre-pass's twins' guides: xgpu_spirv.h's C1, x the shaded normal)
layout(set = 1, binding = 3) uniform usampler2D guides;
// (W2: the light table; the reservoirs: RESERVOIR's previous frame's, the main draw's this frame's)
layout(set = 1, binding = 4) uniform sampler2D light_table;
layout(set = 1, binding = 5) uniform sampler2D reservoirs;
layout(set = 3, binding = 8) uniform accelerationStructureEXT scene;
layout(set = 4, binding = 0, std430) readonly buffer HitTable
{
    uint words[];
} table;
layout(set = 4, binding = 1) uniform sampler2D hit_textures[HIT_TEXTURES];

layout(location = 0) out vec4 result;
#ifdef LT_INDIRECT
// (BOUNCE with BOUNCE_HISTORY: the bounce's half-resolution history, its second output; lt/indirect.glsl)
layout(location = 1) out vec4 bounce_history;
#endif

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
float visible_by(vec3 origin, vec3 d, float t_max, int receiver, uint owner, uint mask)
{
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, 0u, mask, origin, 0.0, d, t_max);
    while (rayQueryProceedEXT(q))
    {
        int candidate = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
        bool taken = candidate == 0 || (uint(candidate) != owner && candidate != receiver);
        if (taken && alpha_passes(q))
            rayQueryConfirmIntersectionEXT(q);
    }
    return rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}

float visible(vec3 origin, vec3 d, float t_max, int receiver)
{
    return visible_by(origin, d, t_max, receiver, pass.owners.x, MASK_AO);
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

// the flashlight's irradiance at p (normal n: the geometric one; shaded: the guides' bump-mapped one): the gel and falloff as the game's decoding has them (gamma, so
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
    float facing = dot(n, l), emitting = dot(normalize(f), -l);
    // (the range window: (1 - (d / r)^4)^2, smooth, 0 only at the tag's radius)
    float x = d * abs(pass.light.w), window = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    window *= window;
    if (facing <= 0.0 || emitting <= 0.0 || window <= 0.0)
        return vec3(0.0);
    float seen = 1.0;
    if ((pass.control.w & VISIBILITY) != 0)
    {
        // (off the surface by a little more far away, where the distance's float has less room)
        vec3 origin = p + n * ray_lift(p);
        seen = visible(origin, normalize(emitter - origin), max(length(emitter - origin) - 0.01, 0.0), receiver);
    }
    // (a disk emitter's irradiance: its intensity (the gel's angular profile, linear, by the power's scale) by
    // the cosines at the receiver and the emitter over the distance squared, in the range window)
    // (the receiver's cosine by its shaded, bump-mapped normal, the guides'; the geometric one decides which side is
    // lit and where the shadow rays start)
    return pow(max(pass.color.rgb * gel, vec3(0.0)), vec3(2.2)) *
        (pass.lamp.w * max(dot(shaded, l), 0.0) * emitting * window * seen / max(d * d, 1.0e-4));
}

// ---------- W2: every light

// the light table's texel of a row: 0 the emitter's middle and radius, 1 forward and 1 / range (0 none), 2 up and
// the cone's inner cosine, 3 left and its outer one (-1 none), 4 the linear power and the kind (less than 0: not this
// pass's), 5 the owner, the gel's slot (-1 none), its half width, a sun's angular diameter (another light's: the instance it leaves unlit, 0 none: its owner where its tag says "don't light own object" or it is a projectile's, xgpu_vulkan_lt.c), 6 x the row this light
// had the frame before (-1 none) and y the row now of the light that had this row the frame before (-1 none)
vec4 row_texel(int row, int texel)
{
    return texelFetch(light_table, ivec2(texel, row), 0);
}

float luminance_of(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// a hash's floats in [0, 1) (PCG's)
uint hash_next(inout uint h)
{
    h = h * 747796405u + 2891336453u;
    uint w = ((h >> ((h >> 28u) + 4u)) ^ h) * 277803737u;
    return (w >> 22u) ^ w;
}

float random(inout uint h)
{
    return float(hash_next(h) >> 8) * (1.0 / 16777216.0);
}

// the pixel's point and frame's R2 point, shifted by salt (each light its own strata)
vec2 stratified(ivec2 pixel, float salt)
{
    const vec2 r2 = vec2(0.75487766624669276, 0.56984029099805327);
    vec2 start = vec2(fract(0.5 + float(pixel.x) * r2.x + float(pixel.y) * r2.y),
        fract(52.9829189 * fract(0.06711056 * float(pixel.x) + 0.00583715 * float(pixel.y))));
    return fract(start + (pass.history.y + salt * 7.0) * r2 + vec2(0.3819660113 * salt, 0.6180339887 * salt));
}

// the cell's lights (the grid's mask about p: xgpu.h's dynamic lights' cells; the shadow rays stand in for the
// clusters' walk)
uvec4 cell_lights(vec3 p)
{
    vec3 offset = p - pass.grid[0].xyz;
    vec3 c = sign(offset) * log2(1.0 + abs(offset) * pass.grid[1].w) * pass.grid[1].xyz + vec3(16.0, 16.0, 8.0);
    ivec3 i = clamp(ivec3(floor(c)), ivec3(0), ivec3(31, 31, GRID_Z - 1));
    return texelFetch(cells, i, 0);
}

// the range window (1 - (d / r)^4)^2
float range_window(float d, float inverse_range)
{
    float x = d * inverse_range, w = clamp(1.0 - x * x * x * x, 0.0, 1.0);
    return w * w;
}

// a cone's ramp at the emitter's cosine e (inner, outer: -1 none)
float cone_ramp(float e, float inner, float outer)
{
    if (outer <= -1.0)
        return 1.0;
    if (inner <= outer)
        return e > outer ? 1.0 : 0.0;
    return clamp((e - outer) / (inner - outer), 0.0, 1.0);
}

// a row's light at p, unshadowed, from its emitter's middle (the RIS's target and the heroes' ranking): luminance
float light_estimate(int row, vec3 p, vec3 shaded)
{
    vec4 t0 = row_texel(row, 0), t1 = row_texel(row, 1), t4 = row_texel(row, 4);
    int kind = int(t4.w);
    if (t4.w < 0.0 || kind == DIRECTIONAL)
        return 0.0;
    vec3 to = t0.xyz - p;
    float d2 = max(dot(to, to), 1.0e-8), d = sqrt(d2);
    vec3 l = to / d;
    float facing = max(dot(shaded, l), 0.0), window = range_window(d, t1.w);
    if (facing <= 0.0 || window <= 0.0)
        return 0.0;
    float r2 = t0.w * t0.w, e = dot(t1.xyz, -l), profile;
    if (kind == SPOT)
    {
        vec4 t2 = row_texel(row, 2), t3 = row_texel(row, 3);
        profile = e > 0.0 ? cone_ramp(e, t2.w, t3.w) * e / max(d2, r2) : 0.0;
    }
    else if (kind == PANEL)
        profile = e > 0.0 ? e * r2 / (r2 + d2) : 0.0;
    else
        profile = 1.0 / max(d2, r2);
    return luminance_of(t4.rgb) * profile * facing * window;
}

// how far about a light's middle its emitter's body reaches (its own glow, a bolt's body, the weapon it is on): the
// shadow rays end short of it, and a bounce ray that meets a surface within it has met the light itself
// (lt/indirect.glsl's direct_one)
float emitter_body(float radius)
{
    return max(1.5 * radius, 0.1);
}

// a row's light at p with one point of its emitter (xi) and, with shadow, one ray to it: point a sphere (its disk
// facing p), spot a disk facing forward in its cone (its gel the angular profile), panel a one-sided Lambertian disk
vec3 light_sample(int row, vec3 p, vec3 n, vec3 shaded, vec2 xi, int receiver, bool shadow)
{
    vec4 t0 = row_texel(row, 0), t1 = row_texel(row, 1), t4 = row_texel(row, 4), t5 = row_texel(row, 5);
    int kind = int(t4.w);
    if (t4.w < 0.0 || kind == DIRECTIONAL)
        return vec3(0.0);
    // (the instance it leaves unlit: its tag's "don't light own object", a projectile's own body)
    if (receiver != 0 && int(t5.w) == receiver)
        return vec3(0.0);
    vec3 centre = t0.xyz, f = t1.xyz;
    float radius = t0.w;
    vec2 disk = concentric(xi) * radius;
    vec3 emitter;
    vec4 t2 = row_texel(row, 2), t3 = row_texel(row, 3);
    if (kind == POINT)
    {
        vec3 w = normalize(p - centre);
        vec3 a = normalize(abs(w.z) < 0.9 ? cross(w, vec3(0.0, 0.0, 1.0)) : cross(w, vec3(1.0, 0.0, 0.0)));
        emitter = centre + a * disk.x + cross(w, a) * disk.y;
    }
    else
        emitter = centre + t3.xyz * disk.x + t2.xyz * disk.y;
    vec3 to = emitter - p;
    float d2 = max(dot(to, to), 1.0e-8), d = sqrt(d2);
    vec3 l = to / d;
    float facing = dot(n, l), window = range_window(d, t1.w);
    if (facing <= 0.0 || window <= 0.0)
        return vec3(0.0);
    float r2 = radius * radius, e = dot(f, -l);
    vec3 power = t4.rgb;
    float profile;
    if (kind == SPOT)
    {
        if (e <= 0.0)
            return vec3(0.0);
        profile = cone_ramp(e, t2.w, t3.w) * e / max(d2, 1.0e-4);
        // (a gel: the cells' cookie, linear, in the light's frame at its half width)
        if (t5.y >= 0.0 && t5.z > 0.0)
        {
            vec3 v = p - centre;
            float z = dot(v, f);
            if (z <= 1.0e-4)
                return vec3(0.0);
            float ga = dot(v, t3.xyz) / (z * t5.z), gb = dot(v, t2.xyz) / (z * t5.z);
            if (abs(ga) >= 1.0 || abs(gb) >= 1.0)
                return vec3(0.0);
            profile *= pow(max(cookie(ga, gb), 0.0), 2.2);
        }
    }
    else if (kind == PANEL)
    {
        if (e <= 0.0)
            return vec3(0.0);
        profile = e * r2 / (r2 + d2);
    }
    else
        profile = 1.0 / max(d2, r2);
    float lit = profile * max(dot(shaded, l), 0.0) * window;
    if (lit <= 0.0)
        return vec3(0.0);
    if (shadow)
    {
        vec3 origin = p + n * ray_lift(p);
        vec3 ray = emitter - origin;
        float length_to = length(ray);
        // (ending short of the emitter: its own glow, a bolt's body, the weapon it is on: emitter_body's reach)
        float t_max = length_to - emitter_body(radius);
        if (t_max > 0.0)
            lit *= visible_by(origin, ray / length_to, t_max, receiver, uint(t5.x), MASK_AO);
    }
    return power * lit;
}

||||||| parent of d11dca180 (Ray-traced lighting: a ray starts 1.5 pixels' width off its receiver (the receiver's point is found on the pixel's middle ray, the drawn point anywhere in the pixel; the fixed 0.0005 of the distance, half a pixel at 1280, left whole beach triangles shadowing themselves))
// ---------- the level's light sources (~/dev/rt2-realtime-design.md section 11): each a group of triangles of one
// radiance (Lambertian, one-sided, toward the triangle's winding), sampled as area lights: a group drawn by its
// light about p (its cell's, the grid's: power over the distance squared at the cell, the cell's strongest) mixed
// with its share of the level's power (EMITTER_LEVEL_SHARE: no group is never drawn), a triangle of it by its area
// and a point of that uniformly, one shadow ray. The pass's irradiance is the true one over pi: L cos cos' A / d^2 / pi

vec4 emitter_texel(int k)
{
    return texelFetch(light_table, ivec2(k & 7, EMITTER_ROW + (k >> 3)), 0);
}

// p's cell's first texel (-1 off the grid)
int emitter_cell(vec3 p)
{
    ivec3 c = ivec3(floor((p - pass.emitter_grid.xyz) * pass.emitter_grid.w));
    if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, pass.emitter_counts.xyz)))
        return -1;
    return pass.emitter_bases.y + (EMITTER_CELL_GROUPS / 2) * (c.x + pass.emitter_counts.x * (c.y + pass.emitter_counts.y * c.z));
}

// a group by the level's power (its cumulative share, texel 0's w): a binary search
int emitter_level_pick(float u)
{
    int low = 0, high = pass.emitter_counts.w - 1;
    while (low < high)
    {
        int middle = (low + high) >> 1;
        if (emitter_texel(2 * middle).w > u)
            high = middle;
        else
            low = middle + 1;
    }
    return low;
}

// the cell's entries (EMITTER_CELL_GROUPS groups, each with its chance up to and with it), read once a receiver
vec4 emitter_cell_texels[EMITTER_CELL_GROUPS / 2];

void emitter_cell_read(int cell)
{
    for (int k = 0; k < EMITTER_CELL_GROUPS / 2; k++)
        emitter_cell_texels[k] = cell >= 0 ? emitter_texel(cell + k) : vec4(-1.0, 2.0, -1.0, 2.0);
}

// the cell's chance of a group (its entries'), and with u its pick (-1 none)
float emitter_cell_chance(int group, float u, out int picked)
{
    float before = 0.0, chance = 0.0;
    picked = -1;
    for (int k = 0; k < EMITTER_CELL_GROUPS / 2; k++)
    {
        vec4 t = emitter_cell_texels[k];
        for (int half_ = 0; half_ < 2; half_++)
        {
            float g = half_ == 0 ? t.x : t.z, up_to = half_ == 0 ? t.y : t.w;
            if (g < 0.0)
                return chance;
            if (int(g) == group)
                chance = up_to - before;
            if (picked < 0 && u < up_to)
                picked = int(g);
            before = up_to;
        }
    }
    return chance;
}

// a point of a light source's triangle (tri, its edges' coordinates ab) and its light at p unshadowed in area measure
// (radiance x cos x cos' / d^2 / pi, the pass's units): 0 where it faces away or is behind p
vec3 emitter_light(vec3 p, vec3 n, vec3 shaded, int tri, vec2 ab, out vec3 q)
{
    vec4 t0 = emitter_texel(pass.emitter_bases.x + 3 * tri), t1 = emitter_texel(pass.emitter_bases.x + 3 * tri + 1),
        t2 = emitter_texel(pass.emitter_bases.x + 3 * tri + 2);
    q = t0.xyz + t1.xyz * ab.x + t2.xyz * ab.y;
    vec3 to = q - p;
    float d2 = max(dot(to, to), 1.0e-6);
    vec3 l = to * inversesqrt(d2);
    float emitting = dot(normalize(cross(t1.xyz, t2.xyz)), -l), lit = max(dot(shaded, l), 0.0);
    if (emitting <= 0.0 || dot(n, l) <= 0.0 || lit <= 0.0)
        return vec3(0.0);
    // (the distance squared no less than a hundredth of a world unit's: a receiver touching a lamp)
    return emitter_texel(2 * int(t1.w) + 1).rgb * (lit * emitting / (max(d2, 0.01) * 3.14159265));
}

// whether q (a light source's point) is seen from p (normal n), the receiver's own object left out
float emitter_visible(vec3 p, vec3 n, vec3 q, int receiver)
{
    vec3 origin = p + n * ray_lift(p);
    vec3 ray = q - origin;
    float length_to = length(ray);
    // (ending short of the lamp's own surface, 0.01 under the light)
    float t_max = length_to - 0.02;
    return t_max > 0.0 ? visible_by(origin, ray / length_to, t_max, receiver, 0xffffffffu, MASK_AO) : 0.0;
}

// one candidate point of the level's light sources for p: u the group's choice, xi the point's. Out: its triangle
// and coordinates, its light at p unshadowed (emitter_light) and the chance density of drawing it (per world unit
// squared); false none
bool emitter_candidate(vec3 p, vec3 n, vec3 shaded, int cell, float u, vec2 xi, out int tri, out vec2 ab, out vec3 f,
    out float density)
{
    int group, picked;
    tri = -1;
    ab = vec2(0.0);
    f = vec3(0.0);
    density = 0.0;
    float level_share = cell >= 0 ? EMITTER_LEVEL_SHARE : 1.0;
    if (u < level_share)
        group = emitter_level_pick(u / level_share);
    else
    {
        emitter_cell_chance(-1, (u - level_share) / (1.0 - level_share), picked);
        group = picked;
    }
    if (group < 0)
        return false;
    vec4 g0 = emitter_texel(2 * group), g1 = emitter_texel(2 * group + 1);
    float chance = level_share * g1.w;
    if (cell >= 0)
        chance += (1.0 - level_share) * emitter_cell_chance(group, 2.0, picked);
    if (chance <= 0.0 || g0.z <= 0.0)
        return false;
    // (a triangle by its area: the group's areas up to and with each, a binary search)
    float a = xi.x * g0.z;
    int low = int(g0.x), high = int(g0.x) + int(g0.y) - 1;
    while (low < high)
    {
        int middle = (low + high) >> 1;
        if (emitter_texel(pass.emitter_bases.x + 3 * middle).w > a)
            high = middle;
        else
            low = middle + 1;
    }
    float before = low > int(g0.x) ? emitter_texel(pass.emitter_bases.x + 3 * (low - 1)).w : 0.0;
    float top = emitter_texel(pass.emitter_bases.x + 3 * low).w;
    float x = clamp((a - before) / max(top - before, 1.0e-12), 0.0, 1.0), s = sqrt(x);
    tri = low;
    ab = vec2(s * (1.0 - xi.y), s * xi.y);
    vec3 q;
    f = emitter_light(p, n, shaded, tri, ab, q);
    density = chance / g0.z;
    return true;
}

// RIS over candidates of the level's light sources at p (each drawn by its group's light about p and its area,
// weighted by its own unshadowed light there): the one chosen (tri -1 none), its light, and its weight W (the sum of
// the weights over the candidates and its target)
struct EmitterPick
{
    int tri;
    vec2 ab;
    vec3 f;
    float w;
};

EmitterPick emitter_ris(vec3 p, vec3 n, vec3 shaded, int candidates, inout uint h)
{
    EmitterPick pick = EmitterPick(-1, vec2(0.0), vec3(0.0), 0.0);
    if (pass.emitter_counts.w <= 0)
        return pick;
    int cell = emitter_cell(p + n * 0.02);
    // (a cell no group lights: the level's power alone)
    if (cell >= 0 && emitter_texel(cell).x < 0.0)
        cell = -1;
    emitter_cell_read(cell);
    float w_sum = 0.0, chosen_target = 0.0;
    for (int k = 0; k < candidates; k++)
    {
        int tri;
        vec2 ab;
        vec3 f;
        float density;
        float u = random(h);
        vec2 xi = vec2(random(h), random(h));
        if (!emitter_candidate(p, n, shaded, cell, u, xi, tri, ab, f, density) || density <= 0.0)
            continue;
        float target = luminance_of(f);
        if (target <= 0.0)
            continue;
        float w = target / density;
        w_sum += w;
        if (random(h) * w_sum < w)
        {
            pick.tri = tri;
            pick.ab = ab;
            pick.f = f;
            chosen_target = target;
        }
    }
    pick.w = chosen_target > 0.0 ? w_sum / (float(candidates) * chosen_target) : 0.0;
    return pick;
}

// the level's light sources' light at p: RIS, one shadow ray to the one chosen
vec3 emitter_sample(vec3 p, vec3 n, vec3 shaded, int candidates, inout uint h, int receiver, bool shadow)
{
    EmitterPick pick = emitter_ris(p, n, shaded, candidates, h);
    if (pick.tri < 0 || pick.w <= 0.0)
        return vec3(0.0);
    vec3 q;
    emitter_light(p, n, shaded, pick.tri, pick.ab, q);
    return pick.f * pick.w * (shadow ? emitter_visible(p, n, q, receiver) : 1.0);
}

// ---------- ReSTIR over the level's light sources (~/dev/rt2-realtime-design.md section 11): a pixel's reservoir in
// the reservoirs' right half (from x = the window's width): x its triangle x 64 + its count M (-1 none), y and z the
// point's edge coordinates, w its weight W. RESERVOIR's right half draws this frame's: RIS's pick, its visibility
// traced (an unseen pick keeps its count, its weight 0), merged with the frame before's at the same surface (its count
// capped before the merge so that the sum is at most EMITTER_M_CAP; its sample's light and visibility at p found
// again: the validation); the main draw reuses it spatially and traces the one chosen. The targets: the light at p
// with its visibility (the temporal draw's), unshadowed (the spatial merge's, each neighbour's sample seen from
// there; the final ray decides)
struct EmitterReservoir
{
    int tri;
    vec2 ab;
    float m, w;
};

EmitterReservoir emitter_reservoir_read(vec4 t)
{
    if (t.x < 0.0)
        return EmitterReservoir(-1, vec2(0.0), 0.0, 0.0);
    int packed = int(t.x + 0.5);
    return EmitterReservoir(packed >> 6, t.yz, float(packed & 63), max(t.w, 0.0));
}

vec4 emitter_reservoir_write(EmitterReservoir r)
{
    return r.tri >= 0 ? vec4(float(r.tri * 64 + int(r.m)), r.ab, r.w) : vec4(-1.0, 0.0, 0.0, 0.0);
}

// merged into r (w_sum its running sum, target its sample's): another reservoir's sample by its target at p
void emitter_reservoir_merge(inout EmitterReservoir r, inout float w_sum, inout float target, EmitterReservoir other,
    float other_target, float u)
{
    float w = other_target * other.w * other.m;
    r.m += other.m;
    if (other.tri < 0 || w <= 0.0)
        return;
    w_sum += w;
    if (u * w_sum < w)
    {
        r.tri = other.tri;
        r.ab = other.ab;
        target = other_target;
    }
}

// a reservoir: its light's row (-1 none), the sum of the weights, its count, the chosen one's target
struct Reservoir
{
    int row;
    float w_sum, m, target;
};

bool reservoir_update(inout Reservoir r, int row, float w, float target, float u)
{
    r.w_sum += w;
    if (w > 0.0 && u * r.w_sum < w)
    {
        r.row = row;
        r.target = target;
        return true;
    }
    return false;
}

// the chosen one's weight (W: 1 / its pdf, as RIS has it)
float reservoir_weight(Reservoir r)
{
    return r.row >= 0 && r.target > 0.0 && r.m > 0.0 ? r.w_sum / (r.m * r.target) : 0.0;
}

// the pixel's heroes: the two strongest of its cell's lights (by light_estimate), and how many others there are
void heroes(vec3 p, vec3 shaded, uvec4 mask, out int h0, out int h1, out int others)
{
    float best0 = 0.0, best1 = 0.0;
    int count = 0;
    h0 = -1;
    h1 = -1;
    for (int word = 0; word < 4; word++)
    {
        uint bits = mask[word];
        while (bits != 0u)
        {
            int row = word * 32 + findLSB(bits);
            bits &= bits - 1u;
            if (row >= pass.lights.x)
                break;
            float s = light_estimate(row, p, shaded);
            if (s <= 0.0)
                continue;
            count++;
            if (s > best0)
            {
                best1 = best0;
                h1 = h0;
                best0 = s;
                h0 = row;
            }
            else if (s > best1)
            {
                best1 = s;
                h1 = row;
            }
        }
    }
    others = count - (h0 >= 0 ? 1 : 0) - (h1 >= 0 ? 1 : 0);
    if ((pass.lights.z & LIGHTS_HEROES) == 0)
    {
        others = count;
        h0 = h1 = -1;
    }
}

// the k-th of the cell's lights that is lit there and no hero (-1 none)
int other_light(vec3 p, vec3 shaded, uvec4 mask, int h0, int h1, int k)
{
    for (int word = 0; word < 4; word++)
    {
        uint bits = mask[word];
        while (bits != 0u)
        {
            int row = word * 32 + findLSB(bits);
            bits &= bits - 1u;
            if (row >= pass.lights.x)
                return -1;
            if (row == h0 || row == h1 || light_estimate(row, p, shaded) <= 0.0)
                continue;
            if (k-- == 0)
                return row;
        }
    }
    return -1;
}

// RIS over the non-hero lights: 8 candidates drawn evenly from them (all of them where there are no more than 8)
Reservoir ris(vec3 p, vec3 shaded, uvec4 mask, int h0, int h1, int others, inout uint h)
{
    Reservoir r = Reservoir(-1, 0.0, 0.0, 0.0);
    if (others <= 0)
        return r;
    int candidates = min(others, 8);
    for (int k = 0; k < candidates; k++)
    {
        int index = others <= 8 ? k : min(int(random(h) * float(others)), others - 1);
        int row = other_light(p, shaded, mask, h0, h1, index);
        if (row < 0)
            continue;
        float target = light_estimate(row, p, shaded);
        // (each candidate's source pdf 1 / others: its weight target x others; W = w_sum / (M target))
        reservoir_update(r, row, target * float(others), target, random(h));
    }
    r.m = float(candidates);
    return r;
}

// the reservoir's light at p from a neighbour's (or the last frame's) reservoir, its count at most most: merged into r.
// The count is capped here, before its weight joins the sum, never after: W = w_sum / (M target) holds only when M is
// the count the sum was built with (a cap after the merge left W too large by the count it cut, frame after frame:
// a held light grew without end)
void reservoir_merge(inout Reservoir r, vec4 other, int row, vec3 p, vec3 shaded, int h0, int h1, float most,
    inout uint h)
{
    float m = min(other.z, most);
    if (row < 0 || row >= pass.lights.x || row == h0 || row == h1 || m <= 0.0)
        return;
    float target = light_estimate(row, p, shaded);
    reservoir_update(r, row, target * other.y * m, target, random(h));
    r.m += m;
}

// the suns' whole direct share at p (cmax: sun_power_scale x power x cos, the lightmap's, by the geometric normal as
// the lightmap has no bump), and the part of it a dynamic object blocks where the structure does not (the
// lightmap's own shadows already dark): rays a sun toward points of its disk, as far as the sun (SUN_REACH)
vec3 sun_cmax(vec3 n)
{
    vec3 cmax = vec3(0.0);
    // (lit live: no lightmap, no share of it to take off)
    if ((pass.lights.z & LIGHTS_REAL) != 0)
        return cmax;
    for (int s = 0; s < pass.lights.y; s++)
    {
        vec4 t1 = row_texel(SUN_ROW + s, 1), t4 = row_texel(SUN_ROW + s, 4);
        if (t4.w >= 0.0)
            cmax += t4.rgb * max(dot(n, t1.xyz), 0.0);
    }
    return cmax;
}

// each sun's blocked share: a sun is a disk of light of its angular diameter (a 0.2 degree sun, or b30's 50 degree
// sky light, a wide fill), sampled evenly over its solid angle (a stratified point of the unit disk taken to the
// cone: cos = 1 - r^2 (1 - cos(half))), each ray's light its own cosine at the receiver, so a wide light is partly
// blocked by an object that covers part of it; the share no more than the sun's cmax (its whole share). The mean of
// cos over the cone is cos(axis) (1 + cos(half)) / 2, which the estimate divides out (its exact irradiance:
// power x cos(axis) for a cone above the horizon)
vec3 sun_share(vec3 p, vec3 n, ivec2 pixel, int rays)
{
    vec3 share = vec3(0.0);
    vec3 origin = p + n * ray_lift(p);
    for (int s = 0; s < pass.lights.y; s++)
    {
        int row = SUN_ROW + s;
        vec4 t1 = row_texel(row, 1), t4 = row_texel(row, 4), t5 = row_texel(row, 5);
        vec3 f = t1.xyz, up = row_texel(row, 2).xyz, left = row_texel(row, 3).xyz;
        float cosine = dot(n, f);
        if (cosine <= 0.0 || t4.w < 0.0)
            continue;
        float half_cos = cos(0.5 * t5.w), blocked = 0.0;
        for (int k = 0; k < rays; k++)
        {
            vec2 disk = concentric(stratified(pixel, 17.0 + float(s) * 16.0 + float(k)));
            float r2 = dot(disk, disk), c = 1.0 - r2 * (1.0 - half_cos), sine = sqrt(max(1.0 - c * c, 0.0));
            vec2 around = r2 > 0.0 ? disk * inversesqrt(r2) : vec2(0.0);
            vec3 d = normalize(f * c + (left * around.x + up * around.y) * sine);
            float lit = dot(n, d);
            if (lit > 0.0 && visible_by(origin, d, SUN_REACH, -1, 0xffffffffu, RT_MASK_DYNAMIC) < 1.0 &&
                visible_by(origin, d, SUN_REACH, -1, 0xffffffffu, RT_MASK_STRUCTURE) > 0.0)
                blocked += lit;
        }
        share += t4.rgb * min(blocked / (float(rays) * 0.5 * (1.0 + half_cos)), cosine);
    }
    return share;
}

// lit live: the suns' whole direct light at p: each one's unblocked irradiance (power x the shaded cosine at its axis)
// by the share of its disk (its angular diameter, sampled as sun_share does) that rays find unblocked through every
// occluder (the structure, the objects, the viewer's own model for its shadow). The dome's row is the bounce's
vec3 sun_real(vec3 p, vec3 n, vec3 shaded, ivec2 pixel, int rays)
{
    vec3 light = vec3(0.0);
    vec3 origin = p + n * ray_lift(p);
    for (int s = 0; s < pass.lights.y; s++)
    {
        int row = SUN_ROW + s;
        vec4 t1 = row_texel(row, 1), t4 = row_texel(row, 4), t5 = row_texel(row, 5);
        if (t4.w < 0.0 || t5.w > DOME_DIAMETER)
            continue;
        vec3 f = t1.xyz, up = row_texel(row, 2).xyz, left = row_texel(row, 3).xyz;
        float half_cos = cos(0.5 * t5.w), seen = 0.0;
        // (none of the cone above the surface's horizon)
        if (dot(n, f) <= -sqrt(max(1.0 - half_cos * half_cos, 0.0)))
            continue;
        for (int k = 0; k < rays; k++)
        {
            vec2 disk = concentric(stratified(pixel, 17.0 + float(s) * 16.0 + float(k)));
            float r2 = dot(disk, disk), c = 1.0 - r2 * (1.0 - half_cos), sine = sqrt(max(1.0 - c * c, 0.0));
            vec2 around = r2 > 0.0 ? disk * inversesqrt(r2) : vec2(0.0);
            vec3 d = normalize(f * c + (left * around.x + up * around.y) * sine);
            if (dot(n, d) > 0.0)
                seen += visible_by(origin, d, SUN_REACH, -1, 0xffffffffu, RT_MASK_SUN);
        }
        // (the rays find the share of the cone that is seen; the light is its unblocked irradiance (power x the shaded
        // cosine at its axis: exact for a cone above the horizon) by that share. Where nothing blocks it, it has no
        // noise at all, however wide the light: b30's 50 degree fill no longer shimmers on open ground)
        light += t4.rgb * (max(dot(shaded, f), 0.0) * seen / float(rays));
    }
    return light;
}

// where p (normal n, owner word) was the frame before, on the same surface (the previous frame's guides within 3%
// in distance, 25 degrees in normal, the same owner): its pixel there; else false (a disocclusion)
bool reproject(vec3 p, vec3 n, uint owner, out ivec2 q)
{
    q = ivec2(0);
    if (pass.history.x == 0.0)
        return false;
    vec3 w = p - pass.previous[0].xyz, pf = pass.previous[1].xyz, pr = pass.previous[2].xyz;
    vec3 pu = pass.previous[3].xyz;
    float along = dot(w, pf) / dot(pf, pf);
    vec2 was = vec2(dot(w, pr) / dot(pr, pr), dot(w, pu) / dot(pu, pu)) / along;
    vec2 plow = vec2(pass.previous_corners.xy), phigh = vec2(pass.previous_corners.zw);
    vec2 at = plow + (was + 1.0) * 0.5 * (phigh - plow);
    if (!(along > 0.0 && all(greaterThanEqual(at, plow)) && all(lessThan(at, phigh))))
        return false;
    q = ivec2(floor(at));
    uvec4 before = texelFetch(previous_inputs, q, 0);
    float was_distance = uintBitsToFloat(before.z), now_distance = length(w);
    return (before.y & INPUT_PRESENT) != 0u && abs(was_distance - now_distance) <= 0.03 * now_distance &&
        dot(unfold(unpackSnorm2x16(before.x)), n) >= 0.906 && before.w == owner;
}

uint seed(ivec2 pixel, uint salt)
{
    uint h = uint(pixel.x) * 1973u + uint(pixel.y) * 9277u + uint(pass.history.y) * 26699u + salt * 0x9e3779b9u;
    hash_next(h);
    return h;
}

// (LT_INDIRECT: the indirect light's and reflections' kernel instead, its own main: lt/indirect.glsl)
#ifdef LT_INDIRECT
#include "indirect.glsl"
#else
void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if ((pass.control.w & GRADIENT) != 0)
        pixel = lt_gradient_pixel_in(pixel, pass.history.y);
    uvec4 words = texelFetch(inputs, pixel, 0);
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 shaded = unfold(unpackSnorm2x16(texelFetch(guides, pixel, 0).x));
    if (dot(shaded, shaded) < 0.5 || dot(shaded, n) < 0.0)
        shaded = n;
    vec2 low = vec2(pass.corners.xy), size = vec2(pass.corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / size - 1.0;
    vec3 ray = normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
    vec3 p = pass.camera[0].xyz + ray * uintBitsToFloat(words.z);
    bool present = (words.y & INPUT_PRESENT) != 0u;
    int receiver = int(words.w);
    if ((pass.control.w & COPY) != 0)
    {
        vec4 mean = texelFetch(history_image, pixel, 0);
        if ((pass.control.w & COPY_REACH) != 0)
        {
            vec4 t0 = row_texel(pass.lights.w, 0), t1 = row_texel(pass.lights.w, 1);
            vec3 to = t0.xyz - p;
            result = present ? vec4(length(to) * t1.w, dot(n, to) > 0.0 ? 1.0 : 0.0, float(receiver), 1.0) :
                vec4(-1.0, 0.0, 0.0, 0.0);
            return;
        }
        result = (pass.control.w & COPY_GEOMETRIC) != 0 ? vec4(present ? n : vec3(0.0), 1.0) :
            (pass.control.w & COPY_SHADED) != 0 ? vec4(present ? shaded : vec3(0.0), 1.0) :
            (pass.control.w & COPY_PRESENT) != 0 ? vec4(present ? 1.0 : 0.0) :
            vec4(mean.rgb - (present && receiver == 0 ? sun_cmax(n) : vec3(0.0)), mean.a);
        return;
    }
    if ((pass.control.w & FINAL) != 0)
    {
        result = vec4(texelFetch(history_image, pixel, 0).rgb - (present && receiver == 0 ? sun_cmax(n) : vec3(0.0)),
            (pass.lights.z & LIGHTS_REAL) != 0 ? 1.0 : 0.0);
        return;
    }
    // (the reservoirs: RIS over the cell's lights but the heroes, merged with the last frame's at the same point)
    if ((pass.control.w & RESERVOIR) != 0)
    {
        result = vec4(-1.0, 0.0, 0.0, 0.0);
        if (!present)
            return;
        uvec4 mask = cell_lights(p + n * 0.02);
        int h0, h1, others;
        heroes(p, shaded, mask, h0, h1, others);
        if (others <= 0)
            return;
        uint h = seed(pixel, 1u);
        Reservoir r = ris(p, shaded, mask, h0, h1, others, h);
        ivec2 q;
        if ((pass.control.w & RESET) == 0 && reproject(p, n, words.w, q))
        {
            vec4 before = texelFetch(reservoirs, q, 0);
            int now = before.x >= 0.0 ? int(row_texel(int(before.x), 6).y) : -1;
            // (the history's count at most what leaves this frame's with M_CAP in all)
            reservoir_merge(r, before, now, p, shaded, h0, h1, max(M_CAP - r.m, 0.0), h);
        }
        result = r.row >= 0 ? vec4(float(r.row), reservoir_weight(r), r.m, r.target) : vec4(-1.0, 0.0, 0.0, 0.0);
        return;
    }
    result = vec4(0.0);
    if (!present)
        return;
    vec3 lit = pass.control.y != 0 && (pass.lights.z & LIGHTS_FLASHLIGHT) != 0 ?
        flashlight(p, n, shaded, pixel, receiver) : vec3(0.0);
    if (pass.lights.x > 0)
    {
        bool shadow = (pass.control.w & VISIBILITY) != 0;
        uvec4 mask = cell_lights(p + n * 0.02);
        int h0, h1, others;
        heroes(p, shaded, mask, h0, h1, others);
        if (h0 >= 0)
            lit += light_sample(h0, p, n, shaded, stratified(pixel, 1.0), receiver, shadow);
        if (h1 >= 0)
            lit += light_sample(h1, p, n, shaded, stratified(pixel, 2.0), receiver, shadow);
        if (others > 0 && (pass.lights.z & LIGHTS_RESTIR) != 0)
        {
            uint h = seed(pixel, 2u);
            Reservoir r;
            if ((pass.lights.z & LIGHTS_REUSE) != 0)
            {
                // (the spatial pass: this pixel's reservoir and 4 neighbours' within 16 pixels on a like surface)
                r = Reservoir(-1, 0.0, 0.0, 0.0);
                vec4 own = texelFetch(reservoirs, pixel, 0);
                reservoir_merge(r, own, int(own.x), p, shaded, h0, h1, M_CAP, h);
                float distance = uintBitsToFloat(words.z);
                for (int k = 0; k < 4; k++)
                {
                    float angle = random(h) * 6.2831853, reach = 2.0 + random(h) * 14.0;
                    ivec2 at = pixel + ivec2(round(vec2(cos(angle), sin(angle)) * reach));
                    if (any(lessThan(at, pass.corners.xy)) || any(greaterThanEqual(at, pass.corners.zw)))
                        continue;
                    uvec4 there = texelFetch(inputs, at, 0);
                    if ((there.y & INPUT_PRESENT) == 0u || abs(uintBitsToFloat(there.z) - distance) > 0.1 * distance ||
                        dot(unfold(unpackSnorm2x16(there.x)), n) < 0.9)
                        continue;
                    vec4 other = texelFetch(reservoirs, at, 0);
                    reservoir_merge(r, other, int(other.x), p, shaded, h0, h1, M_CAP, h);
                }
            }
            else
                r = ris(p, shaded, mask, h0, h1, others, h);
            float weight = reservoir_weight(r);
            if (weight > 0.0)
                lit += light_sample(r.row, p, n, shaded, stratified(pixel, 3.0), receiver, shadow) * weight;
        }
    }
    // (the suns on the structure, which the lightmap lights: their whole share less what a dynamic object blocks,
    // FINAL taking the whole off; SUN_FIRST rays where the pixel has no history)
    ivec2 q;
    if ((pass.lights.z & LIGHTS_REAL) != 0 && pass.lights.y > 0)
    {
        // (lit live: every receiver, the suns' whole light)
        bool fresh = (pass.control.w & (RESET | GRADIENT)) == RESET || !reproject(p, n, words.w, q);
        lit += sun_real(p, n, shaded, pixel, fresh ? SUN_FIRST : 1);
    }
    else if (receiver == 0 && pass.lights.y > 0)
    {
        bool fresh = (pass.control.w & (RESET | GRADIENT)) == RESET || !reproject(p, n, words.w, q);
        lit += sun_cmax(n) - sun_share(p, n, pixel, fresh ? SUN_FIRST : 1);
    }
    // (the history at the same point where it is the same surface; else none, a disocclusion)
    vec3 mean = vec3(0.0);
    float count = 0.0;
    if (pass.control.x > 1 && (pass.control.w & RESET) == 0 && reproject(p, n, words.w, q))
    {
        vec4 old = texelFetch(history_image, q, 0);
        mean = old.rgb;
        count = old.a;
    }
    count = min(count + 1.0, float(pass.control.x));
    mean += (lit - mean) / count;
    result = vec4(mean, count);
}
#endif
