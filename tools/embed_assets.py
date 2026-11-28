#!/usr/bin/env python3
"""Embeds the high-res HUD textures (port/assets/hud, made by
tools/hud_assets.py), the menus' titles (port/assets/titles, made by
tools/title_assets.py), their controller button icons (port/assets/buttons,
made by tools/button_assets.py), the fonts the text is drawn with
(port/assets/fonts), the menus' files (port/assets/menus, made by
tools/ce_menus.py), the windows' icon and the loading picture in the game
as C data:

    python tools/embed_assets.py OUTPUT.c ASSETS

The textures', fonts' and menu files' bytes (about 15 MB, which the guest
images have no room for) go into ASSETS, openh1.assets, a file the builds
put beside the game as they do brokers.txt (port/linux/src/
embedded_assets.h: its form, and how the game finds it); OUTPUT.c has their
tables, each entry's bytes as an offset in that file, and the file's size and
CRC-32, so that a file of another build is never read. The window's icon, the
menus' pointer and the loading picture, needed before anything else, stay in
OUTPUT.c.

writes OUTPUT.c with each PNG and the bitmap it stands for (its tag, index
and checksum of its pixels, from port/assets/hud/layout.json,
port/assets/titles/titles.json and port/assets/buttons/buttons.json), the
fonts and their tags from port/assets/fonts/fonts.json, the menu files
listed in port/assets/menus/menus.json, the HD effects' list
(port/assets/effects/effects.json: numbers only), the icon and the picture, as
port/linux/src/hud_hires.h, text_hires.h, menu_files.h and sdl_platform.c
declare them. The builds generate it (hud_assets_build, called by
tools/android_build.py, which the other guest builds share), so the assets
are the committed source and the guests need no files beside their image.

The data are 32-bit words, not bytes: the Android build passes the guest's
assembly through tools/android_asm_convert.py, which rewrites identifiers in
operands (it keeps .ascii strings as they are, but once garbled them).
"""

import json
import math
import struct
import sys
import zlib
from pathlib import Path
from typing import Any, List

ROOT = Path(__file__).resolve().parent.parent
HUD_ASSETS = Path("port/assets/hud")
LAYOUT = HUD_ASSETS / "layout.json"
# the headshot indicator's copies of a texture, by their names' ending, and
# their variant (hud_hires.h's HUD_HIRES_VARIANT_*)
VARIANTS = (("__dotless", 1), ("__dot", 2))
CE_LAYOUT = HUD_ASSETS / "ce_layout.json"
MCC_LAYOUT = HUD_ASSETS / "mcc_layout.json"
CE_STOCK = HUD_ASSETS / "ce_stock.json"
# the HUD textures whose bitmaps Halo Custom Edition lays out alike
CUSTOM_EDITION_HUD = HUD_ASSETS / "custom_edition.json"
TITLE_ASSETS = Path("port/assets/titles")
TITLE_LIST = TITLE_ASSETS / "titles.json"
BUTTON_ASSETS = Path("port/assets/buttons")
BUTTON_LIST = BUTTON_ASSETS / "buttons.json"
CONTROLLER_ASSETS = Path("port/assets/controller_buttons")
CONTROLLER_LIST = CONTROLLER_ASSETS / "buttons.json"
FONT_ASSETS = Path("port/assets/fonts")
FONT_LIST = FONT_ASSETS / "fonts.json"
MSDF_ATLAS = FONT_ASSETS / "msdf_atlas.zlib"
MENU_ASSETS = Path("port/assets/menus")
MENU_LIST = MENU_ASSETS / "menus.json"
# the desktop windows' icon (tools/app_icon.py; port/linux/src/sdl_platform.c)
WINDOW_ICON = Path("port/assets/icon/openh1-icon-256.png")
LOADING_PICTURE = Path("port/assets/loading/loading.rgba.zlib")
LOADING_SOURCE = Path("port/assets/loading/loading-source.rgba.zlib")
# the pointer the menus show in place of the system's (port/linux/src/sdl_platform.c)
MENU_CURSOR = Path("port/assets/cursor/menu_cursor.png")
# the menus' pictures drawn again from the player's maps: a pose each (any
# <bitmap>_<frame>.json dropped in), and the maps they are made for
# (port/linux/include/menu_pictures.h)
PICTURE_ASSETS = Path("port/assets/pictures")
PICTURE_MAPS = PICTURE_ASSETS / "maps.json"
# the bitmaps of ui.map whose frames a pose may draw
PICTURE_BITMAPS = {"sp_levels": "ui\\shell\\bitmaps\\sp_levels", "mp_map_grafix": "ui\\shell\\bitmaps\\mp_map_grafix"}
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# the HD effects, numbers only: each stock bitmap the game redraws from the
# player's maps (port/linux/src/hd_effects.h)
EFFECT_LIST = Path("port/assets/effects/effects.json")
EFFECT_RECIPES = {"flash": 1, "splat": 2}
# the MCC import's fits of our models onto MCC's preview meshes, numbers only (port/linux/src/mcc_import_bake.h;
# tools/mcc_preview_bake.py --fits-out makes it)
MCC_FIT_LIST = Path("port/assets/mcc_import/mcc_import.json")
# the levels' static light sources, a file a BSP, packed by tools/lt_emitters_data.py (port/linux/include/
# lt_emitters_assets.h)
EMITTER_ASSETS = Path("port/assets/emitters")
EMITTER_TOOL = Path("tools/lt_emitters_data.py")


