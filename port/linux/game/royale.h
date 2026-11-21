/* royale.h: Royale, GOAT H1's battle royale (port/linux/game/royale.c)

Squads of two drop onto a map from Pelicans, loot, and fight while a
glassing beam sweeps the map; no one respawns. A player killed is downed
first (a squadmate can revive them), then goes to the gulag once, else is
eliminated and spectates a squadmate. The last squad alive wins.

Core (royale.c) owns the rules, the phases, the squads, the player states,
the game type, the map config and the hooks; each lane is a module of its own
(royale_<lane>.c) that core starts, ticks and draws:

	royale_<lane>_start(config)  once, when a Royale game's map is loaded
	royale_<lane>_tick()         once per game tick, in the simulation (the
	                             same on host and clients, run to run)
	royale_<lane>_draw()         each rendered frame, after the HUD (drawing
	                             lanes only)

Every rule is simulation: random choices use the game's seeded random, never
a clock. With Royale off nothing here does anything. */
#ifndef HALO_ROYALE_H
#define HALO_ROYALE_H

#include "cseries.h"
#include "math/real_math.h"

/* ---- limits */

/* every player the port allows (halo_port_limits.h) */
#define ROYALE_MAXIMUM_PLAYERS HALO_PORT_MAXIMUM_NETWORK_PLAYERS
#define ROYALE_SQUAD_SIZE 2
#define ROYALE_MAXIMUM_SQUADS (ROYALE_MAXIMUM_PLAYERS / ROYALE_SQUAD_SIZE)

/* ---- the game */

/* the current game is a Royale game */
boolean royale_active(void);

/* ---- phases */

enum royale_phase
{
	ROYALE_PHASE_NONE = -1, /* not a Royale game */
	ROYALE_PHASE_LOBBY = 0, /* players gather, squads assigned */
	ROYALE_PHASE_INSERTION, /* Pelicans fly, squads jump */
	ROYALE_PHASE_PLAY, /* the beam sweeps */
	ROYALE_PHASE_ENDGAME, /* the last stage: few squads left, the beam's last sweep */
	ROYALE_PHASE_RESULTS, /* the last squad alive has won: the results screen */
	NUMBER_OF_ROYALE_PHASES
};

enum royale_phase royale_phase(void);
/* the game ticks spent in the current phase */
long royale_phase_time(void);
/* the game ticks since the Royale game began (LOBBY's first tick). Every
machine counts them itself, tick by tick (a client's corrected now and then
by the host's): a lane times things from these, never from game_time_get,
which differs from machine to machine */
long royale_match_time(void);
/* the match time PLAY began at (ENDGAME's too when it came at once), NONE
before */
long royale_play_start_time(void);

/* ---- squads

A player is the player's datum index (players.h), as everywhere in the
game; a squad is 0 .. ROYALE_MAXIMUM_SQUADS - 1, NONE for none. */

short royale_squad_of(long player_index);
/* the squad has a player not eliminated (alive, downed, falling, in the
gulag ...) */
boolean royale_squad_alive(short squad);
/* the squad's players (NONE where a seat is empty) */
long royale_squad_member(short squad, short seat);
/* the other player of the player's squad, NONE if none */
long royale_squadmate_of(long player_index);
/* squads with a player in the game, and those still alive */
short royale_squad_count(void);
short royale_squads_alive(void);

/* ---- player states */

enum royale_player_state
{
	ROYALE_PLAYER_NONE = -1, /* not in the Royale game */
	ROYALE_PLAYER_IN_PELICAN = 0,
	ROYALE_PLAYER_FALLING,
	ROYALE_PLAYER_ALIVE,
	ROYALE_PLAYER_DOWNED, /* crawling; a squadmate can revive them; bleeds out */
	ROYALE_PLAYER_GULAG,
	ROYALE_PLAYER_SPECTATING, /* eliminated, watching a living squadmate */
	ROYALE_PLAYER_ELIMINATED, /* eliminated, no squadmate left to watch */
	NUMBER_OF_ROYALE_PLAYER_STATES
};

enum royale_player_state royale_player_state(long player_index);
/* lanes move players between their own states: insertion (IN_PELICAN ->
FALLING -> ALIVE), gulag (GULAG -> ALIVE on a win, ELIMINATED on a loss).
Core moves ALIVE -> DOWNED -> ALIVE / GULAG / ELIMINATED. */
void royale_set_player_state(long player_index, enum royale_player_state state);
/* a downed player's bleed-out left, in ticks (0 if not downed), and a
revive's progress 0..1 */
long royale_downed_ticks_left(long player_index);
real royale_revive_progress(long player_index);
/* the player watched by a spectating one, NONE if none */
long royale_spectate_target(long player_index);

