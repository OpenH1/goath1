/*
MOD_SHADERS.C

The shaders of mods (mods.c, docs/mods.md "Shaders"): each of a map's model
and environment shaders (shader_model, shader_environment) that a mod has a
tag file of the same group of (tags\...\name.shader_model,
tags\...\name.shader_environment) is drawn with the file's drawing values,
and mods_reload reads them again.

Only what is drawn is the file's: its maps, their scales, its colors, its
flags and its animation functions (a whitelist of fields, below). The map's
keep the rest: the shader's header (its type, its radiosity properties, its
physics flags and material type, which impacts, footsteps and the
collision materials read), the environment's type (whether its BSP's
surfaces are blended), what the tool works out of the values as it builds a
map (but the bump map's runtime scale, which follows the bump map scale),
and every field neither the game nor the tool reads. Nothing a network
game's machines agree on reads a drawing value.

A tag file is Halo PC's (big-endian, version 2, as the Halo Editing Kit and
Invader write them): the shader, then the names of its tag references in
order, each its name_length bytes and a terminator; it must end where its
last name does. Every value is checked: an enum past the values the
renderer indexes by is refused, as is a number that is not finite, a name
not terminated where its length says, or a file of another group than the
map's tag. Every reference must name a tag the map has, of its group: a mod
cannot add tags. A bitmap it names may be any mod's, the built-in HD
effects' or the MCC import's (mod_bitmaps.c plays those over the map's
bitmap tag of the name, which the shader holds by its index), so a mod's
shader and a mod's bitmap together redraw a model. A reference that names
another bitmap than the map's must name one of the type the renderer
samples it as (a cube map for reflections, a 2D bitmap for the rest), with
pages. A scale of 0 is 1, as the tool makes it.

The map's shader tag takes the made definition in place, so every model
part and surface that holds the tag keeps holding it, and its own comes back
when the file goes. A made definition is kept until the map goes, as a frame
may still draw from it.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "main/console.h"
#include "tag_files/tag_groups.h"
#include "../src/port_mods.h" /* port: port/linux/src/port_mods.c */
#include "mods.h"

#include <stdlib.h>
#include <string.h>

/* the game's (port) */
char *tag_get_name(long tag_index);
unsigned long tag_get_group_tag(long tag_index);

/* ---------- constants */

#define SHADER_TAG_VERSION 2
#define SHADER_MODEL_GROUP_TAG 'soso'
#define SHADER_ENVIRONMENT_GROUP_TAG 'senv'
#define LENS_FLARE_GROUP_TAG 'lens'

/* the definitions' sizes (tag_schema_render.c's structures) */
#define SHADER_MODEL_SIZE 0x1B8
#define SHADER_ENVIRONMENT_SIZE 0x344
#define MAXIMUM_SHADER_SIZE SHADER_ENVIRONMENT_SIZE
/* a tag reference: group, name, name_length, index */
#define TAG_REFERENCE_SIZE 0x10
#define TAG_REFERENCE_NAME_LENGTH_OFFSET 0x08
/* the longest tag name (tag_groups.h's) */
#define MAXIMUM_TAG_NAME_LENGTH 255

/* the values the renderer indexes its tables by (tag_schema_render.c's
enums, periodic_functions.c's functions) */
#define NUMBER_OF_PERIODIC_FUNCTIONS 12
#define NUMBER_OF_FUNCTION_SOURCES 5
#define NUMBER_OF_DETAIL_FUNCTIONS 3
#define NUMBER_OF_MODEL_DETAIL_MASKS 9
#define NUMBER_OF_REFLECTION_TYPES 3

/* the environment's bump map, its scale and its runtime scale (the tool's) */
#define ENVIRONMENT_BUMP_MAP_SCALE_OFFSET 0x124
#define ENVIRONMENT_BUMP_MAP_OFFSET 0x128
#define ENVIRONMENT_RUNTIME_BUMP_MAP_SCALE_OFFSET 0x138

/* bitmaps.c's bitmap types */
enum
{
	_bitmap_type_2d,
	_bitmap_type_3d,
	_bitmap_type_cube_map,
};

/* what a field is */
enum
{
	/* a 16-bit word of flags */
	_field_flags,
	/* a 16-bit enum of `maximum` values */
	_field_enum,
	/* 32-bit reals, `count` of them */
	_field_real,
	/* a real scale: 0 is 1 */
	_field_scale,
	/* a tag reference of a 2D bitmap, a cube map, or a lens flare */
	_field_bitmap,
	_field_cube_map,
	_field_lens_flare,
};

