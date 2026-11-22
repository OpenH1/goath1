/*
DYNAMIC_LIGHTS_CELLS.C (test)

The enhanced dynamic lights' grid (port/linux/src/d3d8_device.c's
dynamic_lights_scales, dynamic_lights_edges and dynamic_lights_mark; xgpu.h):
the cells' masks built as halo_dynamic_lights_set builds them hold every light
that reaches a point, the point's cell found as the shaders find it in floats
(xgpu_msl.c's and xgpu_spirv.c's dynamic_light_sum: the offset from the grid's
middle, sign(offset) log2(1 + |offset| / cell) scale + size / 2, floored and
clamped) and a light reaching it where the shaders add it (its distance
squared times one over its radius, squared, under 1). On random light sets
about cameras near and far from the world's middle, with points inside each
light's sphere, at its edge and on the cells' edges, lights past the grid's
reach, the most lights. "dump" reads a file of the game's lights and its
lightmap pass's triangles (LIGHTS_DUMP, written by a build with a dump of
them) and prints the lights in a point's cell's mask, as the masks were built
before (every cell of the box about the light's sphere, the grid reaching
XGPU_DYNAMIC_LIGHTS_REACH) and as now, against those reaching it.
test_dynamic_lights_cells.py takes the code.
*/

#include "harness.h"

#include <math.h>
#include <stdint.h>

#include "under_test.inc"

static const int sizes[3] = { XGPU_DYNAMIC_LIGHTS_GRID_X, XGPU_DYNAMIC_LIGHTS_GRID_Y, XGPU_DYNAMIC_LIGHTS_GRID_Z };
static unsigned int mask[XGPU_DYNAMIC_LIGHTS_GRID_Z][XGPU_DYNAMIC_LIGHTS_GRID_Y][XGPU_DYNAMIC_LIGHTS_GRID_X][4];
static struct halo_dynamic_light lights[XGPU_DYNAMIC_LIGHTS_MAXIMUM];
static float camera[3], scales[3];

/* the masks, as halo_dynamic_lights_set builds them */
static void cells_build(int count)
{
	float lower[3][XGPU_DYNAMIC_LIGHTS_GRID_X], upper[3][XGPU_DYNAMIC_LIGHTS_GRID_X];
	int index, axis;

	memset(mask, 0, sizeof(mask));
	dynamic_lights_scales(lights, count, camera, scales);
	for (axis = 0; axis < 3; axis++)
		dynamic_lights_edges(scales[axis], sizes[axis], lower[axis], upper[axis]);
	for (index = 0; index < count; index++)
	{
		float offset[3];

		if (!(lights[index].radius > 0.0f))
			continue;
		for (axis = 0; axis < 3; axis++)
			offset[axis] = lights[index].position[axis] - camera[axis];
		dynamic_lights_mark(mask, offset, lights[index].radius, scales, (const float (*)[XGPU_DYNAMIC_LIGHTS_GRID_X])lower,
			(const float (*)[XGPU_DYNAMIC_LIGHTS_GRID_X])upper, index);
	}
}

/* the masks as they were built before: every cell of the box about the
light's sphere, the grid reaching XGPU_DYNAMIC_LIGHTS_REACH */
static void cells_build_boxes(int count)
{
	int index, axis, x, y, z;

	memset(mask, 0, sizeof(mask));
	for (axis = 0; axis < 3; axis++)
		scales[axis] = (float)(sizes[axis] / 2) / log2f(1.0f + XGPU_DYNAMIC_LIGHTS_REACH / XGPU_DYNAMIC_LIGHTS_CELL);
	for (index = 0; index < count; index++)
	{
		const struct halo_dynamic_light *light = &lights[index];
		int first[3], last[3];

		if (!(light->radius > 0.0f))
			continue;
		for (axis = 0; axis < 3; axis++)
		{
			first[axis] = dynamic_lights_cell(light->position[axis] - light->radius - camera[axis], scales[axis],
				sizes[axis]);
			last[axis] = dynamic_lights_cell(light->position[axis] + light->radius - camera[axis], scales[axis],
				sizes[axis]);
		}
		for (z = first[2]; z <= last[2]; z++)
			for (y = first[1]; y <= last[1]; y++)
				for (x = first[0]; x <= last[0]; x++)
					mask[z][y][x][index >> 5] |= 1u << (index & 31);
	}
}

