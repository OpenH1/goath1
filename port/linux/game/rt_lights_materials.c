/*
RT_LIGHTS_MATERIALS.C

The rt2 truth scene's lights and materials (the rt2 scene format, local:
materials.json and lights.json). Everything is read from the map's tags as
they are: each number is written beside the tag field it came from (a
"from" note), and where the field's meaning is not plain, the code that
reads it in the game is named.

- materials.json: the engine export writes each row's "index" and "tag";
  this fills in the rest from the shader tag: its type, its maps (written
  once each to the textures folder beside the scene, as 16-bit PNG), how
  the original combines them, its specular and reflection, alpha test and
  two-sidedness, its run-time self-illumination, and its radiosity
  properties (the emission Bungie's lightmap tool lit the level with).
- lights.json: the skies' lights and ambient radiosity, every light datum
  at this tick (object lights, light fixtures, effects, flashlights), every
  light tag of the map (the flashlight's gel and its fields among them),
  the game's distance attenuation, and what the map's lightmaps were baked
  from (its emitting shaders, their power and area).

Textures: "<tag>.lin.png" holds red, green and blue decoded by a pure 2.2
power (docs/rt-plan/4-path-tracing.md's decode, linear light) and alpha as
stored; "<tag>.raw.png" holds the texels as stored (bump maps: vectors). A
cube map is its six faces left to right in D3D's order (+x -x +y -y +z -z),
a volume its slices top to bottom.

Nothing is changed in the game: it is read only, on the game's thread.
*/

#include "cseries.h"
#include "real_math.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "objects/object_types.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "rasterizer/common/rasterizer_common.h"
#include "rasterizer/rasterizer_geometry.h"
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_lights.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "scenario/sky_definitions.h"
#include "structures/structure_bsp_definitions.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "cache/texture_cache.h"
#include "shaders/shader_definitions.h"
#include "shaders/shader_texture_animation.h"
#include "tag_files/tag_groups.h"
#include "memory/zlib/zlib.h"
#include "interface/first_person_weapons.h"
#include "math/periodic_functions.h"
#include "rt_lights_materials.h"

#include <xtl.h>

#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* (the C library's own fopen: the scene folder is a whole host path, as
mkdir takes it, not an Xbox path for port/linux/include/stdio.h to
translate into the data root) */
#undef fopen

/* (cache_files.c) */
boolean tag_index_is_group(long tag_index, long group_tag);
void platform_log(const char *format, ...);
/* (the C library's, as world_sdf.c declares it) */
int mkdir(const char *path, unsigned int mode);
/* (object_lights.c) */
extern struct data_array *light_data;
/* (bitmaps.c: the palettized bump maps' palette, which the rasterizer gives
the device, rasterizer_xbox.c) */
extern pixel32 global_vector_palette[256];

/* (xbox_texture_decode.c, as port/linux/src/xgpu.h declares them; that
header is the hosts', so its texture description is repeated here) */
struct xgpu_texture_description
{
	DWORD format;
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;
	BOOL compressed;
	unsigned long pitch;
	BOOL hires;
	BOOL hires_coverage;
	BOOL hires_point_threshold;
	BOOL hires_msdf;
};
void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
BOOL xgpu_texture_decode_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const DWORD *palette, DWORD *destination);
void xgpu_texture_decode_dxt(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, DWORD *destination);

/* ---------- constants */

enum
{
	_rtlm_shader_screen = 0,
	_rtlm_shader_effect,
	_rtlm_shader_decal,
	_rtlm_shader_environment,
	_rtlm_shader_model,
	_rtlm_shader_transparent_generic,
	_rtlm_shader_transparent_chicago,
	_rtlm_shader_transparent_water,
	_rtlm_shader_transparent_glass,
	_rtlm_shader_transparent_meter,
	_rtlm_shader_transparent_plasma,
	RTLM_SHADER_TYPES,
	RTLM_LIGHT_TAG = 'ligh',
	RTLM_TEXTURES = 1024,
	RTLM_MATERIALS = 4096,
	RTLM_FALLOFF = 33,
	RTLM_DEFAULT_TICK = 300,
};

#define RTLM_PI 3.14159265358979f
#define RTLM_DEGREES(radians) ((radians) * (180.0f / RTLM_PI))

static char const *const rtlm_shader_type_names[RTLM_SHADER_TYPES] =
{
	"screen", "effect", "decal", "environment", "model", "transparent_generic", "transparent_chicago",
	"transparent_water", "transparent_glass", "transparent_meter", "transparent_plasma",
};

static char const *const rtlm_detail_functions[3] = { "double_biased_multiply", "multiply", "double_biased_add" };
static char const *const rtlm_radiosity_levels[4] = { "high", "medium", "low", "turd" };

/* ---------- the tags' layouts (each from the file named, which reads it) */

/* (rasterizer_xbox_environment.c) */
struct rtlm_environment_diffuse
{
	word flags;
	short pad02;
	byte reserved04[0x18];
	struct tag_reference base_map;
	byte reserved2C[0x18];
	short detail_map_function;
	short pad46;
	real primary_detail_map_scale;
	struct tag_reference primary_detail_map;
	real secondary_detail_map_scale;
	struct tag_reference secondary_detail_map;
	byte reserved70[0x18];
	short micro_detail_map_function;
	short pad8A;
	real micro_detail_map_scale;
	struct tag_reference micro_detail_map;
	real_rgb_color material_color;
	byte reservedAC[0xC];
	real bump_map_scale;
	struct tag_reference bump_map;
	real_vector2d runtime_bump_map_scale;
	byte reservedD4[0x40];
};

struct rtlm_environment_self_illumination
{
	word flags;
	short pad02;
	byte reserved04[0x18];
	real_rgb_color primary_on_color;
	real_rgb_color primary_off_color;
	short primary_animation_function;
	short pad36;
	real primary_animation_period;
	real primary_animation_phase;
	byte reserved40[0x18];
	real_rgb_color secondary_on_color;
	real_rgb_color secondary_off_color;
	short secondary_animation_function;
	short pad72;
	real secondary_animation_period;
	real secondary_animation_phase;
	byte reserved7C[0x18];
	real_rgb_color plasma_on_color;
	real_rgb_color plasma_off_color;
	short plasma_animation_function;
	short padAE;
	real plasma_animation_period;
	real plasma_animation_phase;
	byte reservedB8[0x18];
	real map_scale;
	struct tag_reference map;
};

struct rtlm_environment_specular
{
	word flags;
	short type;
	long unused04[4];
	real brightness;
	long unused18[5];
	real_rgb_color view_perpendicular_color;
	real_rgb_color view_parallel_color;
	long unused44[4];
};

struct rtlm_environment_reflection
{
	word flags;
	short type;
	real lightmap_brightness_scale;
	long unused1[7];
	real view_perpendicular_brightness;
	real view_parallel_brightness;
	long unused2[4];
	real mirror_index_of_refraction;
	real mirror_depth;
	long unused3[4];
	struct tag_reference cube_map;
	long unused4[4];
};

struct rtlm_shader_environment
{
	struct shader shader;
	word flags;
	short type;
	real lens_flare_spacing;
	struct tag_reference lens_flare;
	long unused[11];
	struct rtlm_environment_diffuse diffuse;
	struct rtlm_environment_self_illumination self_illumination;
	byte reserved23C[0x18];
	struct rtlm_environment_specular specular;
	struct rtlm_environment_reflection reflection;
};

typedef char rtlm_environment_base_map_assert[
	offsetof(struct rtlm_shader_environment, diffuse.base_map) == 0x88 ? 1 : -1];

/* (rasterizer_xbox_models.c) */
struct rtlm_shader_model
{
	struct shader shader;
	word flags;
	short type;
	byte reserved04[0xC];
	real translucency;
	byte reserved14[0x10];
	short change_color_source;
	byte reserved26[0x1E];
	word self_illumination_flags;
	short pad46;
	short self_illumination_color_source;
	short self_illumination_animation_function;
	real self_illumination_animation_period;
	real_rgb_color self_illumination_animation_color_lower_bound;
	real_rgb_color self_illumination_animation_color_upper_bound;
	byte reserved68[0xC];
	real map_u_scale;
	real map_v_scale;
	struct tag_reference base_map;
	byte reserved8C[8];
	struct tag_reference multipurpose_map;
	byte reservedA4[8];
	short detail_function;
	short detail_mask;
	real detail_map_scale;
	struct tag_reference detail_map;
	real detail_map_v_scale;
	byte reservedC8[0xC];
	struct shader_texture_animation texture_animation;
	byte reserved10C[8];
	real reflection_falloff_distance;
	real reflection_cutoff_distance;
	real perpendicular_brightness;
	real_rgb_color perpendicular_tint_color;
	real parallel_brightness;
	real_rgb_color parallel_tint_color;
	struct tag_reference reflection_cube_map;
};

typedef char rtlm_model_base_map_assert[offsetof(struct rtlm_shader_model, base_map.index) == 0xB0 ? 1 : -1];
typedef char rtlm_model_detail_function_assert[offsetof(struct rtlm_shader_model, detail_function) == 0xD4 ? 1 : -1];

/* (rasterizer_xbox_transparent_geometry.c) */
struct rtlm_generic_map
{
	word flags;
	word pad02;
	real map_u_scale;
	real map_v_scale;
	real map_u_offset;
	real map_v_offset;
	real map_rotation;
	real mipmap_bias;
	struct tag_reference map;
	struct shader_texture_animation map_animation;
};

typedef char rtlm_generic_map_size_assert[sizeof(struct rtlm_generic_map) == 0x64 ? 1 : -1];

struct rtlm_chicago_map
{
	word flags;
	byte reserved_before_functions[42];
	short color_function;
	short alpha_function;
	byte reserved_before_map_u_scale[36];
	real map_u_scale;
	real map_v_scale;
	real map_u_offset;
	real map_v_offset;
	real map_rotation;
	real mipmap_bias;
	struct tag_reference map;
	byte reserved_after_map[40];
	struct shader_texture_animation map_animation;
};

typedef char rtlm_chicago_map_size_assert[sizeof(struct rtlm_chicago_map) == 0xDC ? 1 : -1];

/* (generic and chicago alike to their maps) */
struct rtlm_shader_transparent
{
	struct shader_base shader;
	byte numeric_counter_limit;
	byte flags;
	short type;
	short framebuffer_blend_function;
	short framebuffer_fade_mode;
	short framebuffer_fade_source;
	short pad32;
	real lens_flare_spacing;
	struct tag_reference lens_flare;
	struct tag_block extra_layers;
	struct tag_block maps;
	struct tag_block stages;
};

typedef char rtlm_transparent_maps_assert[offsetof(struct rtlm_shader_transparent, maps) == 0x54 ? 1 : -1];

