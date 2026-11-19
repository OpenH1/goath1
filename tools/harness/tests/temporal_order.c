/* display.temporal_aa "fsr"'s frame order (port/linux/src/d3d8_device.c,
test_temporal_order.py): the front end's own functions (the window's motion
begun and resolved, the view upscaled, the tone map) against fake targets
and a fake backend that keeps what it was sent. A window's 3D view is drawn
at the render scale's targets; its motion resolved there; the upscale then
takes it into the display's back buffer, where the tone map and the HUD are
drawn after it. At the native scale the two are one target, the order the
same; off, nothing is upscaled and nothing moves. */

#include "harness.h"

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
	int format, depth;
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
} device = { TRUE, { 0x1000 }, { 0x2000 }, 10 };

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

/* display.temporal_aa "fsr" */
static int motion_vectors_technique(void)
{
	return 1;
}

static int config_boolean(const char *name)
{
	(void)name;
	return 0;
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

#include "under_test.inc"

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
	CHECK(halo_motion_window(TRUE, 0.25f, -0.25f), "the window's motion not begun");
	draw(SENT_DRAW);
	halo_motion_resolve(0, 0, 852, 480, rows, 1, 0, camera);
	halo_screen_tone_map(0, 0, 852, 480);
	draw(SENT_HUD);
	halo_motion_window(FALSE, 0.0f, 0.0f);
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

static void off(void)
{
	int resolve, tone_map;

	technique_on = FALSE;
	window(0.67f);
	resolve = sent_find(SENT_RESOLVE);
	tone_map = sent_find(SENT_TONE_MAP);
	CHECK(sent_find(SENT_UPSCALE) < 0, "upscaled while off");
	CHECK(resolve > sent_find(SENT_DRAW) && tone_map > resolve, "the order off");
	CHECK(sent[sent_find(SENT_DRAW)].target == sent[sent_find(SENT_HUD)].target &&
		sent[tone_map].target == sent[sent_find(SENT_DRAW)].target, "another target off");
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "upscaled"))
		upscaled(0.67f);
	else if (!strcmp(which, "native"))
		upscaled(1.0f);
	else if (!strcmp(which, "off"))
		off();
	else
		return 2;
	return 0;
}
