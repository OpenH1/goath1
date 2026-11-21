/*
LIGHTS_REACH_PART.C (test)

The lightmap pass's triangles parted by the enhanced dynamic lights' reach
(port/linux/src/d3d8_device.c's lights_reach_part): each triangle in the same
part (reached or not) and in the same order as the test of every triangle
against every light gives (reference_part, the split as it was before the
lights were found once for a run of triangles), on random triangle soups and
light sets: no lights, lights reaching everything, lights far away, triangles
exactly on a reach's edge (and a float either side of it), positions that are
no number, the most lights. test_lights_reach_part.py takes the code.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

typedef unsigned short WORD;
typedef int BOOL;

#include "under_test.inc"

/* the split as it was: every triangle against every light */
static void reference_part(const unsigned char *vertices, unsigned long stride, const WORD *index_data,
	unsigned long count, WORD *reached, unsigned long *reached_count, WORD *unreached, unsigned long *unreached_count)
{
	unsigned long index, lit = 0, unlit = 0;

	for (index = 0; index + 3 <= count; index += 3)
	{
		float low[3], high[3];
		BOOL touched = TRUE;
		int corner, axis, light;

		for (corner = 0; corner < 3; corner++)
		{
			float point[3];

			memcpy(point, vertices + index_data[index + corner] * stride, sizeof(point));
			for (axis = 0; axis < 3; axis++)
			{
				if (!corner || point[axis] < low[axis])
					low[axis] = point[axis];
				if (!corner || point[axis] > high[axis])
					high[axis] = point[axis];
			}
		}
		for (axis = 0; axis < 3; axis++)
		{
			if (high[axis] < dynamic_lights.reach_box[0][axis] || low[axis] > dynamic_lights.reach_box[1][axis])
				touched = FALSE;
		}
		if (touched)
		{
			touched = FALSE;
			for (light = 0; light < dynamic_lights.reach_count && !touched; light++)
			{
				const float *reach = dynamic_lights.reach[light];
				float distance = 0.0f;

				for (axis = 0; axis < 3; axis++)
				{
					float away = reach[axis] < low[axis] ? low[axis] - reach[axis] :
						reach[axis] > high[axis] ? reach[axis] - high[axis] : 0.0f;

					distance += away * away;
				}
				touched = !(distance > reach[3] * reach[3]);
			}
		}
		if (touched)
		{
			memcpy(reached + lit, index_data + index, 3 * sizeof(WORD));
			lit += 3;
		}
		else
		{
			memcpy(unreached + unlit, index_data + index, 3 * sizeof(WORD));
			unlit += 3;
		}
	}
	*reached_count = lit;
	*unreached_count = unlit;
}

static uint32_t seed = 12345;

static uint32_t next(void)
{
	seed = seed * 1664525u + 1013904223u;
	return seed >> 8;
}

/* in [low, high) */
static float between(float low, float high)
{
	return low + (high - low) * (float)(next() & 0xFFFF) / 65536.0f;
}

/* the lights, with their box, as halo_dynamic_lights_set sets them */
static void lights_clear(void)
{
	dynamic_lights.reach_count = 0;
	memset(dynamic_lights.reach_box, 0, sizeof(dynamic_lights.reach_box));
}

static void light_add(float x, float y, float z, float reach)
{
	float *light = dynamic_lights.reach[dynamic_lights.reach_count];
	float position[3];
	int axis;

	position[0] = x, position[1] = y, position[2] = z;
	light[3] = reach;
	for (axis = 0; axis < 3; axis++)
	{
		float low = position[axis] - reach, high = position[axis] + reach;

		light[axis] = position[axis];
		if (!dynamic_lights.reach_count || low < dynamic_lights.reach_box[0][axis])
			dynamic_lights.reach_box[0][axis] = low;
		if (!dynamic_lights.reach_count || high > dynamic_lights.reach_box[1][axis])
			dynamic_lights.reach_box[1][axis] = high;
	}
	dynamic_lights.reach_count++;
}

#define MOST_VERTICES 60000
#define MOST_INDICES 180000

