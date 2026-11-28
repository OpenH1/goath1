/*
DYNAMIC_LIGHTS.C

The enhanced dynamic lights (Video Setup > Graphics > Dynamic Lights,
display.dynamic_lights "enhanced"; "classic", the default, is the game's
own). The lights the game attaches to projectiles (plasma bolts, a
grenade), muzzle flashes and explosions, and the port's own where the tags
have none (light_sources.c: needles, a Wraith's mortar, a fuel rod's
explosion, fires), are drawn by the renderer
(port/linux/src/d3d8_device.c, xgpu_spirv.c, xgpu_msl.c) for each pixel of
the structure and of the models, through the structure's bump maps, with a softer, brighter
falloff in linear light, and the projectiles' lights have a glow drawn about
them. This is the game's side: each frame (lights_preprocess_scene), the
lights it submits to the rasterizer, with what the renderer needs of each
that it does not have (its cone, whether it has a glow, whether the game's
own pass lights the structure with it), go to the renderer.

Where a light is drawn: the game places a light on an object's marker as
the object moves, at each tick (30 a second), and draws the objects between
ticks (port/linux/game/render_interpolation.c), so a bolt's light would step
behind its bolt; object_lights.c places it on the marker as drawn instead,
each frame. A weapon's own lights (its muzzle flash) the game moves to the
first-person weapon for its holder; an effect's light on the weapon (a
firing effect's) stays on the weapon the body holds, so it is moved to the
same marker of the first-person weapon (dynamic_lights_first_person), where
the effect is seen, and from where the first-person weapon (in the VR mode,
the weapon in the hand) is lit.

debug.dynamic_lights_log logs, each frame, where the first projectile's
light is drawn (the smoothness test: a step for each frame, not for each
tick). Only the drawing changes: nothing the game simulates, nothing sent
between machines.

Which lights are drawn (dynamic_lights_rank): a view finds up to
MAXIMUM_RENDERED_LIGHTS lights in the clusters it sees (halo_port_capacity.h:
512 on the desktop builds), but the rasterizer has room for
MAXIMUM_LIGHTS_PER_WINDOW (128) of them, the enhanced dynamic lights' grid
too (XGPU_DYNAMIC_LIGHTS_MAXIMUM), and the game gave them to the first it
found, in the clusters' order. Past 128, the lights are put in the order of
what they show first, so that the 128 drawn are those that matter most to
the view, classic or enhanced.

Which clusters a light reaches (dynamic_lights_clusters): the game lights the
structure with a light only in the clusters its sphere floods into through
the structure's portals (structure_clusters_in_sphere: its surfaces gathered
for its pass are theirs), and an object only by the lights of its clusters.
The renderer lights each pixel by the lights of its cell of the grid, which
is the world's boxes, not its clusters, and reached through walls: a plasma
bolt in the next corridor lit this one's walls. So each light's clusters go
with it, as each cluster's mask of the lights that reach it, and the
structure's 3D BSP, from which a pixel finds its cluster
(dynamic_lights_world; xgpu.h).
*/

#include "cseries.h"
#include "real_math.h"
#include "objects/objects.h"
#include "render/render.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "interface/first_person_weapons.h"
#include "game/game.h"
#include "scenario/scenario.h"
#include "structures/structures.h"
#include "structures/structure_bsp_definitions.h"
#include "physics/bsp3d.h"
#include "tag_files/tag_groups.h"
#include "light_sources.h"
#include "rt_shadows.h"
#include "world_rt.h"
#include "world_ao.h"
#include "rasterizer/rasterizer_environment.h"
#include "lt_lights.h"
#include "render_enumerate.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(const char *name);
/* port/linux/src (halo.log) */
void platform_log(const char *format, ...);
/* port/linux/src (the computer's clock) */
unsigned long long platform_shared_clock_microseconds(void);
void dynamic_lights_clusters(long rasterizer_light_index, short count, short const *clusters);
/* port/linux/game/door_portals.c */
short door_portals_clusters_in_sphere(short cluster_index, real_point3d const *position, real radius,
	short maximum_count, short *clusters);

static struct
{
	/* for each rasterizer light this frame (rasterizer_lights' index) */
	long flags[MAXIMUM_LIGHTS_PER_WINDOW];
	real cone_scale[MAXIMUM_LIGHTS_PER_WINDOW];
	real cone_offset[MAXIMUM_LIGHTS_PER_WINDOW];
	/* its color's scale past the rasterizer's 1 (dynamic_lights_brighten:
	0 for 1) */
	real brightness[MAXIMUM_LIGHTS_PER_WINDOW];
	/* (debug.dynamic_lights_log: where the game placed it, at the tick) */
	real_point3d tick_position[MAXIMUM_LIGHTS_PER_WINDOW];
	/* display.shadows "ray_traced": its object's owner among the ray-traced
	scene's instances (its ultimate parent's: a muzzle flash's the unit
	holding the weapon; dynamic_lights_owner), which its shadows' rays leave
	out; 0 none */
	unsigned long owners[MAXIMUM_LIGHTS_PER_WINDOW];
	/* what it is from frame to frame (DYNAMIC_LIGHTS_KEY_*: its light's
	handle, a copy's, an own light's; 0 not known), and the object it is
	on (NONE none) */
	unsigned long long keys[MAXIMUM_LIGHTS_PER_WINDOW];
	long objects[MAXIMUM_LIGHTS_PER_WINDOW];
	/* a glow standing in for what the lightmap lights (light_sources.c's
	baked: no shadow) */
	boolean baked[MAXIMUM_LIGHTS_PER_WINDOW];
	struct halo_dynamic_light lights[MAXIMUM_LIGHTS_PER_WINDOW];
	unsigned long frame;
	/* each light whose clusters were noted (dynamic_lights_clusters; one
	not noted reaches every cluster), and each cluster's mask of the lights
	that reach it */
	boolean clusters_noted[MAXIMUM_LIGHTS_PER_WINDOW];
	unsigned int cluster_masks[HALO_DYNAMIC_LIGHTS_CLUSTERS][4];
	/* the frame's copies of the game's lights on their other markers
	(dynamic_lights_copy): whose copy (its rasterizer light), which of its
	light's copies (its marker's number), where */
	struct
	{
		long rasterizer_light_index;
		short number;
		real_point3d position;
		real_vector3d axis;
	} copies[MAXIMUM_LIGHTS_PER_WINDOW];
	short copy_count;
	/* the structure whose BSP the renderer has (dynamic_lights_world) */
	void const *world_nodes;
	long world_node_count;
	void const *world_leaves;
	long world_leaf_count;
} dynamic_lights;

/* a light's key (dynamic_lights.keys): the game's (its light's handle), a
copy of one on another marker (its light's handle and the copy's number
among that light's own copies, from 1: DYNAMIC_LIGHTS_COPY_KEY), or one of
the port's own (light_sources.c's key, which has its kind in its top byte,
1 to 3) */
#define DYNAMIC_LIGHTS_KEY_GAME (4ull << 56)
#define DYNAMIC_LIGHTS_KEY_COPY (5ull << 56)