def font_files() -> List[str]:
    """The font files fonts.json uses (each font, and the fallback that
    draws what it has not), each once."""
    if not (ROOT / FONT_LIST).is_file():
        return []
    fonts = json.loads((ROOT / FONT_LIST).read_text())["fonts"]
    return sorted({name for font in fonts for name in (font["file"], font.get("fallback")) if name})


def textures() -> List[tuple]:
    """The textures: each one's folder, its entry in its list, and whether
    it is the menus' (a title or a button icon, drawn with the high-res
    text)."""
    result = []
    for folder, listing, title in ((HUD_ASSETS, LAYOUT, False), (TITLE_ASSETS, TITLE_LIST, True),
                                   (BUTTON_ASSETS, BUTTON_LIST, True), (HUD_ASSETS, CE_LAYOUT, False),
                                   (HUD_ASSETS, MCC_LAYOUT, False),
                                   (CONTROLLER_ASSETS, CONTROLLER_LIST, True)):
        if (ROOT / listing).is_file():
            result += [(folder, asset, title) for asset in json.loads((ROOT / listing).read_text())["assets"]]
            # (the copies with the reticle's middle dot taken out or put in,
            # where tools/hud_assets.py made them, for the headshot indicator:
            # hud_hires.h, variant)
            if listing == LAYOUT:
                result += [(folder, dict(asset, name=f"{asset['name']}{suffix}", variant=variant, sprites=[]), title)
                           for asset in json.loads((ROOT / listing).read_text())["assets"]
                           for suffix, variant in VARIANTS
                           if (ROOT / folder / f"{asset['name']}{suffix}.png").is_file()]
    return result


def menu_files() -> List[str]:
    """The menus' files menus.json lists, relative to their folder."""
    if not (ROOT / MENU_LIST).is_file():
        return []
    return json.loads((ROOT / MENU_LIST).read_text())["files"]


# the menus' heavy vector art (port/linux/src/menu_vector.h): an SVG a frame
# names alone that has filters or this many paths is drawn here, when the
# game is built, at the size a 2160-line display shows it, by
# tools/menu_vector_bake.c (menu_vector.c and plutosvg, so it draws as the
# game does), and embedded as "<svg>.<its CRC-32>.png" in the SVG's place
# (a menus folder's own SVG is drawn by the game, as it starts); the game draws
# that PNG with its mip levels (the host makes them), scaled down. The
# picture is the build's, never the repository's.
VECTOR_HEAVY_PATHS = 200
VECTOR_BAKE_SCALE = 2160 / 480
VECTOR_BAKE_TOOL = Path("tools/menu_vector_bake.c")
VECTOR_BAKE_SOURCES = [Path("port/linux/src/menu_vector.c"), *sorted(Path("port/third_party/plutovg/src").glob("*.c")),
                       Path("port/third_party/plutosvg/plutosvg.c")]
VECTOR_BAKE_HEADERS = [Path("port/linux/src/menu_vector.h"), Path("port/third_party/plutovg/include/plutovg.h"),
                       Path("port/third_party/plutosvg/plutosvg.h"),
                       *sorted(Path("port/third_party/plutovg/src").glob("*.h"))]


def vector_heavy(data: bytes) -> bool:
    """whether an SVG is drawn when the game is built"""
    return b"<filter" in data or data.count(b"<path") >= VECTOR_HEAVY_PATHS


def vector_frames() -> dict:
    """{svg: (width, height)} of the frames of the menus' files that name an
    SVG alone"""
    import xml.etree.ElementTree as ElementTree
    frames = {}
    for name in menu_files():
        if not name.endswith(".xml"):
            continue
        for frame in ElementTree.parse(ROOT / MENU_ASSETS / name).getroot().iter("frame"):
            svg = frame.get("svg")
            if svg and " " not in svg.strip() and frame.get("slice") is None:
                frames.setdefault(svg.strip(), (int(frame.get("width")), int(frame.get("height"))))
    return frames


