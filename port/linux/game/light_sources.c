/*
LIGHT_SOURCES.C

The enhanced dynamic lights' own lights (display.dynamic_lights
"enhanced"; port/linux/game/dynamic_lights.c): what glows on screen in the
retail maps but whose tags give it no light, lit as the game's own lights
of the same kind are. Classic lights draw none of them.

- Projectiles with no light: the needler's needles and the Wraith's mortar
  shell (each in the color of its weapon's own flare light), a rocket's
  exhaust, a Sentinel's beam, c20's index platform's light.
- Glowing parts of objects with no light there: the multiplayer Ghost's
  cockpit and running lights (the campaign Ghost's lights, which its tag
  lacks, made by the same function), the Wraith's and the Covenant
  dropship's hover thrusters, a burning lifepod's fire.
- Effects with no light part: a short flash as one starts (the Wraith
  mortar's and the fuel rods' explosions, the Covenant vehicles' and
  turrets' shots, a shield failing, a needle bursting, lightning), or a
  flickering light while it lasts (fires, sparks, the a50 holograms'
  scramble, b40's generator).

Their colors are those of related tags (the weapon's, the shield's or the
effect's own lights: each row says whose); their reach and timing are as
the game's own lights of that kind have it (a grenade's explosion reaches
10 world units for a second, a muzzle flash 2 for a moment). The lights go
after the game's in the frame's list and take only the room it leaves
(the renderer's 128): the most visible first (as dynamic_lights_rank ranks
the game's), at most LIGHT_SOURCES_MAXIMUM, within LIGHT_SOURCES_RANGE of
the camera. None is a light of the game's: nothing is made in the game
state, and the game reads none of them. Only the drawing changes: nothing
the game simulates, nothing sent between machines.
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "effects/effects.h"
#include "render/render.h"
#include "game/game.h"
#include "tag_files/tag_groups.h"
#include "units/units.h"
#include "light_sources.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/game/dynamic_lights.c */
boolean dynamic_lights_first_person(long weapon_index, real_point3d *position);
real dynamic_lights_share(short count);

enum
{
	LIGHT_SOURCES_MAXIMUM = 32,
	/* the flashes kept a moment after their effects are gone */
	LIGHT_SOURCES_FLASHES = 32,
	/* the tags' absolute indices a map has room for (a tag index's low word) */
	LIGHT_SOURCES_TAG_INDICES = 0x10000,
};

/* lights this far from the camera and nearer are drawn (world units: a
light reaches past it only when it is as wide, an explosion's) */
#define LIGHT_SOURCES_RANGE 40.0f


enum
{
	/* a light while it lasts */
	_light_source_steady = 0,
	/* the same, flickering as a fire does */
	_light_source_burn,
	/* a flash as it starts, fading out over its time */
	_light_source_flash,
};

/* an object's light: on a marker of it (none: its origin), by an outgoing
function of it (NONE: whole) */
struct light_source_object
{
	unsigned long group;
	char const *name;
	char const *marker;
	short kind;
	short function_index;
	real_rgb_color color;
	real radius;
};

/* an effect's light: its time a flash's, seconds */
struct light_source_effect
{
	char const *name;
	short kind;
	real_rgb_color color;
	real radius;
	real time;
};

