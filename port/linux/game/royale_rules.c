/*
ROYALE_RULES.C

Royale's rules as a state machine of plain data (royale_rules.h):

- LOBBY: players gather for settings->lobby_ticks; then the squads are drawn
  (the slots in the game shuffled with the game's seed, paired in order: a
  last odd one plays alone) and INSERTION begins with everyone IN_PELICAN
  (ALIVE at once without an insertion lane).
- INSERTION: the insertion lane moves slots to FALLING and ALIVE; PLAY once
  none is aboard or falling, or after settings->insertion_ticks (those left
  are ALIVE).
- PLAY, then ENDGAME once settings->endgame_squads or fewer are alive.
- RESULTS once one squad is left (or none: the last out wins), then OVER
  after settings->results_ticks.

A lethal hit downs an ALIVE slot (its unit kept, crawling) instead of
killing it; downed, it bleeds out over settings->bleed_out_ticks unless a
squadmate holds a revive for settings->revive_ticks. A downed slot whose
squad has no one left standing bleeds out at once. A slot that dies (downed,
in the gulag, or killed outright) goes to the gulag if the gulag lane takes
it, else it is out: SPECTATING while a squadmate is in the fight,
ELIMINATED once none is. A squad with no one in the fight is out and placed:
its place is the number of squads alive before it went out.

The kill is the killer's, or for a downed slot that bleeds out or dies to no
one, the one who downed it. Nothing here reads a clock or a random of the
game's: the same events give the same state on every run.
*/

#include "cseries.h"
#include "royale_rules.h"

#define SLOT_NONE 0xFF

static boolean royale_rules_valid_slot(
	long slot)
{
	return slot >= 0 && slot < ROYALE_RULES_MAXIMUM_SLOTS;
}

boolean royale_rules_out(
	byte state)
{
	return state == ROYALE_RULES_SPECTATING || state == ROYALE_RULES_ELIMINATED || state == ROYALE_RULES_NONE;
}

/* in the fight and on their feet (a squadmate who can revive, or keeps the
squad's downed alive) */
static boolean royale_rules_standing(
	byte state)
{
	return state == ROYALE_RULES_IN_PELICAN || state == ROYALE_RULES_FALLING || state == ROYALE_RULES_ALIVE ||
		state == ROYALE_RULES_GULAG;
}

void royale_rules_reset(
	struct royale_rules *rules)
{
	long slot;
	short squad;

	rules->phase = ROYALE_RULES_PHASE_LOBBY;
	rules->squad_count = 0;
	rules->squads_alive = 0;
	rules->winner = SLOT_NONE;
	rules->phase_time = 0;
	rules->match_time = 0;
	rules->phase_start = 0;
	rules->play_start = NONE;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot *entry = &rules->slots[slot];

		entry->state = ROYALE_RULES_NONE;
		entry->squad = SLOT_NONE;
		entry->kills = 0;
		entry->downed_by = SLOT_NONE;
		entry->downed_ticks = 0;
		entry->revive_ticks = 0;
		entry->reviver = SLOT_NONE;
		entry->reclaimer = ROYALE_RULES_RECLAIMER_NONE;
		entry->reserved[0] = entry->reserved[1] = 0;
	}
	for (squad = 0; squad < ROYALE_RULES_MAXIMUM_SQUADS; squad++)
		rules->placement[squad] = 0;
}

long royale_rules_squadmate(
	struct royale_rules const *rules,
	long slot)
{
	long other;
	byte squad;

	if (!royale_rules_valid_slot(slot) || rules->slots[slot].squad == SLOT_NONE)
		return NONE;
	squad = rules->slots[slot].squad;
	for (other = 0; other < ROYALE_RULES_MAXIMUM_SLOTS; other++)
	{
		if (other != slot && rules->slots[other].squad == squad)
			return other;
	}
	return NONE;
}

