/*
DYNAMIC_LIGHTS_BOUNCE.C

Bounced light (display.dynamic_light_bounce, with display.dynamic_lights
"enhanced"): the strongest of the frame's
dynamic lights (plasma, explosions, muzzle flashes, rockets, the port's own)
light the walls near them, and each of those walls sends a little of that
light on in its own color, as a dim, wide light just off it. No ray-tracing
GPU is needed: each light casts BOUNCE_RAYS fixed rays (the six ways along
the world's axes) through the structure's collision BSP, as far as its
reach, and where a ray meets a wall in reach, a bounce light is placed
off it toward the light (the lit patch's middle), facing out of it (its
cone a lit wall's spread, widened for a patch). Its color is the light's color, in linear light, times what reaches
the wall (the renderer's falloff at the distance, and how squarely the wall
faces the light), times the wall's shader's base map's average color (taken
once per collision material, BOUNCE_ALBEDO_BUDGET a frame, and kept for the
structure), times BOUNCE_ALBEDO_SCALE.

Only dynamic lights bounce: the lightmaps carry the static lights' bounce
already. The bounce lights go after every other light (dynamic_lights.c's
dynamic_lights_submit, after the shadows' choice, so none casts shadows), in
the room the others leave, at most BOUNCE_MAXIMUM, so they never take a real
light's place. Each has the clusters its sphere floods into, as any other.

Steady: each window keeps each bounced light's track (matched again each
frame by where it is) and each of its rays' bounce lights, which ease toward
what the frame finds (BOUNCE_EASE seconds of game time) and fade where a ray
loses its wall; a steady light's bounce settles and stays, and it fades with
its light. A light no longer among the chosen takes its bounce with it.
Only the drawing changes: nothing simulated, nothing sent between machines.

display.rt_bounce (docs/rt-plan/3-gi.md, stage 3) takes its place where the
backend has ray queries and display.ambient_occlusion's pre-pass runs: the
same choice of lights is handed to the GPU (xgpu_backend_rt_bounce), which
traces their bounce off the real surfaces into a world-space cache
(port/linux/vulkan/xgpu_vulkan_gi.c), and no bounce lights are placed.
*/

#include "cseries.h"
#include "real_math.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "structures/structures.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/collisions.h"
#include "bitmaps/bitmap_group.h"
#include "cache/texture_cache.h"
#include "shaders/shader_definitions.h"
#include "../src/xgpu_coverage.h"
#include "rt_bounce.h"

#include <xtl.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
char const *config_string(char const *name);
/* port/linux/src (halo.log) */
void platform_log(const char *format, ...);
unsigned long long platform_shared_clock_microseconds(void);
/* port/linux/game/dynamic_lights.c */
void dynamic_lights_clusters(long rasterizer_light_index, short count, short const *clusters);
/* port/linux/game/world_ao.c, port/linux/src/xgpu_stream_encode.c */
int world_ao_mode(void);
int halo_world_ao_ray_queries(void);
void xgpu_backend_rt_bounce(struct xgpu_rt_bounce const *bounce);
/* port/linux/game/door_portals.c */
short door_portals_clusters_in_sphere(short cluster_index, real_point3d const *position, real radius,
	short maximum_count, short *clusters);

