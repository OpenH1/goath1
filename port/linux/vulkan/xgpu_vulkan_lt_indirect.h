/*
XGPU_VULKAN_LT_INDIRECT.H

The light-transport pass's indirect light and reflections from the dynamic
lights (~/dev/rt2-realtime-design.md sections 1.3 and 1.4;
xgpu_vulkan_lt_indirect.c), drawn by xgpu_vulkan_lt.c after its direct
light: the bounce at half resolution with the radiance cache, added to the
direct light's diffuse signal; the reflections' GGX rays, their own
(specular) history, read by the surfaces' reflection stage. HALO_LT_INDIRECT
(unset or "on": both; "0": neither; "only": the indirect alone in the light,
checking; "nocache": the bounce without the cache) and HALO_LT_SPECULAR ("0":
no reflections) choose. Render thread alone.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_LT_INDIRECT_H
#define __HALO_LINUX_XGPU_VULKAN_LT_INDIRECT_H

#include "xgpu_stream.h"

#include <stddef.h>
#include <stdint.h>

struct xgpu_vulkan_image;

/* a frame's light transport as the indirect light reads it */
struct xgpu_vulkan_lt_indirect_frame
{
	/* the pre-pass's inputs (this frame's, the last's), its guides, the
	light table */
	struct xgpu_vulkan_image *inputs, *previous_inputs, *guides, *table;
	/* the direct light's draw this frame (its 1-spp sample, or its mean) */
	struct xgpu_vulkan_image *direct;
	const struct xgpu_world_ao_pass *pass;
	/* the direct light's kernel's parameters (lt/e1.frag's, at 1024 of
	the uniforms), their size, where their control (ivec4) is in them */
	const void *params;
	size_t params_size, control_offset;
	const int32_t *rectangle;
	/* XGPU_VULKAN_LT_* (xgpu_vulkan_lt.h); the count's cap; the count
	started again; the last frame's inputs had */
	int mode, cap, start, previous_valid;
	/* the bounce folded into the denoiser's one temporal stage (ours runs:
	"denoise" without RR): no history, upsampling or sum of its own here,
	the draw's diffuse signal the direct light's alone; the denoiser takes
	the bounce from xgpu_vulkan_lt_indirect_bounce */
	int fold;
	/* a level lit live (lt/e1.frag's LIGHTS_REAL): the cache warmed where it
	starts again (INDIRECT_WARM passes) */
	int live;
};

/* whether the indirect light and the reflections are drawn (the settings,
read once); the light the surfaces read is then twice as wide, its right
half the reflections' (xgpu_vulkan_lt_indirect_final) */
int xgpu_vulkan_lt_indirect_on(void);
/* the frame's indirect light and reflections: the diffuse light the
denoiser (or, accumulated, the surfaces) then takes (the direct and the
indirect, one signal), or NULL where there is none (the direct as it is) */
struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_draw(const struct xgpu_vulkan_lt_indirect_frame *frame);
/* this frame's bounce (half resolution: lt/indirect.glsl's BOUNCE), the
phase its blocks were traced at, whether the direct light is to be left out
(HALO_LT_INDIRECT=only); NULL where none was drawn */
struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_bounce(int *phase, int *only);
/* this frame's reflections' noisy radiance (their rays: rgb, a the hit's
distance, less than 0 none) and their hits' share (counted), for the
denoiser; NULL where they are not drawn */
struct xgpu_vulkan_image *xgpu_vulkan_lt_indirect_specular(struct xgpu_vulkan_image **coverage);
/* the light the surfaces read drawn by the denoiser instead of
xgpu_vulkan_lt_indirect_final, and its reflections' denoised radiance (for
HALO_LT_SPECULAR_DUMP) */
void xgpu_vulkan_lt_indirect_finished(struct xgpu_vulkan_image *final, struct xgpu_vulkan_image *specular);
/* the reflections' denoising (HALO_LT_E1=denoise, after the diffuse's) and
their part of the light the surfaces read: final's right half */
void xgpu_vulkan_lt_indirect_final(const struct xgpu_vulkan_lt_indirect_frame *frame, struct xgpu_vulkan_image *final);
/* HALO_LT_E1_DUMP's frame since (with HALO_LT_SPECULAR_DUMP): the
reflections' light as lt_spec_<since>.rgba16f (the E1 dumps' form: rgb the
radiance, a the hits' share) */
void xgpu_vulkan_lt_indirect_dump(const struct xgpu_vulkan_lt_indirect_frame *frame, unsigned long since);

#endif
