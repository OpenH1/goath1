/*
DLSS_ADDON.C

The NVIDIA DLSS add-on (docs/temporal-aa.md, "The add-on"): NVIDIA DLSS is
not in the game's own package, which holds none of NVIDIA's files. On an
NVIDIA GPU with Vulkan (Linux x86-64, Windows x64) Settings > Video > Frame
Rate & Sync offers it (NVIDIA DLSS (DOWNLOAD, ~60 MB) ›), whose screen
(port/assets/menus/screens/dlss_addon.xml) shows NVIDIA's license, and
ACCEPT downloads it:

- what is downloaded is pinned by this build (dlss_addon_pin.h, written by
  tools/dlss_addon.py pin: the release's file's URL, size and SHA-256, and
  the license's), and refused if it is not those bytes;
- the archive (the module, NVIDIA's DLSS runtime and the licenses) is
  unpacked into <add-ons>/dlss.partial (dlss_addon_folder.h: the player's
  own folder, never the game's), its files checked, and only then renamed
  into place as <add-ons>/dlss, with installed.txt, its version, the last
  file written. A failure (no network, a refused download, a full disk, an
  archive that is not the add-on's) deletes dlss.partial: nothing is left
  half installed;
- the renderer loads the module as it starts (xgpu_vulkan_dlss.c): NGX's
  Vulkan extensions are the instance's and the device's from their making,
  and the backend does not make its device again, so DLSS takes effect
  after a restart, which the screen's RESTART NOW does (dlss_addon_restart:
  the game started again, its executable with the same arguments and
  environment, update_relaunch), offered at the menus alone, with no game
  played and no network game hosted or joined (pc_menu_restart_allowed);
- REMOVE DLSS ADD-ON deletes it (its folder renamed away first). Where the
  module is loaded and cannot be deleted (Windows), a remove file marks it,
  and the next start deletes it before the renderer loads anything.

HALO_DLSS_ADDON_URL (a test's) is a folder's address in place of the
release's (http:// on this machine, as update.h allows): the pinned files'
names are asked of it, and still checked against the pin.
*/

#include "dlss_addon.h"
#include "dlss_addon_folder.h"
#include "dlss_addon_pin.h"
#include "platform.h"
#include "update.h"
#include "p2p_internal.h"
#include "../game/menu_table.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the renderer's (d3d8_device.c): an NVIDIA GPU on Vulkan; DLSS drawn */
int halo_dlss_gpu(void);
int halo_temporal_dlss_drawn(void);
/* menu_functions.c's: the game may be restarted now (no game played, no
network game hosted or joined) */
int pc_menu_restart_allowed(void);
/* report.c's: this session ended as an exit ends it (1), or not after all
(0), before the game starts again */
void report_session_handover(int leaving);

/* the platform whose add-on this build downloads (tools/dlss_addon.py's
PLATFORMS), and its pin */
#if defined(HALO_WINDOWS)
#define DLSS_ADDON_PLATFORM "windows-x64"
#define DLSS_ADDON_URL DLSS_ADDON_URL_WINDOWS_X64
#define DLSS_ADDON_SIZE DLSS_ADDON_SIZE_WINDOWS_X64
#define DLSS_ADDON_SHA256 DLSS_ADDON_SHA256_WINDOWS_X64
#elif (defined(__x86_64__) && !defined(HALO_ANDROID) && !defined(HALO_MACOS)) || defined(DLSS_ADDON_TEST)
/* (and tools/harness/tests/dlss_addon.c's, on any computer) */
#define DLSS_ADDON_PLATFORM "linux-x86_64"
#define DLSS_ADDON_URL DLSS_ADDON_URL_LINUX_X86_64
#define DLSS_ADDON_SIZE DLSS_ADDON_SIZE_LINUX_X86_64
#define DLSS_ADDON_SHA256 DLSS_ADDON_SHA256_LINUX_X86_64
#endif

#define DLSS_PATH_SIZE 1200
#define DLSS_ERROR_SIZE 400
/* the most an archive may unpack to (its files: the module, the runtime,
the licenses) */
#define DLSS_MAXIMUM_FILES 8
/* NVIDIA's license, wrapped to the table's column (in characters), and the
most lines it may have */
#define DLSS_LICENSE_WIDTH 50
#define DLSS_LICENSE_MAXIMUM_LINES 2000
#define DLSS_NONE (-1)

/* updater.c's: a zip unpacked into a folder, as a release's is */
int updater_zip_unpack(const char *zip_path, const char *folder, unsigned long long maximum, char names[][256],
	int *name_count, char *error, size_t error_size);

enum
{
	_dlss_idle,
	/* NVIDIA's license being fetched, then shown (ACCEPT) */
	_dlss_license,
	_dlss_license_ready,
	_dlss_downloading,
	_dlss_installing,
	/* installed while the game runs: at the next start */
	_dlss_installed,
	/* removed while the game runs */
	_dlss_removed,
	_dlss_failed,
};

