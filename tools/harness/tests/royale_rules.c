/*
ROYALE_RULES.C (test)

The real Royale rules (port/linux/game/royale_rules.c): squads drawn at the
lobby's end, downs, revives, bleed-outs, the gulag, eliminations, placements,
kills and the phases to the results. test_royale_rules.py takes the code
(royale_rules.inc, its header and its body).
*/

#include "harness.h"

#include "royale_rules.inc"

static struct royale_rules_settings settings =
{
	10, /* lobby_ticks */
	100, /* insertion_ticks */
	30, /* bleed_out_ticks */
	5, /* revive_ticks */
	20, /* results_ticks */
	2, /* endgame_squads */
	TRUE, /* skip_insertion */
};

static struct royale_rules rules;
static long revivers[ROYALE_RULES_MAXIMUM_SLOTS];
static byte events[ROYALE_RULES_MAXIMUM_SLOTS];
static int gulag_takes;
static int gulag_calls;

static boolean gulag(long slot, long killer)
{
	(void)slot;
	(void)killer;
	gulag_calls++;
	if (gulag_takes > 0)
	{
		gulag_takes--;
		return TRUE;
	}
	return FALSE;
}

static void tick(void)
{
	royale_rules_tick(&rules, &settings, revivers, events, 1234, gulag);
}

/* a game of count players, past the lobby: in PLAY */
static void start(long count, unsigned long seed)
{
	long slot;

	royale_rules_reset(&rules);
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
		revivers[slot] = NONE;
	gulag_takes = 0;
	gulag_calls = 0;
	for (slot = 0; slot < count; slot++)
		royale_rules_join(&rules, slot);
	while (rules.phase == ROYALE_RULES_PHASE_LOBBY)
		royale_rules_tick(&rules, &settings, revivers, events, seed, gulag);
	tick();
	CHECK(rules.phase == ROYALE_RULES_PHASE_PLAY || rules.phase == ROYALE_RULES_PHASE_ENDGAME,
		"phase %d after the lobby", rules.phase);
}

/* a slot downed by attacker, its unit then killed by the rules' word */
static void bleed_out(long slot)
{
	int guard = 0;

	while (!(events[slot] & ROYALE_RULES_EVENT_BLED_OUT) && guard++ < 1000)
		tick();
	CHECK(events[slot] & ROYALE_RULES_EVENT_BLED_OUT, "slot %ld never bled out", slot);
	royale_rules_died(&rules, &settings, slot, NONE, gulag);
}

static long enemy_of(long slot)
{
	long other;

	for (other = 0; other < ROYALE_RULES_MAXIMUM_SLOTS; other++)
	{
		if (rules.slots[other].state != ROYALE_RULES_NONE && other != slot &&
			rules.slots[other].squad != rules.slots[slot].squad)
		{
			return other;
		}
	}
	return NONE;
}

static void case_squads(void)
{
	long slot;
	int count[ROYALE_RULES_MAXIMUM_SQUADS] = { 0 };
	byte first[6];

	start(6, 77);
	CHECK(rules.squad_count == 3, "%d squads of 6 players", rules.squad_count);
	CHECK(rules.squads_alive == 3, "%d squads alive", rules.squads_alive);
	for (slot = 0; slot < 6; slot++)
	{
		CHECK(rules.slots[slot].squad < 3, "slot %ld squad %d", slot, rules.slots[slot].squad);
		CHECK(rules.slots[slot].state == ROYALE_RULES_ALIVE, "slot %ld state %d", slot, rules.slots[slot].state);
		count[rules.slots[slot].squad]++;
		first[slot] = rules.slots[slot].squad;
	}
	CHECK(count[0] == 2 && count[1] == 2 && count[2] == 2, "squads of %d %d %d", count[0], count[1], count[2]);
	/* (the same seed, the same squads) */
	start(6, 77);
	for (slot = 0; slot < 6; slot++)
		CHECK(rules.slots[slot].squad == first[slot], "slot %ld drew another squad", slot);
}

