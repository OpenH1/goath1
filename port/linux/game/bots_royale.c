/*
BOTS_ROYALE.C

The bots in a Royale game (bots_royale.h). bots.c plays a standing bot as
in Slayer; here is what Royale changes, by the rules of bots_royale_rules.c:

- Insertion: each squad jumps at a point of its own along its Pelican's
  path (the squads aboard spread along it), drawn from the game's seed and
  its squad's number, so that both of a squad jump and land together; the
  fall is steered to the landing, beside the path and ahead of the beam.
- Play: a bot fights only another squad's player who stands on the island
  as it does (not one aboard, falling or in the gulag); it goes for better
  weapons, a higher loot tier worth more; it keeps ahead of the beam's front
  (out from behind it before all else, fighting or not, and inwards in time
  when the front is about to move on to it); it revives its squadmate when
  no enemy has been about for a while; it goes back to a squadmate it has
  strayed from; and with nothing to do it roams the loot ahead of the beam.
- Gulag: its opponent is its one enemy, and its goal.
- Downed: it crawls to its squadmate (out of the beam first).

Only the host runs the bots (bots.c), from the game state and the game's
seed: the same game, the same bots. royale_bots_tick, which every machine
runs, only keeps the tally of halo.log's lines: each player out, by the
beam or in a fight (where they were as they went down), and the game's end.
*/

#include <math.h>

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "units/units.h"
#include "networking/network_game_globals.h"
#include "tag_files/tag_groups.h"

#include "royale.h"
#include "bots.h"
#include "bots_royale.h"
#include "bots_royale_rules.h"

void platform_log(char const *format, ...);

/* (a landing this far, at most, to one side of the Pelican's path) */
#define BOTS_ROYALE_LANDING_SPREAD 40.0f
/* (squadmates land this far apart) */
#define BOTS_ROYALE_MATE_OFFSET 3.0f
/* (the squads aboard a Pelican jump in this many shares of its way) */
#define BOTS_ROYALE_JUMP_SHARES 4
/* (the loot kinds whose tiers are kept) */
#define BOTS_ROYALE_TIER_KINDS 64
/* (a power-up within this is gone for; health when hurt) */
#define BOTS_ROYALE_PICKUP_DISTANCE 25.0f
/* (a roam spot reached within this) */
#define BOTS_ROYALE_ROAM_REACHED 3.0f

struct bots_royale_bot
{
	unsigned long random;
	/* its squad's jump: the path, the fraction, the landing (planned) */
	boolean planned;
	real path_start_x, path_start_y, path_end_x, path_end_y;
	real jump_fraction;
	real landing_x, landing_y;
	/* the loot spot it roams to (NONE: none yet) */
	short roam_spot;
};

static struct
{
	struct royale_map_config const *config;
	struct bots_royale_box box;
	/* (and debug.bot_player's, the first player's own bot: BOTS_MAXIMUM) */
	struct bots_royale_bot bots[BOTS_MAXIMUM + 1];
	/* the tally (every machine): each player's state last tick, and
	whether they went down in the beam */
	signed char last_state[ROYALE_MAXIMUM_PLAYERS * 2];
	boolean down_in_beam[ROYALE_MAXIMUM_PLAYERS * 2];
	long out_by_beam, out_by_combat;
	boolean reported;
	/* each loot kind's tier, as the ground's items told it (the loot lane
	tells only an item lying there: a weapon in hand is worth its kind's) */
	long tier_definitions[BOTS_ROYALE_TIER_KINDS];
	short tier_values[BOTS_ROYALE_TIER_KINDS];
	short tier_kind_count;
} bots_royale;

/* ---------- small helpers */

static unsigned long bots_royale_mix(unsigned long x)
{
	/* (xorshift32, never at 0) */
	x &= 0xFFFFFFFFUL;
	if (!x)
		x = 0x9E3779B9UL;
	x ^= x << 13;
	x &= 0xFFFFFFFFUL;
	x ^= x >> 17;
	x ^= x << 5;
	return x & 0xFFFFFFFFUL;
}

static real bots_royale_unit_random(unsigned long *state)
{
	*state = bots_royale_mix(*state);
	return (real)(*state & 0xFFFFFF) / (real)0x1000000;
}