def png_rgba(width: int, height: int, texels: bytes) -> bytes:
    """an 8-bit RGBA PNG of the texels, each row filtered by its left
    neighbour (Sub)"""
    stride = width * 4
    rows = bytearray()
    for row in range(height):
        line = texels[row * stride:(row + 1) * stride]
        left = b"\0\0\0\0" + line[:-4]
        rows += b"\1" + bytes((a - b) & 0xFF for a, b in zip(line, left))
    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    return (PNG_SIGNATURE + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(rows), 9)) + chunk(b"IEND", b""))


def vector_bake_tool(folder: Path) -> Path:
    """menu_vector_bake, compiled with the build computer's C compiler ($CC,
    else cc, clang or gcc) into folder, once for its sources; None if none
    could"""
    import hashlib
    import os
    import shutil
    import subprocess
    digest = hashlib.sha256()
    for path in [VECTOR_BAKE_TOOL, *VECTOR_BAKE_SOURCES, *VECTOR_BAKE_HEADERS]:
        digest.update((ROOT / path).read_bytes())
    tool = folder / f"menu_vector_bake-{digest.hexdigest()[:16]}{'.exe' if os.name == 'nt' else ''}"
    if tool.is_file():
        return tool
    compiler = os.environ.get("CC") or next((found for found in map(shutil.which, ("cc", "clang", "gcc")) if found), None)
    if not compiler:
        return None
    folder.mkdir(parents=True, exist_ok=True)
    command = [*compiler.split(), "-O2", "-std=gnu11", "-w", "-DPLUTOVG_BUILD_STATIC", "-DPLUTOSVG_BUILD_STATIC",
               "-DPLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD", f"-I{ROOT / 'port/linux/src'}",
               f"-I{ROOT / 'port/third_party/plutovg/include'}", f"-I{ROOT / 'port/third_party/plutosvg'}",
               "-o", str(tool), str(ROOT / VECTOR_BAKE_TOOL), *(str(ROOT / path) for path in VECTOR_BAKE_SOURCES),
               *([] if os.name == "nt" else ["-lm"])]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        print(f"embed_assets: cannot compile {VECTOR_BAKE_TOOL}: {result.stderr.strip()[:2000]}", file=sys.stderr)
        return None
    return tool


def vector_bakes(folder: Path) -> dict:
    """{svg: its PNG} of the heavy SVGs, drawn (or kept from the last build,
    by the SVG's, the size's and the tool's hash) in folder; any not drawn
    stays an SVG, which the game draws as it starts (and says so here)"""
    import hashlib
    import subprocess
    baked = {}
    heavy = {svg: size for svg, size in vector_frames().items()
             if (ROOT / MENU_ASSETS / svg).is_file() and vector_heavy((ROOT / MENU_ASSETS / svg).read_bytes())}
    tool = vector_bake_tool(folder) if heavy else None
    for svg, (width, height) in sorted(heavy.items()):
        if not tool:
            print(f"embed_assets: {svg} is drawn by the game as it starts (no C compiler to draw it here)",
                  file=sys.stderr)
            continue
        data = (ROOT / MENU_ASSETS / svg).read_bytes()
        key = hashlib.sha256(data + f"{width} {height} {VECTOR_BAKE_SCALE} {tool.name}".encode()).hexdigest()[:24]
        png = folder / f"{key}.png"
        if not png.is_file():
            raw = folder / f"{key}.rgba"
            result = subprocess.run([str(tool), str(raw), str(width), str(height), str(VECTOR_BAKE_SCALE),
                                     str(ROOT / MENU_ASSETS / svg)], capture_output=True, text=True)
            if result.returncode:
                print(f"embed_assets: {svg}: {result.stderr.strip()}", file=sys.stderr)
                continue
            texels = raw.read_bytes()
            if texels[:4] != b"OHVR":
                continue
            out_width, out_height = struct.unpack("<II", texels[4:12])
            png.write_bytes(png_rgba(out_width, out_height, texels[12:]))
            raw.unlink()
        baked[svg] = png.read_bytes()
    return baked


def picture_poses() -> List[Path]:
    """The pictures' pose files, by name."""
    folder = ROOT / PICTURE_ASSETS
    if not folder.is_dir():
        return []
    return sorted(PICTURE_ASSETS / path.name for path in folder.glob("*.json") if path.name != PICTURE_MAPS.name)


def picture_number(value: float) -> str:
    return ("%.4f" % value).rstrip("0").rstrip(".")


