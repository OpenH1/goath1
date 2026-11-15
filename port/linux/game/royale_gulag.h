/* royale_gulag.h: Royale's gulag (port/linux/game/royale_gulag.c)

A player's first death for good sends them to the gulag (once a match, while
the current phase has it open): a walled 1v1 arena in the map's own BSP.
Gulag players fight in pairs, in the order they arrived, with a pistol each;
the first kill wins. The winner redeploys (the insertion lane's
royale_insertion_redeploy); the loser is eliminated. A player left with no
opponent for ROYALE_GULAG_ALONE_SECONDS wins by default, and a fight that
lasts ROYALE_GULAG_FIGHT_SECONDS goes to the healthier fighter. Waiting
players watch the fight.

The lane's calls (start, tick, draw and the hooks) are in royale.h; the
queue below is the pure rule, deterministic and free of the engine, so the
harness tests it (tools/harness/tests/test_royale_gulag_queue.py). */
#ifndef HALO_ROYALE_GULAG_H
#define HALO_ROYALE_GULAG_H

#include "royale.h"

#define ROYALE_GULAG_ALONE_SECONDS 20.0f
#define ROYALE_GULAG_FIGHT_SECONDS 60.0f

enum royale_gulag_event_type
{
	ROYALE_GULAG_EVENT_NONE = 0,
	ROYALE_GULAG_EVENT_FIGHT, /* player[0] and player[1] go into the arena */
	ROYALE_GULAG_EVENT_WIN_BY_DEFAULT, /* player[0] had no opponent in time */
	ROYALE_GULAG_EVENT_TIME_UP, /* the fight of player[0] and player[1] ran out of time */
};

struct royale_gulag_event
{
	short type; /* enum royale_gulag_event_type */
	long player[2];
};

/* the gulag's line and arena: players are datum indices; used is by a
player's absolute index */
struct royale_gulag_queue
{
	long waiting[ROYALE_MAXIMUM_PLAYERS];
	short waiting_count;
	long fighter[2]; /* NONE, NONE when no fight */
	long fight_ticks;
	long alone_ticks;
	boolean used[ROYALE_MAXIMUM_PLAYERS];
};

void royale_gulag_queue_reset(struct royale_gulag_queue *queue);
/* a player's first death for good: TRUE (and in line) if the gulag is open
and they haven't used it */
boolean royale_gulag_queue_enter(struct royale_gulag_queue *queue, long player_index, boolean open);
/* once a tick: at most one event (a fight starts, a default win, time up) */
void royale_gulag_queue_step(struct royale_gulag_queue *queue, long alone_limit_ticks, long fight_limit_ticks,
	struct royale_gulag_event *event);
/* a fighter died: the fight is over and their opponent (returned) won;
NONE if they weren't fighting */
long royale_gulag_queue_fighter_died(struct royale_gulag_queue *queue, long player_index);
/* a player left the game: out of line, or out of the fight (their opponent,
returned, wins); NONE otherwise */
long royale_gulag_queue_leave(struct royale_gulag_queue *queue, long player_index);
/* the line and the fight for the host's state: players by absolute index, the
fight's start as a match time (now: the match time; the fight's ticks are now
less it); the bytes written, 0 if size is too small */
long royale_gulag_queue_write(struct royale_gulag_queue const *queue, long now, byte *buffer, long size);
/* a client's read of it: the bytes read, NONE if not valid; the players
from their absolute index (player_of_slot, NONE: not here yet, left out),
fight_start the fight's match time */
long royale_gulag_queue_read(byte const *buffer, long size, long (*player_of_slot)(long slot),
	struct royale_gulag_queue *queue, long *fight_start);
/* a fighter's opponent, NONE if not fighting; the fighter's side (0, 1) or
NONE; a waiting player's place in line (0 next) or NONE */
long royale_gulag_queue_opponent(struct royale_gulag_queue const *queue, long player_index);
short royale_gulag_queue_side(struct royale_gulag_queue const *queue, long player_index);
short royale_gulag_queue_place(struct royale_gulag_queue const *queue, long player_index);

#endif
