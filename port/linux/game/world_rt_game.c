/*
WORLD_RT_GAME.C

What display.ambient_occlusion "ray_traced"'s scene (world_rt.c) reads of
the game (world_rt.h's world_rt_source_*): the loaded structure BSP's
collision geometry, as world_scene.c fans it, and a definition's nodes'
collision geometry (the first permutation's BSP of each collision node, the
model's node of the same index, as the collision tests take them:
collision_models.c) and its bounding sphere. As drawn (debug.rt_render_geometry, by default),
the render meshes too (docs/rt-plan/1-geometry.md): the structure's, its
lightmaps' materials' surfaces over their compressed environment vertices,
sorted by the material's shader into opaque, alpha-tested and transparent
ones; and a definition's model's, its regions' first permutations at one
level of detail for every view, read from the vertex and index buffers the
game draws them from (as object_mesh.c reads them), each vertex with its
nodes. Read only; render-only.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/object_types.h"
#include "game/players.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "shaders/shaders.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "cache/texture_cache.h"

#include "world_proxies_source.h"
#include "world_rt.h"
#include "world_scene.h"

#include <stddef.h>
#include <string.h>
#include <xtl.h>

/* (cache_files.c) */
boolean tag_index_is_group(long tag_index, long group_tag);
/* (mod_materials.c) a bitmap's mods' material maps (0 none) */
void mod_materials_maps(long base_map_tag_index, unsigned int *normal_map, unsigned int *orm_map);

/* ---------- constants */

enum
{
	/* a shader's type (shaders.c's _shader_type_*) */
	_world_rt_shader_type_environment = 3,
	_world_rt_shader_type_model = 4,
	_world_rt_shader_type_transparent_generic = 5,
	_world_rt_shader_type_transparent_chicago = 6,
	/* a transparent shader's flags and blend (rasterizer_xbox_transparent_
	geometry.c's): alpha-tested, a decal, two-sided; alpha-blended */
	_world_rt_shader_transparent_alpha_tested_bit = 0,
	_world_rt_shader_transparent_decal_bit = 1,
	_world_rt_shader_transparent_two_sided_bit = 2,
	_world_rt_framebuffer_blend_alpha_blend = 0,
	/* shader_environment's flags (rasterizer_xbox_environment.c's) */
	_world_rt_shader_environment_alpha_tested_bit = 0,
	_world_rt_shader_environment_bump_map_is_specular_mask_bit = 1,
	/* shader_model's (rasterizer_xbox_models.c's) */
	_world_rt_shader_model_two_sided_bit = 1,
	_world_rt_shader_model_not_alpha_tested_bit = 2,
	/* a bitmap's type (bitmap_group.h's _bitmap_type_2d) */
	_world_rt_bitmap_type_2d = 0,
};

/* ---------- structures */

/* (tag_schema_render.c's shader_environment_definition: what a hit reads) */
struct world_rt_shader_environment
{
	struct shader shader;
	word flags;
	short type;
	byte reserved2C[0x88 - 0x2C];
	struct tag_reference base_map;
	byte reserved98[0xB4 - 0x98];
	real primary_detail_map_scale;
	struct tag_reference primary_detail_map;
	byte reservedC8[0x124 - 0xC8];
	real bump_map_scale;
	struct tag_reference bump_map;
};

/* (and its shader_model_definition's) */
struct world_rt_shader_model
{
	struct shader shader;
	word flags;
	short type;
	byte reserved2C[0x9C - 0x2C];
	real map_u_scale, map_v_scale;
	struct tag_reference base_map;
	byte reservedB4[0xBC - 0xB4];
	struct tag_reference multipurpose_map;
	byte reservedCC[0xD8 - 0xCC];
	real detail_map_scale;
	struct tag_reference detail_map;
};

/* (rasterizer_xbox_transparent_geometry.c's shader_transparent_generic and
_chicago, the same to their maps; and each one's map: its scales and
bitmap) */
struct world_rt_shader_transparent
{
	struct shader shader;
	byte numeric_counter_limit;
	byte flags;
	short type;
	short framebuffer_blend_function;
	byte reserved2E[0x54 - 0x2E];
	struct tag_block maps;
};

struct world_rt_transparent_generic_map
{
	word flags;
	word pad;
	real map_u_scale, map_v_scale;
	byte reserved0C[0x18 - 0x0C];
	struct tag_reference map;
	byte reserved28[0x64 - 0x28];
};

struct world_rt_transparent_chicago_map
{
	byte reserved00[0x54];
	real map_u_scale, map_v_scale;
	byte reserved5C[0x6C - 0x5C];
	struct tag_reference map;
	byte reserved7C[0xDC - 0x7C];
};

typedef char verify_world_rt_shader_transparent_maps[offsetof(struct world_rt_shader_transparent, maps) == 0x54 ? 1 : -1];
typedef char verify_world_rt_shader_transparent_blend[
	offsetof(struct world_rt_shader_transparent, framebuffer_blend_function) == 0x2C ? 1 : -1];
typedef char verify_world_rt_transparent_generic_map[sizeof(struct world_rt_transparent_generic_map) == 0x64 ? 1 : -1];
typedef char verify_world_rt_transparent_chicago_map[sizeof(struct world_rt_transparent_chicago_map) == 0xDC ? 1 : -1];

/* (rasterizer_geometry.c's environment_vertex_compressed) */
struct environment_vertex_compressed_layout
{
	real_point3d position;
	unsigned long normal, binormal, tangent;
	real_point2d texcoord;
};

