/* A client's sub-steps to the host sized to the game's count of them
(substep_input_size, network_substeps_handle_inputs): at 60 a record has
its one later sub-step's controls, not room for three; each record of a
message read at its own size, in order, and none past the message's end. */
#include "harness.h"
#define csmemcpy memcpy
#define csmemset memset
#define PIN(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
typedef unsigned short word;
typedef struct { real i, j; } real_vector2d;
typedef struct { real yaw, pitch; } real_euler_angles2d;
#define _pi 3.14159265f
enum { MAXIMUM_TRACKED_PLAYERS = 4, MAXIMUM_SIM_SUBSTEPS = 4, MAXIMUM_LATER_SUBSTEPS = MAXIMUM_SIM_SUBSTEPS - 1 };
#define SUBSTEP_HELD_FLAGS 0xFFFF
#include "config.inc"

static short substep_count;
static short network_substeps_count(void) { return substep_count; }
static boolean distributed_machine_has_player(long machine_index, short player_index)
{
	return machine_index == 0 && player_index < 2;
}
static boolean valid_real(real value) { return value == value; }
#define distributed_sender_clock(machine_index) (0)
static struct { struct substep_record heard[MAXIMUM_TRACKED_PLAYERS]; } network_substeps_globals;
static short heard_count;
static void substep_record_heard(struct substep_record *records, short player_index, struct substep_record const *record,
	long now)
{
	(void)now;
	records[player_index] = *record;
	heard_count++;
}
#include "under_test.inc"

/* a record of the player's tick at the count, its later sub-steps' facing its number */
static word put(byte *buffer, short player_index, short count, long tick)
{
	struct distributed_substep_input input;
	short substep;

	memset(&input, 0, sizeof(input));
	input.player_index = (byte)player_index;
	input.substep_count = (byte)count;
	input.tick = tick;
	for (substep = 0; substep < count - 1; substep++)
		input.controls[substep].yaw = (real)(tick - 500 + substep) * 0.25f;
	memcpy(buffer, &input, substep_input_size(count));
	return substep_input_size(count);
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	byte buffer[4 * sizeof(struct distributed_substep_input)];
	word used = 0;

	if (!strcmp(name, "sizes"))
	{
		CHECK(substep_input_size(2) == SUBSTEP_INPUT_HEADER_SIZE + sizeof(struct distributed_substep_input_control),
			"a record at 60 is %u bytes", substep_input_size(2));
		CHECK(substep_input_size(MAXIMUM_SIM_SUBSTEPS) == sizeof(struct distributed_substep_input),
			"a record at 120 is %u bytes", substep_input_size(MAXIMUM_SIM_SUBSTEPS));
		return 0;
	}
	if (!strcmp(name, "read") || !strcmp(name, "read-120"))
	{
		substep_count = !strcmp(name, "read") ? 2 : MAXIMUM_SIM_SUBSTEPS;
		used += put(buffer + used, 0, substep_count, 500);
		used += put(buffer + used, 1, substep_count, 501);
		network_substeps_handle_inputs(0, buffer, buffer + used, 2, FALSE, NONE);
		CHECK(heard_count == 2, "%d of 2 records read", heard_count);
		CHECK(network_substeps_globals.heard[1].key == 501 && network_substeps_globals.heard[1].count == substep_count &&
			network_substeps_globals.heard[1].controls[substep_count - 2].facing.yaw == (real)(1 + substep_count - 2) * 0.25f,
			"the second record read as another");
		return 0;
	}
	if (!strcmp(name, "short"))
	{
		substep_count = 2;
		used += put(buffer + used, 0, 2, 500);
		used += put(buffer + used, 1, 2, 501);
		network_substeps_handle_inputs(0, buffer, buffer + used - 1, 2, FALSE, NONE);
		CHECK(heard_count == 1, "%d records read of a message a byte short of 2", heard_count);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
