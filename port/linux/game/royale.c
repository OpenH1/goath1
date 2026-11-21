/*
ROYALE.C

Royale, GOAT H1's battle royale (royale.h), a mode of free for all Slayer
(game_modes.c): squads of two, no respawns, downs and revives, the gulag,
the last squad alive wins.

The rules are royale_rules.c's, run by the host each tick (the game engine's
update, before the objects move); the host sends its clients the whole state
after Slayer's (game_engine_write_network_state), so every machine's HUD,
spectating and lanes read the same phases, squads and player states. A
player's slot is their absolute player index, as Infection's.

What core does in the game:
- spawning (game_engine_should_spawn_player): everyone spawns in the LOBBY
  (and respawns there); after it only a player a lane asked for
  (royale_request_spawn: the gulag's, insertion's redeploy).
- a lethal hit (damage.c) on an ALIVE player with a squadmate standing downs
  them: their unit keeps a little health, no shields, and crawls (crouched,
  slow, no weapons: royale_player_control); a hit on the downed kills them.
- a squadmate's held action within ROYALE_REVIVE_RADIUS revives; a downed
  player not revived in time bleeds out (their unit killed, no one's kill).
- a death goes to the gulag lane, then the player is out: they watch their
  squadmate (dead_spectate.c) and the Spartan link shows them their squad.
- RESULTS: the placements and kills over the screen, then the game ends.
- the lanes: each one's start on the game's first tick, tick each tick on
  every machine, draw in each local player's HUD (royale_lanes_stub.c gives
  a lane not linked in nothing to do).

Tests (a network test's run alone: royale_test_setting): HALO_ROYALE_LOBBY_SECONDS
and HALO_ROYALE_RESULTS_SECONDS shorten the lobby and the results,
HALO_ROYALE_TEST_END ends the match early, HALO_ROYALE_MAP_CONFIG=<file>
reads a map config from a file.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud.h"
#include "interface/hud_draw.h"
#include "interface/hud_messaging.h"
#include "main/main.h"
#include "math/real_math.h"
#include "memory/data.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "render/render_camera_projection.h"
#include "scenario/scenario.h"
#include "tag_files/tag_groups.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "units/units.h"
#include "units/unit_control_data.h"
#include "game_modes.h"
#include "network_distributed.h"
#include "network_standard.h"
#include "royale.h"
#include "royale_rules.h"
#include "royale_audio.h"
#include "royale_cinema.h"
#include "royale_fx.h"
#include "royale_hud.h"

#include <stdlib.h>
#include <string.h>

/* port/linux/src/port_config.c */
int config_boolean(char const *name);

void platform_log(char const *format, ...);
long game_engine_slayer_write_network_state(byte *buffer, long size);
boolean game_engine_slayer_read_network_state(byte const *buffer, long size, boolean first);
unsigned long game_options_random_seed(void);

/* a downed unit's health (of its maximum), and a revived one's */
#define ROYALE_DOWNED_HEALTH 0.3f
#define ROYALE_REVIVED_HEALTH 0.5f
/* how fast the downed crawl (of a walk) */
#define ROYALE_CRAWL_SPEED 0.3f
#define ROYALE_LOBBY_SECONDS 20
#define ROYALE_INSERTION_SECONDS 120
#define ROYALE_RESULTS_SECONDS 15
/* ENDGAME at this many squads alive or fewer */
#define ROYALE_ENDGAME_SQUADS 3
/* Slayer's score to win, never reached: Royale ends by its own rules */
#define ROYALE_NO_SCORE_TO_WIN 30000
/* a player in the fight with no unit this long after the drop is out */
#define ROYALE_UNITLESS_SECONDS 5

static struct
{
	struct game_engine engine;
	struct game_engine slayer;
	struct royale_rules rules;
	struct royale_rules_settings settings;
	struct royale_map_config config;
	boolean config_loaded;
	/* the lanes started (the game's first update) */
	boolean started;
	boolean results_logged;
	long test_end_ticks;
	/* this tick's: who holds action (players.c), who revives whom, the
	rules' events */
	boolean action_held[ROYALE_RULES_MAXIMUM_SLOTS];
	long reviver[ROYALE_RULES_MAXIMUM_SLOTS];
	byte events[ROYALE_RULES_MAXIMUM_SLOTS];
	/* the host's: a spawn asked for (royale_request_spawn), and the unit
	killed by a bleed-out */
	boolean spawn_requested[ROYALE_RULES_MAXIMUM_SLOTS];
	long bled_unit[ROYALE_RULES_MAXIMUM_SLOTS];
	/* the host's: ticks a player in the fight has had no unit */
	long unitless_ticks[ROYALE_RULES_MAXIMUM_SLOTS];
	/* the host's: the clock's bytes at the end of the state last written
	(royale_network_state_clock_size); a client's: the host's clock taken */
	long clock_bytes;
	boolean clock_taken;
	/* every machine's: the states and phase last logged (royale_log_changes),
	and a client's states taken and clocks put right */
	byte logged_state[ROYALE_RULES_MAXIMUM_SLOTS];
	byte logged_phase;
	long states_taken;
	long clock_corrections;
	/* the hash of this machine's map config (royale_map_config_hash): the
	host's goes in the state; a client's that differs is told once */
	unsigned long config_hash;
	boolean config_mismatch_told;
} royale;

/* (the rules, the gulag and the lanes by player slot: every player the
port allows) */
typedef char royale_slots_cover_players[ROYALE_RULES_MAXIMUM_SLOTS == ROYALE_MAXIMUM_PLAYERS &&
	ROYALE_MAXIMUM_PLAYERS >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS ? 1 : -1];

