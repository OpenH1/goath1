/* bots_royale_rules.h: the bots' Royale decisions as plain rules
(port/linux/game/bots_royale_rules.c)

The rules bots_royale.c asks each tick, apart from the game: plain numbers
in, a decision out, so that tools/test_bots_royale.py runs them without the
game. Nothing here keeps state or draws a random number of its own: a rule
that chooses is handed its random number (the bot's seeded one). */
#ifndef HALO_BOTS_ROYALE_RULES_H
#define HALO_BOTS_ROYALE_RULES_H

/* the play area on the ground (royale.h's bounds' x and y) */
struct bots_royale_box
{
	float minimum_x, minimum_y;
	float maximum_x, maximum_y;
};

/* the beam's sides, as royale.h's enum royale_side numbers them */
enum
{
	_bots_royale_side_north = 0, /* from +y */
	_bots_royale_side_east, /* from +x */
	_bots_royale_side_south, /* from -y */
	_bots_royale_side_west, /* from -x */
};

/* a margin when there is no beam (none drawn yet, or no side) */
#define BOTS_ROYALE_NO_BEAM 100000.0f

/* how far (world units) a point on the ground is ahead of a beam front a
fraction of the box's length in from its side: positive is ahead (safe),
negative behind (glassed). BOTS_ROYALE_NO_BEAM for side < 0 */
float bots_royale_beam_margin(short side, float fraction, struct bots_royale_box const *box, float x, float y);

/* the point nearest (x, y) at least margin ahead of that front: moved
straight inwards, kept a little inside the box */
void bots_royale_beam_refuge(short side, float fraction, struct bots_royale_box const *box, float margin,
	float *x, float *y);

/* ---- insertion */

/* the fraction of its Pelican's path at which a squad jumps: the squads
spread evenly along it by their number among those aboard (rank of count),
each moved within its share by its random number (0 to 1) */
float bots_royale_jump_fraction(short rank, short count, float random);

/* where a squad lands: its jump point (a fraction of the path from
(start_x, start_y) to (end_x, end_y)) moved to one side of the path by a
signed random number (-1 to 1) times spread, then kept inside the box */
void bots_royale_landing(float start_x, float start_y, float end_x, float end_y, float fraction,
	float side_random, float spread, struct bots_royale_box const *box, float *x, float *y);

/* a Pelican's way over the box: where the line through (x, y) along
(velocity_x, velocity_y) goes in and out of it; FALSE when it does not
cross it, or stands still */
int bots_royale_chord(struct bots_royale_box const *box, float x, float y, float velocity_x, float velocity_y,
	float *start_x, float *start_y, float *end_x, float *end_y);

/* how far a point (x, y) is along the path from start to end (0 at start,
1 at end; beyond, outside 0 to 1) */
float bots_royale_path_progress(float start_x, float start_y, float end_x, float end_y, float x, float y);

/* a squad jumps: over the island (its Pelican inside the bounds), at its
fraction of the way or with the island nearly passed */
int bots_royale_should_jump(float progress, float fraction, int over_island);

/* steering a free fall to (to_x, to_y) from (x, y): the way to face
(radians) and the throttle (0 to 1: easing off within a few units) */
void bots_royale_fall_steer(float x, float y, float to_x, float to_y, float *yaw, float *throttle);

/* ---- play */

enum
{
	/* bots.c's own play: fights, weapons and health near, its chase */
	_bots_royale_decide_default = 0,
	/* behind (or nearly) the beam: inwards before all else, fighting or not */
	_bots_royale_decide_flee_beam,
	/* the beam will move past it soon: inwards, still fighting */
	_bots_royale_decide_move_in,
	/* its squadmate down and no enemy about: to them, the action held */
	_bots_royale_decide_revive,
	/* too far from its squadmate: back to them */
	_bots_royale_decide_regroup,
	/* the gulag: at its opponent */
	_bots_royale_decide_gulag,
	NUMBER_OF_BOTS_ROYALE_DECISIONS
};

/* what a bot sees of its Royale game this tick */
struct bots_royale_view
{
	int in_gulag;
	/* how far ahead of the beam's front it is now, and of where the front
	stops this stage (BOTS_ROYALE_NO_BEAM without a beam) */
	float margin_now;
	float margin_target;
	/* the front moving now, or seconds until it next moves */
	int beam_moving;
	float seconds_to_move;
	/* its squadmate: in the game and standing, downed (and how far, and
	how far ahead of the beam they are) */
	int mate_standing;
	int mate_downed;
	float mate_distance;
	float mate_margin_now;
	/* it has an enemy in sight; seconds since it last saw or was hurt by
	one */
	int fighting;
	float seconds_since_enemy;
};

/* the margins the rules keep (world units) and their times (seconds) */
#define BOTS_ROYALE_FLEE_MARGIN 6.0f
#define BOTS_ROYALE_MOVE_IN_MARGIN 12.0f
#define BOTS_ROYALE_MOVE_IN_SECONDS 30.0f
#define BOTS_ROYALE_REFUGE_MARGIN 20.0f
#define BOTS_ROYALE_REVIVE_CALM_SECONDS 3.0f
#define BOTS_ROYALE_REGROUP_DISTANCE 30.0f

short bots_royale_decide(struct bots_royale_view const *view);
/* the decision keeps to its way while it fights (bots.c's goal in the fight) */
int bots_royale_decision_in_fight(short decision);
/* a ground weapon's worth more for its loot tier (royale.h's
enum royale_loot_tier, NONE: not loot), in bots.c's weapon values */
short bots_royale_tier_value(short tier);

#endif
