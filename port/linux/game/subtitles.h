/*
SUBTITLES.H

The campaign's dialogue as subtitles (display.subtitles, #1217), its lines
read from the player's own MCC install (port/linux/src/mcc_subtitles.c).
Only drawn: nothing of the game's state is read but the game's time and the
sounds' tags, and nothing of it is written.
*/

#ifndef __SUBTITLES_H
#define __SUBTITLES_H

#include "cseries.h"

/* a sound started (sound_manager.c's sound_new_impulse): its tag, its
longest permutation's length, and whether it is heard (a scripted line
always; another only as it plays, or with the sound off) */
void subtitles_sound_started(long definition_index, long milliseconds, boolean heard);
/* a map loading (game.c's game_load): the line shown let go, the lines read
now with the setting on, under the loading screen */
void subtitles_map_load(void);
/* the line playing, once over the whole screen (the screen's overlays:
interface.c's interface_draw_fullscreen_overlays) */
void subtitles_draw(void);
/* whether the MCC install's subtitles were found (the settings row's
condition: menu_functions.c's mcc_subtitles) */
boolean subtitles_available(void);

#endif
