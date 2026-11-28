/*
HD_EFFECTS.H

The HD effects (display.hd_effects): stock effect bitmaps (the Assault
Rifle's first-person muzzle flash, and the blood's decals and particles)
made sharper from the player's
own maps, once, and drawn in the maps' bitmaps' place as a mod's bitmaps are
(port/linux/game/mod_bitmaps.c), so that their sprites, sizes and HUD scale
work as a mod's do.

port/assets/effects/effects.json lists them, numbers only (tools/
embed_assets.py makes it hd_effects_embedded): each one's tag, the CRC-32 of
the stock bitmap's pixels (every page's, as the Xbox maps store them), the
recipe that redraws it and at what scale, and the stock maps it can be read
from. hd_effects.c reads the bitmap out of the first of those maps the
player has whose bitmap has that CRC, redraws it (the recipe: for the
flash, tools/hd_muzzle_flash.py's math, in C; for the blood,
tools/hd_blood.py's) and keeps it, a Halo PC
bitmap tag file, in the data folder's hd_effects, beside the menus'
pictures (menu_pictures.c), with a note of what it was made from. It is
made again only when the note is not this build's: its CRC, scale or recipe
version changed, or the file is gone or cut short.

They are made as a phase of the game's start (main.c's main_startup_update,
before the menus' pictures), under the loading screen ("HD effects"), on a
thread of their own; a map loaded before that (a run that starts in a map,
or the setting turned on in one) makes them as it loads. A map's bitmap is
drawn from the file only when its pixels are the stock ones (the CRC): a
map's own or modded bitmap is left as it is, and a mod's file for the same
bitmap is drawn instead of this one. Off, nothing is read or made, and the
maps' bitmaps are drawn as they are.
*/

#ifndef HD_EFFECTS_H
#define HD_EFFECTS_H

/* the recipes (effects.json's "recipe") */
enum
{
	/* tools/hd_muzzle_flash.py: an additive sprite sheet of A1R5G5B5 pages
	upscaled, its banding smoothed, its cores sharpened and its glow
	softened, each sprite as bright as the map's */
	_hd_effect_recipe_flash = 1,
	/* tools/hd_blood.py: DXT1 decal and DXT3 particle sheets of painted
	splats, each sprite's cell upscaled on its own, deblocked and its edges
	made crisp, its mean color the map's; a decal's alpha the map's */
	_hd_effect_recipe_splat = 2,
};

/* an effect (effects.json): the name of its file in the cache, the bitmap
tag it stands for, its recipe, the scale it is redrawn at and the smallest
mip level it keeps, the CRC-32 of the stock bitmap's pixels, and the stock
maps it is read from (the first the player has, by name, spaces between) */
struct hd_effect_embedded
{
	const char *name;
	const char *tag;
	unsigned int recipe;
	unsigned int scale;
	unsigned int smallest;
	unsigned int crc;
	const char *maps;
};

extern const struct hd_effect_embedded hd_effects_embedded[];
extern const unsigned int hd_effects_embedded_count;

/* display.hd_effects (never on a dedicated server or the null renderer,
which draw nothing) */
int hd_effects_enabled(void);
/* whether any effect is to be made (enabled, its file missing or not this
build's, and not already failed in this run) */
int hd_effects_wanted(void);
/* Begins making the effects wanted on a thread of their own, from the maps
in the Xbox folder maps_folder ("d:\maps\": cache_files_map_directory): 0
when none is wanted. */
int hd_effects_build_begin(const char *maps_folder);
/* whether the thread is still making them, and how far it is (0 to 1) */
int hd_effects_build_running(float *progress);
/* Makes the effects wanted now, on this thread (a thread already making
them is waited for). */
void hd_effects_build_now(const char *maps_folder);
/* Changes whenever what hd_effects_bitmap_file says may have: the setting
turned on or off, or effects made. */
unsigned long hd_effects_changes(void);
/* the cached file of the bitmap tag `tag` (its path, in size bytes) and the
CRC-32 of the stock pixels it is made for: 0 when it is off, or there is
none of that tag, or it is not made */
int hd_effects_bitmap_file(const char *tag, char *path, unsigned long size, unsigned int *stock_crc);

/* the recipes' versions, in each note: a change of a recipe's math makes
its effects again (the flash's, then the splat's) */
#define HD_EFFECTS_RECIPE_VERSION 1
#define HD_EFFECTS_SPLAT_VERSION 1

#endif
