/* The host's states of a player at its sub-steps go to a client only while
that client asked for them (network_distributed_host_substep_ended,
distributed_substep_states_wanted, distributed_handle_substep_requests), and
a client asks only where its copy of the player went wrong: a correction
past the tolerance, or a forwarded input missed
(distributed_client_send_substep_requests). The ask opens a window of
SUBSTEP_REQUEST_TICKS of the host's ticks, renewed while the copy keeps
going wrong (at most once in SUBSTEP_REQUEST_RENEW_TICKS), and the window
ends once it stops. debug.network_forms_before sends them as
before. Each tick: the client's end of tick (its asks, delivered to the host
as machine 1's), then the host's sub-steps but the last. */
#include "harness.h"
typedef unsigned short word;
typedef word message_header;
#define csmemcpy memcpy
#define csmemset memset
#define MIN(a, b) ((a) < (b) ? (a) : (b))
enum { _game_connection_network_server = 2, _game_connection_network_client = 1 };
enum
{
	HALO_PORT_MAXIMUM_NETWORK_MACHINES = 4,
	MAXIMUM_TRACKED_PLAYERS = 8,
	DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE = 40,
	SUBSTEPS = 4,
	/* the player each case drives, and one near both clients too */
	PLAYER = 3,
	OTHER = 5,
};
#include "config.inc"

struct distributed_unit_state { byte player_index; };
struct player_datum { long unused; };

