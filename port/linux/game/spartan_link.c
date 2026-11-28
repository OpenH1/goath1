/*
SPARTAN_LINK.C

The HUD's Spartan link (display.spartan_link): live feeds of the living
teammates' views in a row at the bottom of the window, beside the motion
tracker (right of it, within its height, so clear of the text chat and the
messages above it, the panels at the top, and the bottom right's timer;
where the tracker is hidden, where it would be). A key shows and hides it
(controls.spartan_link, B; on a controller in the stock layouts, D-pad
Down), and it stays as the player left it from game to game
(display.spartan_link_shown, their profile's own). The first time a player
is in a game that has it, its key is shown under the tracker's row, once a
profile (display.spartan_link_hint). Co-op (network and split screen) and
team games only, teammates only; a free-for-all game, a Pro 2V2 game, and a
team game whose host turned it off (game.spartan_link: the lobby's and Host
Controls' SPARTAN LINK, network_standard_spartan_link_allowed) have none,
and the key says so on the HUD.

Each feed is the teammate's first-person view as this machine has the
world: their eye (where their body is drawn, render_interpolation.c), their
aim, their weapon's zoom, their weapon and arms, their body hidden, and
none of their HUD. The weapon and arms move as on the teammate's own
screen: first_person_weapons.c animates each linked teammate's in a slot of
its own beside the local players' (its feeds' slots), from the same
messages of their weapon and unit a local player's slot takes (every
machine plays its teammates from their inputs, so each shot's kick and
firing push back, a reload, empty or full, a melee, a grenade's throw, a
weapon put away and readied, overheating and its vent, a charge, come as on
their own machine) and from their unit as this machine has it each tick
(the weapon in their hand, their throttle's moving overlay, their aim's
turning sway). It is hidden while they zoom, as on their screen. Drawn at
the frame's time, so it moves every frame though the feed is drawn in turn. A teammate in a seat whose camera follows the vehicle
is seen from behind. Framed as upright rectangles in the motion tracker's
own color and translucency, the image in its true colors. Under each, within
its width: their name at the left, and at the right their shield and
health as their unit's HUD draws them (its interface's shield and health
meters and their panels, made small, flashing as the HUD's do; thin bars
where a map's HUD has no meter).

On each feed, the teammate's reticle: a dot in a thin ring where they aim,
in the HUD's blue, red with an enemy under it as Halo's turns red (a living
enemy biped, within their weapon's autoaim reach, that a line along their
aim meets first: a collision test of this machine's world, which takes no
seed and makes nothing). Through their eyes it is the feed's middle, the
feed looking along their aim; from behind them in a gunner's seat, the
point their aim meets, seen through the feed's camera. None in a seat
without a weapon (a driver's or a passenger's), nor while they are down.

A teammate who dies is shown down for a moment, then dropped, and comes
back as they respawn: the link stays on, with none alive showing nothing,
until the player turns it off. While this player is dead the link is
hidden (spectating fills the screen: dead_spectate.c) and comes back with
the respawn. Split screen: each window has its own link, of its own
player's teammates, beside its own tracker.

A teammate's ping (network_markers.c: one this player is shown, as it
reaches every machine that shows it) calls their feed up: with the row
hidden by its key, the pinging teammate's feed comes up in it for
SPARTAN_LINK_PING_MILLISECONDS (4 s, half the world marker's time), the
latest ping's first, up to four (only pings since the player last hid it;
a teammate who dies meanwhile drops out); with the row showing, their feed
is not added again but its frame brightens for that time (the tracker's
color paler and whole, the edge thicker, the glow wider), as a popped
feed's does. None where the link is not (SPARTAN LINK off in Settings, the
host's off, a game without it), nor for the player's own pings. On the
feed, while the ping is drawn in the world, its arrow (the world's, in the
pinger's color and fade, behind-a-wall look from the teammate's eye) where
the pinged point or object is in their view, sized to the feed and over the
object as it moves; none where it is off the feed's picture.

The cost: each feed is drawn every frame, as the screen is (render.c's
render_frame, before the window's view), into its own images
(d3d8_device.c's extra views, halo_view_begin) at the size it is drawn on
the screen, without the anti-aliasing pass, multisampling, the world's
ambient occlusion, the objects' shadows, lens flares, decals, the grass and
other detail objects, the weather or the particles too small to see in it,
and its models' levels of detail chosen for its own pixels, coarser than
the screen's (halo_model_detail_scale). With the link hidden, nothing. The
stress test with a partner (benchmark.c, 4K) measured a feed at 0.175 ms of
the game's thread a frame and no GPU time above the runs' spread.
debug.spartan_link_feed_rate draws each instead that many times a second,
in turn, at most one a frame, its image turned to the teammate's look
between (spartan_link_feed_warp); a draw due in a frame that runs a tick's
macro step (the world's update, the slowest frames) then waits for the next
frame, once (spartan_link_frame_judge).

With campaign scoring (campaign_score_hud.c), each teammate's score: with
the link hidden, their names and scores in a line where the row goes
beside the tracker; with it shown, their score in the row under their feed
before their meters, and over the feed's top their streak (3 or more) at
its left and their latest groups of points at its right ("+150 Triple
Kill", the last SPARTAN_LINK_SCORE_MILLISECONDS'), on a dark band.

Only what this machine draws: nothing is sent, the game state is not
touched (no seed is taken, no effect, sound or HUD message made: the
feeds' slots are first_person_weapons.c's own, beside the game state, with
their own idle poses' generator; the key's hint and the refusals are drawn
here). The feeds' slots' animations step once a tick, a slot's few nodes,
for each linked teammate. Only the host's word on a team game's link goes
over the network, in the game record (OpenH1's protocol version 36).
*/

#include "cseries.h"
#include "camera/director.h"
#include "camera/observer.h"
#include "camera/first_person_camera.h"
#include "camera/static_camera.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_messaging.h"
#include "interface/hud_draw.h"
#include "interface/player_ui.h"
#include "interface/ui_widget.h"
#include "game/game_allegiance.h"
#include "interface/hud_definitions.h"
#include "interface/unit_hud_interface_definition.h"
#include "items/weapons.h"
#include "items/weapon_definitions.h"
#include "physics/collisions.h"
#include "physics/collision_usage.h"
#include "main/console.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "scenario/scenario.h"
#include "game/game_globals.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "bitmaps/bitmap_color_conversion.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "units/unit_definitions.h"
#include "units/biped_definitions.h"

#include "halo_keyboard.h"
#include "controller_remap.h"
#include "far_clip.h"
#include "network_coop.h"
#include "network_markers.h"
#include "network_standard.h"
#include "player_settings.h"
#include "spartan_link.h"
#include "campaign_score_hud.h"
#include "royale.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wctype.h>

/* ---------- prototypes */

/* port/linux/src/port_config.c */
char const *config_string(char const *name);
int config_boolean(char const *name);
long config_integer(char const *name);
unsigned long config_changes(void);
void platform_log(char const *format, ...);
unsigned long system_milliseconds(void);
unsigned long long platform_shared_clock_microseconds(void);

/* port/linux/src/d3d8_device.c: the extra views */
void halo_view_begin(int view, unsigned long width, unsigned long height);
void halo_view_end(void);
int halo_view_drawn(int view);
void halo_view_sample(int view);
void halo_view_release(int view);
float halo_screen_pixel_scale(void);
long halo_screen_width(void);

/* port/linux/src/d3d8_device.c: the next frame saved as
debug.screenshot_directory's <name>.bmp */
void halo_screenshot_named(const char *name);

/* source/objects/object_lights.c */
void lights_port_keep(boolean keep);
/* source/rasterizer/xbox/rasterizer_xbox_environment_fog.c */
void fog_screen_port_keep(boolean keep);

/* port/linux/game/render_interpolation.c */
void render_interpolation_unit_camera_position(long unit_index, real_point3d *position);
real render_interpolation_fraction(void);
real render_interpolation_first_person_feed_phase(void);

/* source/interface/hud_nav_points.c: a ping's arrow alone, its middle at
center and height tall */
boolean hud_render_nav_point_arrow(short nav_index, short waypoint_type, real_point2d const *center, real height,
	real_rgb_color const *color, real alpha);

/* port/linux/src/xinput_sdl.c */
int halo_spartan_link_pad_pressed(short port);

/* port/linux/game/network_chat.c */
boolean network_chat_typing(void);

/* port/linux/src/port_config.c: a setting written to config.toml, or to a
player profile's own settings */
int config_write(const char *name, const char *value);
int config_profile_write(const char *folder, const char *name, const char *value);

/* ---------- constants */

#define SPARTAN_LINK_FEEDS 4
/* (the extra views' numbers: 1 + a local player's 4) */
#define SPARTAN_LINK_VIEW(local_player_index, feed) (1 + (local_player_index) * SPARTAN_LINK_FEEDS + (feed))

/* how long a teammate who died is shown down before their feed drops out,
and a refusal's line shown (milliseconds) */
#define SPARTAN_LINK_DOWN_MILLISECONDS 1500
#define SPARTAN_LINK_REFUSAL_MILLISECONDS 2000
/* how long the key's hint is shown, the first time */
#define SPARTAN_LINK_HINT_MILLISECONDS 8000

/* the layout, in the screen's units of a window 480 tall (the HUD's): the
narrowest feed's width (a row of four; a wider row of fewer is as tall),
the gap between them, and the row under each (name, shield and health) */
#define SPARTAN_LINK_NARROWEST 112.0f
#define SPARTAN_LINK_GAP 6.4f
#define SPARTAN_LINK_ROW 12.0f
/* the gap between a feed's picture and the strip of its name and meters */
#define SPARTAN_LINK_STRIP_GAP 2.5f
/* the name's look: the motion tracker's own range label's (its "15m", a
picture in the tracker's bitmap, pale and translucent): the tracker's color
made this much paler, at this opacity */
#define SPARTAN_LINK_NAME_PALER 0.35f
#define SPARTAN_LINK_NAME_ALPHA 0.75f
/* the row's room: from the tracker's right (a gap of SPARTAN_LINK_TRACKER_GAP
of the HUD's units) to this much of the window's width, the bottom right
left to its timer and input display (display.match_timer, input_display.c) */
#define SPARTAN_LINK_TRACKER_GAP 8.0f
#define SPARTAN_LINK_ROOM_RIGHT 0.76f
/* the row's room at the top (display.spartan_link_position "top"): its top
from the view's top edge, and the HUD's panels' room at the left and right
of the window's title-safe frame, in the HUD's units */
#define SPARTAN_LINK_TOP 11.0f
#define SPARTAN_LINK_PANEL_LEFT 180.0f
#define SPARTAN_LINK_PANEL_RIGHT 198.0f
/* where the motion tracker would be while it is not drawn (its circle's
middle from the window's left and bottom, and its radius, in the HUD's units
at its scale): the stock HUD's, as drawn at 16:9 (left 63, middle 104, 403 of
an 854x480 view whose window is 726x408) */
#define SPARTAN_LINK_TRACKER_X 40.0f
#define SPARTAN_LINK_TRACKER_BOTTOM 41.0f
#define SPARTAN_LINK_TRACKER_RADIUS 41.0f
/* the teammate's reticle on their feed: its ring's radius, of the feed's
height, and its curve's steps; and its red, on an enemy (Halo's reticle's) */
#define SPARTAN_LINK_RETICLE_SIZE 0.05f
#define SPARTAN_LINK_RETICLE_SEGMENTS 16
#define SPARTAN_LINK_RETICLE_RED 0xF2FF3424UL
/* a teammate's ping (network_markers.c): how long it brings their feed up
while the row is hidden, and brightens their feed's frame (milliseconds),
the brightening fading out over the last of it; and its arrow on their feed:
its height, of the feed's, far off (nearer, as the world's arrow grows,
up to the most) */
#define SPARTAN_LINK_PING_MILLISECONDS 4000
#define SPARTAN_LINK_PING_FADE_MILLISECONDS 1000
#define SPARTAN_LINK_PING_HEIGHT 0.16f
#define SPARTAN_LINK_PING_HEIGHT_MOST 0.3f

/* ---------- structures */

struct spartan_link_feed
{
	long player_index;
	/* the unit they were last seen alive in */
	long unit_index;
	/* when they were seen dead (0: alive) */
	unsigned long down_at;
	/* the view's size in the screen's units, as last drawn */
	unsigned long width, height;
	/* when it is next drawn (system_milliseconds' thirds: spartan_link_schedule) */
	unsigned long draw_at;
	/* since when their health has flashed, their shield down (0: not) */
	unsigned long health_flash_at;
	/* the game's tick their shield's and their health's HUD pictures began
	flashing (the shield low, the shield down), as the HUD's do (0: not) */
	long shield_flash_tick, health_flash_tick;
	/* (debug.spartan_link_log's: the reticle (0 none, 1 blue, 2 red) and
	the health last logged) */
	short logged_state;
	real logged_health;
	/* their aim as the game has it, this frame (spartan_link_feed_aim_update) */
	boolean aim_shown, aim_enemy;
	real_point3d aim_target;
	/* their look as the feed has it (spartan_link_feed_look): the unit it
	is of, from what to what it glides (forward, up), since when (system
	milliseconds) and over how long, and when it last cut (a jump, or a new
	unit) */
	boolean look_valid;
	long look_unit;
	real_vector3d look_from_forward, look_from_up, look_to_forward, look_to_up;
	unsigned long look_at, look_period, look_cut_at;
	/* the camera its image was drawn from (spartan_link_feed_warp): whose,
	through their eyes or not, where, its axes, its vertical angle's
	tangent and its size, and when */
	boolean image_valid;
	boolean image_through_eyes;
	long image_unit;
	real_point3d image_position;
	real_vector3d image_forward, image_up;
	real image_tangent;
	unsigned long image_width, image_height;
	unsigned long image_at;
	unsigned long image_frame;
};

struct spartan_link_window
{
	/* the player has it on (display.spartan_link_shown, read as each game
	starts), whether any feed shows */
	boolean open;
	/* (hidden) brought up by teammates' pings, and when the player last
	hid it (the pings before that bring nothing up) */
	boolean popped;
	unsigned long closed_at;
	short count;
	struct spartan_link_feed feeds[SPARTAN_LINK_FEEDS];
	/* a line under the tracker's row (a refusal: free for all, Pro 2V2, the
	host's off; the key's hint), since when, and for how long */
	wchar_t line[64];
	unsigned long line_at;
	unsigned long line_milliseconds;
	/* the key's hint asked about in this game (once a game: once a profile
	by display.spartan_link_hint) */
	boolean hint_checked;
	/* (every frame: the next of its feeds to draw in this frame) */
	short next_feed;
};

/* the motion tracker as the HUD last drew it in a window (hud_unit.c): its
picture's left and its circle's middle, in the view's units, and the view's
size and players it was drawn for */
struct spartan_link_tracker
{
	boolean valid;
	real left, center_x, center_y;
	short view_width, view_height;
	short players;
};

