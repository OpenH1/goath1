/* The one bound on a tick another machine states (player_queues_new.c's
distributed_tick_plausible and distributed_tick_stuck), where a client
player's latest is kept: its input (distributed_add_new_ticks), its
sub-steps (network_substeps.c's substep_record_heard) and its live
controls (live_control_heard). A tick no clock near the host's could have
run is not taken, and one kept that was no real tick gives way to the next
that is, so that one input stamped far ahead cannot have every real one
after it refused for the rest of the map; ticks in order, a clock that
leads by its trip, and the host's own update keys go as before. */
#include "harness.h"
typedef unsigned short word;
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
#define TICKS_PER_SECOND 30
#include "config.inc"
static struct { struct live_control live[4]; } network_substeps_globals;
#include "under_test.inc"

enum { NOW = 100000, FAR = 5000 };

static boolean valid;
static long last;
static unsigned short last_flags;
static unsigned long pending;
static unsigned short const flags[4] = { 1, 2, 4, 8 };

static boolean input(long tick, long now)
{
	return distributed_add_new_ticks(&valid, &last, &last_flags, tick, flags, 4, &pending, now);
}

static struct substep_record records[4];

static boolean record(long key, long now)
{
	struct substep_record heard;

	memset(&heard, 0, sizeof(heard));
	heard.key = key;
	heard.count = 2;
	substep_record_heard(records, 1, &heard, now);
	return records[1].valid && records[1].key == key;
}

static boolean live(long key, short substep, boolean host_update, long now)
{
	struct live_control heard;

	memset(&heard, 0, sizeof(heard));
	heard.key = key;
	heard.substep = substep;
	heard.count = 2;
	heard.host_update = host_update;
	return live_control_heard(1, &heard, now);
}

int main(int argc, char **argv)
{
	char const *case_name = argc == 2 ? argv[1] : "";

	/* an input stamped far ahead of any clock, then the client's
	real ones: refused, and the real ones taken */
	CASE("input-far-future")
	{
		CHECK(input(NOW, NOW), "first input");
		CHECK(!input(NOW + FAR * 100, NOW), "an input far in the future was taken");
		CHECK(input(NOW + 1, NOW + 1) && last == NOW + 1, "the real input after it was refused");
		CHECK(!input(0x7FFFFFFF, NOW + 1) && !input(-0x7FFFFFFF, NOW + 1), "a tick at the long's ends was taken");
		return 0;
	}
	/* ... one ahead within the window, taken, gives way to the next real one,
	which brings its own tick's buttons alone */
	CASE("input-stuck")
	{
		CHECK(input(NOW, NOW), "first input");
		CHECK(input(NOW + 400, NOW), "an input within the window refused");
		pending = 0;
		CHECK(input(NOW + 1, NOW + 1), "the real input after one stuck ahead was refused");
		CHECK(last == NOW + 1 && pending == 1, "the input after it took %lu, kept %ld", pending, last);
		pending = 0;
		CHECK(input(NOW + 3, NOW + 3) && pending == (1 | 2), "the next real input, a tick missed: %lu", pending);
		return 0;
	}
	/* (negative controls) the legitimate sequences: in order, buttons of the
	ticks missed, duplicates and reordered ones refused, a clock that leads
	by 12 ticks or trails by a second and a half, the host's own keys (NONE) */
	CASE("input-legit")
	{
		CHECK(input(NOW - 45, NOW) && pending == 1, "a trailing client's first input");
		CHECK(input(NOW - 44, NOW), "next");
		CHECK(!input(NOW - 44, NOW) && !input(NOW - 45, NOW), "a duplicate or older input was taken");
		pending = 0;
		CHECK(input(NOW - 41, NOW) && pending == (1 | 2 | 4), "the ticks missed' buttons: %lu", pending);
		CHECK(input(NOW + 12, NOW) && last == NOW + 12, "a client leading by its trip");
		CHECK(!input(NOW + 11, NOW + 1), "a reordered older input was taken");
		valid = FALSE;
		CHECK(input(5, NONE) && input(6, NONE) && !input(6, NONE), "the host's update keys");
		CHECK(input(NOW * 100, NONE), "the host's update keys bounded");
		return 0;
	}
	/* ... a client whose clock leads further than a clock leads (the host's
	stalled: its time is dropped) keeps its order, no reordered input taken */
	CASE("input-drift")
	{
		CHECK(input(NOW + 100, NOW), "a drifted client's input");
		CHECK(input(NOW + 101, NOW + 1), "its next");
		CHECK(!input(NOW + 99, NOW + 1), "a reordered older input of the drifted client was taken");
		return 0;
	}
	/* the same, of a sub-step record's key */
	CASE("substep-far-future")
	{
		CHECK(record(NOW, NOW), "first record");
		CHECK(!record(NOW + FAR, NOW), "a record far in the future was taken");
		CHECK(record(NOW + 1, NOW + 1), "the real record after it was refused");
		return 0;
	}
	CASE("substep-stuck")
	{
		CHECK(record(NOW, NOW), "first record");
		CHECK(record(NOW + 200, NOW), "a record within the window refused");
		CHECK(record(NOW + 1, NOW + 1), "the real record after one stuck ahead was refused");
		return 0;
	}
	CASE("substep-legit")
	{
		CHECK(record(NOW, NOW) && record(NOW + 1, NOW) && !record(NOW, NOW), "in order");
		CHECK(record(NOW + 12, NOW) && !record(NOW + 11, NOW), "a leading clock's");
		/* (a key ten seconds behind: the sender's game began again, as before) */
		CHECK(record(NOW + 12 - 300, NOW), "a game begun again");
		/* (the host's update keys: unbounded, as before) */
		memset(records, 0, sizeof(records));
		CHECK(record(7, NONE) && !record(6, NONE) && record(NOW * 100, NONE), "the host's updates");
		return 0;
	}
	CASE("live-far-future")
	{
		CHECK(live(NOW, 0, FALSE, NOW), "first live control");
		CHECK(!live(NOW + FAR, 0, FALSE, NOW), "a live control far in the future was taken");
		CHECK(live(NOW + 1, 0, FALSE, NOW + 1), "the real live control after it was refused");
		return 0;
	}
	CASE("live-stuck")
	{
		CHECK(live(NOW, 0, FALSE, NOW), "first live control");
		CHECK(live(NOW + 200, 0, FALSE, NOW), "a live control within the window refused");
		CHECK(live(NOW + 1, 1, FALSE, NOW + 1), "the real live control after one stuck ahead was refused");
		return 0;
	}
	CASE("live-legit")
	{
		CHECK(live(NOW, 0, FALSE, NOW) && live(NOW, 1, FALSE, NOW) && !live(NOW, 0, FALSE, NOW), "in order");
		CHECK(live(NOW + 12, 0, FALSE, NOW) && !live(NOW + 11, 1, FALSE, NOW), "a leading clock's");
		CHECK(live(9, 0, TRUE, NONE) && live(NOW * 100, 0, TRUE, NONE), "the host's update keys");
		return 0;
	}
	fprintf(stderr, "no such case: %s\n", case_name);
	return 2;
}
