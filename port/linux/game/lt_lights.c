/*
LT_LIGHTS.C

The one light-transport pass's light list (lt_lights.h; ~/dev/rt2-realtime-
design.md, W4 and contract C4). Each frame, from dynamic_lights.c's
dynamic_lights_submit (after lights_preprocess_scene: the rasterizer's
lights as drawn, the enhanced lights' copies and own lights, and each one's
key), one record a light, then one a sun, to the backend (xgpu_stream.h's
LT_LIGHTS). Render-only and read only: nothing the simulation reads.

Each record (the reading of the lights follows rt_lights_materials.c's, the
truth export's lights.json, so the two agree at a tick):
- id: dynamic_lights.c's key, stable from frame to frame: a game light's is
  its light datum's handle (index and salt), a copy's its handle and its
  number among that light's copies, an own light's light_sources.c's key; a
  sun's its sky and index.
- kind: a spot where the tag has a cone (runtime falloff cosine not -1) or an
  own light has one, a panel (light_sources.c's displays), a sun; else a point.
- position, forward and up as drawn (rasterizer_lights.lights[]); a gel's
  frame turned by its functions (rasterizer_environment_port_gel, as the gel
  pass and the enhanced flashlight have it); left = forward x up.
- emitter: the middle of what emits, the position but for a first-person
  flashlight, whose gel is projected from behind the eye
  (first_person_weapon_center_flashlight) and whose lamp is on the weapon in
  hand (first_person_weapon_flashlight_lamp): E1's disk, its shadows' origin.
- emitter_radius: the tags have no emitter size. E1's flashlight disk (0.01,
  a lamp's lens); a panel's from its area (light_sources.c: radius squared
  in its cone offset); else dynamic_lights.c's shadow disks' rule (a
  projectile 0.05, others 5% of the reach, 0.1 to 0.5).
- power: the color as the enhanced lights draw it (the rasterizer's, times
  the port's brightening past 1), taken to linear (^2.2), times k
  (lt_power_scale: ~/dev/rt2-calibration.json's flashlight_power_scale and
  dynamic_power_scales, a k a kind; LT_LIGHTS_POWER_SCALE the flashlight's,
  measured for the flashlight alone; the others' k to be measured each once,
  the same way: see the report and lt_lights_power_scale). The gel is the
  angular profile alone (pow(color * gel, 2.2) = pow(color, 2.2) x
  pow(gel, 2.2)).
- range: the reach as drawn (the tag's radius by its modifier), the range
  window's radius; cone_inner and cone_outer: the tag's runtime falloff and
  cutoff cosines (an own light's from its cone's scale and offset); -1 none.
- gel_slot: the flashlight's slot in the backend's flashlight table (the
  order dynamic_lights.c's dynamic_lights_flashlights gives them); gel_tag
  its bitmap. A gel light that is not a flashlight (a lamp: models only, the
  structure lit by the game's own gel pass) has its gel_tag and no slot.
- suns: the sky the most BSP clusters name (lights.json's "sky"), each of its
  lights: forward toward it (vector3d_from_euler_angles2d, as render_sky.c
  aims its lens flare), its angular diameter, power color x the tag's power
  x LT_LIGHTS_SUN_POWER_SCALE (the radiosity tool's units, linear: to be
  calibrated against the lightmap's direct share), flagged LIGHTMAP_DIRECT.

HALO_LT_LIGHTS: 1 builds and sends the list each frame, 0 never; unset, it
is sent with the light-transport pass on (HALO_LT_E1 not off).
HALO_LT_LIGHTS_DUMP=<folder> also writes each frame's list as one JSON line
of <folder>/lt_lights.jsonl (its frame, tick, window and records), to check
against the truth export's lights.json at the same tick.
*/

#include "cseries.h"
#include "real_math.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "game/game.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "rasterizer/rasterizer_environment.h"
#include "interface/first_person_weapons.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structures.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "physics/collisions.h"
#include "halo_dynamic_lights.h"
#include "lt_lights.h"
#include "world_rt.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's own fopen: the dump's folder is a whole host path, not
an Xbox path for port/linux/include/stdio.h to translate) */
#undef fopen

