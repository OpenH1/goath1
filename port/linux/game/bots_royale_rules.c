/*
BOTS_ROYALE_RULES.C

The bots' Royale decisions as plain rules (bots_royale_rules.h): the beam's
margins and the way out from behind it, the squads' jump points and
landings, the free fall's steering, and what a bot does first in play. No
game state and no random numbers of its own: the same numbers in give the
same decision on every machine and every run.
*/

#include <math.h>

#include "bots_royale_rules.h"

#define BOTS_ROYALE_PI 3.14159265358979f
/* (a refuge kept this far inside the box's edges) */
#define BOTS_ROYALE_EDGE 4.0f

static float bots_royale_clamp(float value, float least, float most)
{
	return value < least ? least : value > most ? most : value;
}

float bots_royale_beam_margin(short side, float fraction, struct bots_royale_box const *box, float x, float y)
{
	float width = box->maximum_x - box->minimum_x;
	float depth = box->maximum_y - box->minimum_y;

	switch (side)
	{
	case _bots_royale_side_north: return (box->maximum_y - fraction * depth) - y;
	case _bots_royale_side_south: return y - (box->minimum_y + fraction * depth);
	case _bots_royale_side_east: return (box->maximum_x - fraction * width) - x;
	case _bots_royale_side_west: return x - (box->minimum_x + fraction * width);
	default: return BOTS_ROYALE_NO_BEAM;
	}
}

void bots_royale_beam_refuge(short side, float fraction, struct bots_royale_box const *box, float margin,
	float *x, float *y)
{
	float width = box->maximum_x - box->minimum_x;
	float depth = box->maximum_y - box->minimum_y;
	float least_x = box->minimum_x + BOTS_ROYALE_EDGE, most_x = box->maximum_x - BOTS_ROYALE_EDGE;
	float least_y = box->minimum_y + BOTS_ROYALE_EDGE, most_y = box->maximum_y - BOTS_ROYALE_EDGE;

	/* (no room left ahead of it: as far in as the box goes) */
	switch (side)
	{
	case _bots_royale_side_north:
	{
		float line = box->maximum_y - fraction * depth - margin;

		if (*y > line)
			*y = line;
		break;
	}
	case _bots_royale_side_south:
	{
		float line = box->minimum_y + fraction * depth + margin;

		if (*y < line)
			*y = line;
		break;
	}
	case _bots_royale_side_east:
	{
		float line = box->maximum_x - fraction * width - margin;

		if (*x > line)
			*x = line;
		break;
	}
	case _bots_royale_side_west:
	{
		float line = box->minimum_x + fraction * width + margin;

		if (*x < line)
			*x = line;
		break;
	}
	default:
		break;
	}
	*x = bots_royale_clamp(*x, least_x, most_x);
	*y = bots_royale_clamp(*y, least_y, most_y);
}

float bots_royale_jump_fraction(short rank, short count, float random)
{
	float share;

	if (count < 1)
		count = 1;
	if (rank < 0)
		rank = 0;
	rank = (short)(rank % count);
	/* (the path's ends left: the squads jump between a tenth and nine
	tenths of the way, each in its own share, anywhere in its middle half) */
	share = 0.8f / (float)count;
	return 0.1f + share * ((float)rank + 0.25f + 0.5f * bots_royale_clamp(random, 0.0f, 1.0f));
}

void bots_royale_landing(float start_x, float start_y, float end_x, float end_y, float fraction,
	float side_random, float spread, struct bots_royale_box const *box, float *x, float *y)
{
	float dx = end_x - start_x, dy = end_y - start_y;
	float length = (float)sqrt(dx * dx + dy * dy);
	float px, py;

	px = start_x + dx * fraction;
	py = start_y + dy * fraction;
	if (length > 0.001f)
	{
		/* (to the side: the path's normal) */
		px += -dy / length * side_random * spread;
		py += dx / length * side_random * spread;
	}
	*x = bots_royale_clamp(px, box->minimum_x + BOTS_ROYALE_EDGE, box->maximum_x - BOTS_ROYALE_EDGE);
	*y = bots_royale_clamp(py, box->minimum_y + BOTS_ROYALE_EDGE, box->maximum_y - BOTS_ROYALE_EDGE);
}

