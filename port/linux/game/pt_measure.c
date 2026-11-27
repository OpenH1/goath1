/*
PT_MEASURE.C

Path tracing's stage 0, "Measure", the static fit study
(docs/rt-plan/4-path-tracing.md, "Faithful: fit the lights to the
lightmap"), with HALO_PT_MEASURE_FIT set: how well traced direct light from
the level's static sources explains its lightmap. Measurement only, on the
CPU, game side; nothing here is drawn, read by the simulation, sent or
saved.

A while into the map (HALO_PT_MEASURE_FIT_DELAY windows, 600 by default)
the loaded structure BSP is read: its lightmaps' materials' surfaces over
their compressed environment vertices (as world_rt_game.c reads them), the
shader_environment ones (opaque and alpha-tested, both taken as opaque) the
occluders and receivers, any with radiosity power (transparent ones too) an
emitter; each lightmap page copied (linear: a pure 2.2 decode, the plan's),
each receiver's base map's mean albedo; the scenario's skies' lights
(sky_definitions.h's struct sky_light) and the first sky's outdoor ambient
radiosity (the dome). A BVH over the occluders. Then samples, by area, on
the receivers that have a lightmap (not emitters'), a share each window;
at each, its lightmap texel L and each source group's traced contribution:
- each sky light: color x power x n.l x the share of rays inside its disk
  that escape;
- the dome: the first sky's outdoor ambient color x power x the share of
  cosine rays that escape;
- each emitter shader: color x power x the irradiance from its surfaces
  (cos cos A / r^2), sampled by each triangle's unoccluded share, with
  shadow rays;
- the bounce G: the mean, over the same cosine rays, of what they hit's
  albedo x lightmap (the bake's outgoing light: the plan's terminator).
Last, least squares (non-negative) of L against the groups, RGB, in three
models (four scales over the tags' own powers; a power for each source;
direct light alone), each weighted by energy and relatively; and for each,
the ratio R = L / T per sample (luminance) and its spread; and the
lightmap's mood (its luminance and color, sun against shade). Logged as
"pt fit:" lines, once.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "bitmaps/bitmaps_sampling.h"
#include "cache/texture_cache.h"
#include "rasterizer/rasterizer_geometry.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "scenario/sky_definitions.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"
#include "pt_measure.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (cache_files.c) */
boolean tag_index_is_group(long tag_index, long group_tag);
void platform_log(const char *format, ...);

/* ---------- constants */

enum
{
	PT_SAMPLES_DEFAULT = 16000,
	PT_DELAY_DEFAULT = 600,
	/* the samples traced a window */
	PT_CHUNK = 96,
	/* rays a sample: the cosine rays (dome and bounce), each sky light's,
	the emitters' shadow rays */
	PT_COSINE_RAYS = 48,
	PT_SUN_RAYS = 4,
	PT_EMITTER_RAYS = 32,
	/* the groups at most: the bounce, the dome, the sky lights, the emitter
	shaders (the rest of them the last's) */
	PT_SUNS = 8,
	PT_EMITTERS = 64,
	PT_COLUMNS = 2 + PT_SUNS + PT_EMITTERS,
	PT_EMITTER_TRIANGLES = 30000,
	PT_LEAF = 4,
	/* (shaders.c's) */
	_pt_shader_type_environment = 3,
	_pt_bitmap_type_2d = 0,
	_pt_bitmap_linear_bit = 4,
};

#define PT_OFFSET 0.01f
#define PT_FAR 4000.0f
#define PT_PI 3.14159265f

/* ---------- structures */

/* (world_rt_game.c's: a shader_environment's base map) */
struct pt_shader_environment
{
	struct shader shader;
	word flags;
	short type;
	byte reserved2C[0x88 - 0x2C];
	struct tag_reference base_map;
};

typedef char pt_shader_environment_base_map[offsetof(struct pt_shader_environment, base_map) == 0x88 ? 1 : -1];

struct pt_triangle
{
	float p[3][3];
	float n[3];
	float area;
	float lightmap[3][2];
	unsigned short material;
	unsigned char occluder;
};

struct pt_material
{
	long shader;
	int page, receiver, column;
	float albedo[3], emission[3];
};

struct pt_page
{
	short index;
	int width, height;
	unsigned char *rgb;
};

struct pt_node
{
	float low[3], high[3];
	int first, count;
};

struct pt_sun
{
	float direction[3], color[3], cone;
};

struct pt_sample
{
	float lightmap[3];
	float sun_seen;
	float d[PT_COLUMNS][3];
};

/* ---------- globals */

static struct
{
	int state, given, delay, calls;
	long samples_wanted, samples_done;
	struct pt_triangle *triangles;
	long triangle_count;
	struct pt_material *materials;
	long material_count;
	struct pt_page *pages;
	int page_count;
	struct pt_node *nodes;
	long node_count;
	long *order;
	long *emitters;
	long emitter_count;
	float *emitter_weights;
	float *receiver_cdf;
	long *receiver_triangles;
	long receiver_count;
	struct pt_sun suns[PT_SUNS];
	int sun_count;
	float dome[3];
	int columns;
	long column_shader[PT_EMITTERS];
	float column_emission[PT_EMITTERS][3];
	char column_names[PT_COLUMNS][48];
	struct pt_sample *samples;
	unsigned long random;
	double rays;
	float decode[256];
	short bsp;
} pt;

/* ---------- small math */

