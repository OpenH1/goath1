/*
NETWORK_MARKERS.H

Pings (network_markers.c): a player marks the spot or the thing they aim
at for their team, with Halo's nav point and its distance.
*/

#ifndef NETWORK_MARKERS_H
#define NETWORK_MARKERS_H

/* each frame (main.c): the players' presses, the host's settings to its
clients, the pings faded out */
void network_markers_update(void);
/* a view's pings (hud_nav_points.c's hud_render_nav_points) */
void network_markers_render(short local_player_index);
/* ... and its pinged units' and vehicles' outlines, over its world
(render.c's render_window) */
void network_markers_render_outlines(short local_player_index);
/* whether the controller of the port pings with its stock layout's D-pad
Right now (xinput_sdl.c then keeps that direction from the game) */
unsigned char network_markers_pad_open(short port);
/* the message (network_distributed.c): OpenH1's own, in an OpenH1 game */
boolean network_markers_handles_message(word const *message, word size);
void network_markers_handle_message(long machine_index, word const *message, word size, boolean from_stream);

/* a player's ping (their datum index) as a local player is shown it (their
own, an ally's they see, not a player's this machine muted), while it is
drawn: FALSE for none; else (shown, if not NULL) how long ago it came, where
its arrow is drawn this frame (over its object as drawn, while it is there:
the object, else NONE), its colour and fade, and this map's arrow (NONE for
none). The Spartan link's feeds (spartan_link.c) */
struct network_marker_shown
{
	unsigned long age;
	real_point3d position;
	long object_index;
	real_rgb_color color;
	real alpha;
	short nav_index;
};
boolean network_markers_ping_shown(short local_player_index, long player_index, struct network_marker_shown *shown);
/* (the tests: debug.network_test_ping) the first local player pings what
it aims at, as a press does; FALSE if it could not */
boolean network_markers_ping_aimed(void);
/* ... (a client) one sent as another machine's player's, as a changed build
could send it, for the host to refuse; and count sent at once, past the
client's own limit, for the host to count */
boolean network_markers_ping_forged(void);
short network_markers_ping_burst(short count);
/* ... (the host, or a game of one machine: debug.network_test_teammate_ping)
the first local player's first ally of another machine or a bot pings what
they aim at, as their press would (the Spartan link's pop-up, spartan_link.c);
FALSE if none could */
boolean network_markers_ping_teammate(void);
/* ... and the pings this machine shows now: each one's player (absolute
index), and the count */
short network_markers_shown(short *player_indices, short maximum);

/* the pure parts (tools/harness): a point the host takes (inside the world,
not too far from the player), and whether a ping of a player clears their
last (the same object, or a point near the last) */
boolean network_markers_point_valid(real x, real y, real z);
boolean network_markers_same_spot(long object_index, real const *point, long last_object_index,
	real const *last_point);
/* ... and what a ping of an object of a type is drawn as beside its arrow
(network_markers.c's _marker_shape_*: the arrow alone, a weapon's or
equipment's icon, a unit's or vehicle's outline, and whether through walls) */
short network_markers_shape(short object_type, boolean has_icon, boolean versus_players, boolean enemy_player,
	boolean *through_walls);
/* ... whether the aim reaches an object, the line's hit that far, and how
near its line (network_markers.c's MARKER_PICK_*); and what the host makes
of a ping (_marker_kind_*: its last taken away, an item another's ping is
on claimed, else as sent) */
boolean network_markers_pick(real along, real across, real radius, real hit_distance, real *nearness);
byte network_markers_decided_kind(byte kind, boolean same_as_last, boolean item, boolean pinged_by_another);

#endif