/* the vertices (a position then other data, as a vertex's), the list */
static float vertices[MOST_VERTICES][5];
static WORD indices[MOST_INDICES];
static WORD parts[4][MOST_INDICES];

/* both splits of the list's count indices the same */
static void same(const char *what, unsigned long count)
{
	unsigned long lit[2], unlit[2];

	memset(parts, 0xEE, sizeof(parts));
	lights_reach_part((const unsigned char *)vertices, sizeof(vertices[0]), indices, count, parts[0], &lit[0],
		parts[1], &unlit[0]);
	reference_part((const unsigned char *)vertices, sizeof(vertices[0]), indices, count, parts[2], &lit[1],
		parts[3], &unlit[1]);
	CHECK(lit[0] == lit[1] && unlit[0] == unlit[1], "%s (%lu indices, %d lights): %lu reached and %lu not, not %lu "
		"and %lu", what, count, dynamic_lights.reach_count, lit[0], unlit[0], lit[1], unlit[1]);
	CHECK(!memcmp(parts[0], parts[2], lit[0] * sizeof(WORD)) && !memcmp(parts[1], parts[3], unlit[0] * sizeof(WORD)),
		"%s (%lu indices, %d lights): the parts' triangles differ", what, count, dynamic_lights.reach_count);
}

/* a soup of count indices into vertex_count vertices: in a walk about a world
size across (each triangle near the one before, as a mesh's), or at random */
static void soup(unsigned long vertex_count, unsigned long count, float size, BOOL walk)
{
	float at[3] = { 0.0f, 0.0f, 0.0f };
	unsigned long index;
	int axis;

	for (index = 0; index < vertex_count; index++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			if (walk)
			{
				at[axis] += between(-1.0f, 1.0f);
				if (at[axis] > size / 2 || at[axis] < -size / 2)
					at[axis] = 0.0f;
				vertices[index][axis] = at[axis];
			}
			else
			{
				vertices[index][axis] = between(-size / 2, size / 2);
			}
		}
		vertices[index][3] = vertices[index][4] = -1.0f;
	}
	for (index = 0; index < count; index++)
	{
		if (walk)
		{
			long near = (long)(index / 3) * (long)vertex_count / (long)(count / 3 + 1) + (long)(next() % 7) - 3;

			indices[index] = (WORD)(near < 0 ? 0 : near >= (long)vertex_count ? vertex_count - 1 : near);
		}
		else
		{
			indices[index] = (WORD)(next() % vertex_count);
		}
	}
}

static void lights_random(int count, float size, float least, float most)
{
	int light;

	lights_clear();
	for (light = 0; light < count; light++)
		light_add(between(-size / 2, size / 2), between(-size / 2, size / 2), between(-size / 2, size / 2),
			between(least, most));
}

static int random_soups(void)
{
	static const unsigned long counts[] = { 3, 4, 5, 93, 96, 97, 98, 99, 3 * 64 + 1, 3000, 30001 };
	static const int lights[] = { 0, 1, 2, 7, 60, 128 };
	int round, light, walk, index;

	for (round = 0; round < 8; round++)
	{
		for (walk = 0; walk < 2; walk++)
		{
			unsigned long count;

			for (count = 0; count < sizeof(counts) / sizeof(counts[0]); count++)
			{
				soup(4000, counts[count], 100.0f, walk);
				for (light = 0; light < (int)(sizeof(lights) / sizeof(lights[0])); light++)
				{
					/* near and small, near and large, everything reached, all far away */
					lights_random(lights[light], 100.0f, 0.5f, 6.0f);
					same("small lights", counts[count]);
					lights_random(lights[light], 100.0f, 10.0f, 40.0f);
					same("large lights", counts[count]);
					lights_random(lights[light], 10.0f, 500.0f, 600.0f);
					same("lights reaching everything", counts[count]);
					lights_random(lights[light], 10.0f, 0.5f, 6.0f);
					for (index = 0; index < dynamic_lights.reach_count; index++)
						dynamic_lights.reach[index][0] += 10000.0f;
					dynamic_lights.reach_box[0][0] += 10000.0f;
					dynamic_lights.reach_box[1][0] += 10000.0f;
					same("lights far away", counts[count]);
				}
			}
		}
	}
	return 1;
}

