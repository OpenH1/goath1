/* display.dynamic_resolution's controller (port/linux/src/dynamic_resolution.h)
on made-up GPU times, as tools/test_dynamic_resolution.py builds it: a heavy
scene drops the scale in steps to what fits and never below the floor; a GPU
time that sits in the band, or that a step up would push over it, holds the
scale still (no swinging); a light scene raises it back to the ceiling, a
step at a time; a ceiling at or under the floor is kept. Built with
DYNAMIC_RESOLUTION_TEST_NO_BAND (the negative control), the low mark is the
high one, no band between them, and a scale in the band is not held. */

#ifdef DYNAMIC_RESOLUTION_TEST_NO_BAND
#define DYNAMIC_RESOLUTION_LOW 0.92f
#endif
#include "dynamic_resolution.h"

#include <stdio.h>
#include <stdlib.h>

#define BUDGET 16667.0f /* 60 frames a second */

static int failures;

static void check(int ok, const char *what)
{
	if (!ok)
	{
		printf("FAIL: %s\n", what);
		failures++;
	}
}

/* the GPU's time at a scale for a scene that takes full_us at 1.0 (as the
pixels go, the scale squared) */
static float gpu_at(float full_us, float scale)
{
	return full_us * scale * scale;
}

/* seconds of a scene, each one a window: the changes of scale counted */
static int play(struct dynamic_resolution *state, float full_us, int seconds, float ceiling)
{
	int changes = 0, second;

	for (second = 0; second < seconds; second++)
	{
		float before = state->scale > 0.0f ? state->scale : ceiling;
		float after = dynamic_resolution_window(state, gpu_at(full_us, before), BUDGET, ceiling);

		check(after >= DYNAMIC_RESOLUTION_FLOOR - 0.001f || ceiling < DYNAMIC_RESOLUTION_FLOOR, "under the floor");
		check(after <= ceiling + 0.001f, "over the ceiling");
		check(before - after <= DYNAMIC_RESOLUTION_MOST_STEPS * DYNAMIC_RESOLUTION_STEP + 0.001f, "too far down at once");
		check(after - before <= DYNAMIC_RESOLUTION_STEP + 0.001f, "more than a step up at once");
		changes += after != before;
	}
	return changes;
}

int main(void)
{
	struct dynamic_resolution state;
	uint64_t now;
	uint32_t gpu;
	int frame, changes;

	/* a heavy scene (twice the budget at full scale): down within a few
	seconds to a scale that fits, then held there */
	dynamic_resolution_reset(&state);
	play(&state, 2.0f * BUDGET, 6, 1.0f);
	check(gpu_at(2.0f * BUDGET, state.scale) <= DYNAMIC_RESOLUTION_HIGH * BUDGET, "heavy scene not brought under the high mark");
	check(state.scale >= 0.6f, "heavy scene dropped further than it needed");
	changes = play(&state, 2.0f * BUDGET, 20, 1.0f);
	check(changes == 0, "heavy scene's scale swings once it fits");

	/* a scene far too heavy: the floor, and no lower */
	dynamic_resolution_reset(&state);
	play(&state, 10.0f * BUDGET, 10, 1.0f);
	check(state.scale == DYNAMIC_RESOLUTION_FLOOR, "far too heavy a scene not at the floor");

	/* a scene at 80% of the budget at 0.85: in the band, held (without
	the band, the negative control, it is raised); and one at 88%, near the
	high mark: held too */
	dynamic_resolution_reset(&state);
	state.scale = 0.85f;
	changes = play(&state, 0.80f * BUDGET / (0.85f * 0.85f), 30, 1.0f);
	check(changes == 0 && state.scale == 0.85f, "scale in the band not held");
	dynamic_resolution_reset(&state);
	state.scale = 0.85f;
	changes = play(&state, 0.88f * BUDGET / (0.85f * 0.85f), 30, 1.0f);
	check(changes == 0, "scale at the edge of the band swings");

	/* the heavy scene gone: back up to the ceiling, a step every
	DYNAMIC_RESOLUTION_UP_WINDOWS seconds */
	dynamic_resolution_reset(&state);
	state.scale = 0.6f;
	changes = play(&state, 0.3f * BUDGET, 30, 1.0f);
	check(state.scale == 1.0f, "light scene not back at the ceiling");
	check(changes == 8, "light scene not raised a step at a time");

	/* a ceiling (display.render_scale) of 0.67: never above it; one under
	the floor: kept as it is */
	dynamic_resolution_reset(&state);
	play(&state, 0.2f * BUDGET, 10, 0.67f);
	check(state.scale <= 0.67f + 0.001f, "over a ceiling of 0.67");
	dynamic_resolution_reset(&state);
	play(&state, 10.0f * BUDGET, 10, 0.4f);
	check(state.scale == 0.4f, "a ceiling under the floor not kept");

	/* the frames: a window a second of the running GPU count (wrapping
	past 2^32), the ceiling until the first window is weighed, and an
	unknown count weighing nothing */
	dynamic_resolution_reset(&state);
	now = 1000;
	gpu = 0xffff0000u;
	for (frame = 0; frame < 59; frame++, now += 16667, gpu += 30000)
		check(dynamic_resolution_frame(&state, now, gpu, 1, BUDGET, 1.0f) == 1.0f, "changed before a window");
	for (frame = 0; frame < 5; frame++, now += 16667, gpu += 30000)
		dynamic_resolution_frame(&state, now, gpu, 1, BUDGET, 1.0f);
	check(state.scale < 1.0f, "a second of frames over the budget not weighed");
	dynamic_resolution_reset(&state);
	for (frame = 0; frame < 200; frame++, now += 16667, gpu += 30000)
		check(dynamic_resolution_frame(&state, now, gpu, 0, BUDGET, 1.0f) == 1.0f, "an unknown GPU time weighed");

	if (failures)
		return 1;
	printf("dynamic resolution: ok\n");
	return 0;
}
