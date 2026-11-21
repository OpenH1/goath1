/*
ROYALE_MAP_KEEP.C (test)

The real rule that keeps Royale's players on the island and out of the
interior (port/linux/game/royale_map.c's royale_map_keep_point): a point in
a blocked entrance's box goes out of its nearest side (in x or y, its height
kept); one out of the bounds (or below their bottom, in the sea) goes back
to the last point it stood inside; one inside is remembered as that point.
test_royale_map_keep.py takes the code (generated.inc).
*/

#include "harness.h"

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define ROYALE_MAP_PUSH_MARGIN 0.25f

struct royale_bounds
{
	real_point3d minimum;
	real_point3d maximum;
};

struct royale_blocked_entrance
{
	struct royale_bounds bounds;
};

/* (only what the rule reads) */
struct royale_map_config
{
	struct royale_bounds bounds;
	short blocked_entrance_count;
	struct royale_blocked_entrance blocked_entrances[4];
};

#include "generated.inc"

static struct royale_map_config config =
{
	{ { -95.0f, -90.0f, -1.0f }, { 108.0f, 80.0f, 120.0f } },
	1,
	{ { { { -11.0f, 26.5f, 11.0f }, { -1.5f, 31.0f, 18.0f } } } },
};

static int near(real a, real b)
{
	return a - b < 0.001f && b - a < 0.001f;
}

static void case_blocked(void)
{
	boolean safe_valid = FALSE;
	real_point3d safe = { 0 };
	/* just inside the box's -y side: out of that side, height kept */
	real_point3d point = { -6.0f, 27.0f, 13.0f };

	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_pushed_out, "%s", "keep");
	CHECK(near(point.y, 26.5f - ROYALE_MAP_PUSH_MARGIN) && near(point.x, -6.0f) && near(point.z, 13.0f), "%s", "keep");
	/* near its +x side */
	point.x = -2.0f;
	point.y = 29.0f;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_pushed_out, "%s", "keep");
	CHECK(near(point.x, -1.5f + ROYALE_MAP_PUSH_MARGIN) && near(point.y, 29.0f), "%s", "keep");
	/* now out of it: inside the island, a safe point */
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_inside, "%s", "keep");
	CHECK(safe_valid && near(safe.x, point.x), "%s", "keep");
	/* above the box (its z range) passes */
	point.x = -6.0f;
	point.y = 28.0f;
	point.z = 19.0f;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_inside, "%s", "keep");
}

static void case_bounds(void)
{
	boolean safe_valid = FALSE;
	real_point3d safe = { 0 };
	real_point3d beach = { -80.0f, 10.0f, 0.5f };
	real_point3d point;

	/* out before ever standing inside: left */
	point.x = -120.0f;
	point.y = 0.0f;
	point.z = 2.0f;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_lost, "%s", "keep");
	CHECK(!safe_valid && near(point.x, -120.0f), "%s", "keep");
	/* on the beach: remembered */
	point = beach;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_inside, "%s", "keep");
	/* wading out into the sea, below the bottom: back on the beach */
	point.x = -84.0f;
	point.z = -1.4f;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_put_back, "%s", "keep");
	CHECK(near(point.x, beach.x) && near(point.y, beach.y) && near(point.z, beach.z), "%s", "keep");
	/* past the side: back too */
	point.x = 0.0f;
	point.y = 85.0f;
	point.z = 1.0f;
	CHECK(royale_map_keep_point(&config, &point, &safe_valid, &safe) == _royale_map_kept_put_back, "%s", "keep");
	CHECK(near(point.y, beach.y), "%s", "keep");
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "blocked"))
		case_blocked();
	else if (!strcmp(name, "bounds"))
		case_bounds();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
