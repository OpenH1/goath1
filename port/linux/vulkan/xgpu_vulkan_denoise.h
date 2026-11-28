/*
XGPU_VULKAN_DENOISE.H

The light-transport pass's one denoising and temporal stage (W3,
~/dev/rt2-realtime-design.md section 1.5; xgpu_vulkan_denoise.c): the noisy
1-spp diffuse irradiance (contract C2: HALO_LT_E1=denoise, E1's raw output)
in, the denoised irradiance out (C3: what the surfaces read at
display.rt_bounce's binding). Render thread alone.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_DENOISE_H
#define __HALO_LINUX_XGPU_VULKAN_DENOISE_H

#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

/* this frame's noisy irradiance (raw: RGBA32F, rgb the 1-spp sample) and
the last frame's (NULL where there is none), the pre-pass's inputs (this
frame's, the last's) and guides (xgpu_spirv.h's C1), its uniforms and the
rectangle drawn: the denoised irradiance (RGBA16F, rgb; a the estimate's
standard deviation), or NULL where it cannot be had yet (the raw then bound) */
/* the dynamic lights' bounce folded into the diffuse channel's one temporal
stage (xgpu_vulkan_lt_indirect.c's, HALO_LT_E1=denoise): its half-resolution
samples (lt/indirect.glsl's BOUNCE: a texel a 2x2 block), the phase its
blocks were traced at this frame, whether the direct light is left out
(HALO_LT_INDIRECT=only, checking) */
struct xgpu_vulkan_denoise_bounce
{
	struct xgpu_vulkan_image *image;
	int phase, only;
};
struct xgpu_vulkan_image *xgpu_vulkan_denoise_draw(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *previous_raw, struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *previous_inputs,
	struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass, const int32_t rectangle[4],
	const struct xgpu_vulkan_denoise_bounce *bounce);

/* the light the surfaces read, drawn by the denoiser (xgpu_vulkan_denoise_
frame): its image (RGBA32F; with wide, twice as wide: the right half the
reflections'), the suns (their count, each one's forward and power) whose
whole direct share comes off the structure's light, the reflections' noisy
radiance and hits' share (NULL: none, the right half cleared), whether the
diffuse's denoised irradiance is wanted too (a dump); out: whether it was
drawn (else the caller's own final passes) and the reflections' denoised
radiance */
struct xgpu_vulkan_denoise_final
{
	struct xgpu_vulkan_image *final;
	int wide, suns;
	float sun[16][4];
	/* (a level lit live: no suns' share off, the light's alpha 1, the
	surfaces' lightmap and model lighting dropped: xgpu_spirv.c's lt_e1_add) */
	int real;
	struct xgpu_vulkan_image *specular, *coverage;
	int result;
	int drawn;
	struct xgpu_vulkan_image *specular_denoised;
};
/* both channels (the diffuse's as xgpu_vulkan_denoise_draw's, the
specular's from out's) through their temporal stages, then the spatial
stage's passes shared and the light the surfaces read drawn into out's
final; the diffuse's denoised irradiance where out wants it (else NULL). In
the checking modes, or before its passes are made, the diffuse's alone, as
xgpu_vulkan_denoise_draw (out's drawn 0) */
struct xgpu_vulkan_image *xgpu_vulkan_denoise_frame(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *previous_raw, struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *previous_inputs,
	struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass, const int32_t rectangle[4],
	const struct xgpu_vulkan_denoise_bounce *bounce, struct xgpu_vulkan_denoise_final *out);

/* the specular channel (the light transport's reflections,
xgpu_vulkan_lt_indirect.c): this frame's noisy specular radiance (RGBA,
rgb; a the hit's distance, 0 a miss, less than 0 a pixel without specular)
through the same stages, its own history (reprojected from the
reflection's virtual point, the count 16 to 32 frames by roughness), no
gradients; the denoised radiance (RGBA16F), or NULL */
struct xgpu_vulkan_image *xgpu_vulkan_denoise_specular_draw(struct xgpu_vulkan_image *raw,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *guides, const struct xgpu_world_ao_pass *pass,
	const int32_t rectangle[4]);

/* its passes' pipelines asked for (made apart from the draws), with the
light transport's own */
void xgpu_vulkan_denoise_prepare(void);

/* (debug, HALO_LT_DENOISE_PROBE=1: the workbench's views) of the last
frame drawn, after the work recorded so far (waits for the GPU), into
*halves (malloc'd, the caller's) as RGBA half floats: which 0, the diffuse
temporal stage's probe, an image four windows wide (*width each, *height),
each pixel's four windows' fields as lt/denoise_temporal.frag's PROBE
writes them; 1, the diffuse sample that stage took (the bounce folded in;
a the bounce's count); 2, the denoised diffuse irradiance (the a-trous
passes' last). 1 on success, 0 none drawn (or not half floats) */
int xgpu_vulkan_denoise_probe_read(int which, uint16_t **halves, uint32_t *width, uint32_t *height);

/* (xgpu_vulkan_lt.c's, for the denoiser) a light-transport kernel by its
source's name (lt/<source>: the built-in SPIR-V, or HALO_LT_SHADER_DIR's or
a reload's), none where it is not one; the generation goes up at each reload
(the kernels made again then) */
VkShaderModule xgpu_vulkan_lt_kernel(const char *source);
unsigned xgpu_vulkan_lt_kernels_generation(void);

#endif