/* the lights that bounce (the strongest to the view), the rays each casts,
and the bounce lights a frame at most */
#define BOUNCE_SOURCES 6
#define BOUNCE_RAYS 6
#define BOUNCE_MAXIMUM 32
/* a light that bounces: at least this reach (world units) and brightest
channel */
#define BOUNCE_LEAST_RADIUS 0.5f
#define BOUNCE_LEAST_INTENSITY 0.05f
/* how far a ray goes, of its light's reach */
#define BOUNCE_REACH 0.9f
/* a bounce light: how far off its wall (of the way back to its light,
within these: the lit patch's middle, which spreads its light along the
walls beside it), its cone (a lit wall's cosine, widened: a patch, not a
point; nothing behind its wall), its reach (of its light's, within these),
and the share of what its wall is lit by it sends on */
#define BOUNCE_OFFSET_SCALE 0.5f
#define BOUNCE_OFFSET_LEAST 0.2f
#define BOUNCE_OFFSET_MOST 1.0f
#define BOUNCE_CONE_SCALE 0.6f
#define BOUNCE_CONE_OFFSET 0.4f
#define BOUNCE_RADIUS_SCALE 1.5f
#define BOUNCE_RADIUS_LEAST 1.5f
#define BOUNCE_RADIUS_MOST 8.0f
#define BOUNCE_ALBEDO_SCALE 3.0f
/* a bounce light this dim (its brightest channel, linear) is none */
#define BOUNCE_LEAST_LINEAR 0.002f
/* a wall whose base map cannot be read: a mid grey, linear */
#define BOUNCE_ALBEDO_UNKNOWN 0.18f
/* the base maps read a frame at most, the level read (no larger a side) */
#define BOUNCE_ALBEDO_BUDGET 2
#define BOUNCE_ALBEDO_LEVEL 64
#define BOUNCE_ALBEDO_MATERIALS 1024
/* the easing's time (seconds of game time), and the most of a frame's step */
#define BOUNCE_EASE 0.05f
#define BOUNCE_STEP_MOST 0.25f
/* the same light next frame: within this (world units), or this of its reach */
#define BOUNCE_MATCH 2.0f
#define BOUNCE_MATCH_SCALE 0.5f
/* a ray's wall the same next frame: within this (or this of the bounce
light's reach) and as near this facing */
#define BOUNCE_SAME_WALL 0.5f
#define BOUNCE_SAME_WALL_SCALE 0.25f
#define BOUNCE_SAME_FACING 0.7f
#define BOUNCE_WINDOWS 4
/* the enhanced lights' brightness where debug.dynamic_lights_look gives none
(d3d8_device.c's) */
#define BOUNCE_LIGHTS_BRIGHTNESS 1.7f

enum
{
	/* shader_definitions.h's types: an environment's, a model's */
	_bounce_shader_type_environment = 3,
	_bounce_shader_type_model = 4,
};

/* an environment shader's base map, a model shader's (object_lights.c's,
world_sdf.c's) */
struct bounce_shader_environment
{
	struct shader shader;
	byte reserved28[0x60];
	struct tag_reference base_map;
};
struct bounce_shader_model
{
	struct shader shader;
	byte reserved28[0x7C];
	struct tag_reference base_map;
};
typedef char bounce_shader_environment_base_map[
	offsetof(struct bounce_shader_environment, base_map) == 0x88 ? 1 : -1];
typedef char bounce_shader_model_base_map[offsetof(struct bounce_shader_model, base_map) == 0xA4 ? 1 : -1];

/* a ray's bounce light as it eases: where (on its wall, its wall's facing),
its reach and its color (linear) */
struct bounce_slot
{
	boolean have;
	real_point3d point;
	real_vector3d normal;
	real radius;
	real color[3];
};

/* what a frame finds of a ray (have: a wall lit in reach) */
struct bounce_target
{
	boolean have;
	real_point3d point;
	real_vector3d normal;
	real radius;
	real color[3];
};

/* a bounced light as it was last: where, its reach, its rays' bounce lights */
struct bounce_track
{
	boolean used;
	real_point3d position;
	real radius;
	struct bounce_slot slots[BOUNCE_RAYS];
};

static struct
{
	struct
	{
		struct bounce_track tracks[BOUNCE_SOURCES];
		real time;
		boolean timed;
	} windows[BOUNCE_WINDOWS];
	/* each collision material's base map's average color (linear) for the
	structure's materials, and whether it is known */
	void const *materials;
	long material_count;
	real albedo[BOUNCE_ALBEDO_MATERIALS][3];
	boolean albedo_known[BOUNCE_ALBEDO_MATERIALS];
} bounce;

