/* The game's frames with VRR (frame_pacing.h's HOST_RENDER_DISPLAY_VARIABLE,
host_render_stream.c's display_wait): a display of variable refresh shows
each frame once its GPU work is done, and says it took it later still (the
compositor's answer). A model of one game, its render thread, the GPU and
the display, frame by frame, the game's next frame started as display_wait
starts it: no more than one frame before on the GPU (the GPU's bound), no
more than FRAME_PACING_VARIABLE_QUEUED waiting for the display, its lead
before the GPU is predicted free (frame_pacing_gpu_free,
frame_pacing_variable_start_time), and the frame cap's grid
(sdl_platform.c's platform_frame_wait_until). The frames must come at the
cap whenever the longer of the game's and the GPU's times fits it (not at
their sum: a beta's 4K VRR log, 70 to 100 frames a second under a cap of
135), at the GPU's rate where it does not, and each frame must wait for the
GPU no longer than the margin. */
#include "harness.h"
#include <stdint.h>
#include "under_test.inc"

#define FRAMES 2400
#define FIRST 400
#define MS 1000000ull
/* the cap: 135 frames a second on a 138 Hz display */
#define CAP (1000000000ull / 135)
#define REFRESH_LEAST (1000000000ull / 138)
#define WAIT_MOST FRAME_PACING_WAIT_MAXIMUM_NS

struct scene
{
	char const *name;
	/* the game's time a frame (and up to jitter more), the render thread's
	after the game has given it, the GPU's (and up to jitter more), the
	display's time to show a frame its GPU has done, and to say it took it */
	uint64_t game, game_jitter, render, gpu, gpu_jitter, show, answer;
};

static uint64_t start[FRAMES + 2], submitted[FRAMES + 2], handed[FRAMES + 2], gpu_done[FRAMES + 2];
static uint64_t shown[FRAMES + 2], taken[FRAMES + 2];
static uint64_t seed = 88172645463325252ull;

static uint64_t jitter(uint64_t most)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return most ? seed % most : 0;
}

