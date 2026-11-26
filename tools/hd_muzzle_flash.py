#!/usr/bin/env python3
"""Makes a sharper Assault Rifle muzzle flash as a mod bitmap (docs/mods.md,
Bitmaps): the Xbox map's effects\\particles\\flash\\bitmaps\\flash h ar fp
redrawn at four times its size in 32-bit color, the same sprites in the
same places.

    python tools/hd_muzzle_flash.py --map assets/maps/bloodgulch.map --out mods/hd_muzzle_test

The map's bitmap is two 256 x 256 pages of A1R5G5B5 sprites: the six side
streaks of the particle "flash h ar" (sequences 0 to 5) and the three
bursts of "flash h ar center" (6 to 8), both drawn additively (one, one), so
their color is all that is seen and black is nothing. Their alpha is only
each sprite's box. Five bits a channel band the dim glow into rings, and
the bursts' edges are stair-stepped.

The redraw is the map's own pixels, not new art: each page is upscaled
(bicubic), its banding smoothed away where neighbors differ by about one of
the map's color steps (a step larger than that is an edge and kept), the
bright cores sharpened and the glow and outlines softened, then each sprite
scaled back to the map's light (its summed color) so that it reads as the
same flash. The alpha is each sprite's box again, with soft edges. The mod's
mipmaps go down to the map's smallest (64 x 64).

The game makes the same redraw itself from the player's maps, built in and
on by default (display.hd_effects: port/linux/src/hd_effects.c, the same
math in C; port/assets/effects/effects.json lists it). This script is its
reference, and makes it as a mod to compare with.

Needs NumPy, SciPy and Pillow.
"""

import argparse
import struct
import sys
import zlib
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hud_assets  # noqa: E402  (the Xbox map reader)

NAME = "effects\\particles\\flash\\bitmaps\\flash h ar fp"
SCALE = 4
# the mod's smallest mipmap, as the map's
SMALLEST = 64
# one of the map's color steps (five bits a channel)
STEP = 1 / 31

# bitmap_group.h's values
BITMAP_GROUP_FORMAT_32_BIT = 4
BITMAP_TYPE_2D = 0
BITMAP_FORMAT_A8R8G8B8 = 11
BITMAP_POWER_OF_TWO = 1


def smoothstep(x, low, high):
    t = np.clip((x - low) / (high - low), 0, 1)
    return t * t * (3 - 2 * t)


def blur(image, sigma):
    return ndimage.gaussian_filter(image, (sigma, sigma, 0)) if image.ndim == 3 else ndimage.gaussian_filter(image, sigma)


def decode(bitmap: dict) -> tuple:
    """A page's color (0 to 1) and its 1-bit alpha."""
    raw = hud_assets.stored_texels(bitmap)
    value = raw[..., 0].astype(np.uint32) | (raw[..., 1].astype(np.uint32) << 8)
    color = np.stack([(value >> shift) & 31 for shift in (10, 5, 0)], -1) / 31
    return color, (value >> 15).astype(np.float64)


def upscale(image):
    """Bicubic, held within each texel's 2 x 2 neighbors' colors so that it
    undershoots nowhere (a dark ring beside a bright edge)."""
    channels = image if image.ndim == 3 else image[..., None]
    height, width = channels.shape[:2]
    out = [np.asarray(Image.fromarray(channels[..., c].astype(np.float32), "F").resize(
        (width * SCALE, height * SCALE), Image.BICUBIC)) for c in range(channels.shape[2])]
    out = np.stack(out, -1).astype(np.float64)
    window = (3, 3, 1)
    low = ndimage.minimum_filter(channels, window).repeat(SCALE, 0).repeat(SCALE, 1)
    high = ndimage.maximum_filter(channels, window).repeat(SCALE, 0).repeat(SCALE, 1)
    out = np.clip(out, low, high)
    return out if image.ndim == 3 else out[..., 0]


