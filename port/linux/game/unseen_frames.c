/*
UNSEEN_FRAMES.C

A run whose frames nobody sees (debug.null_renderer, or no display: a
network test's headless host or client, a probe) draws no game frames
(main.c: the menus are still drawn, as their widgets' game data inputs run
as they are), and would otherwise run its frames back to back, thousands a second, each with
no step of the game's clock to run: the clock steps only as its time comes
(game_time_update). So each frame waits, before its time is measured, until
the clock's next step falls due (game_time_seconds_to_next_step): one step
a frame, a tick at game.sim_rate 30 and a sub-step at 60 and 120, as a
display of that rate would pace it. The frames' time is measured as before,
so the game plays as it did; only the frames that ran no step are gone.
While the clock does not run (loading, a pause), a frame waits a step of
game.sim_rate. A dedicated server paces its frames itself
(server_mode_pace_frame), and runs whose frames run fixed times
(debug.frame_time, debug.fixed_step, a movie) are not paced: they run as fast
as they can (main.c).

In an OpenH1 network game the frame waits before it reads its datagrams,
not after (main.c, network_clock_unseen_wait_first): read after, a datagram
that came in the wait was read only by the next frame, after the step the
wait was for, and what it brought ran a step later (a sub-step at 60 and
120, a tick at 30). A host also reads what comes while it waits, as a
dedicated server's frame does (unseen_frame_wait_receiving with
network_distributed_server_receive), so that each input is timed for its
client's clock by when it came, not by the read after it (network_clock.c).
*/

#include "cseries.h"
#include "game/game.h"
#include "sim_rate.h"

/* (the most a frame waits, in seconds: the console, the network and the
events are seen to at least this often) */
#define UNSEEN_FRAME_LONGEST_WAIT (1.0/30.0)

/* the platform layer's */
unsigned long long SDL_GetTicksNS(void);
void platform_delay_nanoseconds(unsigned long long nanoseconds);

static unsigned long long unseen_frame_last;

/* until when a frame waits that is now (nanoseconds of SDL_GetTicksNS) with
wait seconds to the clock's next step, the last frame's wait having ended at
last (0: none): at most UNSEEN_FRAME_LONGEST_WAIT, less the time since last,
which the clock counts already; to the next whole microsecond (the step is
then due, not a rounding short of it) */
unsigned long long unseen_frame_deadline(
	unsigned long long now,
	double wait,
	unsigned long long last)
{
	if (wait > UNSEEN_FRAME_LONGEST_WAIT)
		wait = UNSEEN_FRAME_LONGEST_WAIT;
	if (last && now > last)
		wait -= (double)(now - last)/1e9;
	if (wait <= 0.0)
		return now;
	return now + ((unsigned long long)(wait*1e6) + 1)*1000ULL;
}

/* the wait to the deadline: while 2 ms or more are left, receive (not NULL)
waits for datagrams up to a millisecond short of it, reading and handling
those that come (TRUE: it waited so; FALSE: it cannot, and the rest is
slept), as a dedicated server's frame does (server_mode.c's
server_mode_wait) */
void unseen_frame_wait_until(
	unsigned long long deadline,
	int (*receive)(long milliseconds),
	unsigned long long (*clock)(void),
	void (*sleep)(unsigned long long nanoseconds))
{
	for (;;)
	{
		unsigned long long now = clock();

		if (now >= deadline)
			return;
		if (receive && deadline - now >= 2000000ULL && receive((long)((deadline - now) / 1000000ULL) - 1))
			continue;
		sleep(deadline - now);
		return;
	}
}

void unseen_frame_wait_receiving(
	int (*receive)(long milliseconds))
{
	double wait = (double)game_time_seconds_to_next_step();

	if (wait < 0.0)
		wait = 1.0/(double)sim_rate_next();
	unseen_frame_wait_until(unseen_frame_deadline(SDL_GetTicksNS(), wait, unseen_frame_last), receive,
		SDL_GetTicksNS, platform_delay_nanoseconds);
	unseen_frame_last = SDL_GetTicksNS();
}

void unseen_frame_wait(
	void)
{
	unseen_frame_wait_receiving(NULL);
}

/* A machine whose frames are seen and paced by the frames' limit
(display.max_fps: sdl_platform.c's platform_video_swap) ran a step of the
clock only at the first frame's start after it fell due: up to a frame late
(6.9 ms at 144 frames a second, 16.7 at 60), by the frames' phase against
the steps, so that its input was sampled and sent, and a host's tick run,
that much later than the step's time. In an OpenH1 network game the
frame's wait wakes when a step falls due before the frame's time (main.c's
main_frame_step_due), and a frame of its own runs it then, its input read
then, drawing nothing; the wait then goes on to the frame's time.

When the wait wakes, now with left nanoseconds to the step (0 or less: due
already) and the frame's time at frame_due: at the step, where a frame that
runs it (cost nanoseconds) ends before the frame's time; else 0, the step
left for the frame, at most cost after it fell due */
unsigned long long frame_step_wake(
	unsigned long long now,
	unsigned long long frame_due,
	long long left,
	unsigned long long cost)
{
	unsigned long long wake = left > 0 ? now + (unsigned long long)left : now;

	if (wake + cost >= frame_due)
		return 0;
	return wake ? wake : 1;
}
