/*
ROYALE_CONFIG.C (test)

The real Royale map config reader (port/linux/game/royale_config.c): a good
file reads, and a value out of range refuses the whole file (an infinity or
a NaN, a box upside down, a structure BSP the scenario cannot have, a speed
or a beam stage's duration of nothing). test_royale_config.py takes the code
(royale_config.inc).
*/

#include "harness.h"

#define csmemset memset
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))

void platform_log(char const *format, ...);
void platform_log(char const *format, ...)
{
	(void)format;
}

/* (royale.c's: a test's file setting, none here) */
static char const *royale_test_setting(char const *variable)
{
	(void)variable;
	return NULL;
}

#include "royale_config.inc"

static struct royale_map_config config;

static char const good[] =
	"map = \"b30\"\n"
	"structure_bsp = 0\n"
	"bounds_min = [-95, -90, -1]\n"
	"bounds_max = [108, 80, 120]\n"
	"[[flight_path]]\n"
	"start = [0, 0, 60]\n"
	"end = [10, 10, 60]\n"
	"speed = 9.0\n"
	"[[beam_stage]]\n"
	"delay = 90\n"
	"start = 0.0\n"
	"end = 0.25\n"
	"duration = 60\n"
	"damage_per_second = 0.03\n"
	"[gulag]\n"
	"bounds_min = [-28.5, 39.5, 1]\n"
	"bounds_max = [-17.5, 47.5, 9]\n";

/* the good file with one line changed: whether it reads */
static boolean reads_with(char const *before, char const *after)
{
	static char text[sizeof(good) + 256];
	char const *at = strstr(good, before);

	CHECK(at, "no line %s", before);
	snprintf(text, sizeof(text), "%.*s%s%s", (int)(at - good), good, after, at + strlen(before));
	return royale_map_config_parse(text, &config);
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	CASE("good")
	{
		CHECK(royale_map_config_parse(good, &config), "the good file refused");
		CHECK(config.flight_path_count == 1 && config.beam_stage_count == 1 && config.flight_paths[0].speed == 9.0f,
			"read otherwise");
		/* (a flight with no speed given: the insertion's own) */
		CHECK(reads_with("speed = 9.0\n", ""), "a flight with no speed refused");
		return 0;
	}
	CASE("not-finite")
	{
		CHECK(!reads_with("delay = 90\n", "delay = inf\n"), "an infinity read");
		CHECK(!reads_with("delay = 90\n", "delay = nan\n"), "a NaN read");
		CHECK(!reads_with("bounds_max = [108, 80, 120]\n", "bounds_max = [1e300, 80, 120]\n"), "a huge number read");
		return 0;
	}
	CASE("bounds")
	{
		CHECK(!reads_with("bounds_max = [108, 80, 120]\n", "bounds_max = [-108, 80, 120]\n"), "the map's box upside down");
		CHECK(!reads_with("bounds_max = [-17.5, 47.5, 9]\n", "bounds_max = [-17.5, 47.5, 0]\n"),
			"the gulag's box upside down");
		return 0;
	}
	CASE("structure-bsp")
	{
		CHECK(!reads_with("structure_bsp = 0\n", "structure_bsp = -1\n"), "BSP -1 read");
		CHECK(!reads_with("structure_bsp = 0\n", "structure_bsp = 40\n"), "BSP 40 read");
		CHECK(!reads_with("structure_bsp = 0\n", "structure_bsp = 1.5\n"), "BSP 1.5 read");
		CHECK(reads_with("structure_bsp = 0\n", "structure_bsp = 3\n"), "BSP 3 refused");
		return 0;
	}
	CASE("speed-duration")
	{
		CHECK(!reads_with("speed = 9.0\n", "speed = 0\n"), "a speed of 0 read");
		CHECK(!reads_with("duration = 60\n", "duration = 0\n"), "a duration of 0 read");
		CHECK(!reads_with("duration = 60\n", ""), "a stage with no duration read");
		CHECK(!reads_with("end = 0.25\n", "end = 1.5\n"), "a stage past the map's length read");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
