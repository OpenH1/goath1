"""A client's controls packed for the host (network_substeps.c): its live controls and sub-steps read exactly as the
whole entries were, every bit of every number, at the extremes (-0, NaN, a far tick, every held control, the most
sub-steps), none past its end."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_substeps.c"
CASES = ["up", "substeps"]

FUNCTIONS = (
    "held_bits_pack", "held_bits_unpack", "packed_real_code", "packed_real_write", "packed_real_read",
    "packed_key_write", "packed_key_read", "live_control_up_pack", "live_control_up_unpack", "substep_input_pack",
    "substep_input_unpack")

NEGATIVE_CONTROLS = {
    # -0 taken for +0: a bit of a number read otherwise
    "minus-zero": (("if (!csmemcmp(&value, &packed_reals[code], sizeof(value)))",
                    "if (value == packed_reals[code])"), "up"),
    # a live control's tick always the message's
    "tick-always-message": (("(entry->tick != time ? FLAG(PACKED_WHOLE_KEY_BIT) : 0)", "0"), "up"),
    # a sub-step's sticks as the one before's without its trigger
    "as-before-without-trigger": (("\t\t\t!csmemcmp(&control->primary_trigger, &before->primary_trigger, sizeof(real)))",
                                   "\t\t\tTRUE)"), "substeps"),
    # a far tick in a byte
    "substep-tick-in-byte": (("boolean whole = offset < -128 || offset > 127;", "boolean whole = FALSE;"),
                             "substeps"),
}


def generated(fault=None):
    source = read(SOURCE)
    config = enum_with(read("source/units/units.h"), "_unit_control_crouch_modifier_bit") + "\n"
    config += enum_with(source, "MAXIMUM_LATER_SUBSTEPS = ") + "\n"
    config += re.search(r"static short const live_control_held_bits\[\] =\n\{.*?\n\};\n", source, re.S)[0]
    for name in ("distributed_substep_input_control", "distributed_substep_input", "distributed_live_control"):
        config += structure(source, name) + "\n"
    config += re.search(r"static real const packed_reals\[.*?;\n", source)[0]
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("controls_packing", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("controls_packing", generated(fault)), case)
    assert status == CHECK_FAILED, output
