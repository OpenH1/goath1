/*
NETWORK_MARKERS.C

Pings: a player marks what they aim at for their allies, with Halo's own
nav point (the campaign's arrow and its distance in meters, hud_nav_points.c's
custom_render_nav_point), in the player's colour with their name above it,
seen through walls (the arrow's occluded look behind them), for
MARKER_SHOWN_MILLISECONDS and then faded out. A player has one ping at a
time: a new one takes the place of the last, and a ping of the same spot or
thing again takes it away. The press is controls.ping (the middle mouse
button) or, on a controller, PING in its Button Remapping, which with the
stock layouts is D-pad Right (xinput_sdl.c then keeps that direction's
strafe from the game while pings can be sent).

What the ping is on: the first of the units, vehicles, weapons and equipment
the aim passes near (within MARKER_PICK_TANGENT of the line, a small item's
reach as one of MARKER_PICK_SMALLEST_RADIUS, before what it hits or lying
just past it: network_markers_pick), followed as it moves and drawn over it;
else the point the aim hits (the level, scenery), within MARKER_RANGE. A
weapon in a unit's hands marks the unit; a dead biped's body is passed (to
the weapon lying on it: markers_object_pingable).

Claims: a ping of a weapon or equipment another player's ping (or claim) is
on, which the pinger sees, is a claim of it ("I'm taking it": the host's
network_markers_decided_kind, sent as _marker_kind_claim). The claimed ping
is drawn once, its icon or outline in the claimer's colour, with "Claimed by
<name>" in it over the pinger's name, and shown its whole time again from
the claim; a claim whose ping is gone is drawn as the claimer's own, with
"Claimed" over their name. The claimer's ping of it again takes the claim
away, as any ping's does.

What it is drawn as, beside the arrow (network_markers_shape, chosen on
each machine from the pinged object: nothing of it is sent): a weapon or
equipment its pickup prompt's icon (markers_object_icon) in the pinger's
colour over the arrow, under the name; one with no icon (a health pack, a
powerup: their pickup prompts are text alone), and a unit or vehicle, its
outline in the pinger's colour (network_markers_render_outlines, before the
HUD: the nearest MARKER_OUTLINES_DRAWN of a view), through walls, except an
enemy player's
(on foot or riding) in a game against players, which shows only where it is
seen. An enemy unit or vehicle (markers_object_enemy) has a small red arrow
over the name and a red inner edge to its outline (orange for a reddish
pinger's: markers_enemy_color).

Who sees it: in the campaign (co-op, split screen) every player; in a
multiplayer game with teams, the player's team; in a game without teams,
nobody (pings are not sent), nor in a game of one player alone on this
machine (markers_available). A network game's are OpenH1's own, in an OpenH1
game alone (network_standard_agreed_rate: none in a Legacy game, nor to an
OpenCE machine of a mixed one, network_standard_message_goes_to): a client
sends its player's ping to the host (_distributed_message_marker_up,
reliably), which takes it only over the machine's own stream, from one of
that machine's players with a living unit, at most MARKER_BURST at once and
one each MARKER_REFILL_MILLISECONDS after, a point inside the world and no
further than MARKER_HOST_RANGE from the player, an object that is there and
of the kinds above (else the point). The host decides whether it clears the
player's last, and sends it (or the clearing) on to every OpenH1 machine
with a player who sees it (_distributed_message_marker_down, reliably),
which shows it as the host sent it. The host's network.pings (on by default:
Server Setup's PINGS) goes to its clients with it each MARKER_CONFIG_MILLISECONDS
and as it changes; a client without it (a host before pings) sends none.
Each machine plays the UI's cursor sound as a ping its players see comes.

Mutes, as the text chat's and the voice's: a player this machine muted (the
scoreboard's Mute voice, chat and pings: network_voice_machine_muted) has
their pings neither drawn nor heard here, which no one else is told
(markers_shown_to; the host still sends them). The host's mute for everyone
(the chat's /mute, the scoreboard's Mute for everyone, Host Controls' MUTE:
network_chat_host_mute_minutes) refuses the muted player's pings, as it
relays none of their voice and takes none of their lines: no machine shows
them, their own neither.

Nothing of it is the simulation's: the pings are kept by the machine's
clock, not the game's ticks, and the recorded and replayed game is the same
without them. The pure parts (network_markers_point_valid,
network_markers_same_spot, markers_object_drawn_point) are
tools/harness/tests/test_markers.py's; the name over the arrow
(markers_draw_name) and the shape (network_markers_shape) are
test_hud_marks.py's.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "memory/data.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "camera/observer.h"
#include "camera/first_person_camera.h"
#include "camera/static_camera.h"
#include "cutscene/cinematics.h"
#include "interface/hud.h"
#include "interface/hud_draw.h"
#include "interface/hud_messaging.h"
#include "interface/ui_widget.h"
#include "main/console.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "physics/collision_usage.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "saved games/player_profile.h"
#include "sound/game_sound.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"
#include "tag_files/tag_files.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "units/units.h"
#include "controller_remap.h"
#include "halo_keyboard.h"
#include "network_distributed.h"
#include "network_standard.h"
#include "network_coop.h"
#include "network_chat.h"
#include "network_voice.h"
#include "network_markers.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmaps.h"
#include "cache/texture_cache.h"
#include "interface/hud_definitions.h"
#include "interface/weapon_hud_interface_definition.h"
#include "items/equipment_definitions.h"
#include "items/weapon_definitions.h"
#include "game/game_allegiance.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "draw_bounds.h"
#include "mcc_cache.h"

/* the platform layer's */
unsigned long system_milliseconds(void);
void platform_log(char const *format, ...);
/* port_config.c's */
int config_boolean(const char *name);
unsigned long config_changes(void);
/* network_connection.c's */
boolean network_connection_last_read_was_unreliable(void);
/* xinput_sdl.c's: the stock layout's ping (D-pad Right) pressed on the
port's controller since it was last asked */
int halo_ping_pad_pressed(short port);
/* hud_nav_points.c's: an arrow by its name (NULL: the first), NONE for none */
short hud_find_nav_point_quietly(char const *name);
boolean hud_render_nav_point_colored(short local_player_index, real_point3d const *position, short nav_index,
	short waypoint_type, real_rgb_color const *color, real alpha, real_point2d *drawn_position, real *half_height,
	short *drawn_type);
short hud_get_nav_point_render_type(short local_player_index, real_point3d const *head, real_point3d const *position,
	long reference_object_index);
boolean hud_render_nav_point_arrow(short nav_index, short waypoint_type, real_point2d const *center, real height,
	real_rgb_color const *color, real alpha);
/* hud_messaging.c's, hud_hires_tags.c's, hud_weapon.c's: the pickup prompt's
icons (the HUD globals' message icons), a sprite's high-res stand-in, and a
grenade type's icon */
struct bitmap_data const *hud_hires_sprite_bitmap(struct bitmap_data const *bitmap, short sequence_index);
struct icon_hud_element_definition const *hud_grenade_messaging_icon(short grenade_type);
/* render_objects.c's and rasterizer_xbox_shadows.c's: an object's outline */
void render_object_outline(long object_index);
boolean rasterizer_ping_outline_begin(void);
void rasterizer_ping_outline_pass(boolean mask, real offset_x, real offset_y, pixel32 color, boolean depth_tested);
void rasterizer_ping_outline_clear(void);
void rasterizer_ping_outline_end(void);

/* ---------- constants */

enum
{
	/* the message's format: a machine takes its own alone */
	MARKER_FORMAT = 1,
	/* a ping shown whole, then faded out */
	MARKER_SHOWN_MILLISECONDS = 8000,
	MARKER_FADE_MILLISECONDS = 1500,
	/* the pings a player sends at once, and the time they gain another in
	(the host's count, and the player's own machine's) */
	MARKER_BURST = 3,
	MARKER_REFILL_MILLISECONDS = 1200,
	/* the host: its settings sent again this often (to a machine that
	joined since) */
	MARKER_CONFIG_MILLISECONDS = 3000,
	/* the host: the refusals it logs in a game */
	MARKER_REFUSALS_LOGGED = 32,
	MAXIMUM_MARKER_PLAYERS = MAXIMUM_TRACKED_PLAYERS,
	MAXIMUM_MARKER_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES,
	/* a ping's object's kinds */
	MARKER_OBJECT_MASK = _object_mask_unit | FLAG(_object_type_weapon) | FLAG(_object_type_equipment),
	/* the aim's line: the level, its media skipped, invisible surfaces
	skipped, and those objects and scenery and machines */
	MARKER_AIM_FLAGS = FLAG(_collision_test_front_facing_surfaces_bit) |
		FLAG(_collision_test_ignore_invisible_surfaces_bit) | FLAG(_collision_test_structure_bit) |
		FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_bipeds_bit) |
		FLAG(_collision_test_objects_vehicles_bit) | FLAG(_collision_test_objects_weapons_bit) |
		FLAG(_collision_test_objects_equipment_bit) | FLAG(_collision_test_objects_scenery_bit) |
		FLAG(_collision_test_objects_machines_bit),
};

