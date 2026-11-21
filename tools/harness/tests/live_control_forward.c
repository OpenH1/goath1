/* The host's forwarded live controls on the wire (live_control_forward_pack,
live_control_forward_unpack): a client reads every control exactly as the
whole 19-byte entry gave it (its held controls, sub-step, count, kind of
tick, tick, sticks, trigger and aim, the entry the host sent before, below),
in 12 bytes when its tick is within a byte of the message's, 15 further. */
#include "harness.h"
#include <math.h>
typedef unsigned short word;
#define csmemcpy memcpy
#define csmemset memset
#define NUMBEROF(a) ((short)(sizeof(a) / sizeof((a)[0])))
#define CEILING(n, ceiling) ((n) > (ceiling) ? (ceiling) : (n))
#define PIN(n, floor, ceiling) ((n) < (floor) ? (floor) : CEILING((n), (ceiling)))
#define DISTRIBUTED_UNIT_SCALE 32767.0f
typedef struct { real i, j; } real_vector2d;
struct distributed_vector { short i; short j; short k; };
enum { MAXIMUM_TRACKED_PLAYERS = 128, COUNT = 4, OLD_SIZE = 19 };
#include "config.inc"
#define SUBSTEP_HELD_FLAGS (FLAG(_unit_control_crouch_modifier_bit) | FLAG(_unit_control_jump_bit) | \
	FLAG(_unit_control_weapon_primary_trigger_bit) | FLAG(_unit_control_weapon_secondary_trigger_bit) | \
	FLAG(_unit_control_throw_grenade_bit))
#include "under_test.inc"

/* the entry the host sent before (live_control_forward_write and
network_substeps_handle_forwarded_live as they were) */
static word old_write(short player_index, struct live_control const *live, byte *buffer)
{
	struct distributed_vector aim;
	word held = (word)live->held_flags;

	distributed_vector_pack(&live->aim, DISTRIBUTED_UNIT_SCALE, &aim);
	buffer[0] = (byte)player_index;
	buffer[1] = (byte)live->substep;
	buffer[2] = (byte)live->count;
	buffer[3] = (byte)(live->host_update ? 1 : 0);
	{
		int key = (int)live->key; /* (the game's long: 4 bytes) */
		memcpy(buffer + 4, &key, sizeof(key));
	}
	memcpy(buffer + 8, &held, sizeof(held));
	buffer[10] = (byte)(signed char)(long)floor(PIN(live->throttle.i, -1.0f, 1.0f) * 127.0f + 0.5f);
	buffer[11] = (byte)(signed char)(long)floor(PIN(live->throttle.j, -1.0f, 1.0f) * 127.0f + 0.5f);
	buffer[12] = (byte)(long)floor(PIN(live->primary_trigger, 0.0f, 1.0f) * 255.0f + 0.5f);
	memcpy(buffer + 13, &aim, sizeof(aim));
	return OLD_SIZE;
}

static void old_read(byte const *data, short *player_index, struct live_control *live)
{
	struct distributed_vector aim;
	word held;

	memset(live, 0, sizeof(*live));
	*player_index = data[0];
	live->substep = data[1];
	live->count = data[2];
	live->host_update = (data[3] & 1) != 0;
	{
		int key;
		memcpy(&key, data + 4, sizeof(key));
		live->key = key;
	}
	memcpy(&held, data + 8, sizeof(held));
	live->held_flags = held & SUBSTEP_HELD_FLAGS;
	live->throttle.i = PIN((real)(signed char)data[10] / 127.0f, -1.0f, 1.0f);
	live->throttle.j = PIN((real)(signed char)data[11] / 127.0f, -1.0f, 1.0f);
	live->primary_trigger = (real)data[12] / 255.0f;
	memcpy(&aim, data + 13, sizeof(aim));
	distributed_unit_vector_unpack(&aim, &live->aim);
}

static unsigned long seed = 12345;
static unsigned long next_random(void) { seed = seed * 1103515245ul + 12345ul; return (seed >> 8) & 0xFFFFFF; }
static real random_real(real low, real high) { return low + (high - low) * (real)next_random() / (real)0xFFFFFF; }

