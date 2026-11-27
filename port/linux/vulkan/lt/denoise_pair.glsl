// The spatial stage's shared passes (xgpu_vulkan_denoise.c, lt/denoise_atrous.frag's PAIR and FINAL): the same
// variance-guided a-trous filter as lt/denoise_atrous.frag's single channel, its rules unchanged, run on the diffuse and
// specular channels at once so that each tap's filter guide, point and edge weights (owner, depth, shaded normal) are
// read and worked out once for both; only the luminance's stop is each channel's own.
//
// PAIR (two outputs: the diffuse's, the specular's; steps 1 and 2): each channel from its temporal stage's history
// (FIRST) or its last pass. A specular pixel without specular (no count) is written with a less than 0 (its deviation
// otherwise), so the later passes know it without the history.
// FINAL (the diffuse's last pass, step 4; one draw a half of the light the surfaces read, xgpu_vulkan_lt.c's lt.final):
// the left half the diffuse filtered once more, less the suns' whole direct share on the structure (lt/e1.frag's
// FINAL: the light carries it, the inputs' exact normal); the right half (control.w RIGHT) the specular's last pass,
// at least 0, and the reflections' hits' share in a (lt/indirect.glsl's SPEC_FINAL).

#define RIGHT 1

#ifdef PAIR
layout(set = 1, binding = 0) uniform sampler2D diffuse_source;
layout(set = 1, binding = 1) uniform usampler2D diffuse_history;
layout(set = 1, binding = 2) uniform usampler2D filter_guide;
layout(set = 1, binding = 3) uniform sampler2D specular_source;
layout(set = 1, binding = 4) uniform usampler2D specular_history;

layout(location = 0) out vec4 diffuse_out;
layout(location = 1) out vec4 specular_out;
#else
layout(set = 1, binding = 0) uniform sampler2D diffuse_source;
layout(set = 1, binding = 1) uniform usampler2D diffuse_history;
layout(set = 1, binding = 2) uniform usampler2D filter_guide;
layout(set = 1, binding = 3) uniform usampler2D inputs;
layout(set = 1, binding = 4) uniform sampler2D specular_source;
layout(set = 1, binding = 5) uniform sampler2D coverage;

layout(location = 0) out vec4 result;
#endif

bool inside(ivec2 q)
{
    return all(greaterThanEqual(q, pass.corners.xy)) && all(lessThan(q, pass.corners.zw));
}

// a history's texel as the filter reads it: (rgb, the mean's variance), its count in count
vec4 from_history(uvec4 h, out float count)
{
    vec3 m = vec3(uintBitsToFloat(h.x), uintBitsToFloat(h.y), uintBitsToFloat(h.z));
    vec2 cv = unpackHalf2x16(h.w);
    float l = luminance(m);
    count = cv.x;
    return vec4(m, cv.y * l * l / max(cv.x, 1.0));
}

// a channel's young pixel's variance, guessed (lt/denoise_atrous.frag's): the least of the four 2x2 quadrants'
float quadrant_variance(vec3 quadrants[4])
{
    float spatial = 1.0e30;
    for (int k = 0; k < 4; k++)
        if (quadrants[k].x >= 3.0)
            spatial = min(spatial, max(quadrants[k].z / quadrants[k].x - pow(quadrants[k].y / quadrants[k].x, 2.0),
                0.0));
    return spatial > 1.0e29 ? 0.0 : spatial;
}

void add_quadrants(inout vec3 quadrants[4], int x, int y, float l)
{
    for (int k = 0; k < 4; k++)
        if ((x == 0 || x == ((k & 1) != 0 ? 1 : -1)) && (y == 0 || y == ((k & 2) != 0 ? 1 : -1)))
            quadrants[k] += vec3(1.0, l, l * l);
}

#ifdef PAIR
// the diffuse's texel at q: (rgb, variance), its count (FIRST: the history's; else not needed)
vec4 fetch_diffuse(ivec2 q, bool first, out float count)
{
    count = 4.0;
    if (first)
        return from_history(texelFetch(diffuse_history, q, 0), count);
    vec4 a = texelFetch(diffuse_source, q, 0);
    return vec4(a.rgb, a.a * a.a);
}

