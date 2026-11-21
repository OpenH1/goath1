/*
SERVER_MACHINE_LIFECYCLE.C (test)

The host's one state for each client machine
(source/networking/network_server_manager.c): every event in every state
moves the machine to the state the table below says, or is refused (the
machine left as it was), so that a pair no one decided fails here; every
state that waits on the machine times it out (the lobby and the postgame
too: a dead machine held the lobby's start for ever); a load of another game,
or a second one, is ignored, not a reason to drop the machine; and a joiner
of another network version is told why it is refused
(network_server_message_handler.c). test_server_machine_lifecycle.py takes
the constants (config.inc), the structures (structures.inc) and the code
(under_test.inc).
*/

#include "harness.h"

#include <wchar.h>

typedef unsigned short word;
#define NETWORK_SERVER_MANAGER_FILE "server"
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define csstrcmp strcmp
#define TICKS_PER_SECOND 30
#define NETWORK_GAME_MESSAGE_VERSION 2
#define _network_game_packet_class_client_ingame 5
#define GET_MESSAGE_SIZE(header) ((word)(header))

#include "config.inc"

struct network_connection { int unused; };
struct network_message { word header; };

#define csmemcmp memcmp

/* (source/game/players.h's) */
struct network_player
{
	wchar_t name[12];
	short primary_color_index;
	short icon_index;
	signed char machine_index;
	signed char controller_index;
	signed char team_index;
	signed char player_list_index;
};

#include "structures.inc"

struct network_game
{
	long number_of_games_played;
	struct network_player players[MAXIMUM_NETWORK_PLAYER_COUNT];
};

struct network_game_server
{
	word state;
	struct network_game game;
	struct network_game_server_client_machine client_machines[MAXIMUM_NETWORK_MACHINE_COUNT];
	long next_update_number;
	unsigned long time_of_first_client_loading_completion;
	boolean sent_start_game_message;
};

static struct network_game_server server;
static struct network_connection connection;

/* the wall clock (system_milliseconds), set by each case */
static unsigned long now = 1000000;
static unsigned long system_milliseconds(void) { return now; }

static void network_event(char const *text, ...) { (void)text; }
static long game_time_get(void) { return 1000; }

/* the machines' players, downloads, MCC waits (stubbed by machine) */
static boolean has_players[MAXIMUM_NETWORK_MACHINE_COUNT];
static boolean downloading[MAXIMUM_NETWORK_MACHINE_COUNT];
static boolean mcc_waiting[MAXIMUM_NETWORK_MACHINE_COUNT];
static boolean network_game_server_machine_has_players(struct network_game_server *s, long machine_index)
{
	(void)s;
	return VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) && has_players[machine_index];
}
static boolean network_game_server_machine_has_waiting_players(struct network_game_server *s, long machine_index)
{
	(void)s; (void)machine_index;
	return FALSE;
}
static boolean map_share_machine_downloading(long machine_index)
{
	return VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) && downloading[machine_index];
}
static boolean map_share_machine_needs_map(long machine_index) { return map_share_machine_downloading(machine_index); }
static int mcc_load_network_host_waiting(long machine_index)
{
	return VALID_INDEX(machine_index, MAXIMUM_NETWORK_MACHINE_COUNT) && mcc_waiting[machine_index];
}
static void mcc_load_network_peer_left(long machine_index) { (void)machine_index; }
/* the machine whose party's held room the drop let go (network_party.c) */
static long party_dropped = NONE;
static void network_party_machine_dropped(long machine_index) { party_dropped = machine_index; }
static unsigned long network_client_timeout_milliseconds(void) { return 15000; }
static boolean halo_server_mode(void) { return FALSE; }
static boolean cache_files_precache_map_loaded(char const *name) { (void)name; return TRUE; }
static char multiplayer_map[32] = "beavercreek";
static char *main_get_multiplayer_map_name(void) { return multiplayer_map; }

/* the host's own machine is 0 */
static boolean network_game_server_client_machine_is_local(struct network_game_server *s,
	struct network_game_server_client_machine *machine)
{
	return machine == &s->client_machines[0];
}

static word network_game_server_get_state(struct network_game_server *s, short *substate)
{
	(void)substate;
	return s->state;
}

static int all_loaded;
static void network_game_server_all_machines_have_loaded(struct network_game_server *s)
{
	s->state = _network_game_server_state_ingame;
	all_loaded++;
}

