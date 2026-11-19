/*
XGPU_VULKAN_FSR.H

display.temporal_aa "fsr" in the Vulkan backend (docs/temporal-aa.md): AMD
FidelityFX Super Resolution 3.1's upscaler (port/third_party/fidelityfx-fsr3),
its compute passes dispatched here (xgpu_vulkan_fsr.c), its host side ported
to C from the SDK's ffx_fsr3upscaler.cpp. Render thread alone, as the core.
*/

#ifndef XGPU_VULKAN_FSR_H
#define XGPU_VULKAN_FSR_H

#include "xgpu_vulkan.h"

/* the windows whose history is kept apart (split screen's four) */
#define XGPU_VULKAN_FSR_WINDOWS 4

struct xgpu_vulkan_fsr_frame
{
	/* the window's 3D view as drawn: its target's colour (before the tone
	map), depth and motion (xgpu_stream.h's MOTION: RGBA16F, the target's
	size), and its rectangle in them (left, top, right, bottom) */
	struct xgpu_vulkan_image *color, *depth, *motion;
	int32_t source[4];
	/* the target the view is upscaled into (the window's at the display's
	scale), and its rectangle there */
	struct xgpu_vulkan_image *destination;
	int32_t destination_rectangle[4];
	/* the frame's jitter in the source's pixels (x right, y down) */
	float jitter[2];
	/* the camera: its near and far planes (world units), its vertical
	field of view (radians), and the meters in a world unit */
	float near_plane, far_plane, field_of_view, meters;
	/* the history begun again (a cut, a new map, a resize); the colour is
	display.hdr's linear light (else an SDR picture) */
	int reset, hdr;
};

/* the passes' pipelines made (as a map is prepared with it on), for an SDR
and an HDR colour both */
void xgpu_vulkan_fsr_prepare(void);
/* the window's colour before its transparent draws (particles, effects,
glass, water, lens flares), over its rectangle: the reactive mask is where
they changed it (the SDK's ffxFsr3UpscalerContextGenerateReactiveMask) */
void xgpu_vulkan_fsr_opaque(int window, struct xgpu_vulkan_image *color, const int32_t rectangle[4]);
/* the window's view upscaled into its destination; 0 if it could not be
(nothing written) */
int xgpu_vulkan_fsr_upscale(int window, const struct xgpu_vulkan_fsr_frame *frame);

#endif