def deband(image):
    """In the dim glow, where five bits show as rings: where the upscaled
    color is within about one color step of its neighborhood (a band's
    edge), the neighborhood's smooth color; where it differs more (the
    flash's own edges), the color as it is. Twice, the second wider, for the
    wide glow's rings. The bright parts keep their color's detail, and no
    texel moves further than the map's rounding could have (under one step),
    so the glow spreads nowhere it was not."""
    original = image
    for sigma in (1.5 * SCALE, 3.5 * SCALE):
        smooth = blur(image, sigma)
        difference = np.abs(image - smooth).max(-1, keepdims=True)
        keep = np.maximum(smoothstep(difference / STEP, 1.0, 2.5),
                          smoothstep(smooth.max(-1, keepdims=True), 0.25, 0.5))
        image = smooth * (1 - keep) + image * keep
    return np.clip(image, np.maximum(original - 0.75 * STEP, 0), original + 0.5 * STEP)


def redraw(color, alpha, boxes):
    """A page at SCALE times its size: color and alpha, 0 to 1."""
    up = deband(upscale(color))
    brightness = up.max(-1, keepdims=True)
    core = smoothstep(brightness, 0.5, 0.9)
    # the bright streaks crisp (an unsharp mask held within its
    # neighborhood's colors, so it rings nowhere), the glow and outlines soft
    window = (SCALE + 1, SCALE + 1, 1)
    sharp = np.clip(up + 0.7 * (up - blur(up, SCALE / 2)),
                    ndimage.minimum_filter(up, window), ndimage.maximum_filter(up, window))
    soft = blur(up, 0.6 * SCALE)
    image = np.clip(sharp * core + soft * (1 - core), 0, 1)
    # the alpha, each sprite's box with soft edges; nothing outside it
    mask = np.clip(blur(upscale(alpha), 0.5 * SCALE), 0, 1)
    image *= mask[..., None]
    # each sprite as bright as the map's (its summed color)
    for left, top, right, bottom in boxes:
        cell = (slice(top * SCALE, bottom * SCALE), slice(left * SCALE, right * SCALE))
        old = color[top:bottom, left:right].sum()
        new = image[cell].sum()
        print(f"  sprite {left},{top}: light x{old * SCALE * SCALE / max(new, 1e-9):.3f}")
        if new > 0:
            image[cell] = np.clip(image[cell] * old * SCALE * SCALE / new, 0, 1)
    return image, mask


def sprite_boxes(alpha):
    """Each sprite's box in the page (its alpha's rectangles), in texels."""
    labels, count = ndimage.label(alpha > 0)
    return [(s[1].start, s[0].start, s[1].stop, s[0].stop) for s in ndimage.find_objects(labels)]


