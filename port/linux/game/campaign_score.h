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
	_campaign_score_class_wave,			/* a wave cleared (bigger battles) */
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
	_campaign_score_event_wave_clear,	/* bigger battles' wave cleared */
	_campaign_score_event_death,		/* the player died (points <= 0) */
	_campaign_score_event_betrayal,		/* an ally or another player killed (points <= 0) */
	_campaign_score_event_level_bonus,	/* the level finished */
	_campaign_score_event_grenade,		/* a kill's bonus: a grenade */
	_campaign_score_event_splatter,		/* a kill's bonus: run over */
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

/* the hooks (the game's kill and damage paths, its main loop) */
void campaign_score_initialize(void);
void campaign_score_dispose(void);
/* a map began (game_initialize_for_new_map) or ended (its dispose) */
void campaign_score_new_map(void);
void campaign_score_map_ended(void);
/* the game's tick ended (game_tick) */
void campaign_score_tick(void);
/* a unit died (units.c's unit_kill path, after the killing blow's record):
the unit, and the damage that killed it */
struct damage_data;
void campaign_score_unit_died(long unit_index, struct damage_data const *damage);
/* a plasma grenade stuck to an object (projectiles.c) */
void campaign_score_projectile_stuck(long projectile_index, long object_index);
/* the level was won (the game's win, before the next level) */
void campaign_score_level_won(void);
/* bigger battles: a wave cleared (a hook; nothing calls it without a clean
signal) */
void campaign_score_wave_cleared(long player_index);

#endif