/* ---- slots and players */

static long royale_slot(
	long player_index)
{
	long slot;

	if (player_index == NONE)
		return NONE;
	slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	return VALID_INDEX(slot, ROYALE_RULES_MAXIMUM_SLOTS) ? slot : NONE;
}

/* the player of a slot, NONE if none */
static long royale_player_of_slot(
	long slot)
{
	struct data_iterator iterator;

	if (!VALID_INDEX(slot, ROYALE_RULES_MAXIMUM_SLOTS))
		return NONE;
	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
	{
		if (DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index) == slot)
			return iterator.datum_index;
	}
	return NONE;
}

static struct royale_rules_slot const *royale_entry(
	long player_index)
{
	long slot = royale_slot(player_index);

	return slot == NONE ? NULL : &royale.rules.slots[slot];
}

/* a player's living unit, NONE if none */
static long royale_living_unit(
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

/* ---- the interface (royale.h) */

boolean royale_enabled(
	void)
{
	return config_boolean("game.royale") != 0;
}

boolean royale_game_allowed(
	void)
{
	/* (a Standard game's host or client, or a game of this machine alone;
	the host's or this machine's game.royale on, as a client plays its
	host's game and a film its recording's) */
	return network_standard_simulation_fixes() &&
		(royale_enabled() || network_game_distributed_client() ||
			game_connection() == _game_connection_film_playback);
}

boolean royale_active(
	void)
{
	return game_modes_mode() == _game_mode_royale;
}

enum royale_phase royale_phase(
	void)
{
	if (!royale_active())
		return ROYALE_PHASE_NONE;
	return royale.rules.phase >= ROYALE_RULES_PHASE_RESULTS ? ROYALE_PHASE_RESULTS : (enum royale_phase)royale.rules.phase;
}

long royale_phase_time(void) { return royale_active() ? royale.rules.phase_time : 0; }
long royale_match_time(void) { return royale_active() ? royale.rules.match_time : 0; }

short royale_squad_of(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	return royale_active() && entry && entry->squad != 0xFF ? entry->squad : NONE;
}

boolean royale_squad_alive(
	short squad)
{
	return royale_active() && royale_rules_squad_alive(&royale.rules, squad);
}

long royale_squad_member(
	short squad,
	short seat)
{
	long slot;
	short seen = 0;

	if (!royale_active() || squad == NONE)
		return NONE;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (royale.rules.slots[slot].squad == squad && seen++ == seat)
			return royale_player_of_slot(slot);
	}
	return NONE;
}

long royale_squadmate_of(
	long player_index)
{
	long slot = royale_slot(player_index);

	if (!royale_active() || slot == NONE)
		return NONE;
	return royale_player_of_slot(royale_rules_squadmate(&royale.rules, slot));
}

short royale_squad_count(void) { return royale_active() ? royale.rules.squad_count : 0; }
short royale_squads_alive(void) { return royale_active() ? royale.rules.squads_alive : 0; }

enum royale_player_state royale_player_state(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	if (!royale_active() || !entry || entry->state == ROYALE_RULES_NONE)
		return ROYALE_PLAYER_NONE;
	return (enum royale_player_state)entry->state;
}

void royale_set_player_state(
	long player_index,
	enum royale_player_state state)
{
	long slot = royale_slot(player_index);

	/* (the host's: its clients take its state) */
	if (!royale_active() || slot == NONE || network_game_distributed_client() || state < 0 ||
		state >= NUMBER_OF_ROYALE_PLAYER_STATES)
	{
		return;
	}
	royale_rules_set_state(&royale.rules, &royale.settings, slot, (byte)state);
}

long royale_downed_ticks_left(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	return royale_active() && entry && entry->state == ROYALE_RULES_DOWNED ? entry->downed_ticks : 0;
}

real royale_revive_progress(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	if (!royale_active() || !entry || entry->state != ROYALE_RULES_DOWNED || entry->reviver == 0xFF ||
		royale.settings.revive_ticks <= 0)
	{
		return 0.0f;
	}
	return PIN((real)entry->revive_ticks / (real)royale.settings.revive_ticks, 0.0f, 1.0f);
}

long royale_spectate_target(
	long player_index)
{
	long mate;

	if (royale_player_state(player_index) != ROYALE_PLAYER_SPECTATING)
		return NONE;
	mate = royale_squadmate_of(player_index);
	return royale_living_unit(mate) != NONE ? mate : NONE;
}

enum royale_reclaimer_result royale_player_reclaimer(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	return royale_active() && entry ? (enum royale_reclaimer_result)entry->reclaimer : ROYALE_RECLAIMER_NONE;
}

/* (the rules' results are royale.h's) */
typedef char royale_reclaimer_results_alike[ROYALE_RULES_RECLAIMER_WON == ROYALE_RECLAIMER_WON &&
	ROYALE_RULES_RECLAIMER_LOST == ROYALE_RECLAIMER_LOST ? 1 : -1];

short royale_squad_placement(
	short squad)
{
	return royale_active() && VALID_INDEX(squad, ROYALE_RULES_MAXIMUM_SQUADS) ? royale.rules.placement[squad] : 0;
}

short royale_player_kills(
	long player_index)
{
	struct royale_rules_slot const *entry = royale_entry(player_index);

	return royale_active() && entry ? entry->kills : 0;
}

