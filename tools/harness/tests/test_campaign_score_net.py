"""Campaign scoring's network message (network_campaign_score.c, _distributed_message_campaign_score): a message
packed and read back whole, its bytes as the wire has them (a format older builds must keep reading), the largest
message, counts kept to a word, the assassination's and driver assist's kinds, and what a client refuses (another format, a wrong size, too many records, a player or
an event kind out of range)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, needs_32_bit_abi, read, run, structure

pytestmark = needs_32_bit_abi("the wire's structures as the game's ILP32 guests lay them out (32-bit longs)")

SOURCE = "port/linux/game/network_campaign_score.c"
HEADER = "port/linux/game/network_campaign_score.h"
SCORE_HEADER = "port/linux/game/campaign_score.h"
FUNCTIONS = ("network_campaign_score_word", "network_campaign_score_pack", "network_campaign_score_unpack")
CASES = ["round-trip", "layout", "refused", "clamped", "new-kinds", "full"]

# a fault in the code under test and the case that must catch it
NEGATIVE_CONTROLS = {
    # a message longer than its counts say taken
    "size-longer": (("\t\tsize != sizeof(header) + header.record_count", "\t\tsize < sizeof(header) + header.record_count"),
                    "refused"),
    # the level's end not said
    "level-end": (("(message->level_end ? FLAG(_network_campaign_score_level_end_bit) : 0)", "0"), "round-trip"),
    # a player's score not sent
    "score": (("\t\trecord.score = player->score;\n", ""), "round-trip"),
    # a count not kept to a word: below 0 wraps
    "clamp": (("return (word)PIN(value, 0, 0xFFFF);", "return (word)value;"), "clamped"),
    # a slot past the players taken
    "slot": (("if (!VALID_INDEX(record.slot, CAMPAIGN_SCORE_MAX_PLAYERS))\n\t\t\treturn FALSE;", ""), "refused"),
    # an event of no kind taken
    "kind": (("||\n\t\t\trecord.kind >= NUMBER_OF_CAMPAIGN_SCORE_EVENT_KINDS", ""), "refused"),
    # the kinds an older build knew alone (to the flawless bonus): the new ones refused
    "old-kinds": (("record.kind >= NUMBER_OF_CAMPAIGN_SCORE_EVENT_KINDS", "record.kind > _campaign_score_event_flawless"),
                  "new-kinds"),
    # the multiplier in tenths
    "multiplier": (("message->difficulty_multiplier * 100.0f + 0.5f", "message->difficulty_multiplier * 10.0f + 0.5f"),
                   "layout"),
}


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    score = read(SCORE_HEADER)
    maximum = constant(read("port/linux/include/halo_port_limits.h"), "HALO_PORT_MAXIMUM_NETWORK_PLAYERS")
    types = "\n".join((
        f"#define CAMPAIGN_SCORE_MAX_PLAYERS {maximum}",
        enum_with(score, "_campaign_score_event_kill"),
        structure(score, "campaign_score_event"),
        structure(score, "campaign_score_player"),
        enum_with(header, "NETWORK_CAMPAIGN_SCORE_FORMAT"),
        enum_with(header, "_network_campaign_score_enabled_bit"),
        structure(header, "network_campaign_score_contents"),
        structure(source, "distributed_campaign_score_header"),
        structure(source, "distributed_campaign_score_player"),
        structure(source, "distributed_campaign_score_event"),
        "\n".join(re.findall(r"^typedef char distributed_campaign_score_\w+_size_assert\[\n?[^;]*;", source, re.M)),
    )) + "\n"
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("campaign_score_net", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("campaign_score_net", generated(fault)), case)
    assert status == CHECK_FAILED, output
