/*
RT_SHADOWS.H

display.shadows "ray_traced" (docs/world-ao.md, "Ray-traced shadows"): the
objects' shadows on the level found by the GPU's ray queries through their
collision meshes (the scene display.ambient_occlusion "ray_traced" builds:
world_rt.h), in place of the classic path's caster drawn into the shadow
atlas, its blur, the level's surfaces found under it and drawn again
(render_objects.c's render_object_shadows, rasterizer_xbox_shadows.c,
structure_render_shadow).

The same casters as the classic path (render_objects.c's render_object: a
drawn object not the first-person camera's, nor shadowless, large and dark
enough), each with the classic shadow's direction (its lighting's
shadow_vector: the lightmap's dominant light, at least 45 degrees down) and
darkness (1 - its shadow_color, scaled by its size, darkness and
camouflage), over the same volume (its bounding sphere's radius to each
side, half of it toward the light and four times it away, fading toward
both ends), on the same receivers (the level's surfaces: the backend's pass
leaves the models' pixels, their stencil, as the classic receivers do).

Render-only: nothing here is read by the simulation, sent or saved. Off
("classic", or a backend without ray queries) nothing of it runs.
*/

#ifndef RT_SHADOWS_H
#define RT_SHADOWS_H

/* ---------- constants */

enum
{
	/* the most casters of a window (xgpu_backend.h's XGPU_RT_SHADOWS_CASTERS) */
	RT_SHADOWS_CASTERS = 96,
};

/* ---------- structures */

/* a caster as the backend's pass reads it (xgpu_backend.h's struct
xgpu_rt_shadow_caster): its bounding sphere; its shadow's direction's x and
y (its z, down, from them); its darkness, 1 - the shadow's colour scaled by
its strength, a byte each (red lowest); its owners among the ray-traced
scene's instances (world_rt_owner), 16 bits each: itself, and its first
child (a vehicle's rider), 0 none */
struct rt_shadow_caster
{
	float sphere[4];
	float direction[2];
	unsigned int darkness, owners;
};

/* ---------- prototypes (port/linux/game/rt_shadows.c) */

/* display.shadows is "ray_traced" and the backend has ray queries (read
again as the front end takes the settings up between frames); said once
in the log where it is asked for and cannot be */
int rt_shadows_on(void);

/* a window's casters: begun (none), one added (an object's datum handle,
its first child's (-1 none), its bounding sphere, its lighting's shadow
vector and colour, the strength the classic shadow is drawn at, and its
size on screen, which of more than RT_SHADOWS_CASTERS keeps the largest),
and the window's shadows drawn (the ray-traced scene built for the frame
and the casters handed to the renderer: halo_rt_shadows) */
void rt_shadows_begin(void);
void rt_shadows_caster(long object_index, long child_index, float const center[3], float radius,
	float const shadow_vector[3], float const shadow_color[3], float strength, float pixels);
void rt_shadows_window(float const camera[3]);

/* (measuring, HALO_SHADOW_TIMES: the game thread's time in a window's shadows, either
path: render_object_shadows entered, TRUE, and left) */
void rt_shadows_measure(int entering);

/* the arithmetic: a caster packed (strength 0 to 1) */
void rt_shadows_pack(struct rt_shadow_caster *caster, long object_index, long child_index, float const center[3],
	float radius, float const shadow_vector[3], float const shadow_color[3], float strength);

/* ---------- the front end's (port/linux/src/d3d8_device.c) */

/* the window's casters into the 3D view's level, after its lightmap pass
(count 0: none, every pixel lit) */
void halo_rt_shadows(struct rt_shadow_caster const *casters, int count, float const camera[3]);

#endif
