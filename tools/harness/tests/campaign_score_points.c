/* Campaign scoring's rules (campaign_score.c): the points table, a victim's
class from its tags' names, a kill's bonuses, multi-kills within their window,
streaks, and deductions that never take a score below 0. */
#include "harness.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#include "types.inc"
#include "under_test.inc"

static struct campaign_score_event events[64];
static long event_count;

static long kill(struct campaign_score_player *player, struct campaign_score_timing *timing, long tick,
	short victim_class, short category, boolean head, real multiplier)
{
	return campaign_score_player_kill(player, timing, 0, tick, victim_class, category, head, FALSE, multiplier,
		events, &event_count);
}

/* ... a blow lethal to the unsuspecting that killed outright */
static long kill_unsuspecting(struct campaign_score_player *player, struct campaign_score_timing *timing, long tick,
	short victim_class, short category, real multiplier)
{
	return campaign_score_player_kill(player, timing, 0, tick, victim_class, category, FALSE, TRUE, multiplier,
		events, &event_count);
}

static long count_kind(short kind)
{
	long index;
	long count = 0;

	for (index = 0; index < event_count; index++)
		count += events[index].kind == kind;
	return count;
}

static struct campaign_score_event const *last_of(short kind)
{
	long index;

	for (index = event_count - 1; index >= 0; index--)
	{
		if (events[index].kind == kind)
			return &events[index];
	}
	return NULL;
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	struct campaign_score_player player;
	struct campaign_score_timing timing;

	memset(&player, 0, sizeof(player));
	memset(&timing, 0, sizeof(timing));
	event_count = 0;

