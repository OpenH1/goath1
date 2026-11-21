"""The real royale_gulag_queue_* (port/linux/game/royale_gulag.c): Royale's gulag line. Players go in once a match
while it is open, fight in pairs in arrival order, a dead fighter's opponent wins, a lone player wins by default after
the limit (never during a fight), a fight runs out of time, and a player who leaves keeps the line in order."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["arrival-order", "once-per-match", "win-by-default", "time-up", "leave", "every-player", "network"]

FUNCTIONS = ["royale_gulag_queue_reset", "royale_gulag_queue_enter", "royale_gulag_queue_pop",
             "royale_gulag_queue_step", "royale_gulag_queue_side", "royale_gulag_queue_opponent",
             "royale_gulag_queue_place", "royale_gulag_queue_fighter_died", "royale_gulag_queue_leave",
             "royale_gulag_queue_slot_byte", "royale_gulag_queue_write", "royale_gulag_queue_read"]

# the rules taken apart, and the case that must catch each
NEGATIVE_CONTROLS = {
    "last-in-first-out": (("long player_index = queue->waiting[0];",
                           "long player_index = queue->waiting[queue->waiting_count - 1];"), "arrival-order"),
    "gulag-not-marked-used": (("queue->used[slot] = TRUE;", ";"), "once-per-match"),
    "closed-gulag-open": (("if (!open || player_index == NONE", "if (player_index == NONE"), "once-per-match"),
    "default-win-at-once": (("if (++queue->alone_ticks >= alone_limit_ticks)", "if (TRUE)"), "win-by-default"),
    "dead-fighter-wins": (("return side == NONE ? NONE : queue->fighter[1 - side];",
                           "return side == NONE ? NONE : queue->fighter[side];"), "arrival-order"),
    # the old limit of 64: a player of a higher index never goes to the gulag
    "sized-for-64": (("!VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) || queue->used[slot]",
                      "!VALID_INDEX(slot, 64) || queue->used[slot]"), "every-player"),
    # the fight's start not sent: a client's HUD clock starts over
    "fight-start-lost": (("*fight_start = (long)(start & 0x7FFFFFFFUL);", "*fight_start = (long)(start & 0x7FFFFFFFUL) * 0;"), "network"),
    # the line read in the wrong order
    "line-reversed": (("if (player_index != NONE)\n\t\t\tqueue->waiting[queue->waiting_count++] = player_index;",
                       "if (player_index != NONE) { short at; for (at = queue->waiting_count++; at > 0; at--) queue->waiting[at] = queue->waiting[at - 1]; "
                       "queue->waiting[0] = player_index; }"), "network"),
}


def generated(fault=None):
    header = read("port/linux/game/royale_gulag.h")
    code = "\n".join(function(read("port/linux/game/royale_gulag.c"), name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    players = constant(read("port/linux/game/royale.h") + read("port/linux/include/halo_port_limits.h"),
                       "ROYALE_MAXIMUM_PLAYERS")
    return (("under_test.inc", f"#define ROYALE_MAXIMUM_PLAYERS {players}\n"
             + enum_with(header, "ROYALE_GULAG_EVENT_FIGHT") + "\n"
             + structure(header, "royale_gulag_event") + "\n"
             + structure(header, "royale_gulag_queue") + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_gulag_queue", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_gulag_queue", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_gulag_queue with a fault ({control}) passed '{case}': the test cannot see it"