typedef char verify_world_rt_shader_environment_base_map[
	offsetof(struct world_rt_shader_environment, base_map) == 0x88 ? 1 : -1];
typedef char verify_world_rt_shader_environment_detail_map[
	offsetof(struct world_rt_shader_environment, primary_detail_map) == 0xB8 ? 1 : -1];
typedef char verify_world_rt_shader_environment_bump_map[
	offsetof(struct world_rt_shader_environment, bump_map) == 0x128 ? 1 : -1];
typedef char verify_world_rt_shader_model_base_map[offsetof(struct world_rt_shader_model, base_map) == 0xA4 ? 1 : -1];
typedef char verify_world_rt_shader_model_multipurpose_map[
	offsetof(struct world_rt_shader_model, multipurpose_map) == 0xBC ? 1 : -1];
typedef char verify_world_rt_shader_model_detail_map[offsetof(struct world_rt_shader_model, detail_map) == 0xDC ? 1 : -1];
typedef char verify_world_rt_environment_vertex[sizeof(struct environment_vertex_compressed_layout) == 32 ? 1 : -1];

/* this build's model geometry and part (models.c's, as object_mesh.c has
them: no header declares them) */
struct world_rt_model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct world_rt_model_part
{
	unsigned long flags;
	short shader_index;
	char previous_part_index;
	char next_part_index;
	short centroid_primary_node_index;
	short centroid_secondary_node_index;
	real centroid_primary_node_weight;
	real centroid_secondary_node_weight;
	real_point3d centroid;
	struct tag_block uncompressed_vertices;
	struct tag_block compressed_vertices;
	struct tag_block triangles;
	struct triangle_buffer triangle_buffer;
	struct vertex_buffer vertex_buffer;
};

struct world_rt_model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

typedef char verify_world_rt_model_part[sizeof(struct world_rt_model_part) == 0x68 ? 1 : -1];

enum
{
	/* (models.c's) a part not drawn; one whose vertices name its own nodes */
	_world_rt_model_part_stripped_bit = 0,
	_world_rt_model_part_local_nodes_bit = 1,
	/* the levels of detail (models.c's), and the one the rays take: the
	second most detailed, the nearest there is where a permutation has
	none (docs/rt-plan/1-geometry.md: one level for every view) */
	_world_rt_model_detail_levels = 5,
	_world_rt_model_detail_rays = 3,
};

/* ---------- private code */

/* a compressed normal (rasterizer_geometry.c's) as three floats */
static void unpacked(unsigned long compressed, float out[3])
{
	union real_vector3d vector = uncompress_int32_to_real_vector3d(compressed);

	out[0] = vector.i;
	out[1] = vector.j;
	out[2] = vector.k;
}

/* a tag reference's map, its first bitmap (none: NONE) */
static void map_of(struct tag_reference const *reference, struct world_rt_source_map *map)
{
	map->bitmap = reference->index;
	map->index = 0;
}

/* a transparent shader drawn as a cutout (a tree's leaves, a fence's
card): shader_transparent_generic or _chicago, alpha-tested and blended
by alpha, not a decal, with a first map; its first map's bitmap and
scales into material (NULL: not one) */
static boolean shader_cutout(struct shader const *shader, struct world_rt_source_material *material)
{
	struct world_rt_shader_transparent const *transparent = (void const *)shader;
	struct tag_reference const *map;
	real u_scale, v_scale;

	if (!shader || (shader->base.type != _world_rt_shader_type_transparent_generic &&
		shader->base.type != _world_rt_shader_type_transparent_chicago) ||
		!TEST_FLAG(transparent->flags, _world_rt_shader_transparent_alpha_tested_bit) ||
		TEST_FLAG(transparent->flags, _world_rt_shader_transparent_decal_bit) ||
		transparent->framebuffer_blend_function != _world_rt_framebuffer_blend_alpha_blend ||
		transparent->maps.count <= 0 || !transparent->maps.address)
	{
		return FALSE;
	}
	if (shader->base.type == _world_rt_shader_type_transparent_generic)
	{
		struct world_rt_transparent_generic_map const *first = transparent->maps.address;

		map = &first->map;
		u_scale = first->map_u_scale;
		v_scale = first->map_v_scale;
	}
	else
	{
		struct world_rt_transparent_chicago_map const *first = transparent->maps.address;

		map = &first->map;
		u_scale = first->map_u_scale;
		v_scale = first->map_v_scale;
	}
	if (map->index == NONE)
		return FALSE;
	if (material)
	{
		map_of(map, &material->maps[WORLD_RT_MAP_BASE]);
		material->flags |= WORLD_RT_MATERIAL_ALPHA_TESTED;
		if (TEST_FLAG(transparent->flags, _world_rt_shader_transparent_two_sided_bit))
			material->flags |= WORLD_RT_MATERIAL_TWO_SIDED;
		material->alpha_map = WORLD_RT_MAP_BASE;
		material->alpha_scale[0] = u_scale != 0.0f ? u_scale : 1.0f;
		material->alpha_scale[1] = v_scale != 0.0f ? v_scale : 1.0f;
	}
	return TRUE;
}

