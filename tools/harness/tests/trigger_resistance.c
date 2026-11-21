/*
TRIGGER_RESISTANCE.C (test)

A DualSense's firing trigger resistance (input.trigger_resistance:
port/linux/src/xinput_sdl.c's trigger_port_update, controller_remap_input.c's
controller_remap_fire_trigger): the effect report's bytes for the right
trigger, the left, and both off; the trigger that fires by the player's
layout, only while they play; and what is sent to the controller as the
player goes to a menu, dies, the controller leaves or comes back, and at
exit. test_trigger_resistance.py takes the code.
*/

#include "harness.h"

typedef unsigned long long Uint64;
typedef unsigned int SDL_JoystickID;
enum { SDL_GAMEPAD_TYPE_UNKNOWN, SDL_GAMEPAD_TYPE_XBOXONE = 3, SDL_GAMEPAD_TYPE_PS5 = 7 };
#define PORT_COUNT 4
#define PAD_PLAYERS 4
#define PAD_PLAYER_SLOT(gamepad) ((gamepad) >= 0 && (gamepad) < PAD_PLAYERS ? (gamepad) : PAD_PLAYERS)

#include "constants.inc"
#include "controller_remap.h"

/* the fake world: the controllers connected (id 0 none), the clock, each
player's setting, whether alive, and their remapping's state */
typedef struct
{
	SDL_JoystickID id;
	int type;
	int connected;
	int sends;
	unsigned char report[TRIGGER_REPORT_SIZE];
} SDL_Gamepad;

static SDL_Gamepad world_pads[4];
static Uint64 world_now = 10000;
static unsigned long world_config_changes = 1;
static int world_on[PAD_PLAYERS + 1];
static int world_alive[4];
static int world_connected[4];
static void (*world_exit)(void);

static struct
{
	int context;
	unsigned char layout[CONTROLLER_REMAP_ACTIONS];
} remap_ports[4];

static Uint64 SDL_GetTicks(void) { return world_now; }
static void SDL_Delay(unsigned int milliseconds) { (void)milliseconds; }
static int SDL_GetGamepadType(SDL_Gamepad *gamepad) { return gamepad->type; }
static SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID id)
{
	int index;

	for (index = 0; index < 4; index++)
	{
		if (world_pads[index].connected && world_pads[index].id == id)
			return &world_pads[index];
	}
	return NULL;
}
static int SDL_SendGamepadEffect(SDL_Gamepad *gamepad, const void *data, int size)
{
	CHECK(size == TRIGGER_REPORT_SIZE, "a report of %d bytes", size);
	CHECK(gamepad->connected, "a report sent to a controller gone");
	memcpy(gamepad->report, data, (size_t)size);
	gamepad->sends++;
	return 1;
}
static int fake_atexit(void (*function)(void)) { world_exit = function; return 0; }
#define atexit fake_atexit
static unsigned long config_changes(void) { return world_config_changes; }
static int config_player_boolean(int local_player, const char *name)
{
	CHECK(!strcmp(name, "input.trigger_resistance"), "%s", name);
	return world_on[local_player < 0 ? PAD_PLAYERS : local_player];
}
int controller_remap_connected(short port) { return world_connected[port]; }
int controller_remap_alive(short port) { return world_alive[port]; }

#include "under_test.inc"

/* a pad in a port, its player in play with fire on a trigger */
static SDL_Gamepad *pad(int index, SDL_JoystickID id, int type)
{
	world_pads[index].id = id;
	world_pads[index].type = type;
	world_pads[index].connected = 1;
	return &world_pads[index];
}

static void playing(int port, int fire)
{
	world_on[port] = 1;
	world_alive[port] = 1;
	world_connected[port] = 1;
	remap_ports[port].context = 1;
	remap_ports[port].layout[PAD_FIRE] = (unsigned char)fire;
}

/* the report sets both triggers, the one that fires rigid */
static void check_report(const unsigned char *report, int right, int left)
{
	int index;

	CHECK(report[0] == (TRIGGER_REPORT_RIGHT_BIT | TRIGGER_REPORT_LEFT_BIT), "enable bits %02x", report[0]);
	CHECK(TRIGGER_REPORT_RIGHT == 10 && TRIGGER_REPORT_LEFT == 21 && TRIGGER_REPORT_RIGHT_BIT == 0x04 &&
		TRIGGER_REPORT_LEFT_BIT == 0x08 && TRIGGER_REPORT_SIZE == 47, "not SDL's DS5EffectsState_t layout");
	CHECK(report[10] == (right ? 0x01 : 0x05), "right mode %02x", report[10]);
	CHECK(report[21] == (left ? 0x01 : 0x05), "left mode %02x", report[21]);
	CHECK(report[11] == 0 && report[12] == (right ? 110 : 0), "right %d %d", report[11], report[12]);
	CHECK(report[22] == 0 && report[23] == (left ? 110 : 0), "left %d %d", report[22], report[23]);
	for (index = 1; index < TRIGGER_REPORT_SIZE; index++)
	{
		if (index != 10 && index != 12 && index != 21 && index != 23)
			CHECK(!report[index], "byte %d set: %02x (the rumble's and lights' are SDL's)", index, report[index]);
	}
}

