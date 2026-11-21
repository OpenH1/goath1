/* A frame nobody sees waits for the clock's next step (unseen_frames.c):
how long, and, in an OpenH1 network game, the datagrams that come meanwhile
read as they come (a host's) and never past the step; which machines wait
before their reads (network_clock.c). */
#include "harness.h"
#include <string.h>
#define UNSEEN_FRAME_LONGEST_WAIT (1.0/30.0)
enum { _game_connection_local = 1, _game_connection_network_client = 2, _game_connection_network_server = 3 };

/* a fake clock in nanoseconds, a sleep that moves it, and a receive that
takes datagrams due at the given times (moving the clock to each) or waits
out its time */
static unsigned long long now_ns;
static unsigned long long arrivals[8];
static int arrival_count, arrivals_taken;
static int receives;
static unsigned long long read_at[8];
static unsigned long long slept;

static unsigned long long fake_clock(void) { return now_ns; }
static void fake_sleep(unsigned long long nanoseconds) { now_ns += nanoseconds; slept += nanoseconds; }
static int fake_receive(long milliseconds)
{
	unsigned long long until = now_ns + (unsigned long long)milliseconds * 1000000ULL;

	receives++;
	if (arrivals_taken < arrival_count && arrivals[arrivals_taken] <= until)
	{
		if (arrivals[arrivals_taken] > now_ns)
			now_ns = arrivals[arrivals_taken];
		read_at[arrivals_taken++] = now_ns;
		return TRUE;
	}
	now_ns = until;
	return TRUE;
}

/* the connection and the settings, for network_clock_unseen_wait_first */
static short connection = _game_connection_network_client;
static long agreed_rate = 120;
static int timing_before;
static short game_connection(void) { return connection; }
static long network_standard_agreed_rate(void) { return agreed_rate; }
static int config_boolean(char const *name) { return !strcmp(name, "debug.network_timing_before") && timing_before; }

#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "deadline"))
	{
		/* (a sub-step at 120: 8333 us and a microsecond's rounding) */
		CHECK(unseen_frame_deadline(1000, 1.0 / 120.0, 0) == 1000 + 8334000ULL, "120: %llu",
			unseen_frame_deadline(1000, 1.0 / 120.0, 0) - 1000);
		/* (at most a 30th of a second, less the time since the last wait) */
		CHECK(unseen_frame_deadline(5000000ULL, 1.0, 0) == 5000000ULL + 33334000ULL, "capped");
		CHECK(unseen_frame_deadline(5000000ULL, 0.010, 4000000ULL) == 5000000ULL + 9001000ULL, "less the time since");
		CHECK(unseen_frame_deadline(5000000ULL, 0.0005, 4000000ULL) == 5000000ULL, "none left");
	}
	else if (!strcmp(name, "sleep"))
	{
		/* (no receive: the one sleep to the deadline) */
		now_ns = 100;
		unseen_frame_wait_until(100 + 8334000ULL, NULL, fake_clock, fake_sleep);
		CHECK(now_ns == 100 + 8334000ULL && slept == 8334000ULL, "slept %llu", slept);
	}
	else if (!strcmp(name, "receive"))
	{
		/* (a host: datagrams at 1 ms and 6.5 ms of an 8.334 ms wait, each read
		as it comes; the wait ends at the step, never past it) */
		unsigned long long deadline = 8334000ULL;

		now_ns = 0;
		arrivals[0] = 1000000ULL;
		arrivals[1] = 6500000ULL;
		arrival_count = 2;
		unseen_frame_wait_until(deadline, fake_receive, fake_clock, fake_sleep);
		CHECK(arrivals_taken == 2, "%d read in the wait", arrivals_taken);
		CHECK(read_at[0] == arrivals[0] && read_at[1] == arrivals[1], "read at %llu and %llu", read_at[0],
			read_at[1]);
		CHECK(now_ns == deadline, "ended at %llu, not %llu", now_ns, deadline);
		CHECK(receives >= 2, "%d receives", receives);
	}
	else if (!strcmp(name, "receive-short"))
	{
		/* (under 2 ms left: slept, not received: a receive's millisecond would
		overshoot the step) */
		now_ns = 0;
		arrivals[0] = 500000ULL;
		arrival_count = 1;
		unseen_frame_wait_until(1500000ULL, fake_receive, fake_clock, fake_sleep);
		CHECK(receives == 0 && now_ns == 1500000ULL, "%d receives, ended at %llu", receives, now_ns);
	}
	else if (!strcmp(name, "who"))
	{
		connection = _game_connection_network_client;
		CHECK(network_clock_unseen_wait_first(), "an OpenH1 game's client");
		connection = _game_connection_network_server;
		CHECK(network_clock_unseen_wait_first(), "an OpenH1 game's host");
		connection = _game_connection_local;
		CHECK(!network_clock_unseen_wait_first(), "a game of one machine");
		connection = _game_connection_network_server;
		agreed_rate = 0;
		CHECK(!network_clock_unseen_wait_first(), "a Legacy game");
		agreed_rate = 30;
		timing_before = TRUE;
		CHECK(!network_clock_unseen_wait_first(), "debug.network_timing_before");
	}
	else
		CHECK(0, "no case %s", name);
	return 0;
}
