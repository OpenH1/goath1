/*
XGPU_VULKAN_LT.C

E1 and E2 of the one light-transport pass (xgpu_vulkan_lt.h;
~/dev/rt2-realtime-design.md, section 7): the view's shadowed flashlight's
direct light, traced at the pre-pass's end (xgpu_vulkan_stream.c, where the
light shadows and the bounce are), from the pre-pass's inputs (each pixel's
distance, normal and owner).

The light is a physical spot: a disk at the lamp on the weapon in hand,
facing the beam, its gel and falloff as rt/tonight's decoding of the tags has
them (the cells' XGPU_DYNAMIC_LIGHTS_FLASHLIGHT texels: the gel projected
from the game's light, as its gel pass projects it, its falloff from there),
taken from the game's gamma to linear light (^2.2), times the cosine at the
receiver and one shadow ray a pixel a frame to a point of the disk (the R2
sequence over the frames from a start of the pixel's own: stratified), into
an RGBA32F image whose rgb is the mean and alpha the count. The mean is
count-based (1/n up to the mode's cap), its history the previous frame's
image at the reprojected point where the previous frame's pre-pass found the
same surface there (distance within 3%, normal within 25 degrees, the same
owner); otherwise it starts again (a disocclusion). The count starts again
everywhere when the light comes or goes or jumps.

The pass is a triangle over the window drawn as the light-shadow passes
are (xgpu_vulkan_light_shadows.c: the backend's draw sets, the occlusion's
with the ray-traced scene, the hit set), its kernels GLSL compiled offline
(lt/e1.vert, lt/e1.frag; tools/vulkan_lt_shaders.py). Its image is then
the one the surfaces read at display.rt_bounce's binding, which they add,
in this mode, after the game's own lights, linearly (xgpu_spirv.c's
lt_e1_add).

HALO_LT_E1_DUMP ("1-5,30,100-120": frames counted from the count's last
start) writes the mean at those frames as half floats into HALO_LT_E1_DUMP_DIR
(the current folder by default) as lt_e1_<frame>.rgba16f: a 16-byte header
("LTE1", width, height, frame as uint32) and RGBA rows from the first.
HALO_LT_E1_UNSHADOWED=1 (checking) casts no shadow ray. Render thread alone.
*/

#include "host.h"
#include "xgpu.h"
#include "xgpu_backend.h"
#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_denoise.h"
#include "xgpu_vulkan_dlss.h"
#include "xgpu_vulkan_lt.h"
#include "xgpu_vulkan_lt_indirect.h"
#include "xgpu_vulkan_lt_lights.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lt/xgpu_vulkan_lt_shaders.inc"

/* (debug.pass_times' name of the pass; HALO_VULKAN_DRAW_TIMES' kind) */
#define LT_OWNER "LIGHT TRANSPORT"
#define LT_KIND 0xfa
/* the counts' caps: E2's (the design's diffuse history), the accumulation's */
#define LT_E2_CAP 64
#define LT_ACCUMULATE_CAP (1 << 20)
/* the disk's radius (world units: a lamp's lens, about 3 cm) */
#define LT_DISK_RADIUS 0.01f
/* the flashlight's linear power over the tag's decoding taken to linear, set once so that the beam's centre on the
c20 corner's wall at about 3 m shows as bright as the original's gamma add, falling off physically from there
(~/dev/rt2-calibration.json's flashlight_power_scale and its derivation, with the physical falloff of lt/e1.frag; the truth renderer uses the same) */
#define LT_POWER_SCALE 1.009f
/* the other kinds' (~/dev/rt2-calibration.json's dynamic_power_scales): a
point light's on an object (a muzzle flash), on none (an explosion) */
#define LT_OBJECT_POWER_SCALE 0.845f
/* a projectile's light (a plasma bolt's, a needle's: lt_lights.c flags them),
measured apart (dynamic_power_scales.point_projectile) */
#define LT_PROJECTILE_POWER_SCALE 0.41f
#define LT_EFFECT_POWER_SCALE 7.85f
/* the lamp moved this far in a frame (world units): a jump, the count started again */
#define LT_JUMP 0.25f
/* where the parameters start in the draw set's uniforms (lt/e1.frag's) */
#define LT_PARAMS_OFFSET 1024
#define LT_DUMPS 512

_Static_assert(sizeof(struct xgpu_world_ao_pass) <= LT_PARAMS_OFFSET, "the pass's uniforms before the parameters");
_Static_assert(XGPU_DYNAMIC_LIGHTS_GRID_Z == 16 && XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE == 80 &&
	XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF == 32 && XGPU_WORLD_CASCADES == 4 && XGPU_WORLD_AO_INPUT_PRESENT == 0x400u &&
	XGPU_SPIRV_RT_MASK_AO == 0x03u && XGPU_SPIRV_RT_HIT_TEXTURES == 512u, "lt/e1.frag's constants");

/* lt/e1.frag's, std140 */
struct lt_params
{
	float light[4], color[4], forward[4], up[4], left[4], lamp[4];
	int32_t control[4];
	uint32_t owners[4];
	float falloff[XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF];
	/* W2: the dynamic lights' grid (the block's [0], [1]); the table's
	block lights, its suns, LT_LIGHTS_*, COPY_REACH's row */
	float grid[2][4];
	int32_t lights[4];
};
_Static_assert(LT_PARAMS_OFFSET + sizeof(struct lt_params) <= 192 * 16, "the parameters within the uniforms");
enum
{
	LT_VISIBILITY = 1,
	LT_RESET = 2,
	LT_COPY = 4,
	LT_COPY_GEOMETRIC = 8,
	LT_COPY_SHADED = 16,
	LT_COPY_PRESENT = 32,
	/* (lt/e1.frag's; 64 is the copies' position on rt2/guides) */
	LT_GRADIENT = 128,
	/* (W2's draws: the suns' share off the light; the reservoirs; the
	copy's reach of a row) */
	LT_RESERVOIR = 512,
	LT_COPY_REACH = 1024,
	LT_FINAL = 2048,
};
/* lights[2]'s (lt/e1.frag's LIGHTS_*) */
enum
{
	LT_LIGHTS_FLASHLIGHT = 1,
	LT_LIGHTS_HEROES = 2,
	LT_LIGHTS_RESTIR = 4,
	LT_LIGHTS_REUSE = 8,
};
/* the light table (lt/e1.frag's): a row a light, LT_TABLE_TEXELS RGBA32F
texels; the block's lights' rows by their index there, the suns' from
LT_SUN_ROW */
#define LT_TABLE_TEXELS 8
#define LT_SUN_ROW XGPU_DYNAMIC_LIGHTS_MAXIMUM
#define LT_SUNS 8
#define LT_TABLE_ROWS (LT_SUN_ROW + LT_SUNS)
#define LT_ONLY_IDS 8
/* the suns' direct share of the lightmap, a k for a sun (a narrow sky
light) and one for a fill (a wide one, LT_FILL_DIAMETER across or more:
b30's 50 degree sky light): ~/dev/rt2-calibration.json's sun_power_scale and
fill_power_scale: the sun's from the lightmap's own sun shadows (its
sunlit less its shadowed, b30), the fill's from the truth renderer's static
fit with sun and fill apart (b30, the lightmap ^2.2)) */
#define LT_SUN_POWER_SCALE 0.417f
#define LT_FILL_POWER_SCALE 0.187f
#define LT_FILL_DIAMETER 0.17453293f
enum
{
	LT_KIND_SKIP = -1,
	LT_KIND_POINT,
	LT_KIND_SPOT,
	LT_KIND_PANEL,
	LT_KIND_DIRECTIONAL,
};
/* (without a light list: the emitters' radii as lt_lights.c gives them) */
#define LT_DISK_SCALE 0.05f
#define LT_DISK_LEAST 0.1f
#define LT_DISK_MOST 0.5f

