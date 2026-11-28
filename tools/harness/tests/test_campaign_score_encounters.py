"""Campaign scoring's encounters cleared (campaign_score.c's campaign_score_encounters_step): cleared once its living
enemies all died, a quarter of its kills' points (at least 50) and half again when no player died; none for enemies
migrated to another encounter (scored where they went), erased, still to respawn, or a script's refill after the bonus;
a revert takes the checkpoint's tallies back and its first tick only looks; infection forms do not clear an encounter
whose combat forms live."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/campaign_score.c"
HEADER = "port/linux/game/campaign_score.h"
FUNCTIONS = ("campaign_score_encounter_bonus", "campaign_score_encounters_step")
CASES = ["clear", "flawless", "migrate", "erase", "respawn", "refill", "revert", "swarm"]

# a fault in the code under test and the case that must catch it
NEGATIVE_CONTROLS = {
    # all its points, not a quarter
    "clear-bonus": (("kill_points * campaign_score_bonuses.encounter_percent / 100", "kill_points"), "clear"),
    # flawless whoever died
    "always-flawless": (("team_deaths == tallies->deaths_at_start[encounter] ?", "TRUE ?"), "flawless"),
    # the units kept with the encounters they had first: a migrated one never where it went
    "migrate-stale": (("\tcsmemcpy(tracking->tracked_encounters, encounters, count * sizeof(*encounters));\n", ""),
                      "migrate"),
    # an erased enemy taken as dead
    "erase-any": (("if (!all_died || !any_died ||", "if (!any_died ||"), "erase"),
    # a respawning encounter cleared while empty
    "respawn-ignored": (("(respawn_pending && respawn_pending(encounter)) ||", ""), "respawn"),
    # a refill's clear given a second bonus
    "refill-twice": (("if (tracking->living[encounter] == 0 || living[encounter] > 0 || tallies->awarded[encounter])",
                      "if (tracking->living[encounter] == 0 || living[encounter] > 0)"), "refill"),
    # the revert's first tick judged against the units before it
    "revert-judged": (("for (encounter = 0; !tracking->rescan && encounter", "for (encounter = 0; encounter"),
                      "revert"),
}


def _between(source, start, end):
    return source[source.index(start):source.index(end)]


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    types = "\n".join((
        enum_with(header, "_campaign_score_class_other"),
        structure(header, "campaign_score_player"),
        enum_with(source, "CAMPAIGN_SCORE_MULTI_KILL_TICKS"),
        _between(source, "/* ---------- the points */", "/* ---------- structures */"),
        structure(source, "campaign_score_encounter_tallies"),
        structure(source, "campaign_score_encounter_tracking"),
        structure(source, "campaign_score_encounter_clear"),
    )) + "\n"
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("types.inc", types), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("campaign_score_encounters", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("campaign_score_encounters", generated(fault)), case)
    assert status == CHECK_FAILED, output