short royale_winning_squad(
	void)
{
	return royale_active() && royale.rules.phase >= ROYALE_RULES_PHASE_RESULTS && royale.rules.winner != 0xFF ?
		royale.rules.winner : NONE;
}

struct royale_map_config const *royale_map_config_get(
	void)
{
	return royale_active() ? &royale.config : NULL;
}

long royale_seconds_to_ticks(
	real seconds)
{
	return (long)(seconds * TICKS_PER_SECOND + 0.5f);
}

long royale_revive_ticks(void) { return royale.settings.revive_ticks; }
long royale_bleed_out_ticks(void) { return royale.settings.bleed_out_ticks; }

unsigned long royale_match_seed(
	void)
{
	return game_options_random_seed();
}

void royale_request_spawn(
	long player_index)
{
	long slot = royale_slot(player_index);

	if (slot != NONE)
		royale.spawn_requested[slot] = TRUE;
}

/* ---- the rules' settings and the game's start */

boolean network_test_hooks(void);

char const *royale_test_setting(
	char const *variable)
{
	char const *value = getenv(variable);

	if (!value || !*value)
		return NULL;
	if (!network_test_hooks())
	{
		platform_log("royale: %s ignored: a test's setting (a network test's run alone)", variable);
		return NULL;
	}
	return value;
}

static long royale_debug_seconds(
	char const *variable,
	long fallback)
{
	char const *value = royale_test_setting(variable);

	return value ? MAX(atol(value), 0) : fallback;
}

static void royale_settings_build(
	void)
{
	struct game_variant_options const *options = game_variant_options_get();

	royale.settings.lobby_ticks = royale_debug_seconds("HALO_ROYALE_LOBBY_SECONDS", ROYALE_LOBBY_SECONDS) *
		TICKS_PER_SECOND;
	royale.settings.insertion_ticks = ROYALE_INSERTION_SECONDS * TICKS_PER_SECOND;
	royale.settings.bleed_out_ticks = (options->royale_bleed_out ? options->royale_bleed_out :
		ROYALE_DEFAULT_BLEED_OUT) * TICKS_PER_SECOND;
	royale.settings.revive_ticks = (options->royale_revive ? options->royale_revive : ROYALE_DEFAULT_REVIVE) *
		TICKS_PER_SECOND;
	royale.settings.results_ticks = royale_debug_seconds("HALO_ROYALE_RESULTS_SECONDS", ROYALE_RESULTS_SECONDS) *
		TICKS_PER_SECOND;
	royale.settings.endgame_squads = ROYALE_ENDGAME_SQUADS;
	royale.test_end_ticks = royale_debug_seconds("HALO_ROYALE_TEST_END", 0) * TICKS_PER_SECOND;
	royale.settings.skip_insertion = !royale_insertion_running();
}

/* the map's name without its folders ("levels\b30\b30": "b30") */
static char const *royale_map_short_name(
	void)
{
	char const *name = global_scenario_index != NONE ? tag_get_name(global_scenario_index) : NULL;
	char const *last = name;

	if (!name)
		return "";
	for (; *name; name++)
	{
		if (*name == '\\' || *name == '/')
			last = name + 1;
	}
	return last;
}

static boolean royale_initialize_for_new_map(
	void)
{
	long slot;

	royale_rules_reset(&royale.rules);
	royale_settings_build();
	csmemset(royale.action_held, 0, sizeof(royale.action_held));
	csmemset(royale.spawn_requested, 0, sizeof(royale.spawn_requested));
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		royale.reviver[slot] = NONE;
		royale.bled_unit[slot] = NONE;
		royale.unitless_ticks[slot] = 0;
	}
	royale.started = FALSE;
	royale.results_logged = FALSE;
	royale.clock_bytes = 0;
	royale.clock_taken = FALSE;
	csmemset(royale.logged_state, ROYALE_RULES_NONE, sizeof(royale.logged_state));
	royale.logged_phase = ROYALE_RULES_PHASE_LOBBY;
	royale.states_taken = 0;
	royale.clock_corrections = 0;
	royale.config_loaded = royale_map_config_load(royale_map_short_name(), &royale.config);
	royale.config_hash = royale_map_config_hash(&royale.config);
	platform_log("royale: map %s, config %s, insertion %s", royale_map_short_name(),
		royale.config_loaded ? "loaded" : "none", royale.settings.skip_insertion ? "skipped" : "on");
	/* (Royale ends by its own rules, never at Slayer's score) */
	game_engine_get_variant()->universal_variant.score_to_win = ROYALE_NO_SCORE_TO_WIN;
	return royale.slayer.initialize_for_new_map ? royale.slayer.initialize_for_new_map() : TRUE;
}

/* the lanes' starts, once the map's objects are placed (the first update) */
static void royale_start_lanes(
	void)
{
	struct royale_map_config const *config = &royale.config;

	royale.started = TRUE;
	royale_map_start(config);
	royale_loot_start(config);
	royale_insertion_start(config);
	royale_beam_start(config);
	royale_gulag_start(config);
	royale_bots_start(config);
	royale_scale_start(config);
	royale_cinema_start(config);
	royale_audio_start(config);
	royale_fx_start(config);
	royale_hud_start(config);
}

