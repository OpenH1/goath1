/*
HEADSHOT_INDICATOR.C

The headshot indicator (display.headshot_indicator, each player's own): a red
dot in the middle of the stock pistol's or sniper rifle's reticle when the
shot would hit the head of the aim assist's target (one in its autoaim or
magnetism range), red reticle or not. Later games draw that dot; the stock game does not, so the
setting is off by default, and off nothing is drawn differently.

The test is the game's own. The shot's ray is the one the weapon fires
before its spread (weapon_get_projectile_ray: bent by its autoaim as far as
the target's autoaim level, full in the red reticle's range and less beyond
it; magnetism only slows the stick and bends nothing), tested as a projectile
is (collision_test_vector), and the dot is drawn when what it hits is the aim
assist's target, at a material of its collision model's that takes a
headshot (damage.c's _damage_material_head_bit). Everything it reads is the
map's tags and the objects' positions, which each machine has: the dot is
drawn, nothing is sent or changed, and the game plays the same. A network
game's host may forbid it (network.allow_headshot_indicator), as it may hit
sounds: an OpenH1 game's record carries that to its OpenH1 clients; a Legacy
host cannot say, and its clients show it if their own setting is on.

Only the stock weapons and their stock reticles: a map's own (a custom map's
weapon, a renamed or changed tag, another reticle, a shot that cannot take a
head) is left alone. As the map loads, the weapons' tags of the stock names
are looked at: what the indicator depends on in each (the weapon's aim, its
first trigger's shot and damage, its hud's reticle and the reticle's
bitmaps) is hashed, and the hash must be one of the stock maps'. A Custom
Edition map's bitmaps must be the stock ones of its resource maps too.

Only with the high-res HUD (display.high_res_hud): the dot is a part of the
reticle's texture, as the sniper rifle's own is. Its middle item is drawn
from a placeholder of its bitmap for which the texture cache draws a copy of
the high-res texture (tools/hud_assets.py's): while the shot would be a
headshot, both reticles' copy with the dot (the same dot on both, the
sniper rifle's own taken out); otherwise the pistol's own texture and the
sniper rifle's copy without its dot. Off, or with the stock-resolution HUD,
the reticles are drawn as they are.
*/

#include "cseries.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "interface/hud_definitions.h"
#include "interface/hud_draw.h"
#include "interface/weapon_hud_interface_definition.h"
#include "items/projectile_definitions.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "objects/damage_effect_definitions.h"
#include "objects/damage_resistances.h"
#include "objects/objects.h"
#include "physics/collision_model_definitions.h"
#include "physics/collisions.h"
#include "rasterizer/xbox/rasterizer_xbox_hardware_bitmaps.h"
#include "tag_files/tag_files.h"
#include "tag_files/tag_groups.h"

#include "headshot_indicator.h"
#include "network_standard.h"
#include "player_settings.h"

#include <string.h>

/* the platform layer's (port/linux/src) */
void platform_log(char const *format, ...);
unsigned long config_changes(void);
/* (hud_hires.c) */
int hud_hires_enabled(void);
long hud_hires_variant_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size, int variant);
void hud_hires_register_placeholder(long asset, unsigned long const *texture);
/* (custom_edition_cache.c) */
boolean custom_edition_cache_stock_tag(long tag_index);
boolean custom_edition_cache_tags_loaded(void);

/* ---------- constants */

enum
{
	WEAPON_HUD_INTERFACE_TAG = 'wphi',
	MAXIMUM_WEAPON_HUD_DEPTH = 16,
	/* hud_weapon.c's */
	_crosshair_state_aim = 0,
	_hud_crosshair_not_on_default_zoom_bit = 2,
	_hud_crosshair_hide_outside_area_bit = 4,
	_hud_crosshair_only_on_default_zoom_bit = 6,
	/* bitmaps.c's */
	_bitmap_allocated_bit = 6,
	/* damage.c's */
	_damage_material_head_bit = 0,
	_damage_can_cause_headshots_bit = 1,
	/* hud_hires.h's */
	HUD_HIRES_VARIANT_DOTLESS = 1,
	HUD_HIRES_VARIANT_DOT = 2,
	/* the reticle's middle item: as it is, and while the shot would be a
	headshot */
	_headshot_reticle_normal = 0,
	_headshot_reticle_on_head,
	NUMBER_OF_HEADSHOT_RETICLES
};

