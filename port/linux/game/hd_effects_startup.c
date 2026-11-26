/*
HD_EFFECTS_STARTUP.C

The game's half of the HD effects made as the game starts (hd_effects.h;
the platform's half, which reads the maps, redraws the bitmaps and keeps
them, is port/linux/src/hd_effects.c).

A step of the game's start (main.c's main_startup_update): once the intro
movie has ended or been skipped, and before the menus' pictures, the main
menu still held, the effects wanted (none made yet, or not this build's)
are made on the platform's thread while the loading screen shows "HD
effects", its light crossing once as they are made; it fades out, and the
start goes on. With none wanted (made before, or display.hd_effects off),
nothing is shown. Once a run: a map loaded later makes any still wanted as
it loads (mods.c).
*/

#include "cseries.h"
#include "cache/cache_files.h"

#include "../src/hd_effects.h"
#include "../src/loading_screen.h"

/* sdl_platform.c's: a map load's loading screen given back */
void halo_loading_end(void);
unsigned long long SDL_GetTicksNS(void);

/* (nanoseconds) the loading screen's fade at the end, as the menus'
pictures' (menu_pictures_render.c) */
#define STARTUP_SCREEN_FADE 550000000ULL

enum
{
	_startup_effects_not_begun = 0,
	_startup_effects_making,
	_startup_effects_fading,
	_startup_effects_done,
};

static struct
{
	long state;
	unsigned long long faded;
} hd_effects_startup;

/* (main.c) TRUE while the effects are made, or their screen fades */
int hd_effects_startup_preparing(
	void)
{
	float progress = 0.0f;

	switch (hd_effects_startup.state)
	{
	case _startup_effects_not_begun:
		hd_effects_startup.state = _startup_effects_done;
		if (!hd_effects_wanted() || !hd_effects_build_begin(cache_files_map_directory()))
		{
			return FALSE;
		}
		hd_effects_startup.state = _startup_effects_making;
		/* (the main menu's own load's screen, still up until its first
		frame is shown, given back first) */
		halo_loading_end();
		halo_loading_screen_begin(TRUE);
		halo_loading_screen_progress(0.0f, "HD effects");
		return TRUE;
	case _startup_effects_making:
		if (hd_effects_build_running(&progress))
		{
			halo_loading_screen_progress(progress, "HD effects");
			return TRUE;
		}
		halo_loading_screen_progress(1.0f, "HD effects");
		hd_effects_startup.faded = SDL_GetTicksNS();
		hd_effects_startup.state = _startup_effects_fading;
		return TRUE;
	case _startup_effects_fading:
		if (SDL_GetTicksNS() - hd_effects_startup.faded < STARTUP_SCREEN_FADE)
		{
			return TRUE;
		}
		halo_loading_screen_end();
		hd_effects_startup.state = _startup_effects_done;
		return FALSE;
	default:
		return FALSE;
	}
}
