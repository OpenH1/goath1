"""The host's relayed input to an OpenH1 client packed (_distributed_message_relayed_actions_packed,
distributed_relayed_action_pack): each entry without its parts at their usual values (no choice, sticks, trigger or
buttons), its player a bit where it is the one after the entry before's; the client makes each message back into the
whole message of the same entries, byte for byte (distributed_relayed_actions_unpack). A bot's entry 7 bytes (8 the
message's first), not 14; every part used; a message split where it fills, each first entry's player whole; cut short,
or a first entry after none, no message."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, needs_32_bit_abi, read, run, structure

pytestmark = needs_32_bit_abi("the message's header and update number go on the wire as the game's longs, 4 bytes")

SOURCE = "port/linux/game/network_distributed.c"
HEADER = "port/linux/game/network_distributed.h"
QUEUES = "source/game/player_queues_new.h"
CASES = ["bots", "mixed", "split", "first-unknown"]

NEGATIVE_CONTROLS = {
    # the next player's bit set for a message's first entry (the player after none)
    "first-as-next": (("if (before != NONE && relayed->player_index == before + 1)",
                       "if (relayed->player_index == before + 1)"), "mixed"),
    # a choice left out read as 0, not none
    "choice-default": (("\trelayed->desired_zoom_level = NONE;\n", ""), "bots"),
    # the trigger left out though pulled
    "trigger-dropped": (("\tif (relayed->primary_trigger)\n", "\tif (0)\n"), "mixed"),
    # the latest buttons not read
    "buttons-unread": (("if (TEST_FLAG(parts, _relayed_packed_buttons_bit) &&", "if (FALSE &&"), "mixed"),
    # a message's first entry after a split written against the entry before, in the message before
    "split-unseen": (("if (*before != NONE && !distributed_packer_joins(packer, size))", "if (FALSE)"), "split"),
    # a first entry after none taken as player 0
    "after-none-taken": (("if (before == NONE || before + 1 > 0xFF)", "if (before + 1 > 0xFF)"), "first-unknown"),
}

FUNCTIONS = ("distributed_put", "distributed_take", "distributed_relayed_action_write", "distributed_relayed_action_pack",
             "distributed_relayed_action_unpack", "distributed_packer_begin", "distributed_packer_flush",
             "distributed_packer_joins", "distributed_packer_add", "distributed_relayed_action_add",
             "distributed_relayed_actions_unpack")


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = "enum { DISTRIBUTED_INPUT_HISTORY = %s };\n" % re.search(r"DISTRIBUTED_INPUT_HISTORY = (\d+),",
                                                                       read(QUEUES))[1]
    config += "enum { DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE = %s };\n" % re.search(
        r"DISTRIBUTED_RELAYED_ACTION_MAXIMUM_SIZE = ([^,]+),", source)[1]
    config += enum_with(header, "_distributed_message_relayed_actions_packed") + "\n"
    config += structure(header, "distributed_message_header") + "\n"
    config += structure(source, "distributed_relayed_action") + "\n"
    config += structure(source, "distributed_packer") + "\n"
    config += enum_with(source, "_relayed_packed_choices_bit") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("relayed_action_packed", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("relayed_action_packed", generated(fault)), case)
    assert status == CHECK_FAILED, output