void platform_log(const char *format, ...);
/* (port/linux/src/xgpu_stream_encode.c: count records, xgpu_stream.h's
layout, for window) */
void xgpu_backend_lt_lights(const void *lights, int count, int window, unsigned int frame);
/* (object_lights.c) */
extern struct data_array *light_data;

/* k: the flashlight's, measured (~/dev/rt2-calibration.json); the suns' the
truth helper's fit of the lightmap's direct share (sun_power_scale) */
#define LT_LIGHTS_POWER_SCALE 1.009f
/* the other kinds' k, each measured once the flashlight's way
(~/dev/rt2-calibration.json's dynamic_power_scales and their derivation):
a point light on an object (a muzzle flash, a bolt), a point light on none
(an effect's: an explosion); a spot that is no flashlight (a lamp) and a
panel not measured (none staged): the flashlight's. The backend's light
transport applies the same rule (xgpu_vulkan_lt.c's lt_kind_k). */
#define LT_LIGHTS_OBJECT_POWER_SCALE 0.845f
#define LT_LIGHTS_EFFECT_POWER_SCALE 7.85f
#define LT_LIGHTS_SUN_POWER_SCALE 0.488f
/* the emitters' radii (world units): E1's flashlight disk, then
dynamic_lights.c's shadow disks' (SHADOWED_DISK_*) */
#define LT_LIGHTS_FLASHLIGHT_DISK 0.01f
#define LT_LIGHTS_PROJECTILE_DISK 0.05f
#define LT_LIGHTS_DISK_SCALE 0.05f
#define LT_LIGHTS_DISK_LEAST 0.1f
#define LT_LIGHTS_DISK_MOST 0.5f

/* (rt_lights_materials.c's rtlm_light_definition: the light tag, as far as
its gel) */
struct lt_light_definition
{
	unsigned long flags;
	real radius;
	real radius_modifier_lower_bound;
	real radius_modifier_upper_bound;
	real falloff_angle;
	real cutoff_angle;
	real lens_flare_radius;
	real runtime_cosine_falloff_angle;
	real runtime_cosine_cutoff_angle;
	real specular_radius_multiplier;
	real runtime_sine_cutoff_angle;
	byte reserved2C[0x8];
	unsigned long color_interpolation_flags;
	real_argb_color color_lower_bound;
	real_argb_color color_upper_bound;
	byte reserved58[0xC];
	struct tag_reference gel_map;
};
typedef char lt_light_definition_gel_assert[offsetof(struct lt_light_definition, gel_map) == 0x64 ? 1 : -1];
enum
{
	_lt_definition_no_specular_bit = 1,
	_lt_definition_dont_light_own_object_bit = 2,
};

/* (object_lights.c's light_datum, as far as its flags) */
struct lt_light_datum
{
	struct datum_header header;
	word flags;
};
#define LT_LIGHT_DATUM_FIRST_PERSON_BIT 3

/* (rt_lights_materials.c's rtlm_sky and rtlm_sky_light: the sky tag's lights) */
struct lt_sky_light
{
	struct tag_reference lens_flare;
	char lens_flare_marker_name[32];
	byte unused30[0x4C - 0x30];
	unsigned long flags;
	real_rgb_color color;
	real power;
	real test_distance;
	byte unused64[4];
	real_euler_angles2d direction;
	real diameter;
};
typedef char lt_sky_light_size_assert[sizeof(struct lt_sky_light) == 0x74 ? 1 : -1];
#define LT_SKY_LIGHTS_OFFSET 0xC4

static struct
{
	int wanted;
	char dump_path[1024];
	FILE *dump;
	unsigned int frame;
	struct xgpu_stream_lt_light lights[XGPU_STREAM_LT_LIGHTS_MAXIMUM];
	/* the sky lights.json names (the most clusters'), for the BSP it was found for */
	void const *sky_bsp;
	short sky_index;
} lt = { -1 };

