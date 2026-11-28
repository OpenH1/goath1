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

With the ray-traced lighting (debug.rt_render_geometry "auto", the default,
and display.ray_traced_lighting; docs/rt-plan/1-geometry.md; "on" whatever
the budget and the lighting, "off" never) the scene is traced as drawn, and the
collision meshes are the fallback alone: until a render mesh is made, and
past the GPU's budget (halo_world_rt_budget: the render meshes' and the
posed skinned objects' bytes, estimated at WORLD_RT_TRIANGLE_BYTES a
triangle; logged once a map), where a definition's or the level's
collision meshes stand in for what is left. The structure is traced as
drawn: its render mesh, made the frame after its collision mesh,
which stands in for it until then (so that a load waits for no more than it
did) and is let go of as the render mesh takes its place. That mesh is
indexed, its opaque triangles then its alpha-tested ones (whose rays test
their alpha maps), with what a ray's hit reads (halo_world_rt_mesh): each
vertex's coordinates, normal and tangent, each triangle's material, and the
map's materials, each sent once, whose maps are named by the map's
textures' slots (each found again every frame: its texture's header, sent
as it changes, halo_world_rt_textures).
*/

#include "world_ao.h"
#include "world_proxies_source.h"
#include "world_rt.h"

/* (port/linux/game/lt_lights.c: the ray-traced lighting on) */
int lt_lights_pass_on(void);
/* (port/linux/game/world_proxies_game.c: a static placement the lightmaps hold, world_proxies.h) */
int world_proxies_source_lightmapped(long id);

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *config_string(const char *name);
/* (xgpu_stream_encode.c) the ray-traced scene's render geometry's budget
for this GPU, in MB (0: the backend has none) */
unsigned long halo_world_rt_budget(void);
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
	/* the objects the last frame's scene held with their own meshes, by
	their datum's absolute index (world_rt_object_traced) */
	WORLD_RT_TRACED = 8192,
	/* the BSPs whose meshes are kept by index (a scenario's most:
	scenario_definitions.h's MAXIMUM_STRUCTURE_BSPS_PER_SCENARIO) */
	WORLD_RT_BSPS = 32,
	/* the object lamps' triangles a frame holds (the backend's room:
	xgpu_vulkan_lt.c's LT_OBJECT_LAMPS), and the bitmaps decoded to find
	them kept */
	WORLD_RT_LAMPS = 512,
	WORLD_RT_LAMP_BITMAPS = 64,
};

/* an object lamp's radiance (linear, the pass's units): the live lights'
one k (lt_lights.c's LT_LIGHTS_REAL_*_POWER_SCALE) on the glow as the model
pass draws it, albedo x (mask x colour)^2.2 (tools/rt_truth's
HaloSelfIllum); a triangle glowing less on average (its luminance) is no
lamp, and its samples a side */
#define WORLD_RT_LAMP_K 0.48f
#define WORLD_RT_LAMP_LEAST 0.02f
#define WORLD_RT_LAMP_SAMPLES 6
/* (the maps read at their first level no larger than this each way) */
#define WORLD_RT_LAMP_TEXELS 128

/* the least volume a mesh is turned by: a closed node's is far more */
#define WORLD_RT_ORIENT_LEAST 1.0e-6f

/* ---------- structures */

/* a material of the map: its shader, its lightmap page, the scale its
alpha map is sampled at (a model's) */
struct world_rt_material_key
{
	long shader;
	struct world_rt_source_map lightmap;
	float alpha_scale[2];
};

/* a texture's slot: its map (bitmap NONE: an image's), and the header the
renderer was last given (NULL none) */
struct world_rt_slot
{
	struct world_rt_source_map map;
	unsigned int image;
	void const *header;
	int sent;
};

/* a definition's meshes: one for each node with collision geometry (0
none), or the box where none has */
struct world_rt_definition
{
	long definition; /* its tag index; -1 an empty entry */
	short node_count;
	short boxed;
	/* (as drawn) its meshes are its model's as drawn; or
	skinned, its one skinned mesh (meshes[0]) posed by its nodes' default
	inverses (heap, node_count of them) */
	short drawn, skinned;
	float (*inverses)[3][4];
	float sphere[4];
	unsigned short meshes[WORLD_RT_NODES];
	/* (as drawn) its triangles; and (skinned, past the budget) its
	collision meshes, made the first time one is wanted */
	long triangles;
	short collision_made;
	unsigned short collision[WORLD_RT_NODES];
	/* (its collision meshes) each node's of its other permutations (a
	region's other pieces: a Jackal's shield off), permutation 1 first, 0
	none; and whether any node has them */
	unsigned short permuted[WORLD_RT_NODES][WORLD_RT_PERMUTATIONS - 1];
	short any_permuted;
	/* (as drawn, not skinned) its lamps: its self-illuminated triangles
	where they glow, in their node's space (heap, lamp_count of them) */
	struct world_rt_lamp *lamps;
	long lamp_count;
};

/* a lamp of a definition: its node, its corners in the node's space
(wound to glow toward their normal) and its radiance */
struct world_rt_lamp
{
	short node;
	float corners[9], radiance[3];
};

/* ---------- globals */

