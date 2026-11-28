/*
RT_REFLECTIONS.C

display.rt_reflections (rt_reflections.h, docs/rt-plan/2-reflections.md):
the setting, and the frame's ray-traced scene (world_rt.c, through
world_ao.c's world_ao_rt_frame, as display.shadows "ray_traced" hands it)
for the windows whose surfaces trace their reflections.
*/

#include "rt_reflections.h"
#include "world_ao.h"

#include <string.h>

int config_boolean(const char *name);
unsigned long halo_gpu_settings_taken(void);
void platform_log(const char *format, ...);
/* (lt_lights.c) */
int lt_lights_pass_on(void);

/* ---------- globals */

static struct
{
	int initialized;
	unsigned long read_at;
	int wanted, missing_said;
} rt_reflections;

/* ---------- the setting */

int rt_reflections_on(void)
{
	unsigned long changes = halo_gpu_settings_taken();

	if (!rt_reflections.initialized || rt_reflections.read_at != changes)
	{
		rt_reflections.initialized = 1;
		rt_reflections.read_at = changes;
		rt_reflections.wanted = config_boolean("display.rt_reflections") != 0;
	}
	if (!rt_reflections.wanted)
		return 0;
	if (halo_world_ao_ray_queries())
		return 1;
	if (!rt_reflections.missing_said && halo_world_ao_ray_queries_missing())
	{
		platform_log("reflections: display.rt_reflections keeps the cube maps here: %s",
			halo_world_ao_ray_queries_missing());
		rt_reflections.missing_said = 1;
	}
	return 0;
}

/* ---------- the window */

void rt_reflections_window(int primary)
{
	/* (and for the ray-traced lighting: its lights' shadows from every
	object, which display.ambient_occlusion "off" (the default) otherwise
	leaves out of the scene but for the flashlight's) */
	if (primary && (rt_reflections_on() || (lt_lights_pass_on() && world_ao_rt_scene_ready())))
		world_ao_rt_frame();
}
