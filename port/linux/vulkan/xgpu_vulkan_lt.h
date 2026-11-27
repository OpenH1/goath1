/*
XGPU_VULKAN_LT.H

The one light-transport pass's experiments E1 and E2 (~/dev/rt2-realtime-design.md,
section 7): the flashlight's direct light alone, traced at the pre-pass's end
(xgpu_vulkan_lt.c). HALO_LT_E1 (debug.lt_e1's variable: the backend reads no
config file) chooses it: unset or "off", nothing (Classic and the normal modes
as they are); "raw", one stratified shadow ray a pixel a frame, no history (E1
at 1 spp); "on" (or "e2"), the same through a count-based accumulator (1/n up
to 64 frames) with reprojection rejection (E2); "accumulate", the same up to
2^20 frames, for a held view's comparison with the truth. In every mode the
surfaces found once a pixel leave the view's shadowed flashlight out of the
game's lights and add this pass's light linearly after them
(xgpu_spirv.c's lt_e1_*), at display.rt_bounce's binding (which the mode
takes over).
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_LT_H
#define __HALO_LINUX_XGPU_VULKAN_LT_H

#include "xgpu.h"
#include "xgpu_stream.h"

struct xgpu_vulkan_image;

enum
{
	XGPU_VULKAN_LT_OFF,
	XGPU_VULKAN_LT_RAW,
	XGPU_VULKAN_LT_E2,
	XGPU_VULKAN_LT_ACCUMULATE,
};

/* the mode (HALO_LT_E1, read once) */
int xgpu_vulkan_lt_mode(void);
/* at the pre-pass's end (xgpu_vulkan_stream.c): the pass drawn from its
inputs (the window's target's size), its uniforms and the frame's lights
(the cells as the GPU has them, the block), its light bound where the
surfaces read display.rt_bounce's; nonzero where the mode is on (the bounce
then not drawn) */
int xgpu_vulkan_lt_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const unsigned char *cells, const struct xgpu_dynamic_lights_block *block);
/* debug: the kernels read again, at the pass's next draw, from folder's
<source>.spv files (NULL: HALO_LT_SHADER_DIR's, or the last given), the
built-in ones where a file is not there (the replay's "reload") */
void xgpu_vulkan_lt_shaders_reload(const char *folder);

#endif
