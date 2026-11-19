/*
GAME_DATA_PAGE.C

Settings' GAME DATA page (Mods' GAME DATA ›, port/assets/menus/screens/
game_data.xml, #1216): each source of the game's maps a row, its state, and
in the panel what it is, ADD and OPEN FOLDER.

- XBOX MAPS (map_sources.c): the game runs from them, so they are never
  copied while it does. COPY AGAIN asks for the maps screen at the next
  start (first_start.c, Windows' sdl_platform.c), to copy them again from a
  disc image.
- CUSTOM EDITION MAPS: Custom Edition's installer (halocesetup_en_1.00.exe),
  looked for as the first start looks (game_data_scan.c) when the page
  opens, and its maps written into the custom_maps folder (map_import.c) in
  a thread of their own, the page showing the progress. Once they are added
  COPY AGAIN writes every map again, those the folder has replaced, which
  repairs a damaged one. Without an installer found, ADD looks again.
- MCC CONTENT (mcc_maps.c): the packages in the mcc_maps folder and Steam's
  Workshop maps, found by the game; LOOK AGAIN finds those put there since.

A copy goes on while the player leaves the page. What it added the map
lists see at once (custom_edition_maps.c, mcc_maps.c look again, on the
game's side: game_data_page_take_added), with no restart.
*/

#include "platform.h"
#include "game_data_page.h"
#include "../game/menu_table.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GAME_DATA_NONE (-1)

enum
{
	_row_xbox,
	_row_custom_edition,
	_row_mcc,
	NUMBER_OF_ROWS
};

static struct
{
	short chosen;
	int custom_edition_maps;
	int mcc_maps;
	SDL_AtomicInt added;
} game_data_rows;

void game_data_page_map_counts(int custom_edition, int mcc)
{
	game_data_rows.custom_edition_maps = custom_edition;
	game_data_rows.mcc_maps = mcc;
}

unsigned game_data_page_take_added(void)
{
	return (unsigned)SDL_SetAtomicInt(&game_data_rows.added, 0);
}

#ifndef HALO_ANDROID

#include "ce_installer.h"
#include "game_data_scan.h"
#include "map_import.h"
#include "map_sources.h"
#include "port_config.h"
#include "posix.h"
#include "report.h"
#include "xiso.h"

/* what the page's thread is doing */
enum
{
	_job_none,
	_job_looking,
	_job_copying,
};

/* how the last copy of Custom Edition's maps went */
enum
{
	_copy_none,
	_copy_done,
	_copy_failed,
};

static struct
{
	SDL_AtomicInt job;
	/* (the thread's, under lock) */
	SDL_Mutex *lock;
	int looked;
	int found;
	struct game_data_candidate installer;
	char file[256];
	unsigned long long done;
	unsigned long long total;
	int copy;
	int replaced;
	unsigned long long written;
	char error[512];
	/* (the main thread's) the data root, its places and sources, read as
	the page opens and as a copy ends */
	struct game_data_places places;
	struct map_sources sources;
	int requested;
	int read;
	char opened[GAME_DATA_PATH_SIZE + 64];
} game_data;

/* the running game's data root (its d:\), without its last slash */
static void game_data_root(char *root, size_t size)
{
	size_t length;

	platform_translate_path("d:\\", root, (unsigned long)size);
	length = strlen(root);
	while (length > 1 && root[length - 1] == '/')
		root[--length] = 0;
}

static void game_data_read(void)
{
	char root[GAME_DATA_PATH_SIZE];

	if (!game_data.lock)
		game_data.lock = SDL_CreateMutex();
	game_data_root(root, sizeof(root));
	game_data_default_places(&game_data.places, root);
	map_sources_read(root, &game_data.sources);
	game_data.requested = map_sources_requested();
	game_data.read = 1;
}

/* a source's folder: the Xbox maps', custom_maps, mcc_maps */
static void game_data_folder(short row, char *folder, size_t size)
{
	char root[GAME_DATA_PATH_SIZE];

	game_data_root(root, sizeof(root));
	if (row == _row_custom_edition)
		map_import_custom_maps_folder(root, folder, (int)size);
	else if (row == _row_mcc)
		platform_translate_path("d:\\mcc_maps", folder, (unsigned long)size);
	else
		snprintf(folder, size, "%s/maps", root);
}

