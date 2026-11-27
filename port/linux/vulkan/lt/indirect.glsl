// LT indirect and reflections (~/dev/rt2-realtime-design.md sections 1.3 and 1.4): lt/e1.frag built again with
// LT_INDIRECT (tools/vulkan_lt_shaders.py's lt_indirect_spirv), its lights' functions shared, its direct light's main
// left out; drawn by xgpu_vulkan_lt_indirect.c. Every mode is a triangle over its target; control.w says which:
//
// BOUNCE (half resolution, a texel a 2x2 block): one cosine ray (the guides' bump-mapped normal) from the block's
//   pixel of this frame's phase (another of the block where that one has no surface); at its hit, the material
//   record's albedo (linear) times the hit's dynamic light: one light of the hit's cell chosen by its unshadowed
//   light (the flashlight among them), one shadow ray to it, plus the radiance cache's indirect light there. Only
//   dynamic light: the static bounce is the lightmap's, never counted twice. Out: rgb the irradiance's sample (a
//   cosine sample's estimator: albedo x the hit's irradiance), a 1 + the light-id hash + 1024 x the block's pixel.
// SPECULAR_RAY (full resolution, the guides' specular mask's pixels): one GGX ray (Heitz's visible normals; the
//   guides' roughness, else a fallback by owner); its hit shaded as Halo's lightmap x albedo (the record's lightmap
//   page; a model 0.6 of its albedo, as display.rt_reflections had it) plus the dynamic direct and the cache, faded
//   to nothing at the reach. Out: rgb the radiance (linear), a the hit's distance (0 a miss, -1 no specular).
// COMBINE: the bounce's blocks about each pixel on its surface, weighted (the half resolution's upsampling), added
//   to the direct light's 1-spp sample (the denoiser's input: one diffuse signal) or, ACCUMULATE, counted in their
//   own history (E2, accumulate). SUM: the direct mean and the indirect's.
// SPEC_HISTORY: the specular's count-based history (the hits' share, COVERAGE, or the radiance), reprojected.
// CACHE_RESOLVE (over the cache's slots, lt/indirect_cache.glsl): each slot's cell's bounce samples this frame,
//   gathered where the cell is seen (16 taps over its footprint, each kept where its traced pixel is in that cell),
//   merged by count (1/n up to 256: never an exponential average); a slot starts again when its cell or its
//   light-id hash changes, and keeps its mean where no sample came (off the view, hidden).
// SPEC_FINAL: the light the surfaces read's right half (xgpu_spirv.c's lt_reflection_texel): the specular
//   radiance and the hits' share.

#include "indirect_cache.glsl"

#define BOUNCE 4096
#define SPECULAR_RAY 8192
#define COMBINE 16384
#define SUM 32768
#define SPEC_HISTORY 65536
#define CACHE_RESOLVE 131072
#define SPEC_FINAL 262144
#define ACCUMULATE 524288
#define INDIRECT_ONLY 1048576
#define COVERAGE 2097152
#define CACHE_ON 4194304

// the shader types (shaders.c's; xgpu_spirv.c's RT_REFLECTION_KIND_*)
#define KIND_ENVIRONMENT 3u
#define KIND_MODEL 4u
#define MASK_REFLECTIONS 0x0Bu
// the rays' reach (world units), the reflections' fade from 80% of it (display.rt_reflections')
#define BOUNCE_REACH 32.0
#define SPECULAR_REACH 25.0
#define SPECULAR_FADE 0.8
// a model's light in a reflection (it has no lightmap) and the albedo's texture level
#define OBJECT_LIGHT 0.6
#define ALBEDO_LEVEL 2.0
// the roughness where the guides have none (no MCC/mod .orm.png): the structure's, an object's
#define ROUGH_STRUCTURE 0.08
#define ROUGH_OBJECT 0.2
#define CACHE_CAP 256.0
// the record's flags (xgpu_stream.h's XGPU_WORLD_RT_MATERIAL_MASK_*)
#define MASK_BUMP 0x100u
#define MASK_MULTIPURPOSE 0x200u
// the bounce's rays where its history starts again
#define BOUNCE_FIRST 4
// the roughness up to which a reflection is its mirror ray alone
#define MIRROR_ROUGHNESS 0.1

