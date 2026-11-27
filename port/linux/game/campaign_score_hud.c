/*
CAMPAIGN_SCORE_HUD.C

What campaign scoring shows (campaign_score.c keeps the score; this file
only reads campaign_score.h and draws). Arcade style but quiet:

- The pop-ups. A player's events close together (within
  SCORE_GROUP_MILLISECONDS of the last) are one group, its points summed
  and its words the most notable event's ("+150 Triple Kill", not three
  kills and a multi-kill), shown small at the right of the player's view,
  below the middle and clear of the reticle, for SCORE_POPUP_MILLISECONDS
  after its last event, then faded; the player's own score under it.
  Deaths are not shown. display.score_popups (each player profile's own)
  hides them. None while a cutscene plays.
- The level's summary (campaign_score_level_summary): once a level ends
  with a score, a panel in each view for SCORE_SUMMARY_MILLISECONDS: the
  team's total (co-op) or the player's, NEW PERSONAL BEST when it is one,
  and the players by score (the first SCORE_SUMMARY_ROWS and this view's
  own) with their kills, headshots, deaths and best streak.
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

#include "campaign_score_hud.h"
#include "player_settings.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* port_config.c's */
int config_boolean(char const *name);
unsigned long system_milliseconds(void);

/* ---------- constants */

enum
{
	/* a player's events this close to the group's last are one group */
	SCORE_GROUP_MILLISECONDS = 1000,
	/* a pop-up shown after its group's last event, then its fade, and its
	rise as it comes */
	SCORE_POPUP_MILLISECONDS = 1600,
	SCORE_POPUP_FADE_MILLISECONDS = 350,
	SCORE_POPUP_RISE_MILLISECONDS = 140,
	/* the groups kept (the pop-ups' and the Spartan link's) */
	SCORE_GROUPS = 48,
	/* the summary's time, its fades, and its rows (and the view's own) */
	SCORE_SUMMARY_MILLISECONDS = 12000,
	SCORE_SUMMARY_FADE_MILLISECONDS = 500,
	SCORE_SUMMARY_ROWS = 4,
	/* debug.score_fill: a group every so many seconds, the summary's
	seconds */
	SCORE_FILL_PERIOD = 4,
	SCORE_FILL_SUMMARY_FROM = 40,
};

/* ---------- globals */

