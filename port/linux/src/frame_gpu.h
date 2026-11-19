/*
FRAME_GPU.H

Each frame's own GPU time, from the backend's timestamps to the game's
frame rows (debug.frame_log, the performance overlay, Save Report's GPU
summary: port/linux/src/report.c).

A frame's GPU timestamps are read on the render thread once the GPU is
done with it, a frame or more after the game presented it, and sometimes
two frames' at one frame's end. A running count read at each frame
presented, its change taken as the frame's, then reads 0 for one frame and
two frames' time for the next (a fifth of the rows on a Steam Deck, its 95th
percentile 20.5 ms for a real 15.4). So the host publishes each frame's own
time under the frame's number (frame_gpu_publish, into the ring's
xgpu_stream_gpu_frames: xgpu_stream.h), and the game holds each frame's row
back until that frame's time has come (frame_gpu_hold_*), at most
FRAME_GPU_HOLD frames, after which the row is written with its GPU time
unknown (never 0).

Header only, C89 but the __atomic builtins, with no other header of the
port's but xgpu_stream.h (port/linux/tests/frame_gpu_test.c).
*/

#ifndef FRAME_GPU_H
#define FRAME_GPU_H

#include <stdint.h>

#include "xgpu_stream.h"

#if defined(__GNUC__) || defined(__clang__)
#define FRAME_GPU_UNUSED __attribute__((unused))
#else
#define FRAME_GPU_UNUSED
#endif

/* a row's GPU time never known (report.c's REPORT_GPU_UNKNOWN) */
#define FRAME_GPU_UNKNOWN 0xFFFFFFFFu
/* the most frames a row waits for its GPU time, after the newest frame's
(fewer than XGPU_STREAM_GPU_FRAMES: its slot is not yet a later frame's) */
#ifndef FRAME_GPU_HOLD
#define FRAME_GPU_HOLD 12
#endif
/* the rows held (a power of two, more than FRAME_GPU_HOLD + 1) */
#define FRAME_GPU_ROWS 16

/* (the host's: one writer) frame's own GPU time, in its slot */
static FRAME_GPU_UNUSED void frame_gpu_publish(volatile struct xgpu_stream_gpu_frames *table, uint32_t frame,
	uint32_t microseconds)
{
	uint32_t slot = frame % XGPU_STREAM_GPU_FRAMES;

	if (!table || !frame)
		return;
	__atomic_store_n(&table->frame[slot], 0u, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&table->microseconds[slot], microseconds, __ATOMIC_RELAXED);
	__atomic_store_n(&table->frame[slot], frame, __ATOMIC_RELEASE);
}

/* (the game's) frame's GPU time, if its slot holds it: 1 */
static FRAME_GPU_UNUSED int frame_gpu_take(const volatile struct xgpu_stream_gpu_frames *table, uint32_t frame,
	uint32_t *microseconds)
{
	uint32_t slot = frame % XGPU_STREAM_GPU_FRAMES, first, value;

	if (!table || !frame)
		return 0;
	first = __atomic_load_n(&table->frame[slot], __ATOMIC_ACQUIRE);
	value = __atomic_load_n(&table->microseconds[slot], __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (first != frame || __atomic_load_n(&table->frame[slot], __ATOMIC_RELAXED) != frame)
		return 0;
	*microseconds = value;
	return 1;
}

/* the game's frames waiting for their GPU times, oldest first: each its
number, its time (once settled) and the caller's index of it */
struct frame_gpu_hold
{
	struct
	{
		uint32_t frame, gpu;
		int settled;
		unsigned long index;
	} rows[FRAME_GPU_ROWS];
	unsigned long added, taken;
};

/* a frame presented: its row, held. frame: its number (0, or the last
row's again: no frame of its own, its GPU time unknown); measured: whether
the backend measures the GPU's times at all (if not, unknown at once) */
static FRAME_GPU_UNUSED void frame_gpu_hold_add(struct frame_gpu_hold *hold, uint32_t frame, int measured,
	unsigned long index)
{
	unsigned long slot = hold->added % FRAME_GPU_ROWS;
	int repeated = hold->added && hold->rows[(hold->added - 1) % FRAME_GPU_ROWS].frame == frame;

	/* (full: the oldest let go, its time unknown; frame_gpu_hold_settle
	gives up on a row long before) */
	if (hold->added - hold->taken == FRAME_GPU_ROWS)
		hold->taken++;
	hold->rows[slot].frame = frame;
	hold->rows[slot].index = index;
	hold->rows[slot].settled = !measured || !frame || repeated;
	hold->rows[slot].gpu = FRAME_GPU_UNKNOWN;
	hold->added++;
}

/* the rows held looked for in the table; those waited FRAME_GPU_HOLD
frames settled as unknown */
static FRAME_GPU_UNUSED void frame_gpu_hold_settle(struct frame_gpu_hold *hold,
	const volatile struct xgpu_stream_gpu_frames *table)
{
	unsigned long row;

	for (row = hold->taken; row != hold->added; row++)
	{
		unsigned long slot = row % FRAME_GPU_ROWS;

		if (hold->rows[slot].settled)
			continue;
		if (frame_gpu_take(table, hold->rows[slot].frame, &hold->rows[slot].gpu))
			hold->rows[slot].settled = 1;
		else if (hold->added - 1 - row >= FRAME_GPU_HOLD)
		{
			hold->rows[slot].gpu = FRAME_GPU_UNKNOWN;
			hold->rows[slot].settled = 1;
		}
	}
}

/* the oldest row, once settled (or every row, flushing: those unsettled
unknown): 1, its index and GPU time (FRAME_GPU_UNKNOWN: unknown), taken */
static FRAME_GPU_UNUSED int frame_gpu_hold_next(struct frame_gpu_hold *hold, int flushing, unsigned long *index,
	uint32_t *gpu)
{
	unsigned long slot = hold->taken % FRAME_GPU_ROWS;

	if (hold->taken == hold->added || (!hold->rows[slot].settled && !flushing))
		return 0;
	*index = hold->rows[slot].index;
	*gpu = hold->rows[slot].settled ? hold->rows[slot].gpu : FRAME_GPU_UNKNOWN;
	hold->taken++;
	return 1;
}

#endif