// (the bindings as the modes use them)
#define cache_image history_image

struct Hit
{
    bool hit, attributes;
    float t;
    vec3 p, n, albedo, surface;
    int owner;
};

void frame_about(vec3 n, out vec3 t, out vec3 b)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0, a = -1.0 / (s + n.z), c = n.x * n.y * a;
    t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
    b = vec3(c, s + n.y * n.y * a, -n.y);
}

vec3 cosine_direction(vec3 n, vec2 xi)
{
    vec2 d = concentric(xi);
    vec3 t, b;
    frame_about(n, t, b);
    return normalize(t * d.x + b * d.y + n * sqrt(max(1.0 - dot(d, d), 0.0)));
}

// a GGX normal of the visible ones from v (Heitz 2018), alpha = roughness^2
vec3 ggx_visible_normal(vec3 v, vec3 n, float alpha, vec2 u)
{
    vec3 t, b;
    frame_about(n, t, b);
    vec3 ve = vec3(dot(v, t), dot(v, b), dot(v, n));
    vec3 vh = normalize(vec3(alpha * ve.x, alpha * ve.y, ve.z));
    float lensq = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u.x), phi = 6.2831853 * u.y, a = r * cos(phi), c = r * sin(phi), s = 0.5 * (1.0 + vh.z);
    c = (1.0 - s) * sqrt(max(1.0 - a * a, 0.0)) + s * c;
    vec3 nh = a * t1 + c * t2 + sqrt(max(1.0 - a * a - c * c, 0.0)) * vh;
    vec3 ne = normalize(vec3(alpha * nh.x, alpha * nh.y, max(nh.z, 0.0)));
    return normalize(t * ne.x + b * ne.y + n * ne.z);
}

vec3 hit_texel(uint slot, vec2 st, float level)
{
    return textureLod(hit_textures[nonuniformEXT(min(slot, HIT_TEXTURES - 1u))], st, level).rgb;
}

// the first surface along d (alpha-tested; skip's object left out, -1 none, the structure never): where, its
// normal (toward the ray), its albedo (linear) and, with lightmap, Halo's colour there (base x detail x lightmap,
// gamma), from the hit table (xgpu_spirv.c's rt_reflection_texel's reading)
Hit trace(vec3 origin, vec3 d, float t_max, int skip, bool lightmap)
{
    Hit hit = Hit(false, false, 0.0, vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0), 0);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, 0u, MASK_REFLECTIONS, origin, 0.0, d, t_max);
    while (rayQueryProceedEXT(q))
    {
        int candidate = rayQueryGetIntersectionInstanceCustomIndexEXT(q, false);
        if ((skip < 0 || candidate == 0 || candidate != skip) && alpha_passes(q))
            rayQueryConfirmIntersectionEXT(q);
    }
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return hit;
    hit.hit = true;
    hit.t = rayQueryGetIntersectionTEXT(q, true);
    hit.p = origin + d * hit.t;
    hit.owner = rayQueryGetIntersectionInstanceCustomIndexEXT(q, true);
    uint mesh = uint(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(q, true)) & (HIT_MESHES - 1u);
    uint record = mesh << 2, attributes = table.words[record + 1u];
    if (attributes == 0u)
        return hit;
    hit.attributes = true;
    uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)) +
        (rayQueryGetIntersectionGeometryIndexEXT(q, true) == 1 ? table.words[record + 3u] : 0u);
    uint corners = table.words[record] + primitive * 3u;
    uint material_word = table.words[table.words[record + 2u] + (primitive >> 1)];
    uint at = HIT_MATERIALS + (((material_word >> ((primitive & 1u) << 4)) & 0xFFFFu) << 4);
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, true);
    vec3 w = vec3(1.0 - b.x - b.y, b.x, b.y);
    vec2 st = vec2(0.0), lst = vec2(0.0);
    vec3 normal = vec3(0.0);
    for (int corner = 0; corner < 3; corner++)
    {
        uint first = attributes + table.words[corners + uint(corner)] * 6u;
        st += w[corner] * vec2(uintBitsToFloat(table.words[first]), uintBitsToFloat(table.words[first + 1u]));
        normal += w[corner] * unfold(unpackSnorm2x16(table.words[first + 2u]));
        lst += w[corner] * unpackSnorm2x16(table.words[first + 4u]);
    }
    hit.n = normalize(mat3(rayQueryGetIntersectionObjectToWorldEXT(q, true)) * normal);
    if (dot(hit.n, d) > 0.0)
        hit.n = -hit.n;
    uint kind = table.words[at + 1u];
    vec2 scale = kind == KIND_MODEL ? vec2(uintBitsToFloat(table.words[at + 5u]), uintBitsToFloat(table.words[at + 6u])) :
        vec2(1.0);
    float level = lightmap ? HIT_LEVEL : ALBEDO_LEVEL;
    vec3 base = hit_texel(table.words[at + 7u], st * scale, level);
    uint detail = table.words[at + 8u];
    if (detail != 0u && kind == KIND_ENVIRONMENT)
        base *= 2.0 * hit_texel(detail, st * uintBitsToFloat(table.words[at + 9u]), level);
    base = clamp(base, 0.0, 1.0);
    hit.albedo = pow(base, vec3(2.2));
    if (lightmap)
    {
        uint page = table.words[at + 14u];
        hit.surface = clamp(base * (page != 0u ? hit_texel(page, lst, 0.0) : vec3(OBJECT_LIGHT)), 0.0, 1.0);
    }
    return hit;
}

