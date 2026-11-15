/*
ROYALE_BEAM.C (test)

The real sweep of Royale's glassing beam (port/linux/game/royale_beam.c):
royale_beam_status_at, where the front is at a tick (each stage holds, then
moves its front from its start to its end over its duration; the last
stage's end stays), and how much damage it does; royale_beam_depth_at, how
far a point is behind the front (out of the bounds counts as behind); and
royale_beam_choose_side, the side drawn from the game's seed among the
config's. test_royale_beam.py takes the code (under_test.inc).
*/

#include "harness.h"

#include <math.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define ROYALE_MAXIMUM_BEAM_STAGES 8

#include "under_test.inc"

/* two stages at 30 ticks a second: hold 10 s, 0 -> 0.5 over 20 s; hold 5 s,
0.5 -> 0.9 over 10 s (its damage the default's) */
static struct royale_beam_stage const stages[] =
{
	{ 10.0f, 0.0f, 0.5f, 20.0f, 3.0f },
	{ 5.0f, 0.5f, 0.9f, 10.0f, 0.0f },
};

static struct royale_bounds const bounds = { { -100.0f, -50.0f, 0.0f }, { 100.0f, 150.0f, 50.0f } };

static boolean near(real a, real b)
{
	return fabsf(a - b) < 1.0e-4f;
}

/* the damage a player standing at x, y takes over ticks [0, end): as the
game does it, a share of the stage's per second each tick behind the front */
static real damage_over(real x, real y, short side, long end)
{
	real total = 0.0f;
	long tick;

	for (tick = 0; tick < end; tick++)
	{
		struct royale_beam_status status;

		royale_beam_status_at(stages, 2, 30.0f, tick, &status);
		if (royale_beam_depth_at(&bounds, side, status.front, x, y) > 0.0f)
			total += status.damage_per_second / 30.0f;
	}
	return total;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	struct royale_beam_status status;

	CASE("hold")
	{
		royale_beam_status_at(stages, 2, 30.0f, 0, &status);
		CHECK(status.stage == 0 && !status.moving, "the first tick is not the first stage's hold");
		CHECK(near(status.front, 0.0f) && near(status.target, 0.5f), "the hold's front %f, target %f",
			status.front, status.target);
		CHECK(status.ticks_to_move == 300, "the countdown is %ld ticks, not 300", status.ticks_to_move);
		royale_beam_status_at(stages, 2, 30.0f, 299, &status);
		CHECK(!status.moving && status.ticks_to_move == 1, "the hold's last tick moves (%ld left)",
			status.ticks_to_move);
		return 0;
	}
	CASE("move")
	{
		royale_beam_status_at(stages, 2, 30.0f, 300, &status);
		CHECK(status.moving && near(status.front, 0.0f), "the move does not start at the stage's start");
		royale_beam_status_at(stages, 2, 30.0f, 300 + 300, &status);
		CHECK(status.moving && near(status.front, 0.25f), "halfway through the move the front is %f", status.front);
		CHECK(status.ticks_to_move == 0, "a moving front counts down");
		royale_beam_status_at(stages, 2, 30.0f, 900, &status);
		CHECK(status.stage == 1 && !status.moving && near(status.front, 0.5f),
			"after the first move the second stage does not hold at 0.5 (stage %d, front %f)",
			status.stage, status.front);
		CHECK(status.ticks_to_move == 150, "the second hold counts %ld", status.ticks_to_move);
		return 0;
	}
	CASE("end")
	{
		royale_beam_status_at(stages, 2, 30.0f, 900 + 150 + 300, &status);
		CHECK(status.stage == 2 && !status.moving && near(status.front, 0.9f),
			"after the last stage the front is not at its end (stage %d, front %f)", status.stage, status.front);
		royale_beam_status_at(stages, 2, 30.0f, 1000000, &status);
		CHECK(near(status.front, 0.9f) && status.ticks_to_move == 0, "long after, the front moved on");
		/* the same tick, the same front, whatever the rate's ticks */
		royale_beam_status_at(stages, 2, 120.0f, 4 * 600, &status);
		CHECK(near(status.front, 0.25f), "at 120 ticks a second the front is %f, not 0.25", status.front);
		return 0;
	}
	CASE("depth")
	{
		/* from the north (+y, the bounds 200 long): a quarter glassed is y above 100 */
		CHECK(near(royale_beam_depth_at(&bounds, 0, 0.25f, 0.0f, 120.0f), 20.0f), "behind the north front");
		CHECK(royale_beam_depth_at(&bounds, 0, 0.25f, 0.0f, 80.0f) < 0.0f, "ahead of the north front");
		/* from the west (-x, 200 long): half glassed is x below 0 */
		CHECK(near(royale_beam_depth_at(&bounds, 3, 0.5f, -30.0f, 0.0f), 30.0f), "behind the west front");
		CHECK(royale_beam_depth_at(&bounds, 3, 0.5f, 30.0f, 0.0f) < 0.0f, "ahead of the west front");
		/* east and south */
		CHECK(royale_beam_depth_at(&bounds, 1, 0.5f, 30.0f, 0.0f) > 0.0f, "behind the east front");
		CHECK(royale_beam_depth_at(&bounds, 2, 0.5f, 0.0f, 0.0f) > 0.0f, "behind the south front");
		/* out of the bounds is behind, whatever the front */
		CHECK(royale_beam_depth_at(&bounds, 0, 0.0f, 120.0f, 50.0f) > 0.0f, "out of the bounds is safe");
		CHECK(royale_beam_depth_at(&bounds, 0, 0.0f, 0.0f, 50.0f) < 0.0f, "inside, nothing glassed, is behind");
		return 0;
	}
	CASE("damage")
	{
		/* at the north edge's band (y 140), glassed when the front passes 0.05
		(tick 300 + 0.1 * 600 = 360): 3 a second until tick 900, then the
		default second stage's 0.04 a second for 150 ticks (past the front from tick 361) */
		real taken = damage_over(0.0f, 140.0f, 0, 900 + 150);
		real safe = damage_over(0.0f, -40.0f, 0, 900 + 150);

		CHECK(fabsf(taken - (3.0f * (900 - 361) / 30.0f + 0.04f * 150 / 30.0f)) < 0.05f,
			"the glassed band took %f", taken);
		CHECK(safe == 0.0f, "the far side took %f before the front reached it", safe);
		/* none during the first hold */
		CHECK(damage_over(0.0f, 140.0f, 0, 300) == 0.0f, "damage during the first hold");
		return 0;
	}
	CASE("side")
	{
		long seed;
		short counts[4] = { 0, 0, 0, 0 };

		for (seed = 0; seed < 400; seed++)
		{
			short side = royale_beam_choose_side((unsigned long)seed * 7919UL, 0);

			CHECK(side >= 0 && side < 4, "side %d", side);
			CHECK(side == royale_beam_choose_side((unsigned long)seed * 7919UL, 0), "the same seed, another side");
			counts[side]++;
			/* only the config's sides (east, west) */
			side = royale_beam_choose_side((unsigned long)seed, (1L << 1) | (1L << 3));
			CHECK(side == 1 || side == 3, "a side the config left out: %d", side);
		}
		CHECK(counts[0] > 40 && counts[1] > 40 && counts[2] > 40 && counts[3] > 40,
			"the seeds favor a side: %d %d %d %d", counts[0], counts[1], counts[2], counts[3]);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
