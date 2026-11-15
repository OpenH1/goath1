/*
ROYALE_GULAG.C

Royale's gulag (royale_gulag.h): a player's first death for good, while the
phase has the gulag open (insertion and play; it closes for the endgame),
sends them to a 1v1 arena inside the map's own structure BSP (the engine
holds one BSP at a time, so the arena is a walled spot of the exterior, given
by the map config's gulag block). Gulag players fight in pairs in the order
they arrived, a pistol each and no grenades; the first kill wins, the winner
redeploys (royale_insertion_redeploy) and the loser is eliminated. Waiting
players watch the fight.

The line and the pairing (royale_gulag_queue_*) are pure simulation: driven
only by the deaths core reports and the tick, in the order core reports
them, they come out the same on every machine and every run. Only the host
acts on objects (spawns, weapons, the arena's walls), as gun_game.c does:
objects go to clients through the network.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"
#include "royale.h"
#include "royale_gulag.h"
#include "royale_insertion.h"

#include <stdlib.h>

/* ---- the line and the arena (pure: tools/harness/tests/test_royale_gulag_queue.py) */

void royale_gulag_queue_reset(
	struct royale_gulag_queue *queue)
{
	short index;

	for (index = 0; index < ROYALE_MAXIMUM_PLAYERS; index++)
	{
		queue->waiting[index] = NONE;
		queue->used[index] = FALSE;
	}
	queue->waiting_count = 0;
	queue->fighter[0] = NONE;
	queue->fighter[1] = NONE;
	queue->fight_ticks = 0;
	queue->alone_ticks = 0;
}

boolean royale_gulag_queue_enter(
	struct royale_gulag_queue *queue,
	long player_index,
	boolean open)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	if (!open || player_index == NONE || !VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) || queue->used[slot] ||
		queue->waiting_count >= ROYALE_MAXIMUM_PLAYERS)
	{
		return FALSE;
	}
	queue->used[slot] = TRUE;
	queue->waiting[queue->waiting_count++] = player_index;
	return TRUE;
}

/* the line's first, out of it */
static long royale_gulag_queue_pop(
	struct royale_gulag_queue *queue)
{
	long player_index = queue->waiting[0];
	short index;

	for (index = 1; index < queue->waiting_count; index++)
		queue->waiting[index - 1] = queue->waiting[index];
	queue->waiting[--queue->waiting_count] = NONE;
	return player_index;
}

void royale_gulag_queue_step(
	struct royale_gulag_queue *queue,
	long alone_limit_ticks,
	long fight_limit_ticks,
	struct royale_gulag_event *event)
{
	event->type = ROYALE_GULAG_EVENT_NONE;
	event->player[0] = NONE;
	event->player[1] = NONE;
	if (queue->fighter[0] != NONE)
	{
		queue->alone_ticks = 0;
		if (++queue->fight_ticks >= fight_limit_ticks && fight_limit_ticks > 0)
		{
			event->type = ROYALE_GULAG_EVENT_TIME_UP;
			event->player[0] = queue->fighter[0];
			event->player[1] = queue->fighter[1];
			queue->fighter[0] = NONE;
			queue->fighter[1] = NONE;
			queue->fight_ticks = 0;
		}
	}
	else if (queue->waiting_count >= 2)
	{
		/* (the two who came first, first on side 0) */
		queue->fighter[0] = royale_gulag_queue_pop(queue);
		queue->fighter[1] = royale_gulag_queue_pop(queue);
		queue->fight_ticks = 0;
		queue->alone_ticks = 0;
		event->type = ROYALE_GULAG_EVENT_FIGHT;
		event->player[0] = queue->fighter[0];
		event->player[1] = queue->fighter[1];
	}
	else if (queue->waiting_count == 1)
	{
		if (++queue->alone_ticks >= alone_limit_ticks)
		{
			event->type = ROYALE_GULAG_EVENT_WIN_BY_DEFAULT;
			event->player[0] = royale_gulag_queue_pop(queue);
			queue->alone_ticks = 0;
		}
	}
	else
	{
		queue->alone_ticks = 0;
	}
}

