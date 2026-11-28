/*
LT_EMITTERS.C

The level's static light sources for the light-transport pass (lt_lights.h's
lt_emitters_*; ~/dev/rt2-realtime-design.md section 11): its glowing
surfaces as area lights, each a group of triangles of one radiance, which the
pass samples by power and traces live (lt/e1.frag's emitter_sample), so the
lightmap is no longer the static light where they are known.

Where they come from, for the loaded BSP: ONE rule on every map (campaign
and multiplayer), from each shader's own data, no per-level number. Every
structure surface (opaque, alpha-tested or transparent: Damnation's light
panels are transparent cards) whose shader has radiosity power (the
lightmap tool's emitter, the designers' light for the bake) glows with
radiance power x colour of emitted light / pi (LT_EMITTERS_EXITANCE_TO_
RADIANCE), toward its normal, a transparent shader's card both ways; the
strongest LT_EMITTERS_TRIANGLES_MAXIMUM by power when there are more,
grouped by shader and 12 wu cell. A BSP that has them is lit live
(lt_lights_real_sky): its lightmap dropped in every cluster.

The per-BSP solves (port/assets/emitters/<map>_bsp<k>.json, packed by
tools/lt_emitters_data.py into openh1.assets, lt_emitters_assets.h) are the
check only (HALO_LT_EMITTERS=solved): lamps fitted to each BSP's bake.

They reach the backend in the light list (xgpu_stream.h's LT_LIGHTS) as
records of kind XGPU_LT_LIGHT_EMITTER, LT_EMITTERS_A_FRAME of them each
frame in turn, so a replay begun at any frame has them all within a few
frames; the backend gathers them by the set's generation
(xgpu_vulkan_lt_lights.c) and builds its sampling tables once a set.

HALO_LT_EMITTERS: 0 none (checking); "solved" the per-BSP solve where one is
built in (checking against the rule).
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
#include "lt_emitters_assets.h"
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
/* the one emission rule's constant: a shader's radiosity power x colour of
emitted light is the lightmap tool's emitted exitance M (light leaving a
unit of area), and a Lambertian emitter of exitance M has radiance M / pi,
in the pass's units (a white Lambertian surface lit by radiance L over its
hemisphere shows L). No fit. Checked against the per-BSP solves
(port/assets/emitters/*.json, 2429 lamp groups' solved radiance over power
x colour): median 0.27, power-weighted 0.43, each BSP's 0.15 to 0.6 */
#define LT_EMITTERS_EXITANCE_TO_RADIANCE 0.31830989f
/* the shared emission model's class hook (Erin, 2026-11-28): ENERGY glows
(additive and plasma-style transparent shaders: plasma, shields, energy
fields) take one class factor on both their drawn value and their cast
light, so they glow like hot plasma; rt2-glow-power sets its value (1: the
physical rule, as for every other glowing surface, until then). World
lamps and panels stay physical (factor 1) */
#define LT_EMITTERS_ENERGY_FACTOR 1.0f
#define LT_EMITTERS_SHADER_TRANSPARENT_PLASMA 10
/* (shader_definitions.h's environment type: glows toward its normal; any
other, a transparent card, both ways) */
#define LT_EMITTERS_SHADER_ENVIRONMENT 3
/* the tags' groups' cell (world units) and how far a triangle is lifted off
its surface toward its normal (the solve's) */
#define LT_EMITTERS_CELL 12.0f
#define LT_EMITTERS_LIFT 0.01f


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

