/* royale_insertion.h: Royale's insertion (port/linux/game/royale_insertion.c)

In the INSERTION phase Pelicans fly straight paths across the map, each from
a direction drawn from the game's seeded random, at the map's drop height,
with squads aboard in their troop bays. A player jumps out with jump (or by
leaving the seat) whenever they like over the play area; anyone still aboard
when their Pelican leaves the play area is put out. Spartans fall with no
parachute: while FALLING a player steers in the air and takes no falling
damage, and lands with a heavy impact (screen shake, dust).

The Pelicans are scripted flight, not AI pilots: the host places each where
its path says every tick, and the network carries it to the clients. The
calls royale.h declares for this lane (royale_insertion_start, _tick, _draw,
royale_insertion_pelican_position) are defined in royale_insertion.c. */
#ifndef HALO_ROYALE_INSERTION_H
#define HALO_ROYALE_INSERTION_H

#include "royale.h"

/* ---- for the other lanes */

/* (gulag) a player who won the gulag dropped back in: their unit put high
over the current safe area (outside the glassed part, from the beam's front),
FALLING. Host/simulation, once per gulag win. */
void royale_insertion_redeploy(long player_index);

/* (bots) an IN_PELICAN player jumps at the next tick over the play area (as
jump pressed); FALSE if they are not aboard a Pelican */
boolean royale_insertion_jump(long player_index);
/* (core) the insertion lane is linked in */
boolean royale_insertion_running(void);
/* (core: game_engine.c's remap) TRUE while the insertion makes its own
objects (its Pelicans), which the game engine's vehicle set leaves alone */
boolean royale_insertion_creating(void);

/* ---- the rules (pure: the harness tests them, tools/harness/tests/
test_royale_insertion.py) */

enum
{
	/* the Pelicans' passengers' seats are those whose label has this in it
	(vehicles\pelican\pelican: p-riderlf, p-riderrf, p-riderlb ...) */
	ROYALE_INSERTION_MAXIMUM_PELICANS = 16,
	ROYALE_INSERTION_MAXIMUM_SEATS = 16,
	/* the ticks between one Pelican's start and the next's */
	ROYALE_INSERTION_STAGGER_TICKS = 45,
	/* ticks a jumper is airborne before a touch of the ground counts as the
	landing (the step off the deck) */
	ROYALE_INSERTION_LANDING_GRACE_TICKS = 4,
	/* ticks on the ground without a break that end a fall (a ledge or a tree
	brushed on the way down does not) */
	ROYALE_INSERTION_SETTLE_TICKS = 10,
};

/* the Pelican's speed (world units a second) when the path gives none, and
its flight's length over the play area it aims for (seconds) */
#define ROYALE_INSERTION_DEFAULT_CROSSING_SECONDS 32.0f
#define ROYALE_INSERTION_MINIMUM_SPEED 4.0f
#define ROYALE_INSERTION_MAXIMUM_SPEED 14.0f
/* the drop height above the highest player (or the play area's floor) when the config gives none, and the margin about the players when it gives no bounds */
#define ROYALE_INSERTION_DEFAULT_HEIGHT_ABOVE_PLAYERS 20.0f
#define ROYALE_INSERTION_PLAYERS_MARGIN 10.0f
/* paths begin and end this far outside the play area */
#define ROYALE_INSERTION_PATH_MARGIN 12.0f
/* the share of the play area's size a path's middle may stray from its center */
#define ROYALE_INSERTION_PATH_SPREAD 0.3f
/* air control: the horizontal speed steered toward (world units a second)
and how fast it is reached (world units a second, a second) */
#define ROYALE_INSERTION_AIR_SPEED 3.5f
#define ROYALE_INSERTION_AIR_ACCELERATION 3.0f
/* the share of the Pelican's velocity a jumper keeps */
#define ROYALE_INSERTION_EXIT_CARRY 0.5f
/* a redeploy's height above the ground's top, and its margin inside the safe area */
#define ROYALE_INSERTION_REDEPLOY_MARGIN 8.0f

/* a path across the play area: from a direction drawn from *seed, through a
point near its middle, beyond its edges by the margin, at height */
void royale_insertion_path_draw(struct royale_bounds const *bounds, real height, real speed,
	unsigned long *seed, struct royale_flight_path *path);
/* where a Pelican flying the path is after `ticks` (30 a second) and its
velocity (world units a tick): FALSE once it is past the path's end */
boolean royale_insertion_path_point(struct royale_flight_path const *path, long ticks,
	real_point3d *position, real_vector3d *velocity);
/* the ticks of the path inside the play area (from its edge in to its edge
out): FALSE if it never crosses it */
boolean royale_insertion_path_window(struct royale_flight_path const *path, struct royale_bounds const *bounds,
	long *enter_ticks, long *leave_ticks);
/* the speed a path flies at: its own, else its length over the crossing time
within the limits */
real royale_insertion_path_speed(struct royale_flight_path const *path);
/* one tick of air control: the velocity (world units a tick) steered by the
throttle (forward, left: -1..1) in the facing's frame; only the horizontal
part changes, by at most the acceleration, toward the air speed */
void royale_insertion_air_control(real_vector3d *velocity, real throttle_forward, real throttle_left,
	real_vector3d const *facing);
/* whether a falling player has landed: the ticks since the jump past
the grace, and on the ground for the settle ticks without a break */
boolean royale_insertion_landed(long ticks_falling, long ground_ticks);
/* the safe area: the play area less the part the beam has glassed or will by
the current stage's end (side NONE: all of it) */
void royale_insertion_safe_area(struct royale_bounds const *bounds, short side, real target_fraction,
	struct royale_bounds *safe);
/* a point to drop into the safe area, drawn from *seed, inside it by the margin */
void royale_insertion_redeploy_point(struct royale_bounds const *safe, real height, unsigned long *seed,
	real_point3d *point);

#endif
