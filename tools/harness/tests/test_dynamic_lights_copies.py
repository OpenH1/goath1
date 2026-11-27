"""A light's copies on its other markers keep their keys from frame to frame (port/linux/game/dynamic_lights.c)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

CASES = ["stable-as-others-come-and-go", "distinct-copies", "unknown-light-unkeyed"]
CONTROLS = {
    # (numbered among the frame's copies, as before: another light's copies shift them)
    "numbered-in-frame": (("dynamic_lights.copies[copy_index].number) : 0;", "copy_index + 1) : 0;"),
                          "stable-as-others-come-and-go"),
    "number-dropped": (("dynamic_lights.copies[copy_index].number) : 0;", "1) : 0;"), "distinct-copies"),
}


def generated(control=None):
    source = read("port/linux/game/dynamic_lights.c")
    lights = read("port/linux/include/halo_dynamic_lights.h")
    defines = "\n".join(re.findall(r"^#define HALO_DYNAMIC_LIGHTS_(?:CLUSTERS|MAXIMUM) .+$", lights, re.M)) + "\n" + \
        re.search(r"^#define DYNAMIC_LIGHTS_KEY_GAME .+?^#define DYNAMIC_LIGHTS_COPY_KEY.+?\n.+?\n", source,
                  re.M | re.S).group(0)
    table = re.search(r"^static struct\n\{\n\t/\* for each rasterizer light this frame.*?^\} dynamic_lights;", source,
                      re.M | re.S).group(0)
    code = "\n".join([structure(lights, "halo_dynamic_light"), table, function(source, "dynamic_lights_clusters"),
                      function(source, "dynamic_lights_copy"), function(source, "dynamic_lights_copies")])
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", defines + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("dynamic_lights_copies", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("dynamic_lights_copies", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"
