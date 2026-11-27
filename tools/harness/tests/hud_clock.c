/* The HUD's clock (source/interface/hud.c's hud_draw_match_timer,
display.match_timer): the match's in multiplayer, the mission's in the
campaign and co-op (the level's game time), none in a cinematic, with the
setting off or with the scripts' show_hud off. Its time is drawn through a
fake text drawer, which keeps the last string. The player's Hide HUD
(controls.hide_hud's key, display.hide_hud) hides it with the rest of the
HUD, over map loads, which reset the scripts' show_hud and not it. */
#include "harness.h"

#include <wchar.h>

#include "config.inc"

typedef struct { short x0, y0, x1, y1; } rectangle2d;
typedef struct { real alpha, red, green, blue; } real_argb_color;
struct font_header { short ascending_height, descending_height; };
#define NUMBEROF(array) (sizeof(array) / sizeof(array[0]))
/* (the game's wide printf, whose %s is a wide string's, as Windows' is) */
static int usnprintf(wchar_t *string, unsigned long count, wchar_t const *format, ...)
{
	wchar_t wide_format[64];
	unsigned long at = 0;
	va_list arguments;
	int result;

	for (; *format && at + 2 < NUMBEROF(wide_format); format++)
	{
		if (format[0] == L'%' && format[1] == L's')
			wide_format[at++] = L'%', wide_format[at++] = L'l';
		else
			wide_format[at++] = *format;
	}
	wide_format[at] = 0;
	va_start(arguments, format);
	result = vswprintf(string, count, wide_format, arguments);
	va_end(arguments);
	return result;
}

/* the game: multiplayer or the campaign, its time, a cinematic */
static boolean engine_running, in_cinematic;
static long game_time;
static boolean game_engine_running(void) { return engine_running; }
static boolean cinematic_in_progress(void) { return in_cinematic; }
static long game_time_get(void) { return game_time; }
static struct { boolean show_hud; } scripted = { TRUE }, *hud_scripted_globals = &scripted;

/* the settings */
static boolean match_timer, hide_hud;
static unsigned long changes = 1;
static int config_boolean(const char *name)
{
	return !strcmp(name, "display.match_timer") ? match_timer : !strcmp(name, "display.hide_hud") ? hide_hud : 0;
}

/* the keyboard's actions held (halo_keyboard_actions) */
static unsigned long keys_held;
static unsigned long halo_keyboard_actions(short controller_index) { (void)controller_index; return keys_held; }
/* (the menu studio, debug.menu_studio: its F3 its own) */
static boolean studio;
boolean menu_studio_on(void) { return studio; }
static unsigned long config_changes(void) { return changes; }

/* the view and the text */
static struct font_header font = { 12, 3 };
static struct { struct { rectangle2d window_bounds, viewport_bounds; } camera; short local_player_index; } render =
	{ { { 0, 0, 852, 480 }, { 0, 0, 852, 480 } }, 0 };
static long hud_get_font_index(void) { return 0; }
static struct font_header *font_definition_get(long index) { (void)index; return &font; }
static void hud_get_text_color(real_argb_color *color) { memset(color, 0, sizeof(*color)); }
static void draw_string_set_draw_mode(long font_index, long style, short justification, long flags,
	real_argb_color const *color)
{
	(void)font_index; (void)style; (void)justification; (void)flags; (void)color;
}
static wchar_t drawn[32];
static int draws;
static void rasterizer_draw_unicode_string(rectangle2d const *bounds, void *a, void *b, long flags,
	wchar_t const *string)
{
	(void)bounds; (void)a; (void)b; (void)flags;
	wcsncpy(drawn, string, NUMBEROF(drawn) - 1);
	draws++;
}

/* the HUD's state line (hud_messaging.c's, which draws it whether the HUD
is shown or not): the action's response sets it ("Hold BACK for score"),
hud_enable_custom_state_message clears it */
static wchar_t state_line[32];
static void hud_show_action_response(long player_index)
{
	(void)player_index;
	wcscpy(state_line, L"Hold BACK for score");
}
static void hud_enable_custom_state_message(short local_player_index, boolean enabled)
{
	(void)local_player_index;
	(void)enabled;
	state_line[0] = 0;
}

#include "under_test.inc"

/* the clock this frame draws ("" for none) */
static wchar_t const *frame(void)
{
	draws = 0;
	drawn[0] = 0;
	hud_draw_match_timer();
	CHECK(draws <= 1, "the clock drawn twice in a frame");
	return drawn;
}

