/* royale_rules.h: Royale's rules as a state machine of plain data
(port/linux/game/royale_rules.c)

The phases, squads, player states, downs, revives, bleed-outs, eliminations,
placements and kills, by player slot (a player's absolute index), with no
game state of its own: royale.c feeds it the game's events and each tick's
revivers, and does what it says (a unit healed, a player sent to the gulag).
The host runs it; its clients take its state (royale.c's network state).
Only basic types (boolean, byte, short, long, real, NONE), so the asset-free
harness compiles it whole (tools/harness/tests/royale_rules.c). */
#ifndef HALO_ROYALE_RULES_H
#define HALO_ROYALE_RULES_H

#define ROYALE_RULES_MAXIMUM_SLOTS 128
#define ROYALE_RULES_MAXIMUM_SQUADS 64
#define ROYALE_RULES_SQUAD_SIZE 2

/* (the values of royale.h's enum royale_phase and enum royale_player_state) */
enum
{
	ROYALE_RULES_PHASE_LOBBY = 0,
	ROYALE_RULES_PHASE_INSERTION,
	ROYALE_RULES_PHASE_PLAY,
	ROYALE_RULES_PHASE_ENDGAME,
	ROYALE_RULES_PHASE_RESULTS,
	/* the results shown: the game ends */
	ROYALE_RULES_PHASE_OVER
};

enum
{
	ROYALE_RULES_NONE = 0xFF, /* not in the game */
	ROYALE_RULES_IN_PELICAN = 0,
	ROYALE_RULES_FALLING,
	ROYALE_RULES_ALIVE,
	ROYALE_RULES_DOWNED,
	ROYALE_RULES_GULAG,
	ROYALE_RULES_SPECTATING,
	ROYALE_RULES_ELIMINATED
};

/* a slot's gulag (Reclaimer) result */
enum
{
	ROYALE_RULES_RECLAIMER_NONE = 0, /* never there, or still there */
	ROYALE_RULES_RECLAIMER_WON,
	ROYALE_RULES_RECLAIMER_LOST
};

/* what the rules ask of royale.c, a slot's events from royale_rules_tick */
enum
{
	ROYALE_RULES_EVENT_REVIVED = 1, /* heal the unit; it stands */
	ROYALE_RULES_EVENT_BLED_OUT = 2 /* kill the unit (no killer); then call royale_rules_died */
};

struct royale_rules_settings
{
	long lobby_ticks;
	/* the longest INSERTION lasts: then everyone still aboard or falling is
	ALIVE where they are */
	long insertion_ticks;
	long bleed_out_ticks;
	long revive_ticks;
	long results_ticks;
	/* ENDGAME when this many squads or fewer are alive */
	short endgame_squads;
	/* INSERTION skipped (no insertion lane): everyone starts ALIVE */
	boolean skip_insertion;
};

struct royale_rules_slot
{
	byte state;
	byte squad; /* 0xFF: none */
	byte kills;
	/* the slot that downed them (their kill if they die downed), 0xFF none */
	byte downed_by;
	short downed_ticks; /* left to bleed out */
	short revive_ticks; /* of the revive in progress */
	byte reviver; /* 0xFF: none */
	/* how their time in the gulag (the Reclaimer) ended: ROYALE_RULES_RECLAIMER_* */
	byte reclaimer;
	byte reserved[2];
};

struct royale_rules
{
	byte phase;
	byte squad_count;
	byte squads_alive;
	byte winner; /* squad, 0xFF none */
	/* the clock (ticks): each machine advances it (royale_rules_tick on the
	host, royale_rules_advance on a client), so it never goes on the wire
	but as an occasional correction */
	long phase_time;
	long match_time;
	/* the match time the phase began at (phase_time is match_time less
	it), and the one PLAY began at (NONE before; ENDGAME may begin on the
	same tick) */
	long phase_start;
	long play_start;
	struct royale_rules_slot slots[ROYALE_RULES_MAXIMUM_SLOTS];
	/* each squad's place, 1 the winner, 0 still alive */
	byte placement[ROYALE_RULES_MAXIMUM_SQUADS];
};