/* ---------- the thread: looking for the installer, and copying its maps */

static int game_data_installer_check(const char *path, void *context)
{
	(void)context;
	return ce_installer_is_installer(path);
}

static void game_data_progress(void *context, const char *file, unsigned long long done, unsigned long long total)
{
	(void)context;
	SDL_LockMutex(game_data.lock);
	snprintf(game_data.file, sizeof(game_data.file), "%s", file);
	game_data.done = done;
	game_data.total = total;
	SDL_UnlockMutex(game_data.lock);
}

static int SDLCALL game_data_look_thread(void *context)
{
	struct game_data_places *places = context;
	struct game_data_candidate candidates[GAME_DATA_CANDIDATES];
	int count = game_data_scan(places, game_data_installer_check, NULL, candidates, GAME_DATA_CANDIDATES);

	SDL_LockMutex(game_data.lock);
	game_data.looked = 1;
	game_data.found = count > 0;
	if (count > 0)
		game_data.installer = candidates[0];
	SDL_UnlockMutex(game_data.lock);
	platform_log("game data: %s", count > 0 ? candidates[0].path : "no Custom Edition installer found");
	free(places);
	SDL_SetAtomicInt(&game_data.job, _job_none);
	return 0;
}

struct game_data_copy
{
	char installer[GAME_DATA_PATH_SIZE];
	char folder[GAME_DATA_PATH_SIZE];
	int replace;
};

static int SDLCALL game_data_copy_thread(void *context)
{
	struct game_data_copy *copy = context;
	char error[512] = "";
	unsigned long long written = 0;
	int succeeded = map_import_custom_edition(copy->installer, copy->folder, copy->replace, game_data_progress, NULL,
		&written, error, (int)sizeof(error));

	SDL_LockMutex(game_data.lock);
	game_data.copy = succeeded ? _copy_done : _copy_failed;
	game_data.replaced = copy->replace;
	game_data.written = written;
	snprintf(game_data.error, sizeof(game_data.error), "%s", error);
	SDL_UnlockMutex(game_data.lock);
	free(copy);
	/* (the map lists look again, the sources are read again: the game's
	side, the page) */
	if (succeeded)
		SDL_AddAtomicInt(&game_data_rows.added, GAME_DATA_ADDED_CUSTOM_EDITION);
	SDL_SetAtomicInt(&game_data.job, _job_none);
	return 0;
}

/* the thread started for job (context its, malloc's, freed by it) */
static int game_data_begin(int job, SDL_ThreadFunction function, void *context)
{
	SDL_Thread *thread;

	/* (begun on the main thread alone; the thread only ends a job) */
	if (SDL_GetAtomicInt(&game_data.job) != _job_none)
	{
		free(context);
		return 0;
	}
	SDL_SetAtomicInt(&game_data.job, job);
	thread = SDL_CreateThread(function, "game data", context);
	if (!thread)
	{
		platform_log("game data: could not start its thread (%s)", SDL_GetError());
		free(context);
		SDL_SetAtomicInt(&game_data.job, _job_none);
		return 0;
	}
	SDL_DetachThread(thread);
	return 1;
}

static int game_data_look(void)
{
	struct game_data_places *places = malloc(sizeof(*places));

	if (!places)
		return 0;
	*places = game_data.places;
	return game_data_begin(_job_looking, game_data_look_thread, places);
}

static int game_data_copy(int replace)
{
	struct game_data_copy *copy = malloc(sizeof(*copy));

	if (!copy)
		return 0;
	SDL_LockMutex(game_data.lock);
	snprintf(copy->installer, sizeof(copy->installer), "%s", game_data.installer.path);
	game_data.file[0] = 0;
	game_data.done = game_data.total = 0;
	game_data.copy = _copy_none;
	game_data.error[0] = 0;
	SDL_UnlockMutex(game_data.lock);
	game_data_folder(_row_custom_edition, copy->folder, sizeof(copy->folder));
	copy->replace = replace;
	platform_log("game data: copying Custom Edition's maps from %s into %s%s", copy->installer, copy->folder,
		replace ? " (every map again)" : "");
	return game_data_begin(_job_copying, game_data_copy_thread, copy);
}

/* ---------- the page's words */