/* (colors borrowed: the needler's "needle flare", 0.94 0.44 1; the plasma
grenade's light, 0.5 1 1; the assault rifle's muzzle flash, 0.96 0.91 0.69;
a30's beam emitter, 0.45 0.87 0.99; the campaign Ghost's cockpit, 0.65 0 1,
and running lights, 0 1 1; the frag grenade's explosion, 1 0.81 0.36) */
static struct light_source_object const light_source_objects[] =
{
	{ 'proj', "weapons\\needler\\needle", NULL, _light_source_steady, NONE, { 0.80f, 0.37f, 0.85f }, 1.0f },
	{ 'proj', "vehicles\\wraith\\wraith mortar", NULL, _light_source_steady, NONE, { 0.50f, 1.00f, 1.00f }, 2.0f },
	{ 'proj', "weapons\\rocket launcher\\rocket", "exhaust", _light_source_burn, NONE, { 0.96f, 0.91f, 0.69f }, 1.5f },
	{ 'proj', "characters\\sentinel\\beam", NULL, _light_source_steady, NONE, { 0.45f, 0.87f, 0.99f }, 1.5f },
	{ 'proj', "levels\\c20\\devices\\index platform\\light", NULL, _light_source_steady, NONE,
		{ 0.60f, 0.85f, 0.99f }, 2.0f },
	{ 'vehi', "vehicles\\ghost\\ghost_mp", "cockpit light", _light_source_steady, 0, { 0.65f, 0.00f, 1.00f }, 1.0f },
	{ 'vehi', "vehicles\\ghost\\ghost_mp", "running lights", _light_source_steady, 0, { 0.00f, 1.00f, 1.00f }, 1.0f },
	{ 'vehi', "vehicles\\wraith\\wraith", "hover thrusters", _light_source_steady, NONE, { 0.30f, 0.60f, 0.60f }, 1.5f },
	{ 'vehi', "vehicles\\c_dropship\\c_dropship", "hover thrusters", _light_source_steady, NONE,
		{ 0.30f, 0.60f, 0.60f }, 1.5f },
	{ 'vehi', "vehicles\\lifepod_entry\\lifepod_entry", "burn", _light_source_burn, NONE, { 1.00f, 0.81f, 0.36f }, 4.0f },
	{ 'scen', "scenery\\vehicles\\lifepod_atmosphere_entry\\lifepod_atmosphere_entry", "burn", _light_source_burn,
		NONE, { 1.00f, 0.81f, 0.36f }, 4.0f },
};