int dynamic_lights_flashlight_shadows_on(void);
#define DYNAMIC_LIGHTS_COPY_KEY(light_key, number) \
	(DYNAMIC_LIGHTS_KEY_COPY | ((unsigned long long)(unsigned short)(number) << 32) | ((light_key) & 0xffffffffull))
/* the flashlight on the models (classic: without the ray-traced lighting's
light model): how much farther it reaches than its radius, and how much
brighter (its color, the game's 0 to 1) */
#define DYNAMIC_LIGHTS_FLASHLIGHT_REACH 2.0f
#define DYNAMIC_LIGHTS_FLASHLIGHT_BRIGHTNESS 1.8f

typedef char dynamic_lights_clusters_fit[HALO_DYNAMIC_LIGHTS_CLUSTERS >= MAXIMUM_CLUSTERS_PER_STRUCTURE ? 1 : -1];
typedef char dynamic_lights_lights_fit[HALO_DYNAMIC_LIGHTS_MAXIMUM >= MAXIMUM_LIGHTS_PER_WINDOW ? 1 : -1];

/* a rasterizer light just submitted: its flags (HALO_DYNAMIC_LIGHT_*), its
cone, as rasterizer_set_model_lighting_point_light makes it, and where the
game placed it at the tick */
void dynamic_lights_note(
	long rasterizer_light_index,
	long flags,
	real cone_scale,
	real cone_offset,
	real_point3d const *tick_position)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW)
		return;
	dynamic_lights.flags[rasterizer_light_index] = flags;
	dynamic_lights.cone_scale[rasterizer_light_index] = cone_scale;
	dynamic_lights.cone_offset[rasterizer_light_index] = cone_offset;
	dynamic_lights.tick_position[rasterizer_light_index] = *tick_position;
	dynamic_lights.objects[rasterizer_light_index] = NONE;
}

/* a rasterizer light just submitted: the object it is on (NONE none),
whose collision meshes do not shadow it, and whose surfaces it does not
light (the ray-traced pass's: lt_lights.h). Recorded in every mode
(world_rt_owner is the handle's arithmetic alone): a light's owner does
not depend on which rays happen to be on */
void dynamic_lights_owner(
	long rasterizer_light_index,
	long object_index)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW)
		return;
	/* (an object freed before its light: no owner, none climbed to) */
	if (object_index != NONE && !object_try_and_get(object_index))
		object_index = NONE;
	dynamic_lights.owners[rasterizer_light_index] = object_index != NONE ?
		world_rt_owner(object_get_ultimate_parent(object_index)) : 0;
	dynamic_lights.objects[rasterizer_light_index] = object_index;
}

/* a rasterizer light just submitted: the game's light it is (its handle),
which dynamic_lights_shadowed knows it by from frame to frame */
void dynamic_lights_identify(
	long rasterizer_light_index,
	long light_handle)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW)
		return;
	dynamic_lights.keys[rasterizer_light_index] = DYNAMIC_LIGHTS_KEY_GAME |
		((unsigned long long)(unsigned long)light_handle & 0xffffffffull);
}

/* a rasterizer light just submitted: drawn brighter than the rasterizer's
colors go (to 1), by scale (object_lights.c's light_port_weapon_glow) */
void dynamic_lights_brighten(
	long rasterizer_light_index,
	real scale)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW)
		return;
	dynamic_lights.brightness[rasterizer_light_index] = scale;
}

/* what each of count lights is given of one light's color for the count of
them to be as bright as the one: shared in linear light, as the renderer
adds them (it takes each color to the power 2.2), not in the game's gamma,
where three of a third each would be a quarter as bright */
real dynamic_lights_share(
	short count)
{
	return count > 1 ? powf((real)count, -1.0f / 2.2f) : 1.0f;
}

/* a rasterizer light just submitted: drawn again at another of its
markers (a lens-flare-only light's, object_lights.c: its flare is drawn on
each, a Banshee's two wings, a Ghost's three running lights), with its
color, reach and flags, the marker's forward its axis; number its
marker's among the light's markers (from 1: the light itself is on the
first), which keys it from frame to frame whatever other lights' copies
come and go; each copy reaches the clusters its sphere floods into from
where it is */
void dynamic_lights_copy(
	long rasterizer_light_index,
	short number,
	real_point3d const *position,
	real_vector3d const *axis)
{
	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW ||
		dynamic_lights.copy_count >= MAXIMUM_LIGHTS_PER_WINDOW)
	{
		return;
	}
	dynamic_lights.copies[dynamic_lights.copy_count].rasterizer_light_index = rasterizer_light_index;
	dynamic_lights.copies[dynamic_lights.copy_count].number = number;
	dynamic_lights.copies[dynamic_lights.copy_count].position = *position;
	dynamic_lights.copies[dynamic_lights.copy_count].axis = *axis;
	dynamic_lights.copy_count++;
}

/* the frame's copies (dynamic_lights_copy) after the game's count of
lights, in the room they leave; the count of all */
static long dynamic_lights_copies(
	long count)
{
	long game_count = count;
	short copy_index;

	for (copy_index = 0; copy_index < dynamic_lights.copy_count && count < MAXIMUM_LIGHTS_PER_WINDOW; copy_index++)
	{
		struct halo_dynamic_light *out = &dynamic_lights.lights[count];
		real_point3d const *position = &dynamic_lights.copies[copy_index].position;
		short clusters[MAXIMUM_CLUSTERS_PER_STRUCTURE];
		short cluster_count;
		struct location location;

		if (dynamic_lights.copies[copy_index].rasterizer_light_index >= game_count)
			continue;
		scenario_location_from_point(&location, position);
		if (location.cluster_index == NONE)
			continue;
		*out = dynamic_lights.lights[dynamic_lights.copies[copy_index].rasterizer_light_index];
		dynamic_lights.owners[count] = dynamic_lights.owners[dynamic_lights.copies[copy_index].rasterizer_light_index];
		dynamic_lights.objects[count] = dynamic_lights.objects[dynamic_lights.copies[copy_index].rasterizer_light_index];
		/* (its light's key and its own number among that light's copies,
		not among the frame's: another light's copies coming or going leave
		it as it was) */
		dynamic_lights.keys[count] = dynamic_lights.keys[dynamic_lights.copies[copy_index].rasterizer_light_index] ?
			DYNAMIC_LIGHTS_COPY_KEY(dynamic_lights.keys[dynamic_lights.copies[copy_index].rasterizer_light_index],
			dynamic_lights.copies[copy_index].number) : 0;
		out->position[0] = position->x;
		out->position[1] = position->y;
		out->position[2] = position->z;
		out->axis[0] = dynamic_lights.copies[copy_index].axis.i;
		out->axis[1] = dynamic_lights.copies[copy_index].axis.j;
		out->axis[2] = dynamic_lights.copies[copy_index].axis.k;
		cluster_count = door_portals_clusters_in_sphere(location.cluster_index, position, out->radius,
			MAXIMUM_CLUSTERS_PER_STRUCTURE, clusters);
		dynamic_lights_clusters(count, MIN(cluster_count, MAXIMUM_CLUSTERS_PER_STRUCTURE), clusters);
		count++;
	}
	dynamic_lights.copy_count = 0;
	return count;
}

