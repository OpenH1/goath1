/*
NETWORK_RECEIVE.C

A host of an OpenH1 game that draws its frames reads its datagrams while
its frame waits for the display (NETCODE.md, "Latency by hop").

A machine that draws reads its datagrams once a frame, at its start
(main.c's network_game_client_start_frame): what came while the frame was
drawn and waited for the display (the frame cap's sleep, vsync's, the
frames in flight) waited for the next frame's read, and the host sent on
the input in it to its other clients (forwarded input, live controls:
network_distributed.c) only then, from nothing to a frame later than it
might: a third of a frame on average uncapped, half of one at a cap of 60.

Here, while the game's thread waits for the display at its frame's end
(xgpu_stream_encode.c's xgpu_backend_frame_end, around the frame's commit
and the frame cap's sleep: nothing of the game is touched there), a thread
of its own waits on the host's socket and reads what comes as a dedicated
server's wait does (network_distributed_server_receive: read, handled,
the input in it sent on at once). The game's thread and this one never run
the game's code at once: the game's thread lets this one read only between
network_receive_frame_wait_begin and network_receive_frame_wait_end, and
takes receive_lock to end its wait, which this one holds while it reads;
so what this one reads and writes of the game (the connections, the
messages' handling, the forwarding's state) is handed between the two by
the lock, as from one frame of the game's thread to its next. What it reads
is handled between two frames, as a dedicated server's or a headless host's
reads are: before the next frame's ticks, which apply it at a sub-step's or
a tick's boundary as before, so the ticks and a hosted demo are the same.
It runs in the game's floating-point environment (its rounding and
precision, which the game sets: msvc_crt.c's _controlfp), taken as each
wait begins.

Only an OpenH1 game's host that draws (not a dedicated server, nor a
headless host, which read in their own waits already; not a client, whose
read comes just before the steps that use it: reading sooner would show
nothing sooner), nor with debug.network_timing_before. Where no thread can
be made, the host reads as before.
*/

#include "cseries.h"
#include "game/game.h"
#include "network_standard.h"

#include <pthread.h>

/* network_distributed.c's, network_server_manager.c's, xnet.c's,
port_config.c's, server_mode.c's, unseen_frames.c's */
int network_distributed_server_receive(long milliseconds);
long network_game_server_port_datagram_socket(void);
int xnet_socket_wait_readable(long socket, long milliseconds);
int config_boolean(char const *name);
int halo_server_mode(void);
int halo_frames_unseen(void);
void platform_log(const char *format, ...);
void platform_delay_nanoseconds(unsigned long long nanoseconds);
unsigned long long platform_shared_clock_microseconds(void);

/* ---------- constants */

enum
{
	/* the longest the thread waits on the socket at a time: what
	debug.network_latency holds back comes due by the clock, and the
	forwarding's interval ends, within it (xnet.c's waits are a
	millisecond at a time there too) */
	RECEIVE_WAIT_MILLISECONDS = 1,
	/* the thread's stack: the game's handling of a message keeps buffers
	of its own on it, as on the game's thread */
	RECEIVE_STACK_BYTES = 4 * 1024 * 1024,
};

/* ---------- the floating-point environment

A thread's own (its rounding, precision and masks), as the processor holds
it: the game sets the game's thread's (msvc_crt.c's _controlfp), and the
thread takes it on before it reads. (Not the C library's fegetenv: the
guest's runtime has none.) */
struct network_receive_environment
{
	unsigned int sse;
	unsigned short x87;
	unsigned long long control;
};

static void network_receive_environment_take(
	struct network_receive_environment *environment)
{
	environment->sse = 0;
	environment->x87 = 0;
	environment->control = 0;
#if defined(__i386__) || defined(__x86_64__)
	environment->sse = __builtin_ia32_stmxcsr();
	__asm__ __volatile__ ("fnstcw %0" : "=m" (environment->x87));
#elif defined(__aarch64__)
	environment->control = __builtin_arm_rsr64("fpcr");
#endif
}

static void network_receive_environment_give(
	struct network_receive_environment const *environment)
{
#if defined(__i386__) || defined(__x86_64__)
	unsigned short x87 = environment->x87;

	__builtin_ia32_ldmxcsr(environment->sse);
	__asm__ __volatile__ ("fldcw %0" : : "m" (x87));
#elif defined(__aarch64__)
	__builtin_arm_wsr64("fpcr", environment->control);
#else
	(void)environment;
#endif
}

/* ---------- globals */

static pthread_mutex_t receive_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t receive_waking = PTHREAD_COND_INITIALIZER;
static struct
{
	/* (the game's thread) the thread made, or not to be */
	boolean started;
	boolean failed;
	/* (the game's thread) main.c presents the frame (the thread may read
	in no other wait: a load's progress screen presents from within the
	load); its frame's wait let the thread read */
	boolean presenting;
	boolean waiting;
	/* (receive_lock) the game's thread waits for the display: the thread
	may read; its socket; its floating-point environment; the thread to
	end (a test's) */
	boolean open;
	boolean stopping;
	long socket;
	struct network_receive_environment environment;
	/* (receive_lock) when the wait began, and how long the last lasted
	(microseconds; 0: none yet) */
	unsigned long long began;
	unsigned long long last_length;
	/* (receive_lock) the waits, and the reads the thread made in them */
	unsigned long waits;
	unsigned long reads;
} receive;
static pthread_t receive_thread;

/* ---------- the thread */

/* (receive_lock held) the wait's datagrams read and handled, the input in
them sent on, as a dedicated server's wait does: FALSE, it cannot */
static int network_receive_read(
	void)
{
	return network_distributed_server_receive(0);
}