short royale_gulag_queue_side(
	struct royale_gulag_queue const *queue,
	long player_index)
{
	if (player_index == NONE)
		return NONE;
	if (queue->fighter[0] == player_index)
		return 0;
	if (queue->fighter[1] == player_index)
		return 1;
	return NONE;
}

long royale_gulag_queue_opponent(
	struct royale_gulag_queue const *queue,
	long player_index)
{
	short side = royale_gulag_queue_side(queue, player_index);

	return side == NONE ? NONE : queue->fighter[1 - side];
}

short royale_gulag_queue_place(
	struct royale_gulag_queue const *queue,
	long player_index)
{
	short index;

	for (index = 0; player_index != NONE && index < queue->waiting_count; index++)
	{
		if (queue->waiting[index] == player_index)
			return index;
	}
	return NONE;
}

long royale_gulag_queue_fighter_died(
	struct royale_gulag_queue *queue,
	long player_index)
{
	long winner = royale_gulag_queue_opponent(queue, player_index);

	if (winner == NONE)
		return NONE;
	queue->fighter[0] = NONE;
	queue->fighter[1] = NONE;
	queue->fight_ticks = 0;
	return winner;
}

long royale_gulag_queue_leave(
	struct royale_gulag_queue *queue,
	long player_index)
{
	short place = royale_gulag_queue_place(queue, player_index);
	short index;

	if (place != NONE)
	{
		for (index = place + 1; index < queue->waiting_count; index++)
			queue->waiting[index - 1] = queue->waiting[index];
		queue->waiting[--queue->waiting_count] = NONE;
		queue->alone_ticks = 0;
		return NONE;
	}
	return royale_gulag_queue_fighter_died(queue, player_index);
}

/* a player as a byte: their absolute index, 0xFF for none */
static byte royale_gulag_queue_slot_byte(
	long player_index)
{
	long slot = player_index == NONE ? NONE : DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	return VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) ? (byte)slot : 0xFF;
}

long royale_gulag_queue_write(
	struct royale_gulag_queue const *queue,
	long now,
	byte *buffer,
	long size)
{
	unsigned long start = (unsigned long)(queue->fighter[0] != NONE ? now - queue->fight_ticks : 0);
	short index;

	if (size < 3 + queue->waiting_count + 4)
		return 0;
	*buffer++ = (byte)queue->waiting_count;
	for (index = 0; index < queue->waiting_count; index++)
		*buffer++ = royale_gulag_queue_slot_byte(queue->waiting[index]);
	*buffer++ = royale_gulag_queue_slot_byte(queue->fighter[0]);
	*buffer++ = royale_gulag_queue_slot_byte(queue->fighter[1]);
	buffer[0] = (byte)(start & 0xFF);
	buffer[1] = (byte)((start >> 8) & 0xFF);
	buffer[2] = (byte)((start >> 16) & 0xFF);
	buffer[3] = (byte)((start >> 24) & 0xFF);
	return 3 + queue->waiting_count + 4;
}

long royale_gulag_queue_read(
	byte const *buffer,
	long size,
	long (*player_of_slot)(long slot),
	struct royale_gulag_queue *queue,
	long *fight_start)
{
	short count;
	short index;
	short side;
	unsigned long start;

	if (size < 1)
		return NONE;
	count = buffer[0];
	if (count > ROYALE_MAXIMUM_PLAYERS || size < 3 + count + 4)
		return NONE;
	royale_gulag_queue_reset(queue);
	for (index = 0; index < count; index++)
	{
		byte slot = buffer[1 + index];
		long player_index;

		if (slot >= ROYALE_MAXIMUM_PLAYERS)
			return NONE;
		player_index = player_of_slot(slot);
		if (player_index != NONE)
			queue->waiting[queue->waiting_count++] = player_index;
	}
	for (side = 0; side < 2; side++)
	{
		byte slot = buffer[1 + count + side];

		if (slot != 0xFF && slot >= ROYALE_MAXIMUM_PLAYERS)
			return NONE;
		queue->fighter[side] = slot == 0xFF ? NONE : player_of_slot(slot);
	}
	/* (a fight is two, or none) */
	if ((queue->fighter[0] == NONE) != (queue->fighter[1] == NONE))
		queue->fighter[0] = queue->fighter[1] = NONE;
	start = (unsigned long)buffer[3 + count] | ((unsigned long)buffer[4 + count] << 8) |
		((unsigned long)buffer[5 + count] << 16) | ((unsigned long)buffer[6 + count] << 24);
	*fight_start = (long)(start & 0x7FFFFFFFUL);
	return 3 + count + 4;
}

