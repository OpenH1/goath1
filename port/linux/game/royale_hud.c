/* royale_hud.c: Royale's HUD in Halo's own style: squads and players left, the teammate on the tracker and nav marker, the beam warning

Drawn with the HUD's own parts, as the stock HUD draws its own: the HUD's
digits (the ammo counter's and the campaign timer's, hud_draw_numbers) at
the HUD's anchors, its font in its state colour, the multiplayer HUD's
teammate triangle, the waypoint arrow with its distance in meters
(hud_render_nav_point_colored, as a ping's) and the motion tracker's blips
(motion_sensor.c asks royale_hud_tracker_mate). The timer flashes as the
campaign's does under its cutoff (the HUD globals' timer colours).

Presentation only: everything read here is state every machine has (the
rules' players, squads and states the host sends; the beam's stage from
the synced match clock), never the host's alone, so a client's screen
shows what the host's does. */
#include "cseries.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/texture_cache.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_definitions.h"
#include "interface/hud_draw.h"
#include "interface/hud_messaging.h"
#include "interface/interface.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_camera_projection.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "mcc_hud_draw.h"
#include "royale.h"
#include "royale_beam.h"
#include "royale_hud.h"

/* hud_nav_points.c's (as network_markers.c declares them) */
short hud_find_nav_point_quietly(char const *name);
boolean hud_render_nav_point_colored(short local_player_index, real_point3d const *position, short nav_index,
	short waypoint_type, real_rgb_color const *color, real alpha, real_point2d *drawn_position, real *half_height,
	short *drawn_type);
void draw_quad(rectangle2d *rectangle, pixel32 color);

/* ---- the HUD's own types (hud_draw.c's; each HUD file keeps a copy) */

#define ROYALE_HUD_NUMBER_TAG 'hud#'

struct royale_hud_number_element
{
	struct hud_placement_definition placement;
	struct hud_color_definition colors;
	char digits;
	byte number_flags;
	char fractional_digits;
	byte pad;
	long unused[3];
};

struct royale_hud_number_definition
{
	struct tag_reference number_bitmap;
	char character_width;
	char screen_width;
	char x_offset;
	char y_offset;
	char decimal_point_width;
	char colon_width;
	short pad;
	long unused[19];
};

enum
{
	ROYALE_HUD_NUMBER_LEADING_ZEROS = 1 << 0,
	ROYALE_HUD_NUMBER_TRAILING_M = 1 << 2,
	ROYALE_HUD_NUMBER_COLON = 11 /* the digits' colon frame */
};

/* the waypoint's kinds (hud_nav_points.c): on the screen, off it */
#define ROYALE_HUD_WAYPOINT_ON_SCREEN 0
#define ROYALE_HUD_WAYPOINT_OFF_SCREEN 1

/* the squadmate's nav marker: beyond the motion tracker's reach */
#define ROYALE_HUD_MATE_MARKER_SCALE 1.0f
/* the beam's warning: this long before the ground under the player glasses */
#define ROYALE_HUD_BEAM_WARNING_SECONDS 20.0f
/* the digits' sizes: the counts', the timer's (the campaign's is 2) */
#define ROYALE_HUD_COUNT_SCALE 1.5f
#define ROYALE_HUD_TIMER_SCALE 2.0f
/* the countdown flashes under this */
#define ROYALE_HUD_BEAM_FLASH_SECONDS 10