/* how a player's gulag (the Reclaimer) ended: the host's rules, sent to every
machine with the state */
enum royale_reclaimer_result
{
	ROYALE_RECLAIMER_NONE = 0, /* never in the gulag, or still in it */
	ROYALE_RECLAIMER_WON, /* won it (or by default) and redeployed */
	ROYALE_RECLAIMER_LOST, /* lost the fight, the time up, or left the game from it */
	NUMBER_OF_ROYALE_RECLAIMER_RESULTS
};

enum royale_reclaimer_result royale_player_reclaimer(long player_index);

/* ---- placement and kills (the results screen) */

/* a squad's place: 1 for the winner, 0 while still alive */
short royale_squad_placement(short squad);
short royale_player_kills(long player_index);
/* the winning squad, NONE until RESULTS */
short royale_winning_squad(void);

/* ---- the map config (port/assets/royale/<map>.toml) */

#define ROYALE_MAXIMUM_FLIGHT_PATHS 16
#define ROYALE_MAXIMUM_BEAM_STAGES 8
#define ROYALE_MAXIMUM_LOOT_SPOTS 512
#define ROYALE_MAXIMUM_POD_SPOTS 32
#define ROYALE_MAXIMUM_BLOCKED_ENTRANCES 64

enum royale_side
{
	ROYALE_SIDE_NORTH = 0, /* +y */
	ROYALE_SIDE_EAST, /* +x */
	ROYALE_SIDE_SOUTH, /* -y */
	ROYALE_SIDE_WEST, /* -x */
	NUMBER_OF_ROYALE_SIDES
};

struct royale_bounds
{
	real_point3d minimum;
	real_point3d maximum;
};

/* a Pelican's flight: in at start, out at end, at this height */
struct royale_flight_path
{
	real_point3d start;
	real_point3d end;
	real speed; /* world units per second */
};

/* one stage of the beam's sweep, as fractions of the map's length from the
side it starts on: from start to end over duration seconds (after a wait of
delay seconds) */
struct royale_beam_stage
{
	real delay;
	real start;
	real end;
	real duration;
	real damage_per_second;
};

enum royale_loot_tier
{
	ROYALE_LOOT_COMMON = 0,
	ROYALE_LOOT_RARE,
	ROYALE_LOOT_EPIC,
	ROYALE_LOOT_LEGENDARY,
	NUMBER_OF_ROYALE_LOOT_TIERS
};

struct royale_loot_spot
{
	real_point3d position;
	short tier; /* enum royale_loot_tier */
};

/* an interior entrance closed off: an axis-aligned box no one passes */
struct royale_blocked_entrance
{
	struct royale_bounds bounds;
};

struct royale_gulag_arena
{
	struct royale_bounds bounds;
	real_point3d spawn[2];
	real spawn_facing[2]; /* radians */
};

struct royale_map_config
{
	char map_name[64]; /* the scenario's name, e.g. "b30" */
	short structure_bsp; /* the BSP played in (b30's exterior) */
	struct royale_bounds bounds; /* the play area: out of it is the beam's */
	real drop_height; /* the Pelicans' height (world z) */

	short flight_path_count;
	struct royale_flight_path flight_paths[ROYALE_MAXIMUM_FLIGHT_PATHS];

	/* the beam starts from a side drawn among these (flag per enum royale_side;
	all if 0) */
	long beam_sides;
	short beam_stage_count;
	struct royale_beam_stage beam_stages[ROYALE_MAXIMUM_BEAM_STAGES];

	short loot_spot_count;
	struct royale_loot_spot loot_spots[ROYALE_MAXIMUM_LOOT_SPOTS];

	short pod_spot_count;
	real_point3d pod_spots[ROYALE_MAXIMUM_POD_SPOTS];

	struct royale_gulag_arena gulag;

	short blocked_entrance_count;
	struct royale_blocked_entrance blocked_entrances[ROYALE_MAXIMUM_BLOCKED_ENTRANCES];
};

