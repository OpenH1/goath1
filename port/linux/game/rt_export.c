/*
RT_EXPORT.C

debug.rt_export ("<second> <folder>", HALO_RT_EXPORT): one moment of the
game written to a scene folder in the rt2 truth scene format (local only:
~/dev/rt2-scene-format.md), for a slow, exact path-traced render of it:

	camera.json            the first window's camera as drawn (main.c's)
	geometry/bsp.ply       the loaded structure BSP's render triangles: its
	                       lightmaps' materials' surfaces over their
	                       compressed vertices, every shader's (opaque,
	                       alpha-tested and transparent), each face with its
	                       material and its lightmap page (-1 none)
	geometry/objects/<n>.ply  each object with a model, posed and skinned as
	                       drawn (its nodes' matrices after their default
	                       inverses, two nodes blended a vertex), at its
	                       most detailed level, its regions' permutations as
	                       the object has them; then the first-person weapon
	                       and hands as last drawn. The unit seen from in
	                       first person and what it holds are left out (the
	                       game draws neither)
	geometry/objects.json  what each object file is (its tag, type, counts)
	geometry/lightmaps/<k>.png  the BSP's lightmap pages, 16-bit RGB, each
	                       texel's stored value (the combiners take it as it
	                       is: no sRGB decode on the Xbox)
	materials.json         each shader named: "index" and "tag" alone (the
	                       lights-and-materials helper fills in the rest)
	original.png           the classic renderer's own frame of the moment
	export.json            the game's own counts, to check the files by

The frame is asked for at the setting's second (the cues' clock), before
that frame's tick; the scene is written on the next frame's call, before its
tick, when the objects, the camera and the first-person models still stand
as that frame drew them (with debug.frame_time at a tick, a frame is one
tick: no interpolation between). Read only; render-only.

The model and structure reading follows world_rt_game.c's (its vertex
layouts, node indices and strip winding), all of each mesh kept here.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_group_lookup.h"
#include "cache/cache_files.h"
#include "camera/director.h"
#include "game/game.h"
#include "game/players.h"
#include "models/model_definitions.h"
#include "objects/objects.h"
#include "objects/object_lights_rendering.h"
#include "objects/object_definitions.h"
#include "objects/object_types.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "shaders/shader_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "tag_files/tag_groups.h"

#include "rt_export.h"
#include "world_proxies_source.h"
#include "world_rt.h"

#include "zlib.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtl.h>

/* (the scene folder is a path on this machine, not one of the data root's:
 the host's own fopen and rename, not the game's, which take a path as an
 Xbox one: port/linux/include/stdio.h) */
#undef fopen
#undef rename

int mkdir(const char *path, unsigned int mode);
void platform_log(const char *format, ...);
char const *config_string(char const *name);
boolean tag_index_is_group(long tag_index, long group_tag);
long network_test_cues_start_tick(void);
/* (d3d8_device.c) the next frame presented saved as a PNG in a folder, and
the name it took */
void halo_screenshot_png(const char *directory);
/* (render_cameras.c) */
void render_camera_build_frustum_bounds(struct render_camera const *camera, real_rectangle2d *frustum_bounds);
int halo_screenshot_png_taken(char *name, unsigned long size);
/* (the lights-and-materials helper's, where it is built in) */
extern void rt_lights_materials_write(const char *scene_folder) __attribute__((weak));

/* ---------- constants */

enum
{
	/* the levels of detail (models.c's): the most detailed */
	_rt_export_detail_levels = 5,
	/* a model part's flags (models.c's) */
	_rt_export_part_stripped_bit = 0,
	_rt_export_part_local_nodes_bit = 1,
	/* the materials named at most */
	_rt_export_materials = 8192,
	/* the objects written at most */
	_rt_export_objects = 4096,
	/* the lightmap pages' formats (bitmaps.c's) and the swizzled flag */
	_rt_lightmap_a8 = 0,
	_rt_lightmap_y8,
	_rt_lightmap_ay8,
	_rt_lightmap_a8y8,
	_rt_lightmap_r5g6b5 = 6,
	_rt_lightmap_a1r5g5b5 = 8,
	_rt_lightmap_a4r4g4b4,
	_rt_lightmap_x8r8g8b8,
	_rt_lightmap_a8r8g8b8,
	_rt_lightmap_dxt1 = 14,
	_rt_lightmap_dxt3,
	_rt_lightmap_dxt5,
	_rt_lightmap_swizzled_bit = 3,
	/* the frames waited for the saved frame's name at most */
	_rt_export_png_frames = 120,
};

/* ---------- structures */

/* (world_rt_game.c's: this build's model geometry and part, its shader
reference, the structure's compressed vertex) */
struct rt_export_model_geometry
{
	byte reserved[0x24];
	struct tag_block parts;
};

struct rt_export_model_part
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

struct rt_export_model_shader_reference
{
	struct tag_reference shader;
	short permutation_index;
	word pad;
	long unused[3];
};

struct rt_export_environment_vertex
{
	real_point3d position;
	unsigned long normal, binormal, tangent;
	real_point2d texcoord;
};

typedef char verify_rt_export_model_part[sizeof(struct rt_export_model_part) == 0x68 ? 1 : -1];
typedef char verify_rt_export_environment_vertex[sizeof(struct rt_export_environment_vertex) == 32 ? 1 : -1];

/* a mesh as written: each vertex x y z, nx ny nz, u v, u2 v2, then its
tangent tx ty tz and binormal bx by bz (the vertex's own, which the surface
shader's bump normal is built on: xgpu_spirv.c's tangent * bump.x + binormal
* bump.y + normal * max(bump.z, 0.05)); each face its corners, material and
lightmap page */
#define RT_EXPORT_VERTEX_FLOATS 16
struct rt_export_mesh
{
	float *vertices;
	long vertex_count, vertex_room;
	int32_t *faces;
	long face_count, face_room;
};

/* ---------- globals */

static struct
{
	char setting[600];
	float second;
	char folder[512];
	/* 0 waiting, 1 the frame asked for (the scene next), 2 its picture's
	name awaited, 3 done */
	int phase;
	int png_frames;
	long material_tags[_rt_export_materials];
	long material_count;
	/* the first window's camera as drawn (main.c's), and a copy of it as
	the scene's frame drew it, with that frame's tick, map and label */
	struct render_camera const *camera;
	struct render_camera scene_camera;
	long scene_tick;
	char map[64], label[128];
	/* the saved frame's name (empty: not known yet) and its size (0 not
	read yet) */
	char png_name[128];
	long png_width, png_height;
} rt_export;

/* ---------- private code: files */

static void folder_make(char const *path)
{
	char partial[512];
	size_t index;

	snprintf(partial, sizeof(partial), "%s", path);
	for (index = 1; partial[index]; index++)
	{
		if (partial[index] == '/')
		{
			partial[index] = 0;
			mkdir(partial, 0755);
			partial[index] = '/';
		}
	}
	mkdir(partial, 0755);
}