/* triangles whose nearest corner is just at a reach's edge, a float in, a
float out, each alone in a list (its own run) and among others */
static int edges(void)
{
	static const float reaches[] = { 1.0f, 2.0f, 3.0f, 0.7f, 5.5f, 13.25f };
	unsigned long r, side, axis;

	for (r = 0; r < sizeof(reaches) / sizeof(reaches[0]); r++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			for (side = 0; side < 3; side++)
			{
				float edge = reaches[r], corner = side == 0 ? edge : side == 1 ? nextafterf(edge, 0.0f) :
					nextafterf(edge, 100.0f);
				int vertex, others;

				lights_clear();
				light_add(0.0f, 0.0f, 0.0f, reaches[r]);
				memset(vertices, 0, 3 * sizeof(vertices[0]));
				for (vertex = 0; vertex < 3; vertex++)
					vertices[vertex][axis] = corner + (float)vertex;
				indices[0] = 0, indices[1] = 1, indices[2] = 2;
				same("a triangle at a reach's edge", 3);
				/* and among triangles far off, which the run's box spreads past */
				for (others = 1; others < 40; others++)
				{
					for (vertex = 0; vertex < 3; vertex++)
					{
						vertices[others * 3 + vertex][0] = 50.0f + (float)others;
						vertices[others * 3 + vertex][1] = 50.0f + (float)vertex;
						vertices[others * 3 + vertex][2] = 50.0f;
						indices[others * 3 + vertex] = (WORD)(others * 3 + vertex);
					}
				}
				same("a triangle at a reach's edge among others", 40 * 3);
				/* a light on the far side too */
				light_add(1000.0f, 1000.0f, 1000.0f, 1.0f);
				same("a triangle at a reach's edge, two lights", 3);
			}
		}
	}
	return 1;
}

/* positions that are no number, in one triangle of a run */
static int unordered(void)
{
	int round, axis;

	for (round = 0; round < 64; round++)
	{
		soup(400, 96 * 3, 40.0f, TRUE);
		lights_random(20, 40.0f, 1.0f, 4.0f);
		for (axis = 0; axis < 3; axis++)
			vertices[indices[(next() % 96) * 3 + next() % 3]][next() % 3] = NAN;
		same("positions that are no number", 96 * 3);
	}
	return 1;
}

/* the time each takes on a big list (50,000 triangles, 60 lights) */
static int speed(void)
{
	unsigned long lit, unlit, count = 150000;
	clock_t start;
	double times[2];
	int round;

	soup(MOST_VERTICES, count, 300.0f, TRUE);
	lights_random(60, 300.0f, 2.0f, 8.0f);
	same("a big list", count);
	start = clock();
	for (round = 0; round < 20; round++)
		lights_reach_part((const unsigned char *)vertices, sizeof(vertices[0]), indices, count, parts[0], &lit,
			parts[1], &unlit);
	times[0] = (double)(clock() - start);
	start = clock();
	for (round = 0; round < 20; round++)
		reference_part((const unsigned char *)vertices, sizeof(vertices[0]), indices, count, parts[2], &lit,
			parts[3], &unlit);
	times[1] = (double)(clock() - start);
	printf("50,000 triangles, 60 lights: %.2f ms a split, was %.2f ms (%.1fx); %lu of %lu reached\n",
		times[0] * 1000.0 / CLOCKS_PER_SEC / 20, times[1] * 1000.0 / CLOCKS_PER_SEC / 20,
		times[0] > 0.0 ? times[1] / times[0] : 0.0, lit / 3, count / 3);
	return 1;
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "random"))
		return !random_soups();
	if (!strcmp(name, "edges"))
		return !edges();
	if (!strcmp(name, "unordered"))
		return !unordered();
	if (!strcmp(name, "speed"))
		return !speed();
	fprintf(stderr, "no case %s\n", name);
	return 2;
}