/* the shot's ray, as passenger_camera.c's: weapon_get_projectile_ray's
direction this far */
#define HEADSHOT_RAY_LENGTH 128.0f
enum
{
	_headshot_weapon_pistol,
	_headshot_weapon_sniper_rifle,
	NUMBER_OF_HEADSHOT_WEAPONS
};

/* hud_weapon.c's (as tag_schema_effects.c's) */
struct weapon_hud_crosshair_definition
{
	struct tag_reference bitmap;
	struct tag_block items;
};

struct weapon_hud_crosshairs_element
{
	short crosshair_type;
	short runtime_flags;
	short use_on_map_type;
	short pad;
	long unused[7];
	struct weapon_hud_crosshair_definition crosshairs;
	long unused2[10];
};

struct weapon_hud_crosshair_item
{
	struct hud_placement_definition placement;
	struct hud_color_definition colors;
	short frame_rate;
	short sequence_index;
	unsigned long flags;
	long unused[8];
};

typedef char headshot_weapon_hud_crosshairs_element_size_assert[
	sizeof(struct weapon_hud_crosshairs_element) == 0x68 ? 1 : -1];
typedef char headshot_weapon_hud_crosshair_item_size_assert[
	sizeof(struct weapon_hud_crosshair_item) == 0x6C ? 1 : -1];

/* ---------- globals */

/* the stock weapons' fingerprints (weapon_hash), the same on every map of
the Xbox's campaign and multiplayer that has them */
static unsigned long const pistol_stock_hashes[] = { 0x6F54BC02UL };
static unsigned long const sniper_rifle_stock_hashes[] = { 0x2AAF41C4UL };

static struct
{
	char const *tag;
	/* the hash of each stock map's (the campaign's and multiplayer's) */
	unsigned long const *stock_hashes;
	short stock_hash_count;
	/* the high-res texture's copy its reticle's middle item is drawn from
	when the shot would not be a headshot: none (its own texture), or one
	without its own dot (hud_hires.h's HUD_HIRES_VARIANT_*) */
	int normal_variant;
} const headshot_weapons[NUMBER_OF_HEADSHOT_WEAPONS] =
{
	{ "weapons\\pistol\\pistol", pistol_stock_hashes, NUMBEROF(pistol_stock_hashes), 0 },
	{ "weapons\\sniper rifle\\sniper rifle", sniper_rifle_stock_hashes, NUMBEROF(sniper_rifle_stock_hashes), HUD_HIRES_VARIANT_DOTLESS },
};

/* this map's: each weapon's, if it is the stock one, and its reticle's
middle item (its hud's aim element's item at the middle), drawn with the
dot */
static struct
{
	long definition_index;
	boolean stock;
	long middle_hud_index;
	short middle_crosshair_index;
	short middle_item_index;
} headshot_map_weapons[NUMBER_OF_HEADSHOT_WEAPONS];

/* the placeholders the middle items are drawn from (each a blank bitmap of the
size of the map's bitmap, for which the texture cache draws the high-res
texture's copy): the map's bitmap each is for (its pixels at that address),
the copy's asset, and the settings it was made under */
static struct
{
	struct bitmap_data const *bitmap;
	void *address;
	unsigned long made_at;
	struct bitmap_data *placeholder;
	long asset;
	boolean failed;
} headshot_placeholders[NUMBER_OF_HEADSHOT_WEAPONS][NUMBER_OF_HEADSHOT_RETICLES];

/* ---------- private code */