static struct
{
	short nav_index; /* the waypoint arrow (NONE: none in this map) */
	boolean nav_looked_up;
	/* each local view's: when its warning began (the flash's reference) */
	long warning_since[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
} royale_hud;

/* ---- module */

void royale_hud_start(
	struct royale_map_config const *config)
{
	(void)config;
	royale_hud_reset();
}

void royale_hud_reset(
	void)
{
	short index;

	royale_hud.nav_index = NONE;
	royale_hud.nav_looked_up = FALSE;
	for (index = 0; index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; index++)
		royale_hud.warning_since[index] = NONE;
}

void royale_hud_tick(
	void)
{
}

/* ---- the players and squads (synced: the rules' states) */

static boolean royale_hud_player_in(
	enum royale_player_state state)
{
	return state == ROYALE_PLAYER_IN_PELICAN || state == ROYALE_PLAYER_FALLING || state == ROYALE_PLAYER_ALIVE ||
		state == ROYALE_PLAYER_DOWNED || state == ROYALE_PLAYER_GULAG;
}

static short royale_hud_players_left(
	void)
{
	struct data_iterator iterator;
	short count = 0;

	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
	{
		if (royale_hud_player_in(royale_player_state(iterator.datum_index)))
			count++;
	}
	return count;
}

/* the player's unit if living, else NONE */
static long royale_hud_living_unit(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct unit_datum *unit;

	if (!player || player->unit_index == NONE)
		return NONE;
	unit = unit_try_and_get(player->unit_index);
	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit))
		return NONE;
	return player->unit_index;
}

boolean royale_hud_squadmate(
	long viewer_player_index,
	long player_index)
{
	return royale_active() && viewer_player_index != NONE && player_index != NONE &&
		viewer_player_index != player_index && royale_squadmate_of(viewer_player_index) == player_index;
}

boolean royale_hud_motion_sensor_shown(
	long player_index)
{
	return royale_active() && player_index != NONE && royale_phase() >= ROYALE_PHASE_INSERTION &&
		royale_phase() < ROYALE_PHASE_RESULTS;
}

boolean royale_hud_tracker_mate(
	short local_player_index,
	real_point2d *offset,
	boolean *downed)
{
	long viewer;
	long mate;
	long unit_index;
	long own_unit;
	real_point3d position;
	real_point3d camera;

	if (!royale_active() || local_player_index == NONE)
		return FALSE;
	viewer = local_player_get_player_index(local_player_index);
	mate = viewer != NONE ? royale_squadmate_of(viewer) : NONE;
	unit_index = royale_hud_living_unit(mate);
	own_unit = royale_hud_living_unit(viewer);
	if (unit_index == NONE || own_unit == NONE)
		return FALSE;
	/* (where both are drawn, as the tracker's own blips are) */
	object_get_origin(unit_index, &position);
	render_interpolation_object_point(unit_index, &position);
	render_interpolation_unit_camera_position(own_unit, &camera);
	offset->x = position.x - camera.x;
	offset->y = position.y - camera.y;
	*downed = royale_player_state(mate) == ROYALE_PLAYER_DOWNED;
	return TRUE;
}

/* ---- drawing helpers: the HUD's font, colours and digits */

static boolean royale_hud_font(
	long *font_index,
	short *line)
{
	struct font_header *font;

	*font_index = hud_get_font_index();
	if (*font_index == NONE)
		return FALSE;
	font = font_definition_get(*font_index);
	*line = (short)(font->ascending_height + font->descending_height);
	return TRUE;
}

/* the HUD's text colour (its state messages'), alpha as given */
static real_argb_color royale_hud_text_color(
	real alpha)
{
	real_argb_color color;

	hud_get_text_color(&color);
	color.alpha = alpha;
	return color;
}

static void royale_hud_text(
	rectangle2d const *bounds,
	short justification,
	wchar_t const *text,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();

	if (font_index == NONE)
		return;
	draw_string_set_draw_mode(font_index, NONE, justification, 0, color);
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
}

static struct royale_hud_number_definition const *royale_hud_digits(
	void)
{
	long index = interface_get_tag_index(_interface_hud_digits);

	return index != NONE ? (struct royale_hud_number_definition const *)tag_get(ROYALE_HUD_NUMBER_TAG, index) : NULL;
}

/* a whole number in the HUD's digits at an anchor's offset (the HUD's
units), coloured as colors says (flashing from since when flash) */
static void royale_hud_number(
	short local_player_index,
	short corner,
	short x,
	short y,
	short value,
	char digits,
	byte flags,
	struct hud_color_definition const *colors,
	boolean flash,
	long since,
	real scale)
{
	struct hud_absolute_placement_definition placement;
	struct royale_hud_number_element number;

	csmemset(&placement, 0, sizeof(placement));
	csmemset(&number, 0, sizeof(number));
	placement.corner = corner;
	number.placement.offset.x = x;
	number.placement.offset.y = y;
	number.placement.scale.i = 1.0f;
	number.placement.scale.j = 1.0f;
	number.colors = *colors;
	number.digits = digits;
	number.number_flags = flags;
	hud_draw_numbers(local_player_index, &placement, (struct number_hud_element_definition const *)&number, value,
		NONE, flash ? FLAG(_hud_draw_flashing_bit) : 0, since, scale);
}