/* a string in JSON (a tag's backslashes escaped) */
static void json_string(FILE *file, char const *text)
{
	fputc('"', file);
	for (; text && *text; text++)
	{
		if (*text == '"' || *text == '\\')
			fputc('\\', file);
		if ((unsigned char)*text >= 0x20)
			fputc(*text, file);
	}
	fputc('"', file);
}

static char const *tag_name_of(long tag_index)
{
	char const *name = tag_index == NONE ? NULL : tag_get_name(tag_index);

	return name ? name : "none";
}

/* ---------- private code: meshes */

static int mesh_vertex(struct rt_export_mesh *mesh, float const values[RT_EXPORT_VERTEX_FLOATS])
{
	if (mesh->vertex_count >= mesh->vertex_room)
	{
		long room = mesh->vertex_room ? mesh->vertex_room * 2 : 4096;
		float *grown = realloc(mesh->vertices, (size_t)room * RT_EXPORT_VERTEX_FLOATS * sizeof(float));

		if (!grown)
			return FALSE;
		mesh->vertices = grown;
		mesh->vertex_room = room;
	}
	memcpy(mesh->vertices + mesh->vertex_count * RT_EXPORT_VERTEX_FLOATS, values, RT_EXPORT_VERTEX_FLOATS * sizeof(float));
	mesh->vertex_count++;
	return TRUE;
}

static int mesh_face(struct rt_export_mesh *mesh, long a, long b, long c, long material, long lightmap)
{
	int32_t *face;

	if (mesh->face_count >= mesh->face_room)
	{
		long room = mesh->face_room ? mesh->face_room * 2 : 4096;
		int32_t *grown = realloc(mesh->faces, (size_t)room * 5 * sizeof(int32_t));

		if (!grown)
			return FALSE;
		mesh->faces = grown;
		mesh->face_room = room;
	}
	face = mesh->faces + mesh->face_count * 5;
	face[0] = (int32_t)a;
	face[1] = (int32_t)b;
	face[2] = (int32_t)c;
	face[3] = (int32_t)material;
	face[4] = (int32_t)lightmap;
	mesh->face_count++;
	return TRUE;
}

static void mesh_free(struct rt_export_mesh *mesh)
{
	free(mesh->vertices);
	free(mesh->faces);
	memset(mesh, 0, sizeof(*mesh));
}

/* binary little-endian PLY (the format's layout; the BSP's faces carry
their lightmap page too) */
static int mesh_write(struct rt_export_mesh const *mesh, char const *path, int lightmap)
{
	FILE *file = fopen(path, "wb");
	long index;

	if (!file)
		return FALSE;
	fprintf(file, "ply\nformat binary_little_endian 1.0\ncomment GOAT H1 rt_export\n"
		"element vertex %ld\nproperty float x\nproperty float y\nproperty float z\n"
		"property float nx\nproperty float ny\nproperty float nz\nproperty float u\nproperty float v\n"
		"property float u2\nproperty float v2\nproperty float tx\nproperty float ty\nproperty float tz\n"
		"property float bx\nproperty float by\nproperty float bz\nelement face %ld\nproperty list uchar int vertex_indices\n"
		"property int material\n%send_header\n", mesh->vertex_count, mesh->face_count,
		lightmap ? "property int lightmap\n" : "");
	if (mesh->vertex_count)
		fwrite(mesh->vertices, sizeof(float) * RT_EXPORT_VERTEX_FLOATS, (size_t)mesh->vertex_count, file);
	for (index = 0; index < mesh->face_count; index++)
	{
		unsigned char three = 3;

		fwrite(&three, 1, 1, file);
		fwrite(mesh->faces + index * 5, sizeof(int32_t), lightmap ? 5 : 4, file);
	}
	return fclose(file) == 0;
}

/* the self-illumination colors the model pass drew (rasterizer_xbox_models.c's, after its function and
its external color), per object and shader, the last frame's: the truth's model self-illumination */
#define RT_EXPORT_SELF_ILLUMINATIONS 2048
static struct
{
	long object;
	void const *shader;
	real_rgb_color color, change_color;
} rt_export_self_illuminations[RT_EXPORT_SELF_ILLUMINATIONS];
static long rt_export_self_illumination_count;

void rt_export_note_self_illumination(long unique_identifier, void const *shader, real_rgb_color const *color,
	real_rgb_color const *change_color)
{
	long index;

	if (rt_export.phase == 0 || rt_export.phase == 3)
		return;
	for (index = 0; index < rt_export_self_illumination_count; index++)
	{
		if (rt_export_self_illuminations[index].object == unique_identifier &&
			rt_export_self_illuminations[index].shader == shader)
		{
			rt_export_self_illuminations[index].color = *color;
			rt_export_self_illuminations[index].change_color = *change_color;
			return;
		}
	}
	if (rt_export_self_illumination_count < RT_EXPORT_SELF_ILLUMINATIONS)
	{
		rt_export_self_illuminations[rt_export_self_illumination_count].object = unique_identifier;
		rt_export_self_illuminations[rt_export_self_illumination_count].shader = shader;
		rt_export_self_illuminations[rt_export_self_illumination_count].color = *color;
		rt_export_self_illuminations[rt_export_self_illumination_count].change_color = *change_color;
		rt_export_self_illumination_count++;
	}
}

/* a shader's index among the materials (named once each) */
static long material_index(long shader_tag)
{
	long index;

	for (index = 0; index < rt_export.material_count; index++)
	{
		if (rt_export.material_tags[index] == shader_tag)
			return index;
	}
	if (rt_export.material_count >= _rt_export_materials)
		return -1;
	rt_export.material_tags[rt_export.material_count] = shader_tag;
	return rt_export.material_count++;
}

/* an object's static light as the game's model lighting computes it at this tick
(object_lights.c's lights_prepare_for_object_static: the lightmap sampled beneath the object's centre and
four points around it, averaged; its ambient color and two distant lights, the first along the lightmap's
incident direction, the second up from the surface below; the model pass lights a vertex by ambient +
sum of color x max(0, N . -direction), gamma space) */
static void static_lighting_write(FILE *list, long object_index)
{
	struct render_lighting lighting;
	short light;

	memset(&lighting, 0, sizeof(lighting));
	lights_prepare_for_object_static(object_index, &lighting);
	fprintf(list, ",\"static_lighting\":{\"ambient\":[%.6g,%.6g,%.6g],\"distant\":[",
		lighting.ambient_color.red, lighting.ambient_color.green, lighting.ambient_color.blue);
	for (light = 0; light < lighting.distant_light_count && light < MAXIMUM_RENDERED_DISTANT_LIGHTS; light++)
		fprintf(list, "%s{\"color\":[%.6g,%.6g,%.6g],\"direction\":[%.6g,%.6g,%.6g]}", light ? "," : "",
			lighting.distant_lights[light].color.red, lighting.distant_lights[light].color.green,
			lighting.distant_lights[light].color.blue, lighting.distant_lights[light].direction.i,
			lighting.distant_lights[light].direction.j, lighting.distant_lights[light].direction.k);
	fprintf(list, "]}");
}

