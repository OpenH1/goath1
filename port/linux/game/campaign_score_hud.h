/*
CAMPAIGN_SCORE_HUD.H

What campaign scoring shows (campaign_score_hud.c): the pop-ups, the
level's summary, and the scores the scoreboard (game_engine.c) and the
Spartan link (spartan_link.c) draw. It reads campaign_score.h only.
*/

#ifndef __CAMPAIGN_SCORE_HUD_H
#define __CAMPAIGN_SCORE_HUD_H

#include "campaign_score.h"

/* a group of a player's events close together, as one pop-up shows them
("+150 Triple Kill"): its player (an absolute index), its points, its words
and when its last event came (system milliseconds) */
struct campaign_score_hud_group
{
	short player;
	long points;
	wchar_t text[32];
	unsigned long first_at;
	unsigned long last_at;
};

/* whether this game shows a score: a campaign game with scoring on (or
debug.score_fill's made-up one) */
boolean campaign_score_hud_active(void);
/* the score shown (the game's, or debug.score_fill's); never NULL */
struct campaign_score_state const *campaign_score_hud_state(void);
/* a player's (a player datum index) entry of the score shown, else NULL */
struct campaign_score_player const *campaign_score_hud_player(long player_index);
/* a number with its thousands marked ("12,450") */
void campaign_score_hud_number(long value, wchar_t *text, short size);
/* a player's latest groups (newest first) that came within the last
milliseconds, at most maximum: their count */
short campaign_score_hud_recent(long player_index, unsigned long milliseconds,
	struct campaign_score_hud_group *groups, short maximum);

/* the scoreboard's header (game_engine.c), the summary's: the title and
difficulty, the total large, the best to beat; from top between left and
right, in the view's units, drawn if draw; its height returned */
real campaign_score_hud_board_header(short local_player_index, real left, real right, real top, real alpha,
	boolean draw);

/* render.c, after the Spartan link: the window's pop-ups and the level's
summary */
void campaign_score_hud_draw(short local_player_index);

#endif
