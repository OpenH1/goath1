#include "harness.h"

/* the headshot indicator's (port/linux/game/headshot_indicator.c) reticle and
shot reads, and the placeholders its reticles' high-res copies are drawn for,
on a small world */

typedef union { real n[4]; struct { real x0; real x1; real y0; real y1; }; } real_rectangle2d;
typedef struct { real i, j; } real_vector2d;
typedef struct { short x, y; } point2d;
typedef unsigned long pixel32;
struct tag_block { long count; void *address; };
struct tag_reference { long group_tag; long index; };
#define TAG_BLOCK_GET_ELEMENT(block, element_index, type) (((type *)(block)->address) + (element_index))
#define NUMBEROF(array) (sizeof(array) / sizeof(array[0]))
struct hud_placement_definition { point2d offset; real_vector2d scale; short multiplayer_scaling_flags; short pad; };
struct hud_color_definition { pixel32 color; pixel32 flash_color; };
struct bitmap_data;

#define PLAYER_SETTINGS_SLOTS 4
#define PLAYER_SETTINGS_SLOT(local_player_index) ((local_player_index) < 0 ? 0 : (local_player_index) & 3)
static unsigned long change_count = 1;
static boolean setting[PLAYER_SETTINGS_SLOTS];
static int setting_reads __attribute__((unused)); /* (newer clangs warn of a global set and never read) */
#define config_changes() change_count
#define config_player_boolean(local_player_index, name) (setting_reads++, setting[local_player_index])
/* (a network game's host's rule: network_standard.c's) */
static boolean host_allows __attribute__((unused)) = TRUE;
#define network_standard_headshot_indicator_allowed() host_allows

/* the world: one weapon of each definition, one target, its collision model */
struct weapon_datum { long definition_index; };
static struct weapon_datum weapons[4];
#define weapon_get(weapon_index) (&weapons[weapon_index])

struct weapon_hud_interface_definition { struct tag_reference parent_hud; struct tag_block crosshairs; };
static struct weapon_hud_interface_definition huds[2];
#define tag_get(group, index) ((void *)&huds[index])
struct weapon_definition { struct { struct { struct { long index; } hud_interface; } interface_definition; } weapon; };
static struct weapon_definition weapon_definitions[3];
#define weapon_definition_get(definition_index) (&weapon_definitions[definition_index])

enum { _collision_test_for_projectiles_flags = 0x55, _collision_result_none = 0, _collision_result_structure,
	_collision_result_object };
struct collision_result { short type; long object_index; short material_index; };
struct damage_resistance_material { unsigned short flags; };
struct collision_model { struct { struct tag_block materials; } resistance; };
struct object_datum { long definition_index; };
struct object_definition { struct { struct { long index; } collision_model; } object; };
static long target_now = NONE;
static long unit = 7;
static boolean ray_ok = TRUE;
static real_vector3d ray_length;
static struct collision_result hit;
static boolean hit_any;
static struct damage_resistance_material materials[2] = { { 1 }, { 0 } }; /* head, body */
static struct collision_model model = { { { 2, materials } } };
static struct object_datum target_object = { 0 };
static struct object_definition target_definition = { { { 0 } } };
#define player_control_get_target_object_index(local_player_index) target_now
#define player_control_get_unit_index(local_player_index) unit
static real autoaim_level __attribute__((unused)) = 1.0f;
#define player_control_get_autoaim_level(local_player_index) autoaim_level
#define object_get(object_index) ((void)(object_index), &target_object)
#define object_definition_get(definition_index) ((void)(definition_index), &target_definition)
#define collision_model_definition_get(index) ((void)(index), &model)

static boolean weapon_get_projectile_ray(long weapon_index, short trigger, real_point3d *origin,
	real_vector3d *direction)
{
	(void)weapon_index;
	(void)trigger;
	origin->x = origin->y = origin->z = 0.0f;
	direction->i = 1.0f;
	direction->j = direction->k = 0.0f;
	return ray_ok;
}

