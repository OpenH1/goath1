/*
RASTERIZER_XBOX_SHADOWS.C

symbols in this file:
00161950 01b0:
	_D3DDevice_SetRenderState (0000)
00161B00 0050:
	_D3DDevice_SetTextureStageState (0000)
00161B50 0220:
	_IDirect3DDevice8_SetRenderState@12 (0000)
00161D70 0010:
	__rasterizer_environment_shadows_begin (0000)
00161D80 0060:
	_IDirect3DDevice8_SetTextureStageState@16 (0000)
00161DE0 0010:
	_IDirect3DDevice8_SetVertexShaderConstant@16 (0000)
00161DF0 00a0:
	__rasterizer_environment_shadow_model_begin (0000)
00161E90 0010:
	__rasterizer_environment_shadow_model_end (0000)
00161EA0 0020:
	_IDirect3DDevice8_SetVertexData2f@16 (0000)
00161EC0 0010:
	_IDirect3DDevice8_SetVertexData2s@16 (0000)
00161ED0 0010:
	_IDirect3DDevice8_Begin@8 (0000)
00161EE0 0010:
	_IDirect3DDevice8_End@4 (0000)
00161EF0 0080:
	__rasterizer_environment_shadow_end (0000)
00161F70 0010:
	__rasterizer_environment_shadows_end (0000)
00161F80 0300:
	_rasterizer_shadow_convolve (0000)
00162280 03b0:
	__rasterizer_environment_shadow_begin (0000)
00162630 02b0:
	__rasterizer_environment_shadow_model_draw (0000)
001628E0 05c0:
	__rasterizer_environment_shadow_draw (0000)
002929E0 0039:
	??_C@_0DJ@KIPLBCDL@c?3?2halo?2SOURCE?2rasterizer?2xbox?2r@ (0000)
00292A1C 0027:
	??_C@_0CH@OGPNPLFF@?$CD?$CD?$CD?5WARNING?5empty?5shadow?5has?5bee@ (0000)
00292A44 001c:
	??_C@_0BM@OJNHCFOK@object_bounding_radius?$DO0?40f?$AA@ (0000)
00292A60 0037:
	??_C@_0DH@LMLPNHOO@shadow_color?9?$DOblue?5?$DO?$DN0?40f?5?$CG?$CG?5sha@ (0000)
00292A98 0037:
	??_C@_0DH@OODBCLKP@shadow_color?9?$DOgreen?$DO?$DN0?40f?5?$CG?$CG?5sha@ (0000)
00292AD0 0035:
	??_C@_0DF@OLKILINN@shadow_color?9?$DOred?5?$DO?$DN0?40f?5?$CG?$CG?5shad@ (0000)
00292B08 000d:
	??_C@_0N@NKBPFDCL@shadow_color?$AA@ (0000)
00292B18 000e:
	??_C@_0O@MDCFBAFA@shadow_matrix?$AA@ (0000)
0030CF84 0001:
	_shadow_restored (0000)
0046628C 004a:
	_local_shadow_color (0000)
	_local_object_bounding_radius (000c)
	_local_shadow_matrix (0010)
	_local_parameters (0044)
	_shadow_setup (0048)
	_shadow_used (0049)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h"
#include "math/real_math.h"
#include "game/game_globals.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "interface/hud_draw.h"
#include "rasterizer/common/rasterizer_common.h"
#include "rasterizer/rasterizer_geometry.h"
#include "render/render.h"
#include "render_enumerate.h" /* port: port/linux/game/render_enumerate.c */
#include "shaders/shader_definitions.h"
#include "shaders/shaders.h"

/* January retains the stock XDK D3DINLINE out-of-line wrappers emitted by
 * the real device calls below. Keep the stock inline definitions and do not
 * replace these wrappers with handwritten bodies. */
#include "rasterizer/rasterizer.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "rasterizer/rasterizer_model_types.h"
#include <xtl.h>
#include "rasterizer/xbox/rasterizer_xbox.h"
#include "rasterizer/xbox/rasterizer_xbox_pixel_shader.h"
#include "rasterizer/xbox/rasterizer_xbox_draw_primitives.h"
#include "draw_bounds.h" /* port: port/linux/game/draw_bounds.c */

/* ---------- constants */

/* port: how far past a shadow's volume its map is still sampled inside its
border, as a multiple of the volume's reach: the border's blend with the
map's edge texels, of a map of 128 or more across, filtered over as many as
16 of them where the view of the surface is steep (anisotropic filtering);
here a quarter */
#define SHADOW_BORDER_REACH 1.25f

enum
{
	_rasterizer_target_render_primary = 0,
};

enum
{
	_rasterizer_statistics_mode_enabled = 2,
};

enum
{
	_shader_type_model = 4,
};

enum
{
	_shader_model_detail_after_reflection_bit = 0,
	_shader_model_two_sided_bit,
	_shader_model_not_alpha_tested_bit,
	_shader_model_alpha_blended_decal_bit,
	_shader_model_true_atmospheric_fog_bit,
	_shader_model_nocull_two_sided_bit,
};

enum
{
	RASTERIZER_STENCIL_MODE_NONE = 0,
	RASTERIZER_STENCIL_MODE_REJECT = 2,
};

enum
{
	_shadow_convolution_vertex_shader_index = 0x26,
	_shadow_vertex_shader_index = 0x1d,
	_shadow_model_vertex_shader_index = 0x27,
};

/* ---------- macros */

/* ---------- structures */

struct shader_model_definition
{
	struct shader shader;
	word flags;
	byte reserved2A[0x72];
	real_vector2d map_scale;
	struct tag_reference base_map;
	byte reservedB4[0x24];
	real detail_map_scale;
	struct tag_reference detail_map;
	real detail_map_v_scale;
	byte reservedF0[0xC];
	struct shader_texture_animation animation;
};

typedef char verify_rasterizer_shadows_model_shadow_count_offset[
	offsetof(
		struct rasterizer_frame_statistics_globals,
		model_shadow_count) == 0xF4 ? 1 : -1];
typedef char verify_rasterizer_shadows_model_parameters_size[
	sizeof(struct rasterizer_model_begin_parameters) == 0xCC ? 1 : -1];
typedef char verify_rasterizer_shadows_model_base_map_scale_offset[
	offsetof(struct rasterizer_model_begin_parameters, base_map_scale) == 0xC4
		? 1 : -1];