/* an object's self-illumination and change color as drawn: [{"material", "color", "change_color"}] (gamma
space; rasterizer_xbox_models.c's combiners: light + multipurpose.g x color, then the base map x lerp(1,
change color, multipurpose.a); a shader without a multipurpose map reads 1 there) */
static void self_illumination_write(FILE *list, long object_index)
{
	long index, written = 0;

	for (index = 0; index < rt_export_self_illumination_count; index++)
	{
		long material;

		if (rt_export_self_illuminations[index].object != object_index)
			continue;
		for (material = 0; material < rt_export.material_count; material++)
		{
			if ((void const *)shader_definition_get(rt_export.material_tags[material]) ==
				rt_export_self_illuminations[index].shader)
				break;
		}
		fprintf(list, "%s{\"material\":%ld,\"color\":[%.6g,%.6g,%.6g],\"change_color\":[%.6g,%.6g,%.6g]}",
			written ? "," : ",\"self_illumination\":[", material < rt_export.material_count ? material : -1L,
			rt_export_self_illuminations[index].color.red, rt_export_self_illuminations[index].color.green,
			rt_export_self_illuminations[index].color.blue, rt_export_self_illuminations[index].change_color.red,
			rt_export_self_illuminations[index].change_color.green, rt_export_self_illuminations[index].change_color.blue);
		written++;
	}
	if (written)
		fprintf(list, "]");
}


static void unpacked(unsigned long compressed, float out[3])
{
	union real_vector3d vector = uncompress_int32_to_real_vector3d(compressed);

	out[0] = vector.i;
	out[1] = vector.j;
	out[2] = vector.k;
}

/* a node's matrix as a row-major 3x4 (its columns its axes, scaled, then
its position) */
static void transform_of(real_matrix4x3 const *matrix, float out[3][4])
{
	int row;

	for (row = 0; row < 3; row++)
	{
		out[row][0] = matrix->scale * (&matrix->forward.i)[row];
		out[row][1] = matrix->scale * (&matrix->left.i)[row];
		out[row][2] = matrix->scale * (&matrix->up.i)[row];
		out[row][3] = (&matrix->position.x)[row];
	}
}

static void compose(float const a[3][4], float const b[3][4], float out[3][4])
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

static void apply(float const m[3][4], float const in[3], float out[3], int point)
{
	int row;

	for (row = 0; row < 3; row++)
		out[row] = m[row][0] * in[0] + m[row][1] * in[1] + m[row][2] * in[2] + (point ? m[row][3] : 0.0f);
}

/* a model's part's triangles onto the mesh, its vertices skinned by the
skinning transforms (world space: each node's frame after its default
inverse) */
static long model_part_append(struct rt_export_mesh *mesh, struct model const *model,
	struct rt_export_model_part const *part, float const (*skin)[3][4], short node_count)
{
	struct vertex_buffer const *vertex_buffer = &part->vertex_buffer;
	struct triangle_buffer const *triangle_buffer = &part->triangle_buffer;
	struct rt_export_model_shader_reference const *reference;
	byte *vertices = NULL;
	BYTE *indices = NULL;
	long stride, index_count, index, first_vertex = mesh->vertex_count, added = 0;
	long material;
	float scale[2];

	if (TEST_FLAG(part->flags, _rt_export_part_stripped_bit) || !VALID_INDEX(part->shader_index, model->shaders.count))
		return 0;
	reference = TAG_BLOCK_GET_ELEMENT(&model->shaders, part->shader_index, struct rt_export_model_shader_reference);
	material = material_index(reference->shader.index);
	if (!vertex_buffer->hardware_format || vertex_buffer->count <= 0 || !triangle_buffer->hardware_format ||
		triangle_buffer->count <= 0)
		return 0;
	if (vertex_buffer->type == _rasterizer_vertex_type_model_compressed)
		stride = sizeof(struct model_vertex_compressed);
	else if (vertex_buffer->type == _rasterizer_vertex_type_model_uncompressed)
		stride = sizeof(struct model_vertex_uncompressed);
	else
		return 0;
	if (triangle_buffer->type == _triangle_buffer_type_precompiled_strip)
		index_count = triangle_buffer->count + 2;
	else if (triangle_buffer->type == _triangle_buffer_type_triangles)
		index_count = triangle_buffer->count * 3;
	else
		return 0;
	IDirect3DVertexBuffer8_Lock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format, 0, 0, &vertices, D3DLOCK_READONLY);
	IDirect3DIndexBuffer8_Lock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format, 0, 0, &indices, D3DLOCK_READONLY);
	if (!vertices || !indices)
	{
		if (vertices)
			IDirect3DVertexBuffer8_Unlock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format);
		if (indices)
			IDirect3DIndexBuffer8_Unlock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format);
		return 0;
	}
	/* (a compressed vertex's coordinates scaled by the model's base map
	scale as it is drawn: rasterizer_model_begin_parameters') */
	scale[0] = model->base_map_scale.i != 0.0f ? model->base_map_scale.i : 1.0f;
	scale[1] = model->base_map_scale.j != 0.0f ? model->base_map_scale.j : 1.0f;
	for (index = 0; index < vertex_buffer->count; index++)
	{
		float position[3], normal[3], tangent[3], binormal[3], uv[2], out[RT_EXPORT_VERTEX_FLOATS], a[3], b[3], na[3], nb[3],
			length;
		int frame;
		long node0, node1;
		real weight;
		int axis;

		if (vertex_buffer->type == _rasterizer_vertex_type_model_compressed)
		{
			struct model_vertex_compressed const *in = (void const *)(vertices + index * stride);

			memcpy(position, &in->position, sizeof(position));
			unpacked(in->normal, normal);
			unpacked(in->tangent, tangent);
			unpacked(in->binormal, binormal);
			uv[0] = (float)in->texcoord.x / 32767.0f * scale[0];
			uv[1] = (float)in->texcoord.y / 32767.0f * scale[1];
			node0 = in->nodes[0] / 3;
			node1 = in->nodes[1] / 3;
			weight = (real)in->node_weight * (1.0f / 32767.0f);
		}
		else
		{
			struct model_vertex_uncompressed const *in = (void const *)(vertices + index * stride);

			memcpy(position, &in->position, sizeof(position));
			memcpy(normal, &in->normal, sizeof(normal));
			memcpy(tangent, &in->tangent, sizeof(tangent));
			memcpy(binormal, &in->binormal, sizeof(binormal));
			uv[0] = in->texcoord.x;
			uv[1] = in->texcoord.y;
			node0 = in->nodes[0];
			node1 = in->nodes[1];
			weight = in->node_weights[0];
		}
		/* (a part with its own node map, which this build's part does not
		carry: held to the node its middle is on, as object_mesh.c holds it) */
		if (TEST_FLAG(part->flags, _rt_export_part_local_nodes_bit))
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
		weight = valid_real(weight) ? PIN(weight, 0.0f, 1.0f) : 1.0f;
		apply(skin[node0], position, a, TRUE);
		apply(skin[node1], position, b, TRUE);
		apply(skin[node0], normal, na, FALSE);
		apply(skin[node1], normal, nb, FALSE);
		for (axis = 0; axis < 3; axis++)
		{
			out[axis] = weight * a[axis] + (1.0f - weight) * b[axis];
			out[3 + axis] = weight * na[axis] + (1.0f - weight) * nb[axis];
		}
		length = sqrtf(out[3] * out[3] + out[4] * out[4] + out[5] * out[5]);
		for (axis = 3; axis < 6 && length > 0.0f; axis++)
			out[axis] /= length;
		out[6] = valid_real(uv[0]) ? uv[0] : 0.0f;
		out[7] = valid_real(uv[1]) ? uv[1] : 0.0f;
		out[8] = out[9] = 0.0f;
		/* (the tangent and binormal skinned as the normal is) */
		for (frame = 0; frame < 2; frame++)
		{
			float const *in_vector = frame ? binormal : tangent;
			float *out_vector = out + 10 + 3 * frame;

			apply(skin[node0], in_vector, na, FALSE);
			apply(skin[node1], in_vector, nb, FALSE);
			for (axis = 0; axis < 3; axis++)
				out_vector[axis] = weight * na[axis] + (1.0f - weight) * nb[axis];
			length = sqrtf(out_vector[0] * out_vector[0] + out_vector[1] * out_vector[1] + out_vector[2] * out_vector[2]);
			for (axis = 0; axis < 3 && length > 0.0f; axis++)
				out_vector[axis] /= length;
		}
		if (!mesh_vertex(mesh, out))
			break;
	}
	/* its triangles: a strip's every one (its odd ones turned, its
	degenerate ones left out), or a list's */
	{
		unsigned short const *list = (unsigned short const *)indices;
		long step = triangle_buffer->type == _triangle_buffer_type_precompiled_strip ? 1 : 3;

		for (index = 0; index + 2 < index_count; index += step)
		{
			unsigned short a = list[index], b = list[index + 1], c = list[index + 2];

			if (a == b || b == c || a == c || a >= vertex_buffer->count || b >= vertex_buffer->count ||
				c >= vertex_buffer->count)
				continue;
			if (step == 1 && (index & 1))
			{
				unsigned short swap = b;

				b = c;
				c = swap;
			}
			if (mesh_face(mesh, first_vertex + a, first_vertex + b, first_vertex + c, material, -1))
				added++;
		}
	}
	IDirect3DVertexBuffer8_Unlock((IDirect3DVertexBuffer8 *)vertex_buffer->hardware_format);
	IDirect3DIndexBuffer8_Unlock((IDirect3DIndexBuffer8 *)triangle_buffer->hardware_format);
	return added;
}