/* the shaders' cell of a point (volatile: each step rounded to a float, as
the shaders' are) */
static const unsigned int *shader_cell(const float point[3])
{
	int at[3], axis;

	for (axis = 0; axis < 3; axis++)
	{
		volatile float offset = point[axis] - camera[axis];
		volatile float scaled = fabsf(offset) * (1.0f / XGPU_DYNAMIC_LIGHTS_CELL);
		volatile float logged = log2f(1.0f + scaled);
		volatile float cell = (offset > 0.0f ? 1.0f : offset < 0.0f ? -1.0f : 0.0f) * logged * scales[axis] +
			(float)(sizes[axis] / 2);
		int index = (int)floorf(cell);

		at[axis] = index < 0 ? 0 : index > sizes[axis] - 1 ? sizes[axis] - 1 : index;
	}
	return mask[at[2]][at[1]][at[0]];
}

/* whether the shaders add the light at the point */
static int shader_reaches(const struct halo_dynamic_light *light, const float point[3])
{
	volatile float w = 1.0f / light->radius;
	volatile float to[3], distance_squared, x2;
	int axis;

	for (axis = 0; axis < 3; axis++)
		to[axis] = light->position[axis] - point[axis];
	distance_squared = to[0] * to[0] + to[1] * to[1] + to[2] * to[2];
	x2 = distance_squared * w * w;
	return light->radius > 0.0f && x2 < 1.0f;
}

static unsigned int seed = 12345;

static float random_unit(void)
{
	seed = seed * 1664525u + 1013904223u;
	return (float)(seed >> 8) / 16777216.0f;
}

static float random_between(float low, float high)
{
	return low + (high - low) * random_unit();
}

static long checked;

static void check_point(int count, const float point[3], const char *what)
{
	const unsigned int *cell = shader_cell(point);
	int index;

	for (index = 0; index < count; index++)
	{
		if (!shader_reaches(&lights[index], point))
			continue;
		checked++;
		CHECK(cell[index >> 5] >> (index & 31) & 1, "%s: light %d (%.6g %.6g %.6g, radius %.6g) reaches %.9g %.9g %.9g "
			"(camera %.6g %.6g %.6g) but its cell has it not", what, index, lights[index].position[0],
			lights[index].position[1], lights[index].position[2], lights[index].radius, point[0], point[1], point[2],
			camera[0], camera[1], camera[2]);
	}
}

/* points in each light's sphere (and at its edge: a float either side) */
static void check_spheres(int count, int points, const char *what)
{
	int index, point, axis;

	for (index = 0; index < count; index++)
	{
		for (point = 0; point < points; point++)
		{
			float direction[3], length = 0.0f, distance, at[3];

			for (axis = 0; axis < 3; axis++)
			{
				direction[axis] = random_between(-1.0f, 1.0f);
				length += direction[axis] * direction[axis];
			}
			length = sqrtf(length);
			if (!(length > 1.0e-3f))
				continue;
			distance = lights[index].radius * (point & 1 ? cbrtf(random_unit()) : 1.0f - random_unit() * 1.0e-6f);
			for (axis = 0; axis < 3; axis++)
				at[axis] = lights[index].position[axis] + direction[axis] / length * distance;
			check_point(count, at, what);
		}
	}
}

/* points on the cells' edges (as the shaders' rounding finds them, a few
floats either side) inside a light's sphere */
static void check_edges(int count, const char *what)
{
	int index, axis, cell, step;

	for (index = 0; index < count; index++)
	{
		for (axis = 0; axis < 3; axis++)
		{
			for (cell = 1; cell < sizes[axis]; cell++)
			{
				double t = ((double)cell - sizes[axis] / 2) / scales[axis];
				float edge = camera[axis] + (float)((t < 0 ? -1.0 : 1.0) * XGPU_DYNAMIC_LIGHTS_CELL * (exp2(fabs(t)) - 1.0));
				float at[3];

				memcpy(at, lights[index].position, sizeof(at));
				if (!(fabsf(edge - at[axis]) < lights[index].radius))
					continue;
				for (step = -4; step <= 4; step++)
				{
					at[axis] = edge + (float)step * fmaxf(fabsf(edge), 1.0f) * 1.2e-7f;
					check_point(count, at, what);
				}
			}
		}
	}
}