static void case_report_bytes(void)
{
	unsigned char report[TRIGGER_REPORT_SIZE];

	trigger_resistance_report(XINPUT_GAMEPAD_RIGHT_TRIGGER, report);
	check_report(report, 1, 0);
	trigger_resistance_report(XINPUT_GAMEPAD_LEFT_TRIGGER, report);
	check_report(report, 0, 1);
	trigger_resistance_report(TRIGGER_NONE, report);
	check_report(report, 0, 0);
	/* (fire on A: no trigger resists) */
	trigger_resistance_report(0, report);
	check_report(report, 0, 0);
}

static void case_fire_trigger(void)
{
	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	CHECK(controller_remap_fire_trigger(0) == XINPUT_GAMEPAD_RIGHT_TRIGGER, "the default layout's");
	/* (SWAP TRIGGERS, or a custom layout's left trigger: a left-handed player's) */
	playing(1, XINPUT_GAMEPAD_LEFT_TRIGGER);
	CHECK(controller_remap_fire_trigger(1) == XINPUT_GAMEPAD_LEFT_TRIGGER, "the left trigger's fire");
	CHECK(controller_remap_fire_trigger(0) == XINPUT_GAMEPAD_RIGHT_TRIGGER, "each player their own");
	/* (fire on a button: neither trigger) */
	playing(2, 0);
	CHECK(controller_remap_fire_trigger(2) == -1, "fire on A");
	/* (a free camera available: the player still fires) */
	remap_ports[0].context = 2;
	CHECK(controller_remap_fire_trigger(0) == XINPUT_GAMEPAD_RIGHT_TRIGGER, "beside a debug camera");
	/* (the debug camera controlled, a menu or the pause, the controller gone) */
	remap_ports[0].context = 3;
	CHECK(controller_remap_fire_trigger(0) == -1, "controlling the debug camera");
	remap_ports[0].context = 0;
	CHECK(controller_remap_fire_trigger(0) == -1, "in a menu");
	remap_ports[0].context = 1;
	world_connected[0] = 0;
	CHECK(controller_remap_fire_trigger(0) == -1, "disconnected");
	CHECK(controller_remap_fire_trigger(-1) == -1 && controller_remap_fire_trigger(4) == -1, "no port");
}

static void case_off_sends_nothing(void)
{
	SDL_Gamepad *gamepad = pad(0, 11, SDL_GAMEPAD_TYPE_PS5);

	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	world_on[0] = 0;
	trigger_port_update(0, gamepad, 11);
	remap_ports[0].context = 0;
	world_now += 5000;
	trigger_port_update(0, gamepad, 11);
	CHECK(!gamepad->sends, "%d reports with the setting off", gamepad->sends);
	CHECK(!world_exit, "an exit hook with nothing sent");
}

static void case_not_dualsense(void)
{
	SDL_Gamepad *gamepad = pad(0, 12, SDL_GAMEPAD_TYPE_XBOXONE);

	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	trigger_port_update(0, gamepad, 12);
	CHECK(!gamepad->sends, "%d reports to an Xbox pad", gamepad->sends);
}