/* what a ping of an object is drawn as, with its arrow, distance and name
(network_markers_shape) */
enum
{
	/* the arrow alone */
	_marker_shape_arrow,
	/* and a weapon's or equipment's icon (the pickup prompt's) */
	_marker_shape_icon,
	/* and the outline of a unit or vehicle */
	_marker_shape_outline,
	NUMBER_OF_MARKER_SHAPES
};

/* the message's kinds */
enum
{
	/* a ping of a point, and of an object (followed) */
	_marker_kind_point,
	_marker_kind_object,
	/* (down) the player's ping taken away */
	_marker_kind_clear,
	/* (down) the host's settings: flags, _marker_config_* */
	_marker_kind_config,
	/* (down) a ping of an item another's ping is on: the player claims it
	(a machine before claims takes none: NUMBER_OF_MARKER_KINDS was 4) */
	_marker_kind_claim,
	NUMBER_OF_MARKER_KINDS
};

/* the host's settings' flags */
enum
{
	/* its players may ping */
	_marker_config_on_bit,
};

/* how far the aim reaches (world units: a little under 1000 m, what the
arrow's distance shows) */
#define MARKER_RANGE 320.0f
/* the host: how far from the player's unit a ping may be */
#define MARKER_HOST_RANGE 400.0f
/* the world's bounds, each way (a point past them is not a level's) */
#define MARKER_WORLD_LIMIT 20000.0f
/* a ping this near the player's last (world units: about 5 m), or of its
object, takes it away */
#define MARKER_SAME_SPOT 1.6f
/* an object the aim passes this near (a tangent: about 2 degrees), and its
bounding sphere, is what is aimed at */
#define MARKER_PICK_TANGENT 0.035f
/* ... an object smaller than this (world units: about 0.6 m across, a
health pack's, a grenade's) reached as one of it */
#define MARKER_PICK_SMALLEST_RADIUS 0.1f
/* the arrow's middle (it is drawn about it) over a point (world units: 0.3
m), just over a biped's head, and over an object's middle (a part of its
bounding sphere's radius, which a vehicle's is well above its roof) */
#define MARKER_POINT_HEIGHT 0.1f
#define MARKER_HEAD_HEIGHT 0.1f
#define MARKER_OBJECT_HEIGHT 0.1f
#define MARKER_OBJECT_RADIUS_PART 0.5f
/* a pinged item's icon (the pickup prompt's): its height and widest, parts
of its arrow's height, over the arrow by a gap (the HUD's units) */
#define MARKER_ICON_HEIGHT_PART 0.55f
#define MARKER_ICON_WIDEST_PART 2.6f
#define MARKER_ICON_GAP 2.0f
/* an enemy's cue: a small red arrow over the name, this part of the arrow's
height, and a red inner edge of its outline (this part of the outline's
width) */
#define MARKER_ENEMY_CUE_PART 0.45f
#define MARKER_ENEMY_EDGE_PART 0.4f
/* an outline's width (the view's -1..1 units, of its height: about 4 of a
1080-line display's pixels, whatever the object's size or distance, so a
small or far one reads), its soft glow's (this many times as wide, this
part as opaque), and the most a view draws (the nearest) */
#define MARKER_OUTLINE_WIDTH 0.0075f
#define MARKER_GLOW_WIDTH_PART 2.0f
#define MARKER_GLOW_ALPHA_PART 0.35f
#define MARKER_OUTLINES_DRAWN 8

/* ---------- structures */

struct marker_message
{
	byte format;
	/* the player (absolute index) */
	byte player_index;
	byte kind;
	/* (config: the host's flags) */
	byte flags;
	/* the object (datum index), NONE for a point */
	long object_index;
	/* the point (an object's: where it was) */
	real point[3];
};

typedef char marker_message_size_assert[sizeof(struct marker_message) == 20 ? 1 : -1];

struct marker_wire
{
	struct distributed_message_header header;
	struct marker_message marker;
};

struct marker
{
	boolean active;
	byte kind;
	long object_index;
	real_point3d point;
	unsigned long shown_at;
};

struct marker_rate
{
	short tokens;
	unsigned long refilled_at;
	boolean known;
};

/* ---------- globals */

static struct
{
	/* in a game (its pings let go as the next starts), and the game time
	seen last (a new game's starts lower) */
	boolean in_game;
	long last_game_time;
	struct marker markers[MAXIMUM_MARKER_PLAYERS];
	/* the host's: each player's count; a client's: its own players' */
	struct marker_rate rates[MAXIMUM_MARKER_PLAYERS];
	/* a client: the host's settings, as it sent them */
	boolean config_known;
	byte config_flags;
	/* the host: its settings, as it last sent them */
	boolean config_sent_known;
	byte config_sent_flags;
	unsigned long config_sent_at;
	/* the keyboard's ping held (an edge presses) */
	boolean key_held;
	/* the arrow and the sound, as this game's map has them */
	boolean looked;
	short nav_index;
	long sound_index;
	/* a message of another format noted (once) */
	boolean other_format_noted;
	/* (a test's burst) the client's own count not kept */
	boolean unchecked;
	/* the host: the refusals logged this game */
	short refusals_logged;
} markers_globals;

/* ---------- the pure parts */

boolean network_markers_point_valid(
	real x,
	real y,
	real z)
{
	/* (a NaN fails each) */
	return x > -MARKER_WORLD_LIMIT && x < MARKER_WORLD_LIMIT &&
		y > -MARKER_WORLD_LIMIT && y < MARKER_WORLD_LIMIT &&
		z > -MARKER_WORLD_LIMIT && z < MARKER_WORLD_LIMIT;
}

boolean network_markers_same_spot(
	long object_index,
	real const *point,
	long last_object_index,
	real const *last_point)
{
	real dx = point[0] - last_point[0];
	real dy = point[1] - last_point[1];
	real dz = point[2] - last_point[2];

	if (object_index != NONE || last_object_index != NONE)
		return object_index == last_object_index;
	return dx * dx + dy * dy + dz * dz <= MARKER_SAME_SPOT * MARKER_SAME_SPOT;
}

/* whether the aim reaches an object (its bounding sphere's middle along
the line from the eye, across from it, and its radius) the aim's line hit
something at hit_distance short of or a little past: a small one as one of
MARKER_PICK_SMALLEST_RADIUS; one lying on what the line hit (past it by no
more than the reach across), as an item on the floor short of where the aim
lands is; how near the line, as the reach measures it (0 on it, 1 at its
edge) */
boolean network_markers_pick(
	real along,
	real across,
	real radius,
	real hit_distance,
	real *nearness)
{
	real reach_across = MAX(radius, MARKER_PICK_SMALLEST_RADIUS) + along * MARKER_PICK_TANGENT;

	if (!(along > 0.0f) || along - radius > hit_distance + reach_across || !(across <= reach_across))
		return FALSE;
	*nearness = across / reach_across;
	return TRUE;
}

/* (the host) what a player's ping is (a point's or an object's, kind), as
it decides: their last taken away, again (same_as_last); a claim, of an item
another's ping or claim the pinger sees is on; else as it was */
byte network_markers_decided_kind(
	byte kind,
	boolean same_as_last,
	boolean item,
	boolean pinged_by_another)
{
	if (same_as_last)
		return _marker_kind_clear;
	if (kind == _marker_kind_object && item && pinged_by_another)
		return _marker_kind_claim;
	return kind;
}

/* ---------- the game */

static boolean markers_network_game(
	void)
{
	short connection = game_connection();

	return connection == _game_connection_network_client || connection == _game_connection_network_server;
}

/* (the host, or a game of this machine alone) its setting */
static byte markers_own_config_flags(
	void)
{
	static byte flags = 0;
	static unsigned long read_at = (unsigned long)-1;

	if (read_at != config_changes())
	{
		read_at = config_changes();
		flags = config_boolean("network.pings") ? (byte)FLAG(_marker_config_on_bit) : 0;
	}
	return flags;
}

/* whether pings are played in this game: by this machine's setting, or its
host's, in a game with allies (the campaign, a game with teams), an OpenH1
one when networked, split screen's on one machine */
static boolean markers_available(
	void)
{
	/* (in_game first: the pads are read before the game's time is made) */
	if (!markers_globals.in_game || !game_in_progress())
		return FALSE;
	if (game_engine_running() && !game_engine_has_teams())
		return FALSE;
	switch (game_connection())
	{
	case _game_connection_network_server:
		return network_standard_agreed_rate() != 0 && TEST_FLAG(markers_own_config_flags(), _marker_config_on_bit);
	case _game_connection_network_client:
		return network_standard_agreed_rate() != 0 && markers_globals.config_known &&
			TEST_FLAG(markers_globals.config_flags, _marker_config_on_bit);
	default:
		/* (a game of this machine alone: with players to ping for, split
		screen's; a player alone keeps the D-pad's strafe) */
		return local_player_count() > 1 && TEST_FLAG(markers_own_config_flags(), _marker_config_on_bit);
	}
}

/* whether the viewer sees the pinger's pings: their own; every player in
the campaign; a team's in a game with teams */
static boolean markers_sees(
	struct player_datum const *pinger,
	struct player_datum const *viewer)
{
	if (!pinger || !viewer)
		return FALSE;
	if (pinger == viewer)
		return TRUE;
	if (!game_engine_running())
		return TRUE;
	return game_engine_has_teams() && pinger->team_index == viewer->team_index;
}

