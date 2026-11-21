/*
host_stall_test.c: the stall detector (port/android/host/host_stall.c),
driven with a clock of its own: the game's frames at a 135 fps cap's 7.4 ms,
one of them 25 ms long, and the watchdog woken when it asks to be.

- the 25 ms frame is sampled (debug.stall_ms 12: the threshold twice the
  frames' time, 14.8 ms), at least twice, and logged as a stall;
- the ordinary frames cost no wake-ups but their deadlines' (fewer than one
  a frame) and no samples;
- a slow frame the watchdog could not sample (it was not woken in time)
  still has its line, with its length.

tools/test_host_units.py runs it; its negative control builds host_stall.c
with the old 20 ms floor and fixed 10 ms poll, which miss the 25 ms frame.
*/

#include "host.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
	do \
	{ \
		if (!(condition)) \
		{ \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
			failures++; \
		} \
	} while (0)

/* ---------- what host_stall.c calls */

struct host_guest_image host_image;
#ifdef HALO_GUEST_BASE
uintptr_t host_guest_base;
#endif

static char logged[64 * 1024];
static int interrupts;

void host_log(int priority, const char *text)
{
	size_t length = strlen(logged);

	(void)priority;
	snprintf(logged + length, sizeof(logged) - length, "%s\n", text);
}

void host_logf(int priority, const char *format, ...)
{
	char line[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	host_log(priority, line);
}

void host_debug_stall_thread(int thread)
{
	(void)thread;
}

/* (a sample taken as soon as it is asked for, as the signal does) */
void host_debug_stall_interrupt(int thread)
{
	uint64_t returns[2] = { 0x1000, 0x2000 };

	interrupts++;
	host_stall_record(thread, 0x1234, 0, returns, 2);
}

void host_debug_stall_prepare(void)
{
}

void host_debug_describe_address(uint64_t address, char *buffer, size_t size)
{
	snprintf(buffer, size, "%llx", (unsigned long long)address);
}

/* ---------- the clock */

#define MS(value) ((uint64_t)((value) * 1000000.0))

static uint64_t wake_at;
static int wakes;

/* time run on to until: the watchdog woken each time it asked to be,
before the frame at until (wake-ups at the same time as a frame come
after it, as they may) */
static void run_to(uint64_t until, int watchdog_asleep)
{
	while (!watchdog_asleep && wake_at < until)
	{
		uint64_t now = wake_at;

		wakes++;
		wake_at = host_stall_watch(now);
		if (wake_at < now)
			wake_at = now;
	}
}

static void frame(uint64_t at, int watchdog_asleep)
{
	run_to(at, watchdog_asleep);
	host_stall_frame_at(at);
}

int main(void)
{
	uint64_t now = MS(1000), slow_start;
	int index, wakes_before, interrupts_before;

	CHECK(host_stall_configure(12.0));
	wake_at = host_stall_watch(now);
	/* (a second of ordinary frames: the frames' time learned) */
	for (index = 0; index < 135; index++)
	{
		now += MS(7.4);
		frame(now, 0);
	}
	CHECK(interrupts == 0);
	CHECK(strstr(logged, "stall:") == NULL);

	/* the ordinary frames: no wake-up but a deadline's, no samples */
	wakes_before = wakes;
	for (index = 0; index < 270; index++)
	{
		now += MS(7.4);
		frame(now, 0);
	}
	printf("ordinary frames: %d wake-ups for 270 frames\n", wakes - wakes_before);
	CHECK(wakes - wakes_before <= 270 / 2 + 1);
	CHECK(interrupts == 0);
	CHECK(strstr(logged, "stall:") == NULL);

	/* the 25 ms frame, starting where a 10 ms poll from the start would
	look at it 19 ms in and next at 29 ms, after it ended */
	while ((now - MS(1000)) % MS(10) < MS(1) || (now - MS(1000)) % MS(10) > MS(4))
	{
		now += MS(7.4);
		frame(now, 0);
	}
	interrupts_before = interrupts;
	slow_start = now;
	now += MS(25);
	frame(now, 0);
	now += MS(7.4);
	frame(now, 0);
	now += MS(7.4);
	frame(now, 0);
	printf("the 25 ms frame from %.1f ms: %d samples\n%s", (double)(slow_start - MS(1000)) / 1e6,
		interrupts - interrupts_before, logged);
	CHECK(interrupts - interrupts_before >= 2 * 2);
	CHECK(strstr(logged, "stall: the game made no frame for 25 ms") != NULL);
	CHECK(strstr(logged, "ended before it could be sampled") == NULL);

	/* a slow frame the watchdog was not woken for: its line all the same */
	logged[0] = 0;
	for (index = 0; index < 30; index++)
	{
		now += MS(7.4);
		frame(now, 0);
	}
	interrupts_before = interrupts;
	now += MS(30);
	frame(now, 1);
	wake_at = now;
	now += MS(7.4);
	frame(now, 0);
	now += MS(7.4);
	frame(now, 0);
	printf("an unsampled frame:\n%s", logged);
	CHECK(interrupts == interrupts_before);
	/* (the threshold still twice the frames' time, some 15 ms: the 25 ms
	frame's share of it not yet gone) */
	CHECK(strstr(logged, "stall: a frame of 30.0 ms (over 1") != NULL);
	CHECK(strstr(logged, " ms) ended before it could be sampled; doing: not known") != NULL);

	host_stall_summary();
	printf("%s", failures ? "FAILED\n" : "passed\n");
	return failures ? 1 : 0;
}
