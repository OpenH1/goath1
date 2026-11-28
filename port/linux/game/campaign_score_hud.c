/*
CAMPAIGN_SCORE_HUD.C

What campaign scoring shows (campaign_score.c keeps the score; this file
only reads campaign_score.h and draws). Arcade style but quiet:

- The pop-ups. A player's events close together (within
  SCORE_GROUP_TICKS of the last in the game's time, the events' own ticks)
  are one group, its points summed
  and its words the most notable event's ("+150 Triple Kill", not three
  kills and a multi-kill), shown small just right of and below the
  reticle (clear of it and of a weapon's own counter), for
  SCORE_POPUP_MILLISECONDS after it began to show or its last event, then
  faded; the player's own score under it. Each group waits its turn, the
  oldest first: none is lost to a cutscene (none shows while one plays)
  or a level's loading; one with others waiting is shown more briefly.
  Deaths are not shown. display.score_popups (each player profile's own)
  hides them.
- The level's summary (campaign_score_level_summary): once a level ends
  with a score, and no cutscene plays, a panel in each view for
  SCORE_SUMMARY_MILLISECONDS: the team's total (co-op) or the player's,
  NEW PERSONAL BEST when it is one, and the players by score (the first
  SCORE_SUMMARY_ROWS and this view's own) with their kills, headshots,
  deaths and best streak. Once the next level is being played, only the
  total and the badge, small at the right under the shield's meter, for
  SCORE_SUMMARY_COMPACT_MILLISECONDS, so that it covers none of its
  fighting.
- For the scoreboard (game_engine.c) and the Spartan link (spartan_link.c):
  the score shown, a player's entry, a number's thousands, and a player's
  latest groups (the link's feed of a teammate's events).

debug.score_fill draws all of it from made-up numbers (the players in the
game, a group for each every 4 seconds of the game, the summary at its
40th second), for pictures of it before a score is kept.

Only what this machine draws: nothing is sent and the game state is not
touched.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "interface/hud_messaging.h"
#include "bitmaps/bitmaps.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "memory/data.h"
#include "scenario/scenario.h"
#include "scenario/scenario_definitions.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "text/unicode.h"
#include "tag_files/tag_files.h"

#include "campaign_score_hud.h"
#include "player_settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* port_config.c's */
int config_boolean(char const *name);
unsigned long system_milliseconds(void);
/* frames_clock.c */
int frames_clock(unsigned long *milliseconds);

/* the popups' clock: the computer's, or the frames' where their time is
fixed (debug.frame_time), so that two runs draw the same popups at the same
frames (tools/rt2_clips.py) */
static unsigned long score_hud_now(void)
{
	unsigned long milliseconds;

	return frames_clock(&milliseconds) ? milliseconds : system_milliseconds();
}
void platform_log(char const *format, ...);
void halo_screenshot_named(const char *name);

/* ---------- constants */

enum
{
	/* a player's events this close to the group's last, in the game's time
	(ticks: the events' own, not when they reached this machine, so that a
	level's last and the next's first, which come together after the load,
	stay apart) are one group */
	SCORE_GROUP_TICKS = TICKS_PER_SECOND,
	/* a pop-up shown after its group's last event, then its fade, and its
	rise as it comes */
	SCORE_POPUP_MILLISECONDS = 1600,
	/* (a group shown with more waiting behind it: shorter) */
	SCORE_POPUP_QUEUED_MILLISECONDS = 900,
	/* a group waiting longer than this is not shown (a game left paused) */
	SCORE_POPUP_STALE_MILLISECONDS = 60000,
	SCORE_POPUP_FADE_MILLISECONDS = 350,
	SCORE_POPUP_RISE_MILLISECONDS = 140,
	/* the groups kept (the pop-ups' and the Spartan link's) */
	SCORE_GROUPS = 48,
	/* the summary's time, its fades, and its rows (and the view's own) */
	SCORE_SUMMARY_MILLISECONDS = 6000,
	/* the summary small in a corner, once the next level is being played */
	SCORE_SUMMARY_COMPACT_MILLISECONDS = 4000,
	SCORE_SUMMARY_FADE_MILLISECONDS = 500,
	SCORE_SUMMARY_ROWS = 4,
	/* debug.score_fill: a group every so many seconds, the summary's
	seconds */
	SCORE_FILL_PERIOD = 4,
	SCORE_FILL_SUMMARY_FROM = 40,
};

/* the scoreboard's header's size, the summary's times this */
#define SCORE_BOARD_SCALE 1.35f

/* ---------- globals */

static struct
{
	long cursor;
	struct campaign_score_hud_group groups[SCORE_GROUPS];
	/* each group's rank (its words' event's), as a new event weighs in */
	short ranks[SCORE_GROUPS];
	/* and its last event's tick */
	long ticks[SCORE_GROUPS];
	short next;
	/* the summary shown: its copy, its best, since when */
	struct campaign_score_state summary;
	struct campaign_score_state scratch;
	boolean summary_best;
	boolean summary_shown;
	unsigned long summary_at;
	/* a summary waiting for a cutscene to end; shown small in a corner (the
	next level is played); the level it came in, and the level of the last
	frame drawn */
	boolean summary_pending;
	boolean summary_compact;
	char summary_level[64];
	char drawn_level[64];
	/* when each group's pop-up began to show (0: waiting) */
	unsigned long shown_at[SCORE_GROUPS];
	/* debug.score_pictures: the pop-ups' pictures saved, and the
	summaries' */
	short popup_pictures;
	boolean summary_pictured;
	/* since when a cutscene has held the pop-ups (0: none), whose time
	showing is then given back */
	unsigned long held_at;
	/* debug.score_fill's state, and the last period it made groups of */
	struct campaign_score_state fill;
	long fill_period;
	long fill_map_time;
} score_hud = { 0 };