/* ---------- its reflection (the light transport's: where a reflection
ray is worth its cost, lt/indirect.glsl): shader_environment's specular and
reflection blocks (rt_lights_materials.c's rtlm_environment_specular,
_reflection: the specular brightness at 0x290, its tint head-on and at
grazing view at 0x2A8, 0x2B4; the reflection's brightness head-on and at
grazing view at 0x2F4, 0x2F8), shader_model's (its brightness and tint
head-on and at grazing view at 0x144 to 0x160) */
#define WORLD_RT_ENVIRONMENT_SPECULAR_BRIGHTNESS 0x290
#define WORLD_RT_ENVIRONMENT_SPECULAR_PERPENDICULAR 0x2A8
#define WORLD_RT_ENVIRONMENT_SPECULAR_PARALLEL 0x2B4
#define WORLD_RT_ENVIRONMENT_REFLECTION_PERPENDICULAR 0x2F4
#define WORLD_RT_ENVIRONMENT_REFLECTION_PARALLEL 0x2F8
#define WORLD_RT_MODEL_REFLECTION_PERPENDICULAR 0x144
#define WORLD_RT_MODEL_REFLECTION_PARALLEL 0x154

static float shader_real(void const *shader, int offset)
{
	float value;

	memcpy(&value, (char const *)shader + offset, sizeof(value));
	return value == value ? value : 0.0f;
}

static float shader_luminance(void const *shader, int offset)
{
	return 0.2126f * shader_real(shader, offset) + 0.7152f * shader_real(shader, offset + 4) +
		0.0722f * shader_real(shader, offset + 8);
}

/* a strength as the flags carry it (x 128, 0 to 255) */
static unsigned long reflection_byte(float strength)
{
	float scaled = strength * 128.0f + 0.5f;

	return scaled <= 0.0f ? 0ul : scaled >= 255.0f ? 255ul : (unsigned long)scaled;
}

static void material_reflection(struct shader const *shader, struct world_rt_source_material *material)
{
	float perpendicular, parallel;

	if (shader->base.type == _world_rt_shader_type_environment)
	{
		float brightness = shader_real(shader, WORLD_RT_ENVIRONMENT_SPECULAR_BRIGHTNESS);

		perpendicular = brightness * shader_luminance(shader, WORLD_RT_ENVIRONMENT_SPECULAR_PERPENDICULAR) *
			shader_real(shader, WORLD_RT_ENVIRONMENT_REFLECTION_PERPENDICULAR);
		parallel = brightness * shader_luminance(shader, WORLD_RT_ENVIRONMENT_SPECULAR_PARALLEL) *
			shader_real(shader, WORLD_RT_ENVIRONMENT_REFLECTION_PARALLEL);
		if (TEST_FLAG(((struct world_rt_shader_environment const *)shader)->flags,
			_world_rt_shader_environment_bump_map_is_specular_mask_bit))
			material->flags |= WORLD_RT_MATERIAL_MASK_BUMP;
	}
	else if (shader->base.type == _world_rt_shader_type_model)
	{
		perpendicular = shader_real(shader, WORLD_RT_MODEL_REFLECTION_PERPENDICULAR) *
			shader_luminance(shader, WORLD_RT_MODEL_REFLECTION_PERPENDICULAR + 4);
		parallel = shader_real(shader, WORLD_RT_MODEL_REFLECTION_PARALLEL) *
			shader_luminance(shader, WORLD_RT_MODEL_REFLECTION_PARALLEL + 4);
		material->flags |= WORLD_RT_MATERIAL_MASK_MULTIPURPOSE;
	}
	else
		return;
	material->flags |= reflection_byte(perpendicular) << WORLD_RT_MATERIAL_REFLECTION_PERPENDICULAR_SHIFT |
		reflection_byte(parallel) << WORLD_RT_MATERIAL_REFLECTION_PARALLEL_SHIFT;
}

/* a shader as a render mesh's material: its type and flags, its maps, and
what its alpha test tests (rasterizer_xbox_environment.c: an alpha-tested
environment's bump map's alpha; rasterizer_xbox_models.c: a model's base
map's, unless it is not alpha-tested; both above 0x7F) */
static void material_describe(long shader_index, struct world_rt_source_material *material)
{
	struct shader const *shader = shader_index != NONE && tag_index_is_group(shader_index, SHADER_DEFINITION_TAG) ?
		shader_definition_get(shader_index) : NULL;
	int map;

	memset(material, 0, sizeof(*material));
	material->shader = shader_index;
	material->alpha_map = -1;
	material->alpha_cutoff = 127.0f / 255.0f;
	material->alpha_scale[0] = material->alpha_scale[1] = material->detail_scale = 1.0f;
	for (map = 0; map < WORLD_RT_MAPS; map++)
	{
		material->maps[map].bitmap = NONE;
		material->maps[map].index = 0;
	}
	if (!shader)
		return;
	material->kind = (unsigned long)shader->base.type;
	if (shader->base.type == _world_rt_shader_type_environment)
	{
		struct world_rt_shader_environment const *environment = (void const *)shader;

		map_of(&environment->base_map, &material->maps[WORLD_RT_MAP_BASE]);
		map_of(&environment->primary_detail_map, &material->maps[WORLD_RT_MAP_DETAIL]);
		map_of(&environment->bump_map, &material->maps[WORLD_RT_MAP_BUMP]);
		material->detail_scale = environment->primary_detail_map_scale > 0.0f ? environment->primary_detail_map_scale :
			1.0f;
		if (TEST_FLAG(environment->flags, _world_rt_shader_environment_alpha_tested_bit))
		{
			material->flags |= WORLD_RT_MATERIAL_ALPHA_TESTED;
			material->alpha_map = TEST_FLAG(environment->flags, _world_rt_shader_environment_bump_map_is_specular_mask_bit) ?
				WORLD_RT_MAP_BASE : WORLD_RT_MAP_BUMP;
			if (material->alpha_map == WORLD_RT_MAP_BUMP && environment->bump_map_scale > 0.0f)
				material->alpha_scale[0] = material->alpha_scale[1] = environment->bump_map_scale;
		}
	}
	else if (shader->base.type == _world_rt_shader_type_model)
	{
		struct world_rt_shader_model const *model = (void const *)shader;

		map_of(&model->base_map, &material->maps[WORLD_RT_MAP_BASE]);
		map_of(&model->multipurpose_map, &material->maps[WORLD_RT_MAP_MULTIPURPOSE]);
		map_of(&model->detail_map, &material->maps[WORLD_RT_MAP_DETAIL]);
		material->detail_scale = model->detail_map_scale > 0.0f ? model->detail_map_scale : 1.0f;
		material->alpha_scale[0] = model->map_u_scale != 0.0f ? model->map_u_scale : 1.0f;
		material->alpha_scale[1] = model->map_v_scale != 0.0f ? model->map_v_scale : 1.0f;
		if (TEST_FLAG(model->flags, _world_rt_shader_model_two_sided_bit))
			material->flags |= WORLD_RT_MATERIAL_TWO_SIDED;
		if (!TEST_FLAG(model->flags, _world_rt_shader_model_not_alpha_tested_bit))
		{
			material->flags |= WORLD_RT_MATERIAL_ALPHA_TESTED;
			material->alpha_map = WORLD_RT_MAP_BASE;
		}
	}
	/* (a transparent one drawn as a cutout: its first map's alpha tested) */
	else if (!shader_cutout(shader, material))
		material->flags |= WORLD_RT_MATERIAL_TRANSPARENT;
	material_reflection(shader, material);
	/* (the mods' material maps of its base map: docs/mods.md) */
	if (material->maps[WORLD_RT_MAP_BASE].bitmap != NONE)
		mod_materials_maps(material->maps[WORLD_RT_MAP_BASE].bitmap, &material->normal_image, &material->orm_image);
}