static byte distributed_forward_periods[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static int distributed_substep_sents[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];

/* the world: the host's clients 1 and 2, OpenH1; players PLAYER and OTHER
near both; the client's (machine 1's) keys of the forwarded input its tick
ran of each player */
static long now_time = 1000;
static boolean always;
static long keys[MAXIMUM_TRACKED_PLAYERS];
static struct player_datum players[MAXIMUM_TRACKED_PLAYERS];

static long game_time_get(void) { return now_time; }
static short game_connection(void) { return _game_connection_network_server; }
static boolean game_in_progress(void) { return TRUE; }
static boolean network_standard_hosting(void) { return TRUE; }
static boolean network_standard_machine_is_openh1(long machine_index) { (void)machine_index; return TRUE; }
static boolean network_substeps_active(void) { return TRUE; }
static int config_boolean(char const *name)
{
	CHECK(!strcmp(name, "debug.network_forms_before"), "the setting read: %s", name);
	return always;
}
static long network_substeps_forwarded_key(short player_index) { return keys[player_index]; }
static short distributed_client_machines(long *indices, short maximum)
{
	(void)maximum;
	indices[0] = 1;
	indices[1] = 2;
	return 2;
}
static struct player_datum *distributed_player(short player_index) { return &players[player_index]; }
static void distributed_state_from_player(short player_index, long time, struct distributed_unit_state *state)
{
	(void)time; state->player_index = (byte)player_index;
}
static boolean distributed_substep_state_due(long machine_index, short player_index,
	struct distributed_unit_state const *state)
{
	(void)machine_index; (void)player_index; (void)state; return TRUE;
}
static boolean distributed_substep_state_unchanged(int *sent, struct distributed_unit_state const *state, long now)
{
	(void)sent; (void)state; (void)now; return FALSE;
}
static word distributed_substep_state_write(long machine_index, struct distributed_unit_state const *state,
	byte *buffer)
{
	(void)machine_index;
	buffer[0] = state->player_index;
	return 1;
}

/* the states the host sent each machine of each player this tick */
static short states_sent[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static long packing = NONE;
static void distributed_packer_begin(void *packer, short machine_index, byte type, void const *prefix, word size)
{
	(void)packer; (void)prefix; (void)size;
	CHECK(type == _distributed_message_substep_states, "the states' kind");
	packing = machine_index;
}
static void distributed_packer_add(void *packer, byte const *entry, word size)
{
	(void)packer;
	CHECK(size == 1 && packing != NONE, "an entry of the message begun");
	states_sent[packing][entry[0]]++;
}
static void distributed_packer_flush(void *packer) { (void)packer; packing = NONE; }
static int distributed_packer;
static void distributed_forward_flush(int occasion) { (void)occasion; }
static void distributed_batch_flush(short machine_index) { (void)machine_index; }

/* the client's asks this tick, as sent to the host */
static struct
{
	struct distributed_message_header header;
	struct distributed_substep_request entries[MAXIMUM_TRACKED_PLAYERS];
} asked;
static short asks;
static void distributed_send(void *message, byte type, short count, word size, short destination)
{
	CHECK(type == _distributed_message_substep_state_requests && destination == _distributed_to_host,
		"an ask to the host");
	CHECK(size == sizeof(struct distributed_message_header) + count * sizeof(struct distributed_substep_request),
		"its size");
	memcpy(&asked, message, size);
	asks = count;
}

#include "under_test.inc"

static void reset(void)
{
	short machine_index;
	short player_index;

	memset(distributed_forward_periods, 0, sizeof(distributed_forward_periods));
	for (machine_index = 0; machine_index < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine_index++)
		for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
			distributed_substep_requested_until[machine_index][player_index] = NONE;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
	{
		keys[player_index] = NONE;
		distributed_substep_wants[player_index].last_key = NONE;
		distributed_substep_wants[player_index].asked_time = NONE;
		distributed_substep_wants[player_index].wrong = FALSE;
	}
	distributed_forward_periods[1][PLAYER] = distributed_forward_periods[2][PLAYER] = 1;
	distributed_forward_periods[1][OTHER] = distributed_forward_periods[2][OTHER] = 1;
	keys[PLAYER] = 500;
	keys[OTHER] = 700;
	always = FALSE;
}

/* one tick: the client's asks (delivered unless lost), the host's sub-steps;
the client's keys then advanced (a forwarded input of each run) */
static void tick(boolean lost)
{
	short substep;
	short player_index;

	asks = 0;
	distributed_client_send_substep_requests();
	if (asks && !lost)
		distributed_handle_substep_requests(1, asked.entries, asks);
	memset(states_sent, 0, sizeof(states_sent));
	for (substep = 0; substep < SUBSTEPS; substep++)
		network_distributed_host_substep_ended(substep, SUBSTEPS);
	now_time++;
	for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS; player_index++)
		if (keys[player_index] != NONE)
			keys[player_index]++;
}

/* every forwarded input came and nothing was corrected: no ask, no state */
static void arrive_case(void)
{
	short count;

	reset();
	for (count = 0; count < 60; count++)
	{
		tick(FALSE);
		CHECK(!asks, "no ask (tick %d)", count);
		CHECK(!states_sent[1][PLAYER] && !states_sent[2][PLAYER] && !states_sent[1][OTHER],
			"no state at the sub-steps (tick %d: %d)", count, states_sent[1][PLAYER]);
	}
}

/* a forwarded input missed (the key skips one): the client asks, and the
host sends that player's states to it alone, at each sub-step but the last,
for the window; none after it */
static void dropped_case(void)
{
	short count;
	long first;

	reset();
	tick(FALSE);
	keys[PLAYER]++;
	first = now_time;
	tick(FALSE);
	CHECK(asks == 1 && asked.entries[0].player_index == PLAYER && asked.entries[0].ticks == SUBSTEP_REQUEST_TICKS,
		"one ask, of the player, for the window (%d)", asks);
	for (count = 0; count < 40; count++)
	{
		/* (the ask's own tick the first) */
		long tick_time = count ? now_time : first;
		boolean within = tick_time - first < SUBSTEP_REQUEST_TICKS;

		if (count)
		{
			tick(FALSE);
			CHECK(!asks, "no ask again while nothing goes wrong (tick %d)", count);
		}
		CHECK(states_sent[1][PLAYER] == (within ? SUBSTEPS - 1 : 0),
			"the player's states %s the window (tick %d: %d)", within ? "within" : "after", count,
			states_sent[1][PLAYER]);
		CHECK(!states_sent[2][PLAYER], "none to the machine that did not ask");
		CHECK(!states_sent[1][OTHER], "none of a player not asked for");
	}
}

/* a correction past the tolerance (distributed_place_remote_unit notes it):
the client asks */
static void wrong_case(void)
{
	reset();
	tick(FALSE);
	distributed_substep_wants[OTHER].wrong = TRUE;
	tick(FALSE);
	CHECK(asks == 1 && asked.entries[0].player_index == OTHER, "the corrected player asked for (%d)", asks);
	CHECK(states_sent[1][OTHER] == SUBSTEPS - 1, "and sent at once");
	CHECK(!distributed_substep_wants[OTHER].wrong, "the ask made of it");
}

/* the copy going wrong every tick: asked again once in
SUBSTEP_REQUEST_RENEW_TICKS, the window running on past its first end;
once it stops, the window ends SUBSTEP_REQUEST_TICKS after the last ask */
static void renew_case(void)
{
	short count;
	short total = 0;
	long last = NONE;

	reset();
	for (count = 0; count < 40; count++)
	{
		distributed_substep_wants[PLAYER].wrong = TRUE;
		tick(FALSE);
		if (asks)
		{
			total++;
			last = now_time - 1;
		}
		CHECK(states_sent[1][PLAYER] == SUBSTEPS - 1, "sent while it goes wrong (tick %d)", count);
	}
	CHECK(total == (40 + SUBSTEP_REQUEST_RENEW_TICKS - 1) / SUBSTEP_REQUEST_RENEW_TICKS,
		"asked once in each %d ticks: %d asks", SUBSTEP_REQUEST_RENEW_TICKS, total);
	for (count = 0; count < 2 * SUBSTEP_REQUEST_TICKS; count++)
	{
		long tick_time = now_time;

		tick(FALSE);
		/* (but the first, of a correction made while the last ask was too
		recent to renew: the window then runs from it) */
		CHECK(!asks || (count == 0 && tick_time - last == SUBSTEP_REQUEST_RENEW_TICKS), "no ask once it stops");
		if (asks)
			last = tick_time;
		CHECK(states_sent[1][PLAYER] == (tick_time - last < SUBSTEP_REQUEST_TICKS ? SUBSTEPS - 1 : 0),
			"the window ends %d ticks after the last ask (tick %ld after it: %d)", SUBSTEP_REQUEST_TICKS,
			tick_time - last, states_sent[1][PLAYER]);
	}
}

/* an ask lost: the next correction past the renewal's wait asks again */
static void lost_case(void)
{
	short count;

	reset();
	distributed_substep_wants[PLAYER].wrong = TRUE;
	tick(TRUE);
	CHECK(asks == 1 && !states_sent[1][PLAYER], "asked, lost, nothing sent");
	for (count = 1; count < SUBSTEP_REQUEST_RENEW_TICKS; count++)
	{
		distributed_substep_wants[PLAYER].wrong = TRUE;
		tick(FALSE);
		CHECK(!asks, "not asked again before the renewal's wait");
	}
	distributed_substep_wants[PLAYER].wrong = TRUE;
	tick(FALSE);
	CHECK(asks == 1 && states_sent[1][PLAYER] == SUBSTEPS - 1, "asked again, and sent");
}

/* debug.network_forms_before: every near player's states to
every client, asked for or not, as before */
static void always_case(void)
{
	short count;

	reset();
	always = TRUE;
	for (count = 0; count < 10; count++)
	{
		tick(FALSE);
		CHECK(!asks, "nothing asked");
		CHECK(states_sent[1][PLAYER] == SUBSTEPS - 1 && states_sent[2][PLAYER] == SUBSTEPS - 1 &&
			states_sent[1][OTHER] == SUBSTEPS - 1 && states_sent[2][OTHER] == SUBSTEPS - 1,
			"every near player's to every client (tick %d)", count);
	}
}

/* what a client asks for is bounded: no player past the tracked ones, no
window of none, none longer than SUBSTEP_REQUEST_MAXIMUM_TICKS; a longer
window already open is kept */
static void bounds_case(void)
{
	struct distributed_substep_request requests[3];

	reset();
	requests[0].player_index = MAXIMUM_TRACKED_PLAYERS;
	requests[0].ticks = 10;
	requests[1].player_index = OTHER;
	requests[1].ticks = 0;
	requests[2].player_index = PLAYER;
	requests[2].ticks = 255;
	distributed_handle_substep_requests(1, requests, 3);
	distributed_handle_substep_requests(NONE, requests, 3);
	distributed_handle_substep_requests(HALO_PORT_MAXIMUM_NETWORK_MACHINES, requests, 3);
	CHECK(distributed_substep_requested_until[1][OTHER] == NONE, "no window of none");
	CHECK(distributed_substep_requested_until[1][PLAYER] == now_time + SUBSTEP_REQUEST_MAXIMUM_TICKS,
		"at most %d ticks (%ld)", SUBSTEP_REQUEST_MAXIMUM_TICKS, distributed_substep_requested_until[1][PLAYER] - now_time);
	requests[2].ticks = 2;
	distributed_handle_substep_requests(1, &requests[2], 1);
	CHECK(distributed_substep_requested_until[1][PLAYER] == now_time + SUBSTEP_REQUEST_MAXIMUM_TICKS,
		"a longer window kept");
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "arrive"))
		arrive_case();
	else if (!strcmp(name, "dropped"))
		dropped_case();
	else if (!strcmp(name, "wrong"))
		wrong_case();
	else if (!strcmp(name, "renew"))
		renew_case();
	else if (!strcmp(name, "lost"))
		lost_case();
	else if (!strcmp(name, "always"))
		always_case();
	else if (!strcmp(name, "bounds"))
		bounds_case();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
