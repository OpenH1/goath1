#!/usr/bin/env python3
"""Makes sharper blood as mod bitmaps (docs/mods.md, Bitmaps): the Xbox
maps' blood decal and particle sheets redrawn at four times their size in
32-bit color, the same sprites in the same places.

    python tools/hd_blood.py --map maps/b40.map --out mods/hd_blood_test

The sheets (BLOOD, the "splat" recipe in port/assets/effects/effects.json):
- the decals, effects\\decals\\blood splats\\bitmaps\\: 128 x 128 DXT1 sheets
  of 2 to 4 painted splats or smears, about 60 x 60 texels each. Their
  alpha is only each splat's box (or nothing: the whole sheet opaque); the
  splat is in the color, over white (the multiply decals: human, Elite,
  Hunter) or black (the max and add ones: Grunt, Engineer, the glows).
- the particles, effects\\particles\\solid\\bitmaps\\: 128 x 128 DXT3 sheets
  (64 x 64 for the Engineer's three pages) of 1 to 6 bursts, drawn alpha
  blended. Their shape is in the alpha (four bits), their color flat but
  for a few darker lines.

The redraw is the map's own pixels, not new art. Each sprite's cell (its
box in the page) is redrawn on its own, its edges clamped, so no cell
bleeds into the next; the page around the cells is redrawn whole, the same
way, for the texels between them. Each plane (red, green, blue, and the
particles' alpha):
1. upscaled (bicubic, Catmull-Rom, held within each texel's 3 x 3
   neighbors' range so that it rings nowhere);
2. its DXT blocks and banding smoothed: where it is within about one of the
   format's steps of its neighborhood's smooth value, that value; where it
   differs more (the paint's own detail and edges), itself; no texel moved
   further than the format's rounding could have;
3. its edges made crisp: each texel is pushed toward the least or the most
   of its 9 x 9 neighborhood (one map texel each way), whichever it is
   nearer, as far as the neighborhood's contrast says it is an edge (a
   soft shock filter). A painted droplet's outline, a ramp two map texels
   wide upscaled, becomes one about half a texel wide; the flat paint and
   its soft gradients are left as they are.
Then each cell is given back the map's color (its mean, each channel, so
the tint and how much a multiply decal darkens stay the map's). The
decals' alpha is the map's, each texel 4 x 4 (their boxes as crisp as
they are); the particles' is redrawn as the color is, and where it is 0
their color is its neighbors' (weighted by their alpha), so that no dark
fringe is filtered in. The mipmaps go down to the map's smallest level, a
decal's alpha held 0 or 1 at each.

The game makes the same redraw itself from the player's maps (display.
hd_effects: port/linux/src/hd_effects.c, the same math in C, on 32-bit
floats where this uses NumPy's 64). This script is its reference.

Needs NumPy and Pillow (for --pngs).
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hud_assets  # noqa: E402  (the Xbox map reader and DXT decoder)
import hd_muzzle_flash  # noqa: E402  (the tag file writer)

DECALS = "effects\\decals\\blood splats\\bitmaps\\"
PARTICLES = "effects\\particles\\solid\\bitmaps\\"
BLOOD = [DECALS + name for name in (
    "blood splat", "blood splat elite", "blood splat engineer", "blood splat grunt", "blood splat hunter",
    "blood splat hunter glow", "glowingdrip splat", "blood smear elite lg", "blood smear elite med",
    "blood smear elite sm", "blood smear grunt lg", "blood smear grunt med", "blood smear grunt sm")] + [
    PARTICLES + name for name in (
        "blood burst", "blood generic burst", "blood h impact", "blood elite impact burst", "blood engineer")]
SCALE = 4
# DXT1 and DXT3
FORMAT_DXT1, FORMAT_DXT3 = 14, 15

# the deblocking: one step of the format's color (five bits) and of DXT3's
# alpha (four), and the neighborhood's blur (map texels)
COLOR_STEP = 1 / 31
ALPHA_STEP = 1 / 15
DEBLOCK_SIGMA = 1.0
# the edges: the neighborhood's reach (map texels each way), the contrast
# from which it is an edge, fully at the second, and how far toward the
# least or most a texel goes
EDGE_REACH = 1
EDGE_LOW, EDGE_HIGH = 0.06, 0.2
EDGE_STRENGTH = 0.75
# the particles' color where their alpha is 0: their neighbors' (map texels)
BLEED_SIGMA = 2.0


def smoothstep(x, low, high):
    t = np.clip((x - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def clamp_index(index, count):
    return np.clip(index, 0, count - 1)


def blur(plane, sigma):
    """A Gaussian blur (separable, cut at 4 sigma, the edges clamped)."""
    radius = int(4 * sigma + 0.5)
    taps = np.arange(-radius, radius + 1)
    kernel = np.exp(-0.5 * taps * taps / (sigma * sigma))
    kernel /= kernel.sum()
    height, width = plane.shape
    rows = sum(kernel[k] * plane[:, clamp_index(np.arange(width) + taps[k], width)] for k in range(len(taps)))
    return sum(kernel[k] * rows[clamp_index(np.arange(height) + taps[k], height), :] for k in range(len(taps)))


def extreme(plane, radius, most):
    """The least (or most) of each texel's (2 radius + 1)-square neighborhood, the edges clamped."""
    pick = np.maximum if most else np.minimum
    height, width = plane.shape
    rows = plane
    for k in range(-radius, radius + 1):
        rows = pick(rows, plane[:, clamp_index(np.arange(width) + k, width)])
    out = rows
    for k in range(-radius, radius + 1):
        out = pick(out, rows[clamp_index(np.arange(height) + k, height), :])
    return out


