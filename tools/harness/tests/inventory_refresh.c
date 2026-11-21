/* The host's units' inventories to its clients (distributed_host_send_inventories),
every INVENTORY_INTERVAL_TICKS. Machine 0 is an OpenCE client, machine 1 an
OpenH1 one whose own unit is unit 0; unit 1 is another's, near both. To the
OpenCE machine, and every machine where debug.network_forms_before, as
before: a change of weapons at once, and every unit unchanged every
INVENTORY_REFRESH_TICKS. To the OpenH1 one: a change of weapons at once and
again the next round, its own unit's refresh as before, another's every
INVENTORY_OPENH1_REFRESHES refreshes; a change of weapons lost once heals
the next round, lost twice by the slow refresh. */
#include "harness.h"
#include <math.h>
#include <stddef.h>
typedef unsigned short word;
typedef word message_header;
#define csmemset memset
#define csmemcpy memcpy
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
enum { MAXIMUM_TRACKED_OBJECTS = 8, MAXIMUM_TRACKED_PLAYERS = 4, HALO_PORT_MAXIMUM_NETWORK_MACHINES = 2,
	MAXIMUM_LOCAL_PLAYERS = 1, TICKS_PER_SECOND = 30, DATAGRAM_MAXIMUM_SIZE = 1200, MAXIMUM_WEAPONS_PER_UNIT = 4,
	NUMBER_OF_UNIT_GRENADE_TYPES = 2, _object_dead_bit = 0, _object_mask_unit = 1, UNITS = 2,
	_distributed_message_inventories = 4 };
struct distributed_message_header { word header; byte type; byte count; long game_time; unsigned long checkpoint_epoch; };
#define DATAGRAM_ENTRIES(type) 16
/* the game's units and weapons, as far as an inventory reads them */
struct magazine { short rounds_total; short rounds_loaded; };
struct weapon_datum { struct { struct magazine magazines[2]; real age; } weapon; };
struct unit_datum
{
	struct { unsigned long damage_flags; } object;
	struct { char grenade_counts[NUMBER_OF_UNIT_GRENADE_TYPES]; short current_weapon_index;
		long weapon_object_indices[MAXIMUM_WEAPONS_PER_UNIT]; } unit;
};
struct object_iterator { long index; short next; };
#include "config.inc"

static struct unit_datum units[UNITS];
static struct weapon_datum weapons[8];
static long now_time;
static boolean forms_before;
/* each machine's sends of each unit, and the weapon in hand it last had */
static long sends[HALO_PORT_MAXIMUM_NETWORK_MACHINES][UNITS];
static long had_weapon[HALO_PORT_MAXIMUM_NETWORK_MACHINES][UNITS];
static boolean losing[HALO_PORT_MAXIMUM_NETWORK_MACHINES];

static long game_time_get(void) { return now_time; }
static int config_boolean(char const *name) { (void)name; return forms_before; }
static boolean network_legacy_machine(long machine_index) { return machine_index == 0; }
static void object_iterator_new(struct object_iterator *iterator, long mask, long flags)
{
	(void)mask;
	(void)flags;
	iterator->next = 0;
}
static boolean object_iterator_next(struct object_iterator *iterator)
{
	if (iterator->next >= UNITS)
		return FALSE;
	iterator->index = iterator->next++;
	return TRUE;
}
static struct unit_datum *unit_get(long index) { return &units[index]; }
static struct weapon_datum *weapon_try_and_get(long index) { return index >= 0 && index < 8 ? &weapons[index] : NULL; }
static boolean distributed_object_networked(long index) { (void)index; return TRUE; }
static void object_get_origin(long index, real_point3d *origin) { origin->x = (real)index; origin->y = origin->z = 0.0f; }
static short distributed_host_object_period(short machine_number, real_point3d const *origin)
{
	(void)machine_number;
	(void)origin;
	return 1;
}
static void distributed_send_to_machine(long machine_index, void *message, byte type, short count, word size)
{
	struct distributed_inventory_message const *inventories = (struct distributed_inventory_message const *)message;
	short index;

	(void)size;
	CHECK(type == _distributed_message_inventories, "type %d", type);
	for (index = 0; index < count; index++)
	{
		long unit_index = inventories->inventories[index].unit_index;

		sends[machine_index][unit_index]++;
		if (!losing[machine_index])
			had_weapon[machine_index][unit_index] = inventories->inventories[index].weapon_indices[0];
	}
}
#include "under_test.inc"

