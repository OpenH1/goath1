#include "harness.h"
#include <math.h>

#define TICKS_PER_SECOND 30
#define _pi ((real)3.14159265358979323846f)
#define MIN(a,b) ((a)>(b)?(b):(a))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define PIN(n,floor,ceiling) ((n)<(floor) ? (floor) : MIN((n),(ceiling)))
#include "config.inc"

/* the game's seeded random's shape: a sequence of its own from the seed */
static real real_seed_random_range(unsigned long *seed, real lower, real upper)
{
	*seed = *seed * 1664525UL + 1013904223UL;
	return lower + (upper - lower) * (real)((*seed >> 8) & 0xFFFF) / 65536.0f;
}

#include "under_test.inc"

static struct royale_bounds island(void)
{
	struct royale_bounds bounds = { { -80.0f, -60.0f, 0.0f }, { 120.0f, 90.0f, 25.0f } };

	return bounds;
}

static boolean inside2d(struct royale_bounds const *b, real_point3d const *p, real slack)
{
	return p->x >= b->minimum.x - slack && p->x <= b->maximum.x + slack &&
		p->y >= b->minimum.y - slack && p->y <= b->maximum.y + slack;
}

/* paths from random directions: each starts and ends outside the play area
by the margin, at the height, and crosses it; the directions cover the compass */
static void case_crosses(void)
{
	struct royale_bounds bounds = island();
	unsigned long seed = 12345;
	int quadrants = 0;
	int index;

	for (index = 0; index < 64; index++)
	{
		struct royale_flight_path path;
		long enter;
		long leave;
		real dx;
		real dy;

		royale_insertion_path_draw(&bounds, 55.0f, 0.0f, &seed, &path);
		CHECK(path.start.z == 55.0f && path.end.z == 55.0f, "path %d not at the height", index);
		CHECK(!inside2d(&bounds, &path.start, ROYALE_INSERTION_PATH_MARGIN - 0.5f), "path %d starts inside", index);
		CHECK(!inside2d(&bounds, &path.end, ROYALE_INSERTION_PATH_MARGIN - 0.5f), "path %d ends inside", index);
		CHECK(royale_insertion_path_window(&path, &bounds, &enter, &leave), "path %d misses the island", index);
		CHECK(enter > 0 && leave > enter, "path %d window %ld..%ld", index, enter, leave);
		dx = path.end.x - path.start.x;
		dy = path.end.y - path.start.y;
		quadrants |= 1 << ((dx >= 0.0f) + 2 * (dy >= 0.0f));
	}
	CHECK(quadrants == 15, "directions don't cover the compass (%x)", quadrants);
}

/* the same seed draws the same paths; another seed others */
static void case_seeded(void)
{
	struct royale_bounds bounds = island();
	unsigned long a = 777;
	unsigned long b = 777;
	unsigned long c = 778;
	struct royale_flight_path pa;
	struct royale_flight_path pb;
	struct royale_flight_path pc;

	royale_insertion_path_draw(&bounds, 50.0f, 0.0f, &a, &pa);
	royale_insertion_path_draw(&bounds, 50.0f, 0.0f, &b, &pb);
	royale_insertion_path_draw(&bounds, 50.0f, 0.0f, &c, &pc);
	CHECK(!memcmp(&pa, &pb, sizeof(pa)), "same seed, another path");
	CHECK(memcmp(&pa, &pc, sizeof(pa)), "another seed, the same path");
}