/* ---------- private code */

static boolean score_fill(
	void)
{
	return config_boolean("debug.score_fill") != 0;
}

static boolean score_campaign(
	void)
{
	return game_time_initialized() && game_in_progress() && global_scenario &&
		global_scenario->type == _scenario_type_solo && !game_engine_running();
}

/* a cutscene plays: the game's, or its letterbox showing (a level's
scripted opening with the HUD in part) */
static boolean score_cutscene(
	void)
{
	return cinematic_in_progress() || (cinematic_globals &&
		(cinematic_globals->show_letterbox || cinematic_globals->letterbox_amount > 0.0f));
}

/* the level being played's name ("" for none) */
static char const *score_level(
	void)
{
	return global_scenario_index != NONE ? tag_get_name(global_scenario_index) : "";
}

/* the player datum of an absolute index, else NULL */
static struct player_datum *score_player_at(
	long absolute_index,
	long *player_index)
{
	struct data_iterator iterator;
	struct player_datum *player;

	if (!player_data)
		return NULL;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		if (DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index) == absolute_index)
		{
			if (player_index)
				*player_index = iterator.datum_index;
			return player;
		}
	}
	return NULL;
}

/* an event's words and how notable it is (the group shows its most
notable's); 0 for one not shown (a death) */
static short score_event_words(
	struct campaign_score_event const *event,
	wchar_t *text,
	short size)
{
	long count = event->victim_class;

	text[0] = 0;
	switch (event->kind)
	{
	case _campaign_score_event_kill:
	{
		/* (the core's name of the enemy, "Elite Major") */
		char const *name = campaign_score_class_name(event->victim_class);
		short index;

		for (index = 0; name && name[index] && index < size - 1; index++)
			text[index] = (wchar_t)(unsigned char)name[index];
		text[index] = 0;
		if (!text[0])
			usnprintf(text, size, L"Kill");
		return 1;
	}
	case _campaign_score_event_grenade:
		usnprintf(text, size, L"Grenade Kill");
		return 3;
	case _campaign_score_event_melee:
		usnprintf(text, size, L"Melee Kill");
		return 4;
	case _campaign_score_event_splatter:
		usnprintf(text, size, L"Splatter");
		return 5;
	case _campaign_score_event_assassination:
		usnprintf(text, size, L"Assassination");
		return 6;
	case _campaign_score_event_driver_assist:
		usnprintf(text, size, L"Driver Assist");
		return 3;
	case _campaign_score_event_stick:
		usnprintf(text, size, L"Stuck");
		return 5;
	case _campaign_score_event_headshot:
		usnprintf(text, size, L"Headshot");
		return 5;
	case _campaign_score_event_vehicle:
		usnprintf(text, size, L"Vehicle Destroyed");
		return 6;
	case _campaign_score_event_wave_clear:
		usnprintf(text, size, L"Wave Cleared");
		return 7;
	case _campaign_score_event_encounter_cleared:
		usnprintf(text, size, L"Encounter Cleared");
		return 7;
	case _campaign_score_event_flawless:
		usnprintf(text, size, L"Flawless");
		return 8;
	case _campaign_score_event_streak:
		if (count >= 20)
			usnprintf(text, size, L"Rampage");
		else if (count >= 15)
			usnprintf(text, size, L"Running Riot");
		else if (count >= 10)
			usnprintf(text, size, L"Killing Frenzy");
		else if (count >= 5)
			usnprintf(text, size, L"Killing Spree");
		else
			usnprintf(text, size, L"%ld in a Row", count);
		return 8;
	case _campaign_score_event_multi_kill:
		if (count >= 6)
			usnprintf(text, size, L"Killtrocity");
		else if (count == 5)
			usnprintf(text, size, L"Killtacular");
		else if (count == 4)
			usnprintf(text, size, L"Overkill");
		else if (count == 3)
			usnprintf(text, size, L"Triple Kill");
		else
			usnprintf(text, size, L"Double Kill");
		return 9;
	case _campaign_score_event_level_bonus:
		usnprintf(text, size, L"Level Bonus");
		return 2;
	case _campaign_score_event_betrayal:
		usnprintf(text, size, L"Betrayal");
		return 10;
	default:
		return 0;
	}
}

