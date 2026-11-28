/* ---------- GOAT Settings (display.goat_menus; screens/goat_settings.xml,
docs/menu_screens.md's "GOAT Settings")

Included by menu_functions.c, after Settings' search (settings_search_menu.inc),
whose helpers it uses. While display.goat_menus is on, the hand-written screen
goat_settings takes the place of Settings' first tab (menu_screen.c's
screen_places): one screen whose tabs gather Settings' own rows. Each tab is a
screen of the model's of kind "goat" (model/goat_settings.xml, written by
tools/port_settings.py's GOAT_TABS): its sections' titles, and its rows each a
page's screen (open) and a row's key there (kind "ref"), or a link of its own
(kind "link": its label and the screen it opens). A ref row is the page's own
row: its label, help, choices or slider, and the conditions that show and
enable it, read from its page's screen as that page reads them
(settings_condition with settings_search.condition_screen). A row its page
hides is not listed; a row its page does not enable is shown dimmed.

Left and right step a choice or a slider, A steps a choice (past its last
value to its first) or opens the row's page (a link's own screen); each
change is kept as the pages keep theirs (settings_pending), the header saying
how many. A page opened from here has the changes kept as it opened, and
coming back puts back those it did not write. Y (APPLY) writes them, as a
page's OK does, the profile edited still; B lets go of them and leaves
Settings, as Settings' Cancel does. An APPLY that changes the display asks
KEEP or REVERT over it (goat_keep_revert.inc). */

/* (menu_tags.c's: a screen opened from a widget, and the history's last
gone back to) */
boolean menu_tags_open(struct widget_instance *widget, struct event_record *event, char const *screen,
	boolean *deleted);
void menu_tags_back(struct widget_instance *widget, struct event_record *event, boolean *deleted);

#define GOAT_TAB_COUNT 6
#define GOAT_MAXIMUM_ROWS 224
#define GOAT_DIMMED_COLOR 0x808080FFUL
/* (a row that opens a page: its value "›") */
#define GOAT_OPENS "\342\200\272"

/* the tabs, by their ids (the tab bar's, screens/goat_settings.xml's): each
the model's screen goat/<id> */
static char const *const goat_tab_ids[GOAT_TAB_COUNT] =
{
	"display", "graphics", "audio", "controls", "gameplay", "system"
};

/* a row of the table: a section's heading (row NULL), or a page's row and
the page's screen it is read from (a link's own, its tab's screen) */
struct goat_row
{
	struct halo_menu_model_screen const *page;
	struct halo_menu_model_row const *row;
	char const *heading;
};

static struct
{
	short tab;
	struct goat_row rows[GOAT_MAXIMUM_ROWS];
	short count, first, chosen;
	/* (the tab opened afresh: the focus on its first row) */
	boolean refocus;
	/* (a page opened from here, and what was kept as it opened: the changes,
	each its setting's own value then, and the profile edited) */
	boolean away;
	struct settings_pending kept[SETTINGS_PENDING_COUNT];
	char kept_stored[SETTINGS_PENDING_COUNT][80];
	short kept_count;
	long profile_index;
} goat;

/* whether the hand-written screen takes Settings' place (menu_screen.c's
screen_places) */
int pc_menu_goat_takes(void)
{
	return config_boolean("display.goat_menus");
}

/* the model's screen of the name, NULL if it has none */
static struct halo_menu_model_screen const *goat_model_screen(char const *name)
{
	struct halo_menu_model const *model = menu_engine_model();
	long index;

	for (index = 0; model && name && index < model->screen_count; index++)
	{
		if (!strcmp(model->screens[index].name, name))
			return &model->screens[index];
	}
	return NULL;
}

/* whether a model's row's condition holds (when=, enabled=), its terms of
another row's value read from its page's screen, as Settings' search reads
them */
static boolean goat_model_condition(struct halo_menu_model_screen const *screen, char const *condition)
{
	boolean holds;

	if (!condition || !*condition)
		return TRUE;
	settings_search.condition_screen = screen;
	holds = settings_condition(NULL, condition);
	settings_search.condition_screen = NULL;
	return holds;
}