static real bots_royale_seconds(long ticks)
{
	long second = royale_seconds_to_ticks(1.0f);

	return (real)ticks / (real)(second > 0 ? second : TICKS_PER_SECOND);
}

static boolean bots_royale_player_position(long player_index, real_point3d *position)
{
	struct player_datum *player;

	if (player_index == NONE)
		return FALSE;
	player = player_get(player_index);
	if (player->unit_index == NONE)
		return FALSE;
	*position = object_get(player->unit_index)->object.position;
	return TRUE;
}

static real bots_royale_distance2d(real_point3d const *a, real_point3d const *b)
{
	real dx = a->x - b->x, dy = a->y - b->y;

	return (real)sqrt(dx * dx + dy * dy);
}

/* the beam's margins of a point: now, and where the front stops */
static real bots_royale_margin(real_point3d const *point, boolean target)
{
	short side = royale_beam_side();

	if (!bots_royale.config || side < 0)
		return BOTS_ROYALE_NO_BEAM;
	return bots_royale_beam_margin(side, target ? royale_beam_target() : royale_beam_front(), &bots_royale.box,
		point->x, point->y);
}

static struct bots_royale_bot *bots_royale_bot(long bot_index)
{
	struct bots_royale_bot *bot;

	if (bot_index < 0 || bot_index > BOTS_MAXIMUM)
		return NULL;
	bot = &bots_royale.bots[bot_index];
	if (!bot->random)
	{
		bot->random = bots_royale_mix(0x51ED270BUL * (unsigned long)(bot_index + 1) ^
			(unsigned long)network_game_get_random_seed());
		bot->roam_spot = NONE;
	}
	return bot;
}

/* ---------- the lane's calls (core) */

void royale_bots_start(struct royale_map_config const *config)
{
	csmemset(&bots_royale, 0, sizeof(bots_royale));
	csmemset(bots_royale.last_state, ROYALE_PLAYER_NONE, sizeof(bots_royale.last_state));
	bots_royale.config = config;
	if (config)
	{
		bots_royale.box.minimum_x = config->bounds.minimum.x;
		bots_royale.box.minimum_y = config->bounds.minimum.y;
		bots_royale.box.maximum_x = config->bounds.maximum.x;
		bots_royale.box.maximum_y = config->bounds.maximum.y;
	}
}

void royale_bots_tick(void)
{
	struct data_iterator iterator;
	struct player_datum *player;

	if (!royale_active() || !bots_royale.config)
		return;
	/* the tally: who went down where, and who is out */
	data_iterator_new(&iterator, player_data);
	while ((player = (struct player_datum *)data_iterator_next(&iterator)) != NULL)
	{
		long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		enum royale_player_state state;
		enum royale_player_state last;

		if (slot < 0 || slot >= (long)NUMBEROF(bots_royale.last_state))
			continue;
		state = royale_player_state(iterator.datum_index);
		last = (enum royale_player_state)bots_royale.last_state[slot];
		if (state != last)
		{
			if (last == ROYALE_PLAYER_ALIVE && state != ROYALE_PLAYER_ALIVE)
			{
				real_point3d position;

				/* (where they stood at their last tick standing: the unit
				may be gone) */
				bots_royale.down_in_beam[slot] = bots_royale_player_position(iterator.datum_index, &position) &&
					bots_royale_margin(&position, FALSE) < 0.0f;
			}
			if ((state == ROYALE_PLAYER_SPECTATING || state == ROYALE_PLAYER_ELIMINATED) &&
				last != ROYALE_PLAYER_SPECTATING && last != ROYALE_PLAYER_ELIMINATED)
			{
				if (bots_royale.down_in_beam[slot])
					bots_royale.out_by_beam++;
				else
					bots_royale.out_by_combat++;
				platform_log("royale bots: player %ld out (%s) at %.1fs, squads alive %d",
					slot, bots_royale.down_in_beam[slot] ? "beam" : "combat",
					bots_royale_seconds(royale_match_time()), (int)royale_squads_alive());
			}
			bots_royale.last_state[slot] = (signed char)state;
		}
	}
	if (royale_phase() == ROYALE_PHASE_RESULTS && !bots_royale.reported)
		bots_royale_report();
}

