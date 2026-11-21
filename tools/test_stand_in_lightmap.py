"""A map's preparation draws its other structures' materials with its first's
standing in (render_enumerate.c's stand_ins_next), one lightmap of the first
drawn without its picture for the keys of lightmaps without one. Its last
round gives that picture back even where no material stands in: else the
level draws the lightmap's surfaces with none, black (#1203: a50's first
cliff)."""
from pathlib import Path
import shutil
import subprocess

import pytest

from test_death_timing import function

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r"""
#include <assert.h>
#include <string.h>
typedef int boolean;
#define FALSE 0
#define TRUE 1
#define NONE (-1)
#define MAXIMUM_ENUMERATED_MATERIALS 64
#define MAXIMUM_STAND_IN_ROUNDS 4
#define MATERIAL_KEY_CLASS(key) ((key) >> 16)
#define MATERIAL_KEY_MIRROR 0x80000000UL
#define DATUM_INDEX_TO_ABSOLUTE_INDEX(index) ((index) & 0xffff)
struct tag_reference { long index; };
struct structure_material { struct tag_reference shader; short permutation_index; long first_surface_index;
    long surface_count; unsigned short flags; };
struct tag_block { long count; void *address; };
struct structure_lightmap { short bitmap_index; struct tag_block materials; };
struct structure_bsp { struct tag_block lightmaps; struct tag_block surfaces; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
static struct { boolean mirrors; } enumerate;
struct enumerated_materials { short count; unsigned long keys[MAXIMUM_ENUMERATED_MATERIALS]; };
"""

MAIN = r"""
int main(int argc, char **argv) {
    /* the first structure: a lit cliff (lightmap 0, picture 0), the lit ground (picture 1) */
    static struct structure_material cliff = { { 10 }, 0, 0, 4, 0 }, ground = { { 11 }, 0, 4, 4, 0 };
    static struct structure_lightmap lightmaps[2];
    static struct structure_bsp bsp;
    static struct enumerated_materials enumerated;
    short rounds = 0;

    (void)argv;
    lightmaps[0].bitmap_index = 0; lightmaps[0].materials.count = 1; lightmaps[0].materials.address = &cliff;
    lightmaps[1].bitmap_index = 1; lightmaps[1].materials.count = 1; lightmaps[1].materials.address = &ground;
    bsp.lightmaps.count = 2; bsp.lightmaps.address = lightmaps; bsp.surfaces.count = 8;
    /* another structure's key without a picture, flags no material of the first has (a grav lift's):
       or (the control) one with a picture, which takes no lightmap's */
    stand_ins.pending.count = 1;
    stand_ins.pending.keys[0] = 20 | (argc > 1 ? 0x10000UL : 0) | 5UL << 17;
    stand_ins.pending_shader_indices[0] = 20;
    while (stand_ins_next(&bsp, &enumerated))
        rounds++;
    assert(rounds == 0);
    assert(stand_ins.pending.count == 1);
    /* the level's lightmaps as the map has them, and their materials' shaders */
    assert(!stand_ins.unlit);
    assert(lightmaps[0].bitmap_index == 0 && lightmaps[1].bitmap_index == 1);
    assert(cliff.shader.index == 10 && ground.shader.index == 11);
    return 0;
}
"""


def build(tmp_path, code):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("a C compiler is needed")
    start = code.index("static struct\n{\n\t/* the keys still to draw")
    source = PRELUDE + code[code.index("static boolean materials_have("):code.index("/* each of the structure's materials' keys")]
    source += function(code, "material_key") + "\n"
    source += code[start:code.index("} stand_ins;", start)] + "} stand_ins;\n"
    for name in ("stand_ins_restore", "stand_ins_next"):
        source += function(code, name) + "\n"
    path = tmp_path / "stand_in.c"
    path.write_text(source + MAIN)
    binary = tmp_path / "stand_in"
    subprocess.run([compiler, "-std=c99", "-Wall", "-Wno-unused-function", "-o", str(binary), str(path)], check=True)
    return binary


def test_unlit_stand_in_gives_the_lightmap_back(tmp_path):
    binary = build(tmp_path, (ROOT / "port/linux/game/render_enumerate.c").read_text())
    subprocess.run([str(binary)], check=True, timeout=15)
    subprocess.run([str(binary), "lit"], check=True, timeout=15)


def test_without_the_restore_the_lightmap_stays_unlit(tmp_path):
    """the negative control: the fix taken out, the unlit key leaves lightmap 0 without its picture"""
    code = (ROOT / "port/linux/game/render_enumerate.c").read_text()
    fixed = code[code.index("\tif (!stand_ins.count)\n\t{\n\t\tstand_ins_restore(enumerated);"):]
    fixed = fixed[:fixed.index("return TRUE;") + len("return TRUE;")]
    binary = build(tmp_path, code.replace(fixed, "return stand_ins.count != 0;"))
    assert subprocess.run([str(binary)], timeout=15, capture_output=True).returncode != 0
    subprocess.run([str(binary), "lit"], check=True, timeout=15)
