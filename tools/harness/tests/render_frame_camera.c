/*
RENDER_FRAME_CAMERA.C

The frame's windows drawn (source/render/render.c render_frame): between
frames, render.camera and render.frustum are the first player window's
view, not the last window's drawn (the console window after the players',
at the origin) nor a teammate's Spartan link feed drawn before the player's
view. The windows' drawing is stubbed: each window's draw leaves its own
camera in the globals, as render_window and render_nonplayer_frame do.
*/

#include "harness.h"

typedef struct { short x, y; } point2d;
struct bitmap_data { int unused; };
struct render_camera { real_point3d position; };
struct render_frustum { int window; };
struct render_window
{
	short local_player_index;
	boolean console_window;
	struct render_camera render_camera, rasterizer_camera;
};
struct rasterizer_frame_begin_parameters { real game_time_sec; };

static struct
{
	long frame_index;
	real time_delta_since_tick_sec;
	short window_index;
	struct render_camera camera;
	struct render_frustum frustum;
} render;
static short global_screenshot_size = 1;
static boolean render_link_pass;
/* (the feeds a frame draws before the player's view, one each call) */
static int feeds, feeds_left;
static real_point3d const feed_position = { 9.0f, 9.0f, 9.0f };

#define RENDER_MARK_OTHER 0
#define game_time_get() 0L
#define render_interpolation_game_time_sec(time) ((real)(time))
#define rasterizer_frame_begin(parameters) ((void)(parameters))
#define rasterizer_windows_begin() ((void)0)
#define render_marks_frame_begin() ((void)0)
#define world_ao_frame_begin() ((void)0)
#define motion_vectors_frame_begin() ((void)0)
#define projectile_muzzle_views(on) ((void)(on))
#define render_marks_kind(kind, name) ((void)(kind), (void)(name))
#define halo_screen_ui_offset(on) ((void)(on))
#define progress_bar_eachframe() ((void)0)
#define rasterizer_windows_end() ((void)0)
#define rasterizer_frame_end() ((void)0)

static boolean spartan_link_feed_begin(short local_player_index, struct render_camera *camera)
{
	(void)local_player_index;
	if (!feeds_left)
		return FALSE;
	feeds_left--;
	camera->position = feed_position;
	return TRUE;
}

static void spartan_link_feed_end(void)
{
}

/* (render_window: the window's camera and frustum in the globals; a feed's
window is no player's) */
static void render_player_frame(struct render_window *window, const point2d *screenshot_combined_index)
{
	(void)screenshot_combined_index;
	render.camera = window->render_camera;
	render.frustum.window = window->local_player_index == NONE ? 100 : window->local_player_index;
}

static void render_nonplayer_frame(const struct render_window *window, long window_type)
{
	(void)window_type;
	render.camera = window->render_camera;
	render.frustum.window = 200;
}

#include "under_test.inc"

static struct render_window player(short local_player_index, real x)
{
	struct render_window window;

	memset(&window, 0, sizeof(window));
	window.local_player_index = local_player_index;
	window.render_camera.position.x = x;
	window.render_camera.position.y = 2.0f;
	window.render_camera.position.z = 3.0f;
	window.rasterizer_camera = window.render_camera;
	return window;
}

static struct render_window console(void)
{
	struct render_window window;

	memset(&window, 0, sizeof(window));
	window.local_player_index = NONE;
	window.console_window = TRUE;
	return window;
}

int main(int argc, char **argv)
{
	struct render_window windows[3];
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "player-view-after-console"))
	{
		windows[0] = player(0, 1.0f);
		windows[1] = console();
		render_frame(windows, 2, NULL, NULL, NULL, 0.0f);
		CHECK(render.camera.position.x == 1.0f && render.camera.position.y == 2.0f && render.frustum.window == 0,
			"between frames: camera at %.1f %.1f, frustum of window %d", render.camera.position.x,
			render.camera.position.y, render.frustum.window);
		return 0;
	}
	if (!strcmp(name, "player-view-after-feeds"))
	{
		feeds_left = 2;
		windows[0] = player(0, 1.0f);
		windows[1] = console();
		render_frame(windows, 2, NULL, NULL, NULL, 0.0f);
		CHECK(!feeds_left, "the feeds drawn: %d left", feeds_left);
		CHECK(render.camera.position.x == 1.0f && render.frustum.window == 0,
			"between frames: camera at %.1f, frustum of window %d", render.camera.position.x, render.frustum.window);
		return 0;
	}
	if (!strcmp(name, "split-screen-first-player"))
	{
		windows[0] = player(0, 1.0f);
		windows[1] = player(1, 5.0f);
		windows[2] = console();
		render_frame(windows, 3, NULL, NULL, NULL, 0.0f);
		CHECK(render.camera.position.x == 1.0f && render.frustum.window == 0,
			"between frames: camera at %.1f, frustum of window %d", render.camera.position.x, render.frustum.window);
		return 0;
	}
	if (!strcmp(name, "no-player-window"))
	{
		windows[0] = console();
		render.camera.position.x = 7.0f;
		render_frame(windows, 1, NULL, NULL, NULL, 0.0f);
		CHECK(render.camera.position.x == 0.0f && render.frustum.window == 200,
			"no player's window: the console's camera stays (%.1f, frustum of window %d)", render.camera.position.x,
			render.frustum.window);
		return 0;
	}
	(void)feeds;
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
