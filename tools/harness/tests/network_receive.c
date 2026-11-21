/* A host that draws reads its datagrams on a thread of its own while its
frame waits for the display (network_receive.c): the handoff between the
game's thread and that one. The real thread and lock, a fake socket fed by a
thread of its own as a network would, the game's frames read and work, and
their waits; the game's state is plain variables, so a read of the thread's
outside the frame's wait shows (and, built with ThreadSanitizer, races). */
#include "harness.h"
#include <fenv.h>
#include <pthread.h>
#include <unistd.h>

enum { _game_connection_local = 1, _game_connection_network_client = 2, _game_connection_network_server = 3 };

/* ---------- the socket: datagrams numbered in the order they came */

enum { DATAGRAMS = 6000 };
static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kernel_came = PTHREAD_COND_INITIALIZER;
static int kernel_queue[DATAGRAMS];
static int kernel_first, kernel_count;
static int sent_count;

static void kernel_send(int number)
{
	pthread_mutex_lock(&kernel_lock);
	kernel_queue[kernel_count++] = number;
	pthread_cond_broadcast(&kernel_came);
	pthread_mutex_unlock(&kernel_lock);
}

/* the next datagram, NONE: none */
static int kernel_take(void)
{
	int number = NONE;

	pthread_mutex_lock(&kernel_lock);
	if (kernel_first < kernel_count)
		number = kernel_queue[kernel_first++];
	pthread_mutex_unlock(&kernel_lock);
	return number;
}

static unsigned long long monotonic_microseconds(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000ULL + (unsigned long long)now.tv_nsec / 1000ULL;
}

/* ---------- the game's state (the game's thread's, and the thread's in the waits) */

static long game_socket = 7;
static boolean game_running = TRUE;
static boolean timing_before;
static boolean server_mode;
static boolean frames_unseen;
static short connection = _game_connection_network_server;
static long agreed_rate = 120;
static int handled[DATAGRAMS];
static int handled_count;
static int handled_in_waits;
static int game_rounding = FE_TONEAREST;
/* (atomic) the game's thread at its frame's work, the thread at a read */
static int in_frame_work, in_read;
static int reads_in_work, frames_in_read, rounding_wrong, read_without_game;

static short game_connection(void) { return connection; }
static int halo_server_mode(void) { return server_mode; }
static int halo_frames_unseen(void) { return frames_unseen; }
static boolean game_in_progress(void) { return game_running; }
static long network_standard_agreed_rate(void) { return agreed_rate; }
static int config_boolean(char const *name) { (void)name; return timing_before; }
static long network_game_server_port_datagram_socket(void) { return game_socket; }
static unsigned long long platform_shared_clock_microseconds(void) { return monotonic_microseconds(); }
static void platform_log(const char *format, ...) { (void)format; }

/* what the read hands the game: each datagram that has come, in order */
static void game_handle_all(boolean in_wait)
{
	int number;

	while ((number = kernel_take()) != NONE)
	{
		handled[handled_count++] = number;
		if (in_wait)
			handled_in_waits++;
	}
}

/* the thread's read (network_distributed_server_receive): with the game's
state handed to it, never while the game's thread works, in the game's
floating-point environment */
static int network_receive_read(void)
{
	if (__atomic_load_n(&in_frame_work, __ATOMIC_SEQ_CST))
		__atomic_add_fetch(&reads_in_work, 1, __ATOMIC_SEQ_CST);
	if (fegetround() != game_rounding)
		__atomic_add_fetch(&rounding_wrong, 1, __ATOMIC_SEQ_CST);
	if (network_game_server_port_datagram_socket() == NONE || !game_running)
	{
		__atomic_add_fetch(&read_without_game, 1, __ATOMIC_SEQ_CST);
		return FALSE;
	}
	__atomic_store_n(&in_read, 1, __ATOMIC_SEQ_CST);
	game_handle_all(TRUE);
	/* (a read takes a while: a datagram's handling) */
	usleep(50);
	__atomic_store_n(&in_read, 0, __ATOMIC_SEQ_CST);
	return TRUE;
}

static int network_receive_wait(long socket, long milliseconds)
{
	struct timespec until;
	int ready;

	(void)socket;
	clock_gettime(CLOCK_REALTIME, &until);
	until.tv_nsec += milliseconds * 1000000L;
	if (until.tv_nsec >= 1000000000L)
	{
		until.tv_sec++;
		until.tv_nsec -= 1000000000L;
	}
	pthread_mutex_lock(&kernel_lock);
	while (kernel_first >= kernel_count)
	{
		if (pthread_cond_timedwait(&kernel_came, &kernel_lock, &until))
			break;
	}
	ready = kernel_first < kernel_count;
	pthread_mutex_unlock(&kernel_lock);
	return ready;
}

static void network_receive_rest(void) { usleep(1000); }

#include "under_test.inc"

/* ---------- the network, and the frames */

static unsigned int seed = 12345;
static unsigned int random_below(unsigned int below)
{
	seed = seed * 1103515245u + 12345u;
	return (seed >> 8) % below;
}

static void *network_main(void *unused)
{
	unsigned int local = 777;
	int number;

	(void)unused;
	for (number = 0; number < DATAGRAMS; number++)
	{
		local = local * 1103515245u + 12345u;
		usleep(40 + (local >> 8) % 300);
		kernel_send(number);
		__atomic_store_n(&sent_count, number + 1, __ATOMIC_SEQ_CST);
	}
	return NULL;
}