def cubic_weights(scale):
    """Catmull-Rom's four weights for each of the `scale` output texels a
    map texel i becomes, on texels i - 2 + phase's first tap: for output
    texel o, its center in map texels is (o + 0.5) / scale - 0.5."""
    weights = []
    for phase in range(scale):
        position = (phase + 0.5) / scale - 0.5
        first = int(np.floor(position)) - 1
        f = position - np.floor(position)
        w = [((-0.5 * f + 1.0) * f - 0.5) * f, (1.5 * f - 2.5) * f * f + 1.0,
             ((-1.5 * f + 2.0) * f + 0.5) * f, (0.5 * f - 0.5) * f * f]
        weights.append((first, w))
    return weights


def upscale(plane):
    """Bicubic at SCALE (Catmull-Rom, the edges clamped), held within each
    map texel's 3 x 3 neighbors' range."""
    height, width = plane.shape
    weights = cubic_weights(SCALE)

    def along(image, axis):
        count = image.shape[axis]
        parts = []
        for first, w in weights:
            part = sum(w[k] * np.take(image, clamp_index(np.arange(count) + first + k, count), axis=axis)
                       for k in range(4))
            parts.append(part)
        # interleave the phases: texel i's phase p at i * SCALE + p
        stacked = np.stack(parts, axis + 1)
        shape = list(image.shape)
        shape[axis] *= SCALE
        return stacked.reshape(shape)

    out = along(along(plane, 1), 0)
    low = extreme(plane, 1, False).repeat(SCALE, 0).repeat(SCALE, 1)
    high = extreme(plane, 1, True).repeat(SCALE, 0).repeat(SCALE, 1)
    return np.clip(out, low, high)


def deblock(plane, step):
    """The DXT blocks and banding smoothed (within about a step of the
    neighborhood: its smooth value), the detail beyond that kept; never
    further than the format's rounding could have moved it."""
    smooth = blur(plane, DEBLOCK_SIGMA * SCALE)
    keep = smoothstep(np.abs(plane - smooth) / step, 1.0, 2.5)
    out = smooth * (1 - keep) + plane * keep
    return np.clip(out, plane - step, plane + step)


def crisp(plane):
    """The edges pushed toward their neighborhood's least or most: a soft
    shock filter, as far as the contrast says it is an edge."""
    radius = EDGE_REACH * SCALE
    low, high = extreme(plane, radius, False), extreme(plane, radius, True)
    contrast = high - low
    t = (plane - low) / np.maximum(contrast, 1e-6)
    target = low + contrast * smoothstep(t, 0.25, 0.75)
    edge = smoothstep(contrast, EDGE_LOW, EDGE_HIGH) * EDGE_STRENGTH
    return plane + (target - plane) * edge


def bleed(rgb, alpha):
    """The color where the alpha is 0: the neighbors' color, weighted by
    their alpha (the map's where none has any)."""
    weight = blur(alpha, BLEED_SIGMA)
    out = rgb.copy()
    hidden = (alpha <= 0) & (weight > 1e-4)
    for c in range(3):
        filled = blur(rgb[..., c] * alpha, BLEED_SIGMA) / np.maximum(weight, 1e-4)
        out[..., c] = np.where(hidden, filled, rgb[..., c])
    return out


