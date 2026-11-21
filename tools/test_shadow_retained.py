"""A shadow kept from the frame before must not be one the map's preparation
drew: its passes walk the casters, but what they draw never reaches the atlas
(rasterizer_xbox_shadows.c's retained casters). A caster drawn in a frame of
the game is kept the frame after; one drawn in the preparation's is drawn
again."""
from pathlib import Path
import shutil
import subprocess

import pytest

from test_death_timing import function

ROOT = Path(__file__).resolve().parents[1]


def test_preparation_casters_are_not_kept(tmp_path):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("a C compiler is needed")
    code = (ROOT / "source/rasterizer/xbox/rasterizer_xbox_shadows.c").read_text()
    source = r"""
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef int boolean;
typedef float real;
typedef struct { real red, green, blue; } real_rgb_color;
typedef struct { real m[12]; } real_matrix4x3;
typedef struct D3DSurface D3DSurface;
#define FALSE 0
#define TRUE 1
#define NONE (-1)
#define SHADOW_ATLAS_REGIONS 49
#define SHADOW_ATLAS_CLASSES 3
#define match_assert(file, line, condition) assert(condition)
static boolean render_enumerating = FALSE;
static boolean shadow_atlas_active = TRUE;
static short shadow_atlas_class = 0;
static short shadow_atlas_count = 0;
static short shadow_atlas_region = NONE;
static D3DSurface *shadow_atlas_surfaces[SHADOW_ATLAS_CLASSES][2];
static struct { real_matrix4x3 matrix; real_rgb_color color; real radius; boolean used; }
    shadow_atlas_shadows[SHADOW_ATLAS_REGIONS];
static real_rgb_color local_shadow_color;
static real local_object_bounding_radius;
static real_matrix4x3 local_shadow_matrix;
static boolean shadow_used = FALSE;
static short shadow_atlas_capacity(short atlas_class) { (void)atlas_class; return 14; }
static void shadow_caster_target(short region) { (void)region; }
static long halo_shadow_map_scale(void) { return 4; }
static unsigned long d3d8_target_identity(void *surface) { (void)surface; return 7; }
int config_boolean(char const *name) { (void)name; return 0; }
unsigned long config_changes(void) { return 0; }
void platform_log(char const *format, ...) { (void)format; }
"""
    start = code.index("static boolean shadow_retained_on = FALSE;")
    end = code.index("int config_boolean(char const *name);", start)
    source += code[start:end]
    for name in ("shadow_retained_forget", "rasterizer_shadow_retained_new_map", "shadow_retained_frame_begin",
                 "rasterizer_shadow_retained_caster"):
        source += function(code, name) + "\n"
    source += r"""
/* a frame with one caster, the object 5, whose draws hash to 99: whether its
region was kept */
static boolean frame(boolean enumerating) {
    boolean kept;

    render_enumerating = enumerating;
    shadow_atlas_count = 0;
    shadow_retained_frame_begin();
    if (!shadow_retained_on)
        return FALSE;
    shadow_fingerprinting = TRUE;
    shadow_caster_object = 5;
    shadow_fingerprint = 99;
    kept = rasterizer_shadow_retained_caster();
    shadow_used = TRUE;
    shadow_atlas_owners[shadow_atlas_region].used = shadow_used;
    return kept;
}
int main(void) {
    /* (the game's frames: drawn once, then kept) */
    assert(!frame(FALSE));
    assert(frame(FALSE));
    assert(frame(FALSE));
    /* a new map: drawn again */
    rasterizer_shadow_retained_new_map();
    assert(!frame(FALSE));
    assert(frame(FALSE));
    /* the map's preparation, then the game's first frame: drawn there */
    assert(!frame(TRUE));
    assert(!frame(FALSE));
    assert(frame(FALSE));
    return 0;
}
"""
    path = tmp_path / "retained.c"
    path.write_text(source)
    binary = tmp_path / "retained"
    subprocess.run([compiler, "-std=c99", "-Wall", "-Werror", "-Wno-unused-variable", "-Wno-unused-function",
                    str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    # Kept through the preparation (its guard taken out), the caster it drew
    # is kept in the game's first frame.
    guard = " && !render_enumerating"
    assert guard in source
    path.write_text(source.replace(guard, ""))
    subprocess.run([compiler, "-std=c99", "-w", str(path), "-o", str(binary)], check=True)
    broken = subprocess.run([str(binary)], capture_output=True)
    assert broken.returncode != 0
