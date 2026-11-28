/*
LT_EMITTERS.C

The level's static light sources for the light-transport pass (lt_lights.h's
lt_emitters_*; ~/dev/rt2-realtime-design.md section 11): its glowing
surfaces as area lights, each a group of triangles of one radiance, which the
pass samples by power and traces live (lt/e1.frag's emitter_sample), so the
lightmap is no longer the static light where they are known.

Where they come from, for the loaded BSP:
- solved (port/assets/emitters/<map>_bsp<k>.json, built in by
  tools/lt_emitters_data.py as lt_emitters_data.inc): the indoor emitters'
  solve from the BSP's own lightmap, the level's lamp shaders at the tag's
  colour x relative power, one brightness a lamp group (a shader's surfaces in
  a 12 wu cell), set so each cell's traced light matches the bake's. A BSP
  that has them is lit live (lt_lights_real_sky).
- else the tags: every structure surface whose shader has radiosity power
  (the lightmap tool's emitter), its radiance the shader's colour of emitted
  light x power x LT_EMITTERS_TAG_SCALE, grouped the same way. These light a
  level the sky already lights live (an outdoor level's interiors); they do
  not by themselves turn a level live.

They reach the backend in the light list (xgpu_stream.h's LT_LIGHTS) as
records of kind XGPU_LT_LIGHT_EMITTER, LT_EMITTERS_A_FRAME of them each
frame in turn, so a replay begun at any frame has them all within a few
frames; the backend gathers them by the set's generation
(xgpu_vulkan_lt_lights.c) and builds its sampling tables once a set.

HALO_LT_EMITTERS: 0 none (checking); "tags" the tags' alone, even where a
solved file is built in.
*/

#include "cseries.h"
#include "real_math.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "structures/structures.h"
#include "structures/structure_bsp_definitions.h"
#include "shaders/shader_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"

#include "halo_dynamic_lights.h"
#include "lt_lights.h"
#include "world_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void platform_log(const char *format, ...);
boolean tag_index_is_group(long tag_index, long group_tag);

/* the most triangles and groups a BSP's set holds (the backend's light
table has room for them: xgpu_vulkan_lt.c's LT_EMITTER_*_MAXIMUM) */
#define LT_EMITTERS_TRIANGLES_MAXIMUM 8192
#define LT_EMITTERS_GROUPS_MAXIMUM 1024
/* records a frame (of the list's XGPU_STREAM_LT_LIGHTS_MAXIMUM) */
#define LT_EMITTERS_A_FRAME 192
/* the tags' radiance: colour x power x this, the indoor solve's level scale
(a10's lamps 0.334, c20's 0.384: port/assets/emitters/*.json's fit) */
#define LT_EMITTERS_TAG_SCALE 0.36f
/* the tags' groups' cell (world units) and how far a triangle is lifted off
its surface toward its normal (the solve's) */
#define LT_EMITTERS_CELL 12.0f
#define LT_EMITTERS_LIFT 0.01f

struct lt_emitters_level
{
	char const *scenario;
	short bsp_index;
	int first_group, group_count;
};

struct lt_emitters_group
{
	float radiance[3];
	int first_triangle, triangle_count;
};

#include "lt_emitters_data.inc"

static struct
{
	/* the BSP the set is for (NULL none yet) */
	void const *bsp;
	/* solved (1) or the tags' (0); its generation (nonzero, 24 bits) */
	int solved;
	unsigned int generation;
	int triangle_count, group_count, next;
	float (*triangles)[9];
	int *group_of;
	float (*radiance)[3];
} lt_emitters;

/* HALO_LT_EMITTERS: 1 (unset) all, 0 none, 2 the tags' alone */
static int lt_emitters_setting(void)
{
	static int setting = -1;

	if (setting < 0)
	{
		char const *given = getenv("HALO_LT_EMITTERS");

		setting = !given || !*given ? 1 : !strcmp(given, "tags") ? 2 : strcmp(given, "0") && strcmp(given, "off");
	}
	return setting;
}

