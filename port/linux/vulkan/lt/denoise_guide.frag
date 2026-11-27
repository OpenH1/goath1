// W3's filter guide (xgpu_vulkan_denoise.c): what the a-trous passes read of each pixel, once, in one texel (RGBA32Uint):
// x the shaded normal (the guides'; the geometric one where they have none), y the geometric one (both octahedral,
// packSnorm2x16), z the distance (0 where the pre-pass drew nothing), w the owner.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform usampler2D guides;

layout(location = 0) out uvec4 result;

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = uvec4(0u);
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec3 n = unfold(unpackSnorm2x16(words.x));
    vec3 s = unfold(unpackSnorm2x16(texelFetch(guides, pixel, 0).x));
    uint shaded = dot(s, s) < 0.5 || dot(s, n) < 0.0 ? words.x : texelFetch(guides, pixel, 0).x;
    result = uvec4(shaded, words.x, max(words.z, 1u), words.w);
}