/* ---------- structures */

struct shader_field
{
	short type;
	short offset;
	short count;
};

struct shader_group
{
	unsigned long group_tag;
	char const *extension;
	long size;
	struct shader_field const *fields;
};

/* a shader tag file read, and what it makes */
struct mod_shader
{
	struct mod_shader *next;
	long tag_index;
	char *path;
	unsigned long file_size;
	unsigned long stamp[2];
	long size;
	byte definition[MAXIMUM_SHADER_SIZE];
};

/* a shader of the map drawn from a file: the map's own definition */
struct mod_shader_override
{
	struct mod_shader *shader;
	byte map_definition[MAXIMUM_SHADER_SIZE];
};

/* ---------- globals */

#define ENUM(offset, maximum) { _field_enum, (offset), (maximum) }
#define FLAGS(offset) { _field_flags, (offset), 1 }
#define REALS(offset, count) { _field_real, (offset), (count) }
#define SCALE(offset) { _field_scale, (offset), 1 }
#define BITMAP(offset) { _field_bitmap, (offset), 1 }

/* a texture animation (shader_texture_animation.h) at `offset` */
#define TEXTURE_ANIMATION(offset) \
	ENUM((offset), NUMBER_OF_FUNCTION_SOURCES), ENUM((offset) + 0x02, NUMBER_OF_PERIODIC_FUNCTIONS), \
	REALS((offset) + 0x04, 3), \
	ENUM((offset) + 0x10, NUMBER_OF_FUNCTION_SOURCES), ENUM((offset) + 0x12, NUMBER_OF_PERIODIC_FUNCTIONS), \
	REALS((offset) + 0x14, 3), \
	ENUM((offset) + 0x20, NUMBER_OF_FUNCTION_SOURCES), ENUM((offset) + 0x22, NUMBER_OF_PERIODIC_FUNCTIONS), \
	REALS((offset) + 0x24, 5)

/* (in order of their offsets: the references' names follow the shader in
this order) */
static struct shader_field const shader_model_fields[] =
{
	FLAGS(0x28),
	REALS(0x38, 1), /* translucency */
	ENUM(0x4C, NUMBER_OF_FUNCTION_SOURCES), /* change color source */
	FLAGS(0x6C), /* self-illumination */
	ENUM(0x70, NUMBER_OF_FUNCTION_SOURCES),
	ENUM(0x72, NUMBER_OF_PERIODIC_FUNCTIONS),
	REALS(0x74, 7), /* period, lower and upper bound colors */
	SCALE(0x9C), SCALE(0xA0), /* maps' u and v scales */
	BITMAP(0xA4), /* base map */
	BITMAP(0xBC), /* multipurpose map */
	ENUM(0xD4, NUMBER_OF_DETAIL_FUNCTIONS),
	ENUM(0xD6, NUMBER_OF_MODEL_DETAIL_MASKS),
	SCALE(0xD8),
	BITMAP(0xDC), /* detail map */
	SCALE(0xEC), /* detail map v scale */
	TEXTURE_ANIMATION(0xFC),
	REALS(0x13C, 9), /* reflection falloff and cutoff, brightnesses and tints */
	{ _field_cube_map, 0x164, 1 },
	{ -1, 0, 0 },
};