typedef char verify_rasterizer_shadows_model_animation_offset[
	offsetof(struct rasterizer_model_begin_parameters, animation) == 0x84
		? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_flags_offset[
	offsetof(struct shader_model_definition, flags) == 0x28 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_map_scale_offset[
	offsetof(struct shader_model_definition, map_scale) == 0x9C ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_base_map_offset[
	offsetof(struct shader_model_definition, base_map) == 0xA4 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_detail_scale_offset[
	offsetof(struct shader_model_definition, detail_map_scale) == 0xD8 ? 1 : -1];
typedef char verify_rasterizer_shadows_shader_model_animation_offset[
	offsetof(struct shader_model_definition, animation) == 0xFC ? 1 : -1];

/* ---------- prototypes */

static void rasterizer_shadow_convolve(
	void);
static void rasterizer_shadow_convolve_pass(
	short source_target,
	short destination_target,
	real offset);
/* port: the shadow atlas (rasterizer_shadow_atlas_begin) */
static short shadow_atlas_capacity(
	short atlas_class);
static long shadow_atlas_x(
	short region);
static long shadow_atlas_y(
	short region);
static void shadow_atlas_caster_target(
	short region);
static short shadow_atlas_take(
	short slot);
static short shadow_slot_find(
	short page,
	short level,
	boolean unowned);
static void shadow_slot_region(
	short slot,
	long *x,
	long *y,
	long *side);
static short shadow_atlas_level_choose(
	long object_index,
	real_point3d const *position,
	real radius);
static short shadow_atlas_level_area(
	short level);
/* port: the caster's target (its region's, NONE: its map's), the shadow
model's texture transform, and the retained casters' hash (below) */
static void shadow_caster_target(
	short region);
static void shadow_model_vertex_constants(
	struct shader_model_definition const *model,
	real_vector4d vertex_constants[3]);
static void shadow_retained_hash(
	void const *data,
	unsigned long size);
static void shadow_retained_frame_begin(
	void);

/* ---------- port: the shadow atlas

Each object's shadow was drawn into the 128x128 map, blurred by the chain
of passes (rasterizer_shadow_convolve) into the other, and drawn onto the
level, before the next object's: six passes an object with the port's
512x512 maps, each with a GPU's fixed cost of a pass. With the atlas, the
frame's casters are drawn first, each into a region of 128 of an atlas
(SHADOW_ATLAS_SPACING apart), and the chain's passes are run once over all
of them, back and forth between two atlases: one quad a region, so that the
atlases' guard bands, black as each pass empties the atlas it draws into
(its quads cover their regions, as they cover a map), stay black, and each
region's taps beyond its edge read black as the map's border does. Each
object's receivers are drawn sampling its region of the atlas the chain
ends in as a map of the region's own (d3d8_texture_region: clamped to the
region's edge texels, its border beyond): the level's surfaces are whole,
their texture coordinates far beyond the map's, which only a map's own
border keeps black (sampled as the atlas, they would read the other
regions). A frame's N casters cost 1 + passes passes, all before the
receivers, instead of (1 + passes) * N between them; the shadows are the
same. A pass stores the whole of its target, and a GPU's time for a pass
grows with its target's area as well as with what it draws, so an atlas
holds about the regions a frame needs: the atlases are a class's pair, of
one row of regions, three or seven (shadow_atlas_heights), the class chosen
for the casters the frame before had.

A region is as large as its shadow needs (shadow_atlas_level_choose): a
cell's whole region of 128 units (the maps' pixels, 512 at Enhanced) for a
shadow near the camera, a quarter's or a sixteenth's for one whose sphere
covers few pixels on the screen (at most as many levels as the maps' scale
has above the Xbox's 128 pixels: the classic maps' are all whole). A cell
holds one whole region, four quarters, or sixteen sixteenths, or a mix of
the quarters' (shadow_slot_find: the whole ones from the first cell, the
smaller from the last), each with its own guard band (shadow_slot_region),
and the chain blurs each region at its own scale, its passes the fewer
(shadow_atlas_region_pass_count): a whole region's pixels and passes are as
they were, a smaller one is the shadow a smaller map would make. The chain's
time and the regions a frame needs grow with the regions' area, which far
casters have little of. HALO_SHADOW_SIZES_SAME makes every region whole
(for comparisons).

Casters past the class's cells go to a second page, and a third: the
atlases of another class, the smallest holding the area past the first page
the frame before had (and a few cells), the chain run over each page in
turn, its receivers sampling it as the first's do (no class's atlases are
another class's page and the frame's own at once: what a page draws is not
kept, the retained casters below are the first page's). The larger maps of
1024 (whose atlases would be 128 MB each) and the debugging options take
the per-object path, as do casters past every page's cells or past
RASTERIZER_SHADOW_ATLAS_REGIONS a frame. */

enum
{
	/* the atlases' width, in the game's units (each display.shadow_resolution's
	scale of pixels: 4096 at 512), the classes' heights in
	shadow_atlas_heights */
	SHADOW_ATLAS_WIDTH = 1024,
	SHADOW_ATLAS_CLASSES = 3,
	SHADOW_ATLAS_MAP_SIZE = 128,
	/* the guard band between two regions, at least 2 * scale - 1 pixels at
	any scale (a pass's taps reach 2 * scale - 1 pixels at most beyond its
	region, rasterizer_shadow_convolve_pass), and before the first, half of
	it (beyond the atlas, the border's black) */
	SHADOW_ATLAS_GUARD = 4,
	SHADOW_ATLAS_SPACING = SHADOW_ATLAS_MAP_SIZE + SHADOW_ATLAS_GUARD,
	SHADOW_ATLAS_MARGIN = SHADOW_ATLAS_GUARD / 2,
	SHADOW_ATLAS_COLUMNS = SHADOW_ATLAS_WIDTH / SHADOW_ATLAS_SPACING,
	/* the cells of the largest class's atlases (shadow_atlas_heights'
	last), the slots of a cell (one whole, 4 quarters, 16 sixteenths), the
	regions' sizes, the pages (each class's once) and the casters a frame
	has regions for */
	SHADOW_ATLAS_REGIONS = (1024 / SHADOW_ATLAS_SPACING) * SHADOW_ATLAS_COLUMNS,
	SHADOW_ATLAS_SLOTS = 1 + 4 + 16,
	SHADOW_ATLAS_LEVELS = 3,
	SHADOW_ATLAS_PAGES = SHADOW_ATLAS_CLASSES,
	SHADOW_ATLAS_CASTERS = RASTERIZER_SHADOW_ATLAS_REGIONS,
	/* (a power of two) */
	SHADOW_ATLAS_LEVEL_MEMORIES = 512,
	/* the casters more than the frame before's that a class is chosen for */
	SHADOW_ATLAS_SLACK = 2,
	SHADOW_ATLAS_MOST_SCALE = 4,
};

/* each class's height (a power of two, as a texture's), the rows of
regions it holds those it has room for */
static short const shadow_atlas_heights[SHADOW_ATLAS_CLASSES] = { 256, 512, 1024 };
/* (a cell's slots are a mask's bits) */
typedef char verify_shadow_atlas_slots[SHADOW_ATLAS_SLOTS <= 32 ? 1 : -1];

/* each class's atlases: the casters' (0), and the chain's other */
static D3DTexture *shadow_atlas_textures[SHADOW_ATLAS_CLASSES][2];
static D3DSurface *shadow_atlas_surfaces[SHADOW_ATLAS_CLASSES][2];
static boolean shadow_atlas_class_made[SHADOW_ATLAS_CLASSES];
/* the frame's class, and the area (sixteenths of a cell) of the casters
that began while the atlas was active, the frame before's when it begins */
static short shadow_atlas_class = 0;
static long shadow_atlas_casters = 0;
static boolean shadow_atlas_failed = FALSE;
/* the casters go to the atlas, and the regions it has (the frame's
shadows' records) */
static boolean shadow_atlas_active = FALSE;
static short shadow_atlas_count = 0;
/* the record of the shadow begun last (NONE: its map's), and of the shadow
whose receivers are drawn (NONE: not the atlas's) */
static short shadow_atlas_region = NONE;
static short shadow_atlas_receiving = NONE;
/* each region's shadow, its page, its slot of the page's atlases, its
level, and whether it is drawn this frame (not kept) */
static struct
{
	real_matrix4x3 matrix;
	real_rgb_color color;
	real radius;
	boolean used;
	boolean drawn;
	short page;
	short slot;
	short level;
} shadow_atlas_shadows[SHADOW_ATLAS_CASTERS];
/* the level of the caster begun last, and whether levels are chosen */
static short shadow_atlas_level = 0;
static boolean shadow_atlas_sizes_on = TRUE;
/* each object's level the frame before (shadow_atlas_level_choose), by its
handle's bits */
struct shadow_atlas_level_memory
{
	long object_index;
	short level;
};
static struct shadow_atlas_level_memory shadow_atlas_level_memories[SHADOW_ATLAS_LEVEL_MEMORIES];
/* the frame's pages (the first, the frame's class's, and those the casters
past it took), each one's class and the casters drawn into it; each page's
cells' slots taken this frame (a bit a slot); the class of the page drawn
into or sampled (shadow_atlas_set_target); and the area past the first
page, the frame before's when the atlas begins */
static struct
{
	short atlas_class;
	short drawn;
} shadow_atlas_pages[SHADOW_ATLAS_PAGES];
static unsigned long shadow_atlas_claimed[SHADOW_ATLAS_PAGES][SHADOW_ATLAS_REGIONS];
static short shadow_atlas_page_count = 1;
static short shadow_atlas_page_class = 0;
static long shadow_atlas_spilled = 0;
static long shadow_atlas_pages_spilled = 0;

/* ---------- port: the retained casters

port: on unless HALO_SHADOWS_REDRAWN is set (each shadow drawn and blurred
every frame, its receivers walked, as before: for comparisons).

A caster drawn into the atlas the frame before, whose draws this frame would
be the same, is not drawn nor blurred again: its region of the chain's last
atlas holds what they would make. Its draws are known before any is made: its
models are walked once with nothing drawn (shadow_fingerprinting), their
draws' every input hashed (the shadow's projection, each model's nodes'
matrices, each part's shader, map, triangles, vertices and texture
transform); the same hash as its region's the frame before keeps the region,
else the caster is drawn as before (its models walked again) into the region
it had, or one no caster had. Each region's cell is emptied alone, as on a
tile-based GPU (shadow_atlas_set_target), never the whole atlas, so a kept
region stays as it was; the chain draws the regions drawn this frame alone.
The receivers are drawn as before, each sampling its own region. Kept only
while the atlas's class, the maps' scale, the settings, the map, the
atlases' targets (d3d8_target_identity: made anew, their pixels are gone)
and the view drawing the shadows (a window's: split screen and mirrors
alternate) stay the same.

Nothing is kept from a frame of the map's preparation (render_enumerating,
port/linux/game/render_enumerate.c): its passes walk the casters as a frame
would, but what they draw never reaches the atlas the frames after sample,
so a caster drawn there and kept after had a black region, its shadow gone
(the frames after the preparation's, measured with the atlas read back). */

static boolean shadow_retained_on = FALSE;
static boolean shadow_fingerprinting = FALSE;
static unsigned long long shadow_fingerprint = 0;
static long shadow_caster_object = NONE;
/* each first page's slot's caster as it was last drawn (NONE: none), and
each cell's slots that have one (a bit a slot) */
static struct
{
	long object_index;
	unsigned long long fingerprint;
	unsigned long long unposed_fingerprint;
	boolean used;
} shadow_atlas_owners[SHADOW_ATLAS_REGIONS * SHADOW_ATLAS_SLOTS];
static unsigned long shadow_atlas_owned[SHADOW_ATLAS_REGIONS];
/* (diagnostic: the draws' hash but for the nodes' matrices) */
static unsigned long long shadow_unposed_fingerprint = 0;
static boolean shadow_hashing_pose = FALSE;
static unsigned long shadow_retained_posed_count = 0;
static short shadow_retained_class = NONE;
/* the atlases' targets' identities (d3d8_target_identity) */
static unsigned long shadow_retained_targets[2] = { 0, 0 };
static long shadow_retained_scale = 0;
static unsigned long shadow_retained_changes = 0;
static long shadow_retained_view = NONE;
/* the casters and those kept, logged now and then */
static unsigned long shadow_retained_casters = 0;
static unsigned long shadow_retained_kept_count = 0;
/* (diagnostic: the casters past the first page's regions, those of them
past every page's, drawn into their own maps, and the frames of each class) */
static unsigned long shadow_retained_spill_count = 0;
static unsigned long shadow_retained_overflow_count = 0;
static unsigned long shadow_retained_class_frames[SHADOW_ATLAS_CLASSES] = { 0, 0, 0 };
static unsigned long shadow_retained_level_count[SHADOW_ATLAS_LEVELS] = { 0, 0, 0 };
static unsigned long shadow_retained_frames = 0;
/* the shadow's receivers drawn yet, and their vertices' type
(_rasterizer_environment_shadow_draw: d3d8_draw_state_kept) */
static boolean shadow_receiver_drawn = FALSE;
static long shadow_receiver_type = NONE;
/* the receivers kept (HALO_SHADOWS_REDRAWN unset), read as the frame's
casters begin */
static boolean shadow_receivers_kept = FALSE;

int config_boolean(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
char *getenv(char const *name);
float halo_screen_pixel_scale(void);
real render_frustum_sphere_diameter_in_pixels(
	struct render_frustum const *frustum,
	real_point3d const *point,
	real radius);

/* ---------- globals */

extern D3DDevice *global_d3d_device;
extern struct pixel_shader_definition pixel_shader;

static boolean shadow_restored = TRUE;

static real_rgb_color local_shadow_color = { 0 };
static real local_object_bounding_radius = 0.0f;
static real_matrix4x3 local_shadow_matrix = { 0 };
static struct rasterizer_model_begin_parameters const *local_parameters = NULL;
static boolean shadow_setup = FALSE;
static boolean shadow_used = FALSE;

/* ---------- public code */

void _rasterizer_environment_shadows_begin(
	void)
{
	rasterizer_profile_begin(_rasterizer_profile_environment_shadows);

	return;
}

boolean _rasterizer_environment_shadow_begin(
	long object_index,
	real_matrix4x3 const *shadow_matrix,
	real_rgb_color const *shadow_color,
	real object_bounding_radius,
	real *shadow_volume_bounding_radius)
{
	boolean success = TRUE;
	real inverse_radius;
	real vsh_constants__screenproj[20];

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		147,
		global_d3d_device);

	shadow_fingerprinting = FALSE; /* port: (the retained casters) */
	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			153,
			shadow_matrix);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			154,
			shadow_color);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			155,
			shadow_color->red >=0.0f && shadow_color->red <=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			156,
			shadow_color->green>=0.0f && shadow_color->green<=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			157,
			shadow_color->blue >=0.0f && shadow_color->blue <=1.0f);
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			158,
			object_bounding_radius>0.0f);

		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_CCW);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED |
			D3DCOLORWRITEENABLE_GREEN |
			D3DCOLORWRITEENABLE_BLUE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, FALSE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHATESTENABLE, TRUE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHAREF, 0x7F);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, FALSE);
		IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZBIAS, 0);

		csmemset(&pixel_shader, 0, sizeof(pixel_shader));
		pixel_shader.texture_modes = 1;
		pixel_shader.combiner_count = 1;
		pixel_shader.final_combiner_inputs_abcd = 0x00000020;
		pixel_shader.final_combiner_inputs_efg = 0x00001800;
		rasterizer_set_pixel_shader(&pixel_shader);

		inverse_radius = 1.0f / object_bounding_radius;

		vsh_constants__screenproj[0] = inverse_radius * shadow_matrix->forward.i;
		vsh_constants__screenproj[1] = inverse_radius * shadow_matrix->forward.j;
		vsh_constants__screenproj[2] = inverse_radius * shadow_matrix->forward.k;
		vsh_constants__screenproj[3] = -inverse_radius * dot_product3d(
			&shadow_matrix->forward,
			(real_vector3d const *)&shadow_matrix->position);
		vsh_constants__screenproj[4] = inverse_radius * shadow_matrix->left.i;
		vsh_constants__screenproj[5] = inverse_radius * shadow_matrix->left.j;
		vsh_constants__screenproj[6] = inverse_radius * shadow_matrix->left.k;
		vsh_constants__screenproj[7] = -inverse_radius * dot_product3d(
			&shadow_matrix->left,
			(real_vector3d const *)&shadow_matrix->position);
		vsh_constants__screenproj[8] = 0.0f;
		vsh_constants__screenproj[9] = 0.0f;
		vsh_constants__screenproj[10] = 0.0f;
		vsh_constants__screenproj[11] = 0.5f;
		vsh_constants__screenproj[12] = 0.0f;
		vsh_constants__screenproj[13] = 0.0f;
		vsh_constants__screenproj[14] = 0.0f;
		vsh_constants__screenproj[15] = 1.0f;
		vsh_constants__screenproj[16] = 0.0f;
		vsh_constants__screenproj[17] = 0.0f;
		vsh_constants__screenproj[18] = 0.0f;
		vsh_constants__screenproj[19] = 0.0f;
		IDirect3DDevice8_SetVertexShaderConstant(
			global_d3d_device,
			-68,
			vsh_constants__screenproj,
			5);

		/* port: into a region of the shadow atlas while it has one free
		(rasterizer_shadow_atlas_begin), of the size its shadow needs on the
		screen, else into the map as before */
		shadow_atlas_level = 0;
		if (shadow_atlas_active)
		{
			shadow_atlas_level = shadow_atlas_level_choose(
				object_index,
				(real_point3d const *)&shadow_matrix->position,
				object_bounding_radius);
			shadow_atlas_casters += shadow_atlas_level_area(shadow_atlas_level);
		}
		/* port: (the retained casters, above) its region chosen once its
		draws are known (rasterizer_shadow_retained_caster) */
		shadow_fingerprinting = shadow_retained_on && shadow_atlas_active;
		shadow_atlas_region = NONE;
		if (shadow_fingerprinting)
		{
			shadow_caster_object = object_index;
			shadow_fingerprint = 1469598103934665603ULL;
			shadow_unposed_fingerprint = 1469598103934665603ULL;
			shadow_retained_hash(vsh_constants__screenproj, sizeof(vsh_constants__screenproj));
			shadow_retained_hash(&object_bounding_radius, sizeof(object_bounding_radius));
			shadow_retained_hash(&shadow_atlas_level, sizeof(shadow_atlas_level));
		}
		else
		{
			if (shadow_atlas_active)
			{
				shadow_atlas_region = shadow_atlas_take(shadow_slot_find(0, shadow_atlas_level, FALSE));
			}
			shadow_caster_target(shadow_atlas_region);
		}
		rasterizer_set_stencil_mode(0);

		local_shadow_matrix = *shadow_matrix;
		local_shadow_color = *shadow_color;
		local_object_bounding_radius = object_bounding_radius;
		if (shadow_volume_bounding_radius)
		{
			*shadow_volume_bounding_radius = object_bounding_radius;
		}
		/* port: what its receivers are drawn with, after every caster's
		(rasterizer_shadow_atlas_receive) */
		if (shadow_atlas_region != NONE)
		{
			shadow_atlas_shadows[shadow_atlas_region].matrix = *shadow_matrix;
			shadow_atlas_shadows[shadow_atlas_region].color = *shadow_color;
			shadow_atlas_shadows[shadow_atlas_region].radius = object_bounding_radius;
		}

		local_parameters = 0;
		shadow_setup = FALSE;
		shadow_used = FALSE;
		shadow_restored = FALSE;

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.shadow_count++;
		}
	}

	return success;
}