static struct
{
	int mode, made, failed, said, prepared;
	VkShaderModule vertex, fragment;
	VkSampler sampler;
	struct xgpu_vulkan_pipeline_description pass, copy;
	/* the means in turn (this frame's, the last), the last frame's inputs,
	the dump's halves */
	struct xgpu_vulkan_image *means[2], *previous, *dump;
	/* the pre-pass's twins' guides (xgpu_vulkan_stream.c; xgpu_spirv.h's C1) */
	struct xgpu_vulkan_image *guides;
	int current, previous_valid;
	/* the light last frame (its lamp), for the count's starts */
	int had_light;
	float last_lamp[3];
	unsigned long since_start;
	unsigned char dumps[LT_DUMPS];
	int dumps_read;
	/* (HALO_LT_E1=denoise: this frame's light, for the denoiser's re-shade) */
	struct lt_params reshade_params;
	/* W2: the light table (its image, its texels), the reservoirs in
	turn, the light after the suns' share (what the surfaces read); each
	row's light's id this frame and the last (0 none), the rows' count */
	struct xgpu_vulkan_image *table, *reservoirs[2], *final;
	int handled;
	float texels[LT_TABLE_ROWS][LT_TABLE_TEXELS][4];
	uint32_t ids[2][LT_TABLE_ROWS][2];
	int rows[2], ids_current, reservoir_current, reservoirs_valid;
	/* (the stream's frame, the pre-pass's history[1]: HALO_LT_TABLE_LOG's) */
	unsigned long ids_frame;
	/* the settings (read once): HALO_LT_ONLY, _HEROES, _RESTIR, _SUN,
	_KIND_SCALE, _DROP */
	int settings_read, only, heroes_on, restir_on, sun_on, reuse_on, only_ids;
	uint32_t only_id[LT_ONLY_IDS];
	float kind_scale[5];
	int drop_kind;
	uint32_t drop_id;
} lt = { -1 };

void xgpu_vulkan_lt_guides(struct xgpu_vulkan_image *guides)
{
	lt.guides = guides;
}

int xgpu_vulkan_lt_mode(void)
{
	if (lt.mode < 0)
	{
		const char *given = getenv("HALO_LT_E1");

		lt.mode = XGPU_VULKAN_LT_OFF;
		if (given && *given && strcmp(given, "off") && strcmp(given, "0") && strcmp(given, "false"))
			lt.mode = !strcmp(given, "raw") ? XGPU_VULKAN_LT_RAW : !strcmp(given, "accumulate") ?
				XGPU_VULKAN_LT_ACCUMULATE : !strcmp(given, "denoise") ? XGPU_VULKAN_LT_DENOISE : XGPU_VULKAN_LT_E2;
		if (lt.mode)
			host_logf(HOST_LOG_INFO, "vulkan: light transport E1: \"%s\" (the flashlight's direct light traced, %s)",
				given, lt.mode == XGPU_VULKAN_LT_RAW ? "1 spp, no history" : lt.mode == XGPU_VULKAN_LT_DENOISE ?
				"1 spp, through the denoiser (xgpu_vulkan_denoise.c)" : lt.mode == XGPU_VULKAN_LT_E2 ?
				"1 spp, counted up to 64 frames" : "counted up to 2^20 frames");
	}
	return lt.mode;
}

/* HALO_LT_E1_DUMP's frames, read once */
static void lt_dumps_read(void)
{
	const char *given = getenv("HALO_LT_E1_DUMP");

	lt.dumps_read = 1;
	while (given && *given)
	{
		char *end;
		long low = strtol(given, &end, 10), high = low, frame;

		if (end == given)
			break;
		given = end;
		if (*given == '-')
		{
			high = strtol(given + 1, &end, 10);
			given = end;
		}
		for (frame = low; frame <= high && frame < LT_DUMPS; frame++)
			if (frame >= 0)
				lt.dumps[frame] = 1;
		while (*given == ',' || *given == ' ')
			given++;
	}
}

/* ---------- debug: the kernels from files (HALO_LT_SHADER_DIR; the replay's
"reload", docs/replay_protocol.md). Off unless the folder is given: a
kernel's SPIR-V is read from <folder>/<source>.spv (tools/vulkan_lt_shaders.py
--spv <folder>) where that file is there, else the built-in words; a reload
(any thread, between frames) makes the pass again from them at its next
draw, its count started again. */

static char lt_shader_folder[1024];
static int lt_shader_folder_read, lt_reload_asked;

void xgpu_vulkan_lt_shaders_reload(const char *folder)
{
	if (folder)
	{
		snprintf(lt_shader_folder, sizeof(lt_shader_folder), "%s", folder);
		lt_shader_folder_read = 1;
	}
	__atomic_store_n(&lt_reload_asked, 1, __ATOMIC_RELEASE);
}

static VkShaderModule lt_shader(const char *source, const uint32_t *words, size_t count)
{
	VkShaderModule module;
	uint32_t *read = NULL;
	char path[1100];
	FILE *file;
	long size;

	if (!lt_shader_folder_read)
	{
		const char *given = getenv("HALO_LT_SHADER_DIR");

		snprintf(lt_shader_folder, sizeof(lt_shader_folder), "%s", given ? given : "");
		lt_shader_folder_read = 1;
	}
	snprintf(path, sizeof(path), "%s/%s.spv", lt_shader_folder, source);
	file = *lt_shader_folder ? fopen(path, "rb") : NULL;
	if (file && !fseek(file, 0, SEEK_END) && (size = ftell(file)) >= 20 && !(size & 3) && !fseek(file, 0, SEEK_SET) &&
		(read = malloc((size_t)size)) && fread(read, 1, (size_t)size, file) == (size_t)size && read[0] == 0x07230203u)
	{
		words = read;
		count = (size_t)size / 4;
		host_logf(HOST_LOG_WARN, "vulkan: light transport: %s from %s (%zu words)", source, path, count);
	}
	else if (*lt_shader_folder)
		host_logf(HOST_LOG_WARN, "vulkan: light transport: %s built in (no SPIR-V at %s)", source, path);
	if (file)
		fclose(file);
	module = xgpu_vulkan_shader(words, count);
	free(read);
	return module;
}

/* the made kernels' generation: a reload's next */
static unsigned lt_generation = 1;

unsigned xgpu_vulkan_lt_kernels_generation(void)
{
	return lt_generation;
}

VkShaderModule xgpu_vulkan_lt_kernel(const char *source)
{
	static const struct
	{
		const char *source;
		const uint32_t *words;
		size_t count;
	} kernels[] = {
		{ "e1.vert", lt_e1_vertex_spirv, sizeof(lt_e1_vertex_spirv) / sizeof(uint32_t) },
		{ "denoise_gradient.frag", lt_denoise_gradient_spirv, sizeof(lt_denoise_gradient_spirv) / sizeof(uint32_t) },
		{ "denoise_temporal.frag", lt_denoise_temporal_spirv, sizeof(lt_denoise_temporal_spirv) / sizeof(uint32_t) },
		{ "denoise_atrous.frag", lt_denoise_atrous_spirv, sizeof(lt_denoise_atrous_spirv) / sizeof(uint32_t) },
		{ "denoise_atrous_pair.frag", lt_denoise_atrous_pair_spirv,
			sizeof(lt_denoise_atrous_pair_spirv) / sizeof(uint32_t) },
		{ "denoise_atrous_final.frag", lt_denoise_atrous_final_spirv,
			sizeof(lt_denoise_atrous_final_spirv) / sizeof(uint32_t) },
		{ "denoise_guide.frag", lt_denoise_guide_spirv, sizeof(lt_denoise_guide_spirv) / sizeof(uint32_t) },
		{ "denoise_guide_fold.frag", lt_denoise_guide_fold_spirv, sizeof(lt_denoise_guide_fold_spirv) / sizeof(uint32_t) },
		/* (the indirect light and reflections: xgpu_vulkan_lt_indirect.c) */
		{ "e1_indirect.frag", lt_indirect_spirv, sizeof(lt_indirect_spirv) / sizeof(uint32_t) },
	};
	size_t index;

	for (index = 0; index < sizeof(kernels) / sizeof(kernels[0]); index++)
		if (!strcmp(kernels[index].source, source))
			return lt_shader(source, kernels[index].words, kernels[index].count);
	return VK_NULL_HANDLE;
}