static void case_revive(void)
{
	long a = 0;
	long mate;
	long enemy;
	int index;

	start(4, 5);
	mate = royale_rules_squadmate(&rules, a);
	enemy = enemy_of(a);
	CHECK(mate != NONE && enemy != NONE, "no squadmate or enemy");
	CHECK(royale_rules_lethal(&rules, &settings, a, enemy), "an ALIVE slot with a squadmate is not downed");
	CHECK(rules.slots[a].state == ROYALE_RULES_DOWNED, "state %d", rules.slots[a].state);
	/* (a downed slot hit again dies) */
	CHECK(!royale_rules_lethal(&rules, &settings, a, enemy), "a downed slot was downed again");
	/* (an enemy's hold revives nothing) */
	revivers[a] = enemy;
	for (index = 0; index < 10; index++)
		tick();
	CHECK(rules.slots[a].state == ROYALE_RULES_DOWNED, "an enemy revived them");
	/* (a revive let go starts over) */
	revivers[a] = mate;
	for (index = 0; index < (int)settings.revive_ticks - 1; index++)
		tick();
	revivers[a] = NONE;
	tick();
	revivers[a] = mate;
	for (index = 0; index < (int)settings.revive_ticks - 1; index++)
		tick();
	CHECK(rules.slots[a].state == ROYALE_RULES_DOWNED, "a revive let go went on");
	tick();
	CHECK(rules.slots[a].state == ROYALE_RULES_ALIVE, "not revived: state %d", rules.slots[a].state);
	CHECK(events[a] & ROYALE_RULES_EVENT_REVIVED, "no revive event");
}

static void case_bleed_out(void)
{
	long a = 0;
	long mate;
	long enemy;

	start(4, 5);
	mate = royale_rules_squadmate(&rules, a);
	enemy = enemy_of(a);
	CHECK(royale_rules_lethal(&rules, &settings, a, enemy), "not downed");
	bleed_out(a);
	CHECK(rules.slots[a].state == ROYALE_RULES_SPECTATING, "state %d with a squadmate in", rules.slots[a].state);
	CHECK(rules.slots[enemy].kills == 1, "the downer has %d kills", rules.slots[enemy].kills);
	/* (the last of a squad is not downed: they die) */
	CHECK(!royale_rules_lethal(&rules, &settings, mate, enemy), "the squad's last was downed");
	royale_rules_died(&rules, &settings, mate, enemy, gulag);
	CHECK(rules.slots[mate].state == ROYALE_RULES_ELIMINATED && rules.slots[a].state == ROYALE_RULES_ELIMINATED,
		"states %d %d with the squad out", rules.slots[a].state, rules.slots[mate].state);
	CHECK(rules.slots[enemy].kills == 2, "the killer has %d kills", rules.slots[enemy].kills);
	CHECK(rules.phase == ROYALE_RULES_PHASE_RESULTS, "phase %d with one squad left", rules.phase);
	CHECK(rules.winner == rules.slots[enemy].squad, "winner %d", rules.winner);
	CHECK(rules.placement[rules.slots[a].squad] == 2, "the squad out placed %d", rules.placement[rules.slots[a].squad]);
	CHECK(rules.placement[rules.winner] == 1, "the winner placed %d", rules.placement[rules.winner]);
}

static void case_gulag(void)
{
	long a = 0;
	long enemy;

	start(4, 9);
	enemy = enemy_of(a);
	gulag_takes = 1;
	CHECK(royale_rules_lethal(&rules, &settings, a, enemy), "not downed");
	bleed_out(a);
	CHECK(rules.slots[a].state == ROYALE_RULES_GULAG, "state %d after the gulag took them", rules.slots[a].state);
	CHECK(gulag_calls == 1, "the gulag was asked %d times", gulag_calls);
	/* (a death in the gulag: asked again, and out) */
	royale_rules_died(&rules, &settings, a, enemy, gulag);
	CHECK(gulag_calls == 2, "the gulag was asked %d times", gulag_calls);
	CHECK(rules.slots[a].state == ROYALE_RULES_SPECTATING, "state %d after losing the gulag", rules.slots[a].state);
	CHECK(rules.slots[a].reclaimer == ROYALE_RULES_RECLAIMER_LOST, "the Reclaimer lost reads %d",
		rules.slots[a].reclaimer);
	/* (a gulag winner back in) */
	royale_rules_reset(&rules);
	start(4, 9);
	gulag_takes = 1;
	royale_rules_died(&rules, &settings, a, enemy_of(a), gulag);
	CHECK(rules.slots[a].reclaimer == ROYALE_RULES_RECLAIMER_NONE, "the Reclaimer decided while in it");
	royale_rules_set_state(&rules, &settings, a, ROYALE_RULES_ALIVE);
	CHECK(rules.slots[a].state == ROYALE_RULES_ALIVE, "state %d after winning the gulag", rules.slots[a].state);
	CHECK(rules.slots[a].reclaimer == ROYALE_RULES_RECLAIMER_WON, "the Reclaimer won reads %d",
		rules.slots[a].reclaimer);
}

