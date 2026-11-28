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
  (lt_power_scale: the flashlight's measured, ~/dev/rt2-calibration.json's
  flashlight_power_scale, which a lamp and a panel share; a point light's
  from its reach and emitter, the classic light model's own light at half
  the reach, lt_point_power_scale and dynamic_power_scales). The gel is the
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
#include "lt_lights_inputs.h"
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
/* (the same: the frame's inputs, bytes of them, xgpu_stream.h's LT_INPUTS) */
void xgpu_backend_lt_inputs(const void *inputs, unsigned int bytes, int window, unsigned int frame);
/* (object_lights.c) */
extern struct data_array *light_data;

/* (the k's, the suns' and the sky's, and the emitters' radii: lt_lights_build.c, where the list is built
from what this file gathers) */
/* the share of the BSP's clusters that must see a lit sky for the level to
be lit live: Bungie's lightmap tool lit only those (a sky light's "affects
exteriors"; the outdoor ambient radiosity "for exterior clusters"), so the
tags' lights explain the bake there and nowhere else */
#define LT_LIGHTS_REAL_SKY_SHARE 0.5f
/* the sky tag's outdoor ambient radiosity colour and power, a sky light's
flags (bit 0: it lights exterior clusters) */
#define LT_SKY_OUTDOOR_AMBIENT_COLOR_OFFSET 0x48
#define LT_SKY_OUTDOOR_AMBIENT_POWER_OFFSET 0x54
#define LT_SKY_LIGHT_AFFECTS_EXTERIORS_BIT 0
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
	/* the frame's inputs (lt_lights_inputs.h): the header, its lights, its
	lamps, one record */
	union
	{
		struct lt_inputs head;
		unsigned char bytes[sizeof(struct lt_inputs) + LT_INPUTS_LIGHTS_MOST * sizeof(struct lt_input_light) +
			LT_INPUTS_LAMPS_MOST * sizeof(struct lt_input_lamp)];
	} inputs;
	/* the sky lights.json names (the most clusters'), for the BSP it was found for */
	void const *sky_bsp;
	short sky_index;
	/* whether that BSP is lit live (lt_lights_real_sky): -1 not known */
	void const *real_bsp;
	int live;
} lt = { -1 };

/* ---------- the list */

int config_boolean(const char *name);
int halo_world_ao_ray_queries(void);

/* an environment switch's value: -1 not given, else on (1) or off (0) */
static int lt_switch(char const *name)
{
	char const *given = getenv(name);

	if (!given || !*given)
		return -1;
	return strcmp(given, "off") && strcmp(given, "0") && strcmp(given, "false");
}

int lt_lights_pass_chosen(int lighting, int enhanced)
{
	static int pass = -2;

	if (pass == -2)
		pass = lt_switch("HALO_LT_E1");
	if (pass >= 0)
		return pass;
	return lighting && halo_world_ao_ray_queries() && halo_dynamic_lights_enhanced_chosen(enhanced);
}

int lt_lights_pass_on(void)
{
	return lt_lights_pass_chosen(config_boolean("display.ray_traced_lighting"), -1);
}

/* whether the list is built and sent: HALO_LT_LIGHTS (or HALO_LT_LIGHTS_DUMP)
where given, else the pass on (lt_lights_pass_on) */
static int lt_lights_wanted(void)
{
	if (lt.wanted < 0)
	{
		char const *dump = getenv("HALO_LT_LIGHTS_DUMP");

		lt.wanted = lt_switch("HALO_LT_LIGHTS") >= 0 ? 2 + lt_switch("HALO_LT_LIGHTS") : 0;
		if (dump && *dump)
		{
			snprintf(lt.dump_path, sizeof(lt.dump_path), "%s/lt_lights.jsonl", dump);
			lt.wanted = 3;
		}
	}
	return lt.wanted ? lt.wanted == 3 : lt_lights_pass_on();
}

