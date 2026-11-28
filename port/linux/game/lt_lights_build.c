/*
LT_LIGHTS_BUILD.C

The light list built from its inputs (lt_lights_inputs.h): lt_lights.c's
rules for each record (lt_lights.c's comment says what each member is), the
suns, and the object lamps a view takes, with nothing of the game's state
read here but what lt_lights.c gathered into the inputs. So the same code
runs in the game (lt_lights.c: gather, build, send) and in the replay, built
alone as the light-list unit (LT_LIGHTS_UNIT: lt_lights_unit.so, with musl's
powf and qsort, the game's own, so it computes the game's numbers bit for
bit) and reloaded without recording again (HALO_LT_UNIT; tools/rt2_bench.py
run --unit).

Plain C (gnu89, as the game's), no game headers: what it needs of the game's
math (normalize3d, cross_product3d) is here, the same operations in the same
order.
*/

/* (lt_lights.h's declarations of the game's side; no game headers here) */
typedef int boolean;

#include "lt_lights.h"
#include "lt_lights_inputs.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifdef LT_LIGHTS_UNIT
/* (the game's C library's, musl's, built into the unit: the host's would
round some powers otherwise and order equal lamps otherwise) */
float lt_musl_powf(float x, float y);
void lt_musl_qsort(void *base, size_t count, size_t width, int (*compare)(void const *, void const *));
#define powf lt_musl_powf
#define qsort lt_musl_qsort
#endif

#ifndef NONE
#define NONE -1
#endif

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
/* the emitters' radii (world units): E1's flashlight disk, then
dynamic_lights.c's shadow disks' (SHADOWED_DISK_*) */
#define LT_LIGHTS_FLASHLIGHT_DISK 0.01f
#define LT_LIGHTS_PROJECTILE_DISK 0.05f
#define LT_LIGHTS_DISK_SCALE 0.05f
#define LT_LIGHTS_DISK_LEAST 0.1f
#define LT_LIGHTS_DISK_MOST 0.5f
/* the light tag's flags (lt_lights.c's lt_light_definition) */
#define LT_DEFINITION_NO_SPECULAR_BIT 1
#define LT_DEFINITION_DONT_LIGHT_OWN_OBJECT_BIT 2
/* (world_rt.h's WORLD_RT_LAMP_ADDITIVE: a lamp's sides' bit) */
#define LT_LAMP_SIDE_ADDITIVE 4
/* (halo_dynamic_lights.h's HALO_DYNAMIC_LIGHTS_FLASHLIGHTS: the backend's
flashlight table's slots) */
#define LT_FLASHLIGHT_SLOTS 4
/* the object lamps a view's pass takes (xgpu_vulkan_lt_lights.h's
XGPU_VULKAN_LT_OBJECT_LAMPS_MAXIMUM) */
#define LT_OBJECT_LAMPS_SENT 512

/* ---------- the game's math, as real_math.h has it */

struct lt_vector
{
	float i, j, k;
};

static float lt_normalize(struct lt_vector *v)
{
	float magnitude = (float)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);

	if (!(0.0001f > fabs(magnitude - 0.f)))
	{
		float c = 1.f / magnitude;

		v->i = c * v->i;
		v->j = c * v->j;
		v->k = c * v->k;
	}
	else
		magnitude = 0.f;
	return magnitude;
}

static void lt_cross(struct lt_vector const *a, struct lt_vector const *b, struct lt_vector *result)
{
	float k = a->i * b->j - a->j * b->i;
	float j = a->k * b->i - a->i * b->k;
	float i = a->j * b->k - a->k * b->j;

	result->i = i;
	result->j = j;
	result->k = k;
}

/* ---------- the list */

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
	struct lt_vector forward, up, left;

	forward.i = out->forward[0];
	forward.j = out->forward[1];
	forward.k = out->forward[2];
	up.i = out->up[0];
	up.j = out->up[1];
	up.k = out->up[2];
	lt_cross(&forward, &up, &left);
	lt_normalize(&left);
	lt_copy3(out->left, left.i, left.j, left.k);
}