static void case_results(void)
{
	long slot;
	int index;

	start(6, 3);
	/* (one squad out: two left, the endgame) */
	for (slot = 0; slot < 6; slot++)
	{
		if (rules.slots[slot].squad == 0)
			royale_rules_died(&rules, &settings, slot, NONE, gulag);
	}
	CHECK(rules.phase == ROYALE_RULES_PHASE_ENDGAME, "phase %d with two squads left", rules.phase);
	CHECK(rules.placement[0] == 3, "the first out placed %d", rules.placement[0]);
	for (slot = 0; slot < 6; slot++)
	{
		if (rules.slots[slot].squad == 1)
			royale_rules_died(&rules, &settings, slot, NONE, gulag);
	}
	CHECK(rules.phase == ROYALE_RULES_PHASE_RESULTS && rules.winner == 2, "phase %d winner %d", rules.phase,
		rules.winner);
	for (index = 0; index < (int)settings.results_ticks; index++)
		tick();
	CHECK(rules.phase == ROYALE_RULES_PHASE_OVER, "phase %d after the results", rules.phase);
}

/* the host's rule for a down, and the lethal hit it decides */
static void case_downed_rule(void)
{
	static byte const mate_states[] = { ROYALE_RULES_ALIVE, ROYALE_RULES_FALLING, ROYALE_RULES_IN_PELICAN,
		ROYALE_RULES_GULAG, ROYALE_RULES_DOWNED, ROYALE_RULES_SPECTATING, ROYALE_RULES_ELIMINATED };
	static boolean const downs[] = { TRUE, TRUE, TRUE, FALSE, FALSE, FALSE, FALSE };
	long a = 0;
	long mate;
	unsigned index;

	start(6, 3);
	mate = royale_rules_squadmate(&rules, a);
	for (index = 0; index < sizeof(mate_states); index++)
	{
		struct royale_rules copy;

		rules.slots[mate].state = mate_states[index];
		copy = rules;
		CHECK(royale_rules_can_be_downed(&rules, a) == downs[index], "squadmate in state %d: downed %d",
			mate_states[index], royale_rules_can_be_downed(&rules, a));
		CHECK(royale_rules_lethal(&copy, &settings, a, enemy_of(a)) == downs[index],
			"the lethal hit disagrees with the rule (squadmate in state %d)", mate_states[index]);
	}
}

/* the lanes move only what they may: a landing never stands the downed nor
brings back one out */
static void case_lane_states(void)
{
	long a = 0;
	long other;

	start(6, 3);
	other = enemy_of(a);
	CHECK(royale_rules_lethal(&rules, &settings, a, other), "not downed");
	royale_rules_set_state(&rules, &settings, a, ROYALE_RULES_ALIVE);
	CHECK(rules.slots[a].state == ROYALE_RULES_DOWNED, "a lane stood a downed slot: state %d", rules.slots[a].state);
	royale_rules_set_state(&rules, &settings, a, ROYALE_RULES_FALLING);
	CHECK(rules.slots[a].state == ROYALE_RULES_DOWNED, "a lane made a downed slot fall: state %d", rules.slots[a].state);
	royale_rules_set_state(&rules, &settings, a, ROYALE_RULES_ELIMINATED);
	CHECK(rules.slots[a].state == ROYALE_RULES_SPECTATING, "state %d once out", rules.slots[a].state);
	royale_rules_set_state(&rules, &settings, a, ROYALE_RULES_ALIVE);
	CHECK(rules.slots[a].state == ROYALE_RULES_SPECTATING, "a lane brought one out back: state %d",
		rules.slots[a].state);
	/* (the falling land) */
	royale_rules_set_state(&rules, &settings, other, ROYALE_RULES_FALLING);
	royale_rules_set_state(&rules, &settings, other, ROYALE_RULES_ALIVE);
	CHECK(rules.slots[other].state == ROYALE_RULES_ALIVE, "a landing did not stand: state %d", rules.slots[other].state);
}