/* whether this machine muted the pinger's machine (the scoreboard's mute,
network_voice.c, which mutes their voice and text chat too), in a network
game */
static boolean markers_muted_here(
	struct player_datum const *pinger)
{
	return pinger && markers_network_game() && network_voice_machine_muted(pinger->network_player_data.machine_index);
}

/* whether the viewer, on this machine, is shown the pinger's pings: those
markers_sees gives them, but none of a player this machine muted (their own
always) */
static boolean markers_shown_to(
	struct player_datum const *pinger,
	struct player_datum const *viewer)
{
	return markers_sees(pinger, viewer) && (pinger == viewer || !markers_muted_here(pinger));
}

/* a player's colour: their team's in a game with teams, else their own
(the profile's colour, as a free-for-all game draws their armor) */
static void markers_player_color(
	long player_index,
	real_rgb_color *color)
{
	struct player_datum *player = player_get(player_index);

	if (game_engine_running() && game_engine_has_teams())
		game_engine_player_get_change_color(color, player_index);
	else
		*color = player_profile_get_rgb_color(player->network_player_data.primary_color_index);
}

/* where a ping of an object is drawn this frame: over it as the frame draws
it between the ticks (render_interpolation.c), over a biped's head, over
another's middle by its size; at, its middle as the last tick left it (where
the ping stays once the object is gone). A biped's head is its head marker's,
which is read from the pose drawn already: moved with the object again, as
the middle is, it went a tick's motion further, up and down a step a tick
as the biped rose or fell */
static void markers_object_drawn_point(
	long object_index,
	real_point3d *at,
	real_point3d *position)
{
	real radius;

	object_get_bounding_sphere(object_index, at, &radius);
	*position = *at;
	render_interpolation_object_point(object_index, position);
	if (object_try_and_get_and_verify_type(object_index, _object_mask_biped))
	{
		real_point3d head_position;

		unit_get_head_position(object_index, &head_position);
		position->z = head_position.z + MARKER_HEAD_HEIGHT;
	}
	else
	{
		position->z += radius * MARKER_OBJECT_RADIUS_PART + MARKER_OBJECT_HEIGHT;
	}
}

/* whether a ping's kind is of an object (a claim is too) */
static boolean markers_kind_of_object(
	byte kind)
{
	return kind == _marker_kind_object || kind == _marker_kind_claim;
}

/* where a ping's arrow is drawn this frame: over its object as drawn,
while it is there (its object given back); then where it was (NONE) */
static long markers_drawn_position(
	struct marker *marker,
	real_point3d *position)
{
	if (markers_kind_of_object(marker->kind) && object_try_and_get_and_verify_type(marker->object_index,
		MARKER_OBJECT_MASK))
	{
		markers_object_drawn_point(marker->object_index, &marker->point, position);
		return marker->object_index;
	}
	if (markers_kind_of_object(marker->kind))
	{
		marker->kind = _marker_kind_point;
		marker->object_index = NONE;
	}
	*position = marker->point;
	position->z += MARKER_POINT_HEIGHT;
	return NONE;
}

/* a ping's fade at an age: whole while shown, then faded out */
static real markers_alpha(
	unsigned long age)
{
	return age < MARKER_SHOWN_MILLISECONDS ? 1.0f :
		1.0f - (real)(age - MARKER_SHOWN_MILLISECONDS) / (real)MARKER_FADE_MILLISECONDS;
}

/* the pinger's name over their arrow (drawn at drawn, half_height tall
each way), where the arrow is, not the nearest whole unit of the screen (3
to 5 of a display's pixels) short of it (hud_draw_mark_begin), as a
player's name is: in whole units it stepped against the arrow and the
world under it as the view turned or moved */
static void markers_draw_name(
	long font_index,
	wchar_t const *name,
	real_rgb_color const *color,
	real alpha,
	real_point2d const *drawn,
	real half_height)
{
	struct font_header *font = font_definition_get(font_index);
	real_argb_color text_color;
	rectangle2d bounds;
	point2d whole;

	hud_draw_mark_begin(drawn->x, drawn->y - half_height - 2.0f, &whole);
	bounds.x0 = (short)(whole.x - 160);
	bounds.x1 = (short)(whole.x + 160);
	bounds.y1 = whole.y;
	bounds.y0 = (short)(whole.y - (font->ascending_height + font->descending_height));
	text_color.alpha = alpha;
	text_color.red = color->red;
	text_color.green = color->green;
	text_color.blue = color->blue;
	draw_string_set_draw_mode(font_index, NONE, 2 /* centred */, 0, &text_color);
	rasterizer_text_set_scale(0.75f, (real)whole.x, (real)whole.y);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, name);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
	hud_draw_mark_end();
}

/* a claim's words over the pinger's name: "Claimed by <the claimer>", or
(claimer NULL: the claim drawn as the claimer's own, their name under it)
"Claimed" */
static void markers_claim_label(
	wchar_t const *claimer,
	short claimer_length,
	wchar_t *label,
	short maximum)
{
	static wchar_t const claimed[] = L"Claimed";
	static wchar_t const by[] = L" by ";
	short length = 0;
	short index;

	for (index = 0; claimed[index] && length < maximum - 1; index++)
		label[length++] = claimed[index];
	if (claimer)
	{
		for (index = 0; by[index] && length < maximum - 1; index++)
			label[length++] = by[index];
		for (index = 0; index < claimer_length && claimer[index] && length < maximum - 1; index++)
			label[length++] = claimer[index];
	}
	label[length] = 0;
}

/* what a ping of an object of a type is drawn as (beside its arrow,
distance and name): a weapon or equipment its icon, if it has one, else its
outline (a health pack's, a powerup's), through walls; a unit or a vehicle
its outline, through walls (through_walls), except one with an enemy player
in it in a game against other players (versus_players), which is drawn only
where the viewer sees it (no outline through a wall, which would show where
they hide); else the arrow alone */
short network_markers_shape(
	short object_type,
	boolean has_icon,
	boolean versus_players,
	boolean enemy_player,
	boolean *through_walls)
{
	*through_walls = FALSE;
	switch (object_type)
	{
	case _object_type_weapon:
	case _object_type_equipment:
		if (has_icon)
			return _marker_shape_icon;
		*through_walls = TRUE;
		return _marker_shape_outline;
	case _object_type_biped:
	case _object_type_vehicle:
		*through_walls = !(versus_players && enemy_player);
		return _marker_shape_outline;
	default:
		return _marker_shape_arrow;
	}
}

/* a weapon's or equipment's icon in the HUD's messages, as its pickup
prompt draws it (hud.c: a weapon's HUD interface's messaging icon; a
grenade's, its grenade HUD interface's): NULL for none */
static struct icon_hud_element_definition const *markers_object_icon(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (!object || !hud_globals || hud_globals->messaging.messaging_icons.index == NONE)
		return NULL;
	if (object_try_and_get_and_verify_type(object_index, FLAG(_object_type_weapon)))
	{
		long hud_index = weapon_definition_get(object->definition_index)->weapon.interface_definition.hud_interface.index;
		struct icon_hud_element_definition const *icon;

		if (hud_index == NONE)
			return NULL;
		icon = &((struct weapon_hud_interface_definition *)tag_get('wphi', hud_index))->messaging_icon;
		return icon->sequence_index != NONE ? icon : NULL;
	}
	if (object_try_and_get_and_verify_type(object_index, FLAG(_object_type_equipment)))
	{
		struct equipment_definition *definition = equipment_definition_get(object->definition_index);

		if (definition->equipment.powerup_type != _equipment_powerup_none &&
			definition->equipment.powerup_type != _equipment_powerup_grenade)
		{
			return NULL;
		}
		return hud_grenade_messaging_icon(definition->equipment.grenade_type);
	}
	return NULL;
}

/* a unit's team as the viewer reads it: a vehicle's its gunner's, else its
driver's (as the motion sensor has it), none for an empty one */
static short markers_unit_team(
	long unit_index)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);

	if (!unit)
		return NONE;
	if (object_try_and_get_and_verify_type(unit_index, FLAG(_object_type_vehicle)))
	{
		long seat_index = unit->unit.gunner_object_index != NONE ? unit->unit.gunner_object_index :
			unit->unit.driver_object_index;
		struct unit_datum *seat = seat_index != NONE ? unit_try_and_get(seat_index) : NULL;

		return seat ? seat->object.owner_team_index : NONE;
	}
	return unit->object.owner_team_index;
}

