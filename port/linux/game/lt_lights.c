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
/* a point light's k (a muzzle flash, an explosion, a projectile's light,
the port's own lights: lt_point_power_scale) from the classic light model's
own falloff (display.ray_traced_lighting off: d3d8_device.c's look[0],
xgpu_spirv.c's dynamic_light_sum_classic): the linear light it adds at d is
BRIGHTNESS x color^2.2 x W(d / r) / (1 + core d^2), core = max(CORE / r^2,
1 / CORE_MOST^2), W the range window; so the classic's light scales with
its reach, and one k a kind cannot match it at every reach
(~/dev/rt2-calibration.json's dynamic_power_scales) */
#define LT_LIGHTS_CLASSIC_BRIGHTNESS 1.7f
#define LT_LIGHTS_CLASSIC_CORE 12.0f
#define LT_LIGHTS_CLASSIC_CORE_MOST 0.6f
/* a sun's (a narrow sky light's) and a fill's (a sky light LT_LIGHTS_FILL_DIAMETER
across or more, b30's 50 degree one): ~/dev/rt2-calibration.json's
sun_power_scale and fill_power_scale (xgpu_vulkan_lt.c's, the same rule) */
#define LT_LIGHTS_SUN_POWER_SCALE 0.417f
#define LT_LIGHTS_FILL_POWER_SCALE 0.187f
#define LT_LIGHTS_FILL_DIAMETER 0.17453293f
/* a level lit live (lt_lights_real_sky): the sky's lights and its ambient
dome as the whole light, each group's k for each channel from the truth
renderer's multi-bounce render of b30 (432 samples a pixel, every bounce)
against its lightmap as the game shows it (pages ^2.2), the lightmap the
answer key only: the sun's, and the sky's (its wide lights and its dome:
the lightmap tool's sky alike), red, green and blue (R^2 0.536, 0.568,
0.620; one k for all, 0.457 0.475 0.594, R^2 0.524, 0.554, 0.614;
~/dev/rt2-calibration.json's real_sky colour_fit) */
static const float lt_lights_real_sun_k[3] = { 0.558f, 0.592f, 0.722f };
static const float lt_lights_real_sky_k[3] = { 0.324f, 0.338f, 0.506f };
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

