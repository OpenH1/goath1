/*
ROYALE_CINEMA.C

Royale's presentation moments (royale_cinema.h), drawn from Halo's own
fonts and the director's cameras, as a campaign cutscene would be:

- The flyover: the first seconds of the drop, while the squads ride in from
  past the island's edge (no one can jump yet), a slow letterboxed sweep of
  the island with the match's title and this player's squad.
- The victory: the first seconds of the results, the camera orbiting the
  winning squad, VICTORY for them and DEFEAT with the place for the rest.
- The carnage report: the rest of the results, every squad by its place
  with its players and kills, in the look of Halo's postgame carnage report.
- The Pelicans' bay lit while it carries its squads, so the riders read.

Presentation only: the cameras and the drawing read the synced Royale state
(the phase, its time, the squads, the placements) and change nothing, so
every machine shows the same moment from its own copy of the game.
*/

#include "cseries.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "cutscene/cinematics.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_messaging.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"

#include "royale.h"
#include "royale_cinema.h"

#include <math.h>
#include <string.h>

/* ---------- constants */

/* the flyover's length (it ends sooner if the player leaves the Pelican) */
#define CINEMA_FLYOVER_SECONDS 6.5f
/* the victory orbit's length, then the carnage report */
#define CINEMA_ORBIT_SECONDS 7.0f
/* the letterbox's bars, each a share of the window's height, and their
slide in and out */
#define CINEMA_LETTERBOX 0.12f
#define CINEMA_LETTERBOX_SECONDS 0.5f
/* the orbit: its distance, height over the squad, turn rate */
#define CINEMA_ORBIT_DISTANCE 3.6f
#define CINEMA_ORBIT_HEIGHT 1.3f
#define CINEMA_ORBIT_RATE 0.4f

#define PELICAN_TAG_NAME "vehicles\\pelican\\pelican"


enum cinema_mode
{
	CINEMA_NONE = 0,
	CINEMA_FLYOVER,
	CINEMA_ORBIT,
	CINEMA_REPORT,
	CINEMA_OVERVIEW /* (the camera's only) */
};

/* ---------- structures */

/* the director's controls (director.c's struct camera_control): their start */
struct cinema_controls
{
	short local_player_index;
	boolean active;
	byte pad3;
	real seconds_elapsed;
};

/* the camera's data, in the director's (0x40 bytes) */
struct cinema_camera
{
	short local_player_index;
	short mode;
	real clock; /* seconds into the mode's phase, smoothed between ticks */
	real_point3d focus; /* the orbit's, eased after the squad */
	boolean focus_valid;
	byte pad[3];
	real orbit_start; /* the orbit's first angle */
};

typedef char cinema_camera_size_assert[sizeof(struct cinema_camera) <= 0x40 ? 1 : -1];

/* ---------- globals */

static struct
{
	struct royale_map_config const *config;
	long pelican_definition; /* NONE, or not looked up (-2) */
} cinema = { NULL, -2 };

/* ---------- private code */

static real cinema_phase_seconds(
	void)
{
	return (real)royale_phase_time() / (real)TICKS_PER_SECOND;
}

static long cinema_living_unit(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct object_datum *object;

	if (!player || player->unit_index == NONE ||
		!(object = object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit)) ||
		TEST_FLAG(object->object.damage_flags, _object_dead_bit))
	{
		return NONE;
	}
	return player->unit_index;
}

/* the winning squad's middle (its living units'), FALSE if none stands */
static boolean cinema_winners_focus(
	real_point3d *focus)
{
	short squad = royale_winning_squad();
	short seat;
	short count = 0;
	real_point3d first = { 0 };
	real_point3d last = { 0 };

	if (squad == NONE)
		return FALSE;
	focus->x = focus->y = focus->z = 0.0f;
	for (seat = 0; seat < ROYALE_SQUAD_SIZE; seat++)
	{
		long unit_index = cinema_living_unit(royale_squad_member(squad, seat));
		real_point3d const *center;

		if (unit_index == NONE)
			continue;
		center = &object_get(unit_index)->object.bounding_sphere_center;
		if (!count)
			first = *center;
		last = *center;
		focus->x += center->x;
		focus->y += center->y;
		focus->z += center->z;
		count++;
	}
	if (!count)
		return FALSE;
	/* (two apart: the first of them alone, so the orbit frames someone) */
	if (count == 2 && distance_squared3d(&first, &last) > 6.0f * 6.0f)
	{
		*focus = first;
		return TRUE;
	}
	focus->x /= count;
	focus->y /= count;
	focus->z /= count;
	return TRUE;
}

static boolean cinema_bounds_valid(
	void)
{
	return cinema.config && cinema.config->bounds.maximum.x > cinema.config->bounds.minimum.x &&
		cinema.config->bounds.maximum.y > cinema.config->bounds.minimum.y;
}

static enum cinema_mode cinema_mode_of(
	short local_player_index)
{
	long player_index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS)
		return CINEMA_NONE;
	player_index = local_player_get_player_index(local_player_index);

	if (!royale_active() || player_index == NONE || cinematic_in_progress())
		return CINEMA_NONE;
	switch (royale_phase())
	{
	case ROYALE_PHASE_INSERTION:
		if (cinema_bounds_valid() && cinema_phase_seconds() < CINEMA_FLYOVER_SECONDS &&
			royale_player_state(player_index) == ROYALE_PLAYER_IN_PELICAN)
		{
			return CINEMA_FLYOVER;
		}
		return CINEMA_NONE;
	case ROYALE_PHASE_RESULTS:
	{
		real_point3d focus;

		/* (and once over, until the postgame: the report, in place of the
		game's own scoreboard, game_engine.c) */
		if (!royale_game_over() && cinema_phase_seconds() < CINEMA_ORBIT_SECONDS && cinema_winners_focus(&focus))
			return CINEMA_ORBIT;
		return CINEMA_REPORT;
	}
	default:
		return CINEMA_NONE;
	}
}

