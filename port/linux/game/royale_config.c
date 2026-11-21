/*
ROYALE_CONFIG.C

A Royale map's config (royale.h's struct royale_map_config), from
port/assets/royale/<map>.toml: built into the game (royale_maps.inc, written
by tools/royale_maps.py from those files), or for fast iteration (a network
test's run alone) read from the file HALO_ROYALE_MAP_CONFIG names.

The files are a small part of TOML: comments (#), key = value lines, [table]
and [[array of tables]] headers; a value is a number, a "string", or an
array of numbers or strings ([1.5, 2, -3], ["north", "east"]). A point is
[x, y, z] in world units.

	map = "b30"
	structure_bsp = 0
	drop_height = 40.0
	bounds_min = [-50, -60, -10]
	bounds_max = [80, 70, 40]
	beam_sides = ["north", "east", "south", "west"]

	[[flight_path]]
	start = [x, y, z]
	end = [x, y, z]
	speed = 6.0

	[[beam_stage]]
	delay = 60          # seconds' wait before it moves
	start = 0.0         # fractions of the map's length from the beam's side
	end = 0.25
	duration = 45       # seconds
	damage_per_second = 0.1

	[[loot]]
	position = [x, y, z]
	tier = "common"     # common, rare, epic, legendary (or 0-3)

	[[pod]]
	position = [x, y, z]

	[gulag]
	bounds_min = [x, y, z]
	bounds_max = [x, y, z]
	spawn_a = [x, y, z]
	spawn_b = [x, y, z]
	facing_a = 0.0      # radians
	facing_b = 3.14

	[[blocked]]
	bounds_min = [x, y, z]
	bounds_max = [x, y, z]

An unknown key or table is skipped (a newer file read by an older build); a
line that does not parse fails the whole file.
*/

#include "cseries.h"
#include "royale.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(char const *format, ...);

/* the built-in configs: { map name, text } */
#include "royale_maps.inc"

#define ROYALE_CONFIG_MAXIMUM_VALUES 8
/* the largest number a config takes (world units, seconds): a bigger one,
or an infinity or a NaN, is a broken file */
#define ROYALE_CONFIG_MAXIMUM_MAGNITUDE 1.0e6
/* a scenario's structure BSPs (scenario.h's 32) */
#define ROYALE_CONFIG_MAXIMUM_STRUCTURE_BSPS 32

enum
{
	_section_root = 0,
	_section_flight_path,
	_section_beam_stage,
	_section_loot,
	_section_pod,
	_section_gulag,
	_section_blocked,
	_section_unknown
};

struct royale_config_value
{
	short count;
	boolean is_string;
	real numbers[ROYALE_CONFIG_MAXIMUM_VALUES];
	char strings[ROYALE_CONFIG_MAXIMUM_VALUES][32];
};

static char const *royale_config_skip_space(
	char const *text)
{
	while (*text == ' ' || *text == '\t' || *text == '\r')
		text++;
	return text;
}

/* one scalar: a number or a "string" */
static char const *royale_config_scalar(
	char const *text,
	struct royale_config_value *value)
{
	text = royale_config_skip_space(text);
	if (value->count >= ROYALE_CONFIG_MAXIMUM_VALUES)
		return NULL;
	if (*text == '"')
	{
		short length = 0;

		text++;
		while (*text && *text != '"' && *text != '\n')
		{
			if (length < (short)sizeof(value->strings[0]) - 1)
				value->strings[value->count][length++] = *text;
			text++;
		}
		if (*text != '"')
			return NULL;
		value->strings[value->count][length] = 0;
		value->is_string = TRUE;
		value->count++;
		return text + 1;
	}
	else
	{
		char *end;
		double number = strtod(text, &end);

		/* (not number - number == 0: an infinity or a NaN; nor one too large
		for a real) */
		if (end == text || number - number != 0.0 || number > ROYALE_CONFIG_MAXIMUM_MAGNITUDE ||
			number < -ROYALE_CONFIG_MAXIMUM_MAGNITUDE)
		{
			return NULL;
		}
		value->numbers[value->count++] = (real)number;
		return end;
	}
}

static char const *royale_config_value_parse(
	char const *text,
	struct royale_config_value *value)
{
	csmemset(value, 0, sizeof(*value));
	text = royale_config_skip_space(text);
	if (*text != '[')
		return royale_config_scalar(text, value);
	text++;
	for (;;)
	{
		text = royale_config_skip_space(text);
		if (*text == ']')
			return text + 1;
		text = royale_config_scalar(text, value);
		if (!text)
			return NULL;
		text = royale_config_skip_space(text);
		if (*text == ',')
			text++;
		else if (*text != ']')
			return NULL;
	}
}