static void lt_emitters_clear(void)
{
	free(lt_emitters.triangles);
	free(lt_emitters.group_of);
	free(lt_emitters.radiance);
	lt_emitters.triangles = NULL;
	lt_emitters.group_of = NULL;
	lt_emitters.radiance = NULL;
	lt_emitters.triangle_count = lt_emitters.group_count = lt_emitters.next = 0;
	lt_emitters.solved = 0;
}

static int lt_emitters_room(void)
{
	lt_emitters.triangles = malloc(sizeof(*lt_emitters.triangles) * LT_EMITTERS_TRIANGLES_MAXIMUM);
	lt_emitters.group_of = malloc(sizeof(*lt_emitters.group_of) * LT_EMITTERS_TRIANGLES_MAXIMUM);
	lt_emitters.radiance = malloc(sizeof(*lt_emitters.radiance) * LT_EMITTERS_GROUPS_MAXIMUM);
	return lt_emitters.triangles && lt_emitters.group_of && lt_emitters.radiance;
}

/* the built-in solve for the scenario's BSP (FALSE none) */
static int lt_emitters_solved(char const *scenario, short bsp_index)
{
	int level, group, triangle;

	for (level = 0; scenario && lt_emitters_levels[level].scenario; level++)
	{
		struct lt_emitters_level const *found = &lt_emitters_levels[level];

		if (found->bsp_index != bsp_index || csstrcasecmp(found->scenario, scenario))
			continue;
		for (group = found->first_group; group < found->first_group + found->group_count; group++)
		{
			struct lt_emitters_group const *source = &lt_emitters_groups[group];

			if (lt_emitters.group_count >= LT_EMITTERS_GROUPS_MAXIMUM ||
				lt_emitters.triangle_count + source->triangle_count > LT_EMITTERS_TRIANGLES_MAXIMUM)
				break;
			memcpy(lt_emitters.radiance[lt_emitters.group_count], source->radiance, sizeof(source->radiance));
			for (triangle = 0; triangle < source->triangle_count; triangle++)
			{
				memcpy(lt_emitters.triangles[lt_emitters.triangle_count],
					lt_emitters_triangles[source->first_triangle + triangle], sizeof(float) * 9);
				lt_emitters.group_of[lt_emitters.triangle_count++] = lt_emitters.group_count;
			}
			lt_emitters.group_count++;
		}
		return lt_emitters.group_count > 0;
	}
	return FALSE;
}

/* a tags' triangle: its material and cell, for grouping */
struct lt_emitters_found
{
	long material, cell;
	float corners[9];
};

static int lt_emitters_found_order(void const *a, void const *b)
{
	struct lt_emitters_found const *x = a, *y = b;

	if (x->material != y->material)
		return x->material < y->material ? -1 : 1;
	return x->cell < y->cell ? -1 : x->cell > y->cell;
}

static long lt_emitters_cell_of(float const *corners)
{
	long key = 0;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float middle = (corners[axis] + corners[3 + axis] + corners[6 + axis]) / 3.0f;
		long cell = (long)floorf(middle / LT_EMITTERS_CELL) + 512;

		key = key * 1024 + (cell < 0 ? 0 : cell > 1023 ? 1023 : cell);
	}
	return key;
}