/* whether a unit (or a vehicle, by who is in it) is the viewer's enemy, and
whether it is, or has in it, a player who is (enemy_player) */
static boolean markers_object_enemy(
	struct player_datum const *viewer,
	long object_index,
	boolean *enemy_player)
{
	struct object_datum *viewer_unit = viewer ? object_try_and_get(viewer->unit_index) : NULL;
	short viewer_team = viewer_unit ? viewer_unit->object.owner_team_index : NONE;
	short team = markers_unit_team(object_index);
	struct object_datum *object = object_try_and_get(object_index);
	boolean enemy = team != NONE && viewer_team != NONE && game_team_is_enemy(viewer_team, team);
	long rider_index;
	short count;

	*enemy_player = FALSE;
	if (!object || !viewer_unit)
		return FALSE;
	if (enemy && player_index_from_unit_index(object_index) != NONE)
		*enemy_player = TRUE;
	/* (the players riding it) */
	rider_index = object->object.first_child_object_index;
	for (count = 0; rider_index != NONE && count < 64 && !*enemy_player; count++)
	{
		struct object_datum *rider = object_try_and_get(rider_index);

		if (!rider)
			break;
		if (object_try_and_get_and_verify_type(rider_index, _object_mask_unit) &&
			player_index_from_unit_index(rider_index) != NONE &&
			game_team_is_enemy(viewer_team, rider->object.owner_team_index))
		{
			*enemy_player = TRUE;
		}
		rider_index = rider->object.next_object_index;
	}
	return enemy;
}

/* an enemy's cue's colour: red, or orange for a pinger whose own colour is
reddish (a red team's), so that it still reads apart from theirs */
static void markers_enemy_color(
	real_rgb_color const *pinger_color,
	real_rgb_color *color)
{
	if (pinger_color->red > 0.5f && pinger_color->green < 0.45f && pinger_color->blue < 0.45f)
	{
		color->red = 1.0f;
		color->green = 0.62f;
		color->blue = 0.05f;
	}
	else
	{
		color->red = 0.92f;
		color->green = 0.12f;
		color->blue = 0.10f;
	}
}

/* a pinged item's icon (as the pickup prompt draws it, from the HUD's
message icons: the high-res HUD's stand-in for it, or an MCC map's own),
in the pinger's colour (the icon's white tinted: its alpha the outline),
the middle of its bottom edge at bottom, at most height tall and widest
wide; how tall it was drawn */
static boolean markers_draw_icon(
	struct icon_hud_element_definition const *icon,
	real_rgb_color const *color,
	real alpha,
	real_point2d const *bottom,
	real height,
	real widest,
	real *drawn_height)
{
	real_point2d center;
	long bitmap_group_index = hud_globals->messaging.messaging_icons.index;
	struct bitmap_data const *bitmap = NULL;
	real_rectangle2d const *clip = NULL;
	long frame_index = icon->frame_rate ? game_time_get() / icon->frame_rate : 0;
	real width_pixels, height_pixels, scale;
	point2d corner;
	byte byte_alpha;

	hud_retrieve_bitmap_and_bounding_rect(bitmap_group_index, icon->sequence_index, frame_index, &bitmap, &clip);
	if (!bitmap || !_texture_cache_bitmap_get_hardware_format((struct bitmap_data *)bitmap, FALSE, TRUE))
		return FALSE;
	if (!mcc_cache_tags_loaded())
		bitmap = hud_hires_sprite_bitmap(bitmap, icon->sequence_index);
	width_pixels = bitmap->width * (clip ? clip->x1 - clip->x0 : 1.0f);
	height_pixels = bitmap->height * (clip ? clip->y1 - clip->y0 : 1.0f);
	if (!(width_pixels > 0.0f) || !(height_pixels > 0.0f))
		return FALSE;
	scale = MIN(height / height_pixels, widest / width_pixels);
	*drawn_height = height_pixels * scale;
	center.x = bottom->x;
	center.y = bottom->y - *drawn_height * 0.5f;
	/* (a dark copy under it first, a little down and right, so that it reads
over a bright or busy wall) */
	byte_alpha = (byte)PIN((long)(255.0f * alpha * 0.7f + 0.5f), 0, 255);
	hud_draw_mark_begin(center.x + 1.5f, center.y + 1.5f, &corner);
	hud_draw_bitmap_direct(bitmap, _hud_anchor_center, &corner, clip, scale, 0.0f, (pixel32)byte_alpha << 24, FALSE);
	hud_draw_mark_end();
	byte_alpha = (byte)PIN((long)(255.0f * alpha + 0.5f), 0, 255);
	hud_draw_mark_begin(center.x, center.y, &corner);
	hud_draw_bitmap_direct(bitmap, _hud_anchor_center, &corner, clip, scale, 0.0f,
		((pixel32)byte_alpha << 24) | real_rgb_color_to_pixel32(color), FALSE);
	hud_draw_mark_end();
	return TRUE;
}

/* this map's arrow and sound, looked up once a game */
static void markers_look_up(
	void)
{
	static char const *const arrows[] = { "default", "target_blue", NULL };
	static char const *const sounds[] = { "sound\\sfx\\ui\\cursor", "sound\\sfx\\ui\\forward" };
	short index;

	if (markers_globals.looked)
		return;
	markers_globals.looked = TRUE;
	/* (the campaign's arrow; else the multiplayer goals'; else the first) */
	markers_globals.nav_index = NONE;
	for (index = 0; index < (short)NUMBEROF(arrows) && markers_globals.nav_index == NONE; index++)
		markers_globals.nav_index = hud_find_nav_point_quietly(arrows[index]);
	markers_globals.sound_index = NONE;
	for (index = 0; index < (short)NUMBEROF(sounds) && markers_globals.sound_index == NONE; index++)
		markers_globals.sound_index = tag_loaded(SOUND_DEFINITION_TAG, sounds[index]);
	platform_log("pings: the arrow %d (%s), the sound %s", markers_globals.nav_index,
		markers_globals.nav_index == hud_find_nav_point_quietly("default") ? "default" : "another",
		markers_globals.sound_index != NONE ? "found" : "none");
}

/* a new game: the last one's pings, counts and settings let go */
static void markers_new_game(
	void)
{
	csmemset(markers_globals.markers, 0, sizeof(markers_globals.markers));
	csmemset(markers_globals.rates, 0, sizeof(markers_globals.rates));
	markers_globals.config_known = FALSE;
	markers_globals.config_sent_known = FALSE;
	markers_globals.looked = FALSE;
	markers_globals.refusals_logged = 0;
	markers_globals.nav_index = NONE;
	markers_globals.sound_index = NONE;
}

/* one taken from a count; FALSE if none is left */
static boolean markers_rate_take(
	struct marker_rate *rate,
	unsigned long now)
{
	if (!rate->known)
	{
		rate->known = TRUE;
		rate->tokens = MARKER_BURST;
		rate->refilled_at = now;
	}
	while (rate->tokens < MARKER_BURST && now - rate->refilled_at >= MARKER_REFILL_MILLISECONDS)
	{
		rate->tokens++;
		rate->refilled_at += MARKER_REFILL_MILLISECONDS;
	}
	if (rate->tokens >= MARKER_BURST)
		rate->refilled_at = now;
	if (rate->tokens <= 0)
		return FALSE;
	rate->tokens--;
	return TRUE;
}

/* ---------- shown */

/* a ping as the host decided it (or taken away), on this machine */
static void markers_show(
	short player_index,
	byte kind,
	long object_index,
	real_point3d const *point)
{
	struct marker *marker = &markers_globals.markers[player_index];
	struct player_datum *pinger = distributed_player(player_index);
	short local_player_index;
	boolean seen = FALSE;
	boolean muted = FALSE;

	if (kind == _marker_kind_clear)
	{
		if (marker->active)
			platform_log("pings: player %d's ping taken away", player_index);
		marker->active = FALSE;
		return;
	}
	marker->active = TRUE;
	marker->kind = kind;
	marker->object_index = markers_kind_of_object(kind) ? object_index : NONE;
	marker->point = *point;
	marker->shown_at = system_milliseconds();
	/* (a claim: the claimed pings of it shown their whole time again) */
	if (kind == _marker_kind_claim)
	{
		short other_index;

		for (other_index = 0; other_index < MAXIMUM_MARKER_PLAYERS; other_index++)
		{
			struct marker *other = &markers_globals.markers[other_index];

			if (other_index != player_index && other->active && other->kind == _marker_kind_object &&
				other->object_index == object_index &&
				marker->shown_at - other->shown_at < MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS)
			{
				other->shown_at = marker->shown_at;
			}
		}
	}
	for (local_player_index = local_player_get_next(NONE); local_player_index != NONE;
		local_player_index = local_player_get_next(local_player_index))
	{
		long viewer_index = local_player_get_player_index(local_player_index);

		if (viewer_index == NONE)
			continue;
		if (markers_shown_to(pinger, player_get(viewer_index)))
			seen = TRUE;
		else if (markers_sees(pinger, player_get(viewer_index)))
			muted = TRUE;
	}
	{
		struct object_datum *object = markers_kind_of_object(kind) ? object_try_and_get(object_index) : NULL;

		platform_log("pings: player %d %s %s %d%s%s%s at %.2f %.2f %.2f%s", player_index,
			kind == _marker_kind_claim ? "claimed" : "pinged", markers_kind_of_object(kind) ? "object" : "point",
			markers_kind_of_object(kind) ? (int)object_index : -1, object ? " (" : "",
			object ? tag_get_name(object->definition_index) : "", object ? ")" : "", point->x, point->y, point->z,
			seen ? "" : muted ? " (muted here)" : " (not seen here)");
	}
	markers_look_up();
	if (seen && markers_globals.sound_index != NONE)
		unspatialized_impulse_sound_new(markers_globals.sound_index, 1.0f);
}