/* the same with its right edge at x and its top at y, in the view's
coordinates (the top right anchor's: the digits go leftward from it) */
static void royale_hud_number_at(
	short local_player_index,
	short right,
	short top,
	short value,
	char digits,
	byte flags,
	struct hud_color_definition const *colors,
	boolean flash,
	long since,
	real scale)
{
	short window_right = (short)(render.camera.window_bounds.x1 - render.camera.viewport_bounds.x0);
	short window_top = (short)(render.camera.window_bounds.y0 - render.camera.viewport_bounds.y0);

	royale_hud_number(local_player_index, _hud_anchor_top_right, (short)(window_right - right),
		(short)(top - window_top), value, digits, flags, colors, flash, since, scale);
}

/* where an anchor's offset lands in the view (as hud_draw_numbers places it) */
static point2d royale_hud_point(
	short local_player_index,
	short corner,
	short x,
	short y)
{
	struct hud_absolute_placement_definition placement;
	struct hud_placement_definition offset;
	point2d point;

	csmemset(&placement, 0, sizeof(placement));
	csmemset(&offset, 0, sizeof(offset));
	placement.corner = corner;
	offset.offset.x = x;
	offset.offset.y = y;
	offset.scale.i = offset.scale.j = 1.0f;
	hud_calculate_point(local_player_index, &placement, &offset, NULL, FALSE, 0.0f, &point);
	return point;
}

/* the digits' width and height, in the HUD's units */
static void royale_hud_digit_size(
	short *width,
	short *height)
{
	struct royale_hud_number_definition const *digits = royale_hud_digits();
	struct bitmap_data const *bitmap = NULL;
	real_rectangle2d const *clip = NULL;

	*width = 8;
	*height = 12;
	if (!digits)
		return;
	*width = (short)MAX(digits->screen_width, 4);
	hud_retrieve_bitmap_and_bounding_rect(digits->number_bitmap.index, 0, 0, &bitmap, &clip);
	if (bitmap && clip)
		*height = (short)MAX(bitmap->height * (clip->y1 - clip->y0), 6.0f);
}

/* the timer's colours (the campaign's countdown, its flash under the cutoff) */
static struct hud_color_definition const *royale_hud_timer_colors(
	boolean time_up)
{
	return time_up ? &hud_globals->timer_definition.time_up_color : &hud_globals->timer_definition.color;
}

/* a colour of the HUD's flashing as the timer's does, as a real colour */
static real_argb_color royale_hud_flash_color(
	struct hud_color_definition const *colors,
	boolean flash,
	long since)
{
	real_argb_color color;

	pixel32_to_real_argb_color(flash ? get_flash_color(colors, since) : colors->color, &color);
	if (color.alpha <= 0.0f)
		color.alpha = 1.0f;
	return color;
}

/* ---- squads and players left: bottom right, the HUD's digits */

