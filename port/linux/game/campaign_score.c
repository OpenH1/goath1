/*
CAMPAIGN_SCORE.C

Campaign scoring (campaign_score.h): arcade points for the campaign's
kills, solo and co-op.

- An observer. It reads the game's own records as the game makes them (a
  killing blow's damage as damage.c deals it, a grenade stuck as
  projectiles.c attaches it, a level won as main.c ends it) and writes only
  its own state, outside the game state: no object, random seed or game
  state byte is touched, so the game plays the same with it on and off
  (tools/scorecard/identity.py). A saved game holds none of it.
- The host keeps it: a campaign of this computer alone (solo, split screen,
  whose players share this state) and a co-op host, each with
  campaign.scoring on as the level began. A network client keeps none of
  its own: it takes the host's (network_campaign_score.c), and without the
  host's word it shows no score.
- A checkpoint's revert takes the score back to the checkpoint's, but the
  deaths and betrayals since stay counted (a death that reverts the level
  still costs its points).
- The points are one table (campaign_score_points and
  campaign_score_bonuses), times the difficulty's multiplier; a death or a
  betrayal takes a few points back, never below 0.
- A melee that kills an unaware enemy outright from behind (damage.c's blow
  lethal to the unsuspecting) is an assassination, its bonus in place of the
  melee's. A kill by a gunner or passenger of a vehicle a player drives (a
  player or an ally's AI) gives the driver an assist: half the kill's points
  (the table's, to the nearest), times the multiplier.
- Personal bests: campaign_scores.txt in the config folder, a line each of
  the level, the difficulty, the players and the team's best score.
*/

#include "cseries.h"
#include "cseries/errors.h"
#include "ai/actors.h"
#include "ai/encounters.h"
#include "game/game.h"
#include "game/game_allegiance.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "items/projectiles.h"
#include "objects/damage.h"
#include "objects/objects.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "tag_files/tag_files.h"
#include "units/units.h"
#include "campaign_score.h"
#include "demo.h"
#include "network_campaign_score.h"

#include <stdio.h>
#include <string.h>

/* ---------- prototypes */

/* port_config.c's */
int config_boolean(const char *name);
void config_folder(char *path, size_t size);

/* ---------- constants */

enum
{
	/* kills within this many ticks of the one before are a multi-kill (4 s) */
	CAMPAIGN_SCORE_MULTI_KILL_TICKS = 4 * 30,
	/* a streak's bonus each this many kills without dying */
	CAMPAIGN_SCORE_STREAK_STEP = 5,
	/* the events kept for the popups and the Spartan Link's feed */
	CAMPAIGN_SCORE_EVENT_RING = 256,
	/* the most events one kill makes (the kill, its bonuses, a multi-kill, a
	streak) */
	CAMPAIGN_SCORE_EVENTS_PER_KILL = 6,
	/* the host's word to its clients at most each this many ticks (267 ms) */
	CAMPAIGN_SCORE_SEND_TICKS = 8,
	/* ... and all of it each this many, for a client that missed some */
	CAMPAIGN_SCORE_REFRESH_TICKS = 10 * 30,
	CAMPAIGN_SCORE_MAXIMUM_BESTS = 1024,
	/* the encounters followed (their absolute indices; the game has 128), the
	enemies' units followed in them, and the units killed in one tick */
	CAMPAIGN_SCORE_MAX_ENCOUNTERS = 256,
	CAMPAIGN_SCORE_MAX_TRACKED_UNITS = 4096,
	CAMPAIGN_SCORE_MAX_DIED_PER_TICK = 256,
	CAMPAIGN_SCORE_LEVEL_NAME_SIZE = 64,
};

/* the damage categories (damage.c's, which it keeps to itself) */
enum
{
	_campaign_score_damage_none = 0,
	_campaign_score_damage_falling,
	_campaign_score_damage_bullet,
	_campaign_score_damage_grenade,
	_campaign_score_damage_high_explosive,
	_campaign_score_damage_sniper,
	_campaign_score_damage_melee,
	_campaign_score_damage_flame,
	_campaign_score_damage_mounted_weapon,
	_campaign_score_damage_vehicle,
	_campaign_score_damage_plasma,
	_campaign_score_damage_needle,
	_campaign_score_damage_shotgun,
};

/* ---------- the points */

/* a victim's class: its name, the points for the kill, and whether it
counts toward multi-kills and streaks (the infection forms come in swarms) */
struct campaign_score_class_points
{
	char const *name;
	short points;
	boolean counts_toward_streaks;
};

/* THE POINTS TABLE, by enum campaign_score_victim_class (before the
difficulty's multiplier) */
typedef char campaign_score_classes_assert[
	NUMBER_OF_CAMPAIGN_SCORE_CLASSES <= NUMBEROF(((struct campaign_score_player *)0)->kills_by_class) ? 1 : -1];

static struct campaign_score_class_points const campaign_score_points[NUMBER_OF_CAMPAIGN_SCORE_CLASSES] =
{
	{ "Enemy", 10, TRUE },					/* _campaign_score_class_other */
	{ "Grunt", 10, TRUE },
	{ "Jackal", 20, TRUE },
	{ "Elite", 50, TRUE },
	{ "Elite Major", 75, TRUE },
	{ "Elite Spec Ops", 100, TRUE },		/* stealth, spec ops, zealot, commander */
	{ "Hunter", 150, TRUE },
	{ "Infection Form", 2, FALSE },
	{ "Combat Form", 25, TRUE },
	{ "Carrier Form", 15, TRUE },
	{ "Sentinel", 30, TRUE },
	{ "Marine", 0, FALSE },					/* a betrayal */
	{ "Spartan", 0, FALSE },				/* a betrayal */
	{ "Light Vehicle", 75, FALSE },			/* ghost, banshee, shade */
	{ "Heavy Vehicle", 200, FALSE },		/* wraith, a crewed warthog or scorpion */
	{ "Wave", 0, FALSE },
};

/* the bonuses and deductions (before the multiplier; the deductions are not
multiplied) */
static struct
{
	short headshot;
	short melee;
	/* a melee from behind that killed an unaware enemy outright */
	short assassination;
	short grenade;
	short splatter;
	short stick;
	/* a multi-kill of 2, 3, 4, 5 and 6 or more kills */
	short multi_kill[5];
	/* each CAMPAIGN_SCORE_STREAK_STEP kills without dying, the step's
	number times this, to 4 steps */
	short streak_step;
	short streak_steps_maximum;
	/* an encounter cleared: this percent of its kills' points (their table's,
	before the multiplier), at least the minimum; and flawless, this percent
	of that more */
	short encounter_percent;
	short encounter_minimum;
	short flawless_percent;
	short level_bonus;
	short level_deathless_bonus;
	short death;
	short betrayal_ally;
	short betrayal_player;
	/* a kill by a gunner or passenger of the vehicle a player drives: the
	driver's, this percent of the kill's points (the table's), to the nearest */
	short driver_assist_percent;
} const campaign_score_bonuses =
{
	15, 10, 25, 5, 10, 15,
	{ 25, 50, 100, 150, 200 },
	50, 4,
	25, 50, 50,
	1000, 500,
	25, 50, 100,
	50,
};

