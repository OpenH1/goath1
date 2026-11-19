"""The game's small targets sized for the Xbox's 640x480 screen (the motion
sensor's 64x64 blips, the water's 128x128 ripples) are drawn at the display's
own pixels: halo_target_detail_scale (port/linux/src/d3d8_device.c). The
Xbox's size at 640x480 and below; the display's lines a unit above it, not the
3D view's of display.temporal_aa or supersampling; a side a multiple of 8 (its
three mip levels past the first halve exactly); at most 2048 pixels a side.
The negative control: the fixed size of before fails it."""
from pathlib import Path
import shutil
import subprocess

import pytest

from test_death_timing import function

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r"""
#include <stdio.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
static float screen_scale[2] = { 1.0f, 1.0f };
static float screen_supersampling = 1.0f;
static struct { BOOL rendering; float factor; } temporal = { FALSE, 1.0f };
static long halo_screen_width(void) { return 640; }
#define TARGET_DETAIL_STEP 8
#define TARGET_DETAIL_LARGEST 2048
"""

MAIN = r"""
/* pixels of a side at a display of `lines` (of each of the 480) */
static long pixels(int side, float lines, float supersampling, float temporal_factor)
{
    screen_supersampling = supersampling;
    temporal.rendering = temporal_factor < 1.0f;
    temporal.factor = temporal_factor;
    screen_scale[0] = screen_scale[1] = lines * supersampling * temporal_factor;
    return (long)((float)side * halo_target_detail_scale(side) + 0.5f);
}

int main(void)
{
    printf("%ld %ld\n", pixels(64, 1.0f, 1.0f, 1.0f), pixels(128, 1.0f, 1.0f, 1.0f));
    printf("%ld %ld\n", pixels(64, 0.75f, 1.0f, 1.0f), pixels(128, 0.75f, 1.0f, 1.0f));
    printf("%ld %ld\n", pixels(64, 1080.0f / 480.0f, 1.0f, 1.0f), pixels(128, 1080.0f / 480.0f, 1.0f, 1.0f));
    printf("%ld %ld\n", pixels(64, 2160.0f / 480.0f, 1.0f, 1.0f), pixels(128, 2160.0f / 480.0f, 1.0f, 1.0f));
    printf("%ld %ld\n", pixels(64, 2160.0f / 480.0f, 2.0f, 0.5f), pixels(128, 2160.0f / 480.0f, 2.0f, 0.5f));
    printf("%ld %ld\n", pixels(64, 1440.0f / 480.0f, 1.0f, 1.0f), pixels(128, 1440.0f / 480.0f, 1.0f, 1.0f));
    printf("%ld %ld\n", pixels(64, 25.0f, 1.0f, 1.0f), pixels(128, 25.0f, 1.0f, 1.0f));
    return 0;
}
"""

EXPECTED = [
    "64 128",     # 640x480: the Xbox's
    "64 128",     # a smaller window: never fewer
    "144 288",    # 1080p
    "288 576",    # 4K
    "288 576",    # 4K drawn at half by temporal_aa, supersampled twice: the display's
    "192 384",    # 1440p
    "1600 2048",  # memory's guard
]


def run(tmp_path, body):
    compiler = shutil.which("cc")
    if not compiler:
        pytest.skip("a C compiler is needed")
    program = tmp_path / "detail.c"
    program.write_text(PRELUDE + body + MAIN)
    binary = tmp_path / "detail"
    subprocess.run([compiler, "-std=c99", str(program), "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout.split("\n")[:-1]


def rule():
    return function((ROOT / "port/linux/src/d3d8_device.c").read_text(), "halo_target_detail_scale") + "\n"


def test_targets_at_the_display_pixels(tmp_path):
    lines = run(tmp_path, rule())
    assert lines == EXPECTED
    for line in lines[2:]:
        assert all(int(side) % 8 == 0 for side in line.split())


def test_negative_control_fixed_size_fails(tmp_path):
    fixed = "float halo_target_detail_scale(int side) { (void)side; return 1.0f; }\n"
    assert run(tmp_path, fixed) != EXPECTED