/* HALO_LT_EMITTERS: 1 (unset) the rule's, 0 none, 2 ("tags") the rule's too,
3 ("solved") a built-in per-BSP solve where there is one (the check), else
the rule's */
static int lt_emitters_setting(void)
{
	static int setting = -1;

	if (setting < 0)
	{
		char const *given = getenv("HALO_LT_EMITTERS");

		setting = !given || !*given ? 1 : !strcmp(given, "tags") ? 2 : !strcmp(given, "solved") ? 3 :
			strcmp(given, "0") && strcmp(given, "off");
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

static unsigned int lt_emitters_word(unsigned char const *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8 | (unsigned int)bytes[2] << 16 |
		(unsigned int)bytes[3] << 24;
}

static float lt_emitters_float(unsigned char const *bytes)
{
	unsigned int word = lt_emitters_word(bytes);
	float value;

	memcpy(&value, &word, sizeof(value));
	return value;
}

/* the built-in solve for the scenario's BSP (FALSE none): its pack
(tools/lt_emitters_data.py's form) unpacked from openh1.assets */
static int lt_emitters_solved(char const *scenario, short bsp_index)
{
	unsigned int size = 0, group_count, triangle_count, group, triangle, first, count, value;
	unsigned char *pack;
	unsigned char const *groups, *triangles;

	size = lt_emitters_asset_size(scenario, bsp_index);
	pack = size ? malloc(size) : NULL;
	if (!pack || !lt_emitters_asset_read(scenario, bsp_index, pack, size))
	{
		free(pack);
		return FALSE;
	}
	group_count = size >= 8 ? lt_emitters_word(pack) : 0;
	triangle_count = size >= 8 ? lt_emitters_word(pack + 4) : 0;
	groups = pack + 8;
	triangles = groups + (size_t)group_count * 20;
	if (size < 8 || group_count > LT_EMITTERS_GROUPS_MAXIMUM * 64u || triangle_count > LT_EMITTERS_TRIANGLES_MAXIMUM * 64u ||
		(size_t)size != 8 + (size_t)group_count * 20 + (size_t)triangle_count * 36)
	{
		platform_log("lt emitters: the built-in pack for bsp %d is not whole", bsp_index);
		free(pack);
		return FALSE;
	}
	for (group = 0; group < group_count; group++)
	{
		unsigned char const *source = groups + (size_t)group * 20;

		first = lt_emitters_word(source + 12);
		count = lt_emitters_word(source + 16);
		if (first > triangle_count || count > triangle_count - first ||
			lt_emitters.group_count >= LT_EMITTERS_GROUPS_MAXIMUM ||
			lt_emitters.triangle_count + (int)count > LT_EMITTERS_TRIANGLES_MAXIMUM)
			break;
		for (value = 0; value < 3; value++)
			lt_emitters.radiance[lt_emitters.group_count][value] = lt_emitters_float(source + 4 * value);
		for (triangle = first; triangle < first + count; triangle++)
		{
			for (value = 0; value < 9; value++)
				lt_emitters.triangles[lt_emitters.triangle_count][value] =
					lt_emitters_float(triangles + (size_t)triangle * 36 + 4 * value);
			lt_emitters.group_of[lt_emitters.triangle_count++] = lt_emitters.group_count;
		}
		lt_emitters.group_count++;
	}
	free(pack);
	return lt_emitters.group_count > 0;
}

/* a glowing triangle: its material, cell and order (grouping), its power
(the cap's choice: luminance x area x sides) and its corners */
struct lt_emitters_found
{
	long material, cell, order;
	float power;
	float corners[9];
};

static int lt_emitters_found_order(void const *a, void const *b)
{
	struct lt_emitters_found const *x = a, *y = b;

	if (x->material != y->material)
		return x->material < y->material ? -1 : 1;
	if (x->cell != y->cell)
		return x->cell < y->cell ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
}

/* (the strongest first; ties by the mesh's order, so the choice is the same
every run) */
static int lt_emitters_found_power(void const *a, void const *b)
{
	struct lt_emitters_found const *x = a, *y = b;

	if (x->power != y->power)
		return x->power > y->power ? -1 : 1;
	return x->order < y->order ? -1 : x->order > y->order;
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

/* a material's emitted radiance by the one rule (LT_EMITTERS_EXITANCE_TO_
RADIANCE): its shader's radiosity power x colour of emitted light, the
lightmap tool's emitter, over pi; and whether it glows both ways (a
transparent shader's card) */
static void lt_emitters_rule(long shader_index, float radiance[3], int *two_sided)
{
	struct shader const *shader = shader_index != NONE && tag_index_is_group(shader_index, SHADER_DEFINITION_TAG) ?
		shader_definition_get(shader_index) : NULL;
	struct shader_radiosity_properties const *radiosity = shader ? &shader->base.radiosity : NULL;
	float power = radiosity && radiosity->power > 0.0f && radiosity->power < 1.0e6f ? radiosity->power : 0.0f;

	radiance[0] = radiosity ? radiosity->color_of_emitted_light.red * power * LT_EMITTERS_EXITANCE_TO_RADIANCE : 0.0f;
	radiance[1] = radiosity ? radiosity->color_of_emitted_light.green * power * LT_EMITTERS_EXITANCE_TO_RADIANCE : 0.0f;
	radiance[2] = radiosity ? radiosity->color_of_emitted_light.blue * power * LT_EMITTERS_EXITANCE_TO_RADIANCE : 0.0f;
	*two_sided = shader && shader->base.type != LT_EMITTERS_SHADER_ENVIRONMENT;
	/* (the energy class's factor) */
	if (shader && shader->base.type == LT_EMITTERS_SHADER_TRANSPARENT_PLASMA)
	{
		radiance[0] *= LT_EMITTERS_ENERGY_FACTOR;
		radiance[1] *= LT_EMITTERS_ENERGY_FACTOR;
		radiance[2] *= LT_EMITTERS_ENERGY_FACTOR;
	}
}

/* the BSP's glowing surfaces by the rule: every structure surface, opaque,
alpha-tested or transparent, whose shader emits (lt_emitters_rule), lifted
off its surface toward its normal (a two-sided one's other face too), the
strongest LT_EMITTERS_TRIANGLES_MAXIMUM by power, grouped by shader and
cell */
static void lt_emitters_tags(short bsp_index)
{
	struct world_rt_source_mesh mesh;
	struct lt_emitters_found *found = NULL;
	float (*radiance)[3] = NULL;
	int *two_sided = NULL;
	long triangle, count = 0, room = 0, index, kept;
	double total = 0.0, total_kept = 0.0;

	memset(&mesh, 0, sizeof(mesh));
	if (world_rt_source_structure_mesh_all(bsp_index, &mesh) <= 0 || mesh.material_count <= 0)
		return;
	mesh.vertices = malloc(sizeof(*mesh.vertices) * (size_t)mesh.vertex_count);
	mesh.triangles = malloc(sizeof(*mesh.triangles) * (size_t)mesh.triangle_count);
	mesh.triangle_materials = malloc(sizeof(*mesh.triangle_materials) * (size_t)mesh.triangle_count);
	mesh.materials = malloc(sizeof(*mesh.materials) * (size_t)mesh.material_count);
	radiance = malloc(sizeof(*radiance) * (size_t)mesh.material_count);
	two_sided = malloc(sizeof(*two_sided) * (size_t)mesh.material_count);
	if (!mesh.vertices || !mesh.triangles || !mesh.triangle_materials || !mesh.materials || !radiance || !two_sided ||
		world_rt_source_structure_mesh_all(bsp_index, &mesh) <= 0)
	{
		goto done;
	}
	for (index = 0; index < mesh.material_count; index++)
		lt_emitters_rule(mesh.materials[index].shader, radiance[index], &two_sided[index]);
	for (triangle = 0; triangle < mesh.triangle_count; triangle++)
	{
		long material = mesh.triangle_materials[triangle];

		if (radiance[material][0] + radiance[material][1] + radiance[material][2] > 0.0f)
			room += two_sided[material] ? 2 : 1;
	}
	found = room ? malloc(sizeof(*found) * (size_t)room) : NULL;
	if (!found)
		goto done;
	for (triangle = 0; triangle < mesh.triangle_count; triangle++)
	{
		long material = mesh.triangle_materials[triangle];
		float corners[9], e1[3], e2[3], normal[3], length, luminance;
		int corner, axis, side;

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
		luminance = 0.2126f * radiance[material][0] + 0.7152f * radiance[material][1] + 0.0722f * radiance[material][2];
		for (side = 0; side < (two_sided[material] ? 2 : 1); side++)
		{
			float *out = found[count].corners;
			float lift = side ? -LT_EMITTERS_LIFT : LT_EMITTERS_LIFT;

			memcpy(out, corners, sizeof(corners));
			/* (the other face: the winding turned, lifted the other way) */
			if (side)
			{
				memcpy(out + 3, corners + 6, sizeof(float) * 3);
				memcpy(out + 6, corners + 3, sizeof(float) * 3);
			}
			for (corner = 0; corner < 3; corner++)
				for (axis = 0; axis < 3; axis++)
					out[3 * corner + axis] += normal[axis] / length * lift;
			found[count].material = material;
			found[count].cell = lt_emitters_cell_of(out);
			found[count].order = count;
			found[count].power = luminance * 0.5f * fabsf(length);
			total += found[count].power;
			count++;
		}
	}
	/* (the strongest within the backend's room, the same every run) */
	kept = count;
	if (kept > LT_EMITTERS_TRIANGLES_MAXIMUM)
	{
		qsort(found, (size_t)count, sizeof(*found), lt_emitters_found_power);
		kept = LT_EMITTERS_TRIANGLES_MAXIMUM;
	}
	for (index = 0; index < kept; index++)
		total_kept += found[index].power;
	qsort(found, (size_t)kept, sizeof(*found), lt_emitters_found_order);
	for (index = 0; index < kept; index++)
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
	/* (the four strongest groups, where they are: for staging a look) */
	{
		int shown, group, best;
		static float group_power[LT_EMITTERS_GROUPS_MAXIMUM], group_middle[LT_EMITTERS_GROUPS_MAXIMUM][3];
		int used[LT_EMITTERS_GROUPS_MAXIMUM];

		memset(group_power, 0, sizeof(group_power));
		memset(group_middle, 0, sizeof(group_middle));
		memset(used, 0, sizeof(used));
		for (index = 0; index < lt_emitters.triangle_count; index++)
		{
			float const *c = lt_emitters.triangles[index], *r = lt_emitters.radiance[lt_emitters.group_of[index]];
			float e1[3], e2[3], n[3], area;
			int axis;

			for (axis = 0; axis < 3; axis++)
			{
				e1[axis] = c[3 + axis] - c[axis];
				e2[axis] = c[6 + axis] - c[axis];
			}
			n[0] = e1[1] * e2[2] - e1[2] * e2[1];
			n[1] = e1[2] * e2[0] - e1[0] * e2[2];
			n[2] = e1[0] * e2[1] - e1[1] * e2[0];
			area = 0.5f * sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			group_power[lt_emitters.group_of[index]] += area * (0.2126f * r[0] + 0.7152f * r[1] + 0.0722f * r[2]);
			for (axis = 0; axis < 3; axis++)
				group_middle[lt_emitters.group_of[index]][axis] += area * (c[axis] + c[3 + axis] + c[6 + axis]) / 3.0f;
		}
		for (shown = 0; shown < 4 && shown < lt_emitters.group_count; shown++)
		{
			best = -1;
			for (group = 0; group < lt_emitters.group_count; group++)
				if (!used[group] && (best < 0 || group_power[group] > group_power[best]))
					best = group;
			used[best] = 1;
			{
				float const *r = lt_emitters.radiance[best];
				float luminance = 0.2126f * r[0] + 0.7152f * r[1] + 0.0722f * r[2];
				float area = luminance > 0.0f ? group_power[best] / luminance : 0.0f;

				platform_log("lt emitters: strong group %d: radiance %.3g %.3g %.3g, area %.3g, power %.3g, at %.1f %.1f %.1f",
					best, r[0], r[1], r[2], area, group_power[best],
					area > 0.0f ? group_middle[best][0] / area : 0.0f, area > 0.0f ? group_middle[best][1] / area : 0.0f,
					area > 0.0f ? group_middle[best][2] / area : 0.0f);
			}
		}
	}
	platform_log("lt emitters: the rule's glowing surfaces: %ld triangles (%ld materials), %ld kept, %.1f%% of their "
		"power; total power %.3g", count, mesh.material_count, kept, total > 0.0 ? 100.0 * total_kept / total : 0.0,
		total);
done:
	free(found);
	free(radiance);
	free(two_sided);
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
	if (setting == 3)
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
		lt_emitters.solved ? "solved from the lightmap (the check: HALO_LT_EMITTERS=solved)" :
		"the shaders' own emission (the one rule)");
}

/* ---------- public code */

int lt_emitters_live(void)
{
	lt_emitters_update();
	return lt_emitters.triangle_count > 0;
}

int lt_emitters_fitted(void)
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
		out->flags = XGPU_LT_LIGHT_EMITTER_SOLVED;
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