/* a model posed on its nodes' matrices (world space): its regions'
permutations (the object's; NULL the first), each at its most detailed
level, onto the mesh; its triangles */
static long model_append(struct rt_export_mesh *mesh, long model_index, byte const *permutations,
	real_matrix4x3 const *nodes, short frame_count)
{
	struct model const *model = model_index != NONE ? model_definition_get(model_index) : NULL;
	float skin[WORLD_RT_NODES][3][4];
	short node_index, node_count, region_index;
	long triangles = 0;

	if (!model || !model->nodes.address || model->nodes.count <= 0 || !model->regions.address ||
		!model->geometries.address || model->geometries.count <= 0 || !nodes || frame_count <= 0)
		return 0;
	node_count = (short)MIN(MIN(model->nodes.count, WORLD_RT_NODES), frame_count);
	for (node_index = 0; node_index < node_count; node_index++)
	{
		struct model_node const *node = TAG_BLOCK_GET_ELEMENT(&model->nodes, node_index, struct model_node);
		float frame[3][4], inverse[3][4];

		transform_of(&nodes[node_index], frame);
		transform_of(&node->runtime_default_inverse_matrix, inverse);
		compose(frame, inverse, skin[node_index]);
	}
	for (region_index = 0; region_index < MIN(model->regions.count, MAXIMUM_REGIONS_PER_OBJECT); region_index++)
	{
		struct model_region const *region = TAG_BLOCK_GET_ELEMENT(&model->regions, region_index, struct model_region);
		struct model_region_permutation const *permutation;
		struct rt_export_model_geometry const *geometry;
		short geometry_index = NONE, level, chosen = permutations ? permutations[region_index] : 0;
		long part_index;

		if (region->permutations.count <= 0 || !region->permutations.address)
			continue;
		if (!VALID_INDEX(chosen, region->permutations.count))
			chosen = 0;
		permutation = TAG_BLOCK_GET_ELEMENT(&region->permutations, chosen, struct model_region_permutation);
		for (level = _rt_export_detail_levels - 1; level >= 0 && !VALID_INDEX(geometry_index, model->geometries.count);
			level--)
		{
			geometry_index = permutation->geometry_indices[level];
		}
		if (!VALID_INDEX(geometry_index, model->geometries.count))
			continue;
		geometry = TAG_BLOCK_GET_ELEMENT(&model->geometries, geometry_index, struct rt_export_model_geometry);
		if (geometry->parts.count <= 0 || geometry->parts.count > MAXIMUM_PARTS_PER_MODEL_GEOMETRY ||
			!geometry->parts.address)
			continue;
		for (part_index = 0; part_index < geometry->parts.count; part_index++)
		{
			triangles += model_part_append(mesh, model,
				TAG_BLOCK_GET_ELEMENT(&geometry->parts, part_index, struct rt_export_model_part),
				(float const (*)[3][4])skin, node_count);
		}
	}
	return triangles;
}

/* ---------- private code: the structure */