static void royale_hud_draw_counts(
	short local_player_index,
	long player_index)
{
	short digit_width, digit_height;
	short squads = royale_squads_alive();
	short players = royale_hud_players_left();
	short kills = royale_player_kills(player_index);
	struct hud_color_definition const *colors = royale_hud_timer_colors(FALSE);
	real_argb_color label = royale_hud_text_color(0.9f);
	long font_index;
	short line;
	short row;
	static wchar_t const *const labels[] = { L"SQUADS", L"PLAYERS", L"KILLS" };
	short values[3];

	if (!royale_hud_font(&font_index, &line))
		return;
	values[0] = squads;
	values[1] = players;
	values[2] = kills;
	royale_hud_digit_size(&digit_width, &digit_height);
	for (row = 0; row < 3; row++)
	{
		/* (from the bottom: kills lowest, squads on top; the match clock's
		row under them stays free) */
		short y = (short)(22 + (2 - row) * (MAX((short)(digit_height * ROYALE_HUD_COUNT_SCALE), line) + 4));
		short x = 12;
		point2d right;
		rectangle2d bounds;

		royale_hud_number(local_player_index, _hud_anchor_bottom_right, x, y, values[row], 2,
			ROYALE_HUD_NUMBER_LEADING_ZEROS, colors, FALSE, 0, ROYALE_HUD_COUNT_SCALE);
		right = royale_hud_point(local_player_index, _hud_anchor_bottom_right, x, y);
		bounds.x1 = (short)(right.x - 2 * digit_width * ROYALE_HUD_COUNT_SCALE - 6);
		bounds.x0 = (short)(bounds.x1 - 160);
		bounds.y0 = (short)(right.y - digit_height * ROYALE_HUD_COUNT_SCALE +
			(digit_height * ROYALE_HUD_COUNT_SCALE - line) / 2);
		bounds.y1 = (short)(bounds.y0 + line);
		/* (right justified: 1) */
		royale_hud_text(&bounds, 1, labels[row], &label);
	}
}

/* the waypoint arrow, as a ping's (network_markers.c): the campaign's, else
the multiplayer goals', else the first; NONE if the map has none */
static short royale_hud_nav_index(
	void)
{
	static char const *const arrows[] = { "default", "target_blue", NULL };
	short index;

	if (!royale_hud.nav_looked_up)
	{
		royale_hud.nav_index = NONE;
		for (index = 0; index < (short)NUMBEROF(arrows) && royale_hud.nav_index == NONE; index++)
			royale_hud.nav_index = hud_find_nav_point_quietly(arrows[index]);
		royale_hud.nav_looked_up = TRUE;
	}
	return royale_hud.nav_index;
}

/* ---- the squadmate: the teammate triangle over them, their nav marker far
off, downed flashing red */

static void royale_hud_player_name(long player_index, wchar_t *name, short count);

static void royale_hud_draw_mate_triangle(
	long mate,
	long unit_index,
	boolean downed)
{
	long bitmap_group = interface_get_tag_index(_interface_bitmap_multiplayer_hud);
	struct bitmap_data *bitmap;
	real_point3d head;
	real_point3d view;
	real_point2d screen;
	point2d position;
	real_rectangle2d clip;
	real depth_factor;
	real fade;
	pixel32 color;

	if (bitmap_group == NONE)
		return;
	bitmap = bitmap_group_get_bitmap_from_sequence(bitmap_group, 0, 0);
	if (!bitmap || !_texture_cache_bitmap_get_hardware_format(bitmap, FALSE, TRUE))
		return;
	unit_get_head_position(unit_index, &head);
	head.z += 0.3f;
	matrix4x3_transform_point(&render.frustum.world_to_view, &head, &view);
	if (!render_camera_view_to_screen(&render.camera, &render.frustum, &view, &screen))
		return;
	hud_draw_mark_begin(screen.x - render.camera.viewport_bounds.x0, screen.y - render.camera.viewport_bounds.y0,
		&position);
	/* (as hud.c's teammate triangle: shorter with distance) */
	depth_factor = 1.0f - ((-view.z - render.frustum.z_near) * 100.0f) /
		(MIN(render.frustum.z_far, rasterizer_globals.far_clip_distance) - render.frustum.z_near);
	fade = PIN(depth_factor, 0.075f, 1.0f);
	clip.x0 = 0.0f;
	clip.x1 = 1.0f;
	clip.y0 = (1.0f - fade) * 0.5f;
	clip.y1 = 1.0f;
	/* (downed: flashing, on and off, as the timer's time-up) */
	color = 0xFFFFFFFF;
	if (downed)
	{
		real_argb_color red = royale_hud_flash_color(royale_hud_timer_colors(TRUE), TRUE, 0);

		color = real_argb_color_to_pixel32(&red);
	}
	if (!downed || (game_time_get() / (TICKS_PER_SECOND / 4)) % 2 == 0)
		interface_draw_bitmap(bitmap, &position, &clip, mcc_hud_bitmap_scale(bitmap_group), 0.0f, 1.0f);
	/* (their name over it, as hud.c draws an ally's: the HUD's text colour,
	fading to 0.4 from 15 to 75 world units; DOWNED in the alarm's red) */
	{
		long font_index;
		short line;

		if (royale_hud_font(&font_index, &line))
		{
			wchar_t name[16];
			wchar_t text[40];
			rectangle2d bounds;
			real_argb_color text_color = royale_hud_text_color(PIN(1.0f - (-view.z - 15.0f) / 60.0f, 0.4f, 1.0f));
			short y = (short)(position.y - bitmap->registration_point.y * mcc_hud_bitmap_scale(bitmap_group) - 2);

			royale_hud_player_name(mate, name, NUMBEROF(name));
			if (downed)
			{
				usnprintf(text, NUMBEROF(text) - 1, L"%s  DOWNED", name);
				pixel32_to_real_argb_color(color, &text_color);
			}
			else
				usnprintf(text, NUMBEROF(text) - 1, L"%s", name);
			text[NUMBEROF(text) - 1] = 0;
			bounds.x0 = (short)(position.x - 160);
			bounds.x1 = (short)(position.x + 160);
			bounds.y1 = y;
			bounds.y0 = (short)(y - line);
			royale_hud_text(&bounds, 2, text, &text_color);
		}
	}
	hud_draw_mark_end();
}

