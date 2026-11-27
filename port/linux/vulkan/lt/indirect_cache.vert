// The radiance cache's scatter (xgpu_vulkan_lt_indirect.c; ~/dev/rt2-realtime-design.md section 1.3): a point a
// bounce sample (lt/indirect.glsl's BOUNCE: a texel a 2x2 block), a quarter of them a frame (indirect[2].x the
// quarter), drawn at its cell's slot of the cache's sums (additive blending, cleared each frame): its irradiance and
// 1, or (indirect[2].y 1) its cell's check, the check's square and its light-id hash, and 1, for the resolve's test
// that the slot's samples were all of one cell and one set of lights. A texel or pixel with nothing is put off the
// target.
#version 460
#extension GL_GOOGLE_include_directive : require

#define INPUT_PRESENT 0x400u

// the draw set's uniforms (lt/e1.frag's: struct xgpu_world_ao_pass, and the indirect's at 1536)
layout(set = 0, binding = 1, std140) uniform Pass
{
    vec4 camera[4];
    ivec4 corners;
    vec4 cascades[4];
    vec4 previous[4];
    ivec4 previous_corners;
    vec4 history;
    layout(offset = 1536) vec4 indirect[4];
} pass;

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform sampler2D samples;

layout(location = 0) out vec4 value;

#include "indirect_cache.glsl"

vec3 unfold(vec2 e)
{
    vec2 a = abs(e);
    float z = 1.0 - a.x - a.y;
    float t = max(-z, 0.0);
    vec2 f = e + vec2(e.x >= 0.0 ? -t : t, e.y >= 0.0 ? -t : t);
    return normalize(vec3(f, z));
}

void main()
{
    ivec2 size = ivec2(pass.indirect[0].xy);
    int across = (size.x + 1) / 2, quarter = int(pass.indirect[2].x);
    ivec2 block = ivec2(gl_VertexIndex % across, gl_VertexIndex / across) * 2 + ivec2(quarter & 1, quarter >> 1);
    gl_PointSize = 1.0;
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
    value = vec4(0.0);
    if (any(greaterThanEqual(block, size)))
        return;
    vec4 s = texelFetch(samples, block, 0);
    if (s.a < 0.5 || any(isnan(s.rgb)) || any(isinf(s.rgb)))
        return;
    float code = s.a - 1.0;
    int which = int(floor(code / 1024.0));
    float light = code - 1024.0 * float(which);
    ivec2 pixel = block * 2 + block_pixel(int(pass.indirect[1].y) + which);
    uvec4 words = texelFetch(inputs, pixel, 0);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec2 low = vec2(pass.corners.xy), extent = vec2(pass.corners.zw) - low;
    vec2 screen = (vec2(pixel) + 0.5 - low) * 2.0 / extent - 1.0;
    vec3 ray = normalize(pass.camera[1].xyz + pass.camera[2].xyz * screen.x + pass.camera[3].xyz * screen.y);
    vec3 p = pass.camera[0].xyz + ray * uintBitsToFloat(words.z), n = unfold(unpackSnorm2x16(words.x));
    uvec2 key = cache_key(p, n);
    vec2 slots = pass.indirect[0].zw;
    gl_Position = vec4((vec2(cache_texel(key.x)) + 0.5) / slots * 2.0 - 1.0, 0.0, 1.0);
    float check = float(key.y);
    value = pass.indirect[2].y < 0.5 ? vec4(s.rgb, 1.0) : vec4(check, check * check, light, 1.0);
}
