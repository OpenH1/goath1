/* The host's states of a player at its sub-steps to a client, only where
they tell it more than the last it was sent (distributed_substep_state_unchanged):
an idle player's go again only as the refresh (SUBSTEP_STATE_REFRESH_TICKS);
one that moves, turns, is another unit or (predicted) of another tick of its
client's goes at once. Two sub-steps a tick besides the tick's last (120 a
second), as network_distributed_host_substep_ended sends them. */
#include "harness.h"
#define csmemcmp memcmp
#include "config.inc"
/* (what the code reads of a unit state: network_legacy.h's) */
struct distributed_unit_state
{
	byte flags;
	long unit_index;
	real_point3d position;
	struct distributed_vector velocity;
	struct distributed_vector forward;
	struct distributed_vector up;
	short predicted_time;
	real_vector3d fine_velocity;
	boolean fine;
};
#include "under_test.inc"

enum { SUBSTEPS = 2, TICKS = 300 };

static struct distributed_substep_sent sent;
static long sends;
static long last_send = NONE;
static long longest_gap;

static boolean step(long now, struct distributed_unit_state const *state)
{
	if (distributed_substep_state_unchanged(&sent, state, now))
		return FALSE;
	sends++;
	if (last_send != NONE && now - last_send > longest_gap)
		longest_gap = now - last_send;
	last_send = now;
	return TRUE;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	struct distributed_unit_state state;
	long now;
	short substep;

	memset(&state, 0, sizeof(state));
	sent.time = NONE;
	state.flags = 3;
	state.unit_index = 0x10002;
	state.position.x = 10.0f;
	state.position.y = -4.0f;
	state.position.z = 1.25f;
	state.forward.i = 16000;
	state.up.k = 16384;
	if (!strcmp(name, "idle"))
	{
		for (now = 0; now < TICKS; now++)
			for (substep = 0; substep < SUBSTEPS; substep++)
				step(now, &state);
		if (TICKS - 1 - last_send > longest_gap)
			longest_gap = TICKS - 1 - last_send;
		CHECK(sends <=TICKS / SUBSTEP_STATE_REFRESH_TICKS + 1, "an idle player's state went %ld times in %d ticks",
			sends, TICKS);
		CHECK(longest_gap <= SUBSTEP_STATE_REFRESH_TICKS, "an idle player's state went %ld ticks without a refresh",
			longest_gap);
		return 0;
	}
	if (!strcmp(name, "moved"))
	{
		step(0, &state);
		CHECK(!step(0, &state), "the same state went twice in a tick");
		state.position.x += 1.0f / 4096.0f;
		CHECK(step(0, &state), "a moved player's state did not go at once");
		state.velocity.j = 3;
		CHECK(step(1, &state), "a player's new speed did not go at once");
		state.forward.i--;
		CHECK(step(1, &state), "a turned player's state did not go at once");
		state.up.i = 1;
		CHECK(step(2, &state), "a player's new up did not go at once");
		state.unit_index = 0x20002;
		CHECK(step(2, &state), "another unit's state did not go at once");
		state.fine = TRUE;
		CHECK(step(3, &state), "a state of a fine speed did not go at once");
		state.fine_velocity.i = 0.25f;
		CHECK(step(3, &state), "a new fine speed did not go at once");
		CHECK(!step(3, &state), "an unchanged state went again");
		return 0;
	}
	if (!strcmp(name, "predicted"))
	{
		/* (a client's player, the host's state of its client's tick: one of the next tick goes, though it stands) */
		state.flags |= FLAG(_distributed_unit_predicted_bit);
		for (now = 0; now < 30; now++)
		{
			state.predicted_time = (short)(now + 5);
			CHECK(step(now, &state), "the state of the client's tick %ld did not go", now + 5);
			CHECK(!step(now, &state), "the same tick's state went twice");
		}
		state.flags &= ~FLAG(_distributed_unit_predicted_bit);
		CHECK(step(now, &state), "a state no longer predicted did not go");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
