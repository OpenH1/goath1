/* The host's players' statistics to its clients (distributed_send_statistics),
looked at every STATISTICS_INTERVAL_TICKS and when a kill makes them due,
refreshed every STATISTICS_REFRESH_TICKS. To an OpenCE machine (machine 0),
and every machine where debug.network_forms_before, as before: each change at
once and STATISTICS_REFRESH_PLAYERS unchanged a second. To an OpenH1 one
(machine 1): each change at once and again on the next look not of its tick,
and STATISTICS_OPENH1_REFRESH_PLAYERS unchanged a second, round them all. A
change lost once heals within a look, lost twice within the refresh's round.
PLAYERS players, each its statistics a kill count. */
#include "harness.h"
typedef unsigned short word;
#define csmemset memset
#define csmemcpy memcpy
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
enum { MAXIMUM_TRACKED_PLAYERS = 16, HALO_PORT_MAXIMUM_NETWORK_MACHINES = 2, PLAYERS = 16, TICKS_PER_SECOND = 30,
	_distributed_message_player_statistics = 3 };
#define RELIABLE_ENTRIES(type) 64
struct game_statistics { short kills[4]; short deaths; long shots_fired; };
struct player_datum { struct game_statistics statistics; };
struct distributed_message_header { word header; byte type; byte count; long game_time; unsigned long checkpoint_epoch; };
#include "config.inc"