void _rasterizer_environment_shadow_model_begin(
	struct rasterizer_model_begin_parameters const *parameters)
{
	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		239,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			245,
			parameters);

		local_parameters = parameters;
		shadow_used = TRUE;
		/* port: (the retained casters) its nodes' matrices hashed, not set */
		if (shadow_fingerprinting)
		{
			short count = parameters->skinning.node_matrix_count;

			shadow_retained_hash(&count, sizeof(count));
			if (count > 0 && parameters->skinning.node_matrices)
			{
				shadow_hashing_pose = TRUE;
				shadow_retained_hash(parameters->skinning.node_matrices, count * sizeof(real_matrix4x3));
				shadow_hashing_pose = FALSE;
			}
			return;
		}

		rasterizer_set_model_skinning(&parameters->skinning);

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.model_shadow_count++;
		}
	}

	return;
}

void _rasterizer_environment_shadow_model_end(
	void)
{
	local_parameters = 0;

	return;
}

void _rasterizer_environment_shadow_model_draw(
	struct shader const *shader,
	short bitmap_index,
	struct triangle_buffer const *triangle_buffer,
	struct vertex_buffer const *vertex_buffer)
{
	struct shader_model_definition const *model;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		274,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		match_assert(
			"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
			280,
			shader);

		if (shader->base.type == _shader_type_model)
		{
			model = (struct shader_model_definition *)shader_get_and_verify_type(
				(struct shader *)shader,
				_shader_type_model);

			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				286,
				vertex_buffer);
			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				287,
				triangle_buffer);
			match_assert(
				"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
				288,
				local_parameters);

			/* port: (the retained casters) the draw's inputs hashed, nothing
			drawn: its shader and map, triangles and vertices, and texture
			transform (shadow_model_vertex_constants, as below) */
			if (shadow_fingerprinting)
			{
				real_vector4d vertex_constants[3];
				struct
				{
					void const *shader;
					void const *triangle_buffer;
					void const *vertex_buffer;
					long triangle_count;
					long triangle_type;
					long vertex_type;
					short bitmap_index;
					word flags;
				} draw;

				csmemset(&draw, 0, sizeof(draw));
				draw.shader = shader;
				draw.triangle_buffer = triangle_buffer;
				draw.vertex_buffer = vertex_buffer;
				draw.triangle_count = triangle_buffer->count;
				draw.triangle_type = triangle_buffer->type;
				draw.vertex_type = vertex_buffer->type;
				draw.bitmap_index = bitmap_index;
				draw.flags = model->flags;
				shadow_retained_hash(&draw, sizeof(draw));
				shadow_model_vertex_constants(model, vertex_constants);
				shadow_retained_hash(vertex_constants, sizeof(vertex_constants));
				return;
			}

			if (TEST_FLAG(model->flags, _shader_model_two_sided_bit))
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_NONE);
			}
			else
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_CCW);
			}
			rasterizer_set_vertex_shader_permutation(
				_shadow_model_vertex_shader_index,
				vertex_buffer->type,
				0);

			if (TEST_FLAG(model->flags, _shader_model_not_alpha_tested_bit))
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_PSTEXTUREMODES, 0);
			}
			else
			{
				IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_PSTEXTUREMODES, 1);
				rasterizer_set_texture(
					0,
					0,
					1,
					model->base_map.index,
					bitmap_index);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_ADDRESSU,
					D3DTADDRESS_WRAP);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_ADDRESSV,
					D3DTADDRESS_WRAP);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MAGFILTER,
					D3DTEXF_LINEAR);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MINFILTER,
					D3DTEXF_LINEAR);
				IDirect3DDevice8_SetTextureStageState(
					global_d3d_device,
					0,
					D3DTSS_MIPFILTER,
					D3DTEXF_LINEAR);
			}

			{
				real_vector4d vertex_constants[3];

				shadow_model_vertex_constants(model, vertex_constants);
				IDirect3DDevice8_SetVertexShaderConstant(
					global_d3d_device,
					-84,
					vertex_constants,
					NUMBEROF(vertex_constants));
			}

			rasterizer_draw_static_triangles_static_vertices(
				triangle_buffer,
				0,
				triangle_buffer->count,
				vertex_buffer);

			if (rasterizer_debug_options.statistics_mode ==
				_rasterizer_statistics_mode_enabled)
			{
				rasterizer_frame_statistics.model_shadow_draw_count++;
				rasterizer_frame_statistics.model_shadow_triangle_count +=
					triangle_buffer->count;
				rasterizer_frame_statistics.model_shadow_vertex_count +=
					rasterizer_frame_statistics_count_static_vertices(
						triangle_buffer,
						vertex_buffer);
			}
		}
	}

	return;
}

void _rasterizer_environment_shadow_end(
	void)
{
	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		563,
		global_d3d_device);

	draw_bounds_none(); /* port: (the shadow's receivers are drawn) */

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		if (!shadow_used)
		{
			error(
				_error_silent,
				"### WARNING empty shadow has been cast");
		}

		if (!shadow_restored)
		{
			rasterizer_set_target(
				global_window_parameters.rasterizer_target,
				FALSE,
				FALSE,
				FALSE,
				TRUE);
			shadow_restored = TRUE;
		}
	}

	return;
}