/* a structure material's surfaces' kind (world_rt.h's WORLD_RT_SURFACES_*;
-1 none: no shader). A BSP material that is not a shader_environment is a
transparent one (water, glass, chicago, generic, plasma, meter) */
static int material_surfaces(struct structure_material const *material)
{
	struct shader const *shader;

	if (material->shader.index == NONE || !tag_index_is_group(material->shader.index, SHADER_DEFINITION_TAG))
		return -1;
	shader = shader_definition_get(material->shader.index);
	if (!shader)
		return -1;
	if (shader->base.type != _world_rt_shader_type_environment)
		return shader_cutout(shader, NULL) ? WORLD_RT_SURFACES_ALPHA_TESTED : WORLD_RT_SURFACES_TRANSPARENT;
	return TEST_FLAG(((struct world_rt_shader_environment const *)shader)->flags,
		_world_rt_shader_environment_alpha_tested_bit) ? WORLD_RT_SURFACES_ALPHA_TESTED : WORLD_RT_SURFACES_OPAQUE;
}

/* a tag that is a model (not an object's definition: the first-person
weapon's and hands', world_rt_source_first_person) */
static boolean definition_is_model(long definition)
{
	return tag_index_is_group(definition, MODELS_GROUP_TAG) || tag_index_is_group(definition, 'mod2');
}

/* a definition's collision model (NULL none: a model itself, the
first-person weapon's or hands', has none) */
static struct collision_model const *definition_collision_model(long definition)
{
	struct object_definition const *object_definition;

	if (definition == NONE || definition_is_model(definition))
		return NULL;
	object_definition = object_definition_get(definition);
	if (!object_definition || object_definition->object.collision_model.index == NONE)
		return NULL;
	return collision_model_definition_get(object_definition->object.collision_model.index);
}

/* a collision model's node's BSP of a permutation (NULL none) */
static struct collision_bsp const *node_collision_bsp(struct collision_model const *collision_model, short node_index,
	short permutation)
{
	struct collision_node const *collision_node;

	if (!collision_model || !collision_model->nodes.address || node_index < 0 ||
		node_index >= collision_model->nodes.count)
	{
		return NULL;
	}
	collision_node = TAG_BLOCK_GET_ELEMENT(&collision_model->nodes, node_index, struct collision_node);
	if (!collision_node->bsps.address || permutation < 0 || permutation >= collision_node->bsps.count)
		return NULL;
	return TAG_BLOCK_GET_ELEMENT(&collision_node->bsps, permutation, struct collision_bsp);
}

/* ---------- public code */

short world_rt_source_bsp(void)
{
	return global_scenario_try_and_get() && global_collision_bsp ? global_structure_bsp_index : NONE;
}

long world_rt_source_structure(short bsp_index, float *triangles, long maximum)
{
	struct world_scene_triangles found;
	long count;

	if (!world_scene_static_triangles(bsp_index, &found))
		return 0;
	count = found.count;
	if (triangles)
	{
		count = MIN(count, maximum);
		if (count > 0)
			memcpy(triangles, found.positions, (size_t)count * 9 * sizeof(float));
	}
	world_scene_triangles_free(&found);
	return count;
}

