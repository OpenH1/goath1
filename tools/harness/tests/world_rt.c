/* display.ambient_occlusion "ray_traced"'s scene (port/linux/game/world_rt.c;
test_world_rt.py): a mesh's triangles turned to face out, the box, the
instances' transforms; and a frame of a fake world's objects: the meshes
made once a map as they are first needed (the structure's, a definition's
nodes', the box for one with none), each frame's instances of them on the
objects' nodes, the static scenery left out, all let go of at the map's
end, each BSP's kept by its index (a switch back makes nothing again); and
with debug.rt_render_geometry, the structure's render mesh in its collision
mesh's place from the frame after (its opaque triangles alone), the
collision mesh let go of then. */

#include "harness.h"
#include "world_ao.h"
#include "world_proxies_source.h"
#include "world_rt.h"

#include <math.h>

void platform_log(const char *format, ...)
{
	(void)format;
}

/* debug.rt_render_geometry */
static int render_on;

const char *config_string(const char *name)
{
	return !strcmp(name, "debug.rt_render_geometry") && render_on ? "auto" : "off";
}

/* (the backend's budget: room for all of the fake world's) */
/* (display.ray_traced_lighting, lt_lights.c's: on, so debug.rt_render_geometry
"auto" traces the scene as drawn, as these tests have it) */
int lt_lights_pass_on(void)
{
	return 1;
}

unsigned long halo_world_rt_budget(void)
{
	return 64;
}

unsigned long long platform_shared_clock_microseconds(void)
{
	return 0;
}

/* ---------- the fake world

The loaded BSP (2 triangles); definition 7: two nodes, the first with a
tetrahedron wound inward, the second with none; definition 9: one node and
none (its box); objects 0x10003 (of 7, its nodes at x 10 and 20), 0x20005
(of 9, an item, at y 5, scaled 2), 0x30006 (of 7, static scenery baked)
and 0x40008 (of 11, neither an item nor any collision geometry: none).
With permuted_world, object 0x50010 alone (of 13: one node of two
collision permutations, a tetrahedron, a Jackal's shield up, and one
triangle, its frame with the shield off), its node's permutation
fake_permutation. */

static int permuted_world;
static signed char fake_permutation;

static short bsp_index = 0;
static int structure_asked, definitions_asked;

short world_rt_source_bsp(void)
{
	return bsp_index;
}

long world_rt_source_structure(short index, float *triangles, long maximum)
{
	static const float floor[2][9] = { { 0, 0, 0, 1, 0, 0, 1, 1, 0 }, { 0, 0, 0, 1, 1, 0, 0, 1, 0 } };

	CHECK(index == bsp_index, "another BSP's: %d", index);
	structure_asked++;
	if (triangles)
		memcpy(triangles, floor, sizeof(float) * 9 * (maximum < 2 ? maximum : 2));
	return 2;
}

/* (the render mesh: two opaque triangles of material 0, an alpha-tested
one of material 1, one transparent left out; and the times it was read,
and filled) */
static int render_asked, render_filled;

long world_rt_source_structure_mesh(short index, struct world_rt_source_mesh *mesh)
{
	static const float positions[7][3] = { { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 1, 0 }, { 5, 0, 0 }, { 5, 0, 1 },
		{ 5, 1, 1 } };
	static const unsigned long corners[3][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 4, 5, 6 } };
	int vertex, triangle, material, map;

	CHECK(index == bsp_index, "another BSP's render mesh: %d", index);
	render_asked++;
	if (mesh->vertices)
	{
		CHECK(mesh->vertex_count >= 7 && mesh->triangle_count >= 3 && mesh->material_count >= 2, "room: %ld %ld %ld",
			mesh->vertex_count, mesh->triangle_count, mesh->material_count);
		render_filled++;
		for (vertex = 0; vertex < 7; vertex++)
		{
			memset(&mesh->vertices[vertex], 0, sizeof(mesh->vertices[vertex]));
			memcpy(mesh->vertices[vertex].position, positions[vertex], sizeof(positions[vertex]));
			mesh->vertices[vertex].normal[0] = mesh->vertices[vertex].tangent[1] = 1.0f;
			mesh->vertices[vertex].binormal[2] = vertex == 6 ? -1.0f : 1.0f;
			mesh->vertices[vertex].uv[0] = (float)vertex;
		}
		for (triangle = 0; triangle < 3; triangle++)
		{
			memcpy(mesh->triangles[triangle], corners[triangle], sizeof(corners[triangle]));
			mesh->triangle_materials[triangle] = triangle == 2;
		}
		for (material = 0; material < 2; material++)
		{
			struct world_rt_source_material *out = &mesh->materials[material];

			memset(out, 0, sizeof(*out));
			for (map = 0; map < WORLD_RT_MAPS; map++)
				out->maps[map].bitmap = -1;
			out->shader = 100 + material;
			out->kind = 3;
			out->alpha_cutoff = 0.5f;
			out->alpha_map = material ? WORLD_RT_MAP_BUMP : -1;
			out->flags = material ? WORLD_RT_MATERIAL_ALPHA_TESTED : 0;
			out->alpha_scale[0] = out->alpha_scale[1] = material ? 2.0f : 1.0f;
			out->maps[WORLD_RT_MAP_BASE].bitmap = 200;
			out->maps[WORLD_RT_MAP_BUMP].bitmap = material ? 201 : -1;
			out->maps[WORLD_RT_MAP_LIGHTMAP].bitmap = 300;
		}
	}
	mesh->vertex_count = 7;
	mesh->triangle_count = 3;
	mesh->opaque_count = 2;
	mesh->material_count = 2;
	mesh->left_out = 1;
	return 3;
}