/* the loaded BSP's render triangles (every lightmap's materials' surfaces),
and the game's own counts: its surfaces, and its materials' */
static long structure_append(struct rt_export_mesh *mesh, long *bsp_surfaces, long *material_surfaces,
	long *pages)
{
	struct structure_bsp const *structure_bsp = global_scenario_try_and_get() ? global_structure_bsp_get() : NULL;
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long lightmap_vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_lightmap_compressed);
	long lightmap_index;

	*bsp_surfaces = *material_surfaces = *pages = 0;
	if (!structure_bsp || !structure_bsp->lightmaps.address || !structure_bsp->surfaces.address)
		return 0;
	*bsp_surfaces = structure_bsp->surfaces.count;
	for (lightmap_index = 0; lightmap_index < structure_bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap = TAG_BLOCK_GET_ELEMENT(&structure_bsp->lightmaps, lightmap_index,
			struct structure_lightmap);
		long material_number;

		*pages = MAX(*pages, lightmap->bitmap_index + 1);
		if (!lightmap->materials.address)
			continue;
		for (material_number = 0; material_number < lightmap->materials.count; material_number++)
		{
			struct structure_material const *material = TAG_BLOCK_GET_ELEMENT(&lightmap->materials, material_number,
				struct structure_material);
			byte const *vertices = (byte const *)material->compressed_vertex_data.address;
			long vertex_count = material->vertices.count, first_vertex = mesh->vertex_count;
			long surface_index, vertex_index, material_number_written = material_index(material->shader.index);
			byte const *lightmap_vertices = material->lightmap_vertices.count == vertex_count &&
				material->compressed_vertex_data.size >= vertex_count * (vertex_size + lightmap_vertex_size) ?
				vertices + vertex_count * vertex_size : NULL;

			*material_surfaces += MAX(material->surface_count, 0);
			if (!vertices || vertex_count <= 0 || material->compressed_vertex_data.size < vertex_count * vertex_size ||
				material->surface_count <= 0 || material->first_surface_index < 0 ||
				material->first_surface_index > structure_bsp->surfaces.count - material->surface_count)
			{
				continue;
			}
			for (vertex_index = 0; vertex_index < vertex_count; vertex_index++)
			{
				struct rt_export_environment_vertex const *in = (void const *)(vertices + vertex_index * vertex_size);
				float out[RT_EXPORT_VERTEX_FLOATS];

				memcpy(out, &in->position, 3 * sizeof(float));
				unpacked(in->normal, out + 3);
				unpacked(in->tangent, out + 10);
				unpacked(in->binormal, out + 13);
				out[6] = in->texcoord.x;
				out[7] = in->texcoord.y;
				out[8] = out[9] = 0.0f;
				/* (its lightmap vertex: its incident radiosity's normal, then u
				and v, each short (2 s + 1) / 65535: mod_lightmaps.c) */
				if (lightmap_vertices && lightmap->bitmap_index >= 0)
				{
					short uv[2];

					memcpy(uv, lightmap_vertices + vertex_index * lightmap_vertex_size + 4, sizeof(uv));
					out[8] = (2.0f * uv[0] + 1.0f) / 65535.0f;
					out[9] = (2.0f * uv[1] + 1.0f) / 65535.0f;
				}
				if (!mesh_vertex(mesh, out))
					return mesh->face_count;
			}
			for (surface_index = 0; surface_index < material->surface_count; surface_index++)
			{
				struct structure_surface const *surface = TAG_BLOCK_GET_ELEMENT(&structure_bsp->surfaces,
					material->first_surface_index + surface_index, struct structure_surface);

				if (surface->vertex_indices[0] >= vertex_count || surface->vertex_indices[1] >= vertex_count ||
					surface->vertex_indices[2] >= vertex_count)
				{
					continue;
				}
				mesh_face(mesh, first_vertex + surface->vertex_indices[0], first_vertex + surface->vertex_indices[1],
					first_vertex + surface->vertex_indices[2], material_number_written,
					lightmap_vertices ? lightmap->bitmap_index : -1);
			}
		}
	}
	return mesh->face_count;
}

/* ---------- private code: the lightmap pages */

static void swizzle_masks(long width, long height, unsigned long *x_mask, unsigned long *y_mask)
{
	unsigned long bit = 1, mask_bit = 1;
	int more = TRUE;

	*x_mask = *y_mask = 0;
	while (more)
	{
		more = FALSE;
		if (bit < (unsigned long)width)
		{
			*x_mask |= mask_bit;
			mask_bit <<= 1;
			more = TRUE;
		}
		if (bit < (unsigned long)height)
		{
			*y_mask |= mask_bit;
			mask_bit <<= 1;
			more = TRUE;
		}
		bit <<= 1;
	}
}

static unsigned long swizzle_spread(unsigned long mask, unsigned long value)
{
	unsigned long result = 0, bit;

	for (bit = 1; value && bit; bit <<= 1)
	{
		if (mask & bit)
		{
			if (value & 1)
				result |= bit;
			value >>= 1;
		}
	}
	return result;
}

static unsigned long read16(byte const *bytes)
{
	return (unsigned long)bytes[0] | ((unsigned long)bytes[1] << 8);
}

static void rgb_565(unsigned long value, int rgb[3])
{
	long red = (value >> 11) & 31, green = (value >> 5) & 63, blue = value & 31;

	rgb[0] = (int)((red << 3) | (red >> 2));
	rgb[1] = (int)((green << 2) | (green >> 4));
	rgb[2] = (int)((blue << 3) | (blue >> 2));
}

/* a page's first level's bytes (0: a format not read) */
static long page_size(struct bitmap_data const *bitmap)
{
	long pixels = (long)bitmap->width * bitmap->height;

	switch (bitmap->format)
	{
	case _rt_lightmap_a8: case _rt_lightmap_y8: case _rt_lightmap_ay8:
		return pixels;
	case _rt_lightmap_a8y8: case _rt_lightmap_r5g6b5: case _rt_lightmap_a1r5g5b5: case _rt_lightmap_a4r4g4b4:
		return pixels * 2;
	case _rt_lightmap_x8r8g8b8: case _rt_lightmap_a8r8g8b8:
		return pixels * 4;
	case _rt_lightmap_dxt1:
		return ((bitmap->width + 3) / 4) * ((bitmap->height + 3) / 4) * 8;
	case _rt_lightmap_dxt3: case _rt_lightmap_dxt5:
		return ((bitmap->width + 3) / 4) * ((bitmap->height + 3) / 4) * 16;
	}
	return 0;
}

/* a page's texels as 8-bit RGB (width * height * 3) */
static int page_decode(struct bitmap_data const *bitmap, byte const *pixels, byte *rgb)
{
	long width = bitmap->width, height = bitmap->height, x, y;
	unsigned long x_mask, y_mask;
	int swizzled = TEST_FLAG(bitmap->flags, _rt_lightmap_swizzled_bit) != 0;

	if (bitmap->format >= _rt_lightmap_dxt1)
	{
		long blocks_wide = (width + 3) / 4, block_size = bitmap->format == _rt_lightmap_dxt1 ? 8 : 16;

		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				/* (the color block is the last 8 bytes of a DXT3 or 5 block) */
				byte const *block = pixels + ((y / 4) * blocks_wide + x / 4) * block_size + (block_size - 8);
				unsigned long first = read16(block), second = read16(block + 2);
				unsigned long bits = (unsigned long)block[4] | ((unsigned long)block[5] << 8) |
					((unsigned long)block[6] << 16) | ((unsigned long)block[7] << 24);
				int colors[4][3], channel, which = (int)((bits >> (2 * ((y & 3) * 4 + (x & 3)))) & 3);

				rgb_565(first, colors[0]);
				rgb_565(second, colors[1]);
				for (channel = 0; channel < 3; channel++)
				{
					if (bitmap->format != _rt_lightmap_dxt1 || first > second)
					{
						colors[2][channel] = (2 * colors[0][channel] + colors[1][channel]) / 3;
						colors[3][channel] = (colors[0][channel] + 2 * colors[1][channel]) / 3;
					}
					else
					{
						colors[2][channel] = (colors[0][channel] + colors[1][channel]) / 2;
						colors[3][channel] = 0;
					}
					rgb[(y * width + x) * 3 + channel] = (byte)colors[which][channel];
				}
			}
		}
		return TRUE;
	}
	swizzle_masks(width, height, &x_mask, &y_mask);
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			long at = swizzled ? (long)(swizzle_spread(x_mask, (unsigned long)x) | swizzle_spread(y_mask, (unsigned long)y)) :
				y * width + x;
			int color[3];

			switch (bitmap->format)
			{
			case _rt_lightmap_a8: case _rt_lightmap_y8: case _rt_lightmap_ay8:
				color[0] = color[1] = color[2] = pixels[at];
				break;
			case _rt_lightmap_a8y8:
				color[0] = color[1] = color[2] = pixels[at * 2];
				break;
			case _rt_lightmap_r5g6b5:
				rgb_565(read16(pixels + at * 2), color);
				break;
			case _rt_lightmap_a1r5g5b5:
			{
				unsigned long texel = read16(pixels + at * 2);

				color[0] = (int)(((texel >> 10) & 31) * 255 / 31);
				color[1] = (int)(((texel >> 5) & 31) * 255 / 31);
				color[2] = (int)((texel & 31) * 255 / 31);
				break;
			}
			case _rt_lightmap_a4r4g4b4:
			{
				unsigned long texel = read16(pixels + at * 2);

				color[0] = (int)(((texel >> 8) & 15) * 17);
				color[1] = (int)(((texel >> 4) & 15) * 17);
				color[2] = (int)((texel & 15) * 17);
				break;
			}
			default:
				color[0] = pixels[at * 4 + 2];
				color[1] = pixels[at * 4 + 1];
				color[2] = pixels[at * 4];
				break;
			}
			rgb[(y * width + x) * 3] = (byte)color[0];
			rgb[(y * width + x) * 3 + 1] = (byte)color[1];
			rgb[(y * width + x) * 3 + 2] = (byte)color[2];
		}
	}
	return TRUE;
}