/* a page's row of the key that its page shows (the first, a platform's or
an add-on's each a row of its own), NULL if none is (or the page is a
profile's and none is edited) */
static struct halo_menu_model_row const *goat_model_row(struct halo_menu_model_screen const *screen,
	char const *key)
{
	struct halo_menu_model const *model = menu_engine_model();
	long index;

	if (!model || !screen || !key || (screen->profile && !player_ui_get_edit_player_profile()))
		return NULL;
	for (index = 0; index < screen->row_count; index++)
	{
		struct halo_menu_model_row const *row = &model->rows[screen->first_row + index];

		if (row->key && !strcmp(row->key, key) && goat_model_condition(screen, row->when))
			return row;
	}
	return NULL;
}

static boolean goat_row_heading(short index)
{
	return index >= 0 && index < goat.count && !goat.rows[index].row;
}

static struct goat_row const *goat_row_at(short index)
{
	return index >= 0 && index < goat.count && goat.rows[index].row ? &goat.rows[index] : NULL;
}

/* whether a row is dimmed: its page does not enable it */
static boolean goat_row_dimmed(struct goat_row const *listed)
{
	return listed->row->enabled && !goat_model_condition(listed->page, listed->row->enabled);
}

/* a choice row's setting (NULL: none, or not a choice), and its value's
index among the setting's values (NONE: none of them) */
static short goat_choice_index(struct goat_row const *listed, struct pc_menu_setting **setting)
{
	char value[96];
	short index;

	*setting = strcmp(listed->row->kind, "choice") ? NULL : settings_row_setting(listed->row);
	if (!*setting || !settings_value(listed->row->setting, value, sizeof(value)))
		return NONE;
	for (index = 0; index < (*setting)->value_count; index++)
	{
		if (settings_values_same((*setting)->values[index], value))
			return index;
	}
	return NONE;
}

/* the slider of a slider row, NULL for another */
static struct settings_slider const *goat_slider(struct goat_row const *listed)
{
	return strcmp(listed->row->kind, "slider") ? NULL : settings_slider_made(listed->row);
}

/* the screen A opens for a row (UTF-8 name, the menus' files'): a link's
own; a row that is not a choice or a slider, its page, unless the page is a
tab (this screen's place, or a tab of Settings' own); NULL for none */
static char const *goat_row_page(struct goat_row const *listed)
{
	struct pc_menu_setting *setting;

	if (!strcmp(listed->row->kind, "link"))
		return listed->row->open;
	goat_choice_index(listed, &setting);
	if (setting || goat_slider(listed))
		return NULL;
	if (listed->page->layout && !strcmp(listed->page->layout, "tab"))
		return NULL;
	return listed->page->name;
}

/* the tab's rows, read again: its sections' headings (a section none of
whose rows is shown, none) and the rows their pages show */
static void goat_list(void)
{
	struct halo_menu_model const *model = menu_engine_model();
	struct halo_menu_model_screen const *tab;
	char name[32];
	char const *heading = NULL;
	long index;

	goat.count = 0;
	snprintf(name, sizeof(name), "goat/%s", goat_tab_ids[PIN(goat.tab, 0, GOAT_TAB_COUNT - 1)]);
	tab = goat_model_screen(name);
	for (index = 0; model && tab && index < tab->row_count && goat.count < GOAT_MAXIMUM_ROWS - 1; index++)
	{
		struct halo_menu_model_row const *row = &model->rows[tab->first_row + index];
		struct halo_menu_model_screen const *page = tab;
		struct halo_menu_model_row const *shown = row;

		if (!row->kind)
			continue;
		if (!strcmp(row->kind, "section"))
		{
			heading = row->title;
			continue;
		}
		if (!strcmp(row->kind, "ref"))
		{
			page = goat_model_screen(row->open);
			shown = goat_model_row(page, row->key);
		}
		else if (strcmp(row->kind, "link"))
			continue;
		if (!shown || !shown->label)
			continue;
		if (heading)
		{
			goat.rows[goat.count].page = NULL;
			goat.rows[goat.count].row = NULL;
			goat.rows[goat.count++].heading = heading;
			heading = NULL;
		}
		goat.rows[goat.count].page = page;
		goat.rows[goat.count].row = shown;
		goat.rows[goat.count++].heading = NULL;
	}
}

/* the first row of the tab that is not a heading */
static short goat_first_row(void)
{
	return (short)(goat_row_heading(0) && goat.count > 1 ? 1 : 0);
}