static int lt_shaders(void)
{
	struct xgpu_vulkan_sampler_description sampler;
	int reloaded = __atomic_exchange_n(&lt_reload_asked, 0, __ATOMIC_ACQ_REL);

	if (reloaded)
	{
		lt.made = lt.failed = lt.prepared = lt.previous_valid = 0;
		lt_generation++;
	}
	if (lt.made)
		return 1;
	if (lt.failed)
		return 0;
	lt.vertex = lt_shader("e1.vert", lt_e1_vertex_spirv, sizeof(lt_e1_vertex_spirv) / sizeof(uint32_t));
	lt.fragment = lt_shader("e1.frag", lt_e1_spirv, sizeof(lt_e1_spirv) / sizeof(uint32_t));
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	lt.sampler = xgpu_vulkan_sampler(&sampler);
	if (!lt.vertex || !lt.fragment || !lt.sampler)
	{
		host_logf(HOST_LOG_WARN, "vulkan: light transport E1: its pass cannot be made");
		lt.failed = 1;
		return 0;
	}
	memset(&lt.pass, 0, sizeof(lt.pass));
	lt.pass.vertex_shader = lt.vertex;
	lt.pass.pixel_shader = lt.fragment;
	lt.pass.primitive_class = XGPU_VULKAN_CLASS_TRIANGLES;
	lt.pass.color_mask = 15;
	lt.pass.color_format = XGPU_VULKAN_FORMAT_RGBA32F;
	lt.pass.samples = 1;
	lt.copy = lt.pass;
	lt.copy.color_format = XGPU_VULKAN_FORMAT_RGBA16F;
	lt.made = 1;
	/* (a reload's pipeline made now, so the next play draws with it) */
	if (reloaded)
		(void)xgpu_vulkan_pipeline(&lt.pass);
	return 1;
}

static struct xgpu_vulkan_image *lt_image(struct xgpu_vulkan_image *image, uint8_t format, uint32_t width,
	uint32_t height)
{
	struct xgpu_vulkan_image_description description;

	if (image && xgpu_vulkan_image_description(image)->width == width &&
		xgpu_vulkan_image_description(image)->height == height)
		return image;
	/* (never one the sets still name) */
	if (xgpu_vulkan_rt_bounce_bound(image))
		xgpu_vulkan_rt_bounce_set(NULL);
	xgpu_vulkan_image_destroy(image);
	memset(&description, 0, sizeof(description));
	description.type = XGPU_VULKAN_IMAGE_2D;
	description.format = format;
	description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
	description.width = width;
	description.height = height;
	description.depth_or_layers = 1;
	description.levels = 1;
	return xgpu_vulkan_image_create(&description);
}

/* a cells' texel of the clusters' slice (xgpu.h) */
static const uint32_t *lt_texel(const unsigned char *cells, int index)
{
	return (const uint32_t *)(const void *)(cells + ((XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y *
		XGPU_DYNAMIC_LIGHTS_GRID_Z) + index) * 16);
}

/* the view's shadowed flashlight into the parameters: 0 where there is none */
static int lt_light(const unsigned char *cells, const struct xgpu_dynamic_lights_block *block, struct lt_params *params)
{
	const uint32_t *head = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1);
	const uint32_t *slots = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT);
	int slot, light, count = (int)block->grid[0][3], index;

	if (!head[0] || head[0] > XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS)
		return 0;
	slot = (int)head[0] - 1;
	light = (int)slots[slot] - 1;
	if (light < 0 || light >= count || light >= XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		return 0;
	memcpy(params->light, block->position[light], sizeof(params->light));
	memcpy(params->color, block->color[light], sizeof(params->color));
	memcpy(&params->color[3], &head[1], sizeof(float));
	memcpy(params->forward, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 2 + 3 * slot), sizeof(float) * 3);
	memcpy(params->up, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 3 + 3 * slot), sizeof(float) * 3);
	memcpy(params->left, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 4 + 3 * slot), sizeof(float) * 3);
	params->forward[3] = LT_DISK_RADIUS;
	memcpy(params->lamp, lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 23), sizeof(float) * 3);
	params->lamp[3] = LT_POWER_SCALE;
	params->owners[0] = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 14)[3];
	for (index = 0; index < XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF / 4; index++)
		memcpy(&params->falloff[index * 4], lt_texel(cells, XGPU_DYNAMIC_LIGHTS_ATTENUATION + index), 16);
	return params->light[3] != 0.0f;
}

/* ---------- W2: every light (~/dev/rt2-realtime-design.md, sections 1.2
and 5's W2). The C4 light list (xgpu_vulkan_lt_lights.h, the window's that
came last) gives each of the block's lights (matched by where it is drawn)
its kind, emitter, emitter radius, frame, cone, gel, owner, linear power and
stable id; a light the list does not have (a recording from before the list,
the bounce's) is taken from the block alone: a point or a spot at its
position, its radius lt_lights.c's rule, its power the block's colour, no
owner. Its row in the light table is its index in the block, so the cells'
masks name it; the view's shadowed flashlight keeps E1's own path (its row
skipped). The suns are the list's directional lights. */

/* HALO_LT_KIND_SCALE's kinds (a sun's "sun", a wide sky light's "fill") */
static const char *const lt_kind_names[5] = { "point", "spot", "panel", "sun", "fill" };

static void lt_settings(void)
{
	const char *given;
	int kind;

	if (lt.settings_read)
		return;
	lt.settings_read = 1;
	given = getenv("HALO_LT_ONLY");
	lt.only = given && !strcmp(given, "flashlight") ? 1 : given && !strcmp(given, "others") ? 2 : 0;
	/* (ids:<hex>,<hex>: those lights alone, by their id's low word, the
	flashlight's E1 path left out; the truth's per-light parts) */
	if (given && !strncmp(given, "ids:", 4))
	{
		const char *at = given + 4;

		lt.only = 3;
		while (*at && lt.only_ids < LT_ONLY_IDS)
		{
			char *end;

			lt.only_id[lt.only_ids++] = (uint32_t)strtoul(at, &end, 16);
			at = *end == ',' ? end + 1 : end;
			if (end == at && *at != ',')
				break;
		}
	}
	lt.heroes_on = !getenv("HALO_LT_HEROES") || strcmp(getenv("HALO_LT_HEROES"), "0");
	lt.restir_on = !getenv("HALO_LT_RESTIR") || strcmp(getenv("HALO_LT_RESTIR"), "0");
	lt.sun_on = !getenv("HALO_LT_SUN") || strcmp(getenv("HALO_LT_SUN"), "0");
	/* (HALO_LT_REUSE=0: RIS alone each frame, no reservoirs kept: unbiased,
	the reuse's check) */
	lt.reuse_on = !getenv("HALO_LT_REUSE") || strcmp(getenv("HALO_LT_REUSE"), "0");
	for (kind = 0; kind < 5; kind++)
		lt.kind_scale[kind] = -1.0f;
	/* (the suns' k: the lightmap's direct share's, ~/dev/rt2-calibration.json's
	sun_power_scale and fill_power_scale, whatever k the record was given) */
	lt.kind_scale[3] = LT_SUN_POWER_SCALE;
	lt.kind_scale[4] = LT_FILL_POWER_SCALE;
	given = getenv("HALO_LT_KIND_SCALE");
	while (given && *given)
	{
		for (kind = 0; kind < 5; kind++)
		{
			size_t length = strlen(lt_kind_names[kind]);

			if (!strncmp(given, lt_kind_names[kind], length) && given[length] == '=')
			{
				lt.kind_scale[kind] = (float)atof(given + length + 1);
				break;
			}
		}
		given = strchr(given, ',');
		if (given)
			given++;
	}
	given = getenv("HALO_LT_DROP");
	lt.drop_kind = !given || !*given ? -2 : !strcmp(given, "all") ? 9 : !strcmp(given, "flashlight") ? 8 : -2;
	/* (id:<hex>: that light alone, by its id's low word) */
	if (given && !strncmp(given, "id:", 3))
	{
		lt.drop_kind = 10;
		lt.drop_id = (uint32_t)strtoul(given + 3, NULL, 16);
	}
	for (kind = 0; kind < 3; kind++)
		if (given && !strcmp(given, lt_kind_names[kind]))
			lt.drop_kind = kind;
	if (lt.only || !lt.heroes_on || !lt.restir_on || !lt.sun_on || getenv("HALO_LT_KIND_SCALE") || lt.drop_kind != -2)
		host_logf(HOST_LOG_WARN, "vulkan: light transport: only %d, heroes %d, ReSTIR %d, sun %d, kind scales %g %g %g "
			"%g %g, drop %d (checking)", lt.only, lt.heroes_on, lt.restir_on, lt.sun_on, (double)lt.kind_scale[0],
			(double)lt.kind_scale[1], (double)lt.kind_scale[2], (double)lt.kind_scale[3], (double)lt.kind_scale[4],
			lt.drop_kind);
}