static struct player_datum players[MAXIMUM_TRACKED_PLAYERS];
static long now_time;
static boolean forms_before;
/* what each machine has of each player's kills, and how many entries it was
sent */
static short had[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static long entries_sent[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static long player_sends[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static boolean losing[HALO_PORT_MAXIMUM_NETWORK_MACHINES];

static long game_time_get(void) { return now_time; }
static int config_boolean(char const *name) { (void)name; return forms_before; }
static boolean network_legacy_machine(long machine_index) { return machine_index == 0; }
static struct player_datum *distributed_player(short player_index)
{
	return player_index < PLAYERS ? &players[player_index] : NULL;
}
static short distributed_client_machines(long *machine_indices, short maximum)
{
	(void)maximum;
	machine_indices[0] = 0;
	machine_indices[1] = 1;
	return 2;
}
static unsigned long distributed_checksum(void const *data, long size)
{
	byte const *bytes = (byte const *)data;
	unsigned long checksum = 2166136261UL;
	long index;

	for (index = 0; index < size; index++)
		checksum = ((checksum ^ bytes[index]) * 16777619UL) & 0xFFFFFFFFUL;
	return checksum;
}
static void distributed_send_to_machine(long machine_index, void *message, byte type, short count, word size)
{
	struct distributed_statistics_message const *statistics = (struct distributed_statistics_message const *)message;
	short index;

	(void)size;
	CHECK(type == _distributed_message_player_statistics, "type %d", type);
	entries_sent[machine_index] += count;
	if (losing[machine_index])
		return;
	for (index = 0; index < count; index++)
	{
		had[machine_index][statistics->players[index].player_index] = statistics->players[index].statistics.kills[0];
		player_sends[machine_index][statistics->players[index].player_index]++;
	}
}
#include "under_test.inc"

/* a tick of the host's, as network_distributed_tick calls it */
static void tick(boolean due)
{
	if (due || now_time % STATISTICS_INTERVAL_TICKS == 0)
		distributed_send_statistics(now_time % STATISTICS_REFRESH_TICKS == 0);
	now_time++;
}

static void start(void)
{
	short index;

	memset(players, 0, sizeof(players));
	memset(had, 0xFF, sizeof(had));
	memset(entries_sent, 0, sizeof(entries_sent));
	memset(player_sends, 0, sizeof(player_sends));
	memset(distributed_sent_statistics, 0, sizeof(distributed_sent_statistics));
	distributed_statistics_cursor = 0;
	distributed_statistics_openh1_cursor = 0;
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
		distributed_statistics_repeat_times[index] = NONE;
	now_time = 1;
	/* (the game's first: every player whole, then quiet) */
	while (now_time < 20 * TICKS_PER_SECOND)
		tick(now_time == 1);
	memset(entries_sent, 0, sizeof(entries_sent));
	memset(player_sends, 0, sizeof(player_sends));
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	long first;
	short index;

	start();
	if (!strcmp(name, "quiet") || !strcmp(name, "before"))
	{
		/* (nothing changes for 16 seconds: the OpenCE machine has every player
		every second, the OpenH1 one two a second, round them all) */
		forms_before = !strcmp(name, "before");
		first = now_time;
		while (now_time < first + 16 * TICKS_PER_SECOND)
			tick(FALSE);
		CHECK(entries_sent[0] == 16 * PLAYERS, "the OpenCE machine sent %ld", entries_sent[0]);
		if (forms_before)
			CHECK(entries_sent[1] == 16 * PLAYERS, "(forms before) the OpenH1 machine sent %ld", entries_sent[1]);
		else
		{
			CHECK(entries_sent[1] == 16 * STATISTICS_OPENH1_REFRESH_PLAYERS, "the OpenH1 machine sent %ld",
				entries_sent[1]);
			for (index = 0; index < PLAYERS; index++)
				CHECK(player_sends[1][index] == 2, "player %d refreshed %ld times in 16 s", index, player_sends[1][index]);
		}
		return 0;
	}
	if (!strcmp(name, "kill"))
	{
		/* (a kill: at once to both, again to the OpenH1 one within a look,
		not on the kill's tick, then no more but its refresh) */
		long kill_time;

		while (now_time % STATISTICS_INTERVAL_TICKS != 0)
			tick(FALSE);
		players[3].statistics.kills[0]++;
		kill_time = now_time;
		tick(TRUE);
		CHECK(had[0][3] == 1 && had[1][3] == 1, "the kill had at once (%d, %d)", had[0][3], had[1][3]);
		CHECK(player_sends[1][3] == 1, "the kill went %ld times on its tick", player_sends[1][3]);
		while (now_time <= kill_time + STATISTICS_INTERVAL_TICKS)
			tick(FALSE);
		CHECK(player_sends[1][3] == 2, "the kill went again within a look (%ld)", player_sends[1][3]);
		while (now_time < kill_time + 4 * STATISTICS_INTERVAL_TICKS)
			tick(FALSE);
		CHECK(player_sends[1][3] <= 3, "the kill went on going (%ld)", player_sends[1][3]);
		return 0;
	}
	if (!strcmp(name, "lost") || !strcmp(name, "lost-twice"))
	{
		/* (a kill on a tick that is a look: its send lost, or its repeat's
		too; the OpenH1 machine has it within a look, or the refresh's round) */
		boolean twice = !strcmp(name, "lost-twice");
		long kill_time;

		while (now_time % STATISTICS_INTERVAL_TICKS != 0)
			tick(FALSE);
		players[5].statistics.kills[0] = 7;
		kill_time = now_time;
		losing[1] = TRUE;
		tick(TRUE);
		if (!twice)
			losing[1] = FALSE;
		while (now_time <= kill_time + STATISTICS_INTERVAL_TICKS)
			tick(FALSE);
		losing[1] = FALSE;
		if (!twice)
		{
			CHECK(had[1][5] == 7, "a lost kill had again within a look (%d)", had[1][5]);
			return 0;
		}
		CHECK(had[1][5] != 7, "the test's two sends lost");
		while (had[1][5] != 7 && now_time < kill_time + 20 * TICKS_PER_SECOND)
			tick(FALSE);
		CHECK(had[1][5] == 7 && now_time - kill_time <= (PLAYERS / STATISTICS_OPENH1_REFRESH_PLAYERS + 1) *
			STATISTICS_REFRESH_TICKS, "a kill lost twice had %ld ticks on", now_time - kill_time);
		return 0;
	}
	if (!strcmp(name, "kill-twice-a-tick"))
	{
		/* (a kill made due off a look, and another of the same player's at
		the next look: each change at once, its repeat after) */
		while (now_time % STATISTICS_INTERVAL_TICKS != 1)
			tick(FALSE);
		players[2].statistics.kills[0]++;
		tick(TRUE);
		CHECK(player_sends[1][2] == 1, "the first kill at once");
		while (now_time % STATISTICS_INTERVAL_TICKS != 0)
			tick(FALSE);
		/* (the look's tick, made due by a second kill too: once) */
		players[2].statistics.kills[0]++;
		distributed_send_statistics(FALSE);
		tick(TRUE);
		CHECK(player_sends[1][2] == 2, "the second kill once on its tick, not repeated there (%ld)",
			player_sends[1][2]);
		return 0;
	}
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