/* the backend told when the pass is turned on (a list with window -2, before
the first list) or off (window -1: xgpu_vulkan_lt_lights.c), once as it
changes */
static void lt_lights_announce(int sending)
{
	static int sent = 0;

	if (sending && !sent)
	{
		platform_log("lt lights: the light list sent each frame%s%s (the light-transport pass on)",
			lt.dump_path[0] ? ", dumped to " : "", lt.dump_path);
		xgpu_backend_lt_lights(NULL, 0, -2, lt.frame);
	}
	if (!sending && sent)
	{
		platform_log("lt lights: the light-transport pass off");
		xgpu_backend_lt_lights(NULL, 0, -1, lt.frame);
	}
	sent = sending;
}

void lt_lights_update(void)
{
	lt_lights_announce(lt_lights_wanted());
}

boolean lt_lights_sent(void)
{
	return lt_lights_wanted() != 0;
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

/* whether a sky lights clusters live: an exterior-lighting light with power,
or an outdoor ambient dome */
static int lt_sky_lit(short sky_index)
{
	char const *sky = (char const *)scenario_get_sky(sky_index);
	struct tag_block const *block;
	real power;
	long index;

	if (!sky)
		return FALSE;
	memcpy(&power, sky + LT_SKY_OUTDOOR_AMBIENT_POWER_OFFSET, sizeof(power));
	if (power > 0.0f && power < 1.0e6f)
		return TRUE;
	block = (struct tag_block const *)(sky + LT_SKY_LIGHTS_OFFSET);
	for (index = 0; block->address && index < block->count; index++)
	{
		struct lt_sky_light const *light = (struct lt_sky_light const *)block->address + index;

		if (light->power > 0.0f && TEST_FLAG(light->flags, LT_SKY_LIGHT_AFFECTS_EXTERIORS_BIT))
			return TRUE;
	}
	return FALSE;
}

/* whether the view is in an exterior cluster, one that sees a lit sky (the
camera's cluster, structure_visibility.c's render.visible_sky_index): an
outdoor level's interiors (b30's corridors) keep their lightmap while the
view is in them, their light the emitters' the tags lack until step 2
(~/dev/rt2-realtime-design.md section 9: the view's cluster now, each
surface's own cluster next) */
static int lt_view_exterior(void)
{
	return render.visible_sky_index != NONE && lt_sky_lit(render.visible_sky_index);
}

/* whether the level (its BSP) is lit live, the lightmap dropped: its
clusters that see a lit sky (exterior clusters, the only ones Bungie's
lightmap tool lit with the sky's lights and dome) LT_LIGHTS_REAL_SKY_SHARE
of them or more. Interior clusters' light came from emitters the tags
mostly lack (a10's cryo deck: none), so a level made mostly of them keeps
its lightmap until those are solved (~/dev/rt2-realtime-design.md section
9). A BSP whose light sources are solved (lt_emitters_live) drops its
lightmap in every cluster: its lamps were fitted to the bake less the sky's
share, so the sky is then the sky's whole light (the real k, the dome) in an
interior view too, as the pass's LIGHTS_REAL takes it (xgpu_vulkan_lt.c: set
by the solved lamps whatever the view), never the lightmap-share k under a
pass that no longer takes that share off. HALO_LT_REAL_SKY=0 or 1 overrides
it (checking) */
int lt_lights_real_sky(void)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	struct scenario *scenario = global_scenario_get();
	char const *given = getenv("HALO_LT_REAL_SKY");
	long index, exterior = 0, total = 0;

	if (given && *given)
		return atoi(given) != 0;
	if (!bsp || !scenario)
		return FALSE;
	if (bsp == lt.real_bsp)
		return lt.live && (lt_emitters_live() || lt_view_exterior());
	lt.real_bsp = bsp;
	for (index = 0; bsp->clusters.address && index < bsp->clusters.count; index++)
	{
		struct structure_cluster const *cluster = (struct structure_cluster const *)bsp->clusters.address + index;

		total++;
		if (cluster->sky_index >= 0 && cluster->sky_index < scenario->sky_references.count &&
			lt_sky_lit(cluster->sky_index))
			exterior++;
	}
	lt.live = total > 0 && (float)exterior >= LT_LIGHTS_REAL_SKY_SHARE * (float)total;
	/* (or its interior's light sources solved from its lightmap: the level's
	own lamps traced live, lt_emitters.c) */
	if (!lt.live && lt_emitters_live())
	{
		lt.live = TRUE;
		platform_log("lt lights: %ld of the BSP's %ld clusters see a lit sky; its light sources solved: lit live "
			"(its lamps, the sky's lights and dome, the lightmap dropped)", exterior, total);
		return lt.live;
	}
	platform_log("lt lights: %ld of the BSP's %ld clusters see a lit sky: %s", exterior, total,
		lt.live ? "lit live (the sky's lights and dome, the lightmap dropped)" : "the lightmap kept");
	return lt.live && (lt_emitters_live() || lt_view_exterior());
}