/* FNV-1a */
static unsigned long hash_bytes(unsigned long hash, void const *data, long size)
{
	unsigned char const *bytes = (unsigned char const *)data;
	long index;

	for (index = 0; index < size; index++)
	{
		hash ^= bytes[index];
		hash *= 16777619UL;
		hash &= 0xFFFFFFFFUL;
	}
	return hash;
}

/* a tag's name, or nothing when there is none */
static unsigned long hash_tag(unsigned long hash, long tag_index)
{
	char const *name = tag_index == NONE ? NULL : tag_get_name(tag_index);

	return name ? hash_bytes(hash, name, (long)strlen(name) + 1) : hash_bytes(hash, "", 1);
}

/* the bitmaps the reticle draws: their sizes and formats, and the sprites of
its sequences */
static unsigned long hash_bitmap_group(unsigned long hash, long group_index)
{
	struct bitmap_group *group;
	long index;

	hash = hash_tag(hash, group_index);
	if (group_index == NONE)
		return hash;
	group = bitmap_group_get(group_index);
	hash = hash_bytes(hash, &group->type, sizeof(group->type));
	hash = hash_bytes(hash, &group->bitmaps.count, sizeof(group->bitmaps.count));
	for (index = 0; index < group->bitmaps.count; index++)
	{
		struct bitmap_data *bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, index, struct bitmap_data);

		hash = hash_bytes(hash, &bitmap->width, sizeof(bitmap->width));
		hash = hash_bytes(hash, &bitmap->height, sizeof(bitmap->height));
		hash = hash_bytes(hash, &bitmap->format, sizeof(bitmap->format));
		hash = hash_bytes(hash, &bitmap->registration_point, sizeof(bitmap->registration_point));
		hash = hash_bytes(hash, &bitmap->pixels_size, sizeof(bitmap->pixels_size));
	}
	hash = hash_bytes(hash, &group->sequences.count, sizeof(group->sequences.count));
	for (index = 0; index < group->sequences.count; index++)
	{
		struct bitmap_group_sequence *sequence = TAG_BLOCK_GET_ELEMENT(&group->sequences, index, struct bitmap_group_sequence);
		long sprite;

		hash = hash_bytes(hash, &sequence->sprites.count, sizeof(sequence->sprites.count));
		for (sprite = 0; sprite < sequence->sprites.count; sprite++)
		{
			struct bitmap_group_sprite *entry = TAG_BLOCK_GET_ELEMENT(&sequence->sprites, sprite, struct bitmap_group_sprite);

			hash = hash_bytes(hash, &entry->bitmap_index, sizeof(entry->bitmap_index));
			hash = hash_bytes(hash, &entry->bounds, sizeof(entry->bounds));
			hash = hash_bytes(hash, &entry->registration_point, sizeof(entry->registration_point));
		}
	}
	return hash;
}

/* whether a weapon hud's reticle is all of the map's own resources' (a
Custom Edition map's bitmaps from its stock resource maps): TRUE for the
other maps, whose bitmaps are their own */
static boolean crosshair_bitmaps_stock(long hud_index)
{
	short depth;

	if (!custom_edition_cache_tags_loaded())
		return TRUE;
	for (depth = 0; hud_index != NONE && depth < MAXIMUM_WEAPON_HUD_DEPTH; depth++)
	{
		struct weapon_hud_interface_definition *hud =
			(struct weapon_hud_interface_definition *)tag_get(WEAPON_HUD_INTERFACE_TAG, hud_index);
		long index;

		for (index = 0; index < hud->crosshairs.count; index++)
		{
			struct weapon_hud_crosshairs_element *element = TAG_BLOCK_GET_ELEMENT(
				&hud->crosshairs, index, struct weapon_hud_crosshairs_element);

			if (element->crosshairs.bitmap.index != NONE &&
				!custom_edition_cache_stock_tag(element->crosshairs.bitmap.index))
				return FALSE;
		}
		hud_index = hud->parent_hud.index;
	}
	return TRUE;
}

