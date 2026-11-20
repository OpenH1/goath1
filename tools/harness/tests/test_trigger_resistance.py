"""A DualSense's firing trigger resistance (input.trigger_resistance, trigger_resistance.c): the effect report's bytes
(right, left, off), the trigger bound to fire by each player's layout while they play, and what is sent as the player
pauses, dies, splits the screen, or the controller leaves and comes back; with the port's own code."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["report_bytes", "fire_trigger", "off_sends_nothing", "not_dualsense", "menu_clears", "death_clears",
         "split_screen", "reconnect"]

START = "/* ---------- a DualSense's firing trigger resistance"
END = "/* reads a controller's state; for port 0"

# a fault in the code, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the triggers' effects in each other's place
    "triggers-swapped": ("input", ("TRIGGER_REPORT_LEFT : TRIGGER_REPORT_RIGHT", "TRIGGER_REPORT_RIGHT : TRIGGER_REPORT_LEFT"),
                         "report_bytes"),
    # the right trigger whatever the layout (a left-handed layout's left trigger left alone)
    "layout-ignored": ("remap", ("button = remap_ports[port].layout[PAD_FIRE];", "button = 7;"), "fire_trigger"),
    # the menus' context taken as play: a stiff trigger in the pause menu
    "menus-resist": ("remap", ("remap_ports[port].context != 1 && remap_ports[port].context != 2", "0"),
                     "menu_clears"),
    # a dead player's trigger kept stiff
    "death-ignored": ("input", ("if (wanted && controller_remap_alive((short)port))", "if (wanted)"),
                      "death_clears"),
}


def generated(fault=None):
    inputs = read("port/linux/src/xinput_sdl.c")
    section = inputs[inputs.index(START):inputs.index(END)]
    remap = function(read("port/linux/src/controller_remap_input.c"), "controller_remap_fire_trigger")
    if fault and fault[0] == "input":
        section = mutated(section, *fault[1])
    if fault and fault[0] == "remap":
        remap = mutated(remap, *fault[1])
    defines = re.findall(r"^#define TRIGGER_\w+ .*$", section, re.M)
    body = re.sub(r"^#define TRIGGER_\w+ .*$", "", section, flags=re.M)
    xdk = read("port/include/xdk/xdk_xbox.h")
    constants = "".join(f"#define {name} {constant(xdk, name)}\n"
                        for name in ("XINPUT_GAMEPAD_LEFT_TRIGGER", "XINPUT_GAMEPAD_RIGHT_TRIGGER"))
    return (("constants.inc", constants + "\n".join(defines) + "\n"),
            ("controller_remap.h", read("port/linux/include/controller_remap.h")),
            ("under_test.inc", body + "\n" + remap + "\n"))


@pytest.mark.parametrize("case", CASES)
def test_trigger_resistance(case):
    status, output = run(build("trigger_resistance", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("name", sorted(NEGATIVE_CONTROLS))
def test_trigger_resistance_negative_control(name):
    where, change, case = NEGATIVE_CONTROLS[name]
    status, output = run(build("trigger_resistance", generated((where, change))), case)
    assert status == CHECK_FAILED, output
