"""The bots' Royale rules (port/linux/game/bots_royale_rules.c), apart from the game: the beam's margins and the way
in from behind it, the squads' jump points and landings, the free fall's steering, and what a bot puts first in play
(the beam before a fight, a revive only when calm, its squadmate, the gulag). Each rule is checked with a case it must
refuse beside the one it must take; and the rules built with a planted fault (a fight put before the beam:
BOTS_ROYALE_RULES_MUTATION) must fail the checks, so that they are seen to catch one. Needs clang."""

import subprocess
from pathlib import Path

from mcc_test_support import compile_tool

ROOT = Path(__file__).resolve().parents[1]
RULES = ROOT / "port/linux/game/bots_royale_rules.c"

DRIVER = r'''
#include <math.h>
#include <stdio.h>
#include "bots_royale_rules.h"

static int failures;
#define CHECK(name, condition) do { if (!(condition)) { printf("FAIL %s\n", name); failures++; } \
	else printf("ok %s\n", name); } while (0)

static struct bots_royale_view calm(void)
{
	struct bots_royale_view view = { 0 };
	view.margin_now = BOTS_ROYALE_NO_BEAM;
	view.margin_target = BOTS_ROYALE_NO_BEAM;
	view.seconds_to_move = 1000.0f;
	view.mate_standing = 1;
	view.mate_distance = 5.0f;
	view.mate_margin_now = BOTS_ROYALE_NO_BEAM;
	view.seconds_since_enemy = 60.0f;
	return view;
}

int main(void)
{
	struct bots_royale_box box = { 0.0f, 0.0f, 100.0f, 200.0f };
	struct bots_royale_view view;
	float x, y, yaw, throttle, last;
	short rank, tries;

	/* the beam's margins: from each side, ahead positive, behind negative */
	CHECK("margin_west_ahead", fabsf(bots_royale_beam_margin(_bots_royale_side_west, 0.25f, &box, 50, 0) - 25) < 0.01f);
	CHECK("margin_west_behind", bots_royale_beam_margin(_bots_royale_side_west, 0.25f, &box, 10, 0) < 0);
	CHECK("margin_north_ahead", fabsf(bots_royale_beam_margin(_bots_royale_side_north, 0.5f, &box, 0, 50) - 50) < 0.01f);
	CHECK("margin_north_behind", bots_royale_beam_margin(_bots_royale_side_north, 0.5f, &box, 0, 150) < 0);
	CHECK("margin_east", fabsf(bots_royale_beam_margin(_bots_royale_side_east, 0.1f, &box, 80, 0) - 10) < 0.01f);
	CHECK("margin_south", fabsf(bots_royale_beam_margin(_bots_royale_side_south, 0.1f, &box, 0, 30) - 10) < 0.01f);
	CHECK("margin_no_beam", bots_royale_beam_margin(-1, 0.5f, &box, 0, 0) >= BOTS_ROYALE_NO_BEAM);

	/* the refuge: behind moves in to the margin; ahead stays */
	x = 10; y = 100;
	bots_royale_beam_refuge(_bots_royale_side_west, 0.25f, &box, 20, &x, &y);
	CHECK("refuge_moves_in", fabsf(x - 45) < 0.01f && fabsf(y - 100) < 0.01f);
	CHECK("refuge_is_safe", bots_royale_beam_margin(_bots_royale_side_west, 0.25f, &box, x, y) >= 20 - 0.01f);
	x = 70; y = 100;
	bots_royale_beam_refuge(_bots_royale_side_west, 0.25f, &box, 20, &x, &y);
	CHECK("refuge_ahead_stays", fabsf(x - 70) < 0.01f);
	x = 10; y = 100;
	bots_royale_beam_refuge(_bots_royale_side_west, 0.95f, &box, 20, &x, &y);
	CHECK("refuge_kept_in_box", x <= 100 && x >= 0);

	/* the jump: squads in order along the path, within its middle, each in
	its own share whatever its random number */
	last = 0;
	for (rank = 0; rank < 8; rank++)
	{
		float low = bots_royale_jump_fraction(rank, 8, 0.0f), high = bots_royale_jump_fraction(rank, 8, 1.0f);
		if (low <= last || high < low || low < 0.1f || high > 0.9f)
			break;
		last = high;
	}
	CHECK("jump_spread", rank == 8);
	CHECK("jump_same_squad_same_point", bots_royale_jump_fraction(3, 8, 0.4f) == bots_royale_jump_fraction(3, 8, 0.4f));
	CHECK("jump_one_squad", bots_royale_jump_fraction(0, 1, 0.5f) > 0.4f && bots_royale_jump_fraction(0, 1, 0.5f) < 0.6f);

	/* the landing: beside the path, in the box */
	bots_royale_landing(0, 100, 100, 100, 0.5f, 1.0f, 30.0f, &box, &x, &y);
	CHECK("landing_beside_path", fabsf(x - 50) < 0.01f && fabsf(y - 130) < 0.01f);
	bots_royale_landing(0, 100, 100, 100, 0.5f, 0.0f, 30.0f, &box, &x, &y);
	CHECK("landing_on_path", fabsf(y - 100) < 0.01f);
	bots_royale_landing(0, 190, 100, 190, 0.5f, 1.0f, 40.0f, &box, &x, &y);
	CHECK("landing_in_box", y <= 200 - 4 + 0.01f);
	CHECK("chord_crosses", bots_royale_chord(&box, 50, 100, 1, 0, &x, &y, &yaw, &throttle) &&
		fabsf(x) < 0.01f && fabsf(y - 100) < 0.01f && fabsf(yaw - 100) < 0.01f && fabsf(throttle - 100) < 0.01f);
	CHECK("chord_from_outside", bots_royale_chord(&box, -50, 50, 1, 1, &x, &y, &yaw, &throttle) &&
		fabsf(x) < 0.01f && fabsf(y - 100) < 0.01f && fabsf(yaw - 100) < 0.01f && fabsf(throttle - 200) < 0.01f);
	CHECK("chord_misses", !bots_royale_chord(&box, -50, 50, 0, 1, &x, &y, &yaw, &throttle));
	CHECK("chord_still", !bots_royale_chord(&box, 50, 50, 0, 0, &x, &y, &yaw, &throttle));
	CHECK("jump_at_point", bots_royale_should_jump(0.5f, 0.4f, 1));
	CHECK("no_jump_before_point", !bots_royale_should_jump(0.3f, 0.4f, 1));
	CHECK("no_jump_off_island", !bots_royale_should_jump(0.5f, 0.4f, 0));
	CHECK("no_jump_before_island", !bots_royale_should_jump(-0.1f, 0.0f, 1));
	CHECK("jump_island_nearly_passed", bots_royale_should_jump(0.95f, 0.99f, 1));
	CHECK("progress", fabsf(bots_royale_path_progress(0, 0, 100, 0, 25, 7) - 0.25f) < 0.001f);

	/* the fall: towards the landing, easing off on it */
	bots_royale_fall_steer(0, 0, 0, 50, &yaw, &throttle);
	CHECK("fall_faces_landing", fabsf(yaw - 1.5707963f) < 0.001f && throttle == 1.0f);
	bots_royale_fall_steer(0, 0, 0.5f, 0, &yaw, &throttle);
	CHECK("fall_eases_off", throttle < 0.2f);

	/* play: nothing to do is bots.c's own play */
	view = calm();
	CHECK("calm_default", bots_royale_decide(&view) == _bots_royale_decide_default);
	/* behind the beam: out, even fighting */
	view = calm(); view.margin_now = 2; view.fighting = 1; view.seconds_since_enemy = 0;
	CHECK("flee_beam_while_fighting", bots_royale_decide(&view) == _bots_royale_decide_flee_beam);
	CHECK("flee_beam_in_fight", bots_royale_decision_in_fight(_bots_royale_decide_flee_beam));
	view = calm(); view.margin_now = 2;
	CHECK("flee_beam", bots_royale_decide(&view) == _bots_royale_decide_flee_beam);
	/* the front to move on to it soon: in; not yet for a while: stays */
	view = calm(); view.margin_now = 30; view.margin_target = 4; view.seconds_to_move = 10;
	CHECK("move_in_on_countdown", bots_royale_decide(&view) == _bots_royale_decide_move_in);
	view.seconds_to_move = 100;
	CHECK("no_move_in_long_before", bots_royale_decide(&view) == _bots_royale_decide_default);
	view.seconds_to_move = 100; view.beam_moving = 1;
	CHECK("move_in_while_moving", bots_royale_decide(&view) == _bots_royale_decide_move_in);
	view = calm(); view.margin_now = 30; view.margin_target = 4; view.seconds_to_move = 10; view.fighting = 1;
	CHECK("move_in_while_fighting", bots_royale_decide(&view) == _bots_royale_decide_move_in);
	/* revive: only calm, and not into the beam */
	view = calm(); view.mate_standing = 0; view.mate_downed = 1; view.mate_distance = 12;
	CHECK("revive_when_calm", bots_royale_decide(&view) == _bots_royale_decide_revive);
	view.fighting = 1;
	CHECK("no_revive_fighting", bots_royale_decide(&view) != _bots_royale_decide_revive);
	view.fighting = 0; view.seconds_since_enemy = 1;
	CHECK("no_revive_enemy_near", bots_royale_decide(&view) != _bots_royale_decide_revive);
	view.seconds_since_enemy = 60; view.mate_margin_now = -5;
	CHECK("no_revive_in_beam", bots_royale_decide(&view) != _bots_royale_decide_revive);
	CHECK("revive_not_in_fight", !bots_royale_decision_in_fight(_bots_royale_decide_revive));
	/* the beam before a revive */
	view = calm(); view.mate_standing = 0; view.mate_downed = 1; view.margin_now = 1;
	CHECK("beam_before_revive", bots_royale_decide(&view) == _bots_royale_decide_flee_beam);
	/* its squadmate: back when strayed, not while fighting */
	view = calm(); view.mate_distance = 45;
	CHECK("regroup", bots_royale_decide(&view) == _bots_royale_decide_regroup);
	view.mate_distance = 15;
	CHECK("no_regroup_near", bots_royale_decide(&view) == _bots_royale_decide_default);
	view.mate_distance = 45; view.fighting = 1;
	CHECK("no_regroup_fighting", bots_royale_decide(&view) == _bots_royale_decide_default);
	/* the gulag: its fight, whatever else */
	view = calm(); view.in_gulag = 1; view.margin_now = -50; view.mate_downed = 1;
	CHECK("gulag_first", bots_royale_decide(&view) == _bots_royale_decide_gulag);
	CHECK("gulag_in_fight", bots_royale_decision_in_fight(_bots_royale_decide_gulag));
	view = calm(); view.in_gulag = 1; view.fighting = 1;
	CHECK("gulag_while_fighting", bots_royale_decide(&view) == _bots_royale_decide_gulag);

	/* loot tiers: worth more up the tiers, nothing for none */
	CHECK("tier_none", bots_royale_tier_value(-1) == 0);
	for (tries = 0; tries < 3; tries++)
		if (bots_royale_tier_value(tries + 1) <= bots_royale_tier_value(tries))
			break;
	CHECK("tier_rises", tries == 3);
	CHECK("tier_legendary_pistol_beats_common_rifle", 7 + bots_royale_tier_value(3) > 6 + bots_royale_tier_value(0));

	printf("failures %d\n", failures);
	return failures ? 1 : 0;
}
'''


def _run(tmp_path, mutation):
    work = tmp_path / ("mutant" if mutation else "rules")
    work.mkdir()
    driver = work / "driver.c"
    driver.write_text(DRIVER)
    rules = RULES
    if mutation:
        rules = work / "bots_royale_rules.c"
        rules.write_text("#define BOTS_ROYALE_RULES_MUTATION 1\n" + RULES.read_text())
    exe = compile_tool(work, [driver, rules])
    result = subprocess.run([str(exe)], capture_output=True, text=True)
    return result.returncode, result.stdout


def test_bots_royale_rules(tmp_path):
    code, output = _run(tmp_path, False)
    assert code == 0, output
    assert "failures 0" in output


def test_bots_royale_rules_catch_a_fight_before_the_beam(tmp_path):
    """negative control: the rules with a fight put before the beam fail, and on that check"""
    code, output = _run(tmp_path, True)
    assert code != 0
    assert "FAIL flee_beam_while_fighting" in output
    assert "FAIL move_in_while_fighting" in output