int bots_royale_chord(struct bots_royale_box const *box, float x, float y, float velocity_x, float velocity_y,
	float *start_x, float *start_y, float *end_x, float *end_y)
{
	float low = -1.0e30f, high = 1.0e30f;
	float origin[2], velocity[2], least[2], most[2];
	int axis;

	origin[0] = x; origin[1] = y;
	velocity[0] = velocity_x; velocity[1] = velocity_y;
	least[0] = box->minimum_x; least[1] = box->minimum_y;
	most[0] = box->maximum_x; most[1] = box->maximum_y;
	if (velocity_x * velocity_x + velocity_y * velocity_y < 0.000001f)
		return 0;
	/* (the slabs: each axis's span of the line inside the box) */
	for (axis = 0; axis < 2; axis++)
	{
		if (fabs(velocity[axis]) < 0.000001f)
		{
			if (origin[axis] < least[axis] || origin[axis] > most[axis])
				return 0;
		}
		else
		{
			float a = (least[axis] - origin[axis]) / velocity[axis];
			float b = (most[axis] - origin[axis]) / velocity[axis];

			if (a > b)
			{
				float swap = a;
				a = b;
				b = swap;
			}
			if (a > low)
				low = a;
			if (b < high)
				high = b;
		}
	}
	if (low > high)
		return 0;
	*start_x = x + velocity_x * low;
	*start_y = y + velocity_y * low;
	*end_x = x + velocity_x * high;
	*end_y = y + velocity_y * high;
	return 1;
}

float bots_royale_path_progress(float start_x, float start_y, float end_x, float end_y, float x, float y)
{
	float dx = end_x - start_x, dy = end_y - start_y;
	float length2 = dx * dx + dy * dy;

	if (length2 < 0.000001f)
		return 1.0f;
	return ((x - start_x) * dx + (y - start_y) * dy) / length2;
}

int bots_royale_should_jump(float progress, float fraction, int over_island)
{
	/* (never off the island, before the way over it starts, nor past it:
	the Pelican's line in and out of the bounds) */
	if (!over_island || progress < 0.0f || progress > 1.0f)
		return 0;
	return progress >= fraction || progress >= 0.9f;
}

void bots_royale_fall_steer(float x, float y, float to_x, float to_y, float *yaw, float *throttle)
{
	float dx = to_x - x, dy = to_y - y;
	float distance = (float)sqrt(dx * dx + dy * dy);

	*yaw = distance > 0.001f ? (float)atan2(dy, dx) : 0.0f;
	/* (full on, easing off over the last four units: no circling the
	landing) */
	*throttle = bots_royale_clamp(distance / 4.0f, 0.0f, 1.0f);
}

short bots_royale_decide(struct bots_royale_view const *view)
{
#ifdef BOTS_ROYALE_RULES_MUTATION
	/* (tools/test_bots_royale.py's negative control: a fight before the
	beam, which its checks must catch) */
	if (view->fighting && !view->in_gulag)
		return _bots_royale_decide_default;
#endif
	/* the gulag: its 1v1, nothing else (its walls keep the beam off) */
	if (view->in_gulag)
		return _bots_royale_decide_gulag;
	/* behind or at the beam: out, before any fight */
	if (view->margin_now < BOTS_ROYALE_FLEE_MARGIN)
		return _bots_royale_decide_flee_beam;
	/* the front moving on to it, or about to: inwards in time */
	if (view->margin_target < BOTS_ROYALE_MOVE_IN_MARGIN &&
		(view->beam_moving || view->seconds_to_move < BOTS_ROYALE_MOVE_IN_SECONDS))
	{
		return _bots_royale_decide_move_in;
	}
	/* a downed squadmate, no enemy about, and they are not in the beam */
	if (view->mate_downed && !view->fighting && view->seconds_since_enemy >= BOTS_ROYALE_REVIVE_CALM_SECONDS &&
		view->mate_margin_now >= BOTS_ROYALE_FLEE_MARGIN)
	{
		return _bots_royale_decide_revive;
	}
	/* strayed from its squadmate, out of a fight */
	if (view->mate_standing && !view->fighting && view->mate_distance > BOTS_ROYALE_REGROUP_DISTANCE)
		return _bots_royale_decide_regroup;
	return _bots_royale_decide_default;
}

int bots_royale_decision_in_fight(short decision)
{
	/* (the beam's, and the gulag's: at its opponent, armed or not, never
	off after the island's loot) */
	return decision == _bots_royale_decide_flee_beam || decision == _bots_royale_decide_move_in ||
		decision == _bots_royale_decide_gulag;
}

short bots_royale_tier_value(short tier)
{
	/* (bots.c's values run 3 to 9 a weapon: a tier up beats a weapon a
	little better, a legendary pistol a common rocket launcher's near
	match) */
	return tier < 0 ? (short)0 : (short)(2 * tier);
}
