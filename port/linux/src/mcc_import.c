/*
MCC_IMPORT.C

Import from MCC (mcc_import.h): finding the install, the thread, the note,
and the lookups the game plays the files through.

The data folder's mcc_import holds tags/<tag>.bitmap, .normal.png, .orm.png
and .sound, and import.note, written last:

  openh1-mcc-import <version>
  source <what> <size> <modification time> <path relative to the install or maps>
  bitmap <its stock pixels' CRC-32> <tag>
  map <its tags' CRC-32> <name>
  report <the import's line>

The import is current while every source line still holds (each file's size
and time as they were): an MCC update or changed maps make it stale, and
Settings offers a re-import. Its bitmaps are played only from the note's
list, over the stock pixels it names.
*/

#include "mcc_import.h"
#include "mcc_import_bake.h"
#include "mcc_import_voices.h"
#include "platform.h"
#include "port_config.h"
#include "posix.h"
#include "steam_workshop.h"
#include "ue4_pak.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int mkdir(const char *path, unsigned int mode);
int halo_server_mode(void);
/* (sdl_platform.c) */
int halo_mcc_choose_pak(char *path, int size);

#define MAXIMUM_PATH 1100
#define MAXIMUM_NOTE (256UL * 1024)
#define MAXIMUM_NOTED_BITMAPS 512
#define PAK_RELATIVE "MCC/Content/Paks/MCC-WindowsNoEditor.pak"
#define NOTE_HEAD "openh1-mcc-import"
/* (seconds) how often the state is looked at again */
#define STATE_INTERVAL 5

struct noted_bitmap
{
	char tag[256];
	unsigned int crc;
};

static struct
{
	pthread_mutex_t lock;
	pthread_t thread;
	int thread_started;
	int running;
	int cancel;
	float progress;
	const char *step;
	char report[512];
	unsigned long changes;
	/* the note as last read: its bitmaps, and whether it was read */
	int noted;
	long bitmap_count;
	struct noted_bitmap *bitmaps;
	/* the state as last looked at, and when */
	int state;
	time_t state_time;
	/* the thread's work */
	char maps_folder[MAXIMUM_PATH];
	char install[MAXIMUM_PATH];
	char only[256];
	int voices;
	char folder[MAXIMUM_PATH];
} mcc_import = { PTHREAD_MUTEX_INITIALIZER };

/* ---------- small things */

static const char *import_folder(void)
{
	if (!mcc_import.folder[0])
		snprintf(mcc_import.folder, sizeof(mcc_import.folder), "%s/mcc_import", platform_data_root());
	return mcc_import.folder;
}

static void note_path(char *path, size_t size)
{
	snprintf(path, size, "%s/import.note", import_folder());
}

static int file_stamp(const char *path, unsigned long long *size, unsigned long *seconds)
{
	struct posix_file_information information;

	if (posix_stat(path, &information) != 0 || (information.flags & _posix_file_is_directory))
		return 0;
	*size = (unsigned long long)information.size_high << 32 | information.size_low;
	*seconds = information.modification_seconds;
	return 1;
}

static char *file_text(const char *path, size_t most)
{
	FILE *file = fopen(path, "rb");
	char *text;
	size_t length;

	if (!file)
		return NULL;
	text = malloc(most + 1);
	if (!text)
	{
		fclose(file);
		return NULL;
	}
	length = fread(text, 1, most, file);
	fclose(file);
	text[length] = 0;
	return text;
}