/* (definition 7's model, with model_on: two triangles on node 0 (the
second wound against its normals), one on node 1, whose default inverse
moves it 10 back along x; every other definition none) */
static int model_on, model_asked;

long world_rt_source_model_mesh(long definition, struct world_rt_source_mesh *mesh, float (*inverses)[3][4])
{
	static const float positions[9][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 0, 1 }, { 0, 1, 1 },
		{ 10, 0, 0 }, { 11, 0, 0 }, { 10, 1, 0 } };
	static const unsigned long corners[3][3] = { { 0, 1, 2 }, { 3, 5, 4 }, { 6, 7, 8 } };
	int vertex, triangle, node;

	if (!model_on || definition != 7)
		return 0;
	model_asked++;
	if (mesh->vertices)
	{
		for (vertex = 0; vertex < 9; vertex++)
		{
			memset(&mesh->vertices[vertex], 0, sizeof(mesh->vertices[vertex]));
			memcpy(mesh->vertices[vertex].position, positions[vertex], sizeof(positions[vertex]));
			mesh->vertices[vertex].normal[2] = mesh->vertices[vertex].tangent[0] = mesh->vertices[vertex].binormal[1] = 1.0f;
			/* (the last triangle's corners on node 1, one by its second node alone) */
			mesh->vertices[vertex].nodes[0] = mesh->vertices[vertex].nodes[1] = vertex >= 6;
			mesh->vertices[vertex].weight = 1.0f;
		}
		mesh->vertices[8].nodes[0] = 0;
		/* (model_on 2: blended halfway, a skinned joint) */
		mesh->vertices[8].weight = model_on == 2 ? 0.5f : 0.0f;
		for (triangle = 0; triangle < 3; triangle++)
		{
			memcpy(mesh->triangles[triangle], corners[triangle], sizeof(corners[triangle]));
			mesh->triangle_materials[triangle] = 0;
		}
		memset(&mesh->materials[0], 0, sizeof(mesh->materials[0]));
		for (vertex = 0; vertex < WORLD_RT_MAPS; vertex++)
			mesh->materials[0].maps[vertex].bitmap = -1;
		mesh->materials[0].shader = 400;
		mesh->materials[0].alpha_map = -1;
	}
	if (inverses)
	{
		for (node = 0; node < 2; node++)
		{
			memset(inverses[node], 0, sizeof(inverses[node]));
			inverses[node][0][0] = inverses[node][1][1] = inverses[node][2][2] = 1.0f;
			inverses[node][0][3] = node ? (model_on == 2 ? -5.0f : -10.0f) : 0.0f;
		}
	}
	mesh->vertex_count = 9;
	mesh->triangle_count = 3;
	mesh->opaque_count = 3;
	mesh->material_count = 1;
	return 3;
}

/* (no model has additive parts: no object lamps here) */
long world_rt_source_model_lamps(long definition, struct world_rt_source_mesh *mesh)
{
	(void)definition;
	mesh->vertex_count = mesh->triangle_count = mesh->opaque_count = mesh->material_count = mesh->left_out = 0;
	return 0;
}

/* (no texture's texels are read here) */
unsigned long *world_rt_source_texels(struct world_rt_source_map const *map, unsigned long largest,
	unsigned long *width, unsigned long *height)
{
	(void)map;
	(void)largest;
	*width = *height = 0;
	return NULL;
}

/* (no objects' shading, and no generic shader's stages, here) */
int world_rt_source_shading(long id, struct world_rt_source_shading *shading)
{
	(void)id;
	memset(shading, 0, sizeof(*shading));
	return 0;
}

int world_rt_source_glow_maps(long shader, struct world_rt_source_map maps[4], float transforms[4][4])
{
	(void)shader;
	(void)maps;
	(void)transforms;
	return 0;
}

void world_rt_source_glow(long shader, unsigned long const texels[4], struct world_rt_source_shading const *shading,
	float rgb[3])
{
	(void)shader;
	(void)texels;
	(void)shading;
	rgb[0] = rgb[1] = rgb[2] = 0.0f;
}

