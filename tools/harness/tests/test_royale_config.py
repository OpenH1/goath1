"""The real Royale map config reader (port/linux/game/royale_config.c): a good file reads; a value out of range (an
infinity or a NaN, a box upside down, a structure BSP the scenario cannot have, a speed or a beam stage's duration of
nothing) refuses the whole file, so no machine plays a broken map."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, mutated, read, run, structure  # noqa: E402

CASES = ["good", "not-finite", "bounds", "structure-bsp", "speed-duration"]

# a check taken out, and the case that must catch it
NEGATIVE_CONTROLS = {
    "infinity-read": (("number - number != 0.0 || ", ""), "not-finite"),
    "upside-down-read": (("return bounds->minimum.x <= bounds->maximum.x && bounds->minimum.y <= bounds->maximum.y &&\n"
                          "\t\tbounds->minimum.z <= bounds->maximum.z;", "return bounds != NULL;"), "bounds"),
    "any-bsp-read": (("number != (real)(short)number || number < 0.0f ||", ""), "structure-bsp"),
    "zero-speed-read": (("&path->speed) && path->speed > 0.0f;", "&path->speed);"), "speed-duration"),
    "zero-duration-read": (("if (!(stage->duration > 0.0f) || ", "if ("), "speed-duration"),
}

LIMITS = ["ROYALE_MAXIMUM_FLIGHT_PATHS", "ROYALE_MAXIMUM_BEAM_STAGES", "ROYALE_MAXIMUM_LOOT_SPOTS",
          "ROYALE_MAXIMUM_POD_SPOTS", "ROYALE_MAXIMUM_BLOCKED_ENTRANCES"]
STRUCTURES = ["royale_bounds", "royale_flight_path", "royale_beam_stage", "royale_loot_spot",
              "royale_blocked_entrance", "royale_gulag_arena", "royale_map_config"]


def generated(fault=None):
    royale = read("port/linux/game/royale.h")
    body = read("port/linux/game/royale_config.c")
    body = re.sub(r'^#include [<"].*[>"]$', "", body, flags=re.M)
    body = body.replace('#include "royale_maps.inc"', "")
    if fault:
        body = mutated(body, *fault)
    head = "".join(re.search(r"^#define " + name + r" .*$", royale, re.M).group(0) + "\n" for name in LIMITS)
    head += enum_with(royale, "ROYALE_SIDE_NORTH = 0") + "\n" + enum_with(royale, "ROYALE_LOOT_COMMON = 0") + "\n"
    head += "\n".join(structure(royale, name) for name in STRUCTURES) + "\n"
    head += "#include <math.h>\nstatic struct { char const *name; char const *text; } const royale_maps[] = " \
            "{ { NULL, NULL } };\n"
    return (("royale_config.inc", head + body),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_config", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_config", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_config with a fault ({control}) passed '{case}': the test cannot see it"