static void put32(byte *at, unsigned long value)
{
	at[0] = (byte)(value >> 24);
	at[1] = (byte)(value >> 16);
	at[2] = (byte)(value >> 8);
	at[3] = (byte)value;
}

static void png_chunk(FILE *file, char const *type, byte const *data, unsigned long size)
{
	byte word[4];
	uLong crc = crc32(0L, Z_NULL, 0);

	put32(word, size);
	fwrite(word, 1, 4, file);
	fwrite(type, 1, 4, file);
	if (size)
		fwrite(data, 1, size, file);
	crc = crc32(crc, (Bytef const *)type, 4);
	if (size)
		crc = crc32(crc, data, (uInt)size);
	put32(word, (unsigned long)crc);
	fwrite(word, 1, 4, file);
}

/* 8-bit RGB as a 16-bit RGB PNG (each value v * 257) */
static int png_write16(char const *path, byte const *rgb, long width, long height)
{
	static byte const signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	unsigned long row_bytes = (unsigned long)width * 6 + 1, raw_size = row_bytes * (unsigned long)height;
	uLongf packed_size = raw_size + raw_size / 100 + 64;
	byte *raw = malloc(raw_size), *packed = malloc(packed_size), header[13];
	FILE *file;
	long x, y;
	int written = FALSE;

	if (raw && packed)
	{
		for (y = 0; y < height; y++)
		{
			byte *row = raw + y * row_bytes;

			row[0] = 0;
			for (x = 0; x < width * 3; x++)
				row[1 + x * 2] = row[2 + x * 2] = rgb[y * width * 3 + x];
		}
		if (compress2(packed, &packed_size, raw, raw_size, 6) == Z_OK && (file = fopen(path, "wb")) != NULL)
		{
			put32(header, (unsigned long)width);
			put32(header + 4, (unsigned long)height);
			header[8] = 16;
			header[9] = 2;
			header[10] = header[11] = header[12] = 0;
			fwrite(signature, 1, sizeof(signature), file);
			png_chunk(file, "IHDR", header, sizeof(header));
			png_chunk(file, "IDAT", packed, packed_size);
			png_chunk(file, "IEND", NULL, 0);
			written = fclose(file) == 0;
		}
	}
	free(raw);
	free(packed);
	return written;
}

/* the BSP's lightmap pages written: how many */
static long lightmaps_write(char const *folder, long pages)
{
	struct structure_bsp const *structure_bsp = global_structure_bsp_get();
	long group = structure_bsp ? structure_bsp->lightmap_group.index : NONE;
	long page, written = 0;

	if (group == NONE || !tag_index_is_group(group, BITMAP_GROUP_TAG))
	{
		platform_log("rt export: the BSP's lightmap bitmap %08lx not read (pages %ld)", (unsigned long)group, pages);
		return 0;
	}
	for (page = 0; page < pages; page++)
	{
		struct bitmap_data *bitmap = bitmap_group_try_and_get_bitmap(group, (short)page);
		long size = bitmap ? page_size(bitmap) : 0;
		long room = (size + 0x1FF) & ~0x1FFL;
		byte *bytes, *rgb;
		boolean done = FALSE;
		char path[700];

		if (!size || bitmap->pixels_offset < 0 || bitmap->width <= 0 || bitmap->height <= 0)
		{
			platform_log("rt export: lightmap page %ld not read (format %d)", page, bitmap ? bitmap->format : -1);
			continue;
		}
		bytes = malloc((size_t)room);
		rgb = malloc((size_t)bitmap->width * bitmap->height * 3);
		if (bytes && rgb)
		{
			cache_file_read(bitmap->tag_index, bitmap->pixels_offset, room, bytes, &done, TRUE);
			while (!done)
				cache_file_read_wait();
			snprintf(path, sizeof(path), "%s/geometry/lightmaps/%ld.png", folder, page);
			if (done == TRUE && page_decode(bitmap, bytes, rgb) && png_write16(path, rgb, bitmap->width, bitmap->height))
				written++;
		}
		free(bytes);
		free(rgb);
	}
	return written;
}

/* ---------- private code: the scene */

/* the unit a local player sees from in first person (NONE none) */
static long first_person_unit(void)
{
	if (local_player_get_player_index(0) == NONE ||
		director_get_perspective(0) != _director_perspective_first_person)
		return NONE;
	return director_get_followed_unit(0);
}

/* the object is the unit or held, at any depth, by it */
static int object_under(long object_index, long unit_index)
{
	int depth;

	for (depth = 0; depth < 16 && object_index != NONE; depth++)
	{
		struct object_datum *object = (struct object_datum *)object_try_and_get(object_index);

		if (object_index == unit_index)
			return TRUE;
		if (!object)
			return FALSE;
		object_index = object->object.parent_object_index;
	}
	return FALSE;
}

