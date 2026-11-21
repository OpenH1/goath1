"""The host's relayed input to an OpenH1 client on a change only (distributed_relayed_action_due): an unchanged input
goes only as its refresh; a button, a choice and the end of the player's forwards at once; a change of facing, sticks
or trigger, or sub-steps that changed anything, on the plan's next tick and again on the one after (a facing that
changes every tick goes on the plan's ticks only, as before); one lost datagram heals within two of the plan's
periods, two within the refresh (RELAYED_ACTION_REFRESH_TICKS)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["unchanged", "change", "moving", "lost", "lost-twice", "pressed", "substeps", "covered"]

NEGATIVE_CONTROLS = {
    # sent on every tick of the plan, changed or not (as before)
    "every-planned-tick": (("else if (planned && sent->repeat)", "else if (planned)"), "unchanged"),
    # no refresh: a change whose both sends were lost never heals
    "no-refresh": (("else if (now - sent->time < RELAYED_ACTION_REFRESH_TICKS)", "else if (TRUE)"), "lost-twice"),
    # no second send: one lost change waits for the refresh
    "no-repeat": (("else if (planned && sent->repeat)", "else if (FALSE)"), "lost"),
    # the facing's change not seen
    "yaw-ignored": (("action->yaw != sent->action.yaw || ", ""), "change"),
    # a change of facing sent at once, every tick: more than the plan sent before
    "facing-at-once": (("if (at_once || (planned && moved))", "if (at_once || moved)"), "moving"),
    # sub-steps that changed not seen: their record waits for another change
    "substeps-ignored": (("sent->action.primary_trigger || substeps;", "sent->action.primary_trigger;"), "substeps"),
    # sub-steps sent at once, off the plan's ticks: more than the plan sent before
    "substeps-at-once": (("boolean at_once = pressed ||", "boolean at_once = pressed || substeps ||"), "substeps"),
    # the input after a sub-steps' record not repeated: the client replays the record until the refresh
    "record-replayed": (("else if (planned && sent->repeat)", "else if (FALSE && sent->repeat)"), "substeps"),
    # the forwards' end not seen
    "covered-ignored": (("!sent->valid || sent->covered ||", "!sent->valid ||"), "covered"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "distributed_relayed_action_due")
    if fault:
        code = mutated(code, *fault)
    config = structure(source, "distributed_relayed_action") + "\n"
    config += re.search(r"static (struct distributed_relayed_sent\s*\{.*?\n\} distributed_relayed_sent\[.*?\];)",
                        source, re.S)[1] + "\n"
    config += "enum { RELAYED_ACTION_REFRESH_TICKS = %s };\n" % re.search(
        r"RELAYED_ACTION_REFRESH_TICKS = ([^,]+),", source)[1]
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("relayed_action_changes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("relayed_action_changes", generated(fault)), case)
    assert status == CHECK_FAILED, output
