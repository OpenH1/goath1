/*
VOTEKICK_VOTE.C (test)

A vote of port/linux/game/network_votekick.c as an object with an identity
and an end: every choice names the vote's serial, so a choice for a vote
that has ended (or another) is refused and never starts or counts toward a
new one; a vote ended by a new game cools its starter down and leaves the
gap after it, so it is not started again at once; and a client's choices
name the vote it heard. test_votekick_vote.py takes the header
(config.inc) and the host's and the client's code (under_test.inc).
*/

#include "harness.h"

typedef unsigned short word;
#define NUMBEROF(array) ((long)(sizeof(array) / sizeof((array)[0])))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define csstrcmp strcmp
#define csstrcpy strcpy
#define csmemset memset
#define csmemcpy memcpy
#define TICKS_PER_SECOND 30
#define HALO_PORT_MAXIMUM_NETWORK_MACHINES 16
#define MAXIMUM_TRACKED_PLAYERS 16
#define NO_PLAYER 0xFF
#define _error_log 0

enum { _game_connection_local, _game_connection_network_client, _game_connection_network_server };
enum { _distributed_message_votekick = 1, _distributed_message_votekick_status, _distributed_to_host_reliably };
enum { _server_end_vote };

struct distributed_message_header { long unused; };

#include "config.inc"

/* the world: machines 0 to MACHINES - 1, one player each (its absolute
index the machine's), the host with none */
enum { MACHINES = 5 };

struct player_datum
{
	boolean quit_out_of_game;
	short local_player_index;
	short name[12];
};

static struct player_datum players[MACHINES];
static short connection = _game_connection_network_server;
static unsigned long now = 1000000;
static long game_time = 30 * 60 * 10;

struct data_iterator { long datum_index; long next; };
static int player_data;

static void data_iterator_new(struct data_iterator *iterator, int data) { (void)data; iterator->next = 0; iterator->datum_index = NONE; }
static void *data_iterator_next(struct data_iterator *iterator)
{
	if (iterator->next >= MACHINES)
		return NULL;
	iterator->datum_index = iterator->next++;
	return &players[iterator->datum_index];
}

static unsigned long system_milliseconds(void) { return now; }
static short game_connection(void) { return connection; }
static long game_time_get(void) { return game_time; }
static int config_boolean(const char *name) { (void)name; return TRUE; }
static long config_integer(const char *name)
{
	/* (no time played needed: every machine may start and vote) */
	if (!strcmp(name, "network.votekick_ban_minutes"))
		return 30;
	return 0;
}
static const char *config_string(const char *name) { (void)name; return "dynamic"; }
static void p2p_hardware_id_sanitize(char *destination, int size, const char *source) { snprintf(destination, (size_t)size, "%s", source ? source : ""); }
static long distributed_player_machine(short player_index) { return player_index; }
static struct player_datum *distributed_player(short player_index)
{
	return player_index >= 0 && player_index < MACHINES ? &players[player_index] : NULL;
}
static boolean distributed_player_is_local(short player_index) { (void)player_index; return FALSE; }
static byte distributed_player_to_byte(short player_index) { return player_index == NONE ? NO_PLAYER : (byte)player_index; }
static short distributed_player_from_byte(byte value) { return value == NO_PLAYER ? NONE : value; }
static unsigned long distributed_machine_real_address(long machine_index) { return 0x0A000001 + (unsigned long)machine_index; }
static unsigned long distributed_real_address(unsigned long address) { return address; }
static char hardware_ids[MACHINES][8];
static char const *network_game_server_machine_hardware_id(long machine_index)
{
	snprintf(hardware_ids[machine_index], sizeof(hardware_ids[machine_index]), "hw%ld", machine_index);
	return hardware_ids[machine_index];
}
static short distributed_client_machines(long *machine_indices, short maximum)
{
	short index;

	for (index = 0; index < MACHINES && index < maximum; index++)
		machine_indices[index] = index;
	return index;
}
static void distributed_machine_player_names(long machine_index, char *names, long size) { snprintf(names, (size_t)size, "p%ld", machine_index); }
static void distributed_send_to_machine(long machine_index, void const *message, short type, short count, word size)
{
	(void)machine_index; (void)message; (void)type; (void)count; (void)size;
}
static void distributed_send_notice(char const *text) { (void)text; }
static int refusals;
static void distributed_send_notice_to_machine(long machine_index, char const *text) { (void)machine_index; (void)text; refusals++; }
static void console_warning(const char *format, ...) { (void)format; refusals++; }
static void error(int kind, const char *format, ...) { (void)kind; (void)format; }
static int halo_server_mode(void) { return FALSE; }
static void server_director_end_game(int why, boolean now) { (void)why; (void)now; }
static boolean server_director_playing(void) { return TRUE; }
static boolean game_engine_running(void) { return TRUE; }
static void game_engine_end_game(void) {}
static long kicked = NONE;
static void network_game_server_kick_machine(long machine_index, boolean kept_out) { (void)kept_out; kicked = machine_index; }
static short local_player_get_next(short index) { (void)index; return NONE; }
/* (machine 4: an OpenCE one, or one of a build before serials) */
enum { BEFORE_SERIALS = 4 };
static boolean network_standard_machine_is_openh1(long machine_index) { return machine_index != BEFORE_SERIALS; }

/* a client's sent request, as it went */
static byte sent[4];
static int sends;
static void distributed_send(void const *message, short type, short count, word size, short how)
{
	(void)type; (void)count; (void)how;
	(void)size;
	memcpy(sent, (char const *)message + sizeof(struct distributed_message_header), 4);
	sends++;
}

#include "under_test.inc"