/* the list's record of the block's light at position (its 1 / radius in
w): the same place (within a millimetre) and reach; NULL none */
static const struct xgpu_stream_lt_light *lt_record(const struct xgpu_stream_lt_light *list, int count,
	const float position[4])
{
	int index;

	for (index = 0; index < count; index++)
	{
		const struct xgpu_stream_lt_light *light = &list[index];
		float dx = light->position[0] - position[0], dy = light->position[1] - position[1];
		float dz = light->position[2] - position[2], reach = position[3] != 0.0f ? 1.0f / fabsf(position[3]) : 0.0f;

		if (light->kind != XGPU_LT_LIGHT_DIRECTIONAL && dx * dx + dy * dy + dz * dz < 1.0e-6f &&
			fabsf(light->range - reach) <= 1.0e-3f * (reach > 1.0f ? reach : 1.0f))
			return light;
	}
	return NULL;
}

/* a light's k by its kind (lt_lights.c's lt_power_scale, the same rule:
~/dev/rt2-calibration.json): the flashlight's, a point's on an object (a
muzzle flash, a bolt) or on none (an explosion), the rest the flashlight's */
static float lt_kind_k(const struct xgpu_stream_lt_light *light)
{
	unsigned int source = light->id[1] >> 24;

	if ((light->flags & XGPU_LT_LIGHT_FLASHLIGHT) || light->kind != XGPU_LT_LIGHT_POINT)
		return LT_POWER_SCALE;
	/* (a projectile's; and one of the port's own effect flashes
(light_sources.c's key kind 1: a needle bursting, a shot's flash), lights
of a projectile's reach (1 to 1.5 world units), not an explosion's (10),
whose k is for its reach: the projectile's) */
	if ((light->flags & XGPU_LT_LIGHT_PROJECTILE) || source == 1)
		return LT_PROJECTILE_POWER_SCALE;
	/* (one of the port's own lights on an object or a firer's flames
	(light_sources.c's key kinds 2 and 3): on an object, though a record
	made before lt_lights.c named its object has none) */
	return light->object != 0xffffffffu || source == 2 || source == 3 ? LT_OBJECT_POWER_SCALE :
		LT_EFFECT_POWER_SCALE;
}

/* a record's power as the pass uses it: its kind's k (a recording made
before the kinds were measured carries the flashlight's), or
HALO_LT_KIND_SCALE's where given */
static void lt_power(const struct xgpu_stream_lt_light *light, int kind, float power[3])
{
	int channel;
	/* (a sun's: its kind's scale, the lightmap's direct share's) */
	float k = lt.kind_scale[kind] >= 0.0f ? lt.kind_scale[kind] : lt_kind_k(light);
	float scale = light->power_scale > 0.0f ? k / light->power_scale : 1.0f;

	for (channel = 0; channel < 3; channel++)
		power[channel] = light->power[channel] * scale;
}

static void lt_set(float texel[4], float x, float y, float z, float w)
{
	texel[0] = x;
	texel[1] = y;
	texel[2] = z;
	texel[3] = w;
}

/* the block's light index's kind (LT_KIND_*, LT_KIND_SKIP) as the list or
the block has it, for HALO_LT_DROP */
static int lt_block_kind(const struct xgpu_dynamic_lights_block *block, int index,
	const struct xgpu_stream_lt_light *list, int count)
{
	const struct xgpu_stream_lt_light *light = lt_record(list, count, block->position[index]);

	if (light)
		return light->flags & XGPU_LT_LIGHT_FLASHLIGHT ? 8 : light->kind == XGPU_LT_LIGHT_SPOT ? LT_KIND_SPOT :
			light->kind == XGPU_LT_LIGHT_PANEL ? LT_KIND_PANEL : LT_KIND_POINT;
	return block->cone[index][3] > 0.0f ? LT_KIND_SPOT : LT_KIND_POINT;
}

/* whether a light of the list is one of the port's own lights on a
projectile (light_sources.c: a needle's, a Wraith mortar shell's, a
rocket's exhaust, a Sentinel's beam; in flight or stuck in what it hit),
whose tag has no light: in Halo it is a lens flare and a glow alone, no
light on the world. The light transport lights what Halo's data emits (the
truth's lights.json has no such light; the classic game draws none): with
the pass on, these are flare-only. Measured (~/dev/rt2-calibration.json's
own_projectile_lights): a needle stuck in the player at b30_fight.hrpl frame
540 added 1.48 times the enhanced lights' own light over its reach (1.6 to
1.7 times where k was fitted), the classic game none; the broad pink wash
and the purple first-person weapon of the b30 clip */
static int lt_flare_only(const struct xgpu_stream_lt_light *light)
{
	unsigned int source = light ? light->id[1] >> 24 : 0u;

	return source >= XGPU_LT_LIGHT_SOURCE_OWN_FIRST && source < XGPU_LT_LIGHT_SOURCE_GAME &&
		(light->flags & XGPU_LT_LIGHT_PROJECTILE);
}

void xgpu_vulkan_lt_block_filter(struct xgpu_dynamic_lights_block *block, uint32_t count)
{
	const struct xgpu_stream_lt_light *list;
	int listed, index, drop;

	/* (HALO_LT_DROP in any mode: the classic's "off" too; the own
	projectile lights flare-only with the pass on: lt_flare_only) */
	drop = getenv("HALO_LT_DROP") != NULL;
	if (drop)
	{
		lt_settings();
		drop = lt.drop_kind != -2;
	}
	if (!drop && !xgpu_vulkan_lt_mode())
		return;
	list = xgpu_vulkan_lt_lights(xgpu_vulkan_lt_lights_window(), &listed, NULL);
	for (index = 0; index < (int)count && index < XGPU_DYNAMIC_LIGHTS_MAXIMUM; index++)
	{
		const struct xgpu_stream_lt_light *light = lt_record(list, listed, block->position[index]);
		int kind = drop ? lt_block_kind(block, index, list, listed) : -1;

		if ((drop && (lt.drop_kind == 9 || kind == lt.drop_kind || (lt.drop_kind == 10 && light &&
			light->id[0] == lt.drop_id))) || (xgpu_vulkan_lt_mode() && lt_flare_only(light)))
			block->color[index][0] = block->color[index][1] = block->color[index][2] = 0.0f;
	}
}