static struct
{
	boolean in_game;
	long last_game_time;
	unsigned long read_at;
	boolean enabled;
	real size;
	/* display.spartan_link_position "top" (else beside the tracker), and
	display.spartan_link_stats (the name, shield and health under each feed) */
	boolean top;
	boolean stats;
	/* debug.spartan_link_feed_rate: each feed drawn that many times a second
	in turn (0: every frame) */
	long feed_rate;
	boolean log;
	boolean debug_open;
	long logged_tick;
	struct spartan_link_window windows[MAXIMUM_LOCAL_PLAYERS];
	struct spartan_link_tracker trackers[MAXIMUM_LOCAL_PLAYERS];
	boolean key_held;
	/* the feed drawn this frame (local player, feed; NONE none), and the
	one drawn last, which the next follows round */
	short due_local_player, due_feed;
	short last_local_player, last_feed;
	/* a feed's draw kept off a frame that runs a tick's macro step (the
	world's whole update, the frame's slowest work) to the next frame
	(spartan_link_frame_judge): the macro steps run as this frame began
	(game_macro_steps_run), this frame judged, whether a draw due in it
	waits, whether the last drawn frame ran a macro step, and whether a
	draw waited in this frame and in the last drawn one (never twice) */
	unsigned long frame_macro_steps;
	boolean frame_judged;
	boolean frame_defers;
	boolean last_frame_ticked;
	boolean deferred, last_deferred;
	/* the feed being drawn: whose eyes hide whose body, and whose weapon is
	posed (render_objects.c, first_person_weapons.c) */
	boolean drawing;
	/* when the feed being drawn began, and the feeds' time drawing so far
	(the game's thread: spartan_link_feeds_microseconds) */
	unsigned long long drawing_began, drawing_microseconds;
	/* the frames begun (spartan_link_update: a feed's image of this one is
	its camera's own, not warped) */
	unsigned long frame_serial;
	long hidden_unit;
	long posed_unit;
	/* the views drawn in this game (to free them as it ends), and whose
	view each last drew (a feed moved to another's view waits for its own) */
	unsigned long views_used;
	long view_players[1 + MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS];
	/* (debug.spartan_link_log: a red reticle's frame saved, once a run, and
	a ping's on a feed) */
	boolean red_saved;
	boolean ping_saved;
} spartan_link = { FALSE, 0, (unsigned long)-1, TRUE, 1.0f, FALSE, TRUE, 0, FALSE };

/* ---------- private code */

/* a game in progress (and the game's time made: not before the first map) */
static boolean spartan_link_playing(
	void)
{
	return game_time_initialized() && game_in_progress();
}

static void spartan_link_read(
	void)
{
	char const *size;

	if (spartan_link.read_at == config_changes())
		return;
	spartan_link.read_at = config_changes();
	size = config_string("display.spartan_link");
	spartan_link.enabled = strcmp(size, "off") != 0;
	spartan_link.size = !strcmp(size, "small") ? 0.8f : !strcmp(size, "large") ? 1.2f : 1.0f;
	spartan_link.top = !strcmp(config_string("display.spartan_link_position"), "top");
	spartan_link.stats = config_boolean("display.spartan_link_stats") != 0;
	spartan_link.feed_rate = config_integer("debug.spartan_link_feed_rate");
	spartan_link.log = config_boolean("debug.spartan_link_log");
}

/* a game whose host allows no teammates' feeds: Pro 2V2's, Classic and
Crispy (network_standard_teammate_feeds_allowed) */
static boolean spartan_link_pro_doubles(
	void)
{
	return !network_standard_teammate_feeds_allowed();
}

/* whether this game has a link: co-op (a campaign, network or split
screen) or a team game, in play, not Pro 2V2, and not one whose host turned
it off */
static boolean spartan_link_game(
	void)
{
	if (!spartan_link.enabled || !spartan_link_playing() || cinematic_in_progress() ||
		game_connection() == _game_connection_film_playback)
	{
		return FALSE;
	}
	if (network_coop_active())
		return TRUE;
	/* (Royale: the squad, royale.c) */
	if (royale_active())
		return game_engine_in_play() && !spartan_link_pro_doubles();
	return game_engine_running() ? game_engine_has_teams() && game_engine_in_play() &&
		!spartan_link_pro_doubles() && network_standard_spartan_link_allowed() : TRUE;
}

/* other is self's living teammate (co-op: anyone) */
static boolean spartan_link_teammate(
	long self,
	long other)
{
	struct player_datum *player = player_try_and_get(other);

	if (!player || other == self || player->unit_index == NONE ||
		!object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit))
	{
		return FALSE;
	}
	if (TEST_FLAG(object_get(player->unit_index)->object.damage_flags, _object_dead_bit))
		return FALSE;
	if (royale_active())
		return royale_squadmate_of(self) == other;
	return !game_engine_running() || player->team_index == player_get(self)->team_index;
}

/* a teammate's ping the window's player is shown (their own pings never:
the link has no feed of theirs), come within SPARTAN_LINK_PING_MILLISECONDS:
TRUE with its age (milliseconds) */
static boolean spartan_link_pinged(
	short local_player_index,
	long player_index,
	unsigned long *age)
{
	struct network_marker_shown ping;

	if (player_index == NONE || player_index == local_player_get_player_index(local_player_index) ||
		!network_markers_ping_shown(local_player_index, player_index, &ping) ||
		ping.age >= SPARTAN_LINK_PING_MILLISECONDS)
	{
		return FALSE;
	}
	if (age)
		*age = ping.age;
	return TRUE;
}

/* the hidden window's feeds: the teammates whose pings came since it was
hidden, for SPARTAN_LINK_PING_MILLISECONDS each, the latest first, up to
four (each kept as it was while it stays, so its image and meters go on) */
static void spartan_link_window_pings(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	long self = local_player_get_player_index(local_player_index);
	struct spartan_link_feed feeds[SPARTAN_LINK_FEEDS];
	unsigned long ages[SPARTAN_LINK_FEEDS];
	struct data_iterator iterator;
	struct player_datum *player;
	short count = 0;
	short index;

	if (self == NONE)
	{
		window->count = 0;
		window->popped = FALSE;
		return;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		unsigned long age;
		short at;

		if (!spartan_link_teammate(self, iterator.datum_index) ||
			!spartan_link_pinged(local_player_index, iterator.datum_index, &age) ||
			(window->closed_at && age > now - window->closed_at))
		{
			continue;
		}
		/* (in its place by age, the latest first) */
		for (at = count; at > 0 && ages[at - 1] > age; at--)
		{
			if (at < SPARTAN_LINK_FEEDS)
			{
				feeds[at] = feeds[at - 1];
				ages[at] = ages[at - 1];
			}
		}
		if (at >= SPARTAN_LINK_FEEDS)
			continue;
		csmemset(&feeds[at], 0, sizeof(feeds[at]));
		feeds[at].player_index = iterator.datum_index;
		feeds[at].unit_index = player->unit_index;
		ages[at] = age;
		for (index = 0; index < window->count; index++)
		{
			if (window->feeds[index].player_index == iterator.datum_index &&
				window->feeds[index].unit_index == player->unit_index)
			{
				feeds[at] = window->feeds[index];
				feeds[at].down_at = 0;
			}
		}
		if (count < SPARTAN_LINK_FEEDS)
			count++;
	}
	csmemcpy(window->feeds, feeds, sizeof(feeds[0]) * count);
	window->count = count;
	window->popped = count > 0;
}

/* the window's feeds brought up to date: the downed kept for a moment,
then dropped; the living added in the players' order, up to four */
static void spartan_link_window_update(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	long self = local_player_get_player_index(local_player_index);
	struct data_iterator iterator;
	struct player_datum *player;
	short index;

	if (self == NONE)
	{
		window->count = 0;
		window->open = FALSE;
		return;
	}
	for (index = 0; index < window->count; )
	{
		struct spartan_link_feed *feed = &window->feeds[index];
		struct player_datum *teammate = player_try_and_get(feed->player_index);

		if (spartan_link_teammate(self, feed->player_index) && teammate->unit_index == feed->unit_index)
		{
			feed->down_at = 0;
		}
		else if (!feed->down_at && teammate)
		{
			feed->down_at = now ? now : 1;
		}
		if (!teammate || (feed->down_at && now - feed->down_at >= SPARTAN_LINK_DOWN_MILLISECONDS))
		{
			short later;

			for (later = index; later + 1 < window->count; later++)
				window->feeds[later] = window->feeds[later + 1];
			window->count--;
			continue;
		}
		index++;
	}
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL &&
		window->count < SPARTAN_LINK_FEEDS)
	{
		boolean listed = FALSE;

		if (!spartan_link_teammate(self, iterator.datum_index))
			continue;
		for (index = 0; index < window->count; index++)
		{
			if (window->feeds[index].player_index == iterator.datum_index)
				listed = TRUE;
		}
		if (listed)
			continue;
		csmemset(&window->feeds[window->count], 0, sizeof(window->feeds[window->count]));
		window->feeds[window->count].player_index = iterator.datum_index;
		window->feeds[window->count].unit_index = player->unit_index;
		window->count++;
	}
	/* (nobody left alive: the link stays on, showing nothing, and each
	teammate comes back into it as they respawn; only its key turns it off) */
}

/* the window's player is alive, and sees the link */
static boolean spartan_link_shown(
	short local_player_index)
{
	long self;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		(!spartan_link.windows[local_player_index].open && !spartan_link.windows[local_player_index].popped) ||
		!spartan_link_game())
	{
		return FALSE;
	}
	self = local_player_get_player_index(local_player_index);
	return self != NONE && player_get(self)->unit_index != NONE;
}

/* a line under the tracker's row, for a while */
static void spartan_link_line(
	struct spartan_link_window *window,
	wchar_t const *text,
	unsigned long now,
	unsigned long milliseconds)
{
	ustrncpy(window->line, text, NUMBEROF(window->line) - 1);
	window->line[NUMBEROF(window->line) - 1] = 0;
	window->line_at = now;
	window->line_milliseconds = milliseconds;
}

/* one of a local player's own settings written: their profile's (its
settings.toml), else config.toml's, as Settings writes it (menu_functions.c's
setting_write) */
static void spartan_link_setting_write(
	short local_player_index,
	char const *name,
	boolean value)
{
	char folder[1024];

	if (player_settings_profile_folder(player_ui_get_active_player_profile_index(local_player_index), folder,
		sizeof(folder)))
	{
		config_profile_write(folder, name, value ? "true" : "false");
	}
	else
	{
		config_write(name, value ? "true" : "false");
	}
}

/* a press of the key for a local player: the link on or off (and so it
stays, from game to game), or why there is none */
static void spartan_link_toggle(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || !spartan_link.enabled ||
		!spartan_link_playing() || cinematic_in_progress())
	{
		return;
	}
	window = &spartan_link.windows[local_player_index];
	window->line_milliseconds = 0;
	if (!spartan_link_game())
	{
		spartan_link_line(window, spartan_link_pro_doubles() ? L"Spartan link is off in Pro 2v2" :
			!network_standard_spartan_link_allowed() && game_engine_running() && game_engine_has_teams() ?
			L"Spartan link: the host turned it off" : L"Spartan link: no teammates in free for all", now,
			SPARTAN_LINK_REFUSAL_MILLISECONDS);
		if (spartan_link.log)
		{
			platform_log("spartan link: local player %d: %ls", (int)local_player_index, window->line);
			halo_screenshot_named("spartan_link_refusal");
		}
		return;
	}
	window->open = !window->open;
	spartan_link_setting_write(local_player_index, "display.spartan_link_shown", window->open);
	/* (hidden: only the pings from now on bring feeds up) */
	window->popped = FALSE;
	if (!window->open)
	{
		window->count = 0;
		window->closed_at = now ? now : 1;
	}
	if (window->open)
	{
		spartan_link_window_update(local_player_index, now);
		/* (on with nobody to show: said, the feeds coming as they spawn) */
		if (window->count == 0)
			spartan_link_line(window, L"Spartan link on: no teammates alive", now, SPARTAN_LINK_REFUSAL_MILLISECONDS);
	}
	if (spartan_link.log)
		platform_log("spartan link: local player %d %s, %d feeds", (int)local_player_index,
			window->open ? "open" : "closed", (int)window->count);
}

/* the key's hint, the first time a player is alive in a game that has the
link and it is off: their key (the keyboard's while the keyboard and mouse
play, the first player's; else the controller's D-pad Down), once a
profile */
static void spartan_link_hint(
	short local_player_index,
	unsigned long now)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	long self = local_player_get_player_index(local_player_index);
	char name[64];
	wchar_t text[64];
	short index;

	if (window->hint_checked || self == NONE || player_get(self)->unit_index == NONE || !spartan_link_game())
		return;
	window->hint_checked = TRUE;
	if (config_player_boolean(local_player_index, "display.spartan_link_hint"))
		return;
	spartan_link_setting_write(local_player_index, "display.spartan_link_hint", TRUE);
	/* (one who has it on knows it) */
	if (window->open)
		return;
	name[0] = 0;
	if (local_player_index == controller_remap_local_player(0) || controller_remap_local_player(0) == NONE)
	{
		if (halo_keyboard_prompts() && !halo_keyboard_action_name(HALO_KEYBOARD_SPARTAN_LINK, name, sizeof(name)))
			name[0] = 0;
	}
	if (!name[0])
		csstrcpy(name, "D-pad Down");
	for (index = 0; name[index] && index < (short)NUMBEROF(text) - 1; index++)
		text[index] = (wchar_t)towupper((unsigned char)name[index]);
	text[index] = 0;
	ustrncat(text, L": SPARTAN LINK", NUMBEROF(text) - 1 - ustrlen(text));
	spartan_link_line(window, text, now, SPARTAN_LINK_HINT_MILLISECONDS);
	if (spartan_link.log)
		platform_log("spartan link: local player %d hint: %ls", (int)local_player_index, window->line);
}

/* whether a feed's view holds its teammate's image (drawn, and last for
them: a feed moved to another's place by a drop or a ping has none yet) */
static boolean spartan_link_feed_image(
	short local_player_index,
	short feed)
{
	int view = SPARTAN_LINK_VIEW(local_player_index, feed);

	return halo_view_drawn(view) &&
		spartan_link.view_players[view] == spartan_link.windows[local_player_index].feeds[feed].player_index;
}

/* at a rate (debug.spartan_link_feed_rate a second each, by the real time,
its period counted in thirds of a millisecond), the feed drawn this frame: at
most one a frame of every window's, a feed whose image was never drawn (or
is another's) first, at once, else the next after the last drawn whose time
has come; none while every feed's image is new enough */
static void spartan_link_schedule(
	void)
{
	short local_player_index;
	short feed;
	short best_local = NONE, best_feed = NONE;
	long best_distance = LONG_MAX;
	unsigned long now = system_milliseconds() * 3;
	long last = spartan_link.last_local_player == NONE ? -1 :
		spartan_link.last_local_player * SPARTAN_LINK_FEEDS + spartan_link.last_feed;

	spartan_link.due_local_player = NONE;
	spartan_link.due_feed = NONE;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct spartan_link_window *window = &spartan_link.windows[local_player_index];

		if (!spartan_link_shown(local_player_index))
			continue;
		for (feed = 0; feed < window->count; feed++)
		{
			long slot = local_player_index * SPARTAN_LINK_FEEDS + feed;
			long distance;

			/* (a downed teammate's image stays as it was) */
			if (window->feeds[feed].down_at)
				continue;
			distance = slot > last ? slot - last : slot - last + MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS;
			if (!spartan_link_feed_image(local_player_index, feed))
				distance -= MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS;
			else if ((long)(now - window->feeds[feed].draw_at) < 0)
				continue;
			if (distance < best_distance)
			{
				best_distance = distance;
				best_local = local_player_index;
				best_feed = feed;
			}
		}
	}
	spartan_link.due_local_player = best_local;
	spartan_link.due_feed = best_feed;
}

