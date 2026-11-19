"""MCC's Zoom In and Zoom Out (controls.zoom_in, controls.zoom_out, a custom controller layout's ZOOM IN and ZOOM
OUT): source/game/player_control.c's player_control_zoom_input and source/items/weapons.c's weapon_step_zoom_level step
the zoom in to the weapon's last level and out to none, never wrapping, beside the original's cycle, which is left as it
was. Every level they give is one the cycle gives, and they change only the player's control's zoom level, which the
player's action carries as the cycle's (desired_zoom_level): no change to what the network sends."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["steps_in_and_out_and_stop", "cycle_unchanged_and_levels_the_cycles"]

CONTROLS = [
    # Zoom In wrapping at the last level, as the cycle does
    ("if (direction > 0 && zoom_level < last)\n\t\t\tzoom_level++;",
     "if (direction > 0)\n\t\t\tzoom_level = zoom_level < last ? zoom_level + 1 : NONE;", "steps_in_and_out_and_stop"),
]


def generated(fault=None):
    code = "\n".join((function(read("source/items/weapons.c"), "weapon_rotate_zoom_level"),
                      function(read("source/items/weapons.c"), "weapon_step_zoom_level"),
                      function(read("source/game/player_control.c"), "player_control_zoom_input"))) + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_zoom_step(case):
    status, output = run(build("zoom_step", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_zoom_step_negative_control(before, after, case):
    status, output = run(build("zoom_step", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_only_the_control_zoom_level_changes_and_the_action_carries_it():
    code = read("source/game/player_control.c")
    handle = function(code, "handle_one_player_input")
    # the presses set the player's control's zoom level alone, where the cycle did
    assert re.search(r"player->zoom_level = player_control_zoom_input\(\s*current_weapon_index,\s*"
                     r"player->zoom_level,\s*input\.player_control_flags\);", handle)
    assert "weapon_rotate_zoom_level(" not in handle
    # which the action sends as before (network_distributed.c relays desired_zoom_level unchanged)
    assert "action.desired_zoom_level = player->zoom_level;" in code