/* a rasterizer light just submitted: the clusters it reaches (count less
than 0: every one) */
void dynamic_lights_clusters(
	long rasterizer_light_index,
	short count,
	short const *clusters)
{
	unsigned int bit;
	short index;

	if (rasterizer_light_index < 0 || rasterizer_light_index >= MAXIMUM_LIGHTS_PER_WINDOW || count < 0)
		return;
	dynamic_lights.clusters_noted[rasterizer_light_index] = TRUE;
	bit = 1u << (rasterizer_light_index & 31);
	for (index = 0; index < count; index++)
	{
		if (clusters[index] >= 0 && clusters[index] < HALO_DYNAMIC_LIGHTS_CLUSTERS)
			dynamic_lights.cluster_masks[clusters[index]][rasterizer_light_index >> 5] |= bit;
	}
}

/* the structure's 3D BSP to the renderer (xgpu.h's world), when the
structure is another than it has: its nodes, planes and leaves' clusters;
none (no node) for one past the world's room, whose lights then reach every
cluster */
static void dynamic_lights_world(
	void)
{
	struct structure_bsp *structure = global_structure_bsp_get();
	struct bsp3d *bsp = structure ? global_bsp3d_get() : NULL;
	static unsigned int (*texels)[4];
	long node_count, plane_count, leaf_count, plane_base, leaf_base, total, index;

	if (!bsp)
		return;
	node_count = bsp->nodes.count;
	plane_count = bsp->planes.count;
	leaf_count = structure->leaves.count;
	if (dynamic_lights.world_nodes == bsp->nodes.address && dynamic_lights.world_node_count == node_count &&
		dynamic_lights.world_leaves == structure->leaves.address && dynamic_lights.world_leaf_count == leaf_count)
	{
		return;
	}
	dynamic_lights.world_nodes = bsp->nodes.address;
	dynamic_lights.world_node_count = node_count;
	dynamic_lights.world_leaves = structure->leaves.address;
	dynamic_lights.world_leaf_count = leaf_count;
	plane_base = 1 + node_count;
	leaf_base = plane_base + plane_count;
	total = leaf_base + (leaf_count + 3) / 4;
	if (!texels)
		texels = malloc(HALO_DYNAMIC_LIGHTS_WORLD_TEXELS * sizeof(*texels));
	if (!texels)
		return;
	if (node_count <= 0 || node_count >= LONG_MAX / 4 || plane_count < 0 || leaf_count < 0 ||
		total > HALO_DYNAMIC_LIGHTS_WORLD_TEXELS)
	{
		platform_log("dynamic lights: the structure's BSP (%ld nodes, %ld planes, %ld leaves) is more than the "
			"renderer has room for: its lights reach every cluster", node_count, plane_count, leaf_count);
		memset(texels[0], 0, sizeof(texels[0]));
		halo_dynamic_lights_world(texels, 1);
		return;
	}
	texels[0][0] = (unsigned int)node_count;
	texels[0][1] = (unsigned int)plane_base;
	texels[0][2] = (unsigned int)leaf_base;
	texels[0][3] = (unsigned int)leaf_count;
	for (index = 0; index < node_count; index++)
	{
		struct bsp3d_node const *node = TAG_BLOCK_GET_ELEMENT(&bsp->nodes, index, struct bsp3d_node);

		texels[1 + index][0] = (unsigned int)node->plane_designator;
		texels[1 + index][1] = (unsigned int)node->children[0];
		texels[1 + index][2] = (unsigned int)node->children[1];
		texels[1 + index][3] = 0;
	}
	for (index = 0; index < plane_count; index++)
		memcpy(texels[plane_base + index], TAG_BLOCK_GET_ELEMENT(&bsp->planes, index, real_plane3d), 16);
	memset(texels[leaf_base], 0xff, (size_t)(total - leaf_base) * sizeof(texels[0]));
	for (index = 0; index < leaf_count; index++)
	{
		short cluster = TAG_BLOCK_GET_ELEMENT(&structure->leaves, index, struct structure_leaf)->cluster_index;

		texels[leaf_base + index / 4][index % 4] = cluster >= 0 && cluster < structure->clusters.count ?
			(unsigned int)cluster : 0xffffffffu;
	}
	halo_dynamic_lights_world(texels, (int)total);
}

/* an effect's light on a node of a weapon held by a unit: where it is seen, on the first-person weapon's marker of the same
name, as far from it as from the held weapon's, when the weapon is a local
player's drawn in first person; FALSE if it is not */
boolean dynamic_lights_first_person(
	long weapon_index,
	real_point3d *position)
{
	static char const *const marker_names[] = { "primary trigger", "secondary trigger" };
	real nearest = 0.5f * 0.5f;
	real_point3d moved;
	boolean found = FALSE;
	short name_index;

	for (name_index = 0; name_index < NUMBEROF(marker_names); name_index++)
	{
		struct object_marker held, first_person;
		real distance;

		if (object_get_marker_by_name(weapon_index, marker_names[name_index], &held, 1) < 1)
			continue;
		distance = distance_squared3d(&held.matrix.position, position);
		if (distance >= nearest)
			continue;
		if (first_person_weapon_get_marker_by_name_render(weapon_index, marker_names[name_index], &first_person, 1) < 1)
			continue;
		nearest = distance;
		moved.x = first_person.matrix.position.x + (position->x - held.matrix.position.x);
		moved.y = first_person.matrix.position.y + (position->y - held.matrix.position.y);
		moved.z = first_person.matrix.position.z + (position->z - held.matrix.position.z);
		found = TRUE;
	}
	if (found)
		*position = moved;
	return found;
}

