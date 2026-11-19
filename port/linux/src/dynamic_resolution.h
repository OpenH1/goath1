/*
DYNAMIC_RESOLUTION.H

display.dynamic_resolution (#1226, off by default): on a computer, the
render scale lowered while the GPU cannot keep to the frame rate's target,
and raised again as it can, between DYNAMIC_RESOLUTION_FLOOR and
display.render_scale's (the ceiling). The scale is d3d8_device.c's, chosen
between frames (halo_screen_commit), so the screen's targets, display.temporal_aa's
3D view, its jitter's phases and its LOD bias all follow it together; the
game's state never sees it.

The GPU's time is the backend's running count (xgpu_backend_frame_times),
taken at each frame presented (dynamic_resolution_frame). Every
DYNAMIC_RESOLUTION_WINDOW_US its frames' average is weighed against the
target's frame time (the budget), smoothed with the windows before
(dynamic_resolution_window):

- over DYNAMIC_RESOLUTION_HIGH of the budget: down at once, as far as the
  GPU's time (taken as the pixels', the scale squared) says reaches the
  middle of the band, by one step at least and DYNAMIC_RESOLUTION_MOST_STEPS
  at most;
- under DYNAMIC_RESOLUTION_LOW for DYNAMIC_RESOLUTION_UP_WINDOWS windows
  running: up a step, if the time that step would take stays under the high
  mark;
- between the two: held, so that it does not swing back and forth.

The scale is always a whole number of DYNAMIC_RESOLUTION_STEPs (or the
ceiling), so the targets are made again only as it changes. Header only, C89,
with no other header of the port's (port/linux/tests/dynamic_resolution_test.c).
*/

#ifndef DYNAMIC_RESOLUTION_H
#define DYNAMIC_RESOLUTION_H

#include <math.h>
#include <stdint.h>

#define DYNAMIC_RESOLUTION_FLOOR 0.5f
#define DYNAMIC_RESOLUTION_STEP 0.05f
#define DYNAMIC_RESOLUTION_MOST_STEPS 3
#define DYNAMIC_RESOLUTION_WINDOW_US 1000000u
#define DYNAMIC_RESOLUTION_UP_WINDOWS 2
/* (fractions of the budget; a test may set them) */
#ifndef DYNAMIC_RESOLUTION_HIGH
#define DYNAMIC_RESOLUTION_HIGH 0.92f
#endif
#ifndef DYNAMIC_RESOLUTION_LOW
#define DYNAMIC_RESOLUTION_LOW 0.75f
#endif

#if defined(__GNUC__) || defined(__clang__)
#define DYNAMIC_RESOLUTION_UNUSED __attribute__((unused))
#else
#define DYNAMIC_RESOLUTION_UNUSED
#endif

struct dynamic_resolution
{
	/* the scale drawn (0 until the first window: the ceiling) */
	float scale;
	/* the windows' smoothed average GPU time a frame (0: none yet), and
	the windows running under the low mark */
	float smoothed_us;
	int under;
	/* this window's start, its frames and their GPU time; the running
	count as last taken, and whether it was known */
	uint64_t window_start_us;
	uint32_t window_frames, window_gpu_us, gpu_last;
	int gpu_last_known;
};

static DYNAMIC_RESOLUTION_UNUSED void dynamic_resolution_reset(struct dynamic_resolution *state)
{
	state->scale = 0.0f;
	state->smoothed_us = 0.0f;
	state->under = 0;
	state->window_start_us = 0;
	state->window_frames = state->window_gpu_us = state->gpu_last = 0;
	state->gpu_last_known = 0;
}

/* a scale brought to a whole number of steps (down), within the floor and
the ceiling (a ceiling under the floor: the ceiling) */
static DYNAMIC_RESOLUTION_UNUSED float dynamic_resolution_clamp(float scale, float ceiling)
{
	float steps = floorf(scale / DYNAMIC_RESOLUTION_STEP + 0.001f);

	scale = steps * DYNAMIC_RESOLUTION_STEP;
	if (scale < DYNAMIC_RESOLUTION_FLOOR)
		scale = DYNAMIC_RESOLUTION_FLOOR;
	return scale > ceiling ? ceiling : scale;
}