/* (bitmap 200 loaded, 201 once bump_loaded) */
static int bump_loaded;
static const unsigned long base_header[5], bump_header[5];

void const *world_rt_source_texture(struct world_rt_source_map const *map)
{
	return map->bitmap == 200 ? (void const *)base_header : map->bitmap == 201 && bump_loaded ? (void const *)bump_header :
		NULL;
}

short world_rt_source_definition(long definition, float sphere[4], int *item)
{
	definitions_asked++;
	/* (9 an item; 11 not, with no collision geometry either) */
	*item = definition == 9;
	sphere[0] = 0.0f;
	sphere[1] = 0.0f;
	sphere[2] = 0.5f;
	sphere[3] = 1.0f;
	return definition == 7 ? 2 : definition == 9 || definition == 11 || definition == 13 ? 1 : 0;
}

short world_rt_source_node_permutations(long definition, short node_index)
{
	return definition == 13 && node_index == 0 ? 2 : definition == 7 && node_index == 0 ? 1 : 0;
}

short world_proxies_source_permutations(long id, signed char *permutations, short maximum)
{
	CHECK(id == 0x50010 && maximum >= 1, "the permutations of %lx asked", (unsigned long)id);
	permutations[0] = fake_permutation;
	return 1;
}

long world_rt_source_node(long definition, short node_index, short permutation, float *triangles, long maximum)
{
	/* (a tetrahedron, each face wound inward) */
	static const float inward[4][9] =
	{
		{ 0, 0, 0, 1, 0, 0, 0, 1, 0 },
		{ 0, 0, 0, 0, 0, 1, 1, 0, 0 },
		{ 0, 0, 0, 0, 1, 0, 0, 0, 1 },
		{ 1, 0, 0, 0, 0, 1, 0, 1, 0 },
	};

	/* (13's: its second permutation one triangle, wound inward too) */
	if (definition == 13 && node_index == 0 && permutation == 1)
	{
		if (triangles)
		{
			CHECK(maximum >= 1, "room for %ld", maximum);
			memcpy(triangles, inward[0], sizeof(inward[0]));
		}
		return 1;
	}
	if ((definition != 7 && definition != 13) || node_index != 0 || permutation != 0)
		return 0;
	if (triangles)
	{
		CHECK(maximum >= 4, "room for %ld", maximum);
		memcpy(triangles, inward, sizeof(inward));
	}
	return 4;
}

char const *world_rt_source_name(long definition)
{
	(void)definition;
	return "a definition";
}

long world_proxies_source_instances(struct world_proxies_source_instance *instances, long maximum)
{
	static const long ids[4] = { 0x10003, 0x20005, 0x30006, 0x40008 }, definitions[4] = { 7, 9, 7, 11 };
	long index;

	if (permuted_world)
	{
		memset(&instances[0], 0, sizeof(instances[0]));
		instances[0].id = 0x50010;
		instances[0].definition = 13;
		return 1;
	}
	for (index = 0; index < 4 && index < maximum; index++)
	{
		memset(&instances[index], 0, sizeof(instances[index]));
		instances[index].id = ids[index];
		instances[index].definition = definitions[index];
		instances[index].baked = index == 2;
	}
	return index;
}

/* (world_proxies_game.c's: the fake world's static placement is the one
already baked) */
int world_proxies_source_lightmapped(long id)
{
	(void)id;
	return 0;
}

/* (dynamic_lights.c's: no light fixture is lit live in the fake world) */
int dynamic_lights_fixture_lit(long id)
{
	(void)id;
	return 0;
}

short world_proxies_source_nodes(long id, struct world_proxies_frame *frames, short maximum)
{
	short node, count = id == 0x20005 || id == 0x50010 ? 1 : 2;

	CHECK(maximum >= count, "room for %d", maximum);
	for (node = 0; node < count; node++)
	{
		float scale = id == 0x20005 ? 2.0f : 1.0f;

		memset(&frames[node], 0, sizeof(frames[node]));
		frames[node].axes[0][0] = frames[node].axes[1][1] = frames[node].axes[2][2] = scale;
		frames[node].position[0] = id == 0x20005 ? 0.0f : 10.0f * (float)(node + 1);
		frames[node].position[1] = id == 0x20005 ? 5.0f : 0.0f;
	}
	return count;
}

/* what was handed to the renderer */
static struct
{
	unsigned long id, count;
	float first[9];
} meshes[16];
static int mesh_count, releases;
/* (the last mesh as drawn: its counts, triangles' materials and the
materials it carried; the textures' slots last sent, and the times) */
static struct world_rt_mesh_header drawn;
static unsigned short drawn_materials[3];
static struct world_rt_material carried[4];
static struct world_rt_texture textures_sent[8];
static unsigned long textures_count;
static int textures_sendings;
/* (the meshes let go of alone) */
static unsigned long released[16];
static int released_count;
static struct world_ao_instance instances[16];
static unsigned long instance_count, instance_handings;