/* the players' arrays the late joiner's load reads */
static long network_game_server_frequent_updates_until[MAXIMUM_NETWORK_MACHINE_COUNT];
static struct network_player late_joiner_players[MAXIMUM_NETWORK_MACHINE_COUNT][MAXIMUM_NETWORK_PLAYER_COUNT];
static boolean network_player_is_valid(struct network_player const *player)
{
	return player->machine_index >= 0 && player->controller_index >= 0;
}

/* port: a machine's join time (network_server_manager.c's) */
static unsigned long network_game_server_client_machine_join_times[MAXIMUM_NETWORK_MACHINE_COUNT];

/* what the host sends */
static struct network_message message = { 4 };
static short sent_type = NONE, sent_reason = NONE;
static int writes;
/* the players a late joiner was told were removed and added, in order */
static struct network_player removed[8], added[8];
static int removed_count, added_count;
static void *create_network_game_message(short type, void const *packet, long size)
{
	sent_type = type;
	if (type == _message_server_remove_player_ingame && removed_count < 8)
		removed[removed_count++] = ((struct message_server_remove_player_ingame const *)packet)->player;
	if (type == _message_server_add_player_ingame && added_count < 8)
		added[added_count++] = *(struct network_player const *)packet;
	if (type == _message_server_machine_rejected && size == sizeof(struct message_server_machine_rejected))
		sent_reason = ((struct message_server_machine_rejected const *)packet)->reason;
	return &message;
}
static boolean network_game_server_send_message_to_client_machine(struct network_game_server *s,
	struct network_game_server_client_machine *machine, void *m)
{
	(void)s; (void)machine; (void)m;
	return TRUE;
}
static struct network_connection *network_game_server_get_client_connection(
	struct network_game_server_client_machine *machine)
{
	return machine->connection;
}
static boolean network_game_server_write(struct network_connection *c, void *m, word size, void *address, boolean reliable)
{
	(void)c; (void)m; (void)size; (void)address; (void)reliable;
	writes++;
	return TRUE;
}

/* the message's decoding: the long the client sent */
static long loaded_sent;
static boolean decode_network_game_message(void *packet, const void *encoded, short *size, short *type,
	short *version, long packet_class)
{
	(void)encoded; (void)size; (void)type; (void)version; (void)packet_class;
	((struct message_client_loaded *)packet)->unused = loaded_sent;
	return TRUE;
}

/* (prototypes the code under test has from its file's top) */
void network_game_server_late_joiner_loaded(struct network_game_server *s,
	struct network_game_server_client_machine *machine);
void network_game_server_client_machine_game_loading_complete(struct network_game_server *s,
	struct network_game_server_client_machine *machine);
static boolean network_game_server_player_among(struct network_player const *player,
	struct network_player *players, long count);

#include "under_test.inc"

enum { HOST = 0, MACHINE = 3, OTHER = 4 };

/* the spec: each event's next state from each state (free, connected, lobby,
held, waiting, loading, in_game); R: refused */
#define R _machine_state_refused
#define F _machine_state_free
#define C _machine_state_connected
#define L _machine_state_lobby
#define H _machine_state_held
#define W _machine_state_waiting
#define O _machine_state_loading
#define G _machine_state_in_game
static short const expected[NUMBER_OF_MACHINE_EVENTS][NUMBER_OF_MACHINE_STATES] =
{
	[_machine_event_connect] = { C, R, R, R, R, R, R },
	[_machine_event_join_lobby] = { R, L, R, R, R, R, R },
	[_machine_event_join_in_progress] = { R, W, R, R, R, R, R },
	[_machine_event_start] = { R, R, O, R, R, R, R },
	[_machine_event_hold] = { R, R, H, H, H, R, R },
	[_machine_event_release] = { R, R, R, W, R, R, R },
	[_machine_event_start_late] = { R, R, R, R, O, R, R },
	[_machine_event_loaded] = { R, R, R, R, R, G, R },
	[_machine_event_switched_to_pregame] = { R, R, R, R, R, R, L },
	[_machine_event_round_reset] = { R, C, L, L, L, L, L },
	[_machine_event_precached] = { R, R, L, H, R, R, R },
	[_machine_event_map_changed] = { R, C, L, H, W, O, G },
	[_machine_event_heard] = { R, R, L, H, W, O, G },
	[_machine_event_answered_postgame] = { R, R, L, R, R, R, G },
	[_machine_event_drop] = { R, F, F, F, F, F, F },
};

