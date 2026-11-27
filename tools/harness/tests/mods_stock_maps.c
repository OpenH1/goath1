/*
MODS_STOCK_MAPS.C (test)

The menus' pictures are drawn from the maps as they shipped
(port/linux/game/mods.c's mods_tags_loaded): a map loaded for them plays no
mod and no loose sound, as mods_enable false; any other map (the main
menu's, a game's) plays them. test_mods_stock_maps.py takes the globals
(globals.inc) and the code (under_test.inc).
*/

#include "harness.h"

typedef long int32;
#define csstrncpy strncpy

static int stock_maps, loose_sounds, mods_played = 1, updates, let_go, sized;
static unsigned long scans = 1;

static int menu_pictures_stock_maps(void) { return stock_maps; }
static int config_boolean(char const *key) { (void)key; return loose_sounds; }
static unsigned long port_mods_scans(void) { return scans; }
static long port_mods_scan(void) { scans++; return mods_played; }
static long port_mods_played(void) { return mods_played; }
static void texture_cache_size_for_map(void) { sized++; }
/* (the maps here are the game's own: an MCC map's tags are its converter's,
which no mod plays over) */
static boolean mcc_level_name(char const *name) { (void)name; return FALSE; }
/* (the built-in HD effects, hd_effects.h: off here, so mods_update alone
plays the mods' bitmaps) */
static boolean hd_effects_wanted(void) { return FALSE; }
static unsigned long hd_effects_changes(void) { return 0; }
static void hd_effects_build_now(char const *directory) { (void)directory; }
static char const *cache_files_map_directory(void) { return "maps"; }
static long mod_bitmaps_update(boolean remove_all, boolean at_load) { (void)remove_all; (void)at_load; return 0; }
/* (the MCC import: none here, no test run's import (HALO_MCC_IMPORT), and
its material maps and sounds never played) */
static void mcc_import_now(char const *maps_folder, char const *only, int voices)
{ (void)maps_folder; (void)only; (void)voices; }
static unsigned long mcc_import_changes(void) { return 0; }
static void mod_materials_update(boolean remove_all, boolean at_load) { (void)remove_all; (void)at_load; }
static long mod_sounds_update(boolean remove_all, boolean at_load) { (void)remove_all; (void)at_load; return 0; }

#include "globals.inc"

static void mods_let_go(boolean give_back) { (void)give_back; let_go++; mods_globals.map_loaded = FALSE; }
static void mods_update(boolean remove_all, boolean at_load) { (void)at_load; if (!remove_all) updates++; }

#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	/* a map of the menus' pictures: no mod, no loose sound played on it */
	CASE("pictures-stock")
	{
		stock_maps = 1;
		loose_sounds = 1;
		mods_tags_loaded("a10");
		CHECK(updates == 0, "the mods were played on a map of the menus' pictures (%d)", updates);
		CHECK(!mods_globals.enabled, "the mods are on for a map of the menus' pictures");
		CHECK(!mods_globals.loose_sounds_enabled, "the loose sounds are on for a map of the menus' pictures");
		CHECK(mods_globals.map_loaded, "the map is not noted loaded");
		CHECK(sized == 1, "the texture cache was not sized for the map");
	}

	/* any other map (the main menu's after the pass, a game's) plays them */
	CASE("others-modded")
	{
		stock_maps = 0;
		mods_tags_loaded("ui");
		CHECK(updates == 1, "the mods were not played on the main menu (%d)", updates);
		CHECK(mods_globals.enabled, "the mods are off for a game's map");
		mods_tags_loaded("a10");
		CHECK(updates == 2, "the mods were not played on a game's map (%d)", updates);
		CHECK(let_go == 1, "the map before was not let go of (%d)", let_go);
		/* (a pass's map, then the game's: the mods back) */
		stock_maps = 1;
		mods_tags_loaded("a30");
		CHECK(updates == 2, "the mods were played on a pass's map after a game's");
		stock_maps = 0;
		loose_sounds = 1;
		mods_played = 0;
		mods_tags_loaded("b30");
		CHECK(updates == 3 && mods_globals.loose_sounds_enabled, "the loose sounds were not played after the pass");
	}
	return 0;
}
