/* The host's relayed input to an OpenH1 client on a change only
(distributed_relayed_action_due): an unchanged input goes only as its
refresh, a change on the player's next tick of the plan (a button at once) and again on the one after,
and the client, which keeps the last it had (player_queues_new.c's
update_client_handle_relayed_action), has the host's input again within the
plan's period after the change's after one lost datagram, and within the refresh after the
second send when both are lost.
Player 0 to machine 0, planned every PERIOD ticks. */
#include "harness.h"
#define csmemset memset
enum { HALO_PORT_MAXIMUM_NETWORK_MACHINES = 2, MAXIMUM_TRACKED_PLAYERS = 2, DISTRIBUTED_INPUT_HISTORY = 3,
	TICKS_PER_SECOND = 30, PERIOD = 3, TICKS = 400 };
#include "config.inc"
static struct distributed_relayed_action distributed_host_actions[MAXIMUM_TRACKED_PLAYERS];
#include "under_test.inc"

/* the client's: the last input it had */
static struct distributed_relayed_action client;
static boolean client_valid;
static long sends;
static long last_send_time = NONE;
static long longest_gap;

static boolean same(struct distributed_relayed_action const *a, struct distributed_relayed_action const *b)
{
	return a->yaw == b->yaw && a->pitch == b->pitch && a->throttle_i == b->throttle_i &&
		a->throttle_j == b->throttle_j && a->primary_trigger == b->primary_trigger &&
		a->control_flags[0] == b->control_flags[0] && a->desired_weapon_index == b->desired_weapon_index &&
		a->desired_grenade_index == b->desired_grenade_index && a->desired_zoom_level == b->desired_zoom_level;
}

/* one tick of the host: whether it went (and, unless lost, the client has it) */
static boolean tick(long now, boolean pressed, boolean substeps, boolean lost)
{
	boolean planned = now % PERIOD == 0;
	boolean due = distributed_relayed_action_due(0, 0, pressed, substeps, planned, now);

	if (!due)
		return FALSE;
	sends++;
	if (last_send_time != NONE && now - last_send_time > longest_gap)
		longest_gap = now - last_send_time;
	last_send_time = now;
	if (!lost)
	{
		client = distributed_host_actions[0];
		client_valid = TRUE;
	}
	return TRUE;
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	struct distributed_relayed_action *action = &distributed_host_actions[0];
	long now;

	memset(distributed_relayed_sent, 0, sizeof(distributed_relayed_sent));
	action->yaw = 100;
	action->desired_weapon_index = 0;
	action->desired_grenade_index = 0;
	action->desired_zoom_level = -1;
	if (!strcmp(name, "unchanged"))
	{
		for (now = 0; now < TICKS; now++)
			tick(now, FALSE, FALSE, FALSE);
		/* (the first, its repeat, then the refresh alone) */
		CHECK(sends <= 2 + TICKS / RELAYED_ACTION_REFRESH_TICKS + 1, "%ld sends of an unchanged input in %d ticks",
			sends, TICKS);
		CHECK(longest_gap <= RELAYED_ACTION_REFRESH_TICKS, "an unchanged input went %ld ticks without a refresh",
			longest_gap);
		CHECK(sends >= TICKS / RELAYED_ACTION_REFRESH_TICKS, "no refresh: %ld sends", sends);
		return 0;
	}
	if (!strcmp(name, "change"))
	{
		/* each part of it changed off the plan's ticks: its buttons and
		choices at once, its facing, sticks and trigger on the plan's next
		tick (as a player that changes always went) */
		for (now = 0; now < 200; now++)
		{
			boolean changed = now >= 100 && now % 10 == 1;
			boolean button = FALSE;

			if (changed)
			{
				switch ((now / 10) % 6)
				{
				case 0: action->yaw++; break;
				case 1: action->pitch++; break;
				case 2: action->throttle_i++; break;
				case 3: action->primary_trigger++; break;
				case 4: action->desired_weapon_index ^= 1; button = TRUE; break;
				default: action->control_flags[0] ^= 4; button = TRUE; break;
				}
			}
			CHECK(tick(now, FALSE, FALSE, FALSE) || !button, "tick %ld: a button or choice not sent at once", now);
			if (now % PERIOD == 0)
				CHECK(client_valid && same(&client, action), "tick %ld: the client has not the host's input", now);
		}
		return 0;
	}
	if (!strcmp(name, "moving"))
	{
		/* (aiming all the time: on the plan's ticks only, as before) */
		for (now = 0; now < 100; now++)
		{
			action->yaw++;
			CHECK(tick(now, FALSE, FALSE, FALSE) == (now % PERIOD == 0 || now == 0),
				"tick %ld: a facing that changes every tick %s", now, now % PERIOD ? "sent off the plan's ticks" :
				"not sent on the plan's tick");
		}
		return 0;
	}
	if (!strcmp(name, "lost") || !strcmp(name, "lost-twice"))
	{
		boolean twice = !strcmp(name, "lost-twice");
		long changed_at = 151;
		long healed = NONE;
		short lost_count = 0;

		for (now = 0; now < 150; now++)
			tick(now, FALSE, FALSE, FALSE);
		action->throttle_j = 127;
		for (now = changed_at; now < changed_at + 2 * RELAYED_ACTION_REFRESH_TICKS; now++)
		{
			/* (its first send lost; with twice, its second too) */
			boolean lost = lost_count < (twice ? 2 : 1);

			if (tick(now, FALSE, FALSE, lost) && lost)
				lost_count++;
			if (healed == NONE && same(&client, action))
				healed = now;
		}
		CHECK(healed != NONE, "a lost change never healed");
		if (!twice)
			CHECK(healed - changed_at <= 2 * PERIOD, "one lost change healed after %ld ticks (the plan's period %d)",
				healed - changed_at, PERIOD);
		else
			CHECK(healed - changed_at <= 2 * PERIOD + RELAYED_ACTION_REFRESH_TICKS,
				"two lost sends healed after %ld ticks (the refresh %d after the second)", healed - changed_at,
				RELAYED_ACTION_REFRESH_TICKS);
		return 0;
	}
	if (!strcmp(name, "pressed"))
	{
		for (now = 0; now < 100; now++)
			tick(now, FALSE, FALSE, FALSE);
		/* (a button changed in the ticks its input carries: each of them) */
		for (now = 100; now < 103; now++)
			CHECK(tick(now, TRUE, FALSE, FALSE), "tick %ld: a press in the input's ticks not sent", now);
		return 0;
	}
	if (!strcmp(name, "substeps"))
	{
		for (now = 0; now < 100; now++)
			tick(now, FALSE, FALSE, FALSE);
		/* (a tap inside a tick, off the plan's ticks: its sub-steps' record
		and input on the plan's next tick, and the input again on the one
		after, so that the client replays the record no longer than before) */
		CHECK(!tick(100, FALSE, TRUE, FALSE), "sub-steps sent off the plan's ticks");
		CHECK(tick(102, FALSE, TRUE, FALSE), "the input of sub-steps that changed not sent on the plan's tick");
		CHECK(tick(105, FALSE, FALSE, FALSE), "the input after a sub-steps' record not sent on the plan's next tick");
		return 0;
	}
	if (!strcmp(name, "covered"))
	{
		for (now = 0; now < 100; now++)
			tick(now, FALSE, FALSE, FALSE);
		/* (forwarded meanwhile, which the plan marks: relayed again on the
		first tick it is not, off the plan's ticks) */
		distributed_relayed_sent[0][0].covered = TRUE;
		CHECK(tick(104, FALSE, FALSE, FALSE), "the input not relayed as the forwards stopped");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
