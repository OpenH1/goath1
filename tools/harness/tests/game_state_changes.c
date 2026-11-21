/* The host's game type's state to an OpenH1 client as its changes since the
state last sent it (distributed_game_state_changes_write, _read, _due): an
unchanged state sends nothing; a change of one field sends a few bytes that
make the host's state exactly; a client that joins (or has not the state
last sent) is sent the whole state; a client that missed changes (its state
not the host's) takes none of the later ones and is sent the whole state
within GAME_STATE_REFRESH_TICKS; a change of most of the state goes whole.
The host looks every GAME_STATE_INTERVAL_TICKS, as network_distributed_tick. */
#include "harness.h"
#define csmemcpy memcpy
#define csmemcmp memcmp
#define csmemset memset
typedef unsigned short word;
enum { TICKS_PER_SECOND = 30, STATE_SIZE = 1412 };
#include "config.inc"
#include "under_test.inc"

/* the client: the state it has, and the state the game took */
static byte had[MAXIMUM_GAME_STATE_SIZE];
static long had_size;
static byte taken[MAXIMUM_GAME_STATE_SIZE];
static long taken_size;
/* the host's */
static byte state[MAXIMUM_GAME_STATE_SIZE];
static byte sent[MAXIMUM_GAME_STATE_SIZE];
static long sent_size;
static struct distributed_game_state_machine machine;
static long wholes, changes_sent, bytes;

static void client_whole(byte const *data, long size)
{
	memcpy(had, data, size);
	had_size = size;
	memcpy(taken, data, size);
	taken_size = size;
}

/* a look of the host's (every GAME_STATE_INTERVAL_TICKS): as distributed_send_game_state
with one client; lost, the message is not taken (a client that could not take it) */
static void host_look(long now, boolean lost)
{
	static byte changes[MAXIMUM_GAME_STATE_SIZE];
	boolean changed = sent_size != STATE_SIZE || memcmp(sent, state, STATE_SIZE) != 0;
	long changes_size = 0;

	if (changed)
	{
		changes_size = distributed_game_state_changes_write(sent, sent_size, state, STATE_SIZE, changes,
			sizeof(changes));
		memcpy(sent, state, STATE_SIZE);
		sent_size = STATE_SIZE;
	}
	switch (distributed_game_state_due(&machine, changed, changes_size > 0, now))
	{
	case _game_state_send_whole:
		wholes++;
		bytes += STATE_SIZE;
		if (!lost)
			client_whole(state, STATE_SIZE);
		machine.shared = TRUE;
		machine.changed_since_whole = FALSE;
		machine.whole_time = now;
		break;
	case _game_state_send_changes:
		changes_sent++;
		bytes += changes_size;
		if (!lost && distributed_game_state_changes_read(had, &had_size, changes, changes_size))
		{
			memcpy(taken, had, had_size);
			taken_size = had_size;
		}
		machine.changed_since_whole = TRUE;
		break;
	}
}

static boolean client_current(void)
{
	return taken_size == STATE_SIZE && !memcmp(taken, state, STATE_SIZE);
}

static void fill(byte *data, long size, unsigned seed)
{
	long index;

	for (index = 0; index < size; index++)
	{
		seed = seed * 1103515245u + 12345u;
		data[index] = (byte)(seed >> 16);
	}
}