/* the six ways the rays go */
static real_vector3d const bounce_directions[BOUNCE_RAYS] =
{
	{ 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
	{ 0.0f, 1.0f, 0.0f }, { 0.0f, -1.0f, 0.0f },
	{ 0.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, -1.0f },
};

/* ---------- where a bounce light goes, and how bright (pure: the harness's) */

/* what a wall at distance from a light of reach radius and linear color is
lit by (the renderer's falloff, the wall facing it by facing, its cosine),
sent on by its albedo (linear): the bounce light just off it (offset along
its normal, its cone facing out of it), and its linear color; FALSE if it
is too dim or out of reach */
static boolean dynamic_lights_bounce_place(
	real radius,
	real const linear[3],
	real_point3d const *point,
	real_vector3d const *normal,
	real distance,
	real facing,
	real const albedo[3],
	struct bounce_target *target)
{
	real x2, window, falloff, offset, brightest = 0.0f;
	short channel;

	if (!(radius > 0.0f) || !(distance >= 0.0f) || distance >= radius || !(facing > 0.0f))
		return FALSE;
	x2 = (distance * distance) / (radius * radius);
	window = 1.0f - x2 * x2;
	falloff = window * window;
	for (channel = 0; channel < 3; channel++)
	{
		target->color[channel] = linear[channel] * falloff * (facing > 1.0f ? 1.0f : facing) * albedo[channel] *
			BOUNCE_ALBEDO_SCALE;
		if (target->color[channel] > brightest)
			brightest = target->color[channel];
	}
	if (brightest < BOUNCE_LEAST_LINEAR)
		return FALSE;
	offset = distance * BOUNCE_OFFSET_SCALE;
	offset = offset < BOUNCE_OFFSET_LEAST ? BOUNCE_OFFSET_LEAST : offset > BOUNCE_OFFSET_MOST ? BOUNCE_OFFSET_MOST : offset;
	target->have = TRUE;
	target->normal = *normal;
	target->point.x = point->x + normal->i * offset;
	target->point.y = point->y + normal->j * offset;
	target->point.z = point->z + normal->k * offset;
	target->radius = radius * BOUNCE_RADIUS_SCALE;
	if (target->radius < BOUNCE_RADIUS_LEAST)
		target->radius = BOUNCE_RADIUS_LEAST;
	if (target->radius > BOUNCE_RADIUS_MOST)
		target->radius = BOUNCE_RADIUS_MOST;
	return TRUE;
}

/* a ray's bounce light eased by ease (0 to 1, of the way) toward what the
frame found: kept on the same wall (near and facing alike) and eased in
brightness, begun dark on another, and faded where none was found (gone
once too dim) */
static void dynamic_lights_bounce_ease(
	struct bounce_slot *slot,
	struct bounce_target const *target,
	real ease)
{
	short channel;

	if (target->have)
	{
		real_vector3d offset;
		real same = target->radius * BOUNCE_SAME_WALL_SCALE;

		if (same < BOUNCE_SAME_WALL)
			same = BOUNCE_SAME_WALL;
		offset.i = target->point.x - slot->point.x;
		offset.j = target->point.y - slot->point.y;
		offset.k = target->point.z - slot->point.z;
		if (!slot->have || offset.i * offset.i + offset.j * offset.j + offset.k * offset.k > same * same ||
			slot->normal.i * target->normal.i + slot->normal.j * target->normal.j + slot->normal.k * target->normal.k <
				BOUNCE_SAME_FACING)
		{
			slot->color[0] = slot->color[1] = slot->color[2] = 0.0f;
		}
		slot->have = TRUE;
		slot->point = target->point;
		slot->normal = target->normal;
		slot->radius = target->radius;
		for (channel = 0; channel < 3; channel++)
			slot->color[channel] += (target->color[channel] - slot->color[channel]) * ease;
		return;
	}
	if (!slot->have)
		return;
	for (channel = 0; channel < 3; channel++)
		slot->color[channel] *= 1.0f - ease;
	if (MAX(slot->color[0], MAX(slot->color[1], slot->color[2])) < BOUNCE_LEAST_LINEAR)
		slot->have = FALSE;
}

/* the track a light (at position, of reach radius) had last frame: the
nearest not yet taken within BOUNCE_MATCH (or that of its reach); NONE for
none */
static short dynamic_lights_bounce_match(
	struct bounce_track const *tracks,
	boolean const *taken,
	real_point3d const *position,
	real radius)
{
	real nearest = MAX(BOUNCE_MATCH, radius * BOUNCE_MATCH_SCALE);
	short index, found = NONE;

	nearest *= nearest;
	for (index = 0; index < BOUNCE_SOURCES; index++)
	{
		real dx, dy, dz, distance;

		if (!tracks[index].used || taken[index])
			continue;
		dx = tracks[index].position.x - position->x;
		dy = tracks[index].position.y - position->y;
		dz = tracks[index].position.z - position->z;
		distance = dx * dx + dy * dy + dz * dz;
		if (distance <= nearest)
		{
			nearest = distance;
			found = index;
		}
	}
	return found;
}

/* the lights that bounce, the strongest to the view first (the indices of
up to BOUNCE_SOURCES of count): those that light the structure, of some
reach and brightness, each by its intensity times how wide its reach looks
from the camera (at camera); the earlier among equals; how many */
static short dynamic_lights_bounce_choose(
	struct halo_dynamic_light const *lights,
	long count,
	real_point3d const *camera,
	short *chosen)
{
	real scores[BOUNCE_SOURCES];
	short chosen_count = 0, slot;
	long index;

	for (index = 0; index < count; index++)
	{
		struct halo_dynamic_light const *light = &lights[index];
		real intensity = MAX(light->color[0], MAX(light->color[1], light->color[2]));
		real dx, dy, dz, distance, score;

		/* (not the flashlight's: a narrow beam through its gel, which a
		bounce about its light would spill all round) */
		if ((light->flags & (HALO_DYNAMIC_LIGHT_MODELS_ONLY | HALO_DYNAMIC_LIGHT_FLASHLIGHT)) ||
			!(light->radius >= BOUNCE_LEAST_RADIUS) ||
			!(intensity >= BOUNCE_LEAST_INTENSITY))
		{
			continue;
		}
		dx = light->position[0] - camera->x;
		dy = light->position[1] - camera->y;
		dz = light->position[2] - camera->z;
		distance = sqrtf(dx * dx + dy * dy + dz * dz);
		score = intensity * light->radius / (distance > light->radius ? distance : light->radius);
		for (slot = chosen_count; slot > 0 && scores[slot - 1] < score; slot--)
		{
			if (slot < BOUNCE_SOURCES)
			{
				scores[slot] = scores[slot - 1];
				chosen[slot] = chosen[slot - 1];
			}
		}
		if (slot >= BOUNCE_SOURCES)
			continue;
		scores[slot] = score;
		chosen[slot] = (short)index;
		if (chosen_count < BOUNCE_SOURCES)
			chosen_count++;
	}
	return chosen_count;
}

/* ---------- the walls' colors */

/* a bitmap's first texture's average color (linear); FALSE if it cannot be
read. Its texels are loaded as a draw loads them, and waited for */
static boolean dynamic_lights_bounce_bitmap(long bitmap_tag, real linear[3])
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	DWORD const *header;

	if (bitmap_tag == NONE || !(group = bitmap_group_get(bitmap_tag)) || !group->bitmaps.address ||
		group->bitmaps.count <= 0)
	{
		return FALSE;
	}
	header = texture_cache_port_load((unsigned long)bitmap_tag, 0);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, 0, struct bitmap_data);
	if (!header || !bitmap->base_address)
		return FALSE;
	return xgpu_texture_average_color((unsigned int)header[3], (unsigned int)header[4],
		(unsigned char const *)bitmap->base_address, BOUNCE_ALBEDO_LEVEL, linear) != 0;
}

