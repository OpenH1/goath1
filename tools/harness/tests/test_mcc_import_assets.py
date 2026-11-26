"""The MCC import's shipped fits (port/assets/mcc_import/mcc_import.json, docs/mcc-import.md) are numbers and names
only, and well formed: each model's rotation a rotation or mirror, its scale and "close" distance positive, its map a
stock map, each preview mesh named once with its LOD0's counts; and tools/embed_assets.py makes the C table
(mcc_import_bake.h's mcc_import_fits) a row per model. Nothing of MCC's (no pixels, no audio) may ever be in it."""
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
FITS = ROOT / "port/assets/mcc_import/mcc_import.json"
STOCK_MAPS = {"a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40", "beavercreek", "bloodgulch",
              "boardingaction", "carousel", "chillout", "damnation", "hangemhigh", "longest", "prisoner", "putput",
              "ratrace", "sidewinder", "wizard"}


def test_fits_are_numbers_and_well_formed():
    data = json.loads(FITS.read_text())
    assert data["version"] == 1
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