static struct
{
	/* the meshes given ids (from 1), and whether any was sent since the map
	loaded */
	unsigned long next_mesh;
	int sent;
	/* the structure's mesh traced (0 none) and its BSP (-1 none), the box's;
	(as drawn) whether its render mesh is to be made, the
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
	/* (as drawn) the map's materials (their keys; how many) and textures'
	slots (from 1: each its map or image, and the header last sent), the
	mesh being sent (its data), and the textures' slots to send */
	struct world_rt_material_key *materials;
	long material_count;
	struct world_rt_slot *slots;
	long slot_count;
	unsigned char *blob;
	unsigned long blob_capacity;
	/* the frame the instances were last handed for */
	unsigned long frame;
	int framed;
	/* (on the heap, made once: the guest's image holds no more) */
	struct world_rt_definition *definitions;
	struct world_ao_instance *instances;
	struct world_rt_pose *poses;
	struct world_proxies_source_instance *objects;
	struct world_proxies_frame frames[WORLD_RT_NODES];
	/* (an object's collision permutation of each node now) */
	signed char permutations[WORLD_RT_NODES];
	float *triangles;
	long triangle_capacity;
	int full_logged;
	/* debug.rt_render_geometry, this frame: whether the scene is as drawn,
	and whether the budget holds it ("auto"); the budget (bytes, 0 not yet
	asked), the render meshes' bytes held (estimated), the skinned objects'
	posed this frame, and whether it was logged as past */
	int render_geometry, budgeted;
	unsigned long long budget, render_bytes, skinned_bytes;
	int past_logged;
	/* (measuring: HALO_WORLD_AO_COUNTS) whether the first-person body's
	pose moved, frames posed and moved, the last pose's first transforms */
	long player_posed, player_moved;
	float player_last[4][3][4];
	/* the objects the last frame's scene held with meshes of their own
	(as drawn or their collision meshes, not an item's box), by their
	datum's absolute index: its id and that frame (world_rt_object_traced) */
	unsigned long traced_id[WORLD_RT_TRACED], traced_frame[WORLD_RT_TRACED];
	/* the frame's object lamps (world_rt_lamps): world-space corners,
	radiance, their objects' owners; and the bitmaps decoded to find them
	(ARGB words, NULL one that could not be read) */
	float (*lamp_corners)[9], (*lamp_radiance)[3];
	unsigned long *lamp_owners;
	long lamp_count;
	struct
	{
		long tag;
		short index;
		unsigned long width, height;
		unsigned long *texels;
	} lamp_bitmaps[WORLD_RT_LAMP_BITMAPS];
	int lamp_bitmap_count;
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

unsigned long world_rt_octahedral(float const vector[3])
{
	float length = fabsf(vector[0]) + fabsf(vector[1]) + fabsf(vector[2]), x, y;
	long packed[2];
	int k;

	if (!(length > 0.0f))
		return 0;
	x = vector[0] / length;
	y = vector[1] / length;
	/* (the lower half folded over the diagonals) */
	if (vector[2] < 0.0f)
	{
		float folded_x = (1.0f - fabsf(y)) * (x >= 0.0f ? 1.0f : -1.0f);

		y = (1.0f - fabsf(x)) * (y >= 0.0f ? 1.0f : -1.0f);
		x = folded_x;
	}
	for (k = 0; k < 2; k++)
	{
		float value = k ? y : x;

		value = value < -1.0f ? -1.0f : value > 1.0f ? 1.0f : value;
		packed[k] = (long)floorf(value * 32767.0f + 0.5f);
	}
	return ((unsigned long)packed[0] & 0xFFFFUL) | (((unsigned long)packed[1] & 0xFFFFUL) << 16);
}

int world_rt_inverse(float const in[3][4], float out[3][4])
{
	float determinant = in[0][0] * (in[1][1] * in[2][2] - in[1][2] * in[2][1]) -
		in[0][1] * (in[1][0] * in[2][2] - in[1][2] * in[2][0]) + in[0][2] * (in[1][0] * in[2][1] - in[1][1] * in[2][0]);
	float scale;
	int row, column;

	if (!(fabsf(determinant) > 1.0e-12f))
		return 0;
	scale = 1.0f / determinant;
	out[0][0] = (in[1][1] * in[2][2] - in[1][2] * in[2][1]) * scale;
	out[0][1] = (in[0][2] * in[2][1] - in[0][1] * in[2][2]) * scale;
	out[0][2] = (in[0][1] * in[1][2] - in[0][2] * in[1][1]) * scale;
	out[1][0] = (in[1][2] * in[2][0] - in[1][0] * in[2][2]) * scale;
	out[1][1] = (in[0][0] * in[2][2] - in[0][2] * in[2][0]) * scale;
	out[1][2] = (in[0][2] * in[1][0] - in[0][0] * in[1][2]) * scale;
	out[2][0] = (in[1][0] * in[2][1] - in[1][1] * in[2][0]) * scale;
	out[2][1] = (in[0][1] * in[2][0] - in[0][0] * in[2][1]) * scale;
	out[2][2] = (in[0][0] * in[1][1] - in[0][1] * in[1][0]) * scale;
	for (row = 0; row < 3; row++)
	{
		out[row][3] = 0.0f;
		for (column = 0; column < 3; column++)
			out[row][3] -= out[row][column] * in[column][3];
	}
	return 1;
}

void world_rt_compose(float const a[3][4], float const b[3][4], float out[3][4])
{
	float result[3][4];
	int row, column;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 4; column++)
		{
			result[row][column] = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column] +
				(column == 3 ? a[row][3] : 0.0f);
		}
	}
	memcpy(out, result, sizeof(result));
}

