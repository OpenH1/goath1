/*
SUBTITLES.C

The campaign's dialogue as subtitles (subtitles.h; display.subtitles, off by
default, #1217). The Xbox game had none; MCC has them, and the port cannot
ship MCC's text, so they are read from the player's own MCC install
(port/linux/src/mcc_subtitles.c), once, as the first dialogue plays with the
setting on (and again when display.subtitles_language is changed). Without
the install the Settings row is dimmed and says why.

MCC's lines are keyed by the sound's tag path, which every dialogue sound
starts with (sound_manager.c's sound_new_impulse: the scripted lines, the
cutscenes' among them, and the AI's conversations); a line shows for its
sound's length and half a second more, by the game's time (so a pause holds
it), a newer line in its place. MCC names no speaker; the tag's name mostly
does (a10_010_cryotech, x10_04_cortana), and a name it ends in is shown
above the line.

It is drawn once over the whole screen, centred near its bottom: in a
cutscene in the letterbox's lower bar, else above the bottom edge and
narrower, clear of the motion sensor at the HUD's lower left. The words are
the HUD messages' font through the high-res text, white on a dim panel,
the speaker in the menus' blue. A character the font cannot draw is drawn
as its plain letter (an accent's, keyboard_core.c's table), else the line is
English's; it is hidden while a menu (the pause menu) is open.
*/

#include "cseries.h"
#include "cutscene/cinematics.h"
#include "game/game.h"
#include "interface/hud.h"
#include "interface/hud_definitions.h"
#include "interface/ui_widget.h"
#include "rasterizer/rasterizer.h"
#include "render/render.h"
#include "tag_files/tag_files.h"
#include "text/draw_string.h"
#include "text/font_group.h"
#include "keyboard_core.h"
#include "subtitles.h"
#include "../src/mcc_subtitles.h"
#include "../src/port_config.h"

#include <stdio.h>
#include <string.h>

/* ---------- constants */

enum
{
	SUBTITLES_TEXT = MCC_SUBTITLES_MAXIMUM_TEXT,
	SUBTITLES_SPEAKER = 32,
	/* the hold past the sound's end, in ticks */
	SUBTITLES_HOLD = TICKS_PER_SECOND / 2,
	/* draw_string.c's (its enum is its own) */
	_subtitles_text_centred = 2,
};

/* ---------- globals */

void platform_log(const char *format, ...);

static struct
{
	struct mcc_subtitles *lines;
	/* English's, where another language is chosen */
	struct mcc_subtitles *english;
	char language[32];
	boolean tried;
	/* the pack found (the Settings row's): 0 not looked, 1 found, -1 not */
	int found;

	/* the line shown, from and until the game's time */
	boolean showing;
	long from;
	long until;
	wchar_t text[SUBTITLES_TEXT];
	wchar_t speaker[SUBTITLES_SPEAKER];
	/* the text broken into lines no wider than wrapped_width (in the font
	that wrapped it), at its spaces: draw_unicode_string's own wrap breaks
	inside words */
	wchar_t wrapped[SUBTITLES_TEXT];
	short wrapped_width;
	long wrapped_font;
} subtitles;

/* the tags' speakers: the name the tag's name ends in (its digits and a
letter after them dropped: cor04b) */
static struct
{
	char const *ending;
	wchar_t const *speaker;
} const subtitles_speakers[] =
{
	{ "cortana", L"CORTANA" }, { "cor", L"CORTANA" },
	{ "captkeyes", L"CAPTAIN KEYES" }, { "keyes", L"CAPTAIN KEYES" }, { "key", L"CAPTAIN KEYES" },
	{ "sarge", L"SERGEANT JOHNSON" }, { "sgt", L"SERGEANT JOHNSON" },
	{ "chief", L"MASTER CHIEF" }, { "player", L"MASTER CHIEF" }, { "self", L"MASTER CHIEF" },
	{ "monitor", L"343 GUILTY SPARK" },
	{ "pilot", L"PILOT" }, { "captpilot", L"PILOT" }, { "lifeboatpilot", L"PILOT" },
	{ "bisenti", L"BISENTI" }, { "bis", L"BISENTI" },
	{ "fitzgerald", L"FITZGERALD" },
	{ "mendoza", L"MENDOZA" }, { "men", L"MENDOZA" },
	{ "jen", L"JENKINS" },
	{ "cryotech", L"CRYO TECHNICIAN" }, { "tec", L"CRYO TECHNICIAN" },
	{ "cryoassist", L"CRYO ASSISTANT" }, { "ass", L"CRYO ASSISTANT" },
	{ "aussie", L"MARINE" }, { "crazymarine", L"MARINE" }, { "marine", L"MARINE" }, { "marines", L"MARINE" },
	{ "mar", L"MARINE" },
	{ "crewman", L"CREWMAN" }, { "flightofficer", L"FLIGHT OFFICER" },
};

