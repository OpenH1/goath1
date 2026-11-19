/*
NETWORK_LATENCY_TRACE.C

Where a control change's time goes on its way from one machine's player to
the others' copies of them (debug.network_latency_trace: a file to write;
empty, off, which every call below costs a test of a flag). For the network
tests, whose machines share one computer and so one clock: every time is
CLOCK_MONOTONIC's, in microseconds (platform_shared_clock_microseconds),
and nothing goes on the wire; tools/net_latency.py joins the machines'
files by each change's key and prints each hop's median, 95th percentile
and worst.

Each change is keyed by its kind, its player (absolute index) and its key:

    L  a client's player's live control (60 and 120 Hz): the client's tick
       and its sub-step (network_substeps.c)
    H  the host's own player's live control: the host's update and sub-step
    I  a client's player's input of a tick (at 30 Hz the one path), sub-step 0

The lines (times in microseconds; tick the machine's game time, sub-step its
next sub-step's index):

    # network latency trace 1 rate <sim rate> host|client vsync <0|1> max_fps <n>
                                                       (the display's pacing, as set)
    S <kind> <player> <key> <sub> <input> <step>      the owner: the change, with
                                                       the frame's input read and
                                                       the sub-step's start
    T <kind> <player> <key> <sub> <to> <sent>          its datagram handed to the
                                                       socket (to: -1 the host, else
                                                       the host's machine index)
    V <kind> <player> <key> <sub> <kernel> <read> <handled> <tick>
                                                       received: the kernel's
                                                       timestamp of its datagram
                                                       (0: none), the game's read
                                                       of it from the socket, the
                                                       message handled
    A <kind> <player> <key> <sub> <time> <tick> <step> applied to the machine's
                                                       copy of the player, first
    P <time>                                           a frame presented, after
                                                       one applied or sampled since
                                                       the last (the owner's own
                                                       view of its change)
    B <tick> <time>                                    a tick begun
    K <tick> <host tick> <time>                        a client's tick begun, with
                                                       the host's latest it had
    C <datagrams> <close> <stream> <close stream> <batches>
                                                       datagrams sent, those within
                                                       1 ms of the one before to the
                                                       same peer, and the streams'
                                                       writes, those within 1 ms of
                                                       a datagram to the same peer;
                                                       the netcode's batches of them
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *config_string(char const *name);
int config_boolean(char const *name);
long config_integer(char const *name);
void platform_log(char const *format, ...);
unsigned long long platform_shared_clock_microseconds(void);

int network_latency_trace_on;

/* the changes queued for a machine's next datagram (the host: -1) */
#define TRACE_QUEUED 256
/* (queued: to the destination set) */
#define TRACE_DESTINATION -2
#define TRACE_PEERS 16
#define TRACE_DATAGRAMS 64
#define TRACE_QUEUES 4
#define TRACE_APPLIED_PLAYERS 16

struct trace_change
{
	char kind;
	short player;
	long key;
	short substep;
	short to;
};

static struct
{
	int checked;
	FILE *file;
	unsigned long long frame_input;
	unsigned long long step_begun;
	unsigned long long last_flush;
	int applied_since_present;
	short destination;
	struct trace_change queued[TRACE_QUEUED];
	int queued_count;
	/* the datagram being handled: its kernel timestamp and read */
	unsigned long long current_kernel;
	unsigned long long current_read;
	/* the datagrams read into each connection's queue and not yet handled,
	in order (a host has its own client's connection too) */
	struct
	{
		void const *connection;
		struct
		{
			unsigned long long kernel;
			unsigned long long read;
		} datagrams[TRACE_DATAGRAMS];
		int first;
		int count;
	} queues[TRACE_QUEUES];
	unsigned long long last_kernel;
	unsigned long long last_read;
	/* the latest applied of each kind and player, logged once */
	struct
	{
		long key;
		short substep;
		int valid;
	} applied[3][TRACE_APPLIED_PLAYERS];
	/* the sends by peer: its address, the last datagram's and stream write's time */
	struct
	{
		unsigned long address;
		unsigned long long datagram;
		unsigned long long stream;
	} peers[TRACE_PEERS];
	unsigned long datagrams_sent;
	unsigned long datagrams_close;
	unsigned long stream_sent;
	unsigned long stream_close;
	unsigned long batches_sent;
} trace;

static void trace_close(
	void)
{
	if (trace.file)
	{
		fprintf(trace.file, "C %lu %lu %lu %lu %lu\n", trace.datagrams_sent, trace.datagrams_close, trace.stream_sent,
			trace.stream_close, trace.batches_sent);
		fclose(trace.file);
		trace.file = NULL;
	}
	network_latency_trace_on = 0;
}