/* the window's motion tracker in the view's units: its circle's middle and
radius, where the HUD last drew it in a view of this size (dead, or a
gametype without it, too); where it has not been, where the stock HUD
draws it */
static void spartan_link_tracker_place(
	short local_player_index,
	struct render_camera const *camera,
	real *center_x,
	real *center_y,
	real *radius)
{
	struct spartan_link_tracker const *tracker = &spartan_link.trackers[local_player_index];
	short view_width = (short)(camera->window_bounds.x1 - camera->window_bounds.x0);
	short view_height = (short)(camera->window_bounds.y1 - camera->window_bounds.y0);

	if (tracker->valid && tracker->view_width == view_width && tracker->view_height == view_height &&
		tracker->players == local_player_count() && tracker->center_x > tracker->left)
	{
		*center_x = tracker->center_x;
		*center_y = tracker->center_y;
		*radius = tracker->center_x - tracker->left;
	}
	else
	{
		real hud_scale = hud_globals_get_scale(local_player_count() > 1);

		*center_x = (real)(camera->window_bounds.x0 - camera->viewport_bounds.x0) + SPARTAN_LINK_TRACKER_X * hud_scale;
		*center_y = (real)(camera->window_bounds.y1 - camera->viewport_bounds.y0) -
			SPARTAN_LINK_TRACKER_BOTTOM * hud_scale;
		*radius = SPARTAN_LINK_TRACKER_RADIUS * hud_scale;
	}
}

/* the room of the window's row beside its tracker, in the view's units: its
left, right, top and bottom (the tracker's circle's height), and the scale
of the units the design's are in */
static void spartan_link_room(
	short local_player_index,
	struct render_camera const *camera,
	real_rectangle2d *room,
	real *scale)
{
	real frame_x0 = (real)(camera->window_bounds.x0 - camera->viewport_bounds.x0);
	real width = (real)(camera->window_bounds.x1 - camera->window_bounds.x0);
	real height = (real)(camera->window_bounds.y1 - camera->window_bounds.y0);
	real hud_scale = hud_globals_get_scale(local_player_count() > 1);
	real center_x, center_y, radius;

	spartan_link_tracker_place(local_player_index, camera, &center_x, &center_y, &radius);
	/* (a split screen's window keeps the HUD's size, and the text most of
	its) */
	*scale = PIN(height / 480.0f * 1.5f, 0.6f, 1.0f);
	room->x0 = center_x + radius + SPARTAN_LINK_TRACKER_GAP * hud_scale;
	room->x1 = MAX(frame_x0 + width * SPARTAN_LINK_ROOM_RIGHT, room->x0);
	room->y0 = center_y - radius;
	room->y1 = center_y + radius;
}

/* the room of the window's row at the top (display.spartan_link_position
"top"), in the view's units: across, between the HUD's panels, which are
placed in the window's title-safe frame; down, from the view's own top edge
(SPARTAN_LINK_TOP, up in the panels' line, not under the frame's inset) */
static void spartan_link_top_room(
	struct render_camera const *camera,
	real scale,
	real *left,
	real *right,
	real *top)
{
	real frame_x0 = (real)(camera->window_bounds.x0 - camera->viewport_bounds.x0);
	real width = (real)(camera->window_bounds.x1 - camera->window_bounds.x0);

	*left = frame_x0 + SPARTAN_LINK_PANEL_LEFT * scale;
	*right = MAX(frame_x0 + width - SPARTAN_LINK_PANEL_RIGHT * scale, *left);
	*top = SPARTAN_LINK_TOP * scale;
}

/* the window's feeds' layout: each feed's rectangle in the view's units, from
its top left (left, top, right, bottom), and the scale of the units the
design's are in. Each feed is as tall as display.spartan_link's size takes
of the motion tracker's circle's height (large all of it), less the row of
its name and meters under it (display.spartan_link_stats), narrower where
the room is too narrow for them. At the bottom (display.spartan_link_position):
a row beside the tracker, from its right, within the tracker's circle's
height (so the text chat and the messages above the tracker stay clear); at
the top, centered between the HUD's panels, hung from the view's top edge */
static short spartan_link_layout(
	short local_player_index,
	struct render_camera const *camera,
	real_rectangle2d *rectangles,
	real *scale)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	real_rectangle2d room;
	real feed_width, feed_height, total, row, gap, left, top;
	short count = window->count;
	short index;

	if (count <= 0 || camera->window_bounds.x1 <= camera->window_bounds.x0 ||
		camera->window_bounds.y1 <= camera->window_bounds.y0)
	{
		return 0;
	}
	spartan_link_room(local_player_index, camera, &room, scale);
	row = spartan_link.stats ? SPARTAN_LINK_ROW * *scale : 0.0f;
	gap = SPARTAN_LINK_GAP * *scale;
	/* (medium and large fill the tracker's height, the strip included:
	the row's top and bottom the tracker's circle's) */
	feed_height = (room.y1 - room.y0 - row) * MIN(spartan_link.size, 1.0f);
	if (spartan_link.top)
		spartan_link_top_room(camera, *scale, &room.x0, &room.x1, &top);
	feed_width = feed_height * 16.0f / 9.0f;
	total = feed_width * count + gap * (count - 1);
	if (total > room.x1 - room.x0)
	{
		feed_width = MAX(room.x1 - room.x0 - gap * (count - 1), 0.0f) / count;
		feed_height = feed_width * 9.0f / 16.0f;
		total = feed_width * count + gap * (count - 1);
	}
	left = spartan_link.top ? room.x0 + (room.x1 - room.x0 - total) * 0.5f : room.x0;
	for (index = 0; index < count; index++)
	{
		rectangles[index].x0 = left + index * (feed_width + gap);
		rectangles[index].x1 = rectangles[index].x0 + feed_width;
		if (spartan_link.top)
		{
			rectangles[index].y0 = top;
			rectangles[index].y1 = top + feed_height;
		}
		else
		{
			rectangles[index].y1 = room.y1 - row;
			rectangles[index].y0 = rectangles[index].y1 - feed_height;
		}
	}
	return count;
}

/* the field of view a unit looks through: its weapon's at its zoom (as
dead_spectate.c's) */
static real spartan_link_field_of_view(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct unit_definition *definition = unit_definition_get(unit->definition_index);
	long weapon_index = unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index);
	real field_of_view = definition->unit.camera_field_of_view;

	if (weapon_index != NONE)
		field_of_view = weapon_get_field_of_view(weapon_index, field_of_view, (short)unit->unit.current_zoom_level);
	if (!(field_of_view >= 0.001f && field_of_view <= _pi / 2.f))
		field_of_view = DEGREES_TO_RADIANS(70.f);
	return field_of_view;
}

/* a feed's look glides over the time between its aim's changes, at most
this long (milliseconds) and at least the shorter; a change of more than
the angle (its cosine) is a cut, taken at once */
#define SPARTAN_LINK_LOOK_LONGEST 40
#define SPARTAN_LINK_LOOK_SHORTEST 4
#define SPARTAN_LINK_LOOK_CUT_COSINE 0.7071f

/* (the look between its two ends, t of the way, its up square to it) */
static void spartan_link_look_between(
	struct spartan_link_feed const *feed,
	real t,
	real_vector3d *forward,
	real_vector3d *up)
{
	real along;

	forward->i = feed->look_from_forward.i + (feed->look_to_forward.i - feed->look_from_forward.i) * t;
	forward->j = feed->look_from_forward.j + (feed->look_to_forward.j - feed->look_from_forward.j) * t;
	forward->k = feed->look_from_forward.k + (feed->look_to_forward.k - feed->look_from_forward.k) * t;
	up->i = feed->look_from_up.i + (feed->look_to_up.i - feed->look_from_up.i) * t;
	up->j = feed->look_from_up.j + (feed->look_to_up.j - feed->look_from_up.j) * t;
	up->k = feed->look_from_up.k + (feed->look_to_up.k - feed->look_from_up.k) * t;
	if (normalize3d(forward) == 0.0f)
		*forward = feed->look_to_forward;
	along = up->i * forward->i + up->j * forward->j + up->k * forward->k;
	up->i -= forward->i * along;
	up->j -= forward->j * along;
	up->k -= forward->k * along;
	if (normalize3d(up) == 0.0f)
		*up = feed->look_to_up;
}

/* The feed's look (forward, up), smoothed: a teammate's aim reaches this
machine once a tick (or a sub-step), so a look taken as it is steps at that
rate, which the display's rate shows as judder (the feed's image is warped
to the look of each frame: spartan_link_feed_warp). It glides from where it
was shown to each new aim over the time the last change took (as the
bodies are drawn between the ticks, a step behind), and cuts at once to a
new unit or a large turn. Only this machine's drawing */
static void spartan_link_feed_look(
	struct spartan_link_feed *feed,
	real_vector3d *forward,
	real_vector3d *up)
{
	unsigned long now = system_milliseconds();
	real t;

	if (!feed->look_valid || feed->look_unit != feed->unit_index)
	{
		feed->look_valid = TRUE;
		feed->look_unit = feed->unit_index;
		feed->look_from_forward = feed->look_to_forward = *forward;
		feed->look_from_up = feed->look_to_up = *up;
		feed->look_at = feed->look_cut_at = now;
		feed->look_period = SPARTAN_LINK_LOOK_LONGEST;
		return;
	}
	if (forward->i != feed->look_to_forward.i || forward->j != feed->look_to_forward.j ||
		forward->k != feed->look_to_forward.k || up->i != feed->look_to_up.i || up->j != feed->look_to_up.j ||
		up->k != feed->look_to_up.k)
	{
		unsigned long since = now - feed->look_at;
		real_vector3d shown_forward, shown_up;

		t = feed->look_period ? (real)since / (real)feed->look_period : 1.0f;
		spartan_link_look_between(feed, PIN(t, 0.0f, 1.0f), &shown_forward, &shown_up);
		if (shown_forward.i * forward->i + shown_forward.j * forward->j + shown_forward.k * forward->k <
			SPARTAN_LINK_LOOK_CUT_COSINE)
		{
			feed->look_from_forward = *forward;
			feed->look_from_up = *up;
			feed->look_cut_at = now;
		}
		else
		{
			feed->look_from_forward = shown_forward;
			feed->look_from_up = shown_up;
		}
		feed->look_to_forward = *forward;
		feed->look_to_up = *up;
		feed->look_period = PIN(since, SPARTAN_LINK_LOOK_SHORTEST, SPARTAN_LINK_LOOK_LONGEST);
		feed->look_at = now;
	}
	t = feed->look_period ? (real)(now - feed->look_at) / (real)feed->look_period : 1.0f;
	spartan_link_look_between(feed, PIN(t, 0.0f, 1.0f), forward, up);
}

/* a feed's camera: the teammate's eye as their body is drawn, along their
aim (its look, smoothed: spartan_link_feed_look); or in a seat whose camera
follows the vehicle, from behind. TRUE through their eyes, where their aim
is the view's middle */
static boolean spartan_link_feed_view(
	struct spartan_link_feed *feed,
	struct render_camera *camera)
{
	long unit_index = feed->unit_index;
	struct camera_command command;
	director_perspective perspective;
	real_point3d eye;
	boolean through_eyes;

	first_person_camera_fake(unit_index, &command);
	spartan_link_feed_look(feed, &command.forward, &command.up);
	render_interpolation_unit_camera_position(unit_index, &eye);
	through_eyes = !director_desired_perspective(unit_index, &perspective);
	camera->forward = command.forward;
	camera->up = command.up;
	if (through_eyes)
	{
		camera->position = eye;
	}
	else
	{
		struct object_datum *unit = object_get(unit_index);
		long vehicle_index = unit->object.parent_object_index;
		real radius = vehicle_index != NONE && object_try_and_get(vehicle_index) ?
			object_get(vehicle_index)->object.bounding_sphere_radius : 1.0f;
		real back = radius * 1.6f + 0.6f;

		camera->position.x = eye.x - command.forward.i * back;
		camera->position.y = eye.y - command.forward.j * back;
		camera->position.z = eye.z - command.forward.k * back + radius * 0.35f;
	}
	camera->vertical_field_of_view = 2.0f * arctangent(
		0.75f * render_camera_get_adjusted_field_of_view_tangent(
			through_eyes ? spartan_link_field_of_view(unit_index) : DEGREES_TO_RADIANS(70.0f)),
		1.0f);
	return through_eyes;
}

/* ---------- the teammate's aim */

/* what the aim's line stops at: the level, and the objects a shot would
hit or a reticle is turned by (not the weapons and equipment lying about,
nor an enemy's held weapon, which their body is behind) */
#define SPARTAN_LINK_AIM_FLAGS (FLAG(_collision_test_front_facing_surfaces_bit) | \
	FLAG(_collision_test_ignore_invisible_surfaces_bit) | FLAG(_collision_test_structure_bit) | \
	FLAG(_collision_test_objects_bit) | FLAG(_collision_test_objects_bipeds_bit) | \
	FLAG(_collision_test_objects_vehicles_bit) | FLAG(_collision_test_objects_scenery_bit) | \
	FLAG(_collision_test_objects_machines_bit))
/* how far the aim's line reaches (world units) when the weapon's red reach
is shorter: aim_assist.c's shot reference */
#define SPARTAN_LINK_AIM_REACH 128.0f

/* where a point of the world lands on a feed drawn through camera into
bounds (the window's units: the camera's whole view, its vertical angle
across the bounds' height): FALSE behind the camera, or a reticle of
margin about it not wholly on the feed */
static boolean spartan_link_aim_place(
	struct render_camera const *camera,
	real_rectangle2d const *bounds,
	real margin,
	real_point3d const *point,
	real_point2d *place)
{
	real width = bounds->x1 - bounds->x0;
	real height = bounds->y1 - bounds->y0;
	real tangent_y = (real)tan(camera->vertical_field_of_view * 0.5f);
	real tangent_x = tangent_y * width / (height > 0.0f ? height : 1.0f);
	real_vector3d right, to;
	real depth;

	if (!(width > 0.0f) || !(height > 0.0f) || !(tangent_y > 0.0f))
		return FALSE;
	/* (the camera's right: forward across up, y being the world's left) */
	right.i = camera->forward.j * camera->up.k - camera->forward.k * camera->up.j;
	right.j = camera->forward.k * camera->up.i - camera->forward.i * camera->up.k;
	right.k = camera->forward.i * camera->up.j - camera->forward.j * camera->up.i;
	to.i = point->x - camera->position.x;
	to.j = point->y - camera->position.y;
	to.k = point->z - camera->position.z;
	depth = to.i * camera->forward.i + to.j * camera->forward.j + to.k * camera->forward.k;
	if (!(depth > 0.0f))
		return FALSE;
	place->x = (bounds->x0 + bounds->x1) * 0.5f +
		(to.i * right.i + to.j * right.j + to.k * right.k) / depth / tangent_x * width * 0.5f;
	place->y = (bounds->y0 + bounds->y1) * 0.5f -
		(to.i * camera->up.i + to.j * camera->up.j + to.k * camera->up.k) / depth / tangent_y * height * 0.5f;
	return place->x - margin >= bounds->x0 && place->x + margin <= bounds->x1 &&
		place->y - margin >= bounds->y0 && place->y + margin <= bounds->y1;
}

