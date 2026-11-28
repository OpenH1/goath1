/*
RT_LIGHTS_MATERIALS.H

The rt2 truth scene's lights and materials (rt2 scene format, local:
materials.json and lights.json; port/linux/game/rt_lights_materials.c).
*/

#ifndef RT_LIGHTS_MATERIALS_H
#define RT_LIGHTS_MATERIALS_H

/* fill in scene_folder's materials.json (its rows' "index" and "tag", as
the engine export wrote them) from each shader tag, write each texture once
to the textures folder beside the scene (scene_folder/../textures), and
write scene_folder/lights.json: the skies' lights and ambient light, every
light at this tick, every light tag of the map, and what the map's
lightmaps were baked from. Call on the game's thread, with a map loaded */
void rt_lights_materials_write(const char *scene_folder);

/* the debug trigger (HALO_RT2_LIGHTS_TEST=<folder>): once, at
HALO_RT2_LIGHTS_TEST_TICK game ticks (300 by default), a scene folder
<folder>/<map>_lights_test with a materials.json of the BSP's shaders, then
rt_lights_materials_write on it. Called each frame by the lights'
preprocessing (object_lights.c) */
void rt_lights_materials_debug_frame(void);

#endif