// the specular's, the same; its count (FIRST: the history's; else 1 where it has one, 0 where not)
vec4 fetch_specular(ivec2 q, bool first, out float count)
{
    if (first)
        return from_history(texelFetch(specular_history, q, 0), count);
    vec4 b = texelFetch(specular_source, q, 0);
    count = b.a < 0.0 ? 0.0 : 1.0;
    return vec4(b.rgb, b.a * b.a);
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec2 g = texelFetch(filter_guide, pixel, 0).xy;
    diffuse_out = vec4(0.0);
    specular_out = vec4(0.0, 0.0, 0.0, -1.0);
    if ((g.y & 0xFFFFu) == 0u)
        return;
    bool first = (pass.control.y & FIRST) != 0;
    float dcount, scount;
    vec4 dcentre = fetch_diffuse(pixel, first, dcount), scentre = fetch_specular(pixel, first, scount);
    bool has_specular = scount > 0.0;
    // (the specular's texels read for the whole wave where any of its pixels has specular, else not at all: a branch
    // the wave takes as one)
    bool wave_specular = subgroupAny(has_specular);
    vec4 normals = unpackSnorm4x8(g.x);
    vec3 shaded = unfold(normals.xy), n = unfold(normals.zw);
    float distance = unpackHalf2x16(g.y).x;
    vec3 p = world_point(pixel, distance);
    float dl = luminance(dcentre.rgb), sl = luminance(scentre.rgb);
    bool dyoung = first && dcount < 4.0, syoung = first && has_specular && scount < 4.0;
    float dv = 0.0, dvw = 0.0, sv = 0.0, svw = 0.0;
    vec3 dq[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    vec3 sq[4] = vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            uvec2 other = texelFetch(filter_guide, q, 0).xy;
            if (!inside(q) || (other.y >> 16) != (g.y >> 16) || (other.y & 0xFFFFu) == 0u)
                continue;
            float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
            float dn, sn = 0.0;
            vec4 dc = fetch_diffuse(q, first, dn), sc = wave_specular ? fetch_specular(q, first, sn) : vec4(0.0);
            vec4 on = unpackSnorm4x8(other.x);
            bool alike = (dyoung || syoung) && dot(unfold(on.xy), shaded) >= 0.9994 && dot(unfold(on.zw), n) >= 0.99;
            dv += w * dc.a;
            dvw += w;
            if (dyoung && alike)
                add_quadrants(dq, x, y, luminance(dc.rgb));
            if (has_specular && sn > 0.0)
            {
                sv += w * sc.a;
                svw += w;
                if (syoung && alike)
                    add_quadrants(sq, x, y, luminance(sc.rgb));
            }
        }
    float dspatial = dyoung ? quadrant_variance(dq) : 0.0, sspatial = syoung ? quadrant_variance(sq) : 0.0;
    float ddev = sqrt(dyoung ? dspatial : max(dv / max(dvw, 1.0e-6), 0.0));
    float sdev = sqrt(syoung ? sspatial : max(sv / max(svw, 1.0e-6), 0.0));
    const float kernel[3] = float[3](0.375, 0.25, 0.0625);
    vec3 dsum = vec3(0.0), ssum = vec3(0.0);
    float dweights = 0.0, dvariance = 0.0, sweights = 0.0, svariance = 0.0;
    int step = max(pass.control.z, 1);
    float dsigma = pass.tuning.y * ddev + 1.0e-3 * dl + 1.0e-7, ssigma = pass.tuning.y * sdev + 1.0e-3 * sl + 1.0e-7;
    float sigma_z = pass.tuning.z * distance + 1.0e-4;
    for (int y = -2; y <= 2; y++)
        for (int x = -2; x <= 2; x++)
        {
            ivec2 q = pixel + ivec2(x, y) * step;
            if (!inside(q))
                continue;
            uvec2 other = texelFetch(filter_guide, q, 0).xy;
            if ((other.y & 0xFFFFu) == 0u || (other.y >> 16) != (g.y >> 16))
                continue;
            float dn, sn = 0.0;
            vec4 dc = fetch_diffuse(q, first, dn), sc = wave_specular ? fetch_specular(q, first, sn) : vec4(0.0);
            if (dyoung && dn < 4.0)
                dc.a = dspatial;
            float k = kernel[abs(x)] * kernel[abs(y)], geometry = 0.0;
            if (x != 0 || y != 0)
            {
                vec3 pq = world_point(q, unpackHalf2x16(other.y).x);
                vec4 on = unpackSnorm4x8(other.x);
                geometry = (1.0 - dot(shaded, unfold(on.xy))) / pass.tuning.w + abs(dot(n, pq - p)) / sigma_z;
            }
            float w = k * ((x != 0 || y != 0) ? exp(-(geometry + abs(luminance(dc.rgb) - dl) / dsigma)) : 1.0);
            dsum += w * dc.rgb;
            dvariance += w * w * dc.a;
            dweights += w;
            if (has_specular && sn > 0.0)
            {
                if (syoung && sn < 4.0)
                    sc.a = sspatial;
                float ws = k * ((x != 0 || y != 0) ? exp(-(geometry + abs(luminance(sc.rgb) - sl) / ssigma)) : 1.0);
                ssum += ws * sc.rgb;
                svariance += ws * ws * sc.a;
                sweights += ws;
            }
        }
    diffuse_out = vec4(dsum / dweights, sqrt(dvariance) / dweights);
    if (has_specular)
        specular_out = vec4(ssum / sweights, sqrt(svariance) / sweights);
}
#else
// the suns' whole direct share at the normal n (lt/e1.frag's sun_cmax, from the uniforms)
vec3 sun_cmax(vec3 n)
{
    vec3 cmax = vec3(0.0);
    for (int s = 0; s < pass.suns.x && s < 8; s++)
        cmax += pass.sun[2 * s + 1].rgb * max(dot(n, pass.sun[2 * s].xyz), 0.0);
    return cmax;
}

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    if ((pass.control.w & RIGHT) != 0)
    {
        ivec2 at = pixel - ivec2(int(pass.size.x), 0);
        vec4 s = texelFetch(specular_source, at, 0);
        result = vec4(s.a < 0.0 ? vec3(0.0) : max(s.rgb, vec3(0.0)), clamp(texelFetch(coverage, at, 0).r, 0.0, 1.0));
        return;
    }
    uvec4 words = texelFetch(inputs, pixel, 0);
    vec3 cmax = (words.y & INPUT_PRESENT) != 0u && words.w == 0u ? sun_cmax(unfold(unpackSnorm2x16(words.x))) :
        vec3(0.0);
    uvec2 g = texelFetch(filter_guide, pixel, 0).xy;
    result = vec4(-cmax, 0.0);
    if ((g.y & 0xFFFFu) == 0u)
        return;
    vec4 c = texelFetch(diffuse_source, pixel, 0);
    vec4 centre = vec4(c.rgb, c.a * c.a);
    float count = unpackHalf2x16(texelFetch(diffuse_history, pixel, 0).w).x;
    // (a pixel under 4 frames: the first two passes alone; step 0, checking: the last pass's)
    if (pass.control.z == 0 || (pass.control.z > 2 && count < 4.0))
    {
        result = vec4(centre.rgb - cmax, 0.0);
        return;
    }
    vec4 normals = unpackSnorm4x8(g.x);
    vec3 shaded = unfold(normals.xy), n = unfold(normals.zw);
    float distance = unpackHalf2x16(g.y).x;
    vec3 p = world_point(pixel, distance);
    float lp = luminance(centre.rgb), v = 0.0, vw = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++)
        {
            ivec2 q = pixel + ivec2(x, y);
            uvec2 other = texelFetch(filter_guide, q, 0).xy;
            if (!inside(q) || (other.y >> 16) != (g.y >> 16) || (other.y & 0xFFFFu) == 0u)
                continue;
            float w = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25), a = texelFetch(diffuse_source, q, 0).a;
            v += w * a * a;
            vw += w;
        }
    float deviation = sqrt(max(v / max(vw, 1.0e-6), 0.0));
    const float kernel[3] = float[3](0.375, 0.25, 0.0625);
    vec3 sum = vec3(0.0);
    float weights = 0.0;
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
            if ((other.y & 0xFFFFu) == 0u || (other.y >> 16) != (g.y >> 16))
                continue;
            vec3 rgb = texelFetch(diffuse_source, q, 0).rgb;
            float w = kernel[abs(x)] * kernel[abs(y)];
            if (x != 0 || y != 0)
            {
                vec3 pq = world_point(q, unpackHalf2x16(other.y).x);
                vec4 on = unpackSnorm4x8(other.x);
                w *= exp(-((1.0 - dot(shaded, unfold(on.xy))) / pass.tuning.w + abs(dot(n, pq - p)) / sigma_z +
                    abs(luminance(rgb) - lp) / sigma_l));
            }
            sum += w * rgb;
            weights += w;
        }
    result = vec4(sum / weights - cmax, 0.0);
}
#endif