/* ---------- the list */

static int lt_lights_wanted(void)
{
	if (lt.wanted < 0)
	{
		char const *given = getenv("HALO_LT_LIGHTS");
		char const *pass = getenv("HALO_LT_E1");
		char const *dump = getenv("HALO_LT_LIGHTS_DUMP");

		lt.wanted = pass && *pass && strcmp(pass, "off") && strcmp(pass, "0") && strcmp(pass, "false");
		if (given && *given)
			lt.wanted = strcmp(given, "0") && strcmp(given, "off") && strcmp(given, "false");
		if (dump && *dump)
		{
			snprintf(lt.dump_path, sizeof(lt.dump_path), "%s/lt_lights.jsonl", dump);
			lt.wanted = 1;
		}
		if (lt.wanted)
			platform_log("lt lights: the light list sent each frame%s%s", lt.dump_path[0] ? ", dumped to " : "",
				lt.dump_path);
	}
	return lt.wanted;
}

boolean lt_lights_sent(void)
{
	return lt_lights_wanted() != 0;
}

static float lt_linear(float value)
{
	return value > 0.0f ? powf(value, 2.2f) : 0.0f;
}

static void lt_copy3(float out[3], float x, float y, float z)
{
	out[0] = x;
	out[1] = y;
	out[2] = z;
}

/* left = forward x up, normalized */
static void lt_left(struct xgpu_stream_lt_light *out)
{
	real_vector3d forward, up, left;

	forward.i = out->forward[0];
	forward.j = out->forward[1];
	forward.k = out->forward[2];
	up.i = out->up[0];
	up.j = out->up[1];
	up.k = out->up[2];
	cross_product3d(&forward, &up, &left);
	normalize3d(&left);
	lt_copy3(out->left, left.i, left.j, left.k);
}

/* the sky lights.json takes: the one the most BSP clusters name */
static short lt_sky_index(void)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	struct scenario *scenario = global_scenario_get();
	long counts[32], index;
	short sky_index;

	if (bsp == lt.sky_bsp)
		return lt.sky_index;
	lt.sky_bsp = bsp;
	lt.sky_index = 0;
	memset(counts, 0, sizeof(counts));
	for (index = 0; bsp && bsp->clusters.address && index < bsp->clusters.count; index++)
	{
		struct structure_cluster const *cluster = (struct structure_cluster const *)bsp->clusters.address + index;

		if (cluster->sky_index >= 0 && cluster->sky_index < 32)
			counts[cluster->sky_index]++;
	}
	for (sky_index = 1; scenario && sky_index < scenario->sky_references.count && sky_index < 32; sky_index++)
	{
		if (counts[sky_index] > counts[lt.sky_index])
			lt.sky_index = sky_index;
	}
	return lt.sky_index;
}

