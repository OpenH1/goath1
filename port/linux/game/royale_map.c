/*
ROYALE_MAP.C

A campaign level played as Royale's map (royale_map.h): Silent Cartographer's
island (b30) without its campaign, its interior closed, its players kept on
the island.

The level is loaded as shipped; what changes is changed in memory as it
loads (royale_map_prepare) and put back as it goes (royale_map_restore):

- the globals' multiplayer part, which a campaign level's lack (the game
  engine's unit, sounds and flags), stood in for with the campaign's own
  player unit and no sounds, vehicles or flags;
- the players' starts opened to every game type;
- the campaign's placed objects that are not the island's own taken out of
  the placement (its bipeds, the weapons and equipment lying about, the
  Pelicans, dropships, Banshees and the cutscene's falling boxes): the
  Warthogs, Ghosts and scenery stay.

In the mode (royale_map_mode) the game does not run the level's scripts
(game.c: so no cutscene, objective, AI placement or scripted BSP switch),
place or update its AI (game.c), or switch the BSP for a player walking
into a switch volume (main.c): the exterior structure BSP (0) stays.

Royale's map config (port/assets/royale/b30.toml) then gives the boxes of
the interior's entrances, which no player passes (pushed back out of the
nearest side), and the island's bounds: a player outside them or down in
the water (below the bounds' bottom) is put back where they last stood
inside. Players in a Pelican, falling, in the gulag or out of the game are
left to their own lanes. Every rule is simulation: the same on every
machine, from the game's state alone.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_groups.h"
#include "royale.h"
#include "royale_map.h"

#include <string.h>

/* ---------- constants */

/* game_engine.c's _game_engine_all: a start for every game type */
#define ROYALE_MAP_GAME_TYPE_ALL 12

/* the scenario's placement blocks' element sizes (scenario_definitions.h's
scenario_biped_datum ...: a scenario_object_datum first, its placement
flags at 4) */
#define ROYALE_MAP_BIPED_SIZE 0x78
#define ROYALE_MAP_VEHICLE_SIZE 0x78
#define ROYALE_MAP_WEAPON_SIZE 0x5C
#define ROYALE_MAP_EQUIPMENT_SIZE 0x28
#define ROYALE_MAP_PALETTE_SIZE 0x30

#define ROYALE_MAP_MAXIMUM_CHANGES 1024
#define ROYALE_MAP_MAXIMUM_STARTS 64

/* how far past a blocked entrance's side a player is put, and how far below
the bounds' bottom a player still stands (wading at the water's edge) */
#define ROYALE_MAP_PUSH_MARGIN 0.25f

/* the vehicles of the campaign's cutscenes and scripts, by their tags'
names' last part */
static char const *const royale_map_campaign_vehicles[] =
{
	"pelican",
	"c_dropship",
	"banshee",
	"b30_falling_box",
};

/* ---------- globals */

static struct
{
	boolean prepared;
	/* (the scenario it was prepared on: another's tags were loaded since,
	with nothing to put back) */
	struct scenario const *prepared_scenario;
	/* the globals' multiplayer part as the level had it, and the stand-in */
	struct tag_block *multiplayer_block;
	struct tag_block multiplayer_original;
	struct game_globals_multiplayer_information multiplayer_stand_in;
	/* placement flags changed: where, and what they were */
	short change_count;
	word *change_address[ROYALE_MAP_MAXIMUM_CHANGES];
	word change_original[ROYALE_MAP_MAXIMUM_CHANGES];
	/* the players' starts' game types as they were */
	short start_count;
	short start_original[ROYALE_MAP_MAXIMUM_STARTS][4];

	/* the game's: its config (NULL until royale_map_start), and where each
	player last stood inside */
	struct royale_map_config const *config;
	boolean safe_valid[ROYALE_MAXIMUM_PLAYERS];
	real_point3d safe_point[ROYALE_MAXIMUM_PLAYERS];
} royale_map;