/* whether Custom Edition's maps are there: all of the installer's
(map_sources.c), or any the map lists have (the installer's, some of them) */
static int game_data_custom_edition_added(void)
{
	return game_data.sources.present[_map_source_custom_edition] || game_data_rows.custom_edition_maps > 0;
}

static void game_data_size(unsigned long long bytes, char *text, size_t size)
{
	double megabytes = (double)bytes / 1e6;

	snprintf(text, size, "%.0f MB", bytes && megabytes < 1 ? 1.0 : megabytes);
}

static int game_data_percent(void)
{
	return game_data.total ? (int)(game_data.done * 100 / game_data.total) : 0;
}

/* the source's state, for its row */
static void game_data_state(short row, char *text, size_t size)
{
	int job = SDL_GetAtomicInt(&game_data.job);

	SDL_LockMutex(game_data.lock);
	if (row == _row_xbox)
		snprintf(text, size, "%s", game_data.requested ? "AT NEXT START" :
			game_data.sources.present[_map_source_xbox] ? "INSTALLED" : "MISSING");
	else if (row == _row_custom_edition && job == _job_copying)
		snprintf(text, size, "COPYING %d%%", game_data_percent());
	else if (row == _row_custom_edition && game_data_rows.custom_edition_maps > 0)
		snprintf(text, size, "%d MAP%s", game_data_rows.custom_edition_maps,
			game_data_rows.custom_edition_maps == 1 ? "" : "S");
	else if (row == _row_custom_edition)
		snprintf(text, size, "%s", game_data.sources.present[_map_source_custom_edition] ? "ADDED" :
			game_data.copy == _copy_failed ? "NOT COPIED" : job == _job_looking ? "LOOKING..." : "NOT ADDED");
	else if (game_data_rows.mcc_maps > 0)
		snprintf(text, size, "%d MAP%s", game_data_rows.mcc_maps, game_data_rows.mcc_maps == 1 ? "" : "S");
	else
		snprintf(text, size, "NONE");
	SDL_UnlockMutex(game_data.lock);
}

/* ADD's words for the chosen source ("": none now) */
static void game_data_add_label(char *text, size_t size)
{
	int job = SDL_GetAtomicInt(&game_data.job);

	text[0] = 0;
	if (game_data_rows.chosen == _row_xbox)
		snprintf(text, size, "%s", game_data.requested ? "CANCEL COPY" : "COPY AGAIN");
	else if (game_data_rows.chosen == _row_mcc)
		snprintf(text, size, "LOOK AGAIN");
	else if (job == _job_none && !game_data.found)
		snprintf(text, size, "LOOK AGAIN");
	else if (job == _job_none)
		snprintf(text, size, "%s", game_data_custom_edition_added() ? "COPY AGAIN" : "ADD");
}

static void game_data_status(char *value, unsigned long size)
{
	int job = SDL_GetAtomicInt(&game_data.job);
	char done[32], total[32];

	if (game_data_rows.chosen == _row_xbox)
	{
		if (game_data.requested)
			snprintf(value, size, "The maps will be copied again from your disc image the next time the game "
				"starts. Cancel Copy keeps them as they are.");
		else
			snprintf(value, size, "The maps from your Xbox disc. The game is using them, so Copy Again runs the "
				"next time the game starts.");
		return;
	}
	if (game_data_rows.chosen == _row_mcc)
	{
		snprintf(value, size, "Map packages from the Master Chief Collection, in the mcc_maps folder.%s Put a "
			"package's folder there, then choose Look Again.", config_boolean("game.workshop_maps") ?
			" Steam's Workshop maps are found too." : "");
		return;
	}
	if (job == _job_copying)
	{
		game_data_size(game_data.done, done, sizeof(done));
		game_data_size(game_data.total, total, sizeof(total));
		snprintf(value, size, "Copying %s: %s of %s. You can leave this page while it copies.",
			game_data.file[0] ? game_data.file : "the maps", done, total);
	}
	else if (job == _job_looking)
		snprintf(value, size, "Looking for Custom Edition's installer...");
	else if (game_data.copy == _copy_failed)
		snprintf(value, size, "The copy stopped. %s", game_data.error);
	else if (game_data.copy == _copy_done && game_data.written)
	{
		game_data_size(game_data.written, done, sizeof(done));
		snprintf(value, size, "%s %s of maps. They're in the map lists under Custom Multiplayer.",
			game_data.replaced ? "Copied again:" : "Added", done);
	}
	else if (game_data.copy == _copy_done)
		snprintf(value, size, "You already have every map in the installer. Nothing was copied.");
	else if (game_data_custom_edition_added())
		snprintf(value, size, "Multiplayer maps from Custom Edition's installer. Copy Again copies them all again, "
			"which repairs a damaged map.");
	else if (game_data.found)
		snprintf(value, size, "Multiplayer maps from Custom Edition's installer. They're optional. Add copies "
			"them.");
	else
		snprintf(value, size, "Custom Edition's installer (halocesetup_en_1.00.exe) wasn't found. Put it in "
			"Downloads or on the Desktop, then choose Look Again.");
}