static void royale_hud_draw_mate(
	short local_player_index,
	long player_index)
{
	long mate = royale_squadmate_of(player_index);
	long unit_index = royale_hud_living_unit(mate);
	long own_unit = royale_hud_living_unit(player_index);
	boolean downed;
	real_point3d position;
	real_point3d camera;
	real distance;

	if (unit_index == NONE)
		return;
	downed = royale_player_state(mate) == ROYALE_PLAYER_DOWNED;
	/* (the multiplayer HUD's teammate triangle; the name over it is hud.c's,
	an ally's: royale_hud_squadmate) */
	royale_hud_draw_mate_triangle(mate, unit_index, downed);
	if (own_unit == NONE)
		return;
	object_get_origin(unit_index, &position);
	render_interpolation_object_point(unit_index, &position);
	render_interpolation_unit_camera_position(own_unit, &camera);
	distance = distance3d(&camera, &position);
	/* far off (out of the tracker's reach), or down anywhere: the waypoint
	arrow with its distance, as a ping's */
	if (downed || distance > hud_globals->defaults.motion_sensor_range)
	{
		real_rgb_color color;
		real_point2d drawn;
		real half_height;
		short drawn_type;
		real_point3d head;

		if (royale_hud_nav_index() == NONE)
			return;
		if (downed)
		{
			real_argb_color red = royale_hud_flash_color(royale_hud_timer_colors(TRUE), TRUE, 0);

			color.red = red.red;
			color.green = red.green;
			color.blue = red.blue;
		}
		else
		{
			real_argb_color text = royale_hud_text_color(1.0f);

			color.red = text.red;
			color.green = text.green;
			color.blue = text.blue;
		}
		position.z += 0.9f;
		unit_get_head_position(own_unit, &head);
		hud_render_nav_point_colored(local_player_index, &position, royale_hud.nav_index,
			hud_get_nav_point_render_type(local_player_index, &head, &position, unit_index), &color, 1.0f, &drawn,
			&half_height, &drawn_type);
	}
}

/* ---- downed, reviving, out: the HUD's state lines, centered under the
crosshair, a bleed-out in the timer's digits */

static void royale_hud_bar(
	rectangle2d const *box,
	real progress,
	real_argb_color const *color)
{
	rectangle2d back = *box;
	rectangle2d fill = *box;
	real_argb_color dim = *color;

	dim.alpha = 0.25f;
	draw_quad(&back, real_argb_color_to_pixel32(&dim));
	fill.x1 = (short)(fill.x0 + (fill.x1 - fill.x0) * PIN(progress, 0.0f, 1.0f));
	if (fill.x1 > fill.x0)
		draw_quad(&fill, real_argb_color_to_pixel32(color));
}

