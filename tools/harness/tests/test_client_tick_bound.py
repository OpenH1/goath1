"""The one bound on a tick another machine states (player_queues_new.c's distributed_tick_plausible and
distributed_tick_stuck), where a client player's latest input, sub-step record and live control are kept: one stamped
far ahead is not taken, one kept that no clock could have run gives way to the next real one, and in-order ticks, a
clock leading by its trip and the host's own update keys go as before."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure

QUEUES = "source/game/player_queues_new.c"
SUBSTEPS = "port/linux/game/network_substeps.c"
CASES = ["input-far-future", "input-stuck", "input-legit", "input-drift", "substep-far-future", "substep-stuck",
         "substep-legit", "live-far-future", "live-stuck", "live-legit"]

# a fault in the bound and the case that must catch it
NEGATIVE_CONTROLS = {
    # no window: a tick far ahead taken, every real one after it refused
    "no-window": (("return tick >= now - DISTRIBUTED_TICK_WINDOW_TICKS && tick <= now + DISTRIBUTED_TICK_WINDOW_TICKS;",
                   "return TRUE;"), "input-far-future"),
    # no escape: one kept within the window but ahead of any clock holds every real one off
    "no-escape": (("return last > now + DISTRIBUTED_TICK_LEAD_TICKS && tick <= now + DISTRIBUTED_TICK_LEAD_TICKS;",
                   "return FALSE;"), "input-stuck"),
    # an escape that ignores where the new tick is: a reordered input of a client drifted ahead taken
    "escape-any": (("return last > now + DISTRIBUTED_TICK_LEAD_TICKS && tick <= now + DISTRIBUTED_TICK_LEAD_TICKS;",
                    "return last > now + DISTRIBUTED_TICK_LEAD_TICKS;"), "input-drift"),
    # the tick after an escape bringing the buttons of the ticks "missed" back to the stuck one
    "escape-count": (("if (!*valid || tick <= *last)\n\t\tcount = 1;", "if (!*valid)\n\t\tcount = 1;"),
                     "input-stuck"),
    # the sub-steps' record not bounded
    "substep-unbounded": (("if (now != NONE && !distributed_tick_plausible(record->key, now))\n\t\treturn;", ""),
                          "substep-far-future"),
    # the live controls' not bounded
    "live-unbounded": (("if (now != NONE && !distributed_tick_plausible(live->key, now))\n\t\treturn FALSE;", ""),
                       "live-far-future"),
}


def generated(fault=None):
    queues = read(QUEUES)
    substeps = read(SUBSTEPS)
    header = read("source/game/player_queues_new.h")
    code = "\n".join(function(queues, name) for name in ("distributed_tick_plausible", "distributed_tick_stuck",
                                                          "distributed_add_new_ticks"))
    code += "\n" + "\n".join(function(substeps, name) for name in ("live_control_later", "substep_record_heard",
                                                                   "live_control_heard"))
    if fault:
        code = mutated(code, *fault)
    later = constant(read("port/linux/game/sim_rate.h"), "MAXIMUM_SIM_SUBSTEPS") - 1
    config = enum_with(header, "DISTRIBUTED_TICK_WINDOW_TICKS =") + "\n"
    config += f"enum {{ MAXIMUM_LATER_SUBSTEPS = {later} }};\n"
    for name in ("substep_control", "substep_record", "live_control"):
        config += structure(substeps, name) + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("client_tick_bound", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("client_tick_bound", generated(fault)), case)
    assert status == CHECK_FAILED, output
