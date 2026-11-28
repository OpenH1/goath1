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
/* a point light's k (lt_lights.c's lt_point_power_scale, the same rule:
~/dev/rt2-calibration.json's dynamic_power_scales): the classic light
model's own light (BRIGHTNESS x color^2.2 x W(d / r) / (1 + core d^2),
core = max(CORE / r^2, 1 / CORE_MOST^2)) over the pass's at k = 1, at half
the reach, so each light adds what the classic adds about it, whatever its
reach (a muzzle flash, an explosion, a bolt, the port's own lights) */
#define LT_CLASSIC_BRIGHTNESS 1.7f
#define LT_CLASSIC_CORE 12.0f
#define LT_CLASSIC_CORE_MOST 0.6f
/* the lamp moved this far in a frame (world units): a jump, the count started again */
#define LT_JUMP 0.25f
/* where the parameters start in the draw set's uniforms (lt/e1.frag's) */
#define LT_PARAMS_OFFSET 1024
#define LT_DUMPS 512

_Static_assert(sizeof(struct xgpu_world_ao_pass) <= LT_PARAMS_OFFSET, "the pass's uniforms before the parameters");
_Static_assert(XGPU_DYNAMIC_LIGHTS_GRID_Z == 16 && XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE == 80 &&
	XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF == 32 && XGPU_WORLD_CASCADES == 4 && XGPU_WORLD_AO_INPUT_PRESENT == 0x400u &&
	XGPU_SPIRV_RT_MASK_AO == 0x13u && XGPU_SPIRV_RT_HIT_TEXTURES == 512u, "lt/e1.frag's constants");

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
	/* the level's light sources (lt_emitter_tables): their cells' grid (its
	low corner, 1 / its cell's size), its cells along each axis and the
	groups (0 none), the triangles' and the cells' first texels and the
	triangles */
	float emitter_grid[4];
	int32_t emitter_counts[4];
	int32_t emitter_bases[4];
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
	/* (the level's light sources: each cell's share of rays reaching each
	of its groups, lt_emitter_visibility) */
	LT_EMITTER_VISIBILITY = 4096,
};
/* lights[2]'s (lt/e1.frag's LIGHTS_*) */
enum
{
	LT_LIGHTS_FLASHLIGHT = 1,
	LT_LIGHTS_HEROES = 2,
	LT_LIGHTS_RESTIR = 4,
	LT_LIGHTS_REUSE = 8,
	/* (a level lit live: lt/e1.frag's LIGHTS_REAL) */
	LT_LIGHTS_REAL = 16,
	/* (the level's light sources' reservoirs drawn: LIGHTS_EMITTER_REUSE) */
	LT_LIGHTS_EMITTER_REUSE = 32,
};
/* the light table (lt/e1.frag's): a row a light, LT_TABLE_TEXELS RGBA32F
texels; the block's lights' rows by their index there, the suns' from
LT_SUN_ROW */
#define LT_TABLE_TEXELS 8
#define LT_SUN_ROW XGPU_DYNAMIC_LIGHTS_MAXIMUM
#define LT_SUNS 8
/* (past the suns, the view's shadowed flashlight's row: the pass's own E1
light, which no block row carries for the pass; read by the one light
model's forward receivers alone, xgpu_spirv.h's XGPU_SPIRV_LT_FLASHLIGHT_ROW) */
#define LT_FLASHLIGHT_ROW (LT_SUN_ROW + LT_SUNS)
/* (then the level's light sources' strongest groups about the eye, as
panels, for the forward receivers alone: lt_lamp_rows) */
#define LT_LAMP_ROW (LT_FLASHLIGHT_ROW + 1)
#define LT_LAMPS 4
#define LT_TABLE_ROWS (LT_LAMP_ROW + LT_LAMPS)
/* (the pass's own table, the image its kernels read: the block's rows and
the suns' alone, as before the forward receivers; the flashlight's row and
the frame before's rows are in the forward table, an image of their own,
so that nothing the pass reads changes with them) */
#define LT_PASS_ROWS (LT_SUN_ROW + LT_SUNS)
/* the level's light sources (xgpu_vulkan_lt_emitters; ~/dev/rt2-realtime-
design.md section 11) in the pass's table below its rows, from
LT_EMITTER_ROW to LT_TABLE_HEIGHT, its texels in order (a row's 8 in turn):
- a group 2: its first triangle, its triangles, its area, the groups' power
  up to and with it over all of theirs; its radiance and its power's share;
- a triangle 3 (from the triangles' base): its first corner and the group's
  area up to and with it; its two edges from the first corner (the first's w its group);
- a cell of the grid over them LT_EMITTER_CELL_TEXELS (from the cells' base, rows): up to
  LT_EMITTER_CELL_GROUPS groups (-1 none) each with its chance up to and with
  it, the groups that light the cell most by their power over the distance
  squared (lt/e1.frag's emitter_sample mixes in the level's power too) */
#define LT_EMITTER_ROW LT_PASS_ROWS
#define LT_TABLE_HEIGHT 16384
/* the objects' own lamps this frame (xgpu_vulkan_lt_object_lamps) in the
table's last rows, from LT_OBJECT_LAMP_ROW (lt/e1.frag's OBJECT_LAMP_ROW),
4 texels a triangle: its first corner and the lamps' power share up to and
with it; its first edge and its area; its second edge and 1 where it glows
toward both sides (else 0, toward its normal); its radiance and its power's
share */
#define LT_OBJECT_LAMPS XGPU_VULKAN_LT_OBJECT_LAMPS_MAXIMUM
#define LT_OBJECT_LAMP_ROWS (LT_OBJECT_LAMPS * 4 / LT_TABLE_TEXELS)
#define LT_OBJECT_LAMP_ROW (LT_TABLE_HEIGHT - LT_OBJECT_LAMP_ROWS)
#define LT_EMITTER_TEXELS ((LT_OBJECT_LAMP_ROW - LT_EMITTER_ROW) * LT_TABLE_TEXELS)
#define LT_EMITTER_CELL_GROUPS 32
#define LT_EMITTER_CELL_TEXELS (LT_EMITTER_CELL_GROUPS / 2)
/* the grid's reach past the light sources (world units), and its least cell */
#define LT_EMITTER_MARGIN 24.0f
#define LT_EMITTER_CELL_LEAST 3.0f
_Static_assert(LT_OBJECT_LAMP_ROW == 16128, "lt/e1.frag's OBJECT_LAMP_ROW");
_Static_assert(LT_EMITTER_CELL_GROUPS == 32 && LT_EMITTER_CELL_TEXELS % LT_TABLE_TEXELS == 0 && LT_EMITTER_ROW == 136,
	"the light sources' rows as lt/e1.frag reads them (EMITTER_ROW, EMITTER_CELL_GROUPS)");
_Static_assert(LT_TABLE_TEXELS == XGPU_SPIRV_LT_TABLE_TEXELS && LT_SUN_ROW == XGPU_SPIRV_LT_SUN_ROW &&
	LT_SUNS == XGPU_SPIRV_LT_SUNS && LT_FLASHLIGHT_ROW == XGPU_SPIRV_LT_FLASHLIGHT_ROW &&
	LT_LAMP_ROW == XGPU_SPIRV_LT_LAMP_ROW && LT_LAMPS == XGPU_SPIRV_LT_LAMPS &&
	LT_TABLE_ROWS == XGPU_SPIRV_LT_TABLE_ROWS, "the light table as the forward receivers read it (xgpu_spirv.h)");
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
/* a level lit live: lt_lights.c's real k (its records carry them; these
for HALO_LT_REAL_SKY=1 on a recording made before) */
#define LT_REAL_SUN_POWER_SCALE 0.48f
#define LT_REAL_FILL_POWER_SCALE 0.48f
#define LT_REAL_AMBIENT_POWER_SCALE 0.48f