void _rasterizer_environment_shadow_draw(
	struct shader const *shader,
	short bitmap_index,
	long dynamic_triangle_buffer_index,
	long first_triangle_index,
	long triangle_count,
	struct vertex_buffer const *vertex_buffer)
{
	real vertex_constants[5][4];
	real inverse_radius;
	real inverse_radius_quarter;
	real inverse_radius_double;
	real position_dot_up;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		404,
		global_d3d_device);

	if (global_window_parameters.rasterizer_target ==
			_rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows)
	{
		if (!shadow_setup)
		{
			shadow_receiver_drawn = FALSE; /* port: (below) */
			/* port: a shadow of the atlas is blurred already, into its region
			of the chain's last atlas, sampled as its own map
			(rasterizer_shadow_atlas_convolve) */
			if (shadow_atlas_receiving != NONE)
			{
				real const width = (real)SHADOW_ATLAS_WIDTH;
				real const height = (real)shadow_atlas_heights[shadow_atlas_page_class];
				float region[4];
				long x, y, side;

				shadow_slot_region(shadow_atlas_shadows[shadow_atlas_receiving].slot, &x, &y, &side);
				region[0] = (real)side / width;
				region[1] = (real)side / height;
				region[2] = (real)x / width;
				region[3] = (real)y / height;
				IDirect3DDevice8_SetTexture(
					global_d3d_device,
					0,
					(IDirect3DBaseTexture8 *)shadow_atlas_textures[shadow_atlas_page_class][1]);
				d3d8_texture_region(0, region);
			}
			else
			{
				d3d8_texture_region(0, NULL);
				if (rasterizer_debug_options.shadow_convolution_enabled)
				{
					rasterizer_shadow_convolve();
				}

				rasterizer_set_target_as_texture(
					0,
					(rasterizer_debug_options.shadow_convolution_enabled != FALSE) + 2,
					FALSE);
			}
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				0,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			rasterizer_set_texture_direct(
				1,
				global_rasterizer_data->linear_corner_fade.index,
				0);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				1,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			rasterizer_set_texture_direct(
				2,
				global_rasterizer_data->vector_normalization.index,
				0);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_ADDRESSW,
				D3DTADDRESS_CLAMP);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				2,
				D3DTSS_MIPFILTER,
				D3DTEXF_LINEAR);

			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_CULLMODE,
				D3DCULL_CCW);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_COLORWRITEENABLE,
				D3DCOLORWRITEENABLE_ALL);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHABLENDENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_SRCBLEND,
				D3DBLEND_ZERO);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_DESTBLEND,
				D3DBLEND_INVSRCCOLOR);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_BLENDOP,
				D3DBLENDOP_ADD);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHATESTENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ALPHAREF,
				0);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZENABLE,
				TRUE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZFUNC,
				D3DCMP_EQUAL);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZWRITEENABLE,
				FALSE);
			IDirect3DDevice8_SetRenderState(
				global_d3d_device,
				D3DRS_ZBIAS,
				0);

			csmemset(&pixel_shader, 0, sizeof(pixel_shader));
			pixel_shader.texture_modes = 0x00000021;
			pixel_shader.combiner_count = 4;
			pixel_shader.constant_0[0] = real_rgb_color_to_pixel32(&local_shadow_color);
			pixel_shader.constant_1[0] = 0x00FFFFFF;
			pixel_shader.rgb_inputs[0] = 0x14200000;
			pixel_shader.rgb_outputs[0] = 0x000000C0;
			pixel_shader.rgb_inputs[1] = 0x290C0821;
			pixel_shader.rgb_outputs[1] = 0x000000CD;
			pixel_shader.rgb_inputs[2] = 0x2C200C2D;
			pixel_shader.rgb_outputs[2] = 0x00000C00;
			pixel_shader.rgb_inputs[3] = 0x2C020000;
			pixel_shader.rgb_outputs[3] = 0x000020D0;
			pixel_shader.final_combiner_inputs_abcd = 0x0000002C;
			pixel_shader.final_combiner_inputs_efg = 0x00000D00;
			if (rasterizer_debug_options.shadow_debug_enabled)
			{
				pixel_shader.final_combiner_inputs_abcd = 0x0000000C;
				IDirect3DDevice8_SetRenderState(
					global_d3d_device,
					D3DRS_ALPHATESTENABLE,
					FALSE);
			}
			rasterizer_set_pixel_shader(&pixel_shader);

			rasterizer_set_vertex_shader_permutation(
				_shadow_vertex_shader_index,
				vertex_buffer->type,
				shader_get_vertex_shader_permutation(shader));

			inverse_radius = 1.0f / local_object_bounding_radius;
			inverse_radius_quarter = 1.0f / (local_object_bounding_radius * 4.0f);
			inverse_radius_double = 1.0f / (local_object_bounding_radius * 0.5f);

			vertex_constants[0][0] = local_shadow_matrix.forward.i * inverse_radius * 0.5f;
			vertex_constants[0][1] = local_shadow_matrix.forward.j * inverse_radius * 0.5f;
			vertex_constants[0][2] = local_shadow_matrix.forward.k * inverse_radius * 0.5f;
			vertex_constants[0][3] =
				(1.0f -
				dot_product3d(
					(real_vector3d const *)&local_shadow_matrix.position,
					&local_shadow_matrix.forward) *
				inverse_radius) * 0.5f;
			vertex_constants[1][0] = local_shadow_matrix.left.i * inverse_radius * -0.5f;
			vertex_constants[1][1] = local_shadow_matrix.left.j * inverse_radius * -0.5f;
			vertex_constants[1][2] = local_shadow_matrix.left.k * inverse_radius * -0.5f;
			vertex_constants[1][3] =
				(dot_product3d(
					(real_vector3d const *)&local_shadow_matrix.position,
					&local_shadow_matrix.left) *
				inverse_radius + 1.0f) * 0.5f;
			vertex_constants[2][0] = local_shadow_matrix.up.i * inverse_radius_quarter;
			vertex_constants[2][1] = local_shadow_matrix.up.j * inverse_radius_quarter;
			vertex_constants[2][2] = local_shadow_matrix.up.k * inverse_radius_quarter;
			position_dot_up = dot_product3d(
				(real_vector3d const *)&local_shadow_matrix.position,
				&local_shadow_matrix.up);
			vertex_constants[2][3] =
				-(position_dot_up * inverse_radius_quarter);
			vertex_constants[3][0] = -(local_shadow_matrix.up.i * inverse_radius_double);
			vertex_constants[3][1] = -(local_shadow_matrix.up.j * inverse_radius_double);
			vertex_constants[3][2] = -(local_shadow_matrix.up.k * inverse_radius_double);
			vertex_constants[3][3] = position_dot_up * inverse_radius_double;
			vertex_constants[4][0] = local_shadow_matrix.up.i;
			vertex_constants[4][1] = local_shadow_matrix.up.j;
			vertex_constants[4][2] = local_shadow_matrix.up.k;
			vertex_constants[4][3] = 0.0f;
			IDirect3DDevice8_SetVertexShaderConstant(
				global_d3d_device,
				-81,
				vertex_constants,
				5);

			if (!shadow_restored)
			{
				rasterizer_set_target(
					global_window_parameters.rasterizer_target,
					0,
					0,
					FALSE,
					TRUE);
				shadow_restored = TRUE;
			}

			/* port: the shadow's receivers are the level's surfaces, whole, and
			shadowed only inside the shadow's volume (render_object_shadow_end's
			planes: the radius to each side, half of it toward the light and
			four away), beside which the map is sampled by its border, and a
			little beyond, where the border blends with the map's edge
			(SHADOW_BORDER_REACH). The draws are kept to its place in the view
			(port/linux/game/draw_bounds.c): the same pixels are shadowed */
			{
				real const radius = local_object_bounding_radius;
				real low[3];
				real high[3];

				low[0] = low[1] = -radius * SHADOW_BORDER_REACH;
				high[0] = high[1] = radius * SHADOW_BORDER_REACH;
				low[2] = -radius * 0.5f * SHADOW_BORDER_REACH;
				high[2] = radius * 4.0f * SHADOW_BORDER_REACH;
				draw_bounds_box(
					&local_shadow_matrix.position,
					&local_shadow_matrix.forward,
					&local_shadow_matrix.left,
					&local_shadow_matrix.up,
					low,
					high);
			}

			shadow_setup = TRUE;
		}

		rasterizer_set_stencil_mode(RASTERIZER_STENCIL_MODE_REJECT);
		/* port: (the retained casters, above) the shadow's draws after its first
		(its setup's), of other materials' vertices of the same type, keep
		its state as it was prepared (d3d8_draw_state_kept) */
		d3d8_draw_state_kept(shadow_receivers_kept && shadow_receiver_drawn && vertex_buffer->type == shadow_receiver_type);
		rasterizer_draw_dynamic_triangles_static_vertices(
			dynamic_triangle_buffer_index,
			first_triangle_index,
			triangle_count,
			vertex_buffer);
		d3d8_draw_state_kept(FALSE);
		shadow_receiver_drawn = TRUE;
		shadow_receiver_type = vertex_buffer->type;

		if (rasterizer_debug_options.statistics_mode ==
			_rasterizer_statistics_mode_enabled)
		{
			rasterizer_frame_statistics.shadow_draw_count++;
			rasterizer_frame_statistics.shadow_triangle_count += triangle_count;
			rasterizer_frame_statistics.shadow_vertex_count +=
				rasterizer_frame_statistics_count_dynamic_vertices(
					dynamic_triangle_buffer_index,
					first_triangle_index,
					triangle_count);
		}
	}

	return;
}

void _rasterizer_environment_shadows_end(
	void)
{
	rasterizer_profile_end(_rasterizer_profile_environment_shadows);

	return;
}

/* ---------- private code */

static void rasterizer_shadow_convolve(
	void)
{
	short stage;
	real vertex_constants[8][4];
	/* port: the shadow maps' texels for each of the Xbox's 128 */
	long scale = halo_shadow_map_scale();
	long distance;

	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		31,
		global_d3d_device);

	if (rasterizer_debug_options.draw_environment_shadows &&
		rasterizer_debug_options.shadow_convolution_enabled)
	{
		for (stage = 0; stage < 4; stage++)
		{
			rasterizer_set_target_as_texture(stage, 2, FALSE);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_ADDRESSU,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_ADDRESSV,
				D3DTADDRESS_BORDER);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MAGFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MINFILTER,
				D3DTEXF_LINEAR);
			IDirect3DDevice8_SetTextureStageState(
				global_d3d_device,
				stage,
				D3DTSS_MIPFILTER,
				D3DTEXF_POINT);
		}

		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_CULLMODE,
			D3DCULL_CCW);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_COLORWRITEENABLE,
			D3DCOLORWRITEENABLE_RED |
			D3DCOLORWRITEENABLE_GREEN |
			D3DCOLORWRITEENABLE_BLUE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ALPHABLENDENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ALPHATESTENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ZENABLE,
			FALSE);
		IDirect3DDevice8_SetRenderState(
			global_d3d_device,
			D3DRS_ZBIAS,
			0);

		rasterizer_set_vertex_shader_permutation(
			_shadow_convolution_vertex_shader_index,
			8,
			0);

		vertex_constants[0][0] = 1.0f;
		vertex_constants[0][1] = 0.0f;
		vertex_constants[0][2] = 0.0f;
		vertex_constants[0][3] = -0.00390625f;
		vertex_constants[1][0] = 0.0f;
		vertex_constants[1][1] = 1.0f;
		vertex_constants[1][2] = 0.0f;
		vertex_constants[1][3] = -0.00390625f;
		vertex_constants[2][0] = 1.0f;
		vertex_constants[2][1] = 0.0f;
		vertex_constants[2][2] = 0.0f;
		vertex_constants[2][3] = 0.00390625f;
		vertex_constants[3][0] = 0.0f;
		vertex_constants[3][1] = 1.0f;
		vertex_constants[3][2] = 0.0f;
		vertex_constants[3][3] = 0.00390625f;
		vertex_constants[4][0] = 1.0f;
		vertex_constants[4][1] = 0.0f;
		vertex_constants[4][2] = 0.0f;
		vertex_constants[4][3] = -0.00390625f;
		vertex_constants[5][0] = 0.0f;
		vertex_constants[5][1] = 1.0f;
		vertex_constants[5][2] = 0.0f;
		vertex_constants[5][3] = 0.00390625f;
		vertex_constants[6][0] = 1.0f;
		vertex_constants[6][1] = 0.0f;
		vertex_constants[6][2] = 0.0f;
		vertex_constants[6][3] = 0.00390625f;
		vertex_constants[7][0] = 0.0f;
		vertex_constants[7][1] = 1.0f;
		vertex_constants[7][2] = 0.0f;
		vertex_constants[7][3] = -0.00390625f;
		/* port: half a texel of a larger map (rasterizer_shadow_convolve_pass) */
		if (scale > 1)
		{
			for (distance = 0; distance < 8; distance++)
			{
				vertex_constants[distance][3] /= (real)scale;
			}
		}
		IDirect3DDevice8_SetVertexShaderConstant(
			global_d3d_device,
			-81,
			vertex_constants,
			8);

		csmemset(&pixel_shader, 0, sizeof(pixel_shader));
		pixel_shader.texture_modes = 0x00008421;
		pixel_shader.combiner_count = 1;
		pixel_shader.alpha_inputs[0] = 0x08A009A0;
		pixel_shader.alpha_outputs[0] = 0x00030C00;
		pixel_shader.rgb_inputs[0] = 0x0AA00BA0;
		pixel_shader.rgb_outputs[0] = 0x00030C00;
		pixel_shader.final_combiner_inputs_abcd = 0x0C20001C;
		rasterizer_set_pixel_shader(&pixel_shader);

		rasterizer_set_target(3, 0, 0, FALSE, FALSE);

		IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 0);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			-1.0078125f,
			1.0078125f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 0);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			0.9921875f,
			1.0078125f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 1);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			0.9921875f,
			-0.9921875f);
		IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 1);
		IDirect3DDevice8_SetVertexData2f(
			global_d3d_device,
			0,
			-1.0078125f,
			-0.9921875f);
		IDirect3DDevice8_End(global_d3d_device);

		/* port: the passes that widen the blur of a larger map, back and
		forth between the maps, ending in the secondary */
		for (distance = 1; distance < scale; distance *= 2)
		{
			rasterizer_shadow_convolve_pass(3, 2, (real)distance / (real)(128 * scale));
			rasterizer_shadow_convolve_pass(2, 3, (real)distance / (real)(128 * scale));
		}
	}

	return;
}

