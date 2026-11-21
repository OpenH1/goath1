"""An OpenH1 game's tick states as differences (distributed_unit_difference_write and distributed_unit_difference_read,
OpenH1 protocol version 23): each state read back is exactly the whole state's bytes, through a chain of ticks whose
acks lag (running, turning, hurt, killed, respawned); whole where the client asked for it, nothing was sent at or before
its ack, a part does not fit or the ack is older than the client keeps; a difference from a state the client has not
passed over, the entries after it read as sent; cut short, no entry. Each entry joined (158) and read back as its bytes. Packed (network_bits.h), as the host sends them,
and (-unpacked) in the forms before, as debug.network_forms_before sends them: at the map's bounds, the fastest
speeds, every part changed and none; a state off the grid whole. A client's prediction trimmed; a unit at rest's the
same bytes each tick (held), a moving one's velocity exact."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, needs_32_bit_abi, read, run, structure

pytestmark = needs_32_bit_abi("a unit state's longs go on the wire as the game's, 4 bytes")

SOURCE = "port/linux/game/network_distributed.c"
HEADER = "port/linux/game/network_distributed.h"
# (the unit state's bytes, the Legacy encoding OpenH1's messages carry whole)
LEGACY = "port/linux/game/network_legacy.c"
LEGACY_HEADER = "port/linux/game/network_legacy.h"
CASES = ["joined", "prediction-trim", "prediction-rest"] + [case + mode for case in ("chain", "not-predicted", "whole", "unknown", "short", "deck", "extremes")
         for mode in ("", "-unpacked")]

# a fault in the encoding and the case that must catch it
NEGATIVE_CONTROLS = {
    # the grid's steps truncated, not rounded
    "grid-truncated": (("return (long)(steps >= 0.0f ? steps + 0.5f : steps - 0.5f);", "return (long)steps;"),
                       "chain"),
    "grid-truncated-unpacked": (("return (long)(steps >= 0.0f ? steps + 0.5f : steps - 0.5f);", "return (long)steps;"),
                                "chain-unpacked"),
    # a state sent after the client's ack written against
    "ack-ignored": (("sent != NONE && sent <= acked &&", "sent != NONE &&"), "unknown"),
    "ack-ignored-unpacked": (("sent != NONE && sent <= acked &&", "sent != NONE &&"), "unknown-unpacked"),
    # the client's own ticks' state taken for the tick named
    "base-time-ignored": (("distributed_unit_base_times[player_index][(time - age) & (UNIT_BASE_TICKS - 1)] == time - age ?",
                           "TRUE ?"), "unknown"),
    "base-time-ignored-unpacked": (("distributed_unit_base_times[player_index][(time - age) & (UNIT_BASE_TICKS - 1)] == time - age ?",
                                    "TRUE ?"), "unknown-unpacked"),
    # the shields' change left out
    "vitality-ignored": (("if (written.body_vitality != base.body_vitality || written.shield_vitality != base.shield_vitality)",
                          "if (0)"), "chain-unpacked"),
    # the client's tick's difference read backwards
    "predicted-sign": (("(word)read.predicted_time + (word)(short)small", "(word)read.predicted_time - (word)(short)small"),
                       "chain-unpacked"),
    # a facing's turn past a byte written as a byte
    "forward-overflow": (("if (distributed_vector_difference(&written.forward, &base.forward, small))",
                          "if (distributed_vector_difference(&written.forward, &base.forward, small) || 1)"),
                         "chain-unpacked"),
    # a teleport's step past 16 bits wrapped
    "position-overflow": (("difference < -32767 || difference > 32767 ||", ""), "whole-unpacked"),
    # a rider's deck part left out of the differences (OpenH1 protocol 31): the base's kept
    "deck-ignored": (("if (written.deck_present || base.deck_present)", "if (0)"), "deck-unpacked"),
    # a deck part's going away not sent
    "deck-gone-ignored": (("*cursor++ = written.deck_present ? 1 : 0;", "*cursor++ = 1;"), "deck-unpacked"),
    # a deck part read without its presence checked
    "deck-presence-unchecked": (("|| deck_present > 1)", ")"), "deck"),
    "deck-presence-unchecked-unpacked": (("|| deck_present > 1)", ")"), "deck-unpacked"),
    # (packed) the position read without where the velocity takes it
    "packed-velocity-ignored": (("UNIT_PACKED_VELOCITY_STEPS * (long)(&from.velocity.i)[axis] * age + values[axis]",
                                 "values[axis]"), "chain"),
    # (packed) a client's tick that runs with the host's sent anyway: an unchanged player's bytes
    "packed-time-held": (("(short)(word)((word)base->predicted_time + age) : base->predicted_time",
                          "base->predicted_time : base->predicted_time"), "extremes"),
    # (packed) a negative difference's sign not extended
    "packed-sign-unextended": (("\t\t\tvalue |= ~0UL << width;\n", ""), "chain"),
    # (packed) a facing's largest part's sign dropped
    "packed-facing-sign": (("values[largest] = (negative ? -1 : 1) * (length + rest);",
                            "values[largest] = (negative ? 1 : 1) * (length + rest);"), "extremes"),
    # (packed) a facing's largest part further from its length than its 2 bits tell, sent so
    "packed-rest-unbounded": (("rest < NETWORK_BITS_UNIT_LEAST ||\n\t\trest >= NETWORK_BITS_UNIT_LEAST + (1 << NETWORK_BITS_UNIT_REST_BITS))",
                               "rest < NETWORK_BITS_UNIT_LEAST)"), "extremes"),
    # (packed) a position off the grid sent as a difference
    "packed-off-grid": (("\t\t\t\treturn 0;\n\t\t\t}\n\t\t}\n\t\tSET_FLAG(parts, _unit_packed_position_bit, TRUE);",
                         "\t\t\t\t;\n\t\t\t}\n\t\t}\n\t\tSET_FLAG(parts, _unit_packed_position_bit, TRUE);"), "extremes"),
    # (packed) the deck bit not set
    "packed-deck-bit": (("buffer[0] = (byte)(FLAG(UNIT_PACKED_FORM_BIT) | (deck ? FLAG(UNIT_PACKED_DECK_BIT) : 0)",
                         "buffer[0] = (byte)(FLAG(UNIT_PACKED_FORM_BIT) | 0"), "deck"),
    # (a prediction trimmed) the suspension the host reads left out too
    "trim-suspension": (("state->unit_flags &= (byte)FLAG(_distributed_unit_suspended_bit);", "state->unit_flags = 0;"),
                        "prediction-trim"),
    # (a prediction trimmed) the damage kept
    "trim-damage-kept": (("\tstate->current_body_damage = 0;\n", ""), "prediction-trim"),
    # (a prediction trimmed) a unit at rest's velocity sent as it is (every tick a change: never held)
    "trim-rest-kept": (("\tif (fabs(state->fine_velocity.i) < PREDICTION_REST_SPEED)\n\t\tstate->fine_velocity.i = 0.0f;\n", ""),
                       "prediction-rest"),
    # (a prediction trimmed) a moving unit's slowest part taken for rest
    "trim-rest-too-far": (("fabs(state->fine_velocity.j) < PREDICTION_REST_SPEED",
                           "fabs(state->fine_velocity.j) <= PREDICTION_REST_SPEED"), "prediction-rest"),
    # (joined) the player after the entry before's read as the one before's
    "joined-player-next": (("? joining->player_index + 1 : (long)network_bits_take(bits, 8);",
                            "? joining->player_index : (long)network_bits_take(bits, 8);"), "joined"),
    # (joined) another entry's parts taken for the entry's
    "joined-parts-unchecked": (("if (joining->parts_valid && parts == joining->parts)", "if (joining->parts_valid)"),
                               "chain"),
    # (joined) a facing's parts passed over as though its largest part always came
    "joined-facing-skip": (("(largest == NETWORK_BITS_UNIT_WHOLE ? 3 : 2)", "2"), "extremes"),
    # (joined) a deck part's going away read as one present
    "joined-deck-gone": (("\t\t*cursor++ = present;", "\t\t*cursor++ = 1;"), "deck"),
    # (joined) the age of the entry before kept for one of another age
    "joined-age-kept": (("if (age == joining->age)", "if (1)"), "chain"),
}

FUNCTIONS = ("distributed_put", "distributed_take", "distributed_real_valid", "legacy_put", "legacy_take",
             "distributed_vector_pack", "distributed_vector_unpack", "distributed_deck_put", "distributed_deck_take",
             "distributed_unit_state_write",
             "distributed_unit_state_read", "distributed_vector_difference", "distributed_grid_steps",
             "distributed_unit_state_snap", "distributed_unit_state_as_written", "distributed_unit_base_of",
             "distributed_unit_deck_take", "distributed_unit_packed_time", "distributed_unit_packed_write",
             "distributed_unit_packed_read", "distributed_unit_difference_write", "distributed_unit_difference_read",
             "distributed_unit_packed_skip", "distributed_bits_copy", "distributed_unit_entry_join",
             "distributed_unit_entry_unjoin", "distributed_prediction_trim")


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    legacy_header = read(LEGACY_HEADER)
    code = read("port/linux/game/network_bits.h") + "\n" + \
        "\n".join(function(source + read(LEGACY), name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = "enum { NUMBER_OF_PLAYER_POWERUPS = 2, NO_PLAYER = 0xFF };\n"
    config += "#define DISTRIBUTED_UNIT_SCALE " + re.search(r"#define DISTRIBUTED_UNIT_SCALE\s+(\S+)", header)[1] + "\n"
    config += "#define SUBSTEP_POSITION_SCALE " + re.search(r"#define SUBSTEP_POSITION_SCALE\s+(\S+)", source)[1] + "\n"
    config += "#define DISTRIBUTED_VELOCITY_SCALE " + re.search(r"#define DISTRIBUTED_VELOCITY_SCALE\s+(\S+)", header)[1] + "\n"
    for member in ("_distributed_unit_alive_bit", "_distributed_unit_camouflaged_bit",
                   "_distributed_unit_part_riding_bit", "_distributed_pose_facing"):
        config += enum_with(legacy_header, member) + "\n"
    for member in ("UNIT_BASE_TICKS", "_unit_difference_form_whole", "_unit_difference_flags_bit",
                   "UNIT_PACKED_FORM_BIT", "_unit_packed_position_bit"):
        config += enum_with(source, member) + "\n"
    config += re.search(r"^#define UNIT_PACKED_VELOCITY_STEPS .*$", source, re.M)[0] + "\n"
    config += re.search(r"^#define PREDICTION_REST_SPEED .*$", source, re.M)[0] + "\n"
    config += enum_with(legacy_header, "DISTRIBUTED_UNIT_STATE_MINIMUM_SIZE") + "\n"
    config += structure(read("port/linux/game/vehicle_riding.h"), "vehicle_riding_deck") + "\n"
    config += structure(legacy_header, "distributed_unit_state") + "\n"
    config += structure(source, "distributed_unit_joining") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("unit_difference", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("unit_difference", generated(fault)), case)
    assert status == CHECK_FAILED, output