static int random_sets(void)
{
	static const float middles[] = { 0.0f, 37.5f, -950.0f, 3000.0f };
	int set;

	for (set = 0; set < 400; set++)
	{
		int count = set % 7 == 0 ? XGPU_DYNAMIC_LIGHTS_MAXIMUM : 1 + (int)(random_unit() * 40.0f), index, axis;
		float middle = middles[set % 4], spread = set % 3 == 0 ? 6.0f : set % 3 == 1 ? 60.0f : 400.0f;

		for (axis = 0; axis < 3; axis++)
			camera[axis] = middle + random_between(-50.0f, 50.0f);
		memset(lights, 0, sizeof(lights));
		for (index = 0; index < count; index++)
		{
			for (axis = 0; axis < 3; axis++)
				lights[index].position[axis] = camera[axis] + random_between(-spread, spread);
			lights[index].radius = index % 5 == 0 ? random_between(4.0f, 30.0f) : random_between(0.05f, 3.0f);
			if (index % 11 == 3)
				lights[index].radius = 0.0f;
		}
		cells_build(count);
		check_spheres(count, 200, "random");
		check_edges(count, "edges");
	}
	CHECK(checked > 1000000, "only %ld points reached", checked);
	return 1;
}

/* a light at the camera, one at the grid's reach and one past it */
static int far_lights(void)
{
	int index;

	memset(lights, 0, sizeof(lights));
	camera[0] = 10.0f, camera[1] = -20.0f, camera[2] = 3.0f;
	for (index = 0; index < 3; index++)
	{
		memcpy(lights[index].position, camera, sizeof(camera));
		lights[index].radius = 2.0f;
	}
	lights[1].position[0] += XGPU_DYNAMIC_LIGHTS_REACH - 1.0f;
	lights[2].position[0] -= 3.0f * XGPU_DYNAMIC_LIGHTS_REACH;
	lights[2].position[2] += 900.0f;
	lights[2].radius = 50.0f;
	cells_build(3);
	check_spheres(3, 20000, "far");
	check_edges(3, "far edges");
	return 1;
}

/* the masks of the most lights, all near the camera, built fast */
static int speed(void)
{
	clock_t start;
	int index, axis, round;
	double each;

	memset(lights, 0, sizeof(lights));
	for (axis = 0; axis < 3; axis++)
		camera[axis] = 100.0f;
	for (index = 0; index < XGPU_DYNAMIC_LIGHTS_MAXIMUM; index++)
	{
		for (axis = 0; axis < 3; axis++)
			lights[index].position[axis] = camera[axis] + random_between(-20.0f, 20.0f);
		lights[index].radius = random_between(0.5f, 6.0f);
	}
	start = clock();
	for (round = 0; round < 200; round++)
		cells_build(XGPU_DYNAMIC_LIGHTS_MAXIMUM);
	each = (double)(clock() - start) / CLOCKS_PER_SEC / 200.0 * 1000.0;
	printf("the most lights' masks: %.3f ms a frame\n", each);
	CHECK(each < 2.0, "%.3f ms a frame", each);
	return 1;
}

/* ---------- the game's lights (a dump) */

static float *triangles;
static long triangle_count, triangle_room;

