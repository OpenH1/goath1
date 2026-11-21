/* The host's object states to its clients (network_objects.c's
distributed_host_send_states): a moving object goes by its distance from a
client's players, every HIDDEN_PLAYER_PERIOD_TICKS where none of them can
see its cluster (the player plan's test), and at once as it comes into
their sight or starts moving; an object at rest goes to every client as it
comes to rest or changes at rest (REST_STATE_REPEATS times), and unchanged
only as its slow refresh (RESTING_REFRESH_TICKS). Machines 0 and 1, one
player each at the origin; objects along x, 10 world units apart. */
#include "harness.h"
#include <math.h>
typedef unsigned short word;
typedef word message_header;
#define csmemset memset
#define csmemcpy memcpy
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define LONG_BITS 32
#define LONG_BITS_BITS 5
#define BIT_VECTOR_SIZE_IN_LONGS(bit_count) (((bit_count) + (LONG_BITS - 1)) >> LONG_BITS_BITS)
#define BIT_VECTOR_TEST_FLAG(v, bit) TEST_FLAG((v)[(bit) >> LONG_BITS_BITS], ((bit) & (LONG_BITS - 1)))
#define BIT_VECTOR_SET_FLAG(v, bit, enable) SET_FLAG((v)[(bit) >> LONG_BITS_BITS], ((bit) & (LONG_BITS - 1)), enable)
enum { MAXIMUM_TRACKED_OBJECTS = 64, MAXIMUM_TRACKED_PLAYERS = 4, HALO_PORT_MAXIMUM_NETWORK_MACHINES = 2,
	MAXIMUM_LOCAL_PLAYERS = 1, MAXIMUM_ENTRIES_PER_MESSAGE = 64, DATAGRAM_MAXIMUM_SIZE = 1200,
	TICKS_PER_SECOND = 30, _object_at_rest_bit = 3, OBJECTS = 8 };
struct distributed_vector { short i; short j; short k; };
struct distributed_message_header { message_header header; byte type; byte count; long game_time; unsigned long epoch; };
#define DATAGRAM_ENTRIES(type) ((short)((DATAGRAM_MAXIMUM_SIZE - 2 * sizeof(struct distributed_message_header) - \
	sizeof(word) + sizeof(message_header)) / sizeof(type)))
enum { _distributed_message_object_states = 6 };
#include "config.inc"
struct distributed_object_state_message
{
	struct distributed_message_header header;
	struct distributed_object_state states[MAXIMUM_ENTRIES_PER_MESSAGE];
};
static struct
{
	long accepted_vehicle_index;
	long accepted_host_time;
	long accepted_time;
} objects_host_vehicle_predictions[MAXIMUM_TRACKED_PLAYERS];
static long objects_host_told_count;
static long objects_host_resting_cursor;
static boolean objects_host_state_moving[MAXIMUM_TRACKED_OBJECTS];
static long objects_host_rest_times[MAXIMUM_TRACKED_OBJECTS];
static unsigned long objects_host_rest_checksums[MAXIMUM_TRACKED_OBJECTS];
static boolean objects_host_rest_known[MAXIMUM_TRACKED_OBJECTS];
static long objects_host_rest_sent_times[MAXIMUM_TRACKED_OBJECTS];
static unsigned long objects_host_seen[HALO_PORT_MAXIMUM_NETWORK_MACHINES][BIT_VECTOR_SIZE_IN_LONGS(MAXIMUM_TRACKED_OBJECTS)];

/* the fake world */
struct object_datum { struct { unsigned long flags; real_point3d position; struct { short cluster_index; } location; } object; };
static struct object_datum objects[OBJECTS];
static long now_time;
/* each machine: whether its players see each cluster (object i is in cluster i) */
static boolean sees[HALO_PORT_MAXIMUM_NETWORK_MACHINES][OBJECTS];
/* what went to each machine this tick, by object */
static short sent[HALO_PORT_MAXIMUM_NETWORK_MACHINES][OBJECTS];