static void scale_vector3d(real_vector3d const *vector, real scale, real_vector3d *result)
{
	result->i = vector->i * scale;
	result->j = vector->j * scale;
	result->k = vector->k * scale;
}

static boolean collision_test_vector(long flags, real_point3d const *origin, real_vector3d const *vector,
	long ignored, struct collision_result *result)
{
	(void)origin;
	CHECK(flags == _collision_test_for_projectiles_flags, "not tested as a projectile's");
	CHECK(ignored == unit, "the player's own unit not ignored");
	ray_length = *vector;
	*result = hit;
	return hit_any;
}

/* bitmaps.c's: the map's bitmaps, and the placeholders made */
enum { _bitmap_format_a8y8 = 3 };
struct bitmap_data { short format; unsigned short flags; short width; short height;
	long cache_block_index; void *base_address; unsigned long *hardware_format; };
#define csmemset memset
#define platform_log(...) ((void)0)
static long bitmap_mipmap_get_pixel_data_size(struct bitmap_data const *bitmap, short level)
{
	(void)level;
	return (long)bitmap->width * bitmap->height * 2;
}
#define MADE 8
static struct bitmap_data made[MADE];
static boolean made_live[MADE];
/* the asset each placeholder made was registered for (the stub's two weapons
share one map bitmap, so one asset, which the game's two textures are not) */
static long made_asset[MADE];
static unsigned long made_hardware[MADE][2];
static int made_count, deleted_count;
static boolean new_fails, rasterizer_fails;
static struct bitmap_data *bitmap_2d_new(short width, short height, short mipmaps, short format)
{
	struct bitmap_data *bitmap;

	(void)mipmaps;
	if (new_fails)
		return NULL;
	CHECK(made_count < MADE, "too many placeholders made");
	bitmap = &made[made_count];
	made_asset[made_count] = NONE;
	made_live[made_count++] = TRUE;
	memset(bitmap, 0, sizeof(*bitmap));
	bitmap->format = format;
	bitmap->flags = FLAG(6); /* allocated (bitmaps.c) */
	bitmap->width = width;
	bitmap->height = height;
	return bitmap;
}
static boolean rasterizer_bitmap_new(struct bitmap_data *bitmap)
{
	if (rasterizer_fails)
		return FALSE;
	made_hardware[bitmap - made][1] = 0x1000 + (unsigned long)(bitmap - made);
	bitmap->hardware_format = made_hardware[bitmap - made];
	return TRUE;
}
static void bitmap_delete(struct bitmap_data *bitmap)
{
	CHECK(bitmap >= made && bitmap < made + MADE && made_live[bitmap - made], "not a live placeholder deleted");
	made_live[bitmap - made] = FALSE;
	deleted_count++;
}
/* hud_hires.c's: the high-res HUD on, its textures' variants (an asset each,
or none), and the placeholders registered for them */
static boolean hires_on __attribute__((unused)) = TRUE;
#define hud_hires_enabled() hires_on
static struct bitmap_data map;
static long variant_assets[3] = { NONE, 10, 20 }; /* none, without the dot, with it */
static long hud_hires_variant_find(unsigned long address, unsigned long width, unsigned long height,
	unsigned long level0_size, int variant)
{
	CHECK(address == (unsigned long)map.base_address && width == 64 && height == 64 && level0_size == 64 * 64 * 2,
		"not the map's bitmap's texture looked for");
	CHECK(variant == 1 || variant == 2, "variant %d", variant);
	return variant_assets[variant];
}
static unsigned long const *registered[32];
static void hud_hires_register_placeholder(long asset, unsigned long const *texture)
{
	CHECK(asset >= 0 && asset < 32, "asset %ld", asset);
	registered[asset] = texture;
	{
		short index;

		for (index = 0; texture && index < MADE; index++)
		{
			if (made_live[index] && made[index].hardware_format == texture)
				made_asset[index] = asset;
		}
	}
}

#include "under_test.inc"