static struct shader_field const shader_environment_fields[] =
{
	FLAGS(0x28),
	REALS(0x2C, 1), /* lens flare spacing */
	{ _field_lens_flare, 0x30, 1 },
	FLAGS(0x6C), /* diffuse */
	BITMAP(0x88), /* base map */
	ENUM(0xB0, NUMBER_OF_DETAIL_FUNCTIONS),
	SCALE(0xB4), BITMAP(0xB8), /* primary detail map */
	SCALE(0xC8), BITMAP(0xCC), /* secondary detail map */
	ENUM(0xF4, NUMBER_OF_DETAIL_FUNCTIONS),
	SCALE(0xF8), BITMAP(0xFC), /* micro detail map */
	REALS(0x10C, 3), /* material color */
	SCALE(ENVIRONMENT_BUMP_MAP_SCALE_OFFSET), BITMAP(ENVIRONMENT_BUMP_MAP_OFFSET),
	ENUM(0x150, NUMBER_OF_PERIODIC_FUNCTIONS), REALS(0x154, 2), /* u animation */
	ENUM(0x15C, NUMBER_OF_PERIODIC_FUNCTIONS), REALS(0x160, 2), /* v animation */
	FLAGS(0x180), /* self-illumination */
	REALS(0x19C, 6), ENUM(0x1B4, NUMBER_OF_PERIODIC_FUNCTIONS), REALS(0x1B8, 2), /* primary */
	REALS(0x1D8, 6), ENUM(0x1F0, NUMBER_OF_PERIODIC_FUNCTIONS), REALS(0x1F4, 2), /* secondary */
	REALS(0x214, 6), ENUM(0x22C, NUMBER_OF_PERIODIC_FUNCTIONS), REALS(0x230, 2), /* plasma */
	SCALE(0x250), BITMAP(0x254), /* self-illumination map */
	FLAGS(0x27C), /* specular */
	REALS(0x294, 1), REALS(0x2AC, 6), /* brightness, colors */
	FLAGS(0x2D0), /* reflection */
	ENUM(0x2D2, NUMBER_OF_REFLECTION_TYPES),
	REALS(0x2D4, 1), REALS(0x2F4, 2), REALS(0x30C, 2),
	{ _field_cube_map, 0x324, 1 },
	{ -1, 0, 0 },
};

static struct shader_group const shader_groups[] =
{
	{ SHADER_MODEL_GROUP_TAG, ".shader_model", SHADER_MODEL_SIZE, shader_model_fields },
	{ SHADER_ENVIRONMENT_GROUP_TAG, ".shader_environment", SHADER_ENVIRONMENT_SIZE, shader_environment_fields },
};

static struct
{
	/* by the map's tag, its override (none: the map's own) */
	struct mod_shader_override **overrides;
	long override_count;
	/* every shader made while the map is loaded */
	struct mod_shader *shaders;
	/* the shaders played, mixed (mod_shaders_identity) */
	unsigned long identity;
} mod_shaders_globals;

/* ---------- private code */

static struct shader_group const *group_get(
	unsigned long group_tag)
{
	long index;

	for (index = 0; index < (long)NUMBEROF(shader_groups); index++)
	{
		if (shader_groups[index].group_tag == group_tag)
			return &shader_groups[index];
	}

	return NULL;
}

static void put_u16(
	byte *definition,
	long offset,
	unsigned long value)
{
	unsigned short word = (unsigned short)value;

	memcpy(definition + offset, &word, sizeof(word));
}

static real get_real(
	byte const *definition,
	long offset)
{
	real value;

	memcpy(&value, definition + offset, sizeof(value));
	return value;
}

static void put_real(
	byte *definition,
	long offset,
	real value)
{
	memcpy(definition + offset, &value, sizeof(value));
}

/* the bitmap tag `tag_index`'s first page's type: FALSE if it has none */
static boolean bitmap_type(
	long tag_index,
	short *type)
{
	struct bitmap_group *group = bitmap_group_get(tag_index);
	struct bitmap_data const *bitmap;

	if (!group || group->bitmaps.count <= 0 || !group->bitmaps.address)
		return FALSE;
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	*type = bitmap->type;
	return TRUE;
}