static long game_time_get(void) { return now_time; }
static boolean network_coop_active(void) { return FALSE; }
static boolean cinematic_in_progress(void) { return FALSE; }
struct observer_camera { real_point3d position; };
static struct observer_camera *observer_get_camera(short index) { static struct observer_camera c; (void)index; return &c; }
static boolean vehicle_riding_enabled(void) { return FALSE; }
static struct object_datum *object_get(long object_index) { return &objects[object_index]; }
static long distributed_host_placed_object(long absolute_index) { return absolute_index < OBJECTS ? absolute_index : NONE; }
static void distributed_state_from_object(long object_index, struct distributed_object_state *state)
{
	memset(state, 0, sizeof(*state));
	state->object_index = object_index;
	state->position = objects[object_index].object.position;
	if (TEST_FLAG(objects[object_index].object.flags, _object_at_rest_bit))
		state->flags = 2;
}
static boolean distributed_machine_sees_cluster(long machine_index, short cluster_index)
{
	return cluster_index < 0 || sees[machine_index][cluster_index];
}
/* (every client under its byte budget: test_plan_aim.py's budget case has the stretch) */
static short budget_stretches[4] = { 1, 1, 1, 1 };
static short distributed_budget_stretch(long machine_index) { return budget_stretches[machine_index]; }
static void distributed_send_to_machine(long machine_index, void *message, byte type, short count, word size)
{
	struct distributed_object_state_message *states = message;
	short index;

	(void)type; (void)size;
	for (index = 0; index < count; index++)
		sent[machine_index][states->states[index].object_index]++;
}
/* (a message's encoding, packed or whole, is object_state_packing's) */
static void distributed_host_send_state_message(long machine_index, struct distributed_object_state_message *message,
	short count)
{
	distributed_send_to_machine(machine_index, message, _distributed_message_object_states, count,
		(word)(sizeof(message->header) + count * sizeof(message->states[0])));
}
#include "under_test.inc"

static void tick(void)
{
	memset(sent, 0, sizeof(sent));
	distributed_host_send_states();
	now_time++;
}

