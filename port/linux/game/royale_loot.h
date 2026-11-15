/* royale_loot.h: Royale's loot (port/linux/game/royale_loot.c)

Core calls royale_loot_start, royale_loot_tick and royale_loot_postspawn
(royale.h: the start kit); royale_loot_item_tier is the bots'. The plan (what lies at each
loot spot, and each supply pod's time, spot and contents) is a pure function
of the match's seed and the map config, so every machine draws the same. */
#ifndef HALO_ROYALE_LOOT_H
#define HALO_ROYALE_LOOT_H

#include "royale.h"

/* the kinds of loot, by rarity */
enum royale_loot_rarity
{
	ROYALE_LOOT_RARITY_COMMON = 0, /* the ground's guns and grenades */
	ROYALE_LOOT_RARITY_RARE, /* the power weapons */
	ROYALE_LOOT_RARITY_POWERUP, /* camouflage, overshield, health */
	NUMBER_OF_ROYALE_LOOT_RARITIES
};

enum royale_loot_kind
{
	ROYALE_LOOT_ASSAULT_RIFLE = 0,
	ROYALE_LOOT_PISTOL,
	ROYALE_LOOT_SHOTGUN,
	ROYALE_LOOT_PLASMA_PISTOL,
	ROYALE_LOOT_PLASMA_RIFLE,
	ROYALE_LOOT_NEEDLER,
	ROYALE_LOOT_FRAG_GRENADES,
	ROYALE_LOOT_PLASMA_GRENADES,
	ROYALE_LOOT_SNIPER_RIFLE,
	ROYALE_LOOT_ROCKET_LAUNCHER,
	ROYALE_LOOT_FUEL_ROD,
	ROYALE_LOOT_CAMOUFLAGE,
	ROYALE_LOOT_OVERSHIELD,
	ROYALE_LOOT_HEALTH_PACK,
	NUMBER_OF_ROYALE_LOOT_KINDS,
	ROYALE_LOOT_KIND_NONE = 0xFF
};

#define ROYALE_LOOT_POD_COUNT 3
#define ROYALE_LOOT_POD_ITEMS 3

struct royale_loot_pod_plan
{
	short spot; /* the config's pod spot */
	short drop_second; /* seconds into PLAY it starts to fall */
	byte kinds[ROYALE_LOOT_POD_ITEMS];
};

struct royale_loot_plan
{
	short spot_count;
	byte spot_kinds[ROYALE_MAXIMUM_LOOT_SPOTS]; /* ROYALE_LOOT_KIND_NONE: empty */
	short pod_count;
	struct royale_loot_pod_plan pods[ROYALE_LOOT_POD_COUNT];
};

/* the plan from the seed, the spots' tiers (enum royale_loot_tier) and the
kinds the map has (a flag per enum royale_loot_kind) */
void royale_loot_plan_make(unsigned long seed, short const *spot_tiers, short spot_count, short pod_spot_count,
	unsigned long available, struct royale_loot_plan *plan);
/* the plan this game drew (royale_loot_start) */
struct royale_loot_plan const *royale_loot_plan_get(void);

/* ---------- the rules of the game's side (pure: tools/harness/tests/test_royale_loot.py) */

/* the match time a pod starts to fall: drop_ticks into PLAY, or ENDGAME's
start if that comes first (a pod still to fall falls then); NONE before PLAY.
From the match clock, so every machine's pod falls alike */
long royale_loot_pod_fall_start(long play_start, long endgame_start, long drop_ticks);
/* a pod's fall: this long, from this high over its spot, and its height
over the spot at a share 0..1 of the fall (it brakes near the ground, so
that it stays in view: the crate and royale_fx.c's fire alike) */
#define ROYALE_LOOT_POD_FALL_SECONDS 6.0f
#define ROYALE_LOOT_POD_FALL_HEIGHT 120.0f
real royale_loot_pod_height(real fraction);

enum royale_loot_kit_action
{
	ROYALE_LOOT_KIT_LEAVE = 0, /* its weapons are its own */
	ROYALE_LOOT_KIT_GIVE, /* the start kit: its weapons gone, a pistol */
	ROYALE_LOOT_KIT_KEEP /* its weapons are its own from now (the unit taken as kitted) */
};

/* the host's look at a player's unit each tick: phase and state (royale.h's
enums), whether this unit had the kit (or was kept), whether it holds a
weapon. The start kit goes to a unit of the lobby or the drop, again to one
that fell out of its Pelican with none (the seat drops what a rider holds);
a gulag fighter's and a gulag winner's weapons, and a redeployed one's, are
theirs */
short royale_loot_kit_action(short phase, short state, boolean kitted, boolean armed);

/* the lobby's and the drop's sweep: a loose item is the map's own (deleted)
unless loot placed it or a unit ever held it; a loose start kit pistol goes
anyway (given again on the fall) */
boolean royale_loot_purge_spares(long object_index, long const *placed, short placed_count, long const *held,
	short held_count, long const *kits, short kit_count);

#endif