void halo_world_ao_mesh(unsigned long id, unsigned long count, float const *triangles)
{
	if (!id)
	{
		releases++;
		return;
	}
	CHECK(mesh_count < 16, "too many meshes");
	meshes[mesh_count].id = id;
	meshes[mesh_count].count = count;
	memcpy(meshes[mesh_count].first, triangles, sizeof(meshes[mesh_count].first));
	mesh_count++;
}

void halo_world_rt_mesh(unsigned long id, void const *data, unsigned long size)
{
	struct world_rt_mesh_header const *header = data;
	float const *positions = (float const *)(header + 1);
	unsigned int const *corners = (unsigned int const *)(positions + header->vertex_count * 3);
	struct world_rt_vertex const *vertices = (struct world_rt_vertex const *)(corners + header->triangle_count * 3);
	unsigned short const *materials = (unsigned short const *)(vertices + header->vertex_count);
	struct world_rt_material const *made = (struct world_rt_material const *)((unsigned char const *)materials +
		((header->triangle_count * 2 + 3) & ~3u));
	int corner;

	CHECK(id && mesh_count < 16, "a mesh as drawn %lu", id);
	CHECK(size == (unsigned long)((unsigned char const *)(made + header->material_count) - (unsigned char const *)data),
		"its size %lu", size);
	CHECK(header->triangle_count <= 3 && header->material_count <= 4, "its counts");
	drawn = *header;
	memcpy(drawn_materials, materials, sizeof(drawn_materials));
	memcpy(carried, made, header->material_count * sizeof(*made));
	/* (the structure's: the tangent's sign, the last vertex's binormal
	turned) */
	if (header->vertex_count == 7)
		CHECK(vertices[0].flags == 0 && vertices[6].flags == 1 && vertices[3].uv[0] == 3.0f && vertices[0].normal == 32767,
			"the vertices: %u %u", vertices[0].flags, vertices[6].flags);
	meshes[mesh_count].id = id;
	meshes[mesh_count].count = header->triangle_count;
	for (corner = 0; corner < 3; corner++)
		memcpy(meshes[mesh_count].first + corner * 3, positions + corners[corner] * 3, 3 * sizeof(float));
	mesh_count++;
}

/* (the skinned meshes and the frame's poses) */
static int skinned_count;
static unsigned long skinned_id, skinned_size;
static struct world_rt_pose poses_sent[4];
static unsigned long poses_count;

void halo_world_rt_skinned_mesh(unsigned long id, void const *data, unsigned long size)
{
	struct world_rt_mesh_header const *header = data;

	skinned_count++;
	skinned_id = id;
	skinned_size = size;
	CHECK(header->vertex_count == 9 && header->triangle_count == 3, "the skinned mesh's counts");
	meshes[mesh_count].id = id;
	meshes[mesh_count].count = header->triangle_count;
	mesh_count++;
}

void halo_world_rt_poses(struct world_rt_pose const *poses, unsigned long count)
{
	CHECK(count <= 4, "%lu poses", count);
	memcpy(poses_sent, poses, count * sizeof(*poses));
	poses_count = count;
}

short world_rt_source_first_person(int which, long *model, long *unit, struct world_proxies_frame *frames, short maximum)
{
	(void)which;
	(void)model;
	(void)unit;
	(void)frames;
	(void)maximum;
	return 0;
}

void world_rt_source_first_person_next(void)
{
}

void halo_world_rt_textures(struct world_rt_texture const *textures, unsigned long count)
{
	CHECK(count && count <= 8, "%lu textures", count);
	memcpy(textures_sent, textures, count * sizeof(*textures));
	textures_count = count;
	textures_sendings++;
}

void halo_world_ao_mesh_release(unsigned long id)
{
	CHECK(id && released_count < 16, "let go of %lu", id);
	released[released_count++] = id;
}

void halo_world_ao_instances(struct world_ao_instance const *given, unsigned long count)
{
	CHECK(count <= 16, "%lu instances", count);
	memcpy(instances, given, count * sizeof(*given));
	instance_count = count;
	instance_handings++;
}

#include "under_test.inc"

/* ---------- the cases */

static float signed_volume(float const *triangles, long count)
{
	double volume = 0.0;
	long index;

	for (index = 0; index < count; index++)
	{
		float const *a = triangles + index * 9, *b = a + 3, *c = a + 6;

		volume += a[0] * (b[1] * c[2] - b[2] * c[1]) + a[1] * (b[2] * c[0] - b[0] * c[2]) +
			a[2] * (b[0] * c[1] - b[1] * c[0]);
	}
	return (float)(volume / 6.0);
}