static float dot3(const float a[3], const float b[3])
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void cross3(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

static float luminance(const float c[3])
{
	return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

static float random_unit(void)
{
	/* (a 32-bit xorshift) */
	unsigned long x = pt.random;

	x ^= (x << 13) & 0xFFFFFFFFUL;
	x ^= x >> 17;
	x ^= (x << 5) & 0xFFFFFFFFUL;
	pt.random = x & 0xFFFFFFFFUL;
	return (float)(pt.random >> 8) * (1.0f / 16777216.0f);
}

/* an orthonormal basis about n */
static void basis(const float n[3], float t[3], float b[3])
{
	float a[3] = { 0.0f, 0.0f, 0.0f };
	float length;

	a[fabsf(n[0]) < 0.6f ? 0 : fabsf(n[1]) < 0.6f ? 1 : 2] = 1.0f;
	cross3(n, a, t);
	length = sqrtf(dot3(t, t));
	t[0] /= length;
	t[1] /= length;
	t[2] /= length;
	cross3(n, t, b);
}

static void cosine_direction(const float n[3], float out[3])
{
	float t[3], b[3], u = random_unit(), v = random_unit();
	float r = sqrtf(u), phi = 2.0f * PT_PI * v, x = r * cosf(phi), y = r * sinf(phi), z = sqrtf(1.0f - u);
	int k;

	basis(n, t, b);
	for (k = 0; k < 3; k++)
		out[k] = x * t[k] + y * b[k] + z * n[k];
}

/* a direction inside a cone of half-angle cone about d */
static void cone_direction(const float d[3], float cone, float out[3])
{
	float t[3], b[3], cos_max = cosf(cone), z = 1.0f - random_unit() * (1.0f - cos_max);
	float r = sqrtf(fmaxf(0.0f, 1.0f - z * z)), phi = 2.0f * PT_PI * random_unit();
	int k;

	basis(d, t, b);
	for (k = 0; k < 3; k++)
		out[k] = r * cosf(phi) * t[k] + r * sinf(phi) * b[k] + z * d[k];
}

/* ---------- the BVH */

static float centroid(long triangle, int axis)
{
	struct pt_triangle const *t = &pt.triangles[triangle];

	return t->p[0][axis] + t->p[1][axis] + t->p[2][axis];
}

/* order[first..first+count) partly sorted so that its middle is the median
along axis (quickselect) */
static void select_median(long first, long count, int axis)
{
	long low = first, high = first + count - 1, want = first + count / 2;

	while (low < high)
	{
		float pivot = centroid(pt.order[(low + high) / 2], axis);
		long i = low, j = high;

		while (i <= j)
		{
			while (centroid(pt.order[i], axis) < pivot)
				i++;
			while (centroid(pt.order[j], axis) > pivot)
				j--;
			if (i <= j)
			{
				long swap = pt.order[i];

				pt.order[i] = pt.order[j];
				pt.order[j] = swap;
				i++;
				j--;
			}
		}
		if (want <= j)
			high = j;
		else if (want >= i)
			low = i;
		else
			break;
	}
}

static long build_node(long first, long count)
{
	long index = pt.node_count++, at;
	struct pt_node *node = &pt.nodes[index];
	float clow[3] = { 1e30f, 1e30f, 1e30f }, chigh[3] = { -1e30f, -1e30f, -1e30f };
	int axis, corner;

	for (axis = 0; axis < 3; axis++)
	{
		node->low[axis] = 1e30f;
		node->high[axis] = -1e30f;
	}
	for (at = first; at < first + count; at++)
	{
		struct pt_triangle const *t = &pt.triangles[pt.order[at]];

		for (axis = 0; axis < 3; axis++)
		{
			float c = centroid(pt.order[at], axis);

			for (corner = 0; corner < 3; corner++)
			{
				node->low[axis] = fminf(node->low[axis], t->p[corner][axis]);
				node->high[axis] = fmaxf(node->high[axis], t->p[corner][axis]);
			}
			clow[axis] = fminf(clow[axis], c);
			chigh[axis] = fmaxf(chigh[axis], c);
		}
	}
	if (count <= PT_LEAF)
	{
		node->first = (int)first;
		node->count = (int)count;
		return index;
	}
	axis = chigh[0] - clow[0] > chigh[1] - clow[1] ? 0 : 1;
	if (chigh[2] - clow[2] > chigh[axis] - clow[axis])
		axis = 2;
	select_median(first, count, axis);
	node->count = 0;
	build_node(first, count / 2);
	/* (the right child's index; the left is index + 1) */
	{
		long right = build_node(first + count / 2, count - count / 2);

		pt.nodes[index].first = (int)right;
	}
	return index;
}

static int box_hit(const struct pt_node *node, const float origin[3], const float inverse[3], float far)
{
	float near = 0.0f;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float a = (node->low[axis] - origin[axis]) * inverse[axis];
		float b = (node->high[axis] - origin[axis]) * inverse[axis];

		near = fmaxf(near, fminf(a, b));
		far = fminf(far, fmaxf(a, b));
	}
	return near <= far;
}

/* the nearest occluder along the ray before far (its triangle, -1 none, and
its t, u, v); any: the first found */
static long trace(const float origin[3], const float direction[3], float far, int any, float *t_out, float *u_out,
	float *v_out)
{
	long stack[96], hit = -1;
	int top = 0, axis;
	float inverse[3], best = far;

	pt.rays += 1.0;
	for (axis = 0; axis < 3; axis++)
		inverse[axis] = 1.0f / (fabsf(direction[axis]) > 1e-12f ? direction[axis] : 1e-12f);
	stack[top++] = 0;
	while (top > 0)
	{
		const struct pt_node *node = &pt.nodes[stack[--top]];
		long index = node - pt.nodes, at;

		if (!box_hit(node, origin, inverse, best))
			continue;
		if (!node->count)
		{
			if (top < 94)
			{
				stack[top++] = node->first;
				stack[top++] = index + 1;
			}
			continue;
		}
		for (at = node->first; at < node->first + node->count; at++)
		{
			struct pt_triangle const *tri = &pt.triangles[pt.order[at]];
			float e1[3], e2[3], p[3], q[3], s[3], det, inv, u, v, t;
			int k;

			for (k = 0; k < 3; k++)
			{
				e1[k] = tri->p[1][k] - tri->p[0][k];
				e2[k] = tri->p[2][k] - tri->p[0][k];
				s[k] = origin[k] - tri->p[0][k];
			}
			cross3(direction, e2, p);
			det = dot3(e1, p);
			if (fabsf(det) < 1e-12f)
				continue;
			inv = 1.0f / det;
			u = dot3(s, p) * inv;
			if (u < 0.0f || u > 1.0f)
				continue;
			cross3(s, e1, q);
			v = dot3(direction, q) * inv;
			if (v < 0.0f || u + v > 1.0f)
				continue;
			t = dot3(e2, q) * inv;
			if (t > 1e-4f && t < best)
			{
				best = t;
				hit = pt.order[at];
				if (u_out)
				{
					*u_out = u;
					*v_out = v;
				}
				if (any)
				{
					*t_out = t;
					return hit;
				}
			}
		}
	}
	*t_out = best;
	return hit;
}

/* ---------- the level */

static float shader_power(long shader_index, float emission[3])
{
	struct shader const *shader;

	emission[0] = emission[1] = emission[2] = 0.0f;
	if (shader_index == NONE || !tag_index_is_group(shader_index, SHADER_DEFINITION_TAG))
		return 0.0f;
	shader = shader_definition_get(shader_index);
	if (!shader || !(shader->base.radiosity.power > 0.0f))
		return 0.0f;
	emission[0] = shader->base.radiosity.color_of_emitted_light.red * shader->base.radiosity.power;
	emission[1] = shader->base.radiosity.color_of_emitted_light.green * shader->base.radiosity.power;
	emission[2] = shader->base.radiosity.color_of_emitted_light.blue * shader->base.radiosity.power;
	return shader->base.radiosity.power;
}

/* a 2D bitmap loaded for the CPU's reads (NULL none) */
static struct bitmap_data *bitmap_loaded(long group, short index)
{
	struct bitmap_data *bitmap;

	if (group == NONE || index < 0 || !tag_index_is_group(group, BITMAP_GROUP_TAG))
		return NULL;
	bitmap = bitmap_group_try_and_get_bitmap(group, index);
	if (!bitmap || bitmap->type != _pt_bitmap_type_2d || TEST_FLAG(bitmap->flags, _pt_bitmap_linear_bit) ||
		bitmap->width <= 0 || bitmap->height <= 0 || bitmap->width > 4096 || bitmap->height > 4096)
		return NULL;
	if (!texture_cache_bitmap_load(bitmap) || !bitmap->base_address)
		return NULL;
	return bitmap;
}

static unsigned long pixel_at(struct bitmap_data *bitmap, float x, float y)
{
	real_point2d point;

	point.x = x;
	point.y = y;
	return (unsigned long)bitmap_2d_get_pixel(bitmap, &point, 1.0f);
}

/* a lightmap page's index among those copied (-1 none) */
static int page_of(long group, short index)
{
	struct bitmap_data *bitmap;
	struct pt_page *page;
	int at, x, y;

	if (index < 0)
		return -1;
	for (at = 0; at < pt.page_count; at++)
	{
		if (pt.pages[at].index == index)
			return pt.pages[at].rgb ? at : -1;
	}
	page = &pt.pages[pt.page_count++];
	memset(page, 0, sizeof(*page));
	page->index = index;
	bitmap = bitmap_loaded(group, index);
	if (!bitmap)
		return -1;
	page->width = bitmap->width;
	page->height = bitmap->height;
	page->rgb = malloc((size_t)page->width * page->height * 3);
	if (!page->rgb)
		return -1;
	for (y = 0; y < page->height; y++)
	{
		for (x = 0; x < page->width; x++)
		{
			unsigned long pixel = pixel_at(bitmap, ((float)x + 0.5f) / page->width, ((float)y + 0.5f) / page->height);
			unsigned char *out = &page->rgb[((size_t)y * page->width + x) * 3];

			out[0] = (unsigned char)(pixel >> 16);
			out[1] = (unsigned char)(pixel >> 8);
			out[2] = (unsigned char)pixel;
		}
	}
	return pt.page_count - 1;
}

static void page_read(int page_index, float u, float v, float out[3])
{
	struct pt_page const *page = &pt.pages[page_index];
	int x = (int)floorf(u * page->width), y = (int)floorf(v * page->height);
	unsigned char const *texel;

	x = ((x % page->width) + page->width) % page->width;
	y = ((y % page->height) + page->height) % page->height;
	texel = &page->rgb[((size_t)y * page->width + x) * 3];
	out[0] = pt.decode[texel[0]];
	out[1] = pt.decode[texel[1]];
	out[2] = pt.decode[texel[2]];
}

/* a receiver's base map's mean albedo, linear (0.5 grey where it has none) */
static void albedo_of(long shader_index, float out[3])
{
	struct pt_shader_environment const *environment;
	struct bitmap_data *bitmap;
	int x, y;

	out[0] = out[1] = out[2] = 0.5f;
	if (shader_index == NONE || !tag_index_is_group(shader_index, SHADER_DEFINITION_TAG))
		return;
	environment = (struct pt_shader_environment const *)shader_definition_get(shader_index);
	if (!environment || environment->shader.base.type != _pt_shader_type_environment)
		return;
	bitmap = bitmap_loaded(environment->base_map.index, 0);
	if (!bitmap)
		return;
	out[0] = out[1] = out[2] = 0.0f;
	for (y = 0; y < 16; y++)
	{
		for (x = 0; x < 16; x++)
		{
			unsigned long pixel = pixel_at(bitmap, (x + 0.5f) / 16.0f, (y + 0.5f) / 16.0f);

			out[0] += pt.decode[(pixel >> 16) & 255] / 256.0f;
			out[1] += pt.decode[(pixel >> 8) & 255] / 256.0f;
			out[2] += pt.decode[pixel & 255] / 256.0f;
		}
	}
}

/* the BSP's surfaces read (FALSE: none) */
static int level_read(void)
{
	struct structure_bsp *structure_bsp = global_structure_bsp_get();
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_index, room = 0, material_room = 0, page_room = 0;
	int pass;

	if (!structure_bsp || !structure_bsp->lightmaps.address || !structure_bsp->surfaces.address)
		return FALSE;
	/* (counted, then read) */
	for (pass = 0; pass < 2; pass++)
	{
		pt.triangle_count = pt.material_count = 0;
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
				long vertex_count = material->vertices.count, surface_index;
				byte const *lightmap_vertices = material->lightmap_vertices.count == vertex_count &&
					material->compressed_vertex_data.size >= vertex_count * (vertex_size + 8) ?
					vertices + vertex_count * vertex_size : NULL;
				struct shader const *shader = material->shader.index != NONE &&
					tag_index_is_group(material->shader.index, SHADER_DEFINITION_TAG) ?
					shader_definition_get(material->shader.index) : NULL;
				int environment = shader && shader->base.type == _pt_shader_type_environment;
				float emission[3];
				int emitter = shader_power(material->shader.index, emission) > 0.0f;
				struct pt_material *out_material;

				if (!shader || (!environment && !emitter) || !vertices || vertex_count <= 0 ||
					material->compressed_vertex_data.size < vertex_count * vertex_size || material->surface_count <= 0 ||
					material->first_surface_index < 0 ||
					material->first_surface_index > structure_bsp->surfaces.count - material->surface_count)
					continue;
				if (pass == 0)
				{
					room += material->surface_count;
					material_room++;
					continue;
				}
				out_material = &pt.materials[pt.material_count];
				memset(out_material, 0, sizeof(*out_material));
				out_material->shader = material->shader.index;
				out_material->column = -1;
				out_material->page = environment && lightmap_vertices && lightmap->bitmap_index != NONE ?
					page_of(structure_bsp->lightmap_group.index, lightmap->bitmap_index) : -1;
				out_material->receiver = environment && !emitter && out_material->page >= 0;
				memcpy(out_material->emission, emission, sizeof(emission));
				if (environment)
					albedo_of(material->shader.index, out_material->albedo);
				if (emitter)
				{
					int column;

					for (column = 0; column < pt.columns - 2 - pt.sun_count; column++)
					{
						if (pt.column_shader[column] == material->shader.index)
							break;
					}
					if (column == pt.columns - 2 - pt.sun_count)
					{
						if (column < PT_EMITTERS)
						{
							pt.column_shader[column] = material->shader.index;
							memcpy(pt.column_emission[column], emission, sizeof(emission));
							snprintf(pt.column_names[2 + pt.sun_count + column], sizeof(pt.column_names[0]), "%s",
								tag_get_name(material->shader.index) ? tag_get_name(material->shader.index) : "?");
							pt.columns++;
						}
						else
							column = PT_EMITTERS - 1;
					}
					out_material->column = 2 + pt.sun_count + column;
				}
				for (surface_index = 0; surface_index < material->surface_count; surface_index++)
				{
					struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&structure_bsp->surfaces,
						material->first_surface_index + surface_index, struct structure_surface);
					struct pt_triangle *t = &pt.triangles[pt.triangle_count];
					float e1[3], e2[3], normal_sum[3] = { 0.0f, 0.0f, 0.0f }, length;
					int corner, k;

					if (surface->vertex_indices[0] >= vertex_count || surface->vertex_indices[1] >= vertex_count ||
						surface->vertex_indices[2] >= vertex_count || pt.triangle_count >= room)
						continue;
					for (corner = 0; corner < 3; corner++)
					{
						byte const *vertex = vertices + surface->vertex_indices[corner] * vertex_size;
						union real_vector3d normal = uncompress_int32_to_real_vector3d(*(unsigned long const *)(vertex + 12));

						memcpy(t->p[corner], vertex, sizeof(t->p[corner]));
						normal_sum[0] += normal.i;
						normal_sum[1] += normal.j;
						normal_sum[2] += normal.k;
						t->lightmap[corner][0] = t->lightmap[corner][1] = 0.0f;
						if (lightmap_vertices)
						{
							short const *uv = (short const *)(lightmap_vertices + surface->vertex_indices[corner] * 8 + 4);

							t->lightmap[corner][0] = ((float)uv[0] * 2.0f + 1.0f) / 65535.0f;
							t->lightmap[corner][1] = ((float)uv[1] * 2.0f + 1.0f) / 65535.0f;
						}
					}
					for (k = 0; k < 3; k++)
					{
						e1[k] = t->p[1][k] - t->p[0][k];
						e2[k] = t->p[2][k] - t->p[0][k];
					}
					cross3(e1, e2, t->n);
					length = sqrtf(dot3(t->n, t->n));
					if (!(length > 1e-10f))
						continue;
					t->area = 0.5f * length;
					for (k = 0; k < 3; k++)
						t->n[k] /= length;
					/* (facing as its vertices' normals do) */
					if (dot3(t->n, normal_sum) < 0.0f)
					{
						for (k = 0; k < 3; k++)
							t->n[k] = -t->n[k];
					}
					t->material = (unsigned short)pt.material_count;
					t->occluder = (unsigned char)environment;
					pt.triangle_count++;
				}
				pt.material_count++;
			}
		}
		if (pass == 0)
		{
			pt.triangles = malloc(sizeof(*pt.triangles) * (size_t)(room + 1));
			pt.materials = malloc(sizeof(*pt.materials) * (size_t)(material_room + 1));
			page_room = structure_bsp->lightmaps.count + 1;
			pt.pages = calloc((size_t)page_room, sizeof(*pt.pages));
			if (!pt.triangles || !pt.materials || !pt.pages)
				return FALSE;
		}
	}
	return pt.triangle_count > 0;
}

