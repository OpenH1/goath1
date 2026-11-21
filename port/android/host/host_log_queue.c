/*
HOST_LOG_QUEUE.C

The log's queue: the lines host_log is given and the bytes the guest writes
to its standard output and error (platform_log's lines, the debug echo),
copied into a ring at the call and written, in the order they came, by a
thread of its own. A log's write can block (a pipe whose reader is slow:
the game started from Steam or a desktop launcher), and the thread that
logs is then the game's or the render thread, held up for as long; now the
caller copies its line and returns.

The ring is taken with an atomic flag, not a lock, as signal handlers log
too; a caller that cannot take it (a handler on a thread that was adding a
line) writes its line itself. A full ring drops the line and counts it,
and the count is written, at the place of the lines it stands for, when
there is room again ("the log dropped N lines"). Each record lies whole in
the ring (one that would not fit before its end begins again at its start),
and is written from there and only then removed, so that a flush that
takes over (the writer thread blocked or crashed) writes it again rather
than losing it, and needs no buffer of its own (a crash's handler may have
little stack).

Nothing queued is lost at the end: the exit, a fatal error and a crash's
handler write what is queued on their own thread (host_log_queue_direct),
waiting a moment for the writer thread to finish its record, and every
line after it is written directly; Save Report's reading of the log files
flushes first (host_log_recent). The lines' times are their callers' (the
game's own lines are formatted, with their times, before they come here).

With nothing to write it costs the writer thread a wake-up each
QUEUE_IDLE_MS (the limit of a wake-up a caller could not give) and nothing
else.
*/

#include "host_log_queue.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <sys/uio.h>
#include <unistd.h>
#endif

/* the ring's bytes (a power of 2); the spins a caller waits for it */
#define QUEUE_SIZE (256 * 1024)
#define QUEUE_SPINS 100000
/* the writer thread's longest sleep with nothing queued */
#define QUEUE_IDLE_MS 50

/* (a writer of 0: the rest of the ring skipped) */
struct record
{
	host_log_writer writer;
	uint32_t size;
	int32_t tag;
	uint32_t space;
	uint32_t unused;
};

static struct
{
	_Alignas(16) char ring[QUEUE_SIZE];
	/* written and removed, in all (the flag's) */
	uint64_t head, tail;
	unsigned long dropped;
	/* the ring's flag; the writing's (one thread writes records at a time,
	in order) */
	volatile char busy, writing;
	volatile int started, direct;
	host_log_writer note;
	pthread_mutex_t lock;
	pthread_cond_t wake;
} queue = { .lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER };

static int take(void)
{
	int spins;

	for (spins = 0; spins < QUEUE_SPINS; spins++)
		if (!__atomic_test_and_set(&queue.busy, __ATOMIC_ACQUIRE))
			return 1;
	return 0;
}

static void give(void)
{
	__atomic_clear(&queue.busy, __ATOMIC_RELEASE);
}

