/* The host's objects' states in bits to an OpenH1 client
(network_objects.c's network_objects_pack_state, network_objects_unpack_state,
network_objects_unpack_states, distributed_host_send_state_message): each
state read back is the whole entry's exactly, but its position, which is on
a grid of 1/4096 of a world unit (an 8192th off at most); at the map's
bounds, at the fastest speeds, with every part set and with none; a
position outside the world or not a number goes whole, read back exactly;
an OpenCE machine, and a host with debug.network_forms_before, sends the
whole message as before; a message cut short is none. */
#include "harness.h"
#include <math.h>
typedef unsigned short word;
typedef word message_header;
#define csmemset memset
#define csmemcpy memcpy
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
struct distributed_vector { short i; short j; short k; };
struct distributed_message_header { message_header header; byte type; byte count; long game_time; unsigned long epoch; };
enum { _distributed_message_object_states = 6, _distributed_message_object_states_packed = 149 };
#include "config.inc"
struct distributed_object_state_message
{
	struct distributed_message_header header;
	struct distributed_object_state states[MAXIMUM_ENTRIES_PER_MESSAGE];
};

static boolean distributed_point_valid(real_point3d const *point, real bound)
{
	return fabsf(point->x) <= bound && fabsf(point->y) <= bound && fabsf(point->z) <= bound;
}
static boolean legacy_machine;
static boolean unpacked_setting;
static boolean network_legacy_machine(long machine_index) { (void)machine_index; return legacy_machine; }
static int config_boolean(char const *name)
{
	CHECK(!strcmp(name, "debug.network_forms_before"), "the setting read: %s", name);
	return unpacked_setting;
}
/* what went: its type, count and bytes */
static byte sent_type;
static short sent_count;
static word sent_size;
static byte sent_bytes[4096];
static void distributed_send_to_machine(long machine_index, void *message, byte type, short count, word size)
{
	(void)machine_index;
	sent_type = type;
	sent_count = count;
	sent_size = size;
	CHECK(size <= sizeof(sent_bytes), "a message of %u bytes", size);
	memcpy(sent_bytes, message, size);
	/* (its header as distributed_send fills it in) */
	((struct distributed_message_header *)sent_bytes)->game_time = 4321;
	((struct distributed_message_header *)sent_bytes)->type = type;
	((struct distributed_message_header *)sent_bytes)->count = (byte)count;
}
#include "under_test.inc"

/* a unit vector of 16-bit parts, as distributed_vector_pack makes them */
static struct distributed_vector unit(real i, real j, real k)
{
	struct distributed_vector vector;
	real length = sqrtf(i * i + j * j + k * k);

	vector.i = (short)lroundf(i / length * 32767.0f);
	vector.j = (short)lroundf(j / length * 32767.0f);
	vector.k = (short)lroundf(k / length * 32767.0f);
	return vector;
}

static struct distributed_object_state object(long index, real x, real y, real z)
{
	struct distributed_object_state state;

	memset(&state, 0, sizeof(state));
	state.object_index = index;
	state.position.x = x;
	state.position.y = y;
	state.position.z = z;
	state.forward = unit(1.0f, 0.0f, 0.0f);
	state.up = unit(0.0f, 0.0f, 1.0f);
	return state;
}

