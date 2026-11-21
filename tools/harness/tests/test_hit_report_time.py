"""The host's check of a client's hit against the time its report is about (network_damage.c): the report's time is
believed only as far back as the host measures that client's round trip and a slack, the history holds every tick a
report so taken can be about, and the target is checked from then on. A high-ping hit on a running target is taken;
a report claiming an older time than its client's delay allows is refused; a 30, 60 or 120 Hz shooter is taken alike
(the ticks are the game's); a target where the host never had it is refused. The round trip that bounds a report is the
worst the host measured over the last ROUND_TRIP_WINDOW_TICKS: a lag spike's delayed reports are taken after the smoothed
round trip has fallen back, the bound tightens once the window has passed, and a claim no measurement supports is refused.
Also a reused player slot's reset, its window's among it."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, function, mutated, read, run  # noqa: E402

CASES = ["high-ping-moving", "claimed-too-old", "claimed-future", "never-there", "every-rate", "spike-taken",
         "spike-tightens", "unsupported-claim", "slot-reused"]

# a fault in the host's check, and the case that must catch it
NEGATIVE_CONTROLS = {
    # (the history looked back over a second at most, as it held: the oldest hits checked against the last second)
    "history-a-second": (("ticks = now - report->host_time + TARGET_HISTORY_SLACK_TICKS;",
                          "ticks = MIN(now - report->host_time + TARGET_HISTORY_SLACK_TICKS, 31);"),
                         "high-ping-moving"),
    # (the client's time believed to the longest age, whatever its round trip)
    "time-unbounded": (("report->host_time < distributed_report_earliest(machine_index, player_index))",
                        "now - report->host_time > REPORT_MAXIMUM_AGE_TICKS)"), "claimed-too-old"),
    # (anything taken)
    "anything-taken": (("if (distance <= reach * reach && (!seen || distance < seen_distance))",
                        "if (distance <= reach * reach + 1e9f && (!seen || distance < seen_distance))"), "never-there"),
    # (a reused slot not reset)
    "slot-kept": (("\t\tdistributed_reset_player_slot(player_index);\n\tdamage_players[player_index].identifier",
                   "\t\t;\n\tdamage_players[player_index].identifier"), "slot-reused"),
    # (the bound the smoothed round trip alone, as on the rework: a spike's late reports refused once it falls back)
    "smoothed-only": (("noted->ticks > worst)", "noted->ticks > worst && 0)"), "spike-taken"),
    # (the worst round trip ever measured, never expiring: the bound never tightens back)
    "never-expiring": (("\t\t}\n\t}\n\treturn worst;",
                        "\t\t}\n\t}\n\t{ static real ever; if (worst > ever) ever = worst; return ever; }"),
                       "spike-tightens"),
    # (a reused slot's window kept)
    "window-kept": (("\t\tdamage_round_trips[player_index][index].time = NONE;", "\t\t;"), "slot-reused"),
}


def expression(source, name):
    """An enum constant's expression as the source writes it."""
    return re.search(r"^\s*" + re.escape(name) + r" = ([^,]+),", source, re.M).group(1)


def age_check(source):
    """distributed_report_valid's age check and look back, as a function of its own (REFUSE: refused)."""
    valid = function(source, "distributed_report_valid")
    start = valid.index("\t/* made at a tick of the host's")
    end = valid.index("\t/* damage that player could deal")
    return ("static boolean report_checked(long machine_index, short player_index, struct report const *report,\n"
            "\treal_point3d *seen_position)\n"
            "{\n\tlong now = game_time_get();\n\tlong ticks;\n#define REFUSE(reason) return FALSE\n" + valid[start:end] +
            "\treturn distributed_target_seen(report->object_index, &report->target_position, ticks, seen_position)"
            " == TRUE;\n#undef REFUSE\n}\n")


def generated(fault=None):
    source = read("port/linux/game/network_damage.c")
    config = f"#define TICKS_PER_SECOND {constant(read('source/cseries/cseries.h'), 'TICKS_PER_SECOND')}\n"
    for name in ("TARGET_HISTORY_TICKS", "TARGET_HISTORY_SLACK_TICKS", "REPORT_DELAY_SLACK_TICKS",
                 "REPORT_MAXIMUM_AGE_TICKS", "ROUND_TRIP_WINDOW_TICKS"):
        config += f"#define {name} ({expression(source, name)})\n"
    for name in ("REPORT_HISTORY_TOLERANCE", "REPORT_HISTORY_LEAD_TICKS"):
        config += f"#define {name} {re.search(rf'#define {name} ([0-9.]+f)', source).group(1)}\n"
    code = "\n".join(function(source, name) for name in (
        "damage_history_at", "distributed_distance_squared", "distributed_target_seen", "distributed_worst_round_trip",
        "distributed_report_earliest", "distributed_note_round_trips",
        "distributed_reset_player_slot", "distributed_note_player_slot"))
    code += "\n" + age_check(source)
    if fault:
        code = mutated(code, *fault)
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("hit_report_time", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("hit_report_time", generated(fault)), case)
    assert status == CHECK_FAILED, f"network_damage.c with a fault ({control}) passed '{case}': the test cannot see it"


def test_one_fact():
    """The history's length is the accepted age's (and its slack), not a number of its own; it is noted once a game
    tick (network_damage_host_tick, from the game's 30 Hz tick at every rate); and a report's player's slot is checked
    before its weapons are."""
    source = read("port/linux/game/network_damage.c")
    assert expression(source, "TARGET_HISTORY_TICKS") == "REPORT_MAXIMUM_AGE_TICKS + TARGET_HISTORY_SLACK_TICKS + 1"
    assert "& (TARGET_HISTORY_TICKS - 1)" not in source
    host_tick = function(source, "network_damage_host_tick")
    assert "distributed_note_targets();" in host_tick and "distributed_note_player_slot(player_index);" in host_tick
    # (the round trips noted once a game tick, after the slots are: the window is in the game's ticks at every rate, and
    # a joiner's starts empty)
    assert host_tick.index("distributed_note_player_slot(player_index);") < host_tick.index(
        "distributed_note_round_trips();")
    assert source.count("distributed_note_round_trips();") == 1
    assert expression(source, "ROUND_TRIP_WINDOW_TICKS") == "REPORT_MAXIMUM_AGE_TICKS"
    assert source.count("distributed_note_targets();") == 1
    valid = function(source, "distributed_report_valid")
    assert valid.index("distributed_note_player_slot(player_index);") < valid.index("distributed_player_deals(")