boolean royale_rules_squad_alive(
	struct royale_rules const *rules,
	short squad)
{
	long slot;

	if (squad < 0 || squad >= ROYALE_RULES_MAXIMUM_SQUADS)
		return FALSE;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].squad == squad && !royale_rules_out(rules->slots[slot].state))
			return TRUE;
	}
	return FALSE;
}

void royale_rules_join(
	struct royale_rules *rules,
	long slot)
{
	if (!royale_rules_valid_slot(slot) || rules->phase != ROYALE_RULES_PHASE_LOBBY)
		return;
	if (rules->slots[slot].state == ROYALE_RULES_NONE)
		rules->slots[slot].state = ROYALE_RULES_IN_PELICAN;
}

static void royale_rules_set_phase(
	struct royale_rules *rules,
	byte phase)
{
	rules->phase = phase;
	rules->phase_time = 0;
	rules->phase_start = rules->match_time;
	if ((phase == ROYALE_RULES_PHASE_PLAY || phase == ROYALE_RULES_PHASE_ENDGAME) && rules->play_start == NONE)
		rules->play_start = rules->match_time;
}

/* a squad out of the fight is placed; one squad left (or none) wins */
static void royale_rules_count_squads(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings)
{
	short squad;
	byte alive = 0;
	byte last_alive = SLOT_NONE;

	if (rules->phase == ROYALE_RULES_PHASE_LOBBY)
		return;
	/* (each squad newly out takes the place of the squads alive before it;
	two out on the same tick share it) */
	for (squad = 0; squad < rules->squad_count; squad++)
	{
		if (royale_rules_squad_alive(rules, squad))
		{
			alive++;
			last_alive = (byte)squad;
		}
	}
	for (squad = 0; squad < rules->squad_count; squad++)
	{
		if (!rules->placement[squad] && !royale_rules_squad_alive(rules, squad))
			rules->placement[squad] = (byte)(alive + 1);
	}
	rules->squads_alive = alive;
	if (rules->phase >= ROYALE_RULES_PHASE_RESULTS)
		return;
	if (alive <= 1)
	{
		if (alive == 1)
		{
			rules->winner = last_alive;
			rules->placement[last_alive] = 1;
		}
		else
		{
			/* (the last out: the squads placed 1, if any) */
			for (squad = 0; squad < rules->squad_count; squad++)
			{
				if (rules->placement[squad] == 1)
				{
					rules->winner = (byte)squad;
					break;
				}
			}
		}
		/* (a game of one squad goes on: it has no one to beat) */
		if (rules->squad_count > 1)
			royale_rules_set_phase(rules, ROYALE_RULES_PHASE_RESULTS);
	}
	else if (rules->phase == ROYALE_RULES_PHASE_PLAY && alive <= settings->endgame_squads)
		royale_rules_set_phase(rules, ROYALE_RULES_PHASE_ENDGAME);
}

/* a slot out: watching a squadmate in the fight, else eliminated; and its
squadmate out with no one left to watch eliminated too */
static void royale_rules_eliminate(
	struct royale_rules *rules,
	long slot)
{
	long mate = royale_rules_squadmate(rules, slot);
	boolean mate_in = mate != NONE && !royale_rules_out(rules->slots[mate].state);

	rules->slots[slot].state = mate_in ? ROYALE_RULES_SPECTATING : ROYALE_RULES_ELIMINATED;
	rules->slots[slot].downed_ticks = 0;
	rules->slots[slot].revive_ticks = 0;
	rules->slots[slot].reviver = SLOT_NONE;
	if (mate != NONE && rules->slots[mate].state == ROYALE_RULES_SPECTATING)
		rules->slots[mate].state = ROYALE_RULES_ELIMINATED;
}

