"""The lightmap pass's triangles parted by the enhanced dynamic lights' reach (port/linux/src/d3d8_device.c's
lights_reach_part): the lights found once for each run of triangles, and every triangle in the same part and order
as testing it against every light gave, on random soups and light sets, at a reach's edge and with positions that
are no number; with the runs' boxes made as it parts and kept (lights_reach_list_find), a kept list found again
under other lights, made again once its vertices are written, and dropped as the structure changes. With negative
controls."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

DEVICE = "port/linux/src/d3d8_device.c"

NEGATIVE_CONTROLS = {
    # a run's lights found without those whose reach just touches the run's box
    "run-edge": ("edges", "if (run->unordered || !(distance > reach[3] * reach[3]))",
                 "if (run->unordered || distance < reach[3] * reach[3])"),
    # a kept list found again after its vertices were written (its boxes stale)
    "stale": ("cached", "if (memory_watch_generation(list->watched, list->watched_bytes) != list->generation)",
              "if (0)"),
}


def generated(fault=None):
    source = read(DEVICE)
    # (the split and the kept lists: from the run's size to the split's caller)
    start = source.index("/* the triangles a run's lights are found for at once (lights_reach_part) */")
    code = "\n\n".join([
        "#define XGPU_DYNAMIC_LIGHTS_MAXIMUM 128",
        "static struct { float reach[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4]; float reach_box[2][3]; int reach_count; } "
        "dynamic_lights;",
        source[start:source.index("/* the triangles of a list apart: FALSE if they cannot be told apart", start)]])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["random", "edges", "unordered", "cached", "speed"])
def test_case(case):
    status, output = run(build("lights_reach_part", generated()), case)
    assert status == 0, output
    if output:
        print(output)


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("lights_reach_part", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_maximum_matches():
    """the test's most lights are the game's"""
    assert "#define XGPU_DYNAMIC_LIGHTS_MAXIMUM 128" in read("port/linux/src/xgpu.h")
