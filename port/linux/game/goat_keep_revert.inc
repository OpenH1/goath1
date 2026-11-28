/* ---------- GOAT Settings: KEEP or REVERT after a display change
(screens/goat_keep.xml; included by goat_settings.inc)

APPLY that changes a display setting (goat_keep_settings) sets it for this
run alone (menu_functions.c's settings_held: the display shows it, config.toml
keeps the old values), writes the other changes as ever, then asks over
Settings: KEEP writes the display's into config.toml; REVERT, B or the
countdown's end sets the old values back, the file never having had the new
ones (nor has it if the game closes first). The countdown starts as the
pop-up is first drawn (the new mode on the screen), and A or B in its first
half second does nothing, so a press meant for APPLY is not taken as an
answer. */

/* how long the question waits, and the guard after it is first drawn, in
milliseconds; and how long after the countdown's end a B on Settings is taken
as the one posted to close the pop-up (goat_keep_stray_back) */
#define GOAT_KEEP_MILLISECONDS 20000L
#define GOAT_KEEP_GUARD_MILLISECONDS 500L
#define GOAT_KEEP_STRAY_MILLISECONDS 250L
#define GOAT_KEEP_SETTINGS 8

/* the settings a change of which asks (those the machine has: setting_text) */
static char const *const goat_keep_settings[] =
{
	"display.mode", "display.resolution", "display.window_size", "display.refresh_rate",
	"display.orientation", "display.hdr", "display.render_resolution",
};

static struct
{
	/* (asking; its countdown started, and when) */
	boolean up, started;
	long started_at;
	/* (the countdown ran out, reverted, and B posted to close the pop-up, when) */
	boolean closing;
	long closing_at;
	short controller;
	/* (APPLY's display changes, at their new values) */
	struct settings_pending changes[GOAT_KEEP_SETTINGS];
	short count;
} goat_keep;

/* before APPLY's writes: the display settings it changes, each once; their
count */
static short goat_keep_take(void)
{
	short index, setting;

	goat_keep.count = 0;
	for (index = 0; index < settings_pending.count && goat_keep.count < GOAT_KEEP_SETTINGS; index++)
	{
		struct settings_pending const *entry = &settings_pending.entries[index];
		char now[80];

		for (setting = 0; setting < (short)NUMBEROF(goat_keep_settings); setting++)
		{
			if (!strcmp(entry->name, goat_keep_settings[setting]))
				break;
		}
		if (setting == (short)NUMBEROF(goat_keep_settings) || !setting_text(entry->name, now, sizeof(now), FALSE) ||
			settings_values_same(now, entry->value))
		{
			continue;
		}
		goat_keep.changes[goat_keep.count] = *entry;
		goat_keep.count++;
	}
	return goat_keep.count;
}

/* before APPLY's other writes: the display changes set for this run alone
(held), out of those settings_pending_write writes */
static void goat_keep_hold(void)
{
	short index;

	settings_held.count = 0;
	settings_held.holding = TRUE;
	for (index = 0; index < goat_keep.count; index++)
	{
		struct settings_pending const *change = &goat_keep.changes[index];

		if (setting_write(change->name, change->value))
			platform_log("menus: %s = %s (until KEEP)", change->name, change->value);
		else
			platform_log("menus: could not set %s", change->name);
		settings_pending_remove(change->name);
	}
	settings_held.holding = FALSE;
}

/* after APPLY's writes: the question opened over widget's screen; FALSE if
it could not be (the change kept and saved, as without the question) */
static boolean goat_keep_open(struct widget_instance *widget, struct event_record *event, int *deleted)
{
	goat_keep.up = TRUE;
	goat_keep.started = FALSE;
	goat_keep.closing = FALSE;
	/* (the controller that pressed APPLY: its B closes the pop-up as the
	countdown ends) */
	goat_keep.controller = event && event->controller_index >= 0 && event->controller_index < 4 ?
		event->controller_index : controller_of(widget_instance_get_topmost_parent(widget));
	if (!menu_screen_open_named(widget, event, "goat_keep", deleted))
	{
		platform_log("menus: GOAT settings: there is no screen screens/goat_keep");
		goat_keep.up = FALSE;
		settings_held_keep();
		return FALSE;
	}
	platform_log("menus: GOAT settings: %d display changes: KEEP or REVERT", (int)goat_keep.count);
	return TRUE;
}