/* a row's value as the table shows it (UTF-8): a choice's word (the
model's, else the value's own), a slider's number, "›" for a row that opens
a screen, none for another */
static void goat_value_text(struct goat_row const *listed, char *text, unsigned int size)
{
	struct halo_menu_model const *model = menu_engine_model();
	struct pc_menu_setting *setting;
	struct settings_slider const *slider;
	short index = goat_choice_index(listed, &setting);
	long choice;

	text[0] = 0;
	if (setting)
	{
		if (index == NONE)
		{
			if (!settings_value(listed->row->setting, text, size))
				text[0] = 0;
			return;
		}
		snprintf(text, size, "%s", setting->values[index]);
		for (choice = 0; model && choice < listed->row->choice_count; choice++)
		{
			struct halo_menu_model_choice const *shown = &model->choices[listed->row->first_choice + choice];

			if (shown->value && shown->shown && settings_values_same(shown->value, setting->values[index]))
			{
				snprintf(text, size, "%s", shown->shown);
				return;
			}
		}
		return;
	}
	if ((slider = goat_slider(listed)) != NULL)
	{
		settings_slider_text(slider, settings_slider_value(slider), text, size);
		return;
	}
	if (goat_row_page(listed))
		snprintf(text, size, "%s", GOAT_OPENS);
}

/* a choice row's value a step along (wrap: past the last to the first),
kept as a change unless it is the setting's own; FALSE if it did not move */
static boolean goat_choice_step(struct goat_row const *listed, short step, boolean wrap)
{
	struct pc_menu_setting *setting;
	short index = goat_choice_index(listed, &setting);
	char stored[96];
	short next;
	boolean custom;

	if (!setting || setting->value_count < 1)
		return FALSE;
	next = (short)(index == NONE ? 0 : index + step);
	if (wrap)
		next = (short)((next + setting->value_count) % setting->value_count);
	next = (short)PIN(next, 0, setting->value_count - 1);
	/* (Quality's CUSTOM is not a value of its own, setting_stash's: passed
	over, and at an end not taken) */
	custom = !strcmp(setting->setting, "display.graphics");
	if (custom && !strcmp(setting->values[next], "custom"))
	{
		next = (short)(next + step);
		next = (short)(wrap ? (next + setting->value_count) % setting->value_count :
			PIN(next, 0, setting->value_count - 1));
	}
	if (next == index || (custom && !strcmp(setting->values[next], "custom")))
		return FALSE;
	if (setting_text(setting->setting, stored, sizeof(stored), FALSE) &&
		settings_values_same(stored, setting->values[next]))
	{
		settings_pending_remove(setting->setting);
	}
	else
		settings_pending_set(setting->setting, setting->values[next]);
	return TRUE;
}

/* a slider row's value a step along: to its next stop, else by its step,
stopping at its ends; FALSE if it did not move */
static boolean goat_slider_step(struct goat_row const *listed, short step)
{
	struct settings_slider const *slider = goat_slider(listed);
	double value, next;

	if (!slider)
		return FALSE;
	value = settings_slider_value(slider);
	if (slider->stop_count > 1)
	{
		double place = settings_slider_stop_place(slider, value);
		short at = (short)place;
		short stop = step < 0 ? (short)(place > at ? at : at - 1) : (short)(at + 1);

		next = slider->stops[PIN(stop, 0, slider->stop_count - 1)].value;
	}
	else
		next = PIN(value + step * slider->step, slider->least, slider->most);
	if (next == value)
		return FALSE;
	settings_slider_set(slider, next);
	return TRUE;
}

/* ---- the row source goat.rows */

/* a row that is a heading given the focus: the one past it, the way the
focus was going */
static short goat_off_heading(struct menu_table_view *view, short focused, short shown)
{
	short index = (short)(goat.first + focused);

	if (focused == NONE || view->hover || index >= goat.count || !goat_row_heading(index))
		return focused;
	if (goat.chosen > index && index == 0)
	{
		if (view->titles)
		{
			view->focus_titles = TRUE;
			view->focus = NONE;
			return NONE;
		}
		index = 1;
	}
	else
		index = (short)(goat.chosen > index ? index - 1 : index + 1);
	index = (short)PIN(index, 0, goat.count - 1);
	goat.first = (short)PIN(goat.first, index - shown + 1, index);
	focused = (short)(index - goat.first);
	view->focus = focused;
	view->focus_titles = FALSE;
	return focused;
}