/* where the chosen source comes from */
static void game_data_from(char *value, unsigned long size)
{
	if (game_data_rows.chosen == _row_custom_edition && game_data.found)
	{
		/* ("in Downloads", "in the game's folder") */
		const char *place = game_data.installer.place;

		snprintf(value, size, "From %s, in %s%s", game_data.installer.name, !strncmp(place, "The ", 4) ? "the " : "",
			!strncmp(place, "The ", 4) ? place + 4 : place);
	}
	else if (game_data_rows.chosen == _row_xbox)
	{
		/* (where the disc image the maps were copied from was: xiso.c) */
		char root[GAME_DATA_PATH_SIZE], path[GAME_DATA_PATH_SIZE + 32], image[GAME_DATA_PATH_SIZE];
		FILE *file;

		game_data_root(root, sizeof(root));
		snprintf(path, sizeof(path), "%s/" XISO_IMAGE_FILE, root);
		file = fopen(path, "rb");
		if (file)
		{
			if (fgets(image, sizeof(image), file))
			{
				const char *name = strrchr(image, '/');

				image[strcspn(image, "\r\n")] = 0;
				snprintf(value, size, "From %s", name ? name + 1 : image);
			}
			fclose(file);
		}
	}
}

int game_data_page_value(const char *name, char *value, unsigned long size)
{
	int job = SDL_GetAtomicInt(&game_data.job);

	value[0] = 0;
	if (!game_data.read)
		game_data_read();
	SDL_LockMutex(game_data.lock);
	if (!strcmp(name, "data.detail"))
	{
		/* (how many of the three sources have maps: "2 OF 3 ADDED") */
		if (job == _job_copying)
			snprintf(value, size, "COPYING %d%%", game_data_percent());
		else
			snprintf(value, size, "%d OF %d ADDED", (game_data.sources.present[_map_source_xbox] != 0) +
				(game_data_custom_edition_added() != 0) + (game_data_rows.mcc_maps > 0), NUMBER_OF_ROWS);
	}
	else if (!strcmp(name, "data.heading"))
		snprintf(value, size, "%s", game_data_rows.chosen == _row_xbox ? "Xbox maps" :
			game_data_rows.chosen == _row_mcc ? "MCC content" : "Custom Edition maps");
	else if (!strcmp(name, "data.status"))
		game_data_status(value, size);
	else if (!strcmp(name, "data.from"))
		game_data_from(value, size);
	else if (!strcmp(name, "data.folder"))
	{
		char folder[GAME_DATA_PATH_SIZE], shown[GAME_DATA_PATH_SIZE];

		game_data_folder(game_data_rows.chosen, folder, sizeof(folder));
		game_data_short_path(&game_data.places, folder, shown, (int)sizeof(shown));
		snprintf(value, size, "Folder: %s", shown);
	}
	else if (!strcmp(name, "data.opened"))
		snprintf(value, size, "%s", game_data.opened);
	else if (!strcmp(name, "data.add"))
		game_data_add_label(value, size);
	SDL_UnlockMutex(game_data.lock);
	return value[0] != 0;
}

