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
#include <stddef.h>
#include "xgpu_stream.h"

struct xgpu_vulkan_image;

enum
{
	XGPU_VULKAN_LT_OFF,
	XGPU_VULKAN_LT_RAW,
	XGPU_VULKAN_LT_E2,
	XGPU_VULKAN_LT_ACCUMULATE,
	/* "denoise": RAW's 1 spp through the denoiser (xgpu_vulkan_denoise.c, W3),
	whose result the surfaces read */
	XGPU_VULKAN_LT_DENOISE,
};

/* the mode: HALO_LT_E1 where given (read once; debug), else the game's:
DENOISE while it sends its light lists (display.ray_traced_lighting:
xgpu_vulkan_lt_lights_on), OFF until it does and once it says it is off */
int xgpu_vulkan_lt_mode(void);
/* the pass on and its last frame's level lit live by the sky (lt/e1.frag's
LIGHTS_REAL from the sky's records: its lights the whole static light, no
lightmap, the view where it sees the sky), for the camera's contrast
(xgpu_hdr.h's XGPU_HDR_MODE_CAMERA). Not a level lit by its solved light
sources alone (port/assets/emitters): those are fitted to the bake's own
light, which the contrast would bend a second time */
int xgpu_vulkan_lt_live(void);
/* the pre-pass's twins' guides (xgpu_spirv.h's C1), which the pass reads
for the shaded normal */
void xgpu_vulkan_lt_guides(struct xgpu_vulkan_image *guides);
/* the light table's image (NULL none): this frame's rows, then from
lights[2] the frame before's (the denoiser's deterministic gradients,
lt/denoise_gradient.frag); lights[0] and [1] the block's rows of each,
lights[3] the flashlight's cookie's first texel in the cells */
struct xgpu_vulkan_image *xgpu_vulkan_lt_table_halves(int32_t lights[4]);
/* at the pre-pass's end (xgpu_vulkan_stream.c): the pass drawn from its
inputs (the window's target's size), its uniforms and the frame's lights
(the cells as the GPU has them, the block), its light bound where the
surfaces read display.rt_bounce's; nonzero where the mode is on (the bounce
then not drawn) */
int xgpu_vulkan_lt_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass,
	const unsigned char *cells, const struct xgpu_dynamic_lights_block *block);
/* HALO_LT_E1=denoise, for the denoiser's A-SVGF gradients: the pass drawn
into into (RGBA32F, a texel a 3x3 stratum of the previous frame) with this
frame's light but the previous frame's uniforms, inputs and guides (its
surfaces and its seed), each texel at its stratum's gradient pixel
(lt/gradient_pixel.glsl) */
void xgpu_vulkan_lt_reshade(struct xgpu_vulkan_image *into, const struct xgpu_world_ao_pass *previous_pass,
	struct xgpu_vulkan_image *previous_inputs, struct xgpu_vulkan_image *previous_guides);
/* (xgpu_vulkan_lt_kernel and _kernels_generation, for the denoiser: in
xgpu_vulkan_denoise.h, which has Vulkan's types) */
/* debug: the kernels read again, at the pass's next draw, from folder's
<source>.spv files (NULL: HALO_LT_SHADER_DIR's, or the last given), the
built-in ones where a file is not there (the replay's "reload") */
void xgpu_vulkan_lt_shaders_reload(const char *folder);
/* the HALO_LT_* settings' generation, raised by each reload: the pass, the
bounce and the denoiser read their settings again when it changes */
unsigned xgpu_vulkan_lt_settings_generation(void);
/* every image of the light pass's that one frame leaves to the next (the
pass's, its bounce's, its denoiser's; each named, where its pointer is),
into out (at most most): their count. The replay's rt_state (digests) and
checkpoints read them */
struct xgpu_vulkan_lt_image
{
	const char *name;
	struct xgpu_vulkan_image **image;
};
#define XGPU_VULKAN_LT_IMAGES_MOST 64
int xgpu_vulkan_lt_images(struct xgpu_vulkan_lt_image *out, int most);
int xgpu_vulkan_lt_indirect_images(struct xgpu_vulkan_lt_image *out, int most);
int xgpu_vulkan_denoise_images(struct xgpu_vulkan_lt_image *out, int most);
/* the pre-pass's images the pass reads across frames (xgpu_vulkan_stream.c:
the occlusion's history in turn) */
int xgpu_vulkan_stream_lt_images(struct xgpu_vulkan_lt_image *out, int most);
/* the bounce's and the denoiser's CPU histories: into blob (restore 0) or
from it (restore 1); blob NULL: its size */
size_t xgpu_vulkan_lt_indirect_history(void *blob, int restore);
size_t xgpu_vulkan_denoise_history(void *blob, int restore);
/* a reload's beginning again done now (the kernels and the histories), not
at the next draw */
void xgpu_vulkan_lt_indirect_settle(void);
void xgpu_vulkan_denoise_settle(void);
/* the replay's checkpoint (rt_checkpoint): what 1 keep, 2 put back, 3 let
go, at the next frame's pre-pass (any thread, between frames); whether one
is asked still; whether one is kept */
void xgpu_vulkan_lt_checkpoint_ask(int what);
int xgpu_vulkan_lt_checkpoint_pending(void);
int xgpu_vulkan_lt_checkpoint_valid(void);
/* (the stream's, at each frame's pre-pass: what was asked done) */
void xgpu_vulkan_lt_checkpoint_service(void);
/* debug (HALO_LT_DROP=point|spot|panel|flashlight|all|id:<hex>, the
per-kind calibration's "off", in any mode): the frame's block of lights (count of them) as it
goes to the GPU, those of the kind given their colour 0; otherwise
nothing */
void xgpu_vulkan_lt_block_filter(struct xgpu_dynamic_lights_block *block, uint32_t count);

#endif
