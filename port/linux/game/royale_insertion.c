/*
ROYALE_INSERTION.C

Royale's insertion (royale_insertion.h): the Pelicans' scripted flights with
squads aboard, the jump, the free fall with air control and no falling damage
until the landing, the landing's impact, and the gulag winners' redeploy.

Who does what:
- The host (or a game of one machine) draws the paths, makes the Pelicans,
  seats the squads, places each Pelican on its path every tick, puts the
  jumpers out and moves the players' states (IN_PELICAN -> FALLING -> ALIVE).
  The network carries the Pelicans, the seats and the bipeds to the clients.
- Every machine steers its falling players (a client predicts its own),
  keeps them from falling damage and shows a landing's impact to its own
  players.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "effects/material_effects.h"
#include "effects/player_effects.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "math/real_math.h"
#include "networking/network_game_globals.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "cache/cache_files.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"
#include "units/biped_definitions.h"
#include "units/bipeds.h"
#include "units/units.h"
#include "units/vehicle_definitions.h"

#include "halo_port_limits.h"
#include "royale.h"
#include "royale_insertion.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

#define PELICAN_TAG_NAME "vehicles\\pelican\\pelican"
#define PELICAN_SEAT_SUBSTRING "rider"
/* a jumper starts this far below the Pelican's origin (clear of its hull),
spread sideways by their seat */
#define JUMP_DROP 3.0f
#define JUMP_SPREAD 0.7f
/* the landing's shake (the game's falling damage effect, without its
damage) and dust (the biped's jump-land material effect, about the feet) */
#define LANDING_SHAKE_SCALE 0.8f
#define LANDING_DUST_POINTS 6
#define LANDING_DUST_RADIUS 0.6f
/* (bipeds.c's _material_effect_biped_jump_land) */
#define MATERIAL_EFFECT_BIPED_JUMP_LAND 5
#define INSERTION_SEED_SALT 0x52594C45UL

/* the head of game globals' falling damage block (bipeds.c's
struct game_globals_falling_damage) */
struct insertion_falling_damage
{
	long falling_unused[2];
	real falling_distance_lower_bound;
	real falling_distance_upper_bound;
	struct tag_reference falling_damage;
};

/* ---------- globals */

struct insertion_pelican
{
	long object_index;
	struct royale_flight_path path;
	long start_tick; /* the insertion tick it starts flying on */
	long enter_ticks, leave_ticks; /* its jump window, in its own ticks */
	boolean done;
};

struct insertion_player
{
	short pelican; /* NONE when not aboard */
	short seat_slot; /* the order boarded, for the spread */
	boolean jump_requested;
	boolean jump_was_down;
	boolean falling;
	long falling_ticks;
	long ground_ticks; /* on the ground without a break */
	boolean impact_shown;
	long unit_index; /* the unit falling */
	/* the unit this machine saw land, until the host's state says it is
	no longer falling (a client's land comes before the host's ALIVE) */
	long landed_unit;
};

static struct
{
	boolean started;
	boolean boarded;
	boolean creating; /* (royale_insertion_creating) */
	boolean bounds_from_players;
	unsigned long seed;
	long insertion_ticks;
	struct royale_bounds bounds;
	real height;
	struct royale_map_config const *config;
	short pelican_count;
	struct insertion_pelican pelicans[ROYALE_INSERTION_MAXIMUM_PELICANS];
	struct insertion_player players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
} insertion;

static void insertion_log_area(char const *from);
static void insertion_bounds_from_players(long const *players, short player_count);

/* ---------- the rules (pure) */

static real path_length(struct royale_flight_path const *path, real_vector3d *direction)
{
	real_vector3d d;
	real length;

	d.i = path->end.x - path->start.x;
	d.j = path->end.y - path->start.y;
	d.k = path->end.z - path->start.z;
	length = sqrtf(d.i * d.i + d.j * d.j + d.k * d.k);
	if (direction)
	{
		if (length > 0.0f)
		{
			d.i /= length;
			d.j /= length;
			d.k /= length;
		}
		*direction = d;
	}
	return length;
}

/* the part of the line p + t d (horizontal) inside the box, as [t0, t1]:
FALSE if it misses */
static boolean line_box2d(real px, real py, real dx, real dy, struct royale_bounds const *box, real margin,
	real *t0, real *t1)
{
	real lo = -1.0e30f;
	real hi = 1.0e30f;
	real p[2] = { px, py };
	real d[2] = { dx, dy };
	real mn[2] = { box->minimum.x - margin, box->minimum.y - margin };
	real mx[2] = { box->maximum.x + margin, box->maximum.y + margin };
	int axis;

	for (axis = 0; axis < 2; axis++)
	{
		if (fabsf(d[axis]) < 1.0e-6f)
		{
			if (p[axis] < mn[axis] || p[axis] > mx[axis])
				return FALSE;
		}
		else
		{
			real a = (mn[axis] - p[axis]) / d[axis];
			real b = (mx[axis] - p[axis]) / d[axis];

			if (a > b)
			{
				real swap = a;

				a = b;
				b = swap;
			}
			if (a > lo)
				lo = a;
			if (b < hi)
				hi = b;
		}
	}
	if (lo > hi)
		return FALSE;
	*t0 = lo;
	*t1 = hi;
	return TRUE;
}