void royale_rules_died(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings,
	long slot,
	long killer_slot,
	royale_rules_gulag_proc gulag)
{
	struct royale_rules_slot *entry;
	long credit;
	boolean in_gulag;

	if (!royale_rules_valid_slot(slot))
		return;
	entry = &rules->slots[slot];
	/* (the lobby's deaths count for nothing: its players respawn) */
	if (royale_rules_out(entry->state) || rules->phase == ROYALE_RULES_PHASE_LOBBY ||
		rules->phase >= ROYALE_RULES_PHASE_RESULTS)
	{
		return;
	}
	/* (the kill: the killer's, else the one who downed them; never a
	squadmate's or their own) */
	credit = royale_rules_valid_slot(killer_slot) && killer_slot != slot ? killer_slot :
		entry->state == ROYALE_RULES_DOWNED && entry->downed_by != SLOT_NONE ? entry->downed_by : NONE;
	if (credit != NONE && rules->slots[credit].squad != entry->squad && rules->slots[credit].kills < 0xFF)
		rules->slots[credit].kills++;
	entry->downed_by = SLOT_NONE;
	entry->downed_ticks = 0;
	entry->revive_ticks = 0;
	entry->reviver = SLOT_NONE;
	/* (the gulag hears of every death, one in the gulag's too: its loser is
	out) */
	in_gulag = entry->state == ROYALE_RULES_GULAG;
	if (in_gulag)
		entry->reclaimer = ROYALE_RULES_RECLAIMER_LOST;
	if (gulag && gulag(slot, killer_slot) && !in_gulag)
		entry->state = ROYALE_RULES_GULAG;
	else
		royale_rules_eliminate(rules, slot);
	royale_rules_count_squads(rules, settings);
}

void royale_rules_leave(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings,
	long slot,
	royale_rules_gulag_proc gulag)
{
	(void)gulag;
	if (!royale_rules_valid_slot(slot) || royale_rules_out(rules->slots[slot].state))
		return;
	if (rules->phase == ROYALE_RULES_PHASE_LOBBY)
	{
		rules->slots[slot].state = ROYALE_RULES_NONE;
		return;
	}
	/* (no gulag for one who left; one who left it lost it) */
	if (rules->slots[slot].state == ROYALE_RULES_GULAG)
		rules->slots[slot].reclaimer = ROYALE_RULES_RECLAIMER_LOST;
	royale_rules_eliminate(rules, slot);
	rules->slots[slot].state = ROYALE_RULES_ELIMINATED;
	royale_rules_count_squads(rules, settings);
}

boolean royale_rules_can_be_downed(
	struct royale_rules const *rules,
	long slot)
{
	long mate;

	if (!royale_rules_valid_slot(slot) || rules->phase >= ROYALE_RULES_PHASE_RESULTS ||
		rules->slots[slot].state != ROYALE_RULES_ALIVE)
	{
		return FALSE;
	}
	/* (no squadmate standing to revive them: they die) */
	mate = royale_rules_squadmate(rules, slot);
	return mate != NONE && royale_rules_standing(rules->slots[mate].state) &&
		rules->slots[mate].state != ROYALE_RULES_GULAG;
}

boolean royale_rules_lethal(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings,
	long slot,
	long attacker_slot)
{
	struct royale_rules_slot *entry;

	if (!royale_rules_can_be_downed(rules, slot))
		return FALSE;
	entry = &rules->slots[slot];
	entry->state = ROYALE_RULES_DOWNED;
	entry->downed_by = royale_rules_valid_slot(attacker_slot) && attacker_slot != slot ? (byte)attacker_slot :
		SLOT_NONE;
	entry->downed_ticks = (short)settings->bleed_out_ticks;
	entry->revive_ticks = 0;
	entry->reviver = SLOT_NONE;
	return TRUE;
}