/* a frame: its read and work (the game's state touched), then its present,
the wait for the display within it */
static void frame(int index)
{
	__atomic_store_n(&in_frame_work, 1, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&in_read, __ATOMIC_SEQ_CST))
		frames_in_read++;
	/* (the map changes: no game for a while, the datagrams queued for its
	next; then a game on another socket) */
	if (index % 150 == 120)
		game_running = FALSE;
	else if (index % 150 == 140)
	{
		game_running = TRUE;
		game_socket++;
	}
	/* (the game changes its floating-point rounding: the thread follows) */
	if (index == 100)
	{
		fesetround(FE_TOWARDZERO);
		game_rounding = FE_TOWARDZERO;
	}
	if (game_running)
		game_handle_all(FALSE);
	usleep(random_below(1500));
	__atomic_store_n(&in_frame_work, 0, __ATOMIC_SEQ_CST);
	network_receive_frame_presenting(TRUE);
	network_receive_frame_wait_begin();
	usleep(500 + random_below(4000));
	network_receive_frame_wait_end();
	network_receive_frame_presenting(FALSE);
}

static void handoff_case(void)
{
	pthread_t network;
	unsigned long waits, reads;
	int index, frames;

	CHECK(!pthread_create(&network, NULL, network_main, NULL), "a network thread");
	for (index = 0; __atomic_load_n(&sent_count, __ATOMIC_SEQ_CST) < DATAGRAMS; index++)
		frame(index);
	pthread_join(network, NULL);
	/* (the last frames: what came last read too) */
	frame(index);
	frame(index + 1);
	frames = index + 2;
	network_receive_stop(&waits, &reads);
	game_running = TRUE;
	game_handle_all(FALSE);
	CHECK(handled_count == DATAGRAMS, "each datagram handled once: %d of %d", handled_count, DATAGRAMS);
	for (index = 0; index < DATAGRAMS; index++)
		CHECK(handled[index] == index, "in the order they came: %d at %d", handled[index], index);
	CHECK(reads_in_work == 0 && frames_in_read == 0, "the thread read while the game's thread worked: %d, %d",
		reads_in_work, frames_in_read);
	CHECK(rounding_wrong == 0, "the thread read in another rounding than the game's: %d", rounding_wrong);
	CHECK(read_without_game == 0, "the thread read with no game in progress: %d", read_without_game);
	CHECK(handled_in_waits > DATAGRAMS / 3, "most came in the waits and were read there: %d of %d", handled_in_waits,
		DATAGRAMS);
	CHECK(waits > 0 && reads > 0, "waits %lu, reads %lu", waits, reads);
	printf("%d frames, %lu waits, %lu reads, %d of %d handled in the waits\n", frames, waits, reads, handled_in_waits,
		DATAGRAMS);
}

/* a present from within a load (its progress screen) lets the thread read
nothing: only main.c's, between frames */
static void load_present_case(void)
{
	real left;

	network_receive_frame_wait_begin();
	CHECK(!network_receive_in_wait(&left), "a wait outside main.c's present opened");
	network_receive_frame_wait_end();
	network_receive_frame_presenting(TRUE);
	network_receive_frame_wait_begin();
	CHECK(network_receive_in_wait(&left), "main.c's present's wait opened");
	CHECK(left < 0.0f, "no wait's length known yet: %f", left);
	usleep(20000);
	network_receive_frame_wait_end();
	network_receive_frame_wait_begin();
	CHECK(network_receive_in_wait(&left) && left > 10.0f && left < 1000.0f, "the last wait's length left: %f", left);
	usleep(400000);
	CHECK(network_receive_in_wait(&left) && left == 0.0f, "past the last wait's length: to end any moment, %f", left);
	network_receive_frame_wait_end();
	network_receive_frame_presenting(FALSE);
	CHECK(!network_receive_in_wait(&left), "the wait ended");
	network_receive_stop(NULL, NULL);
}

/* which machines' waits the thread reads in: an OpenH1 game's host that
draws, in its game, not with debug.network_timing_before */
static void who_case(void)
{
	static const struct
	{
		short connection;
		long rate;
		boolean server, unseen, running, before, wanted;
		char const *what;
	} machines[] =
	{
		{ _game_connection_network_server, 120, FALSE, FALSE, TRUE, FALSE, TRUE, "a drawing host of an OpenH1 game" },
		{ _game_connection_network_server, 30, FALSE, FALSE, TRUE, FALSE, TRUE, "... at 30" },
		{ _game_connection_network_client, 120, FALSE, FALSE, TRUE, FALSE, FALSE, "a client" },
		{ _game_connection_local, 120, FALSE, FALSE, TRUE, FALSE, FALSE, "a game of its own" },
		{ _game_connection_network_server, 0, FALSE, FALSE, TRUE, FALSE, FALSE, "a Legacy game's host" },
		{ _game_connection_network_server, 120, TRUE, FALSE, TRUE, FALSE, FALSE, "a dedicated server" },
		{ _game_connection_network_server, 120, FALSE, TRUE, TRUE, FALSE, FALSE, "a host nobody sees" },
		{ _game_connection_network_server, 120, FALSE, FALSE, FALSE, FALSE, FALSE, "a host between games" },
		{ _game_connection_network_server, 120, FALSE, FALSE, TRUE, TRUE, FALSE, "debug.network_timing_before" },
	};
	short index;

	for (index = 0; index < (short)(sizeof(machines) / sizeof(machines[0])); index++)
	{
		connection = machines[index].connection;
		agreed_rate = machines[index].rate;
		server_mode = machines[index].server;
		frames_unseen = machines[index].unseen;
		game_running = machines[index].running;
		timing_before = machines[index].before;
		CHECK(network_receive_wanted() == machines[index].wanted, "%s", machines[index].what);
	}
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "handoff"))
		handoff_case();
	else if (!strcmp(name, "load-present"))
		load_present_case();
	else if (!strcmp(name, "who"))
		who_case();
	else
	{
		fprintf(stderr, "no case %s\n", name);
		return 2;
	}
	return 0;
}