/* a player's slot (absolute index) for the log, NONE for none */
static long bots_royale_slot(long player_index)
{
	return player_index == NONE ? NONE : (long)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
}

void bots_royale_report(void)
{
	short winner = royale_winning_squad();

	bots_royale.reported = TRUE;
	platform_log("royale bots: match over: squad %d won (players %ld and %ld), %.1fs, out by the beam %ld, "
		"in combat %ld, squads %d",
		(int)winner, bots_royale_slot(winner != NONE ? royale_squad_member(winner, 0) : NONE),
		bots_royale_slot(winner != NONE ? royale_squad_member(winner, 1) : NONE),
		bots_royale_seconds(royale_match_time()),
		bots_royale.out_by_beam, bots_royale.out_by_combat, (int)royale_squad_count());
}

/* ---------- insertion */

/* its squad's jump: the path nearest its Pelican, its squad's share of
it, the landing beside it (ahead of the beam's first stage) */
static boolean bots_royale_plan_jump(struct bots_royale_bot *bot, long player_index, real_point3d const *pelican,
	real_vector3d const *velocity)
{
	short squad = royale_squad_of(player_index);
	unsigned long squad_random = bots_royale_mix(0xA511E9B3UL * (unsigned long)(squad + 2) ^
		(unsigned long)network_game_get_random_seed());
	real fraction_random, side_random;

	/* (its Pelican's way over the island: where its line crosses the
	bounds; not yet moving, or not crossing it: no plan yet) */
	if (!bots_royale_chord(&bots_royale.box, pelican->x, pelican->y, velocity->i, velocity->j,
		&bot->path_start_x, &bot->path_start_y, &bot->path_end_x, &bot->path_end_y))
	{
		return FALSE;
	}
	fraction_random = bots_royale_unit_random(&squad_random);
	side_random = bots_royale_unit_random(&squad_random) * 2.0f - 1.0f;
	/* (the squads aboard, numbered together, spread over its shares) */
	bot->jump_fraction = bots_royale_jump_fraction(squad < 0 ? (short)0 : (short)(squad % BOTS_ROYALE_JUMP_SHARES),
		BOTS_ROYALE_JUMP_SHARES, fraction_random);
	bots_royale_landing(bot->path_start_x, bot->path_start_y, bot->path_end_x, bot->path_end_y, bot->jump_fraction,
		side_random, BOTS_ROYALE_LANDING_SPREAD, &bots_royale.box, &bot->landing_x, &bot->landing_y);
	/* (squadmates a little apart: its seat's side) */
	if (royale_squadmate_of(player_index) != NONE && player_index > royale_squadmate_of(player_index))
		bot->landing_x += BOTS_ROYALE_MATE_OFFSET;
	bot->planned = TRUE;
	return TRUE;
}

/* the jump button pressed and let go in turn (a press, not a hold) */
static unsigned long bots_royale_press(unsigned long bit)
{
	return ((game_time_get() / 4) & 1) ? bit : 0;
}

