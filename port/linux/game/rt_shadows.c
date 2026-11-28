/*
RT_SHADOWS.C

display.shadows "ray_traced" (rt_shadows.h, docs/world-ao.md "Ray-traced
shadows"): a window's casters, as render_objects.c's render_object_rt_shadows
finds them, packed for the backend's pass and handed over with the frame's
ray-traced scene (world_rt.c, built here too where
display.ambient_occlusion is not "ray_traced": world_ao_rt_frame).
*/

#include "rt_shadows.h"
#include "world_ao.h"
#include "world_rt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *config_string(const char *name);
unsigned long halo_gpu_settings_taken(void);
/* (the ray-traced lighting on: lt_lights.c) */
int lt_lights_pass_on(void);
void platform_log(const char *format, ...);
unsigned long long SDL_GetTicksNS(void);

/* ---------- globals */

static struct
{
	int initialized;
	unsigned long read_at;
	int wanted, missing_said;
	struct rt_shadow_caster casters[RT_SHADOWS_CASTERS];
	float pixels[RT_SHADOWS_CASTERS];
	int count;
} rt_shadows;

/* ---------- the setting */

int rt_shadows_on(void)
{
	unsigned long changes = halo_gpu_settings_taken();

	if (!rt_shadows.initialized || rt_shadows.read_at != changes)
	{
		const char *setting = config_string("display.shadows");

		rt_shadows.initialized = 1;
		rt_shadows.read_at = changes;
		rt_shadows.wanted = setting && !strcmp(setting, "ray_traced");
	}
	/* (the ray-traced lighting on traces every light's shadows itself: the
	setting kept, drawn as "classic" until it is off again) */
	if (!rt_shadows.wanted || lt_lights_pass_on())
		return 0;
	if (halo_world_ao_ray_queries())
		return 1;
	if (!rt_shadows.missing_said && halo_world_ao_ray_queries_missing())
	{
		platform_log("shadows: display.shadows \"ray_traced\" is drawn as \"classic\" here: %s",
			halo_world_ao_ray_queries_missing());
		rt_shadows.missing_said = 1;
	}
	return 0;
}

/* ---------- the arithmetic */

static unsigned int rt_shadows_byte(float value)
{
	value = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (unsigned int)(value * 255.0f + 0.5f);
}

void rt_shadows_pack(struct rt_shadow_caster *caster, long object_index, long child_index, float const center[3],
	float radius, float const shadow_vector[3], float const shadow_color[3], float strength)
{
	float length = sqrtf(shadow_vector[0] * shadow_vector[0] + shadow_vector[1] * shadow_vector[1] +
		shadow_vector[2] * shadow_vector[2]);
	unsigned int darkness = 0;
	int channel;

	memcpy(caster->sphere, center, 3 * sizeof(float));
	caster->sphere[3] = radius;
	/* (its direction's x and y, unit length: its z, down, is found from
	them; straight down where it has none) */
	caster->direction[0] = length > 0.0f ? shadow_vector[0] / length : 0.0f;
	caster->direction[1] = length > 0.0f ? shadow_vector[1] / length : 0.0f;
	/* (as render_object_shadow_begin scales the colour toward white by the
	strength, and the receivers multiply by 1 - (1 - it)) */
	for (channel = 0; channel < 3; channel++)
		darkness |= rt_shadows_byte((1.0f - shadow_color[channel]) * strength) << (8 * channel);
	caster->darkness = darkness;
	caster->owners = (unsigned int)(world_rt_owner(object_index) & 0xFFFF) |
		(child_index != -1 ? (unsigned int)(world_rt_owner(child_index) & 0xFFFF) << 16 : 0);
}

/* ---------- the window */

void rt_shadows_begin(void)
{
	rt_shadows.count = 0;
}

void rt_shadows_caster(long object_index, long child_index, float const center[3], float radius,
	float const shadow_vector[3], float const shadow_color[3], float strength, float pixels)
{
	int slot = rt_shadows.count, index;

	if (radius <= 0.0f || strength <= 0.0f)
		return;
	/* (more than the pass takes: the smallest on screen given up) */
	if (slot >= RT_SHADOWS_CASTERS)
	{
		slot = 0;
		for (index = 1; index < RT_SHADOWS_CASTERS; index++)
		{
			if (rt_shadows.pixels[index] < rt_shadows.pixels[slot])
				slot = index;
		}
		if (rt_shadows.pixels[slot] >= pixels)
			return;
	}
	else
		rt_shadows.count++;
	rt_shadows_pack(&rt_shadows.casters[slot], object_index, child_index, center, radius, shadow_vector, shadow_color,
		strength);
	rt_shadows.pixels[slot] = pixels;
}

void rt_shadows_window(float const camera[3])
{
	/* (the frame's scene, once a frame, before the pass that traces it) */
	world_ao_rt_frame();
	halo_rt_shadows(rt_shadows.casters, rt_shadows.count, camera);
}

/* ---------- measuring (HALO_SHADOW_TIMES): the game thread's time in the
objects' shadows (render_object_shadows, either path), a window's on
average, logged every 600 windows */

void rt_shadows_measure(int entering)
{
	static int wanted;
	static unsigned long long began, total;
	static long windows;

	if (!wanted)
		wanted = getenv("HALO_SHADOW_TIMES") ? 1 : -1;
	if (wanted < 0)
		return;
	if (entering)
	{
		began = SDL_GetTicksNS();
		return;
	}
	total += SDL_GetTicksNS() - began;
	if (++windows >= 600)
	{
		platform_log("shadows: %s: %.3f ms a window on the game thread", rt_shadows.wanted ? "ray_traced" : "classic",
			(double)total / (double)windows / 1.0e6);
		total = 0;
		windows = 0;
	}
}