/* a client's clock, advanced alone, keeps step with the host's */
static void case_clock(void)
{
	struct royale_rules client;
	long a = 0;
	long mate;
	long play;
	long slot;
	int index;

	/* (two squads, the endgame's count: PLAY and ENDGAME on one tick) */
	start(4, 3);
	CHECK(rules.phase == ROYALE_RULES_PHASE_ENDGAME, "phase %d", rules.phase);
	CHECK(rules.play_start != NONE && rules.play_start == rules.phase_start, "PLAY began %ld, ENDGAME %ld",
		rules.play_start, rules.phase_start);

	start(6, 3);
	CHECK(rules.phase == ROYALE_RULES_PHASE_PLAY && rules.play_start == rules.phase_start, "phase %d", rules.phase);
	play = rules.play_start;
	mate = royale_rules_squadmate(&rules, a);
	CHECK(royale_rules_lethal(&rules, &settings, a, enemy_of(a)), "not downed");
	client = rules;
	for (index = 0; index < 10; index++)
	{
		tick();
		royale_rules_advance(&client, &settings);
	}
	CHECK(client.match_time == rules.match_time && client.phase_time == rules.phase_time,
		"client at %ld/%ld, host at %ld/%ld", client.match_time, client.phase_time, rules.match_time, rules.phase_time);
	CHECK(client.slots[a].downed_ticks == rules.slots[a].downed_ticks, "bleeding out: client %d host %d",
		client.slots[a].downed_ticks, rules.slots[a].downed_ticks);
	/* (a revive: the host's change, then each its own count) */
	revivers[a] = mate;
	tick();
	royale_rules_take(&client, &rules, 0, 0, FALSE);
	for (index = 0; index < (int)settings.revive_ticks - 2; index++)
	{
		tick();
		royale_rules_advance(&client, &settings);
	}
	CHECK(client.slots[a].revive_ticks == rules.slots[a].revive_ticks &&
		client.slots[a].downed_ticks == rules.slots[a].downed_ticks, "reviving: client %d/%d host %d/%d",
		client.slots[a].revive_ticks, client.slots[a].downed_ticks, rules.slots[a].revive_ticks,
		rules.slots[a].downed_ticks);
	revivers[a] = NONE;
	/* (later the endgame: PLAY's start stays) */
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules.slots[slot].squad != SLOT_NONE && rules.slots[slot].squad != rules.slots[a].squad &&
			rules.slots[slot].squad == rules.slots[enemy_of(a)].squad)
		{
			royale_rules_died(&rules, &settings, slot, NONE, gulag);
		}
	}
	CHECK(rules.phase == ROYALE_RULES_PHASE_ENDGAME, "phase %d", rules.phase);
	CHECK(rules.play_start == play && rules.phase_start == rules.match_time, "PLAY began %ld (was %ld), ENDGAME %ld",
		rules.play_start, play, rules.phase_start);
}

