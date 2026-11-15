/*
ROYALE_LOOT.C

Royale's loot (royale.h, royale_loot.h):

- Rarity: common (the assault rifle, pistol, shotgun, plasma pistol and
  rifle, needler, grenades), rare (the sniper rifle, rocket launcher and fuel
  rod gun) and power-ups (active camouflage, overshield, health packs). Only
  the kinds whose tags the map has drop (b30 has no sniper rifle, shotgun or
  fuel rod gun: its rare loot is the rocket launcher).
- The map's own weapons and equipment are deleted before the loot is
  placed; weapons dropped later (by the dead) stay.
- Ground loot: one item (or nothing) at each of the config's loot spots,
  its rarity drawn by the spot's tier (royale_loot_tier_weights), its kind
  by the rarity's weights.
- Covenant supply pods: ROYALE_LOOT_POD_COUNT fall during PLAY onto pod
  spots, each a Covenant storage crate (b30's scenery\c_storage\c_storage:
  no map has a drop pod tag) falling from the sky with a smoke trail and
  landing with an explosion, then ROYALE_LOOT_POD_ITEMS items (rare loot and
  power-ups) round it.
- Scarce ammunition: every weapon placed holds a part of a magazine and a
  small reserve (royale_loot_kinds' rounds); a battery weapon half its
  charge.
- The start kit: a pistol with royale_loot_start_rounds, no grenades.

The plan (each spot's kind, each pod's spot, time and contents) is drawn
once at start from the match's seed with the game's seeded random
(seed_random_range on a seed of its own, which takes nothing from the
game's), so every machine draws the same. Weapons and equipment are the
host's objects (network_objects.c): only the host creates and deletes them.
A pod's crate and effects are every machine's own, at the same ticks.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "effects/effects.h"
#include "game/game.h"
#include "game/players.h"
#include "items/items.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "items/equipment_definitions.h"
#include "math/real_math.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"
#include "royale.h"
#include "royale_loot.h"

#include <math.h>

/* ---------- the loot table */

struct royale_loot_kind_definition
{
	byte rarity; /* enum royale_loot_rarity */
	byte weight; /* within its rarity */
	/* a weapon's rounds placed: loaded and in reserve (a battery weapon:
	none, half its charge) */
	short rounds_loaded;
	short rounds_total;
	long group_tag;
	char const *names[2]; /* the tag, by the names maps give it */
};

