/*
XGPU_METAL_TEMPORAL.H

display.temporal_aa on the Metal backend (xgpu_metal_temporal.m;
docs/temporal-aa.md): the passes of a window's motion (its resolve from the
objects' motion and the depth, debug.motion_vectors' view of it, MetalFX's
reactive mask) and the window's view upscaled by MetalFX's temporal scaler,
as the Vulkan backend's motion passes and FSR (xgpu_vulkan_stream.c,
xgpu_vulkan_fsr.c).
*/

#ifndef __HALO_MACOS_XGPU_METAL_TEMPORAL_H
#define __HALO_MACOS_XGPU_METAL_TEMPORAL_H

#include <stdint.h>

/* the passes' uniforms (the MSL's motion_uniforms): the target's metrics
(1 / width, 1 / height, width, height); the window (x0, y0, width,
height, the target's pixels); the rows of the camera's reprojection and the
motion's values (xgpu_stream.h's struct xgpu_motion_pass: the drawn pixels'
offset, whether the depth is reversed, whether the window has a frame
before) */
struct xgpu_metal_motion_uniforms
{
	float metrics[4];
	float window[4];
	float reproject[4][4];
	float motion[4];
};

/* the passes' functions and the resolve's pipeline (and debug.motion_vectors'
into a target of view_format, XGPU_METAL_FORMAT_*), made on the backend's
queues as a map is prepared; 0 (logged once) if they cannot be */
int xgpu_metal_motion_prepare(uint8_t view_format);
/* the window's motion (into: RGBA16F, the target's size) over the
rectangle (x0, y0, x1, y1) from the objects' (RGBA32F: their motion, their
depth, 1 where drawn) where their depth is still the depth target's, else
the camera's from the depth; and debug.motion_vectors' colours of it drawn
over a target of the format (its colour alone). The backend's state is the
passes' afterwards: the caller sets its own again */
void xgpu_metal_motion_resolve(uint32_t into, uint32_t objects, uint32_t depth, const int32_t rectangle[4],
	const struct xgpu_metal_motion_uniforms *uniforms);
void xgpu_metal_motion_view(uint32_t into, uint8_t format, uint32_t motion, const int32_t rectangle[4],
	const struct xgpu_metal_motion_uniforms *uniforms);
/* display.render_scale's 3D view, no upscaler: a rectangle (x0, y0, x1, y1)
of a colour texture stretched (linear, as the presentation scales a smaller
frame) over a rectangle of another, one sample, of the format
(XGPU_METAL_FORMAT_*: the display's back buffer, where the HUD and menus are
drawn after it). The backend's state is the pass's afterwards */
void xgpu_metal_view_stretch(uint32_t color, const int32_t source[4], uint32_t into, uint8_t format,
	const int32_t place[4]);
/* (the map's preparation) the stretch's pipeline into a target of the format
made, so that its first frame does not miss it */
void xgpu_metal_view_stretch_prepare(uint8_t format);

/* whether the GPU and the system have MetalFX's temporal scaler (macOS 13,
iOS 16 on): 1; 2 on iOS where they have only its spatial one (#1228); 0
neither */
int xgpu_metal_metalfx_supported(void);
/* the window's (0 to 3) colour before its transparent draws (a copy of the
target's texture, of its format and size), for this frame's reactive mask */
void xgpu_metal_metalfx_opaque(int window, uint32_t color);

/* a window's view upscaled (xgpu_stream.h's MOTION _UPSCALE): its colour,
the depth target, its motion (RGBA16F, xgpu_metal_motion_resolve's) over
source (the rectangle in their pixels) into destination's rectangle; the
jitter in the source's pixels (x right, y down); its history begun again
(reset); display.temporal_aa_sharpness (0 to 1, 0 none) */
struct xgpu_metal_metalfx_frame
{
	uint32_t color, depth, motion, destination;
	int32_t source[4], destination_rectangle[4];
	float jitter[2];
	int reset, window;
	float sharpness;
};
/* nonzero if it was drawn; the backend's state is the passes' afterwards */
int xgpu_metal_metalfx_upscale(const struct xgpu_metal_metalfx_frame *frame);
/* MetalFX's output (a target of the rectangle's size) into the
destination's rectangle: copied as it is at sharpness 0, else sharpened
by AMD FSR's RCAS at that sharpness (0 to 1, as FSR's); the backend's state
is the passes' afterwards */
void xgpu_metal_metalfx_place(uint32_t output, uint32_t destination, const int32_t rectangle[4], float sharpness);

#endif