/* (a file written whole or not at all) */
static int write_text(const char *path, const char *text)
{
	char temporary[MAXIMUM_PATH + 8];
	FILE *file;
	int written;

	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fputs(text, file) >= 0;
	written = fclose(file) == 0 && written;
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

static int same_tag(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
	{
		char x = *a == '/' ? '\\' : (*a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a);
		char y = *b == '/' ? '\\' : (*b >= 'A' && *b <= 'Z' ? (char)(*b + 32) : *b);

		if (x != y)
			return 0;
	}
	return *a == *b;
}

/* the tag's file under the import's tags folder, into path */
static void tag_file_path(char *path, unsigned long size, const char *tag, const char *extension)
{
	size_t length, index;

	snprintf(path, size, "%s/tags/", import_folder());
	length = strlen(path);
	for (index = 0; tag[index] && length + 1 < size; index++)
		path[length++] = tag[index] == '\\' ? '/' : tag[index];
	path[length] = 0;
	snprintf(path + length, size - length, "%s", extension);
}

/* ---------- the install */

int mcc_import_install(char *path, unsigned long size)
{
	const char *chosen = config_string("paths.mcc");
	char pak[MAXIMUM_PATH];
	unsigned long long bytes;
	unsigned long seconds;
	int part;

	if (chosen[0])
	{
		snprintf(path, size, "%s", chosen);
		snprintf(pak, sizeof(pak), "%s/%s", chosen, PAK_RELATIVE);
		return file_stamp(pak, &bytes, &seconds);
	}
	if (!steam_workshop_mcc_file(PAK_RELATIVE, pak, sizeof(pak)))
		return 0;
	/* (the install's folder: the pak's path less its four parts) */
	for (part = 0; part < 4; part++)
	{
		char *slash = strrchr(pak, '/'), *backslash = strrchr(pak, '\\');

		if (backslash > slash)
			slash = backslash;
		if (!slash)
			return 0;
		*slash = 0;
	}
	snprintf(path, size, "%s", pak);
	return 1;
}

int mcc_import_choose(void)
{
	char pak[MAXIMUM_PATH], install[MAXIMUM_PATH], check[MAXIMUM_PATH];
	unsigned long long bytes;
	unsigned long seconds;
	int part;

	if (!halo_mcc_choose_pak(pak, sizeof(pak)))
		return 0;
	snprintf(install, sizeof(install), "%s", pak);
	/* (the pak's path less MCC/Content/Paks/<pak>) */
	for (part = 0; part < 4; part++)
	{
		char *slash = strrchr(install, '/'), *backslash = strrchr(install, '\\');

		if (backslash > slash)
			slash = backslash;
		if (!slash)
			return 0;
		*slash = 0;
	}
	snprintf(check, sizeof(check), "%s/%s", install, PAK_RELATIVE);
	if (!file_stamp(check, &bytes, &seconds))
	{
		platform_log("mcc import: %s is not MCC's pak in its install (no %s)", pak, check);
		return 0;
	}
	config_write("paths.mcc", install);
	pthread_mutex_lock(&mcc_import.lock);
	mcc_import.state_time = 0;
	pthread_mutex_unlock(&mcc_import.lock);
	platform_log("mcc import: the install chosen: %s", install);
	return 1;
}

/* ---------- the note */

/* (with the lock) the note's bitmaps read again */
static void note_load(void)
{
	char path[MAXIMUM_PATH], *text, *line, *next;

	free(mcc_import.bitmaps);
	mcc_import.bitmaps = NULL;
	mcc_import.bitmap_count = 0;
	mcc_import.noted = 1;
	note_path(path, sizeof(path));
	text = file_text(path, MAXIMUM_NOTE);
	if (!text || strncmp(text, NOTE_HEAD " ", strlen(NOTE_HEAD) + 1) ||
		!(mcc_import.bitmaps = calloc(MAXIMUM_NOTED_BITMAPS, sizeof(*mcc_import.bitmaps))))
	{
		free(text);
		return;
	}
	for (line = text; line && *line; line = next)
	{
		unsigned int crc;
		int offset = 0;

		next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		if (!strncmp(line, "report ", 7) && !mcc_import.running)
			snprintf(mcc_import.report, sizeof(mcc_import.report), "%s", line + 7);
		if (sscanf(line, "bitmap %8x %n", &crc, &offset) == 1 && offset && mcc_import.bitmap_count < MAXIMUM_NOTED_BITMAPS)
		{
			struct noted_bitmap *bitmap = &mcc_import.bitmaps[mcc_import.bitmap_count++];

			snprintf(bitmap->tag, sizeof(bitmap->tag), "%s", line + offset);
			bitmap->crc = crc;
		}
	}
	free(text);
}

/* the source line of a file now, or "" if it is gone */
static void source_line(char *line, size_t size, const char *what, const char *path)
{
	unsigned long long bytes;
	unsigned long seconds;

	if (file_stamp(path, &bytes, &seconds))
		snprintf(line, size, "source %s %llu %lu\n", what, bytes, seconds);
	else
		snprintf(line, size, "source %s gone\n", what);
}

/* the sources' lines now: the pak, the dialogue bank and each map a fit or layout names */
static void sources_text(char *text, size_t size, const char *install, const char *maps_folder_host, int voices)
{
	char path[MAXIMUM_PATH], line[MAXIMUM_PATH + 64];
	unsigned int fit;

	text[0] = 0;
	snprintf(path, sizeof(path), "%s/%s", install, PAK_RELATIVE);
	source_line(line, sizeof(line), "pak", path);
	strncat(text, line, size - strlen(text) - 1);
	if (voices)
	{
		snprintf(path, sizeof(path), "%s/halo1/sound/pc/sounds_dialogs.fsb", install);
		source_line(line, sizeof(line), "voices", path);
		strncat(text, line, size - strlen(text) - 1);
	}
	/* (the fits' maps, then the layouts' maps, each once) */
	for (fit = 0; fit < mcc_import_fit_count + mcc_import_layout_count; fit++)
	{
		const char *map = fit < mcc_import_fit_count ? mcc_import_fits[fit].map :
			mcc_import_layouts[fit - mcc_import_fit_count].map;
		unsigned int before;
		char what[96];

		for (before = 0; before < fit; before++)
			if (!strcmp(map, before < mcc_import_fit_count ? mcc_import_fits[before].map :
				mcc_import_layouts[before - mcc_import_fit_count].map))
			{
				break;
			}
		if (before < fit)
			continue;
		snprintf(path, sizeof(path), "%s%s.map", maps_folder_host, map);
		snprintf(what, sizeof(what), "map:%s", map);
		source_line(line, sizeof(line), what, path);
		strncat(text, line, size - strlen(text) - 1);
	}
}

/* the maps folder's host path, ending in a separator */
static void maps_folder_host(const char *maps_folder, char *path, size_t size)
{
	size_t length;

	platform_translate_path(maps_folder, path, (unsigned long)size);
	length = strlen(path);
	if (length && path[length - 1] != '/' && path[length - 1] != '\\' && length + 1 < size)
	{
		path[length] = '/';
		path[length + 1] = 0;
	}
}

/* ---------- the thread */

static int thread_progress(void *context, float fraction)
{
	int cancel;

	(void)context;
	pthread_mutex_lock(&mcc_import.lock);
	mcc_import.progress = fraction;
	cancel = mcc_import.cancel;
	pthread_mutex_unlock(&mcc_import.lock);
	return cancel;
}

static void thread_log(const char *line)
{
	platform_log("mcc import: %s", line);
}

/* the dialogue imported from every map of the maps folder (host path) */
static long voices_import(const char *maps, char *report, size_t report_size)
{
	void *directory = posix_directory_open(maps);
	char name[256];
	char *paths[256];
	int count = 0, index;
	long written;

	if (!directory)
	{
		snprintf(report, report_size, "no voices: the maps folder cannot be read");
		return 0;
	}
	while (count < 256 && posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);

		if (length < 5 || strcmp(name + length - 4, ".map") || !strcmp(name, "ui.map"))
			continue;
		if (!(paths[count] = malloc(MAXIMUM_PATH)))
			break;
		snprintf(paths[count], MAXIMUM_PATH, "%s%s", maps, name);
		count++;
	}
	posix_directory_close(directory);
	written = mcc_import_voices(mcc_import.install, (const char *const *)paths, count, import_folder(), thread_progress,
		NULL, report, report_size);
	for (index = 0; index < count; index++)
		free(paths[index]);
	return written;
}

