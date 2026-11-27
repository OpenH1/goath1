/*
WORLD_RT_GAME.C

What display.ambient_occlusion "ray_traced"'s scene (world_rt.c) reads of
the game (world_rt.h's world_rt_source_*): the loaded structure BSP's
collision geometry, as world_scene.c fans it, and a definition's nodes'
collision geometry (the first permutation's BSP of each collision node, the
model's node of the same index, as the collision tests take them:
collision_models.c) and its bounding sphere. With debug.rt_render_geometry,
the structure's render triangles too (docs/rt-plan/1-geometry.md, stage 1):
its lightmaps' materials' surfaces over their compressed environment
vertices, sorted by the material's shader into opaque, alpha-tested and
transparent ones. Read only; render-only.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "models/model_definitions.h"
#include "objects/object_definitions.h"
#include "objects/object_types.h"
#include "physics/collision_bsp_definitions.h"
#include "physics/collision_model_definitions.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "cache/texture_cache.h"

#include "world_rt.h"
#include "world_scene.h"

#include <stddef.h>
#include <string.h>

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
	else
		material->flags |= WORLD_RT_MATERIAL_TRANSPARENT;
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
		return WORLD_RT_SURFACES_TRANSPARENT;
	return TEST_FLAG(((struct world_rt_shader_environment const *)shader)->flags,
		_world_rt_shader_environment_alpha_tested_bit) ? WORLD_RT_SURFACES_ALPHA_TESTED : WORLD_RT_SURFACES_OPAQUE;
}

/* a definition's collision model (NULL none) */
static struct collision_model const *definition_collision_model(long definition)
{
	struct object_definition const *object_definition;

	if (definition == NONE)
		return NULL;
	object_definition = object_definition_get(definition);
	if (!object_definition || object_definition->object.collision_model.index == NONE)
		return NULL;
	return collision_model_definition_get(object_definition->object.collision_model.index);
}

/* a collision model's node's first BSP (NULL none) */
static struct collision_bsp const *node_collision_bsp(struct collision_model const *collision_model, short node_index)
{
	struct collision_node const *collision_node;

	if (!collision_model || !collision_model->nodes.address || node_index < 0 ||
		node_index >= collision_model->nodes.count)
	{
		return NULL;
	}
	collision_node = TAG_BLOCK_GET_ELEMENT(&collision_model->nodes, node_index, struct collision_node);
	if (!collision_node->bsps.address || collision_node->bsps.count <= 0)
		return NULL;
	return TAG_BLOCK_GET_ELEMENT(&collision_node->bsps, 0, struct collision_bsp);
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

long world_rt_source_node(long definition, short node_index, float *triangles, long maximum)
{
	struct collision_bsp const *bsp = node_collision_bsp(definition_collision_model(definition), node_index);
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
