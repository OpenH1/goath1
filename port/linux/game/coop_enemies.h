/*
COOP_ENEMIES.H

Network co-op's extra enemies, for its players (coop_enemies.c).
*/

#ifndef __COOP_ENEMIES_H
#define __COOP_ENEMIES_H

/* network_coop.c: a game begins (its EXTRA ENEMIES, or a local campaign's
BIGGER BATTLES, read) */
void coop_enemies_new_game(
	void);
/* game_state.c: a checkpoint saved, and a game state loaded: what the
extra enemies keep outside it (those waiting, the dropships' riders kept,
which actors are extras) copied, and put back if the load is of the copy's
tick (else forgotten) */
void coop_enemies_before_save(
	void);
void coop_enemies_after_load(
	void);
/* game_state.c: a solo saved game's bigbattles.bin, beside savegame.bin:
written into `buffer` from the game as it is (its size, 0 if `size` is too
small), and read before its image loads (NULL: there is none; the loaded
game then plays at the original's size) */
long coop_enemies_saved_game(
	void *buffer,
	long size);
void coop_enemies_saved_game_load(
	void const *buffer,
	long size);
/* encounters.c, and this file's riders: an extra enemy's actor placed; and
whether an actor is one (ai_script.c's ai_command_list_status counts the
level's own alone) */
void coop_enemies_extra_placed(
	long actor_index);
boolean coop_enemies_actor_is_extra(
	long actor_index);
/* the host's extra enemies and choices, by part, for the demos (demo.c's G
records: network_coop_port_demo_state); NULL past the last */
void *coop_enemies_port_demo_state(
	short part,
	long *size);

/* encounters.c: how many enemies more a squad placed with `count` gets
(none but on a co-op host, for an encounter of the players' enemies) */
short coop_enemies_extra_count(
	long encounter_index,
	short count);

/* actors.c: how many times the level's enemies this game multiplies, as
extra enemies are placed (rounded up: 200% two, STATIC MULTIPLIER 4 four),
1 where it places none (the original's game, and a co-op client's), which
its actors' perception and path timeslices a tick are */
short coop_enemies_scale(
	void);
/* the places tried on the rings around a starting location */
#define COOP_ENEMIES_SPREAD_SPOTS 90

/* encounters.c: where a squad's `extra_number`th extra enemy (counting
from 1, its starting location `starting_location_index`) goes: the
starting location in `location`, its position moved to free ground around
it (or around the squad's others), else to the nearest free firing
position of the encounter, its facing turned a little. FALSE if there is
no room: the enemy is not placed. A squad's extra enemies are placed in
one tick, one after another. */
boolean coop_enemies_extra_location(
	long encounter_index,
	short squad_index,
	short starting_location_index,
	short extra_number,
	struct actor_starting_location *location);
/* units.c: a unit's idle animation begun (from `old_state`, its animation
state before). In network co-op with extra enemies, an AI's biped's
begins at a point of its own, but for one only turning on the spot (the
host's and the clients' copies alike), so that a squad, and the extra
enemies placed with it, don't breathe and turn in step. */
void coop_enemies_idle_started(
	long unit_index,
	short old_state);

/* units.c's vehicle_scripting_load_magic: a rider seated, and one left
without a seat (the vehicle's seats taken: an extra enemy), which is kept,
off the map, until the vehicle's riders get out (TRUE: kept, the caller
erases it) */
void coop_enemies_rider_seated(
	long vehicle_index,
	long unit_index);
boolean coop_enemies_rider_unseated(
	long vehicle_index,
	long unit_index);

/* network_coop.c, at each of the host's ticks' ends: the riders kept placed
at their vehicle's seats once its own riders get out */
void coop_enemies_update(
	void);

#endif
