/*
LIGHT_SOURCES.C

The enhanced dynamic lights' new lights: which objects' lens-flare-only lights
light (source/objects/object_lights.c light_port_flare_lights), how strong a
weapon's glow is (light_port_weapon_glow), and the port's own lights for what
has none (port/linux/game/light_sources.c: an effect's flash, kept and faded
after its effect is gone; only as many as the room the game's lights leave,
the most visible first). The effects are a made-up list.
*/

#include "harness.h"
#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define NUMBEROF(a) (sizeof(a) / sizeof((a)[0]))
#define square_root(x) sqrtf(x)
#define _real_epsilon 0.0001f
typedef struct { real red, green, blue; } real_rgb_color;
typedef unsigned short word;

/* (object_lights.c's world) */
enum
{
	_object_type_biped = 0,
	_object_type_vehicle,
	_object_type_weapon,
	_object_type_equipment,
	_object_type_garbage,
	_object_type_projectile,
	_object_type_scenery,
	_object_type_machine,
	_object_type_control,
	_object_type_light_fixture,
	_object_type_placeholder,
	_object_type_sound_scenery,
	NUMBER_OF_OBJECT_TYPES,
	MAXIMUM_NUMBER_OF_ATTACHMENTS_PER_OBJECT = 8,
};
struct tag_reference { unsigned long group_tag; char const *name; long name_length; long index; };
struct tag_block { long count; void *address; void const *definition; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
struct object_attachment_definition { struct tag_reference type; };
struct object_definition { struct { struct tag_block attachments; } object; };
struct object_datum { long definition_index; struct { short type; long parent_object_index; } object; };
struct light_datum { long definition_index; };
static struct object_definition test_definition;
static struct object_attachment_definition test_attachments[4];
#define object_definition_get(index) (&test_definition)

/* (light_sources.c's) */
static struct { struct { real_point3d position; real_vector3d forward; } camera; } render;
static real test_time;
static boolean test_enhanced_game = TRUE;
#define game_in_progress() test_enhanced_game
#define game_time_get() 0
#define render_interpolation_game_time_sec(ticks) test_time
#define object_try_and_get(index) ((struct object_datum *)NULL)
#define dynamic_lights_first_person(index, position) FALSE
#define render_interpolation_object_point(index, position) ((void)0)

/* the effects there are: an identifier each (0: gone), all of the first row */
static long test_effects[64];
static short test_effect_count;

static long effects_port_next(long effect_index, long *definition_index, long *object_index, boolean *going,
	real_point3d *position)
{
	short index = effect_index == NONE ? 0 : (short)(effect_index & 0xFF) + 1;

	for (; index < test_effect_count; index++)
	{
		if (!test_effects[index])
			continue;
		*definition_index = 0x00010000;
		*object_index = NONE;
		*going = TRUE;
		position->x = 5.0f + (real)index;
		position->y = 0.0f;
		position->z = 0.0f;
		return (test_effects[index] << 16) | index;
	}
	return NONE;
}

#include "under_test.inc"

/* light_sources_find's stand-in: effect tag 0 is the first row (a flash),
no object rows */
static void light_sources_find(void)
{
	memset(light_sources.effect_rows, 0, sizeof(light_sources.effect_rows));
	light_sources.effect_rows[0] = 1;
	light_sources.any_effects = TRUE;
	light_sources.any_objects = FALSE;
	light_sources.found = TRUE;
}

static void light_sources_objects(real time)
{
	(void)time;
}

static void reset(short effects)
{
	short index;

	memset(&light_sources, 0, sizeof(light_sources));
	memset(&render, 0, sizeof(render));
	render.camera.forward.i = 1.0f;
	test_time = 0.0f;
	test_effect_count = effects;
	for (index = 0; index < effects; index++)
		test_effects[index] = 0x100 + index;
}

int main(int argc, char **argv)
{
	char const *case_name;

	CHECK(argc == 2, "one named case");
	case_name = argv[1];
	if (!strcmp(case_name, "flare-units-items"))
	{
		static short const lit[] = { _object_type_biped, _object_type_vehicle, _object_type_weapon,
			_object_type_equipment };
		static short const unlit[] = { _object_type_garbage, _object_type_projectile, _object_type_scenery,
			_object_type_machine, _object_type_control, _object_type_light_fixture, _object_type_placeholder,
			_object_type_sound_scenery };
		struct object_datum object = { 0 };
		unsigned index;

		for (index = 0; index < NUMBEROF(lit); index++)
		{
			object.object.type = lit[index];
			CHECK(light_port_flare_lights(&object), "a unit's or an item's flare lights: type %d", lit[index]);
		}
		/* (a projectile's lights by its own test; the map's own flares stay
		the lightmap's) */
		for (index = 0; index < NUMBEROF(unlit); index++)
		{
			object.object.type = unlit[index];
			CHECK(!light_port_flare_lights(&object), "scenery's, a machine's, the map's: type %d", unlit[index]);
		}
		return 0;
	}
	if (!strcmp(case_name, "weapon-glow"))
	{
		struct object_datum object = { 7, { _object_type_weapon, NONE } };
		struct light_datum light = { 33 };
		real radius, scale;

		/* (the plasma pistol's flare on its two triggers, and its overcharge) */
		test_attachments[0].type.index = 33;
		test_attachments[1].type.index = 33;
		test_attachments[2].type.index = 34;
		test_definition.object.attachments.count = 3;
		test_definition.object.attachments.address = test_attachments;
		scale = light_port_weapon_glow(&light, &object, TRUE, &radius);
		CHECK(fabsf(scale - LIGHT_PORT_WEAPON_GLOW_HELD / 2.0f) < 1e-6f, "held: half the whole each: %f", scale);
		CHECK(radius == LIGHT_PORT_WEAPON_GLOW_HELD_RADIUS, "held: about the hands: %f", radius);
		scale = light_port_weapon_glow(&light, &object, FALSE, &radius);
		CHECK(fabsf(scale - LIGHT_PORT_WEAPON_GLOW_WORLD / 2.0f) < 1e-6f, "on the ground: half each: %f", scale);
		CHECK(radius == LIGHT_PORT_WEAPON_GLOW_WORLD_RADIUS, "on the ground: a pool: %f", radius);
		light.definition_index = 34;
		scale = light_port_weapon_glow(&light, &object, FALSE, &radius);
		CHECK(fabsf(scale - LIGHT_PORT_WEAPON_GLOW_WORLD) < 1e-6f, "one copy: the whole: %f", scale);
		return 0;
	}
	if (!strcmp(case_name, "room-kept"))
	{
		struct light_source const *sources;
		short count;

		reset(40);
		count = light_sources_gather(&sources, 3);
		CHECK(count == 3, "no more than the room the game's lights leave: %d", count);
		CHECK(sources[0].position.x < sources[1].position.x && sources[1].position.x < sources[2].position.x,
			"the nearest first: %f %f %f", sources[0].position.x, sources[1].position.x, sources[2].position.x);
		reset(40);
		count = light_sources_gather(&sources, 0);
		CHECK(count == 0, "none where the game's fill the room: %d", count);
		reset(40);
		count = light_sources_gather(&sources, 100);
		CHECK(count == LIGHT_SOURCES_MAXIMUM, "at most LIGHT_SOURCES_MAXIMUM: %d", count);
		reset(4);
		test_enhanced_game = FALSE;
		count = light_sources_gather(&sources, 100);
		test_enhanced_game = TRUE;
		CHECK(count == 0, "none with no game: %d", count);
		return 0;
	}
	if (!strcmp(case_name, "flash-fades"))
	{
		struct light_source const *sources;
		real duration = light_source_effects[0].time;
		real first, later;
		short count;

		CHECK(light_source_effects[0].kind == _light_source_flash, "the first row is a flash");
		reset(1);
		count = light_sources_gather(&sources, 8);
		CHECK(count == 1, "a flash as its effect starts: %d", count);
		first = sources[0].color.red + sources[0].color.green + sources[0].color.blue;
		test_time = duration * 0.5f;
		/* (its effect gone: it goes on, fading) */
		test_effects[0] = 0;
		count = light_sources_gather(&sources, 8);
		CHECK(count == 1, "the flash outlives its effect: %d", count);
		later = sources[0].color.red + sources[0].color.green + sources[0].color.blue;
		CHECK(fabsf(later - first * 0.5f) < 1e-4f, "half way, half as bright: %f of %f", later, first);
		CHECK(sources[0].radius < light_source_effects[0].radius, "and reaching less: %f", sources[0].radius);
		test_time = duration * 1.01f;
		count = light_sources_gather(&sources, 8);
		CHECK(count == 0, "over once its time is: %d", count);
		/* (an effect that lasts past its flash flashes once, not again
		each time its flash is over: c20's teleportation, #1248) */
		reset(1);
		light_sources_gather(&sources, 8);
		test_time = duration * 1.01f;
		count = light_sources_gather(&sources, 8);
		CHECK(count == 0, "over, its effect lasting: %d", count);
		test_time = duration * 1.5f;
		count = light_sources_gather(&sources, 8);
		CHECK(count == 0, "and not flashing again: %d", count);
		/* (the same effect seen again is the same flash, not a new one) */
		reset(1);
		light_sources_gather(&sources, 8);
		test_time = duration * 0.9f;
		count = light_sources_gather(&sources, 8);
		CHECK(count == 1 && sources[0].color.red < first * 0.2f, "one flash an effect: %f", sources[0].color.red);
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