/* port: the shadow maps can be drawn larger than the Xbox's 128x128
(display.shadow_resolution, port/linux/src/d3d8_device.c): scale times as many
texels each way, a power of two. The Xbox's blur takes four taps diagonally
half a texel off each texel, on the corners it shares with its neighbours.
Each tap averages the four texels there, and together they weigh the texel
and its neighbours 1, 2, 1 each way: a tent over three texels, which is a box
two texels wide taken twice. For the shadows to stay as soft, a larger map's
blur must cover as much of the map: a tent scale times as wide, a box of
2 * scale texels taken twice, which weighs the texels 2 * scale - |d| for |d|
up to 2 * scale - 1 texels away. The Xbox's pass, with its taps half a texel
of the larger map off, is the box of two texels taken twice. A further pass
with its taps diagonally distance texels off, on texels' centres, averages
texels 2 * distance apart each way, which doubles a box of 2 * distance
texels; two passes each at distance 1, 2, ... scale / 2 make both boxes
2 * scale texels wide. The quad's offsets of half a texel stay as they are:
they are half of the game's units, which the port undoes in the same units
at any size (port/linux/src/nv2a_vsh.c), so the quad covers the map. */
static void rasterizer_shadow_convolve_pass(
	short source_target,
	short destination_target,
	real offset)
{
	short stage;
	real vertex_constants[8][4];

	csmemset(vertex_constants, 0, sizeof(vertex_constants));
	for (stage = 0; stage < 4; stage++)
	{
		rasterizer_set_target_as_texture(stage, source_target, FALSE);
		vertex_constants[stage * 2][0] = 1.0f;
		vertex_constants[stage * 2 + 1][1] = 1.0f;
	}
	/* the Xbox pass's corners, in its order */
	vertex_constants[0][3] = -offset;
	vertex_constants[1][3] = -offset;
	vertex_constants[2][3] = offset;
	vertex_constants[3][3] = offset;
	vertex_constants[4][3] = -offset;
	vertex_constants[5][3] = offset;
	vertex_constants[6][3] = offset;
	vertex_constants[7][3] = -offset;
	IDirect3DDevice8_SetVertexShaderConstant(
		global_d3d_device,
		-81,
		vertex_constants,
		8);

	rasterizer_set_target(destination_target, 0, 0, FALSE, FALSE);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, -0.9921875f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, -0.9921875f);
	IDirect3DDevice8_End(global_d3d_device);

	return;
}

/* ---------- port: the shadow atlas (above, "the shadow atlas") */

/* a cell's region of the whole size (level 0), its left and top */
static long shadow_atlas_x(
	short region)
{
	return SHADOW_ATLAS_MARGIN + (region % SHADOW_ATLAS_COLUMNS) * SHADOW_ATLAS_SPACING;
}

static long shadow_atlas_y(
	short region)
{
	return SHADOW_ATLAS_MARGIN + (region / SHADOW_ATLAS_COLUMNS) * SHADOW_ATLAS_SPACING;
}

/* the cells a class's atlases hold */
static short shadow_atlas_capacity(
	short atlas_class)
{
	return (short)((shadow_atlas_heights[atlas_class] / SHADOW_ATLAS_SPACING) * SHADOW_ATLAS_COLUMNS);
}

/* a level's area, in sixteenths of a cell */
static short shadow_atlas_level_area(
	short level)
{
	return (short)(16 >> (2 * level));
}

/* a slot of a cell's level: the whole cell (0), its quarters (1 to 4), the
quarters' quarters (5 to 20, four a quarter) */
static short shadow_slot_level(
	short slot)
{
	short sub = slot % SHADOW_ATLAS_SLOTS;

	return sub == 0 ? 0 : sub < 5 ? 1 : 2;
}

/* a slot's square of its atlas (its share of its cell: the squares of a
level tile the cell, so that emptying one never touches another's), its
left, top and side */
static void shadow_slot_square(
	short slot,
	long *x,
	long *y,
	long *side)
{
	short cell = slot / SHADOW_ATLAS_SLOTS;
	short sub = slot % SHADOW_ATLAS_SLOTS;

	*x = shadow_atlas_x(cell) - SHADOW_ATLAS_MARGIN;
	*y = shadow_atlas_y(cell) - SHADOW_ATLAS_MARGIN;
	*side = SHADOW_ATLAS_SPACING;
	if (sub > 0)
	{
		short quarter = sub < 5 ? sub - 1 : (sub - 5) / 4;

		*side /= 2;
		*x += (quarter & 1) * *side;
		*y += (quarter >> 1) * *side;
		if (sub >= 5)
		{
			short sixteenth = (sub - 5) % 4;

			*side /= 2;
			*x += (sixteenth & 1) * *side;
			*y += (sixteenth >> 1) * *side;
		}
	}

	return;
}

/* a slot's region: its square less its guard band, SHADOW_ATLAS_MARGIN each
side of a whole cell's (128), a unit of a quarter's (64) and a sixteenth's
(31: a quarter of the cell's 132 is 33), more than a pass's taps reach at
the region's own scale (2 * scale - 1 pixels: 3 of a quarter's at 4, 1 of
a sixteenth's, against 4) */
static void shadow_slot_region(
	short slot,
	long *x,
	long *y,
	long *side)
{
	long margin = slot % SHADOW_ATLAS_SLOTS ? 1 : SHADOW_ATLAS_MARGIN;

	shadow_slot_square(slot, x, y, side);
	*x += margin;
	*y += margin;
	*side -= 2 * margin;

	return;
}

/* the slots of a cell a slot overlaps (itself, the slots holding it and
those it holds), a bit each */
static unsigned long shadow_slot_overlaps(
	short slot)
{
	static unsigned long overlaps[SHADOW_ATLAS_SLOTS];
	static boolean made = FALSE;

	if (!made)
	{
		short a, b;

		for (a = 0; a < SHADOW_ATLAS_SLOTS; a++)
		{
			overlaps[a] = 0;
			for (b = 0; b < SHADOW_ATLAS_SLOTS; b++)
			{
				long ax, ay, aside, bx, by, bside;

				shadow_slot_square(a, &ax, &ay, &aside);
				shadow_slot_square(b, &bx, &by, &bside);
				if (ax < bx + bside && bx < ax + aside && ay < by + bside && by < ay + aside)
				{
					overlaps[a] |= 1UL << b;
				}
			}
		}
		made = TRUE;
	}

	return overlaps[slot % SHADOW_ATLAS_SLOTS];
}

/* a slot no caster of the page took this frame, nor one overlapping it */
static boolean shadow_slot_free(
	short page,
	short slot)
{
	return !(shadow_atlas_claimed[page][slot / SHADOW_ATLAS_SLOTS] & shadow_slot_overlaps(slot));
}

/* (the retained casters) a slot of the first page that no caster held, nor
one overlapping it */
static boolean shadow_slot_unowned(
	short slot)
{
	return !(shadow_atlas_owned[slot / SHADOW_ATLAS_SLOTS] & shadow_slot_overlaps(slot));
}

static void shadow_slot_owner_forget(
	short slot)
{
	shadow_atlas_owners[slot].object_index = NONE;
	shadow_atlas_owners[slot].fingerprint = 0;
	shadow_atlas_owners[slot].used = FALSE;
	shadow_atlas_owned[slot / SHADOW_ATLAS_SLOTS] &= ~(1UL << (slot % SHADOW_ATLAS_SLOTS));

	return;
}

/* a free slot of a level on a page (NONE: none), the whole cells from the
first, the smaller from the last, which packs them together and leaves the
whole cells free; one no caster held if unowned */
static short shadow_slot_find(
	short page,
	short level,
	boolean unowned)
{
	short cells = shadow_atlas_capacity(shadow_atlas_pages[page].atlas_class);
	short first = level == 0 ? 0 : level == 1 ? 1 : 5;
	short count = level == 0 ? 1 : level == 1 ? 4 : 16;
	short index, sub;

	for (index = 0; index < cells; index++)
	{
		short cell = level == 0 ? index : cells - 1 - index;

		for (sub = first; sub < first + count; sub++)
		{
			short slot = cell * SHADOW_ATLAS_SLOTS + sub;

			if (shadow_slot_free(page, slot) && (!unowned || shadow_slot_unowned(slot)))
			{
				return slot;
			}
		}
	}

	return NONE;
}

/* the slot's taken this frame; on the first page, what the slots it
overlaps held is drawn over (the retained casters) */
static void shadow_slot_claim(
	short page,
	short slot)
{
	short cell = slot / SHADOW_ATLAS_SLOTS;
	short sub = slot % SHADOW_ATLAS_SLOTS;
	unsigned long overlaps = shadow_slot_overlaps(slot) & ~(1UL << sub);
	short index;

	shadow_atlas_claimed[page][cell] |= 1UL << sub;
	if (page == 0 && shadow_retained_on)
	{
		for (index = 0; index < SHADOW_ATLAS_SLOTS; index++)
		{
			if (overlaps & (1UL << index))
			{
				shadow_slot_owner_forget(cell * SHADOW_ATLAS_SLOTS + index);
			}
		}
	}

	return;
}

/* a render target of the maps' format */
static boolean shadow_atlas_target_make(
	long width,
	long height,
	D3DTexture **texture,
	D3DSurface **surface)
{
	return IDirect3DDevice8_CreateTexture(
			global_d3d_device,
			width,
			height,
			1,
			D3DUSAGE_RENDERTARGET,
			D3DFMT_R5G6B5,
			D3DPOOL_DEFAULT,
			texture) >= 0 &&
		*texture &&
		IDirect3DTexture8_GetSurfaceLevel(*texture, 0, surface) >= 0 &&
		*surface;
}

/* a class's atlases, made the first time they are needed */
static boolean shadow_atlas_make(
	short atlas_class)
{
	short index;

	if (shadow_atlas_failed)
	{
		return FALSE;
	}
	for (index = 0; !shadow_atlas_failed && !shadow_atlas_class_made[atlas_class] && index < 2; index++)
	{
		if (!shadow_atlas_target_make(
			SHADOW_ATLAS_WIDTH,
			shadow_atlas_heights[atlas_class],
			&shadow_atlas_textures[atlas_class][index],
			&shadow_atlas_surfaces[atlas_class][index]))
		{
			shadow_atlas_failed = TRUE;
		}
	}
	if (shadow_atlas_failed)
	{
		error(_error_silent, "### ERROR the shadow atlas cannot be made: shadows are drawn one at a time");
		return FALSE;
	}
	shadow_atlas_class_made[atlas_class] = TRUE;

	return TRUE;
}