static boolean royale_config_point(
	struct royale_config_value const *value,
	real_point3d *point)
{
	if (value->is_string || value->count != 3)
		return FALSE;
	point->x = value->numbers[0];
	point->y = value->numbers[1];
	point->z = value->numbers[2];
	return TRUE;
}

static boolean royale_config_number(
	struct royale_config_value const *value,
	real *number)
{
	if (value->is_string || value->count != 1)
		return FALSE;
	*number = value->numbers[0];
	return TRUE;
}

static short royale_config_side(
	char const *name)
{
	static char const *const names[NUMBER_OF_ROYALE_SIDES] = { "north", "east", "south", "west" };
	short side;

	for (side = 0; side < NUMBER_OF_ROYALE_SIDES; side++)
	{
		if (!strcmp(name, names[side]))
			return side;
	}
	return NONE;
}

static short royale_config_tier(
	struct royale_config_value const *value)
{
	static char const *const names[NUMBER_OF_ROYALE_LOOT_TIERS] = { "common", "rare", "epic", "legendary" };
	short tier;

	if (value->count != 1)
		return NONE;
	if (!value->is_string)
	{
		tier = (short)value->numbers[0];
		return tier >= 0 && tier < NUMBER_OF_ROYALE_LOOT_TIERS ? tier : NONE;
	}
	for (tier = 0; tier < NUMBER_OF_ROYALE_LOOT_TIERS; tier++)
	{
		if (!strcmp(value->strings[0], names[tier]))
			return tier;
	}
	return NONE;
}

/* a key's value into the config: FALSE if it is the wrong kind */
static boolean royale_config_set(
	struct royale_map_config *config,
	short section,
	char const *key,
	struct royale_config_value const *value)
{
	real number;

	switch (section)
	{
	case _section_root:
		if (!strcmp(key, "map"))
		{
			if (!value->is_string || value->count != 1)
				return FALSE;
			strncpy(config->map_name, value->strings[0], sizeof(config->map_name) - 1);
			return TRUE;
		}
		if (!strcmp(key, "structure_bsp"))
		{
			/* (a whole BSP index the scenario can have) */
			if (!royale_config_number(value, &number) || number != (real)(short)number || number < 0.0f ||
				number >= ROYALE_CONFIG_MAXIMUM_STRUCTURE_BSPS)
			{
				return FALSE;
			}
			config->structure_bsp = (short)number;
			return TRUE;
		}
		if (!strcmp(key, "drop_height"))
			return royale_config_number(value, &config->drop_height);
		if (!strcmp(key, "bounds_min"))
			return royale_config_point(value, &config->bounds.minimum);
		if (!strcmp(key, "bounds_max"))
			return royale_config_point(value, &config->bounds.maximum);
		if (!strcmp(key, "beam_sides"))
		{
			short index;

			if (!value->is_string)
				return FALSE;
			config->beam_sides = 0;
			for (index = 0; index < value->count; index++)
			{
				short side = royale_config_side(value->strings[index]);

				if (side == NONE)
					return FALSE;
				config->beam_sides |= FLAG(side);
			}
			return TRUE;
		}
		return TRUE;
	case _section_flight_path:
	{
		struct royale_flight_path *path = &config->flight_paths[config->flight_path_count - 1];

		if (!strcmp(key, "start"))
			return royale_config_point(value, &path->start);
		if (!strcmp(key, "end"))
			return royale_config_point(value, &path->end);
		/* (a speed given is one: none, and the insertion picks its own) */
		if (!strcmp(key, "speed"))
			return royale_config_number(value, &path->speed) && path->speed > 0.0f;
		return TRUE;
	}
	case _section_beam_stage:
	{
		struct royale_beam_stage *stage = &config->beam_stages[config->beam_stage_count - 1];

		if (!strcmp(key, "delay"))
			return royale_config_number(value, &stage->delay);
		if (!strcmp(key, "start"))
			return royale_config_number(value, &stage->start);
		if (!strcmp(key, "end"))
			return royale_config_number(value, &stage->end);
		if (!strcmp(key, "duration"))
			return royale_config_number(value, &stage->duration);
		if (!strcmp(key, "damage_per_second"))
			return royale_config_number(value, &stage->damage_per_second);
		return TRUE;
	}
	case _section_loot:
	{
		struct royale_loot_spot *spot = &config->loot_spots[config->loot_spot_count - 1];

		if (!strcmp(key, "position"))
			return royale_config_point(value, &spot->position);
		if (!strcmp(key, "tier"))
		{
			spot->tier = royale_config_tier(value);
			return spot->tier != NONE;
		}
		return TRUE;
	}
	case _section_pod:
		if (!strcmp(key, "position"))
			return royale_config_point(value, &config->pod_spots[config->pod_spot_count - 1]);
		return TRUE;
	case _section_gulag:
		if (!strcmp(key, "bounds_min"))
			return royale_config_point(value, &config->gulag.bounds.minimum);
		if (!strcmp(key, "bounds_max"))
			return royale_config_point(value, &config->gulag.bounds.maximum);
		if (!strcmp(key, "spawn_a"))
			return royale_config_point(value, &config->gulag.spawn[0]);
		if (!strcmp(key, "spawn_b"))
			return royale_config_point(value, &config->gulag.spawn[1]);
		if (!strcmp(key, "facing_a"))
			return royale_config_number(value, &config->gulag.spawn_facing[0]);
		if (!strcmp(key, "facing_b"))
			return royale_config_number(value, &config->gulag.spawn_facing[1]);
		return TRUE;
	case _section_blocked:
	{
		struct royale_blocked_entrance *blocked = &config->blocked_entrances[config->blocked_entrance_count - 1];

		if (!strcmp(key, "bounds_min"))
			return royale_config_point(value, &blocked->bounds.minimum);
		if (!strcmp(key, "bounds_max"))
			return royale_config_point(value, &blocked->bounds.maximum);
		return TRUE;
	}
	}
	return TRUE;
}

