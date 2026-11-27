/*
MCC_IMPORT.H

Import from MCC (docs/mcc-import.md, issue #1257): sharper weapon,
vehicle and Spartan textures, their material maps, and MCC's clearer English dialogue,
built on the player's machine from their own Halo MCC install and kept in
the data folder's mcc_import. Nothing of MCC's is shipped: the repository
has only numbers (port/assets/mcc_import/mcc_import.json).

The player starts it from Settings (or HALO_MCC_IMPORT for a test run); it
runs on a thread of its own with progress, and can be canceled. Its files
are played as the built-in HD effects are (port/linux/game/mods.c): under
every mod, a mod's file of the same tag winning, a bitmap only over the map's
stock one (its pixels' CRC). display.mcc_import turns them off and on.
*/

#ifndef MCC_IMPORT_H
#define MCC_IMPORT_H

/* where the import stands */
enum
{
	/* no MCC install found (and nothing imported) */
	_mcc_import_no_install,
	/* an install found, nothing imported yet */
	_mcc_import_none,
	/* imported, and MCC and the maps are as they were */
	_mcc_import_current,
	/* imported, but MCC or the maps changed since: a re-import is offered */
	_mcc_import_stale,
	/* importing now */
	_mcc_import_running,
};

/* display.mcc_import (never on a dedicated server or the null renderer) */
int mcc_import_enabled(void);
/* where it stands, with the Xbox maps folder maps_folder ("d:\maps\") to
look at (looked at again at most every few seconds unless `fresh`), and the
last import's line (what it made, or why it failed) */
int mcc_import_state(const char *maps_folder, int fresh);
const char *mcc_import_report(void);
/* the MCC install's folder found (paths.mcc, else Steam's libraries): 1 and
its host path, or 0 */
int mcc_import_install(char *path, unsigned long size);
/* the folder picker, for an install no Steam library has: the player picks
MCC's pak (MCC/Content/Paks/MCC-WindowsNoEditor.pak) and its install becomes
paths.mcc; 0 if none (or not one) was picked */
int mcc_import_choose(void);

/* Begins the import on a thread of its own from the Xbox maps folder
maps_folder ("d:\maps\": cache_files_map_directory) and the install; `only`
limits it to the preview meshes (and layouts' models and folders) whose
names have one of its words (NULL: all) and `voices` says whether the dialogue is imported too. 0 if it
cannot begin (no install, or one running). */
int mcc_import_begin(const char *maps_folder, const char *only, int voices);
/* whether it is running, how far (0 to 1) and at what ("Textures",
"Voices"); asks it to stop */
int mcc_import_running(float *progress, const char **step);
void mcc_import_cancel(void);
/* runs it here (one already running is waited for) */
void mcc_import_now(const char *maps_folder, const char *only, int voices);
/* deletes the import's folder; 0 if some of it is left */
int mcc_import_remove(void);
/* changes whenever what the file lookups below say may have */
unsigned long mcc_import_changes(void);

/* the import's bitmap file of the bitmap tag `tag` (its path into path)
and the CRC-32 of the stock pixels it was baked over: 0 when off or none */
int mcc_import_bitmap_file(const char *tag, char *path, unsigned long size, unsigned int *stock_crc);
/* the import's file of a tag and extension (".normal.png", ".orm.png",
".sound"): 0 when off or none */
int mcc_import_tag_file(const char *tag, const char *extension, char *path, unsigned long size);

/* the importer's version, in the note: a change makes the import stale */
#define MCC_IMPORT_VERSION 2

#endif