static struct
{
	SDL_AtomicInt state;
	/* the download's bytes so far and in all (its KB, an int's) */
	SDL_AtomicInt received_kb, total_kb;
	/* why it failed (written before the state says so) */
	char error[DLSS_ERROR_SIZE];
	/* NVIDIA's license, its text (ASCII) and each line's start and length */
	char *license;
	int line_count;
	int *line_start;
	unsigned char *line_length;
	/* the table: its first row shown */
	short first;
	/* the removal's words: deleted now, or at the next start */
	int remove_at_start;
} dlss_addon;

#ifdef DLSS_ADDON_PLATFORM
/* ---------- the folders */

static void dlss_path(char *path, size_t size, const char *folder, const char *name)
{
	snprintf(path, size, "%s" DLSS_ADDON_SEPARATOR "%s", folder, name);
}

static int dlss_exists(const char *path)
{
	return SDL_GetPathInfo(path, NULL);
}

/* the folder at path and those it is in (as mkdir -p); 1 when it is there */
static int dlss_make_folders(const char *path)
{
	char folder[DLSS_PATH_SIZE];
	size_t index;

	snprintf(folder, sizeof(folder), "%s", path);
	for (index = 1; folder[index]; index++)
	{
		if (folder[index] == DLSS_ADDON_SEPARATOR[0] && folder[index - 1] != ':')
		{
			folder[index] = 0;
			if (!dlss_exists(folder))
				update_make_directory(folder);
			folder[index] = DLSS_ADDON_SEPARATOR[0];
		}
	}
	if (!dlss_exists(folder))
		update_make_directory(folder);
	return dlss_exists(folder);
}

/* the file or folder at path deleted, and what a folder holds (a few
levels); 1 when it is gone */
static int dlss_delete_tree(const char *path, int depth)
{
	int count = 0, index;
	char **names;

	if (!dlss_exists(path))
		return 1;
	names = depth < 4 ? SDL_GlobDirectory(path, NULL, 0, &count) : NULL;
	for (index = 0; names && index < count; index++)
	{
		char child[DLSS_PATH_SIZE];

		dlss_path(child, sizeof(child), path, names[index]);
		dlss_delete_tree(child, depth + 1);
	}
	SDL_free(names);
	update_delete_file(path);
	return !dlss_exists(path);
}

/* the add-on's folder in the add-ons' (its name: DLSS_ADDON_NAME, or
DLSS_ADDON_NAME and a suffix); 0 without a home folder */
static int dlss_folder(char *path, size_t size, const char *suffix)
{
	char folder[1024];

	if (!dlss_addon_folder(folder, sizeof(folder)))
		return 0;
	snprintf(path, size, "%s" DLSS_ADDON_SEPARATOR DLSS_ADDON_NAME "%s", folder, suffix);
	return 1;
}

/* a small file's text (its first line, at most size - 1 bytes) */
static int dlss_read_line(const char *path, char *text, size_t size)
{
	SDL_IOStream *file = SDL_IOFromFile(path, "rb");
	size_t count;

	text[0] = 0;
	if (!file)
		return 0;
	count = SDL_ReadIO(file, text, size - 1);
	SDL_CloseIO(file);
	text[count] = 0;
	text[strcspn(text, "\r\n")] = 0;
	return text[0] != 0;
}

static int dlss_write_text(const char *path, const char *text)
{
	SDL_IOStream *file = SDL_IOFromFile(path, "wb");
	int written;

	if (!file)
		return 0;
	written = SDL_WriteIO(file, text, strlen(text)) == strlen(text);
	return SDL_CloseIO(file) && written;
}

/* the installed add-on's version (installed.txt), in folder; 0 if none is
installed there, or it is to be removed */
static int dlss_installed_in(const char *folder, char *version, size_t size)
{
	char path[DLSS_PATH_SIZE];

	version[0] = 0;
	dlss_path(path, sizeof(path), folder, DLSS_ADDON_REMOVE_MARK);
	if (dlss_exists(path))
		return 0;
	dlss_path(path, sizeof(path), folder, DLSS_ADDON_MODULE);
	if (!dlss_exists(path))
		return 0;
	dlss_path(path, sizeof(path), folder, DLSS_ADDON_INSTALLED);
	return dlss_read_line(path, version, size);
}

static int dlss_installed_version(char *version, size_t size)
{
	char folder[DLSS_PATH_SIZE];

	version[0] = 0;
	return dlss_folder(folder, sizeof(folder), "") && dlss_installed_in(folder, version, size);
}

/* ---------- checking and installing (the download's thread) */