/* the suns after count records; the count of all */
static int lt_suns(int count)
{
	struct scenario *scenario = global_scenario_get();
	short sky_index;
	char const *sky;
	struct tag_block const *block;
	long index;

	if (!scenario || scenario->sky_references.count <= 0)
		return count;
	sky_index = lt_sky_index();
	sky = (char const *)scenario_get_sky(sky_index);
	if (!sky)
		return count;
	block = (struct tag_block const *)(sky + LT_SKY_LIGHTS_OFFSET);
	for (index = 0; block->address && index < block->count && count < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct lt_sky_light const *light = (struct lt_sky_light const *)block->address + index;
		struct xgpu_stream_lt_light *out = &lt.lights[count];
		real_vector3d direction;
		real_euler_angles2d up_angles;
		real_vector3d up;

		memset(out, 0, sizeof(*out));
		vector3d_from_euler_angles2d(&direction, &light->direction);
		/* (an up: the direction's, a quarter turn of pitch above it) */
		up_angles.yaw = light->direction.yaw;
		up_angles.pitch = light->direction.pitch + 0.5f * _pi;
		vector3d_from_euler_angles2d(&up, &up_angles);
		out->id[0] = ((unsigned int)sky_index << 8) | (unsigned int)(index & 0xff);
		out->id[1] = (unsigned int)XGPU_LT_LIGHT_SOURCE_SUN << 24;
		out->kind = XGPU_LT_LIGHT_DIRECTIONAL;
		out->flags = XGPU_LT_LIGHT_LIGHTMAP_DIRECT;
		out->object = 0xffffffffu;
		out->gel_slot = -1;
		out->gel_tag = -1;
		lt_copy3(out->forward, direction.i, direction.j, direction.k);
		lt_copy3(out->up, up.i, up.j, up.k);
		lt_left(out);
		out->cone_inner = out->cone_outer = -1.0f;
		out->power_scale = LT_LIGHTS_SUN_POWER_SCALE;
		lt_copy3(out->color, light->color.red, light->color.green, light->color.blue);
		lt_copy3(out->power, light->color.red * light->power * LT_LIGHTS_SUN_POWER_SCALE,
			light->color.green * light->power * LT_LIGHTS_SUN_POWER_SCALE,
			light->color.blue * light->power * LT_LIGHTS_SUN_POWER_SCALE);
		out->angular_diameter = light->diameter;
		count++;
	}
	return count;
}

/* a light's k by its kind: the flashlight's, a point's on an object or on
none, the rest the flashlight's */
static float lt_power_scale(struct xgpu_stream_lt_light const *light)
{
	if ((light->flags & XGPU_LT_LIGHT_FLASHLIGHT) || light->kind != XGPU_LT_LIGHT_POINT)
		return LT_LIGHTS_POWER_SCALE;
	return light->object != 0xffffffffu ? LT_LIGHTS_OBJECT_POWER_SCALE : LT_LIGHTS_EFFECT_POWER_SCALE;
}

/* the rasterizer light a copy (key) is of: its index, or NONE */
static long lt_copy_parent(unsigned long long const *keys, long game_count, unsigned long long key)
{
	long index;

	for (index = 0; index < game_count; index++)
	{
		if ((keys[index] >> 56) == XGPU_LT_LIGHT_SOURCE_GAME && (keys[index] & 0xffffffffull) == (key & 0xffffffffull))
			return index;
	}
	return NONE;
}