/* a group made or grown by a player's event */
static void score_group_add(
	short player,
	long points,
	short rank,
	wchar_t const *text,
	long tick,
	unsigned long now)
{
	short index;
	short newest = NONE;

	for (index = 0; index < SCORE_GROUPS; index++)
	{
		struct campaign_score_hud_group *group = &score_hud.groups[index];

		if (group->last_at && group->player == player && tick >= score_hud.ticks[index] &&
			tick - score_hud.ticks[index] <= SCORE_GROUP_TICKS &&
			(newest == NONE || group->last_at > score_hud.groups[newest].last_at))
		{
			newest = index;
		}
	}
	if (newest != NONE)
	{
		struct campaign_score_hud_group *group = &score_hud.groups[newest];

		group->points += points;
		group->last_at = now;
		score_hud.ticks[newest] = tick;
		if (rank >= score_hud.ranks[newest])
		{
			score_hud.ranks[newest] = rank;
			ustrncpy(group->text, text, NUMBEROF(group->text) - 1);
			group->text[NUMBEROF(group->text) - 1] = 0;
		}
		return;
	}
	index = score_hud.next;
	score_hud.next = (short)((score_hud.next + 1) % SCORE_GROUPS);
	csmemset(&score_hud.groups[index], 0, sizeof(score_hud.groups[index]));
	score_hud.shown_at[index] = 0;
	score_hud.groups[index].player = player;
	score_hud.groups[index].points = points;
	score_hud.groups[index].first_at = now ? now : 1;
	score_hud.groups[index].last_at = now ? now : 1;
	ustrncpy(score_hud.groups[index].text, text, NUMBEROF(score_hud.groups[index].text) - 1);
	score_hud.ranks[index] = rank;
	score_hud.ticks[index] = tick;
}

/* debug.score_fill's made-up score: the players in the game, each with
numbers of their own */
static void score_fill_update(
	void)
{
	static long const scores[] = { 4250, 3180, 2640, 1975 };
	static long const kills[] = { 38, 29, 24, 17 };
	static long const headshots[] = { 12, 9, 6, 4 };
	static long const deaths[] = { 2, 3, 5, 4 };
	static long const streaks[] = { 11, 8, 6, 5 };
	static long const current[] = { 4, 2, 0, 1 };
	static struct { long points; short rank; wchar_t const *text; } const groups[] =
	{
		{ 150, 9, L"Triple Kill" }, { 75, 5, L"Headshot" }, { 250, 8, L"Killing Spree" },
		{ 60, 1, L"Elite Major" }, { 120, 5, L"Stuck" }, { 90, 4, L"Melee Kill" },
		{ 113, 6, L"Assassination" }, { 38, 3, L"Driver Assist" },
	};
	struct campaign_score_state *fill = &score_hud.fill;
	struct data_iterator iterator;
	struct player_datum *player;
	long order = 0;
	long period;

	csmemset(fill, 0, sizeof(*fill));
	fill->enabled = TRUE;
	fill->difficulty_multiplier = 1.5f;
	if (!player_data)
		return;
	data_iterator_new(&iterator, player_data);
	while ((player = data_iterator_next(&iterator)) != NULL)
	{
		long absolute = DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index);
		struct campaign_score_player *entry;
		long slot = order % NUMBEROF(scores);

		if (absolute >= CAMPAIGN_SCORE_MAX_PLAYERS)
			continue;
		entry = &fill->players[absolute];
		entry->score = scores[slot];
		entry->kills = kills[slot];
		entry->headshots = headshots[slot];
		entry->deaths = deaths[slot];
		entry->best_streak = streaks[slot];
		entry->current_streak = current[slot];
		entry->best_multi = 3;
		fill->team_score += entry->score;
		fill->player_count = (short)MAX(fill->player_count, absolute + 1);
		order++;
	}
	/* (a group for each player every SCORE_FILL_PERIOD seconds of the game,
	from the 6th) */
	period = game_time_get() / TICKS_PER_SECOND / SCORE_FILL_PERIOD;
	if (game_time_get() < score_hud.fill_map_time)
		score_hud.fill_period = 0;
	score_hud.fill_map_time = game_time_get();
	if (period >= 2 && period != score_hud.fill_period)
	{
		order = 0;
		score_hud.fill_period = period;
		data_iterator_new(&iterator, player_data);
		while ((player = data_iterator_next(&iterator)) != NULL)
		{
			long pick = (period + order++) % NUMBEROF(groups);

			score_group_add((short)DATUM_INDEX_TO_ABSOLUTE_INDEX(iterator.datum_index), groups[pick].points,
				groups[pick].rank, groups[pick].text, game_time_get(), score_hud_now());
		}
	}
}

/* the events since the last poll made groups (or debug.score_fill's) */
static void score_poll(
	void)
{
	struct campaign_score_event events[32];
	long count;
	long index;
	long rounds;

	if (score_fill())
	{
		if (score_campaign())
			score_fill_update();
		return;
	}
	for (rounds = 0; rounds < 8; rounds++)
	{
		count = campaign_score_events_since(&score_hud.cursor, events, NUMBEROF(events));
		for (index = 0; index < count; index++)
		{
			wchar_t text[32];
			short rank = score_event_words(&events[index], text, NUMBEROF(text));

			if (rank > 0)
				score_group_add(events[index].player, events[index].points, rank, text, events[index].tick,
					score_hud_now());
		}
		if (count < (long)NUMBEROF(events))
			break;
	}
}