// the flashlight's light at p from its lamp's middle, unshadowed (luminance: the choice's weight)
float flashlight_estimate(vec3 p, vec3 n)
{
    vec3 f = pass.forward.xyz, v = p - pass.light.xyz;
    float z = dot(v, f), width = max(pass.color.w, 1.0e-4);
    if (z <= 1.0e-4)
        return 0.0;
    float a = dot(v, pass.left.xyz) / (z * width), b = dot(v, pass.up.xyz) / (z * width);
    if (abs(a) >= 1.0 || abs(b) >= 1.0)
        return 0.0;
    float gel = cookie(a, b);
    vec3 to = pass.lamp.xyz - p;
    float d = length(to);
    vec3 l = to / max(d, 1.0e-6);
    float facing = dot(n, l), emitting = dot(normalize(f), -l), window = range_window(d, abs(pass.light.w));
    if (gel <= 0.0 || facing <= 0.0 || emitting <= 0.0 || window <= 0.0)
        return 0.0;
    return luminance_of(pow(max(pass.color.rgb * gel, vec3(0.0)), vec3(2.2))) *
        pass.lamp.w * facing * emitting * window / max(d * d, 1.0e-4);
}

// the dynamic direct light at a hit: one of its cell's lights (and the flashlight) chosen by its unshadowed light,
// one shadow ray to it, over its chance
vec3 direct_one(vec3 p, vec3 n, int owner, ivec2 pixel, float salt, inout uint h)
{
    float total = 0.0, chosen_weight = 0.0;
    int chosen = -2;
    if (pass.control.y != 0 && (pass.lights.z & LIGHTS_FLASHLIGHT) != 0)
    {
        float e = flashlight_estimate(p, n);
        if (e > 0.0)
        {
            total = e;
            chosen = -1;
            chosen_weight = e;
        }
    }
    if (pass.lights.x > 0)
    {
        uvec4 mask = cell_lights(p + n * 0.02);
        for (int word = 0; word < 4; word++)
        {
            uint bits = mask[word];
            while (bits != 0u)
            {
                int row = word * 32 + findLSB(bits);
                bits &= bits - 1u;
                if (row >= pass.lights.x)
                    break;
                float e = light_estimate(row, p, n);
                if (e <= 0.0)
                    continue;
                total += e;
                if (random(h) * total < e)
                {
                    chosen = row;
                    chosen_weight = e;
                }
            }
        }
    }
    if (chosen == -2 || chosen_weight <= 0.0)
        return vec3(0.0);
    vec3 lit = chosen == -1 ? flashlight(p, n, n, pixel, owner) :
        light_sample(chosen, p, n, n, stratified(pixel, salt), owner, true);
    return lit * (total / chosen_weight);
}

