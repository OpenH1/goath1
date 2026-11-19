#include "harness.h"

/* MCC's Zoom In and Zoom Out (source/game/player_control.c's player_control_zoom_input, source/items/weapons.c's
weapon_step_zoom_level) beside the original's cycle (weapon_rotate_zoom_level): a step in stops at the weapon's last
level, a step out at none, neither wraps; a reloading weapon keeps its zoom; and every level they give is one the
cycle gives too, so the player's action (desired_zoom_level) carries an ordinary zoom to the other machines. */

#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

enum
{
	_player_control_input_zoom_bit = 2,
	_player_control_input_zoom_in_bit = 6,
	_player_control_input_zoom_out_bit,
};

struct weapon_datum { long definition_index; };
struct weapon_definition { struct { short zoom_level_count; } weapon; };

/* the fake world: one weapon, its zoom levels, and whether it reloads */
static struct weapon_datum world_weapon;
static struct weapon_definition world_definition;
static boolean world_reloading;
static struct weapon_datum *weapon_get(long weapon_index) { (void)weapon_index; return &world_weapon; }
static struct weapon_definition *weapon_definition_get(long index) { (void)index; return &world_definition; }
static boolean weapon_reloading(long weapon_index) { (void)weapon_index; return world_reloading; }

#include "under_test.inc"

#define IN FLAG(_player_control_input_zoom_in_bit)
#define OUT FLAG(_player_control_input_zoom_out_bit)
#define CYCLE FLAG(_player_control_input_zoom_bit)

static short press(short zoom_level, unsigned long flags)
{
	return player_control_zoom_input(7, zoom_level, flags);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	short count;

	CASE("steps_in_and_out_and_stop")
	{
		world_definition.weapon.zoom_level_count = 2;
		CHECK(press(NONE, IN) == 0, "in from none: %d", press(NONE, IN));
		CHECK(press(0, IN) == 1, "in from 0: %d", press(0, IN));
		CHECK(press(1, IN) == 1, "in at the last level wrapped or moved: %d", press(1, IN));
		CHECK(press(1, OUT) == 0, "out from 1: %d", press(1, OUT));
		CHECK(press(0, OUT) == NONE, "out from 0: %d", press(0, OUT));
		CHECK(press(NONE, OUT) == NONE, "out at none wrapped or moved: %d", press(NONE, OUT));
		/* (no zoom pressed: unchanged) */
		CHECK(press(1, 0) == 1, "no press changed it: %d", press(1, 0));
		/* a weapon of no zoom stays unzoomed; one of a level the weapon has not (swapped) comes back to its own */
		world_definition.weapon.zoom_level_count = 0;
		CHECK(press(NONE, IN) == NONE, "in with no zoom levels: %d", press(NONE, IN));
		world_definition.weapon.zoom_level_count = 1;
		CHECK(press(3, IN) == 0, "in past the weapon's levels: %d", press(3, IN));
		CHECK(press(3, OUT) == NONE, "out past the weapon's levels: %d", press(3, OUT));
		/* reloading, as the cycle: unchanged */
		world_definition.weapon.zoom_level_count = 2;
		world_reloading = TRUE;
		CHECK(press(NONE, IN) == NONE && press(1, OUT) == 1, "reloading changed it: %d %d", press(NONE, IN),
			press(1, OUT));
		world_reloading = FALSE;
		return 0;
	}
	CASE("cycle_unchanged_and_levels_the_cycles")
	{
		for (count = 0; count <= 4; count++)
		{
			short level;
			boolean cycle_reaches[6] = { 0 };
			short from = NONE;
			short i;

			world_definition.weapon.zoom_level_count = count;
			/* the cycle as the original's: up a level, at the last to none */
			for (i = 0; i < 6; i++)
			{
				short next = press(from, CYCLE);
				short expected = count == 0 ? NONE : from == count - 1 ? NONE : from + 1;

				CHECK(next == expected, "%d levels: the cycle from %d gave %d", count, from, next);
				/* (the cycle first, as one press of both is the original's alone) */
				CHECK(press(from, CYCLE | IN | OUT) == next, "%d levels: the cycle with the steps from %d", count,
					from);
				cycle_reaches[next + 1] = TRUE;
				from = next;
			}
			/* each step's level, from each the cycle reaches, is one it reaches too: the wire sees nothing new */
			for (level = NONE; level < count; level++)
			{
				short in = press(level, IN);
				short out = press(level, OUT);

				CHECK(cycle_reaches[level + 1], "%d levels: the cycle never reached %d", count, level);
				CHECK(cycle_reaches[in + 1] && cycle_reaches[out + 1], "%d levels: from %d in %d, out %d", count, level,
					in, out);
			}
		}
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
