"""A frame nobody sees waits for the clock's next step (unseen_frames.c): how long; in an OpenH1 network game, before
its reads, a host reading what comes meanwhile as it comes and never past the step; and which machines wait so
(network_clock.c: an OpenH1 game's host and clients, not a Legacy game's, not with debug.network_timing_before)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["deadline", "sleep", "receive", "receive-short", "who"]

NEGATIVE_CONTROLS = {
    # nothing read in the wait: what comes waits for the step
    "no-receive": (("if (receive && deadline - now >= 2000000ULL", "if (0 && deadline - now >= 2000000ULL"),
                   "receive"),
    # the time since the last wait not taken off: each frame a step late
    "no-time-since": (("if (last && now > last)", "if (0)"), "deadline"),
    # debug.network_timing_before not looked at
    "no-before-setting": (('network_standard_agreed_rate() && !config_boolean("debug.network_timing_before")',
                           "network_standard_agreed_rate()"), "who"),
    # a Legacy game's frames waited so too
    "legacy-too": (('network_standard_agreed_rate() && !config_boolean("debug.network_timing_before")',
                    '!config_boolean("debug.network_timing_before")'), "who"),
}


def generated(fault=None):
    unseen = read("port/linux/game/unseen_frames.c")
    clock = read("port/linux/game/network_clock.c")
    code = "\n".join([function(unseen, "unseen_frame_deadline"), function(unseen, "unseen_frame_wait_until"),
                      function(clock, "network_clock_unseen_wait_first")])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("unseen_wait", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("unseen_wait", generated(fault)), case)
    assert status == CHECK_FAILED, output
