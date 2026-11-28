#!/usr/bin/env python3
"""Pack the levels' static light sources for the game (port/linux/game/lt_emitters.c).

    python3 tools/lt_emitters_data.py           # each BSP's lamp groups, triangles and packed size
    python3 tools/lt_emitters_data.py --check   # fails where a file does not build or will not fit the game

It reads every port/assets/emitters/<map>_bsp<k>.json (the format: port/assets/emitters/README.md, the indoor
emitters' solve from each BSP's lightmap) and packs each BSP's lamp groups: each group's radiance (linear, the
game's light units) and its triangles (world units, already lifted off their surfaces and wound to glow toward their
normals; a light card's both faces). The build puts each BSP's pack, zlib-compressed, in openh1.assets
(tools/embed_assets.py, beside the game: port/linux/src/embedded_assets.h), with a compiled-in table naming it by
its scenario's tag path and BSP index (port/linux/include/lt_emitters_assets.h). The game unpacks the loaded BSP's
when the BSP changes, and the light-transport pass traces them live as area lights (lt/e1.frag's emitter_sample).

A pack, little-endian: group count and triangle count (uint32 each); per group its radiance (3 float32), first
triangle and triangle count (uint32 each); then per triangle its 3 corners (9 float32). The positions stay full
floats: they sit 0.01 wu off their surfaces, which 16-bit positions over a level would not keep.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
FOLDER = ROOT / "port/assets/emitters"
# the most a BSP's set holds (port/linux/game/lt_emitters.c's LT_EMITTERS_*_MAXIMUM, the backend's light table)
TRIANGLES_MAXIMUM = 8192
GROUPS_MAXIMUM = 1024


def luminance(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def linear_colour(entry, radiance):
    """the group's radiance with the tag's colour taken to linear light (^2.2), its luminance kept: the tag's colour is
    a display colour (the lightmap shows its light ^2.2), so a blue strip's light is as blue on screen as the bake's;
    the solve's brightness (its per-group scale, fitted on luminance) is kept"""
    colour = entry.get("tag_color")
    if not colour or len(colour) != 3 or max(colour) <= 0.0 or luminance(radiance) <= 0.0:
        return radiance
    linear = [max(c, 0.0) ** 2.2 for c in colour]
    if luminance(linear) <= 0.0:
        return radiance
    k = luminance(radiance) / luminance(linear)
    return [c * k for c in linear]


def files(folder: Path = FOLDER) -> list[Path]:
    """the emitter files, in the order they are packed"""
    return sorted(folder.glob("*_bsp*.json"))


def level(path: Path):
    """(scenario, BSP index, group count, triangle count, pack) of one file, or None when it is not an emitters file"""
    doc = json.loads(path.read_text())
    if not str(doc.get("format", "")).startswith("goat-h1 rt2 emitters"):
        return None
    groups, triangles = [], []
    for entry in list(doc.get("lamps") or []) + list(doc.get("added_emitters") or []):
        radiance = linear_colour(entry, entry.get("radiance") or [0.0, 0.0, 0.0])
        corners = [t for t in entry.get("triangles") or [] if len(t) == 9]
        if not corners or max(radiance) <= 0.0:
            continue
        groups.append((radiance, len(triangles), len(corners)))
        triangles.extend(corners)
    pack = bytearray(struct.pack("<2I", len(groups), len(triangles)))
    for radiance, first, count in groups:
        pack += struct.pack("<3f2I", *radiance, first, count)
    for corners in triangles:
        pack += struct.pack("<9f", *corners)
    return doc["scenario"], int(doc.get("bsp_index", 0)), len(groups), len(triangles), bytes(pack)


def levels(folder: Path = FOLDER):
    """every file's level, each as (file name, scenario, BSP index, group count, triangle count, pack)"""
    found = []
    for path in files(folder):
        built = level(path)
        if built:
            found.append((path.name, *built))
    return found


def compress(pack: bytes) -> bytes:
    return zlib.compress(pack, 9)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail where a file does not build or will not fit")
    args = parser.parse_args()
    problems, total, packed = [], 0, 0
    seen = {}
    for name, scenario, bsp, group_count, triangle_count, pack in levels():
        size = len(compress(pack))
        total += len(pack)
        packed += size
        if group_count > GROUPS_MAXIMUM or triangle_count > TRIANGLES_MAXIMUM:
            problems.append(f"{name}: {group_count} groups, {triangle_count} triangles: more than the game's "
                            f"{GROUPS_MAXIMUM} and {TRIANGLES_MAXIMUM} (lt_emitters.c would cut it short)")
        key = (scenario.lower(), bsp)
        if key in seen:
            problems.append(f"{name}: the same BSP as {seen[key]}")
        seen[key] = name
        if not args.check:
            print(f"{name}: {group_count} groups, {triangle_count} triangles, {size // 1024} KB packed")
    for problem in problems:
        print(problem, file=sys.stderr)
    if not args.check:
        print(f"{len(seen)} BSPs: {total // 1024} KB, {packed // 1024} KB in openh1.assets")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