/* the config of the map being played (NULL when not a Royale game) */
struct royale_map_config const *royale_map_config_get(void);
/* reads port/assets/royale/<map_name>.toml into config: FALSE (config at its
defaults) if there is none or it does not parse */
boolean royale_map_config_load(char const *map_name, struct royale_map_config *config);
/* the same from text (tests) */
boolean royale_map_config_parse(char const *text, struct royale_map_config *config);
/* a hash of a config (as loaded: zeroed first, so its padding counts the
same on every machine): the host's goes in Royale's state, and a client
whose own differs knows its map config is not the host's */
unsigned long royale_map_config_hash(struct royale_map_config const *config);

/* ---- rules' timing (game ticks are at game.sim_rate: use these) */

/* seconds in game ticks at the current tick rate */
long royale_seconds_to_ticks(real seconds);

/* ---- the lanes (each in royale_<lane>.c; core calls them) */

void royale_loot_start(struct royale_map_config const *config);
void royale_loot_tick(void);
void royale_loot_draw(void);

void royale_beam_start(struct royale_map_config const *config);
void royale_beam_tick(void);
void royale_beam_draw(void);

void royale_insertion_start(struct royale_map_config const *config);
void royale_insertion_tick(void);
void royale_insertion_draw(void);

void royale_gulag_start(struct royale_map_config const *config);
void royale_gulag_tick(void);
void royale_gulag_draw(void);
/* (gulag) a player died for good (bled out or finished while downed, or
killed in the gulag): TRUE takes them to the gulag (core sets GULAG and
neither eliminates them nor has them spectate their squad); FALSE and core
eliminates them. Host/simulation, before core marks them ELIMINATED. */
boolean royale_gulag_player_died(long player_index, long killer_player_index);
/* (gulag) where a player spawns: TRUE and core spawns them there */
boolean royale_gulag_spawn_point(long player_index, real_point3d *position, real_vector3d *facing);
/* (gulag) after a player's unit spawns (core calls it for every spawn the
host makes; a client's units come from the network, so what a client must
see of a spawn (a fighter's view turned the arena's way) the gulag's tick
does on every machine) */
void royale_gulag_postspawn(long player_index);
/* (gulag) its line and fight in the host's state (royale_write_network_state):
the bytes written, 0 if size is too small; a client's read (the bytes, NONE
if not valid), taken once the whole state is (take) */
long royale_gulag_write_network_state(byte *buffer, long size);
long royale_gulag_read_network_state(byte const *buffer, long size);
void royale_gulag_take_network_state(void);
/* (gulag) the unit a GULAG player with no unit watches, NONE if none */
long royale_gulag_spectate_unit(long player_index);
/* (gulag, core's) spawn the player's unit at core's next tick, through the
normal spawn path */
void royale_request_spawn(long player_index);

void royale_map_start(struct royale_map_config const *config);
void royale_map_tick(void);

void royale_bots_start(struct royale_map_config const *config);
void royale_bots_tick(void);

void royale_scale_start(struct royale_map_config const *config);
void royale_scale_tick(void);

/* a test's setting (HALO_ROYALE_*: an environment variable), NULL unless
set and the run is a network test's (network_test.c's network_test_hooks):
a player's game never takes one */
char const *royale_test_setting(char const *variable);

/* ---- every game's start and end, Royale or not (game_engine.c's
game_engine_initialize_for_new_map and game_engine_dispose_from_old_map):
core's and each lane's state of the last game cleared, so nothing of a
Royale match (the beam, its tags, the drop, the loot, the gulag's line, the
bots' tally) is left for the next game, of any game type, on any map */
void royale_reset(void);
void royale_map_reset(void);
void royale_loot_reset(void);
void royale_insertion_reset(void);
void royale_beam_reset(void);
void royale_gulag_reset(void);
void royale_bots_reset(void);
void royale_scale_reset(void);

/* ---- hooks other lanes ask core for are added below, each with the lane
that asked */

/* (bots) Royale plays as a mode of Slayer: its game engine is Slayer's
(_game_engine_slayer), free for all, squads kept by core */

/* (bots) reviving: a squadmate holds the action button within this many
world units of a downed player for the gametype's revive time
(royale_revive_ticks; ROYALE_REVIVE_SECONDS by default); a downed player
bleeds out over royale_bleed_out_ticks */
#define ROYALE_REVIVE_RADIUS 0.6f
#define ROYALE_REVIVE_SECONDS 3.0f
#define ROYALE_BLEED_OUT_SECONDS 30.0f
long royale_revive_ticks(void);
long royale_bleed_out_ticks(void);

/* (bots) loot lane: a weapon/item on the ground's tier (enum
royale_loot_tier), NONE if it is not Royale loot */
short royale_loot_item_tier(long object_index);