	CASE("table")
	{
		CHECK(kill(&player, &timing, 0, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f) ==
			10, "a grunt on easy: 10");
		timing.multi_count = 0;
		CHECK(kill(&player, &timing, 1000, _campaign_score_class_elite_major, _campaign_score_damage_bullet, FALSE,
			3.0f) == 225, "an elite major on legendary: 75 times 3");
		CHECK(player.score == 235 && player.kills == 2, "the tallies: %ld points, %ld kills", player.score,
			player.kills);
		CHECK(player.kills_by_class[_campaign_score_class_grunt] == 1 &&
			player.kills_by_class[_campaign_score_class_elite_major] == 1, "the kills by class");
		CHECK(campaign_score_points[_campaign_score_class_hunter].points == 150, "a hunter: 150");
		CHECK(kill(&player, &timing, 5000, _campaign_score_class_vehicle_heavy, _campaign_score_damage_high_explosive,
			FALSE, 1.0f) == 200 && last_of(_campaign_score_event_vehicle), "a wraith: a vehicle's event of 200");
		CHECK(kill(&player, &timing, 9000, 99, _campaign_score_damage_bullet, FALSE, 1.0f) == 10,
			"a class out of range is any enemy's");
		return 0;
	}
	CASE("names")
	{
		CHECK(campaign_score_class_from_names("characters\\elite\\elite", "characters\\elite\\elite major",
			FALSE) == _campaign_score_class_elite_major, "an elite major");
		CHECK(campaign_score_class_from_names("characters\\elite\\elite", "characters\\elite\\elite stealth major",
			FALSE) == _campaign_score_class_elite_special, "a stealth elite");
		CHECK(campaign_score_class_from_names("characters\\elite\\elite", "characters\\elite\\elite minor", FALSE) ==
			_campaign_score_class_elite, "an elite minor");
		CHECK(campaign_score_class_from_names("characters\\Elite\\Elite", NULL, FALSE) == _campaign_score_class_elite,
			"any case, no variant");
		CHECK(campaign_score_class_from_names("characters\\floodcombat elite\\floodcombat elite", NULL, FALSE) ==
			_campaign_score_class_flood_combat, "a combat form of an elite is the flood's");
		CHECK(campaign_score_class_from_names("characters\\flood_infection\\flood_infection", NULL, FALSE) ==
			_campaign_score_class_flood_infection, "an infection form");
		CHECK(campaign_score_class_from_names("characters\\floodcarrier\\floodcarrier", NULL, FALSE) ==
			_campaign_score_class_flood_carrier, "a carrier form");
		CHECK(campaign_score_class_from_names("characters\\hunter\\hunter", NULL, FALSE) == _campaign_score_class_hunter,
			"a hunter");
		CHECK(campaign_score_class_from_names("characters\\jackal\\jackal", NULL, FALSE) == _campaign_score_class_jackal,
			"a jackal");
		CHECK(campaign_score_class_from_names("characters\\grunt\\grunt", NULL, FALSE) == _campaign_score_class_grunt,
			"a grunt");
		CHECK(campaign_score_class_from_names("characters\\sentinel\\sentinel", NULL, FALSE) ==
			_campaign_score_class_sentinel, "a sentinel");
		CHECK(campaign_score_class_from_names("vehicles\\wraith\\wraith", NULL, TRUE) ==
			_campaign_score_class_vehicle_heavy, "a wraith");
		CHECK(campaign_score_class_from_names("vehicles\\ghost\\ghost", NULL, TRUE) ==
			_campaign_score_class_vehicle_light, "a ghost");
		CHECK(campaign_score_class_from_names("custom\\thing", NULL, FALSE) == _campaign_score_class_other,
			"a custom map's unit: any enemy");
		return 0;
	}
	CASE("bonuses")
	{
		CHECK(kill(&player, &timing, 0, _campaign_score_class_elite, _campaign_score_damage_sniper, TRUE, 1.0f) == 65 &&
			player.headshots == 1 && count_kind(_campaign_score_event_headshot) == 1, "a headshot: 50 and 15");
		event_count = 0;
		timing.multi_count = 0;
		CHECK(kill(&player, &timing, 1000, _campaign_score_class_grunt, _campaign_score_damage_grenade, TRUE, 1.0f) ==
			15 && player.headshots == 1 && count_kind(_campaign_score_event_grenade) == 1,
			"a grenade's blow to the head: no headshot, the grenade's 5");
		event_count = 0;
		timing.multi_count = 0;
		CHECK(kill(&player, &timing, 2000, _campaign_score_class_jackal, _campaign_score_damage_melee, FALSE, 2.0f) ==
			60 && player.melee_kills == 1, "a melee on heroic: (20 + 10) times 2");
		return 0;
	}
	CASE("assassination")
	{
		CHECK(kill_unsuspecting(&player, &timing, 0, _campaign_score_class_elite, _campaign_score_damage_melee, 1.0f) ==
			75 && count_kind(_campaign_score_event_assassination) == 1 && count_kind(_campaign_score_event_melee) == 0,
			"an elite assassinated: 50 and 25, no melee's bonus besides");
		CHECK(player.melee_kills == 1, "a melee kill all the same");
		CHECK(campaign_score_bonuses.assassination > campaign_score_bonuses.melee, "more than a melee's");
		event_count = 0;
		timing.multi_count = 0;
		CHECK(kill_unsuspecting(&player, &timing, 1000, _campaign_score_class_grunt, _campaign_score_damage_melee,
			3.0f) == 105, "a grunt on legendary: (10 + 25) times 3");
		event_count = 0;
		timing.multi_count = 0;
		CHECK(kill(&player, &timing, 2000, _campaign_score_class_grunt, _campaign_score_damage_melee, FALSE, 1.0f) == 20 &&
			count_kind(_campaign_score_event_assassination) == 0 && count_kind(_campaign_score_event_melee) == 1,
			"a melee from the front: the melee's 10");
		event_count = 0;
		timing.multi_count = 0;
		CHECK(kill_unsuspecting(&player, &timing, 3000, _campaign_score_class_grunt, _campaign_score_damage_bullet,
			1.0f) == 10 && count_kind(_campaign_score_event_assassination) == 0, "no melee's blow: none");
		return 0;
	}
	CASE("driver-assist")
	{
		CHECK(campaign_score_driver_assist_points(_campaign_score_class_elite, 1.0f) == 25, "an elite: half of 50");
		CHECK(campaign_score_driver_assist_points(_campaign_score_class_elite_major, 1.0f) == 38,
			"an elite major: 37.5 to the nearest");
		CHECK(campaign_score_driver_assist_points(_campaign_score_class_grunt, 3.0f) == 15,
			"a grunt on legendary: 5 times 3");
		CHECK(campaign_score_driver_assist_points(_campaign_score_class_vehicle_heavy, 1.5f) == 150,
			"a wraith on normal: 100 times 1.5");
		CHECK(campaign_score_driver_assist_points(_campaign_score_class_flood_infection, 1.0f) == 1,
			"an infection form: 1");
		CHECK(campaign_score_driver_assist_points(99, 1.0f) == 5, "a class out of range: any enemy's");
		return 0;
	}
	CASE("multi")
	{
		kill(&player, &timing, 0, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_multi_kill) == 0, "one kill: no multi-kill");
		kill(&player, &timing, 60, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(last_of(_campaign_score_event_multi_kill) && last_of(_campaign_score_event_multi_kill)->points == 25 &&
			last_of(_campaign_score_event_multi_kill)->victim_class == 2, "a double kill: 25");
		kill(&player, &timing, 120, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(last_of(_campaign_score_event_multi_kill)->points == 50 &&
			last_of(_campaign_score_event_multi_kill)->victim_class == 3, "a triple kill: 50");
		CHECK(player.best_multi == 3 && player.score == 3 * 10 + 25 + 50, "best 3, %ld points", player.score);
		event_count = 0;
		kill(&player, &timing, 1000, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_multi_kill) == 0 && player.best_multi == 3,
			"a kill long after: a new count");
		return 0;
	}
	CASE("multi-window")
	{
		kill(&player, &timing, 0, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		kill(&player, &timing, CAMPAIGN_SCORE_MULTI_KILL_TICKS + 1, _campaign_score_class_grunt,
			_campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_multi_kill) == 0, "a tick past the window: no multi-kill");
		kill(&player, &timing, 2 * CAMPAIGN_SCORE_MULTI_KILL_TICKS + 1, _campaign_score_class_grunt,
			_campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_multi_kill) == 1, "at the window's end: a double kill");
		return 0;
	}
	CASE("multi-cap")
	{
		long index;

		for (index = 0; index < 9; index++)
			kill(&player, &timing, index, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(last_of(_campaign_score_event_multi_kill)->points == 200 && player.best_multi == 9,
			"past six kills: 200 each");
		return 0;
	}
	CASE("streak")
	{
		long index;

		for (index = 0; index < 5; index++)
			kill(&player, &timing, index * 1000, _campaign_score_class_grunt, _campaign_score_damage_bullet, FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_streak) == 1 && last_of(_campaign_score_event_streak)->points == 50 &&
			player.best_streak == 5, "five kills: a streak of 50");
		campaign_score_player_death(&player, &timing);
		CHECK(player.current_streak == 0 && player.best_streak == 5 && player.deaths == 1, "a death ends it");
		event_count = 0;
		for (index = 0; index < 4; index++)
			kill(&player, &timing, 10000 + index * 1000, _campaign_score_class_grunt, _campaign_score_damage_bullet,
				FALSE, 1.0f);
		CHECK(count_kind(_campaign_score_event_streak) == 0, "four after the death: none");
		for (index = 0; index < 16; index++)
			kill(&player, &timing, 20000 + index * 1000, _campaign_score_class_grunt, _campaign_score_damage_bullet,
				FALSE, 1.0f);
		CHECK(player.best_streak == 20 && last_of(_campaign_score_event_streak)->points == 200,
			"twenty: the most a step gives, 200");
		return 0;
	}
	CASE("swarm")
	{
		long index;

		for (index = 0; index < 6; index++)
			kill(&player, &timing, index, _campaign_score_class_flood_infection, _campaign_score_damage_bullet, FALSE,
				1.0f);
		CHECK(player.score == 12 && player.current_streak == 0 && count_kind(_campaign_score_event_multi_kill) == 0,
			"infection forms: 2 each, no multi-kill or streak (%ld points)", player.score);
		return 0;
	}
	CASE("deduct")
	{
		player.score = 10;
		CHECK(campaign_score_player_death(&player, &timing) == 10 && player.score == 0, "a death at 10: 10 taken");
		CHECK(campaign_score_player_deduct(&player, 100) == 0 && player.score == 0, "never below 0");
		player.score = 500;
		CHECK(campaign_score_player_deduct(&player, campaign_score_bonuses.betrayal_player) == 100 &&
			player.score == 400, "a player betrayed: 100");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