static real cinema_smooth(
	real t)
{
	t = PIN(t, 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

/* the flyover's camera at a time: an arc about the island's middle, high
under the drop's height (inside the level), looking down on it */
static void cinema_flyover_camera(
	real seconds,
	real_point3d *position,
	real_vector3d *forward)
{
	struct royale_map_config const *config = cinema.config;
	real t = cinema_smooth(seconds / CINEMA_FLYOVER_SECONDS);
	real center_x = 0.5f * (config->bounds.minimum.x + config->bounds.maximum.x);
	real center_y = 0.5f * (config->bounds.minimum.y + config->bounds.maximum.y);
	real half = 0.5f * MIN(config->bounds.maximum.x - config->bounds.minimum.x,
		config->bounds.maximum.y - config->bounds.minimum.y);
	real ground = config->bounds.minimum.z + 8.0f;
	real top = config->drop_height > ground + 20.0f ? config->drop_height - 10.0f : ground + 40.0f;
	/* (where the arc starts: the match's own, the same on every machine) */
	real start = (real)(royale_match_seed() % 360UL) * (_pi / 180.0f);
	real angle = start + t * (_pi / 3.0f);
	real radius = half * (1.05f - 0.25f * t);
	real_point3d target;

	position->x = center_x + cosf(angle) * radius;
	position->y = center_y + sinf(angle) * radius;
	position->z = top - 10.0f * t;
	/* (the look leads the arc a little) */
	target.x = center_x + cosf(angle + 0.5f) * half * 0.15f;
	target.y = center_y + sinf(angle + 0.5f) * half * 0.15f;
	target.z = ground;
	vector_from_points3d(position, &target, forward);
	if (normalize3d(forward) <= 0.0001f)
		*forward = *global_forward3d;
}

#define CINEMA_ORBIT_COLLISION (FLAG(_collision_test_front_facing_surfaces_bit) | \
	FLAG(_collision_test_structure_bit) | FLAG(_collision_test_objects_bit) | \
	_collision_test_objects_sight_blocking_flags)

/* how far out from the squad the camera may be at an angle, a share of the
orbit's way (1: clear) */
static real cinema_orbit_clear(
	real_point3d const *focus,
	real angle,
	real distance,
	real_vector3d *out)
{
	struct collision_result collision;

	out->i = cosf(angle) * distance;
	out->j = sinf(angle) * distance;
	out->k = CINEMA_ORBIT_HEIGHT;
	if (collision_test_vector(CINEMA_ORBIT_COLLISION, focus, out, NONE, &collision))
		return MAX(collision.t - 0.3f / MAX(magnitude3d(out), 0.01f), 0.15f);
	return 1.0f;
}

/* the orbit's start: of eight ways round, the one most clear of walls,
rocks and trees over the turn ahead */
static real cinema_orbit_start(
	real_point3d const *focus)
{
	real best_angle = 0.0f;
	real best = -1.0f;
	short way;

	for (way = 0; way < 8; way++)
	{
		real angle = way * (_pi / 4.0f);
		real_vector3d out;
		real clear = cinema_orbit_clear(focus, angle, CINEMA_ORBIT_DISTANCE + 0.6f, &out) +
			cinema_orbit_clear(focus, angle + CINEMA_ORBIT_RATE * CINEMA_ORBIT_SECONDS * 0.5f, CINEMA_ORBIT_DISTANCE,
				&out) +
			cinema_orbit_clear(focus, angle + CINEMA_ORBIT_RATE * CINEMA_ORBIT_SECONDS, CINEMA_ORBIT_DISTANCE, &out);

		if (clear > best + 0.01f)
		{
			best = clear;
			best_angle = angle;
		}
	}
	return best_angle;
}

/* the orbit's camera about the squad: out from it, pulled in short of a
wall, a rock or the ground between */
static void cinema_orbit_camera(
	real seconds,
	real start,
	real_point3d const *focus,
	real_point3d *position,
	real_vector3d *forward)
{
	real angle = start + seconds * CINEMA_ORBIT_RATE;
	real distance = CINEMA_ORBIT_DISTANCE + 0.6f * (1.0f - cinema_smooth(seconds / 2.0f));
	real_vector3d out;
	real_point3d look = *focus;
	real t = cinema_orbit_clear(focus, angle, distance, &out);

	point_from_line3d(focus, &out, t, position);
	look.z += 0.2f;
	vector_from_points3d(position, &look, forward);
	if (normalize3d(forward) <= 0.0001f)
		*forward = *global_forward3d;
}

/* whether a player with no unit has someone to watch (dead_spectate.c's
choice): their squadmate, the gulag's fight they or their squadmate wait
for, or once out, anyone still in the fight */
static boolean cinema_anyone_to_watch(
	long self)
{
	long mate = royale_squadmate_of(self);
	enum royale_player_state state = royale_player_state(self);
	struct data_iterator iterator;
	struct player_datum *player;

	if (royale_gulag_spectate_unit(self) != NONE || cinema_living_unit(mate) != NONE ||
		(mate != NONE && royale_gulag_spectate_unit(mate) != NONE))
	{
		return TRUE;
	}
	if (state != ROYALE_PLAYER_ELIMINATED && state != ROYALE_PLAYER_NONE)
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		enum royale_player_state other = royale_player_state(iterator.datum_index);

		if (iterator.datum_index != self && cinema_living_unit(iterator.datum_index) != NONE &&
			(other == ROYALE_PLAYER_ALIVE || other == ROYALE_PLAYER_DOWNED || other == ROYALE_PLAYER_FALLING))
		{
			return TRUE;
		}
	}
	return FALSE;
}