/* ---- the game's side */

/* a fighter this far past the arena's bounds goes back to their spawn */
#define ROYALE_GULAG_BOUNDS_SLACK 0.25f

static struct
{
	struct royale_gulag_queue queue;
	struct royale_gulag_arena arena;
	boolean have_arena;
	/* (by absolute index, the host's) a default win's player: spawned in the
	arena to have a unit, then redeployed (at once, in the host's
	postspawn: a client never sees that unit in the arena) */
	boolean redeploy_on_spawn[ROYALE_MAXIMUM_PLAYERS];
	/* a client's: the fight's start (match time), and the host's line read
	but not yet taken (royale_gulag_take_network_state) */
	long fight_start;
	struct royale_gulag_queue received;
	long received_fight_start;
	boolean have_received;
	/* (by absolute index) each fighter's unit turned the arena's way */
	long faced_unit[ROYALE_MAXIMUM_PLAYERS];
	/* each side's unit given the kit (the host's) */
	long armed_unit[2];
	/* (debug) the match second the host kills its first local player, and
	again 3 s later (HALO_ROYALE_GULAG_KILL_LOCAL: a test's way into the
	gulag), 0 for never */
	long debug_kill_ticks;
} royale_gulag;

static void royale_gulag_arm_fighters(void);
static void royale_gulag_face_fighters(void);

static boolean royale_gulag_host(
	void)
{
	return !network_game_distributed_client();
}

/* the gulag takes new players in insertion and play; it closes for the
endgame (the line still plays out) */
static boolean royale_gulag_open(
	void)
{
	enum royale_phase phase = royale_phase();

	return royale_gulag.have_arena && (phase == ROYALE_PHASE_INSERTION || phase == ROYALE_PHASE_PLAY);
}

static long royale_gulag_living_unit(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct unit_datum *unit = player && player->unit_index != NONE ? unit_try_and_get(player->unit_index) : NULL;

	return unit && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ? player->unit_index : NONE;
}

static boolean royale_gulag_inside(
	real_point3d const *point,
	real slack)
{
	struct royale_bounds const *bounds = &royale_gulag.arena.bounds;

	return point->x >= bounds->minimum.x - slack && point->x <= bounds->maximum.x + slack &&
		point->y >= bounds->minimum.y - slack && point->y <= bounds->maximum.y + slack &&
		point->z >= bounds->minimum.z - slack && point->z <= bounds->maximum.z + slack;
}

void royale_gulag_start(
	struct royale_map_config const *config)
{
	struct royale_bounds const *bounds = config ? &config->gulag.bounds : NULL;