/* A reference of the file's at `reference`, its name at *name_offset (moved
past it), made the map's tag of its name into the definition at `offset`:
FALSE with *reason saying why not. */
static boolean reference_read(
	byte const *file,
	unsigned long file_size,
	unsigned long *name_offset,
	struct shader_field const *field,
	byte const *map_definition,
	byte *definition,
	char const **reason)
{
	byte const *reference = file + MOD_TAG_FILE_HEADER_SIZE + field->offset;
	unsigned long name_length = mods_read_be32(reference + TAG_REFERENCE_NAME_LENGTH_OFFSET);
	unsigned long group_tag = field->type == _field_lens_flare ? LENS_FLARE_GROUP_TAG : BITMAP_GROUP_TAG;
	struct tag_reference made;
	struct tag_reference map;
	char const *name;

	memcpy(&map, map_definition + field->offset, sizeof(map));
	csmemset(&made, 0, sizeof(made));
	made.group_tag = group_tag;
	made.index = NONE;
	made.name = "";
	if (name_length)
	{
		long tag_index;

		if (name_length > MAXIMUM_TAG_NAME_LENGTH || !mods_in_file(*name_offset, name_length + 1, file_size) ||
			file[*name_offset + name_length] || memchr(file + *name_offset, 0, name_length))
		{
			*reason = "a tag reference's name is not where its length says";
			return FALSE;
		}
		name = (char const *)file + *name_offset;
		*name_offset += name_length + 1;
		if (mods_read_be32(reference) != group_tag)
		{
			*reason = "a tag reference is not of its field's group";
			return FALSE;
		}
		tag_index = tag_loaded((long)group_tag, name);
		if (tag_index == NONE)
		{
			*reason = group_tag == BITMAP_GROUP_TAG ? "it names a bitmap the map has not got (a mod cannot add tags)" :
				"it names a lens flare the map has not got (a mod cannot add tags)";
			return FALSE;
		}
		/* (another bitmap than the map's: one the renderer can sample as this
		field's) */
		if (group_tag == BITMAP_GROUP_TAG && tag_index != map.index)
		{
			short type;

			if (!bitmap_type(tag_index, &type))
			{
				*reason = "it names a bitmap with no pages";
				return FALSE;
			}
			if ((field->type == _field_cube_map) != (type == _bitmap_type_cube_map) ||
				(field->type == _field_bitmap && type != _bitmap_type_2d))
			{
				*reason = field->type == _field_cube_map ? "a reflection map is not a cube map" :
					"a map is not a 2D bitmap";
				return FALSE;
			}
		}
		made.index = tag_index;
		made.name = tag_get_name(tag_index);
		made.name_length = (long)strlen(made.name);
	}
	memcpy(definition + field->offset, &made, sizeof(made));

	return TRUE;
}

/* The map's definition `map_definition` of `group` with the drawing
values of the file (`file_size` bytes at `file`) into `definition`: FALSE
with *reason saying why not. */
static boolean shader_values_read(
	struct shader_group const *group,
	byte const *file,
	unsigned long file_size,
	byte const *map_definition,
	byte *definition,
	char const **reason)
{
	unsigned long name_offset = MOD_TAG_FILE_HEADER_SIZE + (unsigned long)group->size;
	byte const *root = file + MOD_TAG_FILE_HEADER_SIZE;
	struct shader_field const *field;

	if (file_size < name_offset)
	{
		*reason = "it is shorter than its shader";
		return FALSE;
	}
	if (!mods_tag_file_header_valid(file, group->group_tag, SHADER_TAG_VERSION))
	{
		*reason = "it is not a Halo PC shader tag file of the map's shader's group, version 2";
		return FALSE;
	}
	memcpy(definition, map_definition, (size_t)group->size);
	for (field = group->fields; field->type != -1; field++)
	{
		long element;

		switch (field->type)
		{
		case _field_flags:
			put_u16(definition, field->offset, mods_read_be16(root + field->offset));
			break;
		case _field_enum:
		{
			short value = mods_read_short(root + field->offset);

			if (value < 0 || value >= field->count)
			{
				*reason = "an enum is past its values";
				return FALSE;
			}
			put_u16(definition, field->offset, (unsigned long)value);
			break;
		}
		case _field_real:
		case _field_scale:
			for (element = 0; element < field->count; element++)
			{
				real value = mods_read_real(root + field->offset + element * 4);

				if (!mods_finite_real(value))
				{
					*reason = "a number is not finite";
					return FALSE;
				}
				if (field->type == _field_scale && value == 0.f)
					value = 1.f;
				put_real(definition, field->offset + element * 4, value);
			}
			break;
		default:
			if (!reference_read(file, file_size, &name_offset, field, map_definition, definition, reason))
				return FALSE;
			break;
		}
	}
	if (name_offset != file_size)
	{
		*reason = "it does not end where its last tag reference's name does";
		return FALSE;
	}
	/* (the bump map's runtime scale, the tool's, follows its scale: the map's
	in the ratio of the file's scale to the map's) */
	if (group->group_tag == SHADER_ENVIRONMENT_GROUP_TAG)
	{
		real map_scale = get_real(map_definition, ENVIRONMENT_BUMP_MAP_SCALE_OFFSET);
		real scale = get_real(definition, ENVIRONMENT_BUMP_MAP_SCALE_OFFSET);
		long axis;

		for (axis = 0; axis < 2; axis++)
		{
			long offset = ENVIRONMENT_RUNTIME_BUMP_MAP_SCALE_OFFSET + axis * 4;
			real runtime = get_real(map_definition, offset);

			put_real(definition, offset, map_scale != 0.f && runtime != 0.f ? runtime * scale / map_scale : scale);
		}
	}

	return TRUE;
}