void royale_insertion_path_draw(struct royale_bounds const *bounds, real height, real speed,
	unsigned long *seed, struct royale_flight_path *path)
{
	real angle = real_seed_random_range(seed, 0.0f, 2.0f * _pi);
	real size_x = bounds->maximum.x - bounds->minimum.x;
	real size_y = bounds->maximum.y - bounds->minimum.y;
	real cx = 0.5f * (bounds->minimum.x + bounds->maximum.x) +
		real_seed_random_range(seed, -ROYALE_INSERTION_PATH_SPREAD, ROYALE_INSERTION_PATH_SPREAD) * 0.5f * size_x;
	real cy = 0.5f * (bounds->minimum.y + bounds->maximum.y) +
		real_seed_random_range(seed, -ROYALE_INSERTION_PATH_SPREAD, ROYALE_INSERTION_PATH_SPREAD) * 0.5f * size_y;
	real dx = cosf(angle);
	real dy = sinf(angle);
	real t0 = 0.0f;
	real t1 = 0.0f;

	/* (the middle is inside the box, so the line always crosses it) */
	line_box2d(cx, cy, dx, dy, bounds, ROYALE_INSERTION_PATH_MARGIN, &t0, &t1);
	path->start.x = cx + dx * t0;
	path->start.y = cy + dy * t0;
	path->start.z = height;
	path->end.x = cx + dx * t1;
	path->end.y = cy + dy * t1;
	path->end.z = height;
	path->speed = speed;
}

real royale_insertion_path_speed(struct royale_flight_path const *path)
{
	real speed = path->speed;

	if (!(speed > 0.0f))
	{
		speed = path_length(path, NULL) / ROYALE_INSERTION_DEFAULT_CROSSING_SECONDS;
		speed = PIN(speed, ROYALE_INSERTION_MINIMUM_SPEED, ROYALE_INSERTION_MAXIMUM_SPEED);
	}
	return speed;
}

boolean royale_insertion_path_point(struct royale_flight_path const *path, long ticks,
	real_point3d *position, real_vector3d *velocity)
{
	real_vector3d direction;
	real length = path_length(path, &direction);
	real per_tick = royale_insertion_path_speed(path) / (real)TICKS_PER_SECOND;
	real along;

	if (ticks < 0)
		ticks = 0;
	along = per_tick * (real)ticks;
	if (along > length)
		along = length;
	position->x = path->start.x + direction.i * along;
	position->y = path->start.y + direction.j * along;
	position->z = path->start.z + direction.k * along;
	velocity->i = direction.i * per_tick;
	velocity->j = direction.j * per_tick;
	velocity->k = direction.k * per_tick;
	return per_tick * (real)ticks < length;
}

boolean royale_insertion_path_window(struct royale_flight_path const *path, struct royale_bounds const *bounds,
	long *enter_ticks, long *leave_ticks)
{
	real_vector3d direction;
	real length = path_length(path, &direction);
	real per_tick = royale_insertion_path_speed(path) / (real)TICKS_PER_SECOND;
	real horizontal = sqrtf(direction.i * direction.i + direction.j * direction.j);
	real t0;
	real t1;

	if (!(length > 0.0f) || !(horizontal > 0.0f) ||
		!line_box2d(path->start.x, path->start.y, direction.i, direction.j, bounds, 0.0f, &t0, &t1))
	{
		return FALSE;
	}
	if (t1 < 0.0f || t0 > length)
		return FALSE;
	if (t0 < 0.0f)
		t0 = 0.0f;
	if (t1 > length)
		t1 = length;
	*enter_ticks = (long)ceilf(t0 / per_tick);
	*leave_ticks = (long)floorf(t1 / per_tick);
	return *leave_ticks >= *enter_ticks;
}

void royale_insertion_air_control(real_vector3d *velocity, real throttle_forward, real throttle_left,
	real_vector3d const *facing)
{
	real fi = facing->i;
	real fj = facing->j;
	real fl = sqrtf(fi * fi + fj * fj);
	real wish_i;
	real wish_j;
	real throttle = sqrtf(throttle_forward * throttle_forward + throttle_left * throttle_left);
	real speed = ROYALE_INSERTION_AIR_SPEED / (real)TICKS_PER_SECOND;
	real step = ROYALE_INSERTION_AIR_ACCELERATION / (real)(TICKS_PER_SECOND * TICKS_PER_SECOND);
	real di;
	real dj;
	real change;

	if (fl > 0.0f)
	{
		fi /= fl;
		fj /= fl;
	}
	else
	{
		fi = 1.0f;
		fj = 0.0f;
	}
	if (throttle > 1.0f)
	{
		throttle_forward /= throttle;
		throttle_left /= throttle;
	}
	/* (left of the facing is its turn a quarter counterclockwise) */
	wish_i = (fi * throttle_forward - fj * throttle_left) * speed;
	wish_j = (fj * throttle_forward + fi * throttle_left) * speed;
	/* with no throttle, keep what the jump carried: steer only where asked */
	if (throttle <= 0.0f)
		return;
	di = wish_i - velocity->i;
	dj = wish_j - velocity->j;
	change = sqrtf(di * di + dj * dj);
	if (change > step)
	{
		di *= step / change;
		dj *= step / change;
	}
	velocity->i += di;
	velocity->j += dj;
}

