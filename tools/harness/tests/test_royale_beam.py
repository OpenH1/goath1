"""The real sweep of Royale's glassing beam (port/linux/game/royale_beam.c): where the front is at a tick (each stage
holds, then moves), how far a point is behind it (out of the bounds counts as behind), the damage a player takes
standing still, and the side drawn from the game's seed."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

CASES = ["hold", "move", "end", "depth", "damage", "side"]

# the code taken apart, and the case that must catch each fault
NEGATIVE_CONTROLS = {
    # the hold skipped: the front moves from the first tick
    "no-hold": (("if (ticks < hold)", "if (FALSE)"), "hold"),
    # the move's progress from the wrong end
    "move-backward": (("stage->start + (stage->end - stage->start) * (real)ticks / (real)move",
                       "stage->end - (stage->end - stage->start) * (real)ticks / (real)move"), "move"),
    # the front reset after the last stage
    "end-reset": (("status->front = stage->end;\n\t\tstatus->target = stage->end;",
                   "status->front = stages[0].start;\n\t\tstatus->target = stage->end;"), "end"),
    # the front measured from the far side
    "depth-flipped": (("depth = front * length - from_side;", "depth = from_side - front * length;"), "depth"),
    # out of the bounds forgotten
    "no-outside": (("return MAX(depth, outside);", "return depth + 0.0f * outside;"), "depth"),
    # the config's damage ignored (the defaults' only)
    "default-damage-only": (("stage->damage_per_second > 0.0f ?", "FALSE ?"), "damage"),
    # the seed ignored: always the first side
    "side-unseeded": (("return allowed[hash % (unsigned long)count];", "return allowed[0];"), "side"),
}


def generated(fault=None):
    header = read("port/linux/game/royale.h")
    source = read("port/linux/game/royale_beam.c")
    side_enum = header[header.index("enum royale_side"):header.index("};", header.index("enum royale_side")) + 2]
    defaults = re.search(r"static real const royale_beam_default_damage\[[^;]*;", source).group(0)
    code = "\n".join([
        side_enum,
        structure(header, "royale_bounds"),
        structure(header, "royale_beam_stage"),
        structure(source, "royale_beam_status"),
        defaults,
        function(source, "royale_beam_status_at"),
        function(source, "royale_beam_depth_at"),
        function(source, "royale_beam_choose_side"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_beam", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_beam", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_beam with a fault ({control}) passed '{case}': the test cannot see it"