/* the camera's mode: the flyover's, the orbit's, or for a player with no
unit and no one to watch (the game's death camera would hang where the
view last was, or over a body long gone: out of the level, nothing drawn)
the island's overview */
static enum cinema_mode cinema_camera_mode(
	short local_player_index)
{
	enum cinema_mode mode = cinema_mode_of(local_player_index);
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	if (mode == CINEMA_FLYOVER || mode == CINEMA_ORBIT)
		return mode;
	if (!player || !royale_active() || cinematic_in_progress() || !cinema_bounds_valid() ||
		royale_phase() < ROYALE_PHASE_PLAY || player->unit_index != NONE || cinema_anyone_to_watch(player_index))
	{
		return CINEMA_NONE;
	}
	return CINEMA_OVERVIEW;
}

/* the overview: a slow turn high over the island */
static void cinema_overview_camera(
	real seconds,
	real_point3d *position,
	real_vector3d *forward)
{
	struct royale_map_config const *config = cinema.config;
	real center_x = 0.5f * (config->bounds.minimum.x + config->bounds.maximum.x);
	real center_y = 0.5f * (config->bounds.minimum.y + config->bounds.maximum.y);
	real half = 0.5f * MIN(config->bounds.maximum.x - config->bounds.minimum.x,
		config->bounds.maximum.y - config->bounds.minimum.y);
	real ground = config->bounds.minimum.z + 8.0f;
	real top = config->drop_height > ground + 20.0f ? config->drop_height - 12.0f : ground + 38.0f;
	real angle = (real)(royale_match_seed() % 360UL) * (_pi / 180.0f) + seconds * 0.05f;
	real_point3d target;

	position->x = center_x + cosf(angle) * half * 0.85f;
	position->y = center_y + sinf(angle) * half * 0.85f;
	position->z = top;
	target.x = center_x;
	target.y = center_y;
	target.z = ground;
	vector_from_points3d(position, &target, forward);
	if (normalize3d(forward) <= 0.0001f)
		*forward = *global_forward3d;
}

/* ---------- the camera (director.c) */

boolean royale_cinema_camera_wanted(
	short local_player_index)
{
	return cinema_camera_mode(local_player_index) != CINEMA_NONE;
}

void royale_cinema_camera_new(
	void *data,
	short local_player_index)
{
	struct cinema_camera *camera = (struct cinema_camera *)data;

	csmemset(camera, 0, sizeof(*camera));
	camera->local_player_index = local_player_index;
	camera->mode = CINEMA_NONE;
}

void royale_cinema_camera_update(
	void *data,
	void *control_data,
	void *result_data)
{
	struct cinema_camera *camera = (struct cinema_camera *)data;
	struct cinema_controls const *controls = (struct cinema_controls const *)control_data;
	struct observer_command *command = (struct observer_command *)result_data;
	enum cinema_mode mode = cinema_camera_mode(camera->local_player_index);
	/* (the overview's clock the match's, which runs on through the phases) */
	real phase_seconds = mode == CINEMA_OVERVIEW ? (real)royale_match_time() / (real)TICKS_PER_SECOND :
		cinema_phase_seconds();
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	short parameter;

	if (mode == CINEMA_NONE)
		return;
	/* (the clock runs with the frames, kept to the game's ticks) */
	if (camera->mode != mode)
	{
		camera->mode = (short)mode;
		camera->clock = phase_seconds;
		camera->focus_valid = FALSE;
	}
	else
	{
		camera->clock += controls ? controls->seconds_elapsed : 0.0f;
		if (fabsf(camera->clock - phase_seconds) > 0.15f)
			camera->clock = phase_seconds;
	}
	if (mode == CINEMA_FLYOVER)
		cinema_flyover_camera(camera->clock, &position, &forward);
	else if (mode == CINEMA_OVERVIEW)
		cinema_overview_camera(camera->clock, &position, &forward);
	else
	{
		real_point3d focus;
		real seconds = controls ? controls->seconds_elapsed : 0.0f;

		if (!cinema_winners_focus(&focus))
			return;
		if (!camera->focus_valid)
		{
			camera->focus = focus;
			camera->focus_valid = TRUE;
			camera->orbit_start = cinema_orbit_start(&focus);
		}
		else
		{
			/* (eased after the squad as it moves, a steady frame) */
			real fraction = 1.0f - expf(-seconds / 0.4f);

			camera->focus.x += (focus.x - camera->focus.x) * fraction;
			camera->focus.y += (focus.y - camera->focus.y) * fraction;
			camera->focus.z += (focus.z - camera->focus.z) * fraction;
		}
		cinema_orbit_camera(camera->clock, camera->orbit_start, &camera->focus, &position, &forward);
	}
	observer_up_from_forward(&forward, &up);
	csmemset(command, 0, sizeof(*command));
	command->flags = FLAG(_observer_command_valid_bit);
	command->focus_position = position;
	command->focus_offset = *global_zero_vector3d;
	command->focus_distance = 0.0f;
	command->field_of_view = DEGREES_TO_RADIANS(mode == CINEMA_ORBIT ? 55.0f : 62.0f);
	command->forward = forward;
	command->up = up;
	command->focus_velocity = *global_zero_vector3d;
	command->timer = 0.0f;
	/* (each frame where it is: no easing behind) */
	for (parameter = 0; parameter < NUMBER_OF_OBSERVER_COMMAND_PARAMETERS; parameter++)
	{
		command->parameter_flags[parameter] = FLAG(_observer_time_valid_bit) | FLAG(_observer_time_force_bit);
		command->parameter_timers[parameter] = 0.0f;
	}
}