boolean royale_insertion_landed(long ticks_falling, long ground_ticks)
{
	return ticks_falling > ROYALE_INSERTION_LANDING_GRACE_TICKS && ground_ticks >= ROYALE_INSERTION_SETTLE_TICKS;
}

void royale_insertion_safe_area(struct royale_bounds const *bounds, short side, real target_fraction,
	struct royale_bounds *safe)
{
	real fraction = PIN(target_fraction, 0.0f, 0.95f);
	real size_x = bounds->maximum.x - bounds->minimum.x;
	real size_y = bounds->maximum.y - bounds->minimum.y;

	*safe = *bounds;
	switch (side)
	{
	case ROYALE_SIDE_NORTH: safe->maximum.y = bounds->maximum.y - size_y * fraction; break;
	case ROYALE_SIDE_SOUTH: safe->minimum.y = bounds->minimum.y + size_y * fraction; break;
	case ROYALE_SIDE_EAST: safe->maximum.x = bounds->maximum.x - size_x * fraction; break;
	case ROYALE_SIDE_WEST: safe->minimum.x = bounds->minimum.x + size_x * fraction; break;
	default: break;
	}
}

void royale_insertion_redeploy_point(struct royale_bounds const *safe, real height, unsigned long *seed,
	real_point3d *point)
{
	real margin_x = MIN(ROYALE_INSERTION_REDEPLOY_MARGIN, 0.25f * (safe->maximum.x - safe->minimum.x));
	real margin_y = MIN(ROYALE_INSERTION_REDEPLOY_MARGIN, 0.25f * (safe->maximum.y - safe->minimum.y));

	point->x = real_seed_random_range(seed, safe->minimum.x + margin_x, safe->maximum.x - margin_x);
	point->y = real_seed_random_range(seed, safe->minimum.y + margin_y, safe->maximum.y - margin_y);
	point->z = height;
}

/* ---------- the game */

static boolean insertion_authority(void)
{
	return !network_game_distributed_client();
}

static struct insertion_player *insertion_player_get(long player_index)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	return player_index != NONE && VALID_INDEX(slot, HALO_PORT_MAXIMUM_NETWORK_PLAYERS) ?
		&insertion.players[slot] : NULL;
}

static long insertion_player_unit(long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player && player->unit_index != NONE && biped_try_and_get(player->unit_index) ?
		player->unit_index : NONE;
}

static long insertion_pelican_tag(void)
{
	long tag_index = tag_loaded(VEHICLE_DEFINITION_TAG, PELICAN_TAG_NAME);

	if (tag_index == NONE)
	{
		struct tag_iterator iterator;

		/* (another map's Pelican: the first vehicle named for one) */
		tag_iterator_new(&iterator, VEHICLE_DEFINITION_TAG);
		for (tag_index = tag_iterator_next(&iterator); tag_index != NONE; tag_index = tag_iterator_next(&iterator))
		{
			char const *name = tag_get_name(tag_index);

			if (name && strstr(name, "pelican"))
				break;
		}
	}
	return tag_index;
}

static void insertion_pelican_place(struct insertion_pelican *pelican)
{
	real_point3d position;
	real_vector3d velocity;
	real_vector3d forward;
	struct object_datum *object;
	long ticks = insertion.insertion_ticks - pelican->start_tick;

	if (pelican->object_index == NONE || !object_try_and_get_and_verify_type(pelican->object_index, _object_mask_unit))
	{
		pelican->object_index = NONE;
		return;
	}
	if (!royale_insertion_path_point(&pelican->path, ticks, &position, &velocity))
		pelican->done = TRUE;
	forward = velocity;
	forward.k = 0.0f;
	if (normalize3d(&forward) == 0.0f)
		forward = *global_forward3d;
	object_set_position(pelican->object_index, &position, &forward, global_up3d);
	object = object_get(pelican->object_index);
	/* (it flies on, waiting for its start: still) */
	if (ticks < 0 || pelican->done)
		velocity = *global_zero_vector3d;
	object->object.translational_velocity = velocity;
	object->object.angular_velocity = *global_zero_vector3d;
	object->object.flags &= ~FLAG(_object_at_rest_bit);
}

static short insertion_pelican_seats(long vehicle_index, short *seats)
{
	return vehicle_scripting_find_available_seats(vehicle_index, PELICAN_SEAT_SUBSTRING, NONE, seats,
		ROYALE_INSERTION_MAXIMUM_SEATS);
}

static void insertion_set_falling_immunity(long unit_index, boolean immune)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);

	if (unit)
		SET_FLAG(unit->unit.flags, _unit_no_falling_damage_bit, immune);
}

