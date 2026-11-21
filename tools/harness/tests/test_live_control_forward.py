"""The host's forwarded live controls on the wire (network_substeps.c's live_control_forward_pack and
live_control_forward_unpack): a client reads every control exactly as the 19-byte entry before gave it, in 12 bytes
when its tick is within a byte of its message's, 15 further."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run

SOURCE = "port/linux/game/network_substeps.c"
DISTRIBUTED = "port/linux/game/network_distributed.c"
CASES = ["near", "far"]

NEGATIVE_CONTROLS = {
    # the tick always in a byte: a far one read wrong
    "no-whole-tick": (("if (offset >= -128 && offset <= 127)", "if (TRUE)"), "far"),
    # the host's kind of tick left in the player's index
    "kind-in-player": (("(short)(data[0] & ~FLAG(LIVE_CONTROL_FORWARD_HOST_UPDATE_BIT))", "(short)data[0]"), "near"),
    # a held control left out of the byte
    "grenade-dropped": (("\t_unit_control_throw_grenade_bit,\n", ""), "near"),
    # the sub-step read from the wrong bits
    "substep-shifted": (("& ~FLAG(LIVE_CONTROL_FORWARD_WHOLE_KEY_BIT)) >> LIVE_CONTROL_FORWARD_SUBSTEP_SHIFT)",
                         "& ~FLAG(LIVE_CONTROL_FORWARD_WHOLE_KEY_BIT)) >> (LIVE_CONTROL_FORWARD_SUBSTEP_SHIFT - 1))"),
                        "near"),
}


def generated(fault=None):
    source = read(SOURCE)
    distributed = read(DISTRIBUTED)
    header = read("port/linux/game/network_substeps.h")
    table = re.search(r"static short const live_control_held_bits\[\] =\n\{.*?\n\};\n", source, re.S)[0]
    code = table + "\n".join(function(source, name) for name in
                             ("live_control_forward_pack", "live_control_forward_unpack"))
    if fault:
        code = mutated(code, *fault)
    config = enum_with(read("source/units/units.h"), "_unit_control_crouch_modifier_bit") + "\n"
    config += enum_with(header, "LIVE_CONTROL_FORWARD_MAXIMUM_SIZE") + "\n"
    config += "enum { %s };\n" % ", ".join(
        f"{name} = " + re.search(rf"\b{name} = (\d+),", source)[1] for name in
        ("LIVE_CONTROL_FORWARD_SHORT_SIZE", "LIVE_CONTROL_FORWARD_HOST_UPDATE_BIT",
         "LIVE_CONTROL_FORWARD_SUBSTEP_SHIFT", "LIVE_CONTROL_FORWARD_WHOLE_KEY_BIT"))
    config += re.search(r"^struct live_control\n\{.*?\n\};\n", source, re.S | re.M)[0]
    config += "\n".join(function(distributed, name) for name in
                        ("distributed_vector_pack", "distributed_vector_unpack", "distributed_unit_vector_unpack"))
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("live_control_forward", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("live_control_forward", generated(fault)), case)
    assert status == CHECK_FAILED, output
