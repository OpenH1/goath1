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

#include "world_rt.h"
#include "world_scene.h"

#include <string.h>

/* (cache_files.c) */
boolean tag_index_is_group(long tag_index, long group_tag);

/* ---------- constants */

enum
{
	/* a shader's type (shaders.c's _shader_type_*) */
	_world_rt_shader_type_environment = 3,
	/* shader_environment's flags (rasterizer_xbox_environment.c's
	_shader_environment_alpha_tested_bit) */
	_world_rt_shader_environment_alpha_tested_bit = 0,
};

/* ---------- structures */

/* (shaders.c's shader_environment_definition: its flags alone) */
struct world_rt_shader_environment
{
	struct shader shader;
	word flags;
};

/* ---------- private code */

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

long world_rt_source_structure_render(short bsp_index, int surfaces, float *triangles, long maximum)
{
	struct structure_bsp const *structure_bsp;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_index, count = 0;

	/* (the loaded BSP alone, as the collision's: world_rt_source_bsp) */
	if (bsp_index < 0 || bsp_index != world_rt_source_bsp())
		return 0;
	structure_bsp = global_structure_bsp_get();
	if (!structure_bsp || !structure_bsp->lightmaps.address || !structure_bsp->surfaces.address)
		return 0;
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
			long surface_index;

			/* (its compressed vertices there to read and its surfaces among
			the BSP's: mod_lightmaps.c's material_readable) */
			if (material_surfaces(material) != surfaces || !vertices || material->vertices.count <= 0 ||
				material->compressed_vertex_data.size < material->vertices.count * vertex_size ||
				material->surface_count <= 0 || material->first_surface_index < 0 ||
				material->first_surface_index > structure_bsp->surfaces.count - material->surface_count)
			{
				continue;
			}
			for (surface_index = 0; surface_index < material->surface_count; surface_index++)
			{
				struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&structure_bsp->surfaces,
					material->first_surface_index + surface_index, struct structure_surface);
				short corner;

				if (surface->vertex_indices[0] >= material->vertices.count ||
					surface->vertex_indices[1] >= material->vertices.count ||
					surface->vertex_indices[2] >= material->vertices.count)
				{
					continue;
				}
				if (triangles)
				{
					if (count >= maximum)
						return count;
					/* (a compressed environment vertex's position first:
					structure_culling.c reads it so) */
					for (corner = 0; corner < 3; corner++)
					{
						memcpy(triangles + count * 9 + corner * 3,
							vertices + (long)surface->vertex_indices[corner] * vertex_size, 3 * sizeof(float));
					}
				}
				count++;
			}
		}
	}
	return count;
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