/* the Pelican's place along the path: the start at 0, the speed per tick,
the end held and FALSE past it */
static void case_point(void)
{
	struct royale_flight_path path = { { 0.0f, 0.0f, 40.0f }, { 300.0f, 0.0f, 40.0f }, 10.0f };
	real_point3d position;
	real_vector3d velocity;
	long last = (long)(300.0f / (10.0f / TICKS_PER_SECOND));

	CHECK(royale_insertion_path_point(&path, 0, &position, &velocity), "not flying at its start");
	CHECK(position.x == 0.0f && position.z == 40.0f, "start %f", position.x);
	CHECK(fabsf(velocity.i - 10.0f / TICKS_PER_SECOND) < 1e-6f && velocity.j == 0.0f, "velocity %f", velocity.i);
	CHECK(royale_insertion_path_point(&path, 30, &position, &velocity), "stopped at a second");
	CHECK(fabsf(position.x - 10.0f) < 1e-3f, "a second in: %f", position.x);
	CHECK(!royale_insertion_path_point(&path, last + 1, &position, &velocity), "flying past its end");
	CHECK(fabsf(position.x - 300.0f) < 1e-3f, "past the end: %f", position.x);
	/* (no speed: the crossing time's, within the limits) */
	path.speed = 0.0f;
	CHECK(fabsf(royale_insertion_path_speed(&path) - 300.0f / ROYALE_INSERTION_DEFAULT_CROSSING_SECONDS) < 1e-3f,
		"speed %f", royale_insertion_path_speed(&path));
	path.end.x = 10000.0f;
	CHECK(royale_insertion_path_speed(&path) == ROYALE_INSERTION_MAXIMUM_SPEED, "speed over the limit");
}

/* the jump window: inside the island from its first tick to its last, and
not before or after */
static void case_window(void)
{
	struct royale_bounds bounds = island();
	unsigned long seed = 99;
	int index;

	for (index = 0; index < 16; index++)
	{
		struct royale_flight_path path;
		real_point3d position;
		real_vector3d velocity;
		long enter;
		long leave;

		royale_insertion_path_draw(&bounds, 50.0f, 0.0f, &seed, &path);
		CHECK(royale_insertion_path_window(&path, &bounds, &enter, &leave), "no window");
		royale_insertion_path_point(&path, enter, &position, &velocity);
		CHECK(inside2d(&bounds, &position, 0.01f), "outside at the window's start");
		royale_insertion_path_point(&path, leave, &position, &velocity);
		CHECK(inside2d(&bounds, &position, 0.01f), "outside at the window's end");
		royale_insertion_path_point(&path, enter - 2, &position, &velocity);
		CHECK(!inside2d(&bounds, &position, 0.0f), "inside before the window");
		royale_insertion_path_point(&path, leave + 2, &position, &velocity);
		CHECK(!inside2d(&bounds, &position, 0.0f), "inside after the window");
	}
	/* (a path beside the island has none) */
	{
		struct royale_flight_path beside = { { -200.0f, 200.0f, 50.0f }, { 200.0f, 200.0f, 50.0f }, 10.0f };
		long enter;
		long leave;

		CHECK(!royale_insertion_path_window(&beside, &bounds, &enter, &leave), "a window beside the island");
	}
}

/* air control: only where steered, by at most the acceleration a tick, up
to the air speed, horizontally only */
static void case_air(void)
{
	real_vector3d facing = { 0.0f, 1.0f, 0.0f };
	real_vector3d velocity = { 0.05f, 0.0f, -0.5f };
	real step = ROYALE_INSERTION_AIR_ACCELERATION / (real)(TICKS_PER_SECOND * TICKS_PER_SECOND);
	real speed = ROYALE_INSERTION_AIR_SPEED / (real)TICKS_PER_SECOND;
	int tick;

	royale_insertion_air_control(&velocity, 0.0f, 0.0f, &facing);
	CHECK(velocity.i == 0.05f && velocity.j == 0.0f && velocity.k == -0.5f, "moved with no throttle");
	velocity.i = 0.0f;
	royale_insertion_air_control(&velocity, 1.0f, 0.0f, &facing);
	CHECK(velocity.j > 0.0f && fabsf(velocity.j - step) < 1e-6f, "forward step %f (%f)", velocity.j, step);
	CHECK(velocity.k == -0.5f, "vertical changed");
	for (tick = 0; tick < 600; tick++)
		royale_insertion_air_control(&velocity, 1.0f, 0.0f, &facing);
	CHECK(fabsf(velocity.j - speed) < 1e-5f && fabsf(velocity.i) < 1e-6f, "top speed %f %f", velocity.i, velocity.j);
	/* (left of north is west) */
	velocity.i = velocity.j = 0.0f;
	royale_insertion_air_control(&velocity, 0.0f, 1.0f, &facing);
	CHECK(velocity.i < 0.0f && fabsf(velocity.j) < 1e-6f, "left went %f %f", velocity.i, velocity.j);
	/* (a full diagonal is no faster) */
	velocity.i = velocity.j = 0.0f;
	for (tick = 0; tick < 600; tick++)
		royale_insertion_air_control(&velocity, 1.0f, 1.0f, &facing);
	CHECK(sqrtf(velocity.i * velocity.i + velocity.j * velocity.j) <= speed + 1e-5f, "diagonal too fast");
}