long world_rt_source_structure_mesh(short bsp_index, struct world_rt_source_mesh *mesh)
{
	struct structure_bsp const *structure_bsp;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_index;
	int surfaces;
	int filled = mesh->vertices && mesh->triangles && mesh->triangle_materials && mesh->materials;
	long vertex_room = mesh->vertex_count, triangle_room = mesh->triangle_count, material_room = mesh->material_count;

	mesh->vertex_count = mesh->triangle_count = mesh->opaque_count = mesh->material_count = mesh->left_out = 0;
	/* (the loaded BSP alone, as the collision's: world_rt_source_bsp) */
	if (bsp_index < 0 || bsp_index != world_rt_source_bsp())
		return 0;
	structure_bsp = global_structure_bsp_get();
	if (!structure_bsp || !structure_bsp->lightmaps.address || !structure_bsp->surfaces.address)
		return 0;
	/* (the opaque materials, then the alpha-tested) */
	for (surfaces = WORLD_RT_SURFACES_OPAQUE; surfaces <= WORLD_RT_SURFACES_ALPHA_TESTED; surfaces++)
	{
		for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap const *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
				struct structure_lightmap);
			long material_index;

			if (!lightmap->materials.address)
				continue;
			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material const *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_index,
					struct structure_material);
				byte const *vertices = (byte const *)material->compressed_vertex_data.address;
				long surface_index, vertex_index, first_vertex = mesh->vertex_count;
				long vertex_count = material->vertices.count;
				int kind = material_surfaces(material);
				/* (its lightmap vertices after its own, where it has them) */
				byte const *lightmap_vertices = material->lightmap_vertices.count == vertex_count &&
					material->compressed_vertex_data.size >= vertex_count * (vertex_size + 8) ?
					vertices + vertex_count * vertex_size : NULL;

				if (kind == WORLD_RT_SURFACES_TRANSPARENT && surfaces == WORLD_RT_SURFACES_OPAQUE)
					mesh->left_out += MAX(material->surface_count, 0);
				/* (its compressed vertices there to read and its surfaces among
				the BSP's: mod_lightmaps.c's material_readable) */
				if (kind != surfaces || !vertices || vertex_count <= 0 ||
					material->compressed_vertex_data.size < vertex_count * vertex_size ||
					material->surface_count <= 0 || material->first_surface_index < 0 ||
					material->first_surface_index > structure_bsp->surfaces.count - material->surface_count ||
					mesh->material_count >= WORLD_RT_MATERIALS)
				{
					continue;
				}
				if (filled)
				{
					if (mesh->vertex_count + vertex_count > vertex_room || mesh->material_count >= material_room)
						return 0;
					material_describe(material->shader.index, &mesh->materials[mesh->material_count]);
					mesh->materials[mesh->material_count].maps[WORLD_RT_MAP_LIGHTMAP].bitmap =
						lightmap->bitmap_index != NONE ? structure_bsp->lightmap_group.index : NONE;
					mesh->materials[mesh->material_count].maps[WORLD_RT_MAP_LIGHTMAP].index = lightmap->bitmap_index;
					for (vertex_index = 0; vertex_index < vertex_count; vertex_index++)
					{
						struct environment_vertex_compressed_layout const *in =
							(void const *)(vertices + vertex_index * vertex_size);
						struct world_rt_source_vertex *out = &mesh->vertices[first_vertex + vertex_index];

						memset(out, 0, sizeof(*out));
						memcpy(out->position, &in->position, sizeof(out->position));
						unpacked(in->normal, out->normal);
						unpacked(in->tangent, out->tangent);
						unpacked(in->binormal, out->binormal);
						out->uv[0] = in->texcoord.x;
						out->uv[1] = in->texcoord.y;
						if (lightmap_vertices)
						{
							short const *uv = (short const *)(lightmap_vertices + vertex_index * 8 + 4);

							out->lightmap_uv[0] = uv[0];
							out->lightmap_uv[1] = uv[1];
						}
						out->weight = 1.0f;
					}
				}
				mesh->vertex_count += vertex_count;
				for (surface_index = 0; surface_index < material->surface_count; surface_index++)
				{
					struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&structure_bsp->surfaces,
						material->first_surface_index + surface_index, struct structure_surface);
					short corner;

					if (surface->vertex_indices[0] >= vertex_count || surface->vertex_indices[1] >= vertex_count ||
						surface->vertex_indices[2] >= vertex_count)
					{
						continue;
					}
					if (filled)
					{
						if (mesh->triangle_count >= triangle_room)
							return 0;
						for (corner = 0; corner < 3; corner++)
						{
							mesh->triangles[mesh->triangle_count][corner] =
								(unsigned long)(first_vertex + surface->vertex_indices[corner]);
						}
						mesh->triangle_materials[mesh->triangle_count] = (unsigned short)mesh->material_count;
					}
					mesh->triangle_count++;
				}
				mesh->material_count++;
			}
		}
		if (surfaces == WORLD_RT_SURFACES_OPAQUE)
			mesh->opaque_count = mesh->triangle_count;
	}
	return mesh->triangle_count;
}