static void insertion_fall_begin(long player_index, long unit_index)
{
	struct insertion_player *state = insertion_player_get(player_index);

	if (!state)
		return;
	state->pelican = NONE;
	state->falling = TRUE;
	state->falling_ticks = 0;
	state->ground_ticks = 0;
	state->impact_shown = FALSE;
	state->unit_index = unit_index;
	state->jump_requested = FALSE;
	insertion_set_falling_immunity(unit_index, TRUE);
	if (insertion_authority())
		royale_set_player_state(player_index, ROYALE_PLAYER_FALLING);
}

/* a rider out of the Pelican: below its hull, carrying part of its speed */
static void insertion_jump(long player_index, struct insertion_pelican *pelican)
{
	struct insertion_player *state = insertion_player_get(player_index);
	long unit_index = insertion_player_unit(player_index);
	struct object_datum *vehicle;
	real_point3d position;
	real_vector3d left;
	real side;

	if (!state || unit_index == NONE)
		return;
	if (object_get(unit_index)->object.parent_object_index != NONE)
		unit_exit_seat_end(unit_index);
	/* (the Pelican's handle checked: one gone leaves the jumper where they are) */
	vehicle = pelican->object_index != NONE ?
		object_try_and_get_and_verify_type(pelican->object_index, _object_mask_unit) : NULL;
	if (vehicle)
	{
		left.i = -vehicle->object.forward.j;
		left.j = vehicle->object.forward.i;
		left.k = 0.0f;
		side = ((state->seat_slot & 1) ? 1.0f : -1.0f) * (1 + state->seat_slot / 2) * JUMP_SPREAD;
		position = vehicle->object.position;
		position.x += left.i * side;
		position.y += left.j * side;
		position.z -= JUMP_DROP;
		object_set_position(unit_index, &position, NULL, NULL);
		object_get(unit_index)->object.translational_velocity.i =
			vehicle->object.translational_velocity.i * ROYALE_INSERTION_EXIT_CARRY;
		object_get(unit_index)->object.translational_velocity.j =
			vehicle->object.translational_velocity.j * ROYALE_INSERTION_EXIT_CARRY;
		object_get(unit_index)->object.translational_velocity.k = 0.0f;
	}
	insertion_fall_begin(player_index, unit_index);
}

/* a player into a Pelican's seat. In a free-for-all every rider is every
other's enemy, and a unit may not sit beside an enemy (unit_can_enter_seat):
the riders aboard take the boarding unit's team for the moment it sits */
static boolean insertion_enter_seat(long unit_index, long vehicle_index, short seat_index)
{
	short team = object_get(unit_index)->object.owner_team_index;
	short teams[ROYALE_INSERTION_MAXIMUM_SEATS + 4];
	long children[ROYALE_INSERTION_MAXIMUM_SEATS + 4];
	short count = 0;
	long child_index;
	boolean entered;

	for (child_index = object_get(vehicle_index)->object.first_child_object_index;
		child_index != NONE && count < (short)NUMBEROF(children);
		child_index = object_get(child_index)->object.next_object_index)
	{
		struct object_datum *child = object_get(child_index);

		if (TEST_FLAG(_object_mask_unit, child->object.type))
		{
			children[count] = child_index;
			teams[count++] = child->object.owner_team_index;
			child->object.owner_team_index = team;
		}
	}
	entered = unit_enter_seat(unit_index, vehicle_index, seat_index);
	while (count-- > 0)
		object_get(children[count])->object.owner_team_index = teams[count];
	return entered;
}

/* the squads' players in the order they board: squad by squad, a squad's
players together; players of no squad after */
static short insertion_boarding_order(long *players, short maximum)
{
	short count = 0;
	short squad;
	struct data_iterator iterator;
	struct player_datum *player;

	for (squad = 0; squad < ROYALE_MAXIMUM_SQUADS; squad++)
	{
		short seat;

		for (seat = 0; seat < ROYALE_SQUAD_SIZE; seat++)
		{
			long player_index = royale_squad_member(squad, seat);

			if (player_index != NONE && count < maximum && insertion_player_unit(player_index) != NONE)
				players[count++] = player_index;
		}
	}
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		if (royale_squad_of(iterator.datum_index) == NONE && count < maximum &&
			insertion_player_unit(iterator.datum_index) != NONE)
		{
			players[count++] = iterator.datum_index;
		}
	}
	return count;
}

