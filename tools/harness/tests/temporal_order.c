/* display.temporal_aa "fsr"'s frame order (port/linux/src/d3d8_device.c,
test_temporal_order.py): the front end's own functions (the window's motion
begun and resolved, the view upscaled, the tone map) against fake targets
and a fake backend that keeps what it was sent. A window's 3D view is drawn
at the render scale's targets; its motion resolved there; the upscale then
takes it into the display's back buffer, where the tone map and the HUD are
drawn after it. At the native scale the two are one target, the order the
same. Off, below the native scale, the view is drawn at the render
scale too and stretched (technique 0) into the display's back buffer after
its tone map, where the HUD is drawn at the display's scale; at the native
scale off, nothing is stretched and nothing moves. */

#include "harness.h"

#include <math.h>
#include <stdint.h>

typedef int BOOL;
typedef unsigned int xgpu_target;
typedef struct { unsigned long Data; } D3DSurface;

#define XGPU_FORMAT_BGRA8 1
#define XGPU_FORMAT_RGBA16F 2
#define JOURNAL_TONE_MAP 1
#define _anti_aliasing_off 0
#define _anti_aliasing_msaa 4
#define UI_OFFSET 0L

#include "stream.inc"

struct xgpu_render_target
{
	xgpu_target number;
	float scale[2];
	int format, depth, samples;
	unsigned long written;
};

struct render_target_entry
{
	struct xgpu_render_target target;
	const D3DSurface *surface;
	unsigned long last_rendered;
};

static struct
{
	BOOL gl_ready;
	D3DSurface back_buffer, depth_buffer;
	unsigned long frame;
	/* the draws' target (the back buffer, or a shadow map's) */
	const D3DSurface *render_target;
} device = { TRUE, { 0x1000 }, { 0x2000 }, 10, NULL };
static D3DSurface shadow_map = { 0x3000 };

static float screen_scale[2];
static int screen_view;
static BOOL journal_recording, technique_on;
static unsigned long render_target_write_serial;
static struct { BOOL active; } hdr = { TRUE };
static struct { BOOL hdr_view; } inputs;

/* the targets made: one for each surface and scale */
static struct render_target_entry entries[16];
static int entry_count;

static struct render_target_entry *render_target_get(const D3DSurface *surface)
{
	int index;

	for (index = 0; index < entry_count; index++)
	{
		if (entries[index].surface == surface && entries[index].target.scale[0] == screen_scale[0] &&
			entries[index].target.scale[1] == screen_scale[1])
			return &entries[index];
	}
	CHECK(entry_count < 16, "too many targets");
	entries[entry_count].surface = surface;
	entries[entry_count].target.number = (xgpu_target)(entry_count + 1);
	entries[entry_count].target.scale[0] = screen_scale[0];
	entries[entry_count].target.scale[1] = screen_scale[1];
	entries[entry_count].target.depth = surface == &device.depth_buffer;
	entries[entry_count].target.format = surface == &device.depth_buffer ? 0 : XGPU_FORMAT_RGBA16F;
	return &entries[entry_count++];
}

static int anti_aliasing(void)
{
	return _anti_aliasing_off;
}

static BOOL temporal_upscaling(void)
{
	return technique_on;
}

/* display.temporal_aa: 1 "fsr", 2 "dlss", 3 "metalfx" */
static int technique = 1;

static int motion_vectors_technique(void)
{
	return technique;
}

static int config_boolean(const char *name)
{
	(void)name;
	return 0;
}

/* display.temporal_aa_sharpness as the player set it */
static double sharpness_setting;

static double config_real(const char *name)
{
	CHECK(!strcmp(name, "display.temporal_aa_sharpness"), "another setting read: %s", name);
	return sharpness_setting;
}

static void journal_operation(int operation, const void *arguments, unsigned long size)
{
	(void)operation;
	(void)arguments;
	(void)size;
}

/* what the backend was sent, in order */
enum { SENT_DRAW, SENT_RESOLVE, SENT_UPSCALE, SENT_TONE_MAP, SENT_HUD };
static struct
{
	int kind;
	xgpu_target target, destination;
	float scale;
	struct xgpu_temporal_pass pass;
} sent[16];
static int sent_count;

