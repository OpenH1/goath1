/*
host_log_queue_test.c: the log's queue (port/android/host/host_log_queue.c)
with a writer that blocks, as a full pipe does:

- a caller's line returns at once while the writer is blocked;
- once it is not, the lines come out in their order;
- lines past the queue's room are dropped and counted, the count a line of
  its own where they were;
- a flush returns with every line written;
- a write of parts (a writev's vector) comes out whole.

tools/test_host_units.py runs it, and as its negative control with
-DLOG_DIRECT (no queue: each line written by its caller, as before), where
the caller is held up as long as the writer is.
*/

#include "host_log_queue.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static volatile int blocked;
static char output[4 * 1024 * 1024];
static size_t output_length;

static uint64_t now_ms(void)
{
	struct timespec time;

	clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void pause_ms(unsigned milliseconds)
{
	struct timespec time = { 0, (long)milliseconds * 1000000L };

	nanosleep(&time, NULL);
}

/* (the log's output: blocked while blocked is set, as a pipe nobody reads) */
static void append(const char *bytes, size_t size)
{
	while (__atomic_load_n(&blocked, __ATOMIC_ACQUIRE))
		pause_ms(1);
	if (output_length + size < sizeof(output))
	{
		memcpy(output + output_length, bytes, size);
		output_length += size;
		output[output_length] = 0;
	}
}

static void write_line(int tag, const char *line, size_t size)
{
	(void)tag;
	append(line, size);
	append("\n", 1);
}

static void write_bytes(int tag, const char *bytes, size_t size)
{
	(void)tag;
	append(bytes, size);
}

__attribute__((unused)) static void write_note(int tag, const char *line, size_t size)
{
	(void)tag;
	append("NOTE ", 5);
	append(line, size);
	append("\n", 1);
}

/* as host_log does */
static void log_line(const char *line)
{
	if (!host_log_queue_line(write_line, 0, line))
		write_line(0, line, strlen(line));
}

static void *unblock_later(void *argument)
{
	(void)argument;
	pause_ms(300);
	__atomic_store_n(&blocked, 0, __ATOMIC_RELEASE);
	return NULL;
}

int main(void)
{
	char line[160];
	uint64_t start = now_ms(), longest = 0;
	int index, next, dropped = -1, accepted_after;
	const char *parts[3] = { "part one, ", "part two, ", "part three\n" };
	size_t sizes[3] = { 10, 10, 11 };
	pthread_t thread;
	char *at;

#ifndef LOG_DIRECT
	CHECK(host_log_queue_start(write_note) == 0);
#endif
	/* the writer blocked: each caller's line returns at once */
	__atomic_store_n(&blocked, 1, __ATOMIC_RELEASE);
	pthread_create(&thread, NULL, unblock_later, NULL);
	for (index = 0; index < 4000; index++)
	{
		uint64_t before = now_ms();

		snprintf(line, sizeof(line), "line %05d: the game's thread logs something of a normal length here", index);
		log_line(line);
		if (now_ms() - before > longest)
			longest = now_ms() - before;
	}
	printf("4000 lines logged in %llu ms, the longest %llu ms, the writer blocked\n",
		(unsigned long long)(now_ms() - start), (unsigned long long)longest);
	CHECK(longest < 50);
	pthread_join(thread, NULL);

	/* unblocked: what was queued written, then more lines and a flush */
	host_log_queue_flush(2000);
	for (index = 4000; index < 4010; index++)
	{
		snprintf(line, sizeof(line), "line %05d: after", index);
		log_line(line);
	}
	host_log_queue_bytes(write_bytes, 2, 3, parts, sizes);
	host_log_queue_flush(2000);

	/* the lines in order, the dropped ones counted where they were */
	next = 0;
	accepted_after = 0;
	for (at = output; *at; at = strchr(at, '\n') + 1)
	{
		int number, count;

		if (sscanf(at, "line %d", &number) == 1)
		{
			CHECK(number >= next);
			if (number != next)
				CHECK(dropped == number - next);
			next = number + 1;
			if (number >= 4000)
				accepted_after++;
		}
		else if (sscanf(at, "NOTE the log dropped %d lines", &count) == 1)
		{
			CHECK(dropped < 0);
			dropped = count;
		}
		if (!strchr(at, '\n'))
			break;
	}
	printf("written: up to line %d, %d dropped, %d after\n", next - 1, dropped, accepted_after);
	CHECK(next == 4010);
	CHECK(dropped > 0);
	CHECK(accepted_after == 10);
	CHECK(strstr(output, "part one, part two, part three\n") != NULL);

	/* a crash's handler (host_log_queue_direct) with the writer thread
	blocked in its write: what is queued taken over and written (its record
	maybe twice, none lost), in order, and the lines after it directly */
	output_length = 0;
	output[0] = 0;
	__atomic_store_n(&blocked, 1, __ATOMIC_RELEASE);
	pthread_create(&thread, NULL, unblock_later, NULL);
	for (index = 0; index < 5; index++)
	{
		snprintf(line, sizeof(line), "queued %d", index);
		log_line(line);
	}
	pause_ms(20);
	host_log_queue_direct(50);
	log_line("after the crash");
	pthread_join(thread, NULL);
	printf("at a crash:\n%s", output);
	at = output;
	for (index = 0; index < 5; index++)
	{
		snprintf(line, sizeof(line), "queued %d\n", index);
		at = strstr(at, line);
		CHECK(at != NULL);
		if (!at)
			break;
	}
	CHECK(at && strstr(at, "after the crash\n") != NULL);

	printf("%s", failures ? "FAILED\n" : "passed\n");
	return failures ? 1 : 0;
}
