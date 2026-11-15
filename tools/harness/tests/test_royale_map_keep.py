"""The real rule keeping Royale's players on the island (port/linux/game/royale_map.c's royale_map_keep_point): out of
a blocked entrance by its nearest side, back to the last point inside from out of bounds or the sea."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run  # noqa: E402

CASES = ["blocked", "bounds"]

# a fault in the rule, and the case that must catch it
NEGATIVE_CONTROLS = {
    "pushed-out-the-far-side": (("point->y = bounds->minimum.y - ROYALE_MAP_PUSH_MARGIN;",
                                 "point->y = bounds->maximum.y + ROYALE_MAP_PUSH_MARGIN;"), "blocked"),
    "sea-not-checked": (("point->z > bounds->minimum.z - margin && ", ""), "bounds"),
}


def generated(fault=None):
    source = read("port/linux/game/royale_map.c")
    body = "\n\n".join([enum_with(source, "_royale_map_kept_inside"),
                        function(source, "royale_map_inside"), function(source, "royale_map_push_out"),
                        function(source, "royale_map_keep_point")])
    if fault:
        body = mutated(body, *fault)
    return (("generated.inc", body),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_map_keep", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_map_keep", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_map_keep with a fault ({control}) passed '{case}': the test cannot see it"
