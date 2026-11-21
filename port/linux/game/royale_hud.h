/* royale_hud.h: Royale's HUD in Halo's own style: squads and players left, the teammate on the tracker and nav marker, the beam warning (port/linux/game/royale_hud.c)

Core (royale.c) calls these once each; this module owns everything inside
them. Presentation only: read synced Royale state, never change it, so every
screen shows the same thing. */
#ifndef HALO_ROYALE_HUD_H
#define HALO_ROYALE_HUD_H

#include "cseries.h"
#include "math/real_math.h"

struct royale_map_config;

void royale_hud_start(struct royale_map_config const *config);
void royale_hud_reset(void);
void royale_hud_tick(void);
/* the screen's drawing for this local player (after the world) */
void royale_hud_draw(short local_player_index);

/* the HUD's hooks (hud_unit.c, motion_sensor.c): the motion tracker is
shown in Royale (its squadmate alone on it: the game type draws no other
units); the squadmate's blip (where the view's player's mate is drawn,
relative to the view's camera in world units), FALSE if none */
boolean royale_hud_motion_sensor_shown(long player_index);
boolean royale_hud_tracker_mate(short local_player_index, real_point2d *offset, boolean *downed);
/* the player is the viewer's squadmate */
boolean royale_hud_squadmate(long viewer_player_index, long player_index);

#endif