/* the file's size and SHA-256 (lowercase hex), into hex[65]; 0 if it could
not be read whole */
static int dlss_file_digest(const char *path, unsigned long long *size, char *hex)
{
	SDL_IOStream *stream = SDL_IOFromFile(path, "rb");
	Sint64 length = stream ? SDL_GetIOSize(stream) : -1;
	unsigned long long total = 0;
	struct p2p_sha256 context;
	unsigned char buffer[16384], digest[32];
	size_t count;
	int index;

	if (!stream)
		return 0;
	p2p_sha256_begin(&context);
	while ((count = SDL_ReadIO(stream, buffer, sizeof(buffer))) > 0)
	{
		p2p_sha256_add(&context, buffer, (int)count);
		total += count;
	}
	SDL_CloseIO(stream);
	if (length < 0 || total != (unsigned long long)length)
		return 0;
	p2p_sha256_end(&context, digest);
	for (index = 0; index < 32; index++)
		snprintf(hex + index * 2, 3, "%02x", digest[index]);
	*size = total;
	return 1;
}

/* the file at path is the pinned one (its size and SHA-256); else why in
error */
static int dlss_verify(const char *path, unsigned long long size, const char *sha256, char *error, size_t error_size)
{
	unsigned long long had = 0;
	char hex[65];

	if (!sha256[0])
	{
		snprintf(error, error_size, "this build pins no add-on");
		return 0;
	}
	if (!dlss_file_digest(path, &had, hex))
	{
		snprintf(error, error_size, "the download could not be read");
		return 0;
	}
	if (had != size || strcmp(hex, sha256))
	{
		snprintf(error, error_size, "the download is not the add-on this build pins (%llu bytes, SHA-256 %.16s..., "
			"not %llu bytes, %.16s...)", had, hex, size, sha256);
		return 0;
	}
	return 1;
}

/* the archive at zip_path, checked against the pin (size, sha256), unpacked
into <add-ons>/dlss.partial/files and its files checked, then put in place
as <add-ons>/dlss (the one there before replaced), installed.txt (version)
written last; 1 on success, else 0 and why in error. Either way nothing of
dlss.partial is left */
static int dlss_install_zip(const char *addons, const char *zip_path, unsigned long long size, const char *sha256,
	const char *version, char *error, size_t error_size)
{
	char partial[DLSS_PATH_SIZE], files[DLSS_PATH_SIZE], target[DLSS_PATH_SIZE], old[DLSS_PATH_SIZE];
	char path[DLSS_PATH_SIZE + 64];
	char names[64][256];
	int name_count = 0, index, has_module = 0, has_runtime = 0, has_license = 0, installed = 0;

	dlss_path(partial, sizeof(partial), addons, DLSS_ADDON_NAME ".partial");
	dlss_path(files, sizeof(files), partial, "files");
	dlss_path(target, sizeof(target), addons, DLSS_ADDON_NAME);
	dlss_path(old, sizeof(old), addons, DLSS_ADDON_NAME ".old");
	if (!dlss_verify(zip_path, size, sha256, error, error_size))
		goto done;
	dlss_delete_tree(files, 0);
	if (!dlss_make_folders(files))
	{
		snprintf(error, error_size, "could not make the folder %s", files);
		goto done;
	}
	if (!updater_zip_unpack(zip_path, files, size, names, &name_count, error, error_size))
		goto done;
	for (index = 0; index < name_count; index++)
	{
		if (strchr(names[index], '/'))
		{
			snprintf(error, error_size, "the archive has a folder (%s): it is not the add-on's", names[index]);
			goto done;
		}
		has_module |= !strcmp(names[index], DLSS_ADDON_MODULE);
		has_runtime |= !strcmp(names[index], DLSS_ADDON_RUNTIME);
		has_license |= !strcmp(names[index], DLSS_ADDON_LICENSE);
	}
	if (name_count > DLSS_MAXIMUM_FILES || !has_module || !has_runtime || !has_license)
	{
		snprintf(error, error_size, "the archive is not the add-on's (no %s)", !has_module ? DLSS_ADDON_MODULE :
			!has_runtime ? DLSS_ADDON_RUNTIME : !has_license ? DLSS_ADDON_LICENSE : "file it should have");
		goto done;
	}
	dlss_path(path, sizeof(path), files, DLSS_ADDON_INSTALLED);
	snprintf(names[0], sizeof(names[0]), "%s\n", version);
	if (!dlss_write_text(path, names[0]))
	{
		snprintf(error, error_size, "could not write %s (is the disk full?)", path);
		goto done;
	}
	/* (the add-on there before, an older version's, moved aside: put back
	if the new one cannot take its place) */
	dlss_delete_tree(old, 0);
	if (dlss_exists(target) && rename(target, old) != 0)
	{
		snprintf(error, error_size, "the add-on there could not be replaced (it is in use): restart the game and "
			"try again");
		goto done;
	}
	if (rename(files, target) != 0)
	{
		snprintf(error, error_size, "could not put the add-on in %s", target);
		if (dlss_exists(old))
			rename(old, target);
		goto done;
	}
	dlss_delete_tree(old, 0);
	installed = 1;

done:
	dlss_delete_tree(partial, 0);
	return installed;
}