/* the enhanced dynamic lights' own lights (light_sources.c: what glows
with no light in its tags) after the game's count of them, in the room the
game's leave: each with the clusters its sphere floods into from where it
is, as the game's (none drawn where it is in no cluster); the count of all */
static long dynamic_lights_sources(
	long count)
{
	static unsigned long long timed, timed_frames;
	boolean logging = config_boolean("debug.dynamic_lights_log") != 0;
	unsigned long long start = logging ? platform_shared_clock_microseconds() : 0;
	struct light_source const *sources;
	short source_count = light_sources_gather(&sources, (short)(MAXIMUM_LIGHTS_PER_WINDOW - count));
	short source_index;

	for (source_index = 0; source_index < source_count && count < MAXIMUM_LIGHTS_PER_WINDOW; source_index++)
	{
		struct light_source const *source = &sources[source_index];
		struct halo_dynamic_light *out = &dynamic_lights.lights[count];
		short clusters[MAXIMUM_CLUSTERS_PER_STRUCTURE];
		short cluster_count;
		struct location location;

		scenario_location_from_point(&location, &source->position);
		if (location.cluster_index == NONE)
			continue;
		cluster_count = door_portals_clusters_in_sphere(location.cluster_index, &source->position, source->radius,
			MAXIMUM_CLUSTERS_PER_STRUCTURE, clusters);
		out->position[0] = source->position.x;
		out->position[1] = source->position.y;
		out->position[2] = source->position.z;
		out->radius = source->radius;
		out->color[0] = source->color.red;
		out->color[1] = source->color.green;
		out->color[2] = source->color.blue;
		out->axis[0] = source->facing.i;
		out->axis[1] = source->facing.j;
		out->axis[2] = source->facing.k;
		/* (a display's: a panel of the light model, xgpu.h, a disk of its
		area, its radius squared in cone_offset; another's every way) */
		out->cone_scale = 0.0f;
		out->cone_offset = 1.0f;
		out->flags = source->halo ? HALO_DYNAMIC_LIGHT_HALO | HALO_DYNAMIC_LIGHT_PROJECTILE : 0;
		/* (classic: a display's by the cosine off its face, none behind it,
		as a lit panel shines; another's every way) */
		if (!lt_lights_pass_on())
		{
			out->cone_scale = source->facing.i != 0.0f || source->facing.j != 0.0f || source->facing.k != 0.0f ? 1.0f : 0.0f;
			out->cone_offset = out->cone_scale != 0.0f ? 0.0f : 1.0f;
		}
		else if ((source->facing.i != 0.0f || source->facing.j != 0.0f || source->facing.k != 0.0f) && source->area > 0.0f)
		{
			out->cone_scale = 1.0f;
			out->cone_offset = source->area / _pi;
			out->flags |= HALO_DYNAMIC_LIGHT_PANEL;
		}
		dynamic_lights.owners[count] = 0;
		dynamic_lights.objects[count] = NONE;
		dynamic_lights.keys[count] = source->key;
		dynamic_lights.baked[count] = source->baked;
		dynamic_lights_clusters(count, MIN(cluster_count, MAXIMUM_CLUSTERS_PER_STRUCTURE), clusters);
		if (!(dynamic_lights.frame % 72) && source_index < 4 && config_boolean("debug.dynamic_lights_log"))
		{
			platform_log("dynamic lights: frame %lu: own light %ld at %.2f %.2f %.2f radius %.2f color %.2f %.2f %.2f",
				dynamic_lights.frame, count, (double)source->position.x, (double)source->position.y,
				(double)source->position.z, (double)source->radius, (double)source->color.red,
				(double)source->color.green, (double)source->color.blue);
		}
		count++;
	}
	/* (debug.dynamic_lights_log: what they cost, on average) */
	if (logging)
	{
		timed += platform_shared_clock_microseconds() - start;
		if (++timed_frames == 600)
		{
			platform_log("dynamic lights: own lights %.1f us a frame (600 frames)", (double)timed / 600.0);
			timed = timed_frames = 0;
		}
	}
	return count;
}

static void dynamic_lights_shadowed(long count);
static void dynamic_lights_flashlights(long count);
long dynamic_lights_bounce(struct halo_dynamic_light *lights, long count);