/* the frame's light table into lt.texels (and the GPU's), its rows' and
suns' counts into params; skip: the view's shadowed flashlight's index in
the block (-1 none), E1's own */
static void lt_table(const unsigned char *cells, const struct xgpu_dynamic_lights_block *block, int skip,
	struct lt_params *params)
{
	const struct xgpu_stream_lt_light *list;
	const uint32_t *panels = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_PANELS);
	int listed, rows = (int)block->grid[0][3], row, suns = 0, index, now, before;
	struct xgpu_vulkan_image_description description;

	lt_settings();
	list = xgpu_vulkan_lt_lights(xgpu_vulkan_lt_lights_window(), &listed, NULL);
	if (rows < 0)
		rows = 0;
	if (rows > XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		rows = XGPU_DYNAMIC_LIGHTS_MAXIMUM;
	now = lt.ids_current ^= 1;
	before = now ^ 1;
	memset(lt.texels, 0, sizeof(lt.texels));
	memset(lt.ids[now], 0, sizeof(lt.ids[now]));
	for (row = 0; row < LT_TABLE_ROWS; row++)
	{
		lt.texels[row][4][3] = (float)LT_KIND_SKIP;
		lt.texels[row][6][0] = lt.texels[row][6][1] = -1.0f;
	}
	for (row = 0; row < rows && lt.only != 1; row++)
	{
		const float *position = block->position[row];
		const struct xgpu_stream_lt_light *light;
		float (*t)[4] = lt.texels[row];
		int kind;

		/* (models only: the structure lit by the game's own gel pass, the
		models by the game's lights; a light of no colour) */
		if (row == skip || !(position[3] > 0.0f) || !(block->color[row][0] > 0.0f || block->color[row][1] > 0.0f ||
			block->color[row][2] > 0.0f))
			continue;
		light = lt_record(list, listed, position);
		if (lt.only == 3)
		{
			int kept = 0;

			for (index = 0; light && index < lt.only_ids; index++)
				kept |= light->id[0] == lt.only_id[index];
			if (!kept)
				continue;
		}
		if (light)
		{
			if (light->flags & XGPU_LT_LIGHT_MODELS_ONLY)
				continue;
			kind = light->kind == XGPU_LT_LIGHT_SPOT ? LT_KIND_SPOT : light->kind == XGPU_LT_LIGHT_PANEL ?
				LT_KIND_PANEL : LT_KIND_POINT;
			lt_set(t[0], light->emitter[0], light->emitter[1], light->emitter[2], light->emitter_radius);
			lt_set(t[1], light->forward[0], light->forward[1], light->forward[2],
				light->range > 0.0f ? 1.0f / light->range : 0.0f);
			lt_set(t[2], light->up[0], light->up[1], light->up[2], light->cone_inner);
			lt_set(t[3], light->left[0], light->left[1], light->left[2], light->cone_outer);
			lt_power(light, kind, t[4]);
			lt_set(t[5], (float)light->owner, (float)light->gel_slot, light->gel_width, 0.0f);
			lt.ids[now][row][0] = light->id[0];
			lt.ids[now][row][1] = light->id[1] | 1u << 23;
			/* (its stable id, 20 bits of it: the radiance cache's light-id
			hash, lt/indirect.glsl) */
			t[7][0] = (float)((light->id[0] ^ light->id[1] * 0x9e3779b9u) & 0xFFFFFu);
		}
		else
		{
			/* (the block alone: its cone (scale, offset: cos's ramp), its
			colour, lt_lights.c's disk) */
			float reach = 1.0f / position[3], disk = reach * LT_DISK_SCALE, scale = block->cone[row][3];
			const float *axis = block->cone[row];

			disk = disk < LT_DISK_LEAST ? LT_DISK_LEAST : disk > LT_DISK_MOST ? LT_DISK_MOST : disk;
			kind = panels[row >> 5] & (1u << (row & 31)) ? LT_KIND_PANEL : scale > 0.0f ? LT_KIND_SPOT : LT_KIND_POINT;
			lt_set(t[0], position[0], position[1], position[2], kind == LT_KIND_PANEL && block->color[row][3] > 0.0f ?
				sqrtf(block->color[row][3]) : disk);
			lt_set(t[1], axis[0], axis[1], axis[2], position[3]);
			{
				/* (an up and a left about the axis) */
				float ux = fabsf(axis[2]) < 0.9f ? 0.0f : 1.0f, uz = 1.0f - ux;
				float lx = axis[1] * uz, ly = axis[2] * ux - axis[0] * uz, lz = -axis[1] * ux;
				float length = sqrtf(lx * lx + ly * ly + lz * lz);

				length = length > 0.0f ? 1.0f / length : 0.0f;
				lx *= length;
				ly *= length;
				lz *= length;
				lt_set(t[3], lx, ly, lz, kind == LT_KIND_SPOT ? -block->color[row][3] / scale : -1.0f);
				lt_set(t[2], ly * axis[2] - lz * axis[1], lz * axis[0] - lx * axis[2], lx * axis[1] - ly * axis[0],
					kind == LT_KIND_SPOT ? -block->color[row][3] / scale + 1.0f / scale : -1.0f);
			}
			lt_set(t[4], block->color[row][0], block->color[row][1], block->color[row][2], 0.0f);
			/* (no id: its row's, for the cache's light-id hash) */
			t[7][0] = (float)(0x80000u | (unsigned)row);
			if (lt.kind_scale[kind] >= 0.0f)
				for (index = 0; index < 3; index++)
					t[4][index] *= lt.kind_scale[kind];
			lt_set(t[5], 0.0f, -1.0f, 0.0f, 0.0f);
		}
		t[4][3] = (float)kind;
	}
	for (index = 0; index < listed && suns < LT_SUNS && lt.sun_on && lt.only != 3; index++)
	{
		const struct xgpu_stream_lt_light *light = &list[index];
		float (*t)[4] = lt.texels[LT_SUN_ROW + suns];

		if (light->kind != XGPU_LT_LIGHT_DIRECTIONAL || !(light->flags & XGPU_LT_LIGHT_LIGHTMAP_DIRECT))
			continue;
		lt_set(t[1], light->forward[0], light->forward[1], light->forward[2], 0.0f);
		lt_set(t[2], light->up[0], light->up[1], light->up[2], -1.0f);
		lt_set(t[3], light->left[0], light->left[1], light->left[2], -1.0f);
		lt_power(light, light->angular_diameter >= LT_FILL_DIAMETER ? 4 : 3, t[4]);
		t[4][3] = (float)LT_KIND_DIRECTIONAL;
		lt_set(t[5], 0.0f, -1.0f, 0.0f, light->angular_diameter);
		suns++;
	}
	/* (each light's row the frame before, by its stable id, and the
	other way: the reservoirs' temporal reuse) */
	for (row = 0; row < rows; row++)
	{
		int other;

		if (!lt.ids[now][row][1])
			continue;
		for (other = 0; other < lt.rows[before]; other++)
		{
			if (lt.ids[before][other][0] == lt.ids[now][row][0] && lt.ids[before][other][1] == lt.ids[now][row][1])
			{
				lt.texels[row][6][0] = (float)other;
				lt.texels[other][6][1] = (float)row;
				break;
			}
		}
	}
	lt.rows[now] = rows;
	/* (HALO_LT_TABLE_LOG=1, checking: each frame's lights as the pass has
	them, for a light's id to drop or measure) */
	{
		static int table_log = -1;

		if (table_log < 0)
			table_log = getenv("HALO_LT_TABLE_LOG") && strcmp(getenv("HALO_LT_TABLE_LOG"), "0");
		for (row = 0; table_log && row < rows; row++)
			if (lt.texels[row][4][3] >= 0.0f)
				host_logf(HOST_LOG_WARN, "vulkan: light transport: table frame %.0f row %d id %08x %08x kind %.0f at "
					"%.2f %.2f %.2f emitter %.3f range %.2f power %.4f %.4f %.4f", (double)lt.ids_frame, row,
					lt.ids[now][row][0], lt.ids[now][row][1], (double)lt.texels[row][4][3],
					(double)lt.texels[row][0][0], (double)lt.texels[row][0][1], (double)lt.texels[row][0][2],
					(double)lt.texels[row][0][3], lt.texels[row][1][3] > 0.0f ? 1.0 / (double)lt.texels[row][1][3] : 0.0,
					(double)lt.texels[row][4][0], (double)lt.texels[row][4][1], (double)lt.texels[row][4][2]);
	}
	/* (the lights the pass lights: with no more than the 2 heroes, no
	reservoirs are drawn) */
	lt.handled = 0;
	for (row = 0; row < rows; row++)
		lt.handled += lt.texels[row][4][3] >= 0.0f;
	if (!lt.table)
	{
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA32F;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_COPIED;
		description.width = LT_TABLE_TEXELS;
		description.height = LT_TABLE_ROWS;
		description.depth_or_layers = 1;
		description.levels = 1;
		lt.table = xgpu_vulkan_image_create(&description);
	}
	if (lt.table)
		xgpu_vulkan_image_upload(lt.table, 0, 0, 0, 0, 0, LT_TABLE_TEXELS, LT_TABLE_ROWS, 1, lt.texels,
			LT_TABLE_TEXELS * 16, LT_TABLE_TEXELS * LT_TABLE_ROWS * 16);
	memcpy(params->grid[0], block->grid[0], sizeof(params->grid[0]));
	memcpy(params->grid[1], block->grid[1], sizeof(params->grid[1]));
	params->lights[0] = lt.table ? rows : 0;
	/* (none with HALO_LT_ONLY: a light's or the flashlight's alone, for
	their checks) */
	params->lights[1] = lt.table && !lt.only ? suns : 0;
	params->lights[2] = (lt.only >= 2 ? 0 : LT_LIGHTS_FLASHLIGHT) | (lt.heroes_on ? LT_LIGHTS_HEROES : 0) |
		(lt.restir_on && (lt.handled > 2 || !lt.heroes_on) ? LT_LIGHTS_RESTIR : 0);
}