	royale_gulag_queue_reset(&royale_gulag.queue);
	csmemset(royale_gulag.redeploy_on_spawn, 0, sizeof(royale_gulag.redeploy_on_spawn));
	csmemset(royale_gulag.faced_unit, 0xFF, sizeof(royale_gulag.faced_unit));
	royale_gulag.fight_start = 0;
	royale_gulag.have_received = FALSE;
	royale_gulag.armed_unit[0] = NONE;
	royale_gulag.armed_unit[1] = NONE;
	{
		char const *value = royale_test_setting("HALO_ROYALE_GULAG_KILL_LOCAL");

		royale_gulag.debug_kill_ticks = value ? royale_seconds_to_ticks((real)atol(value)) : 0;
	}
	csmemset(&royale_gulag.arena, 0, sizeof(royale_gulag.arena));
	/* (no arena in the config, or an empty box: no gulag, deaths eliminate) */
	royale_gulag.have_arena = bounds && bounds->maximum.x > bounds->minimum.x &&
		bounds->maximum.y > bounds->minimum.y && bounds->maximum.z > bounds->minimum.z;
	if (royale_gulag.have_arena)
		royale_gulag.arena = config->gulag;
}

/* a winner goes back to the island */
static void royale_gulag_win(
	long player_index)
{
	royale_set_player_state(player_index, ROYALE_PLAYER_ALIVE);
	if (royale_gulag_host())
		royale_insertion_redeploy(player_index);
}

static void royale_gulag_spawn_fighter(
	long player_index)
{
	if (royale_gulag_host())
		royale_request_spawn(player_index);
}

/* time up: the healthier fighter wins (the first to arrive on a tie), the
other dies (core then eliminates them: no longer a fighter) */
static void royale_gulag_time_up(
	long first,
	long second)
{
	long units[2] = { royale_gulag_living_unit(first), royale_gulag_living_unit(second) };
	real health[2] = { -1.0f, -1.0f };
	long winner;
	long loser;
	short side;

	for (side = 0; side < 2; side++)
	{
		if (units[side] != NONE)
		{
			struct unit_datum *unit = unit_get(units[side]);

			health[side] = unit->object.body_vitality + unit->object.shield_vitality;
		}
	}
	winner = health[1] > health[0] ? second : first;
	loser = winner == first ? second : first;
	if (royale_gulag_host() && royale_gulag_living_unit(loser) != NONE)
		unit_kill(royale_gulag_living_unit(loser));
	else
		royale_set_player_state(loser, ROYALE_PLAYER_ELIMINATED);
	royale_gulag_win(winner);
}

/* the host keeps the fighters in: one past the bounds goes back to their spawn */
static void royale_gulag_walls(
	void)
{
	short side;

	for (side = 0; side < 2; side++)
	{
		long player_index = royale_gulag.queue.fighter[side];
		long unit_index = royale_gulag_living_unit(player_index);
		real_point3d origin;

		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		if (!royale_gulag_inside(&origin, ROYALE_GULAG_BOUNDS_SLACK))
			player_teleport(player_index, NONE, &royale_gulag.arena.spawn[side]);
	}
}

