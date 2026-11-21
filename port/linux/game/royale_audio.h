/* royale_audio.h: Royale's sound: the Pelican pilot's radio, campaign music at the big moments and the announcer (port/linux/game/royale_audio.c)

Core (royale.c) calls these once each; this module owns everything inside
them. Presentation only: read synced Royale state, never change it, so every
screen shows the same thing. */
#ifndef HALO_ROYALE_AUDIO_H
#define HALO_ROYALE_AUDIO_H

#include "cseries.h"

struct royale_map_config;

void royale_audio_start(struct royale_map_config const *config);
void royale_audio_reset(void);
void royale_audio_tick(void);

#endif