/* an atlas of the page's class (shadow_atlas_page_class) drawn into, all
of it emptied first if clear, the viewport a slot's region (NONE: all of
it); nothing emptied unless empty (port: the chain's passes after its
first, below).

Where a pass costs by the area it covers (d3d8_clears_alone: a tile-based
GPU, for which a class's 1024 by 256 atlas is most of the screen's tiles,
twice a frame, for one caster), each slot's square is emptied instead as
its turn comes, and the rest of the atlas left as it was: nothing samples
it. A pass's taps reach no further than the guard band (shadow_slot_region),
which is the square's, and a receiver samples its region alone
(d3d8_texture_region). The pixels drawn are the same.

port: a slot's square is emptied once a frame in each atlas, not before
every pass of the chain: the casters' atlas as its caster is drawn, the
chain's other by the chain's first pass. A pass's quad covers its region
whole, opaque, as it covers a map (whose passes empty nothing between them,
rasterizer_shadow_convolve), and draws nothing beyond it (the viewport's),
nor does anything else but the emptying: each guard band stays black from
its frame's first emptying through the chain. The chain's taps read only
the colour (not the alpha, which the passes do not write). On an immediate
GPU the four emptyings a frame left out were a sixth of the chain's time
(a Steam Deck with 60 players: the chain's 3.3 ms a frame to 2.8; the rest
is its quads' four taps a pixel, bound by the texture units). Where the tile-based GPU's emptying of a cell
alone (d3d8_clear_viewport_alone) spares the pass loading the atlas, every
pass empties it still. */
static void shadow_atlas_set_target(
	short atlas,
	short slot,
	boolean clear,
	boolean empty)
{
	D3DVIEWPORT8 viewport;

	IDirect3DDevice8_SetRenderTarget(global_d3d_device, shadow_atlas_surfaces[shadow_atlas_page_class][atlas], NULL);
	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = SHADOW_ATLAS_WIDTH;
	viewport.Height = shadow_atlas_heights[shadow_atlas_page_class];
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	/* port: (the retained casters) each square emptied alone, and what the
	atlas holds elsewhere kept (not d3d8_clear_viewport_alone's) */
	if (slot != NONE && (d3d8_clears_alone() || shadow_retained_on))
	{
		D3DVIEWPORT8 square = viewport;
		long x, y, side;

		shadow_slot_square(slot, &x, &y, &side);
		square.X = x;
		square.Y = y;
		square.Width = side;
		square.Height = side;
		IDirect3DDevice8_SetViewport(global_d3d_device, &square);
		if (clear && !shadow_retained_on)
		{
			d3d8_clear_viewport_alone(0);
		}
		else if (empty)
		{
			IDirect3DDevice8_Clear(global_d3d_device, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
		}
	}
	else if (clear && empty)
	{
		IDirect3DDevice8_SetViewport(global_d3d_device, &viewport);
		IDirect3DDevice8_Clear(global_d3d_device, 0, NULL, D3DCLEAR_TARGET, 0, 1.0f, 0);
	}
	if (slot != NONE)
	{
		long x, y, side;

		shadow_slot_region(slot, &x, &y, &side);
		viewport.X = x;
		viewport.Y = y;
		viewport.Width = side;
		viewport.Height = side;
	}
	IDirect3DDevice8_SetViewport(global_d3d_device, &viewport);

	return;
}

/* a caster drawn into its slot's region of its page's casters' atlas, which
the page's first caster empties */
static void shadow_atlas_caster_target(
	short region)
{
	short page = shadow_atlas_shadows[region].page;

	shadow_atlas_page_class = shadow_atlas_pages[page].atlas_class;
	shadow_atlas_set_target(0, shadow_atlas_shadows[region].slot, !shadow_atlas_pages[page].drawn, TRUE);
	shadow_atlas_pages[page].drawn++;

	return;
}

/* a page past the first for a caster of the level that the first page has
no slot for (NONE: none, its map's, past every page's): the page begun
last while it has a slot, else the atlases of a class no page has, the
smallest holding the casters past the first page the frame before had, and
a few, beyond the pages' (else the largest) */
static short shadow_atlas_spill(
	short level,
	short *slot)
{
	short page = shadow_atlas_page_count - 1;
	short needed = (short)((shadow_atlas_pages_spilled + 15) / 16 + SHADOW_ATLAS_SLACK);
	short chosen = NONE;
	short atlas_class, index;

	shadow_retained_spill_count++;
	*slot = page > 0 ? shadow_slot_find(page, level, FALSE) : NONE;
	if (*slot == NONE)
	{
		for (page = 1; page < shadow_atlas_page_count; page++)
		{
			needed -= shadow_atlas_capacity(shadow_atlas_pages[page].atlas_class);
		}
		for (atlas_class = 0; atlas_class < SHADOW_ATLAS_CLASSES; atlas_class++)
		{
			boolean taken = FALSE;

			for (page = 0; page < shadow_atlas_page_count; page++)
			{
				taken = taken || shadow_atlas_pages[page].atlas_class == atlas_class;
			}
			if (!taken && (chosen == NONE || shadow_atlas_capacity(chosen) < needed))
			{
				chosen = atlas_class;
			}
		}
		if (chosen == NONE || !shadow_atlas_make(chosen))
		{
			return NONE;
		}
		page = shadow_atlas_page_count++;
		shadow_atlas_pages[page].atlas_class = chosen;
		shadow_atlas_pages[page].drawn = 0;
		for (index = 0; index < SHADOW_ATLAS_REGIONS; index++)
		{
			shadow_atlas_claimed[page][index] = 0;
		}
		*slot = shadow_slot_find(page, level, FALSE);
	}
	page = shadow_atlas_page_count - 1;
	shadow_atlas_spilled += shadow_atlas_level_area(level);

	return page;
}

/* a record of the frame's shadows for the caster begun last, in the slot
of the first page's (NONE: a page past it, shadow_atlas_spill), drawn this
frame; NONE: none, its map's */
static short shadow_atlas_take(
	short slot)
{
	short page = 0;
	short region;

	if (shadow_atlas_count >= SHADOW_ATLAS_CASTERS)
	{
		shadow_retained_overflow_count++;
		return NONE;
	}
	if (slot == NONE)
	{
		page = shadow_atlas_spill(shadow_atlas_level, &slot);
		if (page == NONE || slot == NONE)
		{
			shadow_retained_overflow_count++;
			return NONE;
		}
	}
	shadow_slot_claim(page, slot);
	region = shadow_atlas_count++;
	shadow_atlas_shadows[region].page = page;
	shadow_atlas_shadows[region].slot = slot;
	shadow_atlas_shadows[region].level = shadow_atlas_level;
	shadow_atlas_shadows[region].drawn = TRUE;
	shadow_atlas_shadows[region].used = FALSE;

	return region;
}

/* the level of a caster's region (its size: 128 units across at 0, half
that at 1, a quarter at 2): the smallest whose pixels across are still
three times the pixels the shadow's sphere covers on the screen (its
receivers stretch it: a low sun's along the ground), and at most the
levels the maps' scale has above the Xbox's 128 (none at 1: the classic
maps are as they were). A caster at a level the frame before keeps it while
its pixels are twice the sphere's, so that it does not change at the edge
(and its retained region with it). */
static short shadow_atlas_level_choose(
	long object_index,
	real_point3d const *position,
	real radius)
{
	long scale = halo_shadow_map_scale();
	short most = 0;
	short level = 0;
	short before = 0;
	real pixels;
	struct shadow_atlas_level_memory *memory;

	while (most < SHADOW_ATLAS_LEVELS - 1 && (2L << most) <= scale)
	{
		most++;
	}
	if (!most || !shadow_atlas_sizes_on || !global_window_parameters.frustum.projection_valid)
	{
		return 0;
	}
	pixels = render_frustum_sphere_diameter_in_pixels(&global_window_parameters.frustum, position, radius) *
		halo_screen_pixel_scale();
	memory = &shadow_atlas_level_memories[((unsigned long)object_index ^ ((unsigned long)object_index >> 16)) &
		(SHADOW_ATLAS_LEVEL_MEMORIES - 1)];
	if (memory->object_index == object_index)
	{
		before = memory->level;
	}
	while (level < most)
	{
		long x, y, side;

		/* (a slot of the next level's region) */
		shadow_slot_region(level + 1 == 1 ? 1 : 5, &x, &y, &side);
		if ((real)(side * scale) < pixels * (level + 1 <= before ? 2.0f : 3.0f))
		{
			break;
		}
		level++;
	}
	memory->object_index = object_index;
	memory->level = level;

	return level;
}

/* the chain's passes for the scale: the Xbox's, and each widening
distance's two (rasterizer_shadow_convolve) */
static short shadow_atlas_pass_count(
	long scale)
{
	short count = 1;
	long distance;

	for (distance = 1; distance < scale; distance *= 2)
	{
		count += 2;
	}

	return count;
}

/* the offset of a pass's taps, in a map's texture coordinates: half a
texel for the Xbox's, its distance in texels for a widening pass's */
static real shadow_atlas_pass_offset(
	short pass,
	long scale)
{
	return pass == 0 ?
		0.00390625f / (real)scale :
		(real)(1L << ((pass - 1) / 2)) / (real)(128 * scale);
}

/* a region's passes, at its own scale (the maps' at level 0, half at 1...) */
static short shadow_atlas_region_pass_count(
	short region,
	long scale)
{
	long own = scale >> shadow_atlas_shadows[region].level;

	return shadow_atlas_pass_count(own > 1 ? own : 1);
}

/* a pass's offset for a region: the maps' at level 0, else in the texels
of the region's own pixels across */
static real shadow_atlas_region_pass_offset(
	short region,
	short pass,
	long scale)
{
	long x, y, side;
	real pixels;

	if (!shadow_atlas_shadows[region].level)
	{
		return shadow_atlas_pass_offset(pass, scale);
	}
	shadow_slot_region(shadow_atlas_shadows[region].slot, &x, &y, &side);
	pixels = (real)(side * scale);

	return pass == 0 ? 0.5f / pixels : (real)(1L << ((pass - 1) / 2)) / pixels;
}

/* the chain's state, as rasterizer_shadow_convolve sets it, sampling an
atlas */
static void shadow_atlas_states(
	short atlas)
{
	short stage;

	for (stage = 0; stage < 4; stage++)
	{
		IDirect3DDevice8_SetTexture(
			global_d3d_device,
			stage,
			(IDirect3DBaseTexture8 *)shadow_atlas_textures[shadow_atlas_page_class][atlas]);
		IDirect3DDevice8_SetTextureStageState(global_d3d_device, stage, D3DTSS_ADDRESSU, D3DTADDRESS_BORDER);
		IDirect3DDevice8_SetTextureStageState(global_d3d_device, stage, D3DTSS_ADDRESSV, D3DTADDRESS_BORDER);
		IDirect3DDevice8_SetTextureStageState(global_d3d_device, stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice8_SetTextureStageState(global_d3d_device, stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
		IDirect3DDevice8_SetTextureStageState(global_d3d_device, stage, D3DTSS_MIPFILTER, D3DTEXF_POINT);
	}
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_CCW);
	IDirect3DDevice8_SetRenderState(
		global_d3d_device,
		D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZBIAS, 0);
	rasterizer_set_vertex_shader_permutation(_shadow_convolution_vertex_shader_index, 8, 0);

	csmemset(&pixel_shader, 0, sizeof(pixel_shader));
	pixel_shader.texture_modes = 0x00008421;
	pixel_shader.combiner_count = 1;
	pixel_shader.alpha_inputs[0] = 0x08A009A0;
	pixel_shader.alpha_outputs[0] = 0x00030C00;
	pixel_shader.rgb_inputs[0] = 0x0AA00BA0;
	pixel_shader.rgb_outputs[0] = 0x00030C00;
	pixel_shader.final_combiner_inputs_abcd = 0x0C20001C;
	rasterizer_set_pixel_shader(&pixel_shader);

	return;
}

/* a pass's quad over the viewport, its taps the pass's corners
(rasterizer_shadow_convolve_pass) about the texels of a slot's region of
the atlas sampled */
static void shadow_atlas_quad(
	short slot,
	real offset)
{
	/* the Xbox pass's corners, in its order */
	static real const signs[8] = { -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f };
	real const width = (real)SHADOW_ATLAS_WIDTH;
	real const height = (real)shadow_atlas_heights[shadow_atlas_page_class];
	real vertex_constants[8][4];
	long x, y, side;
	short stage;

	shadow_slot_region(slot, &x, &y, &side);
	csmemset(vertex_constants, 0, sizeof(vertex_constants));
	for (stage = 0; stage < 4; stage++)
	{
		/* (the map's coordinates, 0 to 1, made the region's) */
		vertex_constants[stage * 2][0] = (real)side / width;
		vertex_constants[stage * 2][3] =
			((real)x + signs[stage * 2] * offset * (real)side) / width;
		vertex_constants[stage * 2 + 1][1] = (real)side / height;
		vertex_constants[stage * 2 + 1][3] =
			((real)y + signs[stage * 2 + 1] * offset * (real)side) / height;
	}
	IDirect3DDevice8_SetVertexShaderConstant(
		global_d3d_device,
		-81,
		vertex_constants,
		8);

	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 0);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, 1.0078125f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 1, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, 0.9921875f, -0.9921875f);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, 4, 0, 1);
	IDirect3DDevice8_SetVertexData2f(global_d3d_device, 0, -1.0078125f, -0.9921875f);
	IDirect3DDevice8_End(global_d3d_device);

	return;
}