void royale_gulag_tick(
	void)
{
	struct royale_gulag_event event;
	short side;

	if (!royale_active())
		return;
	/* (a client's line is the host's, the fight's clock its own match
	time's; a fighter's view turns the arena's way when their unit comes) */
	royale_gulag_face_fighters();
	if (!royale_gulag_host())
	{
		if (royale_gulag.queue.fighter[0] != NONE)
			royale_gulag.queue.fight_ticks = MAX(royale_match_time() - royale_gulag.fight_start, 0);
		return;
	}
	/* (players who quit leave the line; a fighter's opponent wins) */
	for (side = 0; side < 2; side++)
	{
		long player_index = royale_gulag.queue.fighter[side];
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

		if (player_index != NONE && (!player || player->quit_out_of_game))
		{
			long winner = royale_gulag_queue_leave(&royale_gulag.queue, player_index);

			if (winner != NONE)
				royale_gulag_win(winner);
		}
	}
	for (side = royale_gulag.queue.waiting_count - 1; side >= 0; side--)
	{
		long player_index = royale_gulag.queue.waiting[side];
		struct player_datum *player = player_try_and_get(player_index);

		if (!player || player->quit_out_of_game)
			royale_gulag_queue_leave(&royale_gulag.queue, player_index);
	}

	royale_gulag_queue_step(&royale_gulag.queue, royale_seconds_to_ticks(ROYALE_GULAG_ALONE_SECONDS),
		royale_seconds_to_ticks(ROYALE_GULAG_FIGHT_SECONDS), &event);
	if (event.type != ROYALE_GULAG_EVENT_NONE)
	{
		error(_error_silent, "royale gulag: %s %ld %ld (%d waiting)", event.type == ROYALE_GULAG_EVENT_FIGHT ? "fight" :
			event.type == ROYALE_GULAG_EVENT_WIN_BY_DEFAULT ? "win by default" : "time up",
			DATUM_INDEX_TO_ABSOLUTE_INDEX(event.player[0]),
			event.player[1] == NONE ? -1L : DATUM_INDEX_TO_ABSOLUTE_INDEX(event.player[1]),
			royale_gulag.queue.waiting_count);
	}
	switch (event.type)
	{
	case ROYALE_GULAG_EVENT_FIGHT:
		royale_gulag_spawn_fighter(event.player[0]);
		royale_gulag_spawn_fighter(event.player[1]);
		break;
	case ROYALE_GULAG_EVENT_WIN_BY_DEFAULT:
	{
		long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(event.player[0]);

		/* (no unit yet: spawned in the arena, then redeployed from there) */
		if (VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS))
			royale_gulag.redeploy_on_spawn[slot] = TRUE;
		royale_gulag_spawn_fighter(event.player[0]);
		break;
	}
	case ROYALE_GULAG_EVENT_TIME_UP:
		royale_gulag_time_up(event.player[0], event.player[1]);
		break;
	}
	/* (debug: the first local player killed twice, downed then out) */
	if (royale_gulag.debug_kill_ticks > 0 && royale_gulag_host() &&
		(royale_match_time() == royale_gulag.debug_kill_ticks ||
			royale_match_time() == royale_gulag.debug_kill_ticks + royale_seconds_to_ticks(3.0f)))
	{
		long unit_index = royale_gulag_living_unit(local_player_get_player_index(0));

		if (unit_index != NONE)
			unit_kill(unit_index);
	}
	if (royale_gulag_host())
	{
		royale_gulag_arm_fighters();
		royale_gulag_walls();
	}
}

boolean royale_gulag_player_died(
	long player_index,
	long killer_player_index)
{
	long winner;

	(void)killer_player_index;
	if (!royale_active())
		return FALSE;
	/* a fighter killed: the opponent wins, the fighter is eliminated */
	winner = royale_gulag_queue_fighter_died(&royale_gulag.queue, player_index);
	if (winner != NONE)
	{
		error(_error_silent, "royale gulag: %ld won, %ld out", DATUM_INDEX_TO_ABSOLUTE_INDEX(winner),
			DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index));
		royale_gulag_win(winner);
		return FALSE;
	}
	return royale_gulag_queue_enter(&royale_gulag.queue, player_index, royale_gulag_open());
}

boolean royale_gulag_spawn_point(
	long player_index,
	real_point3d *position,
	real_vector3d *facing)
{
	short side = royale_gulag_queue_side(&royale_gulag.queue, player_index);
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);

	if (!royale_active() || !royale_gulag.have_arena)
		return FALSE;
	if (side == NONE && VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) && royale_gulag.redeploy_on_spawn[slot])
		side = 0;
	if (side == NONE)
		return FALSE;
	*position = royale_gulag.arena.spawn[side];
	facing->i = cosf(royale_gulag.arena.spawn_facing[side]);
	facing->j = sinf(royale_gulag.arena.spawn_facing[side]);
	facing->k = 0.0f;
	return TRUE;
}

/* every weapon of a unit gone, the one in its hand too (which
unit_delete_all_weapons keeps), as gun_game.c's */
static void royale_gulag_delete_weapons(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	short slot;
	short other;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index == NONE)
			continue;
		for (other = slot; other < MAXIMUM_WEAPONS_PER_UNIT; other++)
		{
			if (unit->unit.weapon_object_indices[other] == weapon_index)
			{
				unit->unit.weapon_object_indices[other] = NONE;
				if (unit->unit.current_weapon_index == other)
					unit->unit.current_weapon_index = NONE;
				if (unit->unit.desired_weapon_index == other)
					unit->unit.desired_weapon_index = NONE;
			}
		}
		if (object_try_and_get(weapon_index))
			object_delete(weapon_index);
	}
}