static void goat_update(struct menu_table_view *view)
{
	short shown = view->shown;
	short focused = view->focused;

	goat_list();
	if (goat.refocus && shown > 0 && goat.count > 0)
	{
		goat.refocus = FALSE;
		goat.first = 0;
		goat.chosen = goat_first_row();
		focused = (short)(goat.chosen - goat.first);
		view->focus = focused;
	}
	if (focused != NONE && focused == shown - 1 && goat.first + shown < goat.count && list_scrolls_at_end())
	{
		goat.first++;
		view->focus = --focused;
	}
	else if (focused == 0 && goat.first > 0 && list_scrolls_at_end())
	{
		goat.first--;
		view->focus = ++focused;
	}
	goat.first = (short)PIN(goat.first, 0, MAX(0, goat.count - shown));
	focused = goat_off_heading(view, focused, shown);
	/* (a heading is never chosen: the pointer resting on one leaves the
	row chosen as it was) */
	if (focused != NONE && focused >= 0 && focused < shown && !goat_row_heading((short)(goat.first + focused)))
		goat.chosen = (short)(goat.first + focused);
	goat.chosen = (short)PIN(goat.chosen, 0, MAX(0, goat.count - 1));
	if (goat_row_heading(goat.chosen) && goat.chosen + 1 < goat.count)
		goat.chosen++;
	view->count = goat.count;
	view->first = goat.first;
	view->chosen = goat.count ? goat.chosen : NONE;
}

static int goat_heading(short index, unsigned short *words, short size)
{
	if (!goat_row_heading(index) || !goat.rows[index].heading)
		return FALSE;
	halo_menus_utf16(goat.rows[index].heading, words, size);
	return TRUE;
}

/* a row's cells: its label, its value; both dimmed while its page does not
enable it */
static void goat_cell(short index, char const *column, int focused, struct menu_table_cell *cell)
{
	struct goat_row const *listed = goat_row_at(index);
	char text[96];

	(void)focused;
	cell->frame = NONE;
	if (!listed)
		return;
	if (!strcmp(column, "label"))
		snprintf(text, sizeof(text), "%s", listed->row->label);
	else if (!strcmp(column, "value"))
		goat_value_text(listed, text, sizeof(text));
	else
		return;
	halo_menus_utf16(text, cell->text, MENU_TABLE_TEXT_LENGTH);
	if (goat_row_dimmed(listed))
		cell->color = GOAT_DIMMED_COLOR;
}

/* back from a page a row opened: the profile edited again if the page let
go of it, and each change kept as it opened that the page dropped and did
not write (its setting's own as it was then: a page's OK writes them) put
back, the page's own kept as they are */
static void goat_returned(void)
{
	char stored[96];
	short index;

	if (goat.profile_index != NONE && !player_ui_get_edit_player_profile())
		player_ui_begin_editing_profile(goat.profile_index);
	for (index = 0; index < goat.kept_count; index++)
	{
		struct settings_pending const *entry = &goat.kept[index];

		if (settings_pending_find(entry->name) != NONE)
			continue;
		if (!setting_text(entry->name, stored, sizeof(stored), FALSE) ||
			!settings_values_same(stored, goat.kept_stored[index]))
		{
			continue;
		}
		settings_pending_set(entry->name, entry->value);
	}
	goat.kept_count = 0;
}

/* opened: from Settings, on the first tab's first row; back from a page a
row opened, where it was, with the changes kept */
static void goat_opened(void)
{
	if (goat.away)
	{
		goat.away = FALSE;
		goat_returned();
		return;
	}
	goat.tab = 0;
	goat.refocus = TRUE;
}

struct menu_table_source const pc_menu_goat_source =
{
	goat_update, goat_heading, goat_cell, goat_opened,
};

#include "goat_keep_revert.inc"

/* ---- the goat.* bindings and the goat actions */

/* a binding's value, UTF-8 ("goat.<name>": nonzero if it has one): the
tab's id, the chosen row's label and help (its note after it), whether
changes wait and how many, and the words for a tab with no rows */
int pc_menu_goat_value(char const *name, char *value, unsigned long size)
{
	struct goat_row const *listed = goat_row_at(goat.chosen);

	value[0] = 0;
	if (!strncmp(name, "goat.keep_", 10))
		return goat_keep_value(name, value, size);
	if (!strcmp(name, "goat.tab"))
		snprintf(value, size, "%s", goat_tab_ids[PIN(goat.tab, 0, GOAT_TAB_COUNT - 1)]);
	else if (!strcmp(name, "goat.title"))
		snprintf(value, size, "%s", listed ? listed->row->label : "");
	else if (!strcmp(name, "goat.help"))
	{
		if (listed && listed->row->help)
			snprintf(value, size, "%s%s%s", listed->row->help, listed->row->note ? "\n\n" : "",
				listed->row->note ? listed->row->note : "");
	}
	else if (!strcmp(name, "goat.changed"))
	{
		if (settings_pending.count > 0)
			snprintf(value, size, "1");
	}
	else if (!strcmp(name, "goat.pending"))
	{
		if (settings_pending.count == 1)
			snprintf(value, size, "1 CHANGE");
		else if (settings_pending.count > 1)
			snprintf(value, size, "%d CHANGES", (int)settings_pending.count);
	}
	else if (!strcmp(name, "goat.empty"))
	{
		if (!goat.count)
			snprintf(value, size, "No settings here on this machine.");
	}
	return value[0] != 0;
}