static void royale_hud_player_name(
	long player_index,
	wchar_t *name,
	short count)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	short index;

	for (index = 0; player && index < (short)NUMBEROF(player->name) && index < count - 1 && player->name[index];
		index++)
	{
		name[index] = player->name[index];
	}
	name[index] = 0;
	if (!index && count > 1)
	{
		name[0] = L'-';
		name[1] = 0;
	}
}

static void royale_hud_draw_state(
	short local_player_index,
	long player_index)
{
	rectangle2d window = render.camera.window_bounds;
	long font_index;
	short line;
	short middle_x;
	short top;
	rectangle2d bounds;
	rectangle2d bar;
	wchar_t text[96];
	wchar_t name[16];
	long mate = royale_squadmate_of(player_index);
	real_argb_color color = royale_hud_text_color(1.0f);

	if (!royale_hud_font(&font_index, &line))
		return;
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	middle_x = (short)((window.x0 + window.x1) / 2);
	top = (short)(window.y0 + (window.y1 - window.y0) * 62 / 100);
	bounds.x0 = window.x0;
	bounds.x1 = window.x1;
	bounds.y0 = top;
	bounds.y1 = (short)(top + line);
	bar.x0 = (short)(middle_x - 60);
	bar.x1 = (short)(middle_x + 60);
	bar.y0 = (short)(top + line + 3);
	bar.y1 = (short)(bar.y0 + 3);
	switch (royale_player_state(player_index))
	{
	case ROYALE_PLAYER_DOWNED:
	{
		long left = royale_downed_ticks_left(player_index);
		real progress = royale_revive_progress(player_index);

		if (progress > 0.0f)
		{
			royale_hud_text(&bounds, 2, L"BEING REVIVED", &color);
			royale_hud_bar(&bar, progress, &color);
		}
		else
		{
			/* (the bleed-out as the campaign's countdown: its digits,
			flashing in its time-up colour) */
			short digit_width, digit_height;
			short seconds = (short)MIN(left / TICKS_PER_SECOND + 1, 99);
			real_argb_color red = royale_hud_flash_color(royale_hud_timer_colors(TRUE), FALSE, 0);
			long bleed = royale_bleed_out_ticks();

			royale_hud_digit_size(&digit_width, &digit_height);
			royale_hud_text(&bounds, 2, L"DOWNED  -  HOLD ON FOR A REVIVE", &red);
			royale_hud_number_at(local_player_index, (short)(middle_x + digit_width * ROYALE_HUD_TIMER_SCALE),
				(short)(top + line + 4), seconds, 2, ROYALE_HUD_NUMBER_LEADING_ZEROS,
				royale_hud_timer_colors(seconds <= ROYALE_HUD_BEAM_FLASH_SECONDS), seconds <= ROYALE_HUD_BEAM_FLASH_SECONDS,
				0, ROYALE_HUD_TIMER_SCALE);
			bar.y0 = (short)(bar.y0 + digit_height * ROYALE_HUD_TIMER_SCALE + 6);
			bar.y1 = (short)(bar.y0 + 3);
			royale_hud_bar(&bar, bleed > 0 ? (real)left / (real)bleed : 0.0f, &red);
		}
		break;
	}
	case ROYALE_PLAYER_ALIVE:
		if (royale_player_state(mate) == ROYALE_PLAYER_DOWNED)
		{
			real progress = royale_revive_progress(mate);

			royale_hud_player_name(mate, name, NUMBEROF(name));
			if (progress > 0.0f)
				usnprintf(text, NUMBEROF(text) - 1, L"REVIVING %s", name);
			else
				usnprintf(text, NUMBEROF(text) - 1, L"%s IS DOWN  -  HOLD ACTION BESIDE THEM TO REVIVE", name);
			text[NUMBEROF(text) - 1] = 0;
			royale_hud_text(&bounds, 2, text, &color);
			if (progress > 0.0f)
				royale_hud_bar(&bar, progress, &color);
		}
		break;
	case ROYALE_PLAYER_SPECTATING:
		royale_hud_player_name(mate, name, NUMBEROF(name));
		/* (whom they watch is the game's line: dead_spectate.c) */
		royale_hud_text(&bounds, 2, L"ELIMINATED", &color);
		break;
	case ROYALE_PLAYER_ELIMINATED:
	{
		short squad = royale_squad_of(player_index);

		if (squad != NONE && royale_squad_placement(squad))
			usnprintf(text, NUMBEROF(text) - 1, L"ELIMINATED  -  YOUR SQUAD PLACED #%d", royale_squad_placement(squad));
		else
			usnprintf(text, NUMBEROF(text) - 1, L"ELIMINATED");
		text[NUMBEROF(text) - 1] = 0;
		royale_hud_text(&bounds, 2, text, &color);
		break;
	}
	case ROYALE_PLAYER_NONE:
		if (royale_phase() != ROYALE_PHASE_LOBBY)
			royale_hud_text(&bounds, 2, L"SPECTATING  -  THE NEXT ROYALE STARTS AFTER THIS ONE", &color);
		break;
	default:
		break;
	}
}

