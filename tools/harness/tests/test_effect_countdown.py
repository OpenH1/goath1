"""The player effects' countdowns (player_effects.c) as the ticks count them and the frames draw them at game.sim_rate
120: a screen flash drawn through its ticks and the rest of the tick it ran out at, never again after it."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["flash", "restarted"]
CONTROLS = [
    # an expired countdown drawn in every open tick's frames (build-2544's strobe)
    ("(ran_out && duration > 0.0f && *time_left > 0.0f)", "(duration > 0.0f && *time_left > 0.0f)", "flash"),
    # the flag kept from the tick it ran out at
    ("\tSET_FLAG(*flags, ran_out_bit, ran_out);\n", "\tif (ran_out) SET_FLAG(*flags, ran_out_bit, TRUE);\n", "flash"),
    # the open tick's step not counted: the tick it ran out at cut short
    ("\tif (sim_substep_next())\n\t\tticks_left++;\n", "", "flash"),
]


def generated(fault=None):
    effects = read("source/effects/player_effects.c")
    interpolation = read("port/linux/game/render_interpolation.c")
    code = "\n".join([
        function(interpolation, "render_interpolation_ticks_left"),
        function(effects, "player_effect_drawn"),
        function(effects, "player_effect_count_down"),
    ])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("effect_countdown", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("effect_countdown", generated((before, after))), case)
    assert status == CHECK_FAILED, output
