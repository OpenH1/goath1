/* display.shadows "ray_traced" (port/linux/game/rt_shadows.c;
test_rt_shadows.py): on only where the setting says so and the backend has
ray queries (said once where it cannot be); a caster packed as the
backend's pass reads it (its direction, darkness and owners); the largest
on screen kept past the most; the window's scene built before its casters
are handed over. */

#include "harness.h"
#include "rt_shadows.h"
#include "world_ao.h"
#include "world_rt.h"

#include <math.h>

static const char *setting = "classic";
static unsigned long changes = 1;
static int ray_queries, backend_ready, logged, rt_frames, handed, handed_at_frame;
static struct rt_shadow_caster handed_casters[RT_SHADOWS_CASTERS];
static float handed_camera[3];

const char *config_string(const char *name)
{
	CHECK(!strcmp(name, "display.shadows"), "another setting read: %s", name);
	return setting;
}

unsigned long halo_gpu_settings_taken(void)
{
	return changes;
}

int halo_world_ao_ray_queries(void)
{
	return ray_queries;
}

char const *halo_world_ao_ray_queries_missing(void)
{
	return backend_ready && !ray_queries ? "none here" : NULL;
}

void platform_log(const char *format, ...)
{
	(void)format;
	logged++;
}

unsigned long world_rt_owner(long object_index)
{
	return object_index == -1 ? 0 : (unsigned long)(object_index & 0xFFFF) + 1;
}

unsigned long long SDL_GetTicksNS(void)
{
	return 0;
}

void world_ao_rt_frame(void)
{
	rt_frames++;
}

void halo_rt_shadows(struct rt_shadow_caster const *casters, int count, float const camera[3])
{
	handed = count;
	handed_at_frame = rt_frames;
	memcpy(handed_casters, casters, (size_t)count * sizeof(*casters));
	memcpy(handed_camera, camera, sizeof(handed_camera));
}

#include "under_test.inc"

static void setting_case(void)
{
	CHECK(!rt_shadows_on(), "on while \"classic\"");
	setting = "ray_traced";
	changes++;
	CHECK(!rt_shadows_on() && logged == 0, "on before the backend said: %d", logged);
	backend_ready = 1;
	CHECK(!rt_shadows_on() && !rt_shadows_on() && logged == 1, "without ray queries: said %d times", logged);
	ray_queries = 1;
	CHECK(rt_shadows_on(), "off with ray queries");
	setting = "nonsense";
	changes++;
	CHECK(!rt_shadows_on(), "on for another value");
}

static void pack_case(void)
{
	static const float center[3] = { 1.0f, 2.0f, 3.0f };
	static const float vector[3] = { 0.3f, 0.4f, -0.866f };
	static const float color[3] = { 0.5f, 0.75f, 1.0f };
	struct rt_shadow_caster caster;
	float length = sqrtf(0.3f * 0.3f + 0.4f * 0.4f + 0.866f * 0.866f);

	rt_shadows_pack(&caster, 0x10003, 0x20005, center, 0.7f, vector, color, 0.5f);
	CHECK(caster.sphere[0] == 1.0f && caster.sphere[2] == 3.0f && caster.sphere[3] == 0.7f, "the sphere");
	CHECK(fabsf(caster.direction[0] - 0.3f / length) < 1.0e-6f && fabsf(caster.direction[1] - 0.4f / length) < 1.0e-6f,
		"the direction: %g %g", caster.direction[0], caster.direction[1]);
	/* (1 - colour) * strength, a byte each, red lowest */
	CHECK(caster.darkness == (64u | (32u << 8) | (0u << 16)), "the darkness: %08x", caster.darkness);
	CHECK(caster.owners == (4u | (6u << 16)), "the owners: %08x", caster.owners);
	rt_shadows_pack(&caster, 0x10003, -1, center, 0.7f, vector, color, 1.0f);
	CHECK(caster.owners == 4u && caster.darkness == (128u | (64u << 8)), "no child: %08x %08x", caster.owners,
		caster.darkness);
}

static void window_case(void)
{
	static const float center[3] = { 0.0f, 0.0f, 0.0f };
	static const float vector[3] = { 0.0f, 0.0f, -1.0f };
	static const float color[3] = { 0.5f, 0.5f, 0.5f };
	static const float camera[3] = { 5.0f, 6.0f, 7.0f };
	int index, small = 0;

	rt_shadows_begin();
	/* (none for a caster of no strength or size) */
	rt_shadows_caster(1, -1, center, 1.0f, vector, color, 0.0f, 100.0f);
	rt_shadows_caster(2, -1, center, 0.0f, vector, color, 1.0f, 100.0f);
	for (index = 0; index < RT_SHADOWS_CASTERS; index++)
		rt_shadows_caster(10 + index, -1, center, 1.0f, vector, color, 1.0f, index == 7 ? 31.0f : 100.0f + index);
	/* (past the most: the smallest on screen given up for a larger, not a
	smaller) */
	rt_shadows_caster(500, -1, center, 1.0f, vector, color, 1.0f, 40.0f);
	rt_shadows_caster(501, -1, center, 1.0f, vector, color, 1.0f, 20.0f);
	rt_shadows_window(camera);
	CHECK(rt_frames == 1 && handed_at_frame == 1, "the scene built before: %d %d", rt_frames, handed_at_frame);
	CHECK(handed == RT_SHADOWS_CASTERS, "%d handed", handed);
	CHECK(handed_camera[1] == 6.0f, "the camera");
	for (index = 0; index < handed; index++)
	{
		CHECK((handed_casters[index].owners & 0xFFFF) != 18 && (handed_casters[index].owners & 0xFFFF) != 502,
			"the smallest kept: %u", handed_casters[index].owners);
		small += (handed_casters[index].owners & 0xFFFF) == 501;
	}
	CHECK(small == 1, "the larger past the most not kept");
	/* (the next window: none) */
	rt_shadows_begin();
	rt_shadows_window(camera);
	CHECK(handed == 0 && rt_frames == 2, "the next window's: %d", handed);
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "setting"))
		setting_case();
	else if (!strcmp(name, "pack"))
		pack_case();
	else if (!strcmp(name, "window"))
		window_case();
	else
		return 2;
	return 0;
}