/* the frame's lights (rasterizer_lights, as lights_preprocess_scene
submitted them) to the renderer */
void dynamic_lights_submit(
	void)
{
	long light_index;
	long count = MIN(rasterizer_lights.light_count, MAXIMUM_LIGHTS_PER_WINDOW);
	long total;
	real_vector3d right;
	float camera[3], right_axis[3], up_axis[3];
	long logged = 0;
	boolean unlit;

	dynamic_lights.frame++;
	for (light_index = 0; light_index < count; light_index++)
	{
		struct rasterizer_light_submit_parameters const *light = &rasterizer_lights.lights[light_index];
		struct halo_dynamic_light *out = &dynamic_lights.lights[light_index];

		out->position[0] = light->position.x;
		out->position[1] = light->position.y;
		out->position[2] = light->position.z;
		out->radius = light->radius;
		out->color[0] = light->color.red;
		out->color[1] = light->color.green;
		out->color[2] = light->color.blue;
		if (dynamic_lights.brightness[light_index] > 0.0f)
		{
			out->color[0] *= dynamic_lights.brightness[light_index];
			out->color[1] *= dynamic_lights.brightness[light_index];
			out->color[2] *= dynamic_lights.brightness[light_index];
		}
		out->axis[0] = light->forward.i;
		out->axis[1] = light->forward.j;
		out->axis[2] = light->forward.k;
		out->cone_scale = dynamic_lights.cone_scale[light_index];
		out->cone_offset = dynamic_lights.cone_offset[light_index];
		out->flags = dynamic_lights.flags[light_index];
		/* (classic: the flashlight on the models it lights, as bright as the
		game's model lighting has it: the enhanced lights' falloff fades out
		toward the radius, where the game's spotlight is near its full
		strength; its gel lights the structure in the game's own pass. The
		ray-traced lighting's light model lights it through its gel instead) */
		if ((out->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT) && (out->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) &&
			!lt_lights_pass_on())
		{
			out->radius *= DYNAMIC_LIGHTS_FLASHLIGHT_REACH;
			out->color[0] *= DYNAMIC_LIGHTS_FLASHLIGHT_BRIGHTNESS;
			out->color[1] *= DYNAMIC_LIGHTS_FLASHLIGHT_BRIGHTNESS;
			out->color[2] *= DYNAMIC_LIGHTS_FLASHLIGHT_BRIGHTNESS;
		}
		if (logged < 4 && (out->flags & HALO_DYNAMIC_LIGHT_PROJECTILE) && config_boolean("debug.dynamic_lights_log"))
		{
			logged++;
			platform_log("dynamic lights: frame %lu tick %ld + %.3f: projectile light %ld drawn at %.4f %.4f %.4f "
				"(the tick's %.4f %.4f %.4f) radius %.3f color %.2f %.2f %.2f", dynamic_lights.frame, game_time_get(),
				(double)game_time_get_tick_fraction(), light_index, (double)out->position[0], (double)out->position[1],
				(double)out->position[2], (double)dynamic_lights.tick_position[light_index].x,
				(double)dynamic_lights.tick_position[light_index].y, (double)dynamic_lights.tick_position[light_index].z,
				(double)out->radius, (double)out->color[0], (double)out->color[1], (double)out->color[2]);
		}
	}
	if (!(dynamic_lights.frame % 72) && config_boolean("debug.dynamic_lights_log"))
	{
		long projectiles = 0, models_only = 0;

		for (light_index = 0; light_index < count; light_index++)
		{
			projectiles += (dynamic_lights.lights[light_index].flags & HALO_DYNAMIC_LIGHT_PROJECTILE) != 0;
			models_only += (dynamic_lights.lights[light_index].flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) != 0;
		}
		platform_log("dynamic lights: frame %lu: %ld lights, %ld projectiles', %ld models' only; camera %.2f %.2f %.2f "
			"forward %.2f %.2f %.2f", dynamic_lights.frame, count, projectiles, models_only, render.camera.position.x,
			render.camera.position.y, render.camera.position.z, render.camera.forward.i, render.camera.forward.j,
			render.camera.forward.k);
	}
	cross_product3d(&render.camera.forward, &render.camera.up, &right);
	camera[0] = render.camera.position.x;
	camera[1] = render.camera.position.y;
	camera[2] = render.camera.position.z;
	right_axis[0] = right.i;
	right_axis[1] = right.j;
	right_axis[2] = right.k;
	up_axis[0] = render.camera.up.i;
	up_axis[1] = render.camera.up.j;
	up_axis[2] = render.camera.up.k;
	/* (a light whose clusters were not noted reaches every one) */
	for (light_index = 0; light_index < count; light_index++)
	{
		long cluster;

		if (dynamic_lights.clusters_noted[light_index])
			continue;
		for (cluster = 0; cluster < HALO_DYNAMIC_LIGHTS_CLUSTERS; cluster++)
			dynamic_lights.cluster_masks[cluster][light_index >> 5] |= 1u << (light_index & 31);
	}
	dynamic_lights_world();
	/* (a map's preparation's pass lit by none of the renderer's lights: none
	of the port's own lights or the bounce's either, so that its draws are
	those of play no light reaches, the models' and the structure's drawn
	without the lights' loop: render_enumerate_unlit) */
	unlit = render_enumerate_unlit();
	total = dynamic_lights_copies(count);
	if (!unlit)
		total = dynamic_lights_sources(total);
	dynamic_lights_shadowed(total);
	if (lt_lights_pass_on())
		dynamic_lights_flashlights(count);
	/* (the light-transport pass's list: every light as drawn, by its key, lt_lights.c) */
	lt_lights_frame(dynamic_lights.lights, dynamic_lights.keys, dynamic_lights.objects, dynamic_lights.owners,
		dynamic_lights.baked, count, total);
	memset(dynamic_lights.keys, 0, sizeof(dynamic_lights.keys));
	memset(dynamic_lights.baked, 0, sizeof(dynamic_lights.baked));
	if (!unlit)
		total = dynamic_lights_bounce(dynamic_lights.lights, total); /* (dynamic_lights_bounce.c) */
	/* (every light's falloff on the structure: the game's distance
	attenuation, as its gel pass samples it, xgpu.h) */
	if (lt_lights_pass_on())
	{
		void const *attenuation = NULL;

		rasterizer_environment_port_gel_textures(NONE, NULL, &attenuation);
		halo_dynamic_lights_attenuation(attenuation);
	}
	halo_dynamic_lights_set(dynamic_lights.lights, (int)total, camera, right_axis, up_axis,
		(unsigned int const (*)[4])dynamic_lights.cluster_masks);
	memset(dynamic_lights.cluster_masks, 0, sizeof(dynamic_lights.cluster_masks));
	memset(dynamic_lights.clusters_noted, 0, sizeof(dynamic_lights.clusters_noted));
	/* (each submit notes its own again; one not noted is omni) */
	for (light_index = 0; light_index < count; light_index++)
	{
		dynamic_lights.flags[light_index] = 0;
		dynamic_lights.cone_scale[light_index] = 0.0f;
		dynamic_lights.cone_offset[light_index] = 1.0f;
		dynamic_lights.brightness[light_index] = 0.0f;
		dynamic_lights.owners[light_index] = 0;
		dynamic_lights.objects[light_index] = NONE;
		dynamic_lights.tick_position[light_index] = rasterizer_lights.lights[light_index].position;
	}
}

/* ---------- which lights are drawn */

/* a light wholly behind the camera's plane by more than this (world units:
the VR eyes beside the head's centre and the late latch's turn, with room)
lights nothing in view, and goes after every light that may */
#define RANK_BEHIND_MARGIN 0.1f
#define RANK_BEHIND_SCALE (1.0f / 64.0f)

struct rank_entry
{
	real score;
	short index;
};

/* each light as it was last drawn (dynamic_lights_drawn: its handle,
where, how far it reached and its brightest channel), by its index */
static struct
{
	long light;
	real_point3d position;
	real radius;
	real intensity;
} lights_drawn[MAXIMUM_LIGHTS_PER_MAP];

/* a light's part of the view (dynamic_lights_rank): its intensity times how
wide its reach looks from the camera, far lower wholly behind the camera's
plane (forward: the camera's, unit length) */
static real dynamic_lights_score(real_point3d const *position, real radius, real intensity,
	real_vector3d const *forward)
{
	real_vector3d offset;
	real distance, score;

	radius = radius > 1.0e-3f ? radius : 1.0e-3f;
	vector_from_points3d(&render.camera.position, position, &offset);
	distance = magnitude3d(&offset);
	score = intensity * radius / (distance > radius ? distance : radius);
	if (dot_product3d(&offset, forward) < -(radius + RANK_BEHIND_MARGIN))
		score *= RANK_BEHIND_SCALE;
	return score;
}

static int rank_compare(void const *a, void const *b)
{
	struct rank_entry const *left = a;
	struct rank_entry const *right = b;

	if (left->score != right->score)
		return left->score > right->score ? -1 : 1;
	/* (the game's order among equals: the same lights each frame) */
	return left->index - right->index;
}

/* object_lights.c lights_preprocess_scene: a light submitted to the
rasterizer, as it is drawn (between the ticks: its blended color and
radius, on its marker as drawn), which ranks it next frame */
void dynamic_lights_drawn(long light, real_point3d const *position, real radius, real_rgb_color const *color)
{
	long index = DATUM_INDEX_TO_ABSOLUTE_INDEX(light);

	if (index >= MAXIMUM_LIGHTS_PER_MAP)
		return;
	lights_drawn[index].light = light;
	lights_drawn[index].position = *position;
	lights_drawn[index].radius = radius;
	lights_drawn[index].intensity = MAX(color->red, MAX(color->green, color->blue));
}

/* port/linux/game/world_scene.c (display.ambient_occlusion "world"'s light
list): the light last drawn at an absolute index (dynamic_lights_drawn),
its handle, where and how far it reached; FALSE if none has been. It only
reads */
boolean dynamic_lights_drawn_get(long index, long *light, real_point3d *position, real *radius)
{
	if (index < 0 || index >= MAXIMUM_LIGHTS_PER_MAP || !lights_drawn[index].light)
		return FALSE;
	*light = lights_drawn[index].light;
	*position = lights_drawn[index].position;
	*radius = lights_drawn[index].radius;
	return TRUE;
}

