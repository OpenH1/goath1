"""The MCC import's shipped fits (port/assets/mcc_import/mcc_import.json, docs/mcc-import.md) are numbers and names
only, and well formed: each model's rotation a rotation or mirror, its scale and "close" distance positive, its map a
stock map, each preview mesh named once with its LOD0's counts; and tools/embed_assets.py makes the C table
(mcc_import_bake.h's mcc_import_fits) a row per model. Nothing of MCC's (no pixels, no audio) may ever be in it.

Its layouts (MCC's textures already in our bitmap's UV layout, the Spartan's) are names only too, and the bake's
layout code (mcc_import_bake.c) takes MCC's texel at the same UV, finds the textures under the layout's own folder (not
the fitted meshes'), and keeps our texels on the visor (MCC's color's alpha hole, which MCC paints white)."""
import json
import math
import re
import sys
from pathlib import Path

import pytest
from tools.harness import build, function, mutated, read, run, structure

ROOT = Path(__file__).resolve().parents[3]
FITS = ROOT / "port/assets/mcc_import/mcc_import.json"
STOCK_MAPS = {"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40", "beavercreek", "bloodgulch",
              "boardingaction", "carousel", "chillout", "damnation", "hangemhigh", "longest", "prisoner", "putput",
              "ratrace", "sidewinder", "wizard"}


def test_fits_are_numbers_and_well_formed():
    data = json.loads(FITS.read_text())
    assert data["version"] == 1
    assert set(data) <= {"version", "meshes", "layouts"}
    meshes = data["meshes"]
    assert len({mesh["mesh"] for mesh in meshes}) == len(meshes) >= 14
    for mesh in meshes:
        assert set(mesh) == {"mesh", "fallback", "vertices", "triangles", "models"}
        assert mesh["mesh"].startswith("SM_") and mesh["vertices"] > 0 and mesh["triangles"] > 0
        assert mesh["models"]
        for model in mesh["models"]:
            assert set(model) == {"model", "map", "scale", "rotation", "shift", "near"}
            assert model["map"] in STOCK_MAPS
            assert model["scale"] > 0 and model["near"] > 0
            r = model["rotation"]
            assert len(r) == 9 and len(model["shift"]) == 3
            assert all(isinstance(value, float) and math.isfinite(value) for value in r + model["shift"])
            # orthonormal rows, determinant +1 (a rotation) or -1 (a mirror)
            for i in range(3):
                for j in range(3):
                    dot = sum(r[3 * i + k] * r[3 * j + k] for k in range(3))
                    assert abs(dot - (1.0 if i == j else 0.0)) < 1e-6
            det = (r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
                   r[2] * (r[3] * r[7] - r[4] * r[6]))
            assert abs(abs(det) - 1.0) < 1e-6
    # (small: numbers, never a texture's or a sound's bytes)
    assert FITS.stat().st_size < 64 * 1024


def test_embedded_table_has_a_row_per_model():
    sys.path.insert(0, str(ROOT / "tools"))
    import embed_assets
    lines = embed_assets.mcc_fit_lines()
    models = sum(len(mesh["models"]) for mesh in json.loads(FITS.read_text())["meshes"])
    assert f"const unsigned int mcc_import_fit_count = {models};" in lines
    assert sum(line.startswith('\t{ "SM_') for line in lines) == models


LAYOUT_FIELDS = {"model", "map", "bitmap", "root", "color", "normal", "metallic", "occlusion"}


def layouts():
    return json.loads(FITS.read_text()).get("layouts", [])


def test_layouts_are_names_and_well_formed():
    spartan = [layout for layout in layouts() if layout["bitmap"] == "characters\\cyborg\\bitmaps\\cyborg"]
    assert len(spartan) == 1 and spartan[0]["model"] == "characters\\cyborg\\cyborg"
    assert spartan[0]["root"] == "Characters/H1/Spartans/Textures/"
    for layout in layouts():
        assert set(layout) == LAYOUT_FIELDS
        assert layout["map"] in STOCK_MAPS and layout["color"]
        assert layout["root"].endswith("/") and not layout["root"].startswith("/") and ".." not in layout["root"]
        assert all(isinstance(layout[field], str) for field in LAYOUT_FIELDS)