/* ---------- the host */

/* (the host) a ping refused, logged (MARKER_REFUSALS_LOGGED a game at most) */
static void markers_refused(
	short player_index,
	char const *why)
{
	if (markers_globals.refusals_logged >= MARKER_REFUSALS_LOGGED)
		return;
	markers_globals.refusals_logged++;
	platform_log("pings: a ping of player %d refused: %s", player_index, why);
}

/* (the host) a message to every OpenH1 client machine with a player who
sees the pinger's pings (all of them: every OpenH1 client machine) */
static void markers_host_send(
	struct marker_message const *body,
	struct player_datum const *pinger)
{
	long machine_indices[MAXIMUM_MARKER_MACHINES];
	short count;
	short index;
	struct marker_wire message;

	if (game_connection() != _game_connection_network_server)
		return;
	count = distributed_client_machines(machine_indices, MAXIMUM_MARKER_MACHINES);
	for (index = 0; index < count; index++)
	{
		boolean sees = pinger == NULL;

		if (!network_standard_machine_is_openh1(machine_indices[index]))
			continue;
		if (!sees)
		{
			struct data_iterator iterator;
			struct player_datum *player;

			data_iterator_new(&iterator, player_data);
			while (!sees && (player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
			{
				sees = !player->quit_out_of_game &&
					distributed_machine_has_player(machine_indices[index],
						(short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index)) &&
					markers_sees(pinger, player);
			}
		}
		if (sees)
		{
			csmemset(&message, 0, sizeof(message));
			message.marker = *body;
			distributed_send_to_machine_reliably(machine_indices[index], &message, _distributed_message_marker_down,
				1, (word)sizeof(message));
		}
	}
}

/* (the host) whether a pinger's unit is near enough their point: from where
the unit is in the world (a seated unit's own position is its seat's in the
vehicle, near the world's origin wherever the vehicle is) */
static boolean markers_unit_near(
	long unit_index,
	real_point3d const *point)
{
	real_point3d origin;

	object_get_origin(unit_index, &origin);
	return distance3d(&origin, point) <= MARKER_HOST_RANGE;
}

/* (the host) whether another player's ping or claim of an object is up,
one the pinger sees */
static boolean markers_pinged_by_another(
	long object_index,
	short player_index,
	struct player_datum const *pinger,
	unsigned long now)
{
	short other_index;

	for (other_index = 0; other_index < MAXIMUM_MARKER_PLAYERS; other_index++)
	{
		struct marker const *other = &markers_globals.markers[other_index];
		struct player_datum *other_pinger;

		if (other_index == player_index || !other->active || !markers_kind_of_object(other->kind) ||
			other->object_index != object_index ||
			now - other->shown_at >= MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS)
		{
			continue;
		}
		other_pinger = distributed_player(other_index);
		if (other_pinger && !other_pinger->quit_out_of_game && markers_sees(other_pinger, pinger))
			return TRUE;
	}
	return FALSE;
}

/* (the host, or a game of this machine alone) a player's ping, checked,
decided (a new one, or the last taken away), sent on and shown */
static void markers_host_take(
	short player_index,
	byte kind,
	long object_index,
	real_point3d const *point)
{
	struct player_datum *pinger = distributed_player(player_index);
	struct marker *marker;
	struct marker_message body;
	unsigned long now = system_milliseconds();
	long unit_index;
	real_point3d shown = *point;
	real last[3];
	real here[3];

	if (!pinger || pinger->quit_out_of_game || !markers_available() ||
		(kind != _marker_kind_point && kind != _marker_kind_object) ||
		!network_markers_point_valid(point->x, point->y, point->z))
	{
		markers_refused(player_index, "not a ping of the game");
		return;
	}
	/* (a player the host muted for everyone: network_chat.c) */
	if (markers_network_game() && network_chat_host_mute_minutes(player_index) > 0)
	{
		markers_refused(player_index, "muted by the host");
		return;
	}
	/* (a player with a living unit, near enough the point) */
	unit_index = distributed_living_unit(pinger);
	if (unit_index == NONE || !markers_unit_near(unit_index, point))
	{
		markers_refused(player_index, "no living unit near the point");
		return;
	}
	if (!markers_rate_take(&markers_globals.rates[player_index], now))
	{
		markers_refused(player_index, "too many at once");
		return;
	}
	/* (an object of the kinds that is there; else the point) */
	if (kind == _marker_kind_object && !object_try_and_get_and_verify_type(object_index, MARKER_OBJECT_MASK))
		kind = _marker_kind_point;
	if (kind == _marker_kind_object)
	{
		real radius;

		object_get_bounding_sphere(object_index, &shown, &radius);
	}
	else
	{
		object_index = NONE;
	}
	marker = &markers_globals.markers[player_index];
	last[0] = marker->point.x;
	last[1] = marker->point.y;
	last[2] = marker->point.z;
	here[0] = shown.x;
	here[1] = shown.y;
	here[2] = shown.z;
	csmemset(&body, 0, sizeof(body));
	body.format = MARKER_FORMAT;
	body.player_index = (byte)player_index;
	body.kind = kind;
	body.object_index = object_index;
	body.point[0] = shown.x;
	body.point[1] = shown.y;
	body.point[2] = shown.z;
	/* (its last again, still shown: taken away; an item another's ping is
	on: claimed) */
	body.kind = network_markers_decided_kind(kind,
		marker->active && now - marker->shown_at < MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS &&
			network_markers_same_spot(object_index, here,
				markers_kind_of_object(marker->kind) ? marker->object_index : NONE, last),
		kind == _marker_kind_object && object_try_and_get_and_verify_type(object_index, _object_mask_item),
		kind == _marker_kind_object && markers_pinged_by_another(object_index, player_index, pinger, now));
	if (body.kind == _marker_kind_clear)
		body.object_index = NONE;
	markers_host_send(&body, pinger);
	markers_show(player_index, body.kind, body.object_index, &shown);
}

/* (the host, in a game) its settings sent to every OpenH1 client as they
change, and each MARKER_CONFIG_MILLISECONDS */
static void markers_host_update_config(
	unsigned long now)
{
	byte flags = markers_own_config_flags();
	struct marker_message body;

	if (markers_globals.config_sent_known && flags == markers_globals.config_sent_flags &&
		now - markers_globals.config_sent_at < MARKER_CONFIG_MILLISECONDS)
	{
		return;
	}
	markers_globals.config_sent_known = TRUE;
	markers_globals.config_sent_flags = flags;
	markers_globals.config_sent_at = now;
	csmemset(&body, 0, sizeof(body));
	body.format = MARKER_FORMAT;
	body.player_index = NO_PLAYER;
	body.kind = _marker_kind_config;
	body.flags = flags;
	body.object_index = NONE;
	markers_host_send(&body, NULL);
}

/* ---------- aiming */

/* whether an object is one a ping is on: a unit, vehicle, weapon or
equipment, not a dead biped's body (the aim passes it, to the weapon lying
on it, or the point) */
static boolean markers_object_pingable(
	long object_index)
{
	struct object_datum *object = (struct object_datum *)object_try_and_get_and_verify_type(object_index,
		MARKER_OBJECT_MASK);

	return object && !(object->object.type == _object_type_biped &&
		TEST_FLAG(object->object.damage_flags, _object_dead_bit));
}

/* the object a hit object stands for: a held weapon's unit */
static long markers_object_marked(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (object && object->object.parent_object_index != NONE &&
		object_try_and_get_and_verify_type(object_index, _object_mask_item))
	{
		object_index = object->object.parent_object_index;
	}
	return markers_object_pingable(object_index) ? object_index : NONE;
}

/* the outermost object an object is in (a rider's vehicle) */
static long markers_root_object(
	long object_index)
{
	struct object_datum *object = object_try_and_get(object_index);
	short depth;

	for (depth = 0; object && object->object.parent_object_index != NONE && depth < 8; depth++)
	{
		object_index = object->object.parent_object_index;
		object = object_try_and_get(object_index);
	}
	return object_index;
}

/* what a player aims at from their camera (origin, along forward): TRUE
with the kind, the object and the point */
static boolean markers_aim_from(
	long player_index,
	real_point3d const *camera_position,
	real_vector3d const *camera_forward,
	byte *kind,
	long *object_index,
	real_point3d *point)
{
	struct player_datum *player = player_index != NONE ? player_get(player_index) : NULL;
	struct collision_result result;
	real_point3d origin;
	real_vector3d forward;
	real_vector3d vector;
	long unit_index;
	long root_index;
	long hit_object_index = NONE;
	real reach = MARKER_RANGE;
	boolean hit = FALSE;
	short attempt;

	if (!player || (unit_index = distributed_living_unit(player)) == NONE)
		return FALSE;
	origin = *camera_position;
	forward = *camera_forward;
	if (normalize3d(&forward) == 0.0f)
		return FALSE;
	root_index = markers_root_object(unit_index);
	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return FALSE;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
	/* (past the player's own unit and what it rides, which the line from a
	camera behind them passes through) */
	for (attempt = 0; attempt < 4; attempt++)
	{
		scale_vector3d(&forward, reach, &vector);
		hit = collision_test_vector(MARKER_AIM_FLAGS, &origin, &vector, root_index, &result);
		if (!hit || result.type != _collision_result_object || markers_root_object(result.object_index) != root_index)
			break;
		reach -= reach * result.t + 0.05f;
		origin = result.point;
		origin.x += forward.i * 0.05f;
		origin.y += forward.j * 0.05f;
		origin.z += forward.k * 0.05f;
		hit = FALSE;
		if (reach <= 0.0f)
			break;
	}
	--global_current_collision_user_depth;
	if (hit && result.type == _collision_result_object)
		hit_object_index = markers_object_marked(result.object_index);
	/* (else the nearest object the aim passes near, before what it hit) */
	if (hit_object_index == NONE)
	{
		struct object_iterator iterator;
		struct object_datum *object;
		real hit_distance = hit ? distance3d(camera_position, &result.point) : MARKER_RANGE;
		real best = 0.0f;

		object_iterator_new(&iterator, MARKER_OBJECT_MASK, 0);
		while ((object = (struct object_datum *)object_iterator_next(&iterator)) != NULL)
		{
			real_point3d center;
			real_vector3d to;
			real radius;
			real along;
			real across;
			real nearness;

			if (iterator.index == unit_index || markers_root_object(iterator.index) == root_index ||
				object->object.parent_object_index != NONE || !markers_object_pingable(iterator.index))
			{
				continue;
			}
			object_get_bounding_sphere(iterator.index, &center, &radius);
			vector_from_points3d(camera_position, &center, &to);
			along = dot_product3d(&to, &forward);
			across = square_root(MAX(0.0f, dot_product3d(&to, &to) - along * along));
			if (!network_markers_pick(along, across, radius, hit_distance, &nearness))
				continue;
			/* (the nearest the line, as its reach measures it) */
			if (hit_object_index == NONE || nearness < best)
			{
				hit_object_index = iterator.index;
				best = nearness;
			}
		}
	}
	if (hit_object_index != NONE)
	{
		real radius;

		*kind = _marker_kind_object;
		*object_index = hit_object_index;
		object_get_bounding_sphere(hit_object_index, point, &radius);
		return TRUE;
	}
	if (!hit)
		return FALSE;
	*kind = _marker_kind_point;
	*object_index = NONE;
	*point = result.point;
	return TRUE;
}

/* what a local player aims at, from their camera */
static boolean markers_aim(
	short local_player_index,
	byte *kind,
	long *object_index,
	real_point3d *point)
{
	struct observer_result const *camera = observer_get_camera(local_player_index);

	return camera && markers_aim_from(local_player_get_player_index(local_player_index), &camera->position,
		&camera->forward, kind, object_index, point);
}

/* a local player's ping: aimed, then the host's to decide; forged: (a
test's client) sent as another machine's player's, as a changed build could,
for the host to refuse */
static boolean markers_ping_as(
	short local_player_index,
	boolean forged)
{
	long player_index = local_player_get_player_index(local_player_index);
	short absolute_index;
	byte kind;
	long object_index;
	real_point3d point;

	if (player_index == NONE || !markers_available() || cinematic_in_progress() ||
		!markers_aim(local_player_index, &kind, &object_index, &point))
	{
		return FALSE;
	}
	absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (forged)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		absolute_index = NONE;
		data_iterator_new(&iterator, player_data);
		while (absolute_index == NONE && (player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			if (player->local_player_index == NONE)
				absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		}
		if (absolute_index == NONE || game_connection() != _game_connection_network_client)
			return FALSE;
	}
	if (absolute_index < 0 || absolute_index >= MAXIMUM_MARKER_PLAYERS)
		return FALSE;
	if (game_connection() != _game_connection_network_client)
	{
		markers_host_take(absolute_index, kind, object_index, &point);
		return TRUE;
	}
	/* (not more than the host takes; a test's burst all sent, for the host
	to count) */
	if (!forged && !markers_globals.unchecked &&
		!markers_rate_take(&markers_globals.rates[absolute_index], system_milliseconds()))
	{
		return FALSE;
	}
	{
		struct marker_wire message;

		csmemset(&message, 0, sizeof(message));
		message.marker.format = MARKER_FORMAT;
		message.marker.player_index = (byte)absolute_index;
		message.marker.kind = kind;
		message.marker.object_index = object_index;
		message.marker.point[0] = point.x;
		message.marker.point[1] = point.y;
		message.marker.point[2] = point.z;
		distributed_send(&message, _distributed_message_marker_up, 1, (word)sizeof(message),
			_distributed_to_host_reliably);
	}
	return TRUE;
}

static boolean markers_ping(
	short local_player_index)
{
	return markers_ping_as(local_player_index, FALSE);
}

/* ---------- public code */

unsigned char network_markers_pad_open(
	short port)
{
	return markers_available() && controller_remap_local_player(port) != NONE && controller_remap_playing(port) &&
		!cinematic_in_progress();
}

void network_markers_update(
	void)
{
	unsigned long now = system_milliseconds();
	short port;
	boolean key;

	/* (a game: begun, or a new one, whose time starts again) */
	if (!game_in_progress() || (!markers_network_game() && game_connection() != _game_connection_local))
	{
		if (markers_globals.in_game)
			markers_new_game();
		markers_globals.in_game = FALSE;
	}
	else
	{
		if (!markers_globals.in_game || game_time_get() < markers_globals.last_game_time)
			markers_new_game();
		markers_globals.in_game = TRUE;
		markers_globals.last_game_time = game_time_get();
		if (game_connection() == _game_connection_network_server && network_standard_agreed_rate() != 0)
			markers_host_update_config(now);
	}
	/* (the presses, every port's taken whether they ping or not: a custom
	layout's PING, else the stock layouts' D-pad Right; the keyboard's,
	the first player's) */
	for (port = 0; port < 4; port++)
	{
		boolean pressed;
		unsigned char layout[CONTROLLER_REMAP_ACTIONS];

		if (controller_remap_read(port, layout))
			pressed = controller_remap_pressed(port, 1u << PAD_PING) != 0;
		else
			pressed = halo_ping_pad_pressed(port) != 0;
		if (pressed && network_markers_pad_open(port))
			markers_ping(controller_remap_local_player(port));
	}
	key = (halo_keyboard_actions(0) & FLAG(HALO_KEYBOARD_PING)) != 0;
	if (key && !markers_globals.key_held && markers_available() && !console_is_active() && !network_chat_typing())
	{
		short local_player_index = controller_remap_local_player(0);

		if (local_player_index == NONE)
			local_player_index = local_player_get_next(NONE);
		if (local_player_index != NONE && !ui_widgets_active_for_local_player(local_player_index))
			markers_ping(local_player_index);
	}
	markers_globals.key_held = key;
}

/* a player's ping, while it is up and the viewer is shown it: its pinger
(else NULL) */
static struct player_datum *markers_live(
	short player_index,
	struct player_datum const *viewer,
	unsigned long now)
{
	struct marker const *marker = &markers_globals.markers[player_index];
	struct player_datum *pinger;

	if (!marker->active || now - marker->shown_at >= MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS)
		return NULL;
	pinger = distributed_player(player_index);
	if (!pinger || pinger->quit_out_of_game || !markers_shown_to(pinger, viewer))
		return NULL;
	return pinger;
}

/* a claim's claimed ping, which draws it: another's ping of its object, up
and shown the viewer (NONE: the claim drawn as the claimer's own) */
static short markers_claimed_ping(
	short claimer_index,
	struct player_datum const *viewer,
	unsigned long now)
{
	long object_index = markers_globals.markers[claimer_index].object_index;
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_MARKER_PLAYERS; player_index++)
	{
		struct marker const *marker = &markers_globals.markers[player_index];

		if (player_index != claimer_index && marker->kind == _marker_kind_object &&
			marker->object_index == object_index && markers_live(player_index, viewer, now))
		{
			return player_index;
		}
	}
	return NONE;
}

/* whose claim a ping is drawn with: a claim's own player's (drawn as their
own); an object's ping's, the last claim of it up and shown the viewer
(NONE for none) */
static short markers_claimer(
	short player_index,
	struct player_datum const *viewer,
	unsigned long now)
{
	struct marker const *marker = &markers_globals.markers[player_index];
	short claimer_index = NONE;
	short other_index;

	if (marker->kind == _marker_kind_claim)
		return player_index;
	if (marker->kind != _marker_kind_object)
		return NONE;
	for (other_index = 0; other_index < MAXIMUM_MARKER_PLAYERS; other_index++)
	{
		struct marker const *other = &markers_globals.markers[other_index];

		if (other_index != player_index && other->kind == _marker_kind_claim &&
			other->object_index == marker->object_index && markers_live(other_index, viewer, now) &&
			(claimer_index == NONE ||
				other->shown_at - markers_globals.markers[claimer_index].shown_at < 0x80000000UL))
		{
			claimer_index = other_index;
		}
	}
	return claimer_index;
}

/* a player's name, as a wide string */
static void markers_player_name(
	struct player_datum const *player,
	wchar_t *name)
{
	short index;

	for (index = 0; index < (short)NUMBEROF(player->name); index++)
		name[index] = player->name[index];
	name[NUMBEROF(player->name)] = 0;
}

void network_markers_render(
	short local_player_index)
{
	unsigned long now = system_milliseconds();
	long viewer_index;
	struct player_datum *viewer;
	real_point3d head;
	long font_index;
	short player_index;

	if (!markers_globals.in_game || local_player_index == NONE || cinematic_in_progress())
		return;
	viewer_index = local_player_get_player_index(local_player_index);
	viewer = viewer_index != NONE ? player_get(viewer_index) : NULL;
	if (!viewer || viewer->unit_index == NONE)
		return;
	markers_look_up();
	if (markers_globals.nav_index == NONE)
		return;
	unit_get_head_position(viewer->unit_index, &head);
	font_index = hud_get_font_index();
	for (player_index = 0; player_index < MAXIMUM_MARKER_PLAYERS; player_index++)
	{
		struct marker *marker = &markers_globals.markers[player_index];
		struct player_datum *pinger;
		unsigned long age;
		real_point3d position;
		real_rgb_color color;
		real_point2d drawn;
		real half_height;
		real alpha;
		short drawn_type;
		long reference_index = NONE;
		short claimer_index;
		struct player_datum *claimer = NULL;
		real_rgb_color claim_color;

		if (!marker->active)
			continue;
		age = now - marker->shown_at;
		pinger = distributed_player(player_index);
		if (age >= MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS || !pinger || pinger->quit_out_of_game)
		{
			marker->active = FALSE;
			continue;
		}
		if (!markers_shown_to(pinger, viewer))
			continue;
		/* (a claim of a ping up: drawn with it) */
		if (marker->kind == _marker_kind_claim && object_try_and_get(marker->object_index) &&
			markers_claimed_ping(player_index, viewer, now) != NONE)
		{
			continue;
		}
		reference_index = markers_drawn_position(marker, &position);
		alpha = markers_alpha(age);
		markers_player_color(DATUM_INDEX_NEW(player_index, pinger->identifier), &color);
		claim_color = color;
		claimer_index = reference_index != NONE ? markers_claimer(player_index, viewer, now) : NONE;
		if (claimer_index != NONE)
		{
			claimer = distributed_player(claimer_index);
			markers_player_color(DATUM_INDEX_NEW(claimer_index, claimer->identifier), &claim_color);
		}
		if (!hud_render_nav_point_colored(local_player_index, &position, markers_globals.nav_index,
			hud_get_nav_point_render_type(local_player_index, &head, &position, reference_index), &color, alpha,
			&drawn, &half_height, &drawn_type))
		{
			continue;
		}
		if (drawn_type != 1 /* off the screen */)
		{
			/* (over the arrow, bottom up: a pinged item's icon, the player's
			name, and an enemy's cue) */
			real top = drawn.y - half_height;
			struct icon_hud_element_definition const *icon = NULL;
			boolean enemy = FALSE;

			if (reference_index != NONE)
			{
				struct object_datum *object = object_get(reference_index);
				boolean through_walls;
				boolean enemy_player;

				icon = markers_object_icon(reference_index);
				if (network_markers_shape(object->object.type, icon != NULL, FALSE, FALSE, &through_walls) !=
					_marker_shape_icon)
				{
					icon = NULL;
				}
				if (object_try_and_get_and_verify_type(reference_index, _object_mask_unit))
					enemy = markers_object_enemy(viewer, reference_index, &enemy_player);
			}
			if (icon)
			{
				real height = half_height * 2.0f * MARKER_ICON_HEIGHT_PART;
				real drawn_height;
				real_point2d center;

				center.x = drawn.x;
				center.y = top - MARKER_ICON_GAP;
				if (markers_draw_icon(icon, &claim_color, alpha, &center, height,
					half_height * 2.0f * MARKER_ICON_WIDEST_PART, &drawn_height))
				{
					top -= MARKER_ICON_GAP + drawn_height;
				}
			}
			/* (the player's name over the arrow, on the screen; over it a
			claim's, in the claimer's colour) */
			if (font_index != NONE)
			{
				wchar_t name[NUMBEROF(pinger->name) + 1];
				struct font_header *font = font_definition_get(font_index);

				markers_player_name(pinger, name);
				markers_draw_name(font_index, name, &color, alpha, &drawn, drawn.y - top);
				top -= 2.0f + font->ascending_height * 0.75f;
				if (claimer)
				{
					wchar_t label[NUMBEROF(pinger->name) + 16];

					markers_claim_label(claimer_index == player_index ? NULL : claimer->name,
						(short)NUMBEROF(claimer->name), label, (short)NUMBEROF(label));
					markers_draw_name(font_index, label, &claim_color, alpha, &drawn, drawn.y - top);
					top -= 2.0f + font->ascending_height * 0.75f;
				}
			}
			/* (an enemy's: a small red arrow over it all, the pinger's colour
			still the ping's) */
			if (enemy)
			{
				real_rgb_color enemy_red;
				real_point2d center;
				real height = half_height * 2.0f * MARKER_ENEMY_CUE_PART;

				markers_enemy_color(&color, &enemy_red);
				center.x = drawn.x;
				center.y = top - 1.0f - height * 0.5f;
				hud_render_nav_point_arrow(markers_globals.nav_index, 0 /* on the screen */, &center, height,
					&enemy_red, alpha);
			}
		}
	}
}

/* a view's pinged units', vehicles' and iconless items' outlines
(render.c's render_window, over its world, before its HUD): the nearest
MARKER_OUTLINES_DRAWN of those its player is shown, not the one they are in,
each in its pinger's colour (a claimed one's in its claimer's; an enemy's
with a red inner edge), drawn through walls or where seen as
network_markers_shape says. Nothing is drawn, and nothing costs, without
such a ping */
void network_markers_render_outlines(
	short local_player_index)
{
	struct
	{
		long object_index;
		real distance;
		real_rgb_color color;
		real alpha;
		boolean enemy;
		boolean through_walls;
	} outlines[MARKER_OUTLINES_DRAWN];
	unsigned long now = system_milliseconds();
	long viewer_index;
	struct player_datum *viewer;
	long viewer_root;
	real_point3d head;
	short count = 0;
	short player_index;
	short index;
	real aspect;

	if (!markers_globals.in_game || local_player_index == NONE || cinematic_in_progress())
		return;
	viewer_index = local_player_get_player_index(local_player_index);
	viewer = viewer_index != NONE ? player_get(viewer_index) : NULL;
	if (!viewer || viewer->unit_index == NONE)
		return;
	viewer_root = markers_root_object(viewer->unit_index);
	unit_get_head_position(viewer->unit_index, &head);
	for (player_index = 0; player_index < MAXIMUM_MARKER_PLAYERS; player_index++)
	{
		struct marker *marker = &markers_globals.markers[player_index];
		struct player_datum *pinger;
		struct object_datum *object;
		unsigned long age;
		boolean through_walls;
		boolean enemy_player;
		boolean enemy = FALSE;
		boolean item;
		real distance;
		short slot;
		short claimer_index;

		if (!marker->active || !markers_kind_of_object(marker->kind))
			continue;
		age = now - marker->shown_at;
		pinger = markers_live(player_index, viewer, now);
		if (!pinger)
			continue;
		object = (struct object_datum *)object_try_and_get_and_verify_type(marker->object_index, MARKER_OBJECT_MASK);
		if (!object || marker->object_index == viewer->unit_index || marker->object_index == viewer_root)
			continue;
		/* (a claim of a ping up: outlined with it) */
		if (marker->kind == _marker_kind_claim && markers_claimed_ping(player_index, viewer, now) != NONE)
			continue;
		item = object_try_and_get_and_verify_type(marker->object_index, _object_mask_item) != NULL;
		enemy_player = FALSE;
		if (!item)
			enemy = markers_object_enemy(viewer, marker->object_index, &enemy_player);
		if (network_markers_shape(object->object.type, item && markers_object_icon(marker->object_index) != NULL,
			game_engine_running(), enemy_player, &through_walls) != _marker_shape_outline)
		{
			continue;
		}
		distance = distance3d(&head, &object->object.bounding_sphere_center);
		/* (the nearest kept: the farthest of a full list given up) */
		slot = count;
		if (count == MARKER_OUTLINES_DRAWN)
		{
			short farthest = 0;

			for (index = 1; index < count; index++)
			{
				if (outlines[index].distance > outlines[farthest].distance)
					farthest = index;
			}
			if (outlines[farthest].distance <= distance)
				continue;
			slot = farthest;
		}
		else
		{
			count++;
		}
		outlines[slot].object_index = marker->object_index;
		outlines[slot].distance = distance;
		claimer_index = markers_claimer(player_index, viewer, now);
		if (claimer_index == NONE)
			claimer_index = player_index;
		markers_player_color(DATUM_INDEX_NEW(claimer_index, distributed_player(claimer_index)->identifier),
			&outlines[slot].color);
		outlines[slot].alpha = markers_alpha(age);
		outlines[slot].enemy = enemy;
		outlines[slot].through_walls = through_walls;
	}
	if (count == 0 || !rasterizer_ping_outline_begin())
		return;
	aspect = (real)(render.camera.viewport_bounds.y1 - render.camera.viewport_bounds.y0) /
		(real)MAX(1, render.camera.viewport_bounds.x1 - render.camera.viewport_bounds.x0);
	for (index = 0; index < count; index++)
	{
		static real const directions[8][2] =
		{
			{ 1.0f, 0.0f }, { -1.0f, 0.0f }, { 0.0f, 1.0f }, { 0.0f, -1.0f },
			{ 0.7071f, 0.7071f }, { -0.7071f, 0.7071f }, { 0.7071f, -0.7071f }, { -0.7071f, -0.7071f },
		};
		real_rgb_color enemy_red;
		struct object_datum *object = object_get(outlines[index].object_index);
		byte alpha = (byte)PIN((long)(255.0f * outlines[index].alpha + 0.5f), 0, 255);
		short ring;

		/* (scissored to the object and its outline's pixels about it) */
		draw_bounds_cube(&object->object.bounding_sphere_center,
			object->object.bounding_sphere_radius * 1.5f + outlines[index].distance * 0.03f + 0.1f);
		markers_enemy_color(&outlines[index].color, &enemy_red);
		rasterizer_ping_outline_pass(TRUE, 0.0f, 0.0f, 0, FALSE);
		render_object_outline(outlines[index].object_index);
		/* (an enemy's edge, the outline, its glow: the first drawn of them
		keeps a pixel) */
		for (ring = outlines[index].enemy ? 0 : 1; ring < 3; ring++)
		{
			real width = ring == 0 ? MARKER_OUTLINE_WIDTH * MARKER_ENEMY_EDGE_PART :
				ring == 1 ? MARKER_OUTLINE_WIDTH : MARKER_OUTLINE_WIDTH * MARKER_GLOW_WIDTH_PART;
			byte ring_alpha = ring == 2 ? (byte)(alpha * MARKER_GLOW_ALPHA_PART) : alpha;
			pixel32 color = ((pixel32)ring_alpha << 24) |
				real_rgb_color_to_pixel32(ring == 0 ? &enemy_red : &outlines[index].color);
			short direction;

			for (direction = 0; direction < 8; direction++)
			{
				rasterizer_ping_outline_pass(FALSE, directions[direction][0] * width * aspect,
					directions[direction][1] * width, color, !outlines[index].through_walls);
				render_object_outline(outlines[index].object_index);
			}
		}
		rasterizer_ping_outline_clear();
	}
	draw_bounds_none();
	rasterizer_ping_outline_end();
}

boolean network_markers_ping_shown(
	short local_player_index,
	long player_index,
	struct network_marker_shown *shown)
{
	short absolute_index = player_index != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
	long viewer_index = local_player_index != NONE ? local_player_get_player_index(local_player_index) : NONE;
	struct player_datum *pinger = player_try_and_get(player_index);
	struct marker *marker;
	unsigned long age;