/* (not the emitters' fires, burning_flame's and plasma_flame's, whose
scenery has a light of its own; colors borrowed: the plasma grenade's
explosion, 0.53 0.95 0.98; the
fuel rod's exhaust, 0.68 1 0.45; the Banshee's bolt, 0.5 1 1; the Covenant
turret's plasma, 0.21 0.02 1; the needler's explosion, 0.97 0.34 0.93; the
energy sword's, 0 1 1; a30's beam emitter; the c10 teleporter's light,
0.98 0.73 0; the frag grenade's explosion for fire; the shields' as their
shaders draw them: an Elite's and a Jackal's blue, a Spartan's gold) */
static struct light_source_effect const light_source_effects[] =
{
	/* explosions, as a grenade's (10 world units, a second) */
	{ "vehicles\\wraith\\effects\\wraith mortar explosion", _light_source_flash, { 0.53f, 0.95f, 0.98f }, 10.0f, 1.0f },
	{ "weapons\\fuel rod gun\\effects\\explosion", _light_source_flash, { 0.68f, 1.00f, 0.45f }, 8.0f, 1.0f },
	{ "vehicles\\banshee\\fuel rod explosion", _light_source_flash, { 0.68f, 1.00f, 0.45f }, 8.0f, 1.0f },
	{ "weapons\\fuel rod gun\\fuel rod gun detonating", _light_source_flash, { 0.68f, 1.00f, 0.45f }, 8.0f, 1.0f },
	{ "weapons\\needler\\effects\\needle detonate", _light_source_flash, { 0.97f, 0.34f, 0.93f }, 1.5f, 0.25f },
	/* shots, as a muzzle flash (2 world units, a moment) */
	{ "vehicles\\wraith\\effects\\wraith fire", _light_source_flash, { 0.50f, 1.00f, 1.00f }, 3.0f, 0.2f },
	{ "vehicles\\banshee\\banshee fuel rod fire", _light_source_flash, { 0.68f, 1.00f, 0.45f }, 3.0f, 0.15f },
	{ "weapons\\fuel rod gun\\effects\\fuel rod fire", _light_source_flash, { 0.68f, 1.00f, 0.45f }, 2.5f, 0.15f },
	{ "vehicles\\banshee\\banshee gun fire beam", _light_source_flash, { 0.50f, 1.00f, 1.00f }, 2.0f, 0.1f },
	{ "vehicles\\ghost\\ghost gun fire beam", _light_source_flash, { 0.50f, 1.00f, 1.00f }, 2.0f, 0.1f },
	{ "vehicles\\c gun turret\\effects\\c gun turret fire", _light_source_flash, { 0.21f, 0.02f, 1.00f }, 2.0f, 0.12f },
	/* shields failing */
	{ "characters\\elite\\effects\\elite shield depletion", _light_source_flash, { 0.35f, 0.55f, 1.00f }, 2.5f, 0.4f },
	{ "characters\\jackal\\effects\\shield depletion", _light_source_flash, { 0.40f, 0.50f, 1.00f }, 2.0f, 0.4f },
	{ "characters\\sentinel\\effects\\sentinel shield depletion", _light_source_flash, { 0.45f, 0.87f, 0.99f }, 2.0f,
		0.4f },
	{ "characters\\cyborg\\cyborg shield depletion", _light_source_flash, { 1.00f, 0.85f, 0.40f }, 2.5f, 0.4f },
	{ "scenery\\c_field_generator\\effects\\shield depletion", _light_source_flash, { 0.45f, 0.87f, 0.99f }, 3.0f,
		0.5f },
	{ "weapons\\energy sword\\energy sword dispersal", _light_source_flash, { 0.00f, 1.00f, 1.00f }, 2.0f, 0.5f },
	/* energy */
	{ "levels\\c20\\devices\\index platform\\effects\\lightning", _light_source_flash, { 0.60f, 0.85f, 0.99f }, 6.0f,
		0.15f },
	{ "levels\\c20\\devices\\index platform\\effects\\lightning ring", _light_source_flash, { 0.60f, 0.85f, 0.99f },
		6.0f, 0.15f },
	{ "levels\\c10\\scenery\\mri lightning\\mri light flash", _light_source_flash, { 0.60f, 0.85f, 0.99f }, 6.0f,
		0.15f },
	{ "cinematics\\effects\\teleportation\\teleportation", _light_source_flash, { 0.98f, 0.73f, 0.00f }, 3.0f, 1.0f },
	/* fires and sparks, while they last */
	{ "effects\\burning large", _light_source_burn, { 1.00f, 0.60f, 0.25f }, 4.0f, 0.0f },
	{ "effects\\c10_lift_falling_burning large", _light_source_burn, { 1.00f, 0.60f, 0.25f }, 4.0f, 0.0f },
	{ "characters\\sentinel\\effects\\burning", _light_source_burn, { 1.00f, 0.60f, 0.25f }, 2.0f, 0.0f },
	{ "scenery\\emitters\\sparks constant\\effects\\sparks constant", _light_source_burn, { 0.60f, 0.54f, 0.36f },
		1.5f, 0.0f },
	{ "scenery\\emitters\\sparks spurt\\effects\\sparks spurt", _light_source_flash, { 1.00f, 0.90f, 0.60f }, 2.0f,
		0.3f },
	{ "effects\\panel death", _light_source_flash, { 1.00f, 0.90f, 0.60f }, 2.0f, 0.3f },
	{ "levels\\a50\\devices\\interior tech objects\\holo control\\holo control scrambled", _light_source_burn,
		{ 0.27f, 0.52f, 0.59f }, 2.0f, 0.0f },
	{ "levels\\a50\\devices\\interior tech objects\\holo map\\holo map scrambled", _light_source_burn,
		{ 0.27f, 0.52f, 0.59f }, 2.0f, 0.0f },
	{ "levels\\b40\\devices\\b40_generator_fx\\effects\\generator particles", _light_source_burn,
		{ 0.27f, 0.52f, 0.59f }, 3.0f, 0.0f },
	{ "levels\\b40\\devices\\b40_generator_fx\\effects\\generator particle stream overload", _light_source_burn,
		{ 0.45f, 0.87f, 0.99f }, 4.0f, 0.0f },
};