static void arithmetic_case(void)
{
	float box[12 * 9], turned[12 * 9], flat[9] = { 0, 0, 0, 1, 0, 0, 0, 1, 0 }, kept[9];
	struct world_proxies_frame frame;
	float transform[3][4];
	static const float sphere[4] = { 0.0f, 0.0f, 1.0f, 2.0f };
	long index;
	int k;

	/* the box faces out (its volume 8), and is kept so */
	world_rt_box(box);
	CHECK(fabsf(signed_volume(box, 12) - 8.0f) < 1.0e-5f, "the box's volume %g", signed_volume(box, 12));
	memcpy(turned, box, sizeof(box));
	CHECK(fabsf(world_rt_orient(turned, 12, 1.0e-6f) - 8.0f) < 1.0e-5f && !memcmp(turned, box, sizeof(box)),
		"the box turned");
	/* wound inward, turned to face out: each triangle's second and third
	corners swapped */
	for (index = 0; index < 12; index++)
	{
		for (k = 0; k < 3; k++)
		{
			turned[index * 9 + 3 + k] = box[index * 9 + 6 + k];
			turned[index * 9 + 6 + k] = box[index * 9 + 3 + k];
		}
	}
	CHECK(signed_volume(turned, 12) < 0.0f, "wound inward");
	CHECK(fabsf(world_rt_orient(turned, 12, 1.0e-6f) - 8.0f) < 1.0e-5f && !memcmp(turned, box, sizeof(box)),
		"the inward box turned to face out");
	/* a flat mesh as it is */
	memcpy(kept, flat, sizeof(flat));
	world_rt_orient(kept, 1, 1.0e-6f);
	CHECK(!memcmp(kept, flat, sizeof(flat)), "a flat mesh turned");
	/* a frame's transform: its axes as columns, its position last */
	memset(&frame, 0, sizeof(frame));
	frame.axes[0][1] = 1.0f;
	frame.axes[1][0] = -1.0f;
	frame.axes[2][2] = 1.0f;
	frame.position[0] = 3.0f;
	frame.position[1] = 4.0f;
	frame.position[2] = 5.0f;
	world_rt_transform(&frame, transform);
	CHECK(transform[1][0] == 1.0f && transform[0][1] == -1.0f && transform[2][2] == 1.0f && transform[0][3] == 3.0f &&
		transform[1][3] == 4.0f && transform[2][3] == 5.0f && transform[0][0] == 0.0f, "the frame's transform");
	/* the box's about a sphere in the node's space */
	world_rt_box_transform(&frame, sphere, transform);
	CHECK(fabsf(transform[0][1] + 2.0f * WORLD_RT_BOX_SHARE) < 1.0e-6f &&
		fabsf(transform[2][2] - 2.0f * WORLD_RT_BOX_SHARE) < 1.0e-6f && transform[2][3] == 6.0f &&
		transform[0][3] == 3.0f, "the box's transform");
	CHECK(world_rt_owner(-1) == 0 && world_rt_owner(0x10003) == 4 && world_rt_owner(0x7fff0000) == 1,
		"the owners");
	/* the octahedral normals: up, along x, down (a corner), along -y */
	{
		static const float up[3] = { 0, 0, 1 }, x[3] = { 1, 0, 0 }, down[3] = { 0, 0, -1 }, minus_y[3] = { 0, -1, 0 };

		CHECK(world_rt_octahedral(up) == 0 && world_rt_octahedral(x) == 32767 &&
			world_rt_octahedral(down) == (32767UL | 32767UL << 16) && world_rt_octahedral(minus_y) == 0x80010000UL,
			"the octahedral map: %lx %lx %lx %lx", world_rt_octahedral(up), world_rt_octahedral(x),
			world_rt_octahedral(down), world_rt_octahedral(minus_y));
	}
}

static struct world_ao_instance const *instance_of(unsigned long owner, unsigned int kind)
{
	unsigned long index;

	for (index = 0; index < instance_count; index++)
	{
		if (instances[index].owner == owner && instances[index].kind == kind)
			return &instances[index];
	}
	return NULL;
}