void royale_rules_set_state(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings,
	long slot,
	byte state)
{
	if (!royale_rules_valid_slot(slot) || rules->slots[slot].state == ROYALE_RULES_NONE)
		return;
	/* (a lane never brings back one out, nor stands one downed: a landing
	after a down in the air, a stale win) */
	if (state != ROYALE_RULES_SPECTATING && state != ROYALE_RULES_ELIMINATED &&
		(royale_rules_out(rules->slots[slot].state) || rules->slots[slot].state == ROYALE_RULES_DOWNED))
	{
		return;
	}
	/* (out of the gulag: back in the fight won it, out of the game lost it) */
	if (rules->slots[slot].state == ROYALE_RULES_GULAG && state != ROYALE_RULES_GULAG)
	{
		rules->slots[slot].reclaimer = state == ROYALE_RULES_SPECTATING || state == ROYALE_RULES_ELIMINATED ?
			ROYALE_RULES_RECLAIMER_LOST : ROYALE_RULES_RECLAIMER_WON;
	}
	if (state == ROYALE_RULES_SPECTATING || state == ROYALE_RULES_ELIMINATED)
	{
		if (!royale_rules_out(rules->slots[slot].state))
			royale_rules_eliminate(rules, slot);
	}
	else
	{
		rules->slots[slot].state = state;
		if (state != ROYALE_RULES_DOWNED)
		{
			rules->slots[slot].downed_ticks = 0;
			rules->slots[slot].revive_ticks = 0;
			rules->slots[slot].reviver = SLOT_NONE;
		}
	}
	royale_rules_count_squads(rules, settings);
}

/* the squads drawn: the slots in the game shuffled (a xorshift of the
seed), paired in order */
static void royale_rules_draw_squads(
	struct royale_rules *rules,
	unsigned long seed)
{
	byte order[ROYALE_RULES_MAXIMUM_SLOTS];
	long count = 0;
	long slot;
	long index;
	unsigned long state = seed ? seed : 0x9E3779B9UL;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state != ROYALE_RULES_NONE)
			order[count++] = (byte)slot;
	}
	for (index = count - 1; index > 0; index--)
	{
		long other;
		byte swap;

		state ^= (state << 13) & 0xFFFFFFFFUL;
		state ^= state >> 17;
		state ^= (state << 5) & 0xFFFFFFFFUL;
		other = (long)(state % (unsigned long)(index + 1));
		swap = order[index];
		order[index] = order[other];
		order[other] = swap;
	}
	rules->squad_count = 0;
	for (index = 0; index < count && index / ROYALE_RULES_SQUAD_SIZE < ROYALE_RULES_MAXIMUM_SQUADS; index++)
	{
		rules->slots[order[index]].squad = (byte)(index / ROYALE_RULES_SQUAD_SIZE);
		rules->squad_count = (byte)(index / ROYALE_RULES_SQUAD_SIZE + 1);
	}
	/* (past the squads' count: no squad, out) */
	for (; index < count; index++)
		rules->slots[order[index]].state = ROYALE_RULES_ELIMINATED;
}