static void set_moving(short object, boolean moving)
{
	SET_FLAG(objects[object].object.flags, _object_at_rest_bit, !moving);
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	short object, machine;
	long index;

	objects_host_told_count = OBJECTS;
	objects_host_viewers.count = 2;
	for (machine = 0; machine < 2; machine++)
	{
		objects_host_viewers.indices[machine] = machine;
		objects_host_viewers.machines[machine].unit_count = 1;
		for (object = 0; object < OBJECTS; object++)
			sees[machine][object] = TRUE;
	}
	for (index = 0; index < MAXIMUM_TRACKED_OBJECTS; index++)
	{
		objects_host_rest_times[index] = NONE;
		objects_host_rest_sent_times[index] = NONE;
	}
	for (object = 0; object < OBJECTS; object++)
	{
		objects[object].object.position.x = 10.0f * object;
		objects[object].object.location.cluster_index = object;
		set_moving(object, FALSE);
	}
	now_time = 1000;
	/* (a second for the map's own to be refreshed once, and known) */
	for (index = 0; index < 2 * RESTING_REFRESH_TICKS; index++)
		tick();
	if (!strcmp(name, "resting-unchanged"))
	{
		long sends[OBJECTS] = { 0 };
		long last[OBJECTS];
		long ticks = 4 * RESTING_REFRESH_TICKS;

		for (object = 0; object < OBJECTS; object++)
			last[object] = NONE;
		for (index = 0; index < ticks; index++)
		{
			long time = now_time;

			tick();
			for (object = 0; object < OBJECTS; object++)
			{
				if (!sent[0][object])
					continue;
				CHECK(last[object] == NONE || time - last[object] >= RESTING_REFRESH_TICKS,
					"object %d at rest unchanged sent again after %ld ticks", object, time - last[object]);
				sends[object]++;
				last[object] = time;
			}
		}
		for (object = 0; object < OBJECTS; object++)
		{
			CHECK(sends[object] >= ticks / RESTING_REFRESH_TICKS - 1 && sends[object] <= ticks / RESTING_REFRESH_TICKS + 1,
				"object %d at rest unchanged sent %ld times in %ld ticks (its refresh %d)", object, sends[object], ticks,
				RESTING_REFRESH_TICKS);
		}
		return 0;
	}
	if (!strcmp(name, "resting-changed") || !strcmp(name, "came-to-rest"))
	{
		long changed_at;
		short sends = 0;

		if (!strcmp(name, "came-to-rest"))
		{
			set_moving(3, TRUE);
			tick();
			tick();
			set_moving(3, FALSE);
		}
		else
			objects[3].object.position.z += 0.5f;
		changed_at = now_time;
		for (index = 0; index < 3 * REST_STATE_REPEAT_TICKS; index++)
		{
			long time = now_time;

			tick();
			if (time == changed_at)
				CHECK(sent[0][3] && sent[1][3], "an object %s not sent to every client at once",
					!strcmp(name, "came-to-rest") ? "come to rest" : "changed at rest");
			sends += sent[0][3] ? 1 : 0;
		}
		CHECK(sends >= REST_STATE_REPEATS, "sent %d times, not %d", sends, REST_STATE_REPEATS);
		return 0;
	}
	if (!strcmp(name, "resting-jitter"))
	{
		short sends = 0;

		/* (at rest, but its state changing every tick: no more than once a
		repeat's interval, each with its latest) */
		for (index = 0; index < 4 * REST_STATE_REPEAT_TICKS; index++)
		{
			objects[3].object.position.z += 0.01f;
			tick();
			sends += sent[0][3] ? 1 : 0;
		}
		CHECK(sends >= 3 && sends <= 5, "an object changing at rest every tick sent %d times in %d ticks", sends,
			4 * REST_STATE_REPEAT_TICKS);
		return 0;
	}
	if (!strcmp(name, "unseen") || !strcmp(name, "comes-into-view"))
	{
		short seen_sends = 0, unseen_sends = 0;

		/* object 1 (10 units out, every tick by its distance) moving; machine
		1's players cannot see its cluster */
		sees[1][1] = FALSE;
		set_moving(1, TRUE);
		tick();
		for (index = 0; index < 60; index++)
		{
			tick();
			seen_sends += sent[0][1] ? 1 : 0;
			unseen_sends += sent[1][1] ? 1 : 0;
		}
		CHECK(seen_sends == 60, "a near moving object in sight sent %d ticks of 60", seen_sends);
		if (!strcmp(name, "unseen"))
		{
			CHECK(unseen_sends <= 60 / HIDDEN_PLAYER_PERIOD_TICKS + 1 && unseen_sends >= 60 / HIDDEN_PLAYER_PERIOD_TICKS - 1,
				"a moving object out of sight sent %d ticks of 60", unseen_sends);
			return 0;
		}
		/* (object 7, 70 units out: every third tick by its distance, come
		into sight off both periods' ticks: at once) */
		sees[1][7] = FALSE;
		set_moving(7, TRUE);
		for (index = 0; index < 10; index++)
			tick();
		while ((now_time + 7) % 3 == 0 || (now_time + 7) % HIDDEN_PLAYER_PERIOD_TICKS == 0)
			tick();
		sees[1][7] = TRUE;
		tick();
		CHECK(sent[1][7], "a moving object come into sight not sent at once");
		return 0;
	}
	if (!strcmp(name, "budget"))
	{
		short near_sends = 0, far_sends = 0, seen_sends = 0;

		/* machine 1 over its byte budget (its far and unseen four times as
		seldom): object 1 (10 units out) and 7 (70 out) moving, out of its
		sight; machine 0 sees both */
		budget_stretches[1] = 4;
		sees[1][1] = FALSE;
		sees[1][7] = FALSE;
		set_moving(1, TRUE);
		set_moving(7, TRUE);
		tick();
		for (index = 0; index < 120; index++)
		{
			tick();
			near_sends += sent[1][1] ? 1 : 0;
			far_sends += sent[1][7] ? 1 : 0;
			seen_sends += sent[0][7] ? 1 : 0;
		}
		CHECK(near_sends == 120 / HIDDEN_PLAYER_PERIOD_TICKS, "a near object out of sight sent %d ticks of 120, as before",
			near_sends);
		CHECK(far_sends == 120 / (4 * HIDDEN_PLAYER_PERIOD_TICKS), "a far object out of sight sent %d ticks of 120",
			far_sends);
		CHECK(seen_sends == 120 / 3, "a far object in sight of the other client sent %d ticks of 120", seen_sends);
		return 0;
	}
	if (!strcmp(name, "starts-moving"))
	{
		/* object 7, 70 units out (every third tick by its distance), out of
		machine 1's sight, starts moving off both periods' ticks */
		sees[1][7] = FALSE;
		while ((now_time + 7) % 3 == 0 || (now_time + 7) % HIDDEN_PLAYER_PERIOD_TICKS == 0)
			tick();
		set_moving(7, TRUE);
		tick();
		CHECK(sent[0][7] && sent[1][7], "an object that started moving not sent to every client at once");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