static void *import_thread(void *context)
{
	char path[MAXIMUM_PATH], report[512] = "", maps[MAXIMUM_PATH];
	char *note = malloc(MAXIMUM_NOTE);
	struct ue4_pak *pak = NULL;
	const char *reason = NULL;
	struct timespec began, ended;
	long written = -1;
	long cores = sysconf(_SC_NPROCESSORS_ONLN);

	(void)context;
	clock_gettime(CLOCK_MONOTONIC, &began);
	snprintf(maps, sizeof(maps), "%s", mcc_import.maps_folder);
	if (!note)
	{
		snprintf(report, sizeof(report), "out of memory");
		goto finished;
	}
	snprintf(note, MAXIMUM_NOTE, NOTE_HEAD " %d\n", MCC_IMPORT_VERSION);
	sources_text(note + strlen(note), MAXIMUM_NOTE - strlen(note), mcc_import.install, maps, mcc_import.voices);
	/* (what was imported before goes first: a canceled or failed import
	leaves nothing half made) */
	note_path(path, sizeof(path));
	remove(path);
	mkdir(import_folder(), 0755);
	snprintf(path, sizeof(path), "%s/%s", mcc_import.install, PAK_RELATIVE);
	pak = ue4_pak_open(path, &reason);
	if (!pak)
	{
		snprintf(report, sizeof(report), "MCC's pak could not be read: %s", reason);
		goto finished;
	}
	{
		struct mcc_bake_options options;
		char bake_report[256];

		memset(&options, 0, sizeof(options));
		options.maps_folder = maps;
		options.out_folder = import_folder();
		options.only = mcc_import.only[0] ? mcc_import.only : NULL;
		options.threads = cores > 2 ? (int)cores - 1 : 1;
		options.max_size = 2048;
		options.progress = thread_progress;
		options.note = note;
		options.note_size = MAXIMUM_NOTE;
		options.say = thread_log;
		pthread_mutex_lock(&mcc_import.lock);
		mcc_import.step = "Textures";
		pthread_mutex_unlock(&mcc_import.lock);
		written = mcc_bake_textures(pak, &options, bake_report, sizeof(bake_report));
		snprintf(report, sizeof(report), "%s", bake_report);
	}
	if (written >= 0 && mcc_import.voices)
	{
		char voices_report[256] = "";
		long voices;

		pthread_mutex_lock(&mcc_import.lock);
		mcc_import.step = "Voices";
		mcc_import.progress = 0.0f;
		pthread_mutex_unlock(&mcc_import.lock);
		voices = voices_import(maps, voices_report, sizeof(voices_report));
		size_t length = strlen(report);

		snprintf(report + length, sizeof(report) - length, "; %s", voices_report);
		written = voices < 0 ? -1 : written + voices;
	}

finished:
	ue4_pak_close(pak);
	clock_gettime(CLOCK_MONOTONIC, &ended);
	{
		double seconds = (double)(ended.tv_sec - began.tv_sec) + (double)(ended.tv_nsec - began.tv_nsec) / 1e9;
		size_t length = strlen(report);

		snprintf(report + length, sizeof(report) - length, " in %.0f s", seconds);
	}
	platform_log("mcc import: %s", report);
	if (note && written >= 0)
	{
		size_t length = strlen(note);

		snprintf(note + length, MAXIMUM_NOTE - length, "report %s\n", report);
		note_path(path, sizeof(path));
		write_text(path, note);
	}
	free(note);
	pthread_mutex_lock(&mcc_import.lock);
	snprintf(mcc_import.report, sizeof(mcc_import.report), "%s", report);
	note_load();
	mcc_import.running = 0;
	mcc_import.state_time = 0;
	mcc_import.changes++;
	pthread_mutex_unlock(&mcc_import.lock);
	return NULL;
}

