/*
CONTACT_SHADOWS.C

display.contact_shadows (contact_shadows.h): a window's casters, as
render_objects.c's render_object_rt_shadows finds them, packed as the
ray-traced shadows' are (rt_shadows_pack) and handed to the backend's pass
(halo_contact_shadows), which marches toward each one's light from the
depth: the window's light taken before the classic shadows' receivers
(halo_contact_shadows_light), the pass after them, so that a pixel keeps
the darker of the two shadows, not their product.
*/

#include "contact_shadows.h"

#include <string.h>

int config_boolean(const char *name);
unsigned long halo_gpu_settings_taken(void);
/* (lt_lights.c) */
int lt_lights_pass_on(void);
int lt_lights_real_sky(void);

static struct
{
	int initialized;
	unsigned long read_at;
	int wanted;
	struct rt_shadow_caster casters[CONTACT_SHADOWS_CASTERS];
	float pixels[CONTACT_SHADOWS_CASTERS];
	int count;
	/* the window's light was taken, and its camera */
	int taken;
	float camera[3];
} contact_shadows;

int contact_shadows_on(void)
{
	unsigned long changes = halo_gpu_settings_taken();

	if (!contact_shadows.initialized || contact_shadows.read_at != changes)
	{
		contact_shadows.initialized = 1;
		contact_shadows.read_at = changes;
		contact_shadows.wanted = config_boolean("display.contact_shadows") != 0;
	}
	/* (a level lit live: its shadows the traced sun's and sky's alone,
	lt_lights.h) */
	return contact_shadows.wanted && !rt_shadows_on() && !(lt_lights_pass_on() && lt_lights_real_sky());
}

void contact_shadows_begin(void)
{
	contact_shadows.count = 0;
	contact_shadows.taken = 0;
}

void contact_shadows_caster(long object_index, long child_index, float const center[3], float radius,
	float const shadow_vector[3], float const shadow_color[3], float strength, float pixels)
{
	int slot = contact_shadows.count, index;

	if (radius <= 0.0f || strength <= 0.0f)
		return;
	/* (more than the pass takes: the smallest on screen given up) */
	if (slot >= CONTACT_SHADOWS_CASTERS)
	{
		slot = 0;
		for (index = 1; index < CONTACT_SHADOWS_CASTERS; index++)
		{
			if (contact_shadows.pixels[index] < contact_shadows.pixels[slot])
				slot = index;
		}
		if (contact_shadows.pixels[slot] >= pixels)
			return;
	}
	else
		contact_shadows.count++;
	rt_shadows_pack(&contact_shadows.casters[slot], object_index, child_index, center, radius, shadow_vector,
		shadow_color, strength);
	contact_shadows.pixels[slot] = pixels;
}

void contact_shadows_window(float const camera[3])
{
	if (contact_shadows.count <= 0)
		return;
	memcpy(contact_shadows.camera, camera, sizeof(contact_shadows.camera));
	contact_shadows.taken = 1;
	halo_contact_shadows_light();
}

void contact_shadows_draw(void)
{
	if (!contact_shadows.taken)
		return;
	contact_shadows.taken = 0;
	halo_contact_shadows(contact_shadows.casters, contact_shadows.count, contact_shadows.camera);
}