static void insertion_board(void)
{
	long tag_index = insertion_pelican_tag();
	long players[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	short player_count = insertion_boarding_order(players, (short)NUMBEROF(players));
	short seats[ROYALE_INSERTION_MAXIMUM_SEATS];
	short seats_per_pelican = 0;
	short pelican_count;
	short players_per_pelican;
	short index;
	short next = 0;

	short config_paths[ROYALE_MAXIMUM_FLIGHT_PATHS];
	short config_path_count = 0;

	insertion.boarded = TRUE;
	if (insertion.bounds_from_players)
		insertion_bounds_from_players(players, player_count);
	/* the map's paths with a length, shuffled */
	if (insertion.config)
	{
		short path_index;

		for (path_index = 0; path_index < insertion.config->flight_path_count &&
			path_index < ROYALE_MAXIMUM_FLIGHT_PATHS; path_index++)
		{
			struct royale_flight_path const *path = &insertion.config->flight_paths[path_index];

			if (path_length(path, NULL) > 1.0f)
				config_paths[config_path_count++] = path_index;
		}
		for (path_index = config_path_count - 1; path_index > 0; path_index--)
		{
			short other = seed_random_range(&insertion.seed, 0, (short)(path_index + 1));
			short swap = config_paths[path_index];

			config_paths[path_index] = config_paths[other];
			config_paths[other] = swap;
		}
	}
	if (tag_index == NONE || player_count == 0)
	{
		if (tag_index == NONE)
			error(_error_log, "royale insertion: no Pelican in this map: players drop from the sky");
		/* (no Pelican: everyone drops from their own path's start) */
		for (index = 0; index < player_count; index++)
		{
			long unit_index = insertion_player_unit(players[index]);
			real_point3d point;

			royale_insertion_redeploy_point(&insertion.bounds, insertion.height, &insertion.seed, &point);
			object_set_position(unit_index, &point, NULL, NULL);
			insertion_fall_begin(players[index], unit_index);
		}
		return;
	}
	for (index = 0; index < ROYALE_INSERTION_MAXIMUM_PELICANS && next < player_count; index++)
	{
		struct insertion_pelican *pelican = &insertion.pelicans[index];
		struct object_placement_data placement;
		real_point3d position;
		real_vector3d velocity;
		short seat_count;
		short seat;
		short boarded = 0;

		csmemset(pelican, 0, sizeof(*pelican));
		pelican->object_index = NONE;
		if (index < config_path_count)
		{
			/* (the map's own paths, drawn in a random order, each flown
			either way) */
			pelican->path = insertion.config->flight_paths[config_paths[index]];
			if (seed_random_range(&insertion.seed, 0, 2))
			{
				real_point3d swap = pelican->path.start;

				pelican->path.start = pelican->path.end;
				pelican->path.end = swap;
			}
		}
		else
		{
			royale_insertion_path_draw(&insertion.bounds, insertion.height, 0.0f, &insertion.seed, &pelican->path);
		}
		if (!royale_insertion_path_window(&pelican->path, &insertion.bounds, &pelican->enter_ticks,
			&pelican->leave_ticks))
		{
			pelican->enter_ticks = 0;
			pelican->leave_ticks = 0;
		}
		pelican->start_tick = index * ROYALE_INSERTION_STAGGER_TICKS;
		royale_insertion_path_point(&pelican->path, 0, &position, &velocity);
		object_placement_data_new(&placement, tag_index, NONE);
		placement.position = position;
		placement.forward = velocity;
		placement.forward.k = 0.0f;
		if (normalize3d(&placement.forward) == 0.0f)
			placement.forward = *global_forward3d;
		placement.up = *global_up3d;
		SET_FLAG(placement.flags, _new_object_never_automatically_delete_bit, TRUE);
		insertion.creating = TRUE;
		pelican->object_index = object_new(&placement);
		insertion.creating = FALSE;
		if (pelican->object_index == NONE)
		{
			error(_error_log, "royale insertion: Pelican %d not made at %.1f %.1f %.1f", index, position.x,
				position.y, position.z);
			break;
		}
		insertion.pelican_count = index + 1;
		SET_FLAG(object_get(pelican->object_index)->object.damage_flags, _object_cannot_take_damage_bit, TRUE);
		unit_set_enterable_by_player(pelican->object_index, FALSE);
		object_set_automatic_deactivation(pelican->object_index, FALSE);

		seat_count = insertion_pelican_seats(pelican->object_index, seats);
		error(_error_log, "royale insertion: Pelican %d (%d seats) from %.1f %.1f to %.1f %.1f at %.1f, window %ld..%ld",
			index, seat_count, pelican->path.start.x, pelican->path.start.y, pelican->path.end.x, pelican->path.end.y,
			pelican->path.start.z, pelican->enter_ticks, pelican->leave_ticks);
		if (seats_per_pelican == 0)
			seats_per_pelican = seat_count;
		/* (spread the players evenly over as many Pelicans as they need, an
		even count to each so a squad's two board together) */
		pelican_count = (short)((player_count + MAX(1, seats_per_pelican) - 1) / MAX(1, seats_per_pelican));
		pelican_count = (short)PIN(pelican_count, 1, ROYALE_INSERTION_MAXIMUM_PELICANS);
		players_per_pelican = (short)((player_count + pelican_count - 1) / pelican_count);
		players_per_pelican = (short)MIN((players_per_pelican + 1) & ~1, seat_count);
		for (seat = 0; seat < seat_count && boarded < players_per_pelican && next < player_count; seat++)
		{
			long player_index = players[next];
			long unit_index = insertion_player_unit(player_index);
			struct insertion_player *state = insertion_player_get(player_index);

			next++;
			if (!state || unit_index == NONE)
				continue;
			if (object_get(unit_index)->object.parent_object_index != NONE)
				unit_exit_seat_end(unit_index);
			object_set_position(unit_index, &position, NULL, NULL);
			if (!insertion_enter_seat(unit_index, pelican->object_index, seats[seat]))
			{
				/* (a seat it can't take: it drops over the play area) */
				real_point3d point;

				royale_insertion_redeploy_point(&insertion.bounds, insertion.height, &insertion.seed, &point);
				object_set_position(unit_index, &point, NULL, NULL);
				insertion_fall_begin(player_index, unit_index);
				continue;
			}
			state->pelican = index;
			state->seat_slot = boarded;
			state->falling = FALSE;
			state->jump_requested = FALSE;
			state->jump_was_down = TRUE;
			insertion_set_falling_immunity(unit_index, TRUE);
			royale_set_player_state(player_index, ROYALE_PLAYER_IN_PELICAN);
			boarded++;
		}
		insertion_pelican_place(pelican);
	}
	/* (more players than seats: the rest drop at once) */
	for (; next < player_count; next++)
	{
		long unit_index = insertion_player_unit(players[next]);
		real_point3d point;

		royale_insertion_redeploy_point(&insertion.bounds, insertion.height, &insertion.seed, &point);
		object_set_position(unit_index, &point, NULL, NULL);
		insertion_fall_begin(players[next], unit_index);
	}
}

/* the host's Pelicans and riders, each tick */
static void insertion_fly(void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	short index;

	for (index = 0; index < insertion.pelican_count; index++)
	{
		struct insertion_pelican *pelican = &insertion.pelicans[index];

		if (pelican->object_index != NONE)
			insertion_pelican_place(pelican);
	}
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		struct insertion_player *state = insertion_player_get(iterator.datum_index);
		struct insertion_pelican *pelican;
		struct unit_datum *unit;
		long ticks;
		boolean jump_down;

		if (!state || state->pelican == NONE || !VALID_INDEX(state->pelican, insertion.pelican_count))
			continue;
		pelican = &insertion.pelicans[state->pelican];
		if (player->unit_index == NONE || !(unit = unit_try_and_get(player->unit_index)))
		{
			state->pelican = NONE;
			continue;
		}
		ticks = insertion.insertion_ticks - pelican->start_tick;
		jump_down = TEST_FLAG(unit->unit.control_flags, _unit_control_jump_bit) != 0;
		if (unit->object.parent_object_index != pelican->object_index)
		{
			/* (out by the action button, or the seat lost) */
			insertion_jump(iterator.datum_index, pelican);
		}
		else if (ticks >= pelican->leave_ticks || pelican->done || pelican->object_index == NONE)
		{
			insertion_jump(iterator.datum_index, pelican);
		}
		else if (ticks >= pelican->enter_ticks && ((jump_down && !state->jump_was_down) || state->jump_requested))
		{
			insertion_jump(iterator.datum_index, pelican);
		}
		state->jump_was_down = jump_down;
	}
	/* Pelicans flown out, and empty, go */
	for (index = 0; index < insertion.pelican_count; index++)
	{
		struct insertion_pelican *pelican = &insertion.pelicans[index];

		if (pelican->object_index != NONE && pelican->done)
		{
			if (object_try_and_get_and_verify_type(pelican->object_index, _object_mask_unit))
				object_delete(pelican->object_index);
			pelican->object_index = NONE;
		}
	}
}