/* the view's shadowed flashlight's index in the block (-1 none) */
static int lt_flashlight_index(const unsigned char *cells, const struct xgpu_dynamic_lights_block *block)
{
	const uint32_t *head = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 1);
	const uint32_t *slots = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_FLASHLIGHT);
	int light;

	if (!head[0] || head[0] > XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS)
		return -1;
	light = (int)slots[head[0] - 1] - 1;
	return light >= 0 && light < (int)block->grid[0][3] && light < XGPU_DYNAMIC_LIGHTS_MAXIMUM ? light : -1;
}

/* a pass's triangle over rectangle of into: the pre-pass's inputs, the last
mean, the last frame's inputs; the uniforms, the lights, the occlusion's and
the hits' sets */
static void lt_pass_draw(struct xgpu_vulkan_image *into, const int32_t rectangle[4],
	const struct xgpu_world_ao_pass *uniforms, const struct lt_params *params,
	const struct xgpu_vulkan_pipeline_description *pass, struct xgpu_vulkan_image *inputs,
	struct xgpu_vulkan_image *history, struct xgpu_vulkan_image *previous, struct xgpu_vulkan_image *guides,
	struct xgpu_vulkan_image *reservoirs)
{
	struct xgpu_vulkan_dynamic_state dynamic;
	struct xgpu_vulkan_draw draw;
	unsigned char *bytes;
	int stage;

	memset(&draw, 0, sizeof(draw));
	draw.pipeline = xgpu_vulkan_pipeline_ready(pass);
	if (!draw.pipeline)
		return;
	for (stage = 0; stage < 4; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = lt.sampler;
	}
	draw.textures[0].image = inputs;
	draw.textures[1].image = history;
	draw.textures[2].image = previous;
	draw.textures[3].image = guides ? guides : previous;
	/* (W2: the light table, the reservoirs; where there are none, an
	image of the pass's that the kernel then does not read) */
	for (stage = 4; stage < 6; stage++)
	{
		draw.textures[stage].kind = XGPU_VULKAN_IMAGE_2D;
		draw.textures[stage].sampler = lt.sampler;
	}
	draw.textures[4].image = lt.table ? lt.table : history;
	draw.textures[5].image = reservoirs ? reservoirs : history;
	/* (the draw set's uniforms' range, which the constants' share) */
	bytes = xgpu_vulkan_upload(192 * 16, 256, &draw.uniforms);
	if (!bytes)
		return;
	memset(bytes, 0, 192 * 16);
	memcpy(bytes, uniforms, sizeof(*uniforms));
	memcpy(bytes + LT_PARAMS_OFFSET, params, sizeof(*params));
	draw.constants = draw.uniforms;
	memset(&dynamic, 0, sizeof(dynamic));
	dynamic.viewport[0] = dynamic.scissor[0] = rectangle[0];
	dynamic.viewport[1] = dynamic.scissor[1] = rectangle[1];
	dynamic.viewport[2] = dynamic.scissor[2] = rectangle[2] - rectangle[0];
	dynamic.viewport[3] = dynamic.scissor[3] = rectangle[3] - rectangle[1];
	dynamic.depth_range[1] = 1.0f;
	draw.dynamic = &dynamic;
	draw.topology = XGPU_VULKAN_TOPOLOGY_TRIANGLES;
	draw.count = 3;
	draw.ambient_occlusion = 1;
	draw.dynamic_lights = 1;
	draw.kind[0] = LT_KIND;
	xgpu_vulkan_set_target_views(into, NULL, 0, 1);
	xgpu_vulkan_draw(&draw);
}

/* HALO_LT_E1_DUMP: the mean as half floats, written as its frame comes */
static void lt_dump_one(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs, int what, const char *suffix);

/* HALO_LT_E1_DUMP: the mean (the irradiance), the geometric and shaded
normals, the pixels the pre-pass drew */
static void lt_dump(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs)
{
	lt_dump_one(rectangle, uniforms, params, mean, inputs, 0, "");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_GEOMETRIC, "_normal_geometric");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_SHADED, "_normal_shaded");
	lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_PRESENT, "_present");
	/* (HALO_LT_E1_DUMP_REACH=<hex id>: each pixel's distance to that
	light over its range, whether it faces it, its owner: the per-kind
	calibration's band) */
	if (getenv("HALO_LT_E1_DUMP_REACH"))
	{
		/* (the row whose light's id's low word is the hex given) */
		uint32_t id = (uint32_t)strtoul(getenv("HALO_LT_E1_DUMP_REACH"), NULL, 16);
		int row;

		params->lights[3] = 0;
		for (row = 0; row < LT_TABLE_ROWS; row++)
			if (lt.ids[lt.ids_current][row][1] && lt.ids[lt.ids_current][row][0] == id)
				params->lights[3] = row;
		lt_dump_one(rectangle, uniforms, params, mean, inputs, LT_COPY_REACH, "_reach");
	}
}

static void lt_dump_one(const int32_t rectangle[4], const struct xgpu_world_ao_pass *uniforms, struct lt_params *params,
	struct xgpu_vulkan_image *mean, struct xgpu_vulkan_image *inputs, int what, const char *suffix)
{
	const struct xgpu_vulkan_image_description *size = xgpu_vulkan_image_description(mean);
	const char *folder = getenv("HALO_LT_E1_DUMP_DIR");
	uint16_t *halves = NULL;
	uint32_t header[4];
	char path[1024];
	FILE *file;