/* object_lights.c lights_preprocess_scene: the scene's lights (light
handles, as structure_visibility_find_objects found them), most visible
first when there are more than the rasterizer has room for (none moved
otherwise: the order is the game's). A light's part of the view is its
intensity (its colour's brightest channel) times how wide its reach looks
from the camera: its radius over its distance, at most 1 (the camera in
it), so a near light and a far wide one both rank high, a small far one
last; and far lower wholly behind the camera's plane. Each as it was drawn
last frame (dynamic_lights_drawn: as the frame blends it between ticks, a
frame old), else as the game has it (describe: the tick's), and one of no
known brightness (new this frame) as bright. */
void dynamic_lights_rank(long *lights, short count,
	void (*describe)(long light, real_point3d *position, real *radius, real *intensity))
{
	static struct rank_entry entries[MAXIMUM_RENDERED_LIGHTS];
	static long ranked[MAXIMUM_RENDERED_LIGHTS];
	real_vector3d forward = render.camera.forward;
	short index;

	if (count <= MAXIMUM_LIGHTS_PER_WINDOW || count > MAXIMUM_RENDERED_LIGHTS)
		return;
	if (normalize3d(&forward) <= 0.0f)
		forward.i = forward.j = forward.k = 0.0f;
	for (index = 0; index < count; index++)
	{
		real_point3d position;
		real radius = 0.0f, intensity = 0.0f;
		long drawn = DATUM_INDEX_TO_ABSOLUTE_INDEX(lights[index]);

		if (drawn < MAXIMUM_LIGHTS_PER_MAP && lights_drawn[drawn].light == lights[index])
		{
			position = lights_drawn[drawn].position;
			radius = lights_drawn[drawn].radius;
			intensity = lights_drawn[drawn].intensity;
		}
		else
		{
			describe(lights[index], &position, &radius, &intensity);
			if (!(intensity > 0.0f))
				intensity = 1.0f;
		}
		entries[index].score = dynamic_lights_score(&position, radius, intensity, &forward);
		entries[index].index = index;
	}
	qsort(entries, count, sizeof(*entries), rank_compare);
	for (index = 0; index < count; index++)
		ranked[index] = lights[entries[index].index];
	memcpy(lights, ranked, count * sizeof(*lights));
}

/* ---------- which lights cast shadows (display.shadows "ray_traced")

docs/dynamic-shadows-plan.md, stage 1: of the frame's lights as they go to
the renderer (the game's, their copies and the port's own), at most
HALO_DYNAMIC_LIGHTS_SHADOWED cast shadows, those whose shadows show:

- not a flashlight: it casts its own, by its own map, on every surface
  (dynamic_lights_flashlights, below);
- a light that reaches SHADOWED_LEAST_RADIUS or more and has lasted
  SHADOWED_LEAST_AGE (an explosion, a fire, a lamp, a large charged shot,
  the port's own larger lights). A plasma bolt, a needle or a muzzle flash
  reaches 1.5 to 2 world units for a few frames: its shadow is a speck on
  a surface it barely lights, gone before it is seen, and takes a slot;
- not a light that lights only the models (a lamp's gel, which the game's
  own pass draws on the structure), nor a glow of the port's own standing
  in for what the lightmap lights (a powerup's, a teleporter's: Prisoner's
  overshield took a slot and drew a speckled shadow at the feet), nor a
  light out of reach or dark;
- by the same part of the view dynamic_lights_rank ranks the lights by, a
  light chosen last frame counted SHADOWED_KEEP times as much (a light
  keeps its shadow until another clearly passes it: no flicker between two
  alike).

None qualifying, none is chosen and the renderer draws no pass. Each with
its disk's radius, the size of what glows (a projectile's small; an
explosion's or a fire's larger, by its reach), and its owner, whose
collision meshes the rays leave out. The renderer traces them by ray queries where it has
them (xgpu.h's XGPU_DYNAMIC_LIGHTS_SHADOWED); none where the setting is off
(and with HALO_LIGHT_SHADOWS_OFF=1, for measuring). */
#define SHADOWED_DISK_PROJECTILE 0.05f
#define SHADOWED_DISK_SCALE 0.05f
#define SHADOWED_DISK_LEAST 0.1f
#define SHADOWED_DISK_MOST 0.5f
/* (world units; seconds of game time; how much more a light chosen last
frame counts) */
#define SHADOWED_LEAST_RADIUS 3.0f
#define SHADOWED_LEAST_AGE 0.1f
#define SHADOWED_KEEP 1.5f

/* the lights that qualified last frame, by key, and since when (seconds);
those chosen */
static struct
{
	struct
	{
		unsigned long long key;
		real since;
	} seen[2][MAXIMUM_LIGHTS_PER_WINDOW];
	int seen_count[2];
	int current;
	unsigned long long chosen[HALO_DYNAMIC_LIGHTS_SHADOWED];
	int chosen_count;
} shadowed_history;

/* the lights of count (each one's key, 0 not known; baked: a glow
standing in for the lightmap's light) to shadow at now (seconds), into indices with
their scores, most first; how many (at most HALO_DYNAMIC_LIGHTS_SHADOWED).
It keeps what it chose and how long each has lasted for the next frame */
static int dynamic_lights_shadow_pick(struct halo_dynamic_light const *lights, unsigned long long const *keys,
	boolean const *baked, long count, real now, real_vector3d const *forward, int *indices, real *scores)
{
	int last = shadowed_history.current, next = !last;
	int chosen = 0, slot, seen;
	long light_index;

	shadowed_history.seen_count[next] = 0;
	for (light_index = 0; light_index < count; light_index++)
	{
		struct halo_dynamic_light const *light = &lights[light_index];
		real intensity = MAX(light->color[0], MAX(light->color[1], light->color[2]));
		unsigned long long key = keys[light_index];
		real_point3d position;
		real score, since = now;

		if (!(light->radius > 0.0f) || !(intensity > 0.0f) ||
			(light->flags & (HALO_DYNAMIC_LIGHT_MODELS_ONLY | HALO_DYNAMIC_LIGHT_FLASHLIGHT)))
			continue;
		if (light->radius < SHADOWED_LEAST_RADIUS || baked[light_index])
			continue;
		/* (how long it has lasted: since it first qualified) */
		if (key)
		{
			for (seen = 0; seen < shadowed_history.seen_count[last]; seen++)
			{
				if (shadowed_history.seen[last][seen].key == key)
				{
					since = MIN(shadowed_history.seen[last][seen].since, now);
					break;
				}
			}
			if (shadowed_history.seen_count[next] < MAXIMUM_LIGHTS_PER_WINDOW)
			{
				shadowed_history.seen[next][shadowed_history.seen_count[next]].key = key;
				shadowed_history.seen[next][shadowed_history.seen_count[next]++].since = since;
			}
		}
		if (now - since < SHADOWED_LEAST_AGE)
			continue;
		position.x = light->position[0];
		position.y = light->position[1];
		position.z = light->position[2];
		score = dynamic_lights_score(&position, light->radius, intensity, forward);
		for (slot = 0; key && slot < shadowed_history.chosen_count; slot++)
		{
			if (shadowed_history.chosen[slot] == key)
			{
				score *= SHADOWED_KEEP;
				break;
			}
		}
		/* (kept the highest first; the earlier among equals) */
		for (slot = chosen; slot > 0 && scores[slot - 1] < score; slot--)
		{
			if (slot < HALO_DYNAMIC_LIGHTS_SHADOWED)
			{
				scores[slot] = scores[slot - 1];
				indices[slot] = indices[slot - 1];
			}
		}
		if (slot >= HALO_DYNAMIC_LIGHTS_SHADOWED)
			continue;
		scores[slot] = score;
		indices[slot] = (int)light_index;
		if (chosen < HALO_DYNAMIC_LIGHTS_SHADOWED)
			chosen++;
	}
	shadowed_history.current = next;
	shadowed_history.chosen_count = chosen;
	for (slot = 0; slot < chosen; slot++)
		shadowed_history.chosen[slot] = keys[indices[slot]];
	return chosen;
}

