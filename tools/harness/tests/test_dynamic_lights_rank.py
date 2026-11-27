"""The scene's lights ranked past the rasterizer's 128, and those that cast ray-traced shadows
(port/linux/game/dynamic_lights.c)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, inline, mutated, read, run, structure

CASES = ["untouched-below", "near-and-wide-first", "behind-last", "stable-ties", "unknown-bright", "camera-inside",
         "over-capacity-untouched", "drawn-wins", "drawn-faded-low", "drawn-reused-slot",
         "shadow-small-skipped", "shadow-lasting", "shadow-unknown-skipped", "shadow-models-baked-skipped",
         "shadow-flashlight-skipped", "shadow-kept", "shadow-none"]
CONTROLS = {
    "rank-at-capacity": (("if (count <= MAXIMUM_LIGHTS_PER_WINDOW ||", "if (count < MAXIMUM_LIGHTS_PER_WINDOW ||"),
                         "untouched-below"),
    "ignore-radius": (("score = intensity * radius / (distance > radius ? distance : radius);",
                       "score = intensity / (distance > radius ? distance : radius);"), "near-and-wide-first"),
    "behind-ranked-alike": (("score *= RANK_BEHIND_SCALE;", "score *= 1.0f;"), "behind-last"),
    "ascending": (("return left->score > right->score ? -1 : 1;", "return left->score > right->score ? 1 : -1;"),
                  "near-and-wide-first"),
    "unstable-ties": (("return left->index - right->index;", "return right->index - left->index;"), "stable-ties"),
    "dark-unknown": (("intensity = 1.0f;", "intensity = 0.0f;"), "unknown-bright"),
    "ignore-drawn": (("lights_drawn[drawn].light == lights[index]", "FALSE"), "drawn-wins"),
    "drawn-any-handle": (("lights_drawn[drawn].light == lights[index]", "TRUE"), "drawn-reused-slot"),
    "shadow-any-reach": (("light->radius < SHADOWED_LEAST_RADIUS ||", "FALSE ||"), "shadow-small-skipped"),
    "shadow-no-age": (("now - since < SHADOWED_LEAST_AGE", "FALSE"), "shadow-lasting"),
    "shadow-baked-shadowed": (("|| baked[light_index])", ")"), "shadow-models-baked-skipped"),
    "shadow-flashlight-slot": (("(HALO_DYNAMIC_LIGHT_MODELS_ONLY | HALO_DYNAMIC_LIGHT_FLASHLIGHT)))",
                                "(HALO_DYNAMIC_LIGHT_MODELS_ONLY)))"), "shadow-flashlight-skipped"),
    "shadow-no-keep": (("score *= SHADOWED_KEEP;", "score *= 1.0f;"), "shadow-kept"),
    "shadow-keep-forever": (("score *= SHADOWED_KEEP;", "score *= 100.0f;"), "shadow-kept"),
}


def generated(control=None):
    source = read("port/linux/game/dynamic_lights.c")
    header = read("port/linux/include/halo_dynamic_lights.h")
    math = read("source/math/real_math.h")
    defines = "\n".join(re.findall(r"^#define (?:RANK|SHADOWED)_\w+ .+$", source, re.M)) + "\n" + \
        "\n".join(re.findall(r"^#define HALO_DYNAMIC_LIGHTS_SHADOWED .+$", header, re.M)) + "\n" + \
        structure(header, "halo_dynamic_light") + "\n" + enum_with(header, "HALO_DYNAMIC_LIGHT_MODELS_ONLY")
    helpers = "\n".join(inline(math, name) for name in ["vector_from_points3d", "dot_product3d", "magnitude_squared3d",
                                                         "magnitude3d", "scale_vector3d", "normalize3d"])
    table = re.search(r"^static struct\n\{\n\tlong light;.*?\} lights_drawn\[MAXIMUM_LIGHTS_PER_MAP\];", source,
                      re.M | re.S).group(0)
    history = re.search(r"^static struct\n\{\n\tstruct\n\t\{\n\t\tunsigned long long key;.*?\} shadowed_history;", source,
                        re.M | re.S).group(0)
    code = structure(source, "rank_entry") + "\n" + table + "\n" + function(source, "dynamic_lights_score") + "\n" + \
        function(source, "rank_compare") + "\n" + \
        function(source, "dynamic_lights_drawn") + "\n" + function(source, "dynamic_lights_rank") + "\n" + \
        history + "\n" + function(source, "dynamic_lights_shadow_pick")
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", defines + "\n" + helpers + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("dynamic_lights_rank", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("dynamic_lights_rank", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"
