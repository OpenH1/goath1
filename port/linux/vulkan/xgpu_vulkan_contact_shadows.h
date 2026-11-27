/*
XGPU_VULKAN_CONTACT_SHADOWS.H

display.contact_shadows in the Vulkan backend (xgpu_backend.h's
xgpu_backend_contact_shadows; docs/dynamic-shadows-plan.md, stage 2): the
pass's pipeline, sampler and block, which xgpu_vulkan_stream.c's
stream_contact_shadows draws with over a window's 3D view after its
lightmap pass.
*/

#ifndef XGPU_VULKAN_CONTACT_SHADOWS_H
#define XGPU_VULKAN_CONTACT_SHADOWS_H

#include "xgpu_vulkan.h"

struct xgpu_stream_rt_shadows;

/* the most casters the pass's block holds (tools/vulkan_post_shaders.py's
CONTACT_SHADOWS' CASTERS; xgpu_backend.h's XGPU_CONTACT_SHADOWS_CASTERS) */
#define XGPU_VULKAN_CONTACT_SHADOWS_CASTERS 48
/* the block's size: 16 vec4s of header, then two a caster */
#define XGPU_VULKAN_CONTACT_SHADOWS_BLOCK ((16 + 2 * XGPU_VULKAN_CONTACT_SHADOWS_CASTERS) * 16)

/* the pass's pipeline into targets of these formats and samples (0: none
can be, said once in the log) */
int xgpu_vulkan_contact_shadows_pipeline(uint8_t color_format, uint8_t depth_format, uint8_t samples,
	struct xgpu_vulkan_pipeline_description *pipeline);
/* the depth's sampler (by texel), and the light's (the view before the
classic receivers: the pass's second texture) */
VkSampler xgpu_vulkan_contact_shadows_sampler(void);
/* the pass's block (XGPU_VULKAN_CONTACT_SHADOWS_BLOCK bytes) of a record,
drawn with that viewport and depth range; 0 if the view has no inverse */
int xgpu_vulkan_contact_shadows_block(const struct xgpu_stream_rt_shadows *record,
	const struct xgpu_vulkan_dynamic_state *dynamic, void *block);
/* (xgpu_vulkan_stream.c's, the ray-traced shadows' pass's too) the
framebuffer's (x, y, depth, 1), times w, of a world point as the game's
programs place it under a record's view, drawn with that viewport and depth
range (rows), into project (NULL: not wanted), and its inverse into
unproject; 0 if it has none */
int xgpu_vulkan_shadows_window(const struct xgpu_stream_rt_shadows *record,
	const struct xgpu_vulkan_dynamic_state *dynamic, float project[4][4], float unproject[4][4]);

#endif
