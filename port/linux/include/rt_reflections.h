/*
RT_REFLECTIONS.H

display.rt_reflections (docs/rt-plan/2-reflections.md): the shiny surfaces'
reflections traced by the GPU's ray queries through the ray-traced scene
(world_rt.h) in place of their cube maps. The rasterizer tags the draws
whose texture 3 is the reflection's cube map (halo_rt_reflection,
port/linux/src/d3d8_device.c), and the backend's pixel shaders trace that
texel alone, the game's combiners (its masks, fresnel and tint) as they
were. Here: the setting, and the frame's scene handed over for the windows
that trace.

Render-only: nothing here is read by the simulation, sent or saved. Off (or
a backend without ray queries) nothing of it runs.
*/

#ifndef RT_REFLECTIONS_H
#define RT_REFLECTIONS_H

/* display.rt_reflections is on and the backend has ray queries (read again
as the front end takes the settings up between frames); said once in the
log where it is asked for and cannot be */
int rt_reflections_on(void);

/* a window about to be drawn (primary: the screen's 3D view, not a mirror's
or the Spartan link's): the frame's ray-traced scene handed over once a
frame, before its surfaces trace it */
void rt_reflections_window(int primary);

#endif
