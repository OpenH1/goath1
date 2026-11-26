/* Which GPU the game draws with (#1269): the devices the Vulkan loader
lists, ranked (xgpu_vulkan_gpu.c). No Vulkan here, so that the rule can be
tested over made-up device lists (tests/gpu_choose_test.c). */
#ifndef XGPU_VULKAN_GPU_H
#define XGPU_VULKAN_GPU_H

#include <stddef.h>
#include <stdint.h>

/* VkPhysicalDeviceType's values */
#define XGPU_GPU_OTHER 0
#define XGPU_GPU_INTEGRATED 1
#define XGPU_GPU_DISCRETE 2
#define XGPU_GPU_VIRTUAL 3
#define XGPU_GPU_CPU 4

struct xgpu_gpu
{
	const char *name;
	/* XGPU_GPU_* */
	int type;
	/* Vulkan 1.3, with a graphics queue that presents to the window */
	int usable;
	/* the adapter that drives the window's monitor (Windows: its DXGI
	output's adapter's LUID is the device's) */
	int drives_display;
	/* its device-local memory, bytes */
	uint64_t local_memory;
};

/* "discrete", "integrated" and so on */
const char *xgpu_gpu_type_name(int type);

/* The device to draw with, an index into gpus, or -1 if none is usable.
setting is display.gpu: "auto" (or NULL, empty) ranks the usable devices:
a discrete GPU before an integrated one (a laptop's display is its
integrated GPU's, while its discrete one draws faster and Windows copies
its frames over), then the one that drives the window's display, then the
one with the most device-local memory, then the first listed. A number is
an index into the list; other text the best usable device whose name has
it (any case: "nvidia", "RTX 5070"); a setting no usable device answers to
falls back to auto. why is told the reason, for the log. */
int xgpu_gpu_choose(const struct xgpu_gpu *gpus, int count, const char *setting, char *why, size_t why_size);

#endif
