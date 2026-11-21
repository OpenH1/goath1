/* An OpenH1 client's input held while it is the same as the last it sent
whole (distributed_input_held, distributed_input_checksum), and the host's
take of it (distributed_handle_held, distributed_handle_inputs): an idle
client sends its input whole INPUT_HELD_REPEATS ticks after a change and
every INPUT_HELD_REFRESH_TICKS, held otherwise; the host takes the input of
every tick as before, and its round trip and clock are measured every tick;
a change goes whole at once; a change whose whole sends were all lost is
taken within the refresh, its echo measured all the while.
One client machine (0) with player 0, the host 2 ticks ahead of its word. */
#include "harness.h"
#include <limits.h>
#include <math.h>
#define csmemcpy memcpy
#define csmemset memset
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
typedef unsigned short word;
typedef struct { real yaw, pitch; } real_euler_angles2d;
typedef struct { real i, j; } real_vector2d;
enum { HALO_PORT_MAXIMUM_NETWORK_MACHINES = 2, MAXIMUM_TRACKED_PLAYERS = 4, MAXIMUM_LOCAL_PLAYERS = 4,
	DISTRIBUTED_INPUT_HISTORY = 4, TICKS_PER_SECOND = 30, UNIT_STATES_WHOLE = 255, MAXIMUM_ROUND_TRIP_TICKS = 60,
	_distributed_message_player_inputs_held = 145, _distributed_message_player_prediction_held = 146, TICKS = 300 };
#include "config.inc"

