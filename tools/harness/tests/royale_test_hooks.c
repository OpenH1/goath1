/*
ROYALE_TEST_HOOKS.C (test)

Royale's test settings (port/linux/game/royale.c's royale_test_setting and
royale_debug_seconds, network_test.c's network_test_hooks): a network test's
run takes HALO_ROYALE_* from the environment; a player's game (no
debug.network_test) never does, whatever is set. test_royale_test_hooks.py
takes the code (under_test.inc).
*/

#include "harness.h"

#define MAX(a, b) ((a) > (b) ? (a) : (b))

static char const *network_test_setting = "";

static char const *config_string(char const *name)
{
	return !strcmp(name, "debug.network_test") ? network_test_setting : "";
}

static void platform_log(char const *format, ...)
{
	(void)format;
}

#include "under_test.inc"

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	setenv("HALO_ROYALE_LOBBY_SECONDS", "3", 1);
	setenv("HALO_ROYALE_MAP_CONFIG", "b30-test.toml", 1);
	unsetenv("HALO_ROYALE_RESULTS_SECONDS");
	CASE("test_run")
	{
		network_test_setting = "log";
		CHECK(royale_debug_seconds("HALO_ROYALE_LOBBY_SECONDS", 20) == 3, "a test's lobby seconds not taken");
		CHECK(royale_debug_seconds("HALO_ROYALE_RESULTS_SECONDS", 15) == 15, "an unset setting not the default");
		CHECK(royale_test_setting("HALO_ROYALE_MAP_CONFIG") &&
			!strcmp(royale_test_setting("HALO_ROYALE_MAP_CONFIG"), "b30-test.toml"), "a test's map config not taken");
		return 0;
	}
	CASE("player_game")
	{
		network_test_setting = "";
		CHECK(royale_debug_seconds("HALO_ROYALE_LOBBY_SECONDS", 20) == 20, "a player's game took the lobby seconds");
		CHECK(royale_test_setting("HALO_ROYALE_MAP_CONFIG") == NULL, "a player's game took a map config file");
		return 0;
	}
	fprintf(stderr, "no case %s\n", case_name);
	return 2;
}