/* whether what the aim stopped at turns the teammate's reticle red, as
Halo's does for the targets its autoaim takes (aim_assist.c's): a living
biped of a team that is the teammate's enemy, not wholly camouflaged nor
one autoaim ignores, within the weapon's reach (distance and range, world
units; no range: never). A held object is its holder's */
static boolean spartan_link_aim_enemy(
	short team_index,
	long object_index,
	real distance,
	real range)
{
	struct object_datum *object = object_try_and_get(object_index);

	if (object && !TEST_FLAG(_object_mask_unit, object->object.type) && object->object.parent_object_index != NONE)
	{
		object_index = object->object.parent_object_index;
		object = object_try_and_get(object_index);
	}
	if (!object || !(range > 0.0f) || !(distance <= range) || !TEST_FLAG(_object_mask_biped, object->object.type))
		return FALSE;
	return !TEST_FLAG(object->object.damage_flags, _object_dead_bit) &&
		!TEST_FLAG(object->object.flags, _object_invisible_bit) &&
		unit_get(object_index)->unit.active_camouflage < 1.0f &&
		game_team_is_enemy(team_index, object->object.owner_team_index) &&
		!TEST_FLAG(biped_definition_get(object->definition_index)->unit.flags, _unit_ignored_by_autoaiming_bit);
}

/* the object at the root of what an object rides */
static long spartan_link_root(
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

/* the first thing along the teammate's aim from origin (forward a unit
vector), out to reach, past their own unit and what it rides: TRUE with
it. A collision test only, which reads the world: no seed, no effect */
static boolean spartan_link_aim_ray(
	long unit_index,
	real_point3d const *origin,
	real_vector3d const *forward,
	real reach,
	struct collision_result *result)
{
	long root_index = spartan_link_root(unit_index);
	real_point3d from = *origin;
	boolean hit = FALSE;
	short attempt;

	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return FALSE;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
	for (attempt = 0; attempt < 4 && reach > 0.0f; attempt++)
	{
		real_vector3d vector;

		vector.i = forward->i * reach;
		vector.j = forward->j * reach;
		vector.k = forward->k * reach;
		hit = collision_test_vector(SPARTAN_LINK_AIM_FLAGS, &from, &vector, root_index, result);
		if (!hit || result->type != _collision_result_object || spartan_link_root(result->object_index) != root_index)
			break;
		/* (their own, or what they ride: on past it) */
		hit = FALSE;
		reach -= reach * result->t + 0.05f;
		from.x = result->point.x + forward->i * 0.05f;
		from.y = result->point.y + forward->j * 0.05f;
		from.z = result->point.z + forward->k * 0.05f;
	}
	--global_current_collision_user_depth;
	return hit;
}

/* ---------- the aim as this machine has it */

/* whether the teammate has an aim their own HUD draws a reticle for
(hud_weapon.c's hud_render_weapon_interface): a weapon of their own, their
seat's when it is a gunner's, or none at all on foot (the default
reticle); not in a seat with no weapon (a driver's or a passenger's, whose
camera follows the vehicle) */
static boolean spartan_link_aims(
	long unit_index)
{
	struct unit_datum *unit = unit_get(unit_index);
	struct unit_datum *parent;
	struct unit_seat const *seat;

	if (unit_inventory_get_weapon(unit_index, unit->unit.current_weapon_index) != NONE)
		return TRUE;
	if (unit->object.parent_object_index == NONE || unit->unit.parent_seat_index == NONE)
		return !unit_get_weapon_count(unit_index);
	parent = unit_try_and_get(unit->object.parent_object_index);
	if (!parent)
		return FALSE;
	seat = TAG_BLOCK_GET_ELEMENT(&unit_definition_get(parent->definition_index)->unit.seats,
		unit->unit.parent_seat_index, struct unit_seat);
	return TEST_FLAG(seat->flags, _unit_seat_gunner_bit) &&
		unit_inventory_get_weapon(unit->object.parent_object_index, parent->unit.current_weapon_index) != NONE;
}

/* how far the teammate's weapon turns the reticle red: its autoaim's reach
at its zoom (aim_assist.c's unit_get_aim_assist_parameters); 0 for none */
static real spartan_link_aim_range(
	long unit_index)
{
	long aiming_index = unit_get_aiming_unit_index(unit_index);
	struct unit_datum *unit;
	struct weapon_definition *definition;
	long weapon_index;
	short zoom_level;

	if (aiming_index == NONE || !unit_try_and_get(aiming_index))
		return 0.0f;
	unit = unit_get(aiming_index);
	weapon_index = unit_inventory_get_weapon(aiming_index, unit->unit.current_weapon_index);
	if (weapon_index == NONE)
		return 0.0f;
	definition = weapon_definition_get(weapon_get(weapon_index)->definition_index);
	zoom_level = unit_get_zoom_level(aiming_index);
	if (zoom_level == NONE && TEST_FLAG(definition->weapon.flags, _weapon_aim_assists_only_when_zoomed_bit))
		return 0.0f;
	return definition->weapon.aim_assist_parameters.autoaim_distance *
		weapon_get_zoom_magnification(weapon_index, zoom_level);
}

/* the teammate's aim as the game has it (spartan_link_update's, outside
the frame's drawing, whose objects are posed between ticks): whether they
have one to show (not down, nor in a seat without a weapon), the point it
stops at, and whether that is an enemy the reticle turns red for */
static void spartan_link_feed_aim_update(
	struct spartan_link_feed *feed)
{
	struct player_datum *teammate = player_try_and_get(feed->player_index);
	struct camera_command command;
	struct collision_result collision;
	real_vector3d forward;
	real range, reach;

	feed->aim_shown = FALSE;
	feed->aim_enemy = FALSE;
	if (!teammate || feed->down_at || !object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit) ||
		TEST_FLAG(object_get(feed->unit_index)->object.damage_flags, _object_dead_bit) ||
		!spartan_link_aims(feed->unit_index))
	{
		return;
	}
	first_person_camera_fake(feed->unit_index, &command);
	forward = command.forward;
	if (normalize3d(&forward) == 0.0f)
		return;
	range = spartan_link_aim_range(feed->unit_index);
	reach = MAX(range, SPARTAN_LINK_AIM_REACH);
	if (spartan_link_aim_ray(feed->unit_index, &command.position, &forward, reach, &collision))
	{
		feed->aim_target = collision.point;
		feed->aim_enemy = collision.type == _collision_result_object && spartan_link_aim_enemy(
			(short)teammate->team_index, collision.object_index, distance3d(&command.position, &collision.point), range);
	}
	else
	{
		point_from_line3d(&command.position, &forward, reach, &feed->aim_target);
	}
	feed->aim_shown = TRUE;
}

/* the teammate's reticle on their feed (bounds), in the window's units:
FALSE for none (no aim to show, or off the feed's picture). Through their
eyes it is the feed's middle (the feed looks along the aim); from behind
them, the point the aim stops at, seen through the feed's camera */
static boolean spartan_link_feed_aim_place(
	struct spartan_link_feed *feed,
	real_rectangle2d const *bounds,
	real margin,
	real_point2d *place)
{
	struct render_camera camera;

	if (!feed->aim_shown || feed->down_at || !object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit))
		return FALSE;
	if (spartan_link_feed_view(feed, &camera))
	{
		place->x = (bounds->x0 + bounds->x1) * 0.5f;
		place->y = (bounds->y0 + bounds->y1) * 0.5f;
		return TRUE;
	}
	return spartan_link_aim_place(&camera, bounds, margin, &feed->aim_target, place);
}

/* ---------- the drawing's pieces */

/* the maps' white texture, NULL where the map has none (menu_shapes.c's) */
static struct bitmap_data *spartan_link_white(
	void)
{
	struct game_globals *game_globals = scenario_get_game_globals();
	struct game_globals_rasterizer_data *rasterizer_data;

	if (!game_globals || !game_globals->rasterizer_data.count)
		return NULL;
	rasterizer_data = TAG_BLOCK_GET_ELEMENT(&game_globals->rasterizer_data, 0, struct game_globals_rasterizer_data);
	return TAG_BLOCK_GET_ELEMENT(&bitmap_group_get(rasterizer_data->default_textures[0].index)->bitmaps, 1,
		struct bitmap_data);
}

/* a quadrilateral, its corners in the window's units (clockwise from the
top left), of the white texture or (u, v from 0 to 1 over bounds) the
feed's image; its corners on the display's pixels' edges (snap), or where
they fall (a reticle's curves) */
/* a quad at x, y (snapped to the display's pixels or not), its texture
coordinates u, v */
static void spartan_link_quad_mapped(
	struct bitmap_data *white,
	real const x[4],
	real const y[4],
	real const u[4],
	real const v[4],
	pixel32 color,
	boolean snap)
{
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	struct dynamic_screen_vertex vertices[4];
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;
	short index;

	for (index = 0; index < 4; index++)
	{
		/* (on the display's pixels' edges: where the screen's unit 0 falls
		is the Xbox's pixels' centres, menu_shapes.c) */
		vertices[index].position.x = (snap ? floorf(x[index] * pixels + 0.5f) / pixels : x[index]) + 0.5f;
		vertices[index].position.y = (snap ? floorf(y[index] * pixels + 0.5f) / pixels : y[index]) + 0.5f;
		vertices[index].texture_coordinates.x = u[index];
		vertices[index].texture_coordinates.y = v[index];
		vertices[index].color = color;
	}
	csmemset(&parameters, 0, sizeof(parameters));
	parameters.map[0] = white;
	parameters.map_texture_scale[0].i = parameters.map_texture_scale[0].j = 1.0f;
	parameters.map_scale[0].i = parameters.map_scale[0].j = 1.0f;
	rasterizer_globals.current_lock_operation = _rasterizer_lock_cinematics;
	rasterizer_psuedo_dynamic_screen_quad_draw(&parameters, vertices);
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;
}

static void spartan_link_quad_snapped(
	struct bitmap_data *white,
	real const x[4],
	real const y[4],
	pixel32 color,
	real_rectangle2d const *bounds,
	boolean snap)
{
	struct rasterizer_dynamic_screen_geometry_parameters parameters;
	struct dynamic_screen_vertex vertices[4];
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;
	short index;

	for (index = 0; index < 4; index++)
	{
		/* (on the display's pixels' edges: where the screen's unit 0 falls
		is the Xbox's pixels' centres, menu_shapes.c) */
		vertices[index].position.x = (snap ? floorf(x[index] * pixels + 0.5f) / pixels : x[index]) + 0.5f;
		vertices[index].position.y = (snap ? floorf(y[index] * pixels + 0.5f) / pixels : y[index]) + 0.5f;
		vertices[index].texture_coordinates.x = bounds ? (x[index] - bounds->x0) / (bounds->x1 - bounds->x0) : 0.0f;
		vertices[index].texture_coordinates.y = bounds ? (y[index] - bounds->y0) / (bounds->y1 - bounds->y0) : 0.0f;
		vertices[index].color = color;
	}
	csmemset(&parameters, 0, sizeof(parameters));
	parameters.map[0] = white;
	parameters.map_texture_scale[0].i = parameters.map_texture_scale[0].j = 1.0f;
	parameters.map_scale[0].i = parameters.map_scale[0].j = 1.0f;
	rasterizer_globals.current_lock_operation = _rasterizer_lock_cinematics;
	rasterizer_psuedo_dynamic_screen_quad_draw(&parameters, vertices);
	rasterizer_globals.current_lock_operation = _rasterizer_lock_none;
}

static void spartan_link_quad(
	struct bitmap_data *white,
	real const x[4],
	real const y[4],
	pixel32 color,
	real_rectangle2d const *bounds)
{
	spartan_link_quad_snapped(white, x, y, color, bounds, TRUE);
}

static void spartan_link_rectangle(
	struct bitmap_data *white,
	real x0,
	real y0,
	real x1,
	real y1,
	pixel32 color)
{
	real x[4], y[4];

	if (x1 <= x0 || y1 <= y0)
		return;
	x[0] = x0; y[0] = y0;
	x[1] = x1; y[1] = y0;
	x[2] = x1; y[2] = y1;
	x[3] = x0; y[3] = y1;
	spartan_link_quad(white, x, y, color, NULL);
}

/* a colour of the HUD's text (its messages': hud_get_text_color), at alpha */
static pixel32 spartan_link_color(
	real alpha,
	real brightness)
{
	real_argb_color text_color;
	real_argb_color const *text = hud_get_text_color(&text_color);
	real_argb_color color;

	color.alpha = alpha;
	color.red = PIN(text->red * brightness, 0.0f, 1.0f);
	color.green = PIN(text->green * brightness, 0.0f, 1.0f);
	color.blue = PIN(text->blue * brightness, 0.0f, 1.0f);
	return real_argb_color_to_pixel32(&color);
}

/* a line of text in the HUD's font, its top at y in the window's units,
scaled: from x (justification 0), ending at x + width (1), or centred over
width (2) */
static void spartan_link_text(
	wchar_t const *text,
	real x,
	real y,
	real width,
	short justification,
	real text_scale,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	struct font_header *font;
	rectangle2d bounds, text_bounds, cursor_bounds;
	real height, text_width, left;

	if (font_index == NONE || !text[0])
		return;
	font = font_definition_get(font_index);
	height = (real)(font->ascending_height + font->descending_height);
	/* (measured unscaled, left-justified; then drawn from where it starts,
	scaled about that corner) */
	draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 2000;
	bounds.y1 = (short)ceilf(height);
	draw_unicode_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);
	text_width = (real)(text_bounds.x1 - text_bounds.x0) * text_scale;
	left = justification == 1 ? x + width - text_width : justification == 2 ? x + (width - text_width) * 0.5f : x;
	bounds.x0 = (short)floorf(left);
	bounds.y0 = (short)floorf(y);
	bounds.x1 = (short)(bounds.x0 + 2000);
	bounds.y1 = (short)ceilf(y + height);
	rasterizer_text_set_scale(text_scale, (real)bounds.x0, (real)bounds.y0);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

/* the motion tracker's own color in a window: its picture's tint in the
local player's unit HUD (the motion sensor's background), its alpha its
translucency; a tint without a hue of its own (the stock HUD's white, the
picture's blue its bitmap's) takes the HUD's blue, at the tint's alpha */
static pixel32 spartan_link_tracker_color(
	short local_player_index)
{
	long self = local_player_get_player_index(local_player_index);
	struct unit_datum *unit = self != NONE ? unit_try_and_get(player_get(self)->unit_index) : NULL;
	pixel32 color = 0xFFFFFFFFUL;
	long hud_index;
	real_argb_color tint;
	real highest, lowest;

	if (unit)
	{
		hud_index = unit_definition_get_active_hud_index(unit_definition_get(unit->definition_index),
			local_player_count() > 1);
		if (hud_index != NONE)
			color = unit_hud_interface_definition_get(hud_index)->motion_sensor.background.colors.color;
	}
	pixel32_to_real_argb_color(color, &tint);
	highest = MAX(tint.red, MAX(tint.green, tint.blue));
	lowest = MIN(tint.red, MIN(tint.green, tint.blue));
	if (highest - lowest < 0.15f)
		return spartan_link_color(tint.alpha > 0.0f ? tint.alpha : 1.0f, 1.0f);
	return color;
}

/* The feed's image warped to the teammate's look of this frame, where it
was drawn in an earlier frame (debug.spartan_link_feed_rate's, a few times a
second, which a display of 120 Hz or more shows as a picture that turns in
steps; one drawn this frame is shown as drawn). Each frame shows it turned by the
camera's turn since it was drawn (the feed's look, smoothed:
spartan_link_feed_look): a point of the feed's picture, a direction from
the camera now, is where that direction fell in the image, on a grid of
SPARTAN_LINK_WARP_CELLS a side (a turn of a few degrees bends the picture
less than a grid cell's straight edges show). Only the turn: the camera's
move since (a tick's walk) is not, which needs the image's depth. What the
image has none of (the edge turned into view) stretches its edge. None
(the image as drawn) where the camera cut since: another unit (a respawn),
a change through their eyes or from behind, a jump of the camera's place
(a teleport) or of its look, a zoom, or an image a while old */
#define SPARTAN_LINK_WARP_CELLS 4
#define SPARTAN_LINK_WARP_JUMP 3.0f
#define SPARTAN_LINK_WARP_TURN_COSINE 0.866f
#define SPARTAN_LINK_WARP_OLDEST 250