/* HALO_LT_REAL_SKY=1 (checking): a recording's suns lit live */
static int lt_real_replay(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("HALO_LT_REAL_SKY") && atoi(getenv("HALO_LT_REAL_SKY")) == 1;
	return on;
}
enum
{
	LT_KIND_SKIP = XGPU_SPIRV_LT_KIND_CLASSIC,
	/* (the block's row of the view's shadowed flashlight: not the pass's
	block row, lit from LT_FLASHLIGHT_ROW) */
	LT_KIND_FLASHLIGHT = XGPU_SPIRV_LT_KIND_FLASHLIGHT,
	/* (the pass's kinds from 0: lt/e1.frag's POINT, SPOT, PANEL, DIRECTIONAL) */
	LT_KIND_POINT = 0,
	LT_KIND_SPOT,
	LT_KIND_PANEL,
	LT_KIND_DIRECTIONAL,
};
/* (without a light list: the emitters' radii as lt_lights.c gives them) */
_Static_assert(LT_KIND_POINT == 0 && LT_KIND_SPOT == 1 && LT_KIND_PANEL == 2 && LT_KIND_DIRECTIONAL == 3,
	"the kinds as lt/e1.frag numbers them");
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
	/* (the forward table: this frame's rows, the flashlight's among them,
	then the frame before's: the one light model's forward receivers' and
	the denoiser's deterministic gradients', xgpu_vulkan_lt_table_halves) */
	struct xgpu_vulkan_image *forward_table;
	/* (each row's light is one the game placed on the first-person
	weapon (XGPU_LT_LIGHT_FIRST_PERSON): the forward table's texel 7's y) */
	unsigned char first_person[LT_TABLE_ROWS];
	int handled;
	float texels[LT_TABLE_ROWS][LT_TABLE_TEXELS][4];
	/* (the frame before's, kept below this frame's in the table's image
	(rows from LT_TABLE_ROWS): the denoiser's deterministic gradients,
	xgpu_vulkan_lt_table_halves) */
	float previous_texels[LT_TABLE_ROWS][LT_TABLE_TEXELS][4];
	int previous_rows;
	uint32_t ids[2][LT_TABLE_ROWS][2];
	int rows[2], ids_current, reservoir_current, reservoirs_valid;
	/* (the stream's frame, the pre-pass's history[1]: HALO_LT_TABLE_LOG's) */
	unsigned long ids_frame;
	/* the settings (read once): HALO_LT_ONLY, _HEROES, _RESTIR, _SUN,
	_KIND_SCALE, _DROP */
	int settings_read, only, heroes_on, restir_on, sun_on, reuse_on, only_ids, emitter_reservoirs_valid;
	uint32_t only_id[LT_ONLY_IDS];
	float kind_scale[5];
	int drop_kind;
	uint32_t drop_id;
	/* the last pass lit its level live by the sky's traced lights, the
	view in a cluster that sees the sky (the camera's contrast,
	xgpu_vulkan_lt_live; not where the level's light sources solved from
	its own lightmap are the light alone: their fit is the bake's light
	already, its spread in it) */
	int live;
	/* the light sources' tables (lt_emitter_tables): the set's number they
	are of (0 none), the one in the table's image, its parameters, whether
	the set is solved (the level lit live) */
	int emitter_version, emitter_uploaded, emitter_seen;
	struct xgpu_vulkan_image *emitter_visibility;
	float emitter_grid[4];
	int32_t emitter_counts[4], emitter_bases[4];
	int emitter_solved;
} lt = { -1 };

void xgpu_vulkan_lt_guides(struct xgpu_vulkan_image *guides)
{
	lt.guides = guides;
}

struct xgpu_vulkan_image *xgpu_vulkan_lt_table_halves(int32_t lights[4])
{
	lights[0] = lt.forward_table ? lt.rows[lt.ids_current] : 0;
	lights[1] = lt.forward_table ? lt.previous_rows : 0;
	lights[2] = LT_TABLE_ROWS;
	lights[3] = XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE;
	return lt.forward_table;
}

int xgpu_vulkan_lt_live(void)
{
	return xgpu_vulkan_lt_mode() && lt.live;
}