/* the add-on deleted (its folder renamed away first, so the next start
never loads half of it); where a file cannot be deleted (its module loaded
on Windows) the remove mark is left in the folder, and the next start
deletes it (dlss_addon_start). 1 deleted now, 2 at the next start, 0 if it
could not be */
static int dlss_remove_in(const char *addons, char *error, size_t error_size)
{
	char target[DLSS_PATH_SIZE], removed[DLSS_PATH_SIZE], mark[DLSS_PATH_SIZE + 16];

	dlss_path(target, sizeof(target), addons, DLSS_ADDON_NAME);
	dlss_path(removed, sizeof(removed), addons, DLSS_ADDON_NAME ".removed");
	if (!dlss_exists(target))
		return 1;
	dlss_delete_tree(removed, 0);
	if (rename(target, removed) == 0)
	{
		if (dlss_delete_tree(removed, 0))
			return 1;
		/* (a file of it held open: deleted at the next start) */
		return 2;
	}
	dlss_path(mark, sizeof(mark), target, DLSS_ADDON_REMOVE_MARK);
	if (dlss_write_text(mark, "removed\n"))
		return 2;
	snprintf(error, error_size, "could not remove %s", target);
	return 0;
}

/* (as the game starts: what an install or a removal left) */
static void dlss_clean_in(const char *addons)
{
	char path[DLSS_PATH_SIZE], mark[DLSS_PATH_SIZE + 16];

	dlss_path(path, sizeof(path), addons, DLSS_ADDON_NAME ".partial");
	dlss_delete_tree(path, 0);
	dlss_path(path, sizeof(path), addons, DLSS_ADDON_NAME ".removed");
	dlss_delete_tree(path, 0);
	dlss_path(path, sizeof(path), addons, DLSS_ADDON_NAME ".old");
	dlss_delete_tree(path, 0);
	dlss_path(path, sizeof(path), addons, DLSS_ADDON_NAME);
	dlss_path(mark, sizeof(mark), path, DLSS_ADDON_REMOVE_MARK);
	if (dlss_exists(mark))
		dlss_delete_tree(path, 0);
}

void dlss_addon_start(void)
{
	char addons[1024];

	if (dlss_addon_folder(addons, sizeof(addons)))
		dlss_clean_in(addons);
}

/* ---------- the downloads */

/* the pinned file's address: the release's, or HALO_DLSS_ADDON_URL's
folder's (its name the release's) */
static void dlss_url(char *url, size_t size, const char *pinned)
{
	const char *base = getenv("HALO_DLSS_ADDON_URL");
	const char *name = strrchr(pinned, '/');

	if (base && *base && name)
		snprintf(url, size, "%s%s%s", base, base[strlen(base) - 1] == '/' ? "" : "/", name + 1);
	else
		snprintf(url, size, "%s", pinned);
}

static void dlss_progress(void *context, unsigned long long received, unsigned long long total)
{
	(void)context;
	SDL_SetAtomicInt(&dlss_addon.received_kb, (int)(received / 1024));
	if (total)
		SDL_SetAtomicInt(&dlss_addon.total_kb, (int)(total / 1024));
}

/* url into the file at path, no larger than maximum: https:// streamed
(update_download, its redirects followed: GitHub's file host), a test's
http:// on this machine had whole; 1 on success, else why in error */
static int dlss_fetch(const char *url, const char *path, unsigned long long maximum, char *error, size_t error_size)
{
	struct update_response response;
	SDL_IOStream *file;
	int written = 0;

	if (!strncmp(url, "https://", 8))
		return update_download(url, path, maximum, dlss_progress, NULL, error, (int)error_size);
	memset(&response, 0, sizeof(response));
	if (!update_https_request("GET", url, NULL, NULL, 0, (unsigned long)maximum, &response, error, (int)error_size))
		return 0;
	if (response.status != 200 || !response.body)
		snprintf(error, error_size, "the server answered %d", response.status);
	else if (!(file = SDL_IOFromFile(path, "wb")))
		snprintf(error, error_size, "could not write %s", path);
	else
	{
		written = SDL_WriteIO(file, response.body, response.body_size) == response.body_size;
		written = SDL_CloseIO(file) && written;
		if (!written)
			snprintf(error, error_size, "could not write %s (is the disk full?)", path);
		else
			dlss_progress(NULL, response.body_size, response.body_size);
	}
	free(response.body);
	return written;
}

static void dlss_fail(const char *why)
{
	snprintf(dlss_addon.error, sizeof(dlss_addon.error), "%s", why);
	platform_log("dlss: the add-on: %s", why);
	SDL_SetAtomicInt(&dlss_addon.state, _dlss_failed);
}