int lt_lights_pass_on(void)
{
	static int pass = -2;

	if (pass == -2)
		pass = lt_switch("HALO_LT_E1");
	if (pass >= 0)
		return pass;
	return config_boolean("display.ray_traced_lighting") && halo_world_ao_ray_queries() &&
		halo_dynamic_lights_enhanced();
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

/* the backend told when the pass is turned off (a list with window -1:
xgpu_vulkan_lt_lights.c), once, after lists were sent */
static void lt_lights_announce(int sending)
{
	static int sent = 0;

	if (sending && !sent)
		platform_log("lt lights: the light list sent each frame%s%s (the light-transport pass on)",
			lt.dump_path[0] ? ", dumped to " : "", lt.dump_path);
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

/* the suns after count records; the count of all */
static int lt_suns(int count)
{
	struct scenario *scenario = global_scenario_get();
	int live;
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
	live = lt_lights_real_sky();
	block = (struct tag_block const *)(sky + LT_SKY_LIGHTS_OFFSET);
	for (index = 0; block->address && index < block->count && count < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct lt_sky_light const *light = (struct lt_sky_light const *)block->address + index;
		struct xgpu_stream_lt_light *out = &lt.lights[count];
		real_vector3d direction;
		real_euler_angles2d up_angles;
		real_vector3d up;

		real_euler_angles2d angles = light->direction;

		memset(out, 0, sizeof(*out));
		lt_sun_angles(sky_index, index, &angles);
		vector3d_from_euler_angles2d(&direction, &angles);
		/* (an up: the direction's, a quarter turn of pitch above it) */
		up_angles.yaw = angles.yaw;
		up_angles.pitch = angles.pitch + 0.5f * _pi;
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
		out->power_scale = light->diameter >= LT_LIGHTS_FILL_DIAMETER ? LT_LIGHTS_FILL_POWER_SCALE :
			LT_LIGHTS_SUN_POWER_SCALE;
		lt_copy3(out->color, light->color.red, light->color.green, light->color.blue);
		lt_copy3(out->power, light->color.red * light->power * out->power_scale,
			light->color.green * light->power * out->power_scale, light->color.blue * light->power * out->power_scale);
		/* (lit live: its group's k each channel, a wide one the sky's) */
		if (live)
		{
			float const *k = light->diameter >= LT_LIGHTS_FILL_DIAMETER ? lt_lights_real_sky_k : lt_lights_real_sun_k;

			out->flags |= XGPU_LT_LIGHT_REAL_SKY;
			out->power_scale = k[1];
			lt_copy3(out->power, light->color.red * light->power * k[0], light->color.green * light->power * k[1],
				light->color.blue * light->power * k[2]);
		}
		out->angular_diameter = light->diameter;
		count++;
	}
	/* (lit live: the sky's outdoor ambient radiosity, a dome of that
	radiance over the whole sphere (the lightmap tool's uniform sky; below
	the horizon the ground stands in front of it), forward up, its angular
	diameter the whole turn) */
	if (live && count < XGPU_STREAM_LT_LIGHTS_MAXIMUM)
	{
		struct xgpu_stream_lt_light *out = &lt.lights[count];
		real_rgb_color color;
		real power;

		memcpy(&color, sky + LT_SKY_OUTDOOR_AMBIENT_COLOR_OFFSET, sizeof(color));
		memcpy(&power, sky + LT_SKY_OUTDOOR_AMBIENT_POWER_OFFSET, sizeof(power));
		if (power > 0.0f && power < 1.0e6f)
		{
			memset(out, 0, sizeof(*out));
			out->id[0] = ((unsigned int)sky_index << 8) | 0xffu;
			out->id[1] = (unsigned int)XGPU_LT_LIGHT_SOURCE_SUN << 24;
			out->kind = XGPU_LT_LIGHT_DIRECTIONAL;
			out->flags = XGPU_LT_LIGHT_LIGHTMAP_DIRECT | XGPU_LT_LIGHT_REAL_SKY | XGPU_LT_LIGHT_SKY_DOME;
			out->object = 0xffffffffu;
			out->gel_slot = -1;
			out->gel_tag = -1;
			lt_copy3(out->forward, 0.0f, 0.0f, 1.0f);
			lt_copy3(out->up, 1.0f, 0.0f, 0.0f);
			lt_left(out);
			out->cone_inner = out->cone_outer = -1.0f;
			out->power_scale = lt_lights_real_sky_k[1];
			lt_copy3(out->color, color.red, color.green, color.blue);
			lt_copy3(out->power, color.red * power * lt_lights_real_sky_k[0], color.green * power * lt_lights_real_sky_k[1],
				color.blue * power * lt_lights_real_sky_k[2]);
			out->angular_diameter = _pi * 2.0f;
			count++;
		}
	}
	return count;
}

/* a point light's k: the classic's light at half its reach over the
pass's there at k = 1, BRIGHTNESS max(d^2, emitter^2) / (1 + core d^2) at
d = r / 2 (both have the same range window): so it adds what the classic
adds where most of what it lights is seen; tools/rt2_effect_k.py measured
the light each kind adds over its held frames' pixels against the classic's
(~/dev/rt2-calibration.json's dynamic_power_scales: the need over this k
0.57 to 1.2, about 1 on average; the classic's whole flux over the reach
was 0.8 to 1.6) */
static float lt_point_power_scale(float range, float emitter_radius)
{
	float core = LT_LIGHTS_CLASSIC_CORE / (range * range);
	float least = 1.0f / (LT_LIGHTS_CLASSIC_CORE_MOST * LT_LIGHTS_CLASSIC_CORE_MOST);
	float d2 = 0.25f * range * range, r2 = emitter_radius * emitter_radius;

	if (!(range > 0.0f))
		return 0.0f;
	core = core > least ? core : least;
	return LT_LIGHTS_CLASSIC_BRIGHTNESS * (d2 > r2 ? d2 : r2) / (1.0f + core * d2);
}

/* a light's k by its kind: a point's from its reach (lt_point_power_scale),
the rest (a flashlight, a lamp, a panel) the flashlight's */
static float lt_power_scale(struct xgpu_stream_lt_light const *light)
{
	if ((light->flags & XGPU_LT_LIGHT_FLASHLIGHT) || light->kind != XGPU_LT_LIGHT_POINT)
		return LT_LIGHTS_POWER_SCALE;
	return lt_point_power_scale(light->range, light->emitter_radius);
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
		{
			struct object_datum const *carrier;

			out->object = (unsigned int)(key & 0xffffffffull);
			/* (a projectile's own light, a needle's: a projectile's light, as
			the game's own projectiles' are flagged) */
			carrier = (struct object_datum const *)object_try_and_get((long)out->object);
			if (carrier && carrier->object.type == _object_type_projectile)
				out->flags |= XGPU_LT_LIGHT_PROJECTILE;
		}
		/* (its owner in every mode: the top parent of the object it is on,
		a held weapon's holder (world_rt_owner is the handle's arithmetic
		alone). dynamic_lights.c records one only with ray-traced shadows,
		and none for its own lights, the ammo counter's in its holder's
		hands among them. The pass's rays leave the owner out, and its
		lights never light the owner's own surfaces (lt/indirect.glsl's
		direct_one): a light on its carrier's model would light that model
		from a few centimetres) */
		out->owner = (unsigned int)owners[index];
		if (!out->owner && out->object != 0xffffffffu && object_try_and_get((long)out->object))
			out->owner = (unsigned int)world_rt_owner(object_get_ultimate_parent((long)out->object));
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

/* the frame's object lamps (world_rt.c's world_rt_lamps) after the list:
records of kind XGPU_LT_LIGHT_OBJECT_EMITTER, the frame's whole; the
count with them */
static int lt_object_lamps_append(struct xgpu_stream_lt_light *lights, int count, int maximum)
{
	float const (*corners)[9];
	float const (*radiance)[3];
	unsigned long const *owners;
	long lamps = world_rt_lamps(&corners, &radiance, &owners), index;

	for (index = 0; index < lamps && count < maximum; index++)
	{
		struct xgpu_stream_lt_light *out = &lights[count++];

		memset(out, 0, sizeof(*out));
		out->id[0] = (unsigned int)index;
		out->kind = XGPU_LT_LIGHT_OBJECT_EMITTER;
		out->object = (unsigned int)lamps;
		out->owner = (unsigned int)owners[index];
		out->gel_slot = -1;
		out->gel_tag = -1;
		memcpy(out->position, corners[index], sizeof(float) * 3);
		memcpy(out->forward, corners[index] + 3, sizeof(float) * 3);
		memcpy(out->up, corners[index] + 6, sizeof(float) * 3);
		memcpy(out->power, radiance[index], sizeof(float) * 3);
		out->cone_inner = out->cone_outer = -1.0f;
		out->power_scale = 1.0f;
	}
	return count;
}

/* ---------- public code */

void lt_lights_frame(struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count)
{
	int written, window = render.window_index;

	lt_lights_announce(lt_lights_wanted());
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
	/* (and this frame's share of the level's light sources' triangles, after
	the lights: not in the dump) */
	xgpu_backend_lt_lights(lt.lights, lt_object_lamps_append(lt.lights, lt_emitters_append(lt.lights, written,
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