static void mod_shader_free(
	struct mod_shader *shader)
{
	if (shader->path)
		free(shader->path);
	free(shader);
}

/* the shader made from the file `file` for the map's tag `tag_index` (whose
own definition is `map_definition`), or NULL with *reason saying why not */
static struct mod_shader *mod_shader_new(
	long tag_index,
	long file,
	struct shader_group const *group,
	byte const *map_definition,
	char const **reason)
{
	struct mod_shader *made;
	unsigned long size;
	byte *bytes = mods_tag_file_read(file, &size, reason);

	if (!bytes)
		return NULL;
	made = malloc(sizeof(*made));
	if (!made)
	{
		*reason = "out of memory";
		free(bytes);
		return NULL;
	}
	csmemset(made, 0, sizeof(*made));
	made->tag_index = tag_index;
	made->size = group->size;
	if (!shader_values_read(group, bytes, size, map_definition, made->definition, reason) ||
		!port_mods_file_stamp(file, &made->file_size, made->stamp) ||
		!(made->path = malloc(strlen(port_mods_file_path(file)) + 1)))
	{
		if (!*reason)
			*reason = "out of memory";
		free(bytes);
		mod_shader_free(made);
		return NULL;
	}
	strcpy(made->path, port_mods_file_path(file));
	free(bytes);

	return made;
}

static struct mod_shader_override **override_slot(
	long tag_index)
{
	long absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index);

	return absolute_index >= 0 && absolute_index < mod_shaders_globals.override_count ?
		&mod_shaders_globals.overrides[absolute_index] : NULL;
}

/* the table of overrides, one slot for each of the map's tags up to its
last shader of a group read here; FALSE when it cannot be made */
static boolean overrides_new(
	void)
{
	long count = 0;
	long group;

	if (mod_shaders_globals.overrides)
		return TRUE;
	for (group = 0; group < (long)NUMBEROF(shader_groups); group++)
	{
		struct tag_iterator iterator;
		long tag_index;

		tag_iterator_new(&iterator, shader_groups[group].group_tag);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
			count = MAX(count, DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) + 1);
	}
	if (count <= 0 || !(mod_shaders_globals.overrides = malloc((size_t)count * sizeof(*mod_shaders_globals.overrides))))
		return FALSE;
	memset(mod_shaders_globals.overrides, 0, (size_t)count * sizeof(*mod_shaders_globals.overrides));
	mod_shaders_globals.override_count = count;

	return TRUE;
}

static struct mod_shader *shader_kept(
	long tag_index,
	char const *path,
	unsigned long size,
	unsigned long const stamp[2])
{
	struct mod_shader *made;

	for (made = mod_shaders_globals.shaders; made; made = made->next)
	{
		if (made->tag_index == tag_index && made->file_size == size && made->stamp[0] == stamp[0] &&
			made->stamp[1] == stamp[1] && !strcmp(made->path, path))
		{
			return made;
		}
	}

	return NULL;
}

/* the shaders played, mixed: the same whatever the order */
static unsigned long identity_of_played(
	void)
{
	unsigned long identity = 0;
	long index;

	for (index = 0; index < mod_shaders_globals.override_count; index++)
	{
		struct mod_shader_override const *override = mod_shaders_globals.overrides[index];
		unsigned long word;
		long byte_index;

		if (!override || !override->shader)
			continue;
		word = (unsigned long)index * 0x9E3779B1UL;
		for (byte_index = 0; byte_index < override->shader->size; byte_index++)
			word = (word ^ override->shader->definition[byte_index]) * 0x01000193UL;
		identity += word & 0xffffffffUL;
	}

	return identity & 0xffffffffUL;
}

/* ---------- public code */

