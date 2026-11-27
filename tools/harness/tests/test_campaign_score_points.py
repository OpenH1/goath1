"""Campaign scoring's rules (campaign_score.c): the points table, a victim's class from its tags' names, a kill's
bonuses (headshot, melee, an assassination's in the melee's place, grenade), a driver's assist, multi-kills within their window (and the most a step gives), streaks that a death
ends, infection forms that count toward neither, and deductions that never take a score below 0."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/campaign_score.c"
HEADER = "port/linux/game/campaign_score.h"
FUNCTIONS = ("campaign_score_name_has", "campaign_score_class_from_names", "campaign_score_scaled",
             "campaign_score_note_event", "campaign_score_player_kill", "campaign_score_player_deduct",
             "campaign_score_player_death", "campaign_score_driver_assist_points")
CASES = ["table", "names", "bonuses", "assassination", "driver-assist", "multi", "multi-window", "multi-cap", "streak", "swarm", "deduct"]

# a fault in the code under test and the case that must catch it
NEGATIVE_CONTROLS = {
    # the window twice as long: kills far apart a multi-kill
    "window": (("tick - timing->last_kill_tick <= CAMPAIGN_SCORE_MULTI_KILL_TICKS",
                "tick - timing->last_kill_tick <= 2 * CAMPAIGN_SCORE_MULTI_KILL_TICKS"), "multi-window"),
    # a double kill given the triple's bonus
    "multi-step": (("(short)MIN(timing->multi_count - 2,", "(short)MIN(timing->multi_count - 1,"), "multi"),
    # no cap on the steps: a step past the table's end
    "multi-cap": (("NUMBEROF(campaign_score_bonuses.multi_kill) - 1)", "NUMBEROF(campaign_score_bonuses.multi_kill))"),
                  "multi-cap"),
    # a death that keeps the streak
    "streak-kept": (("\tplayer->current_streak = 0;\n\ttiming->multi_count = 0;", "\ttiming->multi_count = 0;"),
                    "streak"),
    # a deduction below 0
    "below-zero": (("long taken = MIN(points, player->score);", "long taken = points;"), "deduct"),
    # the elite looked for before the flood: a combat form of an elite an elite
    "flood-order": (('\tif (campaign_score_name_has(unit_name, "flood"))\n\t\treturn _campaign_score_class_flood_combat;\n',
                     ""), "names"),
    # any blow to the head a headshot (a grenade's)
    "head-any": (("if (head && (category == _campaign_score_damage_bullet ||", "if (head || (category == 255 ||"),
                 "bonuses"),
    # an assassination given the melee's bonus
    "assassination-melee": (("unsuspecting ? campaign_score_bonuses.assassination :", "FALSE ? 0 :"), "assassination"),
    # an assassination's bonus without its melee (any instant kill of the unsuspecting)
    "assassination-any": (("\telse if (category == _campaign_score_damage_melee)\n",
                           "\telse if (category == _campaign_score_damage_melee || unsuspecting)\n"), "assassination"),
    # the assist's share rounded down: an elite major's 37
    "assist-round": (("driver_assist_percent + 50) / 100", "driver_assist_percent) / 100"), "driver-assist"),
    # the assist's share after the multiplier is left out
    "assist-multiplier": (("return campaign_score_scaled(share, multiplier);", "return share;"), "driver-assist"),
    # the swarm's forms counted toward streaks
    "swarm-streak": (("if (class_points->counts_toward_streaks)", "if (TRUE)"), "swarm"),
}


def _between(source, start, end):
    return source[source.index(start):source.index(end)]


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    types = "\n".join((
        enum_with(header, "_campaign_score_class_other"),
        enum_with(header, "_campaign_score_event_kill"),
        structure(header, "campaign_score_event"),
        structure(header, "campaign_score_player"),
        enum_with(source, "CAMPAIGN_SCORE_MULTI_KILL_TICKS"),
        enum_with(source, "_campaign_score_damage_none"),
        _between(source, "/* ---------- the points */", "/* ---------- structures */"),
        structure(source, "campaign_score_timing"),
    )) + "\n"
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("campaign_score_points", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("campaign_score_points", generated(fault)), case)
    assert status == CHECK_FAILED, output
