// W3's filter guide (xgpu_vulkan_denoise.c): what the a-trous passes read of each pixel, once, in one 8-byte texel
// (RG32Uint): x the shaded normal (the guides'; the geometric one where they have none) and the geometric one,
// octahedral, packSnorm4x8 (about half a degree); y the distance as a half float (0 where the pre-pass drew nothing)
// and the owner's low 16 bits above it.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform usampler2D guides;

layout(location = 0) out uvec2 result;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = uvec2(0u);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec2 geometric = unpackSnorm2x16(words.x), shaded = unpackSnorm2x16(texelFetch(guides, pixel, 0).x);
    vec3 n = unfold(geometric), s = unfold(shaded);
    if (dot(s, s) < 0.5 || dot(s, n) < 0.0)
        shaded = geometric;
    result = uvec2(packSnorm4x8(vec4(shaded, geometric)),
        (packHalf2x16(vec2(max(uintBitsToFloat(words.z), 1.0e-3), 0.0)) & 0xFFFFu) | (words.w << 16));
}