static boolean spartan_link_feed_warp(
	struct spartan_link_feed *feed,
	real_rectangle2d const *bounds,
	real u[SPARTAN_LINK_WARP_CELLS + 1][SPARTAN_LINK_WARP_CELLS + 1],
	real v[SPARTAN_LINK_WARP_CELLS + 1][SPARTAN_LINK_WARP_CELLS + 1])
{
	struct render_camera camera;
	real_vector3d right, image_right;
	real width = bounds->x1 - bounds->x0;
	real height = bounds->y1 - bounds->y0;
	real tangent_y, tangent_x, image_tangent_x;
	real_vector3d moved;
	short row, column;
	boolean turned = FALSE;

	if (!feed->image_valid || feed->image_frame == spartan_link.frame_serial || feed->down_at ||
		feed->image_unit != feed->unit_index ||
		!object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit) ||
		!(width > 0.0f) || !(height > 0.0f) || !feed->image_height || !(feed->image_tangent > 0.0f) ||
		system_milliseconds() - feed->image_at > SPARTAN_LINK_WARP_OLDEST ||
		(long)(feed->look_cut_at - feed->image_at) > 0)
	{
		return FALSE;
	}
	if (spartan_link_feed_view(feed, &camera) != feed->image_through_eyes)
		return FALSE;
	tangent_y = (real)tan(camera.vertical_field_of_view * 0.5f);
	if (!(fabsf(tangent_y - feed->image_tangent) <= feed->image_tangent * 0.02f))
		return FALSE;
	moved.i = camera.position.x - feed->image_position.x;
	moved.j = camera.position.y - feed->image_position.y;
	moved.k = camera.position.z - feed->image_position.z;
	if (!(moved.i * moved.i + moved.j * moved.j + moved.k * moved.k <= SPARTAN_LINK_WARP_JUMP * SPARTAN_LINK_WARP_JUMP))
		return FALSE;
	if (!(camera.forward.i * feed->image_forward.i + camera.forward.j * feed->image_forward.j +
		camera.forward.k * feed->image_forward.k >= SPARTAN_LINK_WARP_TURN_COSINE))
	{
		return FALSE;
	}
	if (camera.forward.i == feed->image_forward.i && camera.forward.j == feed->image_forward.j &&
		camera.forward.k == feed->image_forward.k && camera.up.i == feed->image_up.i &&
		camera.up.j == feed->image_up.j && camera.up.k == feed->image_up.k)
	{
		return FALSE;
	}
	tangent_x = tangent_y * width / height;
	image_tangent_x = feed->image_tangent * (real)feed->image_width / (real)feed->image_height;
	/* (each camera's right: forward across up, as spartan_link_aim_place's) */
	right.i = camera.forward.j * camera.up.k - camera.forward.k * camera.up.j;
	right.j = camera.forward.k * camera.up.i - camera.forward.i * camera.up.k;
	right.k = camera.forward.i * camera.up.j - camera.forward.j * camera.up.i;
	image_right.i = feed->image_forward.j * feed->image_up.k - feed->image_forward.k * feed->image_up.j;
	image_right.j = feed->image_forward.k * feed->image_up.i - feed->image_forward.i * feed->image_up.k;
	image_right.k = feed->image_forward.i * feed->image_up.j - feed->image_forward.j * feed->image_up.i;
	for (row = 0; row <= SPARTAN_LINK_WARP_CELLS; row++)
	{
		real screen_y = 1.0f - 2.0f * (real)row / SPARTAN_LINK_WARP_CELLS;

		for (column = 0; column <= SPARTAN_LINK_WARP_CELLS; column++)
		{
			real screen_x = 2.0f * (real)column / SPARTAN_LINK_WARP_CELLS - 1.0f;
			real_vector3d direction;
			real depth, image_x, image_y;

			direction.i = camera.forward.i + right.i * screen_x * tangent_x + camera.up.i * screen_y * tangent_y;
			direction.j = camera.forward.j + right.j * screen_x * tangent_x + camera.up.j * screen_y * tangent_y;
			direction.k = camera.forward.k + right.k * screen_x * tangent_x + camera.up.k * screen_y * tangent_y;
			depth = direction.i * feed->image_forward.i + direction.j * feed->image_forward.j +
				direction.k * feed->image_forward.k;
			if (!(depth > 0.01f))
				return FALSE;
			image_x = (direction.i * image_right.i + direction.j * image_right.j + direction.k * image_right.k) /
				depth / image_tangent_x;
			image_y = (direction.i * feed->image_up.i + direction.j * feed->image_up.j +
				direction.k * feed->image_up.k) / depth / feed->image_tangent;
			u[row][column] = PIN((image_x + 1.0f) * 0.5f, 0.0f, 1.0f);
			v[row][column] = PIN((1.0f - image_y) * 0.5f, 0.0f, 1.0f);
			if (fabsf(u[row][column] - (real)column / SPARTAN_LINK_WARP_CELLS) > 0.0005f ||
				fabsf(v[row][column] - (real)row / SPARTAN_LINK_WARP_CELLS) > 0.0005f)
			{
				turned = TRUE;
			}
		}
	}
	return turned;
}

/* the feed's image over bounds, warped to this frame's look where it can
be (spartan_link_feed_warp), else as it was drawn */
static void spartan_link_feed_image_draw(
	struct bitmap_data *white,
	struct spartan_link_feed *feed,
	real x[4],
	real y[4],
	real_rectangle2d const *bounds,
	pixel32 color)
{
	real u[SPARTAN_LINK_WARP_CELLS + 1][SPARTAN_LINK_WARP_CELLS + 1];
	real v[SPARTAN_LINK_WARP_CELLS + 1][SPARTAN_LINK_WARP_CELLS + 1];
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;
	real x0, y0, x1, y1;
	short row, column;

	if (!spartan_link_feed_warp(feed, bounds, u, v))
	{
		spartan_link_quad(white, x, y, color, bounds);
		return;
	}
	/* (the grid between the bounds' edges on the display's pixels, as the
	quad's corners are put) */
	x0 = floorf(bounds->x0 * pixels + 0.5f) / pixels;
	y0 = floorf(bounds->y0 * pixels + 0.5f) / pixels;
	x1 = floorf(bounds->x1 * pixels + 0.5f) / pixels;
	y1 = floorf(bounds->y1 * pixels + 0.5f) / pixels;
	for (row = 0; row < SPARTAN_LINK_WARP_CELLS; row++)
	{
		for (column = 0; column < SPARTAN_LINK_WARP_CELLS; column++)
		{
			real cell_x[4], cell_y[4], cell_u[4], cell_v[4];
			real left = x0 + (x1 - x0) * (real)column / SPARTAN_LINK_WARP_CELLS;
			real right = x0 + (x1 - x0) * (real)(column + 1) / SPARTAN_LINK_WARP_CELLS;
			real top = y0 + (y1 - y0) * (real)row / SPARTAN_LINK_WARP_CELLS;
			real bottom = y0 + (y1 - y0) * (real)(row + 1) / SPARTAN_LINK_WARP_CELLS;

			cell_x[0] = left; cell_y[0] = top; cell_u[0] = u[row][column]; cell_v[0] = v[row][column];
			cell_x[1] = right; cell_y[1] = top; cell_u[1] = u[row][column + 1]; cell_v[1] = v[row][column + 1];
			cell_x[2] = right; cell_y[2] = bottom; cell_u[2] = u[row + 1][column + 1]; cell_v[2] = v[row + 1][column + 1];
			cell_x[3] = left; cell_y[3] = bottom; cell_u[3] = u[row + 1][column]; cell_v[3] = v[row + 1][column];
			spartan_link_quad_mapped(white, cell_x, cell_y, cell_u, cell_v, color, FALSE);
		}
	}
}

/* the frame about a feed: its image in its own colors (or, never drawn,
dark glass), and an upright edge in the motion tracker's color, line and translucency
(spartan_link_tracker_color), with a soft glow about it as the tracker's
ring has, so the feeds read as part of its cluster */
static void spartan_link_draw_feed(
	struct bitmap_data *white,
	short local_player_index,
	short index,
	real_rectangle2d const *bounds,
	real scale)
{
	struct spartan_link_feed *feed = &spartan_link.windows[local_player_index].feeds[index];
	int view = SPARTAN_LINK_VIEW(local_player_index, index);
	real pixel = 1.0f / (halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f);
	real edge = MAX(pixel, 0.6f * scale);
	real glow = edge * 2.5f;
	real x[4], y[4];
	pixel32 tracker = spartan_link_tracker_color(local_player_index);
	pixel32 edge_color = tracker;
	pixel32 glow_color = (tracker & 0x00FFFFFFUL) | ((pixel32)(((tracker >> 24) & 0xFF) / 4) << 24);
	unsigned long age;
	short side;

	/* (their ping: the frame brightened, the tracker's color made paler and
	whole, its edge thicker and its glow wider and stronger, fading back over
	the last of it) */
	if (spartan_link_pinged(local_player_index, feed->player_index, &age))
	{
		real lit = age + SPARTAN_LINK_PING_FADE_MILLISECONDS < SPARTAN_LINK_PING_MILLISECONDS ? 1.0f :
			(real)(SPARTAN_LINK_PING_MILLISECONDS - age) / (real)SPARTAN_LINK_PING_FADE_MILLISECONDS;
		real_argb_color color;

		pixel32_to_real_argb_color(tracker, &color);
		color.red += (1.0f - color.red) * 0.55f * lit;
		color.green += (1.0f - color.green) * 0.55f * lit;
		color.blue += (1.0f - color.blue) * 0.55f * lit;
		color.alpha += (1.0f - color.alpha) * lit;
		edge_color = real_argb_color_to_pixel32(&color);
		color.alpha *= 0.25f + 0.35f * lit;
		glow_color = real_argb_color_to_pixel32(&color);
		edge *= 1.0f + lit;
		glow *= 1.0f + 0.6f * lit;
	}

	/* the image in its own colors (a down teammate's darkened) */
	x[0] = bounds->x0; y[0] = bounds->y0;
	x[1] = bounds->x1; y[1] = bounds->y0;
	x[2] = bounds->x1; y[2] = bounds->y1;
	x[3] = bounds->x0; y[3] = bounds->y1;
	if (spartan_link_feed_image(local_player_index, index))
	{
		halo_view_sample(view);
		spartan_link_feed_image_draw(white, feed, x, y, bounds, feed->down_at ? 0xFF585858UL : 0xFFFFFFFFUL);
		halo_view_sample(0);
	}
	else
	{
		spartan_link_quad(white, x, y, 0xC0081018UL, NULL);
	}
	/* the glow (outside the edge, faint), then the edge: top, bottom, left,
	right */
	for (side = 0; side < 2; side++)
	{
		real out = side == 0 ? glow : 0.0f;
		real width = side == 0 ? glow : edge;
		pixel32 color = side == 0 ? glow_color : edge_color;

		spartan_link_rectangle(white, bounds->x0 - out, bounds->y0 - out, bounds->x1 + out, bounds->y0 - out + width, color);
		spartan_link_rectangle(white, bounds->x0 - out, bounds->y1 + out - width, bounds->x1 + out, bounds->y1 + out, color);
		spartan_link_rectangle(white, bounds->x0 - out, bounds->y0 - out + width, bounds->x0 - out + width,
			bounds->y1 + out - width, color);
		spartan_link_rectangle(white, bounds->x1 + out - width, bounds->y0 - out + width, bounds->x1 + out,
			bounds->y1 + out - width, color);
	}
}

/* the reticle's radius on a feed: a twentieth of its height, three of the
display's pixels at least */
static real spartan_link_reticle_radius(
	real_rectangle2d const *bounds)
{
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;

	return MAX((bounds->y1 - bounds->y0) * SPARTAN_LINK_RETICLE_SIZE, 3.0f / pixels);
}

/* the teammate's reticle at place: a dot and a thin ring about it, in the
HUD's blue, or red on an enemy */
static void spartan_link_draw_reticle(
	struct bitmap_data *white,
	real_point2d const *place,
	real radius,
	boolean enemy)
{
	real pixels = halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f;
	real thickness = MAX(radius * 0.18f, 1.0f / pixels);
	real dot = MAX(radius * 0.22f, 1.0f / pixels);
	pixel32 color = enemy ? SPARTAN_LINK_RETICLE_RED : spartan_link_color(0.95f, 1.0f);
	real x[4], y[4];
	short index;

	for (index = 0; index < SPARTAN_LINK_RETICLE_SEGMENTS; index++)
	{
		real angle0 = (real)index * 2.0f * _pi / SPARTAN_LINK_RETICLE_SEGMENTS;
		real angle1 = (real)(index + 1) * 2.0f * _pi / SPARTAN_LINK_RETICLE_SEGMENTS;

		x[0] = place->x + cosf(angle0) * radius; y[0] = place->y + sinf(angle0) * radius;
		x[1] = place->x + cosf(angle1) * radius; y[1] = place->y + sinf(angle1) * radius;
		x[2] = place->x + cosf(angle1) * (radius - thickness); y[2] = place->y + sinf(angle1) * (radius - thickness);
		x[3] = place->x + cosf(angle0) * (radius - thickness); y[3] = place->y + sinf(angle0) * (radius - thickness);
		spartan_link_quad_snapped(white, x, y, color, NULL, FALSE);
	}
	/* (the dot: an octagon, its middle band and the two ends) */
	{
		real side = dot * 0.4142f;

		spartan_link_rectangle(white, place->x - dot, place->y - side, place->x + dot, place->y + side, color);
		x[0] = place->x - side; y[0] = place->y - dot;
		x[1] = place->x + side; y[1] = place->y - dot;
		x[2] = place->x + dot; y[2] = place->y - side;
		x[3] = place->x - dot; y[3] = place->y - side;
		spartan_link_quad_snapped(white, x, y, color, NULL, FALSE);
		x[0] = place->x - dot; y[0] = place->y + side;
		x[1] = place->x + dot; y[1] = place->y + side;
		x[2] = place->x + side; y[2] = place->y + dot;
		x[3] = place->x - side; y[3] = place->y + dot;
		spartan_link_quad_snapped(white, x, y, color, NULL, FALSE);
	}
}

/* the HUD's health meter's colour at a vitality, from the unit's own HUD
(hud_unit.c's: its full colour at or above the meter's upper cutoff, its
empty colour at or below the lower, its middle colour between), and while
flashing (since flash_at) the meter's background's flash, as the HUD's
health panel flashes while the shield is down; the HUD's text colour where
the unit has none */
static pixel32 spartan_link_health_color(
	long unit_index,
	real vitality,
	unsigned long flash_at)
{
	struct unit_definition *definition = unit_definition_get(unit_get(unit_index)->definition_index);
	long hud_index = unit_definition_get_active_hud_index(definition, FALSE);
	struct unit_hud_interface_definition const *hud;
	struct hud_color_definition const *flash;
	real_argb_color color, flash_color;
	real fraction = 0.0f;

	if (hud_index == NONE)
		return spartan_link_color(0.95f, 1.0f);
	hud = unit_hud_interface_definition_get(hud_index);
	pixel32_to_real_argb_color(
		vitality >= hud->health_meter.health_extras.max_cutoff ? hud->health_meter.meter.max_color :
		vitality <= hud->health_meter.health_extras.min_cutoff ? hud->health_meter.meter.min_color :
		hud->health_meter.health_extras.mid_color, &color);
	color.alpha = 0.95f;
	/* (the flash: get_flash_color's pulse, its period's first flashes) */
	flash = &hud->health_meter.background.colors;
	if (flash_at && flash->flash_period > 0.0f && flash->flash_length > 0.0f)
	{
		real phase = fmodf((real)(system_milliseconds() - flash_at) * 0.001f, flash->flash_period);
		real cycle = flash->flash_delay + flash->flash_length;

		if (phase < flash->number_of_flashes * cycle)
		{
			phase = fmodf(phase, cycle);
			if (phase < flash->flash_length)
				fraction = sqrtf(PIN(1.0f - (cosf(phase / flash->flash_length * 2.0f * _pi) + 1.0f) * 0.5f, 0.0f, 1.0f));
		}
	}
	pixel32_to_real_argb_color(flash->flash_color, &flash_color);
	color.red += (flash_color.red - color.red) * fraction;
	color.green += (flash_color.green - color.green) * fraction;
	color.blue += (flash_color.blue - color.blue) * fraction;
	return real_argb_color_to_pixel32(&color);
}