static struct
{
	long cursor;
	struct campaign_score_hud_group groups[SCORE_GROUPS];
	/* each group's rank (its words' event's), as a new event weighs in */
	short ranks[SCORE_GROUPS];
	short next;
	/* the summary shown: its copy, its best, since when */
	struct campaign_score_state summary;
	struct campaign_score_state scratch;
	boolean summary_best;
	boolean summary_shown;
	unsigned long summary_at;
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

/* an enemy's name for a kill's words */
static wchar_t const *score_class_name(
	long victim_class)
{
	switch (victim_class)
	{
	case _campaign_score_class_grunt: return L"Grunt";
	case _campaign_score_class_jackal: return L"Jackal";
	case _campaign_score_class_elite: return L"Elite";
	case _campaign_score_class_elite_major: return L"Elite Major";
	case _campaign_score_class_elite_special: return L"Elite Officer";
	case _campaign_score_class_hunter: return L"Hunter";
	case _campaign_score_class_flood_infection: return L"Infection Form";
	case _campaign_score_class_flood_combat: return L"Flood";
	case _campaign_score_class_flood_carrier: return L"Carrier Form";
	case _campaign_score_class_sentinel: return L"Sentinel";
	case _campaign_score_class_vehicle_light:
	case _campaign_score_class_vehicle_heavy: return L"Vehicle";
	default: return L"Kill";
	}
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
		usnprintf(text, size, L"%s", score_class_name(event->victim_class));
		return 1;
	case _campaign_score_event_grenade:
		usnprintf(text, size, L"Grenade Kill");
		return 3;
	case _campaign_score_event_melee:
		usnprintf(text, size, L"Melee Kill");
		return 4;
	case _campaign_score_event_splatter:
		usnprintf(text, size, L"Splatter");
		return 5;
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
	unsigned long now)
{
	short index;
	short newest = NONE;

	for (index = 0; index < SCORE_GROUPS; index++)
	{
		struct campaign_score_hud_group *group = &score_hud.groups[index];

		if (group->last_at && group->player == player && now - group->last_at <= SCORE_GROUP_MILLISECONDS &&
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
	score_hud.groups[index].player = player;
	score_hud.groups[index].points = points;
	score_hud.groups[index].first_at = now ? now : 1;
	score_hud.groups[index].last_at = now ? now : 1;
	ustrncpy(score_hud.groups[index].text, text, NUMBEROF(score_hud.groups[index].text) - 1);
	score_hud.ranks[index] = rank;
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
				groups[pick].rank, groups[pick].text, system_milliseconds());
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
				score_group_add(events[index].player, events[index].points, rank, text, system_milliseconds());
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
	draw_string_set_draw_mode(font_index, NONE, 0, 0, color);
	width = score_text_width(text) * text_scale;
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

/* the window's player's latest group, as a pop-up */
static void score_draw_popup(
	short local_player_index)
{
	long self = local_player_get_player_index(local_player_index);
	struct campaign_score_hud_group group;
	struct campaign_score_player const *entry;
	unsigned long now = system_milliseconds();
	unsigned long age;
	real_argb_color color;
	real x0, y0, width, height, unit, alpha, rise, x, y;
	wchar_t text[48];
	wchar_t points[16];

	if (self == NONE || !config_player_boolean(local_player_index, "display.score_popups") ||
		cinematic_in_progress() ||
		!campaign_score_hud_recent(self, SCORE_POPUP_MILLISECONDS + SCORE_POPUP_FADE_MILLISECONDS, &group, 1) ||
		!group.text[0])
	{
		return;
	}
	age = now - group.last_at;
	alpha = age <= SCORE_POPUP_MILLISECONDS ? 1.0f :
		1.0f - (real)(age - SCORE_POPUP_MILLISECONDS) / SCORE_POPUP_FADE_MILLISECONDS;
	rise = PIN((real)(now - group.first_at) / SCORE_POPUP_RISE_MILLISECONDS, 0.0f, 1.0f);
	alpha *= rise;
	if (alpha <= 0.0f)
		return;
	score_view(&x0, &y0, &width, &height);
	unit = height / 480.0f;
	/* (right of the reticle, below the middle: off its line, clear of the
	shield at the top right and the timer at the bottom right) */
	x = x0 + MIN(width * 0.5f + height * 0.46f, width - 24.0f * unit);
	y = y0 + height * 0.57f + (1.0f - rise) * 6.0f * unit;
	campaign_score_hud_number(group.points >= 0 ? group.points : -group.points, points, NUMBEROF(points));
	usnprintf(text, NUMBEROF(text), L"%s%s %s", group.points >= 0 ? L"+" : L"-", points, group.text);
	score_color(&color, 0.35f, 0.95f * alpha);
	score_text(text, x, y, 1, 0.78f * unit, &color);
	entry = campaign_score_hud_player(self);
	if (entry)
	{
		campaign_score_hud_number(entry->score, points, NUMBEROF(points));
		score_color(&color, 0.0f, 0.6f * alpha);
		score_text(points, x, y + score_line_height() * 0.8f * unit, 1, 0.6f * unit, &color);
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

/* the level's summary in the window, as it is shown */
static void score_draw_summary(
	short local_player_index)
{
	struct campaign_score_state const *state = &score_hud.summary;
	unsigned long now = system_milliseconds();
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
	wchar_t text[64];
	wchar_t number[16];
	boolean coop;

	if (!score_hud.summary_shown)
		return;
	age = now - score_hud.summary_at;
	if (age >= SCORE_SUMMARY_MILLISECONDS)
	{
		score_hud.summary_shown = FALSE;
		return;
	}
	alpha = MIN((real)age / SCORE_SUMMARY_FADE_MILLISECONDS,
		(real)(SCORE_SUMMARY_MILLISECONDS - age) / SCORE_SUMMARY_FADE_MILLISECONDS);
	alpha = PIN(alpha, 0.0f, 1.0f);
	score_view(&x0, &y0, &width, &height);
	unit = height / 480.0f;
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
	panel_width = MIN(width - 32.0f * unit, 440.0f * unit);
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
	y = top + 8.0f * unit;
	score_color(&color, 0.0f, 0.75f * alpha);
	score_text(L"LEVEL COMPLETE", left, y, 0, 0.7f * unit, &color);
	if (state->difficulty_multiplier > 0.0f)
	{
		usnprintf(text, NUMBEROF(text), L"DIFFICULTY x%.1f", state->difficulty_multiplier);
		score_text(text, right, y, 1, 0.6f * unit, &color);
	}
	y += line * 1.1f;
	/* (the total: the team's in co-op, else the player's) */
	campaign_score_hud_number(coop ? state->team_score :
		self_slot != NONE && self_slot < CAMPAIGN_SCORE_MAX_PLAYERS ? state->players[self_slot].score : state->team_score,
		number, NUMBEROF(number));
	score_color(&color, 0.5f, alpha);
	score_text(coop ? L"TEAM SCORE" : L"SCORE", left, y + line * 0.55f, 0, 0.8f * unit, &color);
	score_text(number, right, y, 1, 1.4f * unit, &color);
	y += line * 2.3f;
	if (score_hud.summary_best)
	{
		/* (a star's words and a frame, not a color alone: the printer's
		and the eye's) */
		real best_width = score_text_width(L"NEW PERSONAL BEST") * 0.75f * unit + 16.0f * unit;

		score_panel(left, y - 2.0f * unit, left + best_width, y + line + 1.0f * unit, 0xFFE8C15AUL, alpha);
		color.red = 0.08f;
		color.green = 0.06f;
		color.blue = 0.02f;
		color.alpha = alpha;
		score_text(L"NEW PERSONAL BEST", left + 8.0f * unit, y, 0, 0.75f * unit, &color);
		y += line * 1.1f + 4.0f * unit;
	}
	/* (the players' table: name, score, kills, headshots, deaths, best
	streak) */
	columns[4] = right;
	columns[3] = right - 70.0f * unit;
	columns[2] = right - 130.0f * unit;
	columns[1] = right - 205.0f * unit;
	columns[0] = right - 255.0f * unit;
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

		if (player)
		{
			ustrncpy(name, player->name, NUMBEROF(player->name));
			name[NUMBEROF(player->name)] = 0;
		}
		else
		{
			usnprintf(name, NUMBEROF(name), L"PLAYER %d", (int)slots[index] + 1);
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

	if (score_fill())
	{
		long second = score_campaign() ? game_time_get() / TICKS_PER_SECOND : 0;

		if (second == SCORE_FILL_SUMMARY_FROM && !score_hud.summary_shown)
		{
			score_hud.summary = score_hud.fill;
			score_hud.summary_best = TRUE;
			score_hud.summary_shown = TRUE;
			score_hud.summary_at = system_milliseconds() - SCORE_SUMMARY_FADE_MILLISECONDS;
		}
		return;
	}
	if (campaign_score_level_summary(&score_hud.scratch, &best) &&
		memcmp(&score_hud.scratch, &score_hud.summary, sizeof(score_hud.summary)) != 0)
	{
		score_hud.summary = score_hud.scratch;
		score_hud.summary_best = best;
		score_hud.summary_shown = TRUE;
		score_hud.summary_at = system_milliseconds();
	}
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
	unsigned long now = system_milliseconds();
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