/* a collision material's albedo (linear: its shader's base map's average
color), read once for the structure; FALSE while it is not yet known (budget:
the reads left this frame) */
static boolean dynamic_lights_bounce_albedo(
	struct structure_bsp *structure,
	short material_index,
	short *budget,
	real albedo[3])
{
	if (bounce.materials != structure->collision_materials.address ||
		bounce.material_count != structure->collision_materials.count)
	{
		bounce.materials = structure->collision_materials.address;
		bounce.material_count = structure->collision_materials.count;
		memset(bounce.albedo_known, 0, sizeof(bounce.albedo_known));
	}
	if (material_index < 0 || material_index >= structure->collision_materials.count ||
		material_index >= BOUNCE_ALBEDO_MATERIALS)
	{
		albedo[0] = albedo[1] = albedo[2] = BOUNCE_ALBEDO_UNKNOWN;
		return TRUE;
	}
	if (!bounce.albedo_known[material_index])
	{
		struct structure_collision_material const *material;
		long shader_tag;
		real *known = bounce.albedo[material_index];
		boolean read = FALSE;

		if (*budget <= 0)
			return FALSE;
		(*budget)--;
		material = TAG_BLOCK_GET_ELEMENT(&structure->collision_materials, material_index,
			struct structure_collision_material);
		shader_tag = material->shader.index;
		if (shader_tag != NONE)
		{
			struct shader *shader = shader_definition_get(shader_tag);

			if (shader && shader->base.type == _bounce_shader_type_environment)
				read = dynamic_lights_bounce_bitmap(((struct bounce_shader_environment *)shader)->base_map.index, known);
			else if (shader && shader->base.type == _bounce_shader_type_model)
				read = dynamic_lights_bounce_bitmap(((struct bounce_shader_model *)shader)->base_map.index, known);
		}
		if (!read)
			known[0] = known[1] = known[2] = BOUNCE_ALBEDO_UNKNOWN;
		bounce.albedo_known[material_index] = TRUE;
	}
	memcpy(albedo, bounce.albedo[material_index], sizeof(real) * 3);
	return TRUE;
}