static void import_join(int wait)
{
	int running;

	pthread_mutex_lock(&mcc_import.lock);
	running = mcc_import.running;
	pthread_mutex_unlock(&mcc_import.lock);
	if (mcc_import.thread_started && (wait || !running))
	{
		pthread_join(mcc_import.thread, NULL);
		mcc_import.thread_started = 0;
	}
}

/* ---------- public code */

int mcc_import_enabled(void)
{
	return config_boolean("display.mcc_import") && !halo_server_mode() && !config_boolean("debug.null_renderer");
}

const char *mcc_import_report(void)
{
	return mcc_import.report;
}

int mcc_import_state(const char *maps_folder, int fresh)
{
	time_t now = time(NULL);
	int state;

	pthread_mutex_lock(&mcc_import.lock);
	if (mcc_import.running)
	{
		pthread_mutex_unlock(&mcc_import.lock);
		return _mcc_import_running;
	}
	if (!fresh && mcc_import.state_time && now - mcc_import.state_time < STATE_INTERVAL)
	{
		state = mcc_import.state;
		pthread_mutex_unlock(&mcc_import.lock);
		return state;
	}
	pthread_mutex_unlock(&mcc_import.lock);
	{
		char install[MAXIMUM_PATH], path[MAXIMUM_PATH], maps[MAXIMUM_PATH], *note, *now_sources;
		int found = mcc_import_install(install, sizeof(install));

		note_path(path, sizeof(path));
		note = file_text(path, MAXIMUM_NOTE);
		if (!note || strncmp(note, NOTE_HEAD " ", strlen(NOTE_HEAD) + 1))
		{
			state = found ? _mcc_import_none : _mcc_import_no_install;
		}
		else if (atoi(note + strlen(NOTE_HEAD) + 1) != MCC_IMPORT_VERSION || !found)
		{
			state = found ? _mcc_import_stale : _mcc_import_current;
		}
		else
		{
			/* (each source line of the note as it is now) */
			char *line, *next;

			maps_folder_host(maps_folder, maps, sizeof(maps));
			now_sources = malloc(64 * 1024);
			state = _mcc_import_current;
			if (now_sources)
			{
				sources_text(now_sources, 64 * 1024, install, maps, strstr(note, "\nsource voices ") != NULL);
				for (line = note; line && *line; line = next)
				{
					next = strchr(line, '\n');
					if (!next)
						break;
					*next++ = 0;
					if (!strncmp(line, "source ", 7))
					{
						char wanted[MAXIMUM_PATH + 80];

						snprintf(wanted, sizeof(wanted), "%s\n", line);
						if (!strstr(now_sources, wanted))
							state = _mcc_import_stale;
					}
				}
				free(now_sources);
			}
		}
		free(note);
	}
	pthread_mutex_lock(&mcc_import.lock);
	if (!mcc_import.running)
	{
		mcc_import.state = state;
		mcc_import.state_time = now;
	}
	pthread_mutex_unlock(&mcc_import.lock);
	return state;
}