/* the HUD's font's line, and its text measured (unscaled) */
static real score_text_width(
	wchar_t const *text)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds, text_bounds, cursor_bounds;

	if (font_index == NONE || !text[0])
		return 0.0f;
	/* (in the HUD's font: what the last text drew in is not this) */
	{
		real_argb_color white = { 1.0f, 1.0f, 1.0f, 1.0f };

		draw_string_set_draw_mode(font_index, NONE, 0, 0, &white);
	}
	bounds.x0 = 0;
	bounds.y0 = 0;
	bounds.x1 = 4000;
	bounds.y1 = 200;
	draw_unicode_string_compute_bounds(&bounds, text, &text_bounds, &cursor_bounds);
	return (real)(text_bounds.x1 - text_bounds.x0);
}

static real score_line_height(
	void)
{
	long font_index = hud_get_font_index();
	struct font_header *font;

	if (font_index == NONE)
		return 0.0f;
	font = font_definition_get(font_index);
	return (real)(font->ascending_height + font->descending_height);
}

/* a line of text in the HUD's font, its top at y in the view's units,
scaled: from x (justification 0), ending at x (1), or centred on x (2) */
static void score_text(
	wchar_t const *text,
	real x,
	real y,
	short justification,
	real text_scale,
	real_argb_color const *color)
{
	long font_index = hud_get_font_index();
	rectangle2d bounds;
	real width;
	real left;

	if (font_index == NONE || !text[0] || color->alpha <= 0.0f)
		return;
	width = score_text_width(text) * text_scale;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
	left = justification == 1 ? x - width : justification == 2 ? x - width * 0.5f : x;
	bounds.x0 = (short)floorf(left);
	bounds.y0 = (short)floorf(y);
	bounds.x1 = (short)(bounds.x0 + 4000);
	bounds.y1 = (short)ceilf(y + score_line_height());
	rasterizer_text_set_scale(text_scale, (real)bounds.x0, (real)bounds.y0);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, text);
	rasterizer_text_set_scale(1.0f, 0.0f, 0.0f);
}

static void score_panel(
	real x0,
	real y0,
	real x1,
	real y1,
	pixel32 color,
	real alpha)
{
	rectangle2d rectangle;
	long panel_alpha = (long)((color >> 24) * PIN(alpha, 0.0f, 1.0f) + 0.5f);

	rectangle.x0 = (short)floorf(x0);
	rectangle.y0 = (short)floorf(y0);
	rectangle.x1 = (short)ceilf(x1);
	rectangle.y1 = (short)ceilf(y1);
	draw_quad(&rectangle, ((pixel32)panel_alpha << 24) | (color & 0x00FFFFFFUL));
}

/* the view's size, in the HUD's units (its top left 0, 0) */
static void score_view(
	real *x0,
	real *y0,
	real *width,
	real *height)
{
	/* (the HUD's units are the view's, from its top left: the window's
	frame within it) */
	*x0 = (real)(render.camera.window_bounds.x0 - render.camera.viewport_bounds.x0);
	*y0 = (real)(render.camera.window_bounds.y0 - render.camera.viewport_bounds.y0);
	*width = (real)(render.camera.window_bounds.x1 - render.camera.window_bounds.x0);
	*height = (real)(render.camera.window_bounds.y1 - render.camera.window_bounds.y0);
}

static void score_color(
	real_argb_color *color,
	real paler,
	real alpha)
{
	hud_get_text_color(color);
	color->red += (1.0f - color->red) * paler;
	color->green += (1.0f - color->green) * paler;
	color->blue += (1.0f - color->blue) * paler;
	color->alpha = alpha;
}

