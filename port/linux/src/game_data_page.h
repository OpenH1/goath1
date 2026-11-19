/*
GAME_DATA_PAGE.H

Settings' GAME DATA page (port/assets/menus/screens/game_data.xml,
game_data_page.c): each source of the game's maps, its state, ADD and OPEN
FOLDER, the copies run in the game with their progress, no restart.
*/

#ifndef GAME_DATA_PAGE_H
#define GAME_DATA_PAGE_H

struct menu_table_source;

/* what a copy added since it was last asked (game_data_page_take_added):
the map lists that must look again */
enum
{
	GAME_DATA_ADDED_CUSTOM_EDITION = 1,
	GAME_DATA_ADDED_MCC = 2,
};

/* the screen's bindings' values ("data.<name>", UTF-8: nonzero if it has
one), its actions ("data add", "data open folder"; row: the table's row it
is for, or -1 for the chosen one: nonzero if done), and its table's rows,
the sources */
int game_data_page_value(const char *name, char *value, unsigned long size);
int game_data_page_action(const char *name, short row);
extern struct menu_table_source const game_data_page_source;

/* (the game's side, each frame the page is drawn) how many Custom Edition
and MCC maps the map lists have (custom_edition_maps.c, mcc_maps.c) */
void game_data_page_map_counts(int custom_edition, int mcc);

/* (the game's side, each frame a screen is drawn) what was added since the
last call (GAME_DATA_ADDED_*), and taken: custom_edition_maps.c and
mcc_maps.c then look again */
unsigned game_data_page_take_added(void);

#endif