int mcc_import_begin(const char *maps_folder, const char *only, int voices)
{
	char install[MAXIMUM_PATH];

	import_join(0);
	if (mcc_import.thread_started || !maps_folder || !mcc_import_install(install, sizeof(install)))
		return 0;
	pthread_mutex_lock(&mcc_import.lock);
	snprintf(mcc_import.install, sizeof(mcc_import.install), "%s", install);
	maps_folder_host(maps_folder, mcc_import.maps_folder, sizeof(mcc_import.maps_folder));
	snprintf(mcc_import.only, sizeof(mcc_import.only), "%s", only ? only : "");
	mcc_import.voices = voices;
	mcc_import.running = 1;
	mcc_import.cancel = 0;
	mcc_import.progress = 0.0f;
	mcc_import.step = "Textures";
	/* (nothing of the import is played while it is made) */
	free(mcc_import.bitmaps);
	mcc_import.bitmaps = NULL;
	mcc_import.bitmap_count = 0;
	mcc_import.noted = 1;
	mcc_import.changes++;
	pthread_mutex_unlock(&mcc_import.lock);
	platform_log("mcc import: from %s, the maps in %s%s%s", install, mcc_import.maps_folder, only ? ", only " : "",
		only ? only : "");
	if (pthread_create(&mcc_import.thread, NULL, import_thread, NULL) == 0)
		mcc_import.thread_started = 1;
	else
		import_thread(NULL);
	return 1;
}

