/*
CAMPAIGN_SCORE.H

Campaign scoring (campaign_score.c): points for the campaign's kills, solo
and co-op (online, LAN and split screen), arcade style. The score watches
the game's own records of deaths and damage and never changes the
simulation. The host keeps it (campaign.scoring, its option); a network
client takes the host's through the net sync, and split-screen players
share the host's state. A client whose host sends none shows no score.

The players are the game's player slots (players.c's datum index's
absolute index), the same on every machine of a network game.
*/

#ifndef __CAMPAIGN_SCORE_H
#define __CAMPAIGN_SCORE_H

/* ---------- constants */

/* a co-op game's most players (Server Setup's PLAYERS goes to the network
game's maximum) */
#define CAMPAIGN_SCORE_MAX_PLAYERS HALO_PORT_MAXIMUM_NETWORK_PLAYERS

/* campaign_score_player.kills_by_class's entries, and an event's
victim_class */
enum campaign_score_victim_class
{
	_campaign_score_class_other = 0,
	_campaign_score_class_grunt,
	_campaign_score_class_jackal,
	_campaign_score_class_elite,
	_campaign_score_class_elite_major,
	_campaign_score_class_elite_special,	/* stealth, spec ops, zealot, commander */
	_campaign_score_class_hunter,
	_campaign_score_class_flood_infection,
	_campaign_score_class_flood_combat,
	_campaign_score_class_flood_carrier,
	_campaign_score_class_sentinel,
	_campaign_score_class_marine,			/* betrayals of allies */
	_campaign_score_class_player,			/* betrayals of players */
	_campaign_score_class_vehicle_light,	/* ghost, banshee, a crewed turret */
	_campaign_score_class_vehicle_heavy,	/* wraith, a crewed warthog, scorpion */
	_campaign_score_class_wave,			/* (unused: Halo's fights are encounters) */
	NUMBER_OF_CAMPAIGN_SCORE_CLASSES
};

enum campaign_score_event_kind
{
	_campaign_score_event_kill = 0,
	_campaign_score_event_headshot,		/* a kill's bonus: the killing blow to the head */
	_campaign_score_event_melee,		/* a kill's bonus: a melee blow */
	_campaign_score_event_stick,		/* a plasma grenade stuck to an enemy */
	_campaign_score_event_multi_kill,	/* kills close together: points is the bonus, victim_class the count */
	_campaign_score_event_streak,		/* kills without dying: victim_class the streak */
	_campaign_score_event_vehicle,		/* a crewed vehicle destroyed */
	_campaign_score_event_wave_clear,	/* (unused: _campaign_score_event_encounter_cleared) */
	_campaign_score_event_death,		/* the player died (points <= 0) */
	_campaign_score_event_betrayal,		/* an ally or another player killed (points <= 0) */
	_campaign_score_event_level_bonus,	/* the level finished */
	_campaign_score_event_grenade,		/* a kill's bonus: a grenade */
	_campaign_score_event_splatter,		/* a kill's bonus: run over */
	/* an enemy encounter (a fight the level's designer placed) cleared: every
	player's, victim_class the encounter's kills; and its flawless bonus, no
	player having died during it */
	_campaign_score_event_encounter_cleared,
	_campaign_score_event_flawless,
	/* a kill's bonus: a melee from behind that killed an unaware enemy
	outright (the game's instant kill of the unsuspecting), in place of the
	melee's */
	_campaign_score_event_assassination,
	/* a kill by a gunner or passenger (a player or an ally's AI) of the
	vehicle a player drives: the driver's share; victim_class the kill's */
	_campaign_score_event_driver_assist,
	NUMBER_OF_CAMPAIGN_SCORE_EVENT_KINDS
};

/* ---------- structures */

struct campaign_score_event
{
	short player;
	short kind;
	long points;
	long tick;
	long victim_class;	/* for the text, e.g. "Elite Major" */
};

struct campaign_score_player
{
	long score, kills, headshots, melee_kills, sticks, deaths, betrayals, best_streak, current_streak, best_multi;
	long kills_by_class[16];	/* by enum campaign_score_victim_class */
};

struct campaign_score_state
{
	boolean enabled;	/* the host option, as the host set it */
	long team_score;
	short player_count;
	struct campaign_score_player players[CAMPAIGN_SCORE_MAX_PLAYERS];
	float difficulty_multiplier;
};

/* ---------- prototypes/CAMPAIGN_SCORE.C */

/* whether this game keeps a score: a campaign game whose host has
campaign.scoring on (a client: whose host sent its score) */
boolean campaign_score_enabled(void);
/* the score as this machine has it (the host's own, or the host's last
word to a client); never NULL */
struct campaign_score_state const *campaign_score_get(void);
/* popups and the Spartan Link's feed: events newer than *cursor, at most max;
returns the count and moves the cursor (*cursor 0 at first) */
long campaign_score_events_since(long *cursor, struct campaign_score_event *out, long max);
/* the level's summary, and a personal best (per level, difficulty, solo or
co-op player count): FALSE until a level has ended with a score */
boolean campaign_score_level_summary(struct campaign_score_state *out, boolean *new_personal_best);
/* the personal best kept for the level played, its difficulty and the game's
player count (this machine's file, a client's too): FALSE for none. Cheap
each frame (the file is read as each map begins) */
boolean campaign_score_personal_best(long *score);

/* the name of a victim class for the text ("Elite Major") */
char const *campaign_score_class_name(long victim_class);

/* the hooks: none changes the game */
/* game.c: a map began (game_initialize_for_new_map); and each tick's end */
void campaign_score_new_map(void);
void campaign_score_tick(void);
/* damage.c: a killing blow (the unit's body depleted by it, alive before),
as it is dealt, its actor still the unit's: the damage, its category
(damage.c's _damage_category_*), whether it struck the head, and whether it
killed outright as a blow lethal to the unsuspecting (a melee from behind
on an unaware unit) */
struct damage_data;
void campaign_score_unit_killed(long unit_index, struct damage_data const *damage, short damage_category,
	boolean head, boolean unsuspecting);
/* projectiles.c: a projectile stuck to an object */
void campaign_score_projectile_stuck(long projectile_index, long object_index);
/* game_state.c: a checkpoint saved, and a revert to it */
void campaign_score_checkpoint_saved(void);
void campaign_score_reverted(void);
/* main.c: the level won, before the next level */
void campaign_score_level_won(void);

/* network_campaign_score.c: a client's word from its host (the state's
header, a player's record, an event, the level's end) */
void campaign_score_heard_state(boolean enabled, long team_score, short player_count, real difficulty_multiplier);
void campaign_score_heard_player(short slot, struct campaign_score_player const *player);
void campaign_score_heard_event(struct campaign_score_event const *event);
void campaign_score_heard_level_end(void);

#endif