void royale_reset(
	void)
{
	long slot;

	royale_rules_reset(&royale.rules);
	csmemset(&royale.config, 0, sizeof(royale.config));
	royale.config_loaded = FALSE;
	royale.config_hash = 0;
	royale.config_mismatch_told = FALSE;
	royale.started = FALSE;
	royale.results_logged = FALSE;
	royale.test_end_ticks = 0;
	royale.clock_bytes = 0;
	royale.clock_taken = FALSE;
	csmemset(royale.action_held, 0, sizeof(royale.action_held));
	csmemset(royale.events, 0, sizeof(royale.events));
	csmemset(royale.spawn_requested, 0, sizeof(royale.spawn_requested));
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		royale.reviver[slot] = NONE;
		royale.bled_unit[slot] = NONE;
		royale.unitless_ticks[slot] = 0;
	}
	royale_map_reset();
	royale_loot_reset();
	royale_insertion_reset();
	royale_beam_reset();
	royale_gulag_reset();
	royale_bots_reset();
	royale_scale_reset();
	royale_cinema_reset();
	royale_audio_reset();
	royale_fx_reset();
	royale_hud_reset();
}

/* ---- the host's rules each tick */

static boolean royale_gulag_proc(
	long slot,
	long killer_slot)
{
	if (!game_variant_options_get()->royale_gulag)
		return FALSE;
	return royale_gulag_player_died(royale_player_of_slot(slot), royale_player_of_slot(killer_slot));
}

/* who revives each downed player this tick: their squadmate, standing on
foot within reach, holding action */
static void royale_find_revivers(
	void)
{
	long slot;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		long mate;
		long unit_index;
		long mate_unit_index;
		real_point3d position;
		real_point3d mate_position;
		real dx, dy, dz;

		royale.reviver[slot] = NONE;
		if (royale.rules.slots[slot].state != ROYALE_RULES_DOWNED)
			continue;
		mate = royale_rules_squadmate(&royale.rules, slot);
		if (mate == NONE || royale.rules.slots[mate].state != ROYALE_RULES_ALIVE || !royale.action_held[mate])
			continue;
		unit_index = royale_living_unit(royale_player_of_slot(slot));
		mate_unit_index = royale_living_unit(royale_player_of_slot(mate));
		if (unit_index == NONE || mate_unit_index == NONE ||
			object_get(mate_unit_index)->object.parent_object_index != NONE)
		{
			continue;
		}
		object_get_origin(unit_index, &position);
		object_get_origin(mate_unit_index, &mate_position);
		dx = position.x - mate_position.x;
		dy = position.y - mate_position.y;
		dz = position.z - mate_position.z;
		if (dx * dx + dy * dy + dz * dz <= ROYALE_REVIVE_RADIUS * ROYALE_REVIVE_RADIUS)
			royale.reviver[slot] = mate;
	}
}

static void royale_host_tick(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;
	long slot;
	byte phase = royale.rules.phase;

	/* (the lobby's players in; any who left out) */
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		slot = royale_slot(iterator.datum_index);
		if (slot == NONE)
			continue;
		if (player->quit_out_of_game)
			royale_rules_leave(&royale.rules, &royale.settings, slot, royale_gulag_proc);
		else
			royale_rules_join(&royale.rules, slot);
	}
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (royale.rules.slots[slot].state != ROYALE_RULES_NONE && royale_player_of_slot(slot) == NONE)
			royale_rules_leave(&royale.rules, &royale.settings, slot, royale_gulag_proc);
	}
	/* (debug: HALO_ROYALE_TEST_END=<seconds> puts every squad but the first
	out at that time of the match, for the results screen) */
	if (royale.test_end_ticks > 0 && royale.rules.match_time == royale.test_end_ticks)
	{
		for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
		{
			if (royale.rules.slots[slot].squad != 0xFF && royale.rules.slots[slot].squad != 0)
				royale_rules_leave(&royale.rules, &royale.settings, slot, royale_gulag_proc);
		}
		platform_log("royale: test end");
	}
	/* (a player in the fight with no unit for ROYALE_UNITLESS_SECONDS, once
	the drop is over, is out: a unit lost without a death, or a seat never
	taken, would keep their squad alive for ever. Not the gulag's, whose
	lane spawns them) */
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		byte state = royale.rules.slots[slot].state;
		boolean in_fight = state == ROYALE_RULES_IN_PELICAN || state == ROYALE_RULES_FALLING ||
			state == ROYALE_RULES_ALIVE || state == ROYALE_RULES_DOWNED;

		if (!in_fight || royale.rules.phase < ROYALE_RULES_PHASE_PLAY ||
			royale.rules.phase >= ROYALE_RULES_PHASE_RESULTS ||
			royale_living_unit(royale_player_of_slot(slot)) != NONE || royale.spawn_requested[slot])
		{
			royale.unitless_ticks[slot] = 0;
			continue;
		}
		if (++royale.unitless_ticks[slot] >= ROYALE_UNITLESS_SECONDS * TICKS_PER_SECOND)
		{
			platform_log("royale: slot %ld out: no unit for %d seconds (state %d)", slot, ROYALE_UNITLESS_SECONDS,
				state);
			royale.unitless_ticks[slot] = 0;
			royale_rules_died(&royale.rules, &royale.settings, slot, NONE, royale_gulag_proc);
		}
	}
	royale_find_revivers();
	royale_rules_tick(&royale.rules, &royale.settings, royale.reviver, royale.events, royale_match_seed(),
		royale_gulag_proc);
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		long player_index;
		long unit_index;

		if (!royale.events[slot])
			continue;
		player_index = royale_player_of_slot(slot);
		unit_index = royale_living_unit(player_index);
		if (royale.events[slot] & ROYALE_RULES_EVENT_REVIVED)
		{
			if (unit_index != NONE)
				object_get(unit_index)->object.body_vitality = ROYALE_REVIVED_HEALTH;
			platform_log("royale: slot %ld revived", slot);
		}
		if (royale.events[slot] & ROYALE_RULES_EVENT_BLED_OUT)
		{
			/* (its unit's death is the death: player_killed_player hears it) */
			if (unit_index == NONE)
				royale_rules_died(&royale.rules, &royale.settings, slot, NONE, royale_gulag_proc);
			else if (royale.bled_unit[slot] != unit_index)
			{
				royale.bled_unit[slot] = unit_index;
				unit_kill(unit_index);
			}
		}
	}
	if (phase != royale.rules.phase)
		platform_log("royale: phase %d (%d squads alive)", royale.rules.phase, royale.rules.squads_alive);
}

