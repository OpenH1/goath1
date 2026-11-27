// The radiance cache's cells (~/dev/rt2-realtime-design.md section 1.3; xgpu_vulkan_lt_indirect.c): a world-space grid
// of our own about the eye, keyed by position, the normal's octant and an LOD by distance: CASCADES cascades of
// CELLS^3 cells, cascade k's cells CACHE_BASE x 2^k across, the eye's window of each (CELLS/2 cells either side of
// its cell) addressed by wrapping (the cell's index mod CELLS), so a moving eye needs no copy; each slot keeps a
// 4-bit check of its cell's absolute place, so a slot the window wrapped onto another cell reads as empty. A point's
// cascade is the least whose window holds it (cells about 6% of the distance: the later bounces are smooth). A
// slot is a texel of a 2D image, CELLS^2 wide: x + CELLS y across, z + CELLS (octant + 8 cascade) down.
// Needs pass.camera.

#define CELLS 32
#define CASCADES 6
#define CACHE_BASE 0.1

uint cache_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float cascade_size(int k)
{
    return CACHE_BASE * exp2(float(k));
}

// a point's cascade (-1: beyond the last's window)
int cascade_of(vec3 p)
{
    vec3 d = abs(p - pass.camera[0].xyz);
    float far = max(d.x, max(d.y, d.z));
    int k = int(max(ceil(log2(max(far / (float(CELLS / 2 - 1) * CACHE_BASE), 1.0e-6))), 0.0));
    return k < CASCADES ? k : -1;
}

// the check of a cell (its absolute index in its cascade), 1 to 15
uint cell_check(ivec3 c, int k)
{
    return 1u + cache_hash(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ uint(k)) % 15u;
}

uint octant_of(vec3 n)
{
    return (n.x >= 0.0 ? 1u : 0u) | (n.y >= 0.0 ? 2u : 0u) | (n.z >= 0.0 ? 4u : 0u);
}

ivec2 slot_texel(ivec3 c, uint octant, int k)
{
    ivec3 w = c & (CELLS - 1);
    return ivec2(w.x + CELLS * w.y, w.z + CELLS * (int(octant) + 8 * k));
}

// the bounce's 2x2 block's pixels in turn: the one traced at a frame's phase (0 to 3)
ivec2 block_pixel(int phase)
{
    const ivec2 order[4] = ivec2[4](ivec2(0, 0), ivec2(1, 1), ivec2(1, 0), ivec2(0, 1));
    return order[phase & 3];
}