static void rounds(long ticks)
{
	long end = now_time + ticks;

	for (; now_time < end; now_time++)
	{
		if (now_time % INVENTORY_INTERVAL_TICKS == 0)
			distributed_host_send_inventories();
	}
}

static void start(void)
{
	short index;

	memset(units, 0, sizeof(units));
	memset(objects_host_inventories, 0, sizeof(objects_host_inventories));
	for (index = 0; index < UNITS; index++)
	{
		short slot;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			units[index].unit.weapon_object_indices[slot] = NONE;
		units[index].unit.weapon_object_indices[0] = index;
		weapons[index].weapon.magazines[0].rounds_loaded = 12;
		objects_host_inventories[index].carried_time = NONE;
		objects_host_inventories[index].ammunition_time = NONE;
	}
	objects_host_viewers.count = 2;
	objects_host_viewers.indices[0] = 0;
	objects_host_viewers.indices[1] = 1;
	objects_host_viewers.machines[0].unit_count = 0;
	objects_host_viewers.machines[1].unit_count = 1;
	objects_host_viewers.machines[1].unit_indices[0] = 0;
	now_time = 3;
	rounds(30 * TICKS_PER_SECOND);
	memset(sends, 0, sizeof(sends));
}

int main(int argc, char **argv)
{
	char const *name = argc > 1 ? argv[1] : "";

	start();
	if (!strcmp(name, "quiet") || !strcmp(name, "before"))
	{
		/* (nothing changes for 20 seconds) */
		forms_before = !strcmp(name, "before");
		rounds(20 * INVENTORY_REFRESH_TICKS);
		CHECK(sends[0][0] == 20 && sends[0][1] == 20, "the OpenCE machine had %ld and %ld", sends[0][0], sends[0][1]);
		CHECK(sends[1][0] == 20, "the OpenH1 machine had its own %ld times", sends[1][0]);
		if (forms_before)
			CHECK(sends[1][1] == 20, "(forms before) the OpenH1 machine had another's %ld times", sends[1][1]);
		else
			CHECK(sends[1][1] == 20 / INVENTORY_OPENH1_REFRESHES, "the OpenH1 machine had another's %ld times",
				sends[1][1]);
		return 0;
	}
	if (!strcmp(name, "pickup") || !strcmp(name, "lost") || !strcmp(name, "lost-twice"))
	{
		/* (unit 1 takes up another weapon: at once to both, again the round
		after to the OpenH1 one, which had it so even having lost the first;
		lost twice, by the slow refresh) */
		boolean lost = strcmp(name, "pickup") != 0;
		boolean twice = !strcmp(name, "lost-twice");
		long change_time;

		while (now_time % INVENTORY_INTERVAL_TICKS != 0)
			now_time++;
		units[1].unit.weapon_object_indices[0] = 5;
		change_time = now_time;
		losing[1] = lost;
		rounds(1);
		CHECK(sends[0][1] == 1 && had_weapon[0][1] == 5, "the OpenCE machine had the change at once");
		CHECK(sends[1][1] == 1, "the OpenH1 machine was sent the change at once");
		if (!twice)
			losing[1] = FALSE;
		rounds(INVENTORY_INTERVAL_TICKS);
		losing[1] = FALSE;
		CHECK(sends[0][1] == 1, "the OpenCE machine had it once (%ld)", sends[0][1]);
		CHECK(sends[1][1] == 2, "the OpenH1 machine had it again the round after (%ld)", sends[1][1]);
		if (!twice)
		{
			CHECK(had_weapon[1][1] == 5, "the change had");
			rounds(INVENTORY_OPENH1_REFRESHES * INVENTORY_REFRESH_TICKS - 1 - (now_time - change_time));
			CHECK(sends[1][1] == 2, "then not until the slow refresh (%ld)", sends[1][1]);
			return 0;
		}
		CHECK(had_weapon[1][1] != 5, "the test's two sends lost");
		rounds(INVENTORY_OPENH1_REFRESHES * INVENTORY_REFRESH_TICKS + 1 - (now_time - change_time));
		CHECK(had_weapon[1][1] == 5, "a change lost twice had by the slow refresh");
		return 0;
	}
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