boolean bots_royale_control(long bot_index, long player_index, struct bots_royale_control *control)
{
	struct bots_royale_bot *bot = bots_royale_bot(bot_index);
	enum royale_player_state state;
	real_point3d position;

	if (!royale_active() || !bots_royale.config || !bot)
		return FALSE;
	state = royale_player_state(player_index);
	switch (state)
	{
	case ROYALE_PLAYER_ALIVE:
	case ROYALE_PLAYER_GULAG:
	case ROYALE_PLAYER_NONE:
		/* (between drops: its next jump planned afresh) */
		if (state == ROYALE_PLAYER_ALIVE)
			bot->planned = FALSE;
		return FALSE;

	case ROYALE_PLAYER_IN_PELICAN:
	{
		real_vector3d velocity;
		real progress;

		if (!royale_insertion_pelican_position(player_index, &position, &velocity))
			return TRUE;
		if (!bot->planned && !bots_royale_plan_jump(bot, player_index, &position, &velocity))
			return TRUE;
		progress = bots_royale_path_progress(bot->path_start_x, bot->path_start_y, bot->path_end_x, bot->path_end_y,
			position.x, position.y);
		control->yaw = (real)atan2(bot->landing_y - position.y, bot->landing_x - position.x);
		control->pitch = -0.6f;
		/* (at its point over the island, or the island nearly passed) */
		if (bots_royale_should_jump(progress, bot->jump_fraction,
			position.x >= bots_royale.box.minimum_x && position.x <= bots_royale.box.maximum_x &&
			position.y >= bots_royale.box.minimum_y && position.y <= bots_royale.box.maximum_y))
		{
			control->flags |= bots_royale_press(FLAG(_unit_control_jump_bit));
		}
		return TRUE;
	}

	case ROYALE_PLAYER_FALLING:
	{
		real yaw, throttle, landing_x = bot->landing_x, landing_y = bot->landing_y;

		if (!bots_royale_player_position(player_index, &position))
			return TRUE;
		if (!bot->planned)
		{
			/* (out without a plan: where it is, ahead of the beam) */
			bot->landing_x = position.x;
			bot->landing_y = position.y;
			bot->planned = TRUE;
			landing_x = position.x;
			landing_y = position.y;
		}
		if (royale_beam_side() >= 0)
		{
			bots_royale_beam_refuge(royale_beam_side(), royale_beam_target(), &bots_royale.box,
				BOTS_ROYALE_REFUGE_MARGIN, &landing_x, &landing_y);
		}
		bots_royale_fall_steer(position.x, position.y, landing_x, landing_y, &yaw, &throttle);
		control->yaw = yaw;
		control->pitch = -0.8f;
		control->moving = throttle > 0.05f;
		control->move_yaw = yaw;
		control->throttle = throttle;
		return TRUE;
	}

	case ROYALE_PLAYER_DOWNED:
	{
		real_point3d goal;
		long mate = royale_squadmate_of(player_index);

		if (!bots_royale_player_position(player_index, &position))
			return TRUE;
		goal = position;
		/* (out of the beam first, then to its squadmate) */
		if (bots_royale_margin(&position, FALSE) < BOTS_ROYALE_FLEE_MARGIN)
		{
			bots_royale_beam_refuge(royale_beam_side(), royale_beam_front(), &bots_royale.box,
				BOTS_ROYALE_REFUGE_MARGIN, &goal.x, &goal.y);
		}
		else if (mate != NONE && royale_player_state(mate) == ROYALE_PLAYER_ALIVE)
			bots_royale_player_position(mate, &goal);
		if (bots_royale_distance2d(&goal, &position) > 0.4f)
		{
			control->yaw = (real)atan2(goal.y - position.y, goal.x - position.x);
			control->moving = TRUE;
			control->move_yaw = control->yaw;
			control->throttle = 1.0f;
		}
		return TRUE;
	}

	default:
		/* (spectating, out: nothing) */
		return TRUE;
	}
}

/* ---------- play */

/* the point the beam's rule sends it to: the loot spot nearest it well
ahead of where the front stops (a place the paths reach), else straight in */
static void bots_royale_refuge_point(real_point3d const *position, real_point3d *point)
{
	struct royale_map_config const *config = bots_royale.config;
	short index, best = NONE;
	real best_distance = REAL_MAX;
	real_point3d straight = *position;

	for (index = 0; index < config->loot_spot_count; index++)
	{
		real_point3d const *spot = &config->loot_spots[index].position;
		real distance;

		if (bots_royale_margin(spot, TRUE) < BOTS_ROYALE_REFUGE_MARGIN)
			continue;
		distance = bots_royale_distance2d(spot, position);
		if (distance < best_distance)
		{
			best_distance = distance;
			best = index;
		}
	}
	bots_royale_beam_refuge(royale_beam_side(), royale_beam_target(), &bots_royale.box, BOTS_ROYALE_REFUGE_MARGIN,
		&straight.x, &straight.y);
	/* (a spot no further than half again the straight way in) */
	if (best != NONE && best_distance < 1.5f * bots_royale_distance2d(position, &straight) + 10.0f)
	{
		*point = config->loot_spots[best].position;
		return;
	}
	*point = straight;
}