def mipmaps(texels):
    """The page and its mipmaps (each a 2 x 2 box filter of the last) down
    to SMALLEST."""
    levels = [texels]
    while levels[-1].shape[0] > SMALLEST:
        last = levels[-1]
        levels.append(last.reshape(last.shape[0] // 2, 2, last.shape[1] // 2, 2, 4).mean((1, 3)))
    return levels


def bgra(image, mask):
    texels = np.concatenate([image, mask[..., None]], -1)
    return [np.round(np.clip(level, 0, 1) * 255).astype(np.uint8)[..., [2, 1, 0, 3]] for level in mipmaps(texels)]


def tag_header() -> bytes:
    """A tag file's 0x40-byte header: its group (bitm), its size and its group's version (7)."""
    data = bytearray(0x40)
    data[0x24:0x28] = b"bitm"
    struct.pack_into(">I", data, 0x2C, 0x40)
    struct.pack_into(">H", data, 0x38, 7)
    data[0x3C:0x40] = b"blam"
    return bytes(data)


def tag_file(xbox: hud_assets.XboxMap, pages: list) -> bytes:
    """A Halo PC bitmap tag (big-endian): the map's bitmap's values,
    sequences and sprites, with `pages` (each a list of mip levels, BGRA) as
    its 32-bit 2D bitmaps."""
    address = xbox.tags[("bitm", NAME)]
    root_in = xbox.read(address, 0x6C)
    pixels = b"".join(level.tobytes() for page in pages for level in page)
    root = bytearray(0x6C)
    struct.pack_into(">hhhH", root, 0, *struct.unpack_from("<hhhH", root_in, 0))
    struct.pack_into(">h", root, 2, BITMAP_GROUP_FORMAT_32_BIT)
    struct.pack_into(">fffhHhh", root, 0x08, *struct.unpack_from("<fffhHhh", root_in, 0x08))
    struct.pack_into(">I", root, 0x30, len(pixels))
    struct.pack_into(">ffhhh", root, 0x44, *struct.unpack_from("<ffhhh", root_in, 0x44))
    sequence_count, sequences = struct.unpack_from("<II", root_in, 0x54)
    struct.pack_into(">I", root, 0x54, sequence_count)
    struct.pack_into(">I", root, 0x60, len(pages))
    sequence_data, sprite_data = b"", b""
    for index in range(sequence_count):
        sequence_in = xbox.read(sequences + index * 0x40, 0x40)
        sprite_count, sprites = struct.unpack_from("<II", sequence_in, 0x34)
        sequence = bytearray(0x40)
        sequence[:32] = sequence_in[:32]
        struct.pack_into(">hh", sequence, 0x20, *struct.unpack_from("<hh", sequence_in, 0x20))
        struct.pack_into(">I", sequence, 0x34, sprite_count)
        sequence_data += bytes(sequence)
        for j in range(sprite_count):
            sprite_in = xbox.read(sprites + j * 0x20, 0x20)
            sprite = bytearray(0x20)
            struct.pack_into(">h", sprite, 0, *struct.unpack_from("<h", sprite_in, 0))
            struct.pack_into(">6f", sprite, 0x08, *struct.unpack_from("<6f", sprite_in, 0x08))
            sprite_data += bytes(sprite)
    bitmap_data, offset = b"", 0
    for page in pages:
        size = page[0].shape[0]
        bitmap = bytearray(0x30)
        bitmap[0:4] = b"bitm"
        struct.pack_into(">hhhhhH", bitmap, 4, size, size, 1, BITMAP_TYPE_2D, BITMAP_FORMAT_A8R8G8B8,
                         BITMAP_POWER_OF_TWO)
        struct.pack_into(">h", bitmap, 0x14, len(page) - 1)
        struct.pack_into(">i", bitmap, 0x18, offset)
        offset += sum(level.nbytes for level in page)
        bitmap_data += bytes(bitmap)
    return tag_header() + bytes(root) + pixels + sequence_data + sprite_data + bitmap_data


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--map", type=Path, default=Path("assets/maps/bloodgulch.map"))
    parser.add_argument("--out", type=Path, help="the mod's folder (its mod.toml and tags)")
    parser.add_argument("--pngs", type=Path, help="also write each page, the map's and the redraw, as PNGs here")
    parser.add_argument("--crc", action="store_true",
                        help="print the CRC-32 of the map's bitmap's pixels (port/assets/effects/effects.json) and stop")
    arguments = parser.parse_args()

    xbox = hud_assets.XboxMap(arguments.map)
    if arguments.crc:
        crc = 0
        for bitmap in xbox.bitmap_group(NAME)["bitmaps"]:
            crc = zlib.crc32(bitmap["pixels"], crc)
        print(crc)
        return
    if not arguments.out:
        parser.error("--out is needed")
    pages = []
    for index, bitmap in enumerate(xbox.bitmap_group(NAME)["bitmaps"]):
        color, alpha = decode(bitmap)
        image, mask = redraw(color, alpha, sprite_boxes(alpha))
        pages.append(bgra(image, mask))
        if arguments.pngs:
            arguments.pngs.mkdir(parents=True, exist_ok=True)
            Image.fromarray(np.round(color * 255).astype(np.uint8)).save(arguments.pngs / f"map_{index}.png")
            Image.fromarray(np.round(image * 255).astype(np.uint8)).save(arguments.pngs / f"hd_{index}.png")
    tag = arguments.out / "tags" / (NAME.replace("\\", "/") + ".bitmap")
    tag.parent.mkdir(parents=True, exist_ok=True)
    tag.write_bytes(tag_file(xbox, pages))
    (arguments.out / "mod.toml").write_text(
        'name = "HD muzzle flash (test)"\nid = "hd_muzzle_test"\nversion = "0.1"\n'
        'description = "The Assault Rifle\'s muzzle flash at 4x, in 32-bit color"\nformat = 1\n')
    print(f"wrote {tag} ({tag.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