/* the window's player's pop-up: the group showing (from when it began to
show, or its last event if later), else the oldest waiting, which begins
to show now. A group waits while a cutscene plays or the game loads, so
that none is lost; it is shown more briefly while others wait behind it */
static void score_draw_popup(
	short local_player_index)
{
	long self = local_player_get_player_index(local_player_index);
	short player = self != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(self) : NONE;
	struct campaign_score_hud_group const *group;
	struct campaign_score_player const *entry;
	unsigned long now = score_hud_now();
	unsigned long show = SCORE_POPUP_MILLISECONDS;
	unsigned long age;
	short current = NONE;
	short waiting = NONE;
	short waiting_count = 0;
	short index;
	real_argb_color color;
	real x0, y0, width, height, unit, alpha, rise, x, y;
	wchar_t text[48];
	wchar_t points[16];

	if (player == NONE || !config_player_boolean(local_player_index, "display.score_popups"))
		return;
	/* (a cutscene holds them: the one showing, its time stopped, goes on
	after it) */
	if (score_cutscene())
	{
		if (!score_hud.held_at)
			score_hud.held_at = now ? now : 1;
		return;
	}
	if (score_hud.held_at)
	{
		unsigned long held = now - score_hud.held_at;

		for (index = 0; index < SCORE_GROUPS; index++)
		{
			/* (only those showing as it began) */
			if (score_hud.shown_at[index] > 1 && score_hud.groups[index].last_at &&
				score_hud.held_at - MAX(score_hud.shown_at[index], score_hud.groups[index].last_at) <
					SCORE_POPUP_MILLISECONDS + SCORE_POPUP_FADE_MILLISECONDS)
			{
				score_hud.shown_at[index] = MAX(score_hud.shown_at[index], score_hud.groups[index].last_at) + held;
			}
		}
		score_hud.held_at = 0;
	}
	for (index = 0; index < SCORE_GROUPS; index++)
	{
		struct campaign_score_hud_group const *candidate = &score_hud.groups[index];

		if (!candidate->last_at || candidate->player != player || !candidate->text[0])
			continue;
		if (!score_hud.shown_at[index])
		{
			if (now - candidate->last_at > SCORE_POPUP_STALE_MILLISECONDS)
			{
				score_hud.shown_at[index] = 1;
				continue;
			}
			waiting_count++;
			if (waiting == NONE || candidate->first_at < score_hud.groups[waiting].first_at)
				waiting = index;
		}
		else if (now - MAX(score_hud.shown_at[index], candidate->last_at) <
			SCORE_POPUP_MILLISECONDS + SCORE_POPUP_FADE_MILLISECONDS &&
			(current == NONE || score_hud.shown_at[index] > score_hud.shown_at[current]))
		{
			current = index;
		}
	}
	if (current == NONE)
	{
		if (waiting == NONE)
			return;
		current = waiting;
		score_hud.shown_at[current] = now ? now : 1;
		waiting_count--;
		/* (debug.score_pictures' log: each pop-up as it shows, and how long
		it waited) */
		if (config_boolean("debug.score_pictures"))
			platform_log("campaign score: pop-up of %ld points for player %d shown after %lu ms, %d waiting",
			score_hud.groups[current].points, (int)player, now - score_hud.groups[current].last_at,
			(int)waiting_count);
	}
	group = &score_hud.groups[current];
	if (waiting_count > 0)
		show = SCORE_POPUP_QUEUED_MILLISECONDS;
	age = now - MAX(score_hud.shown_at[current], group->last_at);
	if (age >= show + SCORE_POPUP_FADE_MILLISECONDS)
	{
		/* (its time cut short by those behind it: the next, next frame) */
		score_hud.shown_at[current] = 1;
		return;
	}
	alpha = age <= show ? 1.0f : 1.0f - (real)(age - show) / SCORE_POPUP_FADE_MILLISECONDS;
	rise = PIN((real)(now - score_hud.shown_at[current]) / SCORE_POPUP_RISE_MILLISECONDS, 0.0f, 1.0f);
	alpha *= rise;
	if (alpha <= 0.0f)
		return;
	/* (debug.score_pictures: the first few pop-ups saved, risen) */
	if (rise >= 1.0f && now - score_hud.shown_at[current] < 400 && score_hud.popup_pictures < 4 &&
		config_boolean("debug.score_pictures"))
	{
		char name[32];

		snprintf(name, sizeof(name), "score_popup_%d", (int)++score_hud.popup_pictures);
		halo_screenshot_named(name);
	}
	score_view(&x0, &y0, &width, &height);
	unit = height / 480.0f;
	/* (just right of and below the reticle, small: clear of the reticle,
	and short of a weapon's own counter further right) */
	x = x0 + width * 0.5f + height * 0.075f;
	y = y0 + height * 0.5f + height * 0.07f + (1.0f - rise) * 4.0f * unit;
	campaign_score_hud_number(group->points >= 0 ? group->points : -group->points, points, NUMBEROF(points));
	usnprintf(text, NUMBEROF(text), L"%s%s %s", group->points >= 0 ? L"+" : L"-", points, group->text);
	score_color(&color, 0.35f, 0.9f * alpha);
	score_text(text, x, y, 0, 0.47f * unit, &color);
	entry = campaign_score_hud_player(self);
	/* (the player's total under it, but not under the last level's) */
	if (entry && score_hud.ticks[current] <= game_time_get())
	{
		campaign_score_hud_number(entry->score, points, NUMBEROF(points));
		score_color(&color, 0.0f, 0.55f * alpha);
		score_text(points, x, y + score_line_height() * 0.5f * unit, 0, 0.36f * unit, &color);
	}
}

/* the slots of a score's players, by score (the highest first) */
static short score_ranked(
	struct campaign_score_state const *state,
	short *slots,
	short maximum)
{
	short count = 0;
	short slot;
	short index;

	for (slot = 0; slot < state->player_count && slot < CAMPAIGN_SCORE_MAX_PLAYERS; slot++)
	{
		struct campaign_score_player const *entry = &state->players[slot];

		if (!score_player_at(slot, NULL) && !entry->score && !entry->kills && !entry->deaths)
			continue;
		if (count >= maximum)
			break;
		for (index = count; index > 0 && state->players[slots[index - 1]].score < entry->score; index--)
			slots[index] = slots[index - 1];
		slots[index] = slot;
		count++;
	}
	return count;
}