static void render_target_resolve(struct xgpu_render_target *target)
{
	(void)target;
}

static void sent_add(int kind, xgpu_target target)
{
	CHECK(sent_count < 16, "too much sent");
	sent[sent_count].kind = kind;
	sent[sent_count].target = target;
	sent_count++;
}

static void halo_motion_objects(int begin)
{
	(void)begin;
}

static void xgpu_backend_motion(int phase, xgpu_target target, const struct xgpu_motion_pass *pass)
{
	(void)pass;
	if (phase == XGPU_MOTION_RESOLVE)
		sent_add(SENT_RESOLVE, target);
}

static void xgpu_backend_temporal(xgpu_target source, xgpu_target destination, const struct xgpu_temporal_pass *pass)
{
	sent_add(SENT_UPSCALE, source);
	sent[sent_count - 1].destination = destination;
	sent[sent_count - 1].pass = *pass;
}

static void xgpu_backend_tone_map(xgpu_target target, const long corners[4])
{
	(void)corners;
	sent_add(SENT_TONE_MAP, target);
}

/* (MetalFX's spatial scaler alone: 2) */
static struct { BOOL temporal_metalfx; } xgpu_backend_capabilities = { 1 };

/* render_scale_configured as a phone's build has it (#1228): the host's
HALO_UPSCALE_RENDER_SCALE */
#define HALO_ANDROID 1
#define RENDER_SCALE_LEAST 0.25f

static void platform_log(const char *format, ...)
{
	(void)format;
}

#include "under_test.inc"

/* a phone's render scale, upscaled: the host draws the screen's pixels and
gives the game the render scale, its 3D view's */
static void phone_scale(void)
{
	setenv("HALO_UPSCALE_RENDER_SCALE", "0.67", 1);
	CHECK(fabsf(render_scale_configured() - 0.67f) < 1e-6f, "the phone's render scale %f, not the host's 0.67",
		render_scale_configured());
}

/* a draw into the back buffer, as the game's are (the view's, the HUD's) */
static void draw(int kind)
{
	struct render_target_entry *target = render_target_get(&device.back_buffer);

	sent_add(kind, target->target.number);
	sent[sent_count - 1].scale = target->target.scale[0];
}

static struct render_target_entry *entry(xgpu_target number)
{
	CHECK(number >= 1 && (int)number <= entry_count, "no target %u", number);
	return &entries[number - 1];
}

