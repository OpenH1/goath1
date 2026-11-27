"""Between frames, render.camera and render.frustum are the first player window's view (source/render/render.c
render_frame), not the console window's drawn after it nor a Spartan link feed's."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["player-view-after-console", "player-view-after-feeds", "split-screen-first-player", "no-player-window"]
CONTROLS = {
    # (the last window's camera left in the globals, as before)
    "not-restored": (("\t\trender.camera = player_camera;\n\t\trender.frustum = player_frustum;\n", "\t\t;\n"),
                     "player-view-after-console"),
    # (the last player window's kept: a split screen's second player)
    "last-player-kept": (("\tif (*viewed)\n\t\treturn;\n", ""), "split-screen-first-player"),
}


def generated(control=None):
    source = read("source/render/render.c")
    code = "static void render_frame_player_viewed(struct render_camera *camera, struct render_frustum *frustum, " \
        "boolean *viewed);\n" + function(source, "render_frame") + "\n" + function(source, "render_frame_player_viewed")
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("render_frame_camera", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("render_frame_camera", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"