/* the tags' emitters: the structure's surfaces whose shader has radiosity
power, lifted off their surface toward their normal, grouped by shader and
cell */
static void lt_emitters_tags(short bsp_index)
{
	struct world_rt_source_mesh mesh;
	struct lt_emitters_found *found = NULL;
	float (*radiance)[3] = NULL;
	long triangle, count = 0, index;

	memset(&mesh, 0, sizeof(mesh));
	if (world_rt_source_structure_mesh(bsp_index, &mesh) <= 0 || mesh.material_count <= 0)
		return;
	mesh.vertices = malloc(sizeof(*mesh.vertices) * (size_t)mesh.vertex_count);
	mesh.triangles = malloc(sizeof(*mesh.triangles) * (size_t)mesh.triangle_count);
	mesh.triangle_materials = malloc(sizeof(*mesh.triangle_materials) * (size_t)mesh.triangle_count);
	mesh.materials = malloc(sizeof(*mesh.materials) * (size_t)mesh.material_count);
	radiance = malloc(sizeof(*radiance) * (size_t)mesh.material_count);
	found = malloc(sizeof(*found) * LT_EMITTERS_TRIANGLES_MAXIMUM);
	if (!mesh.vertices || !mesh.triangles || !mesh.triangle_materials || !mesh.materials || !radiance || !found ||
		world_rt_source_structure_mesh(bsp_index, &mesh) <= 0)
	{
		goto done;
	}
	for (index = 0; index < mesh.material_count; index++)
	{
		long shader_index = mesh.materials[index].shader;
		struct shader const *shader = shader_index != NONE && tag_index_is_group(shader_index, SHADER_DEFINITION_TAG) ?
			shader_definition_get(shader_index) : NULL;
		struct shader_radiosity_properties const *radiosity = shader ? &shader->base.radiosity : NULL;
		float power = radiosity && radiosity->power > 0.0f && radiosity->power < 1.0e6f ? radiosity->power : 0.0f;

		radiance[index][0] = radiosity ? radiosity->color_of_emitted_light.red * power * LT_EMITTERS_TAG_SCALE : 0.0f;
		radiance[index][1] = radiosity ? radiosity->color_of_emitted_light.green * power * LT_EMITTERS_TAG_SCALE : 0.0f;
		radiance[index][2] = radiosity ? radiosity->color_of_emitted_light.blue * power * LT_EMITTERS_TAG_SCALE : 0.0f;
	}
	for (triangle = 0; triangle < mesh.triangle_count && count < LT_EMITTERS_TRIANGLES_MAXIMUM; triangle++)
	{
		long material = mesh.triangle_materials[triangle];
		float *corners = found[count].corners, e1[3], e2[3], normal[3], length;
		int corner, axis;

		if (radiance[material][0] + radiance[material][1] + radiance[material][2] <= 0.0f)
			continue;
		for (corner = 0; corner < 3; corner++)
			memcpy(corners + 3 * corner, mesh.vertices[mesh.triangles[triangle][corner]].position, sizeof(float) * 3);
		for (axis = 0; axis < 3; axis++)
		{
			e1[axis] = corners[3 + axis] - corners[axis];
			e2[axis] = corners[6 + axis] - corners[axis];
		}
		normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
		normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
		normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
		length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		if (!(length > 1.0e-8f))
			continue;
		/* (the winding's normal toward the vertices' own: the side it glows) */
		{
			float const *vertex_normal = mesh.vertices[mesh.triangles[triangle][0]].normal;

			if (normal[0] * vertex_normal[0] + normal[1] * vertex_normal[1] + normal[2] * vertex_normal[2] < 0.0f)
			{
				float swap[3];

				memcpy(swap, corners + 3, sizeof(swap));
				memcpy(corners + 3, corners + 6, sizeof(swap));
				memcpy(corners + 6, swap, sizeof(swap));
				length = -length;
			}
		}
		for (corner = 0; corner < 3; corner++)
			for (axis = 0; axis < 3; axis++)
				corners[3 * corner + axis] += normal[axis] / length * LT_EMITTERS_LIFT;
		found[count].material = material;
		found[count].cell = lt_emitters_cell_of(corners);
		count++;
	}
	qsort(found, (size_t)count, sizeof(*found), lt_emitters_found_order);
	for (index = 0; index < count && lt_emitters.group_count <= LT_EMITTERS_GROUPS_MAXIMUM; index++)
	{
		if (!index || found[index].material != found[index - 1].material || found[index].cell != found[index - 1].cell)
		{
			if (lt_emitters.group_count == LT_EMITTERS_GROUPS_MAXIMUM)
				break;
			memcpy(lt_emitters.radiance[lt_emitters.group_count++], radiance[found[index].material], sizeof(float) * 3);
		}
		memcpy(lt_emitters.triangles[lt_emitters.triangle_count], found[index].corners, sizeof(float) * 9);
		lt_emitters.group_of[lt_emitters.triangle_count++] = lt_emitters.group_count - 1;
	}
done:
	free(found);
	free(radiance);
	free(mesh.vertices);
	free(mesh.triangles);
	free(mesh.triangle_materials);
	free(mesh.materials);
}

