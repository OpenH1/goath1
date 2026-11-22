"""The enhanced dynamic lights' grid (port/linux/src/d3d8_device.c's dynamic_lights_scales, dynamic_lights_edges and
dynamic_lights_mark): every light that reaches a point, as the shaders add it, is in the mask of the cell the shaders
put the point in, on random light sets, at the lights' and the cells' edges and past the grid's reach; the most
lights' masks built fast. With negative controls. LIGHTS_DUMP=<file> (a dump of the game's lights and its lightmap
pass's triangles) also prints the lights a lit point's cell has, before and now, against those reaching it."""

import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

DEVICE = "port/linux/src/d3d8_device.c"

NEGATIVE_CONTROLS = {
    # the sphere taken as the light's radius a little short
    "short-reach": ("random", "reach = LIGHT_REACH(radius), reach_squared",
                    "reach = radius * 0.999f, reach_squared"),
    # the grid's outer cells as wide as their own edges, not the world past the grid's reach
    "outer-cells": ("far", "upper[cell] = cell == count - 1 ? INFINITY :", "upper[cell] = 0 ? INFINITY :"),
    # a light's distance from a cell's box below it measured from the box's far side
    "box-distance": ("random", "offset[axis] > high ? offset[axis] - high : 0.0f",
                     "offset[axis] > high ? offset[axis] - low : 0.0f"),
}


def generated(fault=None):
    source = read(DEVICE)
    xgpu = read("port/linux/src/xgpu.h")
    defines = [line for line in xgpu.splitlines() if line.startswith((
        "#define XGPU_DYNAMIC_LIGHTS_MAXIMUM ", "#define XGPU_DYNAMIC_LIGHTS_GRID_", "#define XGPU_DYNAMIC_LIGHTS_CELL ",
        "#define XGPU_DYNAMIC_LIGHTS_REACH "))]
    own = [line for line in source.splitlines() if line.startswith((
        "#define LIGHT_REACH(", "#define DYNAMIC_LIGHTS_LEAST_REACH ", "#define DYNAMIC_LIGHTS_EDGE_MARGIN "))]
    code = "\n\n".join([
        "\n".join(defines + own),
        structure(read("port/linux/include/halo_dynamic_lights.h"), "halo_dynamic_light"),
        function(source, "dynamic_lights_scales"), function(source, "dynamic_lights_edges"),
        function(source, "dynamic_lights_cell"), function(source, "dynamic_lights_mark")])
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", ["random", "far", "speed"])
def test_case(case):
    status, output = run(build("dynamic_lights_cells", generated()), case)
    assert status == 0, output
    if output:
        print(output)


@pytest.mark.skipif(not os.environ.get("LIGHTS_DUMP"), reason="no dump of the game's lights (LIGHTS_DUMP)")
def test_dump():
    status, output = run(build("dynamic_lights_cells", generated()), "dump")
    assert status == 0, output
    print(output)


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("dynamic_lights_cells", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"