/* ---------- the frame's */

/* what a light's ray finds: a wall of the structure in reach, lit, and its
bounce light (dynamic_lights_bounce_place) */
static void dynamic_lights_bounce_cast(
	struct structure_bsp *structure,
	struct halo_dynamic_light const *light,
	real const linear[3],
	real_vector3d const *direction,
	short *budget,
	struct bounce_target *target)
{
	struct collision_result collision;
	real_point3d origin;
	real_vector3d vector;
	real length = light->radius * BOUNCE_REACH, albedo[3], facing;

	target->have = FALSE;
	origin.x = light->position[0];
	origin.y = light->position[1];
	origin.z = light->position[2];
	vector.i = direction->i * length;
	vector.j = direction->j * length;
	vector.k = direction->k * length;
	if (!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_ignore_invisible_surfaces_bit), &origin, &vector, NONE, &collision) ||
		collision.type != _collision_result_structure)
	{
		return;
	}
	facing = -(direction->i * collision.plane.n.i + direction->j * collision.plane.n.j +
		direction->k * collision.plane.n.k);
	if (!dynamic_lights_bounce_albedo(structure, collision.material_index, budget, albedo))
		return;
	dynamic_lights_bounce_place(light->radius, linear, &collision.point, &collision.plane.n, collision.t * length,
		facing, albedo, target);
}

/* ---------- display.rt_bounce: the GPU's bounce */

/* the lights the GPU bounces: dynamic_lights_bounce_choose's, at most
XGPU_RT_BOUNCE_LIGHTS of them, and the flashlight too (its light on the
structure is the game's own gel pass's, models-only to the enhanced lights,
but it bounces all the same); how many */
static short dynamic_lights_bounce_gpu_choose(
	struct halo_dynamic_light const *lights,
	long count,
	real_point3d const *camera,
	short *chosen)
{
	real scores[XGPU_RT_BOUNCE_LIGHTS];
	short chosen_count = 0, slot;
	long index;

	for (index = 0; index < count; index++)
	{
		struct halo_dynamic_light const *light = &lights[index];
		real intensity = MAX(light->color[0], MAX(light->color[1], light->color[2]));
		real dx, dy, dz, distance, score;

		if (((light->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) && !(light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT)) ||
			!(light->radius >= BOUNCE_LEAST_RADIUS) || !(intensity >= BOUNCE_LEAST_INTENSITY))
		{
			continue;
		}
		dx = light->position[0] - camera->x;
		dy = light->position[1] - camera->y;
		dz = light->position[2] - camera->z;
		distance = sqrtf(dx * dx + dy * dy + dz * dz);
		score = intensity * light->radius / (distance > light->radius ? distance : light->radius);
		for (slot = chosen_count; slot > 0 && scores[slot - 1] < score; slot--)
		{
			if (slot < XGPU_RT_BOUNCE_LIGHTS)
			{
				scores[slot] = scores[slot - 1];
				chosen[slot] = chosen[slot - 1];
			}
		}
		if (slot >= XGPU_RT_BOUNCE_LIGHTS)
			continue;
		scores[slot] = score;
		chosen[slot] = (short)index;
		if (chosen_count < XGPU_RT_BOUNCE_LIGHTS)
			chosen_count++;
	}
	return chosen_count;
}