static void camera_write(char const *folder, char const *map, char const *label, long tick)
{
	struct render_camera const *camera = &rt_export.scene_camera;
	real_rectangle2d bounds;
	float viewport_width, viewport_height, half_width, half_height, tangent_v, tangent_h;
	char path[600];
	FILE *file;

	/* (the projection's tangents as render_camera_build_frustum makes them
	of the frustum's bounds) */
	render_camera_build_frustum_bounds(camera, &bounds);
	viewport_width = (float)(camera->viewport_bounds.x1 - camera->viewport_bounds.x0);
	viewport_height = (float)(camera->viewport_bounds.y1 - camera->viewport_bounds.y0);
	half_width = (bounds.x1 - bounds.x0) * 0.5f;
	half_height = (bounds.y1 - bounds.y0) * 0.5f;
	tangent_v = tanf(camera->vertical_field_of_view * 0.5f) * half_height;
	tangent_h = viewport_height > 0.0f ? tanf(camera->vertical_field_of_view * 0.5f) * half_width / viewport_height *
		viewport_width : 0.0f;
	snprintf(path, sizeof(path), "%s/camera.json", folder);
	file = fopen(path, "w");
	if (!file)
		return;
	fprintf(file, "{\"map\":");
	json_string(file, map);
	fprintf(file, ",\"label\":");
	json_string(file, label);
	fprintf(file, ",\"tick\":%ld,\n \"position\":[%.6f,%.6f,%.6f],\"forward\":[%.6f,%.6f,%.6f],\"up\":[%.6f,%.6f,%.6f],\n",
		tick, camera->position.x, camera->position.y, camera->position.z, camera->forward.i, camera->forward.j,
		camera->forward.k, camera->up.i, camera->up.j, camera->up.k);
	fprintf(file, " \"fov_h_deg\":%.5f,\"fov_v_deg\":%.5f,\"width\":%d,\"height\":%d,\n",
		2.0 * atan(tangent_h) * 180.0 / 3.14159265358979, 2.0 * atan(tangent_v) * 180.0 / 3.14159265358979,
		rt_export.png_width ? (int)rt_export.png_width : (int)viewport_width,
		rt_export.png_height ? (int)rt_export.png_height : (int)viewport_height);
	fprintf(file, " \"viewport_logical\":[%d,%d],\"size_from\":\"%s\",\n", (int)viewport_width, (int)viewport_height,
		rt_export.png_width ? "original.png" : "the window's logical viewport (original.png not read yet)");
	fprintf(file, " \"vertical_field_of_view_rad\":%.6f,\"frustum_bounds\":[%.6f,%.6f,%.6f,%.6f],\"z_near\":%.5f,\"z_far\":%.3f,\n",
		camera->vertical_field_of_view, bounds.x0, bounds.x1, bounds.y0, bounds.y1, camera->z_near, camera->z_far);
	fprintf(file, " \"exposure_note\":\"the classic renderer: lit in its combiners at 8 bits a channel and written "
		"as it is (no tone map, no sRGB encode); the display's gamma (display.gamma / the brightness setting) "
		"applied after. original.png includes the HUD unless the run hid it; the sky is drawn but not exported. "
		"Objects are at their most detailed level, which may differ from the level drawn for a distant one; "
		"the first-person weapon and hands are placed in world space from their nodes as drawn\"}\n");
	fclose(file);
}

static void scene_write(void)
{
	struct rt_export_mesh mesh;
	struct object_iterator iterator;
	char path[700], map[64], label[128];
	char const *scenario_name = tag_name_of(global_scenario_index), *slash, *base;
	long bsp_triangles, bsp_surfaces, material_surfaces, pages, lightmap_pages, object_count = 0, objects_seen = 0;
	long objects_no_model = 0, objects_first_person = 0, objects_hidden = 0, object_triangles = 0, index;
	long fp_unit = first_person_unit();
	FILE *list, *file;
	int which;

	/* (the map's short name; the label the folder's name after it) */
	slash = strrchr(scenario_name, '\\');
	snprintf(map, sizeof(map), "%s", slash ? slash + 1 : scenario_name);
	base = strrchr(rt_export.folder, '/');
	base = base ? base + 1 : rt_export.folder;
	snprintf(label, sizeof(label), "%s", strncmp(base, map, strlen(map)) == 0 && base[strlen(map)] == '_' ?
		base + strlen(map) + 1 : base);
	snprintf(path, sizeof(path), "%s/geometry/objects", rt_export.folder);
	folder_make(path);
	snprintf(path, sizeof(path), "%s/geometry/lightmaps", rt_export.folder);
	folder_make(path);
	rt_export.material_count = 0;
	snprintf(rt_export.map, sizeof(rt_export.map), "%s", map);
	snprintf(rt_export.label, sizeof(rt_export.label), "%s", label);
	rt_export.scene_tick = game_time_get();
	if (rt_export.camera)
		rt_export.scene_camera = *rt_export.camera;
	camera_write(rt_export.folder, map, label, rt_export.scene_tick);

	/* the structure */
	memset(&mesh, 0, sizeof(mesh));
	bsp_triangles = structure_append(&mesh, &bsp_surfaces, &material_surfaces, &pages);
	snprintf(path, sizeof(path), "%s/geometry/bsp.ply", rt_export.folder);
	mesh_write(&mesh, path, TRUE);
	mesh_free(&mesh);
	lightmap_pages = lightmaps_write(rt_export.folder, pages);

	/* the objects */
	snprintf(path, sizeof(path), "%s/geometry/objects.json", rt_export.folder);
	list = fopen(path, "w");
	if (list)
		fprintf(list, "[");
	object_iterator_new(&iterator, _object_mask_all, 0);
	while (object_iterator_next(&iterator) && object_count < _rt_export_objects)
	{
		long object_index = iterator.index;
		struct object_datum *object = (struct object_datum *)object_try_and_get(object_index);
		struct object_definition const *definition = object ? object_definition_get(object->definition_index) : NULL;
		real_matrix4x3 const *nodes;
		short node_count;
		long triangles;

		objects_seen++;
		if (!object || !definition || definition->object.model.index == NONE)
		{
			objects_no_model++;
			continue;
		}
		if (TEST_FLAG(object->object.flags, _object_invisible_bit))
		{
			objects_hidden++;
			continue;
		}
		if (fp_unit != NONE && object_under(object_index, fp_unit))
		{
			objects_first_person++;
			continue;
		}
		nodes = object_get_node_matrices(object_index);
		node_count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
		memset(&mesh, 0, sizeof(mesh));
		triangles = model_append(&mesh, definition->object.model.index, object->object.region_permutations, nodes,
			node_count);
		if (triangles > 0)
		{
			snprintf(path, sizeof(path), "%s/geometry/objects/%ld.ply", rt_export.folder, object_count);
			mesh_write(&mesh, path, FALSE);
			if (list)
			{
				fprintf(list, "%s\n {\"file\":\"objects/%ld.ply\",\"tag\":", object_count ? "," : "", object_count);
				json_string(list, tag_name_of(object->definition_index));
				fprintf(list, ",\"model\":");
				json_string(list, tag_name_of(definition->object.model.index));
				fprintf(list, ",\"type\":%d,\"id\":%ld,\"parent\":%ld,\"vertices\":%ld,\"triangles\":%ld",
					object->object.type, object_index, object->object.parent_object_index, mesh.vertex_count, triangles);
				self_illumination_write(list, object_index);
				static_lighting_write(list, object_index);
				fprintf(list, "}");
			}
			object_count++;
			object_triangles += triangles;
		}
		mesh_free(&mesh);
	}
	/* the first-person weapon (0) and hands (1) as last drawn */
	for (which = 0; which < 2; which++)
	{
		struct world_proxies_frame frames[WORLD_RT_NODES];
		real_matrix4x3 nodes[WORLD_RT_NODES];
		long model, unit, triangles;
		short node_count = world_rt_source_first_person(which, &model, &unit, frames, WORLD_RT_NODES), node_index;

		if (node_count <= 0)
			continue;
		/* (back to the nodes' matrices: each axis's scale the forward's) */
		for (node_index = 0; node_index < node_count; node_index++)
		{
			struct world_proxies_frame const *frame = &frames[node_index];
			real scale = sqrtf(frame->axes[0][0] * frame->axes[0][0] + frame->axes[0][1] * frame->axes[0][1] +
				frame->axes[0][2] * frame->axes[0][2]);
			real inverse = scale > 0.0f ? 1.0f / scale : 1.0f;

			nodes[node_index].scale = scale;
			nodes[node_index].forward.i = frame->axes[0][0] * inverse;
			nodes[node_index].forward.j = frame->axes[0][1] * inverse;
			nodes[node_index].forward.k = frame->axes[0][2] * inverse;
			nodes[node_index].left.i = frame->axes[1][0] * inverse;
			nodes[node_index].left.j = frame->axes[1][1] * inverse;
			nodes[node_index].left.k = frame->axes[1][2] * inverse;
			nodes[node_index].up.i = frame->axes[2][0] * inverse;
			nodes[node_index].up.j = frame->axes[2][1] * inverse;
			nodes[node_index].up.k = frame->axes[2][2] * inverse;
			nodes[node_index].position.x = frame->position[0];
			nodes[node_index].position.y = frame->position[1];
			nodes[node_index].position.z = frame->position[2];
		}
		memset(&mesh, 0, sizeof(mesh));
		triangles = model_append(&mesh, model, NULL, nodes, node_count);
		if (triangles > 0)
		{
			snprintf(path, sizeof(path), "%s/geometry/objects/%ld.ply", rt_export.folder, object_count);
			mesh_write(&mesh, path, FALSE);
			if (list)
			{
				fprintf(list, "%s\n {\"file\":\"objects/%ld.ply\",\"tag\":", object_count ? "," : "", object_count);
				json_string(list, tag_name_of(model));
				fprintf(list, ",\"first_person\":\"%s\",\"vertices\":%ld,\"triangles\":%ld}", which ? "hands" : "weapon",
					mesh.vertex_count, triangles);
			}
			object_count++;
			object_triangles += triangles;
		}
		mesh_free(&mesh);
	}
	if (list)
	{
		fprintf(list, "\n]\n");
		fclose(list);
	}

	/* the materials: index and tag alone */
	snprintf(path, sizeof(path), "%s/materials.json", rt_export.folder);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "[");
		for (index = 0; index < rt_export.material_count; index++)
		{
			fprintf(file, "%s\n {\"index\":%ld,\"tag\":", index ? "," : "", index);
			json_string(file, tag_name_of(rt_export.material_tags[index]));
			fprintf(file, "}");
		}
		fprintf(file, "\n]\n");
		fclose(file);
	}

	/* the game's own counts */
	snprintf(path, sizeof(path), "%s/export.json", rt_export.folder);
	file = fopen(path, "w");
	if (file)
	{
		fprintf(file, "{\"scenario\":");
		json_string(file, scenario_name);
		fprintf(file, ",\"bsp_index\":%d,\"tick\":%ld,\n \"bsp_surfaces\":%ld,\"bsp_material_surfaces\":%ld,"
			"\"bsp_triangles_written\":%ld,\"lightmap_pages\":%ld,\"lightmap_pages_written\":%ld,\n"
			" \"objects_in_game\":%ld,\"objects_no_model\":%ld,\"objects_hidden\":%ld,\"objects_first_person_unit\":%ld,"
			"\"object_files\":%ld,\"object_triangles\":%ld,\"materials\":%ld}\n",
			global_structure_bsp_index, game_time_get(), bsp_surfaces, material_surfaces, bsp_triangles, pages,
			lightmap_pages, objects_seen, objects_no_model, objects_hidden, objects_first_person, object_count,
			object_triangles, rt_export.material_count);
		fclose(file);
	}
	platform_log("rt export: %s at tick %ld: BSP %ld triangles (its surfaces %ld), %ld lightmap pages, %ld objects "
		"(%ld triangles; of %ld in the game), %ld materials", rt_export.folder, game_time_get(), bsp_triangles,
		bsp_surfaces, lightmap_pages, object_count, object_triangles, objects_seen, rt_export.material_count);
	if (rt_lights_materials_write)
		rt_lights_materials_write(rt_export.folder);
}

