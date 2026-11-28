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
/* a window's (0 to 3) last list: its lights (count of them; NULL none) and
the game's count of lists sent when it was */
const struct xgpu_stream_lt_light *xgpu_vulkan_lt_lights(int window, int *count, uint32_t *frame);
/* the window whose list came last (-1: none yet): the one the pre-pass
after it draws */
int xgpu_vulkan_lt_lights_window(void);

#endif