/* the GPU bounces the window's lights (the setting on, ray queries, the
pre-pass): the chosen lights handed over, TRUE; else FALSE, and the CPU's
bounce (if on) places its lights */
static boolean dynamic_lights_bounce_gpu(
	struct halo_dynamic_light const *lights,
	long count,
	short window)
{
	static unsigned long long last[BOUNCE_WINDOWS];
	static boolean said;
	struct xgpu_rt_bounce bounce;
	short chosen[XGPU_RT_BOUNCE_LIGHTS], chosen_count, index, channel;
	unsigned long long now;
	real brightness;

	if (!config_boolean("display.rt_bounce") || !halo_dynamic_lights_enhanced() || !halo_world_ao_ray_queries() ||
		!world_ao_mode())
	{
		return FALSE;
	}
	if (!said)
	{
		platform_log("dynamic lights: display.rt_bounce: bounced light traced on the GPU");
		said = TRUE;
	}
	memset(&bounce, 0, sizeof(bounce));
	now = platform_shared_clock_microseconds();
	bounce.window = window;
	bounce.seconds = last[window] && now > last[window] ? (real)((double)(now - last[window]) * 1.0e-6) : 0.0f;
	last[window] = now;
	chosen_count = game_in_progress() && global_structure_bsp_get() ?
		dynamic_lights_bounce_gpu_choose(lights, count, &render.camera.position, chosen) : 0;
	/* (the enhanced lights' brightness, as the renderer scales their color:
	debug.dynamic_lights_look's first number, d3d8_device.c's 1.7) */
	{
		char const *look = config_string("debug.dynamic_lights_look");
		char *end;
		double value = look && *look ? strtod(look, &end) : 0.0;

		brightness = look && *look && end != look && value > 0.0 ? (real)value : BOUNCE_LIGHTS_BRIGHTNESS;
	}
	for (index = 0; index < chosen_count; index++)
	{
		struct halo_dynamic_light const *light = &lights[chosen[index]];
		struct xgpu_rt_bounce_light *out = &bounce.lights[bounce.count++];

		memcpy(out->position, light->position, sizeof(out->position));
		out->radius = light->radius;
		for (channel = 0; channel < 3; channel++)
			out->color[channel] = powf(MAX(light->color[channel], 0.0f), 2.2f) * brightness;
		memcpy(out->axis, light->axis, sizeof(out->axis));
		out->cone_scale = light->cone_scale;
		out->cone_offset = light->cone_offset;
	}
	xgpu_backend_rt_bounce(&bounce);
	return TRUE;
}