/* a flash under way: its effect (which may be gone), its row, when it
began (seconds of game time drawn), where, and whether its effect is there
this frame (a flash over whose effect lasts on is kept, spent, so that it
is not taken for a new one: c20's teleportation, which lasts on after the
level starts, flashed again each second, #1248) */
struct light_source_flash
{
	long effect_index;
	short row;
	real start;
	real_point3d position;
	boolean seen;
};

static struct
{
	/* whether the map's rows have been found (light_sources_new_map) */
	boolean found;
	boolean any_objects;
	boolean any_effects;
	/* each tag's row of light_source_effects, plus one (0: none) */
	byte effect_rows[LIGHT_SOURCES_TAG_INDICES];
	/* each row of light_source_objects' tag (NONE: the map has none) */
	long object_definitions[NUMBEROF(light_source_objects)];
	struct light_source_flash flashes[LIGHT_SOURCES_FLASHES];
	struct light_source candidates[LIGHT_SOURCES_MAXIMUM * 4];
	short candidate_count;
} light_sources;

typedef char light_sources_effect_rows_fit[NUMBEROF(light_source_effects) < 255 ? 1 : -1];

/* object_lights.c's lights_initialize_for_new_map: the rows found again in
the next map's tags */
void light_sources_new_map(
	void)
{
	light_sources.found = FALSE;
}

static void light_sources_find(
	void)
{
	short row;

	memset(light_sources.effect_rows, 0, sizeof(light_sources.effect_rows));
	memset(light_sources.flashes, 0, sizeof(light_sources.flashes));
	light_sources.any_objects = FALSE;
	light_sources.any_effects = FALSE;
	for (row = 0; row < (short)NUMBEROF(light_source_objects); row++)
	{
		light_sources.object_definitions[row] = tag_loaded(light_source_objects[row].group,
			light_source_objects[row].name);
		light_sources.any_objects |= light_sources.object_definitions[row] != NONE;
	}
	for (row = 0; row < (short)NUMBEROF(light_source_effects); row++)
	{
		long tag_index = tag_loaded('effe', light_source_effects[row].name);

		if (tag_index == NONE)
			continue;
		light_sources.effect_rows[DATUM_INDEX_TO_ABSOLUTE_INDEX(tag_index) & (LIGHT_SOURCES_TAG_INDICES - 1)] =
			(byte)(row + 1);
		light_sources.any_effects = TRUE;
	}
	light_sources.found = TRUE;
}

/* a fire's flicker, 0.6 to 1: two waves at each light's own phase */
static real light_sources_flicker(
	real time,
	unsigned long seed)
{
	real phase = (real)(seed % 628) * 0.01f;
	real wave = 0.6f * sinf(time * 13.0f + phase) + 0.4f * sinf(time * 31.0f + 2.3f * phase);

	return 0.8f + 0.2f * wave;
}

/* a light kept as a candidate, scored as dynamic_lights_rank scores the
game's: its brightest channel times its reach's part of the view */
static void light_sources_add(
	real_point3d const *position,
	real radius,
	real_rgb_color const *color,
	real strength,
	boolean halo)
{
	struct light_source *source;
	real_vector3d offset;
	real distance, intensity;

	if (!(strength > 0.0f) || !(radius > 0.0f))
		return;
	vector_from_points3d(&render.camera.position, position, &offset);
	distance = magnitude3d(&offset);
	if (distance - radius > LIGHT_SOURCES_RANGE)
		return;
	intensity = strength * MAX(color->red, MAX(color->green, color->blue));
	if (!(intensity > 0.0f))
		return;
	if (light_sources.candidate_count >= (short)NUMBEROF(light_sources.candidates))
		return;
	source = &light_sources.candidates[light_sources.candidate_count++];
	source->position = *position;
	source->radius = radius;
	source->color.red = color->red * strength;
	source->color.green = color->green * strength;
	source->color.blue = color->blue * strength;
	source->halo = halo;
	source->score = intensity * radius / (distance > radius ? distance : radius);
	if (dot_product3d(&offset, &render.camera.forward) < -(radius + 0.1f))
		source->score *= 1.0f / 64.0f;
}