/* the header the summary and the scoreboard share, from y between left
and right: the title and the difficulty, the total (the team's in co-op,
else the player's) large, and NEW PERSONAL BEST (a badge: a frame and its
words, not a color alone) or the best to beat ("BEST 4,120", best >= 0);
drawn if draw, and where it ends returned */
static real score_draw_header(
	struct campaign_score_state const *state,
	short self_slot,
	boolean coop,
	wchar_t const *title,
	boolean new_best,
	long best,
	real left,
	real right,
	real y,
	real unit,
	real alpha,
	boolean draw)
{
	real line = score_line_height() * 0.7f * unit;
	real_argb_color color;
	wchar_t text[64];
	wchar_t number[16];

	if (draw)
	{
		score_color(&color, 0.0f, 0.75f * alpha);
		score_text(title, left, y, 0, 0.7f * unit, &color);
		if (state->difficulty_multiplier > 0.0f)
		{
			usnprintf(text, NUMBEROF(text), L"DIFFICULTY x%.1f", state->difficulty_multiplier);
			score_text(text, right, y, 1, 0.6f * unit, &color);
		}
	}
	y += line * 1.1f;
	if (draw)
	{
		campaign_score_hud_number(coop ? state->team_score :
			self_slot != NONE && self_slot < CAMPAIGN_SCORE_MAX_PLAYERS ? state->players[self_slot].score :
				state->team_score, number, NUMBEROF(number));
		score_color(&color, 0.5f, alpha);
		score_text(coop ? L"TEAM SCORE" : L"SCORE", left, y + line * 0.55f, 0, 0.8f * unit, &color);
		score_text(number, right, y, 1, 1.4f * unit, &color);
	}
	y += line * 2.3f;
	if (new_best)
	{
		real best_width = score_text_width(L"NEW PERSONAL BEST") * 0.75f * unit + 16.0f * unit;

		if (draw)
		{
			score_panel(left, y - 2.0f * unit, left + best_width, y + line + 1.0f * unit, 0xFFE8C15AUL, alpha);
			color.red = 0.08f;
			color.green = 0.06f;
			color.blue = 0.02f;
			color.alpha = alpha;
			score_text(L"NEW PERSONAL BEST", left + 8.0f * unit, y, 0, 0.75f * unit, &color);
		}
		y += line * 1.1f + 4.0f * unit;
	}
	else if (best >= 0)
	{
		if (draw)
		{
			campaign_score_hud_number(best, number, NUMBEROF(number));
			usnprintf(text, NUMBEROF(text), L"BEST %s", number);
			score_color(&color, 0.0f, 0.75f * alpha);
			score_text(text, left, y, 0, 0.65f * unit, &color);
		}
		y += line * 1.1f;
	}
	return y;
}

