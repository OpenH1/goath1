/*
XGPU_VULKAN_DLSS.H

display.temporal_aa "dlss" in the Vulkan backend (docs/temporal-aa.md):
NVIDIA DLSS Super Resolution (DLAA at the native scale), through NVIDIA's
NGX on Vulkan (Linux; Windows's the same calls). And DLSS Ray
Reconstruction (NGX's DLSS-D, docs/rt-plan/4-path-tracing.md, "Denoising"):
on RTX, the ray-traced passes' denoiser and upscaler in one.

The game itself holds none of NVIDIA's code. Its module beside the
executable (libopenh1_dlss.so, openh1_dlss.dll: port/linux/vulkan/dlss/
openh1_dlss.c, built by configure.py --dlss with the DLSS SDK's NGX library
and its DLSS runtime, libnvidia-ngx-dlss.so / nvngx_dlss.dll, staged beside
it) is loaded at run time (xgpu_vulkan_dlss.c). Without the module, an
NVIDIA GPU or its driver's NGX, nothing changes: no extension, no feature,
and the setting is not offered. Render thread alone, as the core.

The module's interface below (OPENH1_DLSS_MODULE's part: the openh1_dlss
structures and calls) may also be used under the MIT license, as
openh1_dlss.c is, which includes it (the DLSS add-on: tools/dlss_addon.py).
*/

#ifndef XGPU_VULKAN_DLSS_H
#define XGPU_VULKAN_DLSS_H

#ifndef OPENH1_DLSS_MODULE
#include "xgpu_vulkan_fsr.h"
#else
#include <vulkan/vulkan_core.h>
#endif

#include <stdint.h>

/* ---------- the module's calls (its one export, openh1_dlss) */

/* 2: Ray Reconstruction's calls after the first ones (a module of ABI 1
still gives Super Resolution) */
#define OPENH1_DLSS_ABI 2

/* an image the feature reads or writes (in the GENERAL layout): its view (a
depth image's depth alone), size and format */
struct openh1_dlss_image
{
	VkImage image;
	VkImageView view;
	VkFormat format;
	uint32_t width, height;
	VkImageAspectFlags aspect;
};

/* a window's feature: its render size (the most it is given), its output
size; display.hdr's linear light (else an SDR picture); its render preset
(0 NGX's own) */
struct openh1_dlss_create
{
	uint32_t render[2], output[2];
	int hdr, preset;
};

/* a frame: the colour, the depth (reversed), the motion (the frame before's
place less this one's, scaled by motion_scale into the render's pixels),
the output (the whole of it, the output size); the window's corner in the
inputs and its size; the jitter in its pixels; the history begun again */
struct openh1_dlss_frame
{
	struct openh1_dlss_image color, depth, motion, output;
	uint32_t corner[2], render[2];
	float jitter[2], motion_scale[2];
	int reset;
};

/* (ABI 2) a window's Ray Reconstruction feature: its render size, its
output size, its render preset (0 NGX's own). The colour is linear light
(HDR), the depth reversed, the roughness in the normals' w */
struct openh1_dlss_rr_create
{
	uint32_t render[2], output[2];
	int preset;
};

/* (ABI 2) a Ray Reconstruction frame: the noisy colour (the lit picture,
linear), the diffuse and specular albedo, the world's normals with the
roughness in w, the depth (reversed), the motion (as Super Resolution's),
the specular rays' hit distance (its first channel; no view: none), the
output; the window's corner in the inputs and its size; the jitter; the
camera's matrices, world to view and view to clip, 16 floats each with the
translation in elements 12 to 14 (NGX's row vectors: a column-major matrix
of column vectors, Vulkan's, as it is); the history begun again */
struct openh1_dlss_rr_frame
{
	struct openh1_dlss_image color, diffuse_albedo, specular_albedo, normal_roughness, depth, motion,
		specular_hit_distance, output;
	uint32_t corner[2], render[2];
	float jitter[2], motion_scale[2];
	float world_to_view[16], view_to_clip[16];
	int reset;
};

struct openh1_dlss
{
	uint32_t abi;
	/* the instance's and the device's extensions NGX needs (each a name the
	module keeps); their count, at most capacity */
	uint32_t (*instance_extensions)(const char **names, uint32_t capacity);
	uint32_t (*device_extensions)(VkInstance instance, VkPhysicalDevice physical,
		PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **names, uint32_t capacity);
	/* NGX begun on the device; 1 where DLSS Super Resolution is available.
	data_folder: where NGX may write (its logs); log: a line of NGX's (with
	HALO_DLSS_LOG) or the module's own */
	int (*initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
		PFN_vkGetInstanceProcAddr get_instance_proc_addr, PFN_vkGetDeviceProcAddr get_device_proc_addr,
		const char *data_folder, void (*log)(int warning, const char *line));
	/* a window's feature made (its work recorded in commands); NULL if not */
	void *(*feature_create)(VkCommandBuffer commands, const struct openh1_dlss_create *create);
	/* a frame's work recorded in commands; 1 done */
	int (*feature_evaluate)(VkCommandBuffer commands, void *feature, const struct openh1_dlss_frame *frame);
	/* (the work that used it done) */
	void (*feature_release)(void *feature);
	/* (the device idle, every feature released) */
	void (*shutdown)(VkDevice device);
	/* ABI 2. After initialize: 1 where Ray Reconstruction is available, and
	the version of its runtime NGX loaded (or "unknown") in version */
	int (*rr_available)(char *version, uint32_t capacity);
	/* a window's Ray Reconstruction feature made (released by
	feature_release); NULL if not */
	void *(*rr_create)(VkCommandBuffer commands, const struct openh1_dlss_rr_create *create);
	/* a frame's work recorded in commands; 1 done */
	int (*rr_evaluate)(VkCommandBuffer commands, void *feature, const struct openh1_dlss_rr_frame *frame);
};

