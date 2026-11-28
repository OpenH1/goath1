/*
HALO_DYNAMIC_LIGHTS.H

The enhanced dynamic lights (display.dynamic_lights): what the game
(port/linux/game/dynamic_lights.c, source/objects/object_lights.c) hands the
renderer (port/linux/src/d3d8_device.c).
*/

#ifndef HALO_DYNAMIC_LIGHTS_H
#define HALO_DYNAMIC_LIGHTS_H

/* display.dynamic_lights "enhanced" (port/linux/src/d3d8_device.c, the game's
side port/linux/game/dynamic_lights.c): a frame's dynamic light, where it
is drawn, its radius, its color (the game's, 0 to 1) and its cone (the
model lighting's: an axis, scale and offset; omni: 0, 0 and 1) */
struct halo_dynamic_light
{
	float position[3];
	float radius;
	float color[3];
	float cone_offset;
	float axis[3];
	float cone_scale;
	long flags;
};
enum
{
	/* the structure is lit by the game's own pass (a spotlight's gel) */
	HALO_DYNAMIC_LIGHT_MODELS_ONLY = 1,
	/* a glow is drawn about it (a bolt's, a needle's: a projectile with no model) */
	HALO_DYNAMIC_LIGHT_HALO = 2,
	/* a projectile's (debug.dynamic_lights_log) */
	HALO_DYNAMIC_LIGHT_PROJECTILE = 4,
	/* a unit's flashlight (the game's side only: it always casts a
	shadow, dynamic_lights_shadowed) */
	HALO_DYNAMIC_LIGHT_FLASHLIGHT = 8,
	/* a lit panel (a display: light_sources.c), a Lambertian disk facing
	along its axis, its radius squared in cone_offset, its color the
	display's own (xgpu.h's XGPU_DYNAMIC_LIGHTS_PANELS) */
	HALO_DYNAMIC_LIGHT_PANEL = 16,
};
/* the vertex constant (as the game numbers them) a model's draw has the
rasterizer lights it is not lit by in (rasterizer_set_model_lighting: its
object's own lights that say so, render.h's RENDER_LIGHTING_OWN_LIGHT), as
four indices, -1 for none; none of the game's vertex programs reads it.
The per-pixel model lighting has it as its model_lights[12] (xgpu.h) */
#define HALO_DYNAMIC_LIGHTS_OWN_LIGHTS_REGISTER (-57)
/* whether the lights are enhanced, the frame's lights (the camera's
position and its right and up), the structure's lightmap pass drawn, and
the glows drawn into the 3D view */
int halo_dynamic_lights_enhanced(void);
/* ... with display.dynamic_lights chosen "enhanced" (1) or not (0), as the
menus have it before it is kept; -1 as the front end took it up */
int halo_dynamic_lights_enhanced_chosen(int enhanced);
/* whether a map's preparation draws its passes lit by them: enhanced, or
its record made (port/linux/src/d3d8_device.c) */
int halo_dynamic_lights_walked(void);
/* the lights the renderer has room for, the clusters it has a mask of the
lights reaching each for, and the texels of its world, the structure's 3D
BSP (xgpu.h's XGPU_DYNAMIC_LIGHTS_MAXIMUM, _CLUSTERS, _WORLD_TEXELS) */
#define HALO_DYNAMIC_LIGHTS_MAXIMUM 128
#define HALO_DYNAMIC_LIGHTS_CLUSTERS 512
#define HALO_DYNAMIC_LIGHTS_WORLD_TEXELS (240 * 1024)
/* ...; and each cluster's mask of the lights that reach it, four words, a
bit a light (HALO_DYNAMIC_LIGHTS_CLUSTERS of them) */
void halo_dynamic_lights_set(const struct halo_dynamic_light *lights, int count, const float camera[3],
	const float right[3], const float up[3], const unsigned int (*cluster_masks)[4]);
/* display.shadows "ray_traced": the frame's lights that cast shadows (at
most HALO_DYNAMIC_LIGHTS_SHADOWED: each one's index among those
halo_dynamic_lights_set is given next, its disk's radius, world units, and
its owner among the ray-traced scene's instances, world_rt_owner, 0 none),
given before it (count 0: none) and taken by it (xgpu.h's
XGPU_DYNAMIC_LIGHTS_SHADOWED) */
#define HALO_DYNAMIC_LIGHTS_SHADOWED 4
void halo_dynamic_lights_shadowed(const int *indices, const float *disks, const unsigned long *owners, int count);
/* the enhanced flashlight (xgpu.h's XGPU_DYNAMIC_LIGHTS_FLASHLIGHT): the
frame's flashlights, given before halo_dynamic_lights_set and taken by it,
each its light's index among those it is given, its gel's frame as the
game's gel pass has it (its beam, up and left: rasterizer_xbox_environment.c),
and whether its shadow map is traced (one at most), with its owner among the
ray-traced scene's instances (world_rt_owner, which its rays leave out) and
where its shadows are cast from (its lamp on the weapon in hand) */
#define HALO_DYNAMIC_LIGHTS_FLASHLIGHTS 4
struct halo_flashlight
{
	int light;
	float forward[3], up[3], left[3];
	int shadowed;
	unsigned long owner;
	float lamp[3];
};
void halo_dynamic_lights_flashlights(const struct halo_flashlight *flashlights, int count);
/* the gel the flashlights are lit through, and the game's distance
attenuation (each a texture's header, D3DBaseTexture's, as the texture
cache gives it: a cube map, and a volume), made the renderer's cookie and
falloff (xgpu.h); 1 if it could be (the gel decoded), else 0: the
flashlight is then lit as the game lights it */
int halo_dynamic_lights_gel(const void *gel, const void *attenuation);
/* the game's distance attenuation texture (its header; NULL none), every
light's falloff on the structure (xgpu.h's XGPU_DYNAMIC_LIGHTS_ATTENUATION) */
void halo_dynamic_lights_attenuation(const void *attenuation);
/* the structure's 3D BSP, from which a pixel finds its cluster (xgpu.h's
world: count texels of four words, count at most
HALO_DYNAMIC_LIGHTS_WORLD_TEXELS), when it changes; copied */
void halo_dynamic_lights_world(const unsigned int (*texels)[4], int count);
void halo_dynamic_lights_environment(int drawing);
void halo_dynamic_lights_halos_draw(void);

#endif