static uint64_t later(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

/* the frames played: their period and the time from a frame's start to the
screen, on average after the first FIRST */
static void play(struct scene const *scene, double *period, double *latency)
{
	struct frame_pacing_costs leads;
	struct frame_pacing_gpu gpu;
	uint64_t next_frame = 0, total = 0;
	uint32_t frame, retired = 0, measured = 0, led = 0;

	memset(&leads, 0, sizeof(leads));
	memset(&gpu, 0, sizeof(gpu));
	start[1] = 1000 * MS;
	for (frame = 1; frame <= FRAMES; frame++)
	{
		uint64_t now, free, gpu_start;
		uint32_t first, pending;

		/* (the frame: written by the game, its tail played by the render
		thread, drawn by the GPU after the frame before, shown no sooner
		than the display's least refresh after the last) */
		submitted[frame] = start[frame] + scene->game + jitter(scene->game_jitter);
		handed[frame] = later(submitted[frame], handed[frame - 1]) + scene->render;
		gpu_start = later(gpu_done[frame - 1], handed[frame]);
		gpu_done[frame] = gpu_start + scene->gpu + jitter(scene->gpu_jitter);
		shown[frame] = later(gpu_done[frame] + scene->show, frame > 1 ? shown[frame - 1] + REFRESH_LEAST : 0);
		taken[frame] = shown[frame] + scene->answer;
		/* (the next frame's start, as display_wait gives it once the game
		has given this one: no more than one frame before on the GPU) */
		now = submitted[frame];
		if (frame > 1)
			now = later(now, gpu_done[frame - 1]);
		/* (no more than FRAME_PACING_VARIABLE_QUEUED waiting for the display,
		FRAME_PACING_WAIT_MAXIMUM_NS at most) */
		if (frame > FRAME_PACING_VARIABLE_QUEUED && taken[frame - FRAME_PACING_VARIABLE_QUEUED] > now)
			now = taken[frame - FRAME_PACING_VARIABLE_QUEUED] - now < WAIT_MOST ?
				taken[frame - FRAME_PACING_VARIABLE_QUEUED] : now + WAIT_MOST;
		/* (what the host knows then: the GPU's word on the frames it has
		finished, in order, each one's time on it; the leads of the frames
		handed to it) */
		while (retired < frame && gpu_done[retired + 1] <= now)
		{
			retired++;
			frame_pacing_gpu_add(&gpu, gpu_done[retired] - later(handed[retired], retired > 1 ?
				gpu_done[retired - 1] : 0));
		}
		while (led < frame && handed[led + 1] <= now)
		{
			led++;
			frame_pacing_cost_add(&leads, start[led], handed[led]);
		}
		first = retired + 1;
		pending = frame - retired;
		free = frame_pacing_gpu_free(retired ? gpu_done[retired] : 0, first <= frame && handed[first] <= now ?
			handed[first] : 0, pending, frame_pacing_gpu_time(&gpu), now);
		{
			uint64_t start_time = frame_pacing_variable_start_time(free, frame_pacing_budget(&leads, 0));

			if (start_time > now && start_time - now < WAIT_MOST)
				now = start_time;
		}
		/* (the frame cap's wait, on its grid) */
		if (!next_frame)
			next_frame = now;
		if (next_frame > now)
			now = next_frame;
		next_frame = now - next_frame > CAP ? now + CAP : next_frame + CAP;
		start[frame + 1] = now;
		if (frame > FIRST)
		{
			total += shown[frame] - start[frame];
			measured++;
		}
	}
	*period = (double)(start[FRAMES] - start[FIRST]) / (double)(FRAMES - FIRST) / 1e6;
	*latency = (double)total / (double)measured / 1e6;
	printf("%s: %.2f ms a frame (%.1f a second), %.2f ms from a frame's start to the screen\n", scene->name, *period,
		1000.0 / *period, *latency);
}

int main(int argc, char **argv)
{
	char const *which = argc > 1 ? argv[1] : "";
	double period, latency;

	if (!strcmp(which, "cap"))
	{
		/* (the beta log's 4K VRR scene: the game 3.5 ms, the GPU 6.1, together more
		than the cap's 7.4; the display's word a refresh after it shows a
		frame. At the cap, not at their sum) */
		struct scene scene = { "the cap, the display answering late", 3 * MS, MS, MS / 2, 6 * MS, MS / 5, MS, 7 * MS };
		struct scene prompt = { "the cap, the display answering as it shows", 3 * MS, MS, MS / 2, 6 * MS, MS / 5,
			MS / 2, 0 };

		play(&scene, &period, &latency);
		CHECK(period <= (double)CAP / 1e6 * 1.02, "%s: %.2f ms a frame, not the cap's %.2f", scene.name, period,
			(double)CAP / 1e6);
		play(&prompt, &period, &latency);
		CHECK(period <= (double)CAP / 1e6 * 1.02, "%s: %.2f ms a frame, not the cap's %.2f", prompt.name, period,
			(double)CAP / 1e6);
	}
	else if (!strcmp(which, "gpu"))
	{
		/* (the GPU over the cap, 11 ms: frames at the GPU's rate, each
		reaching the GPU as it is free, not a whole frame early: from its
		start to the screen its own times, the margin and a little, not the
		GPU's twice) */
		struct scene scene = { "the GPU over the cap", 3 * MS, MS, MS / 2, 11 * MS, MS / 5, MS, 7 * MS };
		double own = (3.0 + 1.0 + 0.5 + 11.2 + 1.0) + (double)FRAME_PACING_MARGIN_NS / 1e6;

		play(&scene, &period, &latency);
		CHECK(period <= 11.1 * 1.02, "%s: %.2f ms a frame, not the GPU's 11.1", scene.name, period);
		CHECK(latency <= own + 1.0, "%s: %.2f ms from a frame's start to the screen, more than its own %.2f and 1 ms",
			scene.name, latency, own);
	}
	else if (!strcmp(which, "game"))
	{
		/* (the game over the cap, 8 to 8.5 ms, the GPU 4: frames at the
		game's rate) */
		struct scene scene = { "the game over the cap", 8 * MS, MS / 2, MS / 2, 4 * MS, MS / 5, MS, 7 * MS };

		play(&scene, &period, &latency);
		CHECK(period <= 8.25 * 1.02, "%s: %.2f ms a frame, not the game's 8.25", scene.name, period);
	}
	else
	{
		fprintf(stderr, "no such case: %s\n", which);
		return 2;
	}
	return 0;
}
