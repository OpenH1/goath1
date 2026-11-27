// The A-SVGF gradient samples' pixels (xgpu_vulkan_denoise.c): one pixel of each 3x3 stratum a frame, chosen by a
// hash of the stratum and the frame (the passes' history.y). E1's re-shade (e1.frag's GRADIENT) shades these, a texel
// a stratum, at the previous frame's pixels with the previous frame's number; the denoiser's gradient pass reads them.
bool lt_gradient_pixel(ivec2 pixel, float frame)
{
    uvec2 s = uvec2(pixel) / 3u;
    uint h = s.x * 0x8da6b343u ^ s.y * 0xd8163841u ^ uint(frame) * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    uint k = h % 9u;
    return uvec2(pixel) % 3u == uvec2(k % 3u, k / 3u);
}

// the gradient pixel of a 3x3 stratum
ivec2 lt_gradient_pixel_in(ivec2 stratum, float frame)
{
    for (int k = 0; k < 8; k++)
        if (lt_gradient_pixel(stratum * 3 + ivec2(k % 3, k / 3), frame))
            return stratum * 3 + ivec2(k % 3, k / 3);
    return stratum * 3 + ivec2(2, 2);
}