static void dynamic_lights_shadowed(long count)
{
	int indices[HALO_DYNAMIC_LIGHTS_SHADOWED];
	float disks[HALO_DYNAMIC_LIGHTS_SHADOWED];
	unsigned long owners[HALO_DYNAMIC_LIGHTS_SHADOWED];
	real scores[HALO_DYNAMIC_LIGHTS_SHADOWED];
	real_vector3d forward = render.camera.forward;
	int chosen = 0, slot;
	long light_index;
	/* (HALO_LIGHT_SHADOWS_OFF=1, measuring: none chosen, the rest as it is) */
	static int off = -1;

	if (off < 0)
		off = getenv("HALO_LIGHT_SHADOWS_OFF") != NULL;
	if (off || !rt_shadows_on())
	{
		halo_dynamic_lights_shadowed(NULL, NULL, NULL, 0);
		return;
	}
	if (normalize3d(&forward) <= 0.0f)
		forward.i = forward.j = forward.k = 0.0f;
	chosen = dynamic_lights_shadow_pick(dynamic_lights.lights, dynamic_lights.keys, dynamic_lights.baked, count,
		render_interpolation_game_time_sec(game_time_get()), &forward, indices, scores);
	for (slot = 0; slot < chosen; slot++)
	{
		struct halo_dynamic_light const *light = &dynamic_lights.lights[indices[slot]];
		real disk = light->radius * SHADOWED_DISK_SCALE;

		disks[slot] = (light->flags & HALO_DYNAMIC_LIGHT_PROJECTILE) ? SHADOWED_DISK_PROJECTILE :
			disk < SHADOWED_DISK_LEAST ? SHADOWED_DISK_LEAST : disk > SHADOWED_DISK_MOST ? SHADOWED_DISK_MOST : disk;
		owners[slot] = dynamic_lights.owners[indices[slot]];
	}
	/* (HALO_LIGHT_SHADOWS_VIEW, checking: the chosen and the rest, once a
	second, and each frame the chosen change) */
	{
		static int view = -1;
		static unsigned long long said, said_keys[HALO_DYNAMIC_LIGHTS_SHADOWED];
		static int said_count;
		unsigned long long now = platform_shared_clock_microseconds();
		boolean changed = chosen != said_count;

		if (view < 0)
			view = getenv("HALO_LIGHT_SHADOWS_VIEW") && strcmp(getenv("HALO_LIGHT_SHADOWS_VIEW"), "0") ? 1 : 0;
		for (slot = 0; view && !changed && slot < chosen; slot++)
			changed = said_keys[slot] != dynamic_lights.keys[indices[slot]];
		if (view && (now - said >= 1000000 || changed))
		{
			char line[4096];
			int at = snprintf(line, sizeof(line), "light shadows: frame %lu: %d of %ld lights shadowed",
				dynamic_lights.frame, chosen, count);

			said = now;
			said_count = chosen;
			for (slot = 0; slot < chosen; slot++)
				said_keys[slot] = dynamic_lights.keys[indices[slot]];
			for (light_index = 0; light_index < count && at > 0 && at < (int)sizeof(line); light_index++)
			{
				struct halo_dynamic_light const *light = &dynamic_lights.lights[light_index];
				struct object_datum *object = dynamic_lights.objects[light_index] != NONE ?
					object_try_and_get(dynamic_lights.objects[light_index]) : NULL;
				char const *name = object ? tag_get_name(object->definition_index) : NULL;
				char const *slash = name ? strrchr(name, '\\') : NULL;

				for (slot = 0; slot < chosen && indices[slot] != (int)light_index; slot++)
					;
				at += snprintf(line + at, sizeof(line) - (size_t)at, "; %s%ld%s%s%s at %.2f %.2f %.2f r %.2f "
					"key %llx%s%s", slot < chosen ? "[" : "", light_index, slot < chosen ? "]" : "",
					(light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT) ? " flashlight" :
					(light->flags & HALO_DYNAMIC_LIGHT_PROJECTILE) ? " projectile" : "",
					(light->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) ? " models" : "",
					(double)light->position[0], (double)light->position[1], (double)light->position[2],
					(double)light->radius, dynamic_lights.keys[light_index], name ? " on " : "",
					name ? (slash ? slash + 1 : name) : "");
				if (slot < chosen && at > 0 && at < (int)sizeof(line))
					at += snprintf(line + at, sizeof(line) - (size_t)at, " score %.3f disk %.2f owner %lu",
						(double)scores[slot], (double)disks[slot], owners[slot]);
				if (object && at > 0 && at < (int)sizeof(line))
					at += snprintf(line + at, sizeof(line) - (size_t)at, " type %d", (int)object->object.type);
			}
			platform_log("%s", line);
		}
	}
	if (chosen && !(dynamic_lights.frame % 72) && config_boolean("debug.dynamic_lights_log"))
	{
		platform_log("dynamic lights: frame %lu: %d shadowed, the first light %d disk %.2f owner %lu",
			dynamic_lights.frame, chosen, indices[0], (double)disks[0], owners[0]);
	}
	halo_dynamic_lights_shadowed(indices, disks, owners, chosen);
}

/* ---------- the enhanced flashlight (xgpu.h's XGPU_DYNAMIC_LIGHTS_FLASHLIGHT)

A unit's flashlight lights the structure, the models and the first-person
view in the renderer's one loop of the lights, through its own gel as the
game's gel pass projects it (rasterizer_environment_port_gel: its beam, up
and left as the pass has them, its functions turned), with the game's own
distance falloff, and the flashlight of the view, the one nearest its
camera, casts its shadow: one shadow map in the gel's frame, a ray a texel
from the light through the ray-traced scene where the GPU has ray queries,
its wearer, its own first-person weapon and hands, and its own body left
out. The game puts its light on the view's own axis, behind the eye
(first_person_weapon_center_flashlight): its gel is projected from there,
as the game's pass projects it, but a shadow cast from there is always
hidden behind what casts it, so its shadows are cast from its lamp on the
weapon in hand, at its barrel (first_person_weapon_flashlight_lamp), below
and beside the eye, where a shadow shows beside what casts it. With
HALO_FLASHLIGHT_SHADOWS_OFF=1 (measuring) it casts none. */