def redraw_cell(rgb, alpha, decal):
    """A cell (or the page) at SCALE: its color and alpha, 0 to 1."""
    if not decal:
        rgb = bleed(rgb, alpha)
    planes = [crisp(deblock(upscale(rgb[..., c]), COLOR_STEP)) for c in range(3)]
    if decal:
        out_alpha = alpha.repeat(SCALE, 0).repeat(SCALE, 1)
    else:
        out_alpha = crisp(deblock(upscale(alpha), ALPHA_STEP))
    out = np.clip(np.stack(planes, -1), 0, 1)
    out_alpha = np.clip(out_alpha, 0, 1)
    # the map's color: each channel's mean (weighted by the alpha, a particle's)
    weight_in = np.ones_like(alpha) if decal else alpha
    weight_out = np.ones_like(out_alpha) if decal else out_alpha
    if weight_in.sum() > 0 and weight_out.sum() > 0:
        for c in range(3):
            stock = (rgb[..., c] * weight_in).sum() / weight_in.sum()
            made = (out[..., c] * weight_out).sum() / weight_out.sum()
            # (moved toward white or black, whichever the mean is off from, so
            # that a channel at 1 or 0 stays there)
            if made < stock:
                out[..., c] = 1 - (1 - out[..., c]) * (1 - stock) / max(1 - made, 1e-6)
            elif made > stock:
                out[..., c] = out[..., c] * stock / made
    return out, out_alpha


def sprite_cells(group, page, width, height):
    """Each of the page's sprites' boxes, in texels (left, top, right, bottom)."""
    cells = []
    for sequence in group["sequences"]:
        for bitmap, left, top, right, bottom in sequence:
            if bitmap != page:
                continue
            cell = (int(np.floor(left * width + 1e-3)), int(np.floor(top * height + 1e-3)),
                    int(np.ceil(right * width - 1e-3)), int(np.ceil(bottom * height - 1e-3)))
            cell = (max(cell[0], 0), max(cell[1], 0), min(cell[2], width), min(cell[3], height))
            if cell[2] > cell[0] and cell[3] > cell[1] and cell not in cells:
                cells.append(cell)
    return cells


def redraw_page(texels, cells, decal):
    """A page (RGBA bytes) at SCALE: the page whole, then each cell on its own over it."""
    rgb, alpha = texels[..., :3] / 255.0, texels[..., 3] / 255.0
    out, out_alpha = redraw_cell(rgb, alpha, decal)
    for left, top, right, bottom in cells:
        cell_rgb, cell_alpha = redraw_cell(rgb[top:bottom, left:right], alpha[top:bottom, left:right], decal)
        out[top * SCALE:bottom * SCALE, left * SCALE:right * SCALE] = cell_rgb
        out_alpha[top * SCALE:bottom * SCALE, left * SCALE:right * SCALE] = cell_alpha
    return out, out_alpha


