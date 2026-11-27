/*
CAMPAIGN_SCORE.C

Campaign scoring (campaign_score.h). A stub: no score yet.
*/

#include "cseries.h"
#include "campaign_score.h"

/* ---------- globals */

static struct campaign_score_state campaign_score_state;

/* ---------- public code */

boolean campaign_score_enabled(
	void)
{
	return FALSE;
}

struct campaign_score_state const *campaign_score_get(
	void)
{
	return &campaign_score_state;
}

long campaign_score_events_since(
	long *cursor,
	struct campaign_score_event *out,
	long max)
{
	(void)cursor;
	(void)out;
	(void)max;
	return 0;
}

boolean campaign_score_level_summary(
	struct campaign_score_state *out,
	boolean *new_personal_best)
{
	if (out)
		csmemset(out, 0, sizeof(*out));
	if (new_personal_best)
		*new_personal_best = FALSE;
	return FALSE;
}

void campaign_score_initialize(void) {}
void campaign_score_dispose(void) {}
void campaign_score_new_map(void) {}
void campaign_score_map_ended(void) {}
void campaign_score_tick(void) {}
void campaign_score_unit_died(long unit_index, struct damage_data const *damage) { (void)unit_index; (void)damage; }
void campaign_score_projectile_stuck(long projectile_index, long object_index) { (void)projectile_index; (void)object_index; }
void campaign_score_level_won(void) {}
void campaign_score_wave_cleared(long player_index) { (void)player_index; }