static struct weapon_hud_crosshairs_element elements[3];
static struct weapon_hud_crosshair_item items[3];

static void world(void)
{
	/* weapon definition 0: the pistol, 1: the sniper rifle, 2: another; weapon i is of definition i */
	weapons[0].definition_index = 0;
	weapons[1].definition_index = 1;
	weapons[2].definition_index = 2;
	weapon_definitions[1].weapon.interface_definition.hud_interface.index = 1;
	huds[1].crosshairs.count = 3;
	huds[1].crosshairs.address = elements;
	/* a zoom element, an aim element only on full screen, then the aim element with an off-centre item and the
	middle one */
	elements[0].crosshair_type = 1;
	elements[0].crosshairs.items.count = 1;
	elements[0].crosshairs.items.address = &items[2];
	elements[1].crosshair_type = 0;
	elements[1].use_on_map_type = 1;
	elements[1].crosshairs.items.count = 1;
	elements[1].crosshairs.items.address = &items[2];
	elements[2].crosshair_type = 0;
	elements[2].use_on_map_type = 0;
	elements[2].crosshairs.items.count = 2;
	elements[2].crosshairs.items.address = items;
	items[0].placement.offset.x = 150;
	items[1].placement.offset.x = 0;
	items[2].placement.offset.x = 0;
	headshot_map_weapons[_headshot_weapon_pistol].definition_index = 0;
	headshot_map_weapons[_headshot_weapon_sniper_rifle].definition_index = 1;
}

static boolean sniper_stock(void)
{
	headshot_map_weapons[_headshot_weapon_sniper_rifle].stock = find_middle_item(1,
		&headshot_map_weapons[_headshot_weapon_sniper_rifle].middle_hud_index,
		&headshot_map_weapons[_headshot_weapon_sniper_rifle].middle_crosshair_index,
		&headshot_map_weapons[_headshot_weapon_sniper_rifle].middle_item_index);
	return headshot_map_weapons[_headshot_weapon_sniper_rifle].stock;
}

/* the reticles' bitmap: 64 by 64, its pixels loaded or not */
static unsigned char map_pixels[64 * 64 * 2];

static void map_bitmap(boolean loaded)
{
	map.format = _bitmap_format_a8y8;
	map.flags = 0;
	map.width = map.height = 64;
	map.cache_block_index = loaded ? 0 : NONE;
	map.base_address = map_pixels;
}

/* both weapons stock, their reticles' middle item the hud's (1) element 2's
item 1, aiming at the target's head */
static void aiming(boolean head)
{
	sniper_stock();
	map_bitmap(TRUE);
	headshot_map_weapons[_headshot_weapon_pistol].stock = TRUE;
	headshot_map_weapons[_headshot_weapon_pistol].middle_hud_index = 1;
	headshot_map_weapons[_headshot_weapon_pistol].middle_crosshair_index = 2;
	headshot_map_weapons[_headshot_weapon_pistol].middle_item_index = 1;
	setting[0] = TRUE;
	change_count++;
	target_now = 3;
	hit_any = TRUE;
	hit.type = _collision_result_object;
	hit.object_index = 3;
	hit.material_index = head ? 0 : 1;
}

/* the asset a bitmap drawn is a placeholder for, NONE for the map's */
static long drawn_asset(struct bitmap_data const *bitmap)
{
	long asset;

	if (bitmap == &map)
		return NONE;
	CHECK(bitmap >= made && bitmap < made + MADE && made_live[bitmap - made], "not a live placeholder drawn");
	asset = made_asset[bitmap - made];
	CHECK(asset >= 0 && asset < 32 && registered[asset], "a placeholder registered for no texture drawn");
	return asset;
}