/* ---------- private code */

static void royale_map_change_flags(word *flags, word value)
{
	if (royale_map.change_count < ROYALE_MAP_MAXIMUM_CHANGES)
	{
		royale_map.change_address[royale_map.change_count] = flags;
		royale_map.change_original[royale_map.change_count] = *flags;
		royale_map.change_count++;
		*flags = value;
	}
}

/* each placement of the block not placed at the start of the game (its
placement's "not automatic" flag) */
static void royale_map_unplace_all(struct tag_block *block, long element_size)
{
	long index;

	for (index = 0; index < block->count && block->address; index++)
	{
		struct scenario_object_datum *object =
			(struct scenario_object_datum *)((char *)block->address + index * element_size);

		if (!TEST_FLAG(object->placement_flags, _scenario_object_placement_not_automatic_bit))
			royale_map_change_flags(&object->placement_flags,
				object->placement_flags | FLAG(_scenario_object_placement_not_automatic_bit));
	}
}

static boolean royale_map_campaign_vehicle(struct tag_reference const *reference)
{
	char const *name = reference->name;
	char const *last;
	short index;

	if (!name)
		return FALSE;
	last = strrchr(name, '\\');
	last = last ? last + 1 : name;
	for (index = 0; index < (short)NUMBEROF(royale_map_campaign_vehicles); index++)
	{
		if (!strcmp(last, royale_map_campaign_vehicles[index]))
			return TRUE;
	}
	return FALSE;
}

static void royale_map_unplace_campaign_vehicles(struct scenario *scenario)
{
	long index;

	for (index = 0; index < scenario->vehicles.count && scenario->vehicles.address; index++)
	{
		struct scenario_object_datum *object =
			(struct scenario_object_datum *)((char *)scenario->vehicles.address + index * ROYALE_MAP_VEHICLE_SIZE);
		short palette_index = object->palette_entry_index;
		struct tag_reference const *reference;

		if (palette_index < 0 || palette_index >= scenario->vehicle_palette.count || !scenario->vehicle_palette.address)
			continue;
		reference = (struct tag_reference const *)((char *)scenario->vehicle_palette.address +
			palette_index * ROYALE_MAP_PALETTE_SIZE);
		if (royale_map_campaign_vehicle(reference) &&
			!TEST_FLAG(object->placement_flags, _scenario_object_placement_not_automatic_bit))
		{
			royale_map_change_flags(&object->placement_flags,
				object->placement_flags | FLAG(_scenario_object_placement_not_automatic_bit));
		}
	}
}

/* the globals' multiplayer part, which a campaign level's lack: the
campaign's player unit, and no flag, ball, vehicles or sounds */
static void royale_map_stand_in_multiplayer(void)
{
	struct game_globals *game_globals = scenario_get_game_globals();
	struct game_globals_multiplayer_information *stand_in = &royale_map.multiplayer_stand_in;

	if (!game_globals || game_globals->multiplayer_information.count > 0)
		return;
	csmemset(stand_in, 0, sizeof(*stand_in));
	stand_in->flag.index = NONE;
	stand_in->unit.index = NONE;
	stand_in->hill_shader.index = NONE;
	stand_in->flag_shader.index = NONE;
	stand_in->ball.index = NONE;
	if (game_globals->player_information.count > 0 && game_globals->player_information.address)
	{
		struct game_globals_player_information const *player_information =
			(struct game_globals_player_information const *)game_globals->player_information.address;

		stand_in->unit = player_information->player_unit;
	}
	royale_map.multiplayer_block = &game_globals->multiplayer_information;
	royale_map.multiplayer_original = game_globals->multiplayer_information;
	game_globals->multiplayer_information.count = 1;
	game_globals->multiplayer_information.address = stand_in;
	game_globals->multiplayer_information.definition = NULL;
}