/* the landing: grounded after the grace, not in the step off the deck, not
in the air */
static void case_landed(void)
{
	CHECK(!royale_insertion_landed(1, 1), "landed stepping off");
	CHECK(!royale_insertion_landed(ROYALE_INSERTION_LANDING_GRACE_TICKS, ROYALE_INSERTION_SETTLE_TICKS),
		"landed in the grace");
	CHECK(!royale_insertion_landed(200, 0), "landed in the air");
	/* (a brush of a ledge on the way down: still falling, still immune) */
	CHECK(!royale_insertion_landed(200, 1), "landed brushing a ledge");
	CHECK(!royale_insertion_landed(200, ROYALE_INSERTION_SETTLE_TICKS - 1), "landed before settling");
	CHECK(royale_insertion_landed(200, ROYALE_INSERTION_SETTLE_TICKS), "not landed on the ground");
}

/* the safe area: what the beam has not reached, and redeploys inside it */
static void case_safe(void)
{
	struct royale_bounds bounds = island();
	struct royale_bounds safe;
	unsigned long seed = 4;
	int side;

	royale_insertion_safe_area(&bounds, NONE, 0.5f, &safe);
	CHECK(!memcmp(&safe, &bounds, sizeof(safe)), "no beam, a smaller area");
	for (side = 0; side < NUMBER_OF_ROYALE_SIDES; side++)
	{
		int draw;

		royale_insertion_safe_area(&bounds, (short)side, 0.5f, &safe);
		CHECK(fabsf((safe.maximum.x - safe.minimum.x) * (safe.maximum.y - safe.minimum.y) -
			0.5f * (bounds.maximum.x - bounds.minimum.x) * (bounds.maximum.y - bounds.minimum.y)) < 1.0f,
			"side %d: not half", side);
		for (draw = 0; draw < 32; draw++)
		{
			real_point3d point;
			real_point3d glassed = { 0.0f, 0.0f, 0.0f };

			royale_insertion_redeploy_point(&safe, 60.0f, &seed, &point);
			CHECK(inside2d(&safe, &point, 0.0f) && point.z == 60.0f, "side %d: dropped outside", side);
			/* (the glassed half: the side's own edge) */
			switch (side)
			{
			case ROYALE_SIDE_NORTH: CHECK(point.y < 15.0f, "north: in the glass (%f)", point.y); break;
			case ROYALE_SIDE_SOUTH: CHECK(point.y > 15.0f, "south: in the glass (%f)", point.y); break;
			case ROYALE_SIDE_EAST: CHECK(point.x < 20.0f, "east: in the glass (%f)", point.x); break;
			case ROYALE_SIDE_WEST: CHECK(point.x > 20.0f, "west: in the glass (%f)", point.x); break;
			}
			(void)glassed;
		}
	}
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "crosses")) case_crosses();
	else if (!strcmp(name, "seeded")) case_seeded();
	else if (!strcmp(name, "point")) case_point();
	else if (!strcmp(name, "window")) case_window();
	else if (!strcmp(name, "air")) case_air();
	else if (!strcmp(name, "landed")) case_landed();
	else if (!strcmp(name, "safe")) case_safe();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