static int lt_build(struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count)
{
	long index;
	int written = 0, flashlights = 0;

	for (index = 0; index < count && written < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct halo_dynamic_light const *light = &lights[index];
		struct xgpu_stream_lt_light *out = &lt.lights[written];
		unsigned long long key = keys[index];
		unsigned int source = (unsigned int)(key >> 56);
		long drawn_index = index < game_count ? index :
			source == XGPU_LT_LIGHT_SOURCE_COPY ? lt_copy_parent(keys, game_count, key) : NONE;
		struct rasterizer_light_submit_parameters const *drawn = drawn_index != NONE &&
			drawn_index < rasterizer_lights.light_count ? &rasterizer_lights.lights[drawn_index] : NULL;
		struct lt_light_definition const *definition = drawn ?
			(struct lt_light_definition const *)(void const *)drawn->definition : NULL;
		real_vector3d forward, up, left;
		float disk;
		int channel;

		if (!(light->radius > 0.0f) || !(light->color[0] > 0.0f || light->color[1] > 0.0f || light->color[2] > 0.0f))
			continue;
		memset(out, 0, sizeof(*out));
		if (!key)
		{
			key = ((unsigned long long)XGPU_LT_LIGHT_SOURCE_FRAME << 56) | (unsigned long long)index;
			out->flags |= XGPU_LT_LIGHT_UNSTABLE_ID;
		}
		out->id[0] = (unsigned int)(key & 0xffffffffull);
		out->id[1] = (unsigned int)(key >> 32);
		/* (a copy numbered among its own light's copies, in their markers'
		order: dynamic_lights.c's key numbers it among all the frame's copies,
		which shift as other lights' copies come and go) */
		if (source == XGPU_LT_LIGHT_SOURCE_COPY)
		{
			int other;
			unsigned int number = 1;

			for (other = 0; other < written; other++)
			{
				if (lt.lights[other].id[0] == out->id[0] && (lt.lights[other].id[1] >> 24) == XGPU_LT_LIGHT_SOURCE_COPY)
					number++;
			}
			out->id[1] = ((unsigned int)XGPU_LT_LIGHT_SOURCE_COPY << 24) | number;
		}
		out->flags |= (unsigned int)light->flags & 0x1fu;
		if (baked[index])
			out->flags |= XGPU_LT_LIGHT_BAKED;
		out->object = objects[index] != NONE ? (unsigned int)objects[index] : 0xffffffffu;
		/* (one of the port's own lights (light_sources.c) on an object, its
		key's kind 2, or a firer's flames, 3: that object's, its handle in the
		key; it is a light on an object (a bolt's, a thruster's glow), whose k
		is a point's on an object, not an explosion's: lt_power_scale) */
		if ((source == 2 || source == 3) && out->object == 0xffffffffu)
			out->object = (unsigned int)(key & 0xffffffffull);
		out->owner = (unsigned int)owners[index];
		out->gel_slot = -1;
		out->gel_tag = -1;
		lt_copy3(out->position, light->position[0], light->position[1], light->position[2]);
		lt_copy3(out->emitter, light->position[0], light->position[1], light->position[2]);
		lt_copy3(out->forward, light->axis[0], light->axis[1], light->axis[2]);
		if (drawn)
			lt_copy3(out->up, drawn->up.i, drawn->up.j, drawn->up.k);
		else
			lt_copy3(out->up, 0.0f, 0.0f, 1.0f);
		out->range = light->radius;
		lt_copy3(out->color, light->color[0], light->color[1], light->color[2]);
		out->cone_inner = out->cone_outer = -1.0f;
		out->kind = XGPU_LT_LIGHT_POINT;
		if (definition)
		{
			if (definition->runtime_cosine_falloff_angle != -1.0f)
			{
				out->kind = XGPU_LT_LIGHT_SPOT;
				out->cone_inner = definition->runtime_cosine_falloff_angle;
				out->cone_outer = definition->runtime_cosine_cutoff_angle;
			}
			if (TEST_FLAG(definition->flags, _lt_definition_no_specular_bit))
				out->flags |= XGPU_LT_LIGHT_NO_SPECULAR;
			if (TEST_FLAG(definition->flags, _lt_definition_dont_light_own_object_bit))
				out->flags |= XGPU_LT_LIGHT_NOT_OWN_OBJECT;
			if (definition->gel_map.index != NONE)
			{
				out->flags |= XGPU_LT_LIGHT_GEL;
				out->gel_tag = (int)definition->gel_map.index;
			}
		}
		else if (light->flags & HALO_DYNAMIC_LIGHT_PANEL)
			out->kind = XGPU_LT_LIGHT_PANEL;
		else if (light->cone_scale > 0.0f)
		{
			/* (an own light's cone: scale (cos - outer), offset -scale outer) */
			out->kind = XGPU_LT_LIGHT_SPOT;
			out->cone_outer = -light->cone_offset / light->cone_scale;
			out->cone_inner = out->cone_outer + 1.0f / light->cone_scale;
		}
		/* (the light datum's own flag: on the first-person weapon) */
		if (source == XGPU_LT_LIGHT_SOURCE_GAME && light_data)
		{
			struct lt_light_datum const *datum = datum_try_and_get(light_data, (long)out->id[0]);

			if (datum && TEST_FLAG(datum->flags, LT_LIGHT_DATUM_FIRST_PERSON_BIT))
				out->flags |= XGPU_LT_LIGHT_FIRST_PERSON;
		}
		/* (a gel's frame as the gel pass turns it; the flashlights' slots in
		the order dynamic_lights_flashlights gives them) */
		if (index < game_count && rasterizer_environment_port_gel(index, &forward, &up, &left, NULL, NULL))
		{
			lt_copy3(out->forward, forward.i, forward.j, forward.k);
			lt_copy3(out->up, up.i, up.j, up.k);
			lt_copy3(out->left, left.i, left.j, left.k);
			if ((light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT) && !(light->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) &&
				flashlights < HALO_DYNAMIC_LIGHTS_FLASHLIGHTS)
			{
				real_point3d lamp;

				out->gel_slot = flashlights++;
				if (objects[index] != NONE && first_person_weapon_flashlight_lamp(objects[index], &lamp))
					lt_copy3(out->emitter, lamp.x, lamp.y, lamp.z);
			}
		}
		else
			lt_left(out);
		/* (the emitter's size) */
		disk = light->radius * LT_LIGHTS_DISK_SCALE;
		disk = disk < LT_LIGHTS_DISK_LEAST ? LT_LIGHTS_DISK_LEAST : disk > LT_LIGHTS_DISK_MOST ? LT_LIGHTS_DISK_MOST : disk;
		if (light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT)
			disk = LT_LIGHTS_FLASHLIGHT_DISK;
		else if (out->kind == XGPU_LT_LIGHT_PANEL)
			disk = light->cone_offset > 0.0f ? sqrtf(light->cone_offset) : LT_LIGHTS_DISK_LEAST;
		else if (light->flags & HALO_DYNAMIC_LIGHT_PROJECTILE)
			disk = LT_LIGHTS_PROJECTILE_DISK;
		out->emitter_radius = disk;
		/* (the power: its kind's k, once its kind, flags and object are
		known) */
		out->power_scale = lt_power_scale(out);
		for (channel = 0; channel < 3; channel++)
			out->power[channel] = lt_linear(light->color[channel]) * out->power_scale;
		written++;
	}
	return lt_suns(written);
}

