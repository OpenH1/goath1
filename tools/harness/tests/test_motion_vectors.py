"""display.temporal_aa's motion (port/linux/game/motion_vectors.c, docs/temporal-aa.md): the whole file against a small
fake renderer, with the vertex shader's two runs (this frame's registers and the frame before's) done in the test. A
point's motion is where it was drawn the frame before less where it is now: the camera's (the level's reprojection from
the depth, and the first-person weapon's own projection), a rigid object's, a skinned one's (two nodes, and a part's
palette); a teleported object and a camera cut have none; off, nothing."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["camera", "rigid", "skinned", "cut", "off"]
GAME = "port/linux/game/motion_vectors.c"
CONTROLS = [
    # the camera's change the wrong way about: the frame before's projection not the frame before's
    ("sum += inverse[row][inner] * before[inner][column];", "sum += before[row][inner] * inverse[inner][column];",
     "camera"),
    # an object's nodes never kept: its frame before's this frame's
    ("\t\tentry->previous = entry->current;\n\t\tentry->current = swap;", "\t\t(void)swap;", "rigid"),
    # a part's nodes taken in order, not its palette's
    ("short index = palette ? (short)palette[node] : node;", "short index = node;", "skinned"),
    # a teleport kept as motion
    ("\t\tif (dx * dx + dy * dy + dz * dz > MOTION_TELEPORT_DISTANCE * MOTION_TELEPORT_DISTANCE)\n"
     "\t\t\tentry->history = FALSE;\n\t}\n\tmotion_vectors.drawing",
     "\t\t(void)dx; (void)dy; (void)dz;\n\t}\n\tmotion_vectors.drawing", "cut"),
    # a camera cut kept as motion
    ("return dx * dx + dy * dy + dz * dz > MOTION_TELEPORT_DISTANCE * MOTION_TELEPORT_DISTANCE ||",
     "return dx * dx + dy * dy + dz * dz < 0.0f ||", "cut"),
    # on whatever the setting says
    ("!strcmp(setting, \"metalfx\") ? 3 : 0;", "!strcmp(setting, \"metalfx\") ? 3 : 1;", "off"),
]


def generated(fault=None):
    code = read(GAME)
    if fault:
        code = mutated(code, *fault)
    # (the game's headers left out: the test's fakes stand for them)
    code = re.sub(r'^#include ".*"$', "", code, flags=re.M)
    return (("motion_vectors.h", read("port/linux/game/motion_vectors.h")), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("motion_vectors", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("motion_vectors", generated((before, after))), case)
    assert status == CHECK_FAILED, output