def mipmaps(image, alpha, smallest, decal):
    levels = [np.concatenate([image, alpha[..., None]], -1)]
    while levels[-1].shape[0] > smallest:
        last = levels[-1]
        level = last.reshape(last.shape[0] // 2, 2, last.shape[1] // 2, 2, 4).mean((1, 3))
        levels.append(level)
    out = []
    for level in levels:
        level = level.copy()
        if decal:
            level[..., 3] = level[..., 3] >= 0.5
        out.append(np.round(np.clip(level, 0, 1) * 255).astype(np.uint8)[..., [2, 1, 0, 3]])
    return out


def smallest_level(bitmap):
    """The map's smallest mip level's side (its pixels' size says how many it has)."""
    side, size, block = bitmap["width"], 0, 8 if bitmap["format"] == FORMAT_DXT1 else 16
    total = len(bitmap["pixels"])
    while True:
        size += max(1, side // 4) ** 2 * block
        if size >= total or side <= 4:
            return side
        side //= 2


def redraw(xbox, name, pngs=None):
    group = xbox.bitmap_group(name)
    pages = []
    for index, bitmap in enumerate(group["bitmaps"]):
        width, height = bitmap["width"], bitmap["height"]
        # (a DXT1 page is a decal's: its alpha the map's; a DXT3 one a particle's)
        decal = bitmap["format"] == FORMAT_DXT1
        kind = "dxt1" if decal else "dxt3"
        texels = hud_assets.decode_dxt(kind, bitmap["pixels"], width, height).astype(np.float64)
        cells = sprite_cells(group, index, width, height)
        image, alpha = redraw_page(texels, cells, decal)
        pages.append(mipmaps(image, alpha, smallest_level(bitmap), decal))
        report(name, index, texels, image, alpha, cells, decal)
        if pngs:
            preview(pngs, name, index, texels, image, alpha, decal)
    return pages


def report(name, index, texels, image, alpha, cells, decal):
    """Each cell's mean color, the map's and the redraw's (0 to 255)."""
    worst = 0.0
    for left, top, right, bottom in cells:
        stock = texels[top:bottom, left:right]
        made = np.concatenate([image, alpha[..., None]], -1)[top * SCALE:bottom * SCALE, left * SCALE:right * SCALE]
        a = stock[..., 3:] / 255 if not decal else np.ones_like(stock[..., 3:])
        b = made[..., 3:] if not decal else np.ones_like(made[..., 3:])
        mean_stock = (stock[..., :3] * a).sum((0, 1)) / max(a.sum(), 1e-9)
        mean_made = (made[..., :3] * 255 * b).sum((0, 1)) / max(b.sum(), 1e-9)
        worst = max(worst, np.abs(mean_stock - mean_made).max())
        if not decal:
            worst = max(worst, abs(stock[..., 3].mean() - made[..., 3].mean() * 255))
    print(f"  {name.split(chr(92))[-1]} page {index}: {len(cells)} cells, worst cell mean off by {worst:.2f} / 255")


def preview(folder, name, index, texels, image, alpha, decal):
    """The map's page (bilinear at SCALE, as a close look draws it) beside the redraw, over a gray wall."""
    from PIL import Image
    folder.mkdir(parents=True, exist_ok=True)
    height, width = texels.shape[:2]
    stock = np.stack([np.asarray(Image.fromarray(texels[..., c].astype(np.float32), "F").resize(
        (width * SCALE, height * SCALE), Image.BILINEAR)) for c in range(4)], -1) / 255
    made = np.concatenate([image, alpha[..., None]], -1)
    dark = decal and not name.endswith("glowingdrip splat") and texels[..., :3].mean() < 128
    wall = (0.08 if dark else 0.55) + 0.1 * ((np.indices(made.shape[:2]).sum(0) // 32) % 2)[..., None]
    shown = []
    for page in (stock, made):
        rgb, a = page[..., :3], page[..., 3:]
        if not decal:
            shown.append(rgb * a + wall * (1 - a))
        elif name.endswith(("glowingdrip splat",)):
            shown.append(np.clip(wall + rgb * a, 0, 1))
        elif rgb.mean() > 0.5:
            shown.append(wall * rgb)
        else:
            shown.append(np.maximum(wall, rgb * a))
    short = name.split("\\")[-1].replace(" ", "_")
    Image.fromarray(np.round(np.concatenate(shown, 1) * 255).astype(np.uint8)).save(folder / f"{short}_{index}.png")


def write_mod(xbox, name, pages, out):
    """The redraw as a mod's bitmap tag (hd_muzzle_flash.py's writer)."""
    hd_muzzle_flash.NAME = name
    tag = out / "tags" / (name.replace("\\", "/") + ".bitmap")
    tag.parent.mkdir(parents=True, exist_ok=True)
    tag.write_bytes(hd_muzzle_flash.tag_file(xbox, pages))
    return tag


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--map", type=Path, default=Path("maps/b40.map"))
    parser.add_argument("--out", type=Path, help="the mod's folder (its mod.toml and tags)")
    parser.add_argument("--only", help="only the sheet whose name ends so")
    parser.add_argument("--pngs", type=Path, help="also write each page, the map's beside the redraw, as PNGs here")
    parser.add_argument("--crc", action="store_true",
                        help="print each sheet's CRC-32 of its pixels (port/assets/effects/effects.json) and stop")
    arguments = parser.parse_args()

    xbox = hud_assets.XboxMap(arguments.map)
    names = [name for name in BLOOD if ("bitm", name) in xbox.tags and
             (not arguments.only or name.endswith(arguments.only))]
    if arguments.crc:
        for name in names:
            crc = 0
            for bitmap in xbox.bitmap_group(name)["bitmaps"]:
                crc = zlib.crc32(bitmap["pixels"], crc)
            print(crc, smallest_level(xbox.bitmap_group(name)["bitmaps"][0]), name)
        return
    for name in names:
        pages = redraw(xbox, name, arguments.pngs)
        if arguments.out:
            tag = write_mod(xbox, name, pages, arguments.out)
            print(f"wrote {tag} ({tag.stat().st_size} bytes)")
    if arguments.out:
        (arguments.out / "mod.toml").write_text(
            'name = "HD blood (test)"\nid = "hd_blood_test"\nversion = "0.1"\n'
            'description = "The blood decals and particles at 4x, in 32-bit color"\nformat = 1\n')


if __name__ == "__main__":
    main()
