/*
MENU_DIALOG_LIVE.C (test)

A pop-up's words and its choices' labels (docs/menu_screens.md, <dialog>)
that have bindings are filled again each frame (menu_screen.c's
dialog_frame), so a countdown's label counts down while the pop-up is up;
those with no bindings are left as they were made, and so is a choice whose
block has no label. The fixture screen is tools/harness/tests/
menu_dialog_live.xml's (not one of the game's): a question whose words and
one choice read {test.left}. test_menu_dialog_live.py takes the game's code.
*/

#include "harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MENU_SCREEN_MAXIMUM_TEXT 256
#define SCREEN_MAXIMUM_ITEMS 4
enum { MENU_SCREEN_LIST, MENU_SCREEN_ITEM, MENU_SCREEN_DIALOG };

struct widget_instance { char text[MENU_SCREEN_MAXIMUM_TEXT]; int sets; short local_player_index; };
struct menu_screen_block { int kind; char const *text, *label; };
struct menu_screen { struct menu_screen_block blocks[8]; long block_count; };
struct screen_place { char const *screen; };
struct screen_open
{
	struct screen_place const *place;
	struct widget_instance *widget, *dialog_text;
	struct menu_screen_block const *item_blocks[SCREEN_MAXIMUM_ITEMS];
	struct widget_instance *items[SCREEN_MAXIMUM_ITEMS];
	long item_count;
};
struct screen_values { short local_player_index; };
typedef int (*menu_screen_value)(void *context, char const *name, char *value, unsigned long size);

static struct menu_screen fixture;
static long left = 20;
static int bound = 1;

static struct menu_screen const *menu_screens_find(char const *name)
{
	return !strcmp(name, "dialog_live") ? &fixture : NULL;
}

static void screen_values_clear(struct screen_values *values, struct screen_open const *open)
{
	memset(values, 0, sizeof(*values));
	values->local_player_index = open->widget ? open->widget->local_player_index : NONE;
}

static int screen_value(void *context, char const *name, char *value, unsigned long size)
{
	(void)context;
	if (strcmp(name, "test.left") || !bound)
		return 0;
	snprintf(value, size, "%ld", left);
	return 1;
}

/* (menu_screens.c's, as it fills a text's {bindings}: 0 where one has no
value) */
static int menu_screen_text(char const *text, menu_screen_value value, void *context, char *out, unsigned long size)
{
	unsigned long used = 0;

	while (*text && used + 1 < size)
	{
		if (*text == '{')
		{
			char name[64], got[64];
			char const *end = strchr(text, '}');
			size_t length = end ? (size_t)(end - text - 1) : 0;

			if (!end || length >= sizeof(name))
				return 0;
			memcpy(name, text + 1, length);
			name[length] = 0;
			if (!value(context, name, got, sizeof(got)))
				return 0;
			used += (unsigned long)snprintf(out + used, size - used, "%s", got);
			text = end + 1;
			continue;
		}
		out[used++] = *text++;
	}
	out[used < size ? used : size - 1] = 0;
	return 1;
}

static void screen_text_set(struct widget_instance *widget, char const *utf8)
{
	snprintf(widget->text, sizeof(widget->text), "%s", utf8);
	widget->sets++;
}

#include "under_test.inc"

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";
	static struct screen_place const place = { "dialog_live" };
	struct widget_instance screen = { "", 0, 0 }, text = { "", 0, 0 }, keep = { "KEEP", 0, 0 },
		revert = { "REVERT (20)", 0, 0 }, bare = { "", 0, 0 };
	struct screen_open open = { &place, &screen, &text, { NULL }, { NULL }, 0 };

	/* (menu_dialog_live.xml) */
	fixture.blocks[0] = (struct menu_screen_block){ MENU_SCREEN_DIALOG, "Reverting in {test.left} seconds", NULL };
	fixture.blocks[1] = (struct menu_screen_block){ MENU_SCREEN_LIST, NULL, NULL };
	fixture.blocks[2] = (struct menu_screen_block){ MENU_SCREEN_ITEM, NULL, "KEEP" };
	fixture.blocks[3] = (struct menu_screen_block){ MENU_SCREEN_ITEM, NULL, "REVERT ({test.left})" };
	fixture.block_count = 4;
	open.item_blocks[0] = &fixture.blocks[2];
	open.items[0] = &keep;
	open.item_blocks[1] = &fixture.blocks[3];
	open.items[1] = &revert;
	/* (a choice made with no block's label: left alone) */
	open.item_blocks[2] = NULL;
	open.items[2] = &bare;
	open.item_count = 3;
	if (!strcmp(name, "counts"))
	{
		left = 19;
		dialog_frame(&open);
		CHECK(!strcmp(text.text, "Reverting in 19 seconds"), "its words filled again: %s", text.text);
		CHECK(!strcmp(revert.text, "REVERT (19)"), "the bound choice's label filled again: %s", revert.text);
		left = 3;
		dialog_frame(&open);
		CHECK(!strcmp(text.text, "Reverting in 3 seconds") && !strcmp(revert.text, "REVERT (3)"), "and again: %s",
			revert.text);
		return 0;
	}
	if (!strcmp(name, "unbound"))
	{
		dialog_frame(&open);
		CHECK(keep.sets == 0 && !strcmp(keep.text, "KEEP"), "a label with no bindings is left as it was made");
		CHECK(bare.sets == 0, "a choice with no block is left alone");
		return 0;
	}
	if (!strcmp(name, "no-value"))
	{
		/* (a binding with no value: the words stay as they were, not emptied) */
		screen_text_set(&text, "Reverting in 20 seconds");
		text.sets = 0;
		bound = 0;
		dialog_frame(&open);
		CHECK(text.sets == 0 && !strcmp(text.text, "Reverting in 20 seconds"), "the words kept: %s", text.text);
		return 0;
	}
	if (!strcmp(name, "plain"))
	{
		/* (Quit's: no bindings anywhere, nothing set) */
		fixture.blocks[0].text = "Are you sure you want to quit?";
		fixture.blocks[3].label = "BACK";
		dialog_frame(&open);
		CHECK(text.sets == 0 && revert.sets == 0 && keep.sets == 0, "a pop-up with no bindings is not touched");
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