static struct network_game_server_client_machine *machine_in(short state)
{
	struct network_game_server_client_machine *machine = &server.client_machines[MACHINE];

	machine->connection = &connection;
	machine->machine_index = MACHINE;
	machine->state = (byte)state;
	machine->flags = 0;
	machine->last_heard_time = now;
	return machine;
}

static void reset(word server_state)
{
	long index;

	memset(&server, 0, sizeof(server));
	memset(has_players, 0, sizeof(has_players));
	memset(downloading, 0, sizeof(downloading));
	memset(mcc_waiting, 0, sizeof(mcc_waiting));
	for (index = 0; index < MAXIMUM_NETWORK_MACHINE_COUNT; index++)
		server.client_machines[index].machine_index = NONE;
	memset(server.game.players, NONE, sizeof(server.game.players));
	memset(late_joiner_players, NONE, sizeof(late_joiner_players));
	server.state = server_state;
	server.game.number_of_games_played = 7;
	has_players[MACHINE] = has_players[OTHER] = TRUE;
	all_loaded = writes = 0;
	removed_count = added_count = 0;
	sent_type = sent_reason = NONE;
	network_game_server_client_machine_join_times[MACHINE] = now;
}

/* whether the machine is timed out after this much silence */
static boolean silent_for(struct network_game_server_client_machine *machine, unsigned long milliseconds)
{
	machine->last_heard_time = now - milliseconds;
	return network_game_server_client_machine_timed_out(&server, machine);
}

/* the machine is timed out after the limit's silence, not before (0: never) */
static void times_out_after(short state, word server_state, unsigned long limit, char const *what)
{
	struct network_game_server_client_machine *machine;

	reset(server_state);
	machine = machine_in(state);
	if (limit)
	{
		CHECK(!silent_for(machine, limit - 1000), "%s: timed out before its time", what);
		CHECK(silent_for(machine, limit + 1000), "%s: not timed out after %lu ms of silence", what, limit + 1000);
	}
	else
	{
		CHECK(!silent_for(machine, 3600000), "%s: timed out, where it must not be", what);
	}
}

/* a player's record, as the settings have it */
static struct network_player player_of(wchar_t const *name, long machine_index, long team, long slot)
{
	struct network_player player;

	memset(&player, 0, sizeof(player));
	wcsncpy(player.name, name, 11);
	player.machine_index = (signed char)machine_index;
	player.controller_index = 0;
	player.team_index = (signed char)team;
	player.player_list_index = (signed char)slot;
	return player;
}

/* the late joiner (MACHINE) started with the host's player (slot 0), its
own (slot 5) and another machine's Ann (slot 3); the game now has those of
`now` instead; it loads */
static void late_joiner_loads(struct network_player const *own_now, struct network_player const *slot3_now)
{
	struct network_game_server_client_machine *machine;

	reset(_network_game_server_state_ingame);
	late_joiner_players[MACHINE][0] = player_of(L"Hosty", HOST, 0, 0);
	late_joiner_players[MACHINE][3] = player_of(L"Ann", OTHER, 0, 3);
	late_joiner_players[MACHINE][5] = player_of(L"Joiner", MACHINE, 1, 5);
	server.game.players[0] = late_joiner_players[MACHINE][0];
	server.game.players[3] = *slot3_now;
	server.game.players[5] = *own_now;
	machine = machine_in(_machine_state_loading);
	network_game_server_late_joiner_loaded(&server, machine);
	CHECK(machine->state == _machine_state_in_game, "the late joiner's load was not taken");
}

