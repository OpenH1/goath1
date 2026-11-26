/*
GPU_CHOOSE_TEST.C

The GPU the game draws with (xgpu_vulkan_gpu.c, #1269) over made-up device
lists: a desktop's RTX beside a Ryzen's integrated Radeon listed first, a
laptop whose display is its integrated GPU's, two discrete GPUs of which
one drives the monitor, memory between equals, a device that cannot be
used, and display.gpu's index, name and fallbacks.
*/

#include "../xgpu_vulkan_gpu.c"
#include <assert.h>

#define GB (1024ull * 1024 * 1024)

static int choose(const struct xgpu_gpu *gpus, int count, const char *setting, const char *why_has)
{
	char why[256];
	int chosen = xgpu_gpu_choose(gpus, count, setting, why, sizeof(why));

	printf("%s: %d (%s)\n", setting ? setting : "(none)", chosen, why);
	assert(!why_has || strstr(why, why_has));
	return chosen;
}

int main(void)
{
	/* #1269's: the integrated Radeon listed first, the RTX driving the
	monitor */
	struct xgpu_gpu desktop[] = {
		{ "AMD Radeon(TM) Graphics", XGPU_GPU_INTEGRATED, 1, 0, 512ull << 20 },
		{ "NVIDIA GeForce RTX 5070", XGPU_GPU_DISCRETE, 1, 1, 12 * GB },
	};
	/* a laptop: the panel is the integrated GPU's */
	struct xgpu_gpu laptop[] = {
		{ "Intel(R) Iris(R) Xe Graphics", XGPU_GPU_INTEGRATED, 1, 1, 2 * GB },
		{ "NVIDIA GeForce RTX 4060 Laptop GPU", XGPU_GPU_DISCRETE, 1, 0, 8 * GB },
	};
	/* two discrete GPUs: the monitor on the second, the smaller */
	struct xgpu_gpu two[] = {
		{ "NVIDIA GeForce RTX 3090", XGPU_GPU_DISCRETE, 1, 0, 24 * GB },
		{ "AMD Radeon RX 6600", XGPU_GPU_DISCRETE, 1, 1, 8 * GB },
	};
	/* equals but for memory; and a processor's beside them */
	struct xgpu_gpu memory[] = {
		{ "llvmpipe (LLVM 19.1.0, 256 bits)", XGPU_GPU_CPU, 1, 0, 64 * GB },
		{ "AMD Radeon RX 6600", XGPU_GPU_DISCRETE, 1, 0, 8 * GB },
		{ "AMD Radeon RX 7900 XTX", XGPU_GPU_DISCRETE, 1, 0, 24 * GB },
	};
	/* the RTX unusable (below Vulkan 1.3, or no queue that presents) */
	struct xgpu_gpu unusable[] = {
		{ "AMD Radeon(TM) Graphics", XGPU_GPU_INTEGRATED, 1, 0, 512ull << 20 },
		{ "NVIDIA GeForce GTX 680", XGPU_GPU_DISCRETE, 0, 1, 2 * GB },
	};
	struct xgpu_gpu none[] = {
		{ "NVIDIA GeForce GTX 680", XGPU_GPU_DISCRETE, 0, 1, 2 * GB },
	};

	assert(choose(desktop, 2, NULL, "auto: discrete before integrated") == 1);
	assert(choose(desktop, 2, "auto", NULL) == 1);
	assert(choose(desktop, 2, "", NULL) == 1);
	assert(choose(laptop, 2, "auto", "discrete before integrated") == 1);
	assert(choose(two, 2, "auto", "drives the window's display") == 1);
	assert(choose(memory, 3, "auto", "most device-local memory") == 2);
	assert(choose(unusable, 2, "auto", "the only usable device") == 0);
	assert(choose(none, 1, "auto", "no usable device") == -1);
	assert(choose(none, 0, "auto", "no usable device") == -1);

	/* display.gpu: an index, a part of a name in any case, the best of
	several that have it; else auto */
	assert(choose(desktop, 2, "0", "display.gpu 0") == 0);
	assert(choose(desktop, 2, "radeon", "display.gpu \"radeon\"") == 0);
	assert(choose(desktop, 2, "rtx 5070", NULL) == 1);
	assert(choose(memory, 3, "AMD", NULL) == 2);
	assert(choose(memory, 3, "llvmpipe", NULL) == 0);
	assert(choose(desktop, 2, "7", "display.gpu 7 is no usable device; auto: discrete") == 1);
	assert(choose(desktop, 2, "-1", "is no usable device") == 1);
	assert(choose(unusable, 2, "1", "is no usable device; auto: the only usable") == 0);
	assert(choose(unusable, 2, "gtx", "names no usable device") == 0);
	assert(choose(desktop, 2, "intel", "names no usable device; auto: discrete") == 1);
	puts("gpu choice: ok");
	return 0;
}