/* ---------- the Pelicans' bay */

void royale_cinema_object_lighting(
	long object_index,
	struct render_lighting *lighting)
{
	struct object_datum *object;
	long vehicle_index;
	real const lamp = 0.55f;

	if (!royale_active() || royale_phase() != ROYALE_PHASE_INSERTION || !lighting)
		return;
	if (cinema.pelican_definition == -2)
		cinema.pelican_definition = tag_loaded('vehi', PELICAN_TAG_NAME);
	if (cinema.pelican_definition == NONE || !(object = object_try_and_get(object_index)))
		return;
	/* (the Pelican, or a rider in it, or what a rider carries) */
	vehicle_index = object_index;
	while (vehicle_index != NONE && object_get(vehicle_index)->definition_index != cinema.pelican_definition)
		vehicle_index = object_get(vehicle_index)->object.parent_object_index;
	if (vehicle_index == NONE)
		return;
	/* the bay's lamps: a cool light filling the shadows, as the level's
	brightest ambient would */
	lighting->ambient_color.red = MAX(lighting->ambient_color.red, lamp * 0.95f);
	lighting->ambient_color.green = MAX(lighting->ambient_color.green, lamp);
	lighting->ambient_color.blue = MAX(lighting->ambient_color.blue, lamp * 1.1f);
	lighting->shadow_color.red = MAX(lighting->shadow_color.red, lamp * 0.6f);
	lighting->shadow_color.green = MAX(lighting->shadow_color.green, lamp * 0.6f);
	lighting->shadow_color.blue = MAX(lighting->shadow_color.blue, lamp * 0.7f);
}

/* ---------- the drawing */

static long cinema_font(
	boolean large)
{
	long font_index = tag_loaded('font', large ? "ui\\large_ui" : "ui\\small_ui");

	return font_index != NONE ? font_index : hud_get_font_index();
}

static rectangle2d cinema_window(
	void)
{
	rectangle2d window = render.camera.window_bounds;

	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	return window;
}

static short cinema_font_height(
	long font_index)
{
	struct font_header *font;

	if (font_index == NONE)
		return 16;
	font = font_definition_get(font_index);
	return (short)MAX(font->ascending_height + font->descending_height + font->leading_height, 8);
}