/* the gulag's say on a slot that died for good (royale.c asks the gulag
lane): TRUE takes them to the gulag */
typedef boolean (*royale_rules_gulag_proc)(long slot, long killer_slot);

void royale_rules_reset(struct royale_rules *rules);
/* LOBBY: a slot in the game (or one that left) */
void royale_rules_join(struct royale_rules *rules, long slot);
void royale_rules_leave(struct royale_rules *rules, struct royale_rules_settings const *settings, long slot,
	royale_rules_gulag_proc gulag);
/* one tick: the phases' clock; downed slots bleed out or are revived
(reviver[slot]: the squadmate's slot holding the revive this tick, NONE
none); events[slot] gets each slot's ROYALE_RULES_EVENT_* (zeroed first).
seed draws the squads at LOBBY's end. */
void royale_rules_tick(struct royale_rules *rules, struct royale_rules_settings const *settings,
	long const *reviver, byte *events, unsigned long seed, royale_rules_gulag_proc gulag);
/* a lethal hit on a slot's unit: TRUE if it is absorbed (the slot is
downed now), FALSE if the unit dies */
boolean royale_rules_lethal(struct royale_rules *rules, struct royale_rules_settings const *settings, long slot,
	long attacker_slot);
/* a slot's unit died (killer_slot NONE: no one's): the gulag, else out */
void royale_rules_died(struct royale_rules *rules, struct royale_rules_settings const *settings, long slot,
	long killer_slot, royale_rules_gulag_proc gulag);
/* the host's rule for a lethal hit: an ALIVE slot whose squadmate stands
(and is not in the gulag) goes down instead of dying. A client's prediction
asks the same (royale.c) */
boolean royale_rules_can_be_downed(struct royale_rules const *rules, long slot);
/* a lane moves a slot (insertion, gulag). A slot out of the fight or
downed never comes back through a lane: only a revive stands the downed */
void royale_rules_set_state(struct royale_rules *rules, struct royale_rules_settings const *settings, long slot,
	byte state);
/* the slot's squadmate, NONE if none */
long royale_rules_squadmate(struct royale_rules const *rules, long slot);
/* the squad has a slot not out */
boolean royale_rules_squad_alive(struct royale_rules const *rules, short squad);
/* whether the slot is out of the fight for good */
boolean royale_rules_out(byte state);

/* ---- the network state (royale.c sends it after Slayer's)

Two parts: the discrete one (the phase and when it began, the squads, each
slot's state, kills, downer and reviver, the placements), which changes only
with events, so the host sends it when it changes; and the clock (the match
time, and each downed slot's bleed-out and revive), which every machine
advances itself, so it rides along as a correction outside the change check.
Integers little-endian whatever the machine's long. */

/* a client's tick: the clock advanced as the host's royale_rules_tick does
it, without its decisions (those come from the host) */
void royale_rules_advance(struct royale_rules *rules, struct royale_rules_settings const *settings);
/* the parts written: their size, 0 if size is too small */
long royale_rules_write_discrete(struct royale_rules const *rules, byte *buffer, long size);
long royale_rules_write_clock(struct royale_rules const *rules, byte *buffer, long size);
/* the parts read into rules (the clock after the discrete part, whose
downed slots it follows): the bytes read, NONE if they are not a valid
state */
long royale_rules_read_discrete(byte const *buffer, long size, struct royale_rules *rules);
long royale_rules_read_clock(byte const *buffer, long size, struct royale_rules *rules);
/* a client takes the host's state: its discrete part whole; its clock only
where the client's own is more than slack ticks off the host's projected by
ahead ticks (the message's age), or at once (first) */
void royale_rules_take(struct royale_rules *local, struct royale_rules const *host, long ahead, long slack,
	boolean first);

#endif