/* the loaded BSP's set, made when the BSP changes */
static void lt_emitters_update(void)
{
	void const *bsp = global_structure_bsp_get();
	char const *scenario;
	unsigned int hash = 2166136261u;
	int setting = lt_emitters_setting();

	if (bsp == lt_emitters.bsp)
		return;
	lt_emitters_clear();
	lt_emitters.bsp = bsp;
	if (!bsp || !setting || !global_scenario_try_and_get() || !lt_emitters_room())
		return;
	scenario = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	if (setting == 1)
		lt_emitters.solved = lt_emitters_solved(scenario, global_structure_bsp_index);
	if (!lt_emitters.solved)
		lt_emitters_tags(global_structure_bsp_index);
	/* (the set's generation: its source, scenario, BSP and size) */
	for (; scenario && *scenario; scenario++)
		hash = (hash ^ (unsigned char)*scenario) * 16777619u;
	hash = (hash ^ (unsigned int)global_structure_bsp_index) * 16777619u;
	hash = (hash ^ (unsigned int)lt_emitters.triangle_count) * 16777619u;
	hash = (hash ^ (unsigned int)lt_emitters.solved) * 16777619u;
	lt_emitters.generation = (hash & 0xffffffu) | (hash & 0xffffffu ? 0u : 1u);
	platform_log("lt emitters: %d triangles in %d groups, %s", lt_emitters.triangle_count, lt_emitters.group_count,
		lt_emitters.solved ? "solved from the lightmap (the level lit live)" : "the tags' radiosity shaders");
}

/* ---------- public code */

int lt_emitters_live(void)
{
	lt_emitters_update();
	return lt_emitters.solved && lt_emitters.triangle_count > 0;
}

int lt_emitters_append(struct xgpu_stream_lt_light *lights, int count, int maximum)
{
	int sent;

	lt_emitters_update();
	for (sent = 0; sent < LT_EMITTERS_A_FRAME && sent < lt_emitters.triangle_count && count < maximum; sent++)
	{
		struct xgpu_stream_lt_light *out = &lights[count++];
		int index = lt_emitters.next;
		float const *corners = lt_emitters.triangles[index];

		lt_emitters.next = (index + 1) % lt_emitters.triangle_count;
		memset(out, 0, sizeof(*out));
		out->id[0] = (unsigned int)index;
		out->id[1] = (unsigned int)XGPU_LT_LIGHT_SOURCE_EMITTER << 24 | lt_emitters.generation;
		out->kind = XGPU_LT_LIGHT_EMITTER;
		out->flags = lt_emitters.solved ? XGPU_LT_LIGHT_EMITTER_SOLVED : 0;
		out->object = (unsigned int)lt_emitters.triangle_count;
		out->owner = (unsigned int)lt_emitters.group_count;
		out->gel_slot = lt_emitters.group_of[index];
		out->gel_tag = -1;
		memcpy(out->position, corners, sizeof(float) * 3);
		memcpy(out->forward, corners + 3, sizeof(float) * 3);
		memcpy(out->up, corners + 6, sizeof(float) * 3);
		memcpy(out->power, lt_emitters.radiance[lt_emitters.group_of[index]], sizeof(float) * 3);
		out->cone_inner = out->cone_outer = -1.0f;
		out->power_scale = 1.0f;
	}
	return count;
}