struct rtlm_shader_glass
{
	struct shader shader;
	word flags;
	short pad2A;
	byte reserved_before_tint_color[40];
	real_rgb_color tint_color;
	real tint_map_scale;
	struct tag_reference tint_map;
	byte reserved_before_reflection_flags[20];
	word reflection_flags;
	short reflection_type;
	real_argb_color reflection_view_perpendicular_color;
	real_argb_color reflection_view_parallel_color;
	struct tag_reference reflection_map;
	real reflection_bump_map_scale;
	struct tag_reference reflection_bump_map;
	byte reserved_before_diffuse_flags[128];
	word diffuse_flags;
	word pad152;
	real diffuse_map_scale;
	struct tag_reference diffuse_map;
	real diffuse_detail_map_scale;
	struct tag_reference diffuse_detail_map;
};

typedef char rtlm_glass_detail_assert[offsetof(struct rtlm_shader_glass, diffuse_detail_map) == 0x16C ? 1 : -1];

struct rtlm_shader_meter
{
	struct shader shader;
	word flags;
	short pad2A;
	byte reserved_before_map[32];
	struct tag_reference map;
};

/* (object_lights.c's point_light_definition and rasterizer_xbox_environment.c's
gel parameters; 0xD4 and 0xD8, "radiosity intensity" and "radiosity color",
read here alone: no code of the game reads them) */
struct rtlm_light_definition
{
	unsigned long flags;
	real radius;
	real radius_modifier_lower_bound;
	real radius_modifier_upper_bound;
	real falloff_angle;
	real cutoff_angle;
	real lens_flare_radius;
	real runtime_cosine_falloff_angle;
	real runtime_cosine_cutoff_angle;
	real specular_radius_multiplier;
	real runtime_sine_cutoff_angle;
	byte reserved2C[0x8];
	unsigned long color_interpolation_flags;
	real_argb_color color_lower_bound;
	real_argb_color color_upper_bound;
	byte reserved58[0xC];
	struct tag_reference gel_map;
	word pad74;
	short texture_animation_function;
	real texture_animation_rate;
	struct tag_reference gel_secondary_map;
	word pad8C;
	short yaw_function;
	real yaw_period;
	word pad94;
	short roll_function;
	real roll_period;
	word pad9C;
	short pitch_function;
	real pitch_period;
	byte reservedA4[0x8];
	struct tag_reference lens_flare;
	byte reservedBC[0x18];
	real radiosity_intensity;
	real_rgb_color radiosity_color;
	byte reservedE4[0x10];
	real transition_duration;
	word reservedF8;
	short falloff_function;
};

typedef char rtlm_light_lens_flare_assert[offsetof(struct rtlm_light_definition, lens_flare) == 0xAC ? 1 : -1];
typedef char rtlm_light_color_assert[offsetof(struct rtlm_light_definition, color_interpolation_flags) == 0x34 ? 1 : -1];
typedef char rtlm_light_duration_assert[offsetof(struct rtlm_light_definition, transition_duration) == 0xF4 ? 1 : -1];
typedef char rtlm_light_falloff_assert[offsetof(struct rtlm_light_definition, falloff_function) == 0xFA ? 1 : -1];

/* (object_lights.c's light_datum) */
struct rtlm_light
{
	struct datum_header header;
	word flags;
	long definition_index;
	long rasterizer_light_index;
	long marker;
	long cluster_reference;
	real_rgb_color color;
	byte reserved20[0xC];
	long object_index;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real radius;
	long parent_light_index;
	short attachment_marker_index;
	short function_index;
	union
	{
		short color_function_index;
		struct
		{
			real_point3d relative_position;
			real_vector3d relative_forward;
		} node;
	};
	real intensity_scale;
};

typedef char rtlm_light_size_assert[sizeof(struct rtlm_light) == 0x7C ? 1 : -1];

enum
{
	_rtlm_light_dynamic_bit = 0,
	_rtlm_light_no_specular_bit,
	_rtlm_light_dont_light_own_object_bit,
	_rtlm_light_supersize_in_first_person_bit,
	_rtlm_light_is_first_person_flashlight_bit,
	_rtlm_light_dont_fade_active_camouflage_bit,
};

/* (the sky tag: render_sky.c's lights, and the rest as Halo CE's sky tag
lays it out; the offsets checked against render_sky.c's) */
struct rtlm_sky_light
{
	struct tag_reference lens_flare;
	char lens_flare_marker_name[32];
	byte unused30[0x4C - 0x30];
	unsigned long flags;
	real_rgb_color color;
	real power;
	real test_distance;
	byte unused64[4];
	real_euler_angles2d direction;
	real diameter;
};

typedef char rtlm_sky_light_size_assert[sizeof(struct rtlm_sky_light) == 0x74 ? 1 : -1];

struct rtlm_sky
{
	struct tag_reference model;
	struct tag_reference animation_graph;
	byte unused20[0x38 - 0x20];
	real_rgb_color indoor_ambient_radiosity_color;
	real indoor_ambient_radiosity_power;
	real_rgb_color outdoor_ambient_radiosity_color;
	real outdoor_ambient_radiosity_power;
	struct sky_atmospheric_fog outdoor_fog;
	struct sky_atmospheric_fog indoor_fog;
	struct tag_reference indoor_fog_screen;
	byte unusedA8[4];
	struct tag_block shader_functions;
	struct tag_block animations;
	struct tag_block lights;
};

typedef char rtlm_sky_fog_assert[offsetof(struct rtlm_sky, outdoor_fog) == 0x58 ? 1 : -1];
typedef char rtlm_sky_lights_assert[offsetof(struct rtlm_sky, lights) == 0xC4 ? 1 : -1];

/* ---------- the writer's state */

struct rtlm_texture
{
	long tag;
	short index;
	short linear;
	char path[256];
	int ok;
};

static struct
{
	char textures_folder[1024];
	struct rtlm_texture textures[RTLM_TEXTURES];
	int texture_count;
	DWORD palette[256];
} rtlm;

/* a note written at the top of lights.json (NULL none: the debug trigger's) */
static char const *rtlm_note = NULL;

/* ---------- small helpers */

static void rtlm_mkdirs(char const *path)
{
	char buffer[1024];
	size_t at;

	snprintf(buffer, sizeof(buffer), "%s", path);
	for (at = 1; buffer[at]; at++)
	{
		if (buffer[at] == '/')
		{
			buffer[at] = 0;
			mkdir(buffer, 0755);
			buffer[at] = '/';
		}
	}
	mkdir(buffer, 0755);
}

static void rtlm_string(FILE *file, char const *text)
{
	fputc('"', file);
	for (; text && *text; text++)
	{
		unsigned char c = (unsigned char)*text;

		if (c == '"' || c == '\\')
			fprintf(file, "\\%c", c);
		else if (c < 0x20)
			fprintf(file, "\\u%04x", c);
		else
			fputc(c, file);
	}
	fputc('"', file);
}

static void rtlm_rgb(FILE *file, real_rgb_color const *color)
{
	fprintf(file, "[%.6g,%.6g,%.6g]", color->red, color->green, color->blue);
}

static char const *rtlm_tag_name(long tag_index)
{
	char const *name = tag_index != NONE ? tag_get_name(tag_index) : NULL;

	return name ? name : "";
}

static void *rtlm_block(struct tag_block const *block, long index, long size)
{
	if (!block || !block->address || index < 0 || index >= block->count)
		return NULL;
	return (char *)block->address + index * size;
}

/* ---------- PNG (16-bit RGBA, deflated with the game's zlib) */

static voidpf rtlm_zalloc(voidpf opaque, uInt items, uInt size)
{
	void *address = malloc((size_t)items * size);

	/* (the game's malloc, which free here is: cseries.h's; calloc is not) */
	(void)opaque;
	if (address)
		memset(address, 0, (size_t)items * size);
	return address;
}