long mod_shaders_update(
	boolean remove_all,
	boolean at_load,
	boolean *changed_any)
{
	long loaded_count = 0;
	long removed_count = 0;
	long refused_count = 0;
	long played_count = 0;
	boolean changed = FALSE;
	long group_index;
	long index;

	*changed_any = FALSE;
	if (!mods_map_loaded() || !overrides_new())
		return 0;
	for (group_index = 0; group_index < (long)NUMBEROF(shader_groups); group_index++)
	{
		struct shader_group const *group = &shader_groups[group_index];
		struct tag_iterator iterator;
		long tag_index;

		tag_iterator_new(&iterator, group->group_tag);
		while ((tag_index = tag_iterator_next(&iterator)) != NONE)
		{
			struct mod_shader_override **slot = override_slot(tag_index);
			byte *definition = tag_get(group->group_tag, tag_index);
			struct mod_shader_override *override;
			struct mod_shader *made = NULL;
			long file;
			char const *reason = NULL;

			/* (a tag of the group read as its own: a Custom Edition map's
			chicago extended shaders are regrouped, never these) */
			if (!slot || !definition || tag_get_group_tag(tag_index) != group->group_tag)
				continue;
			override = *slot;
			file = remove_all ? NONE : mods_tag_file_find(tag_index, group->extension);
			if (file != NONE)
			{
				unsigned long size;
				unsigned long stamp[2];

				if (port_mods_file_stamp(file, &size, stamp))
					made = shader_kept(tag_index, port_mods_file_path(file), size, stamp);
				if (made && override && made == override->shader)
					continue;
			}
			if (file != NONE && !made)
			{
				if (!override && !(override = *slot = malloc(sizeof(**slot))))
					continue;
				if (!(*slot)->shader)
					memcpy(override->map_definition, definition, (size_t)group->size);
				made = mod_shader_new(tag_index, file, group, override->map_definition, &reason);
				if (made && !mods_memory_take(sizeof(*made)))
				{
					reason = "the mods' tags already hold too much memory";
					mod_shader_free(made);
					made = NULL;
				}
				/* (one that changed and now fails a check leaves what was
				played before, as the other groups' files do) */
				if (!made)
				{
					error(_error_silent, "mods: %s left out: %s", port_mods_file_path(file), reason);
					console_warning("mods: the shader %s of %s left out: %s", tag_get_name(tag_index),
						port_mods_file_mod(file), reason);
					refused_count++;
					if (!override->shader)
					{
						free(override);
						*slot = NULL;
					}
					continue;
				}
				made->next = mod_shaders_globals.shaders;
				mod_shaders_globals.shaders = made;
				loaded_count++;
			}
			if (!override || (!made && !override->shader))
				continue;
			if (override->shader)
			{
				memcpy(definition, override->map_definition, (size_t)group->size);
				override->shader = NULL;
				if (!made)
					removed_count++;
			}
			if (made)
			{
				memcpy(override->map_definition, definition, (size_t)group->size);
				memcpy(definition, made->definition, (size_t)group->size);
				override->shader = made;
			}
			changed = TRUE;
		}
	}
	for (index = 0; index < mod_shaders_globals.override_count; index++)
	{
		if (mod_shaders_globals.overrides[index] && mod_shaders_globals.overrides[index]->shader)
			played_count++;
	}
	mod_shaders_globals.identity = identity_of_played();
	*changed_any = changed;
	if (!at_load || loaded_count || refused_count)
	{
		error(_error_silent, "mods: %ld shaders from their files (%ld new), %ld given back, %ld left out",
			played_count, loaded_count, removed_count, refused_count);
		if (!at_load && mods_say())
		{
			console_printf(FALSE, "mods: %ld shaders from their files (%ld new), %ld given back, %ld left out",
				played_count, loaded_count, removed_count, refused_count);
		}
	}

	return played_count;
}

void mod_shaders_tags_unloaded(
	boolean give_back)
{
	struct mod_shader *made = mod_shaders_globals.shaders;
	long index;

	for (index = 0; index < mod_shaders_globals.override_count; index++)
	{
		struct mod_shader_override *override = mod_shaders_globals.overrides[index];

		if (!override)
			continue;
		if (give_back && override->shader)
		{
			long tag_index = override->shader->tag_index;
			struct shader_group const *group = group_get(tag_get_group_tag(tag_index));
			void *definition = group ? tag_get(group->group_tag, tag_index) : NULL;

			if (definition)
				memcpy(definition, override->map_definition, (size_t)group->size);
		}
		free(override);
	}
	while (made)
	{
		struct mod_shader *next = made->next;

		mod_shader_free(made);
		made = next;
	}
	if (mod_shaders_globals.overrides)
		free(mod_shaders_globals.overrides);
	memset(&mod_shaders_globals, 0, sizeof(mod_shaders_globals));

	return;
}

unsigned long mod_shaders_identity(
	void)
{
	return mod_shaders_globals.identity;
}
