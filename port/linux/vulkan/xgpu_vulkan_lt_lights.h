/*
XGPU_VULKAN_LT_LIGHTS.H

The light-transport pass's light list as the backend receives it
(xgpu_stream.h's LT_LIGHTS, from port/linux/game/lt_lights.c): kept, each
window's last, for the pass to read (xgpu_vulkan_lt_lights.c). No shading
here. Render thread alone.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_LT_LIGHTS_H
#define __HALO_LINUX_XGPU_VULKAN_LT_LIGHTS_H

#include "xgpu_stream.h"

/* the stream's LT_LIGHTS (host_render_stream.h's lt_lights), its count,
window and lights checked by the decoder */
void xgpu_vulkan_lt_lights_receive(void *context, const struct xgpu_stream_lt_lights *record);
/* the same for a list in the host's memory (count records for window, the
game's count of lists sent): the replay's light-list unit's (port/tools/
replay, HALO_LT_UNIT), which builds the list again from its inputs */
void xgpu_vulkan_lt_lights_take(const struct xgpu_stream_lt_light *lights, uint32_t count, int window, uint32_t frame);
/* a window's (0 to 3) last list: its lights (count of them; NULL none) and
the game's count of lists sent when it was */
const struct xgpu_stream_lt_light *xgpu_vulkan_lt_lights(int window, int *count, uint32_t *frame);
/* the window whose list came last (-1: none yet): the one the pre-pass
after it draws */
int xgpu_vulkan_lt_lights_window(void);
/* whether the game has the light-transport pass on (display.ray_traced_lighting):
a list received since its last window -1 (the pass turned off) */
int xgpu_vulkan_lt_lights_on(void);
/* everything received let go, as at the stream's start (a replay played
from its start again: xgpu_vulkan_lt_shaders_reload); between frames */
void xgpu_vulkan_lt_lights_reset(void);

/* the level's static light sources (port/linux/game/lt_emitters.c), the set
the lists last carried whole: each triangle its corners (wound to glow
toward their normal), its group's radiance (linear) and its group; count of
them, groups of groups, whether the set is solved from the lightmap (the
level lit live), and its number (a new one each time a set is whole); NULL
(0s) none */
#define XGPU_VULKAN_LT_EMITTERS_MAXIMUM 8192
struct xgpu_vulkan_lt_emitter
{
	float corners[9], radiance[3];
	int group;
};
const struct xgpu_vulkan_lt_emitter *xgpu_vulkan_lt_emitters(int *count, int *groups, int *solved, int *version);

/* a window's (0 to 3) last list's object lamps (port/linux/game/world_rt.c's
world_rt_lamps: an object's self-illuminated triangles where they glow,
this frame's place): each its corners (wound to glow toward their normal),
radiance (linear), its object's ray-traced owner and whether it glows
toward both sides (an additive part drawn from both); count of them */
#define XGPU_VULKAN_LT_OBJECT_LAMPS_MAXIMUM 512
struct xgpu_vulkan_lt_object_lamp
{
	float corners[9], radiance[3];
	uint32_t owner, two_sided, additive;
};
const struct xgpu_vulkan_lt_object_lamp *xgpu_vulkan_lt_object_lamps(int window, int *count);

#endif