/* the gulag's loadout: a pistol and nothing else */
static void royale_gulag_equip(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	long definition_index = game_engine_loadout_weapon_definition(_loadout_weapon_pistol);
	struct object_placement_data placement;
	struct weapon_datum *weapon;
	struct weapon_definition *definition;
	long weapon_index;
	short index;

	royale_gulag_delete_weapons(unit_index);
	for (index = 0; index < NUMBER_OF_UNIT_GRENADE_TYPES; index++)
		unit->unit.grenade_counts[index] = 0;
	/* (a campaign level played as a map has no multiplayer loadout list:
	the pistol's tag, as the loot lane finds it) */
	if (definition_index == NONE)
		definition_index = tag_loaded('weap', "weapons\\pistol\\pistol");
	if (definition_index == NONE)
	{
		error(_error_silent, "royale gulag: no pistol on this map");
		return;
	}
	object_placement_data_new(&placement, definition_index, NONE);
	weapon_index = object_new(&placement);
	if (weapon_index == NONE)
		return;
	/* (a full magazine and a full reserve) */
	weapon = weapon_try_and_get(weapon_index);
	definition = weapon ? weapon_definition_get(weapon->definition_index) : NULL;
	for (index = 0; definition && index < MIN(definition->weapon.magazines.count, NUMBEROF(weapon->weapon.magazines));
		index++)
	{
		struct weapon_magazine_definition const *magazine =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, index, struct weapon_magazine_definition);

		weapon->weapon.magazines[index].rounds_loaded = magazine->rounds_loaded_maximum;
		weapon->weapon.magazines[index].rounds_total = magazine->rounds_total_maximum;
	}
	if (!unit_add_weapon_to_inventory(unit_index, weapon_index, 2))
	{
		error(_error_silent, "royale gulag: the pistol not taken");
		object_delete(weapon_index);
	}
}

/* a unit holds a weapon */
static boolean royale_gulag_armed(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	short slot;

	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long weapon_index = unit->unit.weapon_object_indices[slot];

		if (weapon_index != NONE && object_try_and_get(weapon_index))
			return TRUE;
	}
	return FALSE;
}

/* the host keeps each fighter's unit armed: the kit once, again if it has
no weapon (none taken, or another lane's kit given over it) */
static void royale_gulag_arm_fighters(
	void)
{
	short side;

	for (side = 0; side < 2; side++)
	{
		long unit_index = royale_gulag_living_unit(royale_gulag.queue.fighter[side]);

		if (unit_index == NONE)
		{
			royale_gulag.armed_unit[side] = NONE;
			continue;
		}
		if (royale_gulag.armed_unit[side] == unit_index && royale_gulag_armed(unit_index))
			continue;
		royale_gulag_equip(unit_index);
		royale_gulag.armed_unit[side] = unit_index;
	}
}

void royale_gulag_postspawn(
	long player_index)
{
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	boolean redeploy = VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) && royale_gulag.redeploy_on_spawn[slot];
	struct player_datum *player;
	real_point3d position;
	real_vector3d facing;
	long unit_index;

	if (!royale_active() || (!redeploy && royale_gulag_queue_side(&royale_gulag.queue, player_index) == NONE))
		return;
	unit_index = royale_gulag_living_unit(player_index);
	if (unit_index != NONE && royale_gulag_host())
	{
		short side = royale_gulag_queue_side(&royale_gulag.queue, player_index);

		royale_gulag_equip(unit_index);
		if (side != NONE)
			royale_gulag.armed_unit[side] = unit_index;
	}
	/* (a local player's view faces the arena's way, as a spawn's does;
	object_set_position turns only the body) */
	player = player_try_and_get(player_index);
	if (unit_index != NONE && player && player->local_player_index != NONE &&
		royale_gulag_spawn_point(player_index, &position, &facing))
	{
		player_control_set_facing(player->local_player_index, &facing);
		if (VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS))
			royale_gulag.faced_unit[slot] = unit_index;
	}
	if (redeploy)
	{
		royale_gulag.redeploy_on_spawn[slot] = FALSE;
		royale_gulag_win(player_index);
	}
}

