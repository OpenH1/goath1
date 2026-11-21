/*
ROYALE_GULAG_QUEUE.C (test)

The real royale_gulag_queue_* (port/linux/game/royale_gulag.c): Royale's gulag
line. Players who die for good go in once a match, while it is open; they
fight in pairs in the order they arrived; a dead fighter's opponent wins; a
player alone in line wins by default after the limit (never while a fight is
on); a fight that runs out of time ends with both handed back. Players are
datum indices (an identifier in the high half). test_royale_gulag_queue.py
takes the code (under_test.inc).
*/

#include "harness.h"

#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))

#include "under_test.inc"

#define ALONE 100
#define FIGHT 1000

/* player n, as a datum index */
#define P(n) ((((long)(0x1000 + (n))) << 16) | (n))

static struct royale_gulag_queue queue;

static struct royale_gulag_event step(void)
{
	struct royale_gulag_event event;

	royale_gulag_queue_step(&queue, ALONE, FIGHT, &event);
	return event;
}

/* the host's players by absolute index; a client may not know one yet */
static long unknown_slot = -1;

static long player_of_slot(long slot)
{
	return slot == unknown_slot ? NONE : P(slot);
}

static void enter(long player)
{
	CHECK(royale_gulag_queue_enter(&queue, player, TRUE), "player %ld kept out of an open gulag", player & 0xFFFF);
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";
	struct royale_gulag_event event;
	short tick;

	royale_gulag_queue_reset(&queue);

	CASE("arrival-order")
	{
		enter(P(3));
		enter(P(1));
		enter(P(6));
		enter(P(2));
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_FIGHT && event.player[0] == P(3) && event.player[1] == P(1),
			"the first fight is not the first two to arrive (%d: %ld, %ld)", event.type, event.player[0] & 0xFFFF,
			event.player[1] & 0xFFFF);
		CHECK(royale_gulag_queue_opponent(&queue, P(3)) == P(1) && royale_gulag_queue_opponent(&queue, P(1)) == P(3),
			"the fighters are not each other's opponents");
		CHECK(royale_gulag_queue_place(&queue, P(6)) == 0 && royale_gulag_queue_place(&queue, P(2)) == 1,
			"the line is not in arrival order");
		/* (no second fight while one is on) */
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_NONE, "a second fight started during the first");
		CHECK(royale_gulag_queue_fighter_died(&queue, P(6)) == NONE, "a waiting player's death ended the fight");
		CHECK(royale_gulag_queue_fighter_died(&queue, P(1)) == P(3), "the dead fighter's opponent did not win");
		CHECK(royale_gulag_queue_fighter_died(&queue, P(3)) == NONE, "a fight ended twice (a trade's second death)");
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_FIGHT && event.player[0] == P(6) && event.player[1] == P(2),
			"the next fight is not the next two in line");
		return 0;
	}
	CASE("once-per-match")
	{
		CHECK(!royale_gulag_queue_enter(&queue, P(4), FALSE), "a closed gulag took a player");
		enter(P(4));
		CHECK(!royale_gulag_queue_enter(&queue, P(4), TRUE), "a player went to the gulag twice");
		enter(P(5));
		step();
		CHECK(royale_gulag_queue_fighter_died(&queue, P(5)) == P(4), "the winner is wrong");
		CHECK(!royale_gulag_queue_enter(&queue, P(4), TRUE), "a gulag winner went back on their next death");
		CHECK(!royale_gulag_queue_enter(&queue, NONE, TRUE), "no player went in");
		return 0;
	}
	CASE("win-by-default")
	{
		enter(P(0));
		for (tick = 1; tick < ALONE; tick++)
		{
			event = step();
			CHECK(event.type == ROYALE_GULAG_EVENT_NONE, "a default win %d ticks early", ALONE - tick);
		}
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_WIN_BY_DEFAULT && event.player[0] == P(0),
			"no default win after the limit (%d)", event.type);
		CHECK(royale_gulag_queue_place(&queue, P(0)) == NONE, "the default winner is still in line");
		/* never while a fight is on, and the wait starts over when it ends */
		enter(P(1));
		enter(P(2));
		enter(P(3));
		step();
		for (tick = 0; tick < 3 * ALONE; tick++)
		{
			event = step();
			CHECK(event.type == ROYALE_GULAG_EVENT_NONE, "a default win during a fight");
		}
		royale_gulag_queue_fighter_died(&queue, P(2));
		for (tick = 1; tick < ALONE; tick++)
			CHECK(step().type == ROYALE_GULAG_EVENT_NONE, "the wait did not start over after the fight");
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_WIN_BY_DEFAULT && event.player[0] == P(3), "no default win for the last");
		return 0;
	}
	CASE("time-up")
	{
		enter(P(1));
		enter(P(2));
		step();
		for (tick = 1; tick < FIGHT; tick++)
			CHECK(step().type == ROYALE_GULAG_EVENT_NONE, "time up %d ticks early", FIGHT - tick);
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_TIME_UP && event.player[0] == P(1) && event.player[1] == P(2),
			"no time up at the limit");
		CHECK(royale_gulag_queue_opponent(&queue, P(1)) == NONE, "the fight goes on after time up");
		return 0;
	}
	CASE("leave")
	{
		enter(P(1));
		enter(P(2));
		enter(P(3));
		enter(P(4));
		enter(P(5));
		step();
		CHECK(royale_gulag_queue_leave(&queue, P(4)) == NONE, "a waiting player's leaving won someone a fight");
		CHECK(royale_gulag_queue_place(&queue, P(3)) == 0 && royale_gulag_queue_place(&queue, P(5)) == 1,
			"the line lost its order when a player left");
		CHECK(royale_gulag_queue_leave(&queue, P(1)) == P(2), "a fighter left and the opponent did not win");
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_FIGHT && event.player[0] == P(3) && event.player[1] == P(5),
			"the fight after a leave is not the next two");
		return 0;
	}
	/* every player the port allows goes in (absolute indices to 127) */
	CASE("every-player")
	{
		enter(P(127));
		enter(P(100));
		enter(P(64));
		CHECK(!royale_gulag_queue_enter(&queue, P(128), TRUE), "an index past the players went in");
		event = step();
		CHECK(event.type == ROYALE_GULAG_EVENT_FIGHT && event.player[0] == P(127) && event.player[1] == P(100),
			"the high players' fight (%d)", event.type);
		CHECK(royale_gulag_queue_place(&queue, P(64)) == 0, "player 64 not in line");
		return 0;
	}

	/* the host's line on the wire: a client reads the same line and fight,
	the fight's start as a match time */
	CASE("network")
	{
		static byte buffer[512];
		struct royale_gulag_queue read;
		long fight_start = -5;
		long size;

		enter(P(3));
		enter(P(1));
		enter(P(120));
		enter(P(9));
		event = step();
		for (tick = 0; tick < 40; tick++)
			step();
		size = royale_gulag_queue_write(&queue, 1000, buffer, sizeof(buffer));
		CHECK(size == 3 + 2 + 4, "the line takes %ld bytes", size);
		CHECK(royale_gulag_queue_write(&queue, 1000, buffer, size - 1) == 0, "written past its room");
		CHECK(royale_gulag_queue_read(buffer, size, player_of_slot, &read, &fight_start) == size, "not read");
		CHECK(read.fighter[0] == P(3) && read.fighter[1] == P(1), "the fight read as %ld %ld", read.fighter[0] & 0xFFFF,
			read.fighter[1] & 0xFFFF);
		CHECK(read.waiting_count == 2 && read.waiting[0] == P(120) && read.waiting[1] == P(9), "the line read otherwise");
		CHECK(fight_start == 1000 - queue.fight_ticks, "the fight began at %ld, not %ld", fight_start,
			1000 - queue.fight_ticks);
		CHECK(royale_gulag_queue_place(&read, P(9)) == 1 && royale_gulag_queue_opponent(&read, P(1)) == P(3),
			"the client's HUD reads another line");
		/* (one the client has not yet is left out; a slot past the players refused) */
		unknown_slot = 120;
		CHECK(royale_gulag_queue_read(buffer, size, player_of_slot, &read, &fight_start) == size &&
			read.waiting_count == 1 && read.waiting[0] == P(9), "an unknown player kept in line");
		buffer[1] = 200;
		CHECK(royale_gulag_queue_read(buffer, size, player_of_slot, &read, &fight_start) == NONE,
			"a player past the limit read");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
