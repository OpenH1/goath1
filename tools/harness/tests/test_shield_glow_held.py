"""A model's shield region's glow: a Jackal's hand-held shield, a field generator (port/linux/game/shield_glow.c)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

CASES = ["jackal", "destroyed", "off", "no-shield", "untinted", "standing-radius", "camouflage"]
CONTROLS = {
    "destroyed-glows": (("TEST_FLAG(object->object.regions_destroyed_flags, region_index) ||", "FALSE ||"),
                        "destroyed"),
    "off-glows": (("if (permutation->name[0] == '~')", "if (FALSE)"), "off"),
    "root-node": (("candidate->node_index = instance->node_index;", "candidate->node_index = 0;"), "jackal"),
    "any-instance": (("if (!found || instance->region_index == region_index)", "if (!found)"), "jackal"),
}


def generated(control=None):
    source = read("port/linux/game/shield_glow.c")
    defines = "\n".join(re.findall(r"^#define SHIELD_GLOW_\w+ .+$", source, re.M))
    code = structure(source, "shield_glow_candidate") + "\n" + function(source, "shield_glow_held")
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", defines + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("shield_glow_held", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("shield_glow_held", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"