static void rtlm_zfree(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static void rtlm_put32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

/* the PNG CRC-32 over a chunk's type and data */
static unsigned long rtlm_crc_chain(char const *type, unsigned char const *data, unsigned long size)
{
	static unsigned long table[256];
	unsigned long crc = 0xFFFFFFFFUL, index;

	if (!table[1])
	{
		for (index = 0; index < 256; index++)
		{
			unsigned long value = index;
			int bit;

			for (bit = 0; bit < 8; bit++)
				value = value & 1 ? 0xEDB88320UL ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
	}
	for (index = 0; index < 4; index++)
		crc = table[(crc ^ (unsigned char)type[index]) & 255] ^ (crc >> 8);
	for (index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 255] ^ (crc >> 8);
	return (crc ^ 0xFFFFFFFFUL) & 0xFFFFFFFFUL;
}

static void rtlm_chunk_chained(FILE *file, char const *type, unsigned char const *data, unsigned long size)
{
	unsigned char word[4];

	rtlm_put32(word, size);
	fwrite(word, 1, 4, file);
	fwrite(type, 1, 4, file);
	if (size)
		fwrite(data, 1, size, file);
	rtlm_put32(word, rtlm_crc_chain(type, data, size));
	fwrite(word, 1, 4, file);
}

/* ARGB words, width by height, as 16-bit RGBA; linear: red, green and blue
by a pure 2.2 power */
static int rtlm_png_write(char const *path, DWORD const *pixels, unsigned long width, unsigned long height, int linear)
{
	static unsigned short decode[256];
	static int decode_linear = -1;
	unsigned long row_size = 1 + width * 8, raw_size = row_size * height, deflated_size, x, y;
	unsigned char *raw = malloc(raw_size), *deflated;
	unsigned char header[13];
	z_stream stream;
	FILE *file;
	int result;

	if (!raw)
		return FALSE;
	if (decode_linear != linear)
	{
		int index;

		for (index = 0; index < 256; index++)
		{
			decode[index] = linear ? (unsigned short)floor(pow(index / 255.0, 2.2) * 65535.0 + 0.5) :
				(unsigned short)(index * 257);
		}
		decode_linear = linear;
	}
	for (y = 0; y < height; y++)
	{
		unsigned char *row = raw + y * row_size;

		row[0] = 0;
		for (x = 0; x < width; x++)
		{
			DWORD pixel = pixels[y * width + x];
			unsigned short channels[4];
			int c;

			channels[0] = decode[(pixel >> 16) & 255];
			channels[1] = decode[(pixel >> 8) & 255];
			channels[2] = decode[pixel & 255];
			channels[3] = (unsigned short)(((pixel >> 24) & 255) * 257);
			for (c = 0; c < 4; c++)
			{
				row[1 + x * 8 + c * 2] = (unsigned char)(channels[c] >> 8);
				row[1 + x * 8 + c * 2 + 1] = (unsigned char)channels[c];
			}
		}
	}
	deflated_size = raw_size + raw_size / 1000 + 64;
	deflated = malloc(deflated_size);
	if (!deflated)
	{
		free(raw);
		return FALSE;
	}
	memset(&stream, 0, sizeof(stream));
	stream.zalloc = rtlm_zalloc;
	stream.zfree = rtlm_zfree;
	result = deflateInit(&stream, 6);
	if (result == Z_OK)
	{
		stream.next_in = raw;
		stream.avail_in = (uInt)raw_size;
		stream.next_out = deflated;
		stream.avail_out = (uInt)deflated_size;
		result = deflate(&stream, Z_FINISH);
		deflated_size = stream.total_out;
		deflateEnd(&stream);
	}
	free(raw);
	if (result != Z_STREAM_END || !(file = fopen(path, "wb")))
	{
		free(deflated);
		return FALSE;
	}
	fwrite("\x89PNG\r\n\x1a\n", 1, 8, file);
	rtlm_put32(header, width);
	rtlm_put32(header + 4, height);
	header[8] = 16;
	header[9] = 6;
	header[10] = header[11] = header[12] = 0;
	rtlm_chunk_chained(file, "IHDR", header, 13);
	rtlm_chunk_chained(file, "IDAT", deflated, deflated_size);
	rtlm_chunk_chained(file, "IEND", NULL, 0);
	free(deflated);
	fclose(file);
	return TRUE;
}

/* ---------- textures */

/* a bitmap's first level, every face or slice, as ARGB words (the faces
left to right, the slices top to bottom); NULL where it cannot be read */
static DWORD *rtlm_bitmap_decode(long bitmap_tag, short bitmap_index, unsigned long *out_width,
	unsigned long *out_height, struct xgpu_texture_description *description)
{
	struct bitmap_group *group;
	struct bitmap_data const *bitmap;
	DWORD const *header;
	unsigned long faces, slices, face_texels, face, x, y, slice;
	DWORD *face_pixels, *pixels;

	if (bitmap_tag == NONE || !tag_index_is_group(bitmap_tag, BITMAP_GROUP_TAG) ||
		!(group = bitmap_group_get(bitmap_tag)) || bitmap_index < 0 || bitmap_index >= group->bitmaps.count)
		return NULL;
	header = texture_cache_port_load((unsigned long)bitmap_tag, (unsigned long)bitmap_index);
	bitmap = TAG_BLOCK_GET_ELEMENT(&group->bitmaps, bitmap_index, struct bitmap_data);
	if (!header || !bitmap || !bitmap->base_address || header[1] == 0xffffffffUL)
		return NULL;
	xgpu_texture_describe(header[3], header[4], description);
	if (!description->width || !description->height || description->width > 4096 || description->height > 4096)
		return NULL;
	faces = description->cube_map ? 6 : 1;
	slices = description->depth > 1 ? description->depth : 1;
	face_texels = description->width * description->height * slices;
	face_pixels = malloc(face_texels * sizeof(DWORD));
	pixels = malloc(face_texels * faces * sizeof(DWORD));
	if (!face_pixels || !pixels)
	{
		free(face_pixels);
		free(pixels);
		return NULL;
	}
	if (!rtlm.palette[255])
	{
		int index;

		for (index = 0; index < 256; index++)
			rtlm.palette[index] = (DWORD)global_vector_palette[index];
	}
	for (face = 0; face < faces; face++)
	{
		unsigned char const *source = (unsigned char const *)bitmap->base_address +
			face * (faces > 1 ? xgpu_texture_face_size(description) : 0);

		if (description->compressed)
			xgpu_texture_decode_dxt(description, 0, source, face_pixels);
		else if (!xgpu_texture_decode_level(description, 0, source, rtlm.palette, face_pixels))
		{
			free(face_pixels);
			free(pixels);
			return NULL;
		}
		for (slice = 0; slice < slices; slice++)
		{
			for (y = 0; y < description->height; y++)
			{
				for (x = 0; x < description->width; x++)
				{
					pixels[(slice * description->height + y) * description->width * faces + face * description->width + x] =
						face_pixels[(slice * description->height + y) * description->width + x];
				}
			}
		}
	}
	free(face_pixels);
	*out_width = description->width * faces;
	*out_height = description->height * slices;
	return pixels;
}

/* a bitmap written once to the textures folder; its path as the scene
names it ("../textures/<name>"), or NULL */
static char const *rtlm_texture(long bitmap_tag, short bitmap_index, int linear)
{
	struct rtlm_texture *texture;
	char name[200], file_path[1300];
	char const *tag_name;
	FILE *existing;
	int at;

	if (bitmap_tag == NONE)
		return NULL;
	for (at = 0; at < rtlm.texture_count; at++)
	{
		texture = &rtlm.textures[at];
		if (texture->tag == bitmap_tag && texture->index == bitmap_index && texture->linear == linear)
			return texture->ok ? texture->path : NULL;
	}
	if (rtlm.texture_count >= RTLM_TEXTURES)
		return NULL;
	texture = &rtlm.textures[rtlm.texture_count++];
	memset(texture, 0, sizeof(*texture));
	texture->tag = bitmap_tag;
	texture->index = bitmap_index;
	texture->linear = (short)linear;
	tag_name = rtlm_tag_name(bitmap_tag);
	for (at = 0; tag_name[at] && at < (int)sizeof(name) - 24; at++)
	{
		char c = tag_name[at];

		name[at] = (c == '\\' || c == '/' || c == ' ' || c == ':') ? '_' : c;
	}
	name[at] = 0;
	if (bitmap_index)
		snprintf(name + at, sizeof(name) - at, "#%d", (int)bitmap_index);
	snprintf(texture->path, sizeof(texture->path), "../textures/%s.%s.png", name, linear ? "lin" : "raw");
	snprintf(file_path, sizeof(file_path), "%s/%s.%s.png", rtlm.textures_folder, name, linear ? "lin" : "raw");
	if ((existing = fopen(file_path, "rb")) != NULL)
	{
		fclose(existing);
		texture->ok = TRUE;
	}
	else
	{
		struct xgpu_texture_description description;
		unsigned long width = 0, height = 0;
		DWORD *pixels = rtlm_bitmap_decode(bitmap_tag, bitmap_index, &width, &height, &description);

		texture->ok = pixels && rtlm_png_write(file_path, pixels, width, height, linear);
		if (!texture->ok)
			platform_log("rt2 lights: %s (bitmap %d) not written", tag_name, (int)bitmap_index);
		free(pixels);
	}
	return texture->ok ? texture->path : NULL;
}

static void rtlm_map(FILE *file, char const *key, long bitmap_tag, int linear, int comma)
{
	char const *path = rtlm_texture(bitmap_tag, 0, linear);

	fprintf(file, "%s\"%s\":", comma ? "," : "", key);
	if (path)
		rtlm_string(file, path);
	else
		fprintf(file, "null");
}

/* ---------- materials */

static long rtlm_shader_by_name(char const *name)
{
	long index;

	if (!global_tag_instances)
		return NONE;
	for (index = 0; index < global_tag_count; index++)
	{
		struct cache_file_tag_instance const *instance = &global_tag_instances[index];

		if (instance->name && (instance->group_tag == SHADER_DEFINITION_TAG ||
			instance->parent_group_tags[0] == SHADER_DEFINITION_TAG ||
			instance->parent_group_tags[1] == SHADER_DEFINITION_TAG) && !strcmp(instance->name, name))
			return instance->tag_index;
	}
	return NONE;
}

static void rtlm_emission(FILE *file, struct shader const *shader)
{
	struct shader_radiosity_properties const *radiosity = &shader->base.radiosity;

	fprintf(file, ",\"emission\":{\"power\":%.6g,\"color\":", radiosity->power);
	rtlm_rgb(file, &radiosity->color_of_emitted_light);
	fprintf(file, ",\"tint\":");
	rtlm_rgb(file, &radiosity->tint_color);
	fprintf(file, ",\"simple_parameterization\":%s,\"ignore_normals\":%s,\"transparent_lit\":%s,\"detail_level\":\"%s\","
		"\"source\":\"shader radiosity properties\",\"from\":\"shader.radiosity: power (0x04), color of emitted light "
		"(0x08), tint color (0x14: the color light passing through this surface takes, for transparent shaders), flags "
		"(0x00: bit 0 simple parameterization, bit 1 ignore normals, bit 2 transparent lit, shaders.c and "
		"rasterizer_xbox_transparent_geometry.c), detail level (0x02: the lightmap tool's subdivision). The game never "
		"reads power or color (only power > 0, to call a surface light for the HDR: "
		"rasterizer_xbox_transparent_geometry.c's transparent_hdr_kind); they are the lightmap tool's emitter. "
		"Emitted radiance is color x power in the tool's units; the tool's own code is not in this source tree (its "
		"files source/structures/radiosity/*.c are empty), so the scale to the lightmap is not known from code\"}",
		TEST_FLAG(radiosity->flags, 0) ? "true" : "false", TEST_FLAG(radiosity->flags, 1) ? "true" : "false",
		TEST_FLAG(radiosity->flags, 2) ? "true" : "false",
		rtlm_radiosity_levels[radiosity->detail_level >= 0 && radiosity->detail_level < 4 ? radiosity->detail_level : 0]);
}

static void rtlm_material_environment(FILE *file, struct rtlm_shader_environment const *shader)
{
	struct rtlm_environment_diffuse const *diffuse = &shader->diffuse;
	struct rtlm_environment_self_illumination const *self = &shader->self_illumination;
	short type = shader->type, function = diffuse->detail_map_function, micro = diffuse->micro_detail_map_function;
	char const *environment_types[3] = { "normal", "blended", "blended_base_specular" };
	char const *mask_notes[3] =
	{
		"base map alpha x (the detail maps' alphas blended by the secondary detail map's alpha) x micro detail alpha",
		"(the detail maps' alphas blended by the base map's alpha) x micro detail alpha",
		"base map alpha x micro detail alpha",
	};

	if (type < 0 || type > 2)
		type = 0;
	if (function < 0 || function > 2)
		function = 0;
	if (micro < 0 || micro > 2)
		micro = 0;
	rtlm_map(file, "base_map", diffuse->base_map.index, TRUE, TRUE);
	rtlm_map(file, "detail_map", diffuse->primary_detail_map.index, TRUE, TRUE);
	fprintf(file, ",\"detail_scale\":%.6g", diffuse->primary_detail_map_scale);
	rtlm_map(file, "secondary_detail_map", diffuse->secondary_detail_map.index, TRUE, TRUE);
	fprintf(file, ",\"secondary_detail_scale\":%.6g", diffuse->secondary_detail_map_scale);
	rtlm_map(file, "micro_detail_map", diffuse->micro_detail_map.index, TRUE, TRUE);
	fprintf(file, ",\"micro_detail_scale\":%.6g,\"environment_type\":\"%s\",\"detail_function\":\"%s\","
		"\"micro_detail_function\":\"%s\",\"rescale_detail_maps\":%s",
		diffuse->micro_detail_map_scale, environment_types[type], rtlm_detail_functions[function],
		rtlm_detail_functions[micro], TEST_FLAG(diffuse->flags, 0) ? "true" : "false");
	rtlm_map(file, "bump_map", diffuse->bump_map.index, FALSE, TRUE);
	/* (bump_scale_runtime: the bump map's texture transform as the lightmap and
	diffuse light passes set it, its u and v scales over the base UV:
	rasterizer_xbox_environment.c:807-808, from the tag's runtime field) */
	fprintf(file, ",\"bump_scale\":%.6g,\"bump_scale_runtime\":[%.6g,%.6g],\"material_color\":",
		diffuse->bump_map_scale, diffuse->runtime_bump_map_scale.i, diffuse->runtime_bump_map_scale.j);
	rtlm_rgb(file, &diffuse->material_color);
	fprintf(file, ",\"albedo_note\":\"rasterizer_xbox_environment.c's texture pass (_rasterizer_environment_diffuse_"
		"texture_draw), in gamma space: detail = %s; then color = %s; then color = %s (micro detail); the "
		"lightmap pass multiplies the framebuffer by the lightmap after. Detail and micro UVs are the base UV times "
		"their scales (times base size / detail size where rescale_detail_maps). With the .lin maps: a multiply "
		"stays exact (x^2.2 y^2.2 = (xy)^2.2), so 2 x base x detail becomes 2^2.2 (4.595) x base x detail; an add "
		"does not, decode after combining. material_color is the diffuse lights' tint (_rasterizer_environment_"
		"diffuse_light_draw), not the albedo\"",
		type == 1 || type == 2 ? "(1 - base.a) x secondary + base.a x primary (0x380A1809)" : "(1 - secondary.a) x secondary + secondary.a x "
			"primary (combiner 0's rgb inputs 0x3A0A1A09)",
		function == 0 ? "2 x base x detail" : function == 1 ? "base x detail" : "base + 2 x detail - 1",
		micro == 0 ? "2 x color x micro" : micro == 1 ? "color x micro" : "color + 2 x micro - 1");
	fprintf(file, ",\"specular\":{\"mask\":\"%s\",\"tint\":",
		TEST_FLAG(shader->flags, 1) ? "the bump map's alpha (flag bump map is specular mask)" : mask_notes[type]);
	rtlm_rgb(file, &shader->specular.view_perpendicular_color);
	fprintf(file, ",\"tint_parallel\":");
	rtlm_rgb(file, &shader->specular.view_parallel_color);
	fprintf(file, ",\"brightness\":%.6g,\"overbright\":%s,\"extra_shiny\":%s,\"lightmap_is_specular\":%s,"
		"\"roughness\":null,\"from\":\"shader_environment specular: brightness (0x14 in the block), view "
		"perpendicular and parallel colors (0x2C, 0x38: the tint at normal and grazing view), flags (0: overbright, "
		"1: extra shiny, 2: lightmap is specular). Halo has no roughness: its specular is a cube map reflection "
		"(below) and the dynamic lights' fixed-power highlight\"}",
		shader->specular.brightness, TEST_FLAG(shader->specular.flags, 0) ? "true" : "false",
		TEST_FLAG(shader->specular.flags, 1) ? "true" : "false", TEST_FLAG(shader->specular.flags, 2) ? "true" : "false");
	fprintf(file, ",\"reflection\":{\"type\":\"%s\"", shader->reflection.type == 0 ? "bumped_cube_map" :
		shader->reflection.type == 1 ? "flat_cube_map" : "bumped_radiosity");
	rtlm_map(file, "cube_map", shader->reflection.cube_map.index, TRUE, TRUE);
	fprintf(file, ",\"lightmap_brightness_scale\":%.6g,\"perpendicular_brightness\":%.6g,\"parallel_brightness\":%.6g,"
		"\"dynamic_mirror\":%s}", shader->reflection.lightmap_brightness_scale,
		shader->reflection.view_perpendicular_brightness, shader->reflection.view_parallel_brightness,
		TEST_FLAG(shader->reflection.flags, 0) ? "true" : "false");
	/* (the alpha test: alpha > 0x7F of the bump map at its scale, or of the base map where the bump map is
	the specular mask, flag bit 1: rasterizer_xbox_environment.c, world_rt_game.c's material_describe) */
	fprintf(file, ",\"alpha_test\":%s,\"bump_map_is_specular_mask\":%s,\"alpha_test_map\":\"%s\","
		"\"two_sided\":false,\"two_sided_from\":\"shader_environment has no two-sided "
		"flag; every environment pass culls (D3DCULL_CCW, rasterizer_xbox_environment.c)\"",
		TEST_FLAG(shader->flags, 0) ? "true" : "false", TEST_FLAG(shader->flags, 1) ? "true" : "false",
		TEST_FLAG(shader->flags, 1) ? "base_map" : "bump_map");
	fprintf(file, ",\"self_illumination\":{");
	rtlm_map(file, "map", self->map.index, TRUE, FALSE);
	fprintf(file, ",\"map_scale\":%.6g,\"primary_on\":", self->map_scale);
	rtlm_rgb(file, &self->primary_on_color);
	fprintf(file, ",\"primary_off\":");
	rtlm_rgb(file, &self->primary_off_color);
	fprintf(file, ",\"secondary_on\":");
	rtlm_rgb(file, &self->secondary_on_color);
	fprintf(file, ",\"secondary_off\":");
	rtlm_rgb(file, &self->secondary_off_color);
	fprintf(file, ",\"plasma_on\":");
	rtlm_rgb(file, &self->plasma_on_color);
	fprintf(file, ",\"plasma_off\":");
	rtlm_rgb(file, &self->plasma_off_color);
	/* (as the lightmap pass draws it this frame: each color between off and on by its function at the game
	time, rasterizer_xbox_environment.c:862-898; the combiners add map.r x primary + map.g x secondary and the
	plasma's to the lightmap before the texture pass multiplies) */
	if (self->primary_animation_period != 0.0f && self->secondary_animation_period != 0.0f &&
		self->plasma_animation_period != 0.0f)
	{
		real now = global_frame_parameters.game_time_sec;
		real p = periodic_function_evaluate(self->primary_animation_function,
			(now + self->primary_animation_phase) / self->primary_animation_period);
		real q = periodic_function_evaluate(self->secondary_animation_function,
			(now + self->secondary_animation_phase) / self->secondary_animation_period);
		real r = periodic_function_evaluate(self->plasma_animation_function,
			(now + self->plasma_animation_phase) / self->plasma_animation_period);

		fprintf(file, ",\"now\":{\"game_time\":%.6g,\"primary\":[%.6g,%.6g,%.6g],\"secondary\":[%.6g,%.6g,%.6g],"
			"\"plasma_value\":%.6g}", now,
			self->primary_off_color.red * (1 - p) + self->primary_on_color.red * p,
			self->primary_off_color.green * (1 - p) + self->primary_on_color.green * p,
			self->primary_off_color.blue * (1 - p) + self->primary_on_color.blue * p,
			self->secondary_off_color.red * (1 - q) + self->secondary_on_color.red * q,
			self->secondary_off_color.green * (1 - q) + self->secondary_on_color.green * q,
			self->secondary_off_color.blue * (1 - q) + self->secondary_on_color.blue * q, r);
	}
	fprintf(file, ",\"from\":\"shader_environment self-illumination: the map's red, green and blue channels mask the "
		"primary, secondary and plasma colors, each between off and on by its animation; drawn at run time only (the "
		"lightmap tool's emission is the radiosity block above, not this)\"}");
}

static void rtlm_material_model(FILE *file, struct rtlm_shader_model const *shader)
{
	short function = shader->detail_function >= 0 && shader->detail_function < 3 ? shader->detail_function : 0;

	rtlm_map(file, "base_map", shader->base_map.index, TRUE, TRUE);
	rtlm_map(file, "detail_map", shader->detail_map.index, TRUE, TRUE);
	fprintf(file, ",\"detail_scale\":%.6g,\"detail_v_scale\":%.6g,\"map_u_scale\":%.6g,\"map_v_scale\":%.6g,"
		"\"detail_function\":\"%s\",\"detail_mask\":%d", shader->detail_map_scale, shader->detail_map_v_scale,
		shader->map_u_scale, shader->map_v_scale, rtlm_detail_functions[function], (int)shader->detail_mask);
	rtlm_map(file, "multipurpose_map", shader->multipurpose_map.index, FALSE, TRUE);
	fprintf(file, ",\"albedo_note\":\"rasterizer_xbox_models.c: base x detail by detail_function (as the "
		"environment's: 2 x base x detail, base x detail, base + 2 x detail - 1), where the detail mask "
		"(detail_mask: none, the multipurpose map's channel or the base map's alpha, rasterizer_xbox_models.c's "
		"detail_mask_alpha_inputs) lets it; the multipurpose map (stored, .raw) holds the specular/reflection mask, "
		"self-illumination, change-color and detail masks in its channels; translucency %.6g\"",
		shader->translucency);
	fprintf(file, ",\"specular\":{");
	rtlm_map(file, "mask", shader->multipurpose_map.index, FALSE, FALSE);
	fprintf(file, ",\"tint\":");
	rtlm_rgb(file, &shader->perpendicular_tint_color);
	fprintf(file, ",\"tint_parallel\":");
	rtlm_rgb(file, &shader->parallel_tint_color);
	fprintf(file, ",\"perpendicular_brightness\":%.6g,\"parallel_brightness\":%.6g,\"falloff_distance\":%.6g,"
		"\"cutoff_distance\":%.6g", shader->perpendicular_brightness, shader->parallel_brightness,
		shader->reflection_falloff_distance, shader->reflection_cutoff_distance);
	rtlm_map(file, "cube_map", shader->reflection_cube_map.index, TRUE, TRUE);
	fprintf(file, ",\"roughness\":null,\"from\":\"shader_model reflection: perpendicular and parallel brightness and "
		"tint, falloff and cutoff distances, cube map; Halo has no roughness\"}");
	fprintf(file, ",\"alpha_test\":%s,\"two_sided\":%s,\"from_flags\":\"shader_model flags: bit 1 two-sided, bit 2 "
		"not alpha-tested, bit 5 no-cull two-sided (rasterizer_xbox_models.c)\"",
		TEST_FLAG(shader->flags, 2) ? "false" : "true",
		(TEST_FLAG(shader->flags, 1) || TEST_FLAG(shader->flags, 5)) ? "true" : "false");
	fprintf(file, ",\"self_illumination\":{\"color_lower\":");
	rtlm_rgb(file, &shader->self_illumination_animation_color_lower_bound);
	fprintf(file, ",\"color_upper\":");
	rtlm_rgb(file, &shader->self_illumination_animation_color_upper_bound);
	fprintf(file, ",\"period\":%.6g,\"from\":\"shader_model self-illumination color bounds and period; masked by the "
		"multipurpose map; run time only\"}", shader->self_illumination_animation_period);
}

static void rtlm_material_transparent(FILE *file, struct rtlm_shader_transparent const *shader, int chicago)
{
	long index;

	fprintf(file, ",\"maps\":[");
	for (index = 0; index < shader->maps.count; index++)
	{
		long size = chicago ? (long)sizeof(struct rtlm_chicago_map) : (long)sizeof(struct rtlm_generic_map);
		void const *element = rtlm_block(&shader->maps, index, size);
		struct tag_reference const *map;
		real u_scale, v_scale;
		char const *path;

		if (!element)
			continue;
		map = chicago ? &((struct rtlm_chicago_map const *)element)->map : &((struct rtlm_generic_map const *)element)->map;
		u_scale = chicago ? ((struct rtlm_chicago_map const *)element)->map_u_scale :
			((struct rtlm_generic_map const *)element)->map_u_scale;
		v_scale = chicago ? ((struct rtlm_chicago_map const *)element)->map_v_scale :
			((struct rtlm_generic_map const *)element)->map_v_scale;
		path = rtlm_texture(map->index, 0, TRUE);
		fprintf(file, "%s{\"map\":", index ? "," : "");
		if (path)
			rtlm_string(file, path);
		else
			fprintf(file, "null");
		fprintf(file, ",\"u_scale\":%.6g,\"v_scale\":%.6g}", u_scale, v_scale);
	}
	fprintf(file, "]");
	if (shader->maps.count > 0)
	{
		void const *first = rtlm_block(&shader->maps, 0,
			chicago ? (long)sizeof(struct rtlm_chicago_map) : (long)sizeof(struct rtlm_generic_map));

		if (first)
			rtlm_map(file, "base_map", chicago ? ((struct rtlm_chicago_map const *)first)->map.index :
				((struct rtlm_generic_map const *)first)->map.index, TRUE, TRUE);
	}
	fprintf(file, ",\"framebuffer_blend_function\":%d,\"stages\":%ld,\"albedo_note\":\"%s: the maps (and %s) "
		"make a color blended into the framebuffer by blend function %d (0 alpha blend, 1 multiply, 2 double "
		"multiply, 3 add, ...); not lit by the lightmap unless transparent_lit\"",
		(int)shader->framebuffer_blend_function, (long)(chicago ? 0 : shader->stages.count),
		chicago ? "shader_transparent_chicago" : "shader_transparent_generic",
		chicago ? "each map's color function" : "its stages' combiners", (int)shader->framebuffer_blend_function);
	fprintf(file, ",\"alpha_test\":%s,\"two_sided\":%s,\"from_flags\":\"transparent flags (0x29): bit 0 alpha-tested, "
		"bit 2 two-sided\"", TEST_FLAG(shader->flags, 0) ? "true" : "false", TEST_FLAG(shader->flags, 2) ? "true" : "false");
}

static void rtlm_material_glass(FILE *file, struct rtlm_shader_glass const *shader)
{
	rtlm_map(file, "base_map", shader->diffuse_map.index, TRUE, TRUE);
	rtlm_map(file, "detail_map", shader->diffuse_detail_map.index, TRUE, TRUE);
	fprintf(file, ",\"base_scale\":%.6g,\"detail_scale\":%.6g,\"tint_color\":", shader->diffuse_map_scale,
		shader->diffuse_detail_map_scale);
	rtlm_rgb(file, &shader->tint_color);
	rtlm_map(file, "tint_map", shader->tint_map.index, TRUE, TRUE);
	fprintf(file, ",\"specular\":{");
	rtlm_map(file, "cube_map", shader->reflection_map.index, TRUE, FALSE);
	fprintf(file, ",\"tint\":");
	rtlm_rgb(file, &shader->reflection_view_perpendicular_color.rgb);
	fprintf(file, ",\"tint_parallel\":");
	rtlm_rgb(file, &shader->reflection_view_parallel_color.rgb);
	fprintf(file, ",\"roughness\":null}");
	fprintf(file, ",\"albedo_note\":\"shader_transparent_glass: tint (color x tint map) multiplies what is behind; "
		"diffuse map x detail map is lit and added; reflection from the cube map\",\"alpha_test\":%s,"
		"\"two_sided\":%s", TEST_FLAG(shader->flags, 0) ? "true" : "false", TEST_FLAG(shader->flags, 2) ? "true" : "false");
}

static void rtlm_material(FILE *file, long index, char const *tag)
{
	long shader_index = rtlm_shader_by_name(tag);
	struct shader const *shader = shader_index != NONE ? shader_definition_get(shader_index) : NULL;
	short type;

	fprintf(file, "{\"index\":%ld,\"tag\":", index);
	rtlm_string(file, tag);
	if (!shader)
	{
		fprintf(file, ",\"type\":null,\"note\":\"no shader tag of this name in the map\"}");
		return;
	}
	type = shader->base.type;
	fprintf(file, ",\"type\":\"%s\",\"type_from\":\"shader.type (0x24)\"",
		type >= 0 && type < RTLM_SHADER_TYPES ? rtlm_shader_type_names[type] : "unknown");
	switch (type)
	{
	case _rtlm_shader_environment:
		rtlm_material_environment(file, (struct rtlm_shader_environment const *)shader);
		break;
	case _rtlm_shader_model:
		rtlm_material_model(file, (struct rtlm_shader_model const *)shader);
		break;
	case _rtlm_shader_transparent_generic:
	case _rtlm_shader_transparent_chicago:
		rtlm_material_transparent(file, (struct rtlm_shader_transparent const *)shader,
			type == _rtlm_shader_transparent_chicago);
		break;
	case _rtlm_shader_transparent_glass:
		rtlm_material_glass(file, (struct rtlm_shader_glass const *)shader);
		break;
	case _rtlm_shader_transparent_meter:
		rtlm_map(file, "base_map", ((struct rtlm_shader_meter const *)shader)->map.index, TRUE, TRUE);
		fprintf(file, ",\"alpha_test\":false,\"two_sided\":false");
		break;
	default:
		fprintf(file, ",\"albedo_note\":\"%s: its maps are not exported (water, plasma and effect shaders draw no "
			"lit albedo)\",\"alpha_test\":false,\"two_sided\":false",
			type >= 0 && type < RTLM_SHADER_TYPES ? rtlm_shader_type_names[type] : "unknown");
		break;
	}
	rtlm_emission(file, shader);
	fprintf(file, "}");
}

/* the rows of materials.json as the export wrote them: each object's
"index" and "tag" */
static long rtlm_materials_read(char const *path, long *indices, char **tags, long maximum)
{
	FILE *file = fopen(path, "rb");
	char *text, *at;
	long size, count = 0;

	if (!file)
		return 0;
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	text = malloc((size_t)size + 1);
	if (!text || fread(text, 1, (size_t)size, file) != (size_t)size)
	{
		fclose(file);
		free(text);
		return 0;
	}
	fclose(file);
	text[size] = 0;
	at = text;
	while (count < maximum && (at = strchr(at, '{')) != NULL)
	{
		char *end = strchr(at, '}'), *key;

		if (!end)
			break;
		*end = 0;
		indices[count] = count;
		tags[count] = NULL;
		if ((key = strstr(at, "\"index\"")) != NULL && (key = strchr(key, ':')) != NULL)
			indices[count] = strtol(key + 1, NULL, 10);
		if ((key = strstr(at, "\"tag\"")) != NULL && (key = strchr(key + 5, '"')) != NULL)
		{
			char *out = malloc(strlen(key) + 1), *write = out;

			for (key++; out && *key && *key != '"'; key++)
			{
				if (*key == '\\' && key[1])
					key++;
				*write++ = *key;
			}
			if (out)
				*write = 0;
			tags[count] = out;
		}
		count++;
		at = end + 1;
	}
	free(text);
	return count;
}

/* ---------- lights */

static void rtlm_euler_direction(real_euler_angles2d const *angles, real_vector3d *direction)
{
	vector3d_from_euler_angles2d(direction, angles);
}

static void rtlm_sky(FILE *file, short sky_index, long clusters)
{
	long sky_tag = scenario_get_sky_definition_index(sky_index);
	struct rtlm_sky const *sky = (struct rtlm_sky const *)scenario_get_sky(sky_index);
	long light_index;

	fprintf(file, "{\"index\":%d,\"tag\":", (int)sky_index);
	rtlm_string(file, rtlm_tag_name(sky_tag));
	fprintf(file, ",\"clusters\":%ld", clusters);
	if (!sky)
	{
		fprintf(file, "}");
		return;
	}
	fprintf(file, ",\"sun\":[");
	for (light_index = 0; light_index < sky->lights.count; light_index++)
	{
		struct rtlm_sky_light const *light = rtlm_block(&sky->lights, light_index, sizeof(struct rtlm_sky_light));
		real_vector3d direction;

		if (!light)
			continue;
		rtlm_euler_direction(&light->direction, &direction);
		fprintf(file, "%s{\"direction\":[%.6g,%.6g,%.6g],\"color\":", light_index ? "," : "", direction.i, direction.j,
			direction.k);
		rtlm_rgb(file, &light->color);
		fprintf(file, ",\"power\":%.6g,\"angular_diameter_deg\":%.6g,\"yaw_deg\":%.6g,\"pitch_deg\":%.6g,"
			"\"test_distance\":%.6g,\"affects_exteriors\":%s,\"affects_interiors\":%s,\"lens_flare\":",
			light->power, RTLM_DEGREES(light->diameter), RTLM_DEGREES(light->direction.yaw),
			RTLM_DEGREES(light->direction.pitch), light->test_distance, TEST_FLAG(light->flags, 0) ? "true" : "false",
			TEST_FLAG(light->flags, 1) ? "true" : "false");
		rtlm_string(file, rtlm_tag_name(light->lens_flare.index));
		fprintf(file, ",\"lens_flare_marker\":");
		rtlm_string(file, light->lens_flare_marker_name[0] ? light->lens_flare_marker_name : "");
		fprintf(file, ",\"from\":\"sky lights block (0xC4), element %ld: direction (0x68, yaw and pitch, radians; "
			"vector3d_from_euler_angles2d, toward the light, as render_sky.c aims its lens flare), color (0x50), "
			"power (0x5C), diameter (0x70, radians), test distance (0x60), flags (0x4C: bit 0 affects exteriors, "
			"bit 1 interiors). The game reads only the lens flare and direction (render_sky.c); color, power, "
			"diameter, test distance and flags are the lightmap tool's alone. power is the tool's scale (unknown "
			"units); color x power is its irradiance on a surface facing it, by the measured fit's reading\"}",
			light_index);
	}
	fprintf(file, "],\"ambient\":{\"color\":");
	rtlm_rgb(file, &sky->outdoor_ambient_radiosity_color);
	fprintf(file, ",\"power\":%.6g,\"from\":\"sky outdoor ambient radiosity color (0x48) and power (0x54): the "
		"lightmap tool's uniform sky dome for exterior clusters; never read by the game\"},\"ambient_indoor\":"
		"{\"color\":", sky->outdoor_ambient_radiosity_power);
	rtlm_rgb(file, &sky->indoor_ambient_radiosity_color);
	fprintf(file, ",\"power\":%.6g,\"from\":\"sky indoor ambient radiosity color (0x38) and power (0x44): the same "
		"for interior clusters\"},\"outdoor_fog_note\":\"sky outdoor fog (0x58): color ", sky->indoor_ambient_radiosity_power);
	fprintf(file, "%.3g %.3g %.3g, maximum density %.3g, start %.3g wu, opaque %.3g wu; indoor fog (0x78): color %.3g "
		"%.3g %.3g, maximum density %.3g, start %.3g, opaque %.3g. The game's fog is a blend after lighting by "
		"distance (rasterizer fog), not a medium\"}",
		sky->outdoor_fog.color.red, sky->outdoor_fog.color.green, sky->outdoor_fog.color.blue,
		sky->outdoor_fog.maximum_density, sky->outdoor_fog.start_distance, sky->outdoor_fog.opaque_distance,
		sky->indoor_fog.color.red, sky->indoor_fog.color.green, sky->indoor_fog.color.blue,
		sky->indoor_fog.maximum_density, sky->indoor_fog.start_distance, sky->indoor_fog.opaque_distance);
}

static void rtlm_cone(FILE *file, struct rtlm_light_definition const *definition)
{
	fprintf(file, ",\"cone_deg\":[%.6g,%.6g],\"cone_cosines\":[%.6g,%.6g],\"cone_from\":\"falloff angle (0x10) and "
		"cutoff angle (0x14), radians in the tag; the game lights by (cos a - runtime cosine cutoff (0x20)) / "
		"(runtime cosine falloff (0x1C) - runtime cosine cutoff), clamped 0..1 (object_lights.c's cone scale and "
		"offset), a off the forward axis; no cone where the runtime falloff cosine is -1. On the structure the gel "
		"cube map shapes it instead\"",
		RTLM_DEGREES(definition->falloff_angle), RTLM_DEGREES(definition->cutoff_angle),
		definition->runtime_cosine_falloff_angle, definition->runtime_cosine_cutoff_angle);
}

static void rtlm_light_definition(FILE *file, long tag_index)
{
	struct rtlm_light_definition const *definition = tag_get(RTLM_LIGHT_TAG, tag_index);
	char const *gel = definition ? rtlm_texture(definition->gel_map.index, 0, TRUE) : NULL;

	fprintf(file, "{\"tag\":");
	rtlm_string(file, rtlm_tag_name(tag_index));
	if (!definition)
	{
		fprintf(file, "}");
		return;
	}
	fprintf(file, ",\"dynamic\":%s,\"flashlight\":%s,\"no_specular\":%s,\"dont_light_own_object\":%s,\"radius\":%.6g,"
		"\"radius_modifier\":[%.6g,%.6g],\"color_lower\":[%.6g,%.6g,%.6g,%.6g],\"color_upper\":[%.6g,%.6g,%.6g,%.6g],"
		"\"color_interpolation_flags\":%lu,\"specular_radius_multiplier\":%.6g,\"lens_flare_radius\":%.6g",
		TEST_FLAG(definition->flags, _rtlm_light_dynamic_bit) ? "true" : "false",
		TEST_FLAG(definition->flags, _rtlm_light_is_first_person_flashlight_bit) ? "true" : "false",
		TEST_FLAG(definition->flags, _rtlm_light_no_specular_bit) ? "true" : "false",
		TEST_FLAG(definition->flags, _rtlm_light_dont_light_own_object_bit) ? "true" : "false",
		definition->radius, definition->radius_modifier_lower_bound, definition->radius_modifier_upper_bound,
		definition->color_lower_bound.alpha, definition->color_lower_bound.red, definition->color_lower_bound.green,
		definition->color_lower_bound.blue, definition->color_upper_bound.alpha, definition->color_upper_bound.red,
		definition->color_upper_bound.green, definition->color_upper_bound.blue, definition->color_interpolation_flags,
		definition->specular_radius_multiplier, definition->lens_flare_radius);
	rtlm_cone(file, definition);
	fprintf(file, ",\"gel\":");
	if (gel)
		rtlm_string(file, gel);
	else
		fprintf(file, "null");
	fprintf(file, ",\"gel_tag\":");
	rtlm_string(file, rtlm_tag_name(definition->gel_map.index));
	fprintf(file, ",\"gel_secondary_tag\":");
	rtlm_string(file, rtlm_tag_name(definition->gel_secondary_map.index));
	fprintf(file, ",\"gel_motion\":{\"texture_animation_function\":%d,\"texture_animation_rate\":%.6g,"
		"\"yaw_function\":%d,\"yaw_period\":%.6g,\"roll_function\":%d,\"roll_period\":%.6g,\"pitch_function\":%d,"
		"\"pitch_period\":%.6g}", (int)definition->texture_animation_function, definition->texture_animation_rate,
		(int)definition->yaw_function, definition->yaw_period, (int)definition->roll_function, definition->roll_period,
		(int)definition->pitch_function, definition->pitch_period);
	fprintf(file, ",\"radiosity\":{\"intensity\":%.6g,\"color\":", definition->radiosity_intensity);
	rtlm_rgb(file, &definition->radiosity_color);
	fprintf(file, "},\"transition_duration\":%.6g,\"falloff_function\":%d,\"lens_flare\":",
		definition->transition_duration, (int)definition->falloff_function);
	rtlm_string(file, rtlm_tag_name(definition->lens_flare.index));
	fprintf(file, ",\"from\":\"light tag: flags (0x00: bit 0 dynamic, 1 no specular, 2 don't light own object, 4 first-"
		"person flashlight), radius (0x04) x radius modifier lower/upper (0x08, 0x0C) by the light's intensity, "
		"color lower/upper bounds ARGB (0x38, 0x48) by the object function or the effect's fade "
		"(object_lights.c), gel cube map (0x64) turned by yaw/roll/pitch functions and periods (0x8E..0xA0, "
		"rasterizer_xbox_environment.c), radiosity intensity (0xD4) and color (0xD8), transition duration (0xF4) "
		"and falloff function (0xFA: an effect light's fade over its duration, not a distance falloff). The "
		"radiosity fields are read by no code of the game. Lights have no power: the rasterizer adds color x "
		"distance attenuation (d / radius) x gel x N.L to the framebuffer, in gamma space\"}");
}

static void rtlm_distance_attenuation(FILE *file)
{
	struct xgpu_texture_description description;
	unsigned long width = 0, height = 0;
	long tag = global_rasterizer_data ? global_rasterizer_data->distance_attenuation.index : NONE;
	DWORD *pixels = tag != NONE ? rtlm_bitmap_decode(tag, 0, &width, &height, &description) : NULL;
	int index;

	fprintf(file, "\"distance_attenuation\":{\"tag\":");
	rtlm_string(file, rtlm_tag_name(tag));
	fprintf(file, ",\"texture\":");
	if (tag != NONE && rtlm_texture(tag, 0, FALSE))
		rtlm_string(file, rtlm_texture(tag, 0, FALSE));
	else
		fprintf(file, "null");
	fprintf(file, ",\"t\":\"d / radius, 0 to 1 in %d steps\",\"value_as_stored\":[", RTLM_FALLOFF);
	for (index = 0; index < RTLM_FALLOFF; index++)
	{
		float t = (float)index / (RTLM_FALLOFF - 1), value = 1.0f - t * t;

		/* (the volume's brightest channel along +x from its middle: as
		rt/tonight's d3d8_device.c reads it, a light's own cube of its
		radius about it, its border black) */
		if (pixels && description.depth > 1)
		{
			long size_x = (long)description.width, size_y = (long)description.height, size_z = (long)description.depth;
			float at = (0.5f + 0.5f * t) * (float)size_x - 0.5f;
			long x0 = (long)floorf(at), y = size_y / 2, z = size_z / 2;
			float f = at - floorf(at), sum = 0.0f;
			int k;

			for (k = 0; k < 2; k++)
			{
				long x = x0 + k;
				DWORD texel;
				float channel;

				if (x < 0 || x >= size_x)
					continue;
				texel = pixels[(z * size_y + y) * size_x + x];
				channel = (float)((texel >> 16) & 255);
				channel = fmaxf(channel, (float)((texel >> 8) & 255));
				channel = fmaxf(channel, (float)(texel & 255));
				sum += (k ? f : 1.0f - f) * channel / 255.0f;
			}
			value = sum;
		}
		fprintf(file, "%s%.5g", index ? "," : "", value);
	}
	fprintf(file, "],\"from\":\"%s: globals' rasterizer data, distance attenuation (game_globals.h), the 3D texture "
		"every light's diffuse pass samples at 0.5 + 0.5 (P - light) / radius (rasterizer_xbox_environment.c's "
		"vertex constant 0.5 / radius), with a black border: as stored (gamma space, as the pass multiplies it), "
		"read along +x from the middle\"}", pixels && description.depth > 1 ? "read" : "not readable: 1 - t^2 in its place");
	free(pixels);
}

/* the BSP's emitting shaders: their area (world units squared) and power,
in the lightmaps' materials (what the lightmap tool lit) */
static void rtlm_radiosity(FILE *file, struct scenario const *scenario)
{
	struct structure_bsp *bsp = global_structure_bsp_get();
	long vertex_size = rasterizer_geometry_get_vertex_size(_rasterizer_vertex_type_environment_compressed);
	long shaders[512], light_tags = 0, light_tags_radiosity = 0, dynamic_light_tags = 0, index, shader_count = 0;
	double areas[512], power_sum = 0.0, power_area_sum = 0.0, total_area = 0.0;
	long lightmap_index;

	fprintf(file, "\"radiosity\":{\"emitter_shaders\":[");
	if (bsp && bsp->lightmaps.address)
	{
		for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
		{
			struct structure_lightmap const *lightmap = rtlm_block(&bsp->lightmaps, lightmap_index,
				sizeof(struct structure_lightmap));
			long material_index;

			if (!lightmap)
				continue;
			for (material_index = 0; material_index < lightmap->materials.count; material_index++)
			{
				struct structure_material const *material = rtlm_block(&lightmap->materials, material_index,
					sizeof(struct structure_material));
				struct shader const *shader;
				byte const *vertices;
				double area = 0.0;
				long surface, at;

				if (!material || material->shader.index == NONE ||
					!tag_index_is_group(material->shader.index, SHADER_DEFINITION_TAG))
					continue;
				shader = shader_definition_get(material->shader.index);
				vertices = (byte const *)material->compressed_vertex_data.address;
				if (vertices && material->compressed_vertex_data.size >= material->vertices.count * vertex_size)
				{
					for (surface = 0; surface < material->surface_count; surface++)
					{
						struct structure_surface const *triangle = rtlm_block(&bsp->surfaces,
							material->first_surface_index + surface, sizeof(struct structure_surface));
						float const *p[3];
						float e1[3], e2[3], n[3];
						int corner;

						if (!triangle)
							continue;
						for (corner = 0; corner < 3; corner++)
						{
							if (triangle->vertex_indices[corner] >= material->vertices.count)
								break;
							p[corner] = (float const *)(vertices + triangle->vertex_indices[corner] * vertex_size);
						}
						if (corner < 3)
							continue;
						for (corner = 0; corner < 3; corner++)
						{
							e1[corner] = p[1][corner] - p[0][corner];
							e2[corner] = p[2][corner] - p[0][corner];
						}
						n[0] = e1[1] * e2[2] - e1[2] * e2[1];
						n[1] = e1[2] * e2[0] - e1[0] * e2[2];
						n[2] = e1[0] * e2[1] - e1[1] * e2[0];
						area += 0.5 * sqrt((double)n[0] * n[0] + (double)n[1] * n[1] + (double)n[2] * n[2]);
					}
				}
				total_area += area;
				if (!shader || !(shader->base.radiosity.power > 0.0f))
					continue;
				for (at = 0; at < shader_count; at++)
				{
					if (shaders[at] == material->shader.index)
						break;
				}
				if (at == shader_count && shader_count < (long)NUMBEROF(shaders))
				{
					shaders[shader_count] = material->shader.index;
					areas[shader_count++] = 0.0;
				}
				if (at < shader_count)
					areas[at] += area;
			}
		}
	}
	for (index = 0; index < shader_count; index++)
	{
		struct shader const *shader = shader_definition_get(shaders[index]);
		struct shader_radiosity_properties const *radiosity = &shader->base.radiosity;

		power_sum += radiosity->power;
		power_area_sum += radiosity->power * areas[index];
		fprintf(file, "%s{\"tag\":", index ? "," : "");
		rtlm_string(file, rtlm_tag_name(shaders[index]));
		fprintf(file, ",\"type\":\"%s\",\"power\":%.6g,\"color\":",
			shader->base.type >= 0 && shader->base.type < RTLM_SHADER_TYPES ? rtlm_shader_type_names[shader->base.type] :
			"unknown", radiosity->power);
		rtlm_rgb(file, &radiosity->color_of_emitted_light);
		fprintf(file, ",\"area_wu2\":%.6g,\"power_x_area\":%.6g,\"flags\":%u,\"detail_level\":%d}", areas[index],
			radiosity->power * areas[index], (unsigned)radiosity->flags, (int)radiosity->detail_level);
	}
	if (global_tag_instances)
	{
		for (index = 0; index < global_tag_count; index++)
		{
			struct rtlm_light_definition const *definition;

			if (global_tag_instances[index].group_tag != RTLM_LIGHT_TAG)
				continue;
			definition = tag_get(RTLM_LIGHT_TAG, global_tag_instances[index].tag_index);
			light_tags++;
			if (definition && TEST_FLAG(definition->flags, _rtlm_light_dynamic_bit))
				dynamic_light_tags++;
			if (definition && definition->radiosity_intensity > 0.0f)
				light_tags_radiosity++;
		}
	}
	fprintf(file, "],\"emitter_shader_count\":%ld,\"power_sum\":%.6g,\"power_x_area_sum\":%.6g,\"bsp_area_wu2\":%.6g,"
		"\"light_tags\":%ld,\"dynamic_light_tags\":%ld,\"light_tags_with_radiosity_intensity\":%ld,"
		"\"light_fixtures_placed\":%ld,\"skies\":%ld,", (long)shader_count, power_sum, power_area_sum, total_area,
		light_tags, dynamic_light_tags, light_tags_radiosity, scenario ? (long)scenario->light_fixtures.count : 0L,
		scenario ? (long)scenario->sky_references.count : 0L);
	fprintf(file, "\"what_the_bake_used\":\"Read from the tags and the game's code (the lightmap tool's own source, "
		"source/structures/radiosity/*.c, is empty here): (1) the BSP's shaders with radiosity power > 0, emitting "
		"color x power from their surfaces (the radiosity block: power, color, tint for light passing through "
		"transparent_lit surfaces, ignore normals, simple parameterization, detail level), which the game itself "
		"never reads; (2) each sky's lights (color, power, direction, diameter, test distance, exterior/interior "
		"flags) as distant suns, and (3) each sky's indoor and outdoor ambient radiosity (color, power) as a dome, "
		"all of them fields the game reads nothing of but the lens flare and direction. Light tags are not "
		"emitters of the bake: they belong to objects (light fixtures, scenery, units), which are not in the BSP "
		"the tool lights, and no flag of a light tag marks it static; their radiosity intensity and color "
		"(0xD4, 0xD8) are counted above (light_tags_with_radiosity_intensity) and read by no game code\"}");
	platform_log("rt2 lights: %s: %ld emitting shaders, power sum %.3f, power x area %.1f (BSP area %.0f wu2), "
		"%ld light tags (%ld dynamic, %ld with radiosity intensity), %ld light fixtures placed",
		cache_file_loaded_map_name() ? cache_file_loaded_map_name() : "?", (long)shader_count, power_sum,
		power_area_sum, total_area, light_tags, dynamic_light_tags, light_tags_radiosity,
		scenario ? (long)scenario->light_fixtures.count : 0L);
}

static void rtlm_lights_write(char const *path)
{
	struct scenario *scenario = global_scenario_get();
	struct structure_bsp *bsp = global_structure_bsp_get();
	FILE *file = fopen(path, "w");
	long cluster_counts[32], index, written = 0, lens_flare_only = 0, dark = 0;
	short sky_index, main_sky = 0;

	if (!file)
		return;
	memset(cluster_counts, 0, sizeof(cluster_counts));
	if (bsp)
	{
		for (index = 0; index < bsp->clusters.count; index++)
		{
			struct structure_cluster const *cluster = rtlm_block(&bsp->clusters, index, sizeof(struct structure_cluster));

			if (cluster && cluster->sky_index >= 0 && cluster->sky_index < 32)
				cluster_counts[cluster->sky_index]++;
		}
	}
	for (sky_index = 1; scenario && sky_index < scenario->sky_references.count && sky_index < 32; sky_index++)
	{
		if (cluster_counts[sky_index] > cluster_counts[main_sky])
			main_sky = sky_index;
	}
	fprintf(file, "{\"map\":");
	rtlm_string(file, cache_file_loaded_map_name() ? cache_file_loaded_map_name() : "");
	fprintf(file, ",\"tick\":%ld,\n", game_time_get());
	if (rtlm_note)
	{
		fprintf(file, "\"note\":");
		rtlm_string(file, rtlm_note);
		fprintf(file, ",\n");
	}
	fprintf(file, "\"sky\":");
	if (scenario && scenario->sky_references.count > 0)
		rtlm_sky(file, main_sky, cluster_counts[main_sky]);
	else
		fprintf(file, "null");
	fprintf(file, ",\n\"sky_from\":\"the sky the most BSP clusters name (structure cluster sky index); all in skies\","
		"\n\"skies\":[");
	for (sky_index = 0; scenario && sky_index < scenario->sky_references.count; sky_index++)
	{
		fprintf(file, "%s", sky_index ? ",\n" : "");
		rtlm_sky(file, sky_index, sky_index < 32 ? cluster_counts[sky_index] : 0);
	}
	fprintf(file, "],\n\"lights\":[");
	if (light_data)
	{
		struct data_iterator iterator;
		struct rtlm_light const *light;

		data_iterator_new(&iterator, light_data);
		while ((light = data_iterator_next(&iterator)) != NULL)
		{
			struct rtlm_light_definition const *definition = tag_get(RTLM_LIGHT_TAG, light->definition_index);
			struct object_datum *object = light->object_index != NONE ? object_try_and_get(light->object_index) : NULL;
			char const *source = "object", *gel;
			struct rasterizer_light_submit_parameters const *drawn;
			real_point3d position;
			real_vector3d forward, up;
			real_rgb_color color;
			real radius, emitter_radius;
			real_point3d emitter;

			if (!definition)
				continue;
			if (!TEST_FLAG(light->flags, 0))
			{
				lens_flare_only++;
				continue;
			}
			/* (a dynamic light at no intensity this tick: a muzzle flash
			between shots) */
			if (!(light->radius > 0.0f) || !(light->color.red > 0.0f || light->color.green > 0.0f ||
				light->color.blue > 0.0f))
			{
				dark++;
				continue;
			}
			if (light->parent_light_index != NONE)
				source = "effect";
			else if (object && object->object.type == _object_type_light_fixture)
				source = "device";
			else if (TEST_FLAG(definition->flags, _rtlm_light_is_first_person_flashlight_bit))
				source = "flashlight";
			/* (as drawn: the rasterizer's light this frame, which the game
			placed apart from the datum for a first-person flashlight, at the
			first-person weapon's "flashlight" marker, 0.5 back along its
			forward: first_person_weapons.c's first_person_weapon_center_
			flashlight, from object_lights.c; the datum where it has none) */
			drawn = light->rasterizer_light_index >= 0 &&
				light->rasterizer_light_index < rasterizer_lights.light_count &&
				rasterizer_lights.lights[light->rasterizer_light_index].definition == (void *)definition ?
				&rasterizer_lights.lights[light->rasterizer_light_index] : NULL;
			position = drawn ? drawn->position : light->position;
			forward = drawn ? drawn->forward : light->forward;
			up = drawn ? drawn->up : light->up;
			color = drawn ? drawn->color : light->color;
			radius = drawn ? drawn->radius : light->radius;
			gel = rtlm_texture(definition->gel_map.index, 0, TRUE);
			/* (the emitter: where light leaves, as the light list's record has it
			(lt_lights.c): a first-person flashlight at its lamp on the weapon in
			hand (first_person_weapon_flashlight_lamp), every other light at its
			position; the disk's radius as lt_lights.c:100-104 and 441-448 size it,
			a projectile's and a panel's apart) */
			emitter = position;
			emitter_radius = radius * 0.05f;
			emitter_radius = emitter_radius < 0.1f ? 0.1f : emitter_radius > 0.5f ? 0.5f : emitter_radius;
			if (!strcmp(source, "flashlight"))
			{
				real_point3d lamp;

				emitter_radius = 0.01f;
				if (light->object_index != NONE && first_person_weapon_flashlight_lamp(light->object_index, &lamp))
					emitter = lamp;
			}
			fprintf(file, "%s{\"tag\":", written ? ",\n" : "");
			rtlm_string(file, rtlm_tag_name(light->definition_index));
			fprintf(file, ",\"kind\":\"%s\",\"position\":[%.6g,%.6g,%.6g],\"direction\":[%.6g,%.6g,%.6g],"
				"\"up\":[%.6g,%.6g,%.6g],\"as_drawn\":%s,\"datum_position\":[%.6g,%.6g,%.6g],"
				"\"datum_direction\":[%.6g,%.6g,%.6g],",
				definition->runtime_cosine_falloff_angle != -1.0f ? "spot" : "point",
				position.x, position.y, position.z, forward.i, forward.j, forward.k, up.i, up.j, up.k,
				drawn ? "true" : "false", light->position.x, light->position.y, light->position.z,
				light->forward.i, light->forward.j, light->forward.k);
			fprintf(file, "\"emitter_position\":[%.6g,%.6g,%.6g],\"emitter_radius\":%.6g,\"color\":",
				emitter.x, emitter.y, emitter.z, emitter_radius);
			rtlm_rgb(file, &color);
			fprintf(file, ",\"power\":1,\"radius\":%.6g,\"falloff\":\"distance_attenuation (this file) at d / radius, "
				"times N.L, times the gel; none past radius\"", radius);
			rtlm_cone(file, definition);
			fprintf(file, ",\"gel\":");
			if (gel)
				rtlm_string(file, gel);
			else
				fprintf(file, "null");
			fprintf(file, ",\"gel_frame\":\"the cube map sampled along (P - light) in the frame of -forward, -left, -up "
				"(left = forward x up), after turning forward and up by the gel's yaw/pitch/roll functions at the game "
				"time (rasterizer_xbox_environment.c's _rasterizer_environment_diffuse_light_begin)\",\"source\":\"%s\","
				"\"attached_to_first_person_weapon\":%s,\"object\":", source,
				TEST_FLAG(light->flags, 3) ? "true" : "false");
			rtlm_string(file, object ? rtlm_tag_name(object->definition_index) : "");
			fprintf(file, ",\"from\":\"position, direction, up, color and radius as drawn this frame (rasterizer_lights.lights[], "
				"what object_lights.c's lights_preprocess_scene submitted; as_drawn false: the light datum's), the "
				"datum's own place (the unit's or object's marker) beside them; color (0..1) the tag's color bounds by the object's function or the "
				"effect's fade, already made; radius the tag's radius x its modifier by that intensity. power 1: "
				"Halo's lights have no power, color is what the rasterizer adds at the light (gamma space)\"}");
			written++;
		}
	}
	fprintf(file, "],\n\"lens_flare_only_lights\":%ld,\"dark_lights\":%ld,\"lights_from\":\"light datums "
		"(object_lights.c's light_data) at this tick; left out: lens flares only (no dynamic flag: they light "
		"nothing) and dynamic lights at no color or radius this tick\",\n\"light_tags\":[", lens_flare_only, dark);
	if (global_tag_instances)
	{
		long count = 0;

		for (index = 0; index < global_tag_count; index++)
		{
			if (global_tag_instances[index].group_tag != RTLM_LIGHT_TAG)
				continue;
			fprintf(file, "%s", count++ ? ",\n" : "");
			rtlm_light_definition(file, global_tag_instances[index].tag_index);
		}
	}
	fprintf(file, "],\n");
	rtlm_distance_attenuation(file);
	fprintf(file, ",\n");
	rtlm_radiosity(file, scenario);
	fprintf(file, "}\n");
	fclose(file);
	platform_log("rt2 lights: %s written: %ld lights at tick %ld (%ld lens flares only)", path, written,
		game_time_get(), lens_flare_only);
}

/* ---------- public code */

void rt_lights_materials_write(const char *scene_folder)
{
	char path[1200], *slash;
	long *indices, count, index;
	char **tags;
	FILE *file;

	if (!scene_folder || !*scene_folder)
		return;
	rtlm_mkdirs(scene_folder);
	snprintf(rtlm.textures_folder, sizeof(rtlm.textures_folder), "%s", scene_folder);
	while ((slash = strrchr(rtlm.textures_folder, '/')) != NULL && !slash[1])
		*slash = 0;
	slash = strrchr(rtlm.textures_folder, '/');
	if (slash)
		snprintf(slash + 1, sizeof(rtlm.textures_folder) - (size_t)(slash + 1 - rtlm.textures_folder), "textures");
	else
		snprintf(rtlm.textures_folder, sizeof(rtlm.textures_folder), "textures");
	rtlm_mkdirs(rtlm.textures_folder);
	rtlm.texture_count = 0;

	indices = malloc(sizeof(long) * RTLM_MATERIALS);
	tags = malloc(sizeof(char *) * RTLM_MATERIALS);
	snprintf(path, sizeof(path), "%s/materials.json", scene_folder);
	count = indices && tags ? rtlm_materials_read(path, indices, tags, RTLM_MATERIALS) : 0;
	if (count > 0 && (file = fopen(path, "w")) != NULL)
	{
		fprintf(file, "[\n");
		for (index = 0; index < count; index++)
		{
			rtlm_material(file, indices[index], tags[index] ? tags[index] : "");
			fprintf(file, "%s\n", index + 1 < count ? "," : "");
		}
		fprintf(file, "]\n");
		fclose(file);
	}
	for (index = 0; index < count; index++)
		free(tags[index]);
	free(indices);
	free(tags);

	snprintf(path, sizeof(path), "%s/lights.json", scene_folder);
	rtlm_lights_write(path);
	platform_log("rt2 lights: %s: %ld materials, %d textures", scene_folder, count, rtlm.texture_count);
}

void rt_lights_materials_debug_frame(void)
{
	static int done = FALSE, written = 0;
	static long tick = -1;
	static struct structure_bsp *last_bsp = NULL;
	char const *root;
	struct structure_bsp *bsp;
	char folder[1024], path[1200];
	char const *map, *name;
	FILE *file;
	long lightmap_index, count = 0, bsp_index = 0;
	long seen[RTLM_MATERIALS];
	struct scenario *scenario;

	if (done)
		return;
	root = getenv("HALO_RT2_LIGHTS_TEST");
	if (!root || !*root)
	{
		done = TRUE;
		return;
	}
	if (tick < 0)
		tick = getenv("HALO_RT2_LIGHTS_TEST_TICK") ? atol(getenv("HALO_RT2_LIGHTS_TEST_TICK")) : RTLM_DEFAULT_TICK;
	bsp = global_structure_bsp_get();
	map = cache_file_loaded_map_name();
	scenario = global_scenario_get();
	if (!bsp || !map || !scenario || game_time_get() < tick || strstr(map, "ui") || bsp == last_bsp)
		return;
	/* (once a BSP the map shows, after the tick: a level's first BSP may be
	its intro's) */
	last_bsp = bsp;
	if (++written >= 4)
		done = TRUE;
	bsp_index = global_structure_bsp_index_get();
	name = strrchr(map, '\\') ? strrchr(map, '\\') + 1 : map;
	snprintf(folder, sizeof(folder), "%s/%s_bsp%ld_lights_test", root, name, bsp_index);
	rtlm_mkdirs(folder);
	snprintf(path, sizeof(path), "%s/materials.json", folder);
	if (!(file = fopen(path, "w")))
		return;
	fprintf(file, "[");
	for (lightmap_index = 0; lightmap_index < bsp->lightmaps.count; lightmap_index++)
	{
		struct structure_lightmap const *lightmap = rtlm_block(&bsp->lightmaps, lightmap_index,
			sizeof(struct structure_lightmap));
		long material_index, at;

		for (material_index = 0; lightmap && material_index < lightmap->materials.count; material_index++)
		{
			struct structure_material const *material = rtlm_block(&lightmap->materials, material_index,
				sizeof(struct structure_material));

			if (!material || material->shader.index == NONE)
				continue;
			for (at = 0; at < count; at++)
			{
				if (seen[at] == material->shader.index)
					break;
			}
			if (at < count || count >= RTLM_MATERIALS)
				continue;
			seen[count] = material->shader.index;
			fprintf(file, "%s\n{\"index\":%ld,\"tag\":", count ? "," : "", count);
			rtlm_string(file, rtlm_tag_name(material->shader.index));
			fprintf(file, "}");
			count++;
		}
	}
	fprintf(file, "\n]\n");
	fclose(file);
	platform_log("rt2 lights: debug trigger at tick %ld: %s, %ld BSP shaders", game_time_get(), folder, count);
	rt_lights_materials_write(folder);
	/* (and existing scenes of this map, HALO_RT2_LIGHTS_SCENES=<folder>;...:
	their materials from the tags, their lights from this run's tick, not
	the scene's, said so in lights.json) */
	if (getenv("HALO_RT2_LIGHTS_SCENES") && written == 1)
	{
		char scenes[4096], *scene, *next;

		snprintf(scenes, sizeof(scenes), "%s", getenv("HALO_RT2_LIGHTS_SCENES"));
		for (scene = scenes; scene && *scene; scene = next)
		{
			char const *base;

			next = strchr(scene, ';');
			if (next)
				*next++ = 0;
			base = strrchr(scene, '/') ? strrchr(scene, '/') + 1 : scene;
			if (strncmp(base, name, strlen(name)) || base[strlen(name)] != '_')
				continue;
			rtlm_note = "materials, textures, the skies, the light tags (the flashlight's gel, cone and radius among "
				"them), the distance attenuation and the radiosity summary hold for this scene; the lights[] and "
				"their positions are NOT this scene's tick but the lights test's (its tick above): the scene's own "
				"come from the engine export calling rt_lights_materials_write at its tick";
			rt_lights_materials_write(scene);
			rtlm_note = NULL;
		}
	}
}