/* ---------- the dump */

static void lt_dump_vector(FILE *file, char const *name, float const *v)
{
	fprintf(file, ",\"%s\":[%.6g,%.6g,%.6g]", name, (double)v[0], (double)v[1], (double)v[2]);
}

static void lt_dump(int count, int window)
{
	static char const *const kinds[] = { "point", "spot", "panel", "directional" };
	static char const *const sources[] = { "frame", "own", "own", "own", "game", "copy", "sun", "frame" };
	int index;

	if (!lt.dump_path[0])
		return;
	if (!lt.dump)
	{
		lt.dump = fopen(lt.dump_path, "w");
		if (!lt.dump)
		{
			platform_log("lt lights: %s could not be written", lt.dump_path);
			lt.dump_path[0] = 0;
			return;
		}
	}
	fprintf(lt.dump, "{\"frame\":%u,\"tick\":%ld,\"tick_fraction\":%.4f,\"window\":%d,\"count\":%d,\"lights\":[",
		lt.frame, (long)game_time_get(), (double)game_time_get_tick_fraction(), window, count);
	for (index = 0; index < count; index++)
	{
		struct xgpu_stream_lt_light const *light = &lt.lights[index];
		char const *tag = light->gel_tag != -1 ? tag_get_name(light->gel_tag) : NULL;

		fprintf(lt.dump, "%s{\"id\":\"%08x%08x\",\"source\":\"%s\",\"kind\":\"%s\",\"flags\":%u,\"object\":%d,"
			"\"owner\":%u", index ? "," : "", light->id[1], light->id[0], sources[(light->id[1] >> 24) & 7],
			light->kind < 4 ? kinds[light->kind] : "?", light->flags, (int)light->object, light->owner);
		lt_dump_vector(lt.dump, "position", light->position);
		lt_dump_vector(lt.dump, "emitter", light->emitter);
		lt_dump_vector(lt.dump, "direction", light->forward);
		lt_dump_vector(lt.dump, "up", light->up);
		lt_dump_vector(lt.dump, "left", light->left);
		lt_dump_vector(lt.dump, "color", light->color);
		lt_dump_vector(lt.dump, "power", light->power);
		fprintf(lt.dump, ",\"power_scale\":%.6g,\"emitter_radius\":%.6g,\"radius\":%.6g,\"cone_cosines\":[%.6g,%.6g],"
			"\"angular_diameter\":%.6g,\"gel_slot\":%d,\"gel_tag\":%d,\"gel\":\"", (double)light->power_scale,
			(double)light->emitter_radius, (double)light->range, (double)light->cone_inner, (double)light->cone_outer,
			(double)light->angular_diameter, light->gel_slot, light->gel_tag);
		for (; tag && *tag; tag++)
			fprintf(lt.dump, *tag == '\\' || *tag == '"' ? "\\%c" : "%c", *tag);
		fprintf(lt.dump, "\"}");
	}
	fprintf(lt.dump, "]}\n");
	fflush(lt.dump);
}