// the light-id hash of p's lights (its cell's, by their stable ids: the light table's texel 7; the flashlight's
// being on), 10 bits
uint light_hash(vec3 p, vec3 n)
{
    uint h = pass.control.y != 0 && (pass.lights.z & LIGHTS_FLASHLIGHT) != 0 ? 0x2545u : 0u;
    if (pass.lights.x > 0)
    {
        uvec4 mask = cell_lights(p + n * 0.02);
        for (int word = 0; word < 4; word++)
        {
            uint bits = mask[word];
            while (bits != 0u)
            {
                int row = word * 32 + findLSB(bits);
                bits &= bits - 1u;
                if (row >= pass.lights.x)
                    break;
                if (row_texel(row, 4).w >= 0.0)
                    h += cache_hash(floatBitsToUint(row_texel(row, 7).x) + 1u);
            }
        }
    }
    return h & 1023u;
}

// the cache's indirect light at a hit: its cell's mean where the slot holds that cell (its check) for the same lights
vec3 cache_lookup(vec3 p, vec3 n)
{
    if ((pass.control.w & CACHE_ON) == 0)
        return vec3(0.0);
    int k = cascade_of(p);
    if (k < 0)
        return vec3(0.0);
    ivec3 c = ivec3(floor(p / cascade_size(k)));
    vec4 slot = texelFetch(cache_image, slot_texel(c, octant_of(n), k), 0);
    uint packed = uint(max(slot.a, 0.0)), count = packed & 511u;
    if (count == 0u || ((packed >> 9) & 15u) != cell_check(c, k) || (packed >> 13) != light_hash(p, n))
        return vec3(0.0);
    return max(slot.rgb, vec3(0.0));
}

// where p is in this frame's pixels (continuous); false off its view
bool project_now(vec3 p, out vec2 at, out float along)
{
    vec3 w = p - pass.camera[0].xyz, f = pass.camera[1].xyz, r = pass.camera[2].xyz, u = pass.camera[3].xyz;
    along = dot(w, f) / dot(f, f);
    at = vec2(0.0);
    if (along <= 0.0)
        return false;
    vec2 screen = vec2(dot(w, r) / dot(r, r), dot(w, u) / dot(u, u)) / along;
    at = vec2(pass.corners.xy) + (screen + 1.0) * 0.5 * vec2(pass.corners.zw - pass.corners.xy);
    return all(greaterThanEqual(at, vec2(pass.corners.xy))) && all(lessThan(at, vec2(pass.corners.zw)));
}