def picture_pose(path: Path, maps: dict) -> dict:
    """A pose file's entry in menu_picture_poses (menu_pictures.h); exits on
    a pose the game could not use."""
    data = (ROOT / path).read_bytes()
    pose = json.loads(data)
    bitmap, _, frame = path.stem.rpartition("_")
    if bitmap not in PICTURE_BITMAPS or not frame.isdigit():
        sys.exit(f"{path}: name it <bitmap>_<frame>.json, of {', '.join(PICTURE_BITMAPS)}")
    scenario = pose["scenario"]
    map_name = scenario.replace("/", "\\").rsplit("\\", 1)[-1]
    if map_name not in maps:
        sys.exit(f"{path}: {map_name} is not in {PICTURE_MAPS} (its cache file checksum)")
    camera = pose["pose"]
    # (the vertical field of view as the game's, across at 4:3; the script's
    # camera halts at 90 degrees: camera_pose.c)
    fov = math.degrees(2 * math.atan(math.tan(math.radians(camera["vfov"]) / 2) * 4 / 3))
    if not 0 < fov < 90:
        sys.exit(f"{path}: a field of view of {fov:.1f} degrees (it must be under 90)")
    words = [camera["x"], camera["y"], camera["z"], camera["yaw"] % 360, camera["pitch"], camera["roll"], fov]
    commands = list(pose.get("prepare", []))
    objects = pose.get("objects", [])
    if objects:
        commands.append('debug_object_place "clear"')
    for item in objects:
        if "seat" in item:
            line = [item["tag"], "seat", item["seat"]]
        else:
            line = [item["tag"]] + [picture_number(item.get(key, 0.0)) for key in ("x", "y", "z", "yaw", "pitch", "roll")]
        if "color" in item:
            line += ["color"] + [picture_number(value) for value in item["color"]]
        commands.append('debug_object_place "%s"' % " ".join(line))
    # ("raw" and "none": the picture's colours as rendered)
    colour = pose.get("colour", "raw")
    if colour not in ("raw", "none", "curve"):
        sys.exit(f'{path}: colour is "raw" or "curve"')
    # (how long the world runs for the objects to settle; 0: left as placed)
    settle = float(pose.get("settle_seconds", 3.0)) if objects else 0.0
    if not 0 <= settle <= 30:
        sys.exit(f"{path}: settle_seconds is 0 to 30")
    return {"name": path.stem, "bitmap": PICTURE_BITMAPS[bitmap], "frame": int(frame), "map": map_name,
            "scenario": scenario, "bsp": int(pose.get("bsp", 0)), "aspect": float(pose["aspect"]),
            "colour": int(colour == "curve"), "settle": int(round(settle * 1000)),
            "camera": " ".join(picture_number(value) for value in words),
            "commands": "".join(line + "\n" for line in commands),
            "checksums": [int(value, 16) for value in maps[map_name]][:4], "hash": zlib.crc32(data)}


def c_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


def picture_lines() -> List[str]:
    """The poses' table (menu_pictures.h)."""
    maps = json.loads((ROOT / PICTURE_MAPS).read_text())["maps"] if (ROOT / PICTURE_MAPS).is_file() else {}
    poses = [picture_pose(path, maps) for path in picture_poses()]
    lines = ['#include "menu_pictures.h"', "", "const struct menu_picture_pose menu_picture_poses[] =", "{"]
    for pose in poses:
        checksums = ", ".join(f"0x{value:08x}u" for value in pose["checksums"])
        lines.append(f'\t{{ {c_string(pose["name"])}, {c_string(pose["bitmap"])}, {pose["frame"]}, '
                     f'{c_string(pose["map"])}, {c_string(pose["scenario"])}, {pose["bsp"]}, {pose["aspect"]!r}f, '
                     f'{pose["colour"]}, {pose["settle"]}, {c_string(pose["camera"])}, {c_string(pose["commands"])}, '
                     f'{{ {checksums} }}, {len(pose["checksums"])}, 0x{pose["hash"]:08x}u }},')
    if not poses:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int menu_picture_pose_count = {len(poses)};")
    lines.append("")
    return lines


def emitters():
    """tools/lt_emitters_data.py, as a module (this file runs as a script and is imported from the package)"""
    try:
        from . import lt_emitters_data
    except ImportError:
        import lt_emitters_data
    return lt_emitters_data


def emitter_lines(assets: "AssetsFile") -> List[str]:
    """The levels' static light sources' table (lt_emitters_assets.h): each BSP's pack compressed in the assets'
    file"""
    tool = emitters()
    lines = ['#include "lt_emitters_assets.h"', "", "struct lt_emitters_asset const lt_emitters_assets[] =", "{"]
    found = tool.levels()
    for name, scenario, bsp, group_count, triangle_count, pack in found:
        if group_count > tool.GROUPS_MAXIMUM or triangle_count > tool.TRIANGLES_MAXIMUM:
            sys.exit(f"{EMITTER_ASSETS / name}: {group_count} groups, {triangle_count} triangles: more than the "
                     f"game holds ({tool.GROUPS_MAXIMUM}, {tool.TRIANGLES_MAXIMUM})")
        packed = tool.compress(pack)
        lines.append(f'\t{{ {c_string(scenario)}, {bsp}, {assets.add(packed)}u, {len(packed)}u, {len(pack)}u }}, '
                     f'/* {name} */')
    if not found:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"unsigned int const lt_emitters_asset_count = {len(found)};")
    lines.append("")
    return lines


