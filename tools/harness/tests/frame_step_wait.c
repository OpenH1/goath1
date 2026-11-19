/* A step of the game's clock that falls due in a seen frame's wait for the
frames' limit runs then, not at the next frame's start (sdl_platform.c's
platform_frame_wait_until, unseen_frames.c's frame_step_wake, main.c's
main_frame_step_due): the wait wakes at the step, before the frame's time,
and goes on to the frame's time after; a step due after the frame's time, or
too near it for a frame to run it first, is left for the frame; which
machines step so (network_clock.c's network_clock_steps_when_due). */
#include "harness.h"
#include <string.h>

typedef unsigned long long Uint64;
enum { _game_connection_local = 1, _game_connection_network_client = 2, _game_connection_network_server = 3 };

/* a fake clock in nanoseconds and a sleep that moves it; the step's time the
fake game gives (0: none) */
static Uint64 now_ns;
static int sleeps;
static Uint64 step_at;
static int asked;

static Uint64 fake_clock(void) { return now_ns; }
static void fake_sleep(Uint64 now, Uint64 until) { (void)now; now_ns = until; sleeps++; }
static unsigned long long fake_step_due(unsigned long long now, unsigned long long frame_due)
{
	(void)now;
	(void)frame_due;
	asked++;
	return step_at;
}

/* the connection and the settings, for network_clock_steps_when_due */
static short connection = _game_connection_network_client;
static long agreed_rate = 120;
static int timing_before;
static int unseen;
static short game_connection(void) { return connection; }
static long network_standard_agreed_rate(void) { return agreed_rate; }
static int halo_frames_unseen(void) { return unseen; }
static int config_boolean(char const *name) { return !strcmp(name, "debug.network_timing_before") && timing_before; }

#include "under_test.inc"

#define MS 1000000ULL

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	/* (144 frames a second) */
	Uint64 interval = 6944444ULL;

	if (!strcmp(name, "step-first"))
	{
		/* (the frame's time 6.9 ms away, a step due at 3 ms: the wait wakes
		at 3 ms, the frame's time still to come; then on to it) */
		Uint64 next_frame = interval;

		now_ns = 0;
		step_at = 3 * MS;
		CHECK(!platform_frame_wait_until(&next_frame, interval, fake_step_due, fake_clock, fake_sleep),
			"the wait ran to the frame's time");
		CHECK(now_ns == 3 * MS, "woke at %llu, not the step's 3 ms", now_ns);
		CHECK(next_frame == interval, "the frame's time moved: %llu", next_frame);
		/* (the step's frame ran; the next step is after the frame's time) */
		now_ns += MS;
		step_at = 0;
		CHECK(platform_frame_wait_until(&next_frame, interval, fake_step_due, fake_clock, fake_sleep),
			"woke again");
		CHECK(now_ns == interval && next_frame == 2 * interval, "at %llu, next %llu", now_ns, next_frame);
	}
	else if (!strcmp(name, "step-due"))
	{
		/* (a step due already as the wait begins: at once, no sleep) */
		Uint64 next_frame = interval;

		now_ns = 2 * MS;
		step_at = 2 * MS;
		sleeps = 0;
		CHECK(!platform_frame_wait_until(&next_frame, interval, fake_step_due, fake_clock, fake_sleep), "waited");
		CHECK(now_ns == 2 * MS && !sleeps, "slept to %llu", now_ns);
	}
	else if (!strcmp(name, "step-later"))
	{
		/* (none due before the frame's time, none wanted: the frame's wait as
		before, its grid kept) */
		Uint64 next_frame = interval;

		now_ns = 1 * MS;
		step_at = 0;
		CHECK(platform_frame_wait_until(&next_frame, interval, fake_step_due, fake_clock, fake_sleep), "woke early");
		CHECK(now_ns == interval && next_frame == 2 * interval, "at %llu, next %llu", now_ns, next_frame);
		next_frame = interval;
		now_ns = 1 * MS;
		CHECK(platform_frame_wait_until(&next_frame, interval, NULL, fake_clock, fake_sleep), "no hook: woke early");
		CHECK(now_ns == interval && next_frame == 2 * interval, "no hook: at %llu", now_ns);
		/* (a frame past its time: none asked, the count begun again) */
		asked = 0;
		next_frame = interval;
		now_ns = 3 * interval;
		CHECK(platform_frame_wait_until(&next_frame, interval, fake_step_due, fake_clock, fake_sleep) && !asked,
			"late frame");
		CHECK(next_frame == 4 * interval, "late frame's next %llu", next_frame);
	}
	else if (!strcmp(name, "wake"))
	{
		/* (a step 3 ms away, a frame costing 1.8 ms, the frame's time at 6.9 ms:
		at the step) */
		CHECK(frame_step_wake(10 * MS, 10 * MS + interval, 3 * MS, 1800000ULL) == 13 * MS, "at the step: %llu",
			frame_step_wake(10 * MS, 10 * MS + interval, 3 * MS, 1800000ULL));
		/* (due already: at once) */
		CHECK(frame_step_wake(10 * MS, 10 * MS + interval, -500000LL, 1800000ULL) == 10 * MS, "due already");
		/* (too near the frame's time for its frame to end first: left for the frame) */
		CHECK(frame_step_wake(10 * MS, 10 * MS + interval, 6 * MS, 1800000ULL) == 0, "too near the frame");
		/* (after the frame's time) */
		CHECK(frame_step_wake(10 * MS, 10 * MS + interval, 8 * MS, 0) == 0, "after the frame");
	}
	else if (!strcmp(name, "who"))
	{
		connection = _game_connection_network_client;
		CHECK(network_clock_steps_when_due(), "an OpenH1 game's client");
		connection = _game_connection_network_server;
		CHECK(network_clock_steps_when_due(), "an OpenH1 game's host");
		unseen = 1;
		CHECK(!network_clock_steps_when_due(), "a host whose frames nobody sees (it waits for its step)");
		unseen = 0;
		timing_before = 1;
		CHECK(network_clock_steps_when_due(), "debug.network_timing_before (steps when due, always)");
		timing_before = 0;
		agreed_rate = 0;
		CHECK(!network_clock_steps_when_due(), "a Legacy game");
		agreed_rate = 120;
		connection = _game_connection_local;
		CHECK(!network_clock_steps_when_due(), "a game of this machine alone");
	}
	else
		return 2;
	return 0;
}
