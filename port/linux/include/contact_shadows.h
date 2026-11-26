/*
CONTACT_SHADOWS.H

display.contact_shadows (docs/dynamic-shadows-plan.md, stage 2): small,
sharp shadows where the objects meet the level (under their feet, against
a wall), found by the GPU from the depth alone: from each pixel of the
level inside a caster's volume, a short march in screen space toward the
caster's light. The casters, their direction and darkness are the classic
shadows' (render_objects.c chooses them as it does for display.shadows
"ray_traced": rt_shadows.h), the most on screen kept. Any renderer can draw
it; it is on by default, and off wherever display.shadows "ray_traced" is
drawn (whose rays find the same contact).

Render-only: nothing here is read by the simulation, sent or saved.
*/

#ifndef CONTACT_SHADOWS_H
#define CONTACT_SHADOWS_H

#include "rt_shadows.h"

enum
{
	/* the most casters of a window (xgpu_backend.h's XGPU_CONTACT_SHADOWS_CASTERS) */
	CONTACT_SHADOWS_CASTERS = 48,
};

/* display.contact_shadows is on and display.shadows "ray_traced" is not
drawn (read again as the front end takes the settings up between frames) */
int contact_shadows_on(void);

/* a window's casters: begun, one added (as rt_shadows_caster), and the
window's contact shadows drawn (halo_contact_shadows) */
void contact_shadows_begin(void);
void contact_shadows_caster(long object_index, long child_index, float const center[3], float radius,
	float const shadow_vector[3], float const shadow_color[3], float strength, float pixels);
void contact_shadows_window(float const camera[3]);

/* ---------- the front end's (port/linux/src/d3d8_device.c) */

/* the window's casters into the 3D view's level, after its lightmap pass */
void halo_contact_shadows(struct rt_shadow_caster const *casters, int count, float const camera[3]);

#endif
