"""The host's objects' states in bits to an OpenH1 client (network_objects.c's network_objects_pack_state,
network_objects_unpack_state, network_objects_unpack_states and distributed_host_send_state_message, with
network_bits.h): each read back as the whole entry, its position on a grid of 1/4096 of a world unit; at the map's
bounds, at the fastest speeds, every part set and none; outside the world or not a number, whole and exact; an OpenCE
machine and debug.network_forms_before sent the whole message; a message cut short is none."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, needs_32_bit_abi, read, run, structure

pytestmark = needs_32_bit_abi("an object's index goes on the wire as the game's long, 4 bytes")

SOURCE = "port/linux/game/network_objects.c"
BITS = "port/linux/game/network_bits.h"
CASES = ["round-trip", "outside", "message", "short"]

NEGATIVE_CONTROLS = {
    # the grid's steps truncated, not rounded: a quarter step off and more
    "position-truncated": (("values[axis] = (long)(steps >= 0.0f ? steps + 0.5f : steps - 0.5f);",
                            "values[axis] = (long)steps;"), "round-trip"),
    # a position outside the world put on the grid, not whole
    "outside-on-grid": (("boolean whole = !distributed_point_valid(&state->position, OBJECT_WORLD_BOUND);",
                         "boolean whole = FALSE;"), "outside"),
    # the client's tick of a predicted state left out
    "time-dropped": (("if (whole || TEST_FLAG(state->flags, _distributed_object_predicted_bit))\n\t\tnetwork_bits_put(",
                      "if (whole)\n\t\tnetwork_bits_put("), "round-trip"),
    # a negative value's sign not extended
    "sign-unextended": (("\t\t\tvalue |= ~0UL << width;\n", ""), "round-trip"),
    # a unit vector's largest part's sign dropped
    "largest-sign-dropped": (("values[largest] = (negative ? -1 : 1) * (length + rest);",
                              "values[largest] = (negative ? 1 : 1) * (length + rest);"), "round-trip"),
    # a largest part further from its length than its 2 bits tell, sent so
    "rest-unbounded": (("rest < NETWORK_BITS_UNIT_LEAST ||\n\t\trest >= NETWORK_BITS_UNIT_LEAST + (1 << NETWORK_BITS_UNIT_REST_BITS))",
                        "rest < NETWORK_BITS_UNIT_LEAST)"), "round-trip"),
    # an OpenCE machine sent the packed message
    "packed-to-opence": (("if (!network_legacy_machine(machine_index) && !config_boolean(",
                          "if (!config_boolean("), "message"),
    # bits read past a message's end
    "unbounded-read": (("\t\tif (byte_index >= bits->size)\n\t\t{\n\t\t\tbits->failed = TRUE;\n\t\t\treturn 0;\n\t\t}\n", ""),
                       "short"),
}

FUNCTIONS = ("network_objects_pack_state", "network_objects_unpack_state", "network_objects_unpack_states",
             "distributed_host_send_state_message")


def generated(fault=None):
    source = read(SOURCE)
    bits = read(BITS)
    code = bits + "\n" + "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = structure(source, "distributed_object_state") + "\n"
    config += enum_with(source, "_distributed_object_predicted_bit") + "\n"
    config += "enum { MAXIMUM_ENTRIES_PER_MESSAGE = %s };\n" % re.search(
        r"MAXIMUM_ENTRIES_PER_MESSAGE = (\d+),", source)[1]
    for name in ("OBJECT_WORLD_BOUND", "OBJECT_PACKED_POSITION_SCALE"):
        config += f"#define {name} " + re.search(rf"^#define {name}\s+(\S+)", source, re.M)[1] + "\n"
    config += enum_with(source, "OBJECT_PACKED_MAXIMUM_SIZE") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("object_state_packing", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("object_state_packing", generated(fault)), case)
    assert status == CHECK_FAILED, output