/* the network state: the discrete part still while only the clock goes on,
read back whole, and a broken one refused */
static void case_network(void)
{
	static byte first[4096];
	static byte second[4096];
	static byte clock[1024];
	struct royale_rules read;
	long size;
	long again;
	long clock_size;
	long slot;
	short squad;
	int index;

	start(6, 3);
	CHECK(royale_rules_lethal(&rules, &settings, 0, enemy_of(0)), "not downed");
	rules.slots[5].reclaimer = ROYALE_RULES_RECLAIMER_WON;
	size = royale_rules_write_discrete(&rules, first, sizeof(first));
	CHECK(size > 0 && size <= 13 + 7 * 6 + rules.squad_count, "the discrete part takes %ld bytes", size);
	for (index = 0; index < 5; index++)
		tick();
	again = royale_rules_write_discrete(&rules, second, sizeof(second));
	CHECK(again == size && !memcmp(first, second, (size_t)size), "the discrete part changed with the clock alone");
	clock_size = royale_rules_write_clock(&rules, clock, sizeof(clock));
	CHECK(clock_size == 4 + 4, "the clock takes %ld bytes with one downed", clock_size);
	CHECK(royale_rules_write_discrete(&rules, second, size - 1) == 0, "written past its room");
	CHECK(royale_rules_read_discrete(second, again, &read) == again, "the discrete part not read");
	CHECK(royale_rules_read_clock(clock, clock_size, &read) == clock_size, "the clock not read");
	CHECK(read.phase == rules.phase && read.squad_count == rules.squad_count &&
		read.squads_alive == rules.squads_alive && read.winner == rules.winner && read.match_time == rules.match_time &&
		read.phase_time == rules.phase_time && read.phase_start == rules.phase_start &&
		read.play_start == rules.play_start, "the head read otherwise");
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot const *a = &read.slots[slot];
		struct royale_rules_slot const *b = &rules.slots[slot];

		CHECK(a->state == b->state && a->squad == b->squad && a->kills == b->kills && a->downed_by == b->downed_by &&
			a->reviver == b->reviver && a->downed_ticks == b->downed_ticks && a->revive_ticks == b->revive_ticks &&
			a->reclaimer == b->reclaimer,
			"slot %ld read otherwise", slot);
	}
	for (squad = 0; squad < rules.squad_count; squad++)
		CHECK(read.placement[squad] == rules.placement[squad], "squad %d placed otherwise", squad);
	/* (refused: one short, a state that is none, a squad past the count) */
	CHECK(royale_rules_read_discrete(second, again - 1, &read) == NONE, "a short state read");
	second[13 + 1] = 9;
	CHECK(royale_rules_read_discrete(second, again, &read) == NONE, "a state 9 read");
	second[13 + 1] = ROYALE_RULES_ALIVE;
	second[13 + 2] = (byte)rules.squad_count;
	CHECK(royale_rules_read_discrete(second, again, &read) == NONE, "a squad past the count read");
}

/* a client takes the host's clock only when its own is off by more than the
slack (the message's age added) */
static void case_take(void)
{
	struct royale_rules host;
	struct royale_rules client;
	long a = 0;

	start(6, 3);
	CHECK(royale_rules_lethal(&rules, &settings, a, enemy_of(a)), "not downed");
	host = rules;
	client = rules;
	client.match_time = host.match_time + 2;
	royale_rules_take(&client, &host, 0, 4, FALSE);
	CHECK(client.match_time == host.match_time + 2, "a clock within the slack put right: %ld", client.match_time);
	client.match_time = host.match_time + 20;
	royale_rules_take(&client, &host, 0, 4, FALSE);
	CHECK(client.match_time == host.match_time, "a clock 20 ticks off kept: %ld", client.match_time);
	royale_rules_take(&client, &host, 3, 4, TRUE);
	CHECK(client.match_time == host.match_time + 3, "the first state's clock not taken: %ld", client.match_time);
	CHECK(client.phase_time == client.match_time - client.phase_start, "the phase's time %ld", client.phase_time);
	/* (a down's count: kept near, taken new) */
	client.slots[a].downed_ticks = (short)(host.slots[a].downed_ticks - 3 - 2);
	royale_rules_take(&client, &host, 3, 4, FALSE);
	CHECK(client.slots[a].downed_ticks == host.slots[a].downed_ticks - 3 - 2, "a near bleed-out put right");
	client.slots[a].state = ROYALE_RULES_ALIVE;
	client.slots[a].downed_ticks = 0;
	royale_rules_take(&client, &host, 3, 4, FALSE);
	CHECK(client.slots[a].downed_ticks == host.slots[a].downed_ticks - 3, "a new down's bleed-out %d, the host's %d",
		client.slots[a].downed_ticks, host.slots[a].downed_ticks);
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "squads"))
		case_squads();
	else if (!strcmp(name, "revive"))
		case_revive();
	else if (!strcmp(name, "bleed-out"))
		case_bleed_out();
	else if (!strcmp(name, "gulag"))
		case_gulag();
	else if (!strcmp(name, "results"))
		case_results();
	else if (!strcmp(name, "downed-rule"))
		case_downed_rule();
	else if (!strcmp(name, "lane-states"))
		case_lane_states();
	else if (!strcmp(name, "clock"))
		case_clock();
	else if (!strcmp(name, "network"))
		case_network();
	else if (!strcmp(name, "take"))
		case_take();
	else
		return 2;
	return 0;
}
