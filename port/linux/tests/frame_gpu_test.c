/* each frame's own GPU time (port/linux/src/frame_gpu.h) on made-up frames,
as tools/test_frame_gpu.py builds it: the host publishes each frame's time
under its number, some a frame late, some two late (two at one frame's end,
as a Steam Deck reads them), one never; the game's rows, held, come out in
order, each with its own frame's time, none 0 and none two frames'; the one
never published unknown. Built with -DFRAME_GPU_HOLD=0 (the negative
control), the rows are not held: a late frame's row is written before its
time comes, and the check fails. */

#include "frame_gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 600

static int failures;

static void check(int ok, const char *what, unsigned long row, unsigned long got, unsigned long wanted)
{
	if (!ok)
	{
		if (failures < 10)
			printf("FAIL: %s: row %lu, GPU %lu, wanted %lu\n", what, row, got, wanted);
		failures++;
	}
}

/* frame's own GPU time (microseconds, never 0) */
static uint32_t gpu_of(uint32_t frame)
{
	return 9000u + (frame * 2654435761u) % 9000u;
}

/* the presents after frame's own its time is published at: most a frame
on, every fifth two on (read with the next one's), every third at once;
frame 97's never (its interval not begun: the query ring full) */
static int delay_of(uint32_t frame)
{
	if (frame == 97)
		return -1;
	if (frame % 5 == 0)
		return 2;
	return frame % 3 == 0 ? 0 : 1;
}

int main(void)
{
	static struct xgpu_stream_gpu_frames table;
	static struct frame_gpu_hold hold;
	unsigned long rows = 0, index;
	uint32_t present, frame, gpu;

	memset(&table, 0, sizeof(table));
	memset(&hold, 0, sizeof(hold));
	for (present = 1; present <= FRAMES; present++)
	{
		/* (the render thread's frame ends since the last present: the times
		read then, oldest first) */
		for (frame = present >= 3 ? present - 2 : 1; frame <= present; frame++)
			if (delay_of(frame) >= 0 && frame + (uint32_t)delay_of(frame) == present)
				frame_gpu_publish(&table, frame, gpu_of(frame));
		/* (the game's present of frame number present, its row index present - 1) */
		frame_gpu_hold_add(&hold, present, 1, present - 1);
		frame_gpu_hold_settle(&hold, &table);
		while (frame_gpu_hold_next(&hold, 0, &index, &gpu))
		{
			uint32_t wanted = index + 1 == 97 ? FRAME_GPU_UNKNOWN : gpu_of((uint32_t)index + 1);

			check(index == rows, "a row out of order", index, gpu, rows);
			check(gpu != 0, "a row of 0", index, gpu, wanted);
			check(gpu == wanted, "a row not its own frame's time", index, gpu, wanted);
			rows++;
		}
	}
	/* (the game's end: those still held) */
	while (frame_gpu_hold_next(&hold, 1, &index, &gpu))
		rows++;
	check(rows == FRAMES, "rows lost", rows, 0, FRAMES);
	/* (a backend that measures nothing: its rows unknown at once, not held) */
	memset(&hold, 0, sizeof(hold));
	frame_gpu_hold_add(&hold, 1, 0, 0);
	frame_gpu_hold_settle(&hold, &table);
	check(frame_gpu_hold_next(&hold, 0, &index, &gpu) && gpu == FRAME_GPU_UNKNOWN, "unmeasured row held", 0, gpu,
		FRAME_GPU_UNKNOWN);
	if (failures)
	{
		printf("%d failures\n", failures);
		return 1;
	}
	printf("ok: %d frames, each row its own frame's GPU time\n", FRAMES);
	return 0;
}