/* the scenario's skies' lights and the first's dome */
static void skies_read(void)
{
	struct scenario *scenario = global_scenario_try_and_get();
	short sky_index;

	pt.dome[0] = pt.dome[1] = pt.dome[2] = 0.0f;
	pt.sun_count = 0;
	if (!scenario)
		return;
	for (sky_index = 0; sky_index < scenario->sky_references.count; sky_index++)
	{
		struct sky *sky = scenario_get_sky(sky_index);
		long light_index;

		if (!sky)
			continue;
		platform_log("pt fit: sky %d (%s): indoor ambient %.3f %.3f %.3f x %.3f, outdoor ambient %.3f %.3f %.3f x %.3f, "
			"%ld lights", (int)sky_index, tag_get_name(scenario_get_sky_definition_index(sky_index)) ?
			tag_get_name(scenario_get_sky_definition_index(sky_index)) : "?",
			sky->indoor_ambient_radiosity_color.red, sky->indoor_ambient_radiosity_color.green,
			sky->indoor_ambient_radiosity_color.blue, sky->indoor_ambient_radiosity_power,
			sky->outdoor_ambient_radiosity_color.red, sky->outdoor_ambient_radiosity_color.green,
			sky->outdoor_ambient_radiosity_color.blue, sky->outdoor_ambient_radiosity_power, (long)sky->lights.count);
		if (sky_index == 0)
		{
			pt.dome[0] = sky->outdoor_ambient_radiosity_color.red * sky->outdoor_ambient_radiosity_power;
			pt.dome[1] = sky->outdoor_ambient_radiosity_color.green * sky->outdoor_ambient_radiosity_power;
			pt.dome[2] = sky->outdoor_ambient_radiosity_color.blue * sky->outdoor_ambient_radiosity_power;
		}
		for (light_index = 0; light_index < sky->lights.count && sky->lights.address; light_index++)
		{
			struct sky_light const *light = TAG_BLOCK_GET_ELEMENT(&sky->lights, light_index, struct sky_light);
			struct pt_sun *sun = &pt.suns[pt.sun_count];
			float yaw = light->direction[0], pitch = light->direction[1];

			platform_log("pt fit: sky %d light %ld: flags %lx, color %.3f %.3f %.3f, power %.3f, test distance %.2f, "
				"direction yaw %.2f pitch %.2f (degrees), diameter %.3f degrees", (int)sky_index, light_index,
				light->flags, light->color.red, light->color.green, light->color.blue, light->power,
				light->test_distance, yaw * 180.0f / PT_PI, pitch * 180.0f / PT_PI, light->diameter * 180.0f / PT_PI);
			if (pt.sun_count >= PT_SUNS)
				continue;
			sun->direction[0] = cosf(pitch) * cosf(yaw);
			sun->direction[1] = cosf(pitch) * sinf(yaw);
			sun->direction[2] = sinf(pitch);
			/* (toward the light: above the horizon) */
			if (sun->direction[2] < 0.0f)
			{
				sun->direction[0] = -sun->direction[0];
				sun->direction[1] = -sun->direction[1];
				sun->direction[2] = -sun->direction[2];
			}
			sun->color[0] = light->color.red * light->power;
			sun->color[1] = light->color.green * light->power;
			sun->color[2] = light->color.blue * light->power;
			sun->cone = fmaxf(light->diameter * 0.5f, 0.25f * PT_PI / 180.0f);
			snprintf(pt.column_names[2 + pt.sun_count], sizeof(pt.column_names[0]), "sky %d light %ld", (int)sky_index,
				light_index);
			pt.sun_count++;
		}
	}
}

