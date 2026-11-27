/*
WORLD_RT.C

display.ambient_occlusion "ray_traced"'s scene (world_rt.h, docs/world-ao.md
"Hardware ray queries"): the meshes the GPU's ray queries trace through,
made as the objects that need them are first drawn, and each frame's
instances of them, handed to the renderer (halo_world_ao_mesh, _instances:
port/linux/src/d3d8_device.c). The game is read through world_rt_source_*
(world_rt_game.c) and the proxies' instances and nodes
(world_proxies_source.h), as drawn: render_interpolation.c's pose.

A mesh is made once a map: each BSP's when it is first drawn (kept by its
index, so that a switch back to one makes nothing again: a10's start goes
0, 7, 0), a definition's nodes' when it is first drawn, the box once. They
are let go of when the map unloads. HALO_WORLD_AO_COUNTS logs, every 600
frames, the instances a frame and the meshes made.

With debug.rt_render_geometry (docs/rt-plan/1-geometry.md) the structure is
traced as drawn: its opaque render triangles, made the frame after its
collision mesh, which stands in for it until then (so that a load waits for
no more than it did) and is let go of as the render mesh takes its place.
*/

#include "world_ao.h"
#include "world_proxies_source.h"
#include "world_rt.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int config_boolean(const char *name);
void platform_log(const char *format, ...);
/* (xbox_kernel.c) the monotonic clock */
unsigned long long platform_shared_clock_microseconds(void);

/* ---------- constants */

enum
{
	/* the definitions kept (a table of them by tag index, open addressing:
	a power of two) */
	WORLD_RT_DEFINITIONS = 1024,
	/* the most objects of a frame read (world_scene.h's
	WORLD_SCENE_MAXIMUM_INSTANCES) */
	WORLD_RT_OBJECTS = 2048,
	/* the BSPs whose meshes are kept by index (a scenario's most:
	scenario_definitions.h's MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO) */
	WORLD_RT_BSPS = 32,
};

/* the least volume a mesh is turned by: a closed node's is far more */
#define WORLD_RT_ORIENT_LEAST 1.0e-6f

/* ---------- structures */

/* a definition's meshes: one for each node with collision geometry (0
none), or the box where none has */
struct world_rt_definition
{
	long definition; /* its tag index; -1 an empty entry */
	short node_count;
	short boxed;
	float sphere[4];
	unsigned short meshes[WORLD_RT_NODES];
};

/* ---------- globals */

static struct
{
	/* the meshes given ids (from 1), and whether any was sent since the map
	loaded */
	unsigned long next_mesh;
	int sent;
	/* the structure's mesh traced (0 none) and its BSP (-1 none), the box's;
	(debug.rt_render_geometry) whether its render mesh is to be made, the
	collision mesh's triangles; and each BSP's meshes made, by its index
	(its collision mesh let go of once its render mesh is made) */
	unsigned long structure, box;
	short bsp_index;
	int render_due;
	long collision_count;
	struct
	{
		unsigned long collision, render;
	} bsps[WORLD_RT_BSPS];
	/* the frame the instances were last handed for */
	unsigned long frame;
	int framed;
	/* (on the heap, made once: the guest's image holds no more) */
	struct world_rt_definition *definitions;
	struct world_ao_instance *instances;
	struct world_proxies_source_instance *objects;
	struct world_proxies_frame frames[WORLD_RT_NODES];
	float *triangles;
	long triangle_capacity;
	int full_logged;
	/* (measuring: HALO_WORLD_AO_COUNTS) */
	int counts_wanted;
	long count_frames, count_instances, count_triangles;
} world_rt = { 1, 0, 0, 0, -1 };

/* ---------- the arithmetic (world_rt.h) */

unsigned long world_rt_owner(long object_index)
{
	return object_index == -1 ? 0 : (unsigned long)(object_index & 0xFFFF) + 1;
}

float world_rt_orient(float *triangles, long count, float least)
{
	double volume = 0.0;
	long index;
	int k;

	/* (six times the volume: each triangle's tetrahedron with the origin) */
	for (index = 0; index < count; index++)
	{
		float const *a = triangles + index * 9, *b = a + 3, *c = a + 6;

		volume += (double)a[0] * ((double)b[1] * c[2] - (double)b[2] * c[1]) +
			(double)a[1] * ((double)b[2] * c[0] - (double)b[0] * c[2]) +
			(double)a[2] * ((double)b[0] * c[1] - (double)b[1] * c[0]);
	}
	volume /= 6.0;
	if (volume < -(double)least)
	{
		for (index = 0; index < count; index++)
		{
			float *b = triangles + index * 9 + 3, *c = b + 3;

			for (k = 0; k < 3; k++)
			{
				float swap = b[k];

				b[k] = c[k];
				c[k] = swap;
			}
		}
		volume = -volume;
	}
	return (float)volume;
}