typedef const struct openh1_dlss *(*openh1_dlss_entry)(void);

#ifndef OPENH1_DLSS_MODULE
/* ---------- the backend's (xgpu_vulkan.c) */

/* the module loaded (not on Android or Apple, nor with
HALO_VULKAN_NO_DLSS); 1 if it was. Then the extensions it asks for, those
the instance or the device has, added to names (their count, at most
capacity: not already there) */
int xgpu_vulkan_dlss_load(void);
uint32_t xgpu_vulkan_dlss_instance_extensions(const char **names, uint32_t count, uint32_t capacity,
	int (*available)(const char *name));
uint32_t xgpu_vulkan_dlss_device_extensions(VkInstance instance, VkPhysicalDevice physical,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **names, uint32_t count, uint32_t capacity,
	int (*available)(const char *name));
/* NGX begun on the device made with them: 1 where DLSS is available */
struct xgpu_vulkan_dlss_device
{
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	PFN_vkGetInstanceProcAddr get_instance_proc_addr;
	PFN_vkGetDeviceProcAddr get_device_proc_addr;
	VkPhysicalDeviceMemoryProperties memory;
	const char *data_folder;
};
int xgpu_vulkan_dlss_create(const struct xgpu_vulkan_dlss_device *device);
/* (the device idle) */
void xgpu_vulkan_dlss_destroy(void);
/* the window's view upscaled into its destination (the same frame as
FSR's); 0 if it could not be (then scaled as it is) */
int xgpu_vulkan_dlss_upscale(int window, const struct xgpu_vulkan_fsr_frame *frame);

/* ---------- Ray Reconstruction (the ray-traced passes' denoiser on RTX) */

/* a window's ray-traced frame, the shared denoiser's inputs
(xgpu_vulkan_denoise.h): the noisy diffuse and specular radiance (RGBA16F;
the specular's alpha its rays' hit distance), demodulated (divided by
their albedo) when demodulated is set; the diffuse and specular albedo; the
world's normals with the roughness in w; the depth (reversed); the motion
(xgpu_stream.h's MOTION: a fraction of the view); their rectangle (left,
top, right, bottom), the same in each, all at the render's size; the
jitter in its pixels; the camera, world to view and view to clip
(column-major, Vulkan's); the history begun again. The output: the target
and its rectangle, which Ray Reconstruction denoises and upscales into */
struct xgpu_vulkan_dlss_rr_frame
{
	struct xgpu_vulkan_image *diffuse, *specular, *diffuse_albedo, *specular_albedo, *normal_roughness, *depth,
		*motion;
	int32_t source[4];
	int demodulated;
	float jitter[2];
	float world_to_view[16], view_to_clip[16];
	int reset;
	struct xgpu_vulkan_image *destination;
	int32_t destination_rectangle[4];
};

/* 1 where Ray Reconstruction is available (the add-on's module of ABI 2,
an RTX GPU, its runtime found): the ray-traced passes then give it their
frame instead of the denoiser and the upscaler */
int xgpu_vulkan_dlss_rr_available(void);
/* the window's ray-traced frame denoised and upscaled into its
destination; 0 if it could not be (nothing written: the denoiser's turn) */
int xgpu_vulkan_dlss_reconstruct(int window, const struct xgpu_vulkan_dlss_rr_frame *frame);

/* ---------- the light-transport pass's denoiser on RTX (xgpu_vulkan_lt.c;
~/dev/rt2-realtime-design.md section 2) */

/* the pass's cameras (xgpu_stream.h's struct xgpu_world_ao_pass's camera,
previous and previous_corners: position, forward, right, up; this frame's
and the last's, and the last's corners) */
struct xgpu_vulkan_dlss_lt_cameras
{
	float camera[4][4], previous[4][4];
	int32_t previous_corners[4];
};

/* 1 where HALO_LT_DENOISER=rr asks for Ray Reconstruction and it is
available: the pass's light then goes to xgpu_vulkan_dlss_lt_denoise and
never to our denoiser (one temporal stage) */
int xgpu_vulkan_dlss_lt_selected(void);
/* the pass's 1-spp light (RGBA32F, its rgb the demodulated diffuse
irradiance) denoised by Ray Reconstruction at the pass's rectangle, from
the pre-pass's inputs and guides (xgpu_spirv.h's C1; NULL none) and the
pass's cameras; reset: the history begun again. The result (RGBA16F, the
inputs' size; its rgb the irradiance) is the context's, kept until the next
call; NULL where there is none this frame (the feature's first submission
not done yet, or it failed) */
struct xgpu_vulkan_image *xgpu_vulkan_dlss_lt_denoise(struct xgpu_vulkan_image *light,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *guides, const struct xgpu_vulkan_dlss_lt_cameras *pass,
	const int32_t rectangle[4], int reset);
#endif

#endif