/* the frame saved renamed original.png, once its name is known and it is
on the disk (the backend may still be writing it), and camera.json written
again with its size: TRUE when done */
static int original_named(void)
{
	char from[700], to[700];
	FILE *file;
	byte header[24];

	if (!rt_export.png_name[0])
	{
		if (!halo_screenshot_png_taken(rt_export.png_name, sizeof(rt_export.png_name)))
			return FALSE;
		if (!rt_export.png_name[0])
		{
			platform_log("rt export: the frame was not saved");
			return TRUE;
		}
	}
	snprintf(from, sizeof(from), "%s/%s", rt_export.folder, rt_export.png_name);
	snprintf(to, sizeof(to), "%s/original.png", rt_export.folder);
	if (rename(from, to) != 0)
		return FALSE;
	file = fopen(to, "rb");
	if (file && fread(header, 1, sizeof(header), file) == sizeof(header) && memcmp(header + 12, "IHDR", 4) == 0)
	{
		rt_export.png_width = ((long)header[16] << 24) | ((long)header[17] << 16) | ((long)header[18] << 8) | header[19];
		rt_export.png_height = ((long)header[20] << 24) | ((long)header[21] << 16) | ((long)header[22] << 8) | header[23];
		camera_write(rt_export.folder, rt_export.map, rt_export.label, rt_export.scene_tick);
	}
	if (file)
		fclose(file);
	return TRUE;
}

/* ---------- public code */

void rt_export_frame(int main_menu_loaded, void const *first_window_camera)
{
	char const *setting = config_string("debug.rt_export");
	long start;

	if (!setting || !setting[0])
		return;
	rt_export.camera = first_window_camera;
	if (strcmp(setting, rt_export.setting) != 0)
	{
		snprintf(rt_export.setting, sizeof(rt_export.setting), "%s", setting);
		rt_export.phase = sscanf(setting, "%f %511s", &rt_export.second, rt_export.folder) == 2 ? 0 : 3;
		if (rt_export.phase)
			platform_log("rt export: debug.rt_export \"<second> <folder>\", not \"%s\"", setting);
	}
	if (rt_export.phase >= 3 || main_menu_loaded || !game_in_progress())
		return;
	switch (rt_export.phase)
	{
	case 0:
		start = network_test_cues_start_tick();
		if (start == NONE || game_time_get() < start + (long)(rt_export.second * TICKS_PER_SECOND + 0.5f))
			return;
		folder_make(rt_export.folder);
		/* (the frame drawn after this tick saved; the scene next frame) */
		halo_screenshot_png(rt_export.folder);
		rt_export.phase = 1;
		break;
	case 1:
		rt_export.png_name[0] = 0;
		rt_export.png_width = rt_export.png_height = 0;
		scene_write();
		rt_export.phase = 2;
		rt_export.png_frames = 0;
		/* (and its name, most likely known already) */
		__attribute__((fallthrough));
	case 2:
		if (original_named())
			rt_export.phase = 3;
		else if (++rt_export.png_frames > _rt_export_png_frames)
		{
			platform_log("rt export: the frame %s not renamed original.png", rt_export.png_name);
			rt_export.phase = 3;
		}
		break;
	}
}