int world_rt_blended(struct world_rt_source_vertex const *vertices, long count)
{
	long index;

	for (index = 0; index < count; index++)
	{
		if (vertices[index].nodes[0] != vertices[index].nodes[1] && vertices[index].weight > 0.02f &&
			vertices[index].weight < 0.98f)
			return 1;
	}
	return 0;
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

/* debug.rt_render_geometry: as drawn where "on", and where "auto" with the
ray-traced lighting (display.ray_traced_lighting: lt_lights.h), within the
budget; else (an older true or false is read as on or off: port_config.c)
the collision geometry, as before it */
static void world_rt_render_mode(void)
{
	const char *mode = config_string("debug.rt_render_geometry");

	world_rt.render_geometry = !strcmp(mode, "on") || (!strcmp(mode, "auto") && lt_lights_pass_on());
	world_rt.budgeted = world_rt.render_geometry && strcmp(mode, "on") != 0;
	if (!world_rt.budget)
	{
		char const *given = getenv("HALO_RT_GEOMETRY_BUDGET_MB");
		unsigned long megabytes = given && atol(given) > 0 ? (unsigned long)atol(given) : halo_world_rt_budget();

		world_rt.budget = (unsigned long long)(megabytes ? megabytes : WORLD_RT_BUDGET_DEFAULT) << 20;
	}
}

/* whether triangles more as drawn fit the budget: the meshes kept (the
level's and definitions', made once) first, a skinned object posed this
frame in what they leave; past it logged once a map */
static int world_rt_render_fits(long triangles, int skinned)
{
	unsigned long long wanted = (unsigned long long)(triangles > 0 ? triangles : 0) * WORLD_RT_TRIANGLE_BYTES;

	if (!world_rt.budgeted || world_rt.render_bytes + (skinned ? world_rt.skinned_bytes : 0) + wanted <= world_rt.budget)
		return 1;
	if (!world_rt.past_logged)
		platform_log("world ao: the ray-traced scene as drawn is past this GPU's budget (%.0f MB; %.1f MB held, %.1f MB "
			"posed, %ld triangles more): the rest traced as collision geometry", (double)(world_rt.budget >> 20),
			(double)world_rt.render_bytes / 1048576.0, (double)world_rt.skinned_bytes / 1048576.0, triangles);
	world_rt.past_logged = 1;
	return 0;
}

/* a new mesh's id (0: the ids are spent) */
static unsigned long world_rt_id_take(void)
{
	if (world_rt.next_mesh > WORLD_RT_MESHES)
	{
		if (!world_rt.full_logged)
			platform_log("world ao: the ray-traced scene has its %d meshes: the objects drawn later have none",
				WORLD_RT_MESHES);
		world_rt.full_logged = 1;
		return 0;
	}
	world_rt.sent = 1;
	return world_rt.next_mesh++;
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
	if (world_rt.render_geometry)
	{
		if (world_rt.bsps[bsp_index].render)
		{
			world_rt.structure = world_rt.bsps[bsp_index].render;
			platform_log("world ao: the ray-traced scene's structure, BSP %d: its render mesh kept", (int)bsp_index);
			return;
		}
		world_rt.render_due = 1;
	}
	/* (its collision mesh, made before) */
	if (world_rt.bsps[bsp_index].collision)
	{
		world_rt.structure = world_rt.bsps[bsp_index].collision;
		platform_log("world ao: the ray-traced scene's structure, BSP %d: its mesh kept", (int)bsp_index);
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

/* ---------- the meshes as drawn */

/* a map's or an image's texture slot (0: none, or none is left) */
static unsigned int world_rt_slot(struct world_rt_source_map const *map, unsigned int image)
{
	long index;

	if ((!map || map->bitmap == -1) && !image)
		return 0;
	if (!world_rt.slots)
	{
		world_rt.slots = calloc(WORLD_RT_TEXTURES, sizeof(*world_rt.slots));
		if (!world_rt.slots)
			return 0;
		world_rt.slot_count = 1;
	}
	for (index = 1; index < world_rt.slot_count; index++)
	{
		struct world_rt_slot const *slot = &world_rt.slots[index];

		if (image ? slot->image == image : slot->map.bitmap == map->bitmap && slot->map.index == map->index)
			return (unsigned int)index;
	}
	if (world_rt.slot_count >= WORLD_RT_TEXTURES)
		return 0;
	memset(&world_rt.slots[index], 0, sizeof(world_rt.slots[index]));
	world_rt.slots[index].map.bitmap = -1;
	if (image)
		world_rt.slots[index].image = image;
	else
		world_rt.slots[index].map = *map;
	world_rt.slot_count++;
	return (unsigned int)index;
}

/* the textures' slots whose headers changed (or were never sent) sent, each
frame: a map's texture as the renderer has it now (loaded once asked for:
world_rt_source_texture), an image's once */
static void world_rt_textures_frame(void)
{
	struct world_rt_texture changed[WORLD_RT_TEXTURES];
	unsigned long count = 0;
	long index;

	for (index = 1; world_rt.slots && index < world_rt.slot_count; index++)
	{
		struct world_rt_slot *slot = &world_rt.slots[index];
		void const *header = slot->image ? NULL : world_rt_source_texture(&slot->map);

		if (slot->sent && header == slot->header)
			continue;
		slot->sent = 1;
		slot->header = header;
		changed[count].slot = (unsigned long)index;
		changed[count].header = header;
		changed[count].image = slot->image;
		count++;
	}
	if (count)
		halo_world_rt_textures(changed, count);
}

/* a mesh's material's index among the map's (made the first time: its
record into *made, *new set; -1 none is left) */
static long world_rt_material(struct world_rt_source_material const *source, struct world_rt_source_map const *lightmap,
	struct world_rt_material *made, int *new_one)
{
	struct world_rt_material_key key;
	long index;

	*new_one = 0;
	memset(&key, 0, sizeof(key));
	key.shader = source->shader;
	key.lightmap = *lightmap;
	key.alpha_scale[0] = source->alpha_scale[0];
	key.alpha_scale[1] = source->alpha_scale[1];
	if (!world_rt.materials)
	{
		world_rt.materials = malloc(sizeof(*world_rt.materials) * WORLD_RT_MATERIALS);
		if (!world_rt.materials)
			return -1;
	}
	for (index = 0; index < world_rt.material_count; index++)
	{
		if (!memcmp(&world_rt.materials[index], &key, sizeof(key)))
			return index;
	}
	if (world_rt.material_count >= WORLD_RT_MATERIALS)
		return -1;
	world_rt.materials[index] = key;
	world_rt.material_count++;
	memset(made, 0, sizeof(*made));
	made->index = (unsigned int)index;
	made->kind = (unsigned int)source->kind;
	made->flags = (unsigned int)source->flags;
	made->alpha_cutoff = source->alpha_cutoff;
	made->alpha_map = source->alpha_map >= 0 && source->alpha_map < WORLD_RT_MAPS ?
		world_rt_slot(&source->maps[source->alpha_map], 0) : 0;
	made->alpha_scale[0] = source->alpha_scale[0];
	made->alpha_scale[1] = source->alpha_scale[1];
	made->base_map = world_rt_slot(&source->maps[WORLD_RT_MAP_BASE], 0);
	made->detail_map = world_rt_slot(&source->maps[WORLD_RT_MAP_DETAIL], 0);
	made->detail_scale = source->detail_scale;
	made->multipurpose_map = world_rt_slot(&source->maps[WORLD_RT_MAP_MULTIPURPOSE], 0);
	made->bump_map = world_rt_slot(&source->maps[WORLD_RT_MAP_BUMP], 0);
	made->normal_map = world_rt_slot(NULL, source->normal_image);
	made->orm_map = world_rt_slot(NULL, source->orm_image);
	made->lightmap = world_rt_slot(lightmap, 0);
	made->self_illumination = source->self_illumination;
	*new_one = 1;
	return index;
}

/* a mesh as drawn laid out as the renderer takes it (world_rt.blob), its
materials the map's; its size (0: none, no room) */
static unsigned long world_rt_blob(struct world_rt_source_mesh const *mesh)
{
	unsigned long size, corners_at, vertices_at, materials_at, made_at;
	struct world_rt_mesh_header *header;
	unsigned short *triangle_materials;
	struct world_rt_material *made;
	long *indices = NULL, index;
	long count = 0;

	if (mesh->vertex_count <= 0 || mesh->triangle_count <= 0)
		return 0;
	corners_at = sizeof(*header) + (unsigned long)mesh->vertex_count * 12;
	vertices_at = corners_at + (unsigned long)mesh->triangle_count * 12;
	materials_at = vertices_at + (unsigned long)mesh->vertex_count * sizeof(struct world_rt_vertex);
	made_at = materials_at + (((unsigned long)mesh->triangle_count * 2 + 3) & ~3UL);
	size = made_at + (unsigned long)mesh->material_count * sizeof(struct world_rt_material);
	if (size > world_rt.blob_capacity)
	{
		unsigned char *grown = realloc(world_rt.blob, size);

		if (!grown)
			return 0;
		world_rt.blob = grown;
		world_rt.blob_capacity = size;
	}
	indices = malloc(sizeof(*indices) * (size_t)(mesh->material_count > 0 ? mesh->material_count : 1));
	if (!indices)
		return 0;
	memset(world_rt.blob, 0, size);
	header = (struct world_rt_mesh_header *)world_rt.blob;
	triangle_materials = (unsigned short *)(world_rt.blob + materials_at);
	made = (struct world_rt_material *)(world_rt.blob + made_at);
	/* its materials the map's, those new to it carried */
	for (index = 0; index < mesh->material_count; index++)
	{
		struct world_rt_source_map const *lightmap = &mesh->materials[index].maps[WORLD_RT_MAP_LIGHTMAP];
		int new_one;

		indices[index] = world_rt_material(&mesh->materials[index], lightmap, &made[count], &new_one);
		count += new_one;
	}
	for (index = 0; index < mesh->vertex_count; index++)
	{
		struct world_rt_source_vertex const *in = &mesh->vertices[index];
		struct world_rt_vertex *out = (struct world_rt_vertex *)(world_rt.blob + vertices_at) + index;
		float cross[3];

		memcpy(world_rt.blob + sizeof(*header) + index * 12, in->position, 12);
		out->uv[0] = in->uv[0];
		out->uv[1] = in->uv[1];
		out->normal = (unsigned int)world_rt_octahedral(in->normal);
		out->tangent = (unsigned int)world_rt_octahedral(in->tangent);
		out->lightmap_uv = ((unsigned int)(unsigned short)in->lightmap_uv[0]) |
			((unsigned int)(unsigned short)in->lightmap_uv[1] << 16);
		cross[0] = in->normal[1] * in->tangent[2] - in->normal[2] * in->tangent[1];
		cross[1] = in->normal[2] * in->tangent[0] - in->normal[0] * in->tangent[2];
		cross[2] = in->normal[0] * in->tangent[1] - in->normal[1] * in->tangent[0];
		out->flags = cross[0] * in->binormal[0] + cross[1] * in->binormal[1] + cross[2] * in->binormal[2] < 0.0f ? 1u : 0u;
	}
	for (index = 0; index < mesh->triangle_count; index++)
	{
		unsigned int corners[3];
		long material = mesh->triangle_materials[index] < mesh->material_count ?
			indices[mesh->triangle_materials[index]] : -1;
		int corner;

		for (corner = 0; corner < 3; corner++)
			corners[corner] = (unsigned int)mesh->triangles[index][corner];
		memcpy(world_rt.blob + corners_at + index * 12, corners, 12);
		/* (none left: the map's first, which the cutoff then tests) */
		triangle_materials[index] = (unsigned short)(material >= 0 ? material : 0);
	}
	free(indices);
	header->vertex_count = (unsigned int)mesh->vertex_count;
	header->triangle_count = (unsigned int)mesh->triangle_count;
	header->opaque_count = (unsigned int)mesh->opaque_count;
	header->material_count = (unsigned int)count;
	/* (the new materials alone: the size less the rest) */
	return made_at + (unsigned long)count * sizeof(struct world_rt_material);
}

/* a source mesh's arrays for its counts (FALSE: no room); and let go of */
static int world_rt_source_mesh_make(struct world_rt_source_mesh *mesh)
{
	mesh->vertices = malloc(sizeof(*mesh->vertices) * (size_t)mesh->vertex_count);
	mesh->triangles = malloc(sizeof(*mesh->triangles) * (size_t)mesh->triangle_count);
	mesh->triangle_materials = malloc(sizeof(*mesh->triangle_materials) * (size_t)mesh->triangle_count);
	mesh->materials = malloc(sizeof(*mesh->materials) * (size_t)(mesh->material_count > 0 ? mesh->material_count : 1));
	return mesh->vertices && mesh->triangles && mesh->triangle_materials && mesh->materials;
}

static void world_rt_source_mesh_free(struct world_rt_source_mesh *mesh)
{
	free(mesh->vertices);
	free(mesh->triangles);
	free(mesh->triangle_materials);
	free(mesh->materials);
	memset(mesh, 0, sizeof(*mesh));
}

/* (as drawn) the structure's render mesh in its collision
mesh's place, the frame after that was made: its opaque triangles, then its
alpha-tested ones; the transparent ones counted and left out */
static void world_rt_structure_render(void)
{
	short bsp_index = world_rt.bsp_index;
	struct world_rt_source_mesh mesh;
	long count, expected;
	unsigned long id, size;
	unsigned long long started = platform_shared_clock_microseconds();

	world_rt.render_due = 0;
	memset(&mesh, 0, sizeof(mesh));
	expected = world_rt_source_structure_mesh(bsp_index, &mesh);
	if (expected <= 0 || expected > WORLD_RT_MESH_TRIANGLES || !world_rt_source_mesh_make(&mesh))
	{
		world_rt_source_mesh_free(&mesh);
		return;
	}
	/* (past the budget: its collision mesh kept) */
	if (!world_rt_render_fits(expected, 0))
	{
		world_rt_source_mesh_free(&mesh);
		return;
	}
	count = world_rt_source_structure_mesh(bsp_index, &mesh);
	size = count == expected ? world_rt_blob(&mesh) : 0;
	id = size ? world_rt_id_take() : 0;
	if (!id)
	{
		world_rt_source_mesh_free(&mesh);
		return;
	}
	halo_world_rt_mesh(id, world_rt.blob, size);
	world_rt.count_triangles += count;
	world_rt.render_bytes += (unsigned long long)count * WORLD_RT_TRIANGLE_BYTES;
	/* (in its collision mesh's place, which is let go of) */
	world_rt.structure = world_rt.bsps[bsp_index].render = id;
	if (world_rt.bsps[bsp_index].collision)
		halo_world_ao_mesh_release(world_rt.bsps[bsp_index].collision);
	world_rt.bsps[bsp_index].collision = 0;
	platform_log("world ao: the ray-traced scene's structure, BSP %d, as drawn: %ld "
		"triangles (collision %ld), %ld of them alpha-tested, %ld transparent left out; %ld vertices, %ld materials "
		"(%ld of the map's), %ld textures; %.0f KB, read in %.2f ms", (int)bsp_index, count, world_rt.collision_count,
		count - mesh.opaque_count, mesh.left_out, mesh.vertex_count, mesh.material_count, world_rt.material_count,
		world_rt.slot_count > 0 ? world_rt.slot_count - 1 : 0, (double)size / 1024.0,
		(double)(platform_shared_clock_microseconds() - started) / 1000.0);
	world_rt_source_mesh_free(&mesh);
}

/* a point (or, rotation alone, a direction) by a row-major 3x4 */
static void world_rt_transformed(float const transform[3][4], float const in[3], int point, float out[3])
{
	int row;

	for (row = 0; row < 3; row++)
	{
		out[row] = transform[row][0] * in[0] + transform[row][1] * in[1] + transform[row][2] * in[2] +
			(point ? transform[row][3] : 0.0f);
	}
}

static void world_rt_unit(float vector[3])
{
	float length = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);

	if (length > 0.0f)
	{
		vector[0] /= length;
		vector[1] /= length;
		vector[2] /= length;
	}
}

int world_rt_triangle_node(struct world_rt_source_vertex const *a, struct world_rt_source_vertex const *b,
	struct world_rt_source_vertex const *c)
{
	struct world_rt_source_vertex const *corners[3];
	int major[3], corner;

	corners[0] = a;
	corners[1] = b;
	corners[2] = c;
	for (corner = 0; corner < 3; corner++)
		major[corner] = corners[corner]->weight >= 0.5f ? corners[corner]->nodes[0] : corners[corner]->nodes[1];
	/* (two corners' node, else the first's) */
	return major[1] == major[2] ? major[1] : major[0];
}

void world_rt_face_out(struct world_rt_source_vertex const *vertices, unsigned long triangle[3])
{
	float const *a = vertices[triangle[0]].position, *b = vertices[triangle[1]].position, *c = vertices[triangle[2]].position;
	float u[3], v[3], facing = 0.0f;
	int k;

	for (k = 0; k < 3; k++)
	{
		u[k] = b[k] - a[k];
		v[k] = c[k] - a[k];
	}
	for (k = 0; k < 3; k++)
	{
		float cross = u[(k + 1) % 3] * v[(k + 2) % 3] - u[(k + 2) % 3] * v[(k + 1) % 3];

		facing += cross * (vertices[triangle[0]].normal[k] + vertices[triangle[1]].normal[k] +
			vertices[triangle[2]].normal[k]);
	}
	if (facing < 0.0f)
	{
		unsigned long swap = triangle[1];

		triangle[1] = triangle[2];
		triangle[2] = swap;
	}
}

/* (as drawn) a skinned definition's mesh: its model's
whole, in its model's space, turned to face as its normals do, with each
vertex's nodes and weight after it (halo_world_rt_skinned_mesh); its nodes'
default inverses kept to pose its objects. Its triangles (0 none) */
static long world_rt_definition_skinned(struct world_rt_source_mesh *mesh, float (*inverses)[3][4],
	struct world_rt_definition *entry)
{
	unsigned long size, id, skin_size = (unsigned long)mesh->vertex_count * 8;
	unsigned char *grown;
	long index;

	for (index = 0; index < mesh->triangle_count; index++)
		world_rt_face_out(mesh->vertices, mesh->triangles[index]);
	size = world_rt_blob(mesh);
	if (!size)
		return 0;
	if (size + skin_size > world_rt.blob_capacity)
	{
		grown = realloc(world_rt.blob, size + skin_size);
		if (!grown)
			return 0;
		world_rt.blob = grown;
		world_rt.blob_capacity = size + skin_size;
	}
	for (index = 0; index < mesh->vertex_count; index++)
	{
		struct world_rt_source_vertex const *vertex = &mesh->vertices[index];
		unsigned int nodes = (unsigned int)(vertex->nodes[0] < entry->node_count ? vertex->nodes[0] : 0) |
			(unsigned int)(vertex->nodes[1] < entry->node_count ? vertex->nodes[1] : 0) << 8;

		memcpy(world_rt.blob + size + index * 8, &nodes, 4);
		memcpy(world_rt.blob + size + index * 8 + 4, &vertex->weight, 4);
	}
	entry->inverses = malloc(sizeof(*entry->inverses) * (size_t)entry->node_count);
	id = entry->inverses ? world_rt_id_take() : 0;
	if (!id)
		return 0;
	memcpy(entry->inverses, inverses, sizeof(*entry->inverses) * (size_t)entry->node_count);
	halo_world_rt_skinned_mesh(id, world_rt.blob, size + skin_size);
	world_rt.count_triangles += mesh->triangle_count;
	entry->meshes[0] = (unsigned short)id;
	entry->skinned = 1;
	entry->drawn = 1;
	entry->triangles = mesh->triangle_count;
	return mesh->triangle_count;
}

/* ---------- object lamps (display.ray_traced_lighting, a level lit live:
lt/e1.frag's object_lamp_sample) */

/* a bitmap's texels as decoded (kept; NULL none) */
static unsigned long const *world_rt_lamp_bitmap(struct world_rt_source_map const *map, unsigned long *width,
	unsigned long *height)
{
	int index;

	if (map->bitmap == -1 || map->index < 0)
		return NULL;
	for (index = 0; index < world_rt.lamp_bitmap_count; index++)
	{
		if (world_rt.lamp_bitmaps[index].tag == map->bitmap && world_rt.lamp_bitmaps[index].index == map->index)
		{
			*width = world_rt.lamp_bitmaps[index].width;
			*height = world_rt.lamp_bitmaps[index].height;
			return world_rt.lamp_bitmaps[index].texels;
		}
	}
	if (world_rt.lamp_bitmap_count >= WORLD_RT_LAMP_BITMAPS)
		return NULL;
	index = world_rt.lamp_bitmap_count++;
	world_rt.lamp_bitmaps[index].tag = map->bitmap;
	world_rt.lamp_bitmaps[index].index = (short)map->index;
	world_rt.lamp_bitmaps[index].texels = world_rt_source_texels(map, WORLD_RT_LAMP_TEXELS,
		&world_rt.lamp_bitmaps[index].width, &world_rt.lamp_bitmaps[index].height);
	*width = world_rt.lamp_bitmaps[index].width;
	*height = world_rt.lamp_bitmaps[index].height;
	return world_rt.lamp_bitmaps[index].texels;
}

/* a texel's channel (shift 16 red, 8 green, 0 blue) at st, wrapped, as a
fraction */
static float world_rt_lamp_texel(unsigned long const *texels, unsigned long width, unsigned long height, float s, float t,
	int shift)
{
	long x = (long)floorf((s - floorf(s)) * (float)width), y = (long)floorf((t - floorf(t)) * (float)height);

	x = x < 0 ? 0 : x >= (long)width ? (long)width - 1 : x;
	y = y < 0 ? 0 : y >= (long)height ? (long)height - 1 : y;
	return (float)((texels[y * (long)width + x] >> shift) & 0xFF) / 255.0f;
}

/* a node's part's lamps onto the definition's: each triangle of a
material that glows (its self-illumination colour) sampled at points
spread over it (its base map and its multipurpose map's green, as the
model pass reads them at the coordinates by the map scale) */
static void world_rt_lamps_made(struct world_rt_definition *entry, short node, struct world_rt_source_mesh const *part)
{
	long triangle;

	for (triangle = 0; triangle < part->triangle_count; triangle++)
	{
		struct world_rt_source_material const *material = &part->materials[part->triangle_materials[triangle]];
		struct world_rt_source_vertex const *corner[3];
		unsigned long const *base, *mask;
		unsigned long base_width = 0, base_height = 0, mask_width = 0, mask_height = 0;
		float colour[3], sum[3] = { 0.0f, 0.0f, 0.0f }, luminance;
		int i, j, channel, count = 0;
		struct world_rt_lamp *lamp;

		if (!material->self_illumination)
			continue;
		mask = world_rt_lamp_bitmap(&material->maps[WORLD_RT_MAP_MULTIPURPOSE], &mask_width, &mask_height);
		if (!mask || !mask_width || !mask_height)
			continue;
		base = world_rt_lamp_bitmap(&material->maps[WORLD_RT_MAP_BASE], &base_width, &base_height);
		for (channel = 0; channel < 3; channel++)
			colour[channel] = (float)((material->self_illumination >> (8 * channel)) & 0xFF) / 255.0f;
		for (i = 0; i < 3; i++)
			corner[i] = &part->vertices[part->triangles[triangle][i]];
		for (i = 0; i < WORLD_RT_LAMP_SAMPLES; i++)
		{
			for (j = 0; j < WORLD_RT_LAMP_SAMPLES - i; j++)
			{
				float a = ((float)i + 0.33f) / (float)WORLD_RT_LAMP_SAMPLES;
				float b = ((float)j + 0.33f) / (float)WORLD_RT_LAMP_SAMPLES;
				float c = 1.0f - a - b, st[2], m;

				if (c < 0.0f)
					continue;
				st[0] = (c * corner[0]->uv[0] + a * corner[1]->uv[0] + b * corner[2]->uv[0]) * material->alpha_scale[0];
				st[1] = (c * corner[0]->uv[1] + a * corner[1]->uv[1] + b * corner[2]->uv[1]) * material->alpha_scale[1];
				m = world_rt_lamp_texel(mask, mask_width, mask_height, st[0], st[1], 8);
				for (channel = 0; channel < 3; channel++)
				{
					float albedo = base && base_width && base_height ?
						powf(world_rt_lamp_texel(base, base_width, base_height, st[0], st[1], 16 - 8 * channel), 2.2f) :
						1.0f;
					float glow = m * colour[channel];

					sum[channel] += albedo * powf(glow > 1.0f ? 1.0f : glow, 2.2f);
				}
				count++;
			}
		}
		if (!count)
			continue;
		for (channel = 0; channel < 3; channel++)
			sum[channel] *= WORLD_RT_LAMP_K / (float)count;
		luminance = 0.2126f * sum[0] + 0.7152f * sum[1] + 0.0722f * sum[2];
		if (luminance < WORLD_RT_LAMP_LEAST)
			continue;
		lamp = realloc(entry->lamps, sizeof(*entry->lamps) * (size_t)(entry->lamp_count + 1));
		if (!lamp)
			return;
		entry->lamps = lamp;
		lamp = &entry->lamps[entry->lamp_count++];
		lamp->node = node;
		for (i = 0; i < 3; i++)
			memcpy(&lamp->corners[3 * i], corner[i]->position, sizeof(float) * 3);
		memcpy(lamp->radiance, sum, sizeof(sum));
	}
}

/* a node's lamps (its instance's transform this frame) onto the frame's */
static void world_rt_lamps_placed(struct world_rt_definition const *definition, short node,
	float const transform[3][4], unsigned long owner)
{
	long index;

	if (!world_rt.lamp_corners)
	{
		world_rt.lamp_corners = malloc(sizeof(*world_rt.lamp_corners) * WORLD_RT_LAMPS);
		world_rt.lamp_radiance = malloc(sizeof(*world_rt.lamp_radiance) * WORLD_RT_LAMPS);
		world_rt.lamp_owners = malloc(sizeof(*world_rt.lamp_owners) * WORLD_RT_LAMPS);
		if (!world_rt.lamp_corners || !world_rt.lamp_radiance || !world_rt.lamp_owners)
			return;
	}
	for (index = 0; index < definition->lamp_count && world_rt.lamp_count < WORLD_RT_LAMPS; index++)
	{
		struct world_rt_lamp const *lamp = &definition->lamps[index];
		int corner;

		if (lamp->node != node)
			continue;
		for (corner = 0; corner < 3; corner++)
			world_rt_transformed(transform, &lamp->corners[3 * corner], 1,
				&world_rt.lamp_corners[world_rt.lamp_count][3 * corner]);
		memcpy(world_rt.lamp_radiance[world_rt.lamp_count], lamp->radiance, sizeof(lamp->radiance));
		world_rt.lamp_owners[world_rt.lamp_count] = owner;
		world_rt.lamp_count++;
	}
}

long world_rt_lamps(float const (**corners)[9], float const (**radiance)[3], unsigned long const **owners)
{
	*corners = (float const (*)[9])world_rt.lamp_corners;
	*radiance = (float const (*)[3])world_rt.lamp_radiance;
	*owners = world_rt.lamp_owners;
	return world_rt.lamp_corners ? world_rt.lamp_count : 0;
}

/* (as drawn) a definition's model's render mesh as drawn,
split by node: each triangle on its corners' major node (two corners', else
the first's; a joint's blended corners held so), its vertices in that
node's space (its default inverse), turned to face as its normals do, a
mesh for each node that has some (entry's meshes). Its triangles made (0:
none, its collision meshes then) */
static long world_rt_definition_render(long definition, struct world_rt_definition *entry)
{
	struct world_rt_source_mesh mesh, part;
	float (*inverses)[3][4] = NULL;
	signed char *nodes = NULL;
	long *stamps = NULL, *remap = NULL, expected, count, triangle, made = 0;
	short node_index;

	memset(&mesh, 0, sizeof(mesh));
	memset(&part, 0, sizeof(part));
	expected = world_rt_source_model_mesh(definition, &mesh, NULL);
	if (expected <= 0 || expected > WORLD_RT_MESH_TRIANGLES || !world_rt_source_mesh_make(&mesh))
		goto done;
	inverses = malloc(sizeof(*inverses) * WORLD_RT_NODES);
	count = inverses ? world_rt_source_model_mesh(definition, &mesh, inverses) : 0;
	/* (past the budget: its collision meshes; a skinned one's own is
	counted as each object of it is posed) */
	if (count != expected || (!world_rt_blended(mesh.vertices, mesh.vertex_count) && !world_rt_render_fits(count, 0)))
		goto done;
	part.vertex_count = mesh.vertex_count;
	part.triangle_count = mesh.triangle_count;
	part.material_count = mesh.material_count;
	nodes = malloc((size_t)mesh.triangle_count);
	stamps = malloc(sizeof(*stamps) * (size_t)mesh.vertex_count);
	remap = malloc(sizeof(*remap) * (size_t)mesh.vertex_count);
	if (!nodes || !stamps || !remap || !world_rt_source_mesh_make(&part))
		goto done;
	free(part.materials);
	part.materials = mesh.materials;
	/* (a model whose joints blend two nodes: one skinned mesh, posed and
	refit for each object) */
	if (world_rt_blended(mesh.vertices, mesh.vertex_count))
	{
		made = world_rt_definition_skinned(&mesh, inverses, entry);
		goto done;
	}
	for (triangle = 0; triangle < mesh.triangle_count; triangle++)
	{
		int node = world_rt_triangle_node(&mesh.vertices[mesh.triangles[triangle][0]],
			&mesh.vertices[mesh.triangles[triangle][1]], &mesh.vertices[mesh.triangles[triangle][2]]);

		nodes[triangle] = (signed char)(node >= 0 && node < entry->node_count ? node : 0);
	}
	memset(stamps, 0xFF, sizeof(*stamps) * (size_t)mesh.vertex_count);
	for (node_index = 0; node_index < entry->node_count; node_index++)
	{
		unsigned long id, size;

		part.vertex_count = part.triangle_count = part.opaque_count = 0;
		part.material_count = mesh.material_count;
		for (triangle = 0; triangle < mesh.triangle_count; triangle++)
		{
			int corner;

			if (nodes[triangle] != node_index)
				continue;
			for (corner = 0; corner < 3; corner++)
			{
				unsigned long vertex = mesh.triangles[triangle][corner];

				if (stamps[vertex] != node_index)
				{
					struct world_rt_source_vertex *out = &part.vertices[part.vertex_count];

					*out = mesh.vertices[vertex];
					world_rt_transformed(inverses[node_index], mesh.vertices[vertex].position, 1, out->position);
					world_rt_transformed(inverses[node_index], mesh.vertices[vertex].normal, 0, out->normal);
					world_rt_transformed(inverses[node_index], mesh.vertices[vertex].tangent, 0, out->tangent);
					world_rt_transformed(inverses[node_index], mesh.vertices[vertex].binormal, 0, out->binormal);
					world_rt_unit(out->normal);
					world_rt_unit(out->tangent);
					world_rt_unit(out->binormal);
					stamps[vertex] = node_index;
					remap[vertex] = part.vertex_count++;
				}
				part.triangles[part.triangle_count][corner] = (unsigned long)remap[vertex];
			}
			world_rt_face_out(part.vertices, part.triangles[part.triangle_count]);
			part.triangle_materials[part.triangle_count] = mesh.triangle_materials[triangle];
			part.opaque_count += triangle < mesh.opaque_count;
			part.triangle_count++;
		}
		if (!part.triangle_count)
			continue;
		world_rt_lamps_made(entry, node_index, &part);
		size = world_rt_blob(&part);
		id = size ? world_rt_id_take() : 0;
		if (!id)
			continue;
		halo_world_rt_mesh(id, world_rt.blob, size);
		world_rt.count_triangles += part.triangle_count;
		entry->meshes[node_index] = (unsigned short)id;
		made += part.triangle_count;
	}
	entry->drawn = made > 0;
	entry->triangles = made;
	world_rt.render_bytes += (unsigned long long)made * WORLD_RT_TRIANGLE_BYTES;
done:
	if (part.materials == mesh.materials)
		part.materials = NULL;
	world_rt_source_mesh_free(&part);
	world_rt_source_mesh_free(&mesh);
	free(inverses);
	free(nodes);
	free(stamps);
	free(remap);
	return made;
}

/* a definition's nodes' collision meshes into meshes (0 none), and with
permuted each node's other permutations' (a region's other pieces: the
object has one of them at a time, as its collision tests take it,
world_proxies_source_permutations; *any_permuted whether any node has
them); their triangles (the first permutation's) */
static long world_rt_collision_meshes(long definition, short node_count, unsigned short *meshes,
	unsigned short (*permuted)[WORLD_RT_PERMUTATIONS - 1], short *any_permuted)
{
	short node_index;
	long triangles = 0;

	for (node_index = 0; node_index < node_count; node_index++)
	{
		short permutations = permuted ? world_rt_source_node_permutations(definition, node_index) : 1, permutation;

		for (permutation = 0; permutation < permutations && permutation < WORLD_RT_PERMUTATIONS; permutation++)
		{
			long count = world_rt_source_node(definition, node_index, permutation, NULL, 0);
			unsigned short mesh;

			if (count <= 0 || count > WORLD_RT_MESH_TRIANGLES || !world_rt_room(count))
				continue;
			count = world_rt_source_node(definition, node_index, permutation, world_rt.triangles, count);
			world_rt_orient(world_rt.triangles, count, WORLD_RT_ORIENT_LEAST);
			mesh = (unsigned short)world_rt_mesh_send(count);
			if (permutation == 0)
			{
				meshes[node_index] = mesh;
				triangles += mesh ? count : 0;
			}
			else
			{
				permuted[node_index][permutation - 1] = mesh;
				*any_permuted = 1;
			}
		}
	}
	return triangles;
}

/* a definition's meshes, made as it is first drawn (NULL: none can be
kept) */
static struct world_rt_definition *world_rt_definition(long definition)
{
	unsigned long slot = ((unsigned long)definition * 2654435761UL) & (WORLD_RT_DEFINITIONS - 1);
	struct world_rt_definition *entry;
	long probes;
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
	/* (as drawn: its model's render mesh, a mesh for each node; its
	collision meshes where it has none, or past the budget) */
	if (entry->node_count > 0 && world_rt.render_geometry)
		triangles = world_rt_definition_render(definition, entry);
	any = triangles > 0;
	if (!any)
		triangles = world_rt_collision_meshes(definition, entry->node_count, entry->meshes, entry->permuted,
			&entry->any_permuted);
	any = triangles > 0;
	/* (none: an item's model's render mesh as drawn, a grenade's say: the
	box about its bounding sphere, far larger than a grenade, cast a square
	shadow about each one lying on the ground) */
	if (!any && item && entry->node_count > 0 && !world_rt.render_geometry)
	{
		triangles = world_rt_definition_render(definition, entry);
		any = triangles > 0;
	}
	/* (still none: its box about its bounding sphere, on its first node) */
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
		platform_log("world ao: the ray-traced scene's %s: %d nodes, %ld triangles%s%s", world_rt_source_name(definition),
			(int)entry->node_count, triangles, entry->boxed ? ", its box" : "", entry->drawn ? ", as drawn" : "");
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

/* (measuring) the first-person body's pose: logged every 600 frames, how
often its nodes moved (they are the game's own, posed whether drawn or not) */
static void world_rt_player_posed(struct world_rt_pose const *pose)
{
	unsigned int nodes = pose->node_count < 4 ? pose->node_count : 4;

	if (world_rt.counts_wanted <= 0)
		return;
	world_rt.player_posed++;
	if (memcmp(world_rt.player_last, pose->transforms, nodes * sizeof(pose->transforms[0])))
		world_rt.player_moved++;
	memcpy(world_rt.player_last, pose->transforms, nodes * sizeof(pose->transforms[0]));
	if (world_rt.player_posed >= 600)
	{
		platform_log("world ao: the ray-traced scene's first-person body: posed %ld frames, its nodes moved in %ld",
			world_rt.player_posed, world_rt.player_moved);
		world_rt.player_posed = world_rt.player_moved = 0;
	}
}

/* a skinned object's pose: each node's skinning transform (its frame as
drawn after its default inverse: the model's space into the world, as the
game's skinned vertex programs take its node matrices) into its first
node's space (first, the instance's transform) */
static void world_rt_pose(struct world_rt_definition const *definition, unsigned long owner, unsigned long key,
	float const first[3][4], struct world_rt_pose *pose)
{
	float inverse[3][4], node[3][4];
	short node_index;

	memset(pose, 0, 16);
	pose->mesh = definition->meshes[0];
	pose->owner = (unsigned int)owner;
	pose->node_count = (unsigned int)definition->node_count;
	pose->key = (unsigned int)key;
	if (!world_rt_inverse(first, inverse))
		memset(inverse, 0, sizeof(inverse));
	for (node_index = 0; node_index < definition->node_count; node_index++)
	{
		world_rt_transform(&world_rt.frames[node_index], node);
		world_rt_compose(node, definition->inverses[node_index], node);
		world_rt_compose(inverse, node, pose->transforms[node_index]);
	}
}

void world_rt_frame(unsigned long frame)
{
	long object_count, object_index, count = 0, pose_count = 0;

	if (world_rt.framed && world_rt.frame == frame)
		return;
	world_rt.framed = 1;
	world_rt.frame = frame;
	if (!world_rt.counts_wanted)
		world_rt.counts_wanted = getenv("HALO_WORLD_AO_COUNTS") ? 1 : -1;
	world_rt_render_mode();
	world_rt.skinned_bytes = 0;
	if (!world_rt.instances)
		world_rt.instances = malloc(sizeof(*world_rt.instances) * WORLD_RT_INSTANCES);
	if (!world_rt.poses)
		world_rt.poses = malloc(sizeof(*world_rt.poses) * WORLD_RT_POSES);
	if (!world_rt.objects)
		world_rt.objects = malloc(sizeof(*world_rt.objects) * WORLD_RT_OBJECTS);
	if (!world_rt.instances || !world_rt.objects || !world_rt.poses)
		return;
	world_rt.lamp_count = 0;
	world_rt_structure();
	world_rt_textures_frame();
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
		/* (and with the ray-traced lighting, a static placement whatever the
		field, which display.ambient_occlusion "off" has none of: the
		lightmaps hold its sun shadow as the structure's, so the pass's sun
		rays take it for the structure, never for a dynamic object whose
		shadow the lightmap lacks (lt/e1.frag's sun_share: the sun's share
		taken off a lightmap already dark there, below 0 in a channel) */
		int structure = object->baked || (lt_lights_pass_on() && world_proxies_source_lightmapped(object->id));
		unsigned long owner = structure ? 0 : world_rt_owner(object->id);
		/* (the body a local player sees from in first person: its own mask,
		as drawn) */
		unsigned int kind = structure ? WORLD_RT_STRUCTURE : object->first_person && world_rt.render_geometry ?
			WORLD_RT_PLAYER : WORLD_RT_OBJECT;
		/* (with the ray-traced lighting, one that casts no shadow, b30's
		ring overhead: its own mask, which the pass's sun leaves out, as the
		game draws it no shadow and the lightmap has none of it) */
		if (kind == WORLD_RT_OBJECT && object->shadowless && lt_lights_pass_on())
			kind = WORLD_RT_SHADOWLESS;
		short node_count, node_index, permutation_count;

		definition = world_rt_definition(object->definition);
		if (!definition || definition->node_count <= 0)
			continue;
		node_count = world_proxies_source_nodes(object->id, world_rt.frames, WORLD_RT_NODES);
		if (node_count <= 0)
			continue;
		/* (its sun shadow the light-transport pass's: lt_lights.c's
		lt_lights_sun_shadow; not a box's, which has no shape of it) */
		if (!definition->boxed && kind != WORLD_RT_STRUCTURE)
		{
			unsigned long slot = (unsigned long)object->id & 0xffffu;
			int meshes = definition->skinned;

			for (node_index = 0; node_index < definition->node_count && !meshes; node_index++)
				meshes = definition->meshes[node_index] || definition->collision[node_index];
			if (meshes && slot < WORLD_RT_TRACED)
			{
				world_rt.traced_id[slot] = (unsigned long)object->id;
				world_rt.traced_frame[slot] = frame;
			}
		}
		if (definition->boxed)
		{
			struct world_ao_instance *instance = world_rt_instance_add(&count, world_rt.box, owner, kind);

			if (instance)
				world_rt_box_transform(&world_rt.frames[0], definition->sphere, instance->transform);
			continue;
		}
		/* (skinned: one instance on its first node, posed; past the budget,
		its collision meshes on its nodes) */
		if (definition->skinned && !world_rt_render_fits(definition->triangles, 1))
		{
			struct world_rt_definition *fallback = (struct world_rt_definition *)definition;

			if (!fallback->collision_made)
				world_rt_collision_meshes(fallback->definition, fallback->node_count, fallback->collision, NULL, NULL);
			fallback->collision_made = 1;
			for (node_index = 0; node_index < node_count && node_index < definition->node_count; node_index++)
			{
				struct world_ao_instance *instance;

				if (!definition->collision[node_index])
					continue;
				instance = world_rt_instance_add(&count, definition->collision[node_index], owner, kind);
				if (instance)
					world_rt_transform(&world_rt.frames[node_index], instance->transform);
			}
			continue;
		}
		if (definition->skinned)
		{
			struct world_ao_instance *instance;

			if (pose_count >= WORLD_RT_POSES || node_count < definition->node_count)
				continue;
			world_rt.skinned_bytes += (unsigned long long)definition->triangles * WORLD_RT_TRIANGLE_BYTES;
			instance = world_rt_instance_add(&count, definition->meshes[0], owner, kind);
			if (!instance)
				continue;
			world_rt_transform(&world_rt.frames[0], instance->transform);
			world_rt_pose(definition, owner, (unsigned long)object->id, instance->transform, &world_rt.poses[pose_count]);
			instance->pad = (unsigned int)++pose_count;
			/* (measuring: whether the first-person body's nodes move) */
			if (kind == WORLD_RT_PLAYER)
				world_rt_player_posed(&world_rt.poses[pose_count - 1]);
			continue;
		}
		/* (each node's collision mesh of the permutation the object has now:
		a dead Jackal's shield is off, its frame alone left, #1311) */
		permutation_count = definition->any_permuted ?
			world_proxies_source_permutations(object->id, world_rt.permutations, WORLD_RT_NODES) : 0;
		for (node_index = 0; node_index < node_count && node_index < definition->node_count; node_index++)
		{
			struct world_ao_instance *instance;
			int permutation = node_index < permutation_count ? world_rt.permutations[node_index] : 0;
			unsigned short mesh = permutation == 0 ? definition->meshes[node_index] :
				permutation > 0 && permutation < WORLD_RT_PERMUTATIONS ?
					definition->permuted[node_index][permutation - 1] : 0;

			if (!mesh)
				continue;
			instance = world_rt_instance_add(&count, mesh, owner, kind);
			if (instance)
				world_rt_transform(&world_rt.frames[node_index], instance->transform);
			/* (its lamps, moved with it) */
			if (instance && permutation == 0 && definition->lamp_count > 0)
				world_rt_lamps_placed(definition, node_index, instance->transform, owner);
		}
	}
	/* (as drawn) the first-person weapon and hands as last drawn, their own
	mask's, on their nodes */
	if (world_rt.render_geometry)
	{
		int which;

		for (which = 0; which < 2 && count < WORLD_RT_INSTANCES; which++)
		{
			long model, unit;
			short node_count = world_rt_source_first_person(which, &model, &unit, world_rt.frames, WORLD_RT_NODES);
			struct world_rt_definition const *definition = node_count > 0 ? world_rt_definition(model) : NULL;
			unsigned long owner = WORLD_RT_FIRST_PERSON_OWNER + (unsigned long)which;
			short node_index;

			if (!definition || !definition->drawn || node_count < definition->node_count)
				continue;
			if (definition->skinned)
			{
				struct world_ao_instance *instance;

				if (pose_count >= WORLD_RT_POSES)
					continue;
				instance = world_rt_instance_add(&count, definition->meshes[0], owner, WORLD_RT_FIRST_PERSON);
				if (!instance)
					continue;
				world_rt_transform(&world_rt.frames[0], instance->transform);
				world_rt_pose(definition, owner, owner, instance->transform, &world_rt.poses[pose_count]);
				instance->pad = (unsigned int)++pose_count;
				continue;
			}
			for (node_index = 0; node_index < definition->node_count; node_index++)
			{
				struct world_ao_instance *instance;

				if (!definition->meshes[node_index])
					continue;
				instance = world_rt_instance_add(&count, definition->meshes[node_index], owner, WORLD_RT_FIRST_PERSON);
				if (instance)
					world_rt_transform(&world_rt.frames[node_index], instance->transform);
			}
		}
		world_rt_source_first_person_next();
	}
	if (pose_count)
		halo_world_rt_poses(world_rt.poses, (unsigned long)pose_count);
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

int world_rt_object_traced(long object_index)
{
	unsigned long slot = (unsigned long)object_index & 0xffffu;

	return world_rt.framed && object_index != -1 && slot < WORLD_RT_TRACED &&
		world_rt.traced_id[slot] == (unsigned long)object_index && world_rt.traced_frame[slot] == world_rt.frame;
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
	free(world_rt.materials);
	world_rt.materials = NULL;
	world_rt.material_count = 0;
	free(world_rt.slots);
	world_rt.slots = NULL;
	world_rt.slot_count = 0;
	free(world_rt.blob);
	world_rt.blob = NULL;
	world_rt.blob_capacity = 0;
	world_rt.framed = 0;
	world_rt.full_logged = 0;
	world_rt.count_triangles = 0;
	world_rt.render_bytes = world_rt.skinned_bytes = 0;
	world_rt.past_logged = 0;
	if (world_rt.definitions)
	{
		long index;

		for (index = 0; index < WORLD_RT_DEFINITIONS; index++)
		{
			if (world_rt.definitions[index].definition != -1)
			{
				free(world_rt.definitions[index].inverses);
				free(world_rt.definitions[index].lamps);
			}
		}
	}
	free(world_rt.definitions);
	world_rt.definitions = NULL;
	{
		int index;

		for (index = 0; index < world_rt.lamp_bitmap_count; index++)
			free(world_rt.lamp_bitmaps[index].texels);
		world_rt.lamp_bitmap_count = 0;
		world_rt.lamp_count = 0;
	}
	free(world_rt.triangles);
	world_rt.triangles = NULL;
	world_rt.triangle_capacity = 0;
}