static void insertion_landing_impact(long player_index, long unit_index)
{
	struct biped_datum *biped = biped_try_and_get(unit_index);
	struct game_globals *globals = scenario_get_game_globals();

	if (!biped)
		return;
	/* the shake: the game's falling damage effect's, on this player's screen
	if it is ours, with no damage */
	if (globals && globals->falling_damage.count > 0)
	{
		struct insertion_falling_damage *falling = TAG_BLOCK_GET_ELEMENT(&globals->falling_damage, 0,
			struct insertion_falling_damage);

		if (falling->falling_damage.index != NONE && player_try_and_get(player_index) &&
			player_get(player_index)->local_player_index != NONE)
		{
			struct damage_data damage;
			struct damage_effect_definition *definition = damage_effect_definition_get(falling->falling_damage.index);
			struct screen_flash_definition flash = definition->screen_flash;

			/* (its shake, impulse and thud, not its red flash: a safe
			landing is no hurt one) */
			damage_data_new(&damage, falling->falling_damage.index);
			csmemset(&definition->screen_flash, 0, sizeof(definition->screen_flash));
			player_effect_start(player_index, &damage, global_down3d, LANDING_SHAKE_SCALE, 0.0f);
			definition->screen_flash = flash;
		}
	}
	/* the dust: the biped's jump-land effect in a ring about the feet */
	{
		struct biped_definition *definition = biped_definition_get(biped->definition_index);

		if (definition->biped.material_effects.index != NONE &&
			material_effect_visible(&biped->object.bounding_sphere_center))
		{
			short point;

			for (point = 0; point < LANDING_DUST_POINTS; point++)
			{
				real angle = (real)point * (2.0f * _pi / LANDING_DUST_POINTS);
				real_point3d where = biped->object.position;

				where.x += cosf(angle) * LANDING_DUST_RADIUS;
				where.y += sinf(angle) * LANDING_DUST_RADIUS;
				material_effect_new_from_point(definition->biped.material_effects.index,
					MATERIAL_EFFECT_BIPED_JUMP_LAND, &where, 1.0f);
			}
		}
	}
}