int main(int argc, char **argv)
{
	const char *case_name = argc > 1 ? argv[1] : "";

	(void)game_engine_running;
	match_timer = TRUE;
	/* (1:05 of game time) */
	game_time = 65 * 30;
	CASE("campaign")
	{
		engine_running = FALSE;
		CHECK(!wcscmp(frame(), L"1:05"), "the campaign's mission timer: \"%ls\"", drawn);
		game_time = 7 * 30 + 29;
		CHECK(!wcscmp(frame(), L"0:07"), "under a minute: \"%ls\"", drawn);
		in_cinematic = TRUE;
		CHECK(!*frame(), "a cinematic's clock");
		in_cinematic = FALSE;
		scripted.show_hud = FALSE;
		CHECK(!*frame(), "the clock with the scripts' HUD hidden");
		return 0;
	}
	CASE("multiplayer")
	{
		engine_running = TRUE;
		CHECK(!wcscmp(frame(), L"1:05"), "the match clock: \"%ls\"", drawn);
		return 0;
	}
	CASE("off")
	{
		match_timer = FALSE;
		engine_running = FALSE;
		CHECK(!*frame(), "the campaign's clock with the setting off");
		engine_running = TRUE;
		CHECK(!*frame(), "the match clock with the setting off");
		/* (Settings turning it on is read at once) */
		match_timer = TRUE;
		changes++;
		CHECK(!wcscmp(frame(), L"1:05"), "the setting turned on not read: \"%ls\"", drawn);
		return 0;
	}
	CASE("hide")
	{
		engine_running = FALSE;
		CHECK(!wcscmp(frame(), L"1:05"), "hidden before the key: \"%ls\"", drawn);
		/* (pressed and held over frames: one toggle) */
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		CHECK(!*frame() && !*frame(), "the key did not hide the clock");
		CHECK(!hud_shown(), "the key did not hide the HUD");
		keys_held = 0;
		CHECK(!*frame(), "let go, shown again");
		/* (a map load: the scripts' show_hud reset to TRUE) */
		scripted.show_hud = TRUE;
		CHECK(!*frame() && !hud_shown(), "a map load showed the HUD");
		/* (another setting changed in Settings: not this one's) */
		changes++;
		CHECK(!hud_shown(), "another setting's change showed the HUD");
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		CHECK(!wcscmp(frame(), L"1:05") && hud_shown(), "pressed again, not shown");
		keys_held = 0;
		/* (with it off, the scripts still hide the HUD) */
		scripted.show_hud = FALSE;
		CHECK(!hud_shown(), "the scripts' show_hud ignored");
		return 0;
	}
	CASE("studio")
	{
		/* (the menu studio on: F3 steps its input, the HUD stays) */
		studio = TRUE;
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		CHECK(hud_shown(), "the studio's F3 hid the HUD");
		keys_held = 0;
		CHECK(hud_shown(), "let go, hidden");
		studio = FALSE;
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		CHECK(!hud_shown(), "the key did not hide the HUD with the studio off");
		return 0;
	}
	CASE("state_line")
	{
		/* (the state line goes with the HUD: the multiplayer start's score
		hint, set while the HUD shows, not left drawn once it is hidden) */
		engine_running = TRUE;
		hud_update_state_line(0);
		CHECK(!wcscmp(state_line, L"Hold BACK for score"), "the HUD shown, no state line: \"%ls\"", state_line);
		scripted.show_hud = FALSE;
		hud_update_state_line(0);
		CHECK(!*state_line, "the scripts' show_hud 0 left the state line: \"%ls\"", state_line);
		scripted.show_hud = TRUE;
		hud_update_state_line(0);
		CHECK(*state_line, "the HUD shown again, no state line");
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		hud_update_state_line(0);
		CHECK(!*state_line, "the player's Hide HUD left the state line: \"%ls\"", state_line);
		return 0;
	}
	CASE("hide_setting")
	{
		/* (display.hide_hud: the state as the game starts, and as Settings sets it) */
		hide_hud = TRUE;
		CHECK(!hud_shown(), "the setting did not hide the HUD");
		keys_held = FLAG(HALO_KEYBOARD_HIDE_HUD);
		CHECK(hud_shown(), "the key did not show it");
		keys_held = 0;
		hide_hud = FALSE;
		changes++;
		CHECK(hud_shown(), "the setting turned off left it hidden");
		hide_hud = TRUE;
		changes++;
		CHECK(!hud_shown(), "the setting turned on not read");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