/* (bots) beam lane: the side it sweeps from (enum royale_side, NONE before
it starts); the fraction of the map's length (bounds' x or y, from that
side) glassed now; the fraction the current stage ends at (while waiting,
the next stage's); ticks until the front next moves (0 while moving) */
short royale_beam_side(void);
real royale_beam_front(void);
real royale_beam_target(void);
long royale_beam_ticks_to_move(void);

/* (bots) insertion lane: an IN_PELICAN player's Pelican, FALSE if none */
boolean royale_insertion_pelican_position(long player_index, real_point3d *position, real_vector3d *velocity);

/* the HUD's fixed rows at the top of a view, so the lanes' lines never
overlap: a row's top and bottom in the view's own coordinates (a split
screen's window's). Row 0 the banner (core's), 1 the beam's countdown, 2 the
beam's MOVE with its arrow in rows 3-4; a gulag player sees only the
gulag's lines, in rows 0-1. Nothing else (a floating name tag) draws above
the bottom of row ROYALE_HUD_ROWS - 1. */
#define ROYALE_HUD_ROW_BANNER 0
#define ROYALE_HUD_ROW_COUNTDOWN 1
#define ROYALE_HUD_ROW_MOVE 2
#define ROYALE_HUD_ROWS 5
void royale_hud_row(short row, short *top, short *bottom);

/* (bots) gulag lane: a player's opponent in the gulag, NONE if none */
long royale_gulag_opponent(long player_index);

/* (loot, beam) the game's seed: the same on every machine and from run to
run (the game options' random_seed); hash it, never draw from the game's
random in a lane's start */
unsigned long royale_match_seed(void);

/* (loot) after a player's unit spawns (the host's spawns: the kit is the
host's), after the game engine's loadout and before royale_gulag_postspawn */
void royale_loot_postspawn(long player_index);

/* (insertion) the insertion lane is linked in (TRUE from its own
definition): without it INSERTION is skipped and everyone starts ALIVE where
they spawned in the lobby */
boolean royale_insertion_running(void);
/* (insertion, for gulag and bots) */
void royale_insertion_redeploy(long player_index);
boolean royale_insertion_jump(long player_index);

/* (insertion) TRUE while the insertion lane makes its own objects (its
Pelicans): the game engine's remap leaves them as they are */
boolean royale_insertion_creating(void);

/* (the game's scoreboard and carnage report, game_engine.c) a player's
place by their squad's, from 0 (the squads tie), NONE while their squad is
still in the fight; the winners' line ("A & B WIN"), FALSE if none */
long royale_player_place(long player_index);
boolean royale_winners_label(wchar_t *text, short count);

/* ---- core's hooks in the game (the game's code calls these) */

#define ROYALE_DEFAULT_BLEED_OUT 30
#define ROYALE_DEFAULT_REVIVE 3

struct game_engine;
struct unit_control_data;

/* Royale is offered here: game.royale, an experimental setting (off by
default), on. Off, no menu or gametype list has it */
boolean royale_enabled(void);
/* a Royale game may be played here: a GOAT H1 Standard game (or a game of
this machine alone) whose host has game.royale on (a client and a film take
the host's game as it is) */
boolean royale_game_allowed(void);
/* Slayer's engine with Royale's rules (game_modes_engine) */
struct game_engine *royale_engine_get(struct game_engine const *slayer);
/* a player's unit spawned (game_modes_postspawn) */
void royale_postspawn(long player_index);
/* whether the host spawns a player now (game_engine_should_spawn_player) */
boolean royale_should_spawn(long player_index);
/* the results shown: the game ends (game_engine_should_end_game) */
boolean royale_game_over(void);
/* the state the host sends its clients (game_engine_write_network_state):
Slayer's and Royale's */
long royale_write_network_state(byte *buffer, long size);
boolean royale_read_network_state(byte const *buffer, long size, boolean first);
/* the bytes at the end of the state last written that are the clock: not
part of what decides whether the state changed, and sent anyway every few
seconds (network_distributed.c); 0 when not a Royale game */
long royale_network_state_clock_size(void);
/* a lethal hit on an object (damage.c): TRUE if it downs a player instead,
their unit kept with a little health */
boolean royale_absorb_lethal_damage(long object_index, long attacker_player_index);
/* a player's control this tick (players.c, before unit_control): a downed
player crawls; a held action is seen for revives */
void royale_player_control(long player_index, struct unit_control_data *control);
/* each local player's HUD (hud.c): core's, then each lane's draw */
void royale_draw(short local_player_index);

#endif