	if (!xgpu_vulkan_pipeline_ready(&lt.copy))
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.copy);
		return;
	}
	lt.dump = lt_image(lt.dump, XGPU_VULKAN_FORMAT_RGBA16F, size->width, size->height);
	if (!lt.dump)
		return;
	params->control[3] = LT_COPY | what;
	lt_pass_draw(lt.dump, rectangle, uniforms, params, &lt.copy, inputs, mean, lt.previous, lt.guides, NULL);
	if (xgpu_vulkan_read_halves(lt.dump, 0, &halves) != 1 || !halves)
		return;
	snprintf(path, sizeof(path), "%s/lt_e1_%lu%s.rgba16f", folder && *folder ? folder : ".", lt.since_start, suffix);
	file = fopen(path, "wb");
	if (file)
	{
		memcpy(header, "LTE1", 4);
		header[1] = size->width;
		header[2] = size->height;
		header[3] = (uint32_t)lt.since_start;
		fwrite(header, sizeof(header), 1, file);
		fwrite(halves, (size_t)size->width * size->height * 8, 1, file);
		fclose(file);
		host_logf(HOST_LOG_INFO, "vulkan: light transport E1: frame %lu's mean written (%s)", lt.since_start, path);
	}
	free(halves);
}

/* whether this frame's light is dumped (HALO_LT_E1_DUMP's frames,
HALO_LT_E1_DUMP_EVERY: every so many frames too, a held frame's) */
static int lt_dumping(void)
{
	static long every = -1;

	if (every < 0)
		every = getenv("HALO_LT_E1_DUMP_EVERY") ? atol(getenv("HALO_LT_E1_DUMP_EVERY")) : 0;
	return (lt.since_start < LT_DUMPS && lt.dumps[lt.since_start]) || (every > 0 && !(lt.since_start % every));
}

void xgpu_vulkan_lt_reshade(struct xgpu_vulkan_image *into, const struct xgpu_world_ao_pass *previous_pass,
	struct xgpu_vulkan_image *previous_inputs, struct xgpu_vulkan_image *previous_guides)
{
	const struct xgpu_vulkan_image_description *size;
	int32_t rectangle[4];
	int axis;