/* waits up to milliseconds for the socket to be readable: 1 readable, 0
not, -1 it cannot */
static int network_receive_wait(
	long socket,
	long milliseconds)
{
	return xnet_socket_wait_readable(socket, milliseconds);
}

static void network_receive_rest(
	void)
{
	platform_delay_nanoseconds(RECEIVE_WAIT_MILLISECONDS * 1000000ULL);
}

/* the thread: while the game's thread waits for the display, waits on its
socket (the lock not held: the game's thread may end its wait meanwhile)
and reads what comes, in its floating-point environment, with the lock
held and the wait still open; else sleeps until a wait begins */
static void *network_receive_main(
	void *unused)
{
	(void)unused;
	pthread_mutex_lock(&receive_lock);
	for (;;)
	{
		long socket;
		int ready;

		while (!receive.open && !receive.stopping)
			pthread_cond_wait(&receive_waking, &receive_lock);
		if (receive.stopping)
			break;
		socket = receive.socket;
		pthread_mutex_unlock(&receive_lock);
		/* (a socket closed meanwhile is the game's thread's, after its
		wait: the read below looks again, with the lock, and reads none) */
		ready = network_receive_wait(socket, RECEIVE_WAIT_MILLISECONDS);
		pthread_mutex_lock(&receive_lock);
		if (!receive.open || receive.stopping)
			continue;
		/* (not readable: what the forwarding's interval held back goes
		once it is up) */
		network_receive_environment_give(&receive.environment);
		if (ready < 0 || !network_receive_read())
		{
			pthread_mutex_unlock(&receive_lock);
			network_receive_rest();
			pthread_mutex_lock(&receive_lock);
			continue;
		}
		if (ready > 0)
			receive.reads++;
	}
	pthread_mutex_unlock(&receive_lock);
	return NULL;
}

/* (the game's thread) the thread made, once: FALSE where it cannot be */
static boolean network_receive_start(
	void)
{
	pthread_attr_t attributes;

	if (receive.started || receive.failed)
		return receive.started;
	if (pthread_attr_init(&attributes))
	{
		receive.failed = TRUE;
		return FALSE;
	}
	pthread_attr_setstacksize(&attributes, RECEIVE_STACK_BYTES);
	receive.started = !pthread_create(&receive_thread, &attributes, network_receive_main, NULL);
	pthread_attr_destroy(&attributes);
	if (!receive.started)
	{
		receive.failed = TRUE;
		platform_log("network: a host's datagrams are read at its frames only (no thread to read them meanwhile)");
	}
	return receive.started;
}

/* ---------- the game's thread */

/* whether this machine's frame's wait for the display lets the thread
read: an OpenH1 game's host whose frames are seen, in its game, not a
dedicated server, nor with debug.network_timing_before */
boolean network_receive_wanted(
	void)
{
	return game_connection() == _game_connection_network_server && !halo_server_mode() && !halo_frames_unseen() &&
		game_in_progress() && network_standard_agreed_rate() && !config_boolean("debug.network_timing_before");
}

/* the frame's wait for the display begins: from here to its end nothing of
the game's is touched on the game's thread, and the thread may read */
void network_receive_frame_wait_begin(
	void)
{
	long socket;

	if (!receive.presenting || receive.waiting || !network_receive_wanted())
		return;
	socket = network_game_server_port_datagram_socket();
	if (socket == NONE || !network_receive_start())
		return;
	pthread_mutex_lock(&receive_lock);
	network_receive_environment_take(&receive.environment);
	receive.socket = socket;
	receive.began = platform_shared_clock_microseconds();
	receive.open = TRUE;
	receive.waits++;
	pthread_cond_signal(&receive_waking);
	pthread_mutex_unlock(&receive_lock);
	receive.waiting = TRUE;
}

/* ... and ends: once the thread's read (if one) is done; from here the
game's thread has the game again, and all the thread did to it */
void network_receive_frame_wait_end(
	void)
{
	if (!receive.waiting)
		return;
	pthread_mutex_lock(&receive_lock);
	receive.open = FALSE;
	receive.last_length = platform_shared_clock_microseconds() - receive.began;
	pthread_mutex_unlock(&receive_lock);
	receive.waiting = FALSE;
}

/* (main.c) the frame presented, and done */
void network_receive_frame_presenting(
	boolean presenting)
{
	receive.presenting = presenting;
}

/* whether the game's code runs in a frame's wait for the display (on the
thread, then), and if so how long the wait is likely to last yet, by the
last wait's length, in milliseconds: 0 where this one has run past it (it
ends any moment: sent at once, what was read would go just before the
step's datagram, within a millisecond of it); negative, not known (none
before) */
boolean network_receive_in_wait(
	real *left_milliseconds)
{
	unsigned long long now;

	*left_milliseconds = -1.0f;
	if (!receive.open)
		return FALSE;
	now = platform_shared_clock_microseconds();
	if (receive.last_length)
	{
		*left_milliseconds = now < receive.began + receive.last_length ?
			(real)(receive.began + receive.last_length - now) / 1000.0f : 0.0f;
	}
	return TRUE;
}

/* (a test's) the thread ended and its counts: the waits, and the reads it
made in them */
void network_receive_stop(
	unsigned long *waits,
	unsigned long *reads)
{
	network_receive_frame_wait_end();
	pthread_mutex_lock(&receive_lock);
	receive.stopping = TRUE;
	pthread_cond_signal(&receive_waking);
	pthread_mutex_unlock(&receive_lock);
	if (receive.started)
		pthread_join(receive_thread, NULL);
	receive.started = FALSE;
	receive.stopping = FALSE;
	if (waits)
		*waits = receive.waits;
	if (reads)
		*reads = receive.reads;
}