/* ---- the beam: its countdown in the timer's digits at the top, and a
warning as the HUD's own (the shield's alarm): flashing, with a waypoint
to safe ground and its distance, while the ground under the player is
glassing or is about to */

static void royale_hud_draw_beam(
	short local_player_index,
	long player_index)
{
	struct royale_beam_hud_state beam;
	long unit_index = royale_hud_living_unit(player_index);
	enum royale_player_state state = royale_player_state(player_index);
	real_point3d origin;
	boolean have_unit = FALSE;
	boolean inside;
	boolean warning;
	long font_index;
	short line;
	short digit_width, digit_height;
	rectangle2d window = render.camera.window_bounds;
	rectangle2d bounds;
	short top_y;
	real_argb_color label;

	if (!royale_hud_font(&font_index, &line))
		return;
	if (unit_index != NONE && (state == ROYALE_PLAYER_ALIVE || state == ROYALE_PLAYER_DOWNED))
	{
		object_get_origin(unit_index, &origin);
		have_unit = TRUE;
	}
	if (!royale_beam_hud_state(have_unit ? &origin : NULL, &beam))
	{
		royale_hud.warning_since[local_player_index] = NONE;
		return;
	}
	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	royale_hud_digit_size(&digit_width, &digit_height);
	inside = have_unit && beam.depth > 0.0f;
	warning = have_unit && (inside || (beam.target_depth > 0.0f && (beam.moving ||
		beam.ticks_to_move < (long)(ROYALE_HUD_BEAM_WARNING_SECONDS * beam.ticks_per_second))));
	if (warning && royale_hud.warning_since[local_player_index] == NONE)
		royale_hud.warning_since[local_player_index] = game_time_get();
	else if (!warning)
		royale_hud.warning_since[local_player_index] = NONE;

	/* the countdown: its label in the countdown's row, M:SS in the timer's
	digits under it (the center anchor's numbers centered on their offset),
	flashing as the campaign's under its cutoff */
	royale_hud_row(ROYALE_HUD_ROW_COUNTDOWN, &bounds.y0, &bounds.y1);
	bounds.x0 = window.x0;
	bounds.x1 = window.x1;
	top_y = (short)(bounds.y1 + 1);
	if (beam.holding)
	{
		long left = (beam.ticks_to_move + (long)beam.ticks_per_second - 1) / (long)MAX(1.0f, beam.ticks_per_second);
		boolean flash = left <= ROYALE_HUD_BEAM_FLASH_SECONDS;
		struct hud_color_definition const *colors = royale_hud_timer_colors(flash);
		short center_x = (short)((window.x0 + window.x1) / 2);
		short digit = (short)(digit_width * ROYALE_HUD_TIMER_SCALE);
		wchar_t colon_text[2] = { L':', 0 };
		real_argb_color colon_color;
		rectangle2d colon;

		label = royale_hud_text_color(0.9f);
		royale_hud_text(&bounds, 2, L"BEAM MOVES IN", &label);
		/* (M:SS centered: the minute left of the colon, the seconds right) */
		royale_hud_number_at(local_player_index, (short)(center_x + digit / 2), top_y, (short)MIN(left / 60, 9), 1,
			ROYALE_HUD_NUMBER_LEADING_ZEROS, colors, flash, 0, ROYALE_HUD_TIMER_SCALE);
		royale_hud_number_at(local_player_index, (short)(center_x + digit / 2 + 2 * digit), top_y, (short)(left % 60),
			2, ROYALE_HUD_NUMBER_LEADING_ZEROS, colors, flash, 0, ROYALE_HUD_TIMER_SCALE);
		colon_color = royale_hud_flash_color(colors, flash, 0);
		colon.x0 = (short)(center_x - 20);
		colon.x1 = (short)(center_x + 20);
		colon.y0 = (short)(top_y + (digit_height * ROYALE_HUD_TIMER_SCALE - line) / 2);
		colon.y1 = (short)(colon.y0 + line);
		royale_hud_text(&colon, 2, colon_text, &colon_color);
	}
	else if (beam.moving)
	{
		label = royale_hud_flash_color(royale_hud_timer_colors(TRUE), TRUE, 0);
		royale_hud_text(&bounds, 2, L"BEAM MOVING", &label);
	}

	/* the warning: flashing as the HUD's alarms do, the way to safety as a
	waypoint with its distance */
	if (warning)
	{
		long since = royale_hud.warning_since[local_player_index];
		real_argb_color alarm = royale_hud_flash_color(royale_hud_timer_colors(TRUE), TRUE, since);
		rectangle2d warning_bounds = bounds;
		real away = (inside ? beam.depth : beam.target_depth) + 3.0f;
		real_point3d safe;
		real_rgb_color arrow;
		real_point2d drawn;
		real half_height;
		short drawn_type;
		real_point3d head;

		warning_bounds.y0 = (short)(top_y + digit_height * ROYALE_HUD_TIMER_SCALE + 4);
		warning_bounds.y1 = (short)(warning_bounds.y0 + line);
		royale_hud_text(&warning_bounds, 2, inside ? L"WARNING: YOU ARE BEING GLASSED" :
			L"WARNING: GLASSING BEAM  -  MOVE", &alarm);
		/* (the way to safe ground in meters, in the digits with their m, as
		a waypoint's: the arrow off the screen shows none) */
		{
			short meters = (short)PIN(away * 3.048f, 0.0f, 999.0f);
			short digit = (short)(digit_width * ROYALE_HUD_COUNT_SCALE);
			short count = (short)(meters >= 100 ? 3 : meters >= 10 ? 2 : 1);

			royale_hud_number_at(local_player_index,
				(short)((window.x0 + window.x1) / 2 + (count + 1) * digit / 2),
				(short)(warning_bounds.y1 + 2), meters, (char)count, ROYALE_HUD_NUMBER_TRAILING_M,
				royale_hud_timer_colors(TRUE), TRUE, since, ROYALE_HUD_COUNT_SCALE);
		}
		if (royale_hud_nav_index() != NONE && unit_index != NONE)
		{
			safe.x = origin.x + beam.safe.i * away;
			safe.y = origin.y + beam.safe.j * away;
			safe.z = origin.z + 0.6f;
			arrow.red = alarm.red;
			arrow.green = alarm.green;
			arrow.blue = alarm.blue;
			unit_get_head_position(unit_index, &head);
			hud_render_nav_point_colored(local_player_index, &safe, royale_hud.nav_index,
				ROYALE_HUD_WAYPOINT_ON_SCREEN, &arrow, 1.0f, &drawn, &half_height, &drawn_type);
		}
	}
}

/* ---- the HUD */

void royale_hud_draw(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);
	enum royale_phase phase = royale_phase();

	if (!royale_active() || player_index == NONE || !hud_globals)
		return;
	if (phase == ROYALE_PHASE_NONE || phase >= ROYALE_PHASE_RESULTS)
		return;
	if (royale_player_state(player_index) == ROYALE_PLAYER_GULAG)
		return;
	royale_hud_draw_mate(local_player_index, player_index);
	royale_hud_draw_state(local_player_index, player_index);
	if (phase >= ROYALE_PHASE_INSERTION)
		royale_hud_draw_counts(local_player_index, player_index);
	royale_hud_draw_beam(local_player_index, player_index);
}