/* where an effect's light is drawn: on its object as drawn, and on a local
player's first-person weapon where its weapon is that */
static void light_sources_effect_position(
	long object_index,
	real_point3d *position)
{
	struct object_datum *object;

	if (object_index == NONE || !(object = object_try_and_get(object_index)))
		return;
	render_interpolation_object_point(object_index, position);
	if (object->object.type == _object_type_weapon && object->object.parent_object_index != NONE)
		dynamic_lights_first_person(object_index, position);
}

static void light_sources_effects(
	real time)
{
	long effect_index = NONE;
	long definition_index, object_index;
	boolean going;
	real_point3d position;
	short index;

	for (index = 0; index < LIGHT_SOURCES_FLASHES; index++)
		light_sources.flashes[index].seen = FALSE;
	while ((effect_index = effects_port_next(effect_index, &definition_index, &object_index, &going, &position)) !=
		NONE)
	{
		short row = (short)light_sources.effect_rows[DATUM_INDEX_TO_ABSOLUTE_INDEX(definition_index) &
			(LIGHT_SOURCES_TAG_INDICES - 1)] - 1;
		struct light_source_effect const *entry;

		if (row < 0)
			continue;
		entry = &light_source_effects[row];
		light_sources_effect_position(object_index, &position);
		if (entry->kind == _light_source_flash)
		{
			struct light_source_flash *flash = NULL;
			struct light_source_flash *free_slot = NULL;
			struct light_source_flash *oldest = NULL;

			for (index = 0; index < LIGHT_SOURCES_FLASHES; index++)
			{
				struct light_source_flash *slot = &light_sources.flashes[index];

				if (slot->effect_index == effect_index && slot->row == row)
				{
					flash = slot;
					break;
				}
				if (!slot->effect_index)
				{
					if (!free_slot)
						free_slot = slot;
				}
				else if (!oldest || slot->start < oldest->start)
				{
					oldest = slot;
				}
			}
			if (!flash)
			{
				/* (a new one: in a free slot, or the oldest's) */
				flash = free_slot ? free_slot : oldest;
				flash->effect_index = effect_index;
				flash->row = row;
				flash->start = time;
			}
			flash->seen = TRUE;
			flash->position = position;
			continue;
		}
		if (going)
			light_sources_add(&position, entry->radius, &entry->color,
				light_sources_flicker(time, (unsigned long)effect_index), FALSE);
	}
	/* (the flashes, as long as they last, their effects gone or not; one
	over is kept, lighting nothing, while its effect lasts: an effect
	flashes once, as it starts) */
	for (index = 0; index < LIGHT_SOURCES_FLASHES; index++)
	{
		struct light_source_flash *flash = &light_sources.flashes[index];
		struct light_source_effect const *entry;
		real fraction;

		if (!flash->effect_index)
			continue;
		entry = &light_source_effects[flash->row];
		fraction = (time - flash->start) / entry->time;
		if (fraction >= 1.0f && flash->seen)
			continue;
		if (fraction >= 1.0f || fraction < -1.0f)
		{
			flash->effect_index = 0;
			continue;
		}
		fraction = MAX(fraction, 0.0f);
		light_sources_add(&flash->position, entry->radius * (0.6f + 0.4f * (1.0f - fraction)), &entry->color,
			1.0f - fraction, FALSE);
	}
}