/* a line's width in the HUD's font, unscaled (spartan_link_text's units) */
static real spartan_link_text_width(
	wchar_t const *text)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds, text_bounds, cursor_bounds;

	if (font_index == NONE || !text[0])
		return 0.0f;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, global_real_argb_white);
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 2000;
	bounds.y1 = 100;
	draw_unicode_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);
	return (real)(text_bounds.x1 - text_bounds.x0);
}

/* ---------- the teammate's shield and health, the HUD's own */

/* (bitmap_group.c's: a group of interface bitmaps, whose sprites' rectangles
are in pixels) */
#define SPARTAN_LINK_INTERFACE_BITMAPS 4

/* a HUD element's rectangle in the view's units as the HUD draws it in this
window (hud_draw.c's hud_calculate_point and its bounds, at the HUD's
scale): FALSE for no picture, or an anchor of MCC's HUD the link does not
place */
static boolean spartan_link_element_rect(
	short corner,
	struct hud_placement_definition const *placement,
	long bitmap_group_index,
	short sequence_index,
	real_rectangle2d *rect)
{
	rectangle2d const *window = &render.camera.window_bounds;
	rectangle2d const *viewport = &render.camera.viewport_bounds;
	struct bitmap_data const *bitmap = NULL;
	real_rectangle2d const *clip = NULL;
	real hud_scale = hud_globals_get_scale(FALSE);
	real width, height, x, y;
	boolean right = corner == _hud_anchor_top_right || corner == _hud_anchor_bottom_right;
	boolean bottom = corner == _hud_anchor_bottom_left || corner == _hud_anchor_bottom_right;

	if (bitmap_group_index == NONE || corner < _hud_anchor_top_left || corner > _hud_anchor_center)
		return FALSE;
	hud_retrieve_bitmap_and_bounding_rect(bitmap_group_index, sequence_index, 0, &bitmap, &clip);
	if (!bitmap)
		return FALSE;
	if (clip && bitmap_group_get(bitmap_group_index)->type == SPARTAN_LINK_INTERFACE_BITMAPS)
	{
		width = clip->x1 - clip->x0;
		height = clip->y1 - clip->y0;
	}
	else if (clip)
	{
		width = (clip->x1 - clip->x0) * bitmap->width;
		height = (clip->y1 - clip->y0) * bitmap->height;
	}
	else
	{
		width = (real)bitmap->width;
		height = (real)bitmap->height;
	}
	width *= placement->scale.i * hud_scale;
	height *= placement->scale.j * hud_scale;
	if (!(width > 0.0f) || !(height > 0.0f))
		return FALSE;
	if (corner == _hud_anchor_center)
	{
		x = (real)((window->x0 + window->x1) / 2 - viewport->x0) + placement->offset.x * hud_scale - width * 0.5f;
		y = (real)((window->y0 + window->y1) / 2 - viewport->y0) + placement->offset.y * hud_scale - height * 0.5f;
	}
	else
	{
		x = (right ? window->x1 - placement->offset.x * hud_scale - width : window->x0 + placement->offset.x * hud_scale) -
			viewport->x0;
		y = (bottom ? window->y1 - placement->offset.y * hud_scale - height : window->y0 + placement->offset.y * hud_scale) -
			viewport->y0;
	}
	rect->x0 = x;
	rect->y0 = y;
	rect->x1 = x + width;
	rect->y1 = y + height;
	return TRUE;
}

/* an element's placement moved from where the HUD draws it (from) to where
the link draws it, the HUD's whole panel (from the union of its pieces) made
k as large and its corner put at the target's: anchored at the top left, its
offset and scale */
static void spartan_link_element_moved(
	struct hud_placement_definition const *placement,
	real_rectangle2d const *from,
	real_rectangle2d const *pieces,
	real_point2d const *target,
	real k,
	struct hud_placement_definition *moved)
{
	real hud_scale = hud_globals_get_scale(FALSE);
	real x = target->x + (from->x0 - pieces->x0) * k -
		(real)(render.camera.window_bounds.x0 - render.camera.viewport_bounds.x0);
	real y = target->y + (from->y0 - pieces->y0) * k -
		(real)(render.camera.window_bounds.y0 - render.camera.viewport_bounds.y0);

	*moved = *placement;
	moved->offset.x = (short)floorf(x / hud_scale + 0.5f);
	moved->offset.y = (short)floorf(y / hud_scale + 0.5f);
	moved->scale.i = placement->scale.i * k;
	moved->scale.j = placement->scale.j * k;
	moved->multiplayer_scaling_flags = 0;
}

/* the teammate's shield and health as their unit's HUD draws them (its
interface's shield and health meters and their panels' pictures, colors and
flashing: hud_unit.c's), made small enough for the room under their feed
(bounds), the panel's proportions kept, at the room's right (its drawn
left given back); FALSE where their HUD has no meter
to draw (the caller draws bars of its own) */
static boolean spartan_link_draw_meters(
	short local_player_index,
	struct spartan_link_feed *feed,
	real_rectangle2d const *bounds,
	real *left)
{
	struct unit_datum *unit = unit_try_and_get(feed->unit_index);
	struct unit_hud_interface_definition const *hud;
	struct hud_absolute_placement_definition top_left;
	struct metered_panel_definition const *panels[2];
	real_rectangle2d meter_rects[2], background_rects[2], pieces;
	boolean meter_drawn[2], background_drawn[2];
	real_point2d target;
	real k, vitality[2];
	short draw_flags[2];
	short index;
	long hud_index;
	boolean any = FALSE;

	if (!unit || !(bounds->x1 > bounds->x0) || !(bounds->y1 > bounds->y0))
		return FALSE;
	hud_index = unit_definition_get_active_hud_index(unit_definition_get(unit->definition_index), FALSE);
	if (hud_index == NONE)
		return FALSE;
	hud = unit_hud_interface_definition_get(hud_index);
	panels[0] = &hud->shield_meter;
	panels[1] = &hud->health_meter;
	vitality[0] = PIN(unit->object.shield_vitality, 0.0f, 1.0f);
	vitality[1] = PIN(unit->object.body_vitality, 0.0f, 1.0f);
	/* (the HUD's flashing: the shield's below a quarter, the health's while
	the shield is down; from the tick each began) */
	draw_flags[0] = unit->object.shield_vitality < 0.25f;
	draw_flags[1] = TEST_FLAG(unit->object.damage_flags, _object_shield_depleted_bit) != 0;
	if (!draw_flags[0])
		feed->shield_flash_tick = 0;
	else if (!feed->shield_flash_tick)
		feed->shield_flash_tick = MAX(game_time_get(), 1);
	if (!draw_flags[1])
		feed->health_flash_tick = 0;
	else if (!feed->health_flash_tick)
		feed->health_flash_tick = MAX(game_time_get(), 1);
	for (index = 0; index < 2; index++)
	{
		meter_drawn[index] = spartan_link_element_rect(hud->absolute_placement.corner, &panels[index]->meter.placement,
			panels[index]->meter.meter_bitmap.index, panels[index]->meter.sequence_index, &meter_rects[index]);
		background_drawn[index] = meter_drawn[index] && spartan_link_element_rect(hud->absolute_placement.corner,
			&panels[index]->background.placement, panels[index]->background.interface_bitmap.index,
			panels[index]->background.sequence_index, &background_rects[index]);
		if (meter_drawn[index])
		{
			if (!any)
				pieces = meter_rects[index];
			any = TRUE;
			pieces.x0 = MIN(pieces.x0, meter_rects[index].x0);
			pieces.y0 = MIN(pieces.y0, meter_rects[index].y0);
			pieces.x1 = MAX(pieces.x1, meter_rects[index].x1);
			pieces.y1 = MAX(pieces.y1, meter_rects[index].y1);
		}
		if (background_drawn[index])
		{
			pieces.x0 = MIN(pieces.x0, background_rects[index].x0);
			pieces.y0 = MIN(pieces.y0, background_rects[index].y0);
			pieces.x1 = MAX(pieces.x1, background_rects[index].x1);
			pieces.y1 = MAX(pieces.y1, background_rects[index].y1);
		}
	}
	/* (both meters, or bars: never one of each) */
	if (!meter_drawn[0] || !meter_drawn[1] || !(pieces.x1 > pieces.x0) || !(pieces.y1 > pieces.y0))
		return FALSE;
	k = MIN((bounds->x1 - bounds->x0) / (pieces.x1 - pieces.x0), (bounds->y1 - bounds->y0) / (pieces.y1 - pieces.y0));
	target.x = bounds->x1 - (pieces.x1 - pieces.x0) * k;
	*left = target.x;
	target.y = (bounds->y0 + bounds->y1 - (pieces.y1 - pieces.y0) * k) * 0.5f;
	csmemset(&top_left, 0, sizeof(top_left));
	top_left.corner = _hud_anchor_top_left;
	for (index = 0; index < 2; index++)
	{
		struct meter_hud_element_definition meter = panels[index]->meter;
		short value_scale = meter.value_scale ? meter.value_scale : index == 0 ? UNSIGNED_CHAR_MAX : 8;
		byte value = (byte)PIN((long)floorf((real)value_scale * vitality[index]), 0, UNSIGNED_CHAR_MAX);

		/* (the health's color by its cutoffs, as the HUD's) */
		if (index == 1)
		{
			struct health_meter_extras_definition const *extras = &panels[1]->health_extras;

			if (unit->object.body_vitality >= extras->max_cutoff)
				meter.min_color = meter.max_color;
			else if (unit->object.body_vitality <= extras->min_cutoff)
				meter.max_color = meter.min_color;
			else
				meter.min_color = meter.max_color = extras->mid_color;
		}
		spartan_link_element_moved(&panels[index]->meter.placement, &meter_rects[index], &pieces, &target, k,
			&meter.placement);
		hud_draw_meter(local_player_index, &top_left, &meter, value, value, draw_flags[index], -1.0f,
			vitality[index]);
		if (background_drawn[index])
		{
			struct static_hud_element_definition background = panels[index]->background;

			spartan_link_element_moved(&panels[index]->background.placement, &background_rects[index], &pieces,
				&target, k, &background.placement);
			/* (its multitexture overlays left out: the HUD's own corner's) */
			background.multitexture_overlays.count = 0;
			hud_draw_static_element(local_player_index, &top_left, &background, draw_flags[index],
				index == 0 ? feed->shield_flash_tick : feed->health_flash_tick);
		}
	}
	return TRUE;
}

/* under a feed, after a small gap, within its width: the teammate's name at
its left edge (in the look of the tracker's range label), and
at its right edge their shield and health as their HUD draws them
(spartan_link_draw_meters; else thin bars of the shield and under it the
health), or DOWN; a clear gap between (the name cut short where it would
reach the meters) */
static void spartan_link_draw_row(
	struct bitmap_data *white,
	short local_player_index,
	short index,
	real_rectangle2d const *bounds,
	real scale)
{
	struct spartan_link_feed *feed = &spartan_link.windows[local_player_index].feeds[index];
	struct player_datum *teammate = player_try_and_get(feed->player_index);
	real pixel = 1.0f / (halo_screen_pixel_scale() > 0.0f ? halo_screen_pixel_scale() : 1.0f);
	real_argb_color text_color;
	real width = bounds->x1 - bounds->x0;
	real top = bounds->y1 + SPARTAN_LINK_STRIP_GAP * scale;
	real gap = 4.0f * scale;
	/* (the text as large as the design's at a feed's width of four, smaller
	in a narrower feed, to a point) */
	real text_scale = 0.6f * MAX(scale, 0.9f) * MAX(spartan_link.size, 0.9f) *
		PIN(width / (SPARTAN_LINK_NARROWEST * scale), 0.75f, 1.0f);
	real shield = 0.0f, health = 0.0f;
	real bar_height = MAX(2.0f * scale, pixel);
	real health_height = MAX(1.5f * scale, pixel);
	real right_left = bounds->x1;
	real name_width;
	wchar_t name[16];
	short length;

	if (!teammate)
		return;
	/* (the tracker's range label's look) */
	pixel32_to_real_argb_color(spartan_link_tracker_color(local_player_index), &text_color);
	text_color.red += (1.0f - text_color.red) * SPARTAN_LINK_NAME_PALER;
	text_color.green += (1.0f - text_color.green) * SPARTAN_LINK_NAME_PALER;
	text_color.blue += (1.0f - text_color.blue) * SPARTAN_LINK_NAME_PALER;
	text_color.alpha = SPARTAN_LINK_NAME_ALPHA;
	ustrncpy(name, teammate->name, NUMBEROF(name) - 1);
	name[NUMBEROF(name) - 1] = 0;
	if (!feed->down_at && object_try_and_get(feed->unit_index))
	{
		shield = PIN(object_get(feed->unit_index)->object.shield_vitality, 0.0f, 1.0f);
		health = PIN(object_get(feed->unit_index)->object.body_vitality, 0.0f, 1.0f);
	}
	/* (the health flashing while the shield is down, as the HUD's: from
	when it went down) */
	if (!feed->down_at && object_try_and_get(feed->unit_index) &&
		TEST_FLAG(object_get(feed->unit_index)->object.damage_flags, _object_shield_depleted_bit))
	{
		if (!feed->health_flash_at)
			feed->health_flash_at = system_milliseconds() ? system_milliseconds() : 1;
	}
	else
	{
		feed->health_flash_at = 0;
	}
	/* (the right: DOWN, else the HUD's own meters in at most half the
	width, the row's height; else bars) */
	if (feed->down_at)
	{
		real down_width = spartan_link_text_width(L"DOWN") * text_scale;

		right_left = bounds->x1 - down_width;
		spartan_link_text(L"DOWN", right_left, top, down_width, 0, text_scale, &text_color);
	}
	else
	{
		real_rectangle2d meters;

		meters.x0 = bounds->x1 - width * 0.5f;
		meters.x1 = bounds->x1;
		meters.y0 = top + 0.5f * scale;
		meters.y1 = top + (SPARTAN_LINK_ROW - SPARTAN_LINK_STRIP_GAP) * scale;
		if (!spartan_link_draw_meters(local_player_index, feed, &meters, &right_left))
		{
			real bar_x0 = bounds->x1 - width * 0.4f;
			real bar_y = top + SPARTAN_LINK_ROW * scale * 0.30f;
			real health_y = bar_y + bar_height + MAX(1.25f * scale, 2.0f * pixel);
			pixel32 health_color = object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit) ?
				spartan_link_health_color(feed->unit_index, health, feed->health_flash_at) :
				spartan_link_color(0.95f, 1.0f);

			right_left = bar_x0;
			spartan_link_rectangle(white, bar_x0, bar_y, bounds->x1, bar_y + bar_height, spartan_link_color(0.35f, 0.6f));
			spartan_link_rectangle(white, bar_x0, bar_y, bar_x0 + (bounds->x1 - bar_x0) * shield, bar_y + bar_height,
				spartan_link_color(0.95f, 1.0f));
			spartan_link_rectangle(white, bar_x0, health_y, bounds->x1, health_y + health_height,
				(health_color & 0x00FFFFFFUL) | 0x40000000UL);
			spartan_link_rectangle(white, bar_x0, health_y, bar_x0 + (bounds->x1 - bar_x0) * health,
				health_y + health_height, health_color);
		}
	}
	/* (campaign scoring: their score before the meters, brighter than the
	name, where it fits) */
	if (campaign_score_hud_active())
	{
		struct campaign_score_player const *entry = campaign_score_hud_player(feed->player_index);
		wchar_t number[16];
		real number_width;
		real_argb_color score_color = text_color;

		campaign_score_hud_number(entry ? entry->score : 0, number, NUMBEROF(number));
		number_width = spartan_link_text_width(number) * text_scale;
		if (bounds->x0 + number_width + 3.0f * gap <= right_left)
		{
			right_left -= number_width + gap;
			score_color.alpha = 1.0f;
			spartan_link_text(number, right_left, top, number_width, 0, text_scale, &score_color);
		}
	}
	/* (the name as long as fits before the gap) */
	for (length = (short)ustrlen(name); length > 0; length--)
	{
		name[length] = 0;
		name_width = spartan_link_text_width(name) * text_scale;
		if (bounds->x0 + name_width + gap <= right_left)
			break;
	}
	name_width = spartan_link_text_width(name) * text_scale;
	spartan_link_text(name, bounds->x0, top, name_width, 0, text_scale, &text_color);
}