	if (!markers_globals.in_game || !game_in_progress() || cinematic_in_progress() || viewer_index == NONE ||
		!pinger || pinger->quit_out_of_game || absolute_index < 0 || absolute_index >= MAXIMUM_MARKER_PLAYERS)
	{
		return FALSE;
	}
	marker = &markers_globals.markers[absolute_index];
	age = system_milliseconds() - marker->shown_at;
	if (!marker->active || age >= MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS ||
		!markers_shown_to(pinger, player_get(viewer_index)))
	{
		return FALSE;
	}
	if (shown)
	{
		shown->age = age;
		shown->object_index = markers_drawn_position(marker, &shown->position);
		shown->alpha = markers_alpha(age);
		markers_player_color(player_index, &shown->color);
		markers_look_up();
		shown->nav_index = markers_globals.nav_index;
	}
	return TRUE;
}

boolean network_markers_ping_aimed(
	void)
{
	short local_player_index = local_player_get_next(NONE);

	return local_player_index != NONE && markers_ping(local_player_index);
}

boolean network_markers_ping_forged(
	void)
{
	short local_player_index = local_player_get_next(NONE);

	return local_player_index != NONE && markers_ping_as(local_player_index, TRUE);
}

boolean network_markers_ping_teammate(
	void)
{
	short local_player_index = local_player_get_next(NONE);
	long viewer_index = local_player_index != NONE ? local_player_get_player_index(local_player_index) : NONE;
	struct data_iterator iterator;
	struct player_datum *player;

	if (viewer_index == NONE || !markers_available() || game_connection() == _game_connection_network_client)
		return FALSE;
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		struct camera_command command;
		long unit_index;
		byte kind;
		long object_index;
		real_point3d point;
		short absolute_index = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);