int network_latency_trace_check(
	void)
{
	if (!trace.checked)
	{
		char const *path = config_string("debug.network_latency_trace");

		trace.checked = 1;
		trace.destination = -1;
		if (path && *path)
		{
			trace.file = fopen(path, "w");
			if (trace.file)
			{
				setvbuf(trace.file, NULL, _IOFBF, 1 << 20);
				atexit(trace_close);
				network_latency_trace_on = 1;
				platform_log("network latency trace: writing %s", path);
			}
			else
				platform_log("network latency trace: cannot write %s", path);
		}
	}
	return network_latency_trace_on;
}

static unsigned long long trace_now(
	void)
{
	return platform_shared_clock_microseconds();
}

void network_latency_trace_header(
	int rate,
	int host)
{
	if (!network_latency_trace_check())
		return;
	fprintf(trace.file, "# network latency trace 1 rate %d %s vsync %d max_fps %ld\n", rate, host ? "host" : "client",
		config_boolean("display.vsync"), config_integer("display.max_fps"));
}

void network_latency_trace_frame_input(
	void)
{
	unsigned long long now;

	if (!network_latency_trace_check())
		return;
	now = trace_now();
	trace.frame_input = now;
	/* (every two seconds, so that a run cut short keeps most of it) */
	if (now - trace.last_flush > 2000000ULL)
	{
		fprintf(trace.file, "C %lu %lu %lu %lu %lu\n", trace.datagrams_sent, trace.datagrams_close, trace.stream_sent,
			trace.stream_close, trace.batches_sent);
		fflush(trace.file);
		trace.last_flush = now;
	}
}

void network_latency_trace_step_begun(
	void)
{
	if (network_latency_trace_on)
		trace.step_begun = trace_now();
}

void network_latency_trace_tick_begun(
	long tick,
	int client,
	long host_tick)
{
	if (!network_latency_trace_on)
		return;
	if (client)
		fprintf(trace.file, "K %ld %ld %llu\n", tick, host_tick, trace_now());
	else
		fprintf(trace.file, "B %ld %llu\n", tick, trace_now());
}

void network_latency_trace_destination(
	short to)
{
	trace.destination = to;
}

/* the owner's change, queued for its next datagram to the host */
void network_latency_trace_sample(
	char kind,
	short player,
	long key,
	short substep)
{
	if (!network_latency_trace_on)
		return;
	fprintf(trace.file, "S %c %d %ld %d %llu %llu\n", kind, player, key, substep, trace.frame_input,
		trace.step_begun);
	/* (its own copy takes it in this step: the next frame presented shows it) */
	trace.applied_since_present = 1;
}

/* a change written into the next datagram to the machine (-1 the host;
-2 the destination set, network_latency_trace_destination) */
void network_latency_trace_queued(
	char kind,
	short player,
	long key,
	short substep,
	short to)
{
	struct trace_change *change;

	if (!network_latency_trace_on || trace.queued_count >= TRACE_QUEUED)
		return;
	change = &trace.queued[trace.queued_count++];
	change->kind = kind;
	change->player = player;
	change->key = key;
	change->substep = substep;
	change->to = to == TRACE_DESTINATION ? trace.destination : to;
}

unsigned long long network_latency_trace_clock(
	void)
{
	return trace_now();
}

/* a datagram to the machine (-1: the host) handed to the socket then */
void network_latency_trace_sent(
	short to,
	unsigned long long now)
{
	int index;
	int kept = 0;

	if (!network_latency_trace_on)
		return;
	trace.batches_sent++;
	if (!trace.queued_count)
		return;
	for (index = 0; index < trace.queued_count; index++)
	{
		struct trace_change const *change = &trace.queued[index];

		if (change->to != to)
		{
			trace.queued[kept++] = *change;
			continue;
		}
		fprintf(trace.file, "T %c %d %ld %d %d %llu\n", change->kind, change->player, change->key, change->substep,
			change->to, now);
	}
	trace.queued_count = kept;
}

/* (the socket layer) a datagram read from the game's socket: the kernel's
timestamp of its arrival, CLOCK_REALTIME's nanoseconds (0: none), made the
monotonic clock's */
void network_latency_trace_datagram_read(
	unsigned long long kernel_realtime_nanoseconds,
	unsigned long long realtime_now_nanoseconds)
{
	unsigned long long now;

	if (!network_latency_trace_on)
		return;
	now = trace_now();
	trace.last_read = now;
	trace.last_kernel = 0;
	if (kernel_realtime_nanoseconds && realtime_now_nanoseconds >= kernel_realtime_nanoseconds)
	{
		unsigned long long ago = (realtime_now_nanoseconds - kernel_realtime_nanoseconds) / 1000ULL;

		trace.last_kernel = now > ago ? now - ago : 0;
	}
}