	if (!into || !previous_pass || !previous_inputs || !lt.made || !xgpu_vulkan_pipeline_ready(&lt.pass))
		return;
	size = xgpu_vulkan_image_description(previous_inputs);
	for (axis = 0; axis < 2; axis++)
	{
		int32_t extent = (int32_t)(axis ? size->height : size->width);
		int32_t low = previous_pass->corners[axis] < 0 ? 0 : previous_pass->corners[axis] > extent ? extent :
			previous_pass->corners[axis];
		int32_t high = previous_pass->corners[axis + 2] < low ? low : previous_pass->corners[axis + 2] > extent ?
			extent : previous_pass->corners[axis + 2];

		/* (a texel a 3x3 stratum) */
		rectangle[axis] = low / 3;
		rectangle[axis + 2] = (high + 2) / 3;
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return;
	lt_pass_draw(into, rectangle, previous_pass, &lt.reshade_params, &lt.pass, previous_inputs, lt.means[lt.current],
		previous_inputs, previous_guides, NULL);
}

int xgpu_vulkan_lt_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const unsigned char *cells, const struct xgpu_dynamic_lights_block *block)
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *history, *denoised = NULL, *reservoirs = NULL, *light_image, *diffuse;
	struct xgpu_vulkan_lt_indirect_frame indirect_frame;
	struct xgpu_vulkan_denoise_final shared;
	struct lt_params params;
	int32_t rectangle[4];
	int axis, light, start;

	if (!xgpu_vulkan_lt_mode())
		return 0;
	/* (what the pass reads is not what it draws into) */
	xgpu_vulkan_rt_bounce_set(NULL);
	if (!inputs || !pass || !cells || !block || !lt_shaders())
		return 1;
	if (!lt.prepared)
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.pass);
		/* (the denoiser's made with it, so that both are there together) */
		if (lt.mode == XGPU_VULKAN_LT_DENOISE)
			xgpu_vulkan_denoise_prepare();
		lt.prepared = 1;
	}
	if (!lt.dumps_read)
		lt_dumps_read();
	if (!xgpu_vulkan_pipeline_ready(&lt.pass) || !xgpu_vulkan_rt_build())
		return 1;
	size = xgpu_vulkan_image_description(inputs);
	for (axis = 0; axis < 2; axis++)
	{
		int32_t extent = (int32_t)(axis ? size->height : size->width);
		int32_t low = pass->corners[axis] < 0 ? 0 : pass->corners[axis] > extent ? extent : pass->corners[axis];
		int32_t high = pass->corners[axis + 2] < low ? low : pass->corners[axis + 2] > extent ? extent :
			pass->corners[axis + 2];

		rectangle[axis] = low;
		rectangle[axis + 2] = high;
	}
	if (rectangle[2] <= rectangle[0] || rectangle[3] <= rectangle[1])
		return 1;
	lt.means[0] = lt_image(lt.means[0], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
	lt.means[1] = lt_image(lt.means[1], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
	if (!lt.previous || xgpu_vulkan_image_description(lt.previous)->width != size->width ||
		xgpu_vulkan_image_description(lt.previous)->height != size->height)
		lt.previous_valid = 0;
	lt.previous = lt_image(lt.previous, XGPU_VULKAN_FORMAT_RGBA32_UINT, size->width, size->height);
	if (!lt.means[0] || !lt.means[1] || !lt.previous)
		return 1;
	memset(&params, 0, sizeof(params));
	light = lt_light(cells, block, &params);
	/* (W2: every other light, from the light table) */
	lt.ids_frame = (unsigned long)pass->history[1];
	lt_table(cells, block, light ? lt_flashlight_index(cells, block) : -1, &params);
	/* (the count started again as the light comes, goes or jumps, or the
	last frame's inputs are not had, or the pre-pass has no history: a cut,
	a replay's hold begun again) */
	start = light != lt.had_light || !lt.previous_valid || pass->history[0] == 0.0f;
	if (light && lt.had_light)
	{
		float dx = params.lamp[0] - lt.last_lamp[0], dy = params.lamp[1] - lt.last_lamp[1];
		float dz = params.lamp[2] - lt.last_lamp[2];

		start |= dx * dx + dy * dy + dz * dz > LT_JUMP * LT_JUMP;
	}
	lt.had_light = light;
	memcpy(lt.last_lamp, params.lamp, sizeof(lt.last_lamp));
	lt.since_start = start ? 1 : lt.since_start + 1;
	{
		static int unshadowed = -1;

		if (unshadowed < 0)
			unshadowed = getenv("HALO_LT_E1_UNSHADOWED") && strcmp(getenv("HALO_LT_E1_UNSHADOWED"), "0");
		params.control[0] = lt.mode == XGPU_VULKAN_LT_RAW || lt.mode == XGPU_VULKAN_LT_DENOISE ? 1 :
			lt.mode == XGPU_VULKAN_LT_E2 ? LT_E2_CAP : LT_ACCUMULATE_CAP;
		params.control[1] = light;
		params.control[2] = XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE;
		params.control[3] = (unshadowed ? 0 : LT_VISIBILITY) | (start ? LT_RESET : 0);
	}
	history = lt.means[lt.current];
	lt.current ^= 1;
	xgpu_vulkan_pass_owner(LT_OWNER);
	/* (W2's reservoirs: RIS and the temporal reuse, from the last frame's,
	which the main draw then reuses spatially) */
	if ((params.lights[2] & LT_LIGHTS_RESTIR) && lt.handled > 2 && lt.reuse_on)
	{
		struct lt_params reservoir_params = params;
		struct xgpu_vulkan_image *before;

		lt.reservoirs[0] = lt_image(lt.reservoirs[0], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
		lt.reservoirs[1] = lt_image(lt.reservoirs[1], XGPU_VULKAN_FORMAT_RGBA32F, size->width, size->height);
		if (lt.reservoirs[0] && lt.reservoirs[1])
		{
			before = lt.reservoirs[lt.reservoir_current];
			lt.reservoir_current ^= 1;
			reservoirs = lt.reservoirs[lt.reservoir_current];
			reservoir_params.control[3] = LT_RESERVOIR | (start || !lt.reservoirs_valid || !lt.previous_valid ?
				LT_RESET : 0);
			lt_pass_draw(reservoirs, rectangle, pass, &reservoir_params, &lt.pass, inputs, history, lt.previous,
				lt.guides, before);
			lt.reservoirs_valid = 1;
			params.lights[2] |= LT_LIGHTS_REUSE;
		}
	}
	else
		lt.reservoirs_valid = 0;
	lt_pass_draw(lt.means[lt.current], rectangle, pass, &params, &lt.pass, inputs, history, lt.previous, lt.guides,
		reservoirs);
	/* (the dynamic lights' indirect light and reflections,
	xgpu_vulkan_lt_indirect.c: the direct light and the bounce one diffuse
	signal from here) */
	memset(&indirect_frame, 0, sizeof(indirect_frame));
	indirect_frame.inputs = inputs;
	indirect_frame.previous_inputs = lt.previous;
	indirect_frame.guides = lt.guides;
	indirect_frame.table = lt.table;
	indirect_frame.direct = lt.means[lt.current];
	indirect_frame.pass = pass;
	indirect_frame.params = &params;
	indirect_frame.params_size = sizeof(params);
	indirect_frame.control_offset = offsetof(struct lt_params, control);
	indirect_frame.rectangle = rectangle;
	indirect_frame.mode = lt.mode;
	indirect_frame.cap = params.control[0];
	indirect_frame.start = start;
	indirect_frame.previous_valid = lt.previous_valid;
	/* (ours the denoiser: the bounce counted in its one temporal stage) */
	indirect_frame.fold = lt.mode == XGPU_VULKAN_LT_DENOISE && !xgpu_vulkan_dlss_lt_selected();
	diffuse = xgpu_vulkan_lt_indirect_draw(&indirect_frame);
	if (!diffuse)
		diffuse = lt.means[lt.current];
	/* (with the indirect light, twice as wide: the right half the
	reflections', xgpu_vulkan_lt_indirect_final) */
	lt.final = lt_image(lt.final, XGPU_VULKAN_FORMAT_RGBA32F, size->width * (xgpu_vulkan_lt_indirect_on() ? 2 : 1),
		size->height);
	memset(&shared, 0, sizeof(shared));
	if (lt.mode == XGPU_VULKAN_LT_DENOISE)
	{
		/* (the denoiser: this frame's sample and the last's, this frame's
		inputs and the last's (before they are copied over), this frame's
		light kept for its re-shade) */
		int had = lt.previous_valid;

		lt.reshade_params = params;
		lt.reshade_params.control[3] &= ~LT_RESET;
		lt.reshade_params.control[3] |= LT_GRADIENT;
		/* (no spatial reuse in the re-shade: the last frame's reservoirs
		are not kept; RIS alone, its seed the last frame's) */
		lt.reshade_params.lights[2] &= ~LT_LIGHTS_REUSE;
		/* (HALO_LT_DENOISER=rr on RTX: DLSS Ray Reconstruction instead,
		xgpu_vulkan_dlss.c; ours then not run at all) */
		if (xgpu_vulkan_dlss_lt_selected())
		{
			struct xgpu_vulkan_dlss_lt_cameras cameras;

			memcpy(cameras.camera, pass->camera, sizeof(cameras.camera));
			memcpy(cameras.previous, pass->previous, sizeof(cameras.previous));
			memcpy(cameras.previous_corners, pass->previous_corners, sizeof(cameras.previous_corners));
			denoised = xgpu_vulkan_dlss_lt_denoise(diffuse, inputs, lt.guides, &cameras, rectangle,
				!had || pass->history[0] == 0.0f);
		}
		else
		{
			/* (its gradients the direct light's alone: the re-shade's,
			against the last frame's direct sample; the bounce folded in by
			its guide pass) */
			/* (ours draws the light the surfaces read itself: its last
			pass the suns' share off, the reflections' half beside it) */
			struct xgpu_vulkan_denoise_bounce bounce;
			int sun;

			bounce.image = xgpu_vulkan_lt_indirect_bounce(&bounce.phase, &bounce.only);
			shared.final = lt.final;
			shared.wide = xgpu_vulkan_lt_indirect_on();
			for (sun = 0; sun < params.lights[1] && sun < LT_SUNS; sun++)
				if (lt.texels[LT_SUN_ROW + sun][4][3] >= 0.0f)
				{
					memcpy(shared.sun[2 * shared.suns], lt.texels[LT_SUN_ROW + sun][1], sizeof(shared.sun[0]));
					memcpy(shared.sun[2 * shared.suns + 1], lt.texels[LT_SUN_ROW + sun][4], sizeof(shared.sun[0]));
					shared.suns++;
				}
			shared.specular = xgpu_vulkan_lt_indirect_specular(&shared.coverage);
			shared.result = lt_dumping();
			denoised = xgpu_vulkan_denoise_frame(diffuse, had ? history : NULL, inputs, had ? lt.previous : NULL,
				lt.guides, pass, rectangle, bounce.image ? &bounce : NULL, &shared);
		}
		xgpu_vulkan_pass_owner(LT_OWNER);
	}
	/* (W2: the light the surfaces read: the light less the suns' whole
	direct share on the structure, which the light carries (lt/e1.frag's
	cmax): less than 0 where a dynamic object shadows the sun; the light's
	history and the denoiser the one temporal stage of both) */
	light_image = denoised ? denoised : diffuse;
	if (lt.final && shared.drawn)
	{
		light_image = lt.final;
		xgpu_vulkan_lt_indirect_finished(lt.final, shared.specular_denoised);
	}
	else if (lt.final)
	{
		struct lt_params final_params = params;

		final_params.control[3] = LT_FINAL;
		lt_pass_draw(lt.final, rectangle, pass, &final_params, &lt.pass, inputs, light_image, lt.previous, lt.guides,
			NULL);
		light_image = lt.final;
		if (xgpu_vulkan_lt_indirect_on())
			xgpu_vulkan_lt_indirect_final(&indirect_frame, lt.final);
	}
	/* (this frame's inputs kept for the next's reprojection) */
	xgpu_vulkan_image_copy(inputs, 0, 0, lt.previous, 0, 0);
	lt.previous_valid = 1;
	xgpu_vulkan_pass_owner(NULL);
	{
		if (lt_dumping())
		{
			lt_dump(rectangle, pass, &params, denoised ? denoised : diffuse, inputs);
			xgpu_vulkan_lt_indirect_dump(&indirect_frame, lt.since_start);
			/* (HALO_LT_E1_DUMP_FINAL: and the light the surfaces read, the
			suns' share in alpha) */
			if (getenv("HALO_LT_E1_DUMP_FINAL"))
				lt_dump_one(rectangle, pass, &params, light_image, inputs, 0, "_final");
		}
	}
	xgpu_vulkan_rt_bounce_set(light_image);
	if (!lt.said)
	{
		lt.said = 1;
		host_logf(HOST_LOG_INFO, "vulkan: light transport E1: the flashlight's direct light traced (%ux%u)",
			size->width, size->height);
	}
	return 1;
}