/* a model part's vertices and triangles onto the mesh (or counted), its
material the mesh's next (FALSE: it cannot be read, the mesh not used) */
static int model_part_add(struct world_rt_source_mesh *mesh, struct model const *model,
	struct world_rt_model_part const *part, int surfaces, short node_count, int filled, long vertex_room,
	long triangle_room, long material_room)
{
	struct vertex_buffer const *vertex_buffer = &part->vertex_buffer;
	struct triangle_buffer const *triangle_buffer = &part->triangle_buffer;
	struct world_rt_model_shader_reference const *reference;
	struct shader const *shader;
	byte *vertices = NULL;
	BYTE *indices = NULL;
	long stride, index_count, index, first_vertex = mesh->vertex_count;
	int kind;
	float scale[2];

	if (TEST_FLAG(part->flags, _world_rt_model_part_stripped_bit) || !VALID_INDEX(part->shader_index, model->shaders.count))
		return TRUE;
	reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, part->shader_index, struct world_rt_model_shader_reference);
	shader = reference->shader.index != NONE && tag_index_is_group(reference->shader.index, SHADER_DEFINITION_TAG) ?
		shader_definition_get(reference->shader.index) : NULL;
	if (!shader || !shader_type_is_valid_for_model(shader->base.type))
		return TRUE;
	/* (its kind: a shader_model alpha-tested unless it says not, a
	transparent one drawn as a cutout alpha-tested, any other transparent
	one left out) */
	if (shader_cutout(shader, NULL))
		kind = WORLD_RT_SURFACES_ALPHA_TESTED;
	else if (shader_type_is_transparent(shader->base.type) || shader->base.type != _world_rt_shader_type_model)
		kind = WORLD_RT_SURFACES_TRANSPARENT;
	else
		kind = TEST_FLAG(((struct world_rt_shader_model const *)shader)->flags, _world_rt_shader_model_not_alpha_tested_bit) ?
			WORLD_RT_SURFACES_OPAQUE : WORLD_RT_SURFACES_ALPHA_TESTED;
	if (kind != surfaces)
		return TRUE;
	if (!vertex_buffer->hardware_format || vertex_buffer->count <= 0 || !triangle_buffer->hardware_format ||
		triangle_buffer->count <= 0)
		return TRUE;
	if (vertex_buffer->type == _rasterizer_vertex_type_model_compressed)
		stride = sizeof(struct model_vertex_compressed);
	else if (vertex_buffer->type == _rasterizer_vertex_type_model_uncompressed)
		stride = sizeof(struct model_vertex_uncompressed);
	else
		return TRUE;
	if (triangle_buffer->type == _triangle_buffer_type_precompiled_strip)
		index_count = triangle_buffer->count + 2;
	else if (triangle_buffer->type == _triangle_buffer_type_triangles)
		index_count = triangle_buffer->count * 3;
	else
		return TRUE;
	if (mesh->material_count >= WORLD_RT_MATERIALS)
		return TRUE;
	if (filled && (mesh->vertex_count + vertex_buffer->count > vertex_room || mesh->material_count >= material_room))
		return FALSE;
	IDirect3DVertexBuffer8_Lock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format, 0, 0, &vertices, D3DLOCK_READONLY);
	IDirect3DIndexBuffer8_Lock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format, 0, 0, &indices, D3DLOCK_READONLY);
	if (!vertices || !indices)
	{
		if (vertices)
			IDirect3DVertexBuffer8_Unlock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format);
		if (indices)
			IDirect3DIndexBuffer8_Unlock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format);
		return TRUE;
	}
	/* (a compressed vertex's coordinates are scaled by the model's base map
	scale as it is drawn: rasterizer_model_begin_parameters') */
	scale[0] = model->base_map_scale.i != 0.0f ? model->base_map_scale.i : 1.0f;
	scale[1] = model->base_map_scale.j != 0.0f ? model->base_map_scale.j : 1.0f;
	if (filled)
	{
		material_describe(reference->shader.index, &mesh->materials[mesh->material_count]);
		for (index = 0; index < vertex_buffer->count; index++)
		{
			struct world_rt_source_vertex *out = &mesh->vertices[first_vertex + index];
			long node0, node1;
			real weight;

			memset(out, 0, sizeof(*out));
			if (vertex_buffer->type == _rasterizer_vertex_type_model_compressed)
			{
				struct model_vertex_compressed const *in = (void const *)(vertices + index * stride);

				memcpy(out->position, &in->position, sizeof(out->position));
				unpacked(in->normal, out->normal);
				unpacked(in->tangent, out->tangent);
				unpacked(in->binormal, out->binormal);
				out->uv[0] = (float)in->texcoord.x / 32767.0f * scale[0];
				out->uv[1] = (float)in->texcoord.y / 32767.0f * scale[1];
				node0 = in->nodes[0] / 3;
				node1 = in->nodes[1] / 3;
				weight = (real)in->node_weight * (1.0f / 32767.0f);
			}
			else
			{
				struct model_vertex_uncompressed const *in = (void const *)(vertices + index * stride);

				memcpy(out->position, &in->position, sizeof(out->position));
				memcpy(out->normal, &in->normal, sizeof(out->normal));
				memcpy(out->tangent, &in->tangent, sizeof(out->tangent));
				memcpy(out->binormal, &in->binormal, sizeof(out->binormal));
				out->uv[0] = in->texcoord.x;
				out->uv[1] = in->texcoord.y;
				node0 = in->nodes[0];
				node1 = in->nodes[1];
				weight = in->node_weights[0];
			}
			/* (a part with its own node map, which this build's part doesn't
			carry: held to the node its middle is on, as object_mesh.c holds
			it) */
			if (TEST_FLAG(part->flags, _world_rt_model_part_local_nodes_bit))
			{
				node0 = node1 = VALID_INDEX(part->centroid_primary_node_index, node_count) ?
					part->centroid_primary_node_index : 0;
				weight = 1.0f;
			}
			if (!VALID_INDEX(node0, node_count))
				node0 = 0;
			if (!VALID_INDEX(node1, node_count))
			{
				node1 = node0;
				weight = 1.0f;
			}
			if (!valid_real(out->position[0]) || !valid_real(out->position[1]) || !valid_real(out->position[2]))
				out->position[0] = out->position[1] = out->position[2] = 0.0f;
			if (!valid_real(out->uv[0]) || !valid_real(out->uv[1]))
				out->uv[0] = out->uv[1] = 0.0f;
			out->nodes[0] = (short)node0;
			out->nodes[1] = (short)node1;
			out->weight = valid_real(weight) ? PIN(weight, 0.0f, 1.0f) : 1.0f;
		}
	}
	mesh->vertex_count += vertex_buffer->count;
	/* its triangles: a strip's every one (turning, its degenerate ones left
	out), or a list's */
	{
		unsigned short const *list = (unsigned short const *)indices;

		for (index = 0; index + 2 < index_count; index += triangle_buffer->type == _triangle_buffer_type_precompiled_strip ? 1 : 3)
		{
			unsigned short a = list[index], b = list[index + 1], c = list[index + 2];

			if (a == b || b == c || a == c || a >= vertex_buffer->count || b >= vertex_buffer->count ||
				c >= vertex_buffer->count)
				continue;
			/* (a strip's odd triangles turn the other way) */
			if (triangle_buffer->type == _triangle_buffer_type_precompiled_strip && (index & 1))
			{
				unsigned short swap = b;

				b = c;
				c = swap;
			}
			if (filled)
			{
				if (mesh->triangle_count >= triangle_room)
					break;
				mesh->triangles[mesh->triangle_count][0] = (unsigned long)(first_vertex + a);
				mesh->triangles[mesh->triangle_count][1] = (unsigned long)(first_vertex + b);
				mesh->triangles[mesh->triangle_count][2] = (unsigned long)(first_vertex + c);
				mesh->triangle_materials[mesh->triangle_count] = (unsigned short)mesh->material_count;
			}
			mesh->triangle_count++;
		}
	}
	IDirect3DVertexBuffer8_Unlock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format);
	IDirect3DIndexBuffer8_Unlock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format);
	mesh->material_count++;
	return TRUE;
}