/* NVIDIA's license (as the game shows it: ASCII, its typographic marks as
plain ones) wrapped into lines of DLSS_LICENSE_WIDTH, its paragraphs kept */
static void dlss_license_wrap(char *text)
{
	size_t read = 0, write = 0;
	int line = 0, start = 0, last_space = -1, column = 0;

	/* (UTF-8's quotes, dashes and the trademark sign as ASCII; the rest '?') */
	while (text[read])
	{
		unsigned char byte = (unsigned char)text[read];

		if (byte < 0x80)
		{
			if (byte != '\r')
				text[write++] = (char)(byte == '\t' ? ' ' : byte);
			read++;
			continue;
		}
		if (byte == 0xE2 && (unsigned char)text[read + 1] == 0x80)
		{
			unsigned char third = (unsigned char)text[read + 2];

			text[write++] = third == 0x98 || third == 0x99 ? '\'' : third == 0x9C || third == 0x9D ? '"' :
				third == 0x93 || third == 0x94 ? '-' : third == 0xA2 ? '*' : '?';
			read += third ? 3 : 2;
			continue;
		}
		if (byte == 0xE2 && (unsigned char)text[read + 1] == 0x84 && (unsigned char)text[read + 2] == 0xA2)
		{
			/* (the trademark sign, 3 bytes, as "TM": the text never grows) */
			text[write++] = 'T';
			text[write++] = 'M';
			read += 3;
			continue;
		}
		if (byte == 0xC2 && (unsigned char)text[read + 1] == 0xA0)
		{
			text[write++] = ' ';
			read += 2;
			continue;
		}
		text[write++] = '?';
		for (read++; ((unsigned char)text[read] & 0xC0) == 0x80; read++)
		{
		}
	}
	text[write] = 0;
	dlss_addon.line_count = 0;
	if (!dlss_addon.line_start)
		dlss_addon.line_start = malloc(sizeof(int) * DLSS_LICENSE_MAXIMUM_LINES);
	if (!dlss_addon.line_length)
		dlss_addon.line_length = malloc(DLSS_LICENSE_MAXIMUM_LINES);
	if (!dlss_addon.line_start || !dlss_addon.line_length)
		return;
	for (read = 0; line < DLSS_LICENSE_MAXIMUM_LINES; read++)
	{
		char character = text[read];

		if (character == '\n' || !character || column >= DLSS_LICENSE_WIDTH)
		{
			int end = (int)read;

			/* (a long line broken at its last space) */
			if (character && character != '\n' && last_space > start)
				end = last_space;
			dlss_addon.line_start[line] = start;
			dlss_addon.line_length[line] = (unsigned char)(end - start);
			line++;
			if (!character)
				break;
			start = character == '\n' ? (int)read + 1 : end == last_space ? end + 1 : end;
			read = (size_t)start - 1;
			column = 0;
			last_space = -1;
			continue;
		}
		if (character == ' ')
			last_space = (int)read;
		column++;
	}
	dlss_addon.line_count = line;
}

static int SDLCALL dlss_license_thread(void *context)
{
	char addons[1024], path[DLSS_PATH_SIZE], url[2048], error[DLSS_ERROR_SIZE] = "";
	SDL_IOStream *file;
	Sint64 size;

	(void)context;
	if (!dlss_addon_folder(addons, sizeof(addons)) || !dlss_make_folders(addons))
	{
		dlss_fail("there is no folder of yours to put it in (no home folder)");
		return 0;
	}
	dlss_path(path, sizeof(path), addons, DLSS_ADDON_NAME "-license.partial");
	dlss_url(url, sizeof(url), DLSS_ADDON_LICENSE_URL);
	if (!dlss_fetch(url, path, DLSS_ADDON_LICENSE_SIZE + 1, error, sizeof(error)) ||
		!dlss_verify(path, DLSS_ADDON_LICENSE_SIZE, DLSS_ADDON_LICENSE_SHA256, error, sizeof(error)))
	{
		char why[DLSS_ERROR_SIZE + 64];

		update_delete_file(path);
		snprintf(why, sizeof(why), "NVIDIA's license could not be had: %s", error);
		dlss_fail(why);
		return 0;
	}
	file = SDL_IOFromFile(path, "rb");
	size = file ? SDL_GetIOSize(file) : -1;
	free(dlss_addon.license);
	dlss_addon.license = size > 0 ? malloc((size_t)size + 8) : NULL;
	if (!dlss_addon.license || SDL_ReadIO(file, dlss_addon.license, (size_t)size) != (size_t)size)
	{
		if (file)
			SDL_CloseIO(file);
		update_delete_file(path);
		dlss_fail("NVIDIA's license could not be read");
		return 0;
	}
	SDL_CloseIO(file);
	update_delete_file(path);
	dlss_addon.license[size] = 0;
	dlss_license_wrap(dlss_addon.license);
	SDL_SetAtomicInt(&dlss_addon.state, _dlss_license_ready);
	return 0;
}