static void frame_case(void)
{
	struct world_ao_instance const *structure, *seven, *nine, *scenery;
	unsigned long index;

	world_rt_frame(1);
	/* the meshes: the structure's, definition 7's first node's (turned to
	face out), the box for 9 */
	CHECK(mesh_count == 3, "%d meshes", mesh_count);
	CHECK(meshes[0].id == 1 && meshes[0].count == 2, "the structure's: %lu %lu", meshes[0].id, meshes[0].count);
	CHECK(meshes[1].id == 2 && meshes[1].count == 4 && meshes[1].first[3] == 0.0f && meshes[1].first[4] == 1.0f,
		"definition 7's node, turned: %lu %lu", meshes[1].id, meshes[1].count);
	CHECK(meshes[2].id == 3 && meshes[2].count == 12, "the box: %lu %lu", meshes[2].id, meshes[2].count);
	/* the instances: the structure, 7's one node with a mesh, 9's box, and
	the baked scenery's (7's) node as the structure is (owner 0) */
	CHECK(instance_count == 4, "%lu instances", instance_count);
	structure = instance_of(0, WORLD_RT_STRUCTURE);
	CHECK(structure && structure->mesh == 1, "the structure first");
	scenery = &instances[3];
	CHECK(scenery->owner == 0 && scenery->kind == WORLD_RT_STRUCTURE && scenery->mesh == 2, "the baked scenery's: %u %u %u",
		scenery->owner, scenery->kind, scenery->mesh);
	seven = instance_of(4, WORLD_RT_OBJECT);
	nine = instance_of(6, WORLD_RT_OBJECT);
	CHECK(structure && structure->mesh == 1 && structure->transform[0][0] == 1.0f && structure->transform[0][3] == 0.0f,
		"the structure's");
	CHECK(seven && seven->mesh == 2 && seven->transform[0][3] == 10.0f && seven->transform[1][1] == 1.0f,
		"definition 7's node");
	CHECK(nine && nine->mesh == 3 && fabsf(nine->transform[0][0] - 2.0f * WORLD_RT_BOX_SHARE) < 1.0e-6f &&
		nine->transform[1][3] == 5.0f && nine->transform[2][3] == 1.0f, "9's box");
	for (index = 0; index < instance_count; index++)
		CHECK(instances[index].owner != 7 && instances[index].owner != 9, "the baked scenery's or 11's");
	/* the same frame again: nothing more; the next, the same meshes */
	world_rt_frame(1);
	CHECK(instance_handings == 1, "handed twice in a frame");
	world_rt_frame(2);
	CHECK(instance_handings == 2 && instance_count == 4 && mesh_count == 3, "the next frame: %lu %d", instance_count,
		mesh_count);
	CHECK(structure_asked == 2 && definitions_asked == 3, "asked again: %d %d", structure_asked, definitions_asked);
	/* another BSP: its structure's mesh */
	bsp_index = 1;
	world_rt_frame(3);
	CHECK(mesh_count == 4 && meshes[3].id == 4 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 4,
		"another BSP's structure");
	/* back to the first: its mesh kept, nothing made or read again */
	bsp_index = 0;
	world_rt_frame(4);
	CHECK(mesh_count == 4 && structure_asked == 4 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 1,
		"back to the first BSP: %d meshes, asked %d", mesh_count, structure_asked);
	CHECK(released_count == 0, "%d let go of", released_count);
	/* the map's end: every mesh let go of, made again from 1 */
	world_rt_map_unloaded();
	CHECK(releases == 1, "%d releases", releases);
	world_rt_frame(5);
	CHECK(mesh_count == 7 && meshes[4].id == 1, "made again: %d %lu", mesh_count, meshes[4].id);
	/* (debug.rt_render_geometry off: the render triangles never read) */
	CHECK(render_asked == 0, "the render triangles read %d times", render_asked);
}