/* dynamic_lights.c's dynamic_lights_submit: the frame's bounce lights after
its count of lights (as they go to the renderer), in the room they leave;
the count of all */
long dynamic_lights_bounce(
	struct halo_dynamic_light *lights,
	long count)
{
	static unsigned long long timed, timed_frames, timed_lights;
	boolean logging;
	unsigned long long start;
	struct structure_bsp *structure;
	struct bounce_track kept[BOUNCE_SOURCES];
	boolean taken[BOUNCE_SOURCES];
	short chosen[BOUNCE_SOURCES], chosen_count, chosen_index, budget = BOUNCE_ALBEDO_BUDGET;
	short window = render.window_index;
	long room, made = 0;
	real time, ease;

	if (window < 0 || window >= BOUNCE_WINDOWS)
		window = 0;
	if (dynamic_lights_bounce_gpu(lights, count, window))
	{
		memset(&bounce.windows[window], 0, sizeof(bounce.windows[window]));
		return count;
	}
	if (!config_boolean("display.dynamic_light_bounce") || !halo_dynamic_lights_enhanced())
		return count;
	structure = global_structure_bsp_get();
	room = MIN((long)BOUNCE_MAXIMUM, (long)MAXIMUM_LIGHTS_PER_WINDOW - count);
	if (!structure || room <= 0 || !game_in_progress())
	{
		memset(&bounce.windows[window], 0, sizeof(bounce.windows[window]));
		return count;
	}
	logging = config_boolean("debug.dynamic_lights_log") != 0;
	start = logging ? platform_shared_clock_microseconds() : 0;
	time = render_interpolation_game_time_sec(game_time_get());
	ease = bounce.windows[window].timed ? time - bounce.windows[window].time : 0.0f;
	ease = ease < 0.0f ? 0.0f : ease > BOUNCE_STEP_MOST ? BOUNCE_STEP_MOST : ease;
	ease = 1.0f - expf(-ease / BOUNCE_EASE);
	bounce.windows[window].time = time;
	bounce.windows[window].timed = TRUE;
	chosen_count = dynamic_lights_bounce_choose(lights, count, &render.camera.position, chosen);
	memset(taken, 0, sizeof(taken));
	memset(kept, 0, sizeof(kept));
	for (chosen_index = 0; chosen_index < chosen_count; chosen_index++)
	{
		struct halo_dynamic_light const *light = &lights[chosen[chosen_index]];
		struct bounce_track *track = &kept[chosen_index];
		real_point3d position;
		real linear[3];
		short ray, previous, channel;

		position.x = light->position[0];
		position.y = light->position[1];
		position.z = light->position[2];
		previous = dynamic_lights_bounce_match(bounce.windows[window].tracks, taken, &position, light->radius);
		if (previous != NONE)
		{
			*track = bounce.windows[window].tracks[previous];
			taken[previous] = TRUE;
		}
		track->used = TRUE;
		track->position = position;
		track->radius = light->radius;
		for (channel = 0; channel < 3; channel++)
			linear[channel] = powf(MAX(light->color[channel], 0.0f), 2.2f);
		for (ray = 0; ray < BOUNCE_RAYS; ray++)
		{
			struct bounce_target target;

			dynamic_lights_bounce_cast(structure, light, linear, &bounce_directions[ray], &budget, &target);
			dynamic_lights_bounce_ease(&track->slots[ray], &target, previous != NONE ? ease : 1.0f);
		}
	}
	memcpy(bounce.windows[window].tracks, kept, sizeof(kept));
	for (chosen_index = 0; chosen_index < chosen_count && made < room; chosen_index++)
	{
		struct bounce_track const *track = &bounce.windows[window].tracks[chosen_index];
		short ray;

		for (ray = 0; ray < BOUNCE_RAYS && made < room; ray++)
		{
			struct bounce_slot const *slot = &track->slots[ray];
			struct halo_dynamic_light *out = &lights[count + made];
			short clusters[MAXIMUM_CLUSTERS_PER_STRUCTURE];
			short cluster_count, channel;
			struct location location;

			if (!slot->have)
				continue;
			scenario_location_from_point(&location, &slot->point);
			if (location.cluster_index == NONE)
				continue;
			cluster_count = door_portals_clusters_in_sphere(location.cluster_index, &slot->point, slot->radius,
				MAXIMUM_CLUSTERS_PER_STRUCTURE, clusters);
			out->position[0] = slot->point.x;
			out->position[1] = slot->point.y;
			out->position[2] = slot->point.z;
			out->radius = slot->radius;
			for (channel = 0; channel < 3; channel++)
				out->color[channel] = powf(MAX(slot->color[channel], 0.0f), 1.0f / 2.2f);
			/* (a lit wall's spread about its normal) */
			out->axis[0] = slot->normal.i;
			out->axis[1] = slot->normal.j;
			out->axis[2] = slot->normal.k;
			out->cone_scale = BOUNCE_CONE_SCALE;
			out->cone_offset = BOUNCE_CONE_OFFSET;
			out->flags = 0;
			dynamic_lights_clusters(count + made, MIN(cluster_count, MAXIMUM_CLUSTERS_PER_STRUCTURE), clusters);
			made++;
		}
	}
	/* (debug.dynamic_lights_log: what they cost, on average, and how many) */
	if (logging)
	{
		timed += platform_shared_clock_microseconds() - start;
		timed_lights += (unsigned long long)made;
		if (++timed_frames == 600)
		{
			platform_log("dynamic lights: bounce %.1f us a frame, %.1f bounce lights (600 frames)",
				(double)timed / 600.0, (double)timed_lights / 600.0);
			timed = timed_frames = timed_lights = 0;
		}
	}
	return count + made;
}