/* (frames: a flashlight seen this recently keeps the ray-traced scene
made each frame, world_ao.c) */
#define FLASHLIGHT_SCENE_KEEP 60

static struct
{
	unsigned long seen_frame;
	boolean seen;
	long gel_index;
	unsigned long gel_frame;
	boolean gel_result;
} flashlight;

/* object_lights.c's light_port_lights_structure: the flashlight's gel (a
bitmap group) could be made the renderer's, so the enhanced lights light
the structure with it (the game's gel pass leaves it out) */
boolean dynamic_lights_flashlight_gel(long gel_bitmap_group_index)
{
	void const *gel = NULL, *attenuation = NULL;

	if (gel_bitmap_group_index == NONE)
		return FALSE;
	if (flashlight.gel_index == gel_bitmap_group_index && flashlight.gel_frame == dynamic_lights.frame &&
		flashlight.gel_frame)
		return flashlight.gel_result;
	rasterizer_environment_port_gel_textures(gel_bitmap_group_index, &gel, &attenuation);
	flashlight.gel_index = gel_bitmap_group_index;
	flashlight.gel_frame = dynamic_lights.frame;
	flashlight.gel_result = gel && halo_dynamic_lights_gel(gel, attenuation);
	return flashlight.gel_result;
}

/* whether the flashlight casts its shadow: enhanced, with ray queries, a
flashlight seen lately; world_ao.c makes the ray-traced scene each frame
then */
int dynamic_lights_flashlight_shadows_on(void)
{
	static int off = -1;

	if (off < 0)
		off = getenv("HALO_FLASHLIGHT_SHADOWS_OFF") && strcmp(getenv("HALO_FLASHLIGHT_SHADOWS_OFF"), "0") ? 1 : 0;
	return !off && lt_lights_pass_on() && flashlight.seen && dynamic_lights.frame - flashlight.seen_frame <= FLASHLIGHT_SCENE_KEEP &&
		halo_dynamic_lights_enhanced() && halo_world_ao_ray_queries();
}

static void dynamic_lights_flashlights(long count)
{
	struct halo_flashlight flashlights[HALO_DYNAMIC_LIGHTS_FLASHLIGHTS];
	real nearest = 0.0f;
	int used = 0, slot, shadowed = -1;
	long light_index;

	for (light_index = 0; light_index < count && used < HALO_DYNAMIC_LIGHTS_FLASHLIGHTS; light_index++)
	{
		struct halo_dynamic_light const *light = &dynamic_lights.lights[light_index];
		struct halo_flashlight *out = &flashlights[used];
		real_vector3d forward, up, left;
		real dx, dy, dz, distance;

		if (!(light->flags & HALO_DYNAMIC_LIGHT_FLASHLIGHT) || (light->flags & HALO_DYNAMIC_LIGHT_MODELS_ONLY) ||
			!rasterizer_environment_port_gel(light_index, &forward, &up, &left, NULL, NULL))
			continue;
		out->light = (int)light_index;
		out->forward[0] = forward.i;
		out->forward[1] = forward.j;
		out->forward[2] = forward.k;
		out->up[0] = up.i;
		out->up[1] = up.j;
		out->up[2] = up.k;
		out->left[0] = left.i;
		out->left[1] = left.j;
		out->left[2] = left.k;
		out->shadowed = 0;
		out->owner = dynamic_lights.owners[light_index];
		out->lamp[0] = light->position[0];
		out->lamp[1] = light->position[1];
		out->lamp[2] = light->position[2];
		/* (its shadows from its lamp on the weapon in hand) */
		{
			real_point3d lamp;

			if (dynamic_lights.objects[light_index] != NONE &&
				first_person_weapon_flashlight_lamp(dynamic_lights.objects[light_index], &lamp))
			{
				out->lamp[0] = lamp.x;
				out->lamp[1] = lamp.y;
				out->lamp[2] = lamp.z;
			}
		}
		dx = light->position[0] - render.camera.position.x;
		dy = light->position[1] - render.camera.position.y;
		dz = light->position[2] - render.camera.position.z;
		distance = dx * dx + dy * dy + dz * dz;
		if (shadowed < 0 || distance < nearest)
		{
			shadowed = used;
			nearest = distance;
		}
		used++;
	}
	if (used)
	{
		flashlight.seen = TRUE;
		flashlight.seen_frame = dynamic_lights.frame;
	}
	/* (the view's own casts its shadow, the frame's ray-traced scene made
	for it first) */
	if (shadowed >= 0 && dynamic_lights_flashlight_shadows_on() && world_ao_rt_scene_ready())
	{
		world_ao_rt_frame();
		flashlights[shadowed].shadowed = 1;
	}
	for (slot = 0; slot < used && !(dynamic_lights.frame % 72) && config_boolean("debug.dynamic_lights_log"); slot++)
	{
		real lamp_x = flashlights[slot].lamp[0] - render.camera.position.x;
		real lamp_y = flashlights[slot].lamp[1] - render.camera.position.y;
		real lamp_z = flashlights[slot].lamp[2] - render.camera.position.z;

		platform_log("dynamic lights: frame %lu: flashlight %d (light %d) at %.3f %.3f %.3f (camera %.3f %.3f %.3f) "
			"lamp %.3f from the eye, %.3f along the view, forward %.2f %.2f %.2f up %.2f %.2f %.2f%s owner %lu",
			dynamic_lights.frame, slot, flashlights[slot].light,
			(double)dynamic_lights.lights[flashlights[slot].light].position[0],
			(double)dynamic_lights.lights[flashlights[slot].light].position[1],
			(double)dynamic_lights.lights[flashlights[slot].light].position[2], (double)render.camera.position.x,
			(double)render.camera.position.y, (double)render.camera.position.z,
			(double)square_root(lamp_x * lamp_x + lamp_y * lamp_y + lamp_z * lamp_z),
			(double)(lamp_x * render.camera.forward.i + lamp_y * render.camera.forward.j + lamp_z * render.camera.forward.k),
			(double)flashlights[slot].forward[0],
			(double)flashlights[slot].forward[1], (double)flashlights[slot].forward[2], (double)flashlights[slot].up[0],
			(double)flashlights[slot].up[1], (double)flashlights[slot].up[2], flashlights[slot].shadowed ? " shadowed" : "",
			flashlights[slot].owner);
	}
	halo_dynamic_lights_flashlights(flashlights, used);
}