long world_rt_source_model_mesh(long definition, struct world_rt_source_mesh *mesh, float (*inverses)[3][4])
{
	struct object_definition const *object_definition;
	struct model const *model;
	int filled = mesh->vertices && mesh->triangles && mesh->triangle_materials && mesh->materials;
	long vertex_room = mesh->vertex_count, triangle_room = mesh->triangle_count, material_room = mesh->material_count;
	short region_index, node_index, node_count;
	int surfaces;

	mesh->vertex_count = mesh->triangle_count = mesh->opaque_count = mesh->material_count = mesh->left_out = 0;
	if (definition == NONE)
		return 0;
	/* (a model itself: the first-person weapon's and hands') */
	if (definition_is_model(definition))
		model = model_definition_get(definition);
	else
	{
		object_definition = object_definition_get(definition);
		if (!object_definition || object_definition->object.model.index == NONE)
			return 0;
		model = model_definition_get(object_definition->object.model.index);
	}
	if (!model || !model->nodes.address || model->nodes.count <= 0 || !model->regions.address ||
		!model->geometries.address || model->geometries.count <= 0)
		return 0;
	node_count = (short)MIN(model->nodes.count, WORLD_RT_NODES);
	if (inverses)
	{
		for (node_index = 0; node_index < node_count; node_index++)
		{
			struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);
			real_matrix4x3 const *matrix = &node->runtime_default_inverse_matrix;
			int row;

			/* (its columns the forward, left and up axes, scaled, then its
			position: a model point into the node's space) */
			for (row = 0; row < 3; row++)
			{
				inverses[node_index][row][0] = matrix->scale * (&matrix->forward.i)[row];
				inverses[node_index][row][1] = matrix->scale * (&matrix->left.i)[row];
				inverses[node_index][row][2] = matrix->scale * (&matrix->up.i)[row];
				inverses[node_index][row][3] = (&matrix->position.x)[row];
			}
		}
	}
	for (surfaces = WORLD_RT_SURFACES_OPAQUE; surfaces <= WORLD_RT_SURFACES_TRANSPARENT; surfaces++)
	{
		long before = mesh->triangle_count;

		for (region_index = 0; region_index < MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT); region_index++)
		{
			struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
			struct model_region_permutation const *permutation;
			short geometry_index = NONE, level, step;

			if (region->permutations.count <= 0 || !region->permutations.address)
				continue;
			permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations, 0, struct model_region_permutation);
			/* (the rays' level, or the nearest there is: the less detailed first) */
			for (step = 0; step < 2 * _world_rt_model_detail_levels && !VALID_INDEX(geometry_index, model->geometries.count);
				step++)
			{
				level = (short)(_world_rt_model_detail_rays + (step & 1 ? (step + 1) / 2 : -(step / 2)));
				if (level >= 0 && level < _world_rt_model_detail_levels)
					geometry_index = permutation->geometry_indices[level];
			}
			if (!VALID_INDEX(geometry_index, model->geometries.count))
				continue;
			{
				struct world_rt_model_geometry const *geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index,
					struct world_rt_model_geometry);
				long part_index;

				if (geometry->parts.count <= 0 || geometry->parts.count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY ||
					!geometry->parts.address)
					continue;
				for (part_index = 0; part_index < geometry->parts.count; part_index++)
				{
					struct world_rt_model_part const *part = TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index,
						struct world_rt_model_part);

					/* (the transparent ones counted, left out) */
					if (surfaces == WORLD_RT_SURFACES_TRANSPARENT)
					{
						struct world_rt_source_mesh counted;

						memset(&counted, 0, sizeof(counted));
						model_part_add(&counted, model, part, surfaces, node_count, FALSE, 0, 0, 0);
						mesh->left_out += counted.triangle_count;
						continue;
					}
					if (!model_part_add(mesh, model, part, surfaces, node_count, filled, vertex_room, triangle_room,
						material_room))
						return 0;
				}
			}
		}
		if (surfaces == WORLD_RT_SURFACES_OPAQUE)
			mesh->opaque_count = mesh->triangle_count;
		(void)before;
	}
	return mesh->triangle_count;
}

