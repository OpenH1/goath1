"""The game's frames with VRR (frame_pacing.h, HOST_RENDER_DISPLAY_VARIABLE): a model of the game, the GPU and a
variable display, each frame started as host_render_stream.c's display_wait starts it. At the cap whenever the longer
of the game's and the GPU's times fits it, not at their sum (a beta's 4K VRR log: 70 to 100 frames a second under a cap
of 135), at the GPU's rate where it does not, and each frame reaching the GPU as it is free, not a frame early."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run

SOURCE = "port/linux/src/frame_pacing.h"
CASES = ["cap", "gpu", "game"]

# a fault in the code under test and the case that must catch it
NEGATIVE_CONTROLS = {
    # one frame waiting for the display, as bounded: the game waits for the GPU to finish the frame before (the sum)
    "serialized": (("#define FRAME_PACING_VARIABLE_QUEUED 2", "#define FRAME_PACING_VARIABLE_QUEUED 1"), "cap"),
    # a frame started once the GPU is predicted free, not its lead before: the game and the GPU take turns
    "after-the-gpu": (("return free > lead ? free - lead : 0;", "return free;"), "cap"),
    # started at once, not just in time: a whole GPU frame more from its start to the screen
    "at-once": (("return free > lead ? free - lead : 0;", "return 0;"), "gpu"),
}


def generated(fault=None):
    code = read(SOURCE)
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("vrr_pacing", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("vrr_pacing", generated(fault)), case)
    assert status == CHECK_FAILED, output
