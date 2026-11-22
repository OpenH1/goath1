#ifndef __HEADSHOT_INDICATOR_H
#define __HEADSHOT_INDICATOR_H

/*
The headshot indicator (display.headshot_indicator, headshot_indicator.c): a
dot in the stock pistol's and sniper rifle's reticle when the shot, with the
game's autoaim, would land on the aim assist's target's head, with the high-res HUD. Off, nothing is
drawn differently.
*/

/* the map's weapons looked at as its tags load (scenario_tags_load), and
forgotten as it unloads */
void headshot_indicator_tags_loaded(void);
void headshot_indicator_tags_unloaded(void);

struct bitmap_data;

/* the bitmap crosshairs_draw draws this item of a reticle (the hud's
crosshairs element crosshair_index, its item item_index) of the player's
weapon from: for the stock pistol's and sniper rifle's middle one, with the
setting and the high-res HUD on, a placeholder for which the high-res
texture's copy with the indicator's dot is drawn while the shot would be a
headshot (and the sniper rifle's without its own otherwise); the bitmap for
the rest */
struct bitmap_data const *headshot_indicator_item_bitmap(short local_player_index, long weapon_index,
	long hud_index, short crosshair_index, short item_index, struct bitmap_data const *bitmap);

#endif