/* one state through the bits: its bytes; read back as the whole entry is,
the position within an 8192th (exactly, whole) */
static long round_trip(struct distributed_object_state const *state, boolean exact)
{
	byte buffer[OBJECT_PACKED_MAXIMUM_SIZE];
	struct network_bits bits;
	struct distributed_object_state read;
	struct distributed_object_state expected = *state;
	short axis;

	network_bits_begin_write(&bits, buffer, sizeof(buffer));
	network_objects_pack_state(&bits, state);
	CHECK(!bits.failed, "object %lx packed", state->object_index);
	network_bits_begin_read(&bits, buffer, network_bits_bytes(&bits));
	CHECK(network_objects_unpack_state(&bits, &read), "object %lx read", state->object_index);
	for (axis = 0; axis < 3 && !exact; axis++)
	{
		real got = (&read.position.x)[axis];
		real sent = (&state->position.x)[axis];

		CHECK(fabs((double)got - (double)sent) <= 1.0 / 8192.0 + fabs((double)sent) * 1e-7,
			"object %lx: axis %d at %f for %f", state->object_index, axis, got, sent);
		(&expected.position.x)[axis] = got;
	}
	CHECK(!memcmp(&read, &expected, sizeof(read)), "object %lx: the entry read back is the whole one's",
		state->object_index);
	return network_bits_bytes(&bits);
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	struct distributed_object_state state;

	if (!strcmp(name, "round-trip"))
	{
		static real const corners[] = { -32767.99f, -1234.5678f, -0.0001f, 0.0f, 0.00006f, 51.2373f, 32767.99f };
		short a, b;

		/* (at the map's bounds and between, the vectors every way) */
		for (a = 0; a < (short)(sizeof(corners) / sizeof(corners[0])); a++)
		{
			for (b = 0; b < (short)(sizeof(corners) / sizeof(corners[0])); b++)
			{
				state = object(0xE1230000 + a * 8 + b, corners[a], corners[b], corners[(a + b) % 7]);
				state.forward = unit(corners[a] + 0.5f, -corners[b], 0.25f);
				state.up = unit(-corners[b] * 0.001f, corners[a] * 0.002f, -1.0f);
				round_trip(&state, FALSE);
			}
		}
		/* (every part set: flags, the client's tick, the fastest velocities) */
		state = object(0x7FFF1FFF, 100.0f, -200.0f, 3.0f);
		state.flags = 0x3F;
		state.time = -32768;
		state.forward = unit(-0.6f, -0.8f, 0.0f);
		state.up = unit(0.0f, 0.0f, -1.0f);
		state.translational_velocity.i = 32767;
		state.translational_velocity.j = -32768;
		state.translational_velocity.k = -1;
		state.angular_velocity.i = -32768;
		state.angular_velocity.j = 32767;
		state.angular_velocity.k = 1;
		round_trip(&state, FALSE);
		/* (a vector that is no unit one: its parts all sent) */
		state.forward.i = 100;
		state.forward.j = 100;
		state.forward.k = 100;
		state.up.i = 32767;
		state.up.j = 3000;
		state.up.k = 0;
		round_trip(&state, FALSE);
		state.forward.i = 0;
		state.forward.j = 0;
		state.forward.k = 0;
		state.up.i = -32768;
		state.up.j = -32768;
		state.up.k = -32768;
		round_trip(&state, FALSE);
		/* (nothing moving, upright: a resting weapon's few bytes) */
		state = object(0x00450012, 31.25f, -77.5f, 0.75f);
		SET_FLAG(state.flags, _distributed_object_at_rest_bit, TRUE);
		CHECK(round_trip(&state, FALSE) <= 18, "a resting upright object in %ld bytes",
			round_trip(&state, FALSE));
		return 0;
	}
	if (!strcmp(name, "outside"))
	{
		/* (outside the world, and not a number: whole, exactly) */
		state = object(0x00010002, 40000.3f, 0.0f, 0.0f);
		state.time = 1234;
		state.translational_velocity.k = -200;
		CHECK(round_trip(&state, TRUE) <= (long)sizeof(state) + 1, "whole in %ld bytes", round_trip(&state, TRUE));
		state.position.y = -1.0e30f;
		round_trip(&state, TRUE);
		state = object(0x00010003, 0.0f, 0.0f, 0.0f);
		state.position.z = nanf("");
		{
			byte buffer[OBJECT_PACKED_MAXIMUM_SIZE];
			struct network_bits bits;
			struct distributed_object_state read;

			network_bits_begin_write(&bits, buffer, sizeof(buffer));
			network_objects_pack_state(&bits, &state);
			network_bits_begin_read(&bits, buffer, network_bits_bytes(&bits));
			CHECK(!bits.failed && network_objects_unpack_state(&bits, &read) && !memcmp(&read, &state, sizeof(read)),
				"not a number: whole, its bits as they were");
		}
		return 0;
	}
	if (!strcmp(name, "message") || !strcmp(name, "short"))
	{
		static struct distributed_object_state_message message;
		static byte unpacked[sizeof(struct distributed_message_header) + 255 * sizeof(struct distributed_object_state)];
		struct distributed_object_state_message const *read = (struct distributed_object_state_message const *)unpacked;
		word size;
		short index;
		short count = 20;

		memset(&message, 0, sizeof(message));
		for (index = 0; index < count; index++)
		{
			message.states[index] = object(0x10000 * (index + 1) + index, 10.0f * index + 0.37f, -5.0f, 1.0f);
			message.states[index].translational_velocity.i = (short)(index * 97 - 900);
			if (index % 3 == 0)
				SET_FLAG(message.states[index].flags, _distributed_object_at_rest_bit, TRUE);
		}
		distributed_host_send_state_message(1, &message, count);
		CHECK(sent_type == _distributed_message_object_states_packed && sent_count == count,
			"an OpenH1 client's packed (type %u)", sent_type);
		CHECK(sent_size * 3 < 2 * (sizeof(message.header) + count * sizeof(message.states[0])),
			"under two thirds of the bytes: %u", sent_size);
		if (!strcmp(name, "short"))
		{
			word cut;

			for (cut = 0; cut < sent_size; cut++)
				CHECK(!network_objects_unpack_states(sent_bytes, cut, unpacked, sizeof(unpacked)), "cut at %u: none",
					cut);
			return 0;
		}
		size = network_objects_unpack_states(sent_bytes, sent_size, unpacked, sizeof(unpacked));
		CHECK(size == sizeof(message.header) + count * sizeof(message.states[0]), "read as the whole (%u)", size);
		CHECK(read->header.type == _distributed_message_object_states && read->header.count == count &&
			read->header.game_time == 4321, "the whole message's header");
		for (index = 0; index < count; index++)
		{
			struct distributed_object_state expected = message.states[index];

			expected.position = read->states[index].position;
			CHECK(fabsf(read->states[index].position.x - message.states[index].position.x) <= 1.0f / 8192.0f &&
				!memcmp(&read->states[index], &expected, sizeof(expected)), "entry %d read as sent", index);
		}
		/* (an OpenCE machine, and the setting: the whole message as before) */
		legacy_machine = TRUE;
		distributed_host_send_state_message(1, &message, count);
		CHECK(sent_type == _distributed_message_object_states &&
			sent_size == sizeof(message.header) + count * sizeof(message.states[0]) &&
			!memcmp(sent_bytes + sizeof(message.header), message.states, count * sizeof(message.states[0])),
			"an OpenCE machine's whole (type %u)", sent_type);
		legacy_machine = FALSE;
		unpacked_setting = TRUE;
		distributed_host_send_state_message(1, &message, count);
		CHECK(sent_type == _distributed_message_object_states, "debug.network_forms_before: whole (type %u)",
			sent_type);
		return 0;
	}
	fprintf(stderr, "no such case: %s\n", name);
	return 2;
}