/* ---------- private code */

/* the language chosen: display.subtitles_language's, "game" the game's
(game.language: English, or the Xbox's language it names) */
static void subtitles_language(char *language, unsigned long size)
{
	static struct { char const *game; char const *language; } const games[] =
	{
		{ "ja", "japanese" }, { "de", "german" }, { "fr", "french" }, { "es", "spanish" }, { "it", "italian" },
	};
	char const *chosen = config_string("display.subtitles_language");
	short index;

	if (chosen && chosen[0] && strcmp(chosen, "game"))
	{
		snprintf(language, size, "%s", chosen);
		return;
	}
	chosen = config_string("game.language");
	for (index = 0; index < (short)NUMBEROF(games); index++)
	{
		if (chosen && !strcmp(chosen, games[index].game))
		{
			snprintf(language, size, "%s", games[index].language);
			return;
		}
	}
	snprintf(language, size, "english");
}

static struct mcc_subtitles *subtitles_load(char const *language)
{
	char path[1024], reason[MCC_SUBTITLES_REASON_SIZE];
	struct mcc_subtitles *lines;

	if (!mcc_subtitles_pak_path(path, sizeof(path)))
	{
		platform_log("subtitles: no MCC install found (Steam), so none are shown");
		return NULL;
	}
	lines = mcc_subtitles_read_file(path, language, reason);
	if (lines)
		platform_log("subtitles: %d %s lines from MCC", mcc_subtitles_count(lines), language);
	else
		platform_log("subtitles: MCC's %s lines not read (%s), so none are shown", language, reason);
	return lines;
}

/* the lines, read for the language chosen; FALSE with the setting off or
none read */
static boolean subtitles_ready(void)
{
	char language[32];

	if (!config_boolean("display.subtitles"))
		return FALSE;
	subtitles_language(language, sizeof(language));
	if (!subtitles.tried || strcmp(language, subtitles.language))
	{
		mcc_subtitles_free(subtitles.lines);
		mcc_subtitles_free(subtitles.english);
		subtitles.lines = subtitles.english = NULL;
		subtitles.tried = TRUE;
		snprintf(subtitles.language, sizeof(subtitles.language), "%s", language);
		subtitles.lines = subtitles_load(language);
		if (subtitles.lines && strcmp(language, "english"))
			subtitles.english = subtitles_load("english");
		subtitles.found = subtitles.lines ? 1 : subtitles.found;
	}
	return subtitles.lines != NULL;
}

static long subtitles_font(void)
{
	return hud_globals ? hud_globals->messaging.single_player_font.index : NONE;
}

/* a line to the words drawn: a line break the text's, a character the font
lacks its plain letter (or the like, for punctuation); FALSE where one has
none the font draws */
static boolean subtitles_words(unsigned short const *line, struct font_header *font, wchar_t *words,
	short count)
{
	short index = 0;

	for (; *line && index < count - 3; line++)
	{
		unsigned short character = *line, plain;

		if (character == 0x0A)
		{
			words[index++] = '\r';
			continue;
		}
		if (character == 0x00A0 || character == '\t')
			character = ' ';
		if (!font || font_get_character_by_ascii_code(font, character))
		{
			words[index++] = (wchar_t)character;
			continue;
		}
		if (character == 0x2018 || character == 0x2019)
			plain = '\'';
		else if (character == 0x201C || character == 0x201D)
			plain = '"';
		else if (character == 0x2013 || character == 0x2014)
			plain = '-';
		else if (character == 0x2026)
		{
			words[index++] = '.';
			words[index++] = '.';
			plain = '.';
		}
		else
			plain = keyboard_accent_plain(character);
		if (!plain || !font_get_character_by_ascii_code(font, plain))
			return FALSE;
		words[index++] = (wchar_t)plain;
	}
	words[index] = 0;
	return TRUE;
}

/* the speaker the tag's name ends in, if it names one */
static wchar_t const *subtitles_speaker(char const *tag_path)
{
	char const *name = strrchr(tag_path, '\\');
	char const *ending;
	char word[24];
	short length, index;

	name = name ? name + 1 : tag_path;
	ending = strrchr(name, '_');
	ending = ending ? ending + 1 : name;
	for (length = 0; ending[length] && length < (short)sizeof(word) - 1; length++)
		word[length] = (char)(ending[length] >= 'A' && ending[length] <= 'Z' ? ending[length] + 32 : ending[length]);
	word[length] = 0;
	/* (cor04b, sgt05: its digits and a letter after them dropped) */
	if (length && word[length - 1] >= 'a' && word[length - 1] <= 'z' && length > 1 &&
		word[length - 2] >= '0' && word[length - 2] <= '9')
		word[--length] = 0;
	while (length && word[length - 1] >= '0' && word[length - 1] <= '9')
		word[--length] = 0;
	for (index = 0; index < (short)NUMBEROF(subtitles_speakers); index++)
	{
		if (!strcmp(word, subtitles_speakers[index].ending))
			return subtitles_speakers[index].speaker;
	}
	return NULL;
}

