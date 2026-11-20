/*
GAME_DATA_PLACES_IOS_TEST.C

The GAME DATA page's places on iOS (port/linux/src/game_data_scan.c, built
with HALO_IOS): the app's Documents (the data root) alone, whatever HOME
says, a path in it shown as the Files app names it, and a scan that finds
the installer put there and nothing outside it.
*/

#include "game_data_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures;

static void check_text(const char *what, const char *got, const char *expected)
{
	if (strcmp(got, expected))
	{
		printf("FAIL %s: \"%s\", not \"%s\"\n", what, got, expected);
		failures++;
	}
}

static int any(const char *path, void *context)
{
	(void)path;
	(void)context;
	return 1;
}

static void make_file(const char *path)
{
	FILE *file = fopen(path, "wb");

	if (!file || fseek(file, 128L * 1024 * 1024, SEEK_SET) || fputc(0, file) == EOF || fclose(file))
	{
		printf("FAIL could not make %s\n", path);
		failures++;
	}
}

int main(int argc, char **argv)
{
	struct game_data_places places;
	struct game_data_candidate candidates[GAME_DATA_CANDIDATES];
	char shown[GAME_DATA_PATH_SIZE], documents[GAME_DATA_PATH_SIZE], path[GAME_DATA_PATH_SIZE + 64];
	int count;

	if (argc < 2)
		return 2;
	/* (the app's container: Documents, and a Downloads beside it, not the
	player's) */
	snprintf(documents, sizeof(documents), "%s/Documents", argv[1]);
	snprintf(path, sizeof(path), "%s/Downloads", argv[1]);
	mkdir(documents, 0755);
	mkdir(path, 0755);
	setenv("HOME", argv[1], 1);
	game_data_default_places(&places, documents);
	check_text("destination", places.destination, documents);
	check_text("home", places.home, "");
	check_text("downloads", places.downloads, "");
	check_text("desktop", places.desktop, "");
	check_text("media", places.media[0], "");
	snprintf(path, sizeof(path), "%s/custom_maps", documents);
	game_data_short_path(&places, path, shown, sizeof(shown));
	check_text("short path", shown, "GOAT H1/custom_maps");
	game_data_short_path(&places, "/private/var/elsewhere", shown, sizeof(shown));
	check_text("a path outside", shown, "/private/var/elsewhere");
	snprintf(path, sizeof(path), "%s/Downloads/halocesetup_en_1.00.exe", argv[1]);
	make_file(path);
	count = game_data_scan(&places, any, NULL, candidates, GAME_DATA_CANDIDATES);
	if (count != 0)
	{
		printf("FAIL found %d outside the app's Documents: %s\n", count, candidates[0].path);
		failures++;
	}
	snprintf(path, sizeof(path), "%s/halocesetup_en_1.00.exe", documents);
	make_file(path);
	count = game_data_scan(&places, any, NULL, candidates, GAME_DATA_CANDIDATES);
	if (count != 1 || strcmp(candidates[0].path, path))
	{
		printf("FAIL the installer in Documents: %d found%s%s\n", count, count ? ", " : "", count ? candidates[0].path : "");
		failures++;
	}
	return failures != 0;
}
