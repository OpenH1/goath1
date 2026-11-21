/* A client's controls packed for the host (network_substeps.c): its live
controls (live_control_up_pack, live_control_up_unpack) and its sub-steps
(substep_input_pack, substep_input_unpack) read exactly as the whole
entries were, every bit of every number: +0, 1 and -1 in their codes, -0,
NaN, infinity and the reals between whole; every held control; the sub-step,
the count and the tick near and far; a sub-step's sticks and trigger as the
one before's only where every bit is the same; none read past its end. */
#include "harness.h"
#include <math.h>
typedef unsigned short word;
#define csmemcpy memcpy
#define csmemset memset
#define csmemcmp memcmp
#define NUMBEROF(a) ((short)(sizeof(a) / sizeof((a)[0])))
#define CEILING(n, ceiling) ((n) > (ceiling) ? (ceiling) : (n))
#define PIN(n, floor, ceiling) ((n) < (floor) ? (floor) : CEILING((n), (ceiling)))
typedef struct { real i, j; } real_vector2d;
enum
{
	MAXIMUM_TRACKED_PLAYERS = 128,
	MAXIMUM_SIM_SUBSTEPS = 4,
};
#include "config.inc"
#include "under_test.inc"

static long test_time = 1000;

static unsigned long seed = 4242;
static unsigned long next_random(void) { seed = seed * 1103515245ul + 12345ul; return (seed >> 8) & 0xFFFFFF; }
static real random_real(real low, real high) { return low + (high - low) * (real)next_random() / (real)0xFFFFFF; }

/* ---------- to the host */

static real special_real(void)
{
	static real const specials[] = { 0.0f, 1.0f, -1.0f, -0.0f, 0.5f, 1.0000001f, -0.9999999f, 1e-30f };
	unsigned long pick = next_random() % 12;
	real value;

	if (pick < NUMBEROF(specials))
		return specials[pick];
	if (pick == 8)
	{
		value = NAN;
		return value;
	}
	if (pick == 9)
		return INFINITY;
	return random_real(-1.5f, 1.5f);
}

static void up_round_trips(void)
{
	long index;
	short smallest = 100;

	for (index = 0; index < 50000; index++)
	{
		struct distributed_live_control entry, after;
		byte packed[LIVE_CONTROL_PACKED_MAXIMUM_SIZE + 4];
		word size;
		unsigned long held = next_random();
		short bit;

		memset(&entry, 0, sizeof(entry));
		entry.player_index = (byte)(next_random() % MAXIMUM_TRACKED_PLAYERS);
		entry.substep_count = (byte)(1 + next_random() % MAXIMUM_SIM_SUBSTEPS);
		entry.substep_index = (byte)(next_random() % entry.substep_count);
		entry.tick = next_random() % 3 ? test_time : test_time + (long)(next_random() % 400000) - 200000;
		/* (the five a sub-step changes: the host reads no others) */
		for (bit = 0; bit < NUMBEROF(live_control_held_bits); bit++)
		{
			if (held & (1ul << bit))
				entry.held_flags |= (word)FLAG(live_control_held_bits[bit]);
		}
		entry.throttle_i = special_real();
		entry.throttle_j = special_real();
		entry.primary_trigger = special_real();
		entry.aim_i = special_real();
		entry.aim_j = random_real(-1.0f, 1.0f);
		entry.aim_k = special_real();
		size = live_control_up_pack(&entry, test_time, packed);
		CHECK(size <= LIVE_CONTROL_PACKED_MAXIMUM_SIZE, "%u bytes", size);
		CHECK(live_control_up_unpack(packed, packed + size, test_time, &after) == size, "not read whole");
		CHECK(!memcmp(&entry, &after, sizeof(entry)),
			"read otherwise: player %u/%u sub-step %u/%u of %u/%u tick %ld/%ld held %x/%x", entry.player_index,
			after.player_index, entry.substep_index, after.substep_index, entry.substep_count, after.substep_count,
			(long)entry.tick, (long)after.tick, entry.held_flags, after.held_flags);
		CHECK(!live_control_up_unpack(packed, packed + size - 1, test_time, &after), "read past its end");
		if (size < smallest)
			smallest = size;
	}
	/* (a keyboard's: sticks and trigger in their codes, the tick the message's) */
	CHECK(smallest == 15, "the least %d bytes", smallest);
}