/* HALO_LT_SUN_ANGLES=<yaw>,<pitch> (degrees; checking): the sky's first
light's direction in place of the tag's. b30's bake's sun shadows do not
match its tag's sun (yaw 50, pitch 35) and could not be matched by a
direction alone (~/dev/rt2-realtime-design.md section 9.3): the tag's is
kept */
static void lt_sun_angles(short sky_index, long light_index, real_euler_angles2d *angles)
{
	char const *given = getenv("HALO_LT_SUN_ANGLES");
	float yaw, pitch;

	(void)sky_index;
	if (given && light_index == 0 && sscanf(given, "%f,%f", &yaw, &pitch) == 2)
	{
		angles->yaw = yaw * (_pi / 180.0f);
		angles->pitch = pitch * (_pi / 180.0f);
	}
}

/* ---------- the inputs (lt_lights_inputs.h): what the list's building reads of the game */

/* the sky's part: the sky lights.json takes, whether the level is lit live,
its lights (their directions as render_sky.c aims its lens flare) and its
outdoor ambient radiosity */
static void lt_gather_sky(struct lt_inputs *in)
{
	struct scenario *scenario = global_scenario_get();
	short sky_index;
	char const *sky;
	struct tag_block const *block;
	long index;

	in->has_sky = FALSE;
	in->sky_light_count = 0;
	if (!scenario || scenario->sky_references.count <= 0)
		return;
	sky_index = lt_sky_index();
	sky = (char const *)scenario_get_sky(sky_index);
	if (!sky)
		return;
	in->has_sky = TRUE;
	in->sky_index = sky_index;
	in->live = lt_lights_real_sky();
	block = (struct tag_block const *)(sky + LT_SKY_LIGHTS_OFFSET);
	for (index = 0; block->address && index < block->count && index < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct lt_sky_light const *light = (struct lt_sky_light const *)block->address + index;
		struct lt_input_sky_light *out;
		real_vector3d direction;
		real_euler_angles2d up_angles;
		real_vector3d up;
		real_euler_angles2d angles = light->direction;

		if (index >= LT_INPUTS_SKY_LIGHTS)
		{
			/* (more than the inputs hold: not built from them) */
			in->version = 0;
			break;
		}
		out = &in->sky_lights[in->sky_light_count++];
		lt_sun_angles(sky_index, index, &angles);
		vector3d_from_euler_angles2d(&direction, &angles);
		/* (an up: the direction's, a quarter turn of pitch above it) */
		up_angles.yaw = angles.yaw;
		up_angles.pitch = angles.pitch + 0.5f * _pi;
		vector3d_from_euler_angles2d(&up, &up_angles);
		out->direction[0] = direction.i;
		out->direction[1] = direction.j;
		out->direction[2] = direction.k;
		out->up[0] = up.i;
		out->up[1] = up.j;
		out->up[2] = up.k;
		out->color[0] = light->color.red;
		out->color[1] = light->color.green;
		out->color[2] = light->color.blue;
		out->power = light->power;
		out->diameter = light->diameter;
		out->flags = (unsigned int)light->flags;
	}
	memcpy(in->ambient_color, sky + LT_SKY_OUTDOOR_AMBIENT_COLOR_OFFSET, sizeof(in->ambient_color));
	memcpy(&in->ambient_power, sky + LT_SKY_OUTDOOR_AMBIENT_POWER_OFFSET, sizeof(in->ambient_power));
}