/* a [table] or [[array of tables]] header: its section (a new element of
an array), _section_unknown for one skipped, NONE if full */
static short royale_config_section(
	struct royale_map_config *config,
	char const *name,
	boolean array)
{
	if (array && !strcmp(name, "flight_path"))
		return config->flight_path_count < ROYALE_MAXIMUM_FLIGHT_PATHS ?
			(config->flight_path_count++, _section_flight_path) : NONE;
	if (array && !strcmp(name, "beam_stage"))
		return config->beam_stage_count < ROYALE_MAXIMUM_BEAM_STAGES ?
			(config->beam_stage_count++, _section_beam_stage) : NONE;
	if (array && !strcmp(name, "loot"))
		return config->loot_spot_count < ROYALE_MAXIMUM_LOOT_SPOTS ?
			(config->loot_spot_count++, _section_loot) : NONE;
	if (array && !strcmp(name, "pod"))
		return config->pod_spot_count < ROYALE_MAXIMUM_POD_SPOTS ? (config->pod_spot_count++, _section_pod) : NONE;
	if (array && !strcmp(name, "blocked"))
		return config->blocked_entrance_count < ROYALE_MAXIMUM_BLOCKED_ENTRANCES ?
			(config->blocked_entrance_count++, _section_blocked) : NONE;
	if (!array && !strcmp(name, "gulag"))
		return _section_gulag;
	return _section_unknown;
}

/* a box's minimum is no more than its maximum (all zero: none given) */
static boolean royale_config_bounds_valid(
	struct royale_bounds const *bounds)
{
	return bounds->minimum.x <= bounds->maximum.x && bounds->minimum.y <= bounds->maximum.y &&
		bounds->minimum.z <= bounds->maximum.z;
}

/* the values in range, as a whole (each number is finite already) */
static boolean royale_config_valid(
	struct royale_map_config const *config)
{
	short index;

	if (!royale_config_bounds_valid(&config->bounds) || !royale_config_bounds_valid(&config->gulag.bounds))
		return FALSE;
	for (index = 0; index < config->blocked_entrance_count; index++)
	{
		if (!royale_config_bounds_valid(&config->blocked_entrances[index].bounds))
			return FALSE;
	}
	for (index = 0; index < config->beam_stage_count; index++)
	{
		struct royale_beam_stage const *stage = &config->beam_stages[index];

		/* (a stage moves over some time, after no negative wait, within the
		map's length, hurting no less than nothing) */
		if (!(stage->duration > 0.0f) || stage->delay < 0.0f || stage->start < 0.0f || stage->start > 1.0f ||
			stage->end < 0.0f || stage->end > 1.0f || stage->damage_per_second < 0.0f)
		{
			return FALSE;
		}
	}
	return TRUE;
}