/* whether the line names its speaker itself ("Cortana: ...") */
static boolean subtitles_named(wchar_t const *text)
{
	short index;

	for (index = 0; text[index] && index < SUBTITLES_SPEAKER; index++)
	{
		if (text[index] == ':')
			return index > 0;
		if (!((text[index] >= 'A' && text[index] <= 'Z') || (text[index] >= 'a' && text[index] <= 'z') ||
			text[index] == ' ' || text[index] == '.' || (text[index] >= '0' && text[index] <= '9')))
			return FALSE;
	}
	return FALSE;
}

/* how wide some of a text is, in the font (draw mode set) */
static short subtitles_width(wchar_t const *text, short count)
{
	wchar_t part[SUBTITLES_TEXT];
	rectangle2d bounds, text_bounds, cursor;

	if (count <= 0)
		return 0;
	bounds.x0 = bounds.y0 = 0;
	bounds.x1 = bounds.y1 = 0x3FFF;
	memcpy(part, text, sizeof(*part) * count);
	part[count] = 0;
	draw_unicode_string_compute_bounds(&bounds, part, &text_bounds, &cursor);
	return (short)MAX(text_bounds.x1, cursor.x0);
}

/* the text into subtitles.wrapped, a line break in place of the space
before a word that would take its line past width (a word wider than it
on a line of its own) */
static void subtitles_wrap(long font_index, short width)
{
	static real_argb_color const white = { 1.0f, 1.0f, 1.0f, 1.0f };
	wchar_t *text = subtitles.wrapped;
	short start = 0, space = NONE, index;

	memcpy(subtitles.wrapped, subtitles.text, sizeof(subtitles.wrapped));
	subtitles.wrapped_width = width;
	subtitles.wrapped_font = font_index;
	draw_string_set_draw_mode(font_index, NONE, 0, 0, &white);
	for (index = 0; ; index++)
	{
		wchar_t character = text[index];

		if (character == ' ' || character == '\r' || !character)
		{
			if (space != NONE && subtitles_width(text + start, (short)(index - start)) > width)
			{
				text[space] = '\r';
				start = (short)(space + 1);
			}
			space = character == ' ' ? index : NONE;
			if (character == '\r')
				start = (short)(index + 1);
		}
		if (!character)
			break;
	}
}

/* whether a tag is under sound\dialog\ (case-blind) */
static boolean subtitles_dialog(char const *tag_path)
{
	static char const prefix[] = "sound\\dialog\\";
	short index;

	for (index = 0; prefix[index]; index++)
	{
		char c = tag_path[index];

		if ((c >= 'A' && c <= 'Z' ? c + 32 : c == '/' ? '\\' : c) != prefix[index])
			return FALSE;
	}
	return TRUE;
}

/* ---------- public code */

boolean subtitles_available(void)
{
	char path[1024];

	if (!subtitles.found)
		subtitles.found = mcc_subtitles_pak_path(path, sizeof(path)) ? 1 : -1;
	return subtitles.found > 0;
}

void subtitles_map_load(
	void)
{
	subtitles.showing = FALSE;
	subtitles_ready();
}

void subtitles_sound_started(
	long definition_index,
	long milliseconds,
	boolean heard)
{
	long font_index;
	char const *tag_path;
	unsigned short const *line, *english;
	struct font_header *font;
	wchar_t const *speaker;

	if (!heard || definition_index == NONE || !subtitles_ready())
		return;
	tag_path = tag_get_name(definition_index);
	/* (only the dialogue's tags are MCC's keys) */
	if (!tag_path || !subtitles_dialog(tag_path))
		return;
	line = mcc_subtitles_find(subtitles.lines, tag_path);
	english = mcc_subtitles_find(subtitles.english, tag_path);
	if (!line)
		return;
	font_index = subtitles_font();
	font = font_index != NONE ? font_definition_get(font_index) : NULL;
	if (!subtitles_words(line, font, subtitles.text, (short)NUMBEROF(subtitles.text)) &&
		(!english || !subtitles_words(english, font, subtitles.text, (short)NUMBEROF(subtitles.text))))
	{
		return;
	}
	speaker = subtitles_named(subtitles.text) ? NULL : subtitles_speaker(tag_path);
	if (speaker)
	{
		short index;

		for (index = 0; speaker[index] && index < SUBTITLES_SPEAKER - 1; index++)
			subtitles.speaker[index] = speaker[index];
		subtitles.speaker[index] = 0;
	}
	else
		subtitles.speaker[0] = 0;
	/* (wrapped as it is first drawn) */
	subtitles.wrapped_width = 0;
	subtitles.from = game_time_get();
	subtitles.until = subtitles.from + TICKS_PER_SECOND * MAX(milliseconds, 0) / 1000 + SUBTITLES_HOLD;
	subtitles.showing = TRUE;
}