def effect_lines() -> List[str]:
    """The HD effects' table (hd_effects.h)."""
    effects = json.loads((ROOT / EFFECT_LIST).read_text())["effects"] if (ROOT / EFFECT_LIST).is_file() else []
    lines = ['#include "hd_effects.h"', "", "const struct hd_effect_embedded hd_effects_embedded[] =", "{"]
    for effect in effects:
        if effect["recipe"] not in EFFECT_RECIPES:
            sys.exit(f"{EFFECT_LIST}: {effect['name']}: no recipe {effect['recipe']!r}")
        lines.append(f'\t{{ {c_string(effect["name"])}, {c_string(effect["tag"])}, {EFFECT_RECIPES[effect["recipe"]]}, '
                     f'{effect["scale"]}, {effect["smallest"]}, 0x{effect["crc"]:08x}u, '
                     f'{c_string(" ".join(effect["maps"]))} }},')
    if not effects:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int hd_effects_embedded_count = {len(effects)};")
    lines.append("")
    return lines


def mcc_fit_lines() -> List[str]:
    """The MCC import's fits' table (mcc_import_bake.h): a row per model."""
    meshes = json.loads((ROOT / MCC_FIT_LIST).read_text())["meshes"] if (ROOT / MCC_FIT_LIST).is_file() else []
    lines = ['#include "mcc_import_bake.h"', "", "const struct mcc_import_fit mcc_import_fits[] =", "{"]
    count = 0

    def numbers(values):
        return ", ".join(repr(float(value)) for value in values)

    for mesh in meshes:
        for model in mesh["models"]:
            if len(model["rotation"]) != 9 or len(model["shift"]) != 3:
                sys.exit(f"{MCC_FIT_LIST}: {mesh['mesh']}: {model['model']}: a rotation of 9 and a shift of 3 numbers")
            lines.append(f'\t{{ {c_string(mesh["mesh"])}, {c_string(mesh["fallback"])}, {int(mesh["vertices"])}, '
                         f'{int(mesh["triangles"])}, {c_string(model["model"])}, {c_string(model["map"])}, '
                         f'{float(model["scale"])!r}, {{ {numbers(model["rotation"])} }}, '
                         f'{{ {numbers(model["shift"])} }}, {float(model["near"])!r} }},')
            count += 1
    if not count:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int mcc_import_fit_count = {count};")
    lines.append("")
    return lines


MCC_LAYOUT_FIELDS = ("model", "map", "bitmap", "root", "color", "normal", "metallic", "occlusion")


def mcc_layout_lines() -> List[str]:
    """The MCC import's texture-only layouts' table (mcc_import_bake.h): a row per bitmap."""
    data = json.loads((ROOT / MCC_FIT_LIST).read_text()) if (ROOT / MCC_FIT_LIST).is_file() else {}
    layouts = data.get("layouts", [])
    lines = ["const struct mcc_import_layout mcc_import_layouts[] =", "{"]
    for layout in layouts:
        if set(layout) != set(MCC_LAYOUT_FIELDS) or not layout["root"].endswith("/"):
            sys.exit(f"{MCC_FIT_LIST}: a layout has {', '.join(MCC_LAYOUT_FIELDS)} (its root ending in /)")
        lines.append("\t{ " + ", ".join(c_string(layout[field]) for field in MCC_LAYOUT_FIELDS) + " },")
    if not layouts:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int mcc_import_layout_count = {len(layouts)};")
    lines.append("")
    return lines


def hud_asset_inputs() -> List[Path]:
    """The files the generated source is made from."""
    inputs = [listing for listing in (LAYOUT, CE_LAYOUT, MCC_LAYOUT, CE_STOCK, CUSTOM_EDITION_HUD, TITLE_LIST, BUTTON_LIST, CONTROLLER_LIST, FONT_LIST, MENU_LIST)
              if (ROOT / listing).is_file()]
    return [*inputs, *(folder / f"{asset['name']}.png" for folder, asset, _ in textures()),
            *(FONT_ASSETS / name for name in font_files()), *(MENU_ASSETS / name for name in menu_files()),
            *([MSDF_ATLAS] if (ROOT / MSDF_ATLAS).is_file() else []),
            *([WINDOW_ICON] if (ROOT / WINDOW_ICON).is_file() else []),
            *([LOADING_PICTURE] if (ROOT / LOADING_PICTURE).is_file() else []),
            *([LOADING_SOURCE] if (ROOT / LOADING_SOURCE).is_file() else []),
            *([MENU_CURSOR] if (ROOT / MENU_CURSOR).is_file() else []),
            *([PICTURE_MAPS] if (ROOT / PICTURE_MAPS).is_file() else []), *picture_poses(),
            *([EFFECT_LIST] if (ROOT / EFFECT_LIST).is_file() else []),
            *([MCC_FIT_LIST] if (ROOT / MCC_FIT_LIST).is_file() else []),
            EMITTER_TOOL, *(EMITTER_ASSETS / path.name for path in emitters().files()),
            *(path for path in [VECTOR_BAKE_TOOL, *VECTOR_BAKE_SOURCES, *VECTOR_BAKE_HEADERS] if (ROOT / path).is_file())]