/* the level's summary in the window, as it is shown */
static void score_draw_summary(
	short local_player_index)
{
	struct campaign_score_state const *state = &score_hud.summary;
	unsigned long now = score_hud_now();
	unsigned long age;
	long self = local_player_get_player_index(local_player_index);
	short self_slot = self != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(self) : NONE;
	short slots[SCORE_SUMMARY_ROWS + 1];
	short ranked[CAMPAIGN_SCORE_MAX_PLAYERS];
	short ranked_count;
	short shown = 0;
	short index;
	real_argb_color color;
	real x0, y0, width, height, unit, alpha, line, left, right, top, y, panel_width;
	real columns[5];
	wchar_t number[16];
	boolean coop;

	unsigned long duration = score_hud.summary_compact ? SCORE_SUMMARY_COMPACT_MILLISECONDS :
		SCORE_SUMMARY_MILLISECONDS;

	if (!score_hud.summary_shown || score_cutscene())
		return;
	age = now - score_hud.summary_at;
	if (age >= duration)
	{
		score_hud.summary_shown = FALSE;
		return;
	}
	alpha = MIN((real)age / SCORE_SUMMARY_FADE_MILLISECONDS, (real)(duration - age) / SCORE_SUMMARY_FADE_MILLISECONDS);
	alpha = PIN(alpha, 0.0f, 1.0f);
	/* (debug.score_pictures: each summary saved, faded in) */
	if (age >= 1000 && !score_hud.summary_pictured && config_boolean("debug.score_pictures"))
	{
		score_hud.summary_pictured = TRUE;
		halo_screenshot_named(score_hud.summary_compact ? "score_summary_compact" : "score_summary");
	}
	score_view(&x0, &y0, &width, &height);
	unit = height / 480.0f;
	/* (the next level being played: small, at the right under the
	shield's meter, the total and the badge) */
	if (score_hud.summary_compact)
	{
		real compact_width = 190.0f * unit;
		real best_width = score_text_width(L"NEW PERSONAL BEST") * 0.45f * unit + 10.0f * unit;
		real compact_line = score_line_height() * 0.5f * unit;

		ranked_count = score_ranked(state, ranked, NUMBEROF(ranked));
		coop = ranked_count > 1;
		right = x0 + width - 20.0f * unit;
		left = right - compact_width;
		top = y0 + height * 0.17f;
		score_panel(left, top, right, top + compact_line * (score_hud.summary_best ? 4.4f : 3.2f) + 8.0f * unit,
			0xB00A1A33UL, alpha);
		score_panel(left, top, right, top + 1.5f * unit, 0xFF6F9CD0UL, alpha);
		left += 8.0f * unit;
		right -= 8.0f * unit;
		y = top + 5.0f * unit;
		score_color(&color, 0.0f, 0.75f * alpha);
		score_text(L"LEVEL COMPLETE", left, y, 0, 0.45f * unit, &color);
		y += compact_line * 1.1f;
		campaign_score_hud_number(coop ? state->team_score : self_slot != NONE &&
			self_slot < CAMPAIGN_SCORE_MAX_PLAYERS ? state->players[self_slot].score : state->team_score,
			number, NUMBEROF(number));
		score_color(&color, 0.5f, alpha);
		score_text(coop ? L"TEAM SCORE" : L"SCORE", left, y + compact_line * 0.45f, 0, 0.45f * unit, &color);
		score_text(number, right, y, 1, 0.8f * unit, &color);
		y += compact_line * 2.0f;
		if (score_hud.summary_best)
		{
			score_panel(left, y - 1.0f * unit, left + best_width, y + compact_line + 1.0f * unit, 0xFFE8C15AUL, alpha);
			color.red = 0.08f;
			color.green = 0.06f;
			color.blue = 0.02f;
			color.alpha = alpha;
			score_text(L"NEW PERSONAL BEST", left + 5.0f * unit, y, 0, 0.45f * unit, &color);
		}
		return;
	}
	line = score_line_height() * 0.7f * unit;
	if (line <= 0.0f)
		return;
	ranked_count = score_ranked(state, ranked, NUMBEROF(ranked));
	coop = ranked_count > 1;
	/* (the first rows, and the view's own after them if it is not one) */
	for (index = 0; index < ranked_count && shown < SCORE_SUMMARY_ROWS; index++)
		slots[shown++] = ranked[index];
	for (index = SCORE_SUMMARY_ROWS; index < ranked_count; index++)
	{
		if (ranked[index] == self_slot)
			slots[shown++] = ranked[index];
	}
	panel_width = MIN(width - 32.0f * unit, 540.0f * unit);
	left = x0 + (width - panel_width) * 0.5f;
	right = left + panel_width;
	top = y0 + height * 0.18f;
	{
		real rows = 3.4f + (score_hud.summary_best ? 1.1f : 0.0f) + 1.3f + shown + 1.2f;

		score_panel(left, top, right, top + rows * line + 12.0f * unit, 0xC00A1A33UL, alpha);
		score_panel(left, top, right, top + 2.0f * unit, 0xFF6F9CD0UL, alpha);
	}
	left += 14.0f * unit;
	right -= 14.0f * unit;
	y = score_draw_header(state, self_slot, coop, L"LEVEL COMPLETE", score_hud.summary_best, NONE, left, right,
		top + 8.0f * unit, unit, alpha, TRUE);
	/* (the players' table: name, score, kills, headshots, deaths, best
	streak) */
	columns[4] = right;
	columns[3] = right - 100.0f * unit;
	columns[2] = right - 170.0f * unit;
	columns[1] = right - 260.0f * unit;
	columns[0] = right - 320.0f * unit;
	score_color(&color, 0.0f, 0.55f * alpha);
	score_text(L"PLAYER", left, y, 0, 0.55f * unit, &color);
	score_text(L"SCORE", columns[0], y, 1, 0.55f * unit, &color);
	score_text(L"KILLS", columns[1], y, 1, 0.55f * unit, &color);
	score_text(L"HEADSHOTS", columns[2], y, 1, 0.55f * unit, &color);
	score_text(L"DEATHS", columns[3], y, 1, 0.55f * unit, &color);
	score_text(L"BEST STREAK", columns[4], y, 1, 0.55f * unit, &color);
	y += line * 1.0f;
	score_panel(left, y - 3.0f * unit, right, y - 2.0f * unit, 0xC02F7FFFUL, alpha);
	for (index = 0; index < shown; index++)
	{
		struct campaign_score_player const *entry = &state->players[slots[index]];
		struct player_datum *player = score_player_at(slots[index], NULL);
		wchar_t name[16];

		/* (a player without a name, one with no profile: PLAYER and their
		number) */
		if (player && player->name[0])
		{
			ustrncpy(name, player->name, NUMBEROF(player->name));
			name[NUMBEROF(player->name)] = 0;
		}
		else
		{
			usnprintf(name, NUMBEROF(name), L"Player %d", (int)slots[index] + 1);
		}
		if (slots[index] == self_slot && coop)
			score_panel(left - 4.0f * unit, y - 1.0f * unit, right + 4.0f * unit, y + line, 0xB00E2A63UL, alpha);
		score_color(&color, slots[index] == self_slot ? 0.5f : 0.15f, alpha);
		score_text(name, left, y, 0, 0.65f * unit, &color);
		campaign_score_hud_number(entry->score, number, NUMBEROF(number));
		score_text(number, columns[0], y, 1, 0.65f * unit, &color);
		usnprintf(number, NUMBEROF(number), L"%ld", entry->kills);
		score_text(number, columns[1], y, 1, 0.65f * unit, &color);
		usnprintf(number, NUMBEROF(number), L"%ld", entry->headshots);
		score_text(number, columns[2], y, 1, 0.65f * unit, &color);
		usnprintf(number, NUMBEROF(number), L"%ld", entry->deaths);
		score_text(number, columns[3], y, 1, 0.65f * unit, &color);
		usnprintf(number, NUMBEROF(number), L"%ld", entry->best_streak);
		score_text(number, columns[4], y, 1, 0.65f * unit, &color);
		y += line;
	}
}

