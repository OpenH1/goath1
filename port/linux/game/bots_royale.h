/* bots_royale.h: the bots in a Royale game (port/linux/game/bots_royale.c)

What bots.c asks while royale_active(): who is an enemy (another squad's
player standing in the same place: the island, or the bot's gulag), whether
Royale has the bot's controls this tick (aboard a Pelican, falling, downed,
out), the goal Royale puts first (the beam, a squadmate down or strayed, the
gulag's opponent), where to roam (the loot ahead of the beam), what a
weapon's loot tier adds to its worth, and whether a place is clear of the
beam. The rules themselves are bots_royale_rules.h's. Needs cseries.h and
math/real_math.h. */
#ifndef HALO_BOTS_ROYALE_H
#define HALO_BOTS_ROYALE_H

/* a bot's controls while Royale has them (bots_royale_control) */
struct bots_royale_control
{
	/* where it looks (radians) */
	real yaw;
	real pitch;
	/* it moves that way (radians, the world's), so hard (0 to 1) */
	boolean moving;
	real move_yaw;
	real throttle;
	/* its buttons (unit_control bits) */
	unsigned long flags;
};

/* Royale has the bot's controls this tick (aboard, falling, downed, out):
control filled; FALSE: bots.c plays it (standing, or in the gulag) */
boolean bots_royale_control(long bot_index, long player_index, struct bots_royale_control *control);

/* the goal Royale puts first (bots_royale_rules.h's decision; the default:
none, bots.c's own play), its point, whether bots.c keeps to it fighting,
and, close to a downed squadmate, the action held (hold_action) and the
last steps walked straight at the point (direct) */
struct bots_royale_goal
{
	short decision;
	real_point3d point;
	boolean in_fight;
	boolean hold_action;
	boolean direct;
};
void bots_royale_goal(long bot_index, long player_index, real_point3d const *position, boolean fighting,
	long ticks_since_enemy, struct bots_royale_goal *goal);

/* where it roams with nothing better to do: a loot spot ahead of the beam;
FALSE for none (bots.c's own roaming) */
boolean bots_royale_roam(long bot_index, real_point3d const *position, real_point3d *point);

/* a player (datum index) is the bot's enemy now */
boolean bots_royale_is_enemy(long player_index, long other_index);
/* a ground weapon's worth more for its loot tier (bots.c's values) */
short bots_royale_item_value(long object_index);
/* a power-up (camouflage, the overshield) or, its body hurt (body: its
vitality, 0 to 1), a health pack lying near and clear of the beam, NONE for
none */
long bots_royale_find_pickup(long player_index, real_point3d const *position, real body, boolean fighting);
/* a place is clear of the beam now (an item there worth going for) */
boolean bots_royale_point_safe(real_point3d const *point);
/* an item there is one the player may go for: clear of the beam, and in
the gulag, inside its arena */
boolean bots_royale_item_allowed(long player_index, real_point3d const *point);

/* halo.log's line of a Royale game's end, from bots.c's minute line */
void bots_royale_report(void);

#endif