static void random_live(struct live_control *live, long time, long spread)
{
	unsigned long held = next_random() & (FLAG(_unit_control_crouch_modifier_bit) |
		FLAG(_unit_control_jump_bit) | FLAG(_unit_control_weapon_primary_trigger_bit) |
		FLAG(_unit_control_weapon_secondary_trigger_bit) | FLAG(_unit_control_throw_grenade_bit) |
		FLAG(_unit_control_action_bit));
	real length;

	memset(live, 0, sizeof(*live));
	live->valid = TRUE;
	live->pending = TRUE;
	live->host_update = (next_random() & 1) != 0;
	live->key = time + (long)(next_random() % (2 * spread + 1)) - spread;
	live->substep = (short)(next_random() % COUNT);
	live->count = COUNT;
	/* (another held bit than the sub-step's: the old entry dropped it too) */
	live->held_flags = held & SUBSTEP_HELD_FLAGS;
	live->throttle.i = random_real(-1.2f, 1.2f);
	live->throttle.j = random_real(-1.2f, 1.2f);
	live->primary_trigger = random_real(-0.1f, 1.1f);
	live->aim.i = random_real(-1.0f, 1.0f);
	live->aim.j = random_real(-1.0f, 1.0f);
	live->aim.k = random_real(-1.0f, 1.0f);
	length = sqrtf(live->aim.i * live->aim.i + live->aim.j * live->aim.j + live->aim.k * live->aim.k);
	if (length > 0.0f)
	{
		live->aim.i /= length;
		live->aim.j /= length;
		live->aim.k /= length;
	}
}

/* every control of a random set read as the old entry gave it */
static void round_trips(long spread, short *largest, short *smallest)
{
	long time = 100000;
	long index;

	*largest = 0;
	*smallest = 1000;
	for (index = 0; index < 20000; index++)
	{
		struct live_control live, before, after;
		short player_index = (short)(next_random() % MAXIMUM_TRACKED_PLAYERS);
		short before_player, after_player;
		byte old[OLD_SIZE], packed[LIVE_CONTROL_FORWARD_MAXIMUM_SIZE + 8];
		word size;

		random_live(&live, time, spread);
		old_write(player_index, &live, old);
		old_read(old, &before_player, &before);
		size = live_control_forward_pack(player_index, &live, time, packed);
		CHECK(size <= LIVE_CONTROL_FORWARD_MAXIMUM_SIZE, "%u bytes written", size);
		CHECK(live_control_forward_unpack(packed, packed + size, time, COUNT, &after_player, &after) == size,
			"the entry not read whole (%u bytes)", size);
		CHECK(after_player == before_player, "player %d read as %d", before_player, after_player);
		CHECK(after.host_update == before.host_update, "the kind of tick not as before");
		CHECK(after.key == before.key, "tick %ld read as %ld (the message's %ld)", before.key, after.key, time);
		CHECK(after.substep == before.substep && after.count == before.count, "sub-step %d of %d read as %d of %d",
			before.substep, before.count, after.substep, after.count);
		CHECK(after.held_flags == before.held_flags, "held %lx read as %lx", before.held_flags, after.held_flags);
		CHECK(after.throttle.i == before.throttle.i && after.throttle.j == before.throttle.j &&
			after.primary_trigger == before.primary_trigger, "the sticks or trigger not as before");
		CHECK(after.aim.i == before.aim.i && after.aim.j == before.aim.j && after.aim.k == before.aim.k,
			"the aim not as before");
		/* (one byte short: none) */
		CHECK(!live_control_forward_unpack(packed, packed + size - 1, time, COUNT, &after_player, &after),
			"an entry read past its message's end");
		if (size > *largest)
			*largest = size;
		if (size < *smallest)
			*smallest = size;
	}
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	short largest, smallest;

	if (!strcmp(name, "near"))
	{
		/* (ticks within a byte of the message's: the short entry) */
		round_trips(100, &largest, &smallest);
		CHECK(largest == LIVE_CONTROL_FORWARD_SHORT_SIZE, "%d bytes an entry of a near tick", largest);
		return 0;
	}
	if (!strcmp(name, "far"))
	{
		/* (a host's update numbered apart from its game time, a client's tick far behind) */
		round_trips(100000, &largest, &smallest);
		CHECK(largest == LIVE_CONTROL_FORWARD_MAXIMUM_SIZE, "%d bytes an entry of a far tick", largest);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
