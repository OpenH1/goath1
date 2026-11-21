"""The host's states of a player at its sub-steps to a client only where they tell it more than the last sent
(distributed_substep_state_unchanged): an idle player's only as the refresh, a moved, turned or other one at once."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["idle", "moved", "predicted"]

NEGATIVE_CONTROLS = {
    # sent every sub-step, changed or not (as before)
    "always": (("if (sent->time != NONE &&", "if (FALSE && sent->time != NONE &&"), "idle"),
    # no refresh: a lost state never sent again while the player stands
    "no-refresh": (("now - sent->time < SUBSTEP_STATE_REFRESH_TICKS &&", ""), "idle"),
    # the place not looked at
    "position-ignored": (("!csmemcmp(&sent->position, &state->position, sizeof(sent->position)) &&", ""), "moved"),
    # the facing not looked at
    "forward-ignored": (("!csmemcmp(&sent->forward, &state->forward, sizeof(sent->forward)) &&", ""), "moved"),
    # the client's tick not looked at
    "tick-ignored": (("(!predicted || sent->predicted_time == state->predicted_time) &&", ""), "predicted"),
    # the last sent not noted
    "not-noted": (("\tsent->time = now;\n", "\t(void)now;\n"), "idle"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "distributed_substep_state_unchanged")
    if fault:
        code = mutated(code, *fault)
    legacy = read("port/linux/game/network_legacy.h")
    config = structure(read("port/linux/game/network_distributed.h"), "distributed_vector") + "\n"
    config += re.search(r"enum\s*\{\s*/\* the player has a unit that is alive \*/.*?\};", legacy, re.S)[0] + "\n"
    config += "enum { SUBSTEP_STATE_REFRESH_TICKS = %s };\n" % re.search(
        r"SUBSTEP_STATE_REFRESH_TICKS = ([^,]+),", source)[1]
    config += structure(source, "distributed_substep_sent") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("substep_state_changes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("substep_state_changes", generated(fault)), case)
    assert status == CHECK_FAILED, output