static int prepare(void)
{
	long index, receivers = 0;
	float total = 0.0f;

	for (index = 0; index < 256; index++)
		pt.decode[index] = powf((float)index / 255.0f, 2.2f);
	pt.random = 0x2545F491UL;
	pt.bsp = global_structure_bsp_index;
	skies_read();
	strcpy(pt.column_names[0], "bounce G");
	strcpy(pt.column_names[1], "dome");
	pt.columns = 2 + pt.sun_count;
	if (!level_read())
	{
		platform_log("pt fit: the BSP could not be read");
		return FALSE;
	}
	/* (the occluders' BVH) */
	pt.order = malloc(sizeof(long) * (size_t)pt.triangle_count);
	pt.nodes = malloc(sizeof(*pt.nodes) * (size_t)(2 * pt.triangle_count + 1));
	pt.emitters = malloc(sizeof(long) * (size_t)pt.triangle_count);
	pt.emitter_weights = malloc(sizeof(float) * (size_t)pt.triangle_count);
	pt.receiver_cdf = malloc(sizeof(float) * (size_t)pt.triangle_count);
	pt.receiver_triangles = malloc(sizeof(long) * (size_t)pt.triangle_count);
	if (!pt.order || !pt.nodes || !pt.emitters || !pt.emitter_weights || !pt.receiver_cdf || !pt.receiver_triangles)
		return FALSE;
	{
		long occluders = 0;

		for (index = 0; index < pt.triangle_count; index++)
		{
			struct pt_triangle const *t = &pt.triangles[index];
			struct pt_material const *m = &pt.materials[t->material];

			if (t->occluder)
				pt.order[occluders++] = index;
			if (m->column >= 0 && pt.emitter_count < PT_EMITTER_TRIANGLES)
				pt.emitters[pt.emitter_count++] = index;
			if (m->receiver)
			{
				total += t->area;
				pt.receiver_triangles[receivers] = index;
				pt.receiver_cdf[receivers++] = total;
			}
		}
		pt.receiver_count = receivers;
		pt.node_count = 0;
		if (occluders)
			build_node(0, occluders);
		platform_log("pt fit: BSP %d: %ld triangles (%ld occluders, %ld BVH nodes), %ld materials, %d lightmap pages, "
			"%ld receivers (area %.0f), %ld emitter triangles of %d shaders, %d sky lights", (int)pt.bsp,
			pt.triangle_count, occluders, pt.node_count, pt.material_count, pt.page_count, receivers, total,
			pt.emitter_count, pt.columns - 2 - pt.sun_count, pt.sun_count);
	}
	for (index = 0; index < pt.columns - 2 - pt.sun_count; index++)
	{
		platform_log("pt fit: emitter %ld: %s, color x power %.3f %.3f %.3f", index, pt.column_names[2 + pt.sun_count + index],
			pt.column_emission[index][0], pt.column_emission[index][1], pt.column_emission[index][2]);
	}
	if (!receivers || !pt.node_count)
		return FALSE;
	pt.samples = calloc((size_t)pt.samples_wanted, sizeof(*pt.samples));
	return pt.samples != NULL;
}

