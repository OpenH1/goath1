/*
GAME_DATA_PLACES_WINDOWS_TEST.C

The GAME DATA page's places on Windows (port/linux/src/game_data_scan.c,
built with HALO_WINDOWS): the user's folder from %USERPROFILE%, its
Downloads and Desktop, the game's folder, each with forward slashes; a path
in the user's folder shown from ~; and the places' text with no drives.
*/

#include "game_data_scan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check_text(const char *what, const char *got, const char *expected)
{
	if (strcmp(got, expected))
	{
		printf("FAIL %s: \"%s\", not \"%s\"\n", what, got, expected);
		failures++;
	}
}

int main(void)
{
	struct game_data_places places;
	char shown[GAME_DATA_PATH_SIZE], text[1024];

	setenv("USERPROFILE", "C:\\Users\\Player", 1);
	setenv("HOME", "/home/elsewhere", 1);
	game_data_default_places(&places, "C:\\Games\\GOAT H1\\data\\");
	check_text("home", places.home, "C:/Users/Player");
	check_text("downloads", places.downloads, "C:/Users/Player/Downloads");
	check_text("desktop", places.desktop, "C:/Users/Player/Desktop");
	check_text("destination", places.destination, "C:/Games/GOAT H1/data");
	game_data_short_path(&places, "C:\\Users\\Player\\Downloads\\halocesetup_en_1.00.exe", shown, sizeof(shown));
	check_text("short path", shown, "~/Downloads/halocesetup_en_1.00.exe");
	game_data_places_text(&places, text, sizeof(text));
	check_text("places' text", text, "Downloads, the Desktop, your home folder, and C:/Games/GOAT H1/data");
	return failures != 0;
}