// a pixel's surface: present, its point, geometric and shaded normals, owner
bool surface_at(ivec2 pixel, out vec3 p, out vec3 n, out vec3 shaded, out uint owner)
{
    uvec4 words = texelFetch(inputs, pixel, 0);
    n = unfold(unpackSnorm2x16(words.x));
    shaded = unfold(unpackSnorm2x16(texelFetch(guides, pixel, 0).x));
    if (dot(shaded, shaded) < 0.5 || dot(shaded, n) < 0.0)
        shaded = n;
    vec2 low = vec2(pass.corners.xy), size = vec2(pass.corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / size - 1.0;
    vec3 ray = normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
    p = pass.camera[0].xyz + ray * uintBitsToFloat(words.z);
    owner = words.w;
    return (words.y & INPUT_PRESENT) != 0u && all(greaterThanEqual(pixel, pass.corners.xy)) &&
        all(lessThan(pixel, pass.corners.zw));
}

vec4 bounce_main(ivec2 block)
{
    int phase = int(pass.indirect[1].y);
    vec3 p, n, shaded;
    uint owner;
    int which = 0;
    ivec2 pixel = ivec2(0);
    bool found = false;
    for (; which < 4 && !found; which++)
    {
        pixel = block * 2 + block_pixel(phase + which);
        found = surface_at(pixel, p, n, shaded, owner);
    }
    if (!found)
        return vec4(0.0);
    which--;
    // (BOUNCE_FIRST rays where the history starts again, so its first frame is as good as a few: the suns' share's
    // rule, lt/e1.frag)
    int rays = (pass.control.w & RESET) != 0 ? BOUNCE_FIRST : 1;
    vec3 e = vec3(0.0), origin = p + n * (0.004 + 0.0005 * length(p - pass.camera[0].xyz));
    uint h = seed(pixel, 11u);
    for (int k = 0; k < rays; k++)
    {
        vec3 d = cosine_direction(shaded, stratified(pixel, 5.0 + 17.0 * float(k)));
        if (dot(d, n) <= 0.0)
            continue;
        Hit hit = trace(origin, d, BOUNCE_REACH, int(owner), false);
        if (hit.attributes)
            e += hit.albedo * (direct_one(hit.p, hit.n, hit.owner, pixel, 6.0 + 17.0 * float(k), h) +
                cache_lookup(hit.p, hit.n));
    }
    e /= float(rays);
    if (any(isnan(e)) || any(isinf(e)))
        e = vec3(0.0);
    return vec4(e, 1.0 + float(light_hash(p, n)) + 1024.0 * float(which));
}

// how much of the reflection a pixel shows (its own surface's: the eye's ray to its point, the material record there):
// the shader's strength head-on to grazing (the record's flags, Schlick's fresnel at this view's cosine c) times its
// specular mask (the base, bump or multipurpose map's alpha); and the light it would reflect, guessed from the light
// there: Halo's lightmap's (linear) and the pass's direct light this frame (the reservoirs' binding), as on a
// half-bright surface. 0 where the eye's ray finds another surface
float reflection_seen(ivec2 pixel, vec3 p, float c)
{
    vec3 eye = pass.camera[0].xyz, d = p - eye;
    float distance = length(d);
    d /= max(distance, 1.0e-6);
    rayQueryEXT q;
    rayQueryInitializeEXT(q, scene, 0u, MASK_REFLECTIONS, eye, 0.0, d, distance * 1.02 + 0.02);
    while (rayQueryProceedEXT(q))
        if (alpha_passes(q))
            rayQueryConfirmIntersectionEXT(q);
    if (rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ||
        abs(rayQueryGetIntersectionTEXT(q, true) - distance) > 0.02 * distance + 0.02)
        return 0.0;
    uint mesh = uint(rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT(q, true)) & (HIT_MESHES - 1u);
    uint record = mesh << 2, attributes = table.words[record + 1u];
    if (attributes == 0u)
        return 0.0;
    uint primitive = uint(rayQueryGetIntersectionPrimitiveIndexEXT(q, true)) +
        (rayQueryGetIntersectionGeometryIndexEXT(q, true) == 1 ? table.words[record + 3u] : 0u);
    uint corners = table.words[record] + primitive * 3u;
    uint material_word = table.words[table.words[record + 2u] + (primitive >> 1)];
    uint at = HIT_MATERIALS + (((material_word >> ((primitive & 1u) << 4)) & 0xFFFFu) << 4);
    uint flags = table.words[at + 2u];
    float perpendicular = float((flags >> 16) & 255u) / 128.0, parallel = float(flags >> 24) / 128.0;
    if (max(perpendicular, parallel) <= 0.0)
        return 0.0;
    vec2 b = rayQueryGetIntersectionBarycentricsEXT(q, true);
    vec3 w = vec3(1.0 - b.x - b.y, b.x, b.y);
    vec2 st = vec2(0.0), lst = vec2(0.0);
    for (int corner = 0; corner < 3; corner++)
    {
        uint first = attributes + table.words[corners + uint(corner)] * 6u;
        st += w[corner] * vec2(uintBitsToFloat(table.words[first]), uintBitsToFloat(table.words[first + 1u]));
        lst += w[corner] * unpackSnorm2x16(table.words[first + 4u]);
    }
    uint kind = table.words[at + 1u];
    vec2 scale = kind == KIND_MODEL ? vec2(uintBitsToFloat(table.words[at + 5u]), uintBitsToFloat(table.words[at + 6u])) :
        vec2(1.0);
    uint mask_map = (flags & MASK_BUMP) != 0u ? table.words[at + 11u] : (flags & MASK_MULTIPURPOSE) != 0u ?
        table.words[at + 10u] : table.words[at + 7u];
    float mask = mask_map != 0u ?
        textureLod(hit_textures[nonuniformEXT(min(mask_map, HIT_TEXTURES - 1u))], st * scale, ALBEDO_LEVEL).a : 1.0;
    float f = pow(clamp(1.0 - c, 0.0, 1.0), 5.0);
    uint page = table.words[at + 14u];
    float light = page != 0u ? luminance_of(pow(hit_texel(page, lst, 0.0), vec3(2.2))) : pow(OBJECT_LIGHT, 2.2);
    light += 0.5 * max(luminance_of(texelFetch(reservoirs, pixel, 0).rgb), 0.0);
    return mix(perpendicular, parallel, f) * mask * light;
}

vec4 specular_main(ivec2 pixel)
{
    vec3 p, n, shaded;
    uint owner;
    if (!surface_at(pixel, p, n, shaded, owner))
        return vec4(0.0, 0.0, 0.0, -1.0);
    uint material = texelFetch(guides, pixel, 0).z;
    // (the structure's alone: the surfaces read the pass's reflections there only, xgpu_spirv.c's
    // lt_reflection_traced; an object's keeps its cube map)
    if (((material >> 24) & 255u) == 0u || owner != 0u)
        return vec4(0.0, 0.0, 0.0, -1.0);
    uint byte = (material >> 16) & 255u;
    float roughness = byte == 255u ? (owner == 0u ? ROUGH_STRUCTURE : ROUGH_OBJECT) : float(byte) / 255.0;
    vec3 v = normalize(pass.camera[0].xyz - p), normal = dot(shaded, v) > 0.0 ? shaded : n;
    // (only where the reflection can be seen: elsewhere the cube map, as the game drew it)
    if (reflection_seen(pixel, p, max(dot(normal, v), 0.0)) < pass.indirect[2].z)
        return vec4(0.0, 0.0, 0.0, -1.0);
    // (a surface this smooth, Halo's own without a roughness map among them: its mirror ray, the same each frame,
    // no noise; its texture's level prefilters the hit)
    // (BOUNCE_FIRST rays where the history starts again, the bounce's rule: a sampled lobe's first frame as good as
    // a few)
    bool mirror = roughness <= MIRROR_ROUGHNESS;
    int rays = !mirror && (pass.control.w & RESET) != 0 ? BOUNCE_FIRST : 1, hits = 0;
    vec3 origin = p + n * (0.004 + 0.0005 * length(p - pass.camera[0].xyz)), sum = vec3(0.0);
    float distance = 0.0;
    uint h = seed(pixel, 13u);
    for (int k = 0; k < rays; k++)
    {
        vec3 l = mirror ? reflect(-v, normal) : reflect(-v, ggx_visible_normal(v, normal,
            max(roughness * roughness, 1.0e-3), stratified(pixel, 9.0 + 17.0 * float(k))));
        if (dot(l, n) <= 0.0)
            l = reflect(-v, normal);
        if (dot(l, n) <= 0.0)
            l = reflect(-v, n);
        // (every owner's: a surface sees itself, the player too)
        Hit hit = trace(origin, l, SPECULAR_REACH, -1, true);
        if (!hit.attributes)
            continue;
        vec3 radiance = pow(hit.surface, vec3(2.2)) + hit.albedo * (direct_one(hit.p, hit.n, hit.owner, pixel,
            10.0 + 17.0 * float(k), h) + cache_lookup(hit.p, hit.n));
        if (any(isnan(radiance)) || any(isinf(radiance)))
            radiance = vec3(0.0);
        sum += radiance * (1.0 - smoothstep(SPECULAR_REACH * SPECULAR_FADE, SPECULAR_REACH, hit.t));
        distance += hit.t;
        hits++;
    }
    // (the hits' radiance and distance their means: the coverage's count, SPEC_HISTORY, keeps the misses' share; a 0
    // where none met the scene)
    return hits > 0 ? vec4(sum / float(hits), max(distance / float(hits), 1.0e-3)) : vec4(0.0);
}

// the bounce's samples about pixel on its surface (the 3x3 blocks about its own, Gaussian, 3 pixels; its noise counted down by the bounce's own history; each block's traced pixel within a
// few pixels, the same owner, its normal within 25 degrees and its point on this pixel's plane)
vec3 upsampled(ivec2 pixel, vec3 p, vec3 n, uint owner)
{
    ivec2 size = ivec2(pass.indirect[0].xy), middle = pixel / 2;
    int phase = int(pass.indirect[1].y);
    float distance = length(p - pass.camera[0].xyz), weights = 0.0;
    vec3 sum = vec3(0.0);
    for (int k = 0; k < 9; k++)
    {
        ivec2 block = middle + ivec2(k % 3 - 1, k / 3 - 1);
        if (any(lessThan(block, ivec2(0))) || any(greaterThanEqual(block, size)))
            continue;
        vec4 s = texelFetch(reservoirs, block, 0);
        if (s.a < 0.5)
            continue;
        int which = int(floor((s.a - 1.0) / 1024.0));
        ivec2 at = block * 2 + block_pixel(phase + which);
        // (its inputs alone: the guides' shaded normal is not tested)
        uvec4 words = texelFetch(inputs, at, 0);
        if ((words.y & INPUT_PRESENT) == 0u || words.w != owner || dot(unfold(unpackSnorm2x16(words.x)), n) < 0.906)
            continue;
        vec2 screen = (vec2(at) + 0.5 - vec2(pass.corners.xy)) * 2.0 / vec2(pass.corners.zw - pass.corners.xy) - 1.0;
        vec3 q = pass.camera[0].xyz + normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x +
            pass.camera[3].xyz * screen.y) * uintBitsToFloat(words.z);
        if (abs(dot(q - p, n)) > 0.02 * distance)
            continue;
        vec2 off = vec2(at - pixel);
        float w = exp(-dot(off, off) * (1.0 / 18.0));
        sum += w * s.rgb;
        weights += w;
    }
    return weights > 0.0 ? sum / weights : vec3(0.0);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    int flags = pass.control.w;
    if ((flags & BOUNCE) != 0)
    {
        result = bounce_main(pixel);
        return;
    }
    if ((flags & SPECULAR_RAY) != 0)
    {
        result = specular_main(pixel);
        return;
    }
    if ((flags & SPEC_FINAL) != 0)
    {
        ivec2 at = pixel - ivec2(int(pass.indirect[1].x), 0);
        result = vec4(max(texelFetch(history_image, at, 0).rgb, vec3(0.0)),
            clamp(texelFetch(reservoirs, at, 0).r, 0.0, 1.0));
        return;
    }
    if ((flags & SUM) != 0)
    {
        vec4 direct = texelFetch(history_image, pixel, 0);
        result = vec4(((flags & INDIRECT_ONLY) != 0 ? vec3(0.0) : direct.rgb) + texelFetch(reservoirs, pixel, 0).rgb,
            direct.a);
        return;
    }
    if ((flags & CACHE_RESOLVE) != 0)
    {
        // (the slot's mean and packed count, check, light hash: count + 512 (check + 16 hash))
        vec4 before = texelFetch(cache_image, pixel, 0);
        result = (flags & RESET) != 0 ? vec4(0.0) : before;
        if ((flags & RESET) != 0)
            return;
        // (the slot's cell: its cascade, octant and wrapped index; the window's cell there about the eye)
        int k = pixel.y / (CELLS * 8);
        uint octant = uint((pixel.y / CELLS) % 8);
        ivec3 wrapped = ivec3(pixel.x % CELLS, pixel.x / CELLS, pixel.y % CELLS);
        float size = cascade_size(k);
        ivec3 origin = ivec3(floor(pass.camera[0].xyz / size)) - CELLS / 2;
        ivec3 c = origin + ((wrapped - origin) & (CELLS - 1));
        vec3 centre = (vec3(c) + 0.5) * size;
        vec2 at;
        float along;
        if (!project_now(centre, at, along))
            return;
        // (the cell's half width in pixels; 4x4 taps over it, each a bounce block's sample taken where its traced
        // pixel's point is of this cell (its cascade, place and octant) and of the first taken's lights)
        float reach = clamp(0.5 * size / (length(pass.camera[2].xyz) * along) * 0.5 *
            float(pass.corners.z - pass.corners.x), 1.0, 64.0);
        ivec2 blocks = ivec2(pass.indirect[0].xy);
        int phase = int(pass.indirect[1].y);
        vec3 sum = vec3(0.0);
        float samples = 0.0;
        uint light = 0xFFFFFFFFu;
        for (int t = 0; t < 16; t++)
        {
            vec2 q = at + (vec2(float(t & 3), float(t >> 2)) - 1.5) / 1.5 * reach;
            ivec2 block = ivec2(floor(q * 0.5));
            if (any(lessThan(block, ivec2(0))) || any(greaterThanEqual(block, blocks)))
                continue;
            vec4 s = texelFetch(reservoirs, block, 0);
            if (s.a < 0.5)
                continue;
            float code = s.a - 1.0;
            int which = int(floor(code / 1024.0));
            uint l = uint(code - 1024.0 * float(which));
            vec3 sp, sn, ss;
            uint other;
            if (!surface_at(block * 2 + block_pixel(phase + which), sp, sn, ss, other) || cascade_of(sp) != k ||
                octant_of(sn) != octant || ivec3(floor(sp / size)) != c || (light != 0xFFFFFFFFu && l != light))
                continue;
            light = l;
            sum += s.rgb;
            samples += 1.0;
        }
        if (samples < 0.5)
            return;
        uint packed = uint(max(before.a, 0.0)), count = packed & 511u, check = cell_check(c, k);
        vec3 mean = before.rgb;
        if (count == 0u || ((packed >> 9) & 15u) != check || (packed >> 13) != light)
        {
            count = 0u;
            mean = vec3(0.0);
        }
        float total = min(float(count) + samples, CACHE_CAP), kept = max(total - samples, 0.0);
        mean = (mean * kept + sum * (total - kept) / samples) / total;
        result = vec4(mean, float(uint(total) + 512u * (check + 16u * light)));
        return;
    }
    vec3 p, n, shaded;
    uint owner;
    bool present = surface_at(pixel, p, n, shaded, owner);
    if ((flags & COMBINE) != 0)
    {
        if ((flags & ACCUMULATE) == 0)
        {
            vec4 direct = texelFetch(history_image, pixel, 0);
            vec3 e = present ? upsampled(pixel, p, n, owner) : vec3(0.0);
            result = vec4(((flags & INDIRECT_ONLY) != 0 ? vec3(0.0) : direct.rgb) + e, direct.a);
            return;
        }
        result = vec4(0.0);
        if (!present)
            return;
        vec3 e = upsampled(pixel, p, n, owner), mean = vec3(0.0);
        float count = 0.0;
        ivec2 q;
        if (pass.control.x > 1 && (flags & RESET) == 0 && reproject(p, n, owner, q))
        {
            vec4 old = texelFetch(light_table, q, 0);
            mean = old.rgb;
            count = old.a;
        }
        count = min(count + 1.0, float(pass.control.x));
        mean += (e - mean) / count;
        result = vec4(mean, count);
        return;
    }
    if ((flags & SPEC_HISTORY) != 0)
    {
        result = vec4(0.0);
        vec4 s = texelFetch(reservoirs, pixel, 0);
        if (!present || s.a < 0.0)
            return;
        vec3 value = (flags & COVERAGE) != 0 ? vec3(s.a > 0.0 ? 1.0 - smoothstep(SPECULAR_REACH * SPECULAR_FADE,
            SPECULAR_REACH, s.a) : 0.0) : s.rgb;
        vec3 mean = vec3(0.0);
        float count = 0.0;
        ivec2 q;
        if ((flags & RESET) == 0 && reproject(p, n, owner, q))
        {
            vec4 old = texelFetch(history_image, q, 0);
            mean = old.rgb;
            count = old.a;
        }
        count = min(count + 1.0, max(pass.indirect[1].w, 1.0));
        mean += (value - mean) / count;
        result = vec4(mean, count);
        return;
    }
    result = vec4(0.0);
}