/* each difficulty's multiplier: easy, normal, heroic, legendary */
static real const campaign_score_difficulty_multipliers[] = { 1.0f, 1.5f, 2.0f, 3.0f };

/* ---------- structures */

/* what a player's kills are timed by: the last kill's tick, the kills since
within the multi-kill's window */
struct campaign_score_timing
{
	long last_kill_tick;
	short multi_count;
	short pad;
};

/* each encounter's tallies, kept with the checkpoint's copy: the points
(the table's, before the multiplier) and the count of the kills of its
enemies, the players' deaths when it began, and whether its bonus was given */
struct campaign_score_encounter_tallies
{
	long kill_points[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	short kills[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	long deaths_at_start[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	boolean awarded[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
};

/* the encounters as the last tick left them: their tallies, their living
enemies, and the living enemies' units, each with its encounter; rescan,
the next tick only looks (after a revert, whose units are another tick's) */
struct campaign_score_encounter_tracking
{
	struct campaign_score_encounter_tallies tallies;
	short living[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	long tracked_units[CAMPAIGN_SCORE_MAX_TRACKED_UNITS];
	short tracked_encounters[CAMPAIGN_SCORE_MAX_TRACKED_UNITS];
	long tracked_count;
	boolean rescan;
};

/* an encounter cleared: its bonus, and its flawless bonus (0: a player died) */
struct campaign_score_encounter_clear
{
	short encounter;
	short kills;
	long bonus;
	long flawless_bonus;
};

struct campaign_score_best
{
	char level[CAMPAIGN_SCORE_LEVEL_NAME_SIZE];
	short difficulty;
	short players;
	long score;
};

/* ---------- globals */

static struct
{
	/* scoring this level (this machine keeps the score: the host's choice),
	and the level won (nothing more counts) */
	boolean keeping;
	boolean level_over;
	/* a network client that heard its host's word this level */
	boolean heard;
	struct campaign_score_state state;
	struct campaign_score_timing timings[CAMPAIGN_SCORE_MAX_PLAYERS];
	/* the score as the last checkpoint had it, and the deaths and betrayals
	since (kept through a revert) */
	struct campaign_score_state checkpoint;
	long deaths_since_checkpoint[CAMPAIGN_SCORE_MAX_PLAYERS];
	long betrayal_points_since_checkpoint[CAMPAIGN_SCORE_MAX_PLAYERS];
	long betrayals_since_checkpoint[CAMPAIGN_SCORE_MAX_PLAYERS];
	/* the players the clients have not had since they changed */
	boolean dirty[CAMPAIGN_SCORE_MAX_PLAYERS];
	/* the events: the ring, how many were ever added (their sequence), and
	how many of them the clients have had */
	struct campaign_score_event events[CAMPAIGN_SCORE_EVENT_RING];
	long event_sequence;
	long events_sent;
	long last_send_tick;
	long last_refresh_tick;
	/* the encounters, the copy of their tallies at the last checkpoint, the
	units killed this tick, and the encounters' indices (by absolute index) */
	struct campaign_score_encounter_tracking encounters;
	struct campaign_score_encounter_tallies encounters_checkpoint;
	long died_units[CAMPAIGN_SCORE_MAX_DIED_PER_TICK];
	short died_count;
	long encounter_indices[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	/* the last level's summary */
	boolean summary_valid;
	boolean summary_new_best;
	struct campaign_score_state summary;
} campaign_score;

/* ---------- private code: the rules (the harness takes these) */

/* the case-insensitive search of a name for a part of it */
static boolean campaign_score_name_has(
	char const *name,
	char const *part)
{
	long name_index;
	long part_index;

	if (!name || !part)
		return FALSE;
	for (name_index = 0; name[name_index]; name_index++)
	{
		for (part_index = 0; part[part_index]; part_index++)
		{
			char a = name[name_index + part_index];
			char b = part[part_index];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (!a || a != b)
				break;
		}
		if (!part[part_index])
			return TRUE;
	}

	return FALSE;
}

/* a victim's class from its unit's tag and its actor variant's (either may be
NULL), and whether it is a vehicle */
static short campaign_score_class_from_names(
	char const *unit_name,
	char const *variant_name,
	boolean vehicle)
{
	if (vehicle)
	{
		if (campaign_score_name_has(unit_name, "wraith") || campaign_score_name_has(unit_name, "scorpion") ||
			campaign_score_name_has(unit_name, "warthog") || campaign_score_name_has(unit_name, "pelican") ||
			campaign_score_name_has(unit_name, "spirit"))
		{
			return _campaign_score_class_vehicle_heavy;
		}
		return _campaign_score_class_vehicle_light;
	}
	if (campaign_score_name_has(unit_name, "infection"))
		return _campaign_score_class_flood_infection;
	if (campaign_score_name_has(unit_name, "carrier"))
		return _campaign_score_class_flood_carrier;
	if (campaign_score_name_has(unit_name, "flood"))
		return _campaign_score_class_flood_combat;
	if (campaign_score_name_has(unit_name, "hunter"))
		return _campaign_score_class_hunter;
	if (campaign_score_name_has(unit_name, "elite"))
	{
		if (campaign_score_name_has(variant_name, "stealth") || campaign_score_name_has(variant_name, "spec") ||
			campaign_score_name_has(variant_name, "zealot") || campaign_score_name_has(variant_name, "commander"))
		{
			return _campaign_score_class_elite_special;
		}
		if (campaign_score_name_has(variant_name, "major"))
			return _campaign_score_class_elite_major;
		return _campaign_score_class_elite;
	}
	if (campaign_score_name_has(unit_name, "jackal"))
		return _campaign_score_class_jackal;
	if (campaign_score_name_has(unit_name, "grunt"))
		return _campaign_score_class_grunt;
	if (campaign_score_name_has(unit_name, "sentinel") || campaign_score_name_has(unit_name, "enforcer"))
		return _campaign_score_class_sentinel;
	if (campaign_score_name_has(unit_name, "marine") || campaign_score_name_has(unit_name, "crewman") ||
		campaign_score_name_has(unit_name, "captain"))
	{
		return _campaign_score_class_marine;
	}

	return _campaign_score_class_other;
}

/* points times the multiplier, to the nearest */
static long campaign_score_scaled(
	long points,
	real multiplier)
{
	return (long)((real)points * multiplier + 0.5f);
}

static void campaign_score_note_event(
	struct campaign_score_event *events,
	long *event_count,
	short player,
	short kind,
	long points,
	long tick,
	long victim_class)
{
	struct campaign_score_event *event = &events[*event_count];

	event->player = player;
	event->kind = kind;
	event->points = points;
	event->tick = tick;
	event->victim_class = victim_class;
	(*event_count)++;
}

/* a player's kill of an enemy of a class at a tick, by damage of a category
(head: the blow struck the head; unsuspecting: it killed outright as lethal
to the unsuspecting): the player's tallies, and the events it makes (at most
CAMPAIGN_SCORE_EVENTS_PER_KILL) added to events; the points it gave */
static long campaign_score_player_kill(
	struct campaign_score_player *player,
	struct campaign_score_timing *timing,
	short player_slot,
	long tick,
	short victim_class,
	short category,
	boolean head,
	boolean unsuspecting,
	real multiplier,
	struct campaign_score_event *events,
	long *event_count)
{
	struct campaign_score_class_points const *class_points;
	long total = 0;
	long points;

	if (!VALID_INDEX(victim_class, NUMBER_OF_CAMPAIGN_SCORE_CLASSES))
		victim_class = _campaign_score_class_other;
	class_points = &campaign_score_points[victim_class];
	points = campaign_score_scaled(class_points->points, multiplier);
	player->kills++;
	if (victim_class < NUMBEROF(player->kills_by_class))
		player->kills_by_class[victim_class]++;
	campaign_score_note_event(events, event_count, player_slot,
		victim_class == _campaign_score_class_vehicle_light || victim_class == _campaign_score_class_vehicle_heavy ?
			_campaign_score_event_vehicle : _campaign_score_event_kill,
		points, tick, victim_class);
	total += points;

	/* (one bonus for the way of it: the head, a melee (an assassination's
	from behind), a grenade, a splatter) */
	if (head && (category == _campaign_score_damage_bullet || category == _campaign_score_damage_sniper ||
		category == _campaign_score_damage_plasma || category == _campaign_score_damage_needle ||
		category == _campaign_score_damage_shotgun || category == _campaign_score_damage_mounted_weapon))
	{
		player->headshots++;
		points = campaign_score_scaled(campaign_score_bonuses.headshot, multiplier);
		campaign_score_note_event(events, event_count, player_slot, _campaign_score_event_headshot, points, tick,
			victim_class);
		total += points;
	}
	else if (category == _campaign_score_damage_melee)
	{
		player->melee_kills++;
		points = campaign_score_scaled(unsuspecting ? campaign_score_bonuses.assassination :
			campaign_score_bonuses.melee, multiplier);
		campaign_score_note_event(events, event_count, player_slot,
			unsuspecting ? _campaign_score_event_assassination : _campaign_score_event_melee, points, tick,
			victim_class);
		total += points;
	}
	else if (category == _campaign_score_damage_grenade)
	{
		points = campaign_score_scaled(campaign_score_bonuses.grenade, multiplier);
		campaign_score_note_event(events, event_count, player_slot, _campaign_score_event_grenade, points, tick,
			victim_class);
		total += points;
	}
	else if (category == _campaign_score_damage_vehicle)
	{
		points = campaign_score_scaled(campaign_score_bonuses.splatter, multiplier);
		campaign_score_note_event(events, event_count, player_slot, _campaign_score_event_splatter, points, tick,
			victim_class);
		total += points;
	}

	if (class_points->counts_toward_streaks)
	{
		/* (a multi-kill: within the window of the kill before) */
		if (timing->multi_count > 0 && tick - timing->last_kill_tick <= CAMPAIGN_SCORE_MULTI_KILL_TICKS &&
			tick >= timing->last_kill_tick)
		{
			timing->multi_count++;
		}
		else
		{
			timing->multi_count = 1;
		}
		timing->last_kill_tick = tick;
		if (timing->multi_count >= 2)
		{
			short step = (short)MIN(timing->multi_count - 2, NUMBEROF(campaign_score_bonuses.multi_kill) - 1);

			points = campaign_score_scaled(campaign_score_bonuses.multi_kill[step], multiplier);
			campaign_score_note_event(events, event_count, player_slot, _campaign_score_event_multi_kill, points,
				tick, timing->multi_count);
			total += points;
			if (timing->multi_count > player->best_multi)
				player->best_multi = timing->multi_count;
		}

		player->current_streak++;
		if (player->current_streak > player->best_streak)
			player->best_streak = player->current_streak;
		if (player->current_streak % CAMPAIGN_SCORE_STREAK_STEP == 0)
		{
			long steps = MIN(player->current_streak / CAMPAIGN_SCORE_STREAK_STEP,
				campaign_score_bonuses.streak_steps_maximum);

			points = campaign_score_scaled(campaign_score_bonuses.streak_step * steps, multiplier);
			campaign_score_note_event(events, event_count, player_slot, _campaign_score_event_streak, points, tick,
				player->current_streak);
			total += points;
		}
	}

	player->score += total;
	return total;
}

/* a driver's assist: the share of a kill of a class by a gunner or passenger
of their vehicle, times the multiplier */
static long campaign_score_driver_assist_points(
	short victim_class,
	real multiplier)
{
	long share;

	if (!VALID_INDEX(victim_class, NUMBER_OF_CAMPAIGN_SCORE_CLASSES))
		victim_class = _campaign_score_class_other;
	share = (campaign_score_points[victim_class].points * campaign_score_bonuses.driver_assist_percent + 50) / 100;
	return campaign_score_scaled(share, multiplier);
}

/* points taken from a player (a death, a betrayal), never below 0: what was
taken */
static long campaign_score_player_deduct(
	struct campaign_score_player *player,
	long points)
{
	long taken = MIN(points, player->score);

	if (taken < 0)
		taken = 0;
	player->score -= taken;
	return taken;
}

/* a player died: their streak and multi-kill end, and a few points go */
static long campaign_score_player_death(
	struct campaign_score_player *player,
	struct campaign_score_timing *timing)
{
	player->deaths++;
	player->current_streak = 0;
	timing->multi_count = 0;
	return campaign_score_player_deduct(player, campaign_score_bonuses.death);
}

/* an encounter's bonus for its kills' points (the table's): before the
multiplier */
static long campaign_score_encounter_bonus(
	long kill_points)
{
	return MAX(campaign_score_bonuses.encounter_minimum, kill_points * campaign_score_bonuses.encounter_percent / 100);
}

/* a tick's look at the encounters: the living enemies' units now (units, and
each one's encounter, by absolute index), whether a unit died
(unit_died: killed, its body there dead or gone with its killing blow), whether an
encounter is still to respawn some (respawn_pending), and the players' deaths
so far. An encounter is cleared when it had living enemies the tick before,
has none now, every one of those died (none erased, none moved to another
encounter: migrated), none is still to respawn, and it was not cleared
before (since the level began, or as the checkpoint reverted to had it).
The cleared ones written to cleared (at most maximum); their count */
static long campaign_score_encounters_step(
	struct campaign_score_encounter_tracking *tracking,
	long const *units,
	short const *encounters,
	long count,
	boolean (*unit_died)(long unit_index),
	boolean (*respawn_pending)(short encounter),
	long team_deaths,
	struct campaign_score_encounter_clear *cleared,
	long maximum)
{
	static short living[CAMPAIGN_SCORE_MAX_ENCOUNTERS];
	struct campaign_score_encounter_tallies *tallies = &tracking->tallies;
	long cleared_count = 0;
	long index;
	short encounter;

	csmemset(living, 0, sizeof(living));
	count = MIN(count, CAMPAIGN_SCORE_MAX_TRACKED_UNITS);
	for (index = 0; index < count; index++)
	{
		if (VALID_INDEX(encounters[index], CAMPAIGN_SCORE_MAX_ENCOUNTERS))
			living[encounters[index]]++;
	}
	for (encounter = 0; !tracking->rescan && encounter < CAMPAIGN_SCORE_MAX_ENCOUNTERS; encounter++)
	{
		boolean all_died = TRUE;
		boolean any_died = FALSE;
		struct campaign_score_encounter_clear *clear;

		/* (a fight begun: the deaths it is flawless without) */
		if (tracking->living[encounter] == 0 && living[encounter] > 0 && !tallies->awarded[encounter])
			tallies->deaths_at_start[encounter] = team_deaths;
		if (tracking->living[encounter] == 0 || living[encounter] > 0 || tallies->awarded[encounter])
			continue;
		for (index = 0; index < tracking->tracked_count; index++)
		{
			if (tracking->tracked_encounters[index] != encounter)
				continue;
			if (unit_died(tracking->tracked_units[index]))
				any_died = TRUE;
			else
				all_died = FALSE;
		}
		if (!all_died || !any_died || (respawn_pending && respawn_pending(encounter)) || cleared_count >= maximum)
			continue;
		tallies->awarded[encounter] = TRUE;
		clear = &cleared[cleared_count++];
		clear->encounter = encounter;
		clear->kills = tallies->kills[encounter];
		clear->bonus = campaign_score_encounter_bonus(tallies->kill_points[encounter]);
		clear->flawless_bonus = team_deaths == tallies->deaths_at_start[encounter] ?
			clear->bonus * campaign_score_bonuses.flawless_percent / 100 : 0;
	}
	tracking->rescan = FALSE;
	csmemcpy(tracking->living, living, sizeof(living));
	csmemcpy(tracking->tracked_units, units, count * sizeof(*units));
	csmemcpy(tracking->tracked_encounters, encounters, count * sizeof(*encounters));
	tracking->tracked_count = count;

	return cleared_count;
}

/* ---------- private code: the game's side */

static void campaign_score_team_total(
	void)
{
	long total = 0;
	short slot;

	for (slot = 0; slot < campaign_score.state.player_count; slot++)
		total += campaign_score.state.players[slot].score;
	campaign_score.state.team_score = total;
}

static void campaign_score_add_events(
	struct campaign_score_event const *events,
	long count)
{
	long index;

	for (index = 0; index < count; index++)
	{
		campaign_score.events[campaign_score.event_sequence % CAMPAIGN_SCORE_EVENT_RING] = events[index];
		campaign_score.event_sequence++;
	}
}

/* a player's slot, NONE for no player of this game */
static short campaign_score_slot(
	long player_index)
{
	short slot;

	if (player_index == NONE || !player_try_and_get(player_index))
		return NONE;
	slot = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (!VALID_INDEX(slot, CAMPAIGN_SCORE_MAX_PLAYERS))
		return NONE;
	if (slot >= campaign_score.state.player_count)
		campaign_score.state.player_count = (short)(slot + 1);
	return slot;
}

/* a player's deduction and its event */
static void campaign_score_deduction(
	short slot,
	short kind,
	long points,
	long victim_class)
{
	struct campaign_score_event event;

	event.player = slot;
	event.kind = (short)kind;
	event.points = -points;
	event.tick = game_time_get();
	event.victim_class = victim_class;
	campaign_score_add_events(&event, 1);
	campaign_score.dirty[slot] = TRUE;
}

static void campaign_score_betrayal(
	short slot,
	long victim_class)
{
	struct campaign_score_player *player = &campaign_score.state.players[slot];
	long points = victim_class == _campaign_score_class_player ? campaign_score_bonuses.betrayal_player :
		campaign_score_bonuses.betrayal_ally;
	long taken;

	player->betrayals++;
	campaign_score.betrayals_since_checkpoint[slot]++;
	taken = campaign_score_player_deduct(player, points);
	campaign_score.betrayal_points_since_checkpoint[slot] += points;
	campaign_score_deduction(slot, _campaign_score_event_betrayal, taken, victim_class);
}

/* whether this machine keeps the score as a level begins: a campaign level
of this computer's or a co-op host's, with campaign.scoring on, not a
demo's playback */
static boolean campaign_score_keeps(
	void)
{
	return game_connection() != _game_connection_network_client && global_scenario &&
		global_scenario->type == _scenario_type_solo && !game_engine_running() && !demo_playing() &&
		config_boolean("campaign.scoring");
}

static short campaign_score_player_total(
	void)
{
	struct data_iterator iterator;
	short count = 0;

	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
		count++;

	return count;
}

/* the class of a unit as it dies (its actor still its own) */
static short campaign_score_unit_class(
	struct unit_datum const *unit)
{
	char const *variant_name = NULL;

	if (unit->unit.player_index != NONE)
		return _campaign_score_class_player;
	if (unit->unit.actor_index != NONE)
	{
		struct actor_datum const *actor = actor_try_and_get(unit->unit.actor_index);

		if (actor && actor->meta.variant_definition_index != NONE)
			variant_name = tag_get_name(actor->meta.variant_definition_index);
	}
	return campaign_score_class_from_names(tag_get_name(unit->definition_index), variant_name,
		unit->object.type == _object_type_vehicle);
}

/* the player driving the vehicle whose gunner or passenger dealt a killing
blow (a player or an ally's AI; the vehicle's own weapon, its gunner's),
whose enemy the victim is: their slot, NONE for none (the driver's own
blow, a splatter or their vehicle's weapon, is theirs) */
static short campaign_score_assisting_driver(
	struct damage_data const *damage,
	struct unit_datum const *victim)
{
	struct unit_datum *shooter;
	struct unit_datum *vehicle;
	struct unit_datum *driver;

	if (damage->owner_object_index == NONE || !(shooter = unit_try_and_get(damage->owner_object_index)))
		return NONE;
	if (shooter->object.type == _object_type_vehicle)
	{
		vehicle = shooter;
		if (vehicle->unit.gunner_object_index == NONE || !(shooter = unit_try_and_get(vehicle->unit.gunner_object_index)))
			return NONE;
	}
	else
	{
		if (shooter->object.parent_object_index == NONE)
			return NONE;
		vehicle = unit_try_and_get(shooter->object.parent_object_index);
	}
	if (!vehicle || vehicle->object.type != _object_type_vehicle || vehicle->unit.driver_object_index == NONE)
		return NONE;
	driver = unit_try_and_get(vehicle->unit.driver_object_index);
	if (!driver || driver == shooter || driver->unit.player_index == NONE ||
		driver->unit.player_index == damage->owner_player_index ||
		!game_team_is_enemy(victim->object.owner_team_index, driver->object.owner_team_index))
	{
		return NONE;
	}
	return campaign_score_slot(driver->unit.player_index);
}

/* ---------- private code: personal bests */

static boolean campaign_score_bests_path(
	char *path,
	size_t size)
{
	char folder[1024];
	int length;

	config_folder(folder, sizeof(folder));
	length = snprintf(path, size, "%scampaign_scores.txt", folder);
	return length > 0 && (size_t)length < size;
}

/* the bests as the file has them, read as each map begins */
static struct
{
	struct campaign_score_best bests[CAMPAIGN_SCORE_MAXIMUM_BESTS];
	long count;
	/* campaign_score_personal_best's last answer, for this many players */
	short cached_players;
	long cached_index;
} campaign_score_bests;

static void campaign_score_bests_read(
	void)
{
	char path[1100];
	char line[256];
	FILE *file;

	campaign_score_bests.count = 0;
	campaign_score_bests.cached_players = NONE;
	if (!campaign_score_bests_path(path, sizeof(path)) || !(file = fopen(path, "r")))
		return;
	while (campaign_score_bests.count < CAMPAIGN_SCORE_MAXIMUM_BESTS && fgets(line, sizeof(line), file))
	{
		struct campaign_score_best *best = &campaign_score_bests.bests[campaign_score_bests.count];
		int read_difficulty;
		int read_players;
		long read_score;

		if (line[0] == '#' || sscanf(line, "%63s %d %d %ld", best->level, &read_difficulty, &read_players,
			&read_score) != 4)
		{
			continue;
		}
		best->level[CAMPAIGN_SCORE_LEVEL_NAME_SIZE - 1] = 0;
		best->difficulty = (short)read_difficulty;
		best->players = (short)read_players;
		best->score = read_score;
		campaign_score_bests.count++;
	}
	fclose(file);
}

/* the best kept for a level, difficulty and player count: its index, NONE */
static long campaign_score_bests_find(
	char const *level,
	short difficulty,
	short players)
{
	long index;

	for (index = 0; index < campaign_score_bests.count; index++)
	{
		struct campaign_score_best const *best = &campaign_score_bests.bests[index];

		if (best->difficulty == difficulty && best->players == players && !strcmp(best->level, level))
			return index;
	}
	return NONE;
}

static char const *campaign_score_level_name(
	void)
{
	return global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
}

/* the level's team score against the best kept for it (TRUE: a new best,
kept and written) */
static boolean campaign_score_record_best(
	char const *level,
	short difficulty,
	short players,
	long score)
{
	char path[1100];
	char temporary[1110];
	FILE *file;
	long index;
	long found;

	if (!level || !*level || score <= 0 || !campaign_score_bests_path(path, sizeof(path)))
		return FALSE;
	found = campaign_score_bests_find(level, difficulty, players);
	if (found != NONE && campaign_score_bests.bests[found].score >= score)
		return FALSE;
	if (found == NONE)
	{
		if (campaign_score_bests.count >= CAMPAIGN_SCORE_MAXIMUM_BESTS)
			return FALSE;
		found = campaign_score_bests.count++;
		csstrncpy(campaign_score_bests.bests[found].level, level, CAMPAIGN_SCORE_LEVEL_NAME_SIZE - 1);
		campaign_score_bests.bests[found].level[CAMPAIGN_SCORE_LEVEL_NAME_SIZE - 1] = 0;
		campaign_score_bests.bests[found].difficulty = difficulty;
		campaign_score_bests.bests[found].players = players;
	}
	campaign_score_bests.bests[found].score = score;
	campaign_score_bests.cached_players = NONE;

	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "w");
	if (!file)
		return TRUE;
	fprintf(file, "# campaign scoring's personal bests: level, difficulty (0 easy to 3 legendary), players, "
		"team score\n");
	for (index = 0; index < campaign_score_bests.count; index++)
	{
		struct campaign_score_best const *best = &campaign_score_bests.bests[index];

		fprintf(file, "%s %d %d %ld\n", best->level, best->difficulty, best->players, best->score);
	}
	if (fclose(file) == 0)
		rename(temporary, path);
	else
		remove(temporary);

	return TRUE;
}

/* the level ended with this score: its summary, and the best of it */
static void campaign_score_summarize(
	void)
{
	short players = campaign_score_player_total();
	char const *level = campaign_score_level_name();

	campaign_score.summary = campaign_score.state;
	campaign_score.summary_valid = TRUE;
	campaign_score.summary_new_best = campaign_score_record_best(level, game_difficulty_level_get(),
		players > 0 ? players : 1, campaign_score.state.team_score);
	error(_error_log, "campaign score: %s ended at %ld (%d players)%s", level, campaign_score.state.team_score,
		players, campaign_score.summary_new_best ? ", a personal best" : "");
}

/* the host's word to its clients: what changed, and the events since */
static void campaign_score_send(
	boolean level_ended)
{
	long now = game_time_get();
	boolean whole = now - campaign_score.last_refresh_tick >= CAMPAIGN_SCORE_REFRESH_TICKS || now <
		campaign_score.last_refresh_tick;
	long first = MAX(campaign_score.events_sent, campaign_score.event_sequence - CAMPAIGN_SCORE_EVENT_RING);

	if (game_connection() != _game_connection_network_server)
	{
		campaign_score.events_sent = campaign_score.event_sequence;
		csmemset(campaign_score.dirty, 0, sizeof(campaign_score.dirty));
		return;
	}
	if (whole)
		campaign_score.last_refresh_tick = now;
	network_campaign_score_send(&campaign_score.state, campaign_score.dirty, whole, level_ended,
		campaign_score.events, CAMPAIGN_SCORE_EVENT_RING, first, campaign_score.event_sequence);
	campaign_score.events_sent = campaign_score.event_sequence;
	campaign_score.last_send_tick = now;
	csmemset(campaign_score.dirty, 0, sizeof(campaign_score.dirty));
}

/* ---------- private code: the encounters */

/* the encounter (its absolute index) of a unit's actor, or its swarm's;
NONE for none */
static short campaign_score_unit_encounter(
	struct unit_datum const *unit)
{
	long actor_index = unit->unit.actor_index != NONE ? unit->unit.actor_index : unit->unit.swarm_actor_index;
	struct actor_datum const *actor = actor_index != NONE ? actor_try_and_get(actor_index) : NULL;
	short encounter;

	if (!actor || actor->meta.encounter_index == NONE)
		return NONE;
	encounter = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(actor->meta.encounter_index);
	return VALID_INDEX(encounter, CAMPAIGN_SCORE_MAX_ENCOUNTERS) ? encounter : NONE;
}

/* (campaign_score_encounters_step's) a unit killed (its killing blow seen
this tick), or its body there dead */
static boolean campaign_score_unit_died(
	long unit_index)
{
	struct unit_datum *unit;
	short index;

	for (index = 0; index < campaign_score.died_count; index++)
	{
		if (campaign_score.died_units[index] == unit_index)
			return TRUE;
	}
	unit = unit_try_and_get(unit_index);
	return unit && TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
}

/* (... and) an encounter that respawns, with some of its squads still to */
static boolean campaign_score_respawn_pending(
	short encounter_absolute_index)
{
	struct encounter_datum *encounter =
		encounter_try_and_get(campaign_score.encounter_indices[encounter_absolute_index]);
	short squad;

	if (!encounter || !encounter->respawn_enabled)
		return FALSE;
	for (squad = 0; squad < encounter->squad_count; squad++)
	{
		short squad_index = (short)(encounter->squad_base + squad);

		if (VALID_INDEX(squad_index, MAXIMUM_SQUADS_PER_MAP) && squad_array[squad_index].respawn_actors_left > 0)
			return TRUE;
	}
	return FALSE;
}

static void campaign_score_track_unit(
	long unit_index,
	short encounter,
	long *units,
	short *encounters,
	long *count)
{
	struct unit_datum *unit = unit_try_and_get(unit_index);

	if (!unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) || *count >= CAMPAIGN_SCORE_MAX_TRACKED_UNITS)
		return;
	units[*count] = unit_index;
	encounters[*count] = encounter;
	(*count)++;
}

/* a tick's look at the encounters: each enemy actor's living units (a
swarm's each), by its encounter now; and the bonus of each one cleared, every
player's */
static void campaign_score_encounters_update(
	void)
{
	static long units[CAMPAIGN_SCORE_MAX_TRACKED_UNITS];
	static short encounters[CAMPAIGN_SCORE_MAX_TRACKED_UNITS];
	struct campaign_score_encounter_clear cleared[16];
	struct data_iterator iterator;
	struct actor_datum *actor;
	long count = 0;
	long team_deaths = 0;
	long cleared_count;
	long index;
	short slot;

	data_iterator_new(&iterator, actor_data);
	while ((actor = (struct actor_datum *)data_iterator_next(&iterator)) != NULL)
	{
		short encounter;

		if (actor->meta.encounter_index == NONE || !game_team_is_enemy(_game_team_player, actor->meta.team_index))
			continue;
		encounter = (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(actor->meta.encounter_index);
		if (!VALID_INDEX(encounter, CAMPAIGN_SCORE_MAX_ENCOUNTERS))
			continue;
		campaign_score.encounter_indices[encounter] = actor->meta.encounter_index;
		if (actor->meta.swarm)
		{
			long unit_index = actor->meta.swarm_unit_index;
			short guard = 0;

			while (unit_index != NONE && guard++ < 256)
			{
				struct unit_datum *unit = unit_try_and_get(unit_index);

				campaign_score_track_unit(unit_index, encounter, units, encounters, &count);
				unit_index = unit ? unit->unit.swarm_next_unit_index : NONE;
			}
		}
		else if (actor->meta.unit_index != NONE)
		{
			campaign_score_track_unit(actor->meta.unit_index, encounter, units, encounters, &count);
		}
	}
	for (slot = 0; slot < campaign_score.state.player_count; slot++)
		team_deaths += campaign_score.state.players[slot].deaths;
	cleared_count = campaign_score_encounters_step(&campaign_score.encounters, units, encounters, count,
		campaign_score_unit_died, campaign_score_respawn_pending, team_deaths, cleared, NUMBEROF(cleared));
	campaign_score.died_count = 0;

	for (index = 0; index < cleared_count; index++)
	{
		struct campaign_score_encounter_clear const *clear = &cleared[index];
		long bonus = campaign_score_scaled(clear->bonus, campaign_score.state.difficulty_multiplier);
		long flawless = campaign_score_scaled(clear->flawless_bonus, campaign_score.state.difficulty_multiplier);
		struct player_datum *player;

		error(_error_log, "campaign score: encounter %d cleared, %d kills of %ld points: +%ld%s", clear->encounter,
			clear->kills, campaign_score.encounters.tallies.kill_points[clear->encounter], bonus,
			flawless ? ", flawless" : "");
		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL)
		{
			struct campaign_score_event event;

			(void)player;
			slot = campaign_score_slot(iterator.datum_index);
			if (slot == NONE)
				continue;
			event.player = slot;
			event.kind = _campaign_score_event_encounter_cleared;
			event.points = bonus;
			event.tick = game_time_get();
			event.victim_class = clear->kills;
			campaign_score_add_events(&event, 1);
			campaign_score.state.players[slot].score += bonus;
			if (flawless)
			{
				event.kind = _campaign_score_event_flawless;
				event.points = flawless;
				campaign_score_add_events(&event, 1);
				campaign_score.state.players[slot].score += flawless;
			}
			campaign_score.dirty[slot] = TRUE;
		}
	}
	if (cleared_count)
		campaign_score_team_total();
}

/* ---------- public code */

boolean campaign_score_enabled(
	void)
{
	return campaign_score.keeping || (campaign_score.heard && campaign_score.state.enabled);
}

struct campaign_score_state const *campaign_score_get(
	void)
{
	return &campaign_score.state;
}

char const *campaign_score_class_name(
	long victim_class)
{
	return VALID_INDEX(victim_class, NUMBER_OF_CAMPAIGN_SCORE_CLASSES) ? campaign_score_points[victim_class].name :
		campaign_score_points[_campaign_score_class_other].name;
}

boolean campaign_score_personal_best(
	long *score)
{
	short players;

	if (score)
		*score = 0;
	if (!global_scenario || global_scenario->type != _scenario_type_solo || !player_data)
		return FALSE;
	players = campaign_score_player_total();
	players = players > 0 ? players : 1;
	if (players != campaign_score_bests.cached_players)
	{
		campaign_score_bests.cached_players = players;
		campaign_score_bests.cached_index = campaign_score_bests_find(campaign_score_level_name(),
			game_difficulty_level_get(), players);
	}
	if (campaign_score_bests.cached_index == NONE)
		return FALSE;
	if (score)
		*score = campaign_score_bests.bests[campaign_score_bests.cached_index].score;
	return TRUE;
}

long campaign_score_events_since(
	long *cursor,
	struct campaign_score_event *out,
	long max)
{
	long first;
	long count = 0;

	if (!cursor)
		return 0;
	if (*cursor > campaign_score.event_sequence || *cursor < 0)
		*cursor = campaign_score.event_sequence;
	first = MAX(*cursor, campaign_score.event_sequence - CAMPAIGN_SCORE_EVENT_RING);
	while (first + count < campaign_score.event_sequence && count < max && out)
	{
		out[count] = campaign_score.events[(first + count) % CAMPAIGN_SCORE_EVENT_RING];
		count++;
	}
	*cursor = first + count;

	return count;
}

boolean campaign_score_level_summary(
	struct campaign_score_state *out,
	boolean *new_personal_best)
{
	if (out)
	{
		if (campaign_score.summary_valid)
			*out = campaign_score.summary;
		else
			csmemset(out, 0, sizeof(*out));
	}
	if (new_personal_best)
		*new_personal_best = campaign_score.summary_valid && campaign_score.summary_new_best;

	return campaign_score.summary_valid;
}

void campaign_score_new_map(
	void)
{
	short difficulty = game_difficulty_level_get();

	csmemset(&campaign_score.state, 0, sizeof(campaign_score.state));
	csmemset(campaign_score.timings, 0, sizeof(campaign_score.timings));
	csmemset(campaign_score.dirty, 0, sizeof(campaign_score.dirty));
	csmemset(campaign_score.deaths_since_checkpoint, 0, sizeof(campaign_score.deaths_since_checkpoint));
	csmemset(campaign_score.betrayals_since_checkpoint, 0, sizeof(campaign_score.betrayals_since_checkpoint));
	csmemset(campaign_score.betrayal_points_since_checkpoint, 0,
		sizeof(campaign_score.betrayal_points_since_checkpoint));
	csmemset(&campaign_score.encounters, 0, sizeof(campaign_score.encounters));
	csmemset(&campaign_score.encounters_checkpoint, 0, sizeof(campaign_score.encounters_checkpoint));
	campaign_score.died_count = 0;
	campaign_score_bests_read();
	campaign_score.heard = FALSE;
	campaign_score.level_over = FALSE;
	campaign_score.keeping = campaign_score_keeps();
	campaign_score.state.enabled = campaign_score.keeping;
	campaign_score.state.difficulty_multiplier = campaign_score_difficulty_multipliers[
		PIN(difficulty, 0, NUMBEROF(campaign_score_difficulty_multipliers) - 1)];
	campaign_score.checkpoint = campaign_score.state;
	campaign_score.events_sent = campaign_score.event_sequence;
	campaign_score.last_send_tick = 0;
	/* (all of it to the clients at the first tick) */
	campaign_score.last_refresh_tick = -CAMPAIGN_SCORE_REFRESH_TICKS;
	network_campaign_score_new_map();
}

void campaign_score_tick(
	void)
{
	long now;

	struct data_iterator iterator;

	if (!campaign_score.keeping || campaign_score.level_over)
		return;
	/* (every player of the game in the count, scored or not) */
	data_iterator_new(&iterator, player_data);
	while (data_iterator_next(&iterator))
		campaign_score_slot(iterator.datum_index);
	campaign_score_encounters_update();
	now = game_time_get();
	if (now - campaign_score.last_send_tick >= CAMPAIGN_SCORE_SEND_TICKS || now < campaign_score.last_send_tick)
		campaign_score_send(FALSE);
}

void campaign_score_unit_killed(
	long unit_index,
	struct damage_data const *damage,
	short damage_category,
	boolean head,
	boolean unsuspecting)
{
	struct unit_datum *unit;
	short victim_slot;
	short killer_slot;
	short driver_slot;
	short victim_class;

	if (!campaign_score.keeping || campaign_score.level_over || !damage)
		return;
	unit = unit_try_and_get(unit_index);
	if (!unit)
		return;
	/* (a death the encounters count, whoever's: its body may go at once) */
	if (campaign_score.died_count < CAMPAIGN_SCORE_MAX_DIED_PER_TICK)
		campaign_score.died_units[campaign_score.died_count++] = unit_index;
	victim_slot = campaign_score_slot(unit->unit.player_index);
	killer_slot = campaign_score_slot(damage->owner_player_index);
	victim_class = campaign_score_unit_class(unit);

	/* a player's death: theirs to count, and a betrayal of another player's */
	if (victim_slot != NONE)
	{
		struct campaign_score_player *victim = &campaign_score.state.players[victim_slot];
		long taken = campaign_score_player_death(victim, &campaign_score.timings[victim_slot]);

		campaign_score.deaths_since_checkpoint[victim_slot]++;
		campaign_score_deduction(victim_slot, _campaign_score_event_death, taken, victim_class);
		if (killer_slot != NONE && killer_slot != victim_slot)
			campaign_score_betrayal(killer_slot, _campaign_score_class_player);
		campaign_score_team_total();
		return;
	}
	/* (the player driving the killer's vehicle, a player's or an AI's kill) */
	driver_slot = campaign_score_assisting_driver(damage, unit);
	if (killer_slot == NONE && driver_slot == NONE)
		return;
	/* (an empty vehicle is no kill) */
	if (unit->object.type == _object_type_vehicle && unit->unit.driver_object_index == NONE &&
		unit->unit.gunner_object_index == NONE)
	{
		return;
	}
	/* an ally's: a player's betrayal (no assist) */
	if (damage->owner_team_index != NONE &&
		!game_team_is_enemy(unit->object.owner_team_index, damage->owner_team_index))
	{
		if (killer_slot != NONE)
		{
			campaign_score_betrayal(killer_slot, _campaign_score_class_marine);
			campaign_score_team_total();
		}
		return;
	}
	if (killer_slot != NONE)
	{
		struct campaign_score_event events[CAMPAIGN_SCORE_EVENTS_PER_KILL];
		long event_count = 0;
		short encounter = campaign_score_unit_encounter(unit);

		/* (its encounter's kills, for the encounter's bonus) */
		if (encounter != NONE && VALID_INDEX(victim_class, NUMBER_OF_CAMPAIGN_SCORE_CLASSES))
		{
			campaign_score.encounters.tallies.kill_points[encounter] += campaign_score_points[victim_class].points;
			campaign_score.encounters.tallies.kills[encounter]++;
		}
		campaign_score_player_kill(&campaign_score.state.players[killer_slot],
			&campaign_score.timings[killer_slot], killer_slot, game_time_get(), victim_class, damage_category, head,
			unsuspecting, campaign_score.state.difficulty_multiplier, events, &event_count);
		campaign_score_add_events(events, event_count);
		campaign_score.dirty[killer_slot] = TRUE;
	}
	/* the driver's assist (the kill's alone: no streak or multi-kill) */
	if (driver_slot != NONE && driver_slot != killer_slot)
	{
		struct campaign_score_event event;

		event.player = driver_slot;
		event.kind = _campaign_score_event_driver_assist;
		event.points = campaign_score_driver_assist_points(victim_class, campaign_score.state.difficulty_multiplier);
		event.tick = game_time_get();
		event.victim_class = victim_class;
		campaign_score.state.players[driver_slot].score += event.points;
		campaign_score_add_events(&event, 1);
		campaign_score.dirty[driver_slot] = TRUE;
	}
	campaign_score_team_total();
}

void campaign_score_projectile_stuck(
	long projectile_index,
	long object_index)
{
	struct object_datum *projectile;
	struct unit_datum *unit;
	struct campaign_score_event event;
	short slot;

	if (!campaign_score.keeping || campaign_score.level_over)
		return;
	projectile = object_try_and_get(projectile_index);
	unit = unit_try_and_get(object_index);
	if (!projectile || !unit || TEST_FLAG(unit->object.damage_flags, _object_dead_bit) ||
		unit->unit.player_index != NONE || !campaign_score_name_has(tag_get_name(projectile->definition_index),
			"grenade"))
	{
		return;
	}
	slot = campaign_score_slot(projectile->object.owner_player_index);
	if (slot == NONE || projectile->object.owner_team_index == NONE ||
		!game_team_is_enemy(unit->object.owner_team_index, projectile->object.owner_team_index))
	{
		return;
	}
	campaign_score.state.players[slot].sticks++;
	event.player = slot;
	event.kind = _campaign_score_event_stick;
	event.points = campaign_score_scaled(campaign_score_bonuses.stick, campaign_score.state.difficulty_multiplier);
	event.tick = game_time_get();
	event.victim_class = campaign_score_unit_class(unit);
	campaign_score.state.players[slot].score += event.points;
	campaign_score_add_events(&event, 1);
	campaign_score.dirty[slot] = TRUE;
	campaign_score_team_total();
}

void campaign_score_checkpoint_saved(
	void)
{
	if (!campaign_score.keeping)
		return;
	if (campaign_score.checkpoint.team_score != campaign_score.state.team_score)
	{
		error(_error_log, "campaign score: a checkpoint at %ld (player 1: %ld, %ld kills, %ld headshots)",
			campaign_score.state.team_score, campaign_score.state.players[0].score,
			campaign_score.state.players[0].kills, campaign_score.state.players[0].headshots);
	}
	campaign_score.checkpoint = campaign_score.state;
	campaign_score.encounters_checkpoint = campaign_score.encounters.tallies;
	csmemset(campaign_score.deaths_since_checkpoint, 0, sizeof(campaign_score.deaths_since_checkpoint));
	csmemset(campaign_score.betrayals_since_checkpoint, 0, sizeof(campaign_score.betrayals_since_checkpoint));
	csmemset(campaign_score.betrayal_points_since_checkpoint, 0,
		sizeof(campaign_score.betrayal_points_since_checkpoint));
}

void campaign_score_reverted(
	void)
{
	short slot;

	if (!campaign_score.keeping || campaign_score.level_over)
		return;
	campaign_score.state = campaign_score.checkpoint;
	/* (the encounters as the checkpoint had them: those cleared since may be
	cleared again; the next tick only looks at the units, another tick's) */
	campaign_score.encounters.tallies = campaign_score.encounters_checkpoint;
	campaign_score.encounters.rescan = TRUE;
	campaign_score.died_count = 0;
	/* (the deaths and betrayals since stay counted, and every player's
	streak ends: what was played since is played again) */
	for (slot = 0; slot < CAMPAIGN_SCORE_MAX_PLAYERS; slot++)
	{
		struct campaign_score_player *player = &campaign_score.state.players[slot];
		long deaths = campaign_score.deaths_since_checkpoint[slot];

		player->deaths += deaths;
		player->betrayals += campaign_score.betrayals_since_checkpoint[slot];
		campaign_score_player_deduct(player, deaths * campaign_score_bonuses.death +
			campaign_score.betrayal_points_since_checkpoint[slot]);
		player->current_streak = 0;
		campaign_score.timings[slot].multi_count = 0;
		campaign_score.dirty[slot] = slot < campaign_score.state.player_count;
	}
	campaign_score_checkpoint_saved();
	campaign_score_team_total();
}

void campaign_score_level_won(
	void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	if (!campaign_score.keeping || campaign_score.level_over)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		short slot = campaign_score_slot(iterator.datum_index);
		struct campaign_score_event event;

		(void)player;
		if (slot == NONE)
			continue;
		event.player = slot;
		event.kind = _campaign_score_event_level_bonus;
		event.points = campaign_score_scaled(campaign_score_bonuses.level_bonus +
			(campaign_score.state.players[slot].deaths == 0 ? campaign_score_bonuses.level_deathless_bonus : 0),
			campaign_score.state.difficulty_multiplier);
		event.tick = game_time_get();
		event.victim_class = _campaign_score_class_other;
		campaign_score.state.players[slot].score += event.points;
		campaign_score_add_events(&event, 1);
		campaign_score.dirty[slot] = TRUE;
	}
	campaign_score_team_total();
	campaign_score_summarize();
	campaign_score_send(TRUE);
	/* (the level's over: nothing more counts until the next) */
	campaign_score.level_over = TRUE;
}

/* ---------- public code: a client's word from its host */

void campaign_score_heard_state(
	boolean enabled,
	long team_score,
	short player_count,
	real difficulty_multiplier)
{
	if (campaign_score.keeping)
		return;
	campaign_score.heard = TRUE;
	campaign_score.state.enabled = enabled;
	campaign_score.state.team_score = team_score;
	campaign_score.state.player_count = (short)PIN(player_count, 0, CAMPAIGN_SCORE_MAX_PLAYERS);
	campaign_score.state.difficulty_multiplier = difficulty_multiplier;
}

void campaign_score_heard_player(
	short slot,
	struct campaign_score_player const *player)
{
	if (campaign_score.keeping || !VALID_INDEX(slot, CAMPAIGN_SCORE_MAX_PLAYERS))
		return;
	campaign_score.state.players[slot] = *player;
	if (slot >= campaign_score.state.player_count)
		campaign_score.state.player_count = (short)(slot + 1);
}

void campaign_score_heard_event(
	struct campaign_score_event const *event)
{
	if (campaign_score.keeping)
		return;
	campaign_score_add_events(event, 1);
}

void campaign_score_heard_level_end(
	void)
{
	if (campaign_score.keeping || !campaign_score.heard || !campaign_score.state.enabled ||
		campaign_score.level_over)
	{
		return;
	}
	campaign_score.level_over = TRUE;
	campaign_score_summarize();
}