void royale_rules_tick(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings,
	long const *reviver,
	byte *events,
	unsigned long seed,
	royale_rules_gulag_proc gulag)
{
	long slot;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
		events[slot] = 0;
	rules->phase_time++;
	rules->match_time++;
	switch (rules->phase)
	{
	case ROYALE_RULES_PHASE_LOBBY:
		if (rules->phase_time >= settings->lobby_ticks)
		{
			royale_rules_draw_squads(rules, seed);
			royale_rules_set_phase(rules, ROYALE_RULES_PHASE_INSERTION);
			for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
			{
				if (rules->slots[slot].state == ROYALE_RULES_IN_PELICAN && settings->skip_insertion)
					rules->slots[slot].state = ROYALE_RULES_ALIVE;
			}
			royale_rules_count_squads(rules, settings);
		}
		return;
	case ROYALE_RULES_PHASE_INSERTION:
	{
		boolean aboard = FALSE;

		for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
		{
			byte state = rules->slots[slot].state;

			if (state == ROYALE_RULES_IN_PELICAN || state == ROYALE_RULES_FALLING)
				aboard = TRUE;
		}
		if (!aboard || rules->phase_time >= settings->insertion_ticks)
		{
			for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
			{
				byte state = rules->slots[slot].state;

				if (state == ROYALE_RULES_IN_PELICAN || state == ROYALE_RULES_FALLING)
					rules->slots[slot].state = ROYALE_RULES_ALIVE;
			}
			royale_rules_set_phase(rules, ROYALE_RULES_PHASE_PLAY);
			royale_rules_count_squads(rules, settings);
		}
		break;
	}
	case ROYALE_RULES_PHASE_RESULTS:
		if (rules->phase_time >= settings->results_ticks)
			royale_rules_set_phase(rules, ROYALE_RULES_PHASE_OVER);
		return;
	case ROYALE_RULES_PHASE_OVER:
		return;
	}

	/* the downed: revived, or bleeding out */
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot *entry = &rules->slots[slot];
		long mate;
		long by;

		if (entry->state != ROYALE_RULES_DOWNED)
			continue;
		mate = royale_rules_squadmate(rules, slot);
		by = reviver ? reviver[slot] : NONE;
		/* (only a squadmate on their feet revives) */
		if (by != NONE && (by != mate || rules->slots[by].state != ROYALE_RULES_ALIVE))
			by = NONE;
		if (by != NONE)
		{
			if (entry->reviver != (byte)by)
				entry->revive_ticks = 0;
			entry->reviver = (byte)by;
			entry->revive_ticks++;
			if (entry->revive_ticks >= settings->revive_ticks)
			{
				entry->state = ROYALE_RULES_ALIVE;
				entry->downed_by = SLOT_NONE;
				entry->downed_ticks = 0;
				entry->revive_ticks = 0;
				entry->reviver = SLOT_NONE;
				events[slot] |= ROYALE_RULES_EVENT_REVIVED;
			}
			/* (bleeding pauses while a revive goes on) */
			continue;
		}
		entry->reviver = SLOT_NONE;
		if (entry->downed_ticks > 0)
			entry->downed_ticks--;
		/* (no one standing to revive them: out at once) */
		if (entry->downed_ticks <= 0 || mate == NONE || !royale_rules_standing(rules->slots[mate].state) ||
			rules->slots[mate].state == ROYALE_RULES_GULAG)
		{
			events[slot] |= ROYALE_RULES_EVENT_BLED_OUT;
		}
	}
	/* (a slot bled out stays DOWNED until royale.c kills its unit and
	royale_rules_died hears of it: its unit's death is the one death) */
	(void)gulag;
	royale_rules_count_squads(rules, settings);
}

/* ---- the network state */

/* a discrete part's slot: slot, state, squad, kills, downed_by, reviver,
reclaimer */
#define ROYALE_RULES_SLOT_BYTES 7
/* the discrete part's head: phase, squad_count, squads_alive, winner,
phase_start, play_start, the slots' count */
#define ROYALE_RULES_HEAD_BYTES 13

static byte *royale_rules_put_long(
	byte *cursor,
	long value)
{
	unsigned long bits = (unsigned long)value;

	cursor[0] = (byte)(bits & 0xFF);
	cursor[1] = (byte)((bits >> 8) & 0xFF);
	cursor[2] = (byte)((bits >> 16) & 0xFF);
	cursor[3] = (byte)((bits >> 24) & 0xFF);
	return cursor + 4;
}

static byte const *royale_rules_get_long(
	byte const *cursor,
	long *value)
{
	unsigned long bits = (unsigned long)cursor[0] | ((unsigned long)cursor[1] << 8) |
		((unsigned long)cursor[2] << 16) | ((unsigned long)cursor[3] << 24);

	/* (sign extended where a long is wider than 32 bits) */
	*value = (bits & 0x80000000UL) ? -(long)(~bits & 0x7FFFFFFFUL) - 1 : (long)bits;
	return cursor + 4;
}

static byte *royale_rules_put_short(
	byte *cursor,
	short value)
{
	unsigned short bits = (unsigned short)value;

	cursor[0] = (byte)(bits & 0xFF);
	cursor[1] = (byte)((bits >> 8) & 0xFF);
	return cursor + 2;
}

static byte const *royale_rules_get_short(
	byte const *cursor,
	short *value)
{
	long bits = (long)cursor[0] | ((long)cursor[1] << 8);

	*value = (short)(bits >= 0x8000 ? bits - 0x10000 : bits);
	return cursor + 2;
}