/* text in bounds (justification 0 left, 1 right, 2 centered), scale times
larger about the bounds' top at its anchor */
static void cinema_text(
	rectangle2d const *bounds,
	long font_index,
	short justification,
	real scale,
	wchar_t const *text,
	real red,
	real green,
	real blue,
	real alpha)
{
	real_argb_color color;
	rectangle2d layout = *bounds;
	real origin_x;

	if (font_index == NONE || alpha <= 0.0f)
		return;
	color.alpha = PIN(alpha, 0.0f, 1.0f);
	color.red = red;
	color.green = green;
	color.blue = blue;
	origin_x = justification == 2 ? 0.5f * (bounds->x0 + bounds->x1) :
		justification == 1 ? (real)bounds->x1 : (real)bounds->x0;
	if (scale != 1.0f)
	{
		/* (laid out in the unscaled units, about the anchor) */
		real half = 0.5f * (bounds->x1 - bounds->x0) / scale;

		if (justification == 2)
		{
			layout.x0 = (short)(origin_x - half);
			layout.x1 = (short)(origin_x + half);
		}
		else if (justification == 1)
			layout.x0 = (short)(origin_x - 2.0f * half);
		else
			layout.x1 = (short)(origin_x + 2.0f * half);
		layout.y1 = (short)(layout.y0 + (bounds->y1 - bounds->y0) / scale);
		rasterizer_text_set_scale(scale, origin_x, (real)bounds->y0);
	}
	draw_string_set_draw_mode(font_index, NONE, justification, 0, &color);
	rasterizer_draw_unicode_string(&layout, NULL, NULL, 0, text);
	if (scale != 1.0f)
		rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static void cinema_quad(
	short x0,
	short y0,
	short x1,
	short y1,
	pixel32 color)
{
	rectangle2d box;

	if (x1 <= x0 || y1 <= y0)
		return;
	box.x0 = x0;
	box.y0 = y0;
	box.x1 = x1;
	box.y1 = y1;
	draw_quad(&box, color);
}

/* a box's outline, a pixel wide */
static void cinema_frame(
	rectangle2d const *box,
	pixel32 color)
{
	cinema_quad(box->x0, box->y0, box->x1, (short)(box->y0 + 1), color);
	cinema_quad(box->x0, (short)(box->y1 - 1), box->x1, box->y1, color);
	cinema_quad(box->x0, box->y0, (short)(box->x0 + 1), box->y1, color);
	cinema_quad((short)(box->x1 - 1), box->y0, box->x1, box->y1, color);
}

static pixel32 cinema_alpha(
	pixel32 color,
	real alpha)
{
	unsigned long a = (unsigned long)((color >> 24) * PIN(alpha, 0.0f, 1.0f));

	return (pixel32)((color & 0x00FFFFFF) | (a << 24));
}

static void cinema_name(
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

static void cinema_squad_names(
	short squad,
	wchar_t *text,
	short count)
{
	wchar_t a[16];
	wchar_t b[16];
	long second = royale_squad_member(squad, 1);

	cinema_name(royale_squad_member(squad, 0), a, NUMBEROF(a));
	if (second == NONE)
		usnprintf(text, count - 1, L"%s", a);
	else
	{
		cinema_name(second, b, NUMBEROF(b));
		usnprintf(text, count - 1, L"%s  &  %s", a, b);
	}
	text[count - 1] = 0;
}

/* the level's name as its players know it */
static wchar_t const *cinema_map_title(
	void)
{
	if (cinema.config && !strcmp(cinema.config->map_name, "b30"))
		return L"THE SILENT CARTOGRAPHER";
	return L"";
}

/* the bars, slid in by amount (0..1) */
static void cinema_letterbox(
	rectangle2d const *window,
	real amount)
{
	short bar = (short)((window->y1 - window->y0) * CINEMA_LETTERBOX * cinema_smooth(amount));

	if (bar <= 0)
		return;
	/* (out past the HUD's safe window, to the screen's edges) */
	cinema_quad((short)(window->x0 - 4000), (short)(window->y0 - 4000), (short)(window->x1 + 4000),
		(short)(window->y0 + bar), 0xFF000000);
	cinema_quad((short)(window->x0 - 4000), (short)(window->y1 - bar), (short)(window->x1 + 4000),
		(short)(window->y1 + 4000), 0xFF000000);
}

static void cinema_draw_flyover(
	long player_index)
{
	rectangle2d window = cinema_window();
	real seconds = cinema_phase_seconds();
	real bars = MIN(seconds, CINEMA_FLYOVER_SECONDS - seconds) / CINEMA_LETTERBOX_SECONDS;
	real fade = PIN((seconds - 0.6f) / 0.8f, 0.0f, 1.0f) * PIN((CINEMA_FLYOVER_SECONDS - 0.4f - seconds) / 0.6f,
		0.0f, 1.0f);
	long large = cinema_font(TRUE);
	long small = cinema_font(FALSE);
	short height = (short)(window.y1 - window.y0);
	short bar = (short)(height * CINEMA_LETTERBOX);
	short line = cinema_font_height(small);
	short squad = royale_squad_of(player_index);
	rectangle2d bounds;
	wchar_t text[96];

	cinema_letterbox(&window, bars);
	/* the title in the upper bar's shadow, the squad in the lower */
	bounds.x0 = window.x0;
	bounds.x1 = window.x1;
	bounds.y0 = (short)(window.y0 + bar + height / 14);
	bounds.y1 = (short)(bounds.y0 + cinema_font_height(large) * 3);
	cinema_text(&bounds, large, 2, 2.4f, L"R O Y A L E", 1.0f, 1.0f, 1.0f, fade);
	bounds.y0 = (short)(bounds.y0 + cinema_font_height(large) * 5 / 2);
	bounds.y1 = (short)(bounds.y0 + line * 2);
	cinema_text(&bounds, small, 2, 1.0f, cinema_map_title(), 0.62f, 0.78f, 1.0f, fade);
	if (squad == NONE)
		return;
	bounds.y1 = (short)(window.y1 - bar - height / 18);
	bounds.y0 = (short)(bounds.y1 - cinema_font_height(large) * 2);
	cinema_squad_names(squad, text, NUMBEROF(text));
	cinema_text(&bounds, large, 2, 1.5f, text, 1.0f, 0.85f, 0.35f, fade);
	bounds.y1 = bounds.y0;
	bounds.y0 = (short)(bounds.y1 - line * 3 / 2);
	usnprintf(text, NUMBEROF(text) - 1, L"YOUR SQUAD  -  %d SQUADS DROP", royale_squad_count());
	text[NUMBEROF(text) - 1] = 0;
	cinema_text(&bounds, small, 2, 1.0f, text, 0.62f, 0.78f, 1.0f, fade);
}

static void cinema_draw_victory(
	long player_index)
{
	rectangle2d window = cinema_window();
	real seconds = cinema_phase_seconds();
	real bars = MIN(seconds, CINEMA_ORBIT_SECONDS - seconds) / CINEMA_LETTERBOX_SECONDS;
	real fade = PIN((seconds - 0.4f) / 0.8f, 0.0f, 1.0f) * PIN((CINEMA_ORBIT_SECONDS - 0.3f - seconds) / 0.6f,
		0.0f, 1.0f);
	long large = cinema_font(TRUE);
	long small = cinema_font(FALSE);
	short height = (short)(window.y1 - window.y0);
	short bar = (short)(height * CINEMA_LETTERBOX);
	short line = cinema_font_height(small);
	short winners = royale_winning_squad();
	short squad = royale_squad_of(player_index);
	boolean won = squad != NONE && squad == winners;
	rectangle2d bounds;
	wchar_t text[96];
	wchar_t names[64];

	cinema_letterbox(&window, bars);
	bounds.x0 = window.x0;
	bounds.x1 = window.x1;
	bounds.y0 = (short)(window.y0 + bar + height / 16);
	bounds.y1 = (short)(bounds.y0 + cinema_font_height(large) * 4);
	if (won)
		cinema_text(&bounds, large, 2, 3.0f, L"V I C T O R Y", 1.0f, 0.85f, 0.35f, fade);
	else if (squad != NONE)
		cinema_text(&bounds, large, 2, 3.0f, L"D E F E A T", 0.85f, 0.25f, 0.2f, fade);
	bounds.y0 = (short)(bounds.y0 + cinema_font_height(large) * 3);
	bounds.y1 = (short)(bounds.y0 + line * 2);
	if (!won && squad != NONE && royale_squad_placement(squad))
	{
		usnprintf(text, NUMBEROF(text) - 1, L"YOUR SQUAD PLACED #%d OF %d", royale_squad_placement(squad),
			royale_squad_count());
		text[NUMBEROF(text) - 1] = 0;
		cinema_text(&bounds, small, 2, 1.0f, text, 0.62f, 0.78f, 1.0f, fade);
	}
	else if (won)
		cinema_text(&bounds, small, 2, 1.0f, L"LAST SQUAD STANDING", 0.62f, 0.78f, 1.0f, fade);
	if (winners == NONE)
		return;
	bounds.y1 = (short)(window.y1 - bar - height / 18);
	bounds.y0 = (short)(bounds.y1 - cinema_font_height(large) * 2);
	cinema_squad_names(winners, names, NUMBEROF(names));
	cinema_text(&bounds, large, 2, 1.5f, names, 1.0f, 0.85f, 0.35f, fade);
	bounds.y1 = bounds.y0;
	bounds.y0 = (short)(bounds.y1 - line * 3 / 2);
	cinema_text(&bounds, small, 2, 1.0f, L"WINNERS", 0.62f, 0.78f, 1.0f, fade);
}

/* the carnage report, as the game's own after a game (game_engine.c's
carnage report): its panel and palette, laid out in its 640 by 480 and
scaled to the view, the game's line, the result's banner,
then a table of the squads by place: their players, each one's kills and
the squad's. This player's squad's row lit, and shown last if below the
rows */
#define CARNAGE_TEXT 0xE6EFFF
#define CARNAGE_TEXT_LIT 0xFFFFFF
#define CARNAGE_INFO 0x9CBBE6
#define CARNAGE_HEADING 0x7F9FCB
#define CARNAGE_BLUE 0x5AA9FF
#define CARNAGE_GOLD 0xFFD45A
#define CARNAGE_TABLE_FILL 0xD8081A40
#define CARNAGE_TABLE_EDGE 0xFF2F7FFF
#define CARNAGE_RULE 0xFF1F4F9A
#define CARNAGE_ROW_RULE 0xFF12305F
#define CARNAGE_ROW_LIT 0xFF0E2A63
#define CARNAGE_ROWS 16

/* the report's 640 by 480 in the view: its scale and its origin */
struct cinema_report
{
	real scale;
	real x;
	real y;
	real alpha;
	short left; /* the panel's sides (report units: wider on a wide view) */
	short right;
};

static short report_x(struct cinema_report const *report, real x) { return (short)(report->x + x * report->scale); }
static short report_y(struct cinema_report const *report, real y) { return (short)(report->y + y * report->scale); }

static void report_quad(
	struct cinema_report const *report,
	real x0,
	real y0,
	real x1,
	real y1,
	unsigned long argb)
{
	cinema_quad(report_x(report, x0), report_y(report, y0), report_x(report, x1), report_y(report, y1),
		cinema_alpha((pixel32)argb, report->alpha));
}

static void report_box(
	struct cinema_report const *report,
	real x0,
	real y0,
	real x1,
	real y1,
	unsigned long fill,
	unsigned long edge)
{
	real unit = 1.0f / MAX(report->scale, 0.01f);

	if (fill >> 24)
		report_quad(report, x0, y0, x1, y1, fill);
	report_quad(report, x0, y0, x1, y0 + unit, edge);
	report_quad(report, x0, y1 - unit, x1, y1, edge);
	report_quad(report, x0, y0, x0 + unit, y1, edge);
	report_quad(report, x1 - unit, y0, x1, y1, edge);
}

/* text in a box of the report's, height units tall, centred on its middle */
static void report_text(
	struct cinema_report const *report,
	real x0,
	real y0,
	real x1,
	real y1,
	wchar_t const *text,
	long font_index,
	short justification,
	real height,
	unsigned long rgb)
{
	struct font_header *font;
	real font_height;
	rectangle2d bounds;
	real top;

	if (font_index == NONE)
		return;
	font = font_definition_get(font_index);
	font_height = (real)MAX(font->ascending_height + font->descending_height, 1);
	top = 0.5f * (y0 + y1) - 0.5f * height;
	bounds.x0 = report_x(report, x0);
	bounds.x1 = report_x(report, x1);
	bounds.y0 = report_y(report, top);
	bounds.y1 = (short)(bounds.y0 + (font->ascending_height + font->descending_height + font->leading_height) * 2 *
		(height * report->scale / font_height));
	cinema_text(&bounds, font_index, justification, height * report->scale / font_height, text,
		((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, report->alpha);
}

static void report_place(
	short place,
	wchar_t *text,
	short count)
{
	usnprintf(text, count - 1, L"%d%s", place,
		place % 100 >= 11 && place % 100 <= 13 ? L"th" : place % 10 == 1 ? L"st" : place % 10 == 2 ? L"nd" :
		place % 10 == 3 ? L"rd" : L"th");
	text[count - 1] = 0;
}

/* a player's kills as the game's own report counts them (its statistics:
the Reclaimer's too), so the two reports agree */
static short cinema_kills(
	long player_index)
{
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;

	return player ? (short)player->statistics.kills[0] : 0;
}

/* a player's Reclaimer (the gulag): WON, LOST, or nothing */
static void cinema_reclaimer(
	struct cinema_report const *report,
	long player_index,
	real x0,
	real y0,
	real x1,
	real y1,
	long font_index,
	real height)
{
	switch (royale_player_reclaimer(player_index))
	{
	case ROYALE_RECLAIMER_WON:
		report_text(report, x0, y0, x1, y1, L"WON", font_index, 1, height, CARNAGE_GOLD);
		break;
	case ROYALE_RECLAIMER_LOST:
		report_text(report, x0, y0, x1, y1, L"LOST", font_index, 1, height, CARNAGE_HEADING);
		break;
	default:
		break;
	}
}

static void cinema_draw_report(
	long player_index)
{
	rectangle2d window = cinema_window();
	real seconds = cinema_phase_seconds() - CINEMA_ORBIT_SECONDS;
	struct cinema_report report;
	long large = cinema_font(TRUE);
	long small = cinema_font(FALSE);
	short squad_count = royale_squad_count();
	short my_squad = royale_squad_of(player_index);
	short winners = royale_winning_squad();
	short my_place = my_squad != NONE ? royale_squad_placement(my_squad) : 0;
	short rows = (short)MIN(squad_count, CARNAGE_ROWS);
	short body_rows = rows;
	short squads_shown[CARNAGE_ROWS + 1];
	short shown = 0;
	real width = (real)(window.x1 - window.x0);
	real height = (real)(window.y1 - window.y0);
	real row_height;
	real text_height;
	real top;
	real column_place, column_a, column_a_kills, column_b, column_b_kills, column_total;
	wchar_t text[160];
	wchar_t name[64];
	short place;
	short index;

	/* (faded in after the orbit; at once when there was none, or once
	over) */
	report.alpha = seconds >= 0.0f && !royale_game_over() ? PIN(seconds / 0.5f, 0.0f, 1.0f) : 1.0f;
	report.scale = MIN(width / 640.0f, height / 480.0f);
	report.x = window.x0 + 0.5f * (width - 640.0f * report.scale);
	report.y = window.y0 + 0.5f * (height - 480.0f * report.scale);
	/* (a wide view: the panel's sides out toward its edges, as the game's) */
	report.left = 24;
	report.right = 616;
	if (width / report.scale > 640.0f)
	{
		real extra = MIN(0.5f * (width / report.scale - 640.0f), 0.5f * (640.0f * 16.0f / 9.0f / (4.0f / 3.0f) - 640.0f));

		report.left = (short)(24 - extra);
		report.right = (short)(616 + extra);
	}
	/* the world dimmed, then the panel */
	cinema_quad((short)(window.x0 - 4000), (short)(window.y0 - 4000), (short)(window.x1 + 4000),
		(short)(window.y1 + 4000), cinema_alpha(0xC0000000, report.alpha));
	/* the panel: its rules above and below, the title's band over the top
	one (the game's report's look, in its palette) */
	report_quad(&report, report.left - 24, 40, report.right + 24, 64, 0x60081A40);
	report_quad(&report, report.left - 24, 64, report.right + 24, 410, 0xE0061433);
	report_quad(&report, report.left - 24, 63, report.right + 24, 64.5f, CARNAGE_TABLE_EDGE);
	report_quad(&report, report.left - 24, 409.5f, report.right + 24, 411, CARNAGE_TABLE_EDGE);
	report_text(&report, report.left, 34, report.right, 62, L"ROYALE CARNAGE REPORT", large, 2, 24.0f,
		CARNAGE_BLUE);
	/* the game's line */
	{
		long match_seconds = royale_match_time() / TICKS_PER_SECOND;

		usnprintf(text, NUMBEROF(text) - 1, L"ROYALE   \x00B7   %s   \x00B7   %ld:%02ld   \x00B7   %d SQUADS",
			cinema_map_title(), match_seconds / 60, match_seconds % 60, squad_count);
		text[NUMBEROF(text) - 1] = 0;
		report_text(&report, report.left, 72, report.right, 88, text, small, 0, 10.0f, CARNAGE_INFO);
	}
	/* the banner: the winners, and this player's place */
	if (winners != NONE && winners == my_squad)
	{
		report_box(&report, report.left, 92, report.right, 122, 0xF0332608, 0xFF000000 | CARNAGE_GOLD);
		report_text(&report, report.left + 12, 92, report.right - 12, 122, L"VICTORY", small, 0, 15.0f, CARNAGE_GOLD);
	}
	else
	{
		report_box(&report, report.left, 92, report.right, 122, 0xF00A2456, 0xFF5AA9FF);
		if (winners != NONE)
		{
			cinema_squad_names(winners, name, NUMBEROF(name));
			usnprintf(text, NUMBEROF(text) - 1, L"%s WIN", name);
		}
		else
			usnprintf(text, NUMBEROF(text) - 1, L"NO SQUAD LEFT");
		text[NUMBEROF(text) - 1] = 0;
		report_text(&report, report.left + 12, 92, report.right - 12, 122, text, small, 0, 15.0f, CARNAGE_BLUE);
	}
	if (my_place)
	{
		wchar_t ordinal[16];

		report_place(my_place, ordinal, NUMBEROF(ordinal));
		usnprintf(text, NUMBEROF(text) - 1, L"%s OF %d", ordinal, squad_count);
		text[NUMBEROF(text) - 1] = 0;
		report_text(&report, report.left + 12, 92, report.right - 12, 122, text, large, 1, 20.0f, CARNAGE_TEXT_LIT);
	}
	/* the squads shown: the first by place, and this player's if below */
	for (place = 1; place <= squad_count && shown < rows; place++)
	{
		short squad;

		for (squad = 0; squad < squad_count && shown < rows; squad++)
		{
			if (royale_squad_placement(squad) == place)
				squads_shown[shown++] = squad;
		}
	}
	if (my_squad != NONE && my_place)
	{
		for (index = 0; index < shown && squads_shown[index] != my_squad; index++)
			;
		if (index == shown)
		{
			if (shown == CARNAGE_ROWS)
				shown--;
			squads_shown[shown++] = my_squad;
		}
	}
	body_rows = shown;
	/* the table */
	row_height = MIN(30.0f, (400.0f - 128.0f - 8.0f - 18.0f) / MAX(body_rows, 1));
	text_height = MIN(11.0f, 0.7f * row_height);
	report_box(&report, report.left, 128, report.right,
		MIN(400.0f, 128.0f + 8.0f + 18.0f + body_rows * row_height), CARNAGE_TABLE_FILL, CARNAGE_TABLE_EDGE);
	{
		real inner = (real)(report.right - report.left - 20);
		real x = (real)report.left + 10;

		column_place = x;
		column_a = x + 42.0f;
		column_total = x + inner;
		column_b_kills = column_total - 90.0f;
		column_b = column_a + 0.5f * (column_b_kills - 54.0f - column_a) + 27.0f;
		column_a_kills = column_b - 20.0f;
	}
	top = 132.0f;
	report_text(&report, column_place, top, column_a, top + 18, L"#", small, 0, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_a, top, column_a_kills, top + 18, L"PLAYER", small, 0, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_a, top, column_a_kills, top + 18, L"KILLS", small, 1, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_a, top, column_a_kills - 44, top + 18, L"RECLAIMER", small, 1, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_b, top, column_b_kills - 44, top + 18, L"RECLAIMER", small, 1, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_b, top, column_b_kills, top + 18, L"PLAYER", small, 0, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_b, top, column_b_kills, top + 18, L"KILLS", small, 1, 8.0f, CARNAGE_HEADING);
	report_text(&report, column_b_kills, top, column_total, top + 18, L"SQUAD KILLS", small, 1, 8.0f, CARNAGE_HEADING);
	top += 18.0f;
	report_quad(&report, report.left + 10, top - 1, report.right - 10, top, CARNAGE_RULE);
	for (index = 0; index < body_rows; index++)
	{
		short squad = squads_shown[index];
		boolean mine = squad == my_squad;
		unsigned long rgb = mine ? CARNAGE_TEXT_LIT : CARNAGE_TEXT;
		long a = royale_squad_member(squad, 0);
		long b = royale_squad_member(squad, 1);
		short kills_a = cinema_kills(a);
		short kills_b = cinema_kills(b);

		if (mine)
			report_box(&report, column_place - 6, top + 1, column_total + 6, top + row_height - 1, CARNAGE_ROW_LIT,
				squad == winners ? 0xFF000000 | CARNAGE_GOLD : CARNAGE_TABLE_EDGE);
		report_place(royale_squad_placement(squad), text, NUMBEROF(text));
		report_text(&report, column_place, top, column_a, top + row_height, text, small, 0, text_height,
			squad == winners ? CARNAGE_GOLD : mine ? CARNAGE_TEXT_LIT : CARNAGE_INFO);
		cinema_name(a, name, 16);
		report_text(&report, column_a, top, column_a_kills, top + row_height, name, small, 0, text_height, rgb);
		cinema_reclaimer(&report, a, column_a, top, column_a_kills - 44, top + row_height, small, text_height);
		usnprintf(text, NUMBEROF(text) - 1, L"%d", kills_a);
		text[NUMBEROF(text) - 1] = 0;
		report_text(&report, column_a, top, column_a_kills, top + row_height, text, small, 1, text_height, rgb);
		if (b != NONE)
		{
			cinema_name(b, name, 16);
			report_text(&report, column_b, top, column_b_kills, top + row_height, name, small, 0, text_height, rgb);
			cinema_reclaimer(&report, b, column_b, top, column_b_kills - 44, top + row_height, small, text_height);
			usnprintf(text, NUMBEROF(text) - 1, L"%d", kills_b);
			text[NUMBEROF(text) - 1] = 0;
			report_text(&report, column_b, top, column_b_kills, top + row_height, text, small, 1, text_height, rgb);
		}
		usnprintf(text, NUMBEROF(text) - 1, L"%d", kills_a + kills_b);
		text[NUMBEROF(text) - 1] = 0;
		report_text(&report, column_b_kills, top, column_total, top + row_height, text, small, 1, text_height,
			CARNAGE_TEXT_LIT);
		top += row_height;
		if (index + 1 < body_rows)
			report_quad(&report, report.left + 10, top - 1, report.right - 10, top, CARNAGE_ROW_RULE);
	}
}

/* ---------- public code */

void royale_cinema_start(
	struct royale_map_config const *config)
{
	cinema.config = config;
	cinema.pelican_definition = -2;
}

void royale_cinema_reset(
	void)
{
	cinema.config = NULL;
	cinema.pelican_definition = -2;
}

void royale_cinema_tick(
	void)
{
}

short royale_cinema_hud_perspective(
	short local_player_index,
	short perspective)
{
	return royale_cinema_owns_screen(local_player_index) ? (short)_director_perspective_neutral : perspective;
}

boolean royale_cinema_owns_screen(
	short local_player_index)
{
	return cinema_mode_of(local_player_index) != CINEMA_NONE;
}

void royale_cinema_draw(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);

	switch (cinema_mode_of(local_player_index))
	{
	case CINEMA_FLYOVER:
		cinema_draw_flyover(player_index);
		break;
	case CINEMA_ORBIT:
		cinema_draw_victory(player_index);
		break;
	case CINEMA_REPORT:
		cinema_draw_report(player_index);
		break;
	default:
		break;
	}
}