int mcc_import_running(float *progress, const char **step)
{
	int running;

	pthread_mutex_lock(&mcc_import.lock);
	running = mcc_import.running;
	if (progress)
		*progress = mcc_import.progress;
	if (step)
		*step = mcc_import.step ? mcc_import.step : "";
	pthread_mutex_unlock(&mcc_import.lock);
	if (!running)
		import_join(0);
	return running;
}

void mcc_import_cancel(void)
{
	pthread_mutex_lock(&mcc_import.lock);
	mcc_import.cancel = 1;
	pthread_mutex_unlock(&mcc_import.lock);
}

void mcc_import_now(const char *maps_folder, const char *only, int voices)
{
	import_join(1);
	if (mcc_import_begin(maps_folder, only, voices))
		import_join(1);
}

/* a folder's files and folders deleted, then it; 0 if some are left */
static int remove_tree(const char *folder, int depth)
{
	void *directory = posix_directory_open(folder);
	char name[256];
	int whole = 1;

	if (!directory)
		return 1;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		char path[MAXIMUM_PATH + 256];
		struct posix_file_information information;

		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		snprintf(path, sizeof(path), "%s/%s", folder, name);
		if (posix_stat(path, &information) != 0)
			continue;
		if (information.flags & _posix_file_is_directory)
			whole &= depth < 32 && remove_tree(path, depth + 1);
		else
			whole &= remove(path) == 0;
	}
	posix_directory_close(directory);
	return posix_mcc_share_remove(folder, 1) && whole;
}

int mcc_import_remove(void)
{
	char path[MAXIMUM_PATH];
	int removed;

	import_join(1);
	pthread_mutex_lock(&mcc_import.lock);
	free(mcc_import.bitmaps);
	mcc_import.bitmaps = NULL;
	mcc_import.bitmap_count = 0;
	mcc_import.noted = 1;
	mcc_import.changes++;
	mcc_import.state_time = 0;
	mcc_import.report[0] = 0;
	pthread_mutex_unlock(&mcc_import.lock);
	/* (the note first: what is left is never played) */
	note_path(path, sizeof(path));
	remove(path);
	removed = remove_tree(import_folder(), 0);
	platform_log("mcc import: removed%s", removed ? "" : ", but some of its files are left");
	return removed;
}

unsigned long mcc_import_changes(void)
{
	unsigned long changes;

	pthread_mutex_lock(&mcc_import.lock);
	changes = mcc_import.changes;
	pthread_mutex_unlock(&mcc_import.lock);
	return changes << 1 | (mcc_import_enabled() ? 1UL : 0UL);
}

int mcc_import_bitmap_file(const char *tag, char *path, unsigned long size, unsigned int *stock_crc)
{
	long index;
	int found = 0;

	if (!tag || !mcc_import_enabled())
		return 0;
	pthread_mutex_lock(&mcc_import.lock);
	if (!mcc_import.running)
	{
		if (!mcc_import.noted)
			note_load();
		for (index = 0; index < mcc_import.bitmap_count && !found; index++)
		{
			if (!same_tag(mcc_import.bitmaps[index].tag, tag))
				continue;
			tag_file_path(path, size, tag, ".bitmap");
			*stock_crc = mcc_import.bitmaps[index].crc;
			found = 1;
		}
	}
	pthread_mutex_unlock(&mcc_import.lock);
	return found;
}

int mcc_import_tag_file(const char *tag, const char *extension, char *path, unsigned long size)
{
	unsigned long long bytes;
	unsigned long seconds;
	unsigned int crc;
	int running;

	if (!tag || !mcc_import_enabled())
		return 0;
	pthread_mutex_lock(&mcc_import.lock);
	running = mcc_import.running;
	pthread_mutex_unlock(&mcc_import.lock);
	if (running)
		return 0;
	/* (a bitmap's material maps only beside a bitmap the import made) */
	if (strcmp(extension, ".sound") && !mcc_import_bitmap_file(tag, path, size, &crc))
		return 0;
	tag_file_path(path, size, tag, extension);
	return file_stamp(path, &bytes, &seconds);
}