void royale_rules_advance(
	struct royale_rules *rules,
	struct royale_rules_settings const *settings)
{
	long slot;

	rules->match_time++;
	rules->phase_time = rules->match_time - rules->phase_start;
	/* (what comes of a revive or a bleed-out is the host's word) */
	if (rules->phase >= ROYALE_RULES_PHASE_RESULTS)
		return;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot *entry = &rules->slots[slot];

		if (entry->state != ROYALE_RULES_DOWNED)
			continue;
		if (entry->reviver != SLOT_NONE)
		{
			if (entry->revive_ticks < settings->revive_ticks)
				entry->revive_ticks++;
		}
		else if (entry->downed_ticks > 0)
			entry->downed_ticks--;
	}
}

long royale_rules_write_discrete(
	struct royale_rules const *rules,
	byte *buffer,
	long size)
{
	byte *cursor = buffer;
	long count = 0;
	long slot;
	short squad;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state != ROYALE_RULES_NONE)
			count++;
	}
	if (size < ROYALE_RULES_HEAD_BYTES + count * ROYALE_RULES_SLOT_BYTES + rules->squad_count)
		return 0;
	*cursor++ = rules->phase;
	*cursor++ = rules->squad_count;
	*cursor++ = rules->squads_alive;
	*cursor++ = rules->winner;
	cursor = royale_rules_put_long(cursor, rules->phase_start);
	cursor = royale_rules_put_long(cursor, rules->play_start);
	*cursor++ = (byte)count;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot const *entry = &rules->slots[slot];

		if (entry->state == ROYALE_RULES_NONE)
			continue;
		*cursor++ = (byte)slot;
		*cursor++ = entry->state;
		*cursor++ = entry->squad;
		*cursor++ = entry->kills;
		*cursor++ = entry->downed_by;
		*cursor++ = entry->reviver;
		*cursor++ = entry->reclaimer;
	}
	for (squad = 0; squad < rules->squad_count; squad++)
		*cursor++ = rules->placement[squad];
	return (long)(cursor - buffer);
}

long royale_rules_write_clock(
	struct royale_rules const *rules,
	byte *buffer,
	long size)
{
	byte *cursor = buffer;
	long count = 0;
	long slot;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state == ROYALE_RULES_DOWNED)
			count++;
	}
	if (size < 4 + count * 4)
		return 0;
	cursor = royale_rules_put_long(cursor, rules->match_time);
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state != ROYALE_RULES_DOWNED)
			continue;
		cursor = royale_rules_put_short(cursor, rules->slots[slot].downed_ticks);
		cursor = royale_rules_put_short(cursor, rules->slots[slot].revive_ticks);
	}
	return (long)(cursor - buffer);
}

/* a slot's byte: a slot, or none */
static boolean royale_rules_valid_slot_byte(
	byte value)
{
	return value == SLOT_NONE || value < ROYALE_RULES_MAXIMUM_SLOTS;
}

long royale_rules_read_discrete(
	byte const *buffer,
	long size,
	struct royale_rules *rules)
{
	byte const *cursor = buffer;
	long count;
	long index;
	short squad;

