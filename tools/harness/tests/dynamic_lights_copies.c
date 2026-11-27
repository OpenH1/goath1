/*
DYNAMIC_LIGHTS_COPIES.C

A light's copies on its other markers (port/linux/game/dynamic_lights.c
dynamic_lights_copy, dynamic_lights_copies): each copy's key, which the
ray-traced shadows' choice and every history know it by from frame to
frame, is its own light's key and its number among that light's copies, so
that it stays the same while other lights' copies come and go, and no two
copies of a frame share one. The lights are made-up handles; every marker
is in cluster 0.
*/

#include "harness.h"

#define MAXIMUM_LIGHTS_PER_WINDOW 128
#define MAXIMUM_CLUSTERS_PER_STRUCTURE 512
#define MIN(a, b) ((a) < (b) ? (a) : (b))

struct location { long leaf_index; short cluster_index; };

static void scenario_location_from_point(struct location *location, real_point3d const *position)
{
	(void)position;
	location->leaf_index = 0;
	location->cluster_index = 0;
}

static short door_portals_clusters_in_sphere(short cluster_index, real_point3d const *position, real radius,
	short maximum_count, short *clusters)
{
	(void)position;
	(void)radius;
	(void)maximum_count;
	clusters[0] = cluster_index;
	return 1;
}

#include "under_test.inc"

/* each frame's lights' keys and markers (a copy's: its marker's number,
where it was placed) */
struct frame
{
	unsigned long long keys[MAXIMUM_LIGHTS_PER_WINDOW];
	real markers[MAXIMUM_LIGHTS_PER_WINDOW];
};

/* a frame's game lights (their rasterizer lights 0 on, each known by its
handle) and their copies: copies[light] of them for each */
static long frame(long const *handles, short const *copies, short light_count, struct frame *out)
{
	short light, number;
	long count;

	memset(&dynamic_lights, 0, sizeof(dynamic_lights));
	for (light = 0; light < light_count; light++)
	{
		dynamic_lights.keys[light] = DYNAMIC_LIGHTS_KEY_GAME | (unsigned long long)(unsigned long)handles[light];
		dynamic_lights.lights[light].radius = 1.0f;
		for (number = 1; number <= copies[light]; number++)
		{
			real_point3d position = { (real)light, (real)number, 0.0f };
			real_vector3d axis = { 1.0f, 0.0f, 0.0f };

			dynamic_lights_copy(light, number, &position, &axis);
		}
	}
	count = dynamic_lights_copies(light_count);
	for (light = 0; light < MAXIMUM_LIGHTS_PER_WINDOW; light++)
	{
		out->keys[light] = dynamic_lights.keys[light];
		out->markers[light] = dynamic_lights.lights[light].position[1];
	}
	return count;
}

/* the copy's key of the frame's (by its light's handle and position: its
marker), 0 if none */
static unsigned long long key_at(struct frame const *frame, long game_count, long count, long handle, short number)
{
	long index;

	for (index = game_count; index < count; index++)
	{
		if ((frame->keys[index] & 0xffffffffull) == (unsigned long long)handle && frame->markers[index] == (real)number)
			return frame->keys[index];
	}
	return 0;
}

int main(int argc, char **argv)
{
	static struct frame first, second;
	char const *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "stable-as-others-come-and-go"))
	{
		/* (a Banshee's wing light, then a Ghost's three running lights; the
		next frame the Banshee is gone, the frame after it is back behind
		the Ghost) */
		static long const both[2] = { 0x10001, 0x20002 }, ghost[1] = { 0x20002 }, swapped[2] = { 0x20002, 0x10001 };
		static short const both_copies[2] = { 1, 2 }, ghost_copies[1] = { 2 }, swapped_copies[2] = { 2, 1 };
		long count = frame(both, both_copies, 2, &first), again;
		short number;

		CHECK(count == 5, "the frame's lights and copies: %ld", count);
		again = frame(ghost, ghost_copies, 1, &second);
		for (number = 1; number <= 2; number++)
		{
			CHECK(key_at(&second, 1, again, 0x20002, number) == key_at(&first, 2, count, 0x20002, number),
				"the Ghost's copy %d: %llx, the frame before %llx", number, key_at(&second, 1, again, 0x20002, number),
				key_at(&first, 2, count, 0x20002, number));
		}
		again = frame(swapped, swapped_copies, 2, &second);
		for (number = 1; number <= 2; number++)
		{
			CHECK(key_at(&second, 2, again, 0x20002, number) == key_at(&first, 2, count, 0x20002, number),
				"the Ghost's copy %d first: %llx", number, key_at(&second, 2, again, 0x20002, number));
		}
		CHECK(key_at(&second, 2, again, 0x10001, 1) == key_at(&first, 2, count, 0x10001, 1),
			"the Banshee's copy after the Ghost's: %llx, before %llx", key_at(&second, 2, again, 0x10001, 1),
			key_at(&first, 2, count, 0x10001, 1));
		return 0;
	}
	if (!strcmp(name, "distinct-copies"))
	{
		static long const handles[3] = { 0x10001, 0x20002, 0x30003 };
		static short const copies[3] = { 3, 2, 4 };
		long count = frame(handles, copies, 3, &first), index, other;

		CHECK(count == 3 + 9, "the frame's lights and copies: %ld", count);
		for (index = 3; index < count; index++)
		{
			CHECK((first.keys[index] >> 56) == (DYNAMIC_LIGHTS_KEY_COPY >> 56), "copy %ld's kind: %llx", index,
				first.keys[index]);
			for (other = 0; other < index; other++)
				CHECK(first.keys[other] != first.keys[index], "copies %ld and %ld share key %llx", other, index,
					first.keys[index]);
		}
		return 0;
	}
	if (!strcmp(name, "unknown-light-unkeyed"))
	{
		long count;

		memset(&dynamic_lights, 0, sizeof(dynamic_lights));
		dynamic_lights.lights[0].radius = 1.0f;
		dynamic_lights_copy(0, 1, &(real_point3d){ 0.0f, 1.0f, 0.0f }, &(real_vector3d){ 1.0f, 0.0f, 0.0f });
		count = dynamic_lights_copies(1);
		CHECK(count == 2 && dynamic_lights.keys[1] == 0, "a copy of a light not known: %ld, key %llx", count,
			dynamic_lights.keys[1]);
		return 0;
	}
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