/* the suns after count records; the count of all */
static int lt_suns(struct lt_inputs const *in, struct xgpu_stream_lt_light *lights, int count)
{
	int live = in->live, index;
	short sky_index = (short)in->sky_index;

	if (!in->has_sky)
		return count;
	for (index = 0; index < in->sky_light_count && count < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct lt_input_sky_light const *light = &in->sky_lights[index];
		struct xgpu_stream_lt_light *out = &lights[count];

		memset(out, 0, sizeof(*out));
		out->id[0] = ((unsigned int)sky_index << 8) | (unsigned int)(index & 0xff);
		out->id[1] = (unsigned int)XGPU_LT_LIGHT_SOURCE_SUN << 24;
		out->kind = XGPU_LT_LIGHT_DIRECTIONAL;
		out->flags = XGPU_LT_LIGHT_LIGHTMAP_DIRECT;
		out->object = 0xffffffffu;
		out->gel_slot = -1;
		out->gel_tag = -1;
		lt_copy3(out->forward, light->direction[0], light->direction[1], light->direction[2]);
		lt_copy3(out->up, light->up[0], light->up[1], light->up[2]);
		lt_left(out);
		out->cone_inner = out->cone_outer = -1.0f;
		out->power_scale = light->diameter >= LT_LIGHTS_FILL_DIAMETER ? LT_LIGHTS_FILL_POWER_SCALE :
			LT_LIGHTS_SUN_POWER_SCALE;
		lt_copy3(out->color, light->color[0], light->color[1], light->color[2]);
		lt_copy3(out->power, light->color[0] * light->power * out->power_scale,
			light->color[1] * light->power * out->power_scale, light->color[2] * light->power * out->power_scale);
		/* (lit live: its group's k each channel, a wide one the sky's) */
		if (live)
		{
			float const *k = light->diameter >= LT_LIGHTS_FILL_DIAMETER ? lt_lights_real_sky_k : lt_lights_real_sun_k;

			out->flags |= XGPU_LT_LIGHT_REAL_SKY;
			out->power_scale = k[1];
			lt_copy3(out->power, light->color[0] * light->power * k[0], light->color[1] * light->power * k[1],
				light->color[2] * light->power * k[2]);
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
		struct xgpu_stream_lt_light *out = &lights[count];
		float const *color = in->ambient_color;
		float power = in->ambient_power;

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
			lt_copy3(out->color, color[0], color[1], color[2]);
			lt_copy3(out->power, color[0] * power * lt_lights_real_sky_k[0], color[1] * power * lt_lights_real_sky_k[1],
				color[2] * power * lt_lights_real_sky_k[2]);
			out->angular_diameter = 3.14159265358979323846f * 2.0f;
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

static unsigned long long lt_key(struct lt_input_light const *light)
{
	return ((unsigned long long)light->key[1] << 32) | (unsigned long long)light->key[0];
}

/* the rasterizer light a copy (key) is of: its index, or NONE */
static long lt_copy_parent(struct lt_input_light const *lights, long game_count, unsigned long long key)
{
	long index;

	for (index = 0; index < game_count; index++)
	{
		unsigned long long other = lt_key(&lights[index]);

		if ((other >> 56) == XGPU_LT_LIGHT_SOURCE_GAME && (other & 0xffffffffull) == (key & 0xffffffffull))
			return index;
	}
	return NONE;
}

/* the lights' records and the suns', every float finite: the count */
int lt_lights_build_list(struct lt_inputs const *in, struct lt_input_light const *lights,
	struct xgpu_stream_lt_light *records)
{
	long index, game_count = in->game_count, count = in->light_count;
	int written = 0, flashlights = 0;

	for (index = 0; index < count && written < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		struct lt_input_light const *light = &lights[index];
		struct xgpu_stream_lt_light *out = &records[written];
		unsigned long long key = lt_key(light);
		unsigned int source = (unsigned int)(key >> 56);
		long drawn_index = index < game_count ? index :
			source == XGPU_LT_LIGHT_SOURCE_COPY ? lt_copy_parent(lights, game_count, key) : NONE;
		struct lt_input_light const *drawn = drawn_index != NONE && lights[drawn_index].has_drawn ?
			&lights[drawn_index] : NULL;
		struct lt_input_light const *definition = drawn && drawn->has_definition ? drawn : NULL;
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
				if (records[other].id[0] == out->id[0] && (records[other].id[1] >> 24) == XGPU_LT_LIGHT_SOURCE_COPY)
					number++;
			}
			out->id[1] = ((unsigned int)XGPU_LT_LIGHT_SOURCE_COPY << 24) | number;
		}
		out->flags |= light->flags & 0x1fu;
		if (light->baked)
			out->flags |= XGPU_LT_LIGHT_BAKED;
		out->object = light->object != NONE ? (unsigned int)light->object : 0xffffffffu;
		/* (one of the port's own lights (light_sources.c) on an object, its
		key's kind 2, or a firer's flames, 3: that object's, its handle in the
		key; it is a light on an object (a bolt's, a thruster's glow), whose k
		is a point's on an object, not an explosion's: lt_power_scale) */
		if ((source == 2 || source == 3) && out->object == 0xffffffffu)
		{
			out->object = (unsigned int)(key & 0xffffffffull);
			/* (a projectile's own light, a needle's: a projectile's light, as
			the game's own projectiles' are flagged) */
			if (light->carrier_exists && light->carrier_projectile)
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
		out->owner = light->owner;
		if (!out->owner && out->object != 0xffffffffu && light->carrier_exists)
			out->owner = light->carrier_owner;
		out->gel_slot = -1;
		out->gel_tag = -1;
		lt_copy3(out->position, light->position[0], light->position[1], light->position[2]);
		lt_copy3(out->emitter, light->position[0], light->position[1], light->position[2]);
		lt_copy3(out->forward, light->axis[0], light->axis[1], light->axis[2]);
		if (drawn)
			lt_copy3(out->up, drawn->drawn_up[0], drawn->drawn_up[1], drawn->drawn_up[2]);
		else
			lt_copy3(out->up, 0.0f, 0.0f, 1.0f);
		out->range = light->radius;
		lt_copy3(out->color, light->color[0], light->color[1], light->color[2]);
		out->cone_inner = out->cone_outer = -1.0f;
		out->kind = XGPU_LT_LIGHT_POINT;
		if (definition)
		{
			if (definition->falloff_cosine != -1.0f)
			{
				out->kind = XGPU_LT_LIGHT_SPOT;
				out->cone_inner = definition->falloff_cosine;
				out->cone_outer = definition->cutoff_cosine;
			}
			if (definition->definition_flags & (1u << LT_DEFINITION_NO_SPECULAR_BIT))
				out->flags |= XGPU_LT_LIGHT_NO_SPECULAR;
			if (definition->definition_flags & (1u << LT_DEFINITION_DONT_LIGHT_OWN_OBJECT_BIT))
				out->flags |= XGPU_LT_LIGHT_NOT_OWN_OBJECT;
			if (definition->gel_index != NONE)
			{
				out->flags |= XGPU_LT_LIGHT_GEL;
				out->gel_tag = definition->gel_index;
			}
		}
		else if (light->flags & XGPU_LT_LIGHT_PANEL_FLAG)
			out->kind = XGPU_LT_LIGHT_PANEL;
		else if (light->cone_scale > 0.0f)
		{
			/* (an own light's cone: scale (cos - outer), offset -scale outer) */
			out->kind = XGPU_LT_LIGHT_SPOT;
			out->cone_outer = -light->cone_offset / light->cone_scale;
			out->cone_inner = out->cone_outer + 1.0f / light->cone_scale;
		}
		/* (the light datum's own flag: on the first-person weapon) */
		if (source == XGPU_LT_LIGHT_SOURCE_GAME && light->first_person)
			out->flags |= XGPU_LT_LIGHT_FIRST_PERSON;
		/* (a gel's frame as the gel pass turns it; the flashlights' slots in
		the order dynamic_lights_flashlights gives them) */
		if (index < game_count && light->has_gel)
		{
			lt_copy3(out->forward, light->gel_forward[0], light->gel_forward[1], light->gel_forward[2]);
			lt_copy3(out->up, light->gel_up[0], light->gel_up[1], light->gel_up[2]);
			lt_copy3(out->left, light->gel_left[0], light->gel_left[1], light->gel_left[2]);
			if ((light->flags & XGPU_LT_LIGHT_FLASHLIGHT) && !(light->flags & XGPU_LT_LIGHT_MODELS_ONLY) &&
				flashlights < LT_FLASHLIGHT_SLOTS)
			{
				out->gel_slot = flashlights++;
				if (light->object != NONE && light->has_lamp)
					lt_copy3(out->emitter, light->lamp[0], light->lamp[1], light->lamp[2]);
			}
		}
		else
			lt_left(out);
		/* (the emitter's size) */
		disk = light->radius * LT_LIGHTS_DISK_SCALE;
		disk = disk < LT_LIGHTS_DISK_LEAST ? LT_LIGHTS_DISK_LEAST : disk > LT_LIGHTS_DISK_MOST ? LT_LIGHTS_DISK_MOST : disk;
		if (light->flags & XGPU_LT_LIGHT_FLASHLIGHT)
			disk = LT_LIGHTS_FLASHLIGHT_DISK;
		else if (out->kind == XGPU_LT_LIGHT_PANEL)
			disk = light->cone_offset > 0.0f ? sqrtf(light->cone_offset) : LT_LIGHTS_DISK_LEAST;
		else if (light->flags & XGPU_LT_LIGHT_PROJECTILE)
			disk = LT_LIGHTS_PROJECTILE_DISK;
		out->emitter_radius = disk;
		/* (the power: its kind's k, once its kind, flags and object are
		known) */
		out->power_scale = lt_power_scale(out);
		for (channel = 0; channel < 3; channel++)
			out->power[channel] = lt_linear(light->color[channel]) * out->power_scale;
		written++;
	}
	written = lt_suns(in, records, written);
	/* (every float finite: none not a number or infinite reaches the pass) */
	{
		int word;

		for (index = 0; index < written; index++)
		{
			float *values = &records[index].position[0];

			for (word = 0; word < 28; word++)
			{
				if (!isfinite(values[word]))
					values[word] = 0.0f;
			}
		}
	}
	return written;
}

/* ---------- the object lamps */

/* a lamp's importance to a view: its power (luminance by area) over its
distance squared from the camera, no nearer than its own size */
static float lt_object_lamp_importance(float const corners[9], float const radiance[3], float const eye[3])
{
	float e1[3], e2[3], c[3], middle[3], d2, area;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		e1[axis] = corners[3 + axis] - corners[axis];
		e2[axis] = corners[6 + axis] - corners[axis];
		middle[axis] = (corners[axis] + corners[3 + axis] + corners[6 + axis]) / 3.0f - eye[axis];
	}
	c[0] = e1[1] * e2[2] - e1[2] * e2[1];
	c[1] = e1[2] * e2[0] - e1[0] * e2[2];
	c[2] = e1[0] * e2[1] - e1[1] * e2[0];
	area = 0.5f * sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
	d2 = middle[0] * middle[0] + middle[1] * middle[1] + middle[2] * middle[2];
	return (0.2126f * radiance[0] + 0.7152f * radiance[1] + 0.0722f * radiance[2]) * area / (d2 > area ? d2 : area);
}

static float const *lt_object_lamp_order_importances;

static int lt_object_lamp_order(void const *a, void const *b)
{
	float x = lt_object_lamp_order_importances[*(long const *)a], y = lt_object_lamp_order_importances[*(long const *)b];

	return x > y ? -1 : x < y ? 1 : 0;
}

/* the frame's object lamps after the list: records of kind
XGPU_LT_LIGHT_OBJECT_EMITTER, every object's where the pass has room, else
those that matter most to this view (a far Pelican's strips do not take a
near shield's room); the count with them */
int lt_lights_build_lamps(struct lt_inputs const *in, struct lt_input_lamp const *taken_lamps,
	struct xgpu_stream_lt_light *lights, int count, int maximum)
{
	static float *importances;
	static long *order;
	long lamps = in->lamps_in_world, index, taken;
	int sides = in->has_sides, sorted = 0;

	if (maximum - count > LT_OBJECT_LAMPS_SENT)
		maximum = count + LT_OBJECT_LAMPS_SENT;
	if (lamps > maximum - count)
	{
		if (!importances)
			importances = malloc(sizeof(*importances) * 4096);
		if (!order)
			order = malloc(sizeof(*order) * 4096);
		if (lamps > 4096 || !importances || !order)
			lamps = lamps < (long)(maximum - count) ? lamps : (long)(maximum - count);
		else
		{
			for (index = 0; index < lamps; index++)
			{
				importances[index] = lt_object_lamp_importance(taken_lamps[index].corners, taken_lamps[index].radiance,
					in->camera) * (sides && (taken_lamps[index].sides & 3) == 2 ? 2.0f : 1.0f);
				order[index] = index;
			}
			lt_object_lamp_order_importances = importances;
			qsort(order, (size_t)lamps, sizeof(*order), lt_object_lamp_order);
			sorted = 1;
		}
	}
	/* (no more than the inputs took: lt_lights.c takes every lamp this can
	reach) */
	if (!sorted && lamps > in->lamp_count)
		lamps = in->lamp_count;
	for (taken = 0; taken < lamps && count < maximum; taken++)
	{
		struct xgpu_stream_lt_light *out = &lights[count++];
		struct lt_input_lamp const *lamp;

		index = sorted ? order[taken] : taken;
		lamp = &taken_lamps[index];
		memset(out, 0, sizeof(*out));
		out->id[0] = (unsigned int)index;
		out->kind = XGPU_LT_LIGHT_OBJECT_EMITTER;
		out->flags = (sides && (lamp->sides & 3) == 2 ? XGPU_LT_LAMP_TWO_SIDED : 0) |
			(sides && (lamp->sides & LT_LAMP_SIDE_ADDITIVE) ? XGPU_LT_LAMP_ADDITIVE : 0);
		out->object = (unsigned int)in->lamps_in_world;
		out->owner = lamp->owner;
		out->gel_slot = -1;
		out->gel_tag = -1;
		memcpy(out->position, lamp->corners, sizeof(float) * 3);
		memcpy(out->forward, lamp->corners + 3, sizeof(float) * 3);
		memcpy(out->up, lamp->corners + 6, sizeof(float) * 3);
		memcpy(out->power, lamp->radiance, sizeof(float) * 3);
		out->cone_inner = out->cone_outer = -1.0f;
		out->power_scale = 1.0f;
	}
	return count;
}

#ifdef LT_LIGHTS_UNIT
/* ---------- the replay's unit (lt_lights_unit.so)

A frame's LT_INPUTS record (its header, lights and lamps) and the LT_LIGHTS
table the game sent with it: the table built again by this code, the level's
light sources' triangles (lt_emitters.c's, kind XGPU_LT_LIGHT_EMITTER: a
share of its set each frame, not built here) taken from the recorded table
in their place, after the lights. The count, or -1 where the inputs are not
this layout's */
__attribute__((visibility("default"))) int lt_lights_unit_version = LT_INPUTS_VERSION;

__attribute__((visibility("default"))) int lt_lights_unit_build(void const *record, unsigned int bytes,
	struct xgpu_stream_lt_light const *recorded, int recorded_count, struct xgpu_stream_lt_light *out, int maximum)
{
	struct lt_inputs const *in = record;
	struct lt_input_light const *lights;
	struct lt_input_lamp const *lamps;
	int count, index;

	if (bytes < sizeof(*in) || in->version != LT_INPUTS_VERSION || in->bytes != bytes || in->light_count < 0 ||
		in->lamp_count < 0 || in->sky_light_count < 0 || in->sky_light_count > LT_INPUTS_SKY_LIGHTS ||
		maximum < XGPU_STREAM_LT_LIGHTS_MAXIMUM ||
		sizeof(*in) + (unsigned long)in->light_count * sizeof(*lights) + (unsigned long)in->lamp_count * sizeof(*lamps) !=
			bytes)
		return -1;
	lights = (struct lt_input_light const *)(in + 1);
	lamps = (struct lt_input_lamp const *)(lights + in->light_count);
	count = lt_lights_build_list(in, lights, out);
	for (index = 0; index < recorded_count && count < XGPU_STREAM_LT_LIGHTS_MAXIMUM; index++)
	{
		if (recorded[index].kind == XGPU_LT_LIGHT_EMITTER)
			out[count++] = recorded[index];
	}
	return lt_lights_build_lamps(in, lamps, out, count, XGPU_STREAM_LT_LIGHTS_MAXIMUM);
}
#endif
