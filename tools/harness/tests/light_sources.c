/*
LIGHT_SOURCES.C

The enhanced dynamic lights' new lights: which objects' lens-flare-only lights
light (source/objects/object_lights.c light_port_flare_lights), how strong a
weapon's glow is (light_port_weapon_glow), and the port's own lights for what
has none (port/linux/game/light_sources.c: an effect's flash, kept and faded
after its effect is gone; only as many as the room the game's lights leave,
the most visible first; a held display lit by its rounds; one flamethrower
light a firer; the emitters' rows). The effects are a made-up list.
*/

#include "harness.h"
#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define PIN(n, floor, ceiling) ((n) < (floor) ? (floor) : MIN((n), (ceiling)))
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
	NUMBER_OF_INCOMING_OBJECT_FUNCTIONS = 4,
	NUMBER_OF_OUTGOING_OBJECT_FUNCTIONS = 4,
	/* (weapon_export_function_mode.h's) */
	_weapon_function_primary_ammunition = 2,
	_weapon_function_secondary_ammunition = 3,
	_weapon_function_illumination = 12,
};
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
struct tag_reference { unsigned long group_tag; char const *name; long name_length; long index; };
struct tag_block { long count; void *address; void const *definition; };
#define TAG_BLOCK_GET_ELEMENT(block, index, type) (&((type *)(block)->address)[index])
struct object_attachment_definition { struct tag_reference type; };
struct object_function_definition { short scale_function_by_function_index, scale_result_by_function_index; };
struct object_definition_part { struct tag_block attachments; struct tag_block functions; };
struct object_definition { struct object_definition_part object; };
struct weapon_definition { struct object_definition_part object; struct { short function_modes[4]; } weapon; };
struct object_datum
{
	long definition_index;
	struct { short type; long parent_object_index; real incoming_function_values[NUMBER_OF_INCOMING_OBJECT_FUNCTIONS]; } object;
};
struct light_datum { long definition_index; short function_index; };
static struct weapon_definition test_definition;
static struct object_attachment_definition test_attachments[4];
static struct object_function_definition test_functions[2];
#define object_definition_get(index) ((struct object_definition *)&test_definition)

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
	/* an empty needler's glow out (#1309): its light's function ("needle
	glow") is scaled by its needles (B in, its primary ammunition), so is its
	glow; a light whose function is not (the plasma pistol's charge) as before */
	if (!strcmp(case_name, "weapon-glow-rounds"))
	{
		struct object_datum object = { 7, { _object_type_weapon, NONE, { 0.0f, 1.0f, 0.0f, 0.0f } } };
		struct light_datum light = { 33, 1 };
		real radius, full, scale;

		test_attachments[0].type.index = 33;
		test_definition.object.attachments.count = 1;
		test_definition.object.attachments.address = test_attachments;
		test_functions[1].scale_function_by_function_index = 2;
		test_definition.object.functions.count = 2;
		test_definition.object.functions.address = test_functions;
		test_definition.weapon.function_modes[0] = _weapon_function_illumination;
		test_definition.weapon.function_modes[1] = _weapon_function_primary_ammunition;
		full = light_port_weapon_glow(&light, &object, FALSE, &radius);
		CHECK(fabsf(full - LIGHT_PORT_WEAPON_GLOW_WORLD) < 1e-6f, "full: as before: %f", full);
		object.object.incoming_function_values[1] = 0.5f;
		scale = light_port_weapon_glow(&light, &object, TRUE, &radius);
		CHECK(fabsf(scale - LIGHT_PORT_WEAPON_GLOW_HELD * 0.5f) < 1e-6f, "half its needles: half: %f", scale);
		object.object.incoming_function_values[1] = 0.0f;
		scale = light_port_weapon_glow(&light, &object, FALSE, &radius);
		CHECK(scale == 0.0f, "empty: out: %f", scale);
		/* (scaled by its illumination, A in, not its rounds: as bright) */
		test_functions[1].scale_function_by_function_index = 1;
		scale = light_port_weapon_glow(&light, &object, FALSE, &radius);
		CHECK(fabsf(scale - LIGHT_PORT_WEAPON_GLOW_WORLD) < 1e-6f, "not by its rounds: as before: %f", scale);
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
	if (!strcmp(case_name, "magazine-strength"))
	{
		real empty = light_sources_magazine_strength(0, 60);
		real half = light_sources_magazine_strength(30, 60);
		real full = light_sources_magazine_strength(60, 60);

		CHECK(fabsf(full - 1.0f) < 1e-6f, "a full magazine, the whole: %f", full);
		CHECK(empty > 0.0f && empty < half && half < full, "fewer rounds, dimmer, but lit: %f %f %f", empty, half,
			full);
		CHECK(light_sources_magazine_strength(0, 0) == empty, "no magazine size, as empty");
		CHECK(light_sources_magazine_strength(90, 60) == full, "no brighter past full");
		return 0;
	}
	if (!strcmp(case_name, "stream-per-firer"))
	{
		short row = NONE;
		short index;
		real strong, weak;

		for (index = 0; index < (short)NUMBEROF(light_source_objects); index++)
		{
			if (light_source_objects[index].kind == _light_source_stream)
				row = index;
		}
		CHECK(row != NONE, "a stream row (the flamethrower's flames)");
		reset(0);
		/* (one firer's ten flames along x from 10 to 19, another's two) */
		for (index = 0; index < 10; index++)
		{
			real_point3d flame = { 10.0f + (real)index, 0.0f, 0.0f };

			light_sources_stream_add(7, row, &flame);
		}
		for (index = 0; index < 2; index++)
		{
			real_point3d flame = { 5.0f, 1.0f, 0.0f };

			light_sources_stream_add(9, row, &flame);
		}
		light_sources_streams(0.0f);
		CHECK(light_sources.candidate_count == 2, "one light a firer: %d", light_sources.candidate_count);
		CHECK(light_sources.stream_count == 0, "the firers forgotten after the frame: %d", light_sources.stream_count);
		CHECK(fabsf(light_sources.candidates[0].position.x - 14.5f) < 1e-4f, "amid its flames: %f",
			light_sources.candidates[0].position.x);
		strong = light_sources.candidates[0].color.red;
		weak = light_sources.candidates[1].color.red;
		CHECK(weak < strong * 0.5f, "two flames light less than ten: %f of %f", weak, strong);
		return 0;
	}
	if (!strcmp(case_name, "emitter-rows"))
	{
		static struct { char const *name; short kind; } const wanted[] = {
			{ "powerups\\over shield", _light_source_pulse },
			{ "powerups\\active camouflage", _light_source_pulse },
			{ "scenery\\teleporter_shield\\teleporter", _light_source_pulse },
			{ "weapons\\assault rifle\\assault rifle", _light_source_magazine },
			{ "weapons\\flamethrower\\flame", _light_source_stream },
		};
		unsigned want, index;

		for (want = 0; want < NUMBEROF(wanted); want++)
		{
			boolean found = FALSE;

			for (index = 0; index < NUMBEROF(light_source_objects); index++)
			{
				struct light_source_object const *entry = &light_source_objects[index];

				if (strcmp(entry->name, wanted[want].name))
					continue;
				found = TRUE;
				CHECK(entry->kind == wanted[want].kind, "%s's kind: %d", entry->name, entry->kind);
				CHECK(entry->radius > 0.0f && entry->radius <= 4.0f, "%s's reach: %f", entry->name, entry->radius);
				CHECK(MAX(entry->color.red, MAX(entry->color.green, entry->color.blue)) > 0.5f, "%s lit",
					entry->name);
				/* (a display on the gun lights about the hands, no more) */
				if (entry->kind == _light_source_magazine)
				{
					CHECK(entry->marker && entry->radius < 0.5f, "%s on its gun, small", entry->name);
				}
			}
			CHECK(found, "a row for %s", wanted[want].name);
		}
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