void const *world_rt_source_texture(struct world_rt_source_map const *map)
{
	struct bitmap_data *bitmap;

	if (map->bitmap == NONE || map->index < 0 || !tag_index_is_group(map->bitmap, BITMAP_GROUP_TAG))
		return NULL;
	bitmap = bitmap_group_try_and_get_bitmap(map->bitmap, map->index);
	/* (a 2D one alone: the rays' alpha maps and the rest are) */
	if (!bitmap || bitmap->type != _world_rt_bitmap_type_2d)
		return NULL;
	return _texture_cache_bitmap_get_hardware_format(bitmap, FALSE, TRUE);
}

short world_rt_source_definition(long definition, float sphere[4], int *item)
{
	struct object_definition const *object_definition;
	struct model const *model;

	sphere[0] = sphere[1] = sphere[2] = sphere[3] = 0.0f;
	*item = FALSE;
	if (definition == NONE)
		return 0;
	if (definition_is_model(definition))
	{
		model = model_definition_get(definition);
		return model && model->nodes.address ? (short)MIN(model->nodes.count, WORLD_RT_NODES) : 0;
	}
	object_definition = object_definition_get(definition);
	if (!object_definition || object_definition->object.model.index == NONE)
		return 0;
	model = model_definition_get(object_definition->object.model.index);
	if (!model || !model->nodes.address || model->nodes.count <= 0)
		return 0;
	sphere[0] = object_definition->object.bounding_offset.x;
	sphere[1] = object_definition->object.bounding_offset.y;
	sphere[2] = object_definition->object.bounding_offset.z;
	sphere[3] = object_definition->object.bounding_radius;
	*item = object_definition->object.type == _object_type_weapon || object_definition->object.type == _object_type_equipment ||
		object_definition->object.type == _object_type_garbage;
	return (short)MIN(model->nodes.count, WORLD_RT_NODES);
}

/* the first-person weapon's and hands' models as last drawn: each local
player's, its models and their nodes (world space), the unit holding them,
the frame drawn in */
static struct
{
	long model;
	long unit;
	unsigned long frame;
	short node_count;
	struct world_proxies_frame frames[WORLD_RT_NODES];
} first_person_drawn[MAXIMUM_LOCAL_PLAYERS][2];
static unsigned long first_person_frame;

void world_rt_source_first_person_drawn(short local_player_index, int which, long model_index, long unit_index,
	real_matrix4x3 const *nodes)
{
	struct model const *model;
	short node_index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || which < 0 || which > 1 ||
		model_index == NONE || !nodes)
		return;
	model = model_definition_get(model_index);
	if (!model || !model->nodes.address)
		return;
	first_person_drawn[local_player_index][which].model = model_index;
	first_person_drawn[local_player_index][which].unit = unit_index;
	first_person_drawn[local_player_index][which].frame = first_person_frame;
	first_person_drawn[local_player_index][which].node_count = (short)MIN(model->nodes.count, WORLD_RT_NODES);
	for (node_index = 0; node_index < first_person_drawn[local_player_index][which].node_count; node_index++)
	{
		struct world_proxies_frame *frame = &first_person_drawn[local_player_index][which].frames[node_index];
		real_matrix4x3 const *matrix = &nodes[node_index];
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			frame->axes[0][axis] = matrix->scale * (&matrix->forward.i)[axis];
			frame->axes[1][axis] = matrix->scale * (&matrix->left.i)[axis];
			frame->axes[2][axis] = matrix->scale * (&matrix->up.i)[axis];
			frame->position[axis] = (&matrix->position.x)[axis];
		}
	}
}

short world_rt_source_first_person(int which, long *model, long *unit, struct world_proxies_frame *frames, short maximum)
{
	short count;

	/* (the first local player's, drawn since world_rt_source_first_person_next) */
	if (which < 0 || which > 1 || first_person_drawn[0][which].frame != first_person_frame ||
		!first_person_drawn[0][which].node_count)
		return 0;
	count = MIN(first_person_drawn[0][which].node_count, maximum);
	*model = first_person_drawn[0][which].model;
	*unit = first_person_drawn[0][which].unit;
	memcpy(frames, first_person_drawn[0][which].frames, sizeof(*frames) * (size_t)count);
	return count;
}

void world_rt_source_first_person_next(void)
{
	first_person_frame++;
}

short world_rt_source_node_permutations(long definition, short node_index)
{
	struct collision_model const *collision_model = definition_collision_model(definition);
	struct collision_node const *collision_node;

	if (!collision_model || !collision_model->nodes.address || node_index < 0 ||
		node_index >= collision_model->nodes.count)
	{
		return 0;
	}
	collision_node = TAG_BLOCK_GET_ELEMENT(&collision_model->nodes, node_index, struct collision_node);
	return collision_node->bsps.address ? (short)collision_node->bsps.count : 0;
}

long world_rt_source_node(long definition, short node_index, short permutation, float *triangles, long maximum)
{
	struct collision_bsp const *bsp = node_collision_bsp(definition_collision_model(definition), node_index,
		permutation);
	long count = bsp ? world_scene_collision_fan(bsp, NULL) : 0;

	if (!triangles || count <= 0)
		return count;
	/* (fanned whole, then: the caller's room is the count it was told) */
	if (count > maximum)
		return 0;
	return world_scene_collision_fan(bsp, triangles);
}

char const *world_rt_source_name(long definition)
{
	return definition == NONE ? "none" : tag_get_name(definition);
}