/* a new summary (the game's, a level ended; or debug.score_fill's, in its
seconds) begins to show */
static void score_summary_update(
	void)
{
	boolean best = FALSE;
	boolean changed = FALSE;
	char const *level = score_level();

	if (score_fill())
	{
		long second = score_campaign() ? game_time_get() / TICKS_PER_SECOND : 0;

		if (second == SCORE_FILL_SUMMARY_FROM && !score_hud.summary_shown && !score_hud.summary_pending)
		{
			score_hud.summary = score_hud.fill;
			best = TRUE;
			changed = TRUE;
		}
	}
	else if (campaign_score_level_summary(&score_hud.scratch, &best) &&
		memcmp(&score_hud.scratch, &score_hud.summary, sizeof(score_hud.summary)) != 0)
	{
		score_hud.summary = score_hud.scratch;
		changed = TRUE;
	}
	if (changed)
	{
		/* (the level it ended in: the one drawn last, the next being loaded
		since if this frame's is another) */
		score_hud.summary_best = best;
		score_hud.summary_pending = TRUE;
		score_hud.summary_shown = FALSE;
		csstrncpy(score_hud.summary_level, score_hud.drawn_level, sizeof(score_hud.summary_level) - 1);
	}
	/* (shown once no cutscene plays: in full on the level it ended in,
	else small in a corner, the next level being played) */
	if (score_hud.summary_pending && !score_cutscene())
	{
		score_hud.summary_pending = FALSE;
		score_hud.summary_shown = TRUE;
		score_hud.summary_pictured = FALSE;
		score_hud.summary_at = score_hud_now();
		score_hud.summary_compact = strcmp(level, score_hud.summary_level) != 0;
	}
	/* (the next level begun while it shows in full: small, for its own
	time) */
	if (score_hud.summary_shown && !score_hud.summary_compact && strcmp(level, score_hud.summary_level) != 0)
	{
		score_hud.summary_compact = TRUE;
		score_hud.summary_pictured = FALSE;
		score_hud.summary_at = score_hud_now();
	}
	csstrncpy(score_hud.drawn_level, level, sizeof(score_hud.drawn_level) - 1);
}

/* ---------- public code */

boolean campaign_score_hud_active(
	void)
{
	return score_campaign() && (score_fill() || campaign_score_enabled());
}

struct campaign_score_state const *campaign_score_hud_state(
	void)
{
	return score_fill() ? &score_hud.fill : campaign_score_get();
}

struct campaign_score_player const *campaign_score_hud_player(
	long player_index)
{
	struct campaign_score_state const *state = campaign_score_hud_state();
	long slot;

	if (player_index == NONE)
		return NULL;
	slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	return slot < state->player_count && slot < CAMPAIGN_SCORE_MAX_PLAYERS ? &state->players[slot] : NULL;
}

void campaign_score_hud_number(
	long value,
	wchar_t *text,
	short size)
{
	char digits[24];
	short length;
	short index;
	short out = 0;
	boolean negative = value < 0;

	snprintf(digits, sizeof(digits), "%ld", negative ? -value : value);
	length = (short)strlen(digits);
	if (negative && out < size - 1)
		text[out++] = L'-';
	for (index = 0; index < length && out < size - 1; index++)
	{
		if (index > 0 && (length - index) % 3 == 0 && out < size - 1)
			text[out++] = L',';
		if (out < size - 1)
			text[out++] = (wchar_t)digits[index];
	}
	text[out] = 0;
}

short campaign_score_hud_recent(
	long player_index,
	unsigned long milliseconds,
	struct campaign_score_hud_group *groups,
	short maximum)
{
	unsigned long now = score_hud_now();
	short player = player_index != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) : NONE;
	short count = 0;
	short step;

	if (!campaign_score_hud_active() || player == NONE)
		return 0;
	score_poll();
	/* (newest first: back from the last made) */
	for (step = 1; step <= SCORE_GROUPS && count < maximum; step++)
	{
		struct campaign_score_hud_group const *group =
			&score_hud.groups[(score_hud.next - step + 2 * SCORE_GROUPS) % SCORE_GROUPS];

		if (group->last_at && group->player == player && now - group->last_at <= milliseconds)
			groups[count++] = *group;
	}
	return count;
}

real campaign_score_hud_board_header(
	short local_player_index,
	real left,
	real right,
	real top,
	real alpha,
	boolean draw)
{
	struct campaign_score_state const *state = campaign_score_hud_state();
	long self = local_player_get_player_index(local_player_index);
	short self_slot = self != NONE ? (short)DATUM_INDEX_TO_ABSOLUTE_INDEX(self) : NONE;
	short ranked[CAMPAIGN_SCORE_MAX_PLAYERS];
	short count = score_ranked(state, ranked, NUMBEROF(ranked));
	real x0, y0, width, height;
	long best = NONE;

	score_view(&x0, &y0, &width, &height);
	if (score_fill())
		best = 4120;
	else if (!campaign_score_personal_best(&best))
		best = NONE;
	/* (a size up from the summary's: the scoreboard's text is larger) */
	return score_draw_header(state, self_slot, count > 1, count > 1 ? L"CO-OP" : L"CAMPAIGN", FALSE, best, left,
		right, top, SCORE_BOARD_SCALE * height / 480.0f, alpha, draw) - top;
}

void campaign_score_hud_draw(
	short local_player_index)
{
	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || !campaign_score_hud_active())
		return;
	score_poll();
	score_summary_update();
	score_draw_popup(local_player_index);
	score_draw_summary(local_player_index);
}