/* an action of GOAT Settings' ("goat <what>"; row: the table's row it is
for, NONE for the chosen one): FALSE if it did nothing */
int pc_menu_goat_action(struct widget_instance *widget, struct event_record *event, char const *name, short row,
	int *deleted)
{
	struct goat_row const *listed;
	boolean widget_deleted = FALSE;
	short index;

	*deleted = FALSE;
	if (!strcmp(name, "goat keep") || !strcmp(name, "goat keep revert"))
		return goat_keep_action(widget, event, name, deleted);
	if (!strncmp(name, "goat tab ", 9))
	{
		for (index = 0; index < GOAT_TAB_COUNT; index++)
		{
			if (!strcmp(name + 9, goat_tab_ids[index]))
			{
				if (goat.tab != index)
				{
					goat.tab = index;
					goat.refocus = TRUE;
				}
				return TRUE;
			}
		}
		return FALSE;
	}
	if (!strcmp(name, "goat cancel"))
	{
		/* (the B posted as KEEP or REVERT ran out, its pop-up closed already) */
		if (goat_keep_stray_back())
			return TRUE;
		settings_pending_clear();
		player_ui_end_editing_profile();
		menu_tags_back(widget, event, &widget_deleted);
		*deleted = widget_deleted;
		return TRUE;
	}
	if (!strcmp(name, "goat apply"))
	{
		if (!settings_pending.count)
			return FALSE;
		/* (a display change: set for this run alone, config.toml's written
		only on KEEP, and the question asked over this screen, KEEP or
		REVERT; the other changes written as ever) */
		if (goat_keep_take())
			goat_keep_hold();
		settings_pending_write(FALSE);
		ui_play_audio_feedback_sound(SOUND_FORWARD);
		if (goat_keep.count)
			goat_keep_open(widget, event, deleted);
		return TRUE;
	}
	listed = goat_row_at(row != NONE ? row : goat.chosen);
	if (!listed)
		return FALSE;
	if (!strcmp(name, "goat left") || !strcmp(name, "goat right"))
	{
		short step = (short)(!strcmp(name, "goat left") ? -1 : 1);

		if (goat_row_dimmed(listed))
			return FALSE;
		if (goat_choice_step(listed, step, FALSE) || goat_slider_step(listed, step))
			ui_play_audio_feedback_sound(SOUND_CURSOR);
		return TRUE;
	}
	if (!strcmp(name, "goat row"))
	{
		char const *page;
		struct pc_menu_setting *setting;

		if (goat_row_dimmed(listed))
		{
			ui_play_audio_feedback_sound(SOUND_ERROR);
			return TRUE;
		}
		goat_choice_index(listed, &setting);
		if (setting)
		{
			if (goat_choice_step(listed, 1, TRUE))
				ui_play_audio_feedback_sound(SOUND_CURSOR);
			return TRUE;
		}
		if (goat_slider(listed))
			return TRUE;
		page = goat_row_page(listed);
		if (!page)
		{
			ui_play_audio_feedback_sound(SOUND_ERROR);
			return TRUE;
		}
		memcpy(goat.kept, settings_pending.entries, sizeof(goat.kept));
		goat.kept_count = settings_pending.count;
		for (index = 0; index < goat.kept_count; index++)
		{
			if (!setting_text(goat.kept[index].name, goat.kept_stored[index], sizeof(goat.kept_stored[index]),
				FALSE))
			{
				goat.kept_stored[index][0] = 0;
			}
		}
		goat.profile_index = player_ui_get_edit_profile_index();
		goat.away = TRUE;
		if (!menu_tags_open(widget, event, page, &widget_deleted))
		{
			goat.away = FALSE;
			platform_log("menus: GOAT settings: there is no screen %s", page);
		}
		*deleted = widget_deleted;
		return TRUE;
	}
	return FALSE;
}