/* the host's world */
struct player_datum { short identifier; long unit_index; };
static struct player_datum players[MAXIMUM_TRACKED_PLAYERS];
static long now;
static long game_time_get(void) { return now; }
static boolean distributed_machine_has_player(long machine_index, short player_index)
{
	return machine_index == 0 && player_index == 0;
}
static struct player_datum *distributed_player(short player_index) { return &players[player_index]; }
static boolean network_standard_hosting(void) { return TRUE; }
static boolean network_coop_active(void) { return FALSE; }
#define distributed_note_input_actions(player_index, action) ((void)0)
#define network_coop_note_player_structure_bsp(player_index, bsp) ((void)0)
#define network_coop_note_player_friendly_fire(player, on) ((void)0)
#define DATUM_INDEX_NEW(index, identifier) (index)
#define distributed_sender_clock(machine_index) (now)
#define distributed_forward_note(player_index, tick, action, flags) ((void)0)
static long distributed_unit_acked_times[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static struct { long input_heard_time[MAXIMUM_TRACKED_PLAYERS]; } distributed_divergence;
static struct
{
	boolean valid;
	real average;
	real deviation;
	boolean milliseconds_valid;
	real milliseconds;
} distributed_round_trips[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
#define distributed_milliseconds_since_tick(time) (NONE)
#define distributed_smooth_ping(valid, value, milliseconds) ((void)0)
/* (what the host took: each tick's input, and its clock's and round trip's words) */
static long taken_tick = NONE;
static real taken_yaw;

static long clock_heard;
static long round_trips;
static boolean update_server_handle_distributed_input(long player, long tick, struct player_action const *action,
	unsigned short const *flags, short history, long clock)
{
	(void)player; (void)flags; (void)history; (void)clock;
	if (taken_tick != NONE && tick <= taken_tick)
		return FALSE;
	taken_tick = tick;
	taken_yaw = action->desired_facing.yaw;

	return TRUE;
}
static void network_clock_input_heard(long machine_index, long tick)
{
	(void)machine_index; (void)tick;
	clock_heard++;
}
/* (each round trip sample past the first: its difference's size taken) */
static double counted_fabs(double x)
{
	round_trips++;
	return x < 0 ? -x : x;
}
#define fabs counted_fabs
/* (debug.network_latency_trace, off) */
#define NETWORK_LATENCY_TRACE(call) ((void)0)
#include "under_test.inc"

static long wholes, helds;

/* one tick of the client: its input of the tick, whole or held, to the host (lost: not had) */
static void client_tick(real yaw, boolean lost)
{
	struct distributed_player_input input;
	unsigned long checksum;
	boolean valid = distributed_round_trips[0].valid;

	memset(&input, 0, sizeof(input));
	input.player_index = 0;
	input.tick = now;
	input.host_time = now - 2;
	input.pad = 1;
	input.action.desired_facing.yaw = yaw;
	input.action.throttle.i = 0.5f;
	checksum = distributed_input_checksum(&input);
	if (distributed_input_held(&distributed_inputs_held_sent[0], checksum, now))
	{
		struct distributed_player_held held;

		helds++;
		memset(&held, 0, sizeof(held));
		held.player_index = 0;
		held.pad = input.pad;
		held.tick = input.tick;
		held.host_time = input.host_time;
		held.checksum = checksum;
		if (!lost)
			distributed_handle_held(0, _distributed_message_player_inputs_held, now, &held, 1);
	}
	else
	{
		wholes++;
		if (!lost)
			distributed_handle_inputs(0, &input, 1, 1);
	}
	/* (the first sample) */
	if (distributed_round_trips[0].valid != valid)
		round_trips++;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	memset(distributed_inputs_held_sent, 0, sizeof(distributed_inputs_held_sent));
	memset(distributed_held_inputs, 0, sizeof(distributed_held_inputs));
	distributed_divergence.input_heard_time[0] = NONE;
	if (!strcmp(name, "idle"))
	{
		for (now = 100; now < 100 + TICKS; now++)
		{
			client_tick(1.0f, FALSE);
			CHECK(taken_tick == now, "the host did not take the idle client's input of tick %ld", now);
		}
		CHECK(wholes <= INPUT_HELD_REPEATS + TICKS / INPUT_HELD_REFRESH_TICKS + 1,
			"an idle client sent its input whole %ld times in %d ticks", wholes, TICKS);
		CHECK(wholes + helds == TICKS, "%ld of %d ticks sent", wholes + helds, TICKS);
		CHECK(clock_heard == TICKS && round_trips == TICKS, "the host's clock heard %ld, round trip measured %ld of %d ticks",
			clock_heard, round_trips, TICKS);
		CHECK(distributed_unit_acked_times[0] == now - 1 - 2 - 1, "the client's word on its states not taken");
		return 0;
	}
	if (!strcmp(name, "change"))
	{
		for (now = 100; now < 120; now++)
			client_tick(1.0f, FALSE);
		wholes = 0;
		client_tick(2.0f, FALSE);
		CHECK(wholes == 1 && taken_yaw == 2.0f, "a change did not go whole at once");
		for (now++; now < 140; now++)
		{
			client_tick(2.0f, FALSE);
			CHECK(taken_tick == now && taken_yaw == 2.0f, "the host took another input at tick %ld", now);
		}
		CHECK(wholes >= INPUT_HELD_REPEATS && wholes <= INPUT_HELD_REPEATS + 20 / INPUT_HELD_REFRESH_TICKS + 1,
			"%ld whole after a change", wholes);
		return 0;
	}
	if (!strcmp(name, "lost"))
	{
		long change;
		long healed = NONE;

		for (now = 100; now < 120; now++)
			client_tick(1.0f, FALSE);
		/* (each whole send of the change lost) */
		change = now;
		for (; now < 160; now++)
		{
			long sent = wholes;

			client_tick(2.0f, now < change + INPUT_HELD_REPEATS);
			if (now >= change + INPUT_HELD_REPEATS && now < 140 && healed == NONE && taken_yaw == 2.0f)
				healed = now;
			if (healed == NONE && now >= change + INPUT_HELD_REPEATS)
				CHECK(taken_yaw == 1.0f && taken_tick < change, "a held input taken onto another (tick %ld)", taken_tick);
			(void)sent;
		}
		CHECK(healed != NONE && healed - (change + INPUT_HELD_REPEATS - 1) <= INPUT_HELD_REFRESH_TICKS,
			"the lost change taken %ld ticks after its last whole send", healed == NONE ? -1 : healed - change);
		/* (its word on the ticks measured while the input was not taken) */
		CHECK(clock_heard == 20 + 40 - INPUT_HELD_REPEATS, "the clock heard %ld", clock_heard);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