/* (network_connection.c) the datagram just read queued for the game on
the connection; and one taken from its queue, the one handled next (none:
from a stream) */
static int trace_queue(
	void const *connection)
{
	int index;
	int free = -1;

	for (index = 0; index < TRACE_QUEUES; index++)
	{
		if (trace.queues[index].connection == connection)
			return index;
		if (free < 0 && !trace.queues[index].connection)
			free = index;
	}
	if (free >= 0)
		trace.queues[free].connection = connection;
	return free;
}

void network_latency_trace_datagram_queued(
	void const *connection)
{
	int queue;
	int slot;

	if (!network_latency_trace_on || (queue = trace_queue(connection)) < 0)
		return;
	if (trace.queues[queue].count == TRACE_DATAGRAMS)
	{
		trace.queues[queue].first = (trace.queues[queue].first + 1) % TRACE_DATAGRAMS;
		trace.queues[queue].count--;
	}
	slot = (trace.queues[queue].first + trace.queues[queue].count++) % TRACE_DATAGRAMS;
	trace.queues[queue].datagrams[slot].kernel = trace.last_kernel;
	trace.queues[queue].datagrams[slot].read = trace.last_read;
}

void network_latency_trace_datagram_dequeued(
	void const *connection,
	int datagram)
{
	int queue;

	if (!network_latency_trace_on)
		return;
	trace.current_kernel = 0;
	trace.current_read = 0;
	if (!datagram || (queue = trace_queue(connection)) < 0 || !trace.queues[queue].count)
		return;
	trace.current_kernel = trace.queues[queue].datagrams[trace.queues[queue].first].kernel;
	trace.current_read = trace.queues[queue].datagrams[trace.queues[queue].first].read;
	trace.queues[queue].first = (trace.queues[queue].first + 1) % TRACE_DATAGRAMS;
	trace.queues[queue].count--;
}

/* (a connection's queue emptied: what it had dropped) */
void network_latency_trace_queue_reset(
	void const *connection)
{
	int queue;

	if (network_latency_trace_on && (queue = trace_queue(connection)) >= 0)
		trace.queues[queue].first = trace.queues[queue].count = 0;
}

void network_latency_trace_received(
	char kind,
	short player,
	long key,
	short substep,
	long tick)
{
	if (!network_latency_trace_on || !trace.current_read)
		return;
	fprintf(trace.file, "V %c %d %ld %d %llu %llu %llu %ld\n", kind, player, key, substep, trace.current_kernel,
		trace.current_read, trace_now(), tick);
}

void network_latency_trace_applied(
	char kind,
	short player,
	long key,
	short substep,
	long tick,
	short step)
{
	int which = kind == 'L' ? 0 : kind == 'H' ? 1 : 2;

	if (!network_latency_trace_on || player < 0 || player >= TRACE_APPLIED_PLAYERS)
		return;
	if (trace.applied[which][player].valid && (key < trace.applied[which][player].key ||
		(key == trace.applied[which][player].key && substep <= trace.applied[which][player].substep)))
	{
		return;
	}
	trace.applied[which][player].valid = 1;
	trace.applied[which][player].key = key;
	trace.applied[which][player].substep = substep;
	fprintf(trace.file, "A %c %d %ld %d %llu %ld %d\n", kind, player, key, substep, trace_now(), tick, step);
	trace.applied_since_present = 1;
}

void network_latency_trace_presented(
	void)
{
	if (!network_latency_trace_on || !trace.applied_since_present)
		return;
	trace.applied_since_present = 0;
	fprintf(trace.file, "P %llu\n", trace_now());
}

/* (the socket layer) a datagram, or a stream's write, to the peer's address
(IPv4, the network's order) */
void network_latency_trace_send(
	unsigned long address,
	int datagram)
{
	unsigned long long now;
	int index;
	int slot = -1;

	if (!network_latency_trace_on || !address)
		return;
	now = trace_now();
	for (index = 0; index < TRACE_PEERS; index++)
	{
		if (trace.peers[index].address == address)
		{
			slot = index;
			break;
		}
		if (slot < 0 && !trace.peers[index].address)
			slot = index;
	}
	if (slot < 0)
		return;
	if (trace.peers[slot].address != address)
	{
		memset(&trace.peers[slot], 0, sizeof(trace.peers[slot]));
		trace.peers[slot].address = address;
	}
	if (datagram)
	{
		trace.datagrams_sent++;
		if (trace.peers[slot].datagram && now - trace.peers[slot].datagram < 1000ULL)
			trace.datagrams_close++;
		trace.peers[slot].datagram = now;
	}
	else
	{
		trace.stream_sent++;
		if (trace.peers[slot].datagram && now - trace.peers[slot].datagram < 1000ULL)
			trace.stream_close++;
		trace.peers[slot].stream = now;
	}
}