static int SDLCALL dlss_download_thread(void *context)
{
	char addons[1024], partial[DLSS_PATH_SIZE], zip[DLSS_PATH_SIZE + 32], url[2048];
	char error[DLSS_ERROR_SIZE] = "", why[DLSS_ERROR_SIZE + 64];

	(void)context;
	if (!dlss_addon_folder(addons, sizeof(addons)))
	{
		dlss_fail("there is no folder of yours to put it in (no home folder)");
		return 0;
	}
	dlss_path(partial, sizeof(partial), addons, DLSS_ADDON_NAME ".partial");
	dlss_delete_tree(partial, 0);
	if (!dlss_make_folders(partial))
	{
		snprintf(why, sizeof(why), "could not make the folder %s", partial);
		dlss_fail(why);
		return 0;
	}
	dlss_path(zip, sizeof(zip), partial, "download.zip");
	dlss_url(url, sizeof(url), DLSS_ADDON_URL);
	platform_log("dlss: downloading the add-on %s (%s)", DLSS_ADDON_VERSION, url);
	SDL_SetAtomicInt(&dlss_addon.total_kb, (int)(DLSS_ADDON_SIZE / 1024));
	if (!dlss_fetch(url, zip, DLSS_ADDON_SIZE + 1, error, sizeof(error)))
	{
		dlss_delete_tree(partial, 0);
		snprintf(why, sizeof(why), "the download failed (%s)", error);
		dlss_fail(why);
		return 0;
	}
	SDL_SetAtomicInt(&dlss_addon.state, _dlss_installing);
	if (!dlss_install_zip(addons, zip, DLSS_ADDON_SIZE, DLSS_ADDON_SHA256, DLSS_ADDON_VERSION, error, sizeof(error)))
	{
		snprintf(why, sizeof(why), "%s", error);
		dlss_fail(why);
		return 0;
	}
	platform_log("dlss: the add-on %s installed in %s" DLSS_ADDON_SEPARATOR DLSS_ADDON_NAME ": DLSS at the next start",
		DLSS_ADDON_VERSION, addons);
	SDL_SetAtomicInt(&dlss_addon.state, _dlss_installed);
	return 0;
}

static void dlss_begin(int (SDLCALL *function)(void *), int state)
{
	SDL_Thread *thread;

	dlss_addon.error[0] = 0;
	SDL_SetAtomicInt(&dlss_addon.received_kb, 0);
	SDL_SetAtomicInt(&dlss_addon.state, state);
	thread = SDL_CreateThread(function, "dlss addon", NULL);
	if (!thread)
		dlss_fail("could not start its thread");
	else
		SDL_DetachThread(thread);
}
#else
/* (no add-on for this platform: none installed, none removed) */
static int dlss_installed_version(char *version, size_t size)
{
	(void)size;
	version[0] = 0;
	return 0;
}

static int dlss_remove_in(const char *addons, char *error, size_t error_size)
{
	(void)addons;
	snprintf(error, error_size, "no add-on on this platform");
	return 0;
}

void dlss_addon_start(void)
{
}
#endif

/* ---------- what the game asks */

static int dlss_pinned(void)
{
#ifdef DLSS_ADDON_PLATFORM
	return DLSS_ADDON_VERSION[0] != 0 && DLSS_ADDON_SHA256[0] != 0 && DLSS_ADDON_URL[0] != 0;
#else
	return 0;
#endif
}

int dlss_addon_installed(void)
{
	char version[64];

	return dlss_installed_version(version, sizeof(version));
}

int dlss_addon_offered(void)
{
	char version[64];

	if (!dlss_pinned() || !halo_dlss_gpu() || halo_temporal_dlss_drawn())
		return 0;
	/* (an older add-on's version: this build's offered in its place) */
	return !dlss_installed_version(version, sizeof(version)) || strcmp(version, DLSS_ADDON_VERSION);
}

int dlss_addon_restart_needed(void)
{
	int state = SDL_GetAtomicInt(&dlss_addon.state);

	/* (installed: DLSS at the next start; removed: its files deleted, or
	DLSS gone, at the next start) */
	return state == _dlss_installed ||
		(state == _dlss_removed && (dlss_addon.remove_at_start || halo_temporal_dlss_drawn()));
}

/* RESTART NOW: the game started again; returns only if it could not be */
static int dlss_addon_restart(void)
{
#ifdef DLSS_ADDON_PLATFORM
	if (!dlss_addon_restart_needed() || !pc_menu_restart_allowed())
		return 0;
	platform_log("dlss: restarting the game for the add-on");
	/* (this session ended first, as an exit ends it, as updater.c's update
	does: the game started again finds no session that did not exit, which
	it would take for a crash) */
	report_session_handover(1);
	if (update_relaunch())
		exit(EXIT_SUCCESS);
	report_session_handover(0);
	snprintf(dlss_addon.error, sizeof(dlss_addon.error), "the game could not be restarted");
	platform_log("dlss: the game could not be restarted: quit and start it again");
#endif
	return 0;
}