#define DRAWN(weapon_index, item_index) \
	drawn_asset(headshot_indicator_item_bitmap(0, weapon_index, 1, 2, item_index, &map))

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";

	world();
	if (!strcmp(test, "middle"))
	{
		/* the aim element on every map type, its item at the middle drawn at every zoom */
		CHECK(sniper_stock(), "no middle item found");
		CHECK(headshot_map_weapons[1].middle_hud_index == 1 && headshot_map_weapons[1].middle_crosshair_index == 2 &&
			headshot_map_weapons[1].middle_item_index == 1, "not the middle item: %ld %d %d",
			headshot_map_weapons[1].middle_hud_index, headshot_map_weapons[1].middle_crosshair_index,
			headshot_map_weapons[1].middle_item_index);
		/* drawn only zoomed: not the middle */
		items[1].flags = FLAG(_hud_crosshair_not_on_default_zoom_bit);
		CHECK(!sniper_stock(), "an item drawn only zoomed taken for the middle");
		return 0;
	}
	if (!strcmp(test, "swapped"))
	{
		struct bitmap_data placeholder;

		/* not on the head: the pistol's own texture, the sniper rifle's without its dot */
		aiming(FALSE);
		CHECK(DRAWN(0, 1) == NONE, "the pistol's reticle not its own");
		CHECK(DRAWN(1, 1) == 10, "the sniper rifle's reticle not drawn without its dot");
		/* on the head: both with the dot */
		hit.material_index = 0;
		CHECK(DRAWN(0, 1) == 20, "the pistol's reticle without the dot on a head");
		CHECK(DRAWN(1, 1) == 20, "the sniper rifle's reticle without the dot on a head");
		/* the reticle not red (the target in the autoaim's falloff, or only the magnetism's range):
		the ray is the shot's, bent as far as that, so still the dot on a head */
		autoaim_level = 0.5f;
		CHECK(DRAWN(0, 1) == 20 && DRAWN(1, 1) == 20, "no dot for a head with the reticle not red");
		autoaim_level = 0.0f;
		CHECK(DRAWN(0, 1) == 20 && DRAWN(1, 1) == 20, "no dot for a head in the magnetism's range only");
		autoaim_level = 1.0f;
		/* only the middle item of the stock weapons, the map's bitmap */
		CHECK(DRAWN(1, 0) == NONE, "another item drawn from a placeholder");
		CHECK(drawn_asset(headshot_indicator_item_bitmap(0, 1, 1, 1, 1, &map)) == NONE, "another element's item");
		CHECK(DRAWN(2, 1) == NONE, "another weapon's");
		CHECK(DRAWN(NONE, 1) == NONE, "with no weapon");
		placeholder = map;
		placeholder.flags |= FLAG(6); /* allocated */
		CHECK(headshot_indicator_item_bitmap(0, 1, 1, 2, 1, &placeholder) == &placeholder,
			"for an allocated bitmap (a high-res placeholder)");
		/* off, another player's (off), the high-res HUD off, a host forbidding it, a map's own sniper rifle: as it is */
		CHECK(drawn_asset(headshot_indicator_item_bitmap(1, 1, 1, 2, 1, &map)) == NONE, "another player's setting taken");
		setting[0] = FALSE;
		change_count++;
		CHECK(DRAWN(0, 1) == NONE && DRAWN(1, 1) == NONE, "with the setting off");
		setting[0] = TRUE;
		change_count++;
		hires_on = FALSE;
		CHECK(DRAWN(0, 1) == NONE && DRAWN(1, 1) == NONE, "with the high-res HUD off");
		hires_on = TRUE;
		host_allows = FALSE;
		CHECK(DRAWN(0, 1) == NONE && DRAWN(1, 1) == NONE, "in a game whose host forbids it");
		host_allows = TRUE;
		headshot_map_weapons[_headshot_weapon_sniper_rifle].stock = FALSE;
		CHECK(DRAWN(1, 1) == NONE, "a map's own sniper rifle's");
		return 0;
	}
	if (!strcmp(test, "changed"))
	{
		aiming(FALSE);
		CHECK(DRAWN(1, 1) == 10, "the setting not read");
		setting[0] = FALSE;
		CHECK(DRAWN(1, 1) == 10, "read again without a change");
		change_count++;
		CHECK(DRAWN(1, 1) == NONE, "not read again on a change");
		return 0;
	}
	if (!strcmp(test, "placeholders"))
	{
		/* not yet loaded: the bitmap, none made */
		aiming(FALSE);
		map.cache_block_index = NONE;
		CHECK(DRAWN(1, 1) == NONE && !made_count, "a placeholder for a bitmap not loaded");
		map.cache_block_index = 0;
		CHECK(DRAWN(1, 1) == 10 && made_count == 1, "none made once loaded");
		CHECK(DRAWN(1, 1) == 10 && made_count == 1, "made again with no change");
		hit.material_index = 0;
		CHECK(DRAWN(1, 1) == 20 && made_count == 2, "the one with the dot not made");
		/* made again when the settings change (the high-res HUD turned off and on), the old one let go */
		change_count++;
		CHECK(DRAWN(1, 1) == 20 && made_count == 3 && deleted_count == 1, "not made again on a change");
		/* and when the bitmap's pixels move: let go, and none for a texture no longer there */
		map.base_address = map_pixels + 2;
		variant_assets[2] = NONE;
		CHECK(DRAWN(1, 1) == NONE && deleted_count == 2 && !registered[20], "the texture's move not seen");
		/* the map unloaded: the rest let go */
		headshot_placeholders_forget();
		CHECK(deleted_count == 3 && !registered[10], "not let go");
		return 0;
	}
	if (!strcmp(test, "missing"))
	{
		/* no texture for the variant, or no placeholder: drawn as it is, not tried again until a change */
		aiming(FALSE);
		variant_assets[1] = NONE;
		CHECK(DRAWN(1, 1) == NONE && !made_count, "a placeholder with no texture");
		variant_assets[1] = 10;
		CHECK(DRAWN(1, 1) == NONE, "tried again with no change");
		change_count++;
		rasterizer_fails = TRUE;
		CHECK(DRAWN(1, 1) == NONE && made_count == 1 && deleted_count == 1, "a placeholder with no texture of its own");
		rasterizer_fails = FALSE;
		change_count++;
		new_fails = TRUE;
		CHECK(DRAWN(1, 1) == NONE, "nothing drawn when none can be made");
		new_fails = FALSE;
		change_count++;
		CHECK(DRAWN(1, 1) == 10, "not tried again after a change");
		return 0;
	}
	if (!strcmp(test, "head"))
	{
		target_now = 3;
		hit_any = TRUE;
		hit.type = _collision_result_object;
		hit.object_index = 3;
		hit.material_index = 0;
		CHECK(headshot_shot_on_head(0, 1), "the target's head missed");
		CHECK(ray_length.i == HEADSHOT_RAY_LENGTH, "the ray's length: %f", ray_length.i);
		hit.material_index = 1;
		CHECK(!headshot_shot_on_head(0, 1), "the body taken for the head");
		hit.material_index = 2;
		CHECK(!headshot_shot_on_head(0, 1), "a material the model does not have");
		hit.material_index = 0;
		hit.object_index = 4;
		CHECK(!headshot_shot_on_head(0, 1), "another object's head (in front of the target) taken");
		hit.object_index = 3;
		hit.type = _collision_result_structure;
		CHECK(!headshot_shot_on_head(0, 1), "a wall taken for the head");
		hit.type = _collision_result_object;
		hit_any = FALSE;
		CHECK(!headshot_shot_on_head(0, 1), "nothing hit taken for the head");
		hit_any = TRUE;
		target_now = NONE;
		CHECK(!headshot_shot_on_head(0, 1), "a head with no target");
		target_now = 3;
		ray_ok = FALSE;
		CHECK(!headshot_shot_on_head(0, 1), "a head with no shot");
		ray_ok = TRUE;
		target_definition.object.collision_model.index = NONE;
		CHECK(!headshot_shot_on_head(0, 1), "a head with no collision model");
		return 0;
	}
	fprintf(stderr, "no such case: %s\n", test);
	return 2;
}