void world_rt_box(float triangles[12 * 9])
{
	/* each face's corners counterclockwise seen from outside, as two
	triangles: its axis, its side, and its two other axes in turn */
	static const signed char faces[6][4][3] =
	{
		{ { 1, -1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { 1, -1, 1 } },
		{ { -1, -1, -1 }, { -1, -1, 1 }, { -1, 1, 1 }, { -1, 1, -1 } },
		{ { -1, 1, -1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, 1, -1 } },
		{ { -1, -1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { -1, -1, 1 } },
		{ { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } },
		{ { -1, -1, -1 }, { -1, 1, -1 }, { 1, 1, -1 }, { 1, -1, -1 } },
	};
	static const int corners[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
	int face, half, corner, k;

	for (face = 0; face < 6; face++)
	{
		for (half = 0; half < 2; half++)
		{
			for (corner = 0; corner < 3; corner++)
			{
				for (k = 0; k < 3; k++)
					triangles[((face * 2 + half) * 3 + corner) * 3 + k] = (float)faces[face][corners[half][corner]][k];
			}
		}
	}
}

void world_rt_transform(struct world_proxies_frame const *frame, float transform[3][4])
{
	int row, column;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
			transform[row][column] = frame->axes[column][row];
		transform[row][3] = frame->position[row];
	}
}

void world_rt_box_transform(struct world_proxies_frame const *frame, float const sphere[4], float transform[3][4])
{
	float half = sphere[3] * WORLD_RT_BOX_SHARE;
	int row, column;

	for (row = 0; row < 3; row++)
	{
		float position = frame->position[row];

		for (column = 0; column < 3; column++)
		{
			transform[row][column] = frame->axes[column][row] * half;
			position += frame->axes[column][row] * sphere[column];
		}
		transform[row][3] = position;
	}
}

/* ---------- the meshes */

/* room for count triangles (FALSE: none) */
static int world_rt_room(long count)
{
	float *grown;

	if (count <= world_rt.triangle_capacity)
		return 1;
	grown = realloc(world_rt.triangles, (size_t)count * 9 * sizeof(float));
	if (!grown)
		return 0;
	world_rt.triangles = grown;
	world_rt.triangle_capacity = count;
	return 1;
}

/* a mesh of count triangles (world_rt.triangles) sent under a new id (0:
the ids are spent, or none) */
static unsigned long world_rt_mesh_send(long count)
{
	unsigned long id;

	if (count <= 0 || count > WORLD_RT_MESH_TRIANGLES)
		return 0;
	if (world_rt.next_mesh > WORLD_RT_MESHES)
	{
		if (!world_rt.full_logged)
			platform_log("world ao: the ray-traced scene has its %d meshes: the objects drawn later have none",
				WORLD_RT_MESHES);
		world_rt.full_logged = 1;
		return 0;
	}
	id = world_rt.next_mesh++;
	halo_world_ao_mesh(id, (unsigned long)count, world_rt.triangles);
	world_rt.sent = 1;
	world_rt.count_triangles += count;
	return id;
}

static void world_rt_structure_render(void);

/* the structure's mesh: another BSP's as it is first drawn, its own kept
since where it has been drawn before */
static void world_rt_structure(void)
{
	short bsp_index = world_rt_source_bsp();
	long count;

	if (bsp_index == world_rt.bsp_index)
	{
		if (world_rt.render_due)
			world_rt_structure_render();
		return;
	}
	world_rt.bsp_index = bsp_index;
	world_rt.structure = 0;
	world_rt.collision_count = 0;
	world_rt.render_due = 0;
	if (bsp_index < 0 || bsp_index >= WORLD_RT_BSPS)
		return;
	/* (its render mesh, made before) */
	if (config_boolean("debug.rt_render_geometry"))
	{
		if (world_rt.bsps[bsp_index].render)
		{
			world_rt.structure = world_rt.bsps[bsp_index].render;
			return;
		}
		world_rt.render_due = 1;
	}
	/* (its collision mesh, made before) */
	if (world_rt.bsps[bsp_index].collision)
	{
		world_rt.structure = world_rt.bsps[bsp_index].collision;
		return;
	}
	count = world_rt_source_structure(bsp_index, NULL, 0);
	if (count > WORLD_RT_MESH_TRIANGLES || !world_rt_room(count))
		return;
	count = world_rt_source_structure(bsp_index, world_rt.triangles, count);
	world_rt.structure = world_rt.bsps[bsp_index].collision = world_rt_mesh_send(count);
	world_rt.collision_count = count;
	platform_log("world ao: the ray-traced scene's structure, BSP %d: %ld triangles", (int)bsp_index, count);
}

/* (debug.rt_render_geometry) the structure's render mesh in its collision
mesh's place, the frame after that was made: its opaque triangles, the
alpha-tested and transparent ones counted and left out */
static void world_rt_structure_render(void)
{
	short bsp_index = world_rt.bsp_index;
	long count, alpha_tested, transparent;
	unsigned long mesh;
	unsigned long long started = platform_shared_clock_microseconds();

	world_rt.render_due = 0;
	count = world_rt_source_structure_render(bsp_index, WORLD_RT_SURFACES_OPAQUE, NULL, 0);
	alpha_tested = world_rt_source_structure_render(bsp_index, WORLD_RT_SURFACES_ALPHA_TESTED, NULL, 0);
	transparent = world_rt_source_structure_render(bsp_index, WORLD_RT_SURFACES_TRANSPARENT, NULL, 0);
	if (count <= 0 || count > WORLD_RT_MESH_TRIANGLES || !world_rt_room(count))
		return;
	count = world_rt_source_structure_render(bsp_index, WORLD_RT_SURFACES_OPAQUE, world_rt.triangles, count);
	mesh = world_rt_mesh_send(count);
	if (!mesh)
		return;
	/* (in its collision mesh's place, which is let go of) */
	world_rt.structure = world_rt.bsps[bsp_index].render = mesh;
	if (world_rt.bsps[bsp_index].collision)
		halo_world_ao_mesh_release(world_rt.bsps[bsp_index].collision);
	world_rt.bsps[bsp_index].collision = 0;
	platform_log("world ao: the ray-traced scene's structure, BSP %d, as drawn (debug.rt_render_geometry): %ld "
		"triangles (collision %ld), %ld alpha-tested and %ld transparent left out; read in %.2f ms", (int)bsp_index,
		count, world_rt.collision_count, alpha_tested, transparent,
		(double)(platform_shared_clock_microseconds() - started) / 1000.0);
}

/* a definition's meshes, made as it is first drawn (NULL: none can be
kept) */
static struct world_rt_definition *world_rt_definition(long definition)
{
	unsigned long slot = ((unsigned long)definition * 2654435761UL) & (WORLD_RT_DEFINITIONS - 1);
	struct world_rt_definition *entry;
	long probes;
	short node_index;
	int any = 0, item = 0;
	long triangles = 0;

	if (!world_rt.definitions)
	{
		world_rt.definitions = malloc(sizeof(*world_rt.definitions) * WORLD_RT_DEFINITIONS);
		if (!world_rt.definitions)
			return NULL;
		for (probes = 0; probes < WORLD_RT_DEFINITIONS; probes++)
			world_rt.definitions[probes].definition = -1;
	}
	for (probes = 0; probes < WORLD_RT_DEFINITIONS; probes++, slot = (slot + 1) & (WORLD_RT_DEFINITIONS - 1))
	{
		entry = &world_rt.definitions[slot];
		if (entry->definition == definition)
			return entry;
		if (entry->definition == -1)
			break;
	}
	if (probes == WORLD_RT_DEFINITIONS)
		return NULL;
	memset(entry, 0, sizeof(*entry));
	entry->definition = definition;
	entry->node_count = world_rt_source_definition(definition, entry->sphere, &item);
	if (entry->node_count > WORLD_RT_NODES)
		entry->node_count = WORLD_RT_NODES;
	for (node_index = 0; node_index < entry->node_count; node_index++)
	{
		long count = world_rt_source_node(definition, node_index, NULL, 0);

		if (count <= 0 || count > WORLD_RT_MESH_TRIANGLES || !world_rt_room(count))
			continue;
		count = world_rt_source_node(definition, node_index, world_rt.triangles, count);
		world_rt_orient(world_rt.triangles, count, WORLD_RT_ORIENT_LEAST);
		entry->meshes[node_index] = (unsigned short)world_rt_mesh_send(count);
		any = any || entry->meshes[node_index];
		triangles += entry->meshes[node_index] ? count : 0;
	}
	/* (none: an item's box about its bounding sphere, on its first node) */
	if (!any && item && entry->node_count > 0 && entry->sphere[3] > 0.0f && entry->sphere[3] <= WORLD_RT_BOX_LARGEST)
	{
		if (!world_rt.box && world_rt_room(12))
		{
			world_rt_box(world_rt.triangles);
			world_rt.box = world_rt_mesh_send(12);
		}
		entry->boxed = world_rt.box != 0;
	}
	/* (measuring) */
	if (world_rt.counts_wanted > 0)
		platform_log("world ao: the ray-traced scene's %s: %d nodes, %ld triangles%s", world_rt_source_name(definition),
			(int)entry->node_count, triangles, entry->boxed ? ", its box" : "");
	return entry;
}

/* ---------- the frame */

static struct world_ao_instance *world_rt_instance_add(long *count, unsigned long mesh, unsigned long owner,
	unsigned int kind)
{
	struct world_ao_instance *instance;

	if (*count >= WORLD_RT_INSTANCES)
		return NULL;
	instance = &world_rt.instances[(*count)++];
	memset(instance, 0, sizeof(*instance));
	instance->mesh = (unsigned int)mesh;
	instance->owner = (unsigned int)owner;
	instance->kind = kind;
	return instance;
}

void world_rt_frame(unsigned long frame)
{
	long object_count, object_index, count = 0;

	if (world_rt.framed && world_rt.frame == frame)
		return;
	world_rt.framed = 1;
	world_rt.frame = frame;
	if (!world_rt.counts_wanted)
		world_rt.counts_wanted = getenv("HALO_WORLD_AO_COUNTS") ? 1 : -1;
	if (!world_rt.instances)
		world_rt.instances = malloc(sizeof(*world_rt.instances) * WORLD_RT_INSTANCES);
	if (!world_rt.objects)
		world_rt.objects = malloc(sizeof(*world_rt.objects) * WORLD_RT_OBJECTS);
	if (!world_rt.instances || !world_rt.objects)
		return;
	world_rt_structure();
	if (world_rt.structure)
	{
		static const float identity[3][4] = { { 1.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f } };
		struct world_ao_instance *instance = world_rt_instance_add(&count, world_rt.structure, 0, WORLD_RT_STRUCTURE);

		if (instance)
			memcpy(instance->transform, identity, sizeof(identity));
	}
	/* (the objects; the static scenery the field and its pages hold, a
	tree or a rock, as the structure is: owner 0, so it stops a ray and
	occludes nothing, its occlusion the field's, not counted twice, yet a
	shadow's ray stops at it. Held weapons and equipment are not here
	(world_proxies_source_instances: their holder covers them, and they
	would shade the hands holding them), nor the first-person weapon and
	hands, drawn in a view of their own, not in the world) */
	object_count = world_proxies_source_instances(world_rt.objects, WORLD_RT_OBJECTS);
	for (object_index = 0; object_index < object_count && count < WORLD_RT_INSTANCES; object_index++)
	{
		struct world_proxies_source_instance const *object = &world_rt.objects[object_index];
		struct world_rt_definition const *definition;
		unsigned long owner = object->baked ? 0 : world_rt_owner(object->id);
		unsigned int kind = object->baked ? WORLD_RT_STRUCTURE : WORLD_RT_OBJECT;
		short node_count, node_index;

		definition = world_rt_definition(object->definition);
		if (!definition || definition->node_count <= 0)
			continue;
		node_count = world_proxies_source_nodes(object->id, world_rt.frames, WORLD_RT_NODES);
		if (node_count <= 0)
			continue;
		if (definition->boxed)
		{
			struct world_ao_instance *instance = world_rt_instance_add(&count, world_rt.box, owner, kind);

			if (instance)
				world_rt_box_transform(&world_rt.frames[0], definition->sphere, instance->transform);
			continue;
		}
		for (node_index = 0; node_index < node_count && node_index < definition->node_count; node_index++)
		{
			struct world_ao_instance *instance;

			if (!definition->meshes[node_index])
				continue;
			instance = world_rt_instance_add(&count, definition->meshes[node_index], owner, kind);
			if (instance)
				world_rt_transform(&world_rt.frames[node_index], instance->transform);
		}
	}
	halo_world_ao_instances(world_rt.instances, (unsigned long)count);
	/* (measuring) */
	if (world_rt.counts_wanted > 0)
	{
		world_rt.count_instances += count;
		if (++world_rt.count_frames >= 600)
		{
			printf("world ao: the ray-traced scene's %.1f instances a frame; %lu meshes made, %ld triangles\n",
				(double)world_rt.count_instances / 600.0, world_rt.next_mesh - 1, world_rt.count_triangles);
			fflush(stdout);
			world_rt.count_frames = world_rt.count_instances = 0;
		}
	}
}

void world_rt_map_unloaded(void)
{
	if (world_rt.sent)
		halo_world_ao_mesh(0, 0, NULL);
	world_rt.sent = 0;
	world_rt.next_mesh = 1;
	world_rt.structure = world_rt.box = 0;
	world_rt.bsp_index = -1;
	world_rt.render_due = 0;
	memset(world_rt.bsps, 0, sizeof(world_rt.bsps));
	world_rt.framed = 0;
	world_rt.full_logged = 0;
	world_rt.count_triangles = 0;
	free(world_rt.definitions);
	world_rt.definitions = NULL;
	free(world_rt.triangles);
	world_rt.triangles = NULL;
	world_rt.triangle_capacity = 0;
}
