/* ---------- GOAT Settings: KEEP or REVERT after a display change
(screens/goat_keep.xml; included by goat_settings.inc)

APPLY that changes a display setting (goat_keep_settings) writes it as ever,
then asks over Settings: the old values are kept, and REVERT, B or the
countdown's end writes them back; KEEP lets them go. The countdown starts as
the pop-up is first drawn (the new mode on the screen), and A or B in its
first half second does nothing, so a press meant for APPLY is not taken as
an answer. */

/* how long the question waits, and the guard after it is first drawn, in
milliseconds */
#define GOAT_KEEP_MILLISECONDS 20000L
#define GOAT_KEEP_GUARD_MILLISECONDS 500L
#define GOAT_KEEP_SETTINGS 8

/* the settings a change of which asks (those the machine has: setting_text) */
static char const *const goat_keep_settings[] =
{
	"display.mode", "display.monitor", "display.resolution", "display.window_size", "display.refresh_rate",
	"display.orientation", "display.hdr", "display.render_resolution",
};

static struct
{
	/* (asking; its countdown started, when, and B posted as it ran out) */
	boolean up, started, posted;
	long started_at;
	short controller;
	struct settings_pending before[GOAT_KEEP_SETTINGS];
	short count;
} goat_keep;

/* before APPLY's writes: the old values of the display settings it changes,
each once; their count */
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
			!strcmp(now, entry->value))
		{
			continue;
		}
		snprintf(goat_keep.before[goat_keep.count].name, sizeof(goat_keep.before[0].name), "%s", entry->name);
		snprintf(goat_keep.before[goat_keep.count].value, sizeof(goat_keep.before[0].value), "%s", now);
		goat_keep.count++;
	}
	return goat_keep.count;
}

/* after APPLY's writes: the question opened over widget's screen; FALSE if
it could not be (the change kept, as without the question) */
static boolean goat_keep_open(struct widget_instance *widget, struct event_record *event, int *deleted)
{
	goat_keep.up = TRUE;
	goat_keep.started = FALSE;
	goat_keep.posted = FALSE;
	/* (the controller that pressed APPLY: its B the countdown's end) */
	goat_keep.controller = event && event->controller_index >= 0 && event->controller_index < 4 ?
		event->controller_index : controller_of(widget_instance_get_topmost_parent(widget));
	if (!menu_screen_open_named(widget, event, "goat_keep", deleted))
	{
		platform_log("menus: GOAT settings: there is no screen screens/goat_keep");
		goat_keep.up = FALSE;
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

/* (menu_screen.c's screen_places) each frame it is drawn: its countdown
started as it is first seen, and B (its REVERT) posted once as it runs out */
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
	if (!goat_keep_left() && !goat_keep.posted)
	{
		goat_keep.posted = TRUE;
		platform_log("menus: GOAT settings: KEEP or REVERT ran out");
		ui_widget_port_post_button(goat_keep.controller, BUTTON_B);
	}
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

/* "goat keep" and "goat keep revert" (B too), past the guard (or the
countdown's end): the question closed, and REVERT's the old values written
back */
static boolean goat_keep_action(struct widget_instance *widget, struct event_record *event, char const *name,
	int *deleted)
{
	boolean revert = strcmp(name, "goat keep") != 0, widget_deleted = FALSE;
	short index;

	if (goat_keep.up && !goat_keep.posted &&
		(!goat_keep.started || (long)system_milliseconds() - goat_keep.started_at < GOAT_KEEP_GUARD_MILLISECONDS))
	{
		return TRUE;
	}
	menu_tags_back(widget, event, &widget_deleted);
	*deleted = widget_deleted;
	if (!goat_keep.up)
		return TRUE;
	goat_keep.up = FALSE;
	if (!revert)
	{
		platform_log("menus: GOAT settings: %d display changes kept", (int)goat_keep.count);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		return TRUE;
	}
	for (index = 0; index < goat_keep.count; index++)
	{
		if (setting_write(goat_keep.before[index].name, goat_keep.before[index].value))
			platform_log("menus: %s = %s (reverted)", goat_keep.before[index].name, goat_keep.before[index].value);
		else
			platform_log("menus: could not set %s back", goat_keep.before[index].name);
	}
	platform_display_apply();
	platform_log("menus: GOAT settings: %d display changes reverted", (int)goat_keep.count);
	ui_play_audio_feedback_sound(SOUND_BACK);
	return TRUE;
}
