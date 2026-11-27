// W3's filter guide (xgpu_vulkan_denoise.c): what the a-trous passes read of each pixel, once, in one 8-byte texel
// (RG32Uint): x the shaded normal (the guides'; the geometric one where they have none) and the geometric one,
// octahedral, packSnorm4x8 (about half a degree); y the distance as a half float (0 where the pre-pass drew nothing)
// and the owner's low 16 bits above it.
// FOLD (built again as denoise_guide_fold.frag): the one diffuse signal too, the second output: the direct light's
// 1-spp sample plus the dynamic lights' bounce (half resolution, lt/indirect.glsl's BOUNCE_HISTORY: a texel a 2x2
// block, the mean of its samples over the last frames; its traced pixel this frame's phase's) upsampled on the
// pixel's surface, so that the denoiser's temporal stage counts both (~/dev/rt2-realtime-design.md section 1.5): the
// 3x3 blocks about the pixel's own (the history is smooth already), each kept where its traced pixel is of the same
// owner, its normal within 25 degrees and its point on this pixel's plane, Gaussian by the pixel distance (3 pixels);
// where none of them is, the 5x5's outer ring.
#version 460
#extension GL_GOOGLE_include_directive : require

#include "denoise_common.glsl"

layout(set = 1, binding = 0) uniform usampler2D inputs;
layout(set = 1, binding = 1) uniform usampler2D guides;
#ifdef FOLD
layout(set = 1, binding = 2) uniform sampler2D direct;
layout(set = 1, binding = 3) uniform sampler2D bounce;
#endif

layout(location = 0) out uvec2 result;
#ifdef FOLD
layout(location = 1) out vec4 combined;

// (lt/indirect_cache.glsl's block_pixel: the 2x2 block's pixel traced at a phase)
ivec2 block_pixel(int phase)
{
    const ivec2 order[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return order[phase & 3];
}

// one block's sample, weighted, where its traced pixel is on this pixel's surface
void tap(ivec2 block, ivec2 pixel, vec3 p, vec3 n, uint owner, float distance, inout vec3 sum, inout float weights)
{
    if (any(lessThan(block, ivec2(0))) || any(greaterThanEqual(block, ivec2(pass.bounce.xy))))
        return;
    vec4 s = texelFetch(bounce, block, 0);
    if (s.a < 0.5)
        return;
    // (the history's alpha: the sample's code + 8192 x its count)
    float code = s.a - 8192.0 * floor(s.a / 8192.0);
    int which = int(floor((code - 1.0) / 1024.0));
    ivec2 at = block * 2 + block_pixel(int(pass.bounce.z) + which);
    uvec4 words = texelFetch(inputs, at, 0);
    if ((words.y & INPUT_PRESENT) == 0u || words.w != owner || dot(unfold(unpackSnorm2x16(words.x)), n) < COS_25)
        return;
    if (abs(dot(world_point(at, uintBitsToFloat(words.z)) - p, n)) > 0.02 * distance)
        return;
    vec2 off = vec2(at - pixel);
    float w = exp(-dot(off, off) * (1.0 / 18.0));
    sum += w * s.rgb;
    weights += w;
}
#endif

void main()
{
    ivec2 pixel = ivec2(gl_FragCoord.xy);
    uvec4 words = texelFetch(inputs, pixel, 0);
    result = uvec2(0u);
#ifdef FOLD
    combined = vec4(0.0);
#endif
    if ((words.y & INPUT_PRESENT) == 0u)
        return;
    vec2 geometric = unpackSnorm2x16(words.x), shaded = unpackSnorm2x16(texelFetch(guides, pixel, 0).x);
    vec3 n = unfold(geometric), s = unfold(shaded);
    if (dot(s, s) < 0.5 || dot(s, n) < 0.0)
        shaded = geometric;
    result = uvec2(packSnorm4x8(vec4(shaded, geometric)),
        (packHalf2x16(vec2(max(uintBitsToFloat(words.z), 1.0e-3), 0.0)) & 0xFFFFu) | (words.w << 16));
#ifdef FOLD
    vec4 light = texelFetch(direct, pixel, 0);
    vec3 p = world_point(pixel, uintBitsToFloat(words.z)), sum = vec3(0.0);
    float distance = length(p - pass.camera[0].xyz), weights = 0.0;
    ivec2 middle = pixel / 2;
    for (int k = 0; k < 9; k++)
        tap(middle + ivec2(k % 3 - 1, k / 3 - 1), pixel, p, n, words.w, distance, sum, weights);
    if (weights <= 0.0)
        for (int k = 0; k < 16; k++)
        {
            // (the ring: its 16 blocks, the rows above and below, then the columns' middle three)
            ivec2 off = k < 10 ? ivec2(k % 5 - 2, k < 5 ? -2 : 2) : ivec2(k < 13 ? -2 : 2, (k - 10) % 3 - 1);
            tap(middle + off, pixel, p, n, words.w, distance, sum, weights);
        }
    vec3 e = weights > 0.0 ? sum / weights : vec3(0.0);
    // (bounce.w's 2, HALO_LT_INDIRECT=only, checking: the bounce alone)
    combined = vec4((int(pass.bounce.w) & 2) != 0 ? e : light.rgb + e, light.a);
#endif
}
