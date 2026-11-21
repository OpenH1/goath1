"""A step of the game's clock that falls due in a seen frame's wait for the frames' limit runs then, before the
frame's time (sdl_platform.c's platform_frame_wait_until, unseen_frames.c's frame_step_wake): the wait wakes at the
step and goes on to the frame's time after, its grid kept; a step due after the frame's time, or too near it for a
frame to run it first, is left for the frame; and which machines step so (network_clock.c: an OpenH1 game's host and
clients whose frames are seen, not a Legacy game's; always, debug.network_timing_before or not)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ["step-first", "step-due", "step-later", "wake", "who"]

NEGATIVE_CONTROLS = {
    # the wait not woken for the step: it runs at the next frame's start, after the frame's time
    "no-wake": (("if (step && step < *next_frame)", "if (0 && step < *next_frame)"), "step-first"),
    # the step's frame's cost not counted: it runs past the frame's time, and the frame is late
    "no-cost": (("if (wake + cost >= frame_due)", "if (wake >= frame_due)"), "wake"),
    # the old switch back: debug.network_timing_before turning steps when due off
    "before-setting-back": (("network_standard_agreed_rate() && !halo_frames_unseen();",
                             'network_standard_agreed_rate() && !halo_frames_unseen() && '
                             '!config_boolean("debug.network_timing_before");'), "who"),
    # a Legacy game's frames stepped so too
    "legacy-too": (("network_standard_agreed_rate() && !halo_frames_unseen()", "!halo_frames_unseen()"), "who"),
}


def generated(fault=None):
    platform = read("port/linux/src/sdl_platform.c")
    unseen = read("port/linux/game/unseen_frames.c")
    clock = read("port/linux/game/network_clock.c")
    code = "\n".join([function(platform, "platform_frame_wait_until"), function(unseen, "frame_step_wake"),
                      function(clock, "network_clock_steps_when_due")])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("frame_step_wait", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("frame_step_wait", generated(fault)), case)
    assert status == CHECK_FAILED, output
