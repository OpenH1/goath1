/*
DLSS_ADDON.C (test)

The NVIDIA DLSS add-on's install and removal (port/linux/src/dlss_addon.c,
the whole file: src/under_test.inc), with updater.c's zip unpacking
(zip_functions.inc) and the real SHA-256 (p2p_crypto.c), on this
computer's files: an archive checked against its pin and put in place, a
tampered one or one that is not the add-on's refused with nothing left
half installed, a removal, and what a crash left deleted at the next start.
test_dlss_addon_install.py makes the archives and runs each case in a process of
its own: <case> <add-ons folder> <zip> <size> <sha256>.

Compiled for this computer, as update_fallback.c is (update_sdl.h's SDL).
*/

#include "harness.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

/* ---------- the crypto, as whole files (test_release_signature.py's) */

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#pragma clang optimize off
#include "monocypher.inc"
#define hash_reduce ed25519_hash_reduce
#include "monocypher_ed25519.inc"
#undef hash_reduce
#pragma clang optimize on
#pragma clang diagnostic pop
#undef FOR
#undef COPY
#undef ZERO
#undef WIPE_CTX
#undef WIPE_BUFFER
#undef MIN

#include "p2p_crypto.inc"

void posix_random_bytes(void *buffer, posix_ulong size)
{
	memset(buffer, 0, size);
}

void p2p_hex(const unsigned char *bytes, int size, char *text)
{
	int index;

	for (index = 0; index < size; index++)
		sprintf(text + index * 2, "%02x", bytes[index]);
}

#include "update.h"
#include "update_sdl.h"

/* ---------- SDL's folder listing, and update.h's files, as they are */

static char **SDL_GlobDirectory(const char *path, const char *pattern, int flags, int *count)
{
	DIR *directory = opendir(path);
	struct dirent *entry;
	char **names = calloc(64, sizeof(char *));
	int found = 0;

	(void)pattern;
	(void)flags;
	*count = 0;
	if (!directory)
	{
		free(names);
		return NULL;
	}
	while ((entry = readdir(directory)) && found < 63)
	{
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, ".."))
			names[found++] = strdup(entry->d_name);
	}
	closedir(directory);
	*count = found;
	return names;
}

/* (SDL_GlobDirectory's names freed with the array, as SDL's one allocation is) */
#define SDL_free(names) dlss_test_free((void *)(names))
static void dlss_test_free(void *pointer)
{
	char **names = pointer;
	int index;

	for (index = 0; names && names[index]; index++)
		free(names[index]);
	free(names);
}

int update_make_executable(const char *path) { (void)path; return 1; }
void update_delete_file(const char *path) { if (remove(path)) rmdir(path); }
int update_make_directory(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST; }
int update_download(const char *url, const char *path, unsigned long long maximum, update_progress_proc progress,
	void *context, char *error, int error_size)
{
	(void)url, (void)path, (void)maximum, (void)progress, (void)context;
	snprintf(error, (size_t)error_size, "no network in the test");
	return 0;
}
int update_https_request(const char *method, const char *url, const char *headers, const void *body,
	unsigned long body_size, unsigned long maximum, struct update_response *response, char *error, int error_size)
{
	(void)method, (void)url, (void)headers, (void)body, (void)body_size, (void)maximum, (void)response;
	snprintf(error, (size_t)error_size, "no network in the test");
	return 0;
}

/* ---------- the game's */

static void platform_log(const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	printf("\n");
}

int halo_dlss_gpu(void) { return 1; }
int halo_temporal_dlss_drawn(void) { return 0; }

/* updater.c's zip unpacking */
#define PATH_SEPARATOR "/"
#define MAXIMUM_UPDATE_FILES 64
#include "zip_functions.inc"

#include "under_test.inc"

/* ---------- the cases */

static int exists(const char *folder, const char *name)
{
	char path[2048];
	struct stat status;

	snprintf(path, sizeof(path), "%s/%s", folder, name);
	return stat(path, &status) == 0;
}

/* nothing of an install's work left in the add-ons' folder */
static void check_no_partial(const char *addons)
{
	CHECK(!exists(addons, "dlss.partial"), "dlss.partial was left");
	CHECK(!exists(addons, "dlss.old"), "dlss.old was left");
	CHECK(!exists(addons, "dlss.removed"), "dlss.removed was left");
}