void bots_royale_goal(long bot_index, long player_index, real_point3d const *position, boolean fighting,
	long ticks_since_enemy, struct bots_royale_goal *goal)
{
	struct bots_royale_view view;
	long mate;
	real_point3d mate_position;
	boolean mate_found;
	long ticks_to_move;

	csmemset(goal, 0, sizeof(*goal));
	goal->decision = _bots_royale_decide_default;
	if (!royale_active() || !bots_royale.config || !bots_royale_bot(bot_index))
		return;
	csmemset(&view, 0, sizeof(view));
	view.in_gulag = royale_player_state(player_index) == ROYALE_PLAYER_GULAG;
	view.margin_now = bots_royale_margin(position, FALSE);
	view.margin_target = bots_royale_margin(position, TRUE);
	ticks_to_move = royale_beam_ticks_to_move();
	view.beam_moving = royale_beam_side() >= 0 && ticks_to_move <= 0;
	view.seconds_to_move = bots_royale_seconds(ticks_to_move);
	mate = royale_squadmate_of(player_index);
	mate_found = mate != NONE && bots_royale_player_position(mate, &mate_position);
	if (mate_found)
	{
		enum royale_player_state mate_state = royale_player_state(mate);

		view.mate_standing = mate_state == ROYALE_PLAYER_ALIVE;
		view.mate_downed = mate_state == ROYALE_PLAYER_DOWNED;
		view.mate_distance = bots_royale_distance2d(&mate_position, position);
		view.mate_margin_now = bots_royale_margin(&mate_position, FALSE);
	}
	view.fighting = fighting;
	view.seconds_since_enemy = bots_royale_seconds(ticks_since_enemy);
	goal->decision = bots_royale_decide(&view);
	goal->in_fight = bots_royale_decision_in_fight(goal->decision);
	switch (goal->decision)
	{
	case _bots_royale_decide_flee_beam:
	case _bots_royale_decide_move_in:
		bots_royale_refuge_point(position, &goal->point);
		break;
	case _bots_royale_decide_revive:
	{
		real distance = view.mate_distance;

		goal->point = mate_position;
		goal->direct = distance < 2.5f;
		goal->hold_action = distance <= ROYALE_REVIVE_RADIUS + 0.3f;
		break;
	}
	case _bots_royale_decide_regroup:
		goal->point = mate_position;
		break;
	case _bots_royale_decide_gulag:
	{
		long opponent = royale_gulag_opponent(player_index);

		/* (no opponent yet: it waits where it is, not roaming off to the
		island's loot) */
		if (opponent == NONE || !bots_royale_player_position(opponent, &goal->point))
			goal->point = *position;
		break;
	}
	default:
		break;
	}
}

boolean bots_royale_roam(long bot_index, real_point3d const *position, real_point3d *point)
{
	struct bots_royale_bot *bot = bots_royale_bot(bot_index);
	struct royale_map_config const *config = bots_royale.config;
	short tries, best = NONE;
	real best_score = REAL_MAX;

	if (!royale_active() || !config || !bot || config->loot_spot_count <= 0)
		return FALSE;
	/* (of eight spots drawn, the nearest well ahead of the beam's stop and
	not where it stands) */
	for (tries = 0; tries < 8; tries++)
	{
		short index = (short)((bots_royale_unit_random(&bot->random) * (real)config->loot_spot_count));
		real_point3d const *spot;
		real distance;

		if (index >= config->loot_spot_count)
			index = (short)(config->loot_spot_count - 1);
		spot = &config->loot_spots[index].position;
		distance = bots_royale_distance2d(spot, position);
		if (index == bot->roam_spot || distance < BOTS_ROYALE_ROAM_REACHED ||
			bots_royale_margin(spot, TRUE) < BOTS_ROYALE_REFUGE_MARGIN)
		{
			continue;
		}
		if (distance < best_score)
		{
			best_score = distance;
			best = index;
		}
	}
	if (best == NONE)
		return FALSE;
	bot->roam_spot = best;
	*point = config->loot_spots[best].position;
	return TRUE;
}

