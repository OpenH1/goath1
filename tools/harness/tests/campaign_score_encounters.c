/* Campaign scoring's encounters cleared (campaign_score.c's
campaign_score_encounters_step): an encounter whose living enemies all died
cleared once, with its bonus and its flawless bonus; none for enemies moved
to another encounter (migrated), erased, still to respawn, or refilled after
the bonus; and a checkpoint's revert, whose copy of the tallies is taken
back and whose first tick only looks. */
#include "harness.h"
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define csmemset memset
#define csmemcpy memcpy
#include "types.inc"
#include "under_test.inc"

static struct campaign_score_encounter_tracking tracking;
static long died[16];
static short died_count;
static boolean respawning;

static boolean unit_died(long unit_index)
{
	short index;

	for (index = 0; index < died_count; index++)
	{
		if (died[index] == unit_index)
			return TRUE;
	}
	return FALSE;
}

static boolean respawn_pending(short encounter)
{
	(void)encounter;
	return respawning;
}

static struct campaign_score_encounter_clear cleared[8];

/* a tick: the living units (with their encounters), the units died since
the last, the players' deaths; the encounters cleared */
static long tick(long const *units, short const *encounters, long count, long const *dead, short dead_count,
	long deaths)
{
	short index;

	for (index = 0; index < dead_count; index++)
		died[index] = dead[index];
	died_count = dead_count;
	return campaign_score_encounters_step(&tracking, units, encounters, count, unit_died, respawn_pending, deaths,
		cleared, NUMBEROF(cleared));
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static long const one_two[] = { 1, 2 };
	static short const in_3[] = { 3, 3 };
	static short const in_4[] = { 4, 4 };
	static long const two[] = { 2 };
	static short const in_4_one[] = { 4 };
	static long const one[] = { 1 };
	static short const in_3_one[] = { 3 };

	memset(&tracking, 0, sizeof(tracking));
	respawning = FALSE;

	CASE("clear")
	{
		tracking.tallies.kill_points[3] = 400;
		tracking.tallies.kills[3] = 2;
		CHECK(tick(one_two, in_3, 2, NULL, 0, 0) == 0, "a fight begun");
		CHECK(tick(one_two, in_3, 1, NULL, 0, 0) == 0, "one left");
		CHECK(tick(NULL, NULL, 0, one_two, 2, 0) == 1, "both dead: cleared");
		CHECK(cleared[0].encounter == 3 && cleared[0].kills == 2 && cleared[0].bonus == 100,
			"a quarter of its 400 points: %ld", cleared[0].bonus);
		CHECK(cleared[0].flawless_bonus == 50, "no player died: half again, %ld", cleared[0].flawless_bonus);
		CHECK(tick(NULL, NULL, 0, NULL, 0, 0) == 0, "once");
		memset(&tracking, 0, sizeof(tracking));
		tracking.tallies.kill_points[3] = 40;
		tick(one, in_3_one, 1, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 1 && cleared[0].bonus == 50, "at least 50: %ld", cleared[0].bonus);
		return 0;
	}
	CASE("flawless")
	{
		tick(one_two, in_3, 2, NULL, 0, 4);
		CHECK(tick(NULL, NULL, 0, one_two, 2, 5) == 1 && cleared[0].flawless_bonus == 0,
			"a player died during it: not flawless");
		return 0;
	}
	CASE("migrate")
	{
		tracking.tallies.kill_points[4] = 200;
		tick(one_two, in_3, 2, NULL, 0, 0);
		/* (2 moved to encounter 4 as 1 died) */
		CHECK(tick(two, in_4_one, 1, one, 1, 0) == 0, "one dead, one migrated: encounter 3 not cleared");
		CHECK(tick(NULL, NULL, 0, two, 1, 0) == 1 && cleared[0].encounter == 4 && cleared[0].bonus == 50,
			"the migrated one dead: encounter 4 cleared");
		memset(&tracking, 0, sizeof(tracking));
		tick(one_two, in_3, 2, NULL, 0, 0);
		CHECK(tick(one_two, in_4, 2, NULL, 0, 0) == 0, "all moved: none");
		CHECK(tick(NULL, NULL, 0, one_two, 2, 0) == 1 && cleared[0].encounter == 4, "and cleared where they went");
		return 0;
	}
	CASE("erase")
	{
		tick(one_two, in_3, 2, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 0, "one dead, one erased: not cleared");
		memset(&tracking, 0, sizeof(tracking));
		tick(one_two, in_3, 2, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, NULL, 0, 0) == 0, "all erased: not cleared");
		return 0;
	}
	CASE("respawn")
	{
		tick(one, in_3_one, 1, NULL, 0, 0);
		respawning = TRUE;
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 0, "empty, but still to respawn: not cleared");
		respawning = FALSE;
		tick(two, in_3_one, 1, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, two, 1, 0) == 1, "its respawns dead too: cleared");
		return 0;
	}
	CASE("refill")
	{
		tick(one, in_3_one, 1, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 1, "cleared");
		tick(two, in_3_one, 1, NULL, 0, 0);
		CHECK(tick(NULL, NULL, 0, two, 1, 0) == 0, "a script's refill cleared: no second bonus");
		return 0;
	}
	CASE("revert")
	{
		static long const nine[] = { 9 };
		static short const in_5[] = { 5 };
		struct campaign_score_encounter_tallies checkpoint;

		tick(one, in_3_one, 1, NULL, 0, 0);
		checkpoint = tracking.tallies;
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 1, "cleared after the checkpoint");
		tick(nine, in_5, 1, NULL, 0, 0);
		/* (the revert: the checkpoint's tallies, encounter 3 living again,
		encounter 5 not yet begun, and unit 9's index now a body's) */
		tracking.tallies = checkpoint;
		tracking.rescan = TRUE;
		CHECK(tick(one, in_3_one, 1, nine, 1, 0) == 0, "the revert's first tick only looks");
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 1 && cleared[0].encounter == 3, "played again: cleared again");
		return 0;
	}
	CASE("swarm")
	{
		static long const forms[] = { 1, 2, 3 };
		static short const in_3_all[] = { 3, 3, 3 };
		static long const infection[] = { 2, 3 };

		tick(forms, in_3_all, 3, NULL, 0, 0);
		CHECK(tick(one, in_3_one, 1, infection, 2, 0) == 0, "the infection forms dead, a combat form left");
		CHECK(tick(NULL, NULL, 0, one, 1, 0) == 1, "and it dead: cleared");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