static void sleep_milliseconds(unsigned milliseconds)
{
	struct timespec time = { (time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L };

	nanosleep(&time, NULL);
}

static size_t record_space(size_t size)
{
	return (sizeof(struct record) + size + 7) & ~(size_t)7;
}

/* (the flag's) a record added (its room known; line: the bytes a line's,
ended by a 0 in the ring) */
static void put(host_log_writer writer, int tag, const char *bytes, size_t size, int line)
{
	size_t space = record_space(size + (line ? 1 : 0)), at = (size_t)(queue.head & (QUEUE_SIZE - 1));
	struct record record;

	if (QUEUE_SIZE - at < space)
	{
		/* (the rest of the ring skipped: a record lies whole) */
		if (QUEUE_SIZE - at >= sizeof(record))
		{
			memset(&record, 0, sizeof(record));
			memcpy(queue.ring + at, &record, sizeof(record));
		}
		queue.head += QUEUE_SIZE - at;
		at = 0;
	}
	memset(&record, 0, sizeof(record));
	record.writer = writer;
	record.size = (uint32_t)size;
	record.tag = tag;
	record.space = (uint32_t)space;
	memcpy(queue.ring + at, &record, sizeof(record));
	memcpy(queue.ring + at + sizeof(record), bytes, size);
	if (line)
		queue.ring[at + sizeof(record) + size] = 0;
	__atomic_store_n(&queue.head, queue.head + space, __ATOMIC_RELEASE);
}

/* (the flag's) the dropped lines' count, as a line of the queue's */
static void put_dropped(void)
{
	char text[96];
	int length = snprintf(text, sizeof(text), "the log dropped %lu lines (its output was blocked)", queue.dropped);

	put(queue.note, 0, text, (size_t)length, 1);
	__atomic_store_n(&queue.dropped, 0, __ATOMIC_RELEASE);
}

static void wake_writer(void)
{
	/* (not waited for: a handler may be on the thread that holds it, and a
	wake-up missed is made up within QUEUE_IDLE_MS) */
	if (pthread_mutex_trylock(&queue.lock) == 0)
	{
		pthread_cond_signal(&queue.wake);
		pthread_mutex_unlock(&queue.lock);
	}
}

static size_t records_of(size_t size)
{
	return size ? (size + HOST_LOG_QUEUE_RECORD - 1) / HOST_LOG_QUEUE_RECORD : 1;
}

/* the parts, in records of up to HOST_LOG_QUEUE_RECORD bytes each, added
together (no other caller's between them), all or none */
static int add(host_log_writer writer, int tag, int count, const char *const *bytes, const size_t *sizes, int line)
{
	size_t records = 0, need, index;
	int part;

	if (!__atomic_load_n(&queue.started, __ATOMIC_ACQUIRE) || __atomic_load_n(&queue.direct, __ATOMIC_ACQUIRE))
		return 0;
	for (part = 0; part < count; part++)
		records += records_of(sizes[part]);
	if (!take())
		return 0;
	if (queue.direct)
	{
		give();
		return 0;
	}
	/* (with room for the ring's end skipped, once a record) */
	need = (records + 1) * record_space(HOST_LOG_QUEUE_RECORD + 1) + (queue.dropped ? 2 * record_space(96) : 0);
	if (QUEUE_SIZE - (queue.head - queue.tail) < need)
	{
		__atomic_store_n(&queue.dropped, queue.dropped + 1, __ATOMIC_RELEASE);
		give();
		return 1;
	}
	if (queue.dropped)
		put_dropped();
	for (part = 0; part < count; part++)
	{
		for (index = 0; index < records_of(sizes[part]); index++)
		{
			size_t size = sizes[part] - index * HOST_LOG_QUEUE_RECORD;

			put(writer, tag, bytes[part] + index * HOST_LOG_QUEUE_RECORD,
				size < HOST_LOG_QUEUE_RECORD ? size : HOST_LOG_QUEUE_RECORD, line);
		}
	}
	give();
	wake_writer();
	return 1;
}

int host_log_queue_line(host_log_writer writer, int tag, const char *line)
{
	size_t size = strlen(line);

	if (size >= HOST_LOG_QUEUE_RECORD)
		size = HOST_LOG_QUEUE_RECORD - 1;
	return add(writer, tag, 1, &line, &size, 1);
}

int host_log_queue_bytes(host_log_writer writer, int tag, int count, const char *const *bytes, const size_t *sizes)
{
	return add(writer, tag, count, bytes, sizes, 0);
}

/* (the writing's) the oldest record written, then removed; 0 if there is
none. The count of lines dropped, with nothing after it, written too. */
static int write_one(void)
{
	struct record record;
	uint64_t at;
	size_t offset;

	if (!take())
		return 0;
	if (queue.tail == queue.head && queue.dropped && queue.note)
		put_dropped();
	for (;;)
	{
		if (queue.tail == queue.head)
		{
			give();
			return 0;
		}
		at = queue.tail;
		offset = (size_t)(at & (QUEUE_SIZE - 1));
		if (QUEUE_SIZE - offset >= sizeof(record))
		{
			memcpy(&record, queue.ring + offset, sizeof(record));
			if (record.writer)
				break;
		}
		/* (the ring's end skipped) */
		queue.tail += QUEUE_SIZE - offset;
	}
	give();
	/* (from the ring: its room is not given back until it is written) */
	record.writer(record.tag, queue.ring + offset + sizeof(record), record.size);
	if (!take())
		return 0;
	/* (unless a flush that took over has removed it) */
	if (queue.tail == at)
		__atomic_store_n(&queue.tail, at + record.space, __ATOMIC_RELEASE);
	give();
	return 1;
}

static int empty(void)
{
	return __atomic_load_n(&queue.tail, __ATOMIC_ACQUIRE) == __atomic_load_n(&queue.head, __ATOMIC_ACQUIRE) &&
		!__atomic_load_n(&queue.dropped, __ATOMIC_ACQUIRE);
}

static void *writer_thread(void *argument)
{
	(void)argument;
	while (!__atomic_load_n(&queue.direct, __ATOMIC_ACQUIRE))
	{
		if (!__atomic_test_and_set(&queue.writing, __ATOMIC_ACQUIRE))
		{
			while (!__atomic_load_n(&queue.direct, __ATOMIC_ACQUIRE) && write_one())
				;
			__atomic_clear(&queue.writing, __ATOMIC_RELEASE);
		}
		pthread_mutex_lock(&queue.lock);
		if (empty() && !__atomic_load_n(&queue.direct, __ATOMIC_ACQUIRE))
		{
			struct timespec until;

			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += QUEUE_IDLE_MS * 1000000L;
			if (until.tv_nsec >= 1000000000L)
			{
				until.tv_sec++;
				until.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&queue.wake, &queue.lock, &until);
		}
		pthread_mutex_unlock(&queue.lock);
	}
	return NULL;
}

int host_log_queue_start(host_log_writer note)
{
	pthread_t thread;
	int error;

	if (queue.started)
		return 0;
	queue.note = note;
	error = pthread_create(&thread, NULL, writer_thread, NULL);
	if (error)
		return error;
	pthread_detach(thread);
	__atomic_store_n(&queue.started, 1, __ATOMIC_RELEASE);
	return 0;
}

void host_log_queue_flush(unsigned timeout_ms)
{
	unsigned waited = 0;
	int taken;

	if (!__atomic_load_n(&queue.started, __ATOMIC_ACQUIRE))
		return;
	/* (the writer thread's record finished first, unless it is blocked or
	this is it, crashed: then taken over, its record written again) */
	while (!(taken = !__atomic_test_and_set(&queue.writing, __ATOMIC_ACQUIRE)) && waited < timeout_ms)
	{
		sleep_milliseconds(1);
		waited++;
	}
	/* (but not while the queue takes lines: the record's room could be
	given to another while the writer thread still writes it) */
	if (!taken && !__atomic_load_n(&queue.direct, __ATOMIC_ACQUIRE))
		return;
	while (write_one())
		;
	if (taken)
		__atomic_clear(&queue.writing, __ATOMIC_RELEASE);
}

void host_log_queue_direct(unsigned timeout_ms)
{
	__atomic_store_n(&queue.direct, 1, __ATOMIC_RELEASE);
	wake_writer();
	host_log_queue_flush(timeout_ms);
}

#ifndef _WIN32
/* (the POSIX hosts') the guest's output written to its descriptor */
static void output_write(int fd, const char *bytes, size_t size)
{
	while (size)
	{
		ssize_t written = write(fd, bytes, size);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return;
		bytes += written;
		size -= (size_t)written;
	}
}

long host_log_queue_output(int fd, const struct iovec *vector, int count)
{
	const char *bytes[64];
	size_t sizes[64];
	long total = 0;
	int index;

	if ((fd != 1 && fd != 2) || count < 0 || count > 64)
		return -1;
	for (index = 0; index < count; index++)
	{
		bytes[index] = vector[index].iov_base;
		sizes[index] = vector[index].iov_len;
		total += (long)vector[index].iov_len;
	}
	return host_log_queue_bytes(output_write, fd, count, bytes, sizes) ? total : -1;
}
#endif