/* every machine: the downed have no shields */
static void royale_downed_update(
	void)
{
	long slot;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		long unit_index;
		struct object_datum *object;

		if (royale.rules.slots[slot].state != ROYALE_RULES_DOWNED)
			continue;
		unit_index = royale_living_unit(royale_player_of_slot(slot));
		if (unit_index == NONE)
			continue;
		object = object_get(unit_index);
		object->object.shield_vitality = 0.0f;
	}
}

/* every machine logs each phase and player state as it comes, alike, with
the match time: a host's and a client's logs compare line by line */
static void royale_log_changes(
	void)
{
	long slot;

	if (royale.logged_phase != royale.rules.phase)
	{
		platform_log("royale: phase %d -> %d at match tick %ld (play began %ld; %ld host states taken, %ld clock fixes)",
			royale.logged_phase, royale.rules.phase, royale.rules.match_time, royale.rules.play_start,
			royale.states_taken, royale.clock_corrections);
		royale.logged_phase = royale.rules.phase;
	}
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (royale.logged_state[slot] == royale.rules.slots[slot].state)
			continue;
		platform_log("royale: slot %ld state %d -> %d at match tick %ld", slot, royale.logged_state[slot],
			royale.rules.slots[slot].state, royale.rules.match_time);
		royale.logged_state[slot] = royale.rules.slots[slot].state;
	}
}

static void royale_log_results(
	void)
{
	short squad;

	if (royale.results_logged || royale.rules.phase < ROYALE_RULES_PHASE_RESULTS)
		return;
	royale.results_logged = TRUE;
	platform_log("royale: results: squad %d wins after %ld ticks (%d squads)",
		royale.rules.winner == 0xFF ? -1 : royale.rules.winner, royale.rules.match_time, royale.rules.squad_count);
	if (network_game_distributed_client())
	{
		platform_log("royale: client took %ld host states, its clock put right %ld times", royale.states_taken,
			royale.clock_corrections);
	}
	for (squad = 0; squad < royale.rules.squad_count; squad++)
	{
		long a = royale_squad_member(squad, 0);
		long b = royale_squad_member(squad, 1);

		platform_log("royale: squad %d place %d kills %d+%d", squad, royale.rules.placement[squad],
			royale_player_kills(a), royale_player_kills(b));
	}
}

static void royale_update(
	void)
{
	if (royale.slayer.update)
		royale.slayer.update();
	if (!royale.started)
		royale_start_lanes();
	if (!network_game_distributed_client())
		royale_host_tick();
	/* (a client's clock goes on by itself, as the host's: the host's state
	only corrects it) */
	else
		royale_rules_advance(&royale.rules, &royale.settings);
	royale_downed_update();
	royale_log_changes();
	royale_log_results();
	/* (the lanes, on every machine, before the objects move) */
	royale_map_tick();
	royale_insertion_tick();
	royale_loot_tick();
	royale_beam_tick();
	royale_gulag_tick();
	royale_bots_tick();
	royale_scale_tick();
	royale_cinema_tick();
	royale_audio_tick();
	royale_fx_tick();
	royale_hud_tick();
	csmemset(royale.action_held, 0, sizeof(royale.action_held));
}

static void royale_player_killed_player(
	long killing_player_index,
	long killing_object_index,
	long dead_player_index,
	boolean friendly_fire)
{
	long slot = royale_slot(dead_player_index);

	if (royale.slayer.player_killed_player)
		royale.slayer.player_killed_player(killing_player_index, killing_object_index, dead_player_index, friendly_fire);
	if (slot == NONE)
		return;
	royale.bled_unit[slot] = NONE;
	/* (a client's rules and gulag line are the host's: royale_read_network_state) */
	if (!network_game_distributed_client())
	{
		platform_log("royale: slot %ld died (state %d) to slot %ld at tick %ld", slot,
			royale.rules.slots[slot].state, royale_slot(killing_player_index), royale.rules.match_time);
		royale_rules_died(&royale.rules, &royale.settings, slot, royale_slot(killing_player_index),
			royale_gulag_proc);
	}
}

/* a player's score, as the scoreboard and the postgame's places rank them:
their squad's place first (100 a squad beaten), then their kills, so the
winning squad's players come first */
static long royale_get_player_score(
	long player_index,
	enum get_score_type score_type)
{
	short squad = royale_squad_of(player_index);
	short place = squad != NONE ? royale_squad_placement(squad) : 0;
	long score = royale_player_kills(player_index);

	(void)score_type;
	if (place > 0)
		score += (long)(royale.rules.squad_count + 1 - place) * 100;
	return score;
}