static int dump_frame(int count, double totals[3], double *weights)
{
	long triangle;
	int build, index;
	double frame[3] = { 0.0, 0.0, 0.0 }, weight = 0.0;

	for (build = 0; build < 2; build++)
	{
		if (build)
			cells_build(count);
		else
			cells_build_boxes(count);
		for (triangle = 0; triangle < triangle_count; triangle++)
		{
			const float *corner = triangles + triangle * 9;
			float edge[2][3], normal[3], area, middle[3], distance_squared = 0.0f;
			int sample, axis;

			for (axis = 0; axis < 3; axis++)
			{
				edge[0][axis] = corner[3 + axis] - corner[axis];
				edge[1][axis] = corner[6 + axis] - corner[axis];
				middle[axis] = (corner[axis] + corner[3 + axis] + corner[6 + axis]) / 3.0f - camera[axis];
				distance_squared += middle[axis] * middle[axis];
			}
			normal[0] = edge[0][1] * edge[1][2] - edge[0][2] * edge[1][1];
			normal[1] = edge[0][2] * edge[1][0] - edge[0][0] * edge[1][2];
			normal[2] = edge[0][0] * edge[1][1] - edge[0][1] * edge[1][0];
			area = 0.5f * sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
			/* (a few points a triangle, each weighed by its triangle's area as
			seen from the camera: about the pixels it covers) */
			for (sample = 0; sample < 16; sample++)
			{
				float u = random_unit(), v = random_unit(), at[3], seen;
				const unsigned int *cell;

				if (u + v > 1.0f)
					u = 1.0f - u, v = 1.0f - v;
				for (axis = 0; axis < 3; axis++)
					at[axis] = corner[axis] + u * edge[0][axis] + v * edge[1][axis];
				seen = area / 16.0f / fmaxf(distance_squared, 0.25f);
				cell = shader_cell(at);
				for (index = 0; index < count; index++)
				{
					int in = cell[index >> 5] >> (index & 31) & 1, reaches = shader_reaches(&lights[index], at);

					frame[build] += in * seen;
					if (!build)
						frame[2] += reaches * seen;
					if (build)
						CHECK(in || !reaches, "dump: light %d reaches a point its cell has not", index);
				}
				if (!build)
					weight += seen;
			}
		}
	}
	if (weight > 0.0)
		printf("  %3d lights, %6ld triangles: stepped %.2f before, %.2f now; reaching %.2f\n", count, triangle_count,
			frame[0] / weight, frame[1] / weight, frame[2] / weight);
	for (index = 0; index < 3; index++)
		totals[index] += frame[index];
	*weights += weight;
	return 1;
}

static int dump(void)
{
	const char *path = getenv("LIGHTS_DUMP");
	FILE *file = path ? fopen(path, "rb") : NULL;
	double totals[3] = { 0.0, 0.0, 0.0 }, weights = 0.0;
	int count = -1, frames = 0, tag;

	CHECK(file, "no dump (LIGHTS_DUMP)");
	for (;;)
	{
		tag = fgetc(file);
		if (tag == 'T' || tag == 'U')
		{
			int n;
			long index;

			CHECK(fread(&n, 4, 1, file) == 1 && n >= 0, "a short dump");
			for (index = 0; index + 3 <= n; index += 3)
			{
				float corners[9];

				CHECK(fread(corners, 4, 9, file) == 9, "a short dump");
				if (tag != 'T')
					continue;
				if (triangle_count == triangle_room)
				{
					triangle_room = triangle_room ? triangle_room * 2 : 4096;
					triangles = realloc(triangles, (size_t)triangle_room * 9 * sizeof(float));
				}
				memcpy(triangles + triangle_count++ * 9, corners, sizeof(corners));
			}
			continue;
		}
		if (count >= 0 && triangle_count)
		{
			dump_frame(count, totals, &weights);
			frames++;
		}
		triangle_count = 0;
		if (tag != 'L')
			break;
		{
			float axes[6];
			int index;

			CHECK(fread(&count, 4, 1, file) == 1 && count >= 0 && count <= XGPU_DYNAMIC_LIGHTS_MAXIMUM &&
				fread(camera, 4, 3, file) == 3 && fread(axes, 4, 6, file) == 6, "a short dump");
			memset(lights, 0, sizeof(lights));
			for (index = 0; index < count; index++)
			{
				int flags;

				CHECK(fread(lights[index].position, 4, 3, file) == 3 && fread(&lights[index].radius, 4, 1, file) == 1 &&
					fread(&flags, 4, 1, file) == 1, "a short dump");
				lights[index].flags = flags;
			}
		}
	}
	fclose(file);
	CHECK(frames && weights > 0.0, "no frames in the dump");
	printf("%d frames: the lights a lit point's cell has, %.2f before, %.2f now; reaching it %.2f\n", frames,
		totals[0] / weights, totals[1] / weights, totals[2] / weights);
	return 1;
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "random"))
		return !random_sets();
	if (!strcmp(name, "far"))
		return !far_lights();
	if (!strcmp(name, "speed"))
		return !speed();
	if (!strcmp(name, "dump"))
		return !dump();
	fprintf(stderr, "unknown case %s\n", name);
	return 2;
}
