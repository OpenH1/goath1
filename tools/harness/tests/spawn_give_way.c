/*
SPAWN_GIVE_WAY.C (test)

A player put in his slot (source/networking/network_game_manager.c's
network_game_spawn_player, the host's choice of slot standing): a datum
there that quit, or is another machine's or controller's, gives way; so
does one at the same machine and controller whose quit was heard but has
not come yet (a machine that loaded the game in progress is told a player
gone and the one that took his machine index, controller and slot together);
a live one at the same machine and controller never does.
test_spawn_give_way.py takes the code (under_test.inc).
*/

#include "harness.h"

typedef unsigned short word;
#define VALID_INDEX(index, count) ((index) >= 0 && (index) < (count))
#define NETWORK_GAME_PLAYER_SLOTS 16

struct network_player
{
	short name[12];
	short primary_color_index;
	short icon_index;
	signed char machine_index;
	signed char controller_index;
	signed char team_index;
	signed char player_list_index;
};

struct player_datum
{
	short identifier;
	struct
	{
		signed char machine_index;
		signed char controller_index;
	} network_player_data;
	long quit_out_of_game_time;
	boolean quit_out_of_game;
	long unit_index;
};

/* (the harness's data array, which is always valid: test_spawn_give_way.py
reads the code's validity check as true) */
static struct player_datum datums[NETWORK_GAME_PLAYER_SLOTS];
static struct data_array players = { "players", NETWORK_GAME_PLAYER_SLOTS, sizeof(struct player_datum), 0, 0x200,
	(unsigned char *)datums };
static struct data_array *player_data = &players;
static long deleted = NONE;

static boolean network_player_is_valid(struct network_player const *player)
{
	return player->machine_index >= 0;
}
static boolean network_game_player_is_local(struct network_player const *player)
{
	(void)player;
	return FALSE;
}
static void network_game_player_forget(long player_index) { (void)player_index; }
static void player_delete(long player_index)
{
	deleted = player_index;
	datums[player_index & 0xFFFF].identifier = 0;
}
/* (datum_new_at_index's: refused where a datum is) */
static long player_new(long machine_index, long player_index, short local_player_index,
	struct network_player const *network_player)
{
	struct player_datum *player = &datums[player_index & 0xFFFF];

	(void)local_player_index;
	if (player_index == NONE || player->identifier)
		return NONE;
	player->identifier = (short)(player_index >> 16);
	player->network_player_data.machine_index = (signed char)machine_index;
	player->network_player_data.controller_index = network_player->controller_index;
	player->quit_out_of_game_time = NONE;
	player->quit_out_of_game = FALSE;
	player->unit_index = NONE;
	return player_index;
}

#include "under_test.inc"

/* slot 3: a datum of machine 2's controller 0, its unit alive, its quit as
given (NONE: none) */
static void slot_taken(long quit_time)
{
	memset(datums, 0, sizeof(datums));
	datums[3].identifier = 0x105;
	datums[3].network_player_data.machine_index = 2;
	datums[3].network_player_data.controller_index = 0;
	datums[3].quit_out_of_game_time = quit_time;
	datums[3].unit_index = 7;
	deleted = NONE;
}

static boolean spawned(long machine_index)
{
	struct network_player player;

	memset(&player, 0, sizeof(player));
	player.machine_index = (signed char)machine_index;
	player.controller_index = 0;
	player.player_list_index = 3;
	return network_game_spawn_player(&player);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("quit-heard")
	{
		slot_taken(1000);
		CHECK(spawned(2), "a player at the machine, controller and slot of one whose quit was heard was not added");
		CHECK(deleted == (0x105 << 16 | 3), "the one who quit kept his datum");
		return 0;
	}
	CASE("other-machine")
	{
		slot_taken(NONE);
		CHECK(spawned(4) && deleted != NONE, "another machine's player did not take the slot the host chose");
		return 0;
	}
	CASE("live")
	{
		slot_taken(NONE);
		CHECK(!spawned(2) && deleted == NONE, "a live player gave way to one at his own machine and controller");
		return 0;
	}
	fprintf(stderr, "unknown case: %s\n", case_name);
	return 2;
}