boolean rasterizer_shadow_atlas_begin(
	void)
{
	/* the class whose atlases hold the cells the frame before's casters
	took and a few */
	shadow_atlas_class = 0;
	while (shadow_atlas_class < SHADOW_ATLAS_CLASSES - 1 &&
		(shadow_atlas_casters + 15) / 16 + SHADOW_ATLAS_SLACK > shadow_atlas_capacity(shadow_atlas_class))
	{
		shadow_atlas_class++;
	}
	shadow_atlas_casters = 0;
	shadow_atlas_active = FALSE;
	shadow_atlas_count = 0;
	shadow_atlas_region = NONE;
	shadow_atlas_receiving = NONE;
	/* (comparisons: every region whole, or every other frame's, the same
	scene drawn both ways where the game is paused) */
	shadow_atlas_sizes_on = getenv("HALO_SHADOW_SIZES_SAME") == NULL;
	if (getenv("HALO_SHADOW_SIZES_ALTERNATE"))
	{
		static unsigned long frames = 0;

		shadow_atlas_sizes_on = (++frames & 1) != 0;
	}
	/* the first page, the frame's class's; the casters past it the frame
	before had, kept for the next page's class (shadow_atlas_spill) */
	shadow_atlas_pages[0].atlas_class = shadow_atlas_class;
	shadow_atlas_pages[0].drawn = 0;
	shadow_atlas_page_count = 1;
	shadow_atlas_page_class = shadow_atlas_class;
	if (global_window_parameters.rasterizer_target == _rasterizer_target_render_primary &&
		rasterizer_debug_options.draw_environment_shadows &&
		rasterizer_debug_options.shadow_convolution_enabled &&
		!rasterizer_debug_options.shadow_debug_enabled &&
		halo_shadow_map_scale() <= SHADOW_ATLAS_MOST_SCALE &&
		shadow_atlas_make(shadow_atlas_class))
	{
		shadow_atlas_active = TRUE;
	}
	shadow_retained_frame_begin();
	if (shadow_atlas_active)
	{
		short index;

		for (index = 0; index < SHADOW_ATLAS_REGIONS; index++)
		{
			shadow_atlas_claimed[0][index] = 0;
		}
		shadow_atlas_pages_spilled = shadow_atlas_spilled;
		shadow_atlas_spilled = 0;
	}

	return shadow_atlas_active;
}

short rasterizer_shadow_atlas_caster_end(
	void)
{
	short region = shadow_atlas_region;

	shadow_fingerprinting = FALSE; /* port: (the retained casters) */
	if (region != NONE)
	{
		shadow_atlas_shadows[region].used = shadow_used;
		shadow_atlas_region = NONE;
		if (shadow_retained_on && shadow_atlas_shadows[region].page == 0)
		{
			shadow_atlas_owners[shadow_atlas_shadows[region].slot].used = shadow_used;
		}
	}

	return region;
}

void rasterizer_shadow_atlas_convolve(
	void)
{
	long scale = halo_shadow_map_scale();
	short pass, region, page;

	if (!shadow_atlas_active)
	{
		return;
	}
	shadow_atlas_active = FALSE;
	if (!shadow_atlas_count)
	{
		return;
	}
	/* port: (debug.pass_times) the chain's passes timed as its own */
	halo_timing_category(HALO_TIMING_SHADOW_BLUR);
	/* each page's chain: every pass, back and forth from the casters'
	atlas, ending in the other (each region's count is odd), whose regions
	the receivers sample; each atlas's guard bands black, as each pass
	empties the atlas it draws into (its quads cover their regions). A
	region's passes are its own scale's (shadow_atlas_region_pass_count): a
	smaller region's chain ends before a larger's, its last in the other
	atlas still. (the retained casters) the regions drawn this frame alone,
	each square emptied alone (shadow_atlas_set_target) */
	for (page = 0; page < shadow_atlas_page_count; page++)
	{
		shadow_atlas_page_class = shadow_atlas_pages[page].atlas_class;
		for (pass = 0; ; pass++)
		{
			boolean any = FALSE;

			for (region = 0; region < shadow_atlas_count; region++)
			{
				if (shadow_atlas_shadows[region].page != page ||
					!shadow_atlas_shadows[region].drawn ||
					pass >= shadow_atlas_region_pass_count(region, scale))
				{
					continue;
				}
				if (!any)
				{
					shadow_atlas_states(pass & 1);
				}
				/* port: (emptied by the chain's first pass alone, but where a
				tile-based GPU empties each pass's squares alone: above) */
				shadow_atlas_set_target(
					!(pass & 1),
					shadow_atlas_shadows[region].slot,
					!any,
					pass == 0 || (d3d8_clears_alone() && !shadow_retained_on));
				shadow_atlas_quad(
					shadow_atlas_shadows[region].slot,
					shadow_atlas_region_pass_offset(region, pass, scale));
				any = TRUE;
			}
			if (!any)
			{
				break;
			}
		}
	}
	halo_timing_category(NONE);
	rasterizer_set_target(
		global_window_parameters.rasterizer_target,
		0,
		0,
		FALSE,
		TRUE);
	shadow_restored = TRUE;

	return;
}

void rasterizer_shadow_atlas_receive(
	short region)
{
	match_assert(
		"c:\\halo\\SOURCE\\rasterizer\\xbox\\rasterizer_xbox_shadows.c",
		0,
		region >= 0 && region < shadow_atlas_count);

	local_shadow_matrix = shadow_atlas_shadows[region].matrix;
	local_shadow_color = shadow_atlas_shadows[region].color;
	local_object_bounding_radius = shadow_atlas_shadows[region].radius;
	local_parameters = NULL;
	shadow_setup = FALSE;
	shadow_used = shadow_atlas_shadows[region].used;
	shadow_restored = FALSE;
	shadow_atlas_page_class = shadow_atlas_pages[shadow_atlas_shadows[region].page].atlas_class;
	shadow_atlas_receiving = region;

	return;
}

void rasterizer_shadow_atlas_end(
	void)
{
	d3d8_texture_region(0, NULL);
	shadow_atlas_active = FALSE;
	shadow_atlas_count = 0;
	shadow_atlas_page_count = 1;
	shadow_atlas_region = NONE;
	shadow_atlas_receiving = NONE;

	return;
}

/* ---------- port: the retained casters (above) */

static void shadow_caster_target(
	short region)
{
	if (region != NONE)
	{
		shadow_atlas_caster_target(region);
	}
	else
	{
		rasterizer_set_target(
			2,
			0,
			rasterizer_debug_options.shadow_debug_enabled ? 0x88888888 : 0,
			TRUE,
			FALSE);
	}

	return;
}

static void shadow_model_vertex_constants(
	struct shader_model_definition const *model,
	real_vector4d vertex_constants[3])
{
	vertex_constants[0].i = model->detail_map_scale;
	vertex_constants[0].j = model->detail_map_scale * model->detail_map_v_scale;
	vertex_constants[0].k = 1.0f;
	vertex_constants[0].l = 1.0f;
	vertex_constants[1].i = 1.0f;
	vertex_constants[1].j = 0.0f;
	vertex_constants[1].k = 0.0f;
	vertex_constants[1].l = 0.0f;
	vertex_constants[2].i = 0.0f;
	vertex_constants[2].j = 1.0f;
	vertex_constants[2].k = 0.0f;
	vertex_constants[2].l = 0.0f;

	shader_texture_animation_evaluate(
		&model->animation,
		&local_parameters->animation,
		local_parameters->base_map_scale.i * model->map_scale.i,
		local_parameters->base_map_scale.j * model->map_scale.j,
		0.0f,
		0.0f,
		0.0f,
		global_frame_parameters.game_time_sec,
		&vertex_constants[1],
		&vertex_constants[2]);

	return;
}

/* FNV-1a, 64 bits, over the bytes */
static void shadow_retained_hash(
	void const *data,
	unsigned long size)
{
	unsigned char const *bytes = (unsigned char const *)data;
	unsigned long long hash = shadow_fingerprint;
	unsigned long index;

	for (index = 0; index < size; index++)
	{
		hash = (hash ^ bytes[index]) * 1099511628211ULL;
	}
	shadow_fingerprint = hash;
	if (!shadow_hashing_pose)
	{
		hash = shadow_unposed_fingerprint;
		for (index = 0; index < size; index++)
		{
			hash = (hash ^ bytes[index]) * 1099511628211ULL;
		}
		shadow_unposed_fingerprint = hash;
	}

	return;
}

static void shadow_retained_forget(
	void)
{
	short slot;

	for (slot = 0; slot < SHADOW_ATLAS_REGIONS * SHADOW_ATLAS_SLOTS; slot++)
	{
		shadow_slot_owner_forget(slot);
	}

	return;
}

void rasterizer_shadow_retained_new_map(
	void)
{
	shadow_retained_class = NONE;

	return;
}

/* the view drawing the frame's shadows (render_object_shadows): a window's,
its player's, a mirror's */
void rasterizer_shadow_retained_view(
	long view)
{
	if (view != shadow_retained_view)
	{
		shadow_retained_class = NONE;
	}
	shadow_retained_view = view;

	return;
}

/* the casters' frame begins (rasterizer_shadow_atlas_begin): what the
regions hold is forgotten if the atlas's class, the maps' scale, the
settings or the atlases' targets changed, in the map's preparation, or with
HALO_SHADOWS_REDRAWN */
static void shadow_retained_frame_begin(
	void)
{
	long scale = halo_shadow_map_scale();
	unsigned long changes = config_changes();
	unsigned long targets[2] = { 0, 0 };
	boolean on;

	shadow_receivers_kept = getenv("HALO_SHADOWS_REDRAWN") == NULL;
	on = shadow_receivers_kept && shadow_atlas_active && !render_enumerating;

	if (on)
	{
		targets[0] = d3d8_target_identity(shadow_atlas_surfaces[shadow_atlas_class][0]);
		targets[1] = d3d8_target_identity(shadow_atlas_surfaces[shadow_atlas_class][1]);
	}
	if (!on || shadow_retained_class != shadow_atlas_class || shadow_retained_scale != scale ||
		shadow_retained_changes != changes || targets[0] != shadow_retained_targets[0] ||
		targets[1] != shadow_retained_targets[1])
	{
		shadow_retained_forget();
	}
	shadow_retained_targets[0] = targets[0];
	shadow_retained_targets[1] = targets[1];
	shadow_retained_on = on;
	shadow_retained_class = on ? shadow_atlas_class : NONE;
	shadow_retained_scale = scale;
	shadow_retained_changes = changes;
	shadow_fingerprinting = FALSE;
	if (on)
	{
		shadow_retained_class_frames[shadow_atlas_class]++;
	}
	if (on && ++shadow_retained_frames >= 1200 && config_boolean("debug.gpu_stats"))
	{
		platform_log("retained shadows: %lu casters in %lu frames, %lu kept (%.1f%%), %lu more but for their nodes' matrices, "
			"%lu past the first page (a page more), %lu past every page's regions; frames of each class %lu, %lu, %lu; "
			"regions of each size %lu, %lu, %lu",
			shadow_retained_casters, shadow_retained_frames, shadow_retained_kept_count,
			shadow_retained_casters ? 100.0 * (double)shadow_retained_kept_count / (double)shadow_retained_casters : 0.0,
			shadow_retained_posed_count, shadow_retained_spill_count, shadow_retained_overflow_count,
			shadow_retained_class_frames[0], shadow_retained_class_frames[1], shadow_retained_class_frames[2],
			shadow_retained_level_count[0], shadow_retained_level_count[1], shadow_retained_level_count[2]);
		shadow_retained_spill_count = 0;
		shadow_retained_overflow_count = 0;
		shadow_retained_class_frames[0] = shadow_retained_class_frames[1] = shadow_retained_class_frames[2] = 0;
		shadow_retained_level_count[0] = shadow_retained_level_count[1] = shadow_retained_level_count[2] = 0;
		shadow_retained_posed_count = 0;
		shadow_retained_frames = 0;
		shadow_retained_casters = 0;
		shadow_retained_kept_count = 0;
	}

	return;
}