def test_embedded_table_has_a_row_per_layout():
    sys.path.insert(0, str(ROOT / "tools"))
    import embed_assets
    lines = embed_assets.mcc_layout_lines()
    assert f"const unsigned int mcc_import_layout_count = {len(layouts())};" in lines
    assert sum(line.startswith('\t{ "characters') for line in lines) == len(layouts())


# ---------- the bake's layout code, compiled (tests/mcc_import_layout.c)

BAKE = "port/linux/src/mcc_import_bake.c"
LAYOUT_FUNCTIONS = ("round_even", "clampd", "unit3", "compare_floats", "percentile", "bilinear_wrap", "layout_asset", "mask_grow",
                    "layout_resample", "layout_visor", "layout_color", "layout_normal", "layout_orm")


def code():
    source = read(BAKE)
    return "\n".join(function(source, name) for name in LAYOUT_FUNCTIONS)


def execute(source, case):
    bake = read(BAKE)
    defines = "\n".join(re.search(r"^#define " + name + r" .*$", bake, re.M).group(0)
                         for name in ("CUSTOMIZATION_ROOT", "MAXIMUM_PATH", "PAD_STEPS", "VISOR_ALPHA", "VISOR_GROWTH"))
    pak = read("port/linux/src/ue4_pak.c")
    pak_code = "\n".join((structure(pak, "pak_entry"), structure(pak, "ue4_pak"),
                          "static " + function(pak, "ue4_pak_find_asset")))
    spartan = next(layout for layout in layouts() if layout["bitmap"].endswith("\\cyborg"))
    root = "MCC/Content/UI/Features/Customization/" + spartan["root"]
    layout = "\n".join((structure(read("port/linux/src/mcc_import_bake.h"), "mcc_import_layout"),
                        f'#define LAYOUT_ROOT "{spartan["root"]}"', f'#define LAYOUT_COLOR "{spartan["color"]}"',
                        f'#define LAYOUT_COLOR_PATH "{root}{spartan["color"]}"'))
    return run(build("mcc_import_layout", (("defines.inc", defines), ("pak.inc", pak_code), ("layout.inc", layout),
                                           ("under_test.inc", source))), case)


@pytest.mark.parametrize("case", ["identity", "root", "color", "visor", "normal"])
def test_layout_bake(case):
    status, output = execute(code(), case)
    assert status == 0, output


@pytest.mark.parametrize("fault,case", [("identity", "identity"), ("root", "root"), ("visor", "visor"),
                                        ("normal", "normal")])
def test_layout_negative_control(fault, case):
    source = code()
    if fault == "identity":
        # (MCC's texel half a texel off ours)
        source = mutated(source, "(x + 0.5) * image_width / width", "x * image_width / width")
    elif fault == "root":
        # (the textures looked for in the fitted meshes' folder)
        source = mutated(source, 'snprintf(root, sizeof(root), "%s%s", CUSTOMIZATION_ROOT, layout->root);',
                         'snprintf(root, sizeof(root), "%sSharedTextures/H1/%.0s", CUSTOMIZATION_ROOT, layout->root);')
    elif fault == "visor":
        # (no visor: MCC's white painted over ours)
        source = mutated(source, "visor[texel] = visor[texel] && near[texel];", "visor[texel] = 0;")
    else:
        # (DirectX's green kept)
        source = mutated(source, "n[1] = -(normal[4 * texel + 1] / 127.5 - 1);", "n[1] = normal[4 * texel + 1] / 127.5 - 1;")
    status, output = execute(source, case)
    assert status == 1 and "check failed" in output, output