static void royale_map_open_starts(struct scenario *scenario)
{
	long index;

	royale_map.start_count = 0;
	for (index = 0; index < scenario->players.count && scenario->players.address &&
		index < ROYALE_MAP_MAXIMUM_STARTS; index++)
	{
		struct player_starting_location *start =
			(struct player_starting_location *)scenario->players.address + index;

		csmemcpy(royale_map.start_original[index], start->game_types, sizeof(start->game_types));
		start->game_types[0] = ROYALE_MAP_GAME_TYPE_ALL;
		royale_map.start_count = (short)(index + 1);
	}
}

static boolean royale_map_inside(struct royale_bounds const *bounds, real_point3d const *point, real margin)
{
	return point->x > bounds->minimum.x - margin && point->x < bounds->maximum.x + margin &&
		point->y > bounds->minimum.y - margin && point->y < bounds->maximum.y + margin &&
		point->z > bounds->minimum.z - margin && point->z < bounds->maximum.z + margin;
}

/* a point in a blocked entrance's box put just out of its nearest side (x or
y: the floor stays the floor) */
static void royale_map_push_out(struct royale_bounds const *bounds, real_point3d *point)
{
	real to_minimum_x = point->x - bounds->minimum.x;
	real to_maximum_x = bounds->maximum.x - point->x;
	real to_minimum_y = point->y - bounds->minimum.y;
	real to_maximum_y = bounds->maximum.y - point->y;
	real nearest = MIN(MIN(to_minimum_x, to_maximum_x), MIN(to_minimum_y, to_maximum_y));

	if (nearest == to_minimum_x)
		point->x = bounds->minimum.x - ROYALE_MAP_PUSH_MARGIN;
	else if (nearest == to_maximum_x)
		point->x = bounds->maximum.x + ROYALE_MAP_PUSH_MARGIN;
	else if (nearest == to_minimum_y)
		point->y = bounds->minimum.y - ROYALE_MAP_PUSH_MARGIN;
	else
		point->y = bounds->maximum.y + ROYALE_MAP_PUSH_MARGIN;
}

/* what royale_map_keep_point did */
enum
{
	_royale_map_kept_inside = 0, /* inside, out of every blocked box: now its last safe point */
	_royale_map_kept_pushed_out, /* in a blocked box: put out of its nearest side */
	_royale_map_kept_put_back, /* out of bounds: put back at its last safe point */
	_royale_map_kept_lost, /* out of bounds with no safe point yet: left */
};

/* a player's point kept on the island and out of the blocked boxes: moved
(and what was done) or, inside, remembered as its last safe point */
static short royale_map_keep_point(struct royale_map_config const *config, real_point3d *point,
	boolean *safe_valid, real_point3d *safe_point)
{
	short index;

	for (index = 0; index < config->blocked_entrance_count; index++)
	{
		struct royale_bounds const *box = &config->blocked_entrances[index].bounds;

		if (royale_map_inside(box, point, 0.0f))
		{
			royale_map_push_out(box, point);
			return _royale_map_kept_pushed_out;
		}
	}
	if (!royale_map_inside(&config->bounds, point, 0.0f))
	{
		if (!*safe_valid)
			return _royale_map_kept_lost;
		*point = *safe_point;
		return _royale_map_kept_put_back;
	}
	*safe_point = *point;
	*safe_valid = TRUE;
	return _royale_map_kept_inside;
}

static void royale_map_move(long object_index, real_point3d const *point)
{
	struct object_datum *object = object_get(object_index);

	object_set_position(object_index, point, NULL, NULL);
	object->object.translational_velocity = *global_zero_vector3d;
}