static void render_case(void)
{
	int asked;

	render_on = 1;
	/* the first frame: the collision mesh alone, as without the switch (a
	load waits for nothing more) */
	world_rt_frame(1);
	CHECK(mesh_count == 3 && render_asked == 0, "the first frame: %d meshes, %d asked", mesh_count, render_asked);
	CHECK(instance_of(0, WORLD_RT_STRUCTURE)->mesh == 1, "the collision mesh first");
	/* the next: the render mesh, in the collision mesh's place: its opaque
	triangles, then its alpha-tested one */
	world_rt_frame(2);
	CHECK(mesh_count == 4 && meshes[3].id == 4 && meshes[3].count == 3 && meshes[3].first[2] == 0.0f &&
		meshes[3].first[5] == 1.0f, "the render mesh: %d %lu %lu", mesh_count, meshes[3].id, meshes[3].count);
	CHECK(render_filled == 1 && drawn.opaque_count == 2 && drawn.vertex_count == 7, "filled %d, %u opaque",
		render_filled, drawn.opaque_count);
	/* (its materials the map's, both new to it; the alpha-tested one's alpha
	map the bump map's slot, at its scale) */
	CHECK(drawn.material_count == 2 && carried[0].index == 0 && carried[1].index == 1 && drawn_materials[0] == 0 &&
		drawn_materials[1] == 0 && drawn_materials[2] == 1, "its materials: %u", drawn.material_count);
	CHECK(carried[0].alpha_map == 0 && carried[1].alpha_map && carried[1].alpha_map == carried[1].bump_map &&
		carried[1].alpha_scale[0] == 2.0f && carried[0].base_map && carried[0].base_map == carried[1].base_map &&
		carried[0].lightmap && carried[0].lightmap != carried[0].base_map && carried[1].flags == WORLD_RT_MATERIAL_ALPHA_TESTED,
		"the materials' slots: %u %u %u", carried[1].alpha_map, carried[0].base_map, carried[0].lightmap);
	/* (the textures' slots: base, lightmap and bump, the bump map not loaded) */
	CHECK(textures_sendings == 1 && textures_count == 3, "the textures sent: %d, %lu", textures_sendings, textures_count);
	CHECK(textures_sent[0].header == base_header && textures_sent[0].slot == carried[0].base_map &&
		textures_sent[2].slot == carried[1].bump_map && !textures_sent[2].header, "the textures' headers");
	CHECK(instance_count == 4 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 4, "the structure's instance: %u",
		instance_of(0, WORLD_RT_STRUCTURE)->mesh);
	/* (and its collision mesh let go of) */
	CHECK(released_count == 1 && released[0] == 1, "let go of: %d %lu", released_count, released[0]);
	/* made once; a texture sent again only as it changes */
	world_rt_frame(3);
	CHECK(mesh_count == 4 && render_filled == 1 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 4, "made again: %d",
		mesh_count);
	CHECK(textures_sendings == 1, "the textures sent again: %d", textures_sendings);
	bump_loaded = 1;
	world_rt_frame(4);
	CHECK(textures_sendings == 2 && textures_count == 1 && textures_sent[0].header == bump_header &&
		textures_sent[0].slot == carried[1].bump_map, "the bump map once loaded: %d %lu", textures_sendings, textures_count);
	/* another BSP: its collision mesh, then its render mesh, whose materials
	the map has already */
	bsp_index = 1;
	world_rt_frame(5);
	CHECK(mesh_count == 5 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 5, "another BSP's collision mesh");
	world_rt_frame(6);
	CHECK(mesh_count == 6 && meshes[5].count == 3 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 6,
		"another BSP's render mesh");
	CHECK(drawn.material_count == 0 && drawn_materials[2] == 1, "its materials carried again: %u", drawn.material_count);
	CHECK(released_count == 2 && released[1] == 5, "another BSP's collision mesh let go of: %d", released_count);
	/* back to the first: its render mesh at once, nothing made or read */
	bsp_index = 0;
	asked = render_asked;
	world_rt_frame(7);
	CHECK(mesh_count == 6 && render_asked == asked && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 4,
		"back to the first BSP: %d meshes, its render mesh %u", mesh_count, instance_of(0, WORLD_RT_STRUCTURE)->mesh);
	world_rt_frame(8);
	CHECK(mesh_count == 6 && render_asked == asked, "back to the first BSP, the frame after: %d", mesh_count);
	/* the map's end: the collision mesh first again, the materials and
	textures the next map's */
	world_rt_map_unloaded();
	world_rt_frame(9);
	CHECK(mesh_count == 9 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 1, "after the map's end: %d", mesh_count);
	world_rt_frame(10);
	CHECK(mesh_count == 10 && instance_of(0, WORLD_RT_STRUCTURE)->mesh == 4 && drawn.material_count == 2,
		"its render mesh again: %u materials", drawn.material_count);
	CHECK(textures_count == 3, "its textures again: %lu", textures_count);
}

static void model_case(void)
{
	struct world_ao_instance const *seven;

	render_on = model_on = 1;
	world_rt_frame(1);
	/* definition 7's model's meshes: node 0's two triangles (the second
	turned to face as its normals do), node 1's one in its own space; no
	collision mesh for it; the other definitions as before */
	CHECK(model_asked == 2, "the model read %d times", model_asked);
	CHECK(mesh_count == 4 && meshes[1].id == 2 && meshes[1].count == 2 && meshes[2].id == 3 && meshes[2].count == 1,
		"the model's meshes: %d (%lu %lu) (%lu %lu)", mesh_count, meshes[1].id, meshes[1].count, meshes[2].id,
		meshes[2].count);
	CHECK(meshes[2].first[0] == 0.0f && meshes[2].first[3] == 1.0f, "node 1's in its own space: %g %g", meshes[2].first[0],
		meshes[2].first[3]);
	CHECK(meshes[3].count == 12, "9's box: %lu", meshes[3].count);
	seven = instance_of(4, WORLD_RT_OBJECT);
	CHECK(seven && seven->mesh == 2 && seven->transform[0][3] == 10.0f, "7's node 0");
	CHECK(instance_count == 6, "%lu instances", instance_count);
	{
		struct world_rt_source_vertex vertices[3];
		unsigned long triangle[3] = { 0, 1, 2 };

		memset(vertices, 0, sizeof(vertices));
		vertices[1].position[0] = vertices[2].position[1] = 1.0f;
		vertices[0].normal[2] = vertices[1].normal[2] = vertices[2].normal[2] = -1.0f;
		world_rt_face_out(vertices, triangle);
		CHECK(triangle[0] == 0 && triangle[1] == 2 && triangle[2] == 1, "turned to face down");
		vertices[0].weight = vertices[1].weight = vertices[2].weight = 1.0f;
		vertices[1].nodes[0] = vertices[2].nodes[0] = 3;
		CHECK(world_rt_triangle_node(&vertices[0], &vertices[1], &vertices[2]) == 3, "two corners' node");
		vertices[2].nodes[0] = 4;
		CHECK(world_rt_triangle_node(&vertices[0], &vertices[1], &vertices[2]) == 0, "else the first's");
	}
}