		if (player->local_player_index != NONE || iterator.datum_index == viewer_index ||
			!markers_sees(player, player_get(viewer_index)) || (unit_index = distributed_living_unit(player)) == NONE ||
			absolute_index >= MAXIMUM_MARKER_PLAYERS)
		{
			continue;
		}
		first_person_camera_fake(unit_index, &command);
		if (!markers_aim_from(iterator.datum_index, &command.position, &command.forward, &kind, &object_index, &point))
			continue;
		markers_host_take(absolute_index, kind, object_index, &point);
		return TRUE;
	}
	return FALSE;
}

short network_markers_ping_burst(
	short count)
{
	short local_player_index = local_player_get_next(NONE);
	short sent = 0;
	short index;

	markers_globals.unchecked = TRUE;
	for (index = 0; index < count && local_player_index != NONE; index++)
		sent += markers_ping(local_player_index) ? 1 : 0;
	markers_globals.unchecked = FALSE;
	return sent;
}

short network_markers_shown(
	short *player_indices,
	short maximum)
{
	unsigned long now = system_milliseconds();
	short count = 0;
	short player_index;

	for (player_index = 0; player_index < MAXIMUM_MARKER_PLAYERS && count < maximum; player_index++)
	{
		struct marker const *marker = &markers_globals.markers[player_index];

		if (marker->active && now - marker->shown_at < MARKER_SHOWN_MILLISECONDS + MARKER_FADE_MILLISECONDS &&
			!markers_muted_here(distributed_player(player_index)))
		{
			player_indices[count++] = player_index;
		}
	}
	return count;
}