	if (size < ROYALE_RULES_HEAD_BYTES)
		return NONE;
	royale_rules_reset(rules);
	rules->phase = *cursor++;
	rules->squad_count = *cursor++;
	rules->squads_alive = *cursor++;
	rules->winner = *cursor++;
	cursor = royale_rules_get_long(cursor, &rules->phase_start);
	cursor = royale_rules_get_long(cursor, &rules->play_start);
	count = *cursor++;
	if (rules->phase > ROYALE_RULES_PHASE_OVER || rules->squad_count > ROYALE_RULES_MAXIMUM_SQUADS ||
		rules->squads_alive > rules->squad_count ||
		(rules->winner != SLOT_NONE && rules->winner >= rules->squad_count) || count > ROYALE_RULES_MAXIMUM_SLOTS ||
		rules->phase_start < 0 || rules->play_start < NONE ||
		size < ROYALE_RULES_HEAD_BYTES + count * ROYALE_RULES_SLOT_BYTES + rules->squad_count)
	{
		return NONE;
	}
	for (index = 0; index < count; index++)
	{
		byte slot = *cursor++;
		struct royale_rules_slot *entry;

		if (slot >= ROYALE_RULES_MAXIMUM_SLOTS || rules->slots[slot].state != ROYALE_RULES_NONE)
			return NONE;
		entry = &rules->slots[slot];
		entry->state = *cursor++;
		entry->squad = *cursor++;
		entry->kills = *cursor++;
		entry->downed_by = *cursor++;
		entry->reviver = *cursor++;
		entry->reclaimer = *cursor++;
		if (entry->state > ROYALE_RULES_ELIMINATED || entry->reclaimer > ROYALE_RULES_RECLAIMER_LOST || (entry->squad != SLOT_NONE && entry->squad >= rules->squad_count) ||
			!royale_rules_valid_slot_byte(entry->downed_by) || !royale_rules_valid_slot_byte(entry->reviver))
		{
			return NONE;
		}
	}
	for (squad = 0; squad < rules->squad_count; squad++)
	{
		rules->placement[squad] = *cursor++;
		if (rules->placement[squad] > rules->squad_count)
			return NONE;
	}
	return (long)(cursor - buffer);
}

long royale_rules_read_clock(
	byte const *buffer,
	long size,
	struct royale_rules *rules)
{
	byte const *cursor = buffer;
	long count = 0;
	long slot;

	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state == ROYALE_RULES_DOWNED)
			count++;
	}
	if (size < 4 + count * 4)
		return NONE;
	cursor = royale_rules_get_long(cursor, &rules->match_time);
	if (rules->match_time < rules->phase_start || rules->match_time < rules->play_start)
		return NONE;
	rules->phase_time = rules->match_time - rules->phase_start;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		if (rules->slots[slot].state != ROYALE_RULES_DOWNED)
			continue;
		cursor = royale_rules_get_short(cursor, &rules->slots[slot].downed_ticks);
		cursor = royale_rules_get_short(cursor, &rules->slots[slot].revive_ticks);
	}
	return (long)(cursor - buffer);
}

/* a client's count: its own, unless more than slack off the host's (or
snapped to it) */
static long royale_rules_corrected(
	long local,
	long host,
	long slack,
	boolean snap)
{
	return snap || local < host - slack || local > host + slack ? host : local;
}

void royale_rules_take(
	struct royale_rules *local,
	struct royale_rules const *host,
	long ahead,
	long slack,
	boolean first)
{
	struct royale_rules previous = *local;
	long slot;

	if (ahead < 0)
		ahead = 0;
	*local = *host;
	local->match_time = royale_rules_corrected(previous.match_time, host->match_time + ahead, slack, first);
	/* (the phase's start is the host's: its time follows the clock) */
	if (local->match_time < local->phase_start)
		local->match_time = local->phase_start;
	local->phase_time = local->match_time - local->phase_start;
	for (slot = 0; slot < ROYALE_RULES_MAXIMUM_SLOTS; slot++)
	{
		struct royale_rules_slot const *was = &previous.slots[slot];
		struct royale_rules_slot *entry = &local->slots[slot];
		/* (a new down, or a revive begun or let go: the host's counts) */
		boolean snap = first || was->state != ROYALE_RULES_DOWNED || was->reviver != entry->reviver;
		long downed = entry->downed_ticks;
		long revive = entry->revive_ticks;

		if (entry->state != ROYALE_RULES_DOWNED)
			continue;
		if (entry->reviver != SLOT_NONE)
			revive += ahead;
		else
			downed = downed > ahead ? downed - ahead : 0;
		entry->downed_ticks = (short)royale_rules_corrected(was->downed_ticks, downed, slack, snap);
		entry->revive_ticks = (short)royale_rules_corrected(was->revive_ticks, revive, slack, snap);
	}
}