/* what the indicator depends on in a weapon's tags: its aim and zoom, its
first trigger's shot (the projectile, its error, its damage's headshots),
its hud's reticles, their items and bitmaps */
static unsigned long weapon_hash(long definition_index)
{
	struct weapon_definition *definition = weapon_definition_get(definition_index);
	struct _weapon_definition *weapon = &definition->weapon;
	unsigned long hash = 2166136261UL;
	long hud_index = weapon->interface_definition.hud_interface.index;
	short depth;

	hash = hash_bytes(hash, &weapon->flags, sizeof(weapon->flags));
	hash = hash_bytes(hash, &weapon->zoom_level_count, sizeof(weapon->zoom_level_count));
	hash = hash_bytes(hash, &weapon->zoom_magnification_minimum, sizeof(weapon->zoom_magnification_minimum));
	hash = hash_bytes(hash, &weapon->zoom_magnification_maximum, sizeof(weapon->zoom_magnification_maximum));
	hash = hash_bytes(hash, &weapon->aim_assist_parameters, sizeof(weapon->aim_assist_parameters));
	hash = hash_bytes(hash, &weapon->triggers.count, sizeof(weapon->triggers.count));
	if (weapon->triggers.count > 0)
	{
		struct weapon_trigger_definition *trigger = TAG_BLOCK_GET_ELEMENT(
			&weapon->triggers, 0, struct weapon_trigger_definition);
		long projectile_index = trigger->projectile.index;

		hash = hash_bytes(hash, &trigger->flags, sizeof(trigger->flags));
		hash = hash_bytes(hash, &trigger->projectiles_per_shot, sizeof(trigger->projectiles_per_shot));
		hash = hash_bytes(hash, &trigger->initial_error, sizeof(trigger->initial_error));
		hash = hash_bytes(hash, &trigger->final_error, sizeof(trigger->final_error));
		hash = hash_bytes(hash, &trigger->first_person_weapon_offset, sizeof(trigger->first_person_weapon_offset));
		hash = hash_tag(hash, projectile_index);
		if (projectile_index != NONE)
		{
			long damage_index = projectile_definition_get(projectile_index)->projectile.impact_damage.index;

			hash = hash_tag(hash, damage_index);
			if (damage_index != NONE)
			{
				struct damage_definition *damage = &damage_effect_definition_get(damage_index)->damage;

				hash = hash_bytes(hash, &damage->category, sizeof(damage->category));
				hash = hash_bytes(hash, &damage->flags, sizeof(damage->flags));
			}
		}
	}
	for (depth = 0; depth < MAXIMUM_WEAPON_HUD_DEPTH; depth++)
	{
		struct weapon_hud_interface_definition *hud;
		long index;

		hash = hash_tag(hash, hud_index);
		if (hud_index == NONE)
			break;
		hud = (struct weapon_hud_interface_definition *)tag_get(WEAPON_HUD_INTERFACE_TAG, hud_index);
		hash = hash_bytes(hash, &hud->crosshairs.count, sizeof(hud->crosshairs.count));
		for (index = 0; index < hud->crosshairs.count; index++)
		{
			struct weapon_hud_crosshairs_element *element = TAG_BLOCK_GET_ELEMENT(
				&hud->crosshairs, index, struct weapon_hud_crosshairs_element);
			long item_index;

			hash = hash_bytes(hash, &element->crosshair_type, sizeof(element->crosshair_type));
			hash = hash_bytes(hash, &element->use_on_map_type, sizeof(element->use_on_map_type));
			hash = hash_bitmap_group(hash, element->crosshairs.bitmap.index);
			hash = hash_bytes(hash, &element->crosshairs.items.count, sizeof(element->crosshairs.items.count));
			for (item_index = 0; item_index < element->crosshairs.items.count; item_index++)
			{
				struct weapon_hud_crosshair_item *item = TAG_BLOCK_GET_ELEMENT(
					&element->crosshairs.items, item_index, struct weapon_hud_crosshair_item);

				hash = hash_bytes(hash, &item->placement.offset, sizeof(item->placement.offset));
				hash = hash_bytes(hash, &item->placement.scale, sizeof(item->placement.scale));
				hash = hash_bytes(hash, &item->placement.multiplayer_scaling_flags,
					sizeof(item->placement.multiplayer_scaling_flags));
				hash = hash_bytes(hash, &item->sequence_index, sizeof(item->sequence_index));
				hash = hash_bytes(hash, &item->flags, sizeof(item->flags));
			}
		}
		hud_index = hud->parent_hud.index;
	}
	return hash;
}