/* every machine: a local fighter's view turned the arena's way once a unit
(the host's spawn does it in royale_gulag_postspawn; a client's unit comes
from the network, whenever the line has said who fights) */
static void royale_gulag_face_fighters(
	void)
{
	short side;

	for (side = 0; side < 2; side++)
	{
		long player_index = royale_gulag.queue.fighter[side];
		long slot = player_index != NONE ? DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
		struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		long unit_index = royale_gulag_living_unit(player_index);
		real_vector3d facing;

		if (!player || player->local_player_index == NONE || unit_index == NONE ||
			!VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS) || royale_gulag.faced_unit[slot] == unit_index)
		{
			continue;
		}
		royale_gulag.faced_unit[slot] = unit_index;
		facing.i = cosf(royale_gulag.arena.spawn_facing[side]);
		facing.j = sinf(royale_gulag.arena.spawn_facing[side]);
		facing.k = 0.0f;
		player_control_set_facing(player->local_player_index, &facing);
	}
}

/* a player by their absolute index, NONE if none */
static long royale_gulag_player_of_slot(
	long slot)
{
	struct data_iterator iterator;

	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
	{
		if (DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index) == slot)
			return iterator.datum_index;
	}
	return NONE;
}

long royale_gulag_write_network_state(
	byte *buffer,
	long size)
{
	return royale_gulag_queue_write(&royale_gulag.queue, royale_match_time(), buffer, size);
}

long royale_gulag_read_network_state(
	byte const *buffer,
	long size)
{
	long read = royale_gulag_queue_read(buffer, size, royale_gulag_player_of_slot, &royale_gulag.received,
		&royale_gulag.received_fight_start);

	royale_gulag.have_received = read != NONE;
	return read;
}

void royale_gulag_take_network_state(
	void)
{
	if (!royale_gulag.have_received || royale_gulag_host())
		return;
	royale_gulag.have_received = FALSE;
	if (royale_gulag.queue.fighter[0] != royale_gulag.received.fighter[0] ||
		royale_gulag.queue.fighter[1] != royale_gulag.received.fighter[1] ||
		royale_gulag.queue.waiting_count != royale_gulag.received.waiting_count)
	{
		error(_error_silent, "royale gulag: the host's line: %ld vs %ld, %d waiting",
			royale_gulag.received.fighter[0] == NONE ? -1L : DATUM_INDEX_TO_ABSOLUTE_INDEX(royale_gulag.received.fighter[0]),
			royale_gulag.received.fighter[1] == NONE ? -1L : DATUM_INDEX_TO_ABSOLUTE_INDEX(royale_gulag.received.fighter[1]),
			royale_gulag.received.waiting_count);
	}
	royale_gulag.queue = royale_gulag.received;
	royale_gulag.fight_start = royale_gulag.received_fight_start;
	if (royale_gulag.queue.fighter[0] != NONE)
		royale_gulag.queue.fight_ticks = MAX(royale_match_time() - royale_gulag.fight_start, 0);
}

long royale_gulag_spectate_unit(
	long player_index)
{
	short side;

	if (!royale_active() || royale_gulag_queue_place(&royale_gulag.queue, player_index) == NONE)
		return NONE;
	for (side = 0; side < 2; side++)
	{
		long unit_index = royale_gulag_living_unit(royale_gulag.queue.fighter[side]);

		if (unit_index != NONE)
			return unit_index;
	}
	return NONE;
}