/* ---------- the messages */

boolean network_markers_handles_message(
	word const *message,
	word size)
{
	struct distributed_message_header header;

	if (size < sizeof(header))
		return FALSE;
	csmemcpy(&header, message, sizeof(header));
	return header.type == _distributed_message_marker_up || header.type == _distributed_message_marker_down;
}

void network_markers_handle_message(
	long machine_index,
	word const *message,
	word size,
	boolean from_stream)
{
	struct distributed_message_header header;
	struct marker_message body;
	real_point3d point;

	/* (whole, one, over a stream, in an OpenH1 game under way) */
	if (size != sizeof(header) + sizeof(body) || !from_stream || !game_in_progress() || !markers_globals.in_game ||
		network_standard_agreed_rate() == 0)
	{
		return;
	}
	csmemcpy(&header, message, sizeof(header));
	csmemcpy(&body, (byte const *)message + sizeof(header), sizeof(body));
	if (header.count != 1)
		return;
	if (body.format != MARKER_FORMAT)
	{
		if (!markers_globals.other_format_noted)
			platform_log("pings: a message of another format (%d), not taken", body.format);
		markers_globals.other_format_noted = TRUE;
		return;
	}
	point.x = body.point[0];
	point.y = body.point[1];
	point.z = body.point[2];
	if (header.type == _distributed_message_marker_up)
	{
		/* (the host's, from a client machine's own player) */
		if (game_connection() != _game_connection_network_server || machine_index < 0 ||
			machine_index >= MAXIMUM_MARKER_MACHINES || body.player_index >= MAXIMUM_MARKER_PLAYERS ||
			!distributed_machine_has_player(machine_index, body.player_index) ||
			(body.kind != _marker_kind_point && body.kind != _marker_kind_object))
		{
			markers_refused(body.player_index, "not a player of its machine's");
			return;
		}
		markers_host_take(body.player_index, body.kind, body.object_index, &point);
		return;
	}
	/* (a client's, from its host) */
	if (game_connection() != _game_connection_network_client || machine_index != NONE)
		return;
	if (body.kind == _marker_kind_config)
	{
		markers_globals.config_known = TRUE;
		markers_globals.config_flags = body.flags;
		return;
	}
	if (body.kind >= NUMBER_OF_MARKER_KINDS || body.player_index >= MAXIMUM_MARKER_PLAYERS ||
		distributed_player_from_byte(body.player_index) == NONE ||
		!network_markers_point_valid(point.x, point.y, point.z))
	{
		return;
	}
	/* (an object this machine does not have: its point) */
	if (markers_kind_of_object(body.kind) &&
		!object_try_and_get_and_verify_type(body.object_index, MARKER_OBJECT_MASK))
	{
		body.kind = _marker_kind_point;
	}
	markers_show(body.player_index, body.kind, body.object_index, &point);
}