/* ---------- public code */

void lt_lights_frame(struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count)
{
	int written, window = render.window_index;

	if (!lt_lights_wanted())
		return;
	written = lt_build(lights, keys, objects, owners, baked, game_count, count);
	/* (every float finite: none not a number or infinite reaches the pass) */
	{
		int index, word;

		for (index = 0; index < written; index++)
		{
			float *values = &lt.lights[index].position[0];

			for (word = 0; word < 28; word++)
			{
				if (!isfinite(values[word]))
					values[word] = 0.0f;
			}
		}
	}
	lt.frame++;
	xgpu_backend_lt_lights(lt.lights, written, window, lt.frame);
	lt_dump(written, window);
}

/* ---------- the objects' sun shadows */

boolean lt_lights_sun_shadow(long object_index)
{
	static int pass_sun = -1;
	struct scenario *scenario;
	struct object_datum const *object;
	struct tag_block const *block;
	char const *sky;
	struct lt_sky_light const *sun = NULL;
	real_vector3d toward, along;
	real_point3d center;
	real radius;
	long index;
	struct collision_result collision;

	if (pass_sun < 0)
	{
		char const *pass = getenv("HALO_LT_E1"), *given = getenv("HALO_LT_SUN");

		pass_sun = pass && *pass && strcmp(pass, "off") && strcmp(pass, "0") && strcmp(pass, "false") &&
			!(given && !strcmp(given, "0"));
	}
	if (!pass_sun || object_index == NONE)
		return FALSE;
	/* (only an object the ray-traced scene holds with a shape of its own,
	which the pass's sun rays meet at any height (lt/e1.frag's sun_share):
	not scenery baked as the structure is, not an item's box, not what the
	scene leaves out (a held weapon, a projectile); the others keep the
	game's shadow) */
	object = object_get(object_index);
	if (!object || object->object.type == _object_type_scenery || !world_rt_object_traced(object_index))
		return FALSE;
	/* (the sky's sun: its narrowest light) */
	scenario = global_scenario_get();
	if (!scenario || scenario->sky_references.count <= 0)
		return FALSE;
	sky = (char const *)scenario_get_sky(lt_sky_index());
	if (!sky)
		return FALSE;
	block = (struct tag_block const *)(sky + LT_SKY_LIGHTS_OFFSET);
	for (index = 0; block->address && index < block->count; index++)
	{
		struct lt_sky_light const *light = (struct lt_sky_light const *)block->address + index;

		if (light->power > 0.0f && (!sun || light->diameter < sun->diameter))
			sun = light;
	}
	if (!sun)
		return FALSE;
	/* (the object's middle in the sun, the structure between them none) */
	vector3d_from_euler_angles2d(&toward, &sun->direction);
	object_get_bounding_sphere(object_index, &center, &radius);
	along = toward;
	scale_vector3d(&along, 1024.0f, &along);
	return !collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&center, &along, NONE, &collision);
}