const char *dlss_addon_attribution(void)
{
	if (!dlss_addon_installed() && !halo_temporal_dlss_drawn())
		return NULL;
	return "Uses NVIDIA DLSS (the NVIDIA DLSS SDK). NVIDIA, NVIDIA RTX and DLSS are trademarks of NVIDIA "
		"Corporation.";
}

/* ---------- the screen (screens/dlss_addon.xml) */

void dlss_addon_screen_opened(void)
{
	int state = SDL_GetAtomicInt(&dlss_addon.state);

	dlss_addon.first = 0;
#ifdef DLSS_ADDON_PLATFORM
	if ((state == _dlss_idle || state == _dlss_failed || state == _dlss_removed) && dlss_addon_offered())
		dlss_begin(dlss_license_thread, _dlss_license);
#else
	(void)state;
#endif
}

static void dlss_megabytes(char *text, size_t size, unsigned long long kilobytes)
{
	snprintf(text, size, "%llu", (kilobytes + 512) / 1024);
}

int dlss_addon_value(const char *name, char *value, unsigned long size)
{
	int state = SDL_GetAtomicInt(&dlss_addon.state);
	int installed = dlss_addon_installed(), drawn = halo_temporal_dlss_drawn();
	char total[24], received[24];

	value[0] = 0;
#ifdef DLSS_ADDON_PLATFORM
	dlss_megabytes(total, sizeof(total), DLSS_ADDON_SIZE / 1024);
#else
	snprintf(total, sizeof(total), "0");
#endif
	dlss_megabytes(received, sizeof(received), (unsigned long long)SDL_GetAtomicInt(&dlss_addon.received_kb));
	if (!strcmp(name, "dlss.detail"))
	{
		char version[64];

		if (dlss_installed_version(version, sizeof(version)))
			snprintf(value, size, "Add-on %s", version);
		else if (dlss_pinned())
			snprintf(value, size, "Download, %s MB", total);
	}
	else if (!strcmp(name, "dlss.heading"))
	{
		snprintf(value, size, "%s", state == _dlss_downloading ? "Downloading NVIDIA DLSS" :
			state == _dlss_installing ? "Installing NVIDIA DLSS" :
			state == _dlss_installed ? "NVIDIA DLSS is installed" :
			state == _dlss_removed ? "NVIDIA DLSS is removed" :
			state == _dlss_failed ? "NVIDIA DLSS was not installed" :
			drawn ? "NVIDIA DLSS is on this computer" :
			installed ? "NVIDIA DLSS is installed" : "NVIDIA DLSS");
	}
	else if (!strcmp(name, "dlss.status"))
	{
		if (state == _dlss_downloading)
			snprintf(value, size, "%s of %s MB downloaded.", received, total);
		else if (state == _dlss_installing)
			snprintf(value, size, "Checking the download and putting it in place.");
		else if (state == _dlss_installed)
			snprintf(value, size, "%s", dlss_addon.error[0] ? "The game could not be restarted: quit and start it "
				"again, then choose NVIDIA DLSS in Upscaler." : pc_menu_restart_allowed() ?
				"DLSS takes effect after a restart. Restart now, then choose NVIDIA DLSS in Upscaler." :
				"DLSS takes effect after a restart: leave this game, then restart here, or quit and start the "
				"game again.");
		else if (state == _dlss_removed)
			snprintf(value, size, dlss_addon.remove_at_start ? "Its files are deleted when the game starts again." :
				"Its files are deleted. DLSS stays until the game is restarted.");
		else if (state == _dlss_failed)
			snprintf(value, size, "Nothing was installed: %s.", dlss_addon.error);
		else if (drawn)
			snprintf(value, size, "Choose NVIDIA DLSS in Upscaler. Remove deletes the add-on.");
		else if (installed)
			snprintf(value, size, "%s", halo_dlss_gpu() ? "DLSS did not start: restart the game, or update NVIDIA's "
				"driver (DLSS needs its NGX). Remove deletes the add-on." :
				"DLSS needs an NVIDIA RTX graphics card with Vulkan. Remove deletes the add-on.");
		else if (!halo_dlss_gpu())
			snprintf(value, size, "DLSS needs an NVIDIA RTX graphics card, drawn with Vulkan. This computer's is "
				"not one.");
		else if (!dlss_pinned())
			snprintf(value, size, "This build has no DLSS add-on to download. The release builds have it.");
		else if (state == _dlss_license)
			snprintf(value, size, "Getting NVIDIA's license.");
		else
			snprintf(value, size, "NVIDIA DLSS is NVIDIA's software, under its license here. Accept downloads it "
				"(%s MB) into your own folder, not the game's.", total);
	}
	else if (!strcmp(name, "dlss.credit"))
	{
		const char *credit = dlss_addon_attribution();

		if (credit)
			snprintf(value, size, "%s", credit);
	}
	else if (!strcmp(name, "dlss.license.empty"))
	{
		snprintf(value, size, "%s", state == _dlss_license ? "Getting NVIDIA's license..." :
			state == _dlss_failed || state == _dlss_removed ? "No license to show." : installed ?
			"The add-on's license is NVIDIA-DLSS-LICENSE.txt in its folder." : "No license to show.");
	}
	else if (!strcmp(name, "dlss.can_accept"))
	{
		if (state == _dlss_license_ready && dlss_addon_offered())
			snprintf(value, size, "yes");
	}
	else if (!strcmp(name, "dlss.can_restart"))
	{
		if (dlss_addon_restart_needed() && pc_menu_restart_allowed())
			snprintf(value, size, "yes");
	}
	else if (!strcmp(name, "dlss.can_remove"))
	{
		if (installed && state != _dlss_downloading && state != _dlss_installing)
			snprintf(value, size, "yes");
	}
	return value[0] != 0;
}

