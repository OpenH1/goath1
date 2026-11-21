/*
ROYALE_LANES_STUB.C

Each Royale lane's calls as weak no-ops, so the game links before (and
without) a lane's own royale_<lane>.c: a lane's definitions replace these.
*/

#include "royale.h"

#define ROYALE_WEAK __attribute__((weak))

ROYALE_WEAK void royale_loot_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_loot_tick(void) {}
ROYALE_WEAK void royale_loot_draw(void) {}

ROYALE_WEAK void royale_beam_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_beam_tick(void) {}
ROYALE_WEAK void royale_beam_draw(void) {}

ROYALE_WEAK void royale_insertion_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_insertion_tick(void) {}
ROYALE_WEAK void royale_insertion_draw(void) {}

ROYALE_WEAK void royale_gulag_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_gulag_tick(void) {}
ROYALE_WEAK void royale_gulag_draw(void) {}
ROYALE_WEAK boolean royale_gulag_player_died(long player_index, long killer_player_index)
{
	(void)player_index;
	(void)killer_player_index;
	return FALSE;
}
ROYALE_WEAK boolean royale_gulag_spawn_point(long player_index, real_point3d *position, real_vector3d *facing)
{
	(void)player_index;
	(void)position;
	(void)facing;
	return FALSE;
}
ROYALE_WEAK void royale_gulag_postspawn(long player_index) { (void)player_index; }
ROYALE_WEAK long royale_gulag_spectate_unit(long player_index) { (void)player_index; return NONE; }

ROYALE_WEAK void royale_map_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_map_tick(void) {}

ROYALE_WEAK void royale_bots_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_bots_tick(void) {}

ROYALE_WEAK void royale_scale_start(struct royale_map_config const *config) { (void)config; }
ROYALE_WEAK void royale_scale_tick(void) {}

ROYALE_WEAK void royale_map_reset(void) {}
ROYALE_WEAK void royale_loot_reset(void) {}
ROYALE_WEAK void royale_insertion_reset(void) {}
ROYALE_WEAK void royale_beam_reset(void) {}
ROYALE_WEAK void royale_gulag_reset(void) {}
ROYALE_WEAK void royale_bots_reset(void) {}
ROYALE_WEAK void royale_scale_reset(void) {}

ROYALE_WEAK short royale_loot_item_tier(long object_index) { (void)object_index; return NONE; }

ROYALE_WEAK short royale_beam_side(void) { return NONE; }
ROYALE_WEAK real royale_beam_front(void) { return 0.0f; }
ROYALE_WEAK real royale_beam_target(void) { return 0.0f; }
ROYALE_WEAK long royale_beam_ticks_to_move(void) { return 0; }

ROYALE_WEAK boolean royale_insertion_pelican_position(long player_index, real_point3d *position, real_vector3d *velocity)
{
	(void)player_index;
	(void)position;
	(void)velocity;
	return FALSE;
}

ROYALE_WEAK long royale_gulag_opponent(long player_index) { (void)player_index; return NONE; }

ROYALE_WEAK void royale_loot_postspawn(long player_index) { (void)player_index; }

ROYALE_WEAK boolean royale_insertion_running(void) { return FALSE; }
ROYALE_WEAK void royale_insertion_redeploy(long player_index) { (void)player_index; }
ROYALE_WEAK boolean royale_insertion_jump(long player_index) { (void)player_index; return FALSE; }

ROYALE_WEAK boolean royale_insertion_creating(void) { return FALSE; }