static void skin_case(void)
{
	struct world_ao_instance const *seven = NULL;
	unsigned long index;

	render_on = 1;
	model_on = 2;
	world_rt_frame(1);
	/* definition 7's joint blends two nodes: one skinned mesh (its skinning
	after its data: 8 bytes a vertex), not a mesh for each node */
	CHECK(skinned_count == 1 && skinned_id == 2, "the skinned mesh: %d %lu", skinned_count, skinned_id);
	CHECK(mesh_count == 3, "%d meshes", mesh_count);
	/* each of its objects one instance on its first node, posed: 0x10003
	and the baked 0x30006 */
	CHECK(poses_count == 2, "%lu poses", poses_count);
	for (index = 0; index < instance_count; index++)
	{
		if (instances[index].owner == 4 && instances[index].kind == WORLD_RT_OBJECT)
			seven = &instances[index];
	}
	CHECK(seven && seven->mesh == 2 && seven->pad == 1 && seven->transform[0][3] == 10.0f, "7's instance: %u %u",
		seven ? seven->mesh : 0, seven ? seven->pad : 0);
	/* its pose: each node's frame after its default inverse, into the first
	node's space (node 1 at 20 after -5, from node 0 at 10: 5) */
	CHECK(poses_sent[0].mesh == 2 && poses_sent[0].owner == 4 && poses_sent[0].node_count == 2 &&
		poses_sent[0].transforms[0][0][3] == 0.0f && poses_sent[0].transforms[1][0][3] == 5.0f &&
		poses_sent[0].transforms[1][0][0] == 1.0f, "its pose: %g %g", poses_sent[0].transforms[0][0][3],
		poses_sent[0].transforms[1][0][3]);
	/* the next frame: posed again, nothing made again */
	world_rt_frame(2);
	CHECK(skinned_count == 1 && poses_count == 2, "the next frame: %d %lu", skinned_count, poses_count);
	{
		float a[3][4] = { { 0, -2, 0, 1 }, { 2, 0, 0, 2 }, { 0, 0, 2, 3 } }, inverse[3][4], back[3][4];
		int row, column;

		CHECK(world_rt_inverse(a, inverse), "an inverse");
		world_rt_compose(a, inverse, back);
		for (row = 0; row < 3; row++)
		{
			for (column = 0; column < 4; column++)
				CHECK(fabsf(back[row][column] - (row == column ? 1.0f : 0.0f)) < 1.0e-6f, "a times its inverse");
		}
	}
}

/* a node's collision mesh of the permutation its object has now (a dead
Jackal's shield off: no shadow or occlusion of it, #1311), each made once */
static void permutation_case(void)
{
	permuted_world = 1;
	fake_permutation = 0;
	world_rt_frame(1);
	/* the structure's, 13's node's two permutations' */
	CHECK(mesh_count == 3 && meshes[1].count == 4 && meshes[2].count == 1, "%d meshes: %lu %lu", mesh_count,
		meshes[1].count, meshes[2].count);
	CHECK(instance_count == 2 && instances[1].mesh == meshes[1].id, "the first permutation's: %lu %u", instance_count,
		instances[1].mesh);
	/* its shield off: the other permutation's mesh, nothing made again */
	fake_permutation = 1;
	world_rt_frame(2);
	CHECK(mesh_count == 3 && instance_count == 2 && instances[1].mesh == meshes[2].id, "the second permutation's: %d %u",
		mesh_count, instances[1].mesh);
	/* a permutation with no geometry (or none): no instance */
	fake_permutation = -1;
	world_rt_frame(3);
	CHECK(instance_count == 1, "none: %lu instances", instance_count);
	fake_permutation = 0;
	world_rt_frame(4);
	CHECK(instance_count == 2 && instances[1].mesh == meshes[1].id, "back up: %u", instances[1].mesh);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "arithmetic"))
		arithmetic_case();
	else if (!strcmp(name, "frame"))
		frame_case();
	else if (!strcmp(name, "render"))
		render_case();
	else if (!strcmp(name, "model"))
		model_case();
	else if (!strcmp(name, "skin"))
		skin_case();
	else if (!strcmp(name, "permutation"))
		permutation_case();
	else
		return 2;
	return 0;
}