void subtitles_draw(
	void)
{
	static real_argb_color const white = { 1.0f, 0.92f, 0.92f, 0.92f };
	/* (the menus' blue, #2896FF) */
	static real_argb_color const blue = { 1.0f, 0.157f, 0.588f, 1.0f };
	rectangle2d const *screen = &render.camera.viewport_bounds;
	long font_index = subtitles_font();
	long now;
	short width, height, most, bottom, line_height, pad, top;
	rectangle2d bounds, text_bounds, speaker_bounds, cursor, panel;
	struct font_header *font;
	boolean cutscene;

	if (!subtitles.showing)
		return;
	if (!config_boolean("display.subtitles") || !game_time_initialized())
	{
		subtitles.showing = FALSE;
		return;
	}
	now = game_time_get();
	/* (a new map's time starts again) */
	if (now < subtitles.from || now >= subtitles.until)
	{
		subtitles.showing = FALSE;
		return;
	}
	width = (short)(screen->x1 - screen->x0);
	height = (short)(screen->y1 - screen->y0);
	if (ui_widgets_active() || font_index == NONE || width <= 0 || height <= 0)
		return;
	font = font_definition_get(font_index);
	line_height = (short)(font->ascending_height + font->descending_height + font->leading_height);
	pad = (short)MAX(4, height / 120);
	cutscene = cinematic_in_progress();
	/* (in a cutscene the letterbox's bar is across the screen; else the
	middle, clear of the motion sensor, and above the bottom edge's safe
	area) */
	most = cutscene ? (short)MIN(width * 4 / 5, height * 3 / 2) : (short)MIN(width * 3 / 5, height);
	bottom = (short)(screen->y1 - height * 8 / 100);

	if (subtitles.wrapped_width != most || subtitles.wrapped_font != font_index)
		subtitles_wrap(font_index, most);
	bounds.x0 = (short)(screen->x0 + (width - most) / 2);
	bounds.x1 = (short)(bounds.x0 + most);
	bounds.y0 = 0;
	bounds.y1 = 0x3FFF;
	draw_string_set_draw_mode(font_index, NONE, _subtitles_text_centred, 0, &white);
	draw_unicode_string_compute_bounds(&bounds, subtitles.wrapped, &text_bounds, &cursor);
	speaker_bounds = text_bounds;
	speaker_bounds.y1 = speaker_bounds.y0;
	if (subtitles.speaker[0])
	{
		draw_string_set_draw_mode(font_index, NONE, _subtitles_text_centred, 0, &blue);
		draw_unicode_string_compute_bounds(&bounds, subtitles.speaker, &speaker_bounds, &cursor);
	}
	/* (the speaker's line over the words, the panel about both; in a
	cutscene, the middle of the letterbox's bar, an eighth of the screen,
	where they fit) */
	if (cutscene)
	{
		short tall = (short)((text_bounds.y1 - text_bounds.y0) + (speaker_bounds.y1 - speaker_bounds.y0));

		bottom = (short)(screen->y1 - MAX(2 * pad, (height / 8 - tall) / 2));
	}
	top =(short)(bottom - (text_bounds.y1 - text_bounds.y0) - (speaker_bounds.y1 - speaker_bounds.y0));
	top = (short)MAX(top, screen->y0 + height / 2);
	panel.x0 = (short)(MIN(text_bounds.x0, subtitles.speaker[0] ? speaker_bounds.x0 : text_bounds.x0) - 2 * pad);
	panel.x1 = (short)(MAX(text_bounds.x1, subtitles.speaker[0] ? speaker_bounds.x1 : text_bounds.x1) + 2 * pad);
	panel.y0 = (short)(top - pad);
	panel.y1 = (short)(bottom + pad);
	draw_quad(&panel, 0x9C000000);

	rasterizer_text_set_shadow_color(0xFF000000);
	bounds.y0 = top;
	bounds.y1 = (short)(bottom + line_height);
	if (subtitles.speaker[0])
	{
		draw_string_set_draw_mode(font_index, NONE, _subtitles_text_centred, 0, &blue);
		rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, subtitles.speaker);
		bounds.y0 = (short)(bounds.y0 + (speaker_bounds.y1 - speaker_bounds.y0));
	}
	draw_string_set_draw_mode(font_index, NONE, _subtitles_text_centred, 0, &white);
	rasterizer_draw_unicode_string(&bounds, NULL, NULL, 0, subtitles.wrapped);
	rasterizer_text_set_shadow_color(0);
}