static void royale_map_keep_player(long player_index, struct player_datum const *player)
{
	struct royale_map_config const *config = royale_map.config;
	long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	long object_index;
	real_point3d origin;
	short kept;

	if (!VALID_INDEX(slot, ROYALE_MAXIMUM_PLAYERS))
		return;
	switch (royale_player_state(player_index))
	{
	case ROYALE_PLAYER_IN_PELICAN:
	case ROYALE_PLAYER_FALLING:
	case ROYALE_PLAYER_GULAG:
	case ROYALE_PLAYER_SPECTATING:
	case ROYALE_PLAYER_ELIMINATED:
		royale_map.safe_valid[slot] = FALSE;
		return;
	default:
		break;
	}
	if (player->unit_index == NONE)
	{
		royale_map.safe_valid[slot] = FALSE;
		return;
	}
	/* (a player in a vehicle: the vehicle) */
	object_index = object_get_ultimate_parent(player->unit_index);
	object_get_origin(object_index, &origin);

	kept = royale_map_keep_point(config, &origin, &royale_map.safe_valid[slot], &royale_map.safe_point[slot]);
	if (kept == _royale_map_kept_pushed_out || kept == _royale_map_kept_put_back)
		royale_map_move(object_index, &origin);
}

/* ---------- public code */

boolean royale_map_mode(void)
{
	/* (Royale's alone: any other game type on a campaign level keeps its
	AI, scripts and BSP switches) */
	return global_scenario && global_scenario->type == _scenario_type_solo && royale_active();
}

void royale_map_prepare(void)
{
	struct scenario *scenario = global_scenario;

	if (royale_map.prepared && royale_map.prepared_scenario != scenario)
		royale_map.prepared = FALSE;
	if (royale_map.prepared || !royale_map_mode())
		return;
	royale_map.prepared = TRUE;
	royale_map.prepared_scenario = scenario;
	royale_map.change_count = 0;
	royale_map.multiplayer_block = NULL;
	royale_map_stand_in_multiplayer();
	royale_map_open_starts(scenario);
	royale_map_unplace_all(&scenario->bipeds, ROYALE_MAP_BIPED_SIZE);
	royale_map_unplace_all(&scenario->weapons, ROYALE_MAP_WEAPON_SIZE);
	royale_map_unplace_all(&scenario->equipment, ROYALE_MAP_EQUIPMENT_SIZE);
	royale_map_unplace_campaign_vehicles(scenario);
	error(_error_silent, "royale map: %s played as a multiplayer map (%d placements taken out, %d starts opened)",
		scenario->type == _scenario_type_solo ? "a campaign level" : "a map",
		(int)royale_map.change_count, (int)royale_map.start_count);
}

void royale_map_restore(void)
{
	struct scenario *scenario = global_scenario;
	short index;

	if (!royale_map.prepared || royale_map.prepared_scenario != scenario)
	{
		royale_map.prepared = FALSE;
		return;
	}
	for (index = royale_map.change_count - 1; index >= 0; index--)
		*royale_map.change_address[index] = royale_map.change_original[index];
	if (scenario)
	{
		for (index = 0; index < royale_map.start_count && index < scenario->players.count; index++)
		{
			struct player_starting_location *start =
				(struct player_starting_location *)scenario->players.address + index;

			csmemcpy(start->game_types, royale_map.start_original[index], sizeof(start->game_types));
		}
	}
	if (royale_map.multiplayer_block)
		*royale_map.multiplayer_block = royale_map.multiplayer_original;
	royale_map.multiplayer_block = NULL;
	royale_map.change_count = 0;
	royale_map.start_count = 0;
	royale_map.prepared = FALSE;
	royale_map.config = NULL;
}

void royale_map_start(struct royale_map_config const *config)
{
	royale_map.config = config;
	csmemset(royale_map.safe_valid, 0, sizeof(royale_map.safe_valid));
}

void royale_map_tick(void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	if (!royale_map.config || !royale_map_mode())
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		royale_map_keep_player(iterator.datum_index, player);
}

/* (royale.h) the game's part: no config, no safe points (the level's tags
are royale_map_restore's) */
void royale_map_reset(void)
{
	royale_map.config = NULL;
	csmemset(royale_map.safe_valid, 0, sizeof(royale_map.safe_valid));
}