static void substep_round_trips(void)
{
	long index;
	long whole = 0, packed_total = 0;

	for (index = 0; index < 50000; index++)
	{
		struct distributed_substep_input input, after;
		byte packed[SUBSTEP_INPUT_PACKED_MAXIMUM_SIZE + 4];
		word size;
		short substep;

		memset(&input, 0, sizeof(input));
		input.player_index = (byte)(next_random() % MAXIMUM_TRACKED_PLAYERS);
		input.substep_count = (byte)(2 + next_random() % (MAXIMUM_SIM_SUBSTEPS - 1));
		input.tick = next_random() % 3 ? test_time + (long)(next_random() % 256) - 128 :
			test_time + (long)(next_random() % 400000) - 200000;
		for (substep = 0; substep < input.substep_count - 1; substep++)
		{
			struct distributed_substep_input_control *control = &input.controls[substep];
			short bit;

			for (bit = 0; bit < NUMBEROF(live_control_held_bits); bit++)
			{
				if (next_random() & 1)
					control->held_flags |= (word)FLAG(live_control_held_bits[bit]);
			}
			control->turned = (byte)(next_random() & 1);
			if (substep && next_random() % 2)
			{
				control->throttle_i = control[-1].throttle_i;
				control->throttle_j = control[-1].throttle_j;
				control->primary_trigger = control[-1].primary_trigger;
				/* (one bit apart: not the same) */
				if (next_random() % 4 == 0)
					control->primary_trigger = next_random() & 1 ? -control->primary_trigger :
						nextafterf(control->primary_trigger, 2.0f);
			}
			else
			{
				control->throttle_i = special_real();
				control->throttle_j = special_real();
				control->primary_trigger = special_real();
			}
			/* (the host reads the facing of a sub-step that turned alone) */
			if (control->turned)
			{
				control->yaw = special_real() * 12.0f;
				control->pitch = special_real();
			}
		}
		size = substep_input_pack(&input, test_time, packed);
		CHECK(size <= SUBSTEP_INPUT_PACKED_MAXIMUM_SIZE, "%u bytes", size);
		CHECK(substep_input_unpack(packed, packed + size, test_time, &after) == size, "not read whole");
		CHECK(!memcmp(&input, &after, sizeof(input)), "read otherwise: %u sub-steps, tick %ld/%ld",
			input.substep_count, (long)input.tick, (long)after.tick);
		CHECK(!substep_input_unpack(packed, packed + size - 1, test_time, &after), "read past its end");
		whole += 8 + 24 * (input.substep_count - 1);
		packed_total += size;
	}
	CHECK(packed_total < whole, "%ld bytes packed of %ld", packed_total, whole);
	{
		/* (a keyboard's at 120, turning each sub-step: 31 bytes of 80) */
		struct distributed_substep_input input, after;
		byte packed[SUBSTEP_INPUT_PACKED_MAXIMUM_SIZE];
		short substep;
		word size;

		memset(&input, 0, sizeof(input));
		input.player_index = 3;
		input.substep_count = 4;
		input.tick = test_time - 1;
		for (substep = 0; substep < 3; substep++)
		{
			input.controls[substep].throttle_i = 1.0f;
			input.controls[substep].throttle_j = -1.0f;
			input.controls[substep].turned = 1;
			input.controls[substep].yaw = 0.25f * substep;
			input.controls[substep].pitch = -0.125f;
		}
		size = substep_input_pack(&input, test_time, packed);
		CHECK(size == 31, "%u bytes", size);
		CHECK(substep_input_unpack(packed, packed + size, test_time, &after) == size &&
			!memcmp(&input, &after, sizeof(input)), "read otherwise");
	}
	{
		/* (no game's count, and the first sub-step as one before it) */
		byte bad[] = { 1, 5, 0, 0, 0, 0 };
		struct distributed_substep_input after;

		CHECK(!substep_input_unpack(bad, bad + sizeof(bad), test_time, &after), "a count of 5 read");
		bad[1] = 2;
		bad[3] = (byte)FLAG(PACKED_AS_BEFORE_BIT);
		CHECK(!substep_input_unpack(bad, bad + sizeof(bad), test_time, &after), "the first as the one before");
	}
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "up"))
	{
		up_round_trips();
		return 0;
	}
	if (!strcmp(name, "substeps"))
	{
		substep_round_trips();
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
