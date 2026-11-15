/* royale_map.h: a campaign level played as a multiplayer map (port/linux/game/royale_map.c)

Royale plays on Silent Cartographer's island (b30), a campaign level. A game
engine on a campaign level (its scenario's type solo) is the Royale map mode:
loaded as it is (the shipped map untouched), then at load time, in memory,
the level's campaign is taken out (its AI, scripts, cutscenes, campaign
objects and BSP switches), the globals' missing multiplayer part stood in for,
and its players' starts opened to every game type. Royale's map config then
closes the interior's entrances and keeps the players on the island. */
#ifndef HALO_ROYALE_MAP_H
#define HALO_ROYALE_MAP_H

#include "cseries.h"

/* a Royale game on a campaign level: its campaign is not played (the AI,
scripts and BSP switches are off). Any other game type keeps them */
boolean royale_map_mode(void);
/* game_engine_initialize_for_new_map (before anything reads the globals'
multiplayer part, and before the scenario's objects are placed): the level
made a multiplayer map in memory, in the Royale map mode */
void royale_map_prepare(void);
/* game_engine_dispose_from_old_map: what royale_map_prepare changed in the
level's tags put back */
void royale_map_restore(void);

#endif