/* every machine's falling players: steering, the immunity, the landing */
static void insertion_fall(void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		struct insertion_player *state = insertion_player_get(iterator.datum_index);
		struct biped_datum *biped;
		boolean airborne;

		if (!state)
			continue;
		/* (a client: falling as the host says, but not again for a unit it
		saw land, before the host's ALIVE comes) */
		if (royale_player_state(iterator.datum_index) != ROYALE_PLAYER_FALLING)
			state->landed_unit = NONE;
		if (!state->falling && royale_player_state(iterator.datum_index) == ROYALE_PLAYER_FALLING &&
			player->unit_index != NONE && player->unit_index != state->landed_unit)
		{
			state->falling = TRUE;
			state->falling_ticks = 0;
			state->ground_ticks = 0;
			state->impact_shown = FALSE;
			state->unit_index = player->unit_index;
		}
		if (!state->falling)
			continue;
		if (player->unit_index != state->unit_index || !(biped = biped_try_and_get(state->unit_index)) ||
			TEST_FLAG(biped->object.damage_flags, _object_dead_bit))
		{
			/* (died, or a new unit: core's) */
			insertion_set_falling_immunity(state->unit_index, FALSE);
			state->falling = FALSE;
			continue;
		}
		if (biped->object.parent_object_index != NONE)
			continue;
		state->falling_ticks++;
		SET_FLAG(biped->unit.flags, _unit_no_falling_damage_bit, TRUE);
		airborne = TEST_FLAG(biped->biped.flags, _biped_airborne_bit) != 0;
		/* (the impact at the first touch of the ground; the fall ends once
		they stand: a brush of a ledge or a tree on the way down keeps them
		falling, and immune) */
		state->ground_ticks = airborne ? 0 : state->ground_ticks + 1;
		if (!airborne && !state->impact_shown && state->falling_ticks > ROYALE_INSERTION_LANDING_GRACE_TICKS)
		{
			state->impact_shown = TRUE;
			insertion_landing_impact(iterator.datum_index, state->unit_index);
		}
		if (royale_insertion_landed(state->falling_ticks, state->ground_ticks))
		{
			insertion_set_falling_immunity(state->unit_index, FALSE);
			state->falling = FALSE;
			state->landed_unit = state->unit_index;
			/* (a landing stands only the falling: one downed or out on the
			way down stays so) */
			if (insertion_authority() && royale_player_state(iterator.datum_index) == ROYALE_PLAYER_FALLING)
			{
				royale_set_player_state(iterator.datum_index, ROYALE_PLAYER_ALIVE);
				error(_error_log, "royale insertion: player %ld landed at %.1f %.1f %.1f after %ld ticks",
					DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), biped->object.position.x,
					biped->object.position.y, biped->object.position.z, state->falling_ticks);
			}
			continue;
		}
		if (airborne)
		{
			royale_insertion_air_control(&biped->object.translational_velocity, biped->unit.throttle.i,
				biped->unit.throttle.j, &biped->unit.desired_facing_vector);
		}
	}
}

/* ---------- public code */

void royale_insertion_start(struct royale_map_config const *config)
{
	short index;

	csmemset(&insertion, 0, sizeof(insertion));
	for (index = 0; index < (short)NUMBEROF(insertion.players); index++)
	{
		insertion.players[index].pelican = NONE;
		insertion.players[index].unit_index = NONE;
		insertion.players[index].landed_unit = NONE;
	}
	for (index = 0; index < ROYALE_INSERTION_MAXIMUM_PELICANS; index++)
		insertion.pelicans[index].object_index = NONE;
	insertion.config = config;
	insertion.started = TRUE;
	/* (the game's seeded random, drawn once: its own sequence after, so
	nothing else's draws move) */
	insertion.seed = *get_global_random_seed_address() ^ INSERTION_SEED_SALT;
	if (config)
	{
		insertion.bounds = config->bounds;
		insertion.height = config->drop_height;
	}
	/* (a map with no config: the play area is where the players stand when
	they board, insertion_bounds_from_players; the BSP's own box can be far
	larger than the level, out where nothing is drawn) */
	insertion.bounds_from_players = insertion.bounds.maximum.x <= insertion.bounds.minimum.x ||
		insertion.bounds.maximum.y <= insertion.bounds.minimum.y;
	if (!insertion.bounds_from_players)
	{
		if (!(insertion.height > insertion.bounds.minimum.z))
			insertion.height = insertion.bounds.minimum.z + ROYALE_INSERTION_DEFAULT_HEIGHT_ABOVE_PLAYERS;
		insertion_log_area("map config");
	}
}

static void insertion_log_area(char const *from)
{
	error(_error_log, "royale insertion: play area %.1f..%.1f x %.1f..%.1f, drop height %.1f (%s)",
		insertion.bounds.minimum.x, insertion.bounds.maximum.x, insertion.bounds.minimum.y,
		insertion.bounds.maximum.y, insertion.height, from);
}