/* the milliseconds left, the whole countdown before it is first drawn */
static long goat_keep_left(void)
{
	long left;

	if (!goat_keep.started)
		return GOAT_KEEP_MILLISECONDS;
	left = GOAT_KEEP_MILLISECONDS - ((long)system_milliseconds() - goat_keep.started_at);
	return left > 0 ? left : 0;
}

/* the question answered (once): KEEP's changes written into config.toml, or
REVERT's old values set back and the display applied again */
static void goat_keep_answer(boolean revert)
{
	if (!goat_keep.up)
		return;
	goat_keep.up = FALSE;
	if (!revert)
	{
		settings_held_keep();
		platform_log("menus: GOAT settings: %d display changes kept", (int)goat_keep.count);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return;
	}
	settings_held_revert();
	platform_display_apply();
	platform_log("menus: GOAT settings: %d display changes reverted", (int)goat_keep.count);
	ui_play_audio_feedback_sound(SOUND_BACK);
}

/* (menu_screen.c's screen_places) each frame it is drawn: its countdown
started as it is first seen; as it runs out, the old values set back here,
whatever has the focus, and B posted to close the pop-up (which can't be
closed while it is drawn) */
void pc_menu_goat_keep_frame(struct widget_instance *screen)
{
	(void)screen;
	if (!goat_keep.up)
		return;
	if (!goat_keep.started)
	{
		goat_keep.started = TRUE;
		goat_keep.started_at = (long)system_milliseconds();
	}
	if (!goat_keep_left())
	{
		platform_log("menus: GOAT settings: KEEP or REVERT ran out");
		goat_keep_answer(TRUE);
		goat_keep.closing = TRUE;
		goat_keep.closing_at = (long)system_milliseconds();
		ui_widget_port_post_button(goat_keep.controller, BUTTON_B);
	}
}

/* (Settings' B, "goat cancel") whether it is the B posted as the countdown
ran out, come after a press of the player's closed the pop-up first: taken,
so it does not leave Settings */
static boolean goat_keep_stray_back(void)
{
	boolean stray = goat_keep.closing &&
		(long)system_milliseconds() - goat_keep.closing_at < GOAT_KEEP_STRAY_MILLISECONDS;

	goat_keep.closing = FALSE;
	return stray;
}

/* goat.keep_text and goat.keep_revert: its words and REVERT's seconds */
static boolean goat_keep_value(char const *name, char *value, unsigned long size)
{
	long seconds = (goat_keep_left() + 999L) / 1000L;

	if (!strcmp(name, "goat.keep_text"))
		snprintf(value, size, "The display goes back as it was in %ld second%s unless you keep it.", seconds,
			seconds == 1 ? "" : "s");
	else if (!strcmp(name, "goat.keep_revert"))
		snprintf(value, size, "REVERT (%ld)", seconds);
	else
		return FALSE;
	return TRUE;
}

/* "goat keep" and "goat keep revert" (B too), past the guard: the question
answered and closed; after the countdown's end (answered already), closed */
static boolean goat_keep_action(struct widget_instance *widget, struct event_record *event, char const *name,
	int *deleted)
{
	boolean widget_deleted = FALSE;

	if (goat_keep.up &&
		(!goat_keep.started || (long)system_milliseconds() - goat_keep.started_at < GOAT_KEEP_GUARD_MILLISECONDS))
	{
		return TRUE;
	}
	goat_keep_answer(strcmp(name, "goat keep") != 0);
	menu_tags_back(widget, event, &widget_deleted);
	*deleted = widget_deleted;
	return TRUE;
}