/* ---------- the teammate's ping on their feed */

/* whether the ping's point is behind something from the teammate's eye
(hud_nav_points.c's hud_get_nav_point_render_type, from their side): the
level or an object other than the pinged one (or what it rides) between. A
collision test only, which reads the world */
static boolean spartan_link_ping_behind(
	long unit_index,
	real_point3d const *eye,
	real_point3d const *position,
	long object_index)
{
	struct collision_result result;
	real_vector3d vector;
	boolean behind;

	if (global_current_collision_user_depth >= MAXIMUM_COLLISION_USER_STACK_DEPTH)
		return FALSE;
	global_current_collision_users[global_current_collision_user_depth++] = _collision_user_ui;
	vector.i = position->x - eye->x;
	vector.j = position->y - eye->y;
	vector.k = position->z - eye->z;
	behind = collision_test_vector(_collision_test_for_line_of_sight_flags, eye, &vector,
		spartan_link_root(unit_index), &result) &&
		(result.type != _collision_result_object || object_index == NONE ||
			spartan_link_root(result.object_index) != spartan_link_root(object_index));
	--global_current_collision_user_depth;
	return behind;
}

/* the teammate's ping on their feed (bounds), where it is in their view:
the world's arrow (network_markers.c's: in their colour and fade, its
behind-a-wall look from their eye), sized to the feed, over its object as
it moves; none where it is off the feed's picture */
static void spartan_link_draw_ping(
	short local_player_index,
	struct spartan_link_feed *feed,
	real_rectangle2d const *bounds)
{
	struct network_marker_shown ping;
	struct render_camera camera;
	real_point3d eye;
	real_point2d place;
	real distance, size, height;

	if (feed->down_at || !object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit) ||
		!network_markers_ping_shown(local_player_index, feed->player_index, &ping) || ping.nav_index == NONE)
	{
		return;
	}
	spartan_link_feed_view(feed, &camera);
	render_interpolation_unit_camera_position(feed->unit_index, &eye);
	/* (as the world's arrow is sized: its least past 15 units, larger
	nearer, hud_nav_points.c's) */
	distance = distance3d(&eye, &ping.position);
	size = distance > 15.0f ? 0.5f : (real)pow(1.0f - distance / 15.0f, 0.7) + 0.5f;
	height = MIN((bounds->y1 - bounds->y0) * SPARTAN_LINK_PING_HEIGHT * size / 0.5f,
		(bounds->y1 - bounds->y0) * SPARTAN_LINK_PING_HEIGHT_MOST);
	if (!spartan_link_aim_place(&camera, bounds, height * 0.5f, &ping.position, &place))
		return;
	hud_render_nav_point_arrow(ping.nav_index,
		spartan_link_ping_behind(feed->unit_index, &eye, &ping.position, ping.object_index) ? 2 /* occluded */ : 0,
		&place, height, &ping.color, ping.alpha);
	/* (debug.spartan_link_log: the first ping on a feed logged and its
	frame saved, once a run) */
	if (spartan_link.log && !spartan_link.ping_saved)
	{
		spartan_link.ping_saved = TRUE;
		platform_log("spartan link ping: %ld local %d (player %ld) at %.1f %.1f of %.1f-%.1f %.1f-%.1f, %.1f tall",
			game_time_get(), (int)local_player_index, (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(feed->player_index), place.x,
			place.y, bounds->x0, bounds->x1, bounds->y0, bounds->y1, height);
		halo_screenshot_named("spartan_link_ping");
	}
}

/* ---------- public code */

void spartan_link_update(
	void)
{
	unsigned long now = system_milliseconds();
	short local_player_index;
	boolean key;
	short port;

	/* (the frame's macro steps are the ones run from here, before its
	clock's update, to its drawing: spartan_link_frame_judge) */
	spartan_link.frame_macro_steps = game_macro_steps_run();
	spartan_link.frame_judged = FALSE;
	spartan_link.frame_serial++;
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		spartan_link.windows[local_player_index].next_feed = 0;
	spartan_link_read();
	/* (a new game: every link closed, as each game starts) */
	if (!spartan_link_playing() || game_time_get() < spartan_link.last_game_time)
	{
		if (spartan_link.in_game)
		{
			short view;

			csmemset(spartan_link.windows, 0, sizeof(spartan_link.windows));
			for (view = 1; view <= MAXIMUM_LOCAL_PLAYERS * SPARTAN_LINK_FEEDS; view++)
			{
				if (spartan_link.views_used & (1UL << view))
					halo_view_release(view);
			}
			spartan_link.views_used = 0;
		}
		spartan_link.in_game = FALSE;
	}
	if (spartan_link_playing())
	{
		if (!spartan_link.in_game)
		{
			csmemset(spartan_link.windows, 0, sizeof(spartan_link.windows));
			spartan_link.last_local_player = NONE;
			spartan_link.last_feed = NONE;
			for (port = 0; port < (short)NUMBEROF(spartan_link.view_players); port++)
				spartan_link.view_players[port] = NONE;
			/* (each player's link as they left it, their profile's;
			debug.spartan_link_open: on from the start, for the tests) */
			spartan_link.debug_open = config_boolean("debug.spartan_link_open");
			for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
			{
				spartan_link.windows[local_player_index].open = spartan_link.debug_open ||
					config_player_boolean(local_player_index, "display.spartan_link_shown");
			}
		}
		spartan_link.in_game = TRUE;
		spartan_link.last_game_time = game_time_get();
	}
	/* the presses: a controller's (the stock layouts' D-pad Down, each
	port's own player's), the keyboard's (the first player's) */
	for (port = 0; port < MAXIMUM_LOCAL_PLAYERS; port++)
	{
		if (halo_spartan_link_pad_pressed(port))
			spartan_link_toggle(controller_remap_local_player(port), now);
	}
	key = (halo_keyboard_actions(0) & FLAG(HALO_KEYBOARD_SPARTAN_LINK)) != 0;
	if (key && !spartan_link.key_held && !console_is_active() && !network_chat_typing())
	{
		short first = controller_remap_local_player(0);

		if (first == NONE)
			first = local_player_get_next(NONE);
		if (first != NONE && !ui_widgets_active_for_local_player(first))
			spartan_link_toggle(first, now);
	}
	spartan_link.key_held = key;
	if (!spartan_link.in_game)
		return;
	/* (debug.spartan_link_log: each tick, every player's eye and aim as this
	machine has them, which another machine's feeds are compared with) */
	if (spartan_link.log && game_time_get() != spartan_link.logged_tick)
	{
		struct data_iterator iterator;
		struct player_datum *player;

		spartan_link.logged_tick = game_time_get();
		data_iterator_new(&iterator, player_data);
		while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
		{
			struct camera_command command;

			if (player->unit_index == NONE || !object_try_and_get_and_verify_type(player->unit_index, _object_mask_unit))
				continue;
			first_person_camera_fake(player->unit_index, &command);
			platform_log("spartan link player: %ld %ld %.4f %.4f %.4f %.5f %.5f %.5f", game_time_get(),
				(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), command.position.x, command.position.y,
				command.position.z, command.forward.i, command.forward.j, command.forward.k);
		}
	}
	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct spartan_link_window *window = &spartan_link.windows[local_player_index];

		/* (on, it stays on: hidden while the game has none, a cutscene's
		too; hidden, the teammates' pings bring their feeds up a while) */
		if (window->open && spartan_link_game())
			spartan_link_window_update(local_player_index, now);
		else if (!window->open && spartan_link_game())
			spartan_link_window_pings(local_player_index, now);
		else if (!window->open)
			window->popped = FALSE;
		spartan_link_hint(local_player_index, now);
		/* (the teammates' aims, while the link is shown) */
		if (spartan_link_shown(local_player_index))
		{
			short feed;

			for (feed = 0; feed < window->count; feed++)
				spartan_link_feed_aim_update(&window->feeds[feed]);
		}
	}
	/* (at a rate, the feed due this frame) */
	if (spartan_link.feed_rate > 0)
		spartan_link_schedule();
	else
		spartan_link.due_local_player = spartan_link.due_feed = NONE;
}

void spartan_link_note_motion_sensor(
	short local_player_index,
	short left,
	short center_x,
	short center_y)
{
	struct spartan_link_tracker *tracker;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || spartan_link.drawing)
		return;
	tracker = &spartan_link.trackers[local_player_index];
	if (spartan_link.log && (!tracker->valid || tracker->left != left || tracker->center_x != center_x ||
		tracker->center_y != center_y))
	{
		platform_log("spartan link tracker: local player %d left %d middle %d %d of %dx%d, HUD scale %.3f",
			(int)local_player_index, (int)left, (int)center_x, (int)center_y,
			(int)(render.camera.window_bounds.x1 - render.camera.window_bounds.x0),
			(int)(render.camera.window_bounds.y1 - render.camera.window_bounds.y0),
			hud_globals_get_scale(local_player_count() > 1));
	}
	tracker->valid = TRUE;
	tracker->left = (real)left;
	tracker->center_x = (real)center_x;
	tracker->center_y = (real)center_y;
	tracker->view_width = (short)(render.camera.window_bounds.x1 - render.camera.window_bounds.x0);
	tracker->view_height = (short)(render.camera.window_bounds.y1 - render.camera.window_bounds.y0);
	tracker->players = local_player_count();
}

unsigned char spartan_link_pad_open(
	short port)
{
	short local_player_index;

	if (!spartan_link.enabled || !spartan_link_playing() || cinematic_in_progress() || !controller_remap_playing(port))
		return FALSE;
	local_player_index = controller_remap_local_player(port);
	return local_player_index != NONE && !ui_widgets_active_for_local_player(local_player_index) &&
		(game_engine_running() || network_coop_active() || local_player_count() > 1);
}

/* the feeds drawn (spartan_link_feeds_drawn) */
static unsigned long spartan_link_feeds_count;

/* once a drawn frame, as its first window draws: whether a feed's draw due
in it waits for the next frame. A frame that runs a tick's macro step is the
slowest (the world's whole update), and a feed's draw there is a second view
on top: it waits a frame, so the frames after the ticks, with no macro step,
take it. Never two frames running (its image is at most a frame later than
it was due, and its next due time is counted from this one's: the rate
holds), and not where the last drawn frame ran a macro step too: a frame
rate so low that every frame has one draws as it was due */
static void spartan_link_frame_judge(
	void)
{
	boolean ticked;

	if (spartan_link.frame_judged)
		return;
	spartan_link.frame_judged = TRUE;
	ticked = game_macro_steps_run() != spartan_link.frame_macro_steps;
	spartan_link.last_deferred = spartan_link.deferred;
	spartan_link.deferred = FALSE;
	spartan_link.frame_defers = ticked && !spartan_link.last_frame_ticked && !spartan_link.last_deferred;
	spartan_link.last_frame_ticked = ticked;
}

/* the next feed of this window to draw now, else NONE: at a rate
(debug.spartan_link_feed_rate), the feed spartan_link_schedule chose, which
waits off a macro step's frame where it has its image; else each feed every
frame, in order, once (the window's next_feed) */
static short spartan_link_feed_next(
	short local_player_index)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	short index;

	if (spartan_link.feed_rate <= 0)
	{
		index = window->next_feed < window->count ? window->next_feed : NONE;
		window->next_feed++;
		return index;
	}
	spartan_link_frame_judge();
	if (local_player_index != spartan_link.due_local_player || spartan_link.due_feed == NONE)
		return NONE;
	index = spartan_link.due_feed;
	/* (one that has its image waits off a macro step's frame; one with none
	yet is drawn at once: spartan_link_frame_judge) */
	if (spartan_link.frame_defers && spartan_link_feed_image(local_player_index, index))
	{
		spartan_link.deferred = TRUE;
		index = NONE;
	}
	/* (the next frame's is another's) */
	else
	{
		spartan_link.last_local_player = spartan_link.due_local_player;
		spartan_link.last_feed = spartan_link.due_feed;
	}
	spartan_link.due_local_player = NONE;
	spartan_link.due_feed = NONE;
	return index;
}

/* the camera, the view and the state for drawing this feed of the window,
and the feed's image's camera kept (spartan_link_feed_warp); FALSE: not to
be drawn now */
static boolean spartan_link_feed_prepare(
	short local_player_index,
	short index,
	real_rectangle2d const *rectangle,
	struct render_camera *camera)
{
	struct spartan_link_window *window = &spartan_link.windows[local_player_index];
	struct spartan_link_feed *feed = &window->feeds[index];
	boolean through_eyes;
	unsigned long width, height;
	int view;

	if (!player_try_and_get(feed->player_index) || feed->down_at ||
		!object_try_and_get_and_verify_type(feed->unit_index, _object_mask_unit))
	{
		return FALSE;
	}
	through_eyes = spartan_link_feed_view(feed, camera);
	camera->mirrored = FALSE;
	camera->z_near = rasterizer_globals.near_clip_distance;
	camera->z_far = render_far_clip_distance(local_player_index);
	/* its window: its size on the screen, from the screen's top left */
	width = (unsigned long)floorf(rectangle->x1 - rectangle->x0 + 0.5f);
	height = (unsigned long)floorf(rectangle->y1 - rectangle->y0 + 0.5f);
	if (width < 8 || height < 8)
		return FALSE;
	camera->viewport_bounds.x0 = 0;
	camera->viewport_bounds.y0 = 0;
	camera->viewport_bounds.x1 = (short)width;
	camera->viewport_bounds.y1 = (short)height;
	camera->window_bounds = camera->viewport_bounds;
	feed->width = width;
	feed->height = height;
	/* (the camera its image is of, which the frames until the next warp it
	from: spartan_link_feed_warp) */
	feed->image_valid = TRUE;
	feed->image_through_eyes = through_eyes;
	feed->image_unit = feed->unit_index;
	feed->image_position = camera->position;
	feed->image_forward = camera->forward;
	feed->image_up = camera->up;
	feed->image_tangent = (real)tan(camera->vertical_field_of_view * 0.5f);
	feed->image_width = width;
	feed->image_height = height;
	feed->image_at = system_milliseconds();
	feed->image_frame = spartan_link.frame_serial;
	/* (at a rate: its next draw a period on from this one's due time, so
	that the rate holds at any frame rate; from now where it had no image or
	fell a period behind) */
	if (spartan_link.feed_rate > 0)
	{
		unsigned long now = system_milliseconds() * 3;
		long period = 3000 / MIN(spartan_link.feed_rate, 3000);

		if (!spartan_link_feed_image(local_player_index, index) || (long)(now - feed->draw_at) >= period)
			feed->draw_at = now;
		feed->draw_at += period;
	}