/* the play area from the players' units (they stood at the level's spawns),
widened by the margin, and the height above the highest of them */
static void insertion_bounds_from_players(long const *players, short player_count)
{
	short index;
	boolean any = FALSE;

	for (index = 0; index < player_count; index++)
	{
		long unit_index = insertion_player_unit(players[index]);
		real_point3d const *p;

		if (unit_index == NONE)
			continue;
		p = &object_get(unit_index)->object.position;
		if (!any)
		{
			insertion.bounds.minimum = *p;
			insertion.bounds.maximum = *p;
			any = TRUE;
		}
		insertion.bounds.minimum.x = MIN(insertion.bounds.minimum.x, p->x);
		insertion.bounds.minimum.y = MIN(insertion.bounds.minimum.y, p->y);
		insertion.bounds.minimum.z = MIN(insertion.bounds.minimum.z, p->z);
		insertion.bounds.maximum.x = MAX(insertion.bounds.maximum.x, p->x);
		insertion.bounds.maximum.y = MAX(insertion.bounds.maximum.y, p->y);
		insertion.bounds.maximum.z = MAX(insertion.bounds.maximum.z, p->z);
	}
	if (!any)
		return;
	insertion.bounds.minimum.x -= ROYALE_INSERTION_PLAYERS_MARGIN;
	insertion.bounds.minimum.y -= ROYALE_INSERTION_PLAYERS_MARGIN;
	insertion.bounds.maximum.x += ROYALE_INSERTION_PLAYERS_MARGIN;
	insertion.bounds.maximum.y += ROYALE_INSERTION_PLAYERS_MARGIN;
	insertion.height = insertion.bounds.maximum.z + ROYALE_INSERTION_DEFAULT_HEIGHT_ABOVE_PLAYERS;
	insertion_log_area("the players' spawns");
}

void royale_insertion_tick(void)
{
	if (!insertion.started || !royale_active())
		return;
	if (royale_phase() == ROYALE_PHASE_INSERTION)
	{
		if (insertion_authority())
		{
			if (!insertion.boarded)
				insertion_board();
			insertion_fly();
		}
		insertion.insertion_ticks++;
	}
	else if (insertion.boarded && insertion.pelican_count > 0 && insertion_authority())
	{
		short index;

		/* (the phase over: anyone still aboard out, the Pelicans gone) */
		for (index = 0; index < insertion.pelican_count; index++)
			insertion.pelicans[index].done = TRUE;
		insertion_fly();
		insertion.pelican_count = 0;
	}
	insertion_fall();
}

void royale_insertion_draw(void)
{
}

boolean royale_insertion_pelican_position(long player_index, real_point3d *position, real_vector3d *velocity)
{
	struct insertion_player *state = insertion_player_get(player_index);
	struct object_datum *object;

	if (!state || state->pelican == NONE || !VALID_INDEX(state->pelican, insertion.pelican_count) ||
		insertion.pelicans[state->pelican].object_index == NONE)
	{
		return FALSE;
	}
	/* (a Pelican destroyed, or its handle stale: none) */
	object = object_try_and_get_and_verify_type(insertion.pelicans[state->pelican].object_index, _object_mask_unit);
	if (!object)
		return FALSE;
	*position = object->object.position;
	*velocity = object->object.translational_velocity;
	return TRUE;
}

boolean royale_insertion_running(void)
{
	return TRUE;
}

boolean royale_insertion_creating(void)
{
	return insertion.creating;
}

boolean royale_insertion_jump(long player_index)
{
	struct insertion_player *state = insertion_player_get(player_index);

	if (!state || state->pelican == NONE)
		return FALSE;
	state->jump_requested = TRUE;
	return TRUE;
}

void royale_insertion_redeploy(long player_index)
{
	struct insertion_player *state = insertion_player_get(player_index);
	long unit_index = insertion_player_unit(player_index);
	struct royale_bounds safe;
	real_point3d point;

	if (!state || unit_index == NONE || !insertion_authority())
		return;
	if (!insertion.started)
		royale_insertion_start(royale_map_config_get());
	royale_insertion_safe_area(&insertion.bounds, royale_beam_side(), royale_beam_target(), &safe);
	royale_insertion_redeploy_point(&safe, insertion.height, &insertion.seed, &point);
	if (!player_teleport(player_index, NONE, &point))
	{
		if (object_get(unit_index)->object.parent_object_index != NONE)
			unit_exit_seat_end(unit_index);
		object_set_position(unit_index, &point, NULL, NULL);
	}
	object_get(unit_index)->object.translational_velocity = *global_zero_vector3d;
	insertion_fall_begin(player_index, unit_index);
}

/* (royale.h) not started: no Pelicans, no one aboard or falling */
void royale_insertion_reset(void)
{
	short index;

	csmemset(&insertion, 0, sizeof(insertion));
	for (index = 0; index < (short)NUMBEROF(insertion.players); index++)
	{
		insertion.players[index].pelican = NONE;
		insertion.players[index].unit_index = NONE;
	}
	for (index = 0; index < ROYALE_INSERTION_MAXIMUM_PELICANS; index++)
		insertion.pelicans[index].object_index = NONE;
}