long royale_gulag_opponent(
	long player_index)
{
	return royale_active() ? royale_gulag_queue_opponent(&royale_gulag.queue, player_index) : NONE;
}

/* a player's name, as wide text */
static void royale_gulag_name(
	long player_index,
	wchar_t *name,
	short count)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	short index;

	for (index = 0; player && index < (short)NUMBEROF(player->name) && index < count - 1 && player->name[index]; index++)
		name[index] = player->name[index];
	name[index] = 0;
}

static void royale_gulag_text(
	short row,
	wchar_t const *text,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds = render.camera.window_bounds;

	if (font_index == NONE)
		return;
	offset_rectangle2d(&bounds, (short)-render.camera.viewport_bounds.x0, (short)-render.camera.viewport_bounds.y0);
	royale_hud_row(row, &bounds.y0, &bounds.y1);
	/* draw mode 2: centered */
	draw_string_set_draw_mode(font_index, NONE, 2, 0, color);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
}

/* a local gulag player's lines, in the HUD's top rows (all a gulag player
sees of Royale's HUD): the fight's clock and the opponent, or their place
in line and the fight they watch */
void royale_gulag_draw(
	void)
{
	long player_index;
	short place;
	short side;
	wchar_t text[96];
	wchar_t name[16];
	real_argb_color gold = { 1.0f, 1.0f, 0.85f, 0.4f };
	real_argb_color white = { 1.0f, 1.0f, 1.0f, 1.0f };

	if (!royale_active() || render.local_player_index == NONE)
		return;
	player_index = local_player_get_player_index(render.local_player_index);
	side = royale_gulag_queue_side(&royale_gulag.queue, player_index);
	place = royale_gulag_queue_place(&royale_gulag.queue, player_index);
	if (side != NONE)
	{
		long seconds = (royale_seconds_to_ticks(ROYALE_GULAG_FIGHT_SECONDS) - royale_gulag.queue.fight_ticks) /
			MAX(royale_seconds_to_ticks(1.0f), 1);

		usnprintf(text, NUMBEROF(text) - 1, L"RECLAIMER: WIN TO REDEPLOY   %ld", MAX(seconds, 0L));
		text[NUMBEROF(text) - 1] = 0;
		royale_gulag_text(0, text, &gold);
		royale_gulag_name(royale_gulag.queue.fighter[1 - side], name, NUMBEROF(name));
		usnprintf(text, NUMBEROF(text) - 1, L"VS %s", name);
		text[NUMBEROF(text) - 1] = 0;
		royale_gulag_text(1, text, &white);
	}
	else if (place != NONE)
	{
		if (place == 0 && royale_gulag.queue.fighter[0] == NONE)
			usnprintf(text, NUMBEROF(text) - 1, L"RECLAIMER: WAITING FOR AN OPPONENT");
		else
			usnprintf(text, NUMBEROF(text) - 1, L"RECLAIMER: YOU FIGHT %s", place < 2 ? L"NEXT" : L"SOON");
		text[NUMBEROF(text) - 1] = 0;
		royale_gulag_text(0, text, &gold);
		if (royale_gulag.queue.fighter[0] != NONE)
		{
			wchar_t other[16];

			royale_gulag_name(royale_gulag.queue.fighter[0], name, NUMBEROF(name));
			royale_gulag_name(royale_gulag.queue.fighter[1], other, NUMBEROF(other));
			usnprintf(text, NUMBEROF(text) - 1, L"WATCHING %s VS %s", name, other);
			text[NUMBEROF(text) - 1] = 0;
			royale_gulag_text(1, text, &white);
		}
	}
}

/* (royale.h) no arena, an empty line, no fighters */
void royale_gulag_reset(
	void)
{
	csmemset(&royale_gulag, 0, sizeof(royale_gulag));
	royale_gulag_queue_reset(&royale_gulag.queue);
	royale_gulag.armed_unit[0] = NONE;
	royale_gulag.armed_unit[1] = NONE;
}
