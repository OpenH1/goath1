/*
TRACE_CHECK.CPP

The Metal emitter's ray step through the distance-field scene
(port/linux/src/xgpu_msl.c's world_trace: its very text, in
world_trace_msl.inc, which tools/test_world_trace.py writes from the
emitter's) run on the CPU (tools/world_ao_light/metal_shim.h) and compared
with its reference, port/linux/game/world_cascades.c's world_trace_ray, at
the made-up scene's rays (scene.c), the structure read from its field
(world_sdf_sample; the shader, its atlas and table): whether each hits,
what, and how far.

  trace_check <count> <tolerance>   prints the largest difference of t;
                                    exits 1 if a ray differs past it
*/

#include "metal_shim.h"

extern "C"
{
#include "world_cascades.h"
#include "world_sdf.h"

void scene_make(struct world_cascades *cascades, float block[WORLD_CASCADES][4]);
void scene_ray(long index, float origin[3], float direction[3], float *t_max, int *mask);
float scene_structure(void *context, float const point[3]);
extern struct world_sdf_volume scene_volume;
}

#include <cstdio>
#include <cstdlib>

/* (in a namespace of its own: its struct world_trace_hit is the C one's
namesake) */
namespace msl
{
#include "world_trace_msl.inc"
}

int main(int argc, char **argv)
{
	static struct world_cascades cascades;
	float block[WORLD_CASCADES][4], worst = 0.0f;
	long count = argc > 2 ? atol(argv[1]) : 0, index, hits = 0, unlike = 0;
	float tolerance = argc > 2 ? (float)atof(argv[2]) : 0.0f;
	float4 vectors[WORLD_CASCADES], field0, field1;
	texture3d<float> texture, atlas_texture, table_texture;
	static signed char atlas[8 * WORLD_SDF_ATLAS_LAYER_BYTES];
	unsigned long layers;

	if (count <= 0)
	{
		fprintf(stderr, "trace_check <count> <tolerance>\n");
		return 2;
	}
	scene_make(&cascades, block);
	for (int cascade = 0; cascade < WORLD_CASCADES; cascade++)
		vectors[cascade] = float4(block[cascade][0], block[cascade][1], block[cascade][2], block[cascade][3]);
	texture.voxels = nullptr;
	texture.texels = nullptr;
	texture.values = nullptr;
	texture.pairs = cascades.texels;
	texture.width = texture.height = WORLD_CASCADE_SPAN;
	texture.depth = WORLD_CASCADE_SPAN * WORLD_CASCADES;
	/* (the level's field as the GPU has it: its atlas's layers, its table,
	its block's two vectors) */
	layers = world_sdf_atlas_layers(&scene_volume.header);
	if (layers > 8)
		return 2;
	for (index = 0; index < (long)layers; index++)
		world_sdf_atlas_layer(&scene_volume, (unsigned long)index, atlas + index * WORLD_SDF_ATLAS_LAYER_BYTES);
	atlas_texture.voxels = atlas;
	atlas_texture.width = atlas_texture.height = WORLD_SDF_ATLAS * WORLD_SDF_BRICK_SPAN;
	atlas_texture.depth = (int)layers * WORLD_SDF_BRICK_SPAN;
	table_texture.texels = &scene_volume.cells[0][0];
	table_texture.width = (int)scene_volume.header.width;
	table_texture.height = (int)scene_volume.header.height;
	table_texture.depth = (int)scene_volume.header.depth;
	field0 = float4(scene_volume.header.origin[0], scene_volume.header.origin[1], scene_volume.header.origin[2],
		1.0f / scene_volume.header.voxel);
	field1 = float4((float)scene_volume.header.width, (float)scene_volume.header.height, (float)scene_volume.header.depth,
		(float)layers);
	for (index = 0; index < count; index++)
	{
		struct world_trace_hit reference;
		float origin[3], direction[3], t_max, difference;
		int mask;

		scene_ray(index, origin, direction, &t_max, &mask);
		world_trace_ray(&cascades, block, scene_structure, &scene_volume, origin, direction, t_max, mask, &reference);
		msl::world_trace_hit hit = msl::world_trace(texture, vectors, atlas_texture, table_texture, field0, field1,
			float3(origin[0], origin[1], origin[2]), float3(direction[0], direction[1], direction[2]), t_max, mask);
		difference = std::fabs(hit.t - reference.t);
		if ((int)hit.hit != reference.hit || hit.kind != reference.kind || difference > tolerance)
		{
			if (unlike++ < 10)
				printf("ray %ld: %d %d %g, the reference's %d %d %g\n", index, (int)hit.hit, hit.kind, hit.t, reference.hit,
					reference.kind, reference.t);
		}
		worst = difference > worst ? difference : worst;
		hits += reference.hit;
	}
	printf("%ld rays (%ld hit), %ld differ, the largest difference of t %.3g\n", count, hits, unlike, worst);
	return unlike ? 1 : 0;
}