def hud_configure_inputs() -> List[Path]:
    """What configure.py is run again for: the lists of assets (and the
    folders, for files added or removed), not each file, which a change of a
    list may rename or remove."""
    inputs = []
    for folder, listing in ((HUD_ASSETS, LAYOUT), (HUD_ASSETS, CE_LAYOUT), (HUD_ASSETS, MCC_LAYOUT), (TITLE_ASSETS, TITLE_LIST), (BUTTON_ASSETS, BUTTON_LIST),
                            (CONTROLLER_ASSETS, CONTROLLER_LIST),
                            (FONT_ASSETS, FONT_LIST), (MENU_ASSETS, MENU_LIST)):
        if (ROOT / listing).is_file():
            inputs += [folder, listing]
    # (a pose dropped in changes the folder: configure.py runs again)
    if (ROOT / PICTURE_ASSETS).is_dir():
        inputs.append(PICTURE_ASSETS)
    # (and an emitter file added or removed)
    if (ROOT / EMITTER_ASSETS).is_dir():
        inputs.append(EMITTER_ASSETS)
    return inputs


def words(data: bytes) -> List[str]:
    """data as lines of 32-bit words (padded with zeros)."""
    padded = data + b"\0" * (-len(data) % 4)
    values = struct.unpack(f"<{len(padded) // 4}I", padded)
    return ["\t" + ", ".join(f"0x{word:08x}" for word in values[start:start + 8]) + ","
            for start in range(0, len(values), 8)]


def assets_file(output: Path) -> Path:
    """the assets' file hud_assets_build makes beside output"""
    return output.with_name(ASSETS_NAME)


def hud_assets_build(n: Any, prefix: str, output: Path) -> List[Path]:
    """Emits the rule that generates output and the assets' file beside it
    (assets_file); returns [output]. It is made whatever assets the checkout
    has (with none, its tables are empty), so that the symbols the platform
    layer refers to are always defined."""
    inputs = hud_asset_inputs()
    n.rule(
        name=f"{prefix}_embed_assets",
        command=f"$python tools/embed_assets.py $out {assets_file(output)}",
        description=f"{prefix.upper()} EMBED $out",
    )
    n.build(outputs=output, rule=f"{prefix}_embed_assets", implicit=[Path("tools/embed_assets.py"), *inputs],
            implicit_outputs=[assets_file(output)])
    return [output]


# the assets' file (port/linux/src/embedded_assets.h)
ASSETS_NAME = "openh1.assets"
ASSETS_MAGIC = 0x4131484F  # "OH1A"
ASSETS_FORM = 1


class AssetsFile:
    """the assets' file's bytes, each blob 16-byte aligned after its header"""

    def __init__(self) -> None:
        self.data = bytearray(struct.pack("<4I", ASSETS_MAGIC, ASSETS_FORM, 0, 0))

    def add(self, blob: bytes) -> int:
        self.data += b"\0" * (-len(self.data) % 16)
        offset = len(self.data)
        self.data += blob
        return offset

    def lines(self) -> List[str]:
        return [f"const unsigned int embedded_assets_size = {len(self.data)}u;",
                f"const unsigned int embedded_assets_crc = 0x{zlib.crc32(bytes(self.data)):08x}u;", ""]