/* ---------- a sample */

static void point_on(struct pt_triangle const *t, float point[3], float *u_out, float *v_out)
{
	float u = random_unit(), v = random_unit();
	int k;

	if (u + v > 1.0f)
	{
		u = 1.0f - u;
		v = 1.0f - v;
	}
	for (k = 0; k < 3; k++)
		point[k] = t->p[0][k] + u * (t->p[1][k] - t->p[0][k]) + v * (t->p[2][k] - t->p[0][k]);
	*u_out = u;
	*v_out = v;
}

static void lightmap_at(struct pt_triangle const *t, float u, float v, float out[3])
{
	struct pt_material const *m = &pt.materials[t->material];
	float lu = t->lightmap[0][0] + u * (t->lightmap[1][0] - t->lightmap[0][0]) + v * (t->lightmap[2][0] - t->lightmap[0][0]);
	float lv = t->lightmap[0][1] + u * (t->lightmap[1][1] - t->lightmap[0][1]) + v * (t->lightmap[2][1] - t->lightmap[0][1]);

	if (m->page < 0)
	{
		out[0] = out[1] = out[2] = 0.0f;
		return;
	}
	page_read(m->page, lu, lv, out);
}

static void sample_trace(struct pt_sample *sample)
{
	float pick = random_unit() * pt.receiver_cdf[pt.receiver_count - 1], x[3], origin[3], u, v, t;
	long low = 0, high = pt.receiver_count - 1, index;
	struct pt_triangle const *tri;
	int ray, k, sun;

	while (low < high)
	{
		long middle = (low + high) / 2;

		if (pt.receiver_cdf[middle] < pick)
			low = middle + 1;
		else
			high = middle;
	}
	tri = &pt.triangles[pt.receiver_triangles[low]];
	point_on(tri, x, &u, &v);
	lightmap_at(tri, u, v, sample->lightmap);
	for (k = 0; k < 3; k++)
		origin[k] = x[k] + tri->n[k] * PT_OFFSET;
	/* (the dome and the bounce) */
	for (ray = 0; ray < PT_COSINE_RAYS; ray++)
	{
		float direction[3], hu = 0.0f, hv = 0.0f;
		long hit;

		cosine_direction(tri->n, direction);
		hit = trace(origin, direction, PT_FAR, FALSE, &t, &hu, &hv);
		if (hit < 0)
		{
			for (k = 0; k < 3; k++)
				sample->d[1][k] += pt.dome[k] / PT_COSINE_RAYS;
		}
		else if (dot3(direction, pt.triangles[hit].n) < 0.0f)
		{
			struct pt_triangle const *h = &pt.triangles[hit];
			float light[3];

			lightmap_at(h, hu, hv, light);
			for (k = 0; k < 3; k++)
				sample->d[0][k] += pt.materials[h->material].albedo[k] * light[k] / PT_COSINE_RAYS;
		}
	}
	/* (the sky lights) */
	sample->sun_seen = 0.0f;
	for (sun = 0; sun < pt.sun_count; sun++)
	{
		float cosine = dot3(tri->n, pt.suns[sun].direction), seen = 0.0f;

		if (cosine <= 0.0f)
			continue;
		for (ray = 0; ray < PT_SUN_RAYS; ray++)
		{
			float direction[3];

			cone_direction(pt.suns[sun].direction, pt.suns[sun].cone, direction);
			if (trace(origin, direction, PT_FAR, TRUE, &t, NULL, NULL) < 0)
				seen += 1.0f / PT_SUN_RAYS;
		}
		for (k = 0; k < 3; k++)
			sample->d[2 + sun][k] = pt.suns[sun].color[k] * cosine * seen;
		if (sun == 0)
			sample->sun_seen = seen;
	}
	/* (the emitters: each triangle by its unoccluded share) */
	if (pt.emitter_count)
	{
		float total = 0.0f;
		long e;

		for (e = 0; e < pt.emitter_count; e++)
		{
			struct pt_triangle const *em = &pt.triangles[pt.emitters[e]];
			float w[3], r2, weight = 0.0f;

			for (k = 0; k < 3; k++)
				w[k] = (em->p[0][k] + em->p[1][k] + em->p[2][k]) * (1.0f / 3.0f) - x[k];
			r2 = fmaxf(dot3(w, w), 0.01f);
			{
				float r = sqrtf(r2), cx = dot3(tri->n, w) / r, ce = -dot3(em->n, w) / r;

				if (cx > 0.0f && ce > 0.0f)
					weight = luminance(pt.materials[em->material].emission) * em->area * cx * ce / r2;
			}
			total += weight;
			pt.emitter_weights[e] = total;
		}
		for (ray = 0; ray < PT_EMITTER_RAYS && total > 0.0f; ray++)
		{
			float choose = random_unit() * total, p_point[3], w[3], r2, r, cx, ce, pu, pv, probability;
			struct pt_triangle const *em;
			long a = 0, b = pt.emitter_count - 1;
			int column;

			while (a < b)
			{
				long middle = (a + b) / 2;

				if (pt.emitter_weights[middle] < choose)
					a = middle + 1;
				else
					b = middle;
			}
			probability = (pt.emitter_weights[a] - (a ? pt.emitter_weights[a - 1] : 0.0f)) / total;
			if (!(probability > 0.0f))
				continue;
			em = &pt.triangles[pt.emitters[a]];
			point_on(em, p_point, &pu, &pv);
			for (k = 0; k < 3; k++)
				w[k] = p_point[k] - origin[k];
			r2 = fmaxf(dot3(w, w), 0.01f);
			r = sqrtf(dot3(w, w));
			if (!(r > 1e-4f))
				continue;
			for (k = 0; k < 3; k++)
				w[k] /= r;
			cx = dot3(tri->n, w);
			ce = -dot3(em->n, w);
			if (cx <= 0.0f || ce <= 0.0f)
				continue;
			if (trace(origin, w, r - 2.0f * PT_OFFSET, TRUE, &t, NULL, NULL) >= 0)
				continue;
			column = pt.materials[em->material].column;
			for (k = 0; k < 3; k++)
				sample->d[column][k] += pt.materials[em->material].emission[k] * cx * ce * em->area / r2 /
					(probability * PT_EMITTER_RAYS);
		}
	}
	(void)index;
}