static boolean handle_loaded(struct network_game_server_client_machine *machine, long sent)
{
	word packet[4] = { 0 };

	loaded_sent = sent;
	return network_game_server_handle_message_client_loaded(&server, machine, packet, (short)sizeof(packet));
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	reset(_network_game_server_state_pregame);
	CASE("table")
	{
		short state, event;

		for (event = 0; event < NUMBER_OF_MACHINE_EVENTS; event++)
		{
			for (state = 0; state < NUMBER_OF_MACHINE_STATES; state++)
			{
				struct network_game_server_client_machine *machine = machine_in(state);
				boolean taken = network_game_server_machine_event(machine, event);

				if (expected[event][state] == R)
				{
					CHECK(!taken && machine->state == state, "event %s in state %s was not refused (now %s)",
						network_game_server_machine_event_names[event], network_game_server_machine_state_names[state],
						network_game_server_machine_state_names[machine->state]);
				}
				else
				{
					CHECK(taken && machine->state == expected[event][state], "event %s in state %s led to %s, not %s",
						network_game_server_machine_event_names[event], network_game_server_machine_state_names[state],
						taken ? network_game_server_machine_state_names[machine->state] : "a refusal",
						network_game_server_machine_state_names[expected[event][state]]);
				}
			}
		}
	}
	CASE("timeouts")
	{
		/* every state that waits on the machine has its time */
		times_out_after(_machine_state_connected, _network_game_server_state_pregame, NETWORK_GAME_SERVER_JOIN_TIMEOUT,
			"connected");
		times_out_after(_machine_state_lobby, _network_game_server_state_pregame, NETWORK_GAME_SERVER_LOBBY_TIMEOUT,
			"lobby");
		times_out_after(_machine_state_lobby, _network_game_server_state_postgame, NETWORK_GAME_SERVER_LOBBY_TIMEOUT,
			"back in the lobby in the postgame");
		times_out_after(_machine_state_held, _network_game_server_state_ingame,
			NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT, "held");
		times_out_after(_machine_state_waiting, _network_game_server_state_ingame,
			NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT, "waiting");
		times_out_after(_machine_state_loading, _network_game_server_state_pregame,
			NETWORK_GAME_SERVER_LOADING_TIMEOUT, "loading at the start");
		times_out_after(_machine_state_loading, _network_game_server_state_ingame,
			NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT, "loading in progress");
		times_out_after(_machine_state_in_game, _network_game_server_state_ingame, 15000, "in game");
		/* (loaded, waiting in silence for the others: the server's wait) */
		times_out_after(_machine_state_in_game, _network_game_server_state_pregame, 0, "loaded at the start");
		/* (one that never answered the postgame keep-alive says nothing there) */
		times_out_after(_machine_state_in_game, _network_game_server_state_postgame, 0, "postgame, not answering");
		times_out_after(_machine_state_free, _network_game_server_state_ingame, 0, "free");
	}
	CASE("loading-wait")
	{
		/* once a machine has loaded, the wait for the others times out the rest */
		server.time_of_first_client_loading_completion = now;
		CHECK(!silent_for(machine_in(_machine_state_loading), 3600000), "a loading machine timed out twice over");
	}
	CASE("mcc-loading")
	{
		mcc_waiting[MACHINE] = TRUE;
		CHECK(!silent_for(machine_in(_machine_state_loading), 3600000), "an MCC conversion's wait was cut short");
	}
	CASE("downloading")
	{
		/* a download's progress times it out (map_share.c), in every state but in game */
		short state;

		downloading[MACHINE] = TRUE;
		for (state = _machine_state_lobby; state < _machine_state_in_game; state++)
			CHECK(!silent_for(machine_in(state), 3600000), "a downloading machine timed out in state %d", state);
		CHECK(silent_for(machine_in(_machine_state_connected), 3600000), "a connection that never joined stayed");
	}
	CASE("local")
	{
		struct network_game_server_client_machine *host = &server.client_machines[HOST];

		host->machine_index = HOST;
		host->state = _machine_state_lobby;
		host->last_heard_time = now - 3600000;
		CHECK(!network_game_server_client_machine_timed_out(&server, host), "the host's own machine timed out");
	}
	CASE("lobby-silent")
	{
		/* a machine in the lobby gone silent (its power cut) is timed out */
		CHECK(silent_for(machine_in(_machine_state_lobby), NETWORK_GAME_SERVER_LOBBY_TIMEOUT + 1000),
			"a silent machine stayed in the lobby");
	}
	CASE("lobby-heard")
	{
		/* (one that says it has the map every second stays) */
		struct network_game_server_client_machine *machine = machine_in(_machine_state_lobby);

		machine->last_heard_time = now - NETWORK_GAME_SERVER_LOBBY_TIMEOUT - 1000;
		network_game_server_client_machine_is_precached(&server, machine, multiplayer_map);
		CHECK(!network_game_server_client_machine_timed_out(&server, machine), "a machine heard was timed out");
		CHECK(TEST_FLAG(machine->flags, _network_client_machine_precached_bit), "its map was not taken");
	}
	CASE("dead-machine-countdown")
	{
		/* a dead machine whose precached map a map change cleared held the
		countdown for ever: now it is timed out, and the start goes on */
		struct network_game_server_client_machine *machine = machine_in(_machine_state_lobby);

		network_game_server_client_machine_is_precached(&server, machine, multiplayer_map);
		network_game_server_machine_event(machine, _machine_event_map_changed);
		CHECK(!network_game_server_have_all_machines_have_precached(&server), "fixture: the map change held nothing");
		now += NETWORK_GAME_SERVER_LOBBY_TIMEOUT + 1000;
		CHECK(network_game_server_client_machine_timed_out(&server, machine), "the dead machine was not timed out");
		network_game_server_machine_event(machine, _machine_event_drop);
		CHECK(network_game_server_have_all_machines_have_precached(&server), "the dead machine still holds the start");
	}
	CASE("postgame-silent")
	{
		/* a machine that answers the postgame keep-alive, gone silent there */
		struct network_game_server_client_machine *machine;

		reset(_network_game_server_state_postgame);
		machine = machine_in(_machine_state_in_game);
		network_game_server_client_machine_is_precached(&server, machine, "any map");
		CHECK(TEST_FLAG(machine->flags, _network_client_machine_answers_postgame_bit), "its answer was not taken");
		CHECK(!silent_for(machine, NETWORK_GAME_SERVER_POSTGAME_TIMEOUT - 1000), "timed out before its time");
		CHECK(silent_for(machine, NETWORK_GAME_SERVER_POSTGAME_TIMEOUT + 1000), "a silent machine stayed in the postgame");
	}
	CASE("round-reset-grace")
	{
		/* one silent in the postgame (a build that does not answer) has the
		lobby's time from the round's reset */
		struct network_game_server_client_machine *machine = machine_in(_machine_state_in_game);

		machine->last_heard_time = now - 3600000;
		network_game_server_machine_event(machine, _machine_event_round_reset);
		CHECK(machine->state == _machine_state_lobby && !network_game_server_client_machine_timed_out(&server, machine),
			"a machine back in the lobby was timed out for its postgame");
	}
	CASE("loaded-wrong-game")
	{
		/* a load of another game: ignored, not counted, not dropped */
		struct network_game_server_client_machine *machine;

		server.sent_start_game_message = TRUE;
		machine = machine_in(_machine_state_loading);
		CHECK(handle_loaded(machine, server.game.number_of_games_played - 1 + 1), "the machine was dropped");
		CHECK(machine->state == _machine_state_loading && !all_loaded, "a load of another game was counted");
		CHECK(handle_loaded(machine, server.game.number_of_games_played + 1), "the machine was dropped");
		CHECK(machine->state == _machine_state_in_game && all_loaded == 1, "its load of this game was not counted");
	}
	CASE("loaded-unnamed")
	{
		/* a build before this one names no game: taken as before */
		struct network_game_server_client_machine *machine;

		server.sent_start_game_message = TRUE;
		machine = machine_in(_machine_state_loading);
		CHECK(handle_loaded(machine, 0) && machine->state == _machine_state_in_game, "a load naming no game was lost");
	}
	CASE("loaded-twice-in-game")
	{
		/* a second load in game dropped the machine: now ignored */
		struct network_game_server_client_machine *machine;

		reset(_network_game_server_state_ingame);
		machine = machine_in(_machine_state_in_game);
		CHECK(handle_loaded(machine, server.game.number_of_games_played + 1), "a second load dropped the machine");
		CHECK(machine->state == _machine_state_in_game, "a second load changed its state");
	}
	CASE("loaded-late")
	{
		struct network_game_server_client_machine *machine;

		reset(_network_game_server_state_ingame);
		machine = machine_in(_machine_state_loading);
		CHECK(handle_loaded(machine, server.game.number_of_games_played + 1) && machine->state == _machine_state_in_game,
			"a late joiner's load was lost");
		machine = machine_in(_machine_state_waiting);
		CHECK(handle_loaded(machine, 0) && machine->state == _machine_state_waiting,
			"a load of a machine never started was taken");
	}
	CASE("version-old")
	{
		struct network_game_server_client_machine *machine = machine_in(_machine_state_connected);

		CHECK(network_game_server_refuse_other_version(machine, HALO_PORT_NETWORK_VERSION - 1), "not refused");
		CHECK(writes == 1 && sent_type == _message_server_machine_rejected && sent_reason == _rejection_code_version_too_old,
			"an older joiner was not told why (%d writes, reason %d)", writes, sent_reason);
	}
	CASE("version-new")
	{
		struct network_game_server_client_machine *machine = machine_in(_machine_state_connected);

		CHECK(network_game_server_refuse_other_version(machine, HALO_PORT_NETWORK_VERSION + 1), "not refused");
		CHECK(writes == 1 && sent_reason == _rejection_code_version_too_new, "a newer joiner was not told why");
	}
	CASE("version-same")
	{
		struct network_game_server_client_machine *machine = machine_in(_machine_state_connected);

		CHECK(!network_game_server_refuse_other_version(machine, HALO_PORT_NETWORK_VERSION) && !writes,
			"a joiner of this version was refused");
	}
	CASE("phase")
	{
		/* the one answer others ask for */
		static short const phases[NUMBER_OF_MACHINE_STATES] =
		{
			_network_machine_phase_none, _network_machine_phase_connecting, _network_machine_phase_lobby,
			_network_machine_phase_downloading, _network_machine_phase_waiting, _network_machine_phase_loading,
			_network_machine_phase_loaded,
		};
		short state;

		for (state = 0; state < NUMBER_OF_MACHINE_STATES; state++)
		{
			machine_in(state);
			CHECK(network_game_server_machine_phase(&server, MACHINE) == phases[state], "state %d's phase is %d",
				state, network_game_server_machine_phase(&server, MACHINE));
		}
		downloading[MACHINE] = TRUE;
		machine_in(_machine_state_lobby);
		CHECK(network_game_server_machine_phase(&server, MACHINE) == _network_machine_phase_downloading,
			"a machine downloading in the lobby was not");
	}
	/* a player gone while a machine loaded the game in progress, and another
	added at his machine index, controller and slot (a new machine at the
	lowest free index, the lowest free slot): the late joiner is told both,
	the gone one first */
	CASE("late-replaced")
	{
		struct network_player own = player_of(L"Joiner", MACHINE, 1, 5);
		struct network_player newcomer = player_of(L"Newcomer", OTHER, 1, 3);

		late_joiner_loads(&own, &newcomer);
		CHECK(removed_count == 1 && !wcscmp(removed[0].name, L"Ann"),
			"the player gone while it loaded was not removed (%d removed)", removed_count);
		CHECK(added_count == 1 && !wcscmp(added[0].name, L"Newcomer") && added[0].team_index == 1,
			"the player who took his place was not added (%d added)", added_count);
	}
	/* the same player no longer as the machine was told (a bot back on the
	other team, a name): told again; nothing changed, nothing told; and the
	machine's own players are never removed from it */
	CASE("late-changed")
	{
		struct network_player own = player_of(L"Joiner", MACHINE, 1, 5);
		struct network_player ann = player_of(L"Ann", OTHER, 0, 3);
		struct network_player ann_other_team = player_of(L"Ann", OTHER, 1, 3);
		struct network_player renamed = player_of(L"Anna", OTHER, 0, 3);
		struct network_player own_other_team = player_of(L"Joiner", MACHINE, 0, 5);

		late_joiner_loads(&own, &ann_other_team);
		CHECK(removed_count == 1 && added_count == 1 && added[0].team_index == 1,
			"a player on another team than the machine was told was not told again");
		late_joiner_loads(&own, &renamed);
		CHECK(removed_count == 1 && added_count == 1 && !wcscmp(added[0].name, L"Anna"),
			"a player of another name than the machine was told was not told again");
		late_joiner_loads(&own, &ann);
		CHECK(removed_count == 0 && added_count == 0, "players as the machine was told were told again");
		late_joiner_loads(&own_other_team, &ann);
		CHECK(removed_count == 0 && added_count == 0, "the machine's own player was removed from it");
	}
	/* what is a machine's in every state (its party's held room) ends with
	its drop, from any state, and with nothing else */
	CASE("drop-ends-party")
	{
		short state, event;

		for (state = _machine_state_connected; state < NUMBER_OF_MACHINE_STATES; state++)
		{
			for (event = 0; event < NUMBER_OF_MACHINE_EVENTS; event++)
			{
				if (event == _machine_event_drop)
					continue;
				party_dropped = NONE;
				network_game_server_machine_event(machine_in(state), event);
				CHECK(party_dropped == NONE, "event %s in state %s let the machine's party go",
					network_game_server_machine_event_names[event], network_game_server_machine_state_names[state]);
			}
			party_dropped = NONE;
			network_game_server_machine_event(machine_in(state), _machine_event_drop);
			CHECK(party_dropped == MACHINE, "a drop in state %s kept the machine's party",
				network_game_server_machine_state_names[state]);
		}
	}
	return 0;
}