int dlss_addon_action(const char *name)
{
	int state = SDL_GetAtomicInt(&dlss_addon.state);

	if (!strcmp(name, "dlss accept"))
	{
#ifdef DLSS_ADDON_PLATFORM
		if (state != _dlss_license_ready || !dlss_addon_offered())
			return 0;
		dlss_begin(dlss_download_thread, _dlss_downloading);
		return 1;
#else
		return 0;
#endif
	}
	if (!strcmp(name, "dlss remove"))
	{
		char addons[1024], error[DLSS_ERROR_SIZE] = "";
		int removed;

		if (!dlss_addon_installed() || state == _dlss_downloading || state == _dlss_installing ||
			!dlss_addon_folder(addons, sizeof(addons)))
		{
			return 0;
		}
		removed = dlss_remove_in(addons, error, sizeof(error));
		if (!removed)
		{
			snprintf(dlss_addon.error, sizeof(dlss_addon.error), "%s", error);
			platform_log("dlss: the add-on: %s", error);
			return 0;
		}
		dlss_addon.remove_at_start = removed == 2;
		platform_log("dlss: the add-on removed%s", removed == 2 ? " (its files at the next start)" : "");
		SDL_SetAtomicInt(&dlss_addon.state, _dlss_removed);
		return 1;
	}
	if (!strcmp(name, "dlss restart"))
		return dlss_addon_restart();
	if (!strcmp(name, "dlss page up") || !strcmp(name, "dlss page down"))
	{
		dlss_addon.first = (short)(dlss_addon.first + (!strcmp(name, "dlss page up") ? -12 : 12));
		return 1;
	}
	return 0;
}

/* ---------- the table: NVIDIA's license's lines */

static int dlss_license_shown(void)
{
	return SDL_GetAtomicInt(&dlss_addon.state) == _dlss_license_ready && dlss_addon.line_start &&
		dlss_addon.line_length;
}

static void dlss_source_update(struct menu_table_view *view)
{
	short count = dlss_license_shown() ? (short)dlss_addon.line_count : 0;
	short shown = view->shown, focused = view->focused;

	/* (the focus kept off the table's ends while there is more past them,
	the lines moving instead) */
	if (focused == shown - 1 && dlss_addon.first + shown < count)
	{
		dlss_addon.first++;
		view->focus = --focused;
	}
	else if (focused == 0 && dlss_addon.first > 0)
	{
		dlss_addon.first--;
		view->focus = ++focused;
	}
	if (dlss_addon.first > count - shown)
		dlss_addon.first = (short)(count - shown);
	if (dlss_addon.first < 0)
		dlss_addon.first = 0;
	view->count = count;
	view->first = dlss_addon.first;
	view->chosen = DLSS_NONE;
	view->focus_titles = 0;
}

static int dlss_source_heading(short index, unsigned short *words, short size)
{
	(void)index;
	(void)words;
	(void)size;
	return 0;
}

static void dlss_source_cell(short index, char const *column, int focused, struct menu_table_cell *cell)
{
	int length, character;

	(void)focused;
	cell->frame = DLSS_NONE;
	if (strcmp(column, "line") || !dlss_license_shown() || index < 0 || index >= dlss_addon.line_count)
		return;
	length = dlss_addon.line_length[index];
	if (length > MENU_TABLE_TEXT_LENGTH - 1)
		length = MENU_TABLE_TEXT_LENGTH - 1;
	for (character = 0; character < length; character++)
		cell->text[character] = (unsigned char)dlss_addon.license[dlss_addon.line_start[index] + character];
	cell->text[length] = 0;
}

struct menu_table_source const dlss_addon_license_source =
{
	dlss_source_update, dlss_source_heading, dlss_source_cell, dlss_addon_screen_opened, NULL
};