boolean royale_map_config_parse(
	char const *text,
	struct royale_map_config *config)
{
	short section = _section_root;
	long line_number = 0;

	csmemset(config, 0, sizeof(*config));
	while (text && *text)
	{
		char const *line = royale_config_skip_space(text);
		char const *next = strchr(text, '\n');

		line_number++;
		text = next ? next + 1 : NULL;
		if (*line == '#' || *line == '\n' || !*line)
			continue;
		if (*line == '[')
		{
			boolean array = line[1] == '[';
			char name[32];
			short length = 0;
			char const *cursor = line + (array ? 2 : 1);

			while (*cursor && *cursor != ']' && *cursor != '\n' && length < (short)sizeof(name) - 1)
				name[length++] = *cursor++;
			name[length] = 0;
			if (*cursor != ']' || (array && cursor[1] != ']'))
				goto fail;
			section = royale_config_section(config, name, array);
			if (section == NONE)
				goto fail;
			continue;
		}
		{
			char key[32];
			short length = 0;
			char const *cursor = line;
			struct royale_config_value value;

			while ((*cursor == '_' || (*cursor >= 'a' && *cursor <= 'z') || (*cursor >= 'A' && *cursor <= 'Z') ||
				(*cursor >= '0' && *cursor <= '9')) && length < (short)sizeof(key) - 1)
			{
				key[length++] = *cursor++;
			}
			key[length] = 0;
			cursor = royale_config_skip_space(cursor);
			if (!length || *cursor != '=')
				goto fail;
			cursor = royale_config_value_parse(cursor + 1, &value);
			if (!cursor)
				goto fail;
			cursor = royale_config_skip_space(cursor);
			if (*cursor && *cursor != '\n' && *cursor != '#')
				goto fail;
			if (section != _section_unknown && !royale_config_set(config, section, key, &value))
				goto fail;
		}
	}
	if (!royale_config_valid(config))
	{
		platform_log("royale: map config: a value out of range");
		csmemset(config, 0, sizeof(*config));
		return FALSE;
	}
	return TRUE;

fail:
	platform_log("royale: map config: line %ld does not parse", line_number);
	csmemset(config, 0, sizeof(*config));
	return FALSE;
}

boolean royale_map_config_load(
	char const *map_name,
	struct royale_map_config *config)
{
	/* (a test's file: royale_test_setting) */
	char const *path = royale_test_setting("HALO_ROYALE_MAP_CONFIG");
	short index;

	csmemset(config, 0, sizeof(*config));
	if (path && *path)
	{
		FILE *file = fopen(path, "rb");

		/* (a path of the machine's the game may not see as it is: the file's
		name in the data folder, the game's own) */
		if (!file)
		{
			char const *name = strrchr(path, '/');

			file = fopen(name ? name + 1 : path, "rb");
		}

		if (file)
		{
			long size;
			char *text;
			boolean parsed = FALSE;

			fseek(file, 0, SEEK_END);
			size = ftell(file);
			fseek(file, 0, SEEK_SET);
			text = size > 0 && size < 4 * 1024 * 1024 ? (char *)malloc((size_t)size + 1) : NULL;
			if (text && fread(text, 1, (size_t)size, file) == (size_t)size)
			{
				text[size] = 0;
				parsed = royale_map_config_parse(text, config);
			}
			free(text);
			fclose(file);
			platform_log("royale: map config %s %s", path, parsed ? "read" : "refused");
			if (parsed)
				return TRUE;
		}
		else
			platform_log("royale: map config %s not found", path);
	}
	for (index = 0; index < (short)NUMBEROF(royale_maps); index++)
	{
		if (royale_maps[index].name && !strcmp(royale_maps[index].name, map_name))
			return royale_map_config_parse(royale_maps[index].text, config);
	}
	return FALSE;
}

unsigned long royale_map_config_hash(
	struct royale_map_config const *config)
{
	byte const *bytes = (byte const *)config;
	unsigned long hash = 2166136261UL;
	unsigned long index;

	/* (FNV-1a) */
	for (index = 0; index < (unsigned long)sizeof(*config); index++)
		hash = ((hash ^ bytes[index]) * 16777619UL) & 0xFFFFFFFFUL;
	return hash;
}