int game_data_page_action(const char *name, short row)
{
	if (row >= 0 && row < NUMBER_OF_ROWS)
		game_data_rows.chosen = row;
	if (!game_data.read)
		game_data_read();
	if (!strcmp(name, "data add"))
	{
		int job = SDL_GetAtomicInt(&game_data.job), found, present;

		if (game_data_rows.chosen == _row_xbox)
		{
			/* (the maps screen at the next start: map_sources.c) */
			map_sources_request(!game_data.requested);
			game_data.requested = map_sources_requested();
			return 1;
		}
		if (game_data_rows.chosen == _row_mcc)
		{
			SDL_AddAtomicInt(&game_data_rows.added, GAME_DATA_ADDED_MCC);
			return 1;
		}
		if (job != _job_none)
			return 0;
		SDL_LockMutex(game_data.lock);
		found = game_data.found;
		SDL_UnlockMutex(game_data.lock);
		present = game_data_custom_edition_added();
		return found ? game_data_copy(present) : game_data_look();
	}
	if (!strcmp(name, "data open folder"))
	{
		char folder[GAME_DATA_PATH_SIZE], shown[GAME_DATA_PATH_SIZE];

		game_data_folder(game_data_rows.chosen, folder, sizeof(folder));
		posix_make_directory(folder);
		game_data_short_path(&game_data.places, folder, shown, (int)sizeof(shown));
		SDL_LockMutex(game_data.lock);
		snprintf(game_data.opened, sizeof(game_data.opened), report_open_folder(folder) ?
			"Opened %s on the desktop." : "Could not open %s: open it yourself.", shown);
		SDL_UnlockMutex(game_data.lock);
		return 1;
	}
	return 0;
}

/* the page opened: its sources read, the installer looked for */
static void game_data_opened(void)
{
	game_data_read();
	game_data.opened[0] = 0;
	SDL_LockMutex(game_data.lock);
	if (game_data.copy != _copy_failed)
		game_data.copy = _copy_none;
	SDL_UnlockMutex(game_data.lock);
	if (SDL_GetAtomicInt(&game_data.job) == _job_none)
		game_data_look();
}

/* each frame: a copy ended since, its sources read again */
static void game_data_frame(void)
{
	static int last_job;
	int job = SDL_GetAtomicInt(&game_data.job);

	if (last_job == _job_copying && job != _job_copying)
		game_data_read();
	last_job = job;
}

#else

/* (the Android app imports the game data itself: its launcher's) */
static void game_data_state(short row, char *text, size_t size)
{
	snprintf(text, size, "%s", row == _row_xbox ? "INSTALLED" : "");
}

int game_data_page_value(const char *name, char *value, unsigned long size)
{
	(void)name;
	(void)size;
	value[0] = 0;
	return 0;
}

int game_data_page_action(const char *name, short row)
{
	(void)name;
	(void)row;
	return 0;
}

static void game_data_opened(void)
{
}

static void game_data_frame(void)
{
}

#endif

/* ---------- the table: the sources */

static void game_data_source_update(struct menu_table_view *view)
{
	game_data_frame();
	if (view->focused >= 0 && view->focused < NUMBER_OF_ROWS)
		game_data_rows.chosen = view->focused;
	view->count = NUMBER_OF_ROWS;
	view->first = 0;
	view->chosen = game_data_rows.chosen;
	view->focus_titles = 0;
}

static int game_data_source_heading(short index, unsigned short *words, short size)
{
	(void)index;
	(void)words;
	(void)size;
	return 0;
}

static void game_data_source_cell(short index, char const *column, int focused, struct menu_table_cell *cell)
{
	static char const *const names[NUMBER_OF_ROWS] = { "XBOX MAPS", "CUSTOM EDITION MAPS", "MCC CONTENT" };
	char text[MENU_TABLE_TEXT_LENGTH];
	int character;

	(void)focused;
	cell->frame = GAME_DATA_NONE;
	if (index < 0 || index >= NUMBER_OF_ROWS)
		return;
	text[0] = 0;
	if (!strcmp(column, "source"))
		snprintf(text, sizeof(text), "%s", names[index]);
	else if (!strcmp(column, "state"))
		game_data_state(index, text, sizeof(text));
	for (character = 0; text[character] && character < MENU_TABLE_TEXT_LENGTH - 1; character++)
		cell->text[character] = (unsigned char)text[character];
	cell->text[character] = 0;
}

struct menu_table_source const game_data_page_source =
{
	game_data_source_update, game_data_source_heading, game_data_source_cell, game_data_opened, NULL
};