/* the scoreboard's and the carnage report's score: the squad's kills, as
their Kills column counts them (the game's statistics: the Reclaimer's
too) */
static wchar_t *royale_format_player_score(
	long player_index,
	wchar_t *text)
{
	struct player_datum *player = player_try_and_get(player_index);
	long mate = royale_squadmate_of(player_index);
	struct player_datum *mate_player = mate != NONE ? player_try_and_get(mate) : NULL;
	long kills = (player ? player->statistics.kills[0] : 0) + (mate_player ? mate_player->statistics.kills[0] : 0);

	usprintf(text, L"%ld", kills);
	return text;
}

static wchar_t *royale_format_score_name(
	wchar_t *text)
{
	usprintf(text, L"Squad kills");
	return text;
}

/* (the game's "won" and "lost": the winning squad's players won, everyone
else lost; NONE before there is a winner) */
static long royale_did_player_win(
	long player_index)
{
	short winners = royale_winning_squad();

	if (winners == NONE)
		return NONE;
	return royale_squad_of(player_index) == winners ? TRUE : FALSE;
}

long royale_player_place(
	long player_index)
{
	short squad = royale_squad_of(player_index);
	short place = squad != NONE ? royale_squad_placement(squad) : 0;

	return place > 0 ? (long)(place - 1) : NONE;
}

boolean royale_winners_label(
	wchar_t *text,
	short count)
{
	short squad = royale.rules.winner != 0xFF ? (short)royale.rules.winner : NONE;
	wchar_t names[2][16];
	short seat;

	if (!royale_active() || squad == NONE || count < 2)
		return FALSE;
	for (seat = 0; seat < 2; seat++)
	{
		struct player_datum *player = player_try_and_get(royale_squad_member(squad, seat));
		short index;

		for (index = 0; player && index < (short)NUMBEROF(player->name) && index < 15 && player->name[index]; index++)
			names[seat][index] = player->name[index];
		names[seat][index] = 0;
	}
	if (names[1][0])
		usnprintf(text, count - 1, L"%s & %s WIN", names[0], names[1]);
	else
		usnprintf(text, count - 1, L"%s WINS", names[0]);
	text[count - 1] = 0;
	return TRUE;
}

struct game_engine *royale_engine_get(
	struct game_engine const *slayer)
{
	royale.slayer = *slayer;
	royale.engine = *slayer;
	royale.engine.initialize_for_new_map = royale_initialize_for_new_map;
	royale.engine.update = royale_update;
	royale.engine.player_killed_player = royale_player_killed_player;
	royale.engine.get_player_score = royale_get_player_score;
	royale.engine.format_player_score = royale_format_player_score;
	royale.engine.format_score_name = royale_format_score_name;
	royale.engine.did_player_win = royale_did_player_win;
	return &royale.engine;
}

/* ---- the game's hooks */

boolean royale_should_spawn(
	long player_index)
{
	long slot = royale_slot(player_index);

	if (slot == NONE || !royale_active())
		return FALSE;
	if (royale.rules.phase == ROYALE_RULES_PHASE_LOBBY)
		return TRUE;
	return royale.spawn_requested[slot] && !royale_rules_out(royale.rules.slots[slot].state);
}

void royale_postspawn(
	long player_index)
{
	long slot = royale_slot(player_index);
	struct player_datum *player = player_get(player_index);
	real_point3d position;
	real_vector3d facing;

	if (slot != NONE)
	{
		royale.spawn_requested[slot] = FALSE;
		royale.bled_unit[slot] = NONE;
	}
	if (player->unit_index != NONE && royale_gulag_spawn_point(player_index, &position, &facing))
		object_set_position(player->unit_index, &position, &facing, NULL);
	/* (the lobby's: stepped off the start they spawned at, in a ring by
	their slot, so the map's few starts (a campaign level's) stay free for
	the rest of a big game's players) */
	else if (player->unit_index != NONE && slot != NONE && royale.rules.phase == ROYALE_RULES_PHASE_LOBBY)
	{
		real angle = (real)(slot % 16) * (6.2831853f / 16.0f);
		real radius = 0.6f + 0.45f * (real)(slot / 16);

		object_get_origin(player->unit_index, &position);
		position.x += radius * cosf(angle);
		position.y += radius * sinf(angle);
		position.z += 0.1f;
		object_set_position(player->unit_index, &position, NULL, NULL);
	}
	royale_loot_postspawn(player_index);
	royale_gulag_postspawn(player_index);
}

boolean royale_game_over(
	void)
{
	return royale_active() && royale.rules.phase == ROYALE_RULES_PHASE_OVER;
}

boolean royale_absorb_lethal_damage(
	long object_index,
	long attacker_player_index)
{
	struct unit_datum *unit;
	long player_index;
	long slot;
	boolean downed;

	if (!royale_active())
		return FALSE;
	unit = (struct unit_datum *)object_try_and_get_and_verify_type(object_index, _object_mask_biped);
	if (!unit || unit->unit.player_index == NONE)
		return FALSE;
	player_index = unit->unit.player_index;
	slot = royale_slot(player_index);
	if (slot == NONE)
		return FALSE;
	if (network_game_distributed_client())
	{
		/* (a client's prediction, by the host's own rule on the host's
		state; its kill comes from the host if they die) */
		downed = royale_rules_can_be_downed(&royale.rules, slot);
	}
	else
	{
		downed = royale_rules_lethal(&royale.rules, &royale.settings, slot, royale_slot(attacker_player_index));
		if (downed)
			platform_log("royale: slot %ld downed by %ld", slot, royale_slot(attacker_player_index));
	}
	if (!downed)
		return FALSE;
	unit->object.body_vitality = ROYALE_DOWNED_HEALTH;
	unit->object.shield_vitality = 0.0f;
	return TRUE;
}