/* the lights' part: each light as drawn and what its rasterizer light, its
tag, its datum, the gel pass and its object say of it */
static void lt_gather_lights(struct lt_inputs *in, struct lt_input_light *out_lights,
	struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count)
{
	long index;

	in->light_count = (int)count;
	in->game_count = (int)game_count;
	if (count > LT_INPUTS_LIGHTS_MOST)
	{
		in->version = 0;
		in->light_count = 0;
		return;
	}
	for (index = 0; index < count; index++)
	{
		struct halo_dynamic_light const *light = &lights[index];
		struct lt_input_light *out = &out_lights[index];
		unsigned long long key = keys[index];
		unsigned int source = (unsigned int)(key >> 56);
		long carrier = objects[index] != NONE ? objects[index] : (source == 2 || source == 3) ? (long)(key & 0xffffffffull) :
			NONE;
		real_vector3d forward, up, left;

		memset(out, 0, sizeof(*out));
		memcpy(out->position, light->position, sizeof(out->position));
		memcpy(out->axis, light->axis, sizeof(out->axis));
		out->radius = light->radius;
		memcpy(out->color, light->color, sizeof(out->color));
		out->flags = (unsigned int)light->flags;
		out->cone_scale = light->cone_scale;
		out->cone_offset = light->cone_offset;
		out->key[0] = (unsigned int)(key & 0xffffffffull);
		out->key[1] = (unsigned int)(key >> 32);
		out->object = (int)objects[index];
		out->owner = (unsigned int)owners[index];
		out->baked = baked[index] != 0;
		out->gel_index = NONE;
		if (index < rasterizer_lights.light_count)
		{
			struct rasterizer_light_submit_parameters const *drawn = &rasterizer_lights.lights[index];
			struct lt_light_definition const *definition =
				(struct lt_light_definition const *)(void const *)drawn->definition;

			out->has_drawn = TRUE;
			out->drawn_up[0] = drawn->up.i;
			out->drawn_up[1] = drawn->up.j;
			out->drawn_up[2] = drawn->up.k;
			if (definition)
			{
				out->has_definition = TRUE;
				out->falloff_cosine = definition->runtime_cosine_falloff_angle;
				out->cutoff_cosine = definition->runtime_cosine_cutoff_angle;
				out->definition_flags = (unsigned int)definition->flags;
				out->gel_index = (int)definition->gel_map.index;
			}
		}
		if (source == XGPU_LT_LIGHT_SOURCE_GAME && light_data)
		{
			struct lt_light_datum const *datum = datum_try_and_get(light_data, (long)out->key[0]);

			out->first_person = datum && TEST_FLAG(datum->flags, LT_LIGHT_DATUM_FIRST_PERSON_BIT);
		}
		if (index < game_count && rasterizer_environment_port_gel(index, &forward, &up, &left, NULL, NULL))
		{
			real_point3d lamp;

			out->has_gel = TRUE;
			memcpy(out->gel_forward, &forward, sizeof(out->gel_forward));
			memcpy(out->gel_up, &up, sizeof(out->gel_up));
			memcpy(out->gel_left, &left, sizeof(out->gel_left));
			if ((light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT) && objects[index] != NONE &&
				first_person_weapon_flashlight_lamp(objects[index], &lamp))
			{
				out->has_lamp = TRUE;
				memcpy(out->lamp, &lamp, sizeof(out->lamp));
			}
		}
		if (carrier != NONE)
		{
			struct object_datum const *object = (struct object_datum const *)object_try_and_get(carrier);

			out->carrier_exists = object != NULL;
			out->carrier_projectile = object && object->object.type == _object_type_projectile;
			out->carrier_owner = object ? (unsigned int)world_rt_owner(object_get_ultimate_parent(carrier)) : 0;
		}
	}
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

/* the object lamps' part (world_rt.c's world_rt_lamps, world_rt_lamp_sides)
and the camera their choice is by: every lamp where lt_lights_build.c ranks
them, else the first it sends */
static void lt_gather_lamps(struct lt_inputs *in, struct lt_input_lamp *out)
{
	float const (*corners)[9];
	float const (*radiance)[3];
	unsigned long const *owners;
	long lamps = world_rt_lamps(&corners, &radiance, &owners), index, taken;
	unsigned char const *sides = world_rt_lamp_sides();

	in->camera[0] = render.camera.position.x;
	in->camera[1] = render.camera.position.y;
	in->camera[2] = render.camera.position.z;
	in->lamps_in_world = (int)lamps;
	in->has_sides = sides != NULL;
	taken = lamps <= LT_INPUTS_LAMPS_MOST ? lamps : XGPU_STREAM_LT_LIGHTS_MAXIMUM < LT_INPUTS_LAMPS_MOST ?
		XGPU_STREAM_LT_LIGHTS_MAXIMUM : LT_INPUTS_LAMPS_MOST;
	for (index = 0; index < taken; index++)
	{
		memcpy(out[index].corners, corners[index], sizeof(out[index].corners));
		memcpy(out[index].radiance, radiance[index], sizeof(out[index].radiance));
		out[index].owner = (unsigned int)owners[index];
		out[index].sides = sides ? sides[index] : 0;
	}
	in->lamp_count = (int)taken;
}

typedef char lt_lamp_additive_assert[WORLD_RT_LAMP_ADDITIVE == 4 ? 1 : -1];

/* whether the inputs ride in the stream (xgpu_stream.h's LT_INPUTS): while
the frames are recorded (HALO_RENDER_RECORD), for the replay to build the
list again, or HALO_LT_INPUTS=1 */
static int lt_inputs_sent(void)
{
	static int sent = -1;

	if (sent < 0)
	{
		char const *record = getenv("HALO_RENDER_RECORD");

		sent = lt_switch("HALO_LT_INPUTS") >= 0 ? lt_switch("HALO_LT_INPUTS") : record && *record;
	}
	return sent;
}

/* ---------- public code */

void lt_lights_frame(struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count)
{
	int written, window = render.window_index;
	struct lt_inputs *in = &lt.inputs.head;
	struct lt_input_light *in_lights = (struct lt_input_light *)(in + 1);
	struct lt_input_lamp *in_lamps;

	lt_lights_announce(lt_lights_wanted());
	if (!lt_lights_wanted())
		return;
	/* (what the building reads of the game, then the list built from it
	alone: lt_lights_build.c) */
	memset(in, 0, sizeof(*in));
	in->version = LT_INPUTS_VERSION;
	in->window = window;
	lt_gather_sky(in);
	lt_gather_lights(in, in_lights, lights, keys, objects, owners, baked, game_count, count);
	in_lamps = (struct lt_input_lamp *)(in_lights + in->light_count);
	lt_gather_lamps(in, in_lamps);
	in->bytes = (unsigned int)(sizeof(*in) + in->light_count * sizeof(*in_lights) + in->lamp_count * sizeof(*in_lamps));
	written = lt_lights_build_list(in, in_lights, lt.lights);
	lt.frame++;
	if (lt_inputs_sent())
		xgpu_backend_lt_inputs(in, in->bytes, window, lt.frame);
	/* (and this frame's share of the level's light sources' triangles, after
	the lights: not in the dump) */
	xgpu_backend_lt_lights(lt.lights, lt_lights_build_lamps(in, in_lamps, lt.lights, lt_emitters_append(lt.lights, written,
		XGPU_STREAM_LT_LIGHTS_MAXIMUM), XGPU_STREAM_LT_LIGHTS_MAXIMUM), window, lt.frame);
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
		char const *given = getenv("HALO_LT_SUN");

		pass_sun = !(given && !strcmp(given, "0"));
	}
	if (!pass_sun || object_index == NONE || !lt_lights_pass_on())
		return FALSE;
	/* (a level lit live: every shadow the traced sun's and sky's, never the
	game's projected one, whatever the object's shape in the scene) */
	if (lt_lights_real_sky())
		return object_try_and_get(object_index) != NULL;
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
