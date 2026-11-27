"""Bounced light's placement, easing and choice (port/linux/game/dynamic_lights_bounce.c)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

CASES = ["placed-off-wall", "out-of-reach", "albedo-tints", "radius-clamped", "steady", "fades-with-source",
         "new-wall-starts-dark", "match-nearest", "choose-strongest"]
CONTROLS = {
    "into-the-wall": (("point->x + normal->i * offset", "point->x - normal->i * offset"),
                      "placed-off-wall"),
    "no-falloff": (("falloff = window * window;", "falloff = 1.0f;"), "placed-off-wall"),
    "no-albedo": ((" * albedo[channel] *", " *"), "albedo-tints"),
    "carried-to-new-wall": (("\t\t\tslot->color[0] = slot->color[1] = slot->color[2] = 0.0f;\n", "\t\t\t;\n"),
                            "new-wall-starts-dark"),
    "never-fades": (("slot->color[channel] *= 1.0f - ease;", "slot->color[channel] *= 1.0f;"), "fades-with-source"),
    "models-only-bounce": (("(light->flags & (HALO_DYNAMIC_LIGHT_MODELS_ONLY | HALO_DYNAMIC_LIGHT_FLASHLIGHT)) ||", "0 ||"),
                           "choose-strongest"),
    "match-taken": (("if (!tracks[index].used || taken[index])", "if (!tracks[index].used)"), "match-nearest"),
}


def generated(control=None):
    source = read("port/linux/game/dynamic_lights_bounce.c")
    lights = read("port/linux/include/halo_dynamic_lights.h")
    defines = "\n".join(re.findall(r"^#define BOUNCE_\w+ .+$", source, re.M))
    flags = re.search(r"^enum\n\{\n\t/\* the structure is lit.*?^\};", lights, re.M | re.S).group(0)
    code = "\n".join([structure(lights, "halo_dynamic_light"), flags, structure(source, "bounce_slot"),
                      structure(source, "bounce_target"), structure(source, "bounce_track"),
                      function(source, "dynamic_lights_bounce_place"), function(source, "dynamic_lights_bounce_ease"),
                      function(source, "dynamic_lights_bounce_match"), function(source, "dynamic_lights_bounce_choose")])
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", defines + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("dynamic_lights_bounce", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("dynamic_lights_bounce", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"