/* the weapon's reticle's middle item: in its own hud, the first aim element
drawn on every map type with an item at the middle drawn at every zoom */
static boolean find_middle_item(long definition_index, long *hud_index, short *crosshair_index,
	short *item_index)
{
	long index = weapon_definition_get(definition_index)->weapon.interface_definition.hud_interface.index;
	struct weapon_hud_interface_definition *hud;
	long element_index;

	if (index == NONE)
		return FALSE;
	hud = (struct weapon_hud_interface_definition *)tag_get(WEAPON_HUD_INTERFACE_TAG, index);
	for (element_index = 0; element_index < hud->crosshairs.count; element_index++)
	{
		struct weapon_hud_crosshairs_element *element = TAG_BLOCK_GET_ELEMENT(
			&hud->crosshairs, element_index, struct weapon_hud_crosshairs_element);
		long item;

		if (element->crosshair_type != _crosshair_state_aim || element->use_on_map_type != 0)
			continue;
		for (item = 0; item < element->crosshairs.items.count; item++)
		{
			struct weapon_hud_crosshair_item *entry = TAG_BLOCK_GET_ELEMENT(
				&element->crosshairs.items, item, struct weapon_hud_crosshair_item);

			if (entry->placement.offset.x == 0 && entry->placement.offset.y == 0 &&
				!TEST_FLAG(entry->flags, _hud_crosshair_not_on_default_zoom_bit) &&
				!TEST_FLAG(entry->flags, _hud_crosshair_hide_outside_area_bit) &&
				!TEST_FLAG(entry->flags, _hud_crosshair_only_on_default_zoom_bit))
			{
				*hud_index = index;
				*crosshair_index = (short)element_index;
				*item_index = (short)item;
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* each player's setting, read again when the settings change */
static boolean headshot_indicator_enabled(short local_player_index)
{
	static struct
	{
		boolean initialized;
		unsigned long read_at;
		boolean enabled;
	} players[PLAYER_SETTINGS_SLOTS];
	short slot = PLAYER_SETTINGS_SLOT(local_player_index);
	unsigned long changes = config_changes();

	if (!players[slot].initialized || players[slot].read_at != changes)
	{
		players[slot].enabled = config_player_boolean(local_player_index, "display.headshot_indicator") ? TRUE : FALSE;
		players[slot].initialized = TRUE;
		players[slot].read_at = changes;
	}
	return players[slot].enabled;
}

/* which of the stock weapons the player's is, if it is one of this map's
stock ones, with the setting and the high-res HUD on and a network game's
host allowing it (network.allow_headshot_indicator): NONE otherwise */
static short headshot_weapon_of(short local_player_index, long weapon_index)
{
	long definition_index;
	short index;

	if (weapon_index == NONE || !headshot_indicator_enabled(local_player_index) ||
		!hud_hires_enabled() || !network_standard_headshot_indicator_allowed())
	{
		return NONE;
	}
	definition_index = weapon_get(weapon_index)->definition_index;
	for (index = 0; index < NUMBER_OF_HEADSHOT_WEAPONS; index++)
	{
		if (headshot_map_weapons[index].stock &&
			headshot_map_weapons[index].definition_index == definition_index)
		{
			return index;
		}
	}
	return NONE;
}

/* whether the weapon's shot, as it would be fired now, lands on its
target's head */
static boolean headshot_shot_on_head(short local_player_index, long weapon_index)
{
	long target_index = player_control_get_target_object_index(local_player_index);
	long owner_index = player_control_get_unit_index(local_player_index);
	real_point3d origin;
	real_vector3d direction;
	real_vector3d ray;
	struct collision_result collision;
	struct object_datum *target;
	long collision_model_index;
	struct collision_model *collision_model;
	struct damage_resistance_material *material;

	if (target_index == NONE || owner_index == NONE ||
		!weapon_get_projectile_ray(weapon_index, 0, &origin, &direction))
	{
		return FALSE;
	}
	scale_vector3d(&direction, HEADSHOT_RAY_LENGTH, &ray);
	if (!collision_test_vector(_collision_test_for_projectiles_flags, &origin, &ray, owner_index, &collision) ||
		collision.type != _collision_result_object ||
		collision.object_index != target_index)
	{
		return FALSE;
	}
	target = object_get(target_index);
	collision_model_index = object_definition_get(target->definition_index)->object.collision_model.index;
	if (collision_model_index == NONE)
		return FALSE;
	collision_model = collision_model_definition_get(collision_model_index);
	if (collision.material_index < 0 || collision.material_index >= collision_model->resistance.materials.count)
		return FALSE;
	material = TAG_BLOCK_GET_ELEMENT(&collision_model->resistance.materials, collision.material_index,
		struct damage_resistance_material);
	return TEST_FLAG(material->flags, _damage_material_head_bit) ? TRUE : FALSE;
}

static void headshot_placeholder_forget(short index, short reticle)
{
	if (headshot_placeholders[index][reticle].placeholder)
	{
		hud_hires_register_placeholder(headshot_placeholders[index][reticle].asset, NULL);
		bitmap_delete(headshot_placeholders[index][reticle].placeholder);
	}
	csmemset(&headshot_placeholders[index][reticle], 0, sizeof(headshot_placeholders[index][reticle]));
	headshot_placeholders[index][reticle].asset = NONE;
}

static void headshot_placeholders_forget(void)
{
	short index;
	short reticle;

	for (index = 0; index < NUMBER_OF_HEADSHOT_WEAPONS; index++)
	{
		for (reticle = 0; reticle < NUMBER_OF_HEADSHOT_RETICLES; reticle++)
			headshot_placeholder_forget(index, reticle);
	}
}

/* the placeholder a weapon's middle item is drawn from, for which the
texture cache draws a variant of the high-res texture of the map's bitmap:
made once its pixels are loaded and the texture is found for them, again
when they move or the settings change; NULL when there is none */
static struct bitmap_data const *headshot_placeholder(short index, short reticle,
	struct bitmap_data const *bitmap, int variant)
{
	if (headshot_placeholders[index][reticle].bitmap != bitmap ||
		headshot_placeholders[index][reticle].address != bitmap->base_address ||
		headshot_placeholders[index][reticle].made_at != config_changes())
	{
		headshot_placeholder_forget(index, reticle);
		headshot_placeholders[index][reticle].bitmap = bitmap;
		headshot_placeholders[index][reticle].address = bitmap->base_address;
		headshot_placeholders[index][reticle].made_at = config_changes();
	}
	if (!headshot_placeholders[index][reticle].placeholder && !headshot_placeholders[index][reticle].failed)
	{
		struct bitmap_data *placeholder = NULL;
		long asset;

		/* (its pixels loaded: the texture cache's, as hud_hires_tags.c's) */
		if (bitmap->cache_block_index == NONE || !bitmap->base_address)
			return NULL;
		asset = hud_hires_variant_find((unsigned long)bitmap->base_address, bitmap->width, bitmap->height,
			bitmap_mipmap_get_pixel_data_size((struct bitmap_data *)bitmap, 0), variant);
		if (asset != NONE)
			placeholder = bitmap_2d_new(bitmap->width, bitmap->height, 0, bitmap->format);
		if (placeholder && !rasterizer_bitmap_new(placeholder))
		{
			bitmap_delete(placeholder);
			placeholder = NULL;
		}
		if (placeholder)
		{
			hud_hires_register_placeholder(asset, (unsigned long const *)placeholder->hardware_format);
			headshot_placeholders[index][reticle].asset = asset;
		}
		else
		{
			platform_log("headshot indicator: no high-res reticle for %s: drawn as it is", headshot_weapons[index].tag);
			headshot_placeholders[index][reticle].failed = TRUE;
		}
		headshot_placeholders[index][reticle].placeholder = placeholder;
	}
	return headshot_placeholders[index][reticle].placeholder;
}

/* ---------- public code */

void headshot_indicator_tags_loaded(void)
{
	short index;

	csmemset(headshot_map_weapons, 0, sizeof(headshot_map_weapons));
	for (index = 0; index < NUMBER_OF_HEADSHOT_WEAPONS; index++)
	{
		long definition_index = tag_loaded(WEAPON_DEFINITION_TAG, headshot_weapons[index].tag);
		unsigned long hash;
		boolean stock = FALSE;
		short stock_index;

		headshot_map_weapons[index].definition_index = definition_index;
		headshot_map_weapons[index].middle_hud_index = NONE;
		if (definition_index == NONE)
			continue;
		hash = weapon_hash(definition_index);
		for (stock_index = 0; stock_index < headshot_weapons[index].stock_hash_count; stock_index++)
		{
			if (headshot_weapons[index].stock_hashes[stock_index] == hash)
				stock = TRUE;
		}
		if (stock && !crosshair_bitmaps_stock(weapon_definition_get(definition_index)->weapon.interface_definition.hud_interface.index))
			stock = FALSE;
		if (stock && !find_middle_item(definition_index, &headshot_map_weapons[index].middle_hud_index,
			&headshot_map_weapons[index].middle_crosshair_index, &headshot_map_weapons[index].middle_item_index))
		{
			stock = FALSE;
		}
		platform_log("headshot indicator: %s %08lx (%s)", headshot_weapons[index].tag, hash,
			stock ? "stock" : "not stock: no indicator");
		headshot_map_weapons[index].stock = stock;
	}
}

void headshot_indicator_tags_unloaded(void)
{
	csmemset(headshot_map_weapons, 0, sizeof(headshot_map_weapons));
	headshot_placeholders_forget();
}

struct bitmap_data const *headshot_indicator_item_bitmap(short local_player_index, long weapon_index,
	long hud_index, short crosshair_index, short item_index, struct bitmap_data const *bitmap)
{
	short index = headshot_weapon_of(local_player_index, weapon_index);
	short reticle;
	int variant;
	struct bitmap_data const *placeholder;

	/* (the map's bitmap: not one of the high-res HUD's placeholders) */
	if (index == NONE ||
		TEST_FLAG(bitmap->flags, _bitmap_allocated_bit) ||
		headshot_map_weapons[index].middle_hud_index != hud_index ||
		headshot_map_weapons[index].middle_crosshair_index != crosshair_index ||
		headshot_map_weapons[index].middle_item_index != item_index)
	{
		return bitmap;
	}
	/* (any autoaim level: the shot is bent as far as the target's level, so a
	head in the falloff beyond the red reticle's range can still be hit) */
	reticle = headshot_shot_on_head(local_player_index, weapon_index) ?
		_headshot_reticle_on_head : _headshot_reticle_normal;
	variant = reticle == _headshot_reticle_on_head ? HUD_HIRES_VARIANT_DOT : headshot_weapons[index].normal_variant;
	if (!variant)
		return bitmap;
	placeholder = headshot_placeholder(index, reticle, bitmap, variant);
	return placeholder ? placeholder : bitmap;
}