static void light_sources_objects(
	real time)
{
	struct object_iterator iterator;
	struct object_datum *object;

	object_iterator_new(&iterator, _object_mask_projectile | _object_mask_vehicle | _object_mask_scenery, 0);
	while ((object = object_iterator_next(&iterator)) != NULL)
	{
		short row;

		if (TEST_FLAG(object->object.flags, _object_invisible_bit) ||
			!TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		{
			continue;
		}
		if (distance_squared3d(&object->object.bounding_sphere_center, &render.camera.position) >
			(LIGHT_SOURCES_RANGE + 4.0f) * (LIGHT_SOURCES_RANGE + 4.0f))
		{
			continue;
		}
		for (row = 0; row < (short)NUMBEROF(light_source_objects); row++)
		{
			struct light_source_object const *entry = &light_source_objects[row];
			real strength = 1.0f;
			real_point3d position;

			if (light_sources.object_definitions[row] != object->definition_index)
				continue;
			if (TEST_FLAG(_object_mask_unit, object->object.type) && !(object->object.body_vitality > 0.0f))
				continue;
			if (entry->function_index != NONE)
				strength = render_interpolation_object_function_values(iterator.index)[entry->function_index];
			if (entry->kind == _light_source_burn)
				strength *= light_sources_flicker(time, (unsigned long)iterator.index + row);
			if (TEST_FLAG(_object_mask_unit, object->object.type) &&
				unit_get(iterator.index)->unit.active_camouflage > 0.0f)
			{
				strength *= 1.0f - unit_get(iterator.index)->unit.active_camouflage;
			}
			if (!(strength > 0.0f))
				continue;
			if (entry->marker)
			{
				struct object_marker markers[MAXIMUM_LENS_FLARES_PER_LIGHT];
				short count = object_get_marker_by_name(iterator.index, entry->marker, markers, NUMBEROF(markers));
				short index;

				/* (a light at each, as many as a light's flares are drawn
				on, its strength shared in linear light as the game's
				lights' over their markers: a pair of thrusters as bright
				as one; the Ghost's three running lights all lit) */
				for (index = 0; index < count; index++)
				{
					light_sources_add(&markers[index].matrix.position, entry->radius, &entry->color,
						strength * dynamic_lights_share(count), FALSE);
				}
				continue;
			}
			render_interpolation_object_origin(iterator.index, &position);
			light_sources_add(&position, entry->radius, &entry->color, strength,
				object->object.type == _object_type_projectile &&
					object_definition_get(object->definition_index)->object.model.index == NONE);
		}
	}
}

static int light_sources_compare(
	void const *a,
	void const *b)
{
	struct light_source const *left = a;
	struct light_source const *right = b;

	if (left->score != right->score)
		return left->score > right->score ? -1 : 1;
	return 0;
}

/* dynamic_lights.c's dynamic_lights_submit, with the enhanced dynamic
lights: the frame's lights of these, the most visible first, as many as
room (the renderer's room past the game's lights) and LIGHT_SOURCES_MAXIMUM;
how many */
short light_sources_gather(
	struct light_source const **sources,
	short room)
{
	real time = render_interpolation_game_time_sec(game_time_get());

	light_sources.candidate_count = 0;
	*sources = light_sources.candidates;
	if (room <= 0 || !game_in_progress())
		return 0;
	if (!light_sources.found)
		light_sources_find();
	if (light_sources.any_effects)
		light_sources_effects(time);
	if (light_sources.any_objects)
		light_sources_objects(time);
	if (light_sources.candidate_count > 1)
		qsort(light_sources.candidates, light_sources.candidate_count, sizeof(*light_sources.candidates),
			light_sources_compare);
	return MIN(light_sources.candidate_count, MIN(room, LIGHT_SOURCES_MAXIMUM));
}