static struct royale_loot_kind_definition const royale_loot_kinds[NUMBER_OF_ROYALE_LOOT_KINDS] =
{
	{ ROYALE_LOOT_RARITY_COMMON, 10, 32, 32, 'weap', { "weapons\\assault rifle\\assault rifle", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 8, 8, 8, 'weap', { "weapons\\pistol\\pistol", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 6, 4, 4, 'weap', { "weapons\\shotgun\\shotgun", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 8, 0, 0, 'weap', { "weapons\\plasma pistol\\plasma pistol", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 8, 0, 0, 'weap', { "weapons\\plasma rifle\\plasma rifle", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 6, 20, 0, 'weap', { "weapons\\needler\\needler", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 5, 0, 0, 'eqip', { "weapons\\frag grenade\\frag grenade", NULL } },
	{ ROYALE_LOOT_RARITY_COMMON, 4, 0, 0, 'eqip', { "weapons\\plasma grenade\\plasma grenade", NULL } },
	{ ROYALE_LOOT_RARITY_RARE, 4, 4, 0, 'weap', { "weapons\\sniper rifle\\sniper rifle", NULL } },
	{ ROYALE_LOOT_RARITY_RARE, 3, 2, 0, 'weap', { "weapons\\rocket launcher\\rocket launcher", NULL } },
	{ ROYALE_LOOT_RARITY_RARE, 3, 0, 0, 'weap', { "weapons\\fuel rod gun\\fuel rod gun", NULL } },
	/* (the stock maps' power-ups, b30's too, are named by their folders) */
	{ ROYALE_LOOT_RARITY_POWERUP, 3, 0, 0, 'eqip',
		{ "powerups\\active camouflage", "powerups\\active camouflage\\active camouflage" } },
	{ ROYALE_LOOT_RARITY_POWERUP, 3, 0, 0, 'eqip', { "powerups\\over shield", "powerups\\over shield\\over shield" } },
	{ ROYALE_LOOT_RARITY_POWERUP, 6, 0, 0, 'eqip', { "powerups\\health pack", "powerups\\healthpack\\healthpack" } },
};

/* a spot's tier (enum royale_loot_tier): the weights of nothing, common,
rare and power-up loot there */
static byte const royale_loot_tier_weights[NUMBER_OF_ROYALE_LOOT_TIERS][1 + NUMBER_OF_ROYALE_LOOT_RARITIES] =
{
	{ 15, 70, 5, 10 }, /* common */
	{ 0, 40, 35, 25 }, /* rare */
	{ 0, 10, 50, 40 }, /* epic */
	{ 0, 0, 70, 30 }, /* legendary: a supply pod's */
};

/* the start kit's pistol */
static short const royale_loot_start_rounds[2] = { 8, 8 };

#define ROYALE_LOOT_POD_FIRST_SECOND 30
#define ROYALE_LOOT_POD_INTERVAL_SECONDS 45
#define ROYALE_LOOT_POD_JITTER_SECONDS 20
#define ROYALE_LOOT_POD_ITEM_RADIUS 1.6f
#define ROYALE_LOOT_TRAIL_SECONDS 0.1f
#define ROYALE_LOOT_MAXIMUM_PLACED (ROYALE_MAXIMUM_LOOT_SPOTS + ROYALE_LOOT_POD_COUNT * ROYALE_LOOT_POD_ITEMS)

/* ---------- the plan (pure: royale_loot_plan_make) */

/* a spot's rarity, NONE for nothing */
static short royale_loot_roll_rarity(
	unsigned long *seed,
	short tier)
{
	byte const *weights = royale_loot_tier_weights[PIN(tier, 0, NUMBER_OF_ROYALE_LOOT_TIERS - 1)];
	short total = 0;
	short roll;
	short index;

	for (index = 0; index <= NUMBER_OF_ROYALE_LOOT_RARITIES; index++)
		total += weights[index];
	roll = seed_random_range(seed, 0, total);
	for (index = 0; index <= NUMBER_OF_ROYALE_LOOT_RARITIES; index++)
	{
		roll -= weights[index];
		if (roll < 0)
			return index - 1;
	}
	return NONE;
}

/* a kind of the rarity the map has (of the commons when it has none of
it), ROYALE_LOOT_KIND_NONE if none */
static byte royale_loot_roll_kind(
	unsigned long *seed,
	short rarity,
	unsigned long available)
{
	short total = 0;
	short roll;
	short kind;

	for (kind = 0; kind < NUMBER_OF_ROYALE_LOOT_KINDS; kind++)
	{
		if (royale_loot_kinds[kind].rarity == rarity && TEST_FLAG(available, kind))
			total += royale_loot_kinds[kind].weight;
	}
	if (!total)
		return rarity == ROYALE_LOOT_RARITY_COMMON ? ROYALE_LOOT_KIND_NONE :
			royale_loot_roll_kind(seed, ROYALE_LOOT_RARITY_COMMON, available);
	roll = seed_random_range(seed, 0, total);
	for (kind = 0; kind < NUMBER_OF_ROYALE_LOOT_KINDS; kind++)
	{
		if (royale_loot_kinds[kind].rarity != rarity || !TEST_FLAG(available, kind))
			continue;
		roll -= royale_loot_kinds[kind].weight;
		if (roll < 0)
			return (byte)kind;
	}
	return ROYALE_LOOT_KIND_NONE;
}

static byte royale_loot_roll(
	unsigned long *seed,
	short tier,
	unsigned long available)
{
	short rarity = royale_loot_roll_rarity(seed, tier);

	/* (the kind is drawn even for nothing, so each spot takes as much of
	the seed: a spot's loot does not shift every later one's) */
	byte kind = royale_loot_roll_kind(seed, rarity == NONE ? ROYALE_LOOT_RARITY_COMMON : rarity, available);

	return rarity == NONE ? ROYALE_LOOT_KIND_NONE : kind;
}

void royale_loot_plan_make(
	unsigned long seed,
	short const *spot_tiers,
	short spot_count,
	short pod_spot_count,
	unsigned long available,
	struct royale_loot_plan *plan)
{
	/* (the loot's own seed, from the match's: it takes nothing of the
	game's) */
	unsigned long loot_seed = seed ^ 0x6C6F6F74;
	short taken[ROYALE_MAXIMUM_POD_SPOTS];
	short index;
	short item;

	csmemset(plan, 0, sizeof(*plan));
	plan->spot_count = (short)PIN(spot_count, 0, ROYALE_MAXIMUM_LOOT_SPOTS);
	for (index = 0; index < plan->spot_count; index++)
		plan->spot_kinds[index] = royale_loot_roll(&loot_seed, spot_tiers[index], available);
	pod_spot_count = (short)PIN(pod_spot_count, 0, ROYALE_MAXIMUM_POD_SPOTS);
	plan->pod_count = (short)MIN(ROYALE_LOOT_POD_COUNT, pod_spot_count);
	for (index = 0; index < pod_spot_count; index++)
		taken[index] = index;
	for (index = 0; index < plan->pod_count; index++)
	{
		struct royale_loot_pod_plan *pod = &plan->pods[index];
		/* (a spot of those not taken) */
		short pick = seed_random_range(&loot_seed, index, pod_spot_count);
		short spot = taken[pick];

		taken[pick] = taken[index];
		taken[index] = spot;
		pod->spot = spot;
		pod->drop_second = (short)(ROYALE_LOOT_POD_FIRST_SECOND + index * ROYALE_LOOT_POD_INTERVAL_SECONDS +
			seed_random_range(&loot_seed, 0, ROYALE_LOOT_POD_JITTER_SECONDS));
		for (item = 0; item < ROYALE_LOOT_POD_ITEMS; item++)
		{
			/* (a pod holds something: rare loot and power-ups) */
			byte kind = royale_loot_roll(&loot_seed, ROYALE_LOOT_LEGENDARY, available);

			pod->kinds[item] = kind != ROYALE_LOOT_KIND_NONE ? kind :
				royale_loot_roll_kind(&loot_seed, ROYALE_LOOT_RARITY_COMMON, available);
		}
	}
}

real royale_loot_pod_height(
	real fraction)
{
	real f = PIN(fraction, 0.0f, 1.0f);

	/* (fast from the sky, braking hard near the ground, still moving as it
	strikes) */
	return ROYALE_LOOT_POD_FALL_HEIGHT * (1.0f - f) * (1.0f - 0.8f * f);
}

long royale_loot_pod_fall_start(
	long play_start,
	long endgame_start,
	long drop_ticks)
{
	long start;

	if (play_start == NONE)
		return NONE;
	start = play_start + drop_ticks;
	if (endgame_start != NONE && endgame_start < start)
		start = endgame_start;
	return start;
}

short royale_loot_kit_action(
	short phase,
	short state,
	boolean kitted,
	boolean armed)
{
	/* (the gulag gives its own, and a winner keeps it) */
	if (state == ROYALE_PLAYER_GULAG)
		return kitted ? ROYALE_LOOT_KIT_LEAVE : ROYALE_LOOT_KIT_KEEP;
	if (!kitted)
		return phase == ROYALE_PHASE_LOBBY || phase == ROYALE_PHASE_INSERTION ? ROYALE_LOOT_KIT_GIVE : ROYALE_LOOT_KIT_KEEP;
	return state == ROYALE_PLAYER_FALLING && !armed ? ROYALE_LOOT_KIT_GIVE : ROYALE_LOOT_KIT_LEAVE;
}

static boolean royale_loot_listed(
	long const *list,
	short count,
	long object_index)
{
	short index;

	for (index = 0; index < count; index++)
	{
		if (list[index] == object_index)
			return TRUE;
	}
	return FALSE;
}

boolean royale_loot_purge_spares(
	long object_index,
	long const *placed,
	short placed_count,
	long const *held,
	short held_count,
	long const *kits,
	short kit_count)
{
	if (royale_loot_listed(kits, kit_count, object_index))
		return FALSE;
	return royale_loot_listed(placed, placed_count, object_index) || royale_loot_listed(held, held_count, object_index);
}

/* ---------- the game */

/* the weapons the host has seen held in the lobby and the drop (spared by
the sweep), and the start kits' pistols */
#define ROYALE_LOOT_MAXIMUM_HELD 1024
#define ROYALE_LOOT_MAXIMUM_KITS 512

enum
{
	_royale_loot_pod_waiting = 0,
	_royale_loot_pod_falling,
	_royale_loot_pod_landed
};

static struct
{
	boolean started;
	boolean placed; /* the ground loot (the host's) */
	struct royale_map_config const *config;
	unsigned long seed;
	unsigned long available;
	long definitions[NUMBER_OF_ROYALE_LOOT_KINDS];
	struct royale_loot_plan plan;
	long crate_definition;
	long trail_effect;
	long landing_effect;
	struct
	{
		short state;
		long fall_start; /* royale_match_time() */
		long crate_index;
	} pods[ROYALE_LOOT_POD_COUNT];
	/* the host: the loot it placed (spared by royale_loot_purge) */
	long placed_items[ROYALE_LOOT_MAXIMUM_PLACED];
	short placed_count;
	/* the host: each player's unit given the start kit (or whose weapons
	are its own: royale_loot_kit_action) */
	long kitted_unit[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];
	/* the host: weapons a unit held (a real drop when loose), and the kits'
	pistols */
	long held_items[ROYALE_LOOT_MAXIMUM_HELD];
	short held_count;
	long kit_items[ROYALE_LOOT_MAXIMUM_KITS];
	short kit_count;
} royale_loot;

static long royale_loot_find_tag(
	long group_tag,
	char const *const *names,
	short name_count)
{
	short index;

	for (index = 0; index < name_count; index++)
	{
		long tag_index = names[index] ? tag_loaded(group_tag, names[index]) : NONE;

		if (tag_index != NONE)
			return tag_index;
	}
	return NONE;
}

struct royale_loot_plan const *royale_loot_plan_get(
	void)
{
	return &royale_loot.plan;
}

void royale_loot_start(
	struct royale_map_config const *config)
{
	static char const *const crate_names[] = { "scenery\\c_storage\\c_storage" };
	static char const *const trail_names[] =
		{ "weapons\\frag grenade\\effects\\smoke", "scenery\\emitters\\heavysmoke\\effects\\heavysmoke" };
	static char const *const landing_names[] =
		{ "vehicles\\pelican\\effects\\pelican explosion", "weapons\\frag grenade\\effects\\explosion" };
	short spot_tiers[ROYALE_MAXIMUM_LOOT_SPOTS];
	short index;

	csmemset(&royale_loot, 0, sizeof(royale_loot));
	csmemset(royale_loot.kitted_unit, 0xFF, sizeof(royale_loot.kitted_unit));
	royale_loot.config = config;
	if (!config)
		return;
	for (index = 0; index < NUMBER_OF_ROYALE_LOOT_KINDS; index++)
	{
		royale_loot.definitions[index] = royale_loot_find_tag(royale_loot_kinds[index].group_tag,
			royale_loot_kinds[index].names, NUMBEROF(royale_loot_kinds[index].names));
		if (royale_loot.definitions[index] != NONE)
			SET_FLAG(royale_loot.available, index, TRUE);
	}
	royale_loot.crate_definition = royale_loot_find_tag('scen', crate_names, NUMBEROF(crate_names));
	royale_loot.trail_effect = royale_loot_find_tag('effe', trail_names, NUMBEROF(trail_names));
	royale_loot.landing_effect = royale_loot_find_tag('effe', landing_names, NUMBEROF(landing_names));
	for (index = 0; index < config->loot_spot_count && index < ROYALE_MAXIMUM_LOOT_SPOTS; index++)
		spot_tiers[index] = config->loot_spots[index].tier;
	royale_loot.seed = royale_match_seed();
	royale_loot_plan_make(royale_loot.seed, spot_tiers, config->loot_spot_count, config->pod_spot_count,
		royale_loot.available, &royale_loot.plan);
	for (index = 0; index < ROYALE_LOOT_POD_COUNT; index++)
		royale_loot.pods[index].crate_index = NONE;
	royale_loot.started = TRUE;
	error(_error_silent, "royale: loot: seed %08lX, kinds %05lX, %d spots, %d pods, crate %s, effects %s/%s",
		royale_loot.seed, royale_loot.available, royale_loot.plan.spot_count, royale_loot.plan.pod_count,
		royale_loot.crate_definition != NONE ? "yes" : "no", royale_loot.trail_effect != NONE ? "yes" : "no",
		royale_loot.landing_effect != NONE ? "yes" : "no");
}

/* a weapon's scarce rounds */
static void royale_loot_ration(
	long weapon_index,
	short rounds_loaded,
	short rounds_total)
{
	struct weapon_datum *weapon = weapon_try_and_get(weapon_index);
	struct weapon_definition *definition;
	short magazine;

	if (!weapon)
		return;
	definition = weapon_definition_get(weapon->definition_index);
	if (definition->weapon.magazines.count <= 0)
	{
		/* (a battery weapon: half its charge) */
		weapon->weapon.age = 0.5f;
		return;
	}
	for (magazine = 0; magazine < MIN(definition->weapon.magazines.count, NUMBEROF(weapon->weapon.magazines));
		magazine++)
	{
		struct weapon_magazine_definition const *magazine_definition =
			TAG_BLOCK_GET_ELEMENT(&definition->weapon.magazines, magazine, struct weapon_magazine_definition);

		if (magazine_definition->rounds_loaded_maximum <= 0 && magazine_definition->rounds_total_maximum <= 0)
		{
			weapon->weapon.age = 0.5f;
			continue;
		}
		weapon->weapon.magazines[magazine].rounds_loaded =
			(short)MIN(rounds_loaded, magazine_definition->rounds_loaded_maximum);
		weapon->weapon.magazines[magazine].rounds_total =
			(short)MIN(rounds_total, magazine_definition->rounds_total_maximum);
	}
}

/* the host places a kind of loot */
static long royale_loot_place(
	byte kind,
	real_point3d const *position)
{
	struct object_placement_data placement;
	long object_index;

	if (kind >= NUMBER_OF_ROYALE_LOOT_KINDS || royale_loot.definitions[kind] == NONE)
		return NONE;
	object_placement_data_new(&placement, royale_loot.definitions[kind], NONE);
	placement.position = *position;
	placement.position.z += 0.15f;
	object_index = object_new(&placement);
	if (object_index == NONE)
		return NONE;
	object_set_garbage(object_index, FALSE);
	if (royale_loot.placed_count < ROYALE_LOOT_MAXIMUM_PLACED)
		royale_loot.placed_items[royale_loot.placed_count++] = object_index;
	if (royale_loot_kinds[kind].group_tag == 'weap')
		royale_loot_ration(object_index, royale_loot_kinds[kind].rounds_loaded, royale_loot_kinds[kind].rounds_total);
	return object_index;
}

/* the host deletes the map's own weapons and equipment lying loose (not
loot's, nor a drop of a weapon a unit held: royale_loot_purge_spares) */
static void royale_loot_purge(
	void)
{
	struct object_iterator iterator;
	struct object_datum *object;
	long doomed[256];
	short count = 0;
	short index;

	object_iterator_new(&iterator, _object_mask_weapon | _object_mask_equipment, 0);
	while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL && count < NUMBEROF(doomed))
	{
		if (object->object.parent_object_index != NONE)
			continue;
		if (!royale_loot_purge_spares(iterator.index, royale_loot.placed_items, royale_loot.placed_count,
			royale_loot.held_items, royale_loot.held_count, royale_loot.kit_items, royale_loot.kit_count))
		{
			doomed[count++] = iterator.index;
		}
	}
	for (index = 0; index < count; index++)
		object_delete(doomed[index]);
	if (count)
		error(_error_silent, "royale: loot: %d of the map's items deleted", count);
}

static void royale_loot_place_ground(
	void)
{
	struct royale_map_config const *config = royale_loot.config;
	short index;
	short placed = 0;

	for (index = 0; index < royale_loot.plan.spot_count; index++)
	{
		if (royale_loot_place(royale_loot.plan.spot_kinds[index], &config->loot_spots[index].position) != NONE)
			placed++;
	}
	error(_error_silent, "royale: loot: %d items on %d spots", placed, royale_loot.plan.spot_count);
}

static void royale_loot_effect(
	long definition_index,
	real_point3d const *position)
{
	if (definition_index == NONE)
		return;
	effect_new_unattached_from_markers(definition_index, NONE, global_zero_vector3d, 1, NULL, position, global_up3d,
		1.0f, 1.0f, NULL, NULL, TRUE);
}

/* every weapon of a unit gone, the one in its hand too (which
unit_delete_all_weapons keeps) */
static void royale_loot_delete_weapons(
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

/* whether the unit holds any weapon */
static boolean royale_loot_unit_armed(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);
	short slot;

	for (slot = 0; unit && slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		if (unit->unit.weapon_object_indices[slot] != NONE)
			return TRUE;
	}
	return FALSE;
}

/* the start kit: the unit's weapons and grenades gone, a pistol low on
rounds */
static void royale_loot_give_kit(
	long player_index)
{
	struct player_datum *player = player_try_and_get(player_index);
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	struct unit_datum *unit;
	struct object_placement_data placement;
	long weapon_index;
	short grenade;

	if (!royale_loot.started || network_game_distributed_client() || !player ||
		!VALID_INDEX(slot, HALO_PORT_MAXIMUM_NETWORK_PLAYERS) || player->quit_out_of_game ||
		player->unit_index == NONE)
	{
		return;
	}
	unit = unit_try_and_get(player->unit_index);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return;
	switch (royale_loot_kit_action(royale_phase(), royale_player_state(player_index),
		royale_loot.kitted_unit[slot] == player->unit_index, royale_loot_unit_armed(player->unit_index)))
	{
	case ROYALE_LOOT_KIT_GIVE:
		break;
	case ROYALE_LOOT_KIT_KEEP:
		royale_loot.kitted_unit[slot] = player->unit_index;
		return;
	default:
		return;
	}
	royale_loot.kitted_unit[slot] = player->unit_index;
	royale_loot_delete_weapons(player->unit_index);
	for (grenade = 0; grenade < NUMBER_OF_UNIT_GRENADE_TYPES; grenade++)
		unit->unit.grenade_counts[grenade] = 0;
	if (royale_loot.definitions[ROYALE_LOOT_PISTOL] == NONE)
		return;
	object_placement_data_new(&placement, royale_loot.definitions[ROYALE_LOOT_PISTOL], NONE);
	weapon_index = object_new(&placement);
	if (weapon_index == NONE)
		return;
	royale_loot_ration(weapon_index, royale_loot_start_rounds[0], royale_loot_start_rounds[1]);
	if (!unit_add_weapon_to_inventory(player->unit_index, weapon_index, 2))
		object_delete(weapon_index);
	else if (royale_loot.kit_count < ROYALE_LOOT_MAXIMUM_KITS)
		royale_loot.kit_items[royale_loot.kit_count++] = weapon_index;
}

/* the host: the weapons its players hold now, noted (a real drop when they
let go of one) */
static void royale_loot_note_held(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct unit_datum *unit = player->unit_index != NONE ? unit_try_and_get(player->unit_index) : NULL;
		short slot;

		for (slot = 0; unit && slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long weapon_index = unit->unit.weapon_object_indices[slot];

			if (weapon_index != NONE && royale_loot.held_count < ROYALE_LOOT_MAXIMUM_HELD &&
				!royale_loot_listed(royale_loot.held_items, royale_loot.held_count, weapon_index))
			{
				royale_loot.held_items[royale_loot.held_count++] = weapon_index;
			}
		}
	}
}

void royale_loot_postspawn(
	long player_index)
{
	royale_loot_give_kit(player_index);
}

/* each pod: waits for its time in PLAY, falls (every machine its own
crate), lands (the host places its loot) */
static void royale_loot_pods_tick(
	void)
{
	struct royale_map_config const *config = royale_loot.config;
	long now = royale_match_time();
	long endgame_start = royale_phase() == ROYALE_PHASE_ENDGAME ? now - royale_phase_time() : NONE;
	long fall_ticks = MAX(royale_seconds_to_ticks(ROYALE_LOOT_POD_FALL_SECONDS), 1);
	long trail_ticks = MAX(royale_seconds_to_ticks(ROYALE_LOOT_TRAIL_SECONDS), 1);
	short index;

	if (royale_phase() != ROYALE_PHASE_PLAY && royale_phase() != ROYALE_PHASE_ENDGAME)
		return;
	for (index = 0; index < royale_loot.plan.pod_count; index++)
	{
		struct royale_loot_pod_plan const *plan = &royale_loot.plan.pods[index];
		real_point3d const *ground = &config->pod_spots[plan->spot];
		real_point3d position = *ground;
		long elapsed;
		real fraction;
		short item;

		/* (from the match clock: every machine's pod falls alike; one still
		to fall at ENDGAME falls then) */
		if (royale_loot.pods[index].state == _royale_loot_pod_waiting)
		{
			long fall_start = royale_loot_pod_fall_start(royale_play_start_time(), endgame_start,
				royale_seconds_to_ticks((real)plan->drop_second));

			if (fall_start == NONE || now < fall_start)
				continue;
			royale_loot.pods[index].state = _royale_loot_pod_falling;
			royale_loot.pods[index].fall_start = fall_start;
			position.z += ROYALE_LOOT_POD_FALL_HEIGHT;
			if (royale_loot.crate_definition != NONE)
			{
				struct object_placement_data placement;

				object_placement_data_new(&placement, royale_loot.crate_definition, NONE);
				placement.position = position;
				royale_loot.pods[index].crate_index = object_new(&placement);
			}
			error(_error_silent, "royale: loot: supply pod %d falls on spot %d at match tick %ld", index, plan->spot,
				fall_start);
		}
		if (royale_loot.pods[index].state != _royale_loot_pod_falling)
			continue;
		elapsed = MAX(now - royale_loot.pods[index].fall_start, 0);
		fraction = MIN((real)elapsed / (real)fall_ticks, 1.0f);
		position.z = ground->z + royale_loot_pod_height(fraction);
		if (royale_loot.pods[index].crate_index != NONE && object_try_and_get(royale_loot.pods[index].crate_index))
			object_set_position(royale_loot.pods[index].crate_index, &position, NULL, NULL);
		if (fraction < 1.0f)
		{
			if (elapsed % trail_ticks == 0)
				royale_loot_effect(royale_loot.trail_effect, &position);
			continue;
		}
		royale_loot.pods[index].state = _royale_loot_pod_landed;
		royale_loot_effect(royale_loot.landing_effect, ground);
		if (network_game_distributed_client())
			continue;
		for (item = 0; item < ROYALE_LOOT_POD_ITEMS; item++)
		{
			real angle = (real)item * (2.0f * _pi / (real)ROYALE_LOOT_POD_ITEMS);
			real_point3d spot = *ground;

			spot.x += ROYALE_LOOT_POD_ITEM_RADIUS * cosf(angle);
			spot.y += ROYALE_LOOT_POD_ITEM_RADIUS * sinf(angle);
			royale_loot_place(plan->kinds[item], &spot);
		}
		error(_error_silent, "royale: loot: supply pod %d landed at match tick %ld", index, now);
	}
}

void royale_loot_tick(
	void)
{
	struct player_datum *player;
	struct data_iterator iterator;
	boolean host = !network_game_distributed_client();

	if (!royale_loot.started || !royale_active())
		return;
	if (host)
	{
		if (royale_phase() == ROYALE_PHASE_LOBBY || royale_phase() == ROYALE_PHASE_INSERTION)
			royale_loot_note_held();
		/* the ground loot placed at once, and the map's own items deleted
		(again each second: scripts and BSP loads place theirs late) until
		PLAY, when players can drop their own */
		if (!royale_loot.placed || ((royale_phase() == ROYALE_PHASE_LOBBY || royale_phase() == ROYALE_PHASE_INSERTION) &&
			royale_phase_time() % MAX(royale_seconds_to_ticks(1.0f), 1) == 0))
		{
			royale_loot_purge();
		}
		if (!royale_loot.placed)
		{
			royale_loot_place_ground();
			royale_loot.placed = TRUE;
		}
		/* (a backstop for royale_loot_postspawn) */
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			royale_loot_give_kit(iterator.datum_index);
	}
	royale_loot_pods_tick();
}

short royale_loot_item_tier(
	long object_index)
{
	struct object_datum *object = object_try_and_get_and_verify_type(object_index,
		_object_mask_weapon | _object_mask_equipment);
	short kind;

	if (!royale_loot.started || !object || object->object.parent_object_index != NONE)
		return NONE;
	for (kind = 0; kind < NUMBER_OF_ROYALE_LOOT_KINDS; kind++)
	{
		if (royale_loot.definitions[kind] == object->definition_index)
		{
			switch (royale_loot_kinds[kind].rarity)
			{
				case ROYALE_LOOT_RARITY_RARE: return ROYALE_LOOT_RARE;
				case ROYALE_LOOT_RARITY_POWERUP: return ROYALE_LOOT_EPIC;
				default: return ROYALE_LOOT_COMMON;
			}
		}
	}
	return NONE;
}

/* (royale.h) no config, no plan, nothing placed or kitted */
void royale_loot_reset(
	void)
{
	csmemset(&royale_loot, 0, sizeof(royale_loot));
	csmemset(royale_loot.kitted_unit, 0xFF, sizeof(royale_loot.kitted_unit));
}