	view = SPARTAN_LINK_VIEW(local_player_index, index);
	spartan_link_feeds_count++;
	spartan_link.views_used |= 1UL << view;
	spartan_link.view_players[view] = feed->player_index;
	spartan_link.drawing = TRUE;
	spartan_link.drawing_began = platform_shared_clock_microseconds();
	spartan_link.hidden_unit = through_eyes ? feed->unit_index : NONE;
	spartan_link.posed_unit = through_eyes && object_get(feed->unit_index)->object.parent_object_index == NONE ?
		feed->unit_index : NONE;
	halo_view_begin(view, width, height);
	/* (what the view's drawing sets of the lights and the fog screen, put
	back after it) */
	lights_port_keep(TRUE);
	fog_screen_port_keep(TRUE);
	if (spartan_link.log)
	{
		platform_log("spartan link: %ld %d %ld %d %.4f %.4f %.4f %.5f %.5f %.5f %.4f %.3f %.3f", game_time_get(),
			(int)local_player_index, (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(feed->player_index), through_eyes ? 1 : 0,
			camera->position.x, camera->position.y, camera->position.z, camera->forward.i, camera->forward.j,
			camera->forward.k, camera->vertical_field_of_view, render_interpolation_fraction(),
			render_interpolation_first_person_feed_phase());
	}
	return TRUE;
}

/* (render.c's render_frame calls it until it is FALSE, each TRUE a feed
drawn into its view, then spartan_link_feed_end) */
boolean spartan_link_feed_begin(
	short local_player_index,
	struct render_camera *camera)
{
	real_rectangle2d rectangles[SPARTAN_LINK_FEEDS];
	struct render_camera window_camera = *camera;
	real scale;
	short count;
	short index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS ||
		!spartan_link_shown(local_player_index))
	{
		return FALSE;
	}
	count = spartan_link_layout(local_player_index, camera, rectangles, &scale);
	while ((index = spartan_link_feed_next(local_player_index)) != NONE)
	{
		if (index >= count)
			break;
		*camera = window_camera;
		if (spartan_link_feed_prepare(local_player_index, index, &rectangles[index], camera))
			return TRUE;
	}
	*camera = window_camera;
	return FALSE;
}

void spartan_link_feed_end(
	void)
{
	if (spartan_link.drawing)
		spartan_link.drawing_microseconds += platform_shared_clock_microseconds() - spartan_link.drawing_began;
	halo_view_end();
	lights_port_keep(FALSE);
	fog_screen_port_keep(FALSE);
	spartan_link.drawing = FALSE;
	spartan_link.hidden_unit = NONE;
	spartan_link.posed_unit = NONE;
}

unsigned long spartan_link_feeds_drawn(
	void)
{
	return spartan_link_feeds_count;
}

unsigned long long spartan_link_feeds_microseconds(
	void)
{
	return spartan_link.drawing_microseconds;
}

boolean spartan_link_drawing(
	void)
{
	return spartan_link.drawing;
}

long spartan_link_hidden_unit(
	void)
{
	return spartan_link.drawing ? spartan_link.hidden_unit : NONE;
}

long spartan_link_posed_unit(
	void)
{
	return spartan_link.drawing ? spartan_link.posed_unit : NONE;
}

/* (debug.spartan_link_log: a feed's first-person weapon's state as it
changes, first_person_weapons.c's feeds' slots) */
void spartan_link_feed_state_log(
	long unit_index,
	char const *state,
	real firing_push_back_velocity)
{
	if (spartan_link.log)
	{
		platform_log("spartan link feed state: %ld %lx %s %.3f", game_time_get(), (unsigned long)unit_index, state,
			firing_push_back_velocity);
	}
}

short spartan_link_linked_units(
	long *unit_indices,
	short maximum_count)
{
	short count = 0;
	short local_player_index;

	for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
	{
		struct spartan_link_window *window = &spartan_link.windows[local_player_index];
		short feed;

		if (!spartan_link.in_game || !spartan_link_shown(local_player_index))
			continue;
		for (feed = 0; feed < window->count; feed++)
		{
			long unit_index = window->feeds[feed].unit_index;
			short index;

			if (window->feeds[feed].down_at || unit_index == NONE)
				continue;
			for (index = 0; index < count && unit_indices[index] != unit_index; index++)
				;
			if (index == count && count < maximum_count)
				unit_indices[count++] = unit_index;
		}
	}
	return count;
}

/* ---------- campaign scoring on the link (campaign_score_hud.c) */

/* a teammate's groups of points shown over their feed for this long
(milliseconds), at most two */
#define SPARTAN_LINK_SCORE_MILLISECONDS 8000

static real spartan_link_font_height(
	void)
{
	long font_index = hud_get_font_index();
	struct font_header *font;

	if (font_index == NONE)
		return 0.0f;
	font = font_definition_get(font_index);
	return (real)(font->ascending_height + font->descending_height);
}

/* "+150 Triple Kill" */
static void spartan_link_score_words(
	struct campaign_score_hud_group const *group,
	wchar_t *text,
	short size)
{
	wchar_t number[16];

	campaign_score_hud_number(group->points >= 0 ? group->points : -group->points, number, NUMBEROF(number));
	usnprintf(text, size, L"%s%s %s", group->points >= 0 ? L"+" : L"-", number, group->text);
	text[size - 1] = 0;
}

/* over a feed's top (bounds), on a dark band: the teammate's latest groups
of points at its right (the newest first, the older paler) and their streak
(3 or more) at its left, on a line of its own where the first group leaves
no room */
static void spartan_link_draw_score(
	struct bitmap_data *white,
	short local_player_index,
	short index,
	real_rectangle2d const *bounds,
	real scale)
{
	struct spartan_link_feed *feed = &spartan_link.windows[local_player_index].feeds[index];
	struct campaign_score_player const *entry;
	struct campaign_score_hud_group groups[2];
	real width = bounds->x1 - bounds->x0;
	real text_scale = 0.5f * MAX(scale, 0.9f) * MAX(spartan_link.size, 0.9f) *
		PIN(width / (SPARTAN_LINK_NARROWEST * scale), 0.75f, 1.0f);
	real line_height = spartan_link_font_height() * text_scale;
	real pad = 2.0f * scale;
	real streak_width = 0.0f;
	real_argb_color color;
	wchar_t streak[24];
	wchar_t text[48];
	boolean streak_shown;
	boolean streak_alone = FALSE;
	short count;
	short lines;
	short line;

	if (!campaign_score_hud_active() || feed->down_at || line_height <= 0.0f)
		return;
	entry = campaign_score_hud_player(feed->player_index);
	count = campaign_score_hud_recent(feed->player_index, SPARTAN_LINK_SCORE_MILLISECONDS, groups, NUMBEROF(groups));
	streak_shown = entry && entry->current_streak >= 3;
	if (!count && !streak_shown)
		return;
	if (streak_shown)
	{
		usnprintf(streak, NUMBEROF(streak), L"STREAK %ld", entry->current_streak);
		streak_width = spartan_link_text_width(streak) * text_scale;
		if (count)
		{
			spartan_link_score_words(&groups[0], text, NUMBEROF(text));
			streak_alone = streak_width + spartan_link_text_width(text) * text_scale + 4.0f * pad > width;
		}
	}
	lines = (short)(MAX(count, 1) + (streak_alone ? 1 : 0));
	spartan_link_rectangle(white, bounds->x0, bounds->y0, bounds->x1, bounds->y0 + lines * line_height + 2.0f * pad,
		0x8C000000UL);
	hud_get_text_color(&color);
	color.red += (1.0f - color.red) * 0.5f;
	color.green += (1.0f - color.green) * 0.5f;
	color.blue += (1.0f - color.blue) * 0.5f;
	for (line = 0; line < count; line++)
	{
		color.alpha = line == 0 ? 1.0f : 0.65f;
		spartan_link_score_words(&groups[line], text, NUMBEROF(text));
		spartan_link_text(text, bounds->x0, bounds->y0 + pad + line * line_height, width - 2.0f * pad, 1, text_scale,
			&color);
	}
	if (streak_shown)
	{
		color.alpha = 0.9f;
		spartan_link_text(streak, bounds->x0 + 2.0f * pad,
			bounds->y0 + pad + (streak_alone ? count : 0) * line_height, streak_width, 0, text_scale, &color);
	}
}

/* with the link hidden, where its row goes (beside the tracker, or at the
top), the teammates' names and scores in a line ("ALEX 1,250   SAM 900"),
at most three: a co-op game with scoring, its player alive */
static void spartan_link_draw_scores_line(
	short local_player_index)
{
	long self = local_player_get_player_index(local_player_index);
	struct data_iterator iterator;
	struct player_datum *player;
	real_argb_color color;
	real_rectangle2d room;
	real line_scale;
	wchar_t text[96];
	short shown = 0;

	if (!campaign_score_hud_active() || !spartan_link_game() || game_engine_running() || self == NONE ||
		player_get(self)->unit_index == NONE)
	{
		return;
	}
	text[0] = 0;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL && shown < 3)
	{
		struct campaign_score_player const *entry;
		wchar_t name[16];
		wchar_t number[16];
		long length = ustrlen(text);

		if (iterator.datum_index == self || player->quit_out_of_game)
			continue;
		entry = campaign_score_hud_player(iterator.datum_index);
		ustrncpy(name, player->name, NUMBEROF(player->name));
		name[NUMBEROF(player->name)] = 0;
		campaign_score_hud_number(entry ? entry->score : 0, number, NUMBEROF(number));
		usnprintf(text + length, NUMBEROF(text) - length, L"%s%s %s", shown ? L"   " : L"", name, number);
		text[NUMBEROF(text) - 1] = 0;
		shown++;
	}
	if (!shown)
		return;
	spartan_link_room(local_player_index, &render.camera, &room, &line_scale);
	pixel32_to_real_argb_color(spartan_link_tracker_color(local_player_index), &color);
	color.red += (1.0f - color.red) * SPARTAN_LINK_NAME_PALER;
	color.green += (1.0f - color.green) * SPARTAN_LINK_NAME_PALER;
	color.blue += (1.0f - color.blue) * SPARTAN_LINK_NAME_PALER;
	color.alpha = SPARTAN_LINK_NAME_ALPHA;
	if (spartan_link.top)
	{
		real top;

		spartan_link_top_room(&render.camera, line_scale, &room.x0, &room.x1, &top);
		spartan_link_text(text, room.x0, top, room.x1 - room.x0, 2, 0.7f * line_scale, &color);
	}
	else
	{
		spartan_link_text(text, room.x0, (room.y0 + room.y1) * 0.5f - SPARTAN_LINK_ROW * 0.5f * line_scale,
			room.x1 - room.x0, 0, 0.7f * line_scale, &color);
	}
}

void spartan_link_draw(
	short local_player_index)
{
	struct spartan_link_window *window;
	struct bitmap_data *white;
	real_rectangle2d rectangles[SPARTAN_LINK_FEEDS];
	real scale;
	short count;
	short index;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || spartan_link.drawing)
		return;
	window = &spartan_link.windows[local_player_index];
	white = NULL;
	/* (the HUD's coordinates are the view's own, from its top left: a split
	screen's lower window's too. A line where the row goes, beside the
	tracker, as the HUD's text: a refusal, or the key's hint) */
	if (window->line_milliseconds && system_milliseconds() - window->line_at < window->line_milliseconds)
	{
		real_argb_color color;
		real_rectangle2d room;
		real line_scale;

		spartan_link_room(local_player_index, &render.camera, &room, &line_scale);
		hud_get_text_color(&color);
		color.alpha = 1.0f;
		if (spartan_link.top)
		{
			real top;

			spartan_link_top_room(&render.camera, line_scale, &room.x0, &room.x1, &top);
			spartan_link_text(window->line, room.x0, top, room.x1 - room.x0, 2, 0.8f * line_scale, &color);
		}
		else
		{
			spartan_link_text(window->line, room.x0,
				(room.y0 + room.y1) * 0.5f - SPARTAN_LINK_ROW * 0.5f * line_scale, room.x1 - room.x0, 0,
				0.8f * line_scale, &color);
		}
	}
	/* (campaign scoring: with the link hidden, the teammates' scores in its
	place, none while a line shows there) */
	if (!spartan_link_shown(local_player_index) &&
		(!window->line_milliseconds || system_milliseconds() - window->line_at >= window->line_milliseconds))
	{
		spartan_link_draw_scores_line(local_player_index);
	}
	if (!spartan_link_shown(local_player_index))
		return;
	white = spartan_link_white();
	if (!white)
		return;
	count = spartan_link_layout(local_player_index, &render.camera, rectangles, &scale);
	for (index = 0; index < count; index++)
	{
		spartan_link_draw_feed(white, local_player_index, index, &rectangles[index], scale);
		if (spartan_link.stats)
			spartan_link_draw_row(white, local_player_index, index, &rectangles[index], scale);
		spartan_link_draw_score(white, local_player_index, index, &rectangles[index], scale);
		/* (the teammate's reticle, on their feed's picture) */
		if (halo_view_drawn(SPARTAN_LINK_VIEW(local_player_index, index)))
		{
			struct spartan_link_feed *feed = &window->feeds[index];
			real radius = spartan_link_reticle_radius(&rectangles[index]);
			real_point2d place;
			boolean enemy = feed->aim_enemy;
			boolean shown = spartan_link_feed_aim_place(feed, &rectangles[index], radius, &place);

			spartan_link_draw_ping(local_player_index, feed, &rectangles[index]);
			if (shown)
				spartan_link_draw_reticle(white, &place, radius, enemy);
			/* (debug.spartan_link_log: the reticle's and the health's
			changes, and the first red's frame saved) */
			if (spartan_link.log)
			{
				short state = !shown ? 0 : enemy ? 2 : 1;
				real health = object_try_and_get(feed->unit_index) ?
					object_get(feed->unit_index)->object.body_vitality : 0.0f;

				if (state != feed->logged_state || fabsf(health - feed->logged_health) >= 0.05f)
				{
					platform_log("spartan link reticle: %ld local %d feed %d (player %ld) %s at %.1f %.1f of %.1f-%.1f "
						"%.1f-%.1f, health %.2f", game_time_get(), (int)local_player_index, (int)index,
						(long)DATUM_INDEX_TO_ABSOLUTE_INDEX(feed->player_index),
						state == 2 ? "red" : state == 1 ? "blue" : "none", place.x, place.y, rectangles[index].x0,
						rectangles[index].x1, rectangles[index].y0, rectangles[index].y1, health);
					feed->logged_state = state;
					feed->logged_health = health;
				}
				if (state == 2 && !spartan_link.red_saved)
				{
					spartan_link.red_saved = TRUE;
					halo_screenshot_named("spartan_link_red");
				}
			}
		}
	}
}