int main(int argc, char **argv)
{
	char const *test = argc > 1 ? argv[1] : "";
	char const *addons = argc > 2 ? argv[2] : ".";
	char const *zip = argc > 3 ? argv[3] : "";
	unsigned long long size = argc > 4 ? strtoull(argv[4], NULL, 10) : 0;
	char const *sha256 = argc > 5 ? argv[5] : "";
	char error[400] = "", version[64] = "", dlss[1200];

	snprintf(dlss, sizeof(dlss), "%s/dlss", addons);
	if (!strcmp(test, "install") || !strcmp(test, "remove") || !strcmp(test, "replace"))
	{
		CHECK(dlss_install_zip(addons, zip, size, sha256, "310.9.16-test", error, sizeof(error)),
			"the add-on was refused: %s", error);
		CHECK(exists(dlss, "libopenh1_dlss.so") && exists(dlss, "libnvidia-ngx-dlss.so.310.9.16") &&
			exists(dlss, "NVIDIA-DLSS-LICENSE.txt"), "the add-on's files are not in place");
		CHECK(dlss_installed_in(dlss, version, sizeof(version)) && !strcmp(version, "310.9.16-test"),
			"installed.txt says %s", version);
		check_no_partial(addons);
		if (!strcmp(test, "replace"))
		{
			/* (an older add-on there: replaced, nothing of it left) */
			char marker[1300];
			FILE *file;

			snprintf(marker, sizeof(marker), "%s/stale.txt", dlss);
			file = fopen(marker, "wb");
			fclose(file);
			snprintf(marker, sizeof(marker), "%s/dlss.partial", addons);
			mkdir(marker, 0755);
			snprintf(marker, sizeof(marker), "%s/dlss.partial/download.zip", addons);
			{
				char source[1300];
				FILE *in, *out = fopen(marker, "wb");
				char buffer[4096];
				size_t count;

				snprintf(source, sizeof(source), "%s/source.zip", addons);
				in = fopen(source, "rb");
				CHECK(in && out, "no copy of the archive");
				while ((count = fread(buffer, 1, sizeof(buffer), in)) > 0)
					fwrite(buffer, 1, count, out);
				fclose(in);
				fclose(out);
			}
			CHECK(dlss_install_zip(addons, marker, size, sha256, "310.9.16-next", error, sizeof(error)),
				"the new add-on was refused: %s", error);
			CHECK(!exists(dlss, "stale.txt"), "the old add-on's file was kept");
			CHECK(dlss_installed_in(dlss, version, sizeof(version)) && !strcmp(version, "310.9.16-next"),
				"installed.txt says %s", version);
			check_no_partial(addons);
		}
		if (!strcmp(test, "remove"))
		{
			CHECK(dlss_remove_in(addons, error, sizeof(error)) == 1, "not removed: %s", error);
			CHECK(!exists(addons, "dlss"), "the add-on's folder is still there");
			CHECK(!dlss_installed_in(dlss, version, sizeof(version)), "it still reads as installed");
			check_no_partial(addons);
		}
		return 0;
	}
	if (!strcmp(test, "refused"))
	{
		/* (a tampered archive, or one that is not the add-on's: nothing installed, nothing left) */
		CHECK(!dlss_install_zip(addons, zip, size, sha256, "310.9.16-test", error, sizeof(error)),
			"the archive was taken");
		printf("refused: %s\n", error);
		CHECK(!exists(addons, "dlss"), "a dlss folder was made");
		check_no_partial(addons);
		return 0;
	}
	if (!strcmp(test, "clean"))
	{
		/* (what a crash left: a partial install, and a removal marked while its module was loaded) */
		char path[1300];
		FILE *file;

		snprintf(path, sizeof(path), "%s/dlss.partial/files", addons);
		CHECK(dlss_make_folders(path), "could not make %s", path);
		snprintf(path, sizeof(path), "%s/dlss.partial/files/libopenh1_dlss.so", addons);
		file = fopen(path, "wb");
		fclose(file);
		CHECK(dlss_make_folders(dlss), "could not make %s", dlss);
		snprintf(path, sizeof(path), "%s/libopenh1_dlss.so", dlss);
		file = fopen(path, "wb");
		fclose(file);
		snprintf(path, sizeof(path), "%s/remove", dlss);
		file = fopen(path, "wb");
		fclose(file);
		CHECK(!dlss_installed_in(dlss, version, sizeof(version)), "a marked add-on reads as installed");
		dlss_clean_in(addons);
		CHECK(!exists(addons, "dlss"), "the marked add-on was kept");
		check_no_partial(addons);
		return 0;
	}
	fprintf(stderr, "no case %s\n", test);
	return 2;
}
