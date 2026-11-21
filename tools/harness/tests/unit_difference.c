/* An OpenH1 game's tick states as differences (distributed_unit_difference_write
and distributed_unit_difference_read, OpenH1 protocol version 23): the host
writes a player's state to a client as its difference from the latest state
of them it sent the client at a tick no later than the one the client said
it took, else whole; the client reads it against the state it took of that
tick. Read back, every state is exactly what the whole state would have
been (the same bytes written again), through a chain of ticks whose acks
lag; a difference from a state the client has not is passed over, the
entries after it read as sent. Machine 1 is the client. Each case packed
(distributed_unit_packed_write, as the host sends them) and, -unpacked, in
the forms before (debug.network_forms_before). */
#include "harness.h"
#include <math.h>
typedef unsigned short word;
enum { MAXIMUM_TRACKED_PLAYERS = 4, HALO_PORT_MAXIMUM_NETWORK_MACHINES = 4 };
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define csmemset memset
#define csmemcpy memcpy
#define csmemcmp memcmp
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define UNIT_WORLD_BOUND 32768.0f
struct distributed_vector { short i; short j; short k; };
#include "config.inc"
static struct distributed_unit_state distributed_tick_states[MAXIMUM_TRACKED_PLAYERS][UNIT_BASE_TICKS];
static long distributed_tick_state_times[MAXIMUM_TRACKED_PLAYERS][UNIT_BASE_TICKS];
static long distributed_unit_sent_times[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS][UNIT_SENT_TICKS];
static long distributed_unit_acked_times[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static struct distributed_unit_state distributed_unit_bases[MAXIMUM_TRACKED_PLAYERS][UNIT_BASE_TICKS];
static long distributed_unit_base_times[MAXIMUM_TRACKED_PLAYERS][UNIT_BASE_TICKS];
static boolean distributed_tick_states_unpacked;

static long now_time;
/* the first byte of the last entry tick() wrote */
static byte last_form;
static long game_time_get(void) { return now_time; }
static boolean distributed_point_valid(real_point3d const *point, real bound) {
	return fabsf(point->x) <= bound && fabsf(point->y) <= bound && fabsf(point->z) <= bound;
}
static boolean distributed_scripted_pose_valid(struct distributed_unit_state const *state) { (void)state; return TRUE; }
/* (vehicle_riding.c's checks of a deck part: the vehicle of the test's one,
its ticks a window's) */
enum { DECK_VEHICLE = 0x00170005, DECK_NODES = 3 };
static boolean vehicle_riding_deck_values_valid(struct vehicle_riding_deck const *deck) {
	return deck->place.x == deck->place.x && deck->velocity.i == deck->velocity.i;
}
static boolean vehicle_riding_deck_vehicle_named(long vehicle_index) { return vehicle_index == DECK_VEHICLE; }
static short biped_riding_vehicle_node_count(long vehicle_index) { return vehicle_index == DECK_VEHICLE ? DECK_NODES : NONE; }
static boolean vehicle_riding_deck_valid(struct vehicle_riding_deck const *deck, short node_count) {
	return deck->ticks >= 1 && deck->ticks <= 60 && deck->node_index >= NONE && deck->node_index < node_count;
}
#include "under_test.inc"

/* a difference's first byte: with the deck part, and a difference at all
(packed, or as before) */
static boolean deck_form(byte form)
{
	return distributed_tick_states_unpacked ? form == _unit_difference_form_deck :
		TEST_FLAG(form, UNIT_PACKED_FORM_BIT) && TEST_FLAG(form, UNIT_PACKED_DECK_BIT);
}
static boolean difference_form(byte form)
{
	return distributed_tick_states_unpacked ? form == _unit_difference_form_difference :
		TEST_FLAG(form, UNIT_PACKED_FORM_BIT) && !TEST_FLAG(form, UNIT_PACKED_DECK_BIT);
}

static void reset(void)
{
	short player, tick;
	long machine;

	for (player = 0; player < MAXIMUM_TRACKED_PLAYERS; player++)
	{
		for (tick = 0; tick < UNIT_BASE_TICKS; tick++)
		{
			distributed_tick_state_times[player][tick] = NONE;
			distributed_unit_base_times[player][tick] = NONE;
		}
		for (machine = 0; machine < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine++)
		{
			for (tick = 0; tick < UNIT_SENT_TICKS; tick++)
				distributed_unit_sent_times[machine][player][tick] = NONE;
		}
	}
	for (machine = 0; machine < HALO_PORT_MAXIMUM_NETWORK_MACHINES; machine++)
		distributed_unit_acked_times[machine] = NONE;
}

/* a player on foot of a client (predicted) */
static struct distributed_unit_state on_foot(byte player, real x, real y, real z, short predicted)
{
	struct distributed_unit_state state;

	memset(&state, 0, sizeof(state));
	state.player_index = player;
	state.flags = FLAG(_distributed_unit_alive_bit) | FLAG(_distributed_unit_placed_bit) |
		FLAG(_distributed_unit_predicted_bit);
	state.killing_player_index = NO_PLAYER;
	state.unit_index = 0x12340000 + player;
	state.vehicle_index = NONE;
	state.seat_index = NONE;
	state.position.x = x;
	state.position.y = y;
	state.position.z = z;
	state.velocity.i = 300;
	state.forward.i = 32000;
	state.forward.j = 2000;
	state.up.k = (short)DISTRIBUTED_UNIT_SCALE;
	state.predicted_time = predicted;
	state.body_vitality = 16384;
	state.shield_vitality = 16384;
	return state;
}

/* the host's tick: the state kept on the grid and sent to machine 1 (as the
host's send does), the client reading it; the bytes, whether a difference,
and whether known */
static word tick(struct distributed_unit_state const *host_state, long time, boolean predicted, boolean lose,
	boolean *difference, boolean *known)
{
	byte buffer[1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
	byte expected[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
	byte got[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
	struct distributed_unit_state *kept;
	struct distributed_unit_state read;
	long *sent = distributed_unit_sent_times[1][host_state->player_index];
	short slot = (short)(time & (UNIT_BASE_TICKS - 1));
	short index, oldest = 0;
	word size, expected_size, got_size;

	now_time = time;
	kept = &distributed_tick_states[host_state->player_index][slot];
	*kept = *host_state;
	distributed_unit_state_snap(kept);
	/* (on the grid, an 8192th of a world unit off at most) */
	CHECK(fabsf(kept->position.x - host_state->position.x) <= 1.0f / 8192.0f + 1e-6f &&
		fabsf(kept->position.y - host_state->position.y) <= 1.0f / 8192.0f + 1e-6f &&
		fabsf(kept->position.z - host_state->position.z) <= 1.0f / 8192.0f + 1e-6f,
		"tick %ld: the position on the grid (%f %f %f) for (%f %f %f)", time, kept->position.x, kept->position.y,
		kept->position.z, host_state->position.x, host_state->position.y, host_state->position.z);
	distributed_tick_state_times[host_state->player_index][slot] = time;
	size = distributed_unit_difference_write(1, host_state->player_index, kept, predicted, buffer);
	for (index = 1; index < UNIT_SENT_TICKS; index++)
	{
		if (sent[index] == NONE || (sent[oldest] != NONE && sent[index] < sent[oldest]))
			oldest = index;
	}
	sent[oldest] = time;
	*difference = buffer[0] != 0;
	last_form = buffer[0];
	/* (joined, as the host sends an OpenH1 client the entries: read back as
	the same bytes, each head against the entry before's on both machines) */
	if (!distributed_tick_states_unpacked)
	{
		static struct distributed_unit_joining host_joining = { NONE, 0, FALSE, 0 };
		static struct distributed_unit_joining client_joining = { NONE, 0, FALSE, 0 };
		byte joined[4 + 2 * DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
		byte back[1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
		struct network_bits bits;
		struct network_bits reading;
		word back_size;

		network_bits_begin_write(&bits, joined, sizeof(joined));
		CHECK(distributed_unit_entry_join(&host_joining, buffer, size, &bits), "tick %ld: the entry joined", time);
		network_bits_begin_read(&reading, joined, network_bits_bytes(&bits));
		back_size = distributed_unit_entry_unjoin(&client_joining, &reading, back, sizeof(back));
		CHECK(back_size == size && !memcmp(back, buffer, size) && reading.position == bits.position,
			"tick %ld: the joined entry read back as its %u bytes (%u, %ld bits of %ld)", time, size, back_size,
			reading.position, bits.position);
		CHECK(bits.position <= 8 * size + 2, "tick %ld: joined, %ld bits for %u bytes", time, bits.position, size);
	}
	if (lose)
	{
		*known = FALSE;
		return size;
	}
	CHECK(distributed_unit_difference_read(buffer, buffer + size, time, &read, known) == size,
		"the reader takes the bytes the writer wrote (%u)", size);
	if (!*known)
		return size;
	distributed_unit_bases[read.player_index][slot] = read;
	distributed_unit_base_times[read.player_index][slot] = time;
	/* (exactly the whole state's bytes) */
	expected_size = distributed_unit_state_write(kept, predicted, expected);
	got_size = distributed_unit_state_write(&read, TEST_FLAG(read.flags, _distributed_unit_predicted_bit), got);
	CHECK(expected_size == got_size && !memcmp(expected, got, expected_size),
		"tick %ld: the state read is the whole state's (%u bytes against %u)", time, got_size, expected_size);
	return size;
}

int main(int argc, char **argv)
{
	char which[64];
	struct distributed_unit_state state;
	boolean difference, known;
	word size;
	long time;
	size_t length;

	snprintf(which, sizeof(which), "%s", argc > 1 ? argv[1] : "");
	length = strlen(which);
	/* (the forms before packing: debug.network_forms_before) */
	if (length > 9 && !strcmp(which + length - 9, "-unpacked"))
	{
		distributed_tick_states_unpacked = TRUE;
		which[length - 9] = 0;
	}
	reset();
	if (!strcmp(which, "prediction-trim"))
	{
		/* (an OpenH1 client's prediction to its host, without the parts the
		host reads not: shorter, and every part it reads as it was) */
		byte whole[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE + 12];
		byte trimmed_bytes[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE + 12];
		struct distributed_unit_state trimmed;
		struct distributed_unit_state read;
		word whole_size, trimmed_size;

		state = on_foot(1, 12.5f, -3.25f, 0.5f, 0);
		SET_FLAG(state.flags, _distributed_unit_predicted_bit, FALSE);
		SET_FLAG(state.flags, _distributed_unit_shield_charging_bit, TRUE);
		state.current_body_damage = 300;
		state.recent_shield_damage = 900;
		state.powerup_durations[0] = 77;
		state.active_camouflage = 12;
		SET_FLAG(state.unit_flags, _distributed_unit_camouflaged_bit, TRUE);
		SET_FLAG(state.unit_flags, _distributed_unit_suspended_bit, TRUE);
		state.fine = TRUE;
		state.fine_velocity.i = 0.293f;
		state.fine_velocity.k = -0.011f;
		whole_size = distributed_unit_state_write(&state, TRUE, whole);
		trimmed = state;
		distributed_prediction_trim(&trimmed);
		trimmed_size = distributed_unit_state_write(&trimmed, TRUE, trimmed_bytes);
		CHECK(trimmed_size + 8 + 2 * NUMBER_OF_PLAYER_POWERUPS <= whole_size, "trimmed %u bytes, whole %u",
			trimmed_size, whole_size);
		CHECK(distributed_unit_state_read(trimmed_bytes, trimmed_bytes + trimmed_size, &read) == trimmed_size,
			"the trimmed prediction read");
		CHECK(read.player_index == state.player_index && read.unit_index == state.unit_index &&
			TEST_FLAG(read.flags, _distributed_unit_alive_bit) && TEST_FLAG(read.flags, _distributed_unit_placed_bit) &&
			!memcmp(&read.position, &state.position, sizeof(read.position)) &&
			!memcmp(&read.forward, &state.forward, sizeof(read.forward)) &&
			!memcmp(&read.up, &state.up, sizeof(read.up)) && read.fine &&
			!memcmp(&read.fine_velocity, &state.fine_velocity, sizeof(read.fine_velocity)) &&
			TEST_FLAG(read.unit_flags, _distributed_unit_suspended_bit),
			"the parts the host reads kept");
		CHECK(!read.current_body_damage && !read.recent_shield_damage && !read.powerup_durations[0] &&
			!read.active_camouflage && !read.body_vitality, "the parts the host reads not left out");
		return 0;
	}
	if (!strcmp(which, "prediction-rest"))
	{
		/* (a unit standing still: its velocity, its physics' leftover,
		another each tick and never none, goes as none, so the ticks'
		predictions are the same bytes, held; a velocity that moves it,
		however slowly, goes exactly as it is) */
		byte first[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE + 12];
		byte second[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE + 12];
		byte still[DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE + 12];
		struct distributed_unit_state rest, next, moving, read;
		word first_size, second_size, still_size, size;

		rest = on_foot(1, 12.5f, -3.25f, 0.5f, 0);
		SET_FLAG(rest.flags, _distributed_unit_predicted_bit, FALSE);
		rest.fine = TRUE;
		rest.fine_velocity.i = -4.40514778e-28f;
		rest.fine_velocity.j = -9.66490318e-26f;
		rest.fine_velocity.k = 3.89606383e-27f;
		next = rest;
		next.fine_velocity.i *= -0.75f;
		next.fine_velocity.j *= -0.75f;
		next.fine_velocity.k *= -0.75f;
		distributed_prediction_trim(&rest);
		distributed_prediction_trim(&next);
		first_size = distributed_unit_state_write(&rest, TRUE, first);
		second_size = distributed_unit_state_write(&next, TRUE, second);
		CHECK(first_size == second_size && !memcmp(first, second, first_size),
			"a unit at rest's predictions of two ticks the same bytes");
		rest.fine_velocity.i = rest.fine_velocity.j = rest.fine_velocity.k = 0.0f;
		still_size = distributed_unit_state_write(&rest, TRUE, still);
		CHECK(still_size == first_size && !memcmp(still, first, first_size), "as a unit with no velocity at all");
		/* (the slowest that moves it a step of the grid in a minute, and a
		walk's, kept) */
		moving = on_foot(1, 12.5f, -3.25f, 0.5f, 0);
		SET_FLAG(moving.flags, _distributed_unit_predicted_bit, FALSE);
		moving.fine = TRUE;
		moving.fine_velocity.i = 0.293f;
		moving.fine_velocity.j = -PREDICTION_REST_SPEED;
		moving.fine_velocity.k = 2.0f * PREDICTION_REST_SPEED;
		next = moving;
		distributed_prediction_trim(&next);
		size = distributed_unit_state_write(&next, TRUE, first);
		CHECK(distributed_unit_state_read(first, first + size, &read) == size && read.fine &&
			!memcmp(&read.fine_velocity, &moving.fine_velocity, sizeof(read.fine_velocity)),
			"a moving unit's velocity kept exactly (%g %g %g)", read.fine_velocity.i, read.fine_velocity.j,
			read.fine_velocity.k);
		return 0;
	}
	if (!strcmp(which, "joined"))
	{
		/* (three players a tick, one after another and one apart, as a
		message's entries joined: each read back as its bytes, against the
		one before's) */
		struct distributed_unit_state players[3];
		short index;

		players[0] = on_foot(0, 10.0f, 20.0f, 1.0f, 500);
		players[1] = on_foot(1, -10.0f, 25.0f, 1.0f, 500);
		players[2] = on_foot(3, 40.0f, -5.0f, 2.0f, 500);
		for (time = 500; time < 520; time++)
		{
			distributed_unit_acked_times[1] = time - 1 - (time % 2);
			for (index = 0; index < 3; index++)
			{
				players[index].position.x += 0.05f * (index + 1);
				players[index].forward.j = (short)(players[index].forward.j + 40 * index);
				players[index].predicted_time = (short)(players[index].predicted_time + 1);
				tick(&players[index], time, TRUE, FALSE, &difference, &known);
				CHECK(time < 503 || (difference && known), "tick %ld, player %d: a difference, known", time, index);
			}
		}
		return 0;
	}
	if (!strcmp(which, "chain"))
	{
		/* (a player running and turning, hurt, killed, respawned, the
		client's word a tick or three behind) */
		state = on_foot(2, 51.2373f, -77.5011f, 0.7531f, 1000);
		tick(&state, 100, TRUE, FALSE, &difference, &known);
		CHECK(!difference, "no ack yet: whole");
		for (time = 101; time < 160; time++)
		{
			distributed_unit_acked_times[1] = time - 1 - (time % 3);
			state.position.x += 0.2137f;
			state.position.y -= 0.0441f * (real)(time % 5);
			state.velocity.i = (short)(state.velocity.i + (time % 7) * 9);
			state.forward.j = (short)(state.forward.j + 70);
			state.predicted_time = (short)(state.predicted_time + 1);
			if (time == 120)
			{
				state.shield_vitality = 0;
				SET_FLAG(state.flags, _distributed_unit_shield_depleted_bit, TRUE);
				state.current_shield_damage = 9000;
			}
			if (time == 130)
			{
				state.flags = 0;
				state.killing_player_index = 1;
			}
			if (time == 140)
			{
				state = on_foot(2, -20.0f, 30.0f, 2.0f, (short)(state.predicted_time + 1));
				state.unit_index = 0x12350002;
			}
			size = tick(&state, time, TRUE, FALSE, &difference, &known);
			CHECK(known, "tick %ld known", time);
			/* (packed, a dead player's few bytes go whole where that is shorter) */
			if (time > 102 && time != 140 &&
				(distributed_tick_states_unpacked || TEST_FLAG(state.flags, _distributed_unit_alive_bit)))
			{
				CHECK(difference, "tick %ld a difference", time);
			}
			/* (from the tick before: the position, the velocity's and the
			facing's changes in a byte a part, the client's tick) */
			if (time > 102 && time < 120 && time % 3 == 0)
				CHECK(size <= 18, "tick %ld: a running player's difference %u bytes", time, size);
		}
		return 0;
	}
	if (!strcmp(which, "not-predicted"))
	{
		/* (a host's player or bot to another client: no predicted tick) */
		state = on_foot(0, 5.0f, 5.0f, 5.0f, 0);
		state.flags &= ~FLAG(_distributed_unit_predicted_bit);
		tick(&state, 10, FALSE, FALSE, &difference, &known);
		distributed_unit_acked_times[1] = 10;
		state.position.z += 0.5f;
		tick(&state, 11, FALSE, FALSE, &difference, &known);
		CHECK(difference && known, "a difference");
		return 0;
	}
	if (!strcmp(which, "whole"))
	{
		state = on_foot(3, 0.0f, 0.0f, 0.0f, 200);
		tick(&state, 20, TRUE, FALSE, &difference, &known);
		/* (the client asked for them whole) */
		distributed_unit_acked_times[1] = NONE;
		state.position.x += 0.1f;
		tick(&state, 21, TRUE, FALSE, &difference, &known);
		CHECK(!difference && known, "asked for whole: whole");
		/* (acked a tick none was sent at, before any: whole) */
		distributed_unit_acked_times[1] = 19;
		tick(&state, 22, TRUE, FALSE, &difference, &known);
		CHECK(!difference, "nothing sent at or before the ack: whole");
		/* (a teleport past 8 world units: whole in the forms before, a
		difference packed) */
		distributed_unit_acked_times[1] = 22;
		state.position.y += 9.0f;
		tick(&state, 23, TRUE, FALSE, &difference, &known);
		CHECK(difference == !distributed_tick_states_unpacked && known, "a teleport: %s",
			difference ? "a difference" : "whole");
		/* (the latest acked older than the client keeps) */
		distributed_unit_acked_times[1] = 23;
		tick(&state, 23 + UNIT_BASE_TICKS, TRUE, FALSE, &difference, &known);
		CHECK(!difference, "an old ack: whole");
		return 0;
	}
	if (!strcmp(which, "unknown"))
	{
		byte buffer[2 * (1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE)];
		struct distributed_unit_state other = on_foot(1, 9.0f, 9.0f, 9.0f, 7);
		struct distributed_unit_state read;
		word first, second, taken;

		/* (tick 50's state lost: the client's ack names 49, and tick 51's goes
		against 49's) */
		state = on_foot(0, 1.0f, 2.0f, 3.0f, 70);
		tick(&state, 48, TRUE, FALSE, &difference, &known);
		distributed_unit_acked_times[1] = 48;
		state.position.x += 0.25f;
		tick(&state, 49, TRUE, FALSE, &difference, &known);
		CHECK(difference && known, "tick 49 a difference");
		state.position.x += 0.25f;
		tick(&state, 50, TRUE, TRUE, &difference, &known);
		CHECK(difference, "tick 50 a difference (lost)");
		distributed_unit_acked_times[1] = 49;
		state.position.x += 0.25f;
		tick(&state, 51, TRUE, FALSE, &difference, &known);
		CHECK(difference && known, "tick 51 against the tick acked, not the one lost");
		/* (an ack naming tick 52, its message of another player having come
		and this player's lost) */
		state.position.x += 0.25f;
		tick(&state, 52, TRUE, TRUE, &difference, &known);
		distributed_unit_acked_times[1] = 52;
		now_time = 53;
		state.position.x += 0.25f;
		distributed_tick_states[0][53 & (UNIT_BASE_TICKS - 1)] = state;
		distributed_unit_state_snap(&distributed_tick_states[0][53 & (UNIT_BASE_TICKS - 1)]);
		distributed_tick_state_times[0][53 & (UNIT_BASE_TICKS - 1)] = 53;
		first = distributed_unit_difference_write(1, 0, &distributed_tick_states[0][53 & (UNIT_BASE_TICKS - 1)], TRUE,
			buffer);
		CHECK(buffer[0] != 0, "written against tick 52");
		second = distributed_unit_difference_write(1, 1, &other, TRUE, buffer + first);
		taken = distributed_unit_difference_read(buffer, buffer + first + second, 53, &read, &known);
		CHECK(taken == first && !known, "a difference from a state not taken: passed over");
		taken = distributed_unit_difference_read(buffer + first, buffer + first + second, 53, &read, &known);
		CHECK(taken == second && known && read.player_index == 1, "the entry after it read as sent");
		return 0;
	}
	if (!strcmp(which, "short"))
	{
		byte buffer[1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
		struct distributed_unit_state read;
		word cut;

		state = on_foot(2, 3.0f, 3.0f, 3.0f, 5);
		tick(&state, 300, TRUE, FALSE, &difference, &known);
		distributed_unit_acked_times[1] = 300;
		state.position.z += 0.25f;
		state.body_vitality = 1000;
		now_time = 301;
		distributed_tick_states[2][301 & (UNIT_BASE_TICKS - 1)] = state;
		distributed_tick_state_times[2][301 & (UNIT_BASE_TICKS - 1)] = 301;
		size = distributed_unit_difference_write(1, 2, &state, TRUE, buffer);
		CHECK(buffer[0] != 0, "a difference");
		for (cut = 0; cut < size; cut++)
			CHECK(!distributed_unit_difference_read(buffer, buffer + cut, 301, &read, &known), "cut at %u: none", cut);
		return 0;
	}
	if (!strcmp(which, "deck"))
	{
		/* (OpenH1 protocol 31: a player who steps onto a vehicle's deck, rides
		it, jumps off into its window and lands away from it; each state read
		back is the whole state's, its deck part with it) */
		state = on_foot(1, 10.0f, 10.0f, 1.0f, 40);
		tick(&state, 400, TRUE, FALSE, &difference, &known);
		for (time = 401; time < 440; time++)
		{
			distributed_unit_acked_times[1] = time - 1;
			state.position.x += 0.125f;
			state.predicted_time = (short)(state.predicted_time + 1);
			if (time == 405)
			{
				state.deck_present = TRUE;
				state.deck.vehicle_index = DECK_VEHICLE;
				state.deck.node_index = NONE;
				state.deck.ticks = 60;
				state.deck.place.x = 0.5f;
				state.deck.place.y = -0.25f;
				state.deck.place.z = 0.75f;
				state.deck.velocity.i = 0.0625f;
			}
			if (time > 405 && time < 420)
				state.deck.place.x += 0.03125f;
			if (time == 420)
				state.deck.node_index = 2;
			if (time >= 425 && time < 430)
			{
				/* (in the air: the window runs down) */
				state.deck.ticks = (char)(60 - (time - 424));
				state.deck.flags = 1;
				state.deck.peak = 1.5f;
			}
			if (time == 430)
				state.deck_present = FALSE;
			tick(&state, time, TRUE, FALSE, &difference, &known);
			CHECK(known && difference, "tick %ld: a known difference", time);
			/* (the deck part goes where it changed, and only there) */
			if (time == 405 || (time > 405 && time <= 420) || (time >= 425 && time <= 430))
				CHECK(deck_form(last_form), "tick %ld: the deck part not sent (form %u)", time, last_form);
			else
				CHECK(difference_form(last_form), "tick %ld: the deck part sent unchanged (form %u)", time, last_form);
		}
		/* (a presence byte not 0 or 1 from another machine: no state) */
		{
			byte buffer[1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
			struct distributed_unit_state read;

			distributed_unit_acked_times[1] = 439;
			state.deck_present = TRUE;
			state.deck.ticks = 60;
			now_time = 440;
			distributed_tick_states[1][440 & (UNIT_BASE_TICKS - 1)] = state;
			distributed_unit_state_snap(&distributed_tick_states[1][440 & (UNIT_BASE_TICKS - 1)]);
			distributed_tick_state_times[1][440 & (UNIT_BASE_TICKS - 1)] = 440;
			size = distributed_unit_difference_write(1, 1, &distributed_tick_states[1][440 & (UNIT_BASE_TICKS - 1)],
				TRUE, buffer);
			CHECK(deck_form(buffer[0]) && buffer[size - 1 - DISTRIBUTED_DECK_SIZE] == 1, "a deck part written");
			buffer[size - 1 - DISTRIBUTED_DECK_SIZE] = 2;
			CHECK(!distributed_unit_difference_read(buffer, buffer + size, 440, &read, &known),
				"a deck part's presence of 2 taken");
		}
		return 0;
	}
	if (!strcmp(which, "extremes"))
	{
		struct distributed_unit_state off;
		byte buffer[1 + DISTRIBUTED_UNIT_STATE_MAXIMUM_SIZE];
		struct distributed_unit_state read;

		/* (the map's corner to its other, at the fastest speeds) */
		state = on_foot(1, -32767.75f, -32767.75f, -32767.75f, 30000);
		state.velocity.i = state.velocity.j = state.velocity.k = -32768;
		tick(&state, 500, TRUE, FALSE, &difference, &known);
		distributed_unit_acked_times[1] = 500;
		state.position.x = state.position.y = state.position.z = 32767.75f;
		state.velocity.i = state.velocity.j = state.velocity.k = 32767;
		state.predicted_time = (short)(state.predicted_time + 1);
		tick(&state, 501, TRUE, FALSE, &difference, &known);
		CHECK(known && difference == !distributed_tick_states_unpacked, "corner to corner known");
		/* (every part changed: flags, killer, unit, vehicle, position,
		velocity, a facing whose largest part is negative, up, the client's
		tick, shields, damage, powerups, camouflage) */
		distributed_unit_acked_times[1] = 501;
		state.flags ^= FLAG(_distributed_unit_shield_charging_bit) | FLAG(_distributed_unit_shield_depleted_bit);
		state.killing_player_index = 3;
		state.unit_index = 0x7FFE0001;
		state.vehicle_index = 0x00200030;
		state.seat_index = 2;
		state.position.x = 12.5f;
		state.position.y = -0.25f;
		state.position.z = 2.0f;
		state.velocity.i = -1;
		state.velocity.j = 0;
		state.velocity.k = 1;
		state.forward.i = -3000;
		state.forward.j = -32630;
		state.forward.k = 0;
		state.up.i = 32767;
		state.up.j = 3000;
		state.up.k = 0;
		state.predicted_time = (short)(state.predicted_time + 9999);
		state.body_vitality = 1;
		state.shield_vitality = 65535;
		state.current_body_damage = 65535;
		state.recent_body_damage = 0;
		state.current_shield_damage = 12345;
		state.recent_shield_damage = 54321;
		state.powerup_durations[0] = -1;
		state.powerup_durations[1] = 32767;
		state.unit_flags = 0xFF;
		state.active_camouflage = 200;
		tick(&state, 502, TRUE, FALSE, &difference, &known);
		CHECK(known, "every part changed known");
		/* (a facing far from a unit vector, every way) */
		distributed_unit_acked_times[1] = 502;
		state.forward.i = 32767;
		state.forward.j = 3000;
		state.forward.k = 0;
		state.predicted_time = (short)(state.predicted_time + 1);
		tick(&state, 503, TRUE, FALSE, &difference, &known);
		distributed_unit_acked_times[1] = 503;
		state.forward.i = -32768;
		state.forward.j = -32768;
		state.forward.k = 0;
		state.predicted_time = (short)(state.predicted_time + 1);
		tick(&state, 504, TRUE, FALSE, &difference, &known);
		CHECK(known && difference, "far from a unit vector: a difference");
		/* (each part alone, as a difference: its largest part negative, the
		facing; then the rest) */
		for (time = 0; time < 11; time++)
		{
			distributed_unit_acked_times[1] = 504 + time * 2;
			now_time = 505 + time * 2;
			state.predicted_time = (short)(state.predicted_time + 2);
			switch (time)
			{
			case 0: state.forward.i = -3000; state.forward.j = -32630; break;
			case 1: state.flags ^= FLAG(_distributed_unit_shield_over_charging_bit); break;
			case 2: state.killing_player_index = 0; break;
			case 3: state.unit_index = 0x7FFE0002; break;
			case 4: state.vehicle_index = NONE; state.seat_index = NONE; break;
			case 5: state.up.i = 0; state.up.j = 0; state.up.k = 32767; break;
			case 6: state.powerup_durations[0] = 5; break;
			case 7: state.active_camouflage = 0; break;
			case 8: state.body_vitality = 16000; state.shield_vitality = 0; break;
			case 9: state.current_body_damage = 0; state.recent_shield_damage = 100; break;
			default: state.predicted_time = (short)(state.predicted_time - 700); break;
			}
			/* (the tick between, unsent: the next goes two ticks on) */
			size = tick(&state, 505 + time * 2, TRUE, FALSE, &difference, &known);
			CHECK(known && difference, "part %ld alone: a difference", time);
		}
		/* (nothing changed but the client's tick, a tick on: the player and
		an empty part byte, packed) */
		distributed_unit_acked_times[1] = 525;
		state.predicted_time = (short)(state.predicted_time + 1);
		size = tick(&state, 526, TRUE, FALSE, &difference, &known);
		CHECK(known && difference, "unchanged: a difference");
		if (!distributed_tick_states_unpacked)
			CHECK(size == 3, "unchanged: %u bytes", size);
		/* (off the grid, as no host keeps a state: whole, read back as it) */
		off = state;
		off.position.x += 1.0f / 65536.0f;
		distributed_unit_acked_times[1] = 526;
		now_time = 527;
		distributed_tick_states[1][527 & (UNIT_BASE_TICKS - 1)] = off;
		distributed_tick_state_times[1][527 & (UNIT_BASE_TICKS - 1)] = 527;
		size = distributed_unit_difference_write(1, 1, &off, TRUE, buffer);
		CHECK(buffer[0] == _unit_difference_form_whole, "off the grid: whole (form %u)", buffer[0]);
		CHECK(distributed_unit_difference_read(buffer, buffer + size, 527, &read, &known) == size && known &&
			read.position.x == off.position.x, "off the grid: read as it is");
		return 0;
	}
	fprintf(stderr, "no such case: %s\n", which);
	return 2;
}