/* one window's average GPU time a frame (average_us) against the budget:
the scale from now on */
static DYNAMIC_RESOLUTION_UNUSED float dynamic_resolution_window(struct dynamic_resolution *state, float average_us,
	float budget_us, float ceiling)
{
	float scale = state->scale > 0.0f ? dynamic_resolution_clamp(state->scale, ceiling) : ceiling;
	float load;

	if (!(budget_us > 0.0f) || !(average_us >= 0.0f) || ceiling <= DYNAMIC_RESOLUTION_FLOOR)
	{
		state->scale = ceiling;
		state->under = 0;
		return state->scale;
	}
	/* (half the window's, half the ones before: a spike of one window
	still moves it, a single frame's does not) */
	state->smoothed_us = state->smoothed_us > 0.0f ? 0.5f * (state->smoothed_us + average_us) : average_us;
	/* (the window's own, where it is above: a scene turned heavy is met at
	once) */
	load = (average_us > state->smoothed_us ? average_us : state->smoothed_us) / budget_us;
	if (load > DYNAMIC_RESOLUTION_HIGH && scale > DYNAMIC_RESOLUTION_FLOOR)
	{
		float middle = 0.5f * (DYNAMIC_RESOLUTION_HIGH + DYNAMIC_RESOLUTION_LOW);
		float wanted = scale * sqrtf(middle / load);
		float least = scale - DYNAMIC_RESOLUTION_MOST_STEPS * DYNAMIC_RESOLUTION_STEP;
		float most = scale - DYNAMIC_RESOLUTION_STEP;

		/* (to the nearest step, one at least, DYNAMIC_RESOLUTION_MOST_STEPS
		at most) */
		wanted = floorf(wanted / DYNAMIC_RESOLUTION_STEP + 0.5f) * DYNAMIC_RESOLUTION_STEP;
		wanted = wanted < least ? least : wanted > most ? most : wanted;
		wanted = dynamic_resolution_clamp(wanted, ceiling);
		/* (the time now the smaller scale's, as the pixels go) */
		state->smoothed_us *= (wanted * wanted) / (scale * scale);
		scale = wanted;
		state->under = 0;
	}
	else if (load < DYNAMIC_RESOLUTION_LOW && scale < ceiling)
	{
		float up = dynamic_resolution_clamp(scale + DYNAMIC_RESOLUTION_STEP, ceiling);

		if (++state->under >= DYNAMIC_RESOLUTION_UP_WINDOWS &&
			load * (up * up) / (scale * scale) < DYNAMIC_RESOLUTION_HIGH)
		{
			state->smoothed_us *= (up * up) / (scale * scale);
			scale = up;
			state->under = 0;
		}
	}
	else
		state->under = 0;
	state->scale = scale;
	return scale;
}

/* a frame presented at now_us, the GPU's running count of microseconds
(modulo 2^32) and whether it is known: the scale from now on (the ceiling
until a window has been weighed) */
static DYNAMIC_RESOLUTION_UNUSED float dynamic_resolution_frame(struct dynamic_resolution *state, uint64_t now_us,
	uint32_t gpu_us, int gpu_known, float budget_us, float ceiling)
{
	if (gpu_known && state->gpu_last_known)
	{
		state->window_gpu_us += gpu_us - state->gpu_last;
		state->window_frames++;
	}
	state->gpu_last = gpu_us;
	state->gpu_last_known = gpu_known;
	if (!state->window_start_us)
		state->window_start_us = now_us;
	if (now_us - state->window_start_us >= DYNAMIC_RESOLUTION_WINDOW_US)
	{
		if (state->window_frames)
			dynamic_resolution_window(state, (float)state->window_gpu_us / (float)state->window_frames, budget_us,
				ceiling);
		state->window_start_us = now_us;
		state->window_frames = state->window_gpu_us = 0;
	}
	return state->scale > 0.0f ? dynamic_resolution_clamp(state->scale, ceiling) : ceiling;
}

#endif