void royale_player_control(
	long player_index,
	struct unit_control_data *control)
{
	long slot = royale_slot(player_index);

	if (!royale_active() || slot == NONE)
		return;
	if (TEST_FLAG(control->control_flags, _unit_control_action_bit))
		royale.action_held[slot] = TRUE;
	if (royale.rules.slots[slot].state != ROYALE_RULES_DOWNED)
		return;
	/* (crawling: crouched, slow, no shots, grenades, melee, jumps, swaps or
	equipment) */
	control->control_flags = (word)((control->control_flags & ~(UNIT_CONTROL_GUNNER_MASK |
		FLAG(_unit_control_jump_bit) | FLAG(_unit_control_use_equipment_bit) | FLAG(_unit_control_action_bit))) |
		FLAG(_unit_control_crouch_modifier_bit));
	control->primary_trigger = 0.0f;
	control->throttle.i *= ROYALE_CRAWL_SPEED;
	control->throttle.j *= ROYALE_CRAWL_SPEED;
}

/* ---- the network state: Slayer's (its size first), then the rules'
discrete part and the gulag's line, then the clock

What changes only with events comes first: the host sends the state when it
changes (game_engine_write_network_state). The clock comes last, outside
that check (royale_network_state_clock_size): every machine advances its own
(royale_rules_advance), and the host's rides along as a correction with each
state, and at least every few seconds (network_distributed.c). */

/* a client's clock more than this off the host's (the message's age added)
is put right; nearer, it keeps its own steady count */
#define ROYALE_CLOCK_SLACK_TICKS (TICKS_PER_SECOND / 4)

/* (a client) the host's map config's hash against this machine's: the host's
config rules (its bounds, beam and gulag are the game's: the host decides
every death); a client whose own differs draws and predicts by a wrong one,
which its player is told, once a game */
static void royale_config_check(
	unsigned long host_hash)
{
	short local_player_index;

	if (host_hash == royale.config_hash || royale.config_mismatch_told)
		return;
	royale.config_mismatch_told = TRUE;
	platform_log("royale: this machine's map config (%08lx) is not the host's (%08lx): the host's rules hold, "
		"this machine's beam, bounds and gulag may show wrong", royale.config_hash, host_hash);
	for (local_player_index = 0; local_player_index < MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		if (local_player_get_player_index(local_player_index) != NONE)
			hud_print_message(local_player_index, L"Your Royale map data differs from the host's: update the game.");
	}
}

long royale_write_network_state(
	byte *buffer,
	long size)
{
	long slayer_size;
	long written;
	long rules_size;
	long gulag_size;
	long clock_size;
	byte *cursor;
	byte *end = buffer + size;

	royale.clock_bytes = 0;
	if (size < (long)sizeof(long))
		return 0;
	/* (Slayer's whole, then Royale's in what is left: Slayer's takes 2.4 KB
	with 64-bit longs, and given less it writes nothing, which a client
	refuses) */
	slayer_size = game_engine_slayer_write_network_state(buffer + sizeof(long), size - (long)sizeof(long));
	if (slayer_size <= 0)
		return 0;
	csmemcpy(buffer, &slayer_size, sizeof(slayer_size));
	cursor = buffer + sizeof(long) + slayer_size;
	rules_size = royale_rules_write_discrete(&royale.rules, cursor, (long)(end - cursor));
	if (!rules_size)
		return 0;
	cursor += rules_size;
	gulag_size = royale_gulag_write_network_state(cursor, (long)(end - cursor));
	if (!gulag_size)
		return 0;
	cursor += gulag_size;
	/* (the host's map config's hash: a client's own that differs is told,
	royale_config_check) */
	if (end - cursor < (long)sizeof(royale.config_hash))
		return 0;
	csmemcpy(cursor, &royale.config_hash, sizeof(royale.config_hash));
	cursor += sizeof(royale.config_hash);
	/* (more of the discrete part goes here: the clock stays last) */
	clock_size = royale_rules_write_clock(&royale.rules, cursor, (long)(end - cursor));
	if (!clock_size)
		return 0;
	cursor += clock_size;
	written = (long)(cursor - buffer);
	royale.clock_bytes = clock_size;
	return written;
}

long royale_network_state_clock_size(
	void)
{
	return royale_active() ? royale.clock_bytes : 0;
}

boolean royale_read_network_state(
	byte const *buffer,
	long size,
	boolean first)
{
	long slayer_size;
	long read;
	struct royale_rules rules;
	unsigned long host_config_hash;
	byte const *cursor;
	byte const *end = buffer + size;

	if (!network_game_distributed_client() || size < (long)sizeof(long))
		return FALSE;
	csmemcpy(&slayer_size, buffer, sizeof(slayer_size));
	if (slayer_size < 0 || slayer_size > size - (long)sizeof(long))
		return FALSE;
	cursor = buffer + sizeof(long) + slayer_size;
	read = royale_rules_read_discrete(cursor, (long)(end - cursor), &rules);
	if (read == NONE)
		return FALSE;
	cursor += read;
	read = royale_gulag_read_network_state(cursor, (long)(end - cursor));
	if (read == NONE)
		return FALSE;
	cursor += read;
	if (end - cursor < (long)sizeof(host_config_hash))
		return FALSE;
	csmemcpy(&host_config_hash, cursor, sizeof(host_config_hash));
	cursor += sizeof(host_config_hash);
	read = royale_rules_read_clock(cursor, (long)(end - cursor), &rules);
	if (read == NONE || cursor + read != end)
		return FALSE;
	if (!game_engine_slayer_read_network_state(buffer + sizeof(long), slayer_size, first))
		return FALSE;
	/* (the host's clock was its own when it sent this, half a round trip
	ago) */
	{
		long before = royale.rules.match_time;
		long ahead = (long)(distributed_own_round_trip_ticks() / 2.0f + 0.5f);

		royale_rules_take(&royale.rules, &rules, ahead, ROYALE_CLOCK_SLACK_TICKS, first || !royale.clock_taken);
		royale.states_taken++;
		if (royale.rules.match_time != before)
		{
			royale.clock_corrections++;
			platform_log("royale: client clock %ld -> %ld (host's %ld, %ld ticks ago)", before, royale.rules.match_time,
				rules.match_time, ahead);
		}
	}
	royale.clock_taken = TRUE;
	royale_gulag_take_network_state();
	royale_config_check(host_config_hash);
	return TRUE;
}

