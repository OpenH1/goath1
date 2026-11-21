/* royale_fx.h: Royale's world effects: the Covenant ship over the beam, the rumble, orange sky and shake near the beam, the supply pods' fiery fall (port/linux/game/royale_fx.c)

Core (royale.c) calls these once each; this module owns everything inside
them. Presentation only: read synced Royale state, never change it, so every
screen shows the same thing. */
#ifndef HALO_ROYALE_FX_H
#define HALO_ROYALE_FX_H

#include "cseries.h"

struct royale_map_config;

void royale_fx_start(struct royale_map_config const *config);
void royale_fx_reset(void);
void royale_fx_tick(void);
/* the screen's drawing for this local player (after the world) */
void royale_fx_draw(short local_player_index);
/* the world's drawing, from render.c with the transparent geometry */
void royale_fx_render(void);

#endif
