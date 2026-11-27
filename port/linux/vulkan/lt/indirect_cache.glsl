// The radiance cache's keys (~/dev/rt2-realtime-design.md section 1.3; xgpu_vulkan_lt_indirect.c): a world-space hash
// grid of our own, its cell a point's by its position, its normal's octant and an LOD by its distance from the eye (a
// cell about 1% of the distance: some 8 pixels across at 1080p), hashed into the cache's slots (a texel each) with a
// 4-bit check beside. Shared by the cache's scatter (lt/indirect_cache.vert) and its lookups (lt/indirect.glsl).
// Needs pass.camera, pass.indirect.

// the least cell (world units: 15 cm) and the cell's share of the distance
#define CACHE_BASE 0.05
#define CACHE_SCALE 0.01

uint cache_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// x the slot, y the check (1 to 15)
uvec2 cache_key(vec3 p, vec3 n)
{
    float distance = length(p - pass.camera[0].xyz);
    float lod = clamp(ceil(log2(max(distance * CACHE_SCALE / CACHE_BASE, 1.0))), 0.0, 15.0);
    ivec3 c = ivec3(floor(p / (CACHE_BASE * exp2(lod))));
    uint octant = (n.x >= 0.0 ? 1u : 0u) | (n.y >= 0.0 ? 2u : 0u) | (n.z >= 0.0 ? 4u : 0u);
    uint h = cache_hash(uint(c.x) + cache_hash(uint(c.y) + cache_hash(uint(c.z) + cache_hash(uint(lod) | octant << 4))));
    uint slots = uint(pass.indirect[0].z) * uint(pass.indirect[0].w);
    return uvec2(h % slots, 1u + cache_hash(h ^ 0x5bd1e995u) % 15u);
}

ivec2 cache_texel(uint slot)
{
    uint width = uint(pass.indirect[0].z);
    return ivec2(int(slot % width), int(slot / width));
}

// the bounce's 2x2 block's pixels in turn: the one traced at a frame's phase (0 to 3)
ivec2 block_pixel(int phase)
{
    const ivec2 order[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return order[phase & 3];
}