/* one window, render_window's order: the motion begun, the view drawn, the
motion resolved (and upscaled), the tone map, the HUD, the motion ended */
static void window(float factor)
{
	static const float rows[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
	static const float camera[3] = { 0.0625f, 1024.0f, 1.2f };
	float display = 1080.0f / 480.0f;

	screen_scale[0] = screen_scale[1] = display;
	temporal.factor = factor;
	halo_screen_view_begin(TRUE);
	CHECK(halo_motion_window(technique_on, 0.25f, -0.25f) == technique_on, "the window's motion begun %d",
		!technique_on);
	draw(SENT_DRAW);
	if (technique_on)
		halo_motion_resolve(0, 0, 852, 480, rows, 1, 0, camera);
	halo_screen_tone_map(0, 0, 852, 480);
	halo_screen_view_end(TRUE, 0, 0, 852, 480);
	draw(SENT_HUD);
	halo_motion_window(FALSE, 0.0f, 0.0f);
	halo_screen_view_end(FALSE, 0, 0, 0, 0);
	CHECK(screen_scale[0] == display && screen_scale[1] == display, "the display's scale not back: %f",
		screen_scale[0]);
}

static int sent_find(int kind)
{
	int index;

	for (index = 0; index < sent_count; index++)
	{
		if (sent[index].kind == kind)
			return index;
	}
	return -1;
}

static void upscaled(float factor)
{
	float display = 1080.0f / 480.0f;
	int draw_at, resolve, upscale, tone_map, hud;

	technique_on = TRUE;
	window(factor);
	draw_at = sent_find(SENT_DRAW);
	resolve = sent_find(SENT_RESOLVE);
	upscale = sent_find(SENT_UPSCALE);
	tone_map = sent_find(SENT_TONE_MAP);
	hud = sent_find(SENT_HUD);
	CHECK(draw_at >= 0 && resolve > draw_at && upscale > resolve && tone_map > upscale && hud > tone_map,
		"the order: draw %d resolve %d upscale %d tone map %d HUD %d", draw_at, resolve, upscale, tone_map, hud);
	/* (the view at the render scale; its motion resolved there; the upscale
	from it into the display's back buffer) */
	CHECK(sent[draw_at].scale == display * factor, "the view drawn at %f, not %f", sent[draw_at].scale,
		display * factor);
	CHECK(sent[resolve].target == sent[draw_at].target, "the motion resolved in another target");
	CHECK(sent[upscale].target == sent[draw_at].target, "the upscale's source not the view's target");
	CHECK(entry(sent[upscale].destination)->target.scale[0] == display &&
		!entry(sent[upscale].destination)->target.depth, "the upscale's destination not the display's back buffer");
	CHECK(entry(sent[upscale].pass.depth)->target.depth &&
		entry(sent[upscale].pass.depth)->target.scale[0] == display * factor, "the upscale's depth not the view's");
	CHECK((factor < 1.0f) == (sent[upscale].destination != sent[upscale].target),
		"the source and destination one target: %d", factor < 1.0f);
	/* (its corners: the view's at each scale) */
	CHECK(sent[upscale].pass.corners[2] == (int32_t)(852 * display * factor + 0.5f) &&
		sent[upscale].pass.destination[2] == (int32_t)(852 * display + 0.5f), "the corners %d %d",
		sent[upscale].pass.corners[2], sent[upscale].pass.destination[2]);
	CHECK(sent[upscale].pass.reset == 0 && sent[upscale].pass.technique == 1, "the pass's history or technique");
	/* (the tone map and the HUD over the upscaled view, at the display's
	scale) */
	CHECK(sent[tone_map].target == sent[upscale].destination, "the tone map not over the upscaled view");
	CHECK(sent[hud].target == sent[upscale].destination && sent[hud].scale == display,
		"the HUD drawn at %f, not over the upscaled view", sent[hud].scale);
}

/* off at the native scale: one target, nothing moved */
static void off(void)
{
	int tone_map;

	technique_on = FALSE;
	window(1.0f);
	tone_map = sent_find(SENT_TONE_MAP);
	CHECK(sent_find(SENT_UPSCALE) < 0 && sent_find(SENT_RESOLVE) < 0, "upscaled or resolved while off");
	CHECK(tone_map > sent_find(SENT_DRAW), "the order off");
	CHECK(sent[sent_find(SENT_DRAW)].target == sent[sent_find(SENT_HUD)].target &&
		sent[tone_map].target == sent[sent_find(SENT_DRAW)].target, "another target off");
}

/* display.render_scale off below the native scale: the view (and its tone
map) at the render scale, stretched (technique 0) into the display's back
buffer, the HUD drawn there at the display's scale */
static void stretched(void)
{
	float display = 1080.0f / 480.0f, factor = 0.67f;
	int draw_at, tone_map, stretch, hud;

	technique_on = FALSE;
	window(factor);
	draw_at = sent_find(SENT_DRAW);
	tone_map = sent_find(SENT_TONE_MAP);
	stretch = sent_find(SENT_UPSCALE);
	hud = sent_find(SENT_HUD);
	CHECK(sent_find(SENT_RESOLVE) < 0, "motion resolved while off");
	CHECK(draw_at >= 0 && tone_map > draw_at && stretch > tone_map && hud > stretch,
		"the order: draw %d tone map %d stretch %d HUD %d", draw_at, tone_map, stretch, hud);
	CHECK(sent[draw_at].scale == display * factor, "the view drawn at %f, not %f", sent[draw_at].scale,
		display * factor);
	CHECK(sent[tone_map].target == sent[draw_at].target, "the tone map not over the view at its scale");
	CHECK(sent[stretch].pass.technique == 0 && sent[stretch].target == sent[draw_at].target &&
		sent[stretch].destination != sent[stretch].target &&
		entry(sent[stretch].destination)->target.scale[0] == display &&
		!entry(sent[stretch].destination)->target.depth, "the stretch not from the view into the display's back buffer");
	CHECK(sent[stretch].pass.corners[2] == (int32_t)(852 * display * factor + 0.5f) &&
		sent[stretch].pass.destination[2] == (int32_t)(852 * display + 0.5f), "the corners %d %d",
		sent[stretch].pass.corners[2], sent[stretch].pass.destination[2]);
	CHECK(sent[hud].target == sent[stretch].destination && sent[hud].scale == display, "the HUD drawn at %f, not %f",
		sent[hud].scale, display);
}

/* display.temporal_aa_sharpness sent with the upscale (FSR's RCAS), 0 to 1:
0 none, past either end the end */
static void sharpness(void)
{
	static const double set[] = { 0.0, 0.5, 1.0, 3.0, -1.0 };
	static const float sent_as[] = { 0.0f, 0.5f, 1.0f, 1.0f, 0.0f };
	int index, upscale;

	technique_on = TRUE;
	for (index = 0; index < 5; index++)
	{
		sent_count = 0;
		sharpness_setting = set[index];
		window(0.67f);
		upscale = sent_find(SENT_UPSCALE);
		CHECK(upscale >= 0, "not upscaled");
		CHECK(sent[upscale].pass.sharpness == sent_as[index], "sharpness %g sent as %g, not %g", set[index],
			sent[upscale].pass.sharpness, sent_as[index]);
	}
}

/* the upscaler's texture LOD bias: each vendor's at the render scale, for
the view's draws into the back buffer alone, while the window's motion is
on; and the jitter's phases (AMD's 8 times the display's pixels per the
render's, squared) */
static void lod_bias_at(int which, float factor, float expected, int phases)
{
	float display = 1080.0f / 480.0f;

	technique = which;
	technique_on = TRUE;
	screen_scale[0] = screen_scale[1] = display;
	temporal.factor = factor;
	CHECK(halo_motion_jitter_phases() == phases, "technique %d at %g: %d phases, not %d", which, factor,
		halo_motion_jitter_phases(), phases);
	device.render_target = &device.back_buffer;
	CHECK(draw_lod_bias() == 0.0f, "a bias before the window's motion");
	CHECK(halo_motion_window(TRUE, 0.25f, -0.25f), "the window's motion not begun");
	CHECK(fabsf(draw_lod_bias() - expected) < 0.001f, "technique %d at %g: bias %f, not %f", which, factor,
		draw_lod_bias(), expected);
	device.render_target = &shadow_map;
	CHECK(draw_lod_bias() == 0.0f, "a shadow map's draws biased: %f", draw_lod_bias());
	device.render_target = &device.back_buffer;
	halo_motion_window(FALSE, 0.0f, 0.0f);
	CHECK(draw_lod_bias() == 0.0f, "the HUD's draws biased: %f", draw_lod_bias());
	device.render_target = NULL;
}

static void lod_bias(void)
{
	/* (FSR: log2 less 1; DLSS, MetalFX: log2) */
	lod_bias_at(1, 0.67f, log2f(0.67f) - 1.0f, 17);
	lod_bias_at(1, 1.0f, -1.0f, 8);
	lod_bias_at(1, 0.5f, -2.0f, 32);
	lod_bias_at(2, 0.67f, log2f(0.67f), 17);
	lod_bias_at(2, 1.0f, 0.0f, 8);
	lod_bias_at(3, 0.67f, log2f(0.67f), 17);
	/* (off: none, the base phases) */
	technique = 1;
	technique_on = FALSE;
	temporal.factor = 1.0f;
	device.render_target = &device.back_buffer;
	CHECK(halo_motion_jitter_phases() == 8, "off: %d phases", halo_motion_jitter_phases());
	halo_motion_window(TRUE, 0.25f, -0.25f);
	CHECK(draw_lod_bias() == 0.0f, "biased while off: %f", draw_lod_bias());
	halo_motion_window(FALSE, 0.0f, 0.0f);
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "upscaled"))
		upscaled(0.67f);
	else if (!strcmp(which, "sharpness"))
		sharpness();
	else if (!strcmp(which, "native"))
		upscaled(1.0f);
	else if (!strcmp(which, "off"))
		off();
	else if (!strcmp(which, "stretched"))
		stretched();
	else if (!strcmp(which, "lod_bias"))
		lod_bias();
	else if (!strcmp(which, "phone_scale"))
		phone_scale();
	else
		return 2;
	return 0;
}