static void case_menu_clears(void)
{
	SDL_Gamepad *gamepad = pad(0, 13, SDL_GAMEPAD_TYPE_PS5);
	int sends;

	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == 1, "%d", gamepad->sends);
	check_report(gamepad->report, 1, 0);
	world_now += 16;
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == 1, "a report every frame");
	/* (the pause menu: let go at once) */
	world_now += 16;
	remap_ports[0].context = 0;
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == 2, "%d", gamepad->sends);
	check_report(gamepad->report, 0, 0);
	/* (sent again a while, then left) */
	world_now += TRIGGER_RESEND_MS;
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == 3, "the clear not sent again");
	check_report(gamepad->report, 0, 0);
	world_now += TRIGGER_SETTLE_MS;
	sends = gamepad->sends;
	trigger_port_update(0, gamepad, 13);
	world_now += TRIGGER_RESEND_MS;
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == sends, "the clear sent for ever");
	/* (back in play: resisting again, and kept so) */
	remap_ports[0].context = 1;
	trigger_port_update(0, gamepad, 13);
	check_report(gamepad->report, 1, 0);
	sends = gamepad->sends;
	world_now += TRIGGER_SETTLE_MS * 2;
	trigger_port_update(0, gamepad, 13);
	CHECK(gamepad->sends == sends + 1, "the resistance not sent again");
	/* (the setting turned off in play) */
	world_on[0] = 0;
	world_config_changes++;
	trigger_port_update(0, gamepad, 13);
	check_report(gamepad->report, 0, 0);
	/* (and at exit, let go) */
	world_on[0] = 1;
	world_config_changes++;
	trigger_port_update(0, gamepad, 13);
	check_report(gamepad->report, 1, 0);
	CHECK(world_exit != NULL, "no exit hook");
	world_exit();
	check_report(gamepad->report, 0, 0);
}

static void case_death_clears(void)
{
	SDL_Gamepad *gamepad = pad(0, 14, SDL_GAMEPAD_TYPE_PS5);

	playing(0, XINPUT_GAMEPAD_LEFT_TRIGGER);
	trigger_port_update(0, gamepad, 14);
	check_report(gamepad->report, 0, 1);
	world_alive[0] = 0;
	world_now += 16;
	trigger_port_update(0, gamepad, 14);
	check_report(gamepad->report, 0, 0);
}

static void case_split_screen(void)
{
	SDL_Gamepad *first = pad(0, 15, SDL_GAMEPAD_TYPE_PS5), *second = pad(1, 16, SDL_GAMEPAD_TYPE_PS5);

	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	playing(1, XINPUT_GAMEPAD_LEFT_TRIGGER);
	trigger_port_update(0, first, 15);
	trigger_port_update(1, second, 16);
	check_report(first->report, 1, 0);
	check_report(second->report, 0, 1);
	/* (player 2 pauses; player 1 plays on) */
	remap_ports[1].context = 0;
	world_now += 16;
	trigger_port_update(0, first, 15);
	trigger_port_update(1, second, 16);
	check_report(first->report, 1, 0);
	check_report(second->report, 0, 0);
}

static void case_reconnect(void)
{
	SDL_Gamepad *gamepad = pad(0, 17, SDL_GAMEPAD_TYPE_PS5);

	playing(0, XINPUT_GAMEPAD_RIGHT_TRIGGER);
	trigger_port_update(0, gamepad, 17);
	check_report(gamepad->report, 1, 0);
	/* (gone: nothing can be sent; SDL_SendGamepadEffect checks) */
	gamepad->connected = 0;
	world_connected[0] = 0;
	trigger_port_update(0, NULL, 0);
	/* (back, a new id, in the pause menu: let go, whatever it kept) */
	gamepad = pad(0, 18, SDL_GAMEPAD_TYPE_PS5);
	gamepad->sends = 0;
	memset(gamepad->report, 0xee, sizeof(gamepad->report));
	world_connected[0] = 1;
	remap_ports[0].context = 0;
	trigger_port_update(0, gamepad, 18);
	CHECK(gamepad->sends == 1, "%d", gamepad->sends);
	check_report(gamepad->report, 0, 0);
	/* (a controller that moves to another port, still connected: let go) */
	remap_ports[0].context = 1;
	world_now += 16;
	trigger_port_update(0, gamepad, 18);
	check_report(gamepad->report, 1, 0);
	trigger_port_update(0, NULL, 0);
	check_report(gamepad->report, 0, 0);
}

int main(int argc, char **argv)
{
	static const struct { const char *name; void (*run)(void); } cases[] =
	{
		{ "report_bytes", case_report_bytes },
		{ "fire_trigger", case_fire_trigger },
		{ "off_sends_nothing", case_off_sends_nothing },
		{ "not_dualsense", case_not_dualsense },
		{ "menu_clears", case_menu_clears },
		{ "death_clears", case_death_clears },
		{ "split_screen", case_split_screen },
		{ "reconnect", case_reconnect },
	};
	int index;

	for (index = 0; argc > 1 && index < (int)(sizeof(cases) / sizeof(cases[0])); index++)
	{
		if (!strcmp(argv[1], cases[index].name))
		{
			cases[index].run();
			return 0;
		}
	}
	fprintf(stderr, "no case %s\n", argc > 1 ? argv[1] : "");
	return 2;
}