/* the host's state and the client's, with the client sent the whole state */
static void joined(void)
{
	fill(state, STATE_SIZE, 7);
	memset(&machine, 0, sizeof(machine));
	sent_size = 0;
	host_look(0, FALSE);
	CHECK(wholes == 1 && client_current(), "a joiner was not sent the whole state (%ld whole)", wholes);
	wholes = changes_sent = bytes = 0;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	long now;

	if (!strcmp(name, "unchanged"))
	{
		joined();
		for (now = GAME_STATE_INTERVAL_TICKS; now < 60 * TICKS_PER_SECOND; now += GAME_STATE_INTERVAL_TICKS)
			host_look(now, FALSE);
		CHECK(wholes + changes_sent == 0, "an unchanged state sent %ld whole, %ld changes", wholes, changes_sent);
		return 0;
	}
	if (!strcmp(name, "one-field"))
	{
		joined();
		/* (a score of 4 bytes) */
		state[700] ^= 0x5A;
		state[702] ^= 0x01;
		host_look(GAME_STATE_INTERVAL_TICKS, FALSE);
		CHECK(changes_sent == 1 && wholes == 0, "%ld changes, %ld whole for one field", changes_sent, wholes);
		CHECK(bytes <= GAME_STATE_CHANGES_HEADER_SIZE + GAME_STATE_RUN_HEADER_SIZE + 4,
			"one field's change took %ld bytes", bytes);
		CHECK(client_current(), "the client's state is not the host's after one field's change");
		return 0;
	}
	if (!strcmp(name, "joiner"))
	{
		fill(state, STATE_SIZE, 3);
		/* (another client's changes went all along: this one joins with none) */
		memcpy(sent, state, STATE_SIZE);
		sent_size = STATE_SIZE;
		state[10]++;
		memset(&machine, 0, sizeof(machine));
		had_size = taken_size = 0;
		host_look(GAME_STATE_INTERVAL_TICKS, FALSE);
		CHECK(wholes == 1 && changes_sent == 0, "a client without the state got %ld changes", changes_sent);
		CHECK(client_current(), "a joiner's state is not the host's");
		state[11]++;
		host_look(2 * GAME_STATE_INTERVAL_TICKS, FALSE);
		CHECK(changes_sent == 1 && client_current(), "after its whole state, a joiner not sent changes alone");
		return 0;
	}
	if (!strcmp(name, "lost"))
	{
		long healed = NONE;
		long lost_at = 10 * GAME_STATE_INTERVAL_TICKS;

		joined();
		/* (a score that changes every look: the king's, the ball's) */
		for (now = GAME_STATE_INTERVAL_TICKS; now < 40 * TICKS_PER_SECOND; now += GAME_STATE_INTERVAL_TICKS)
		{
			state[100 + (now / GAME_STATE_INTERVAL_TICKS) % 4]++;
			/* (and, in the lost one, a field that changes no more: a flag taken) */
			if (now == lost_at)
				state[900]++;
			host_look(now, now == lost_at);
			if (now == lost_at)
				CHECK(!client_current(), "the lost change was had");
			if (now > lost_at && healed == NONE && client_current())
				healed = now;
			if (now > lost_at + GAME_STATE_INTERVAL_TICKS && healed == NONE)
				CHECK(had_size == 0, "changes taken onto a state the host did not have");
		}
		CHECK(healed != NONE && healed - lost_at <= GAME_STATE_REFRESH_TICKS + GAME_STATE_INTERVAL_TICKS,
			"a lost change healed after %ld ticks", healed == NONE ? -1 : healed - lost_at);
		/* (and it costs a whole state a refresh, not every look) */
		CHECK(wholes <= 40 * TICKS_PER_SECOND / GAME_STATE_REFRESH_TICKS + 1, "%ld whole states", wholes);
		return 0;
	}
	if (!strcmp(name, "most-changed"))
	{
		joined();
		fill(state, STATE_SIZE, 99);
		host_look(GAME_STATE_INTERVAL_TICKS, FALSE);
		CHECK(wholes == 1 && changes_sent == 0, "a state changed throughout went as changes");
		CHECK(client_current(), "the client's state is not the host's");
		return 0;
	}
	if (!strcmp(name, "exact"))
	{
		/* (every game type's state: changes of any bytes, anywhere, make the host's exactly) */
		unsigned seed = 1;
		long round;

		joined();
		for (round = 0; round < 2000; round++)
		{
			long count = 1 + round % 24;
			long index;

			for (index = 0; index < count; index++)
			{
				seed = seed * 1103515245u + 12345u;
				state[(seed >> 8) % STATE_SIZE] = (byte)(seed >> 3);
			}
			if (round % 97 == 0)
			{
				state[0]++;
				state[STATE_SIZE - 1]++;
			}
			host_look((round + 1) * GAME_STATE_INTERVAL_TICKS, FALSE);
			CHECK(client_current(), "round %ld: the client's state is not the host's", round);
		}
		CHECK(changes_sent > 1900, "only %ld of the changes went as changes", changes_sent);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