long royale_play_start_time(
	void)
{
	return royale_active() ? royale.rules.play_start : NONE;
}

/* ---- the HUD */

static void royale_text(
	rectangle2d const *bounds,
	short justification,
	wchar_t const *text,
	real red,
	real green,
	real blue,
	real alpha)
{
	long font_index = hud_get_font_index();
	real_argb_color color;

	if (font_index == NONE)
		return;
	color.alpha = alpha;
	color.red = red;
	color.green = green;
	color.blue = blue;
	draw_string_set_draw_mode(font_index, NONE, justification, 0, &color);
	rasterizer_draw_unicode_string(bounds, NULL, NULL, 0, text);
}

static short royale_line_height(
	void)
{
	long font_index = hud_get_font_index();
	struct font_header *font;

	if (font_index == NONE)
		return 16;
	font = font_definition_get(font_index);
	return (short)MAX(font->ascending_height + font->descending_height + font->leading_height, 8);
}

/* the view's window in the view's coordinates */
static rectangle2d royale_window(
	void)
{
	rectangle2d window = render.camera.window_bounds;

	offset_rectangle2d(&window, -render.camera.viewport_bounds.x0, -render.camera.viewport_bounds.y0);
	return window;
}

void royale_hud_row(
	short row,
	short *top,
	short *bottom)
{
	rectangle2d window = royale_window();
	short line = royale_line_height();

	*top = (short)(window.y0 + line / 2 + row * line);
	*bottom = (short)(*top + line);
}

/* the line at the top: the lobby's countdown, and DROP while this player is
aboard or falling (once down, the squads left and the kills are the HUD's:
royale_hud.c) */
static void royale_draw_banner(
	long player_index)
{
	rectangle2d window = royale_window();
	rectangle2d bounds;
	wchar_t text[96];
	long seconds;
	enum royale_player_state state = royale_player_state(player_index);

	switch (royale.rules.phase)
	{
	case ROYALE_RULES_PHASE_LOBBY:
		seconds = MAX(royale.settings.lobby_ticks - royale.rules.phase_time, 0) / TICKS_PER_SECOND;
		usnprintf(text, NUMBEROF(text) - 1, L"ROYALE  -  SQUADS DRAWN IN %ld", seconds + 1);
		break;
	case ROYALE_RULES_PHASE_INSERTION:
		if (state != ROYALE_PLAYER_IN_PELICAN && state != ROYALE_PLAYER_FALLING)
			return;
		usnprintf(text, NUMBEROF(text) - 1, L"DROP  -  %d SQUADS", royale.rules.squads_alive);
		break;
	case ROYALE_RULES_PHASE_PLAY:
	case ROYALE_RULES_PHASE_ENDGAME:
		/* (a Reclaimer's winner falling back to the island) */
		if (state != ROYALE_PLAYER_FALLING)
			return;
		usnprintf(text, NUMBEROF(text) - 1, L"RECLAIMED  -  REDEPLOYING");
		break;
	default:
		return;
	}
	text[NUMBEROF(text) - 1] = 0;
	bounds.x0 = window.x0;
	bounds.x1 = window.x1;
	royale_hud_row(ROYALE_HUD_ROW_BANNER, &bounds.y0, &bounds.y1);
	royale_text(&bounds, 2, text, 1.0f, 0.85f, 0.4f, 1.0f);
}

void royale_draw(
	short local_player_index)
{
	long player_index = local_player_get_player_index(local_player_index);

	if (!royale_active() || player_index == NONE || cinematic_in_progress())
		return;
	/* (the flyover, the victory and the carnage report: the cinema's
	alone, royale_cinema.c) */
	if (royale.rules.phase >= ROYALE_RULES_PHASE_RESULTS || royale_cinema_owns_screen(local_player_index))
	{
		royale_cinema_draw(local_player_index);
		return;
	}
	/* (in the gulag: its lines alone; the beam, the squad and the map are
	not theirs until they win) */
	if (royale_player_state(player_index) == ROYALE_PLAYER_GULAG)
	{
		royale_gulag_draw();
		return;
	}
	/* (the squadmate, the downed/out lines and the counts are the HUD's:
	royale_hud.c) */
	royale_draw_banner(player_index);
	royale_loot_draw();
	royale_insertion_draw();
	royale_beam_draw();
	royale_gulag_draw();
	royale_fx_draw(local_player_index);
	royale_hud_draw(local_player_index);
	royale_cinema_draw(local_player_index);
}