def write_output(output: Path, lines: List[str]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    text = "\n".join(lines) + "\n"
    if not output.is_file() or output.read_text() != text:
        output.write_text(text)
def server_ui_build(n: Any, prefix: str, output: Path) -> List[Path]:
    """Emits the rule that generates a dedicated server's admin page as C
    (tools/server_ui_embed.py, port/assets/server_ui); returns [output]"""
    ui = Path("port/assets/server_ui")
    inputs = sorted(path for path in (ROOT / ui).glob("*") if path.is_file())
    n.rule(
        name=f"{prefix}_server_ui",
        command="$python tools/server_ui_embed.py $out",
        description=f"{prefix.upper()} EMBED $out",
    )
    n.build(outputs=output, rule=f"{prefix}_server_ui",
            implicit=[Path("tools/server_ui_embed.py"), Path("tools/server_sim.py"),
                      *(ui / path.name for path in inputs)])
    return [output]


def png_size(data: bytes, name: str) -> tuple:
    """The width and height of an 8-bit RGBA, non-interlaced PNG (the only
    kind port/linux/src/hud_hires.c reads)."""
    if data[:8] != PNG_SIGNATURE or data[12:16] != b"IHDR":
        sys.exit(f"{name}: not a PNG")
    width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", data[16:29])
    if depth != 8 or colour != 6 or interlace != 0:
        sys.exit(f"{name}: must be 8-bit RGBA and not interlaced")
    return width, height


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit("usage: embed_assets.py OUTPUT.c ASSETS")
    assets = AssetsFile()
    lines = [
        "/* generated by tools/embed_assets.py from port/assets: do not edit */",
        "",
        '#include "hud_hires.h"',
        "",
    ]
    table = []
    custom_edition = {}
    if (ROOT / CUSTOM_EDITION_HUD).is_file():
        custom_edition = {(entry["tag"], entry["bitmap"]): entry
                          for entry in json.loads((ROOT / CUSTOM_EDITION_HUD).read_text())["assets"]}
    for index, (folder, asset, title) in enumerate(textures()):
        name = f"{asset['name']}.png"
        data = (ROOT / folder / name).read_bytes()
        width, height = png_size(data, name)
        scale = asset["scale"]
        if (width, height) != (asset["width"] * scale, asset["height"] * scale):
            sys.exit(f"{name}: {width}x{height}, not {scale}x its bitmap's {asset['width']}x{asset['height']}")
        offset = assets.add(data)
        tag = asset["tag"].replace("\\", "\\\\")
        coverage = int(asset.get("coverage", False) or any(cell.get("kind") == "meter" for cell in asset.get("cells", [])))
        point_threshold = int(asset.get("point_threshold", False) or any(cell.get("thresholds") for cell in asset.get("cells", [])))
        # (the sequences whose sprites it stands for, a bit each of 32; 0: the whole bitmap)
        sequences = [sprite["sequence"] for sprite in asset.get("sprites", [])]
        if any(not 0 <= sequence < 32 for sequence in sequences):
            sys.exit(f"{name}: sprite sequences must be 0 to 31 (hud_hires.h: sprites)")
        sprites = sum(1 << sequence for sequence in set(sequences))
        # (their cells, each checked on its own texels, where the list has
        # their checksums: hud_hires.h, hud_hires_cell)
        cells = [sprite for sprite in asset.get("sprites", []) if "crc" in sprite]
        if cells:
            lines.append(f"static const struct hud_hires_cell cells{index}[] = {{")
            lines.extend(f"\t{{ {cell['sequence']}, {', '.join(str(value) for value in cell['cell'])}, "
                         f"0x{cell['crc']:08x}u }}," for cell in cells)
            lines.append("};")
            lines.append("")
        # (a Custom Edition map's stock bitmap it is drawn for too, and that
        # bitmap's first level as the Custom Edition loader leaves it)
        ce = custom_edition.get((asset["tag"], asset["bitmap"]), {})
        if ce.get("replacement", asset["name"]) != asset["name"] or asset.get("variant"):
            ce = {}
        table.append(f'\t{{ "{tag}", {asset["bitmap"]}, {width}, {height}, 0x{asset["crc"]:08x}u, {coverage}, '
                     f'{point_threshold}, {int(title)}, 0x{sprites:x}u, '
                     f'{f"cells{index}" if cells else "0"}, {len(cells)}, '
                     f'{int(bool(ce))}, {int(asset.get("ce_only", False))}, '
                     f'{asset.get("variant", 0)}, {ce.get("width", 0)}, {ce.get("height", 0)}, '
                     f'{ce.get("size", 0)}, 0x{ce.get("crc", 0):08x}u, {offset}u, {len(data)} }},')
    lines.append("const struct hud_hires_embedded hud_hires_embedded[] =")
    lines.append("{")
    lines.extend(table)
    if not table:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int hud_hires_embedded_count = {len(table)};")
    # (the Custom Edition maps' stock HUD sheets the high-res HUD upscales,
    # each by its first level as the Custom Edition loader leaves it)
    stock = json.loads((ROOT / CE_STOCK).read_text())["bitmaps"] if (ROOT / CE_STOCK).is_file() else []
    lines.append("const struct hud_hires_ce_stock hud_hires_ce_stock[] = {")
    for item in stock:
        tag = item['tag'].replace('\\', '\\\\')
        lines.append(f'\t{{ "{tag}", {item["bitmap"]}, {item["width"]}, {item["height"]}, '
                     f'{item["size"]}, 0x{item["crc"]:08x}u }},')
    if not stock:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int hud_hires_ce_stock_count = {len(stock)};")
    lines.append("")
    # the fonts, and which draws each font tag (text_hires.h)
    lines.append('#include "text_hires.h"')
    lines.append("")
    files = font_files()
    font_offsets = [assets.add((ROOT / FONT_ASSETS / name).read_bytes()) for name in files]
    fonts = json.loads((ROOT / FONT_LIST).read_text())["fonts"] if files else []
    lines.append("const struct text_hires_embedded text_hires_embedded[] =")
    lines.append("{")
    for font in fonts:
        tag = font["tag"].replace("\\", "\\\\")
        # (the font, and its fallback: none, 0)
        fields = []
        for name in (font["file"], font.get("fallback")):
            if name:
                size = (ROOT / FONT_ASSETS / name).stat().st_size
                fields.append(f'"{name}", {font_offsets[files.index(name)]}u, {size}')
            else:
                fields.append("0, 0u, 0")
        lines.append(f'\t{{ "{tag}", {fields[0]}, {fields[1]} }},')
    if not fonts:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int text_hires_embedded_count = {len(fonts)};")
    # the fonts' glyphs' MSDF atlas (the high-res text's, made by
    # tools/msdf_atlas.py; of size 0 where the checkout has none)
    data = (ROOT / MSDF_ATLAS).read_bytes() if files and (ROOT / MSDF_ATLAS).is_file() else b""
    lines.append(f"const unsigned int text_hires_msdf_offset = {assets.add(data) if data else 0}u;")
    lines.append(f"const unsigned int text_hires_msdf_size = {len(data)}u;")
    lines.append("")
    # the menus' files (menu_files.h)
    lines.append('#include "menu_files.h"')
    lines.append("")
    # (each heavy SVG as the PNG drawn of it here, named the SVG's, its CRC-32 and .png)
    baked = vector_bakes(Path(sys.argv[1]).resolve().parent / "menu_vector")
    menus, blobs = [], []
    for name in menu_files():
        data = baked[name] if name in baked else (ROOT / MENU_ASSETS / name).read_bytes()
        if name in baked:
            name = f"{name}.{zlib.crc32((ROOT / MENU_ASSETS / name).read_bytes()) & 0xFFFFFFFF:08x}.png"
        if name.endswith(".png"):
            png_size(data, name)
        menus.append(name)
        blobs.append(data)
    menu_offsets = [assets.add(data) for data in blobs]
    lines.append("const struct menu_file_embedded menu_files_embedded[] =")
    lines.append("{")
    for index, name in enumerate(menus):
        lines.append(f'\t{{ "{name}", {menu_offsets[index]}u, {len(blobs[index])} }},')
    if not menus:
        lines.append("\t{ 0 },")
    lines.append("};")
    lines.append(f"const unsigned int menu_files_embedded_count = {len(menus)};")
    # the windows' icon, a PNG (of size 0 where the checkout has none)
    data = (ROOT / WINDOW_ICON).read_bytes() if (ROOT / WINDOW_ICON).is_file() else b""
    lines.append("")
    lines.append("const unsigned int platform_window_icon[] = {")
    lines.extend(words(data) if data else ["\t0,"])
    lines.append("};")
    lines.append(f"const unsigned long platform_window_icon_size = {len(data)};")
    # the menus' pointer, a PNG (of size 0 where the checkout has none)
    data = (ROOT / MENU_CURSOR).read_bytes() if (ROOT / MENU_CURSOR).is_file() else b""
    lines.append("")
    lines.append("const unsigned int platform_menu_cursor[] = {")
    lines.extend(words(data) if data else ["\t0,"])
    lines.append("};")
    lines.append(f"const unsigned long platform_menu_cursor_size = {len(data)};")
    data = (ROOT / LOADING_PICTURE).read_bytes() if (ROOT / LOADING_PICTURE).is_file() else b""
    lines.append("")
    lines.append("const unsigned int platform_loading_picture[] = {")
    lines.extend(words(data) if data else ["\t0,"])
    lines.append("};")
    lines.append(f"const unsigned long platform_loading_picture_size = {len(data)};")
    # and at its own size (1600x1200), which the sharp frame resamples
    data = (ROOT / LOADING_SOURCE).read_bytes() if (ROOT / LOADING_SOURCE).is_file() else b""
    lines.append("")
    lines.append("const unsigned int platform_loading_source[] = {")
    lines.extend(words(data) if data else ["\t0,"])
    lines.append("};")
    lines.append(f"const unsigned long platform_loading_source_size = {len(data)};")
    lines.append("")
    lines.extend(picture_lines())
    lines.extend(effect_lines())
    lines.extend(mcc_fit_lines())
    lines.extend(mcc_layout_lines())
    lines.extend(emitter_lines(assets))
    # the assets' file, its size and CRC-32 in its header too
    struct.pack_into("<I", assets.data, 8, len(assets.data))
    lines.append('#include "embedded_assets.h"')
    lines.append("")
    lines.extend(assets.lines())
    write_output(Path(sys.argv[1]), lines)
    pack = Path(sys.argv[2])
    pack.parent.mkdir(parents=True, exist_ok=True)
    if not pack.is_file() or pack.read_bytes() != bytes(assets.data):
        pack.write_bytes(bytes(assets.data))


if __name__ == "__main__":
    main()