/* machine's request on its stream, as the wire has it: kind, player, choice,
vote (each a second apart: the host takes one a second) */
static void request(long machine_index, byte player_index, byte choice, byte vote)
{
	byte entries[4] = { _network_vote_kick, player_index, choice, vote };

	now += 1000;
	network_votekick_handle_request(machine_index, entries, TRUE);
}

enum { TARGET = 3, YES = 1, NO = 2 };

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	byte first;

	/* a ballot naming a vote that ended is refused: it neither counts in
	the vote running now nor starts one */
	CASE("ballot-for-ended-vote")
	{
		request(0, TARGET, YES, 0);
		CHECK(votekick.active, "machine 0's start started no vote");
		first = votekick.serial;
		CHECK(first != 0, "the vote has no serial");
		/* (it runs out) */
		now += 46 * 1000;
		game_time = 30 * 600;
		network_votekick_host_tick();
		CHECK(!votekick.active, "the vote did not time out");
		/* (its ballot after it, with no vote running: refused, no start) */
		request(1, TARGET, YES, first);
		CHECK(!votekick.active, "a ballot for the ended vote started one");
		/* (a new vote, against another, after the gap) */
		now += 31 * 1000;
		request(2, 4, YES, 0);
		CHECK(votekick.active && votekick.serial != first, "the new vote did not start, or has the old serial");
		CHECK(votekick.votes == 1, "%d yes at the start, not 1", votekick.votes);
		/* (a ballot for the old vote, naming this vote's target: not counted) */
		request(1, 4, YES, first);
		CHECK(votekick.votes == 1, "a ballot for the ended vote counted in the new one (%d yes)", votekick.votes);
		/* (the same ballot naming this vote: counted) */
		request(1, 4, YES, votekick.serial);
		CHECK(votekick.votes == 2, "a ballot for the vote running did not count (%d yes)", votekick.votes);
		return 0;
	}
	/* a vote ended by a new game: its starter cools down (as if it failed),
	the gap after it runs on, and no one else is kept from the target (no
	one voted it down) */
	CASE("game-end-cooldown")
	{
		request(0, TARGET, YES, 0);
		CHECK(votekick.active, "machine 0's start started no vote");
		network_votekick_new_game();
		CHECK(!votekick.active, "the vote outlived its game");
		/* (at once: the gap) */
		request(1, TARGET, YES, 0);
		CHECK(!votekick.active, "a vote started at once after the game ended the last");
		/* (after the gap: not its starter, but another may) */
		now += 31 * 1000;
		request(0, TARGET, YES, 0);
		CHECK(!votekick.active, "the starter of the vote the game ended started it again at once");
		request(1, TARGET, YES, 0);
		CHECK(votekick.active, "another could not vote against the target (protected though no one voted no)");
		return 0;
	}
	/* a yes sent before the map changed neither starts a vote after it nor
	counts in one */
	CASE("yes-across-map-change")
	{
		request(0, TARGET, YES, 0);
		first = votekick.serial;
		network_votekick_new_game();
		/* (the yes arrives in the new game) */
		request(1, TARGET, YES, first);
		CHECK(!votekick.active, "a yes from before the map change started a vote in the new game");
		now += 31 * 1000;
		request(2, TARGET, YES, 0);
		CHECK(votekick.active, "machine 2's start started no vote");
		request(1, TARGET, YES, first);
		CHECK(votekick.votes == 1, "a yes from before the map change counted in the new vote (%d yes)", votekick.votes);
		return 0;
	}
	/* a start names no vote: it is a yes (the starter's, or in the vote
	running if it is the same one), never a no */
	CASE("start-is-a-yes")
	{
		request(0, TARGET, NO, 0);
		CHECK(!votekick.active, "a no started a vote");
		request(0, TARGET, YES, 0);
		CHECK(votekick.active, "a start started no vote");
		request(1, TARGET, NO, 0);
		CHECK(votekick.no_votes == 0, "a no naming no vote counted");
		request(2, TARGET, YES, 0);
		CHECK(votekick.votes == 2, "the scoreboard's start in the same vote did not count as a yes (%d)", votekick.votes);
		request(1, TARGET, NO, votekick.serial);
		CHECK(votekick.no_votes == 1, "a no naming the vote did not count");
		return 0;
	}
	/* a machine of a build before serials (an OpenCE one) names no vote: its
	no counts in the vote running, and starts none */
	CASE("before-serials-no")
	{
		request(BEFORE_SERIALS, TARGET, NO, 0);
		CHECK(!votekick.active, "a no started a vote");
		request(0, TARGET, YES, 0);
		request(BEFORE_SERIALS, TARGET, NO, 0);
		CHECK(votekick.no_votes == 1, "the no of a machine before serials did not count (%d)", votekick.no_votes);
		return 0;
	}
	/* a client's choices name the vote it heard; a start names none */
	CASE("client-names-the-vote")
	{
		struct distributed_votekick_status status;

		connection = _game_connection_network_client;
		memset(&status, 0, sizeof(status));
		status.kind = _network_vote_kick;
		status.player_index = TARGET;
		status.flags = FLAG(_votekick_status_may_vote_bit);
		status.vote = 7;
		network_votekick_handle_status(&status);
		CHECK(network_vote_choose(TRUE) && sends == 1, "no choice sent");
		CHECK(sent[2] == YES && sent[3] == 7, "the yes named vote %d, not 7", sent[3]);
		CHECK(network_votekick_request(TARGET) && sent[3] == 7, "the scoreboard's yes in the vote named %d, not 7", sent[3]);
		CHECK(network_votekick_request(4) && sent[3] == 0, "a start against another named vote %d", sent[3]);
		return 0;
	}
	fprintf(stderr, "no such case: %s\n", case_name);
	return 2;
}