boolean bots_royale_is_enemy(long player_index, long other_index)
{
	enum royale_player_state state, other_state;
	short squad;

	if (player_index == other_index || other_index == NONE)
		return FALSE;
	state = royale_player_state(player_index);
	other_state = royale_player_state(other_index);
	/* (the gulag: its opponent only) */
	if (state == ROYALE_PLAYER_GULAG)
	{
		long opponent = royale_gulag_opponent(player_index);

		return opponent != NONE && DATUM_INDEX_TO_ABSOLUTE_INDEX(opponent) == DATUM_INDEX_TO_ABSOLUTE_INDEX(other_index);
	}
	if (state != ROYALE_PLAYER_ALIVE)
		return FALSE;
	squad = royale_squad_of(player_index);
	if (squad != NONE && squad == royale_squad_of(other_index))
		return FALSE;
	/* (another squad's, on the island: standing, or down and to finish) */
	return other_state == ROYALE_PLAYER_ALIVE || other_state == ROYALE_PLAYER_DOWNED;
}

short bots_royale_item_value(long object_index)
{
	struct object_datum *object;
	short tier, kind;

	if (!royale_active())
		return 0;
	object = (struct object_datum *)object_try_and_get_and_verify_type(object_index,
		_object_mask_weapon | _object_mask_equipment);
	if (!object)
		return 0;
	tier = royale_loot_item_tier(object_index);
	for (kind = 0; kind < bots_royale.tier_kind_count; kind++)
	{
		if (bots_royale.tier_definitions[kind] == object->definition_index)
		{
			if (tier == NONE)
				tier = bots_royale.tier_values[kind];
			break;
		}
	}
	if (tier != NONE && kind == bots_royale.tier_kind_count && kind < BOTS_ROYALE_TIER_KINDS)
	{
		bots_royale.tier_definitions[kind] = object->definition_index;
		bots_royale.tier_values[kind] = tier;
		bots_royale.tier_kind_count++;
	}
	return bots_royale_tier_value(tier);
}

long bots_royale_find_pickup(long player_index, real_point3d const *position, real body, boolean fighting)
{
	struct object_iterator iterator;
	long best_index = NONE;
	real best_distance = fighting ? 8.0f : BOTS_ROYALE_PICKUP_DISTANCE;

	if (!royale_active() || !bots_royale.config)
		return NONE;
	object_iterator_new(&iterator, _object_mask_equipment, 0);
	while (object_iterator_next(&iterator))
	{
		struct object_datum *object = object_get(iterator.index);
		char const *name = tag_get_name(object->definition_index);
		real distance;

		if (object->object.parent_object_index != NONE || !name ||
			!TEST_FLAG(object->object.flags, _object_connected_to_map_bit))
		{
			continue;
		}
		/* (health only hurt; the power-ups, camouflage and the overshield,
		always; nothing else: grenades are walked over) */
		if (strstr(name, "health"))
		{
			if (body >= 0.75f)
				continue;
		}
		else if (!strstr(name, "camo") && !strstr(name, "shield"))
			continue;
		if (!bots_royale_item_allowed(player_index, &object->object.position))
			continue;
		distance = bots_royale_distance2d(&object->object.position, position);
		if (distance < best_distance)
		{
			best_distance = distance;
			best_index = iterator.index;
		}
	}
	return best_index;
}

boolean bots_royale_item_allowed(long player_index, real_point3d const *point)
{
	struct royale_bounds const *arena;

	if (!royale_active() || !bots_royale.config)
		return TRUE;
	if (royale_player_state(player_index) != ROYALE_PLAYER_GULAG)
		return bots_royale_point_safe(point);
	arena = &bots_royale.config->gulag.bounds;
	return point->x >= arena->minimum.x && point->x <= arena->maximum.x && point->y >= arena->minimum.y &&
		point->y <= arena->maximum.y;
}

boolean bots_royale_point_safe(real_point3d const *point)
{
	if (!royale_active() || !bots_royale.config)
		return TRUE;
	return bots_royale_margin(point, FALSE) >= BOTS_ROYALE_FLEE_MARGIN + 2.0f &&
		bots_royale_margin(point, TRUE) >= 0.0f;
}

/* (royale.h) no config, no tally */
void royale_bots_reset(void)
{
	royale_bots_start(NULL);
}