/* ---------- the fit */

/* a model: each column's group (-1 left out), its groups' count */
struct pt_model
{
	char const *name;
	int group[PT_COLUMNS];
	int groups;
	int relative;
};

static int compare_floats(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static float percentile(float *sorted, long count, float share)
{
	long at = (long)(share * (float)(count - 1) + 0.5f);

	return count ? sorted[at < 0 ? 0 : at >= count ? count - 1 : at] : 0.0f;
}

/* a sample's group values (RGB) under a model */
static void grouped(const struct pt_model *model, const struct pt_sample *sample, float out[PT_COLUMNS][3])
{
	int column, k;

	memset(out, 0, sizeof(float) * PT_COLUMNS * 3);
	for (column = 0; column < pt.columns; column++)
	{
		if (model->group[column] < 0)
			continue;
		for (k = 0; k < 3; k++)
			out[model->group[column]][k] += sample->d[column][k];
	}
}

static void fit(const struct pt_model *model, float *ratios)
{
	static double ata[PT_COLUMNS][PT_COLUMNS], atb[PT_COLUMNS];
	static float g[PT_COLUMNS][3];
	double x[PT_COLUMNS], residual = 0.0, spread = 0.0, mean = 0.0, shares[4] = { 0.0, 0.0, 0.0, 0.0 };
	long s, count = 0, zero = 0, within25 = 0, within2 = 0, outside4 = 0;
	int i, j, k, sweep, n = model->groups;
	char line[1024];
	int used;

	memset(ata, 0, sizeof(ata));
	memset(atb, 0, sizeof(atb));
	for (s = 0; s < pt.samples_done; s++)
	{
		const struct pt_sample *sample = &pt.samples[s];
		double weight = 1.0;

		if (model->relative)
		{
			double l = luminance(sample->lightmap) + 0.01;

			weight = 1.0 / (l * l);
		}
		grouped(model, sample, g);
		for (k = 0; k < 3; k++)
		{
			for (i = 0; i < n; i++)
			{
				atb[i] += weight * g[i][k] * sample->lightmap[k];
				for (j = 0; j < n; j++)
					ata[i][j] += weight * g[i][k] * g[j][k];
			}
		}
	}
	/* (non-negative: coordinate descent on the normal equations) */
	for (i = 0; i < n; i++)
		x[i] = 0.0;
	for (sweep = 0; sweep < 3000; sweep++)
	{
		for (i = 0; i < n; i++)
		{
			double r = atb[i];

			if (!(ata[i][i] > 0.0))
				continue;
			for (j = 0; j < n; j++)
				r -= ata[i][j] * x[j];
			x[i] += r / ata[i][i];
			if (x[i] < 0.0)
				x[i] = 0.0;
		}
	}
	for (s = 0; s < pt.samples_done; s++)
		mean += luminance(pt.samples[s].lightmap);
	mean /= pt.samples_done ? pt.samples_done : 1;
	for (s = 0; s < pt.samples_done; s++)
	{
		const struct pt_sample *sample = &pt.samples[s];
		float traced[3] = { 0.0f, 0.0f, 0.0f }, l = luminance(sample->lightmap), tl;
		int column;

		grouped(model, sample, g);
		for (i = 0; i < n; i++)
		{
			for (k = 0; k < 3; k++)
				traced[k] += (float)x[i] * g[i][k];
		}
		tl = luminance(traced);
		residual += (double)(l - tl) * (l - tl);
		spread += (double)(l - mean) * (l - mean);
		/* (the shares of the fitted light: bounce, dome, sky lights, emitters) */
		for (column = 0; column < pt.columns; column++)
		{
			int group = model->group[column];
			int kind = column == 0 ? 0 : column == 1 ? 1 : column < 2 + pt.sun_count ? 2 : 3;

			if (group >= 0)
				shares[kind] += x[group] * luminance(sample->d[column]);
		}
		if (l < 1e-4f)
			continue;
		if (!(tl > 1e-6f))
		{
			zero++;
			continue;
		}
		ratios[count++] = l / tl;
	}
	qsort(ratios, (size_t)count, sizeof(float), compare_floats);
	for (s = 0; s < count; s++)
	{
		within25 += ratios[s] >= 0.8f && ratios[s] <= 1.25f;
		within2 += ratios[s] >= 0.5f && ratios[s] <= 2.0f;
		outside4 += ratios[s] < 0.25f || ratios[s] > 4.0f;
	}
	{
		double total_share = shares[0] + shares[1] + shares[2] + shares[3];
		long all = count + zero;

		if (!(total_share > 0.0))
			total_share = 1.0;
		if (!all)
			all = 1;
		platform_log("pt fit: model %s (%s): explained %.3f of the lightmap's luminance variance; R p5 %.2f p25 %.2f "
			"median %.2f p75 %.2f p95 %.2f; within 0.8-1.25 %.1f%%, 0.5-2 %.1f%%, outside 0.25-4 %.1f%% (T 0: %.1f%%); "
			"light shares bounce %.0f%% dome %.0f%% sky lights %.0f%% emitters %.0f%%", model->name,
			model->relative ? "relative" : "energy", spread > 0.0 ? 1.0 - residual / spread : 0.0,
			percentile(ratios, count, 0.05f), percentile(ratios, count, 0.25f), percentile(ratios, count, 0.5f),
			percentile(ratios, count, 0.75f), percentile(ratios, count, 0.95f), 100.0 * within25 / all,
			100.0 * within2 / all, 100.0 * (outside4 + zero) / all, 100.0 * zero / all, 100.0 * shares[0] / total_share,
			100.0 * shares[1] / total_share, 100.0 * shares[2] / total_share, 100.0 * shares[3] / total_share);
	}
	used = snprintf(line, sizeof(line), "pt fit: model %s (%s) powers:", model->name, model->relative ? "relative" : "energy");
	for (i = 0; i < n && used < (int)sizeof(line) - 64; i++)
		used += snprintf(line + used, sizeof(line) - (size_t)used, " %.3g", x[i]);
	platform_log("%s", line);
}

static void mood(void)
{
	float *values = malloc(sizeof(float) * (size_t)(pt.samples_done + 1));
	double sums[2][4], all[3] = { 0.0, 0.0, 0.0 }, gamma = 0.0;
	long s, count = 0, counts[2] = { 0, 0 };
	float *split[2];

	if (!values)
		return;
	memset(sums, 0, sizeof(sums));
	split[0] = malloc(sizeof(float) * (size_t)(pt.samples_done + 1));
	split[1] = malloc(sizeof(float) * (size_t)(pt.samples_done + 1));
	for (s = 0; s < pt.samples_done; s++)
	{
		const struct pt_sample *sample = &pt.samples[s];
		float l = luminance(sample->lightmap);
		int k;

		values[count++] = l;
		gamma += powf(l, 1.0f / 2.2f);
		for (k = 0; k < 3; k++)
			all[k] += sample->lightmap[k];
		/* (sunlit: the first sky light's rays all escape; shaded: none, but
		some sky seen) */
		if (pt.sun_count && split[0] && split[1])
		{
			int which = sample->sun_seen >= 0.99f && luminance(sample->d[2]) > 0.0f ? 0 :
				sample->sun_seen <= 0.01f && luminance(sample->d[1]) > 0.0f ? 1 : -1;

			if (which >= 0)
			{
				split[which][counts[which]++] = l;
				for (k = 0; k < 3; k++)
					sums[which][k] += sample->lightmap[k];
				sums[which][3] += luminance(sample->d[1]) > 0.0f ? 1.0 : 0.0;
			}
		}
	}
	qsort(values, (size_t)count, sizeof(float), compare_floats);
	{
		double sum = all[0] + all[1] + all[2];

		if (!(sum > 0.0))
			sum = 1.0;
		platform_log("pt fit: mood: the lightmap's linear luminance p5 %.3f p25 %.3f median %.3f p75 %.3f p95 %.3f "
			"max %.3f; mean encoded %.3f; its light's color (energy) r %.3f g %.3f b %.3f",
			percentile(values, count, 0.05f), percentile(values, count, 0.25f), percentile(values, count, 0.5f),
			percentile(values, count, 0.75f), percentile(values, count, 0.95f), count ? values[count - 1] : 0.0f,
			count ? gamma / count : 0.0, all[0] / sum, all[1] / sum, all[2] / sum);
	}
	if (split[0] && split[1] && counts[0] && counts[1])
	{
		int which;
		float medians[2];

		for (which = 0; which < 2; which++)
		{
			double sum = sums[which][0] + sums[which][1] + sums[which][2];

			qsort(split[which], (size_t)counts[which], sizeof(float), compare_floats);
			medians[which] = percentile(split[which], counts[which], 0.5f);
			if (!(sum > 0.0))
				sum = 1.0;
			platform_log("pt fit: mood: %s (%ld samples): median luminance %.3f, color r %.3f g %.3f b %.3f",
				which ? "in the sun's shadow, sky seen" : "in full sun", counts[which], medians[which],
				sums[which][0] / sum, sums[which][1] / sum, sums[which][2] / sum);
		}
		platform_log("pt fit: mood: the bake's sun to shade contrast (medians) %.2f", medians[1] > 0.0f ?
			medians[0] / medians[1] : 0.0f);
	}
	free(split[0]);
	free(split[1]);
	free(values);
}

static void report(void)
{
	float *ratios = malloc(sizeof(float) * (size_t)(pt.samples_done + 1));
	struct pt_model model;
	int column, relative;

	platform_log("pt fit: BSP %d: %ld samples, %.0f rays", (int)pt.bsp, pt.samples_done, pt.rays);
	mood();
	if (!ratios)
		return;
	for (relative = 0; relative < 2; relative++)
	{
		/* (the tags' own powers, four scales: bounce, dome, sky lights,
		emitters) */
		memset(&model, 0, sizeof(model));
		model.name = "tags x4";
		model.relative = relative;
		for (column = 0; column < pt.columns; column++)
			model.group[column] = column < 2 ? column : column < 2 + pt.sun_count ? 2 : 3;
		model.groups = 4;
		fit(&model, ratios);
		/* (a power for each source) */
		model.name = "each source";
		for (column = 0; column < pt.columns; column++)
			model.group[column] = column;
		model.groups = pt.columns;
		fit(&model, ratios);
		/* (direct light alone: no bounce) */
		model.name = "direct alone";
		model.group[0] = -1;
		for (column = 1; column < pt.columns; column++)
			model.group[column] = column < 2 + pt.sun_count ? column - 1 : pt.sun_count + 1;
		model.groups = pt.sun_count + 2;
		fit(&model, ratios);
	}
	{
		char line[1024];
		int used = snprintf(line, sizeof(line), "pt fit: columns:");

		for (column = 0; column < pt.columns && used < (int)sizeof(line) - 64; column++)
			used += snprintf(line + used, sizeof(line) - (size_t)used, " [%d] %s;", column, pt.column_names[column]);
		platform_log("%s", line);
	}
	free(ratios);
}

static void release(void)
{
	int page;

	for (page = 0; page < pt.page_count; page++)
		free(pt.pages[page].rgb);
	free(pt.pages);
	free(pt.triangles);
	free(pt.materials);
	free(pt.nodes);
	free(pt.order);
	free(pt.emitters);
	free(pt.emitter_weights);
	free(pt.receiver_cdf);
	free(pt.receiver_triangles);
	free(pt.samples);
	pt.pages = NULL;
	pt.triangles = NULL;
	pt.materials = NULL;
	pt.nodes = NULL;
	pt.order = pt.emitters = pt.receiver_triangles = NULL;
	pt.emitter_weights = pt.receiver_cdf = NULL;
	pt.samples = NULL;
}

/* ---------- public code */

void pt_measure_frame(void)
{
	long chunk;

	if (!pt.given)
	{
		char const *value = getenv("HALO_PT_MEASURE_FIT");
		char const *delay = getenv("HALO_PT_MEASURE_FIT_DELAY");

		pt.given = 1;
		pt.state = value ? 1 : -1;
		pt.samples_wanted = value && atol(value) > 0 ? atol(value) : PT_SAMPLES_DEFAULT;
		pt.delay = delay && atoi(delay) > 0 ? atoi(delay) : PT_DELAY_DEFAULT;
	}
	if (pt.state <= 0 || pt.state == 4)
		return;
	if (pt.state == 1)
	{
		if (++pt.calls < pt.delay || !global_scenario_try_and_get() || !global_structure_bsp_get())
			return;
		platform_log("pt fit: the static fit study begins (%ld samples)", pt.samples_wanted);
		pt.state = prepare() ? 2 : 4;
		if (pt.state == 4)
			release();
		return;
	}
	if (pt.bsp != global_structure_bsp_index)
	{
		platform_log("pt fit: the BSP changed: the study stopped");
		release();
		pt.state = 4;
		return;
	}
	for (chunk = 0; chunk < PT_CHUNK && pt.samples_done < pt.samples_wanted; chunk++)
		sample_trace(&pt.samples[pt.samples_done++]);
	if (pt.samples_done >= pt.samples_wanted)
	{
		report();
		release();
		pt.state = 4;
	}
}