int xgpu_vulkan_lt_mode(void)
{
	/* (-1 not read; -2 not given: the stream's, the game's light lists) */
	if (lt.mode == -1)
	{
		const char *given = getenv("HALO_LT_E1");

		lt.mode = -2;
		if (given && *given)
		{
			lt.mode = XGPU_VULKAN_LT_OFF;
			if (strcmp(given, "off") && strcmp(given, "0") && strcmp(given, "false"))
				lt.mode = !strcmp(given, "raw") ? XGPU_VULKAN_LT_RAW : !strcmp(given, "accumulate") ?
					XGPU_VULKAN_LT_ACCUMULATE : !strcmp(given, "denoise") ? XGPU_VULKAN_LT_DENOISE : XGPU_VULKAN_LT_E2;
			if (lt.mode)
				host_logf(HOST_LOG_INFO, "vulkan: light transport E1: \"%s\" (the flashlight's direct light traced, %s)",
					given, lt.mode == XGPU_VULKAN_LT_RAW ? "1 spp, no history" : lt.mode == XGPU_VULKAN_LT_DENOISE ?
					"1 spp, through the denoiser (xgpu_vulkan_denoise.c)" : lt.mode == XGPU_VULKAN_LT_E2 ?
					"1 spp, counted up to 64 frames" : "counted up to 2^20 frames");
		}
	}
	/* (not given: display.ray_traced_lighting's, as the game's light lists
	say, the shipping configuration: the denoised pass) */
	if (lt.mode == -2)
		return xgpu_vulkan_lt_lights_on() ? XGPU_VULKAN_LT_DENOISE : XGPU_VULKAN_LT_OFF;
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
/* (the HALO_LT_* settings' generation: a reload's, so that the pass, the
bounce and the denoiser read them again; the replay's "rt_set") */
static unsigned lt_settings_generation = 1;

unsigned xgpu_vulkan_lt_settings_generation(void)
{
	return __atomic_load_n(&lt_settings_generation, __ATOMIC_ACQUIRE);
}

void xgpu_vulkan_lt_shaders_reload(const char *folder)
{
	if (folder)
	{
		snprintf(lt_shader_folder, sizeof(lt_shader_folder), "%s", folder);
		lt_shader_folder_read = 1;
	}
	__atomic_add_fetch(&lt_settings_generation, 1, __ATOMIC_ACQ_REL);
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

	if (lt.settings_read == (int)xgpu_vulkan_lt_settings_generation())
		return;
	lt.settings_read = (int)xgpu_vulkan_lt_settings_generation();
	lt.only_ids = 0;
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

/* (HALO_LT_TABLE_LOG: each row's record's flags this frame, its object's
absence in the top bit; 0 a row from the block alone) */
static uint32_t lt_row_flags[XGPU_DYNAMIC_LIGHTS_MAXIMUM];

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

/* a point light's k: the classic's light at half its reach over the
pass's there at k = 1, BRIGHTNESS max(d^2, emitter^2) / (1 + core d^2) at
d = r / 2 (both have the same range window): so it adds what the classic
adds where most of what it lights is seen; tools/rt2_effect_k.py measured
the light each kind adds over its held frames' pixels against the classic's
(~/dev/rt2-calibration.json's dynamic_power_scales: the need over this k
0.57 to 1.2, about 1 on average; the classic's whole flux over the reach
was 0.8 to 1.6); lt_lights.c's lt_point_power_scale, operation for operation */
static float lt_point_k(float range, float emitter_radius)
{
	float core = LT_CLASSIC_CORE / (range * range);
	float least = 1.0f / (LT_CLASSIC_CORE_MOST * LT_CLASSIC_CORE_MOST);
	float d2 = 0.25f * range * range, r2 = emitter_radius * emitter_radius;

	if (!(range > 0.0f))
		return 0.0f;
	core = core > least ? core : least;
	return LT_CLASSIC_BRIGHTNESS * (d2 > r2 ? d2 : r2) / (1.0f + core * d2);
}

/* a light's k by its kind (lt_lights.c's lt_power_scale, the same rule): a
point's from its reach (a recording made before carries its kind's old k,
taken off by the record's power_scale), the rest the flashlight's */
static float lt_kind_k(const struct xgpu_stream_lt_light *light)
{
	if ((light->flags & XGPU_LT_LIGHT_FLASHLIGHT) || light->kind != XGPU_LT_LIGHT_POINT)
		return LT_POWER_SCALE;
	return lt_point_k(light->range, light->emitter_radius);
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

void xgpu_vulkan_lt_block_filter(struct xgpu_dynamic_lights_block *block, uint32_t count)
{
	const struct xgpu_stream_lt_light *list;
	int listed, index;

	/* (in any mode: the classic's "off" too) */
	if (!getenv("HALO_LT_DROP"))
		return;
	lt_settings();
	if (lt.drop_kind == -2)
		return;
	list = xgpu_vulkan_lt_lights(xgpu_vulkan_lt_lights_window(), &listed, NULL);
	for (index = 0; index < (int)count && index < XGPU_DYNAMIC_LIGHTS_MAXIMUM; index++)
	{
		int kind = lt_block_kind(block, index, list, listed);
		const struct xgpu_stream_lt_light *light = lt.drop_kind == 10 ? lt_record(list, listed, block->position[index]) :
			NULL;

		if (lt.drop_kind == 9 || kind == lt.drop_kind || (light && light->id[0] == lt.drop_id))
			block->color[index][0] = block->color[index][1] = block->color[index][2] = 0.0f;
	}
}

/* ---------- the level's light sources (xgpu_vulkan_lt_lights.h's set)

Built once a set, into the table's rows from LT_EMITTER_ROW (above). Each
group's triangles are sampled by area, a group by its power: anywhere by its
share of the level's, and within the grid by its light at the cell's middle
(power over the distance squared, none where its plane faces away from the
whole cell), the cell's LT_EMITTER_CELL_GROUPS strongest. */

struct lt_emitter_group
{
	int first, count;
	float area, power, low[3], high[3], middle[3], axis[3];
	/* (every triangle facing within ~18 degrees of axis) */
	int flat;
};

static float lt_emitter_texels[LT_EMITTER_TEXELS][4];
/* each cell's groups (-1 none) and their light there unshadowed, the
strongest first, before the visibility (lt_emitter_visibility) scales them */
#define LT_EMITTER_CELLS_MAXIMUM (LT_EMITTER_TEXELS / LT_EMITTER_CELL_TEXELS)
static int lt_emitter_cell_groups[LT_EMITTER_CELLS_MAXIMUM][LT_EMITTER_CELL_GROUPS];
static float lt_emitter_cell_weights[LT_EMITTER_CELLS_MAXIMUM][LT_EMITTER_CELL_GROUPS];
/* each group's middle, axis (its summed normal, unit), area, radiance and
whether it is flat (lt_lamp_rows' panels) */
#define LT_EMITTER_GROUPS_MAXIMUM 1024
static struct
{
	float middle[3], axis[3], area, radiance[3];
	int flat;
} lt_emitter_group_info[LT_EMITTER_GROUPS_MAXIMUM];
/* each cell's groups' weights as its chances have them (after the
visibility) */
static float lt_emitter_cell_final[LT_EMITTER_CELLS_MAXIMUM][LT_EMITTER_CELL_GROUPS];
/* the share of its weight a group keeps where no ray from the cell reached
it: none of the light is ever unreachable (the sampling stays unbiased
anyway: lt/e1.frag mixes in the level's power) */
#define LT_EMITTER_VISIBILITY_FLOOR 0.02f

/* a cell's texels: its groups each with its chance up to and with it, the
weights scaled by each group's share of rays that reached it from the cell
(visibility, NULL: all) */
static void lt_emitter_cell_write(int cell, float (*texels)[4], const float *visibility)
{
	float weights[LT_EMITTER_CELL_GROUPS], sum = 0.0f, up_to = 0.0f;
	int slot, last = -1;

	for (slot = 0; slot < LT_EMITTER_CELL_GROUPS; slot++)
	{
		float seen = visibility ? visibility[slot] : 1.0f;

		weights[slot] = lt_emitter_cell_groups[cell][slot] >= 0 ? lt_emitter_cell_weights[cell][slot] *
			(seen > LT_EMITTER_VISIBILITY_FLOOR ? seen : LT_EMITTER_VISIBILITY_FLOOR) : 0.0f;
		lt_emitter_cell_final[cell][slot] = weights[slot];
		sum += weights[slot];
		if (weights[slot] > 0.0f)
			last = slot;
	}
	for (slot = 0; slot < LT_EMITTER_CELL_GROUPS; slot++)
	{
		float *texel = texels[slot >> 1] + 2 * (slot & 1);
		int kept = slot <= last && lt_emitter_cell_groups[cell][slot] >= 0;

		up_to += kept ? weights[slot] : 0.0f;
		texel[0] = kept ? (float)lt_emitter_cell_groups[cell][slot] : -1.0f;
		texel[1] = kept && sum > 0.0f ? (slot == last ? 1.0f : up_to / sum) : 2.0f;
	}
}

static void lt_emitter_cross(const float a[3], const float b[3], float out[3])
{
	out[0] = a[1] * b[2] - a[2] * b[1];
	out[1] = a[2] * b[0] - a[0] * b[2];
	out[2] = a[0] * b[1] - a[1] * b[0];
}

/* a group's light at a cell (its middle, half its diagonal): 0 where the
group's plane faces away from all the cell */
static float lt_emitter_weight(const struct lt_emitter_group *group, const float middle[3], float half)
{
	float d2 = 0.0f, facing = 0.0f;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float below = group->low[axis] - middle[axis], above = middle[axis] - group->high[axis];
		float out = below > 0.0f ? below : above > 0.0f ? above : 0.0f;

		d2 += out * out;
		facing += (middle[axis] - group->middle[axis]) * group->axis[axis];
	}
	if (group->flat && facing < -half - 0.05f)
		return 0.0f;
	return group->power / (d2 > half * half ? d2 : half * half);
}

static void lt_emitter_tables(void)
{
	const struct xgpu_vulkan_lt_emitter *set;
	struct lt_emitter_group *groups;
	int count, group_count, solved, version, triangle, index, cell, cells, dims[3], axis;
	int triangle_base, cell_base;
	float total = 0.0f, running = 0.0f, low[3] = { 1.0e30f, 1.0e30f, 1.0e30f }, high[3] = { -1.0e30f, -1.0e30f, -1.0e30f };
	float size;

	set = xgpu_vulkan_lt_emitters(&count, &group_count, &solved, &version);
	if (version == lt.emitter_version)
		return;
	lt.emitter_version = version;
	memset(lt.emitter_counts, 0, sizeof(lt.emitter_counts));
	lt.emitter_solved = 0;
	triangle_base = 2 * group_count;
	cell_base = (triangle_base + 3 * count + LT_TABLE_TEXELS - 1) / LT_TABLE_TEXELS * LT_TABLE_TEXELS;
	if (!set || count <= 0 || group_count <= 0 || group_count > LT_EMITTER_GROUPS_MAXIMUM ||
		cell_base + LT_TABLE_TEXELS > LT_EMITTER_TEXELS)
	{
		if (set)
			host_logf(HOST_LOG_WARN, "vulkan: light transport: the level's light sources (%d triangles) do not fit",
				count);
		return;
	}
	groups = calloc((size_t)group_count, sizeof(*groups));
	if (!groups)
		return;
	memset(lt_emitter_texels, 0, sizeof(lt_emitter_texels));
	for (index = 0; index < group_count; index++)
	{
		groups[index].first = -1;
		for (axis = 0; axis < 3; axis++)
		{
			groups[index].low[axis] = 1.0e30f;
			groups[index].high[axis] = -1.0e30f;
		}
		groups[index].flat = 1;
	}
	/* (the triangles in their groups' order: the game sends them so) */
	for (triangle = 0; triangle < count; triangle++)
	{
		const struct xgpu_vulkan_lt_emitter *in = &set[triangle];
		struct lt_emitter_group *group = &groups[in->group];
		float e1[3], e2[3], normal[3], area;
		int corner;

		if (group->first < 0)
			group->first = triangle;
		group->count = triangle - group->first + 1;
		for (axis = 0; axis < 3; axis++)
		{
			e1[axis] = in->corners[3 + axis] - in->corners[axis];
			e2[axis] = in->corners[6 + axis] - in->corners[axis];
		}
		lt_emitter_cross(e1, e2, normal);
		area = 0.5f * sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		group->area += area;
		for (axis = 0; axis < 3; axis++)
		{
			group->axis[axis] += normal[axis];
			for (corner = 0; corner < 3; corner++)
			{
				float v = in->corners[3 * corner + axis];

				group->low[axis] = v < group->low[axis] ? v : group->low[axis];
				group->high[axis] = v > group->high[axis] ? v : group->high[axis];
			}
		}
		lt_set(lt_emitter_texels[triangle_base + 3 * triangle], in->corners[0], in->corners[1], in->corners[2],
			group->area);
		lt_set(lt_emitter_texels[triangle_base + 3 * triangle + 1], e1[0], e1[1], e1[2], (float)in->group);
		lt_set(lt_emitter_texels[triangle_base + 3 * triangle + 2], e2[0], e2[1], e2[2], 0.0f);
	}
	for (index = 0; index < group_count; index++)
	{
		struct lt_emitter_group *group = &groups[index];
		const float *radiance = group->first >= 0 ? set[group->first].radiance : NULL;
		float length = sqrtf(group->axis[0] * group->axis[0] + group->axis[1] * group->axis[1] +
			group->axis[2] * group->axis[2]);

		if (!radiance || group->area <= 0.0f)
			continue;
		group->power = (0.2126f * radiance[0] + 0.7152f * radiance[1] + 0.0722f * radiance[2]) * group->area;
		for (axis = 0; axis < 3; axis++)
		{
			group->middle[axis] = 0.5f * (group->low[axis] + group->high[axis]);
			group->axis[axis] = length > 0.0f ? group->axis[axis] / length : 0.0f;
			low[axis] = group->low[axis] < low[axis] ? group->low[axis] : low[axis];
			high[axis] = group->high[axis] > high[axis] ? group->high[axis] : high[axis];
		}
		/* (flat: the summed normal nearly as long as the triangles' areas'
		sum, twice the area) */
		group->flat = length >= 0.95f * 2.0f * group->area;
		total += group->power;
	}
	if (!(total > 0.0f))
	{
		free(groups);
		return;
	}
	for (index = 0; index < group_count; index++)
	{
		const struct lt_emitter_group *group = &groups[index];
		const float *radiance = group->first >= 0 ? set[group->first].radiance : NULL;

		running += group->power;
		memcpy(lt_emitter_group_info[index].middle, group->middle, sizeof(group->middle));
		memcpy(lt_emitter_group_info[index].axis, group->axis, sizeof(group->axis));
		lt_emitter_group_info[index].area = group->area;
		lt_emitter_group_info[index].flat = group->flat && group->power > 0.0f;
		if (radiance)
			memcpy(lt_emitter_group_info[index].radiance, radiance, sizeof(lt_emitter_group_info[index].radiance));
		lt_set(lt_emitter_texels[2 * index], (float)(group->first < 0 ? 0 : group->first), (float)group->count,
			group->area, index == group_count - 1 ? 1.0f : running / total);
		if (radiance && group->power > 0.0f)
			lt_set(lt_emitter_texels[2 * index + 1], radiance[0], radiance[1], radiance[2], group->power / total);
	}
	/* (the grid: cells as small as its rows let them be) */
	cells = (LT_EMITTER_TEXELS - cell_base) / LT_EMITTER_CELL_TEXELS;
	for (axis = 0; axis < 3; axis++)
	{
		low[axis] -= LT_EMITTER_MARGIN;
		high[axis] += LT_EMITTER_MARGIN;
	}
	for (size = LT_EMITTER_CELL_LEAST;; size *= 1.125f)
	{
		long all = 1;

		for (axis = 0; axis < 3; axis++)
		{
			dims[axis] = (int)ceilf((high[axis] - low[axis]) / size);
			dims[axis] = dims[axis] < 1 ? 1 : dims[axis];
			all *= dims[axis];
		}
		if (all <= cells)
		{
			cells = (int)all;
			break;
		}
	}
	for (cell = 0; cell < cells; cell++)
	{
		float middle[3], weights[LT_EMITTER_CELL_GROUPS];
		int chosen[LT_EMITTER_CELL_GROUPS], kept = 0, at = cell, slot;
		float (*texels)[4] = &lt_emitter_texels[cell_base + cell * LT_EMITTER_CELL_TEXELS];

		for (axis = 0; axis < 3; axis++)
		{
			middle[axis] = low[axis] + ((float)(at % dims[axis]) + 0.5f) * size;
			at /= dims[axis];
		}
		for (index = 0; index < group_count; index++)
		{
			float w = groups[index].power > 0.0f ? lt_emitter_weight(&groups[index], middle, 0.5f * size * 1.7320508f) :
				0.0f;

			if (!(w > 0.0f))
				continue;
			/* (kept strongest first) */
			if (kept == LT_EMITTER_CELL_GROUPS && w <= weights[kept - 1])
				continue;
			slot = kept < LT_EMITTER_CELL_GROUPS ? kept++ : kept - 1;
			while (slot > 0 && weights[slot - 1] < w)
			{
				weights[slot] = weights[slot - 1];
				chosen[slot] = chosen[slot - 1];
				slot--;
			}
			weights[slot] = w;
			chosen[slot] = index;
		}
		for (slot = 0; slot < LT_EMITTER_CELL_GROUPS; slot++)
		{
			lt_emitter_cell_groups[cell][slot] = slot < kept ? chosen[slot] : -1;
			lt_emitter_cell_weights[cell][slot] = slot < kept ? weights[slot] : 0.0f;
		}
		lt_emitter_cell_write(cell, texels, NULL);
	}
	free(groups);
	lt.emitter_grid[0] = low[0];
	lt.emitter_grid[1] = low[1];
	lt.emitter_grid[2] = low[2];
	lt.emitter_grid[3] = 1.0f / size;
	lt.emitter_counts[0] = dims[0];
	lt.emitter_counts[1] = dims[1];
	lt.emitter_counts[2] = dims[2];
	lt.emitter_counts[3] = group_count;
	lt.emitter_bases[0] = triangle_base;
	lt.emitter_bases[1] = cell_base;
	lt.emitter_bases[2] = count;
	lt.emitter_bases[3] = 0;
	lt.emitter_solved = solved;
	lt.emitter_uploaded = 0;
	lt.emitter_seen = 0;
	host_logf(HOST_LOG_INFO, "vulkan: light transport: the level's light sources as lights: %d triangles, %d groups "
		"(power %.3g), a grid of %dx%dx%d cells of %.2f wu", count, group_count, (double)total, dims[0], dims[1],
		dims[2], (double)size);
}

/* the forward receivers' lamps (the first-person weapon, the blended and
self-lit passes, which the pass's samples do not reach: xgpu_spirv.c's
lt_forward_light): the LT_LAMPS flat groups that light the eye's cell most
(its chances' weights, visibility in them), each a one-sided Lambertian disk
of its area at its middle facing its axis, its radiance the panel's power
(lt/e1.frag's PANEL: L r^2 / (r^2 + d^2) on its axis, the disk's light) */
static void lt_lamp_rows(const float eye[3])
{
	int cell = -1, axis, slot, lamp, taken[LT_LAMPS], count = 0;
	float best[LT_LAMPS];

	for (axis = 0, cell = 0; axis < 3; axis++)
	{
		int c = (int)floorf((eye[axis] - lt.emitter_grid[axis]) * lt.emitter_grid[3]);

		if (c < 0 || c >= lt.emitter_counts[axis])
			return;
		cell += c * (axis == 0 ? 1 : axis == 1 ? lt.emitter_counts[0] : lt.emitter_counts[0] * lt.emitter_counts[1]);
	}
	for (slot = 0; slot < LT_EMITTER_CELL_GROUPS; slot++)
	{
		int group = lt_emitter_cell_groups[cell][slot], at;
		float w = lt_emitter_cell_final[cell][slot];

		if (group < 0 || group >= LT_EMITTER_GROUPS_MAXIMUM || !lt_emitter_group_info[group].flat || !(w > 0.0f))
			continue;
		if (count == LT_LAMPS && w <= best[count - 1])
			continue;
		at = count < LT_LAMPS ? count++ : count - 1;
		while (at > 0 && best[at - 1] < w)
		{
			best[at] = best[at - 1];
			taken[at] = taken[at - 1];
			at--;
		}
		best[at] = w;
		taken[at] = group;
	}
	for (lamp = 0; lamp < count; lamp++)
	{
		float (*t)[4] = lt.texels[LT_LAMP_ROW + lamp];
		const float *f = lt_emitter_group_info[taken[lamp]].axis;
		float up[3], left[3], length;

		/* (any frame about its axis: the disk is round) */
		if (fabsf(f[2]) < 0.9f)
			lt_emitter_cross(f, (const float[3]){ 0.0f, 0.0f, 1.0f }, up);
		else
			lt_emitter_cross(f, (const float[3]){ 1.0f, 0.0f, 0.0f }, up);
		length = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
		for (axis = 0; axis < 3; axis++)
			up[axis] /= length;
		lt_emitter_cross(f, up, left);
		lt_set(t[0], lt_emitter_group_info[taken[lamp]].middle[0], lt_emitter_group_info[taken[lamp]].middle[1],
			lt_emitter_group_info[taken[lamp]].middle[2], sqrtf(lt_emitter_group_info[taken[lamp]].area / 3.14159265f));
		lt_set(t[1], f[0], f[1], f[2], 0.0f);
		lt_set(t[2], up[0], up[1], up[2], -1.0f);
		lt_set(t[3], left[0], left[1], left[2], -1.0f);
		lt_set(t[4], lt_emitter_group_info[taken[lamp]].radiance[0], lt_emitter_group_info[taken[lamp]].radiance[1],
			lt_emitter_group_info[taken[lamp]].radiance[2], (float)LT_KIND_PANEL);
		lt_set(t[5], 0.0f, -1.0f, 0.0f, 0.0f);
		lt_set(t[6], -1.0f, -1.0f, 0.0f, 0.0f);
	}
}

/* the frame's object lamps into the table's last rows (LT_OBJECT_LAMP_ROW):
their count (0 none) */
static int lt_object_lamp_rows(const float eye[3])
{
	static float texels[LT_OBJECT_LAMPS * 4][4];
	const struct xgpu_vulkan_lt_object_lamp *lamps;
	float total = 0.0f, running = 0.0f, power[LT_OBJECT_LAMPS], area[LT_OBJECT_LAMPS];
	int count, index, axis, used = 0;

	lamps = xgpu_vulkan_lt_object_lamps(xgpu_vulkan_lt_lights_window(), &count);
	if (!lamps || count <= 0 || !lt.table)
		return 0;
	if (count > LT_OBJECT_LAMPS)
		count = LT_OBJECT_LAMPS;
	for (index = 0; index < count; index++)
	{
		const struct xgpu_vulkan_lt_object_lamp *lamp = &lamps[index];
		float e1[3], e2[3], normal[3];

		for (axis = 0; axis < 3; axis++)
		{
			e1[axis] = lamp->corners[3 + axis] - lamp->corners[axis];
			e2[axis] = lamp->corners[6 + axis] - lamp->corners[axis];
		}
		lt_emitter_cross(e1, e2, normal);
		area[index] = 0.5f * sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
		/* (drawn by its power over its distance squared from the eye, no
		nearer than its size: a far Pelican's bright strips do not take the
		candidates from a near shield; the pass divides by the chance) */
		{
			float to[3], d2;

			for (axis = 0; axis < 3; axis++)
				to[axis] = (lamp->corners[axis] + lamp->corners[3 + axis] + lamp->corners[6 + axis]) / 3.0f - eye[axis];
			d2 = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
			power[index] = (0.2126f * lamp->radiance[0] + 0.7152f * lamp->radiance[1] + 0.0722f * lamp->radiance[2]) *
				area[index] / (d2 > area[index] ? d2 : area[index]) * (lamp->two_sided ? 2.0f : 1.0f);
		}
		if (!(power[index] > 0.0f) || !isfinite(power[index]))
			power[index] = 0.0f;
		total += power[index];
	}
	if (!(total > 0.0f))
		return 0;
	for (index = 0; index < count; index++)
	{
		const struct xgpu_vulkan_lt_object_lamp *lamp = &lamps[index];

		running += power[index];
		lt_set(texels[4 * index], lamp->corners[0], lamp->corners[1], lamp->corners[2],
			index == count - 1 ? 1.0f : running / total);
		lt_set(texels[4 * index + 1], lamp->corners[3] - lamp->corners[0], lamp->corners[4] - lamp->corners[1],
			lamp->corners[5] - lamp->corners[2], area[index]);
		lt_set(texels[4 * index + 2], lamp->corners[6] - lamp->corners[0], lamp->corners[7] - lamp->corners[1],
			lamp->corners[8] - lamp->corners[2], lamp->two_sided ? 1.0f : 0.0f);
		lt_set(texels[4 * index + 3], lamp->radiance[0], lamp->radiance[1], lamp->radiance[2], power[index] / total);
	}
	used = (4 * count + LT_TABLE_TEXELS - 1) / LT_TABLE_TEXELS;
	xgpu_vulkan_image_upload(lt.table, 0, 0, 0, LT_OBJECT_LAMP_ROW, 0, LT_TABLE_TEXELS, (uint32_t)used, 1, texels,
		LT_TABLE_TEXELS * 16, LT_TABLE_TEXELS * (uint32_t)used * 16);
	return count;
}

/* the frame's light table into lt.texels (and the GPU's), its rows' and
suns' counts into params; skip: the view's shadowed flashlight's index in
the block (-1 none), E1's own */
static void lt_table(const unsigned char *cells, const struct xgpu_dynamic_lights_block *block, int skip,
	struct lt_params *params)
{
	const struct xgpu_stream_lt_light *list;
	const uint32_t *panels = lt_texel(cells, XGPU_DYNAMIC_LIGHTS_PANELS);
	int listed, rows = (int)block->grid[0][3], row, suns = 0, index, now, before, real = 0;
	struct xgpu_vulkan_image_description description;

	lt_settings();
	list = xgpu_vulkan_lt_lights(xgpu_vulkan_lt_lights_window(), &listed, NULL);
	if (rows < 0)
		rows = 0;
	if (rows > XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		rows = XGPU_DYNAMIC_LIGHTS_MAXIMUM;
	now = lt.ids_current ^= 1;
	before = now ^ 1;
	memcpy(lt.previous_texels, lt.texels, sizeof(lt.texels));
	memset(lt.first_person, 0, sizeof(lt.first_person));
	lt.previous_rows = lt.rows[before];
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
		lt_row_flags[row] = light ? light->flags | (light->object == 0xffffffffu ? 0x80000000u : 0u) : 0u;
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
			/* (w: the instance the light leaves unlit, 0 none: its owner,
			where its tag says "don't light own object" (a Covenant door's
			light, inside the door: the original leaves it out of the
			object's lights) or where it is a projectile's (a plasma bolt,
			a needle: the glowing thing is the light, so a ray that meets
			it has met the light, lt/indirect.glsl's direct_one). Its
			instance among the ray-traced scene's: world_rt_owner's
			arithmetic on the object's handle for records made before
			lt_lights.c gave every light its owner) */
			lt_set(t[5], (float)light->owner, (float)light->gel_slot, light->gel_width,
				(light->flags & (XGPU_LT_LIGHT_PROJECTILE | XGPU_LT_LIGHT_NOT_OWN_OBJECT)) &&
				(light->owner || light->object != 0xffffffffu) ?
				(float)(light->owner ? light->owner : (light->object & 0xFFFFu) + 1u) : 0.0f);
			lt.first_person[row] = (light->flags & XGPU_LT_LIGHT_FIRST_PERSON) != 0;
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
		/* (a level lit live: the record's power, its own k (the sky's whole
		light, lt_lights.c's real k), the dome among them) */
		if (light->flags & XGPU_LT_LIGHT_REAL_SKY)
		{
			lt_set(t[4], light->power[0], light->power[1], light->power[2], 0.0f);
			real = 1;
		}
		else if (lt_real_replay() && light->power_scale > 0.0f)
		{
			/* (HALO_LT_REAL_SKY=1, checking a recording made before: its
			suns lit live by lt_lights.c's real k) */
			float k = (light->angular_diameter >= LT_FILL_DIAMETER ? LT_REAL_FILL_POWER_SCALE :
				LT_REAL_SUN_POWER_SCALE) / light->power_scale;

			lt_set(t[4], light->power[0] * k, light->power[1] * k, light->power[2] * k, 0.0f);
			real = 1;
		}
		else
			lt_power(light, light->angular_diameter >= LT_FILL_DIAMETER ? 4 : 3, t[4]);
		t[4][3] = (float)LT_KIND_DIRECTIONAL;
		lt_set(t[5], 0.0f, -1.0f, 0.0f, light->angular_diameter);
		suns++;
	}
	/* (HALO_LT_REAL_SKY=1 on a recording made before: the sky's dome,
	HALO_LT_DOME's colour x power (b30's by default), by the real k) */
	if (real && lt_real_replay() && suns < LT_SUNS)
	{
		float (*t)[4] = lt.texels[LT_SUN_ROW + suns];
		float dome[3] = { 0.572549f * 0.12f, 0.552941f * 0.12f, 0.705882f * 0.12f };
		const char *given = getenv("HALO_LT_DOME");

		if (given)
			sscanf(given, "%f,%f,%f", &dome[0], &dome[1], &dome[2]);
		lt_set(t[1], 0.0f, 0.0f, 1.0f, 0.0f);
		lt_set(t[2], 1.0f, 0.0f, 0.0f, -1.0f);
		lt_set(t[3], 0.0f, 1.0f, 0.0f, -1.0f);
		lt_set(t[4], dome[0] * LT_REAL_AMBIENT_POWER_SCALE, dome[1] * LT_REAL_AMBIENT_POWER_SCALE,
			dome[2] * LT_REAL_AMBIENT_POWER_SCALE, (float)LT_KIND_DIRECTIONAL);
		lt_set(t[5], 0.0f, -1.0f, 0.0f, 6.2831853f);
		suns++;
	}
	/* (the view's shadowed flashlight, E1's light (its parameters, lt_light),
	in its own row for the one light model's forward receivers, the pass's
	formula as a spot: the disk at the lamp, the gel from the game's light
	(its origin in texel 6), the range window by the light's radius, the
	linear power pow(colour, 2.2) by the flashlight's k; its block row marked
	so the forward receivers' classic sum leaves it out) */
	if (skip >= 0 && skip < rows)
	{
		float (*t)[4] = lt.texels[LT_FLASHLIGHT_ROW];

		lt_set(t[0], params->lamp[0], params->lamp[1], params->lamp[2], LT_DISK_RADIUS);
		lt_set(t[1], params->forward[0], params->forward[1], params->forward[2], fabsf(params->light[3]));
		lt_set(t[2], params->up[0], params->up[1], params->up[2], -1.0f);
		lt_set(t[3], params->left[0], params->left[1], params->left[2], -1.0f);
		for (index = 0; index < 3; index++)
			t[4][index] = powf(params->color[index] > 0.0f ? params->color[index] : 0.0f, 2.2f) * LT_POWER_SCALE;
		t[4][3] = (float)LT_KIND_SPOT;
		lt_set(t[5], (float)params->owners[0], 0.0f, params->color[3], 0.0f);
		lt_set(t[6], params->light[0], params->light[1], params->light[2], 1.0f);
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
					"%.2f %.2f %.2f emitter %.3f range %.2f power %.4f %.4f %.4f flags %08x", (double)lt.ids_frame, row,
					lt.ids[now][row][0], lt.ids[now][row][1], (double)lt.texels[row][4][3],
					(double)lt.texels[row][0][0], (double)lt.texels[row][0][1], (double)lt.texels[row][0][2],
					(double)lt.texels[row][0][3], lt.texels[row][1][3] > 0.0f ? 1.0 / (double)lt.texels[row][1][3] : 0.0,
					(double)lt.texels[row][4][0], (double)lt.texels[row][4][1], (double)lt.texels[row][4][2],
					(unsigned)lt_row_flags[row]);
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
		/* (the light sources' rows below the pass's) */
		description.height = LT_TABLE_HEIGHT;
		description.depth_or_layers = 1;
		description.levels = 1;
		lt.table = xgpu_vulkan_image_create(&description);
		lt.emitter_uploaded = 0;
	}
	if (lt.table)
		xgpu_vulkan_image_upload(lt.table, 0, 0, 0, 0, 0, LT_TABLE_TEXELS, LT_PASS_ROWS, 1, lt.texels,
			LT_TABLE_TEXELS * 16, LT_TABLE_TEXELS * LT_PASS_ROWS * 16);
	/* (the level's light sources, once a set) */
	lt_emitter_tables();
	if (lt.table && lt.emitter_counts[3] > 0 && !lt.emitter_uploaded)
	{
		uint32_t used = (uint32_t)((lt.emitter_bases[1] + LT_EMITTER_CELL_TEXELS * lt.emitter_counts[0] *
			lt.emitter_counts[1] * lt.emitter_counts[2]) / LT_TABLE_TEXELS);

		xgpu_vulkan_image_upload(lt.table, 0, 0, 0, LT_EMITTER_ROW, 0, LT_TABLE_TEXELS, used, 1, lt_emitter_texels,
			LT_TABLE_TEXELS * 16, LT_TABLE_TEXELS * used * 16);
		lt.emitter_uploaded = 1;
	}
	/* (the forward receivers' lamps, where the level is lit live by them) */
	if (((real && suns > 0) || lt.emitter_solved) && lt.table && lt.emitter_counts[3] > 0 && lt.emitter_uploaded &&
		!lt.only)
		lt_lamp_rows(block->grid[0]);
	/* (the forward table: this frame's rows, then the frame before's) */
	if (!lt.forward_table)
	{
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA32F;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_COPIED;
		description.width = LT_TABLE_TEXELS;
		description.height = 2 * LT_TABLE_ROWS;
		description.depth_or_layers = 1;
		description.levels = 1;
		lt.forward_table = xgpu_vulkan_image_create(&description);
	}
	if (lt.forward_table)
	{
		static float halves[2 * LT_TABLE_ROWS][LT_TABLE_TEXELS][4];

		memcpy(halves, lt.texels, sizeof(lt.texels));
		memcpy(halves[LT_TABLE_ROWS], lt.previous_texels, sizeof(lt.previous_texels));
		/* (the block's row of the flashlight its own row lights: left out
		of the forward receivers' classic sum) */
		if (skip >= 0 && skip < rows)
			halves[skip][4][3] = (float)LT_KIND_FLASHLIGHT;
		for (row = 0; row < rows; row++)
			halves[row][7][1] = lt.first_person[row] ? 1.0f : 0.0f;
		xgpu_vulkan_image_upload(lt.forward_table, 0, 0, 0, 0, 0, LT_TABLE_TEXELS, 2 * LT_TABLE_ROWS, 1, halves,
			LT_TABLE_TEXELS * 16, LT_TABLE_TEXELS * 2 * LT_TABLE_ROWS * 16);
	}
	/* (the forward receivers read it at the occlusion set's binding) */
	xgpu_vulkan_lt_table_set(lt.forward_table);
	memcpy(params->grid[0], block->grid[0], sizeof(params->grid[0]));
	memcpy(params->grid[1], block->grid[1], sizeof(params->grid[1]));
	params->lights[0] = lt.table ? rows : 0;
	/* (none with HALO_LT_ONLY: a light's or the flashlight's alone, for
	their checks) */
	params->lights[1] = lt.table && !lt.only ? suns : 0;
	params->lights[2] = (lt.only >= 2 ? 0 : LT_LIGHTS_FLASHLIGHT) | (lt.heroes_on ? LT_LIGHTS_HEROES : 0) |
		(lt.restir_on && (lt.handled > 2 || !lt.heroes_on) ? LT_LIGHTS_RESTIR : 0) |
		(real && params->lights[1] > 0 ? LT_LIGHTS_REAL : 0) |
		(lt.table && lt.emitter_counts[3] > 0 && lt.emitter_uploaded && lt.emitter_solved ? LT_LIGHTS_REAL : 0);
	/* (the level's light sources, where its table has them and the level is
	lit live: elsewhere the lightmap holds their light already) */
	if ((params->lights[2] & LT_LIGHTS_REAL) && lt.table && lt.emitter_counts[3] > 0 && lt.emitter_uploaded &&
		!lt.only)
	{
		memcpy(params->emitter_grid, lt.emitter_grid, sizeof(params->emitter_grid));
		memcpy(params->emitter_counts, lt.emitter_counts, sizeof(params->emitter_counts));
		memcpy(params->emitter_bases, lt.emitter_bases, sizeof(params->emitter_bases));
	}
	/* (the objects' own lamps, in a level lit live: emitter_bases' w their
count, lt/e1.frag's object_lamp_sample) */
	params->emitter_bases[3] = (params->lights[2] & LT_LIGHTS_REAL) && !lt.only ? lt_object_lamp_rows(block->grid[0]) : 0;
	lt.live = real && params->lights[1] > 0;
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

/* a half float's value (visibility shares: 0 to 1) */
static float lt_half(uint16_t h)
{
	int exponent = (h >> 10) & 31;
	float mantissa = (float)(h & 1023);

	if (h & 0x8000)
		return 0.0f;
	if (!exponent)
		return mantissa * (1.0f / 16777216.0f);
	if (exponent == 31)
		return 1.0f;
	return ldexpf(1.0f + mantissa / 1024.0f, exponent - 15);
}

/* once a set, with the ray-traced scene built: each cell's share of rays
from points of it to points of each of its groups that the structure lets
through (lt/e1.frag's EMITTER_VISIBILITY, a texel 4 groups), read back (the
draw waited for: once, at the level's start), the cells' chances scaled by
it, so a lamp behind a wall is seldom drawn for a room it does not light */
static void lt_emitter_visibility(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const struct lt_params *frame_params)
{
	int cells = lt.emitter_counts[0] * lt.emitter_counts[1] * lt.emitter_counts[2], cell;
	int32_t rectangle[4];
	struct lt_params params = *frame_params;
	uint16_t *halves = NULL;
	double mean = 0.0;
	long counted = 0;

	/* (the pass's kernel into a half-float target: the dump's pipeline) */
	if (!xgpu_vulkan_pipeline_ready(&lt.copy))
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.copy);
		return;
	}
	lt.emitter_seen = 1;
	if (cells <= 0 || cells > LT_EMITTER_CELLS_MAXIMUM)
		return;
	lt.emitter_visibility = lt_image(lt.emitter_visibility, XGPU_VULKAN_FORMAT_RGBA16F, LT_EMITTER_CELL_GROUPS / 4,
		(uint32_t)cells);
	if (!lt.emitter_visibility)
		return;
	memcpy(params.emitter_grid, lt.emitter_grid, sizeof(params.emitter_grid));
	memcpy(params.emitter_counts, lt.emitter_counts, sizeof(params.emitter_counts));
	memcpy(params.emitter_bases, lt.emitter_bases, sizeof(params.emitter_bases));
	params.control[3] = LT_EMITTER_VISIBILITY;
	rectangle[0] = rectangle[1] = 0;
	rectangle[2] = LT_EMITTER_CELL_GROUPS / 4;
	rectangle[3] = cells;
	lt_pass_draw(lt.emitter_visibility, rectangle, pass, &params, &lt.copy, inputs, lt.means[0], lt.previous,
		lt.guides, NULL);
	if (xgpu_vulkan_read_halves(lt.emitter_visibility, 0, &halves) != 1 || !halves)
		return;
	for (cell = 0; cell < cells; cell++)
	{
		float seen[LT_EMITTER_CELL_GROUPS];
		int slot;

		for (slot = 0; slot < LT_EMITTER_CELL_GROUPS; slot++)
		{
			seen[slot] = lt_half(halves[(size_t)cell * LT_EMITTER_CELL_GROUPS + (size_t)slot]);
			if (lt_emitter_cell_groups[cell][slot] >= 0)
			{
				mean += seen[slot];
				counted++;
			}
		}
		lt_emitter_cell_write(cell, &lt_emitter_texels[lt.emitter_bases[1] + cell * LT_EMITTER_CELL_TEXELS], seen);
	}
	free(halves);
	xgpu_vulkan_image_upload(lt.table, 0, 0, 0, LT_EMITTER_ROW + (uint32_t)(lt.emitter_bases[1] / LT_TABLE_TEXELS), 0,
		LT_TABLE_TEXELS, (uint32_t)(cells * LT_EMITTER_CELL_TEXELS / LT_TABLE_TEXELS), 1,
		lt_emitter_texels[lt.emitter_bases[1]], LT_TABLE_TEXELS * 16,
		(uint32_t)(cells * LT_EMITTER_CELL_TEXELS * 16));
	host_logf(HOST_LOG_INFO, "vulkan: light transport: the light sources' cells' visibility found (%d cells, mean "
		"share %.2f)", cells, counted ? mean / (double)counted : 0.0);
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
	/* (the mode in effect: HALO_LT_E1's, or, not given, the game's light
	lists' (display.ray_traced_lighting's shipping configuration: the
	denoised pass), never the setting's unread -2: tested as lt.mode, that
	ran the shipping pass as "accumulate", its history counted up to 2^20
	frames and no denoiser, so a light that went stayed in it for hundreds of
	frames) */
	int mode = xgpu_vulkan_lt_mode();

	if (!mode)
		return 0;
	/* (what the pass reads is not what it draws into) */
	xgpu_vulkan_rt_bounce_set(NULL);
	if (!inputs || !pass || !cells || !block || !lt_shaders())
		return 1;
	if (!lt.prepared)
	{
		(void)xgpu_vulkan_pipeline_prepare(&lt.pass);
		/* (the denoiser's made with it, so that both are there together) */
		if (mode == XGPU_VULKAN_LT_DENOISE)
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
	/* (the level's light sources: once a set, their cells' visibility) */
	if (lt.table && lt.emitter_counts[3] > 0 && lt.emitter_uploaded && !lt.emitter_seen)
		lt_emitter_visibility(inputs, pass, &params);
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
		params.control[0] = mode == XGPU_VULKAN_LT_RAW || mode == XGPU_VULKAN_LT_DENOISE ? 1 :
			mode == XGPU_VULKAN_LT_E2 ? LT_E2_CAP : LT_ACCUMULATE_CAP;
		params.control[1] = light;
		params.control[2] = XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE;
		params.control[3] = (unshadowed ? 0 : LT_VISIBILITY) | (start ? LT_RESET : 0);
	}
	history = lt.means[lt.current];
	lt.current ^= 1;
	xgpu_vulkan_pass_owner(LT_OWNER);
	/* (W2's reservoirs: RIS and the temporal reuse, from the last frame's,
	which the main draw then reuses spatially; the left half the dynamic
	lights', the right half the level's light sources' (lt/e1.frag's
	LIGHTS_EMITTER_REUSE)) */
	{
		int dynamic = (params.lights[2] & LT_LIGHTS_RESTIR) && lt.handled > 2 && lt.reuse_on;
		int emitters = params.emitter_counts[3] > 0 && lt.reuse_on;

		if (dynamic || emitters)
		{
			struct lt_params reservoir_params = params;
			struct xgpu_vulkan_image *before;

			lt.reservoirs[0] = lt_image(lt.reservoirs[0], XGPU_VULKAN_FORMAT_RGBA32F, 2 * size->width, size->height);
			lt.reservoirs[1] = lt_image(lt.reservoirs[1], XGPU_VULKAN_FORMAT_RGBA32F, 2 * size->width, size->height);
			if (lt.reservoirs[0] && lt.reservoirs[1])
			{
				before = lt.reservoirs[lt.reservoir_current];
				lt.reservoir_current ^= 1;
				reservoirs = lt.reservoirs[lt.reservoir_current];
				if (dynamic)
				{
					reservoir_params.control[3] = LT_RESERVOIR | (start || !lt.reservoirs_valid || !lt.previous_valid ?
						LT_RESET : 0);
					lt_pass_draw(reservoirs, rectangle, pass, &reservoir_params, &lt.pass, inputs, history,
						lt.previous, lt.guides, before);
					params.lights[2] |= LT_LIGHTS_REUSE;
				}
				if (emitters)
				{
					int32_t right[4];

					right[0] = rectangle[0] + (int32_t)size->width;
					right[1] = rectangle[1];
					right[2] = rectangle[2] + (int32_t)size->width;
					right[3] = rectangle[3];
					reservoir_params.control[3] = LT_RESERVOIR | (start || !lt.emitter_reservoirs_valid ||
						!lt.previous_valid ? LT_RESET : 0);
					lt_pass_draw(reservoirs, right, pass, &reservoir_params, &lt.pass, inputs, history, lt.previous,
						lt.guides, before);
					params.lights[2] |= LT_LIGHTS_EMITTER_REUSE;
				}
			}
			else
				dynamic = emitters = 0;
		}
		lt.reservoirs_valid = dynamic;
		lt.emitter_reservoirs_valid = emitters;
	}
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
	indirect_frame.mode = mode;
	indirect_frame.cap = params.control[0];
	indirect_frame.start = start;
	indirect_frame.previous_valid = lt.previous_valid;
	indirect_frame.live = (params.lights[2] & LT_LIGHTS_REAL) != 0;
	/* (ours the denoiser: the bounce counted in its one temporal stage) */
	indirect_frame.fold = mode == XGPU_VULKAN_LT_DENOISE && !xgpu_vulkan_dlss_lt_selected();
	diffuse = xgpu_vulkan_lt_indirect_draw(&indirect_frame);
	if (!diffuse)
		diffuse = lt.means[lt.current];
	/* (with the indirect light, twice as wide: the right half the
	reflections', xgpu_vulkan_lt_indirect_final) */
	lt.final = lt_image(lt.final, XGPU_VULKAN_FORMAT_RGBA32F, size->width * (xgpu_vulkan_lt_indirect_on() ? 2 : 1),
		size->height);
	memset(&shared, 0, sizeof(shared));
	if (mode == XGPU_VULKAN_LT_DENOISE)
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
		lt.reshade_params.lights[2] &= ~(LT_LIGHTS_REUSE | LT_LIGHTS_EMITTER_REUSE);
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
			shared.real = (params.lights[2] & LT_LIGHTS_REAL) != 0;
			for (sun = 0; sun < params.lights[1] && sun < LT_SUNS && !shared.real; sun++)
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