/* whether the caster begun last is being walked for its draws' hash, its
models to be walked again if rasterizer_shadow_retained_caster says so */
boolean rasterizer_shadow_retained_fingerprinting(
	void)
{
	return shadow_fingerprinting;
}

/* the caster walked: TRUE if its region holds what it would draw (nothing
to draw), else its target set (its region, or its map's once every page
is full, as rasterizer_environment_shadow_begin chooses) and its models to
be walked again to draw them. Its draws' hash has its region's level, so a
region of another size is never kept (shadow_atlas_level_choose keeps the
level while it can). */
boolean rasterizer_shadow_retained_caster(
	void)
{
	unsigned long long fingerprint = shadow_fingerprint;
	short cells = shadow_atlas_capacity(shadow_atlas_class);
	short owned = NONE;
	short slot = NONE;
	short region;
	short cell, sub;

	if (!shadow_fingerprinting)
	{
		return FALSE;
	}
	shadow_fingerprinting = FALSE;
	shadow_retained_casters++;
	for (cell = 0; cell < cells && owned == NONE; cell++)
	{
		if (!shadow_atlas_owned[cell])
		{
			continue;
		}
		for (sub = 0; sub < SHADOW_ATLAS_SLOTS && owned == NONE; sub++)
		{
			short index = cell * SHADOW_ATLAS_SLOTS + sub;

			if (shadow_atlas_owners[index].object_index == shadow_caster_object && shadow_slot_free(0, index))
			{
				owned = index;
			}
		}
	}

	if (owned != NONE && shadow_atlas_owners[owned].fingerprint != fingerprint &&
		shadow_atlas_owners[owned].unposed_fingerprint == shadow_unposed_fingerprint)
	{
		shadow_retained_posed_count++;
	}
	if (owned != NONE && shadow_atlas_owners[owned].fingerprint == fingerprint &&
		(region = shadow_atlas_take(owned)) != NONE)
	{
		shadow_atlas_shadows[region].drawn = FALSE;
		shadow_used = shadow_atlas_owners[owned].used;
		shadow_retained_kept_count++;
		shadow_retained_level_count[shadow_atlas_level]++;
		shadow_atlas_region = region;
		shadow_atlas_shadows[region].matrix = local_shadow_matrix;
		shadow_atlas_shadows[region].color = local_shadow_color;
		shadow_atlas_shadows[region].radius = local_object_bounding_radius;
		return TRUE;
	}

	/* the region it had if of its level, else one no caster had, else one
	whose caster has not come yet; else a page past the first */
	if (owned != NONE && shadow_slot_level(owned) == shadow_atlas_level)
	{
		slot = owned;
	}
	else if (owned != NONE)
	{
		shadow_slot_owner_forget(owned);
	}
	if (slot == NONE)
	{
		slot = shadow_slot_find(0, shadow_atlas_level, TRUE);
	}
	if (slot == NONE)
	{
		slot = shadow_slot_find(0, shadow_atlas_level, FALSE);
	}
	region = shadow_atlas_take(slot);
	shadow_atlas_region = region;
	if (region == NONE)
	{
		shadow_caster_target(NONE);
		return FALSE;
	}
	shadow_retained_level_count[shadow_atlas_level]++;
	if (shadow_atlas_shadows[region].page == 0)
	{
		slot = shadow_atlas_shadows[region].slot;
		shadow_atlas_owners[slot].object_index = shadow_caster_object;
		shadow_atlas_owners[slot].fingerprint = fingerprint;
		shadow_atlas_owners[slot].unposed_fingerprint = shadow_unposed_fingerprint;
		shadow_atlas_owners[slot].used = FALSE;
		shadow_atlas_owned[slot / SHADOW_ATLAS_SLOTS] |= 1UL << (slot % SHADOW_ATLAS_SLOTS);
	}
	shadow_atlas_shadows[region].matrix = local_shadow_matrix;
	shadow_atlas_shadows[region].color = local_shadow_color;
	shadow_atlas_shadows[region].radius = local_object_bounding_radius;
	shadow_caster_target(region);
	shadow_used = FALSE;

	return FALSE;
}

/* ---------- port: a ping's outline */

/* port: the outline of a pinged unit or vehicle (port/linux/game/
network_markers.c, drawn by render_objects.c's render_object_outline): the
object's models drawn as its shadow's casters are (the shadow's vertex
shader, the models' alpha-tested maps), into the view with its camera's
projection in place of the shadow's. A mask pass marks the object's pixels
in the stencil's top bit (PING_OUTLINE_STENCIL_BIT, which the game's own
modes never use: theirs are the low two), drawing no colour; each ring pass
draws the object again moved a few pixels on the screen, in a colour where
the bit is not set, and sets it there (so that the rings, overlapping, blend
once); the object's screen box is then cleared of the bit with one quad (the
caller's draw bounds scissoring it). Through walls the passes test no depth;
depth tested, a ring is drawn only where the object is in front of what the
view has there (the mask not: so that no outline is drawn about a part of
it hidden behind a wall). Nothing of it is drawn without a ping. */

#define PING_OUTLINE_STENCIL_BIT 0x80

static short ping_outline_stencil_mode = NONE;

static void ping_outline_projection(
	real offset_x,
	real offset_y)
{
	real vertex_constants[5][4];
	short column;
	short row;

	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			const real *n = global_window_parameters.frustum.world_to_view.n[row];

			vertex_constants[column][row] =
				n[0] * global_window_parameters.frustum.projection_matrix[0][column] +
				n[1] * global_window_parameters.frustum.projection_matrix[1][column] +
				n[2] * global_window_parameters.frustum.projection_matrix[2][column];
		}
		vertex_constants[column][3] +=
			global_window_parameters.frustum.projection_matrix[3][column];
	}
	/* (moved on the screen: x + offset_x w, y + offset_y w) */
	for (row = 0; row < 4; row++)
	{
		vertex_constants[0][row] += offset_x * vertex_constants[3][row];
		vertex_constants[1][row] += offset_y * vertex_constants[3][row];
		vertex_constants[4][row] = 0.0f;
	}
	IDirect3DDevice8_SetVertexShaderConstant(global_d3d_device, -68, vertex_constants, 5);

	return;
}

boolean rasterizer_ping_outline_begin(
	void)
{
	if (global_window_parameters.rasterizer_target != _rasterizer_target_render_primary ||
		!rasterizer_debug_options.draw_environment_shadows)
	{
		return FALSE;
	}
	shadow_fingerprinting = FALSE;
	ping_outline_stencil_mode = rasterizer_get_stencil_mode();
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHATESTENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZWRITEENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZBIAS, 0);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_FOGENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILENABLE, TRUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILREF, PING_OUTLINE_STENCIL_BIT);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILMASK, PING_OUTLINE_STENCIL_BIT);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILWRITEMASK, PING_OUTLINE_STENCIL_BIT);

	return TRUE;
}

/* a pass: the mask (no colour, every pixel of the object), or a ring moved
offset_x, offset_y (the view's -1..1 units) in a colour */
void rasterizer_ping_outline_pass(
	boolean mask,
	real offset_x,
	real offset_y,
	pixel32 color,
	boolean depth_tested)
{
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_COLORWRITEENABLE, mask ? 0 :
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, !mask);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_BLENDOP, D3DBLENDOP_ADD);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, !mask && depth_tested);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILFUNC, mask ? D3DCMP_ALWAYS : D3DCMP_NOTEQUAL);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);

	/* the colour and its alpha: rgb = c0, alpha = c0 */
	csmemset(&pixel_shader, 0, sizeof(pixel_shader));
	pixel_shader.combiner_count = 1;
	pixel_shader.final_combiner_inputs_abcd = 0x00000001;
	pixel_shader.final_combiner_inputs_efg = 0x00001100;
	pixel_shader.final_combiner_constant_0 = color;
	rasterizer_set_pixel_shader(&pixel_shader);
	ping_outline_projection(offset_x, offset_y);

	return;
}

/* the bit cleared again, over the view (scissored by the caller's draw
bounds to the object's box) */
void rasterizer_ping_outline_clear(
	void)
{
	real screen_constants[5][4];
	short screen_width = global_window_parameters.camera.viewport_bounds.x1 -
		global_window_parameters.camera.viewport_bounds.x0;
	short screen_height = global_window_parameters.camera.viewport_bounds.y1 -
		global_window_parameters.camera.viewport_bounds.y0;

	if (screen_width <= 0 || screen_height <= 0)
		return;
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_COLORWRITEENABLE, 0);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ALPHABLENDENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, FALSE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_CULLMODE, D3DCULL_NONE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_STENCILPASS, D3DSTENCILOP_ZERO);
	rasterizer_set_vertex_shader_permutation(4, 8, 0);
	csmemset(screen_constants, 0, sizeof(screen_constants));
	screen_constants[0][0] = 2.0f / screen_width;
	screen_constants[0][3] = -1.0f - 1.0f / screen_width;
	screen_constants[1][1] = -2.0f / screen_height;
	screen_constants[1][3] = 1.0f + 1.0f / screen_height;
	screen_constants[2][3] = 0.5f;
	screen_constants[3][3] = 1.0f;
	screen_constants[4][3] = 1.0f;
	IDirect3DDevice8_SetVertexShaderConstant(global_d3d_device, -68, screen_constants, 5);
	csmemset(&pixel_shader, 0, sizeof(pixel_shader));
	pixel_shader.combiner_count = 1;
	pixel_shader.final_combiner_inputs_abcd = 0x00000001;
	pixel_shader.final_combiner_inputs_efg = 0x00001100;
	rasterizer_set_pixel_shader(&pixel_shader);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_PSTEXTUREMODES, 0);
	IDirect3DDevice8_Begin(global_d3d_device, D3DPT_TRIANGLEFAN);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_SPECULAR, 0, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_POSITION, 0, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_SPECULAR, 0, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_POSITION, screen_width, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_SPECULAR, 0, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_POSITION, screen_width, screen_height);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_SPECULAR, 0, 0);
	IDirect3DDevice8_SetVertexData2s(global_d3d_device, D3DVSDE_POSITION, 0, screen_height);
	IDirect3DDevice8_End(global_d3d_device);

	return;
}

void rasterizer_ping_outline_end(
	void)
{
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	IDirect3DDevice8_SetRenderState(global_d3d_device, D3DRS_ZENABLE, TRUE);
	rasterizer_stencil_mode_restore(ping_outline_stencil_mode);

	return;
}
