/*
XGPU_VULKAN_DLSS.C

display.temporal_aa "dlss" in the Vulkan backend (docs/temporal-aa.md,
xgpu_vulkan_dlss.h): NVIDIA DLSS Super Resolution through the module beside
the executable, which alone holds NVIDIA's NGX library.

- Loaded before the instance is made: the instance's and the device's
  extensions NGX asks for are added (those they have), and only then; NGX
  begun once the device is made, on an NVIDIA GPU alone. The module is the
  DLSS add-on's, in the player's add-ons folder (dlss_addon_folder.h: the
  game downloads it, port/linux/src/dlss_addon.c), else a build's beside
  the executable (configure.py --dlss). Installed while the game runs, it
  is loaded at the next start: NGX's extensions are the instance's and
  the device's from their making, which the backend does not make again.
- Each window (split screen's four) has its own feature, made for its
  render size and output size, and made again when they change. The
  inputs are FSR's (xgpu_vulkan_fsr.h's frame): the colour, the depth
  (reversed), the motion (a fraction of the view: scaled by the render's
  size into its pixels), the jitter and the reset, read in place (their
  corner given). The output is the module's own RGBA16F image, blitted into
  the destination's rectangle.
- On the core's command buffer between wide barriers, as FSR's work.
- Ray Reconstruction (the module's ABI 2, xgpu_vulkan_dlss_reconstruct):
  a window's ray-traced frame, the shared denoiser's inputs. A compute
  pass (tools/vulkan_dlss_shaders.py) makes its one noisy colour (the
  diffuse and specular radiance, remodulated by their albedo) and the
  specular hit distance alone, into the window's own images at the inputs'
  size and texels; the feature reads them and the guides in place and
  writes its RGBA16F output, blitted into the destination's rectangle.
  HALO_DLSS_RR_SMOKE=1 (with display.temporal_aa "dlss") runs it once on a
  synthetic noisy frame and logs whether it was made, recorded and wrote a
  finite picture.
- The light-transport pass's denoiser on RTX (xgpu_vulkan_dlss_lt_denoise,
  HALO_LT_E1=denoise with HALO_LT_DENOISER=rr; ~/dev/rt2-realtime-design.md
  section 2): Ray Reconstruction in the pass's one temporal stage's place,
  ours (xgpu_vulkan_denoise.c) then not run at all. A compute pass
  (tools/vulkan_dlss_shaders.py's LT_GATHER) makes its inputs from the pass's
  1-spp irradiance (demodulated: the albedos given are 1 and 0, so that what
  it denoises is the light alone, which the surfaces then take times their
  own albedo, C3), the pre-pass's inputs and the guides (C1: the shaded
  normal, the roughness, the motion where they carry it, else the cameras'
  reprojection); its depth and matrices are the pass's camera's own. It runs
  at the pass's size (its output the render's: the surfaces read the light
  at their texels; the picture's upscaling stays the temporal technique's),
  before any transparent is drawn (none is in its inputs: its bias mask has
  nothing to flag). The result, an RGBA16F image of the pass's size, is what
  the pass's sun share and the surfaces read. "DENOISE (DLSS RR)" in
  debug.pass_times.

Render thread alone, as the core.
*/

#include "xgpu_vulkan_dlss.h"
#include "host.h"
#include "host_load_trace.h"
#include "dlss_addon_folder.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__) && !defined(__APPLE__)
#include <dlfcn.h>
#endif

#define GARBAGE 16
/* (Ray Reconstruction's composition: descriptor pools in turn, each a
submission's) */
#define RR_POOLS 4
#define RR_POOL_SETS 8

struct dlss_output
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t width, height;
	VkFormat format;
};

/* a window's Ray Reconstruction: its feature, sizes (the render's, the
output's, the inputs' images'), the composed colour and hit distance (the
inputs' size), the output */
struct dlss_rr_context
{
	void *feature;
	uint32_t render[2], output[2], inputs[2];
	struct dlss_output color, hit_distance, output_image;
};

struct dlss_context
{
	void *feature;
	uint32_t render[2], output[2];
	int hdr;
	struct dlss_output output_image;
};

/* the light-transport pass's Ray Reconstruction (xgpu_vulkan_dlss_lt_denoise):
its feature, the render's size (the pass's rectangle) and the images' (the
pass's inputs'), its inputs as the gather writes them, the albedos (1, 0),
its output, and the result the pass is given (the inputs' size); the
submission that made the feature (its frames only once it is done: NGX's),
and whether its first frame is still to come */
struct dlss_lt_context
{
	void *feature;
	uint64_t made_serial;
	int fresh;
	uint32_t render[2], size[2];
	struct dlss_output color, normal_roughness, depth, motion, diffuse_albedo, specular_albedo, output_image;
	struct xgpu_vulkan_image *result;
};

static struct
{
	const struct openh1_dlss *calls;
	int ready, failed_said;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	struct dlss_context contexts[XGPU_VULKAN_FSR_WINDOWS];
	/* Ray Reconstruction: available, its windows (and the smoke test's,
	the last), the composition's pipeline */
	int rr_ready, rr_failed_said;
	char rr_version[64];
	struct dlss_rr_context rr_contexts[XGPU_VULKAN_FSR_WINDOWS + 1];
	VkSampler point;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout pipeline_layout;
	VkPipeline compose;
	/* the light-transport pass's: its gather's set layout, pipeline layout
	and pipeline; its context; HALO_LT_DENOISER read (-1 not yet) */
	VkDescriptorSetLayout lt_set_layout;
	VkPipelineLayout lt_pipeline_layout;
	VkPipeline lt_gather;
	struct dlss_lt_context lt;
	int lt_selected, lt_failed_said, lt_said;
	struct
	{
		VkDescriptorPool pool;
		uint64_t serial;
	} pools[RR_POOLS];
	int pool;
	/* features and images let go of, released once the submission that
	last used them is done */
	struct
	{
		void *feature;
		struct dlss_output images[3];
		uint64_t serial;
	} garbage[GARBAGE];
	uint32_t garbage_count;
	double recording;
	uint32_t recorded;
#define DLSS_FUNCTIONS(F) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) F(vkCmdPipelineBarrier) F(vkCmdBlitImage) \
	F(vkCreateSampler) F(vkDestroySampler) F(vkCreateShaderModule) F(vkDestroyShaderModule) \
	F(vkCreateDescriptorSetLayout) F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout) \
	F(vkDestroyPipelineLayout) F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool) \
	F(vkDestroyDescriptorPool) F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) \
	F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdPushConstants) F(vkCmdDispatch) \
	F(vkCmdClearColorImage) F(vkCmdClearDepthStencilImage) F(vkCmdCopyBufferToImage) F(vkCmdCopyImageToBuffer) \
	F(vkCreateBuffer) F(vkDestroyBuffer) F(vkGetBufferMemoryRequirements) F(vkBindBufferMemory) F(vkMapMemory) \
	F(vkUnmapMemory)
#define DECLARE(name) PFN_##name name;
	DLSS_FUNCTIONS(DECLARE)
#undef DECLARE
} dlss;

#include "xgpu_vulkan_dlss_shaders.inc"

static void rr_setup(void);
static void rr_teardown(void);
static void lt_setup(void);
static void lt_release(void);
static void smoke_resources_free(void);

/* ---------- the module */

#if !defined(__ANDROID__) && !defined(__APPLE__)
/* the module at path (NULL: the one beside the executable) loaded: its
entry, NULL if it could not be */
static openh1_dlss_entry module_open(const char *path)
{
#ifdef _WIN32
	HMODULE library;

	if (path)
	{
		wchar_t wide[1200];

		/* (the DLSS runtime beside it found there: the module's own folder,
		openh1_dlss.c's) */
		if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, (int)(sizeof(wide) / sizeof(wide[0]))))
			return NULL;
		library = LoadLibraryExW(wide, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	}
	else
		library = LoadLibraryExW(L"openh1_dlss.dll", NULL,
			LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
	return library ? (openh1_dlss_entry)(void *)GetProcAddress(library, "openh1_dlss") : NULL;
#else
	/* (beside the executable: its rpath's $ORIGIN) */
	void *library = dlopen(path ? path : "libopenh1_dlss.so", RTLD_NOW | RTLD_LOCAL);

	if (!library && path)
		host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"dlss\": the DLSS add-on's module did not load (%s)",
			dlerror());
	return library ? (openh1_dlss_entry)dlsym(library, "openh1_dlss") : NULL;
#endif
}
#endif

int xgpu_vulkan_dlss_load(void)
{
#if defined(__ANDROID__) || defined(__APPLE__)
	return 0;
#else
	openh1_dlss_entry entry = NULL;

	if (dlss.calls)
		return 1;
	if (getenv("HALO_VULKAN_NO_DLSS"))
		return 0;
	/* (the add-on's, unless it is to be removed: dlss_addon_folder.h) */
	{
		char folder[1024], path[1200], mark[1200];
		FILE *file;

		if (dlss_addon_folder(folder, sizeof(folder)))
		{
			snprintf(path, sizeof(path), "%s" DLSS_ADDON_SEPARATOR DLSS_ADDON_NAME DLSS_ADDON_SEPARATOR
				DLSS_ADDON_MODULE, folder);
			snprintf(mark, sizeof(mark), "%s" DLSS_ADDON_SEPARATOR DLSS_ADDON_NAME DLSS_ADDON_SEPARATOR
				DLSS_ADDON_REMOVE_MARK, folder);
			if ((file = fopen(mark, "rb")) != NULL)
			{
				fclose(file);
				path[0] = 0;
			}
			else if ((file = fopen(path, "rb")) != NULL)
				fclose(file);
			else
				path[0] = 0;
			if (path[0])
				entry = module_open(path);
			if (entry)
				host_logf(HOST_LOG_INFO, "vulkan: display.temporal_aa \"dlss\": the DLSS add-on's module, %s", path);
		}
	}
	/* (a build's, beside the executable) */
	if (!entry)
		entry = module_open(NULL);
	if (!entry)
		return 0;
	dlss.calls = entry();
	/* (an older module, ABI 1, still gives Super Resolution: its calls are
	the first ones) */
	if (!dlss.calls || dlss.calls->abi < 1 || dlss.calls->abi > OPENH1_DLSS_ABI)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"dlss\": the DLSS module is of another build");
		dlss.calls = NULL;
		return 0;
	}
	return 1;
#endif
}

/* (names not already there that the instance or device has) */
static uint32_t names_add(const char **wanted, uint32_t wanted_count, const char **names, uint32_t count,
	uint32_t capacity, int (*available)(const char *name), const char *what)
{
	uint32_t index, have;

	for (index = 0; index < wanted_count && count < capacity; index++)
	{
		for (have = 0; have < count && strcmp(names[have], wanted[index]); have++)
		{
		}
		if (have < count)
			continue;
		if (!available(wanted[index]))
		{
			host_logf(HOST_LOG_INFO, "vulkan: display.temporal_aa \"dlss\": the %s has no %s", what, wanted[index]);
			continue;
		}
		names[count++] = wanted[index];
	}
	return count;
}

uint32_t xgpu_vulkan_dlss_instance_extensions(const char **names, uint32_t count, uint32_t capacity,
	int (*available)(const char *name))
{
	const char *wanted[16];
	uint32_t wanted_count;

	if (!dlss.calls)
		return count;
	wanted_count = dlss.calls->instance_extensions(wanted, 16);
	return names_add(wanted, wanted_count, names, count, capacity, available, "instance");
}

uint32_t xgpu_vulkan_dlss_device_extensions(VkInstance instance, VkPhysicalDevice physical,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **names, uint32_t count, uint32_t capacity,
	int (*available)(const char *name))
{
	const char *wanted[16];
	uint32_t wanted_count;

	if (!dlss.calls)
		return count;
	wanted_count = dlss.calls->device_extensions(instance, physical, get_instance_proc_addr, wanted, 16);
	return names_add(wanted, wanted_count, names, count, capacity, available, "device");
}

static void module_log(int warning, const char *line)
{
	host_logf(warning ? HOST_LOG_WARN : HOST_LOG_INFO, "vulkan: display.temporal_aa \"dlss\": %s", line);
}

int xgpu_vulkan_dlss_create(const struct xgpu_vulkan_dlss_device *device)
{
	const struct openh1_dlss *calls = dlss.calls;

	memset(&dlss, 0, sizeof(dlss));
	dlss.calls = calls;
	if (!calls)
		return 0;
	dlss.device = device->device;
	dlss.memory = device->memory;
#define LOAD(name) dlss.name = (PFN_##name)device->get_device_proc_addr(device->device, #name); \
	if (!dlss.name) return 0;
	DLSS_FUNCTIONS(LOAD)
#undef LOAD
	if (!calls->initialize(device->instance, device->physical, device->device, device->get_instance_proc_addr,
		device->get_device_proc_addr, device->data_folder, module_log))
		return 0;
	dlss.ready = 1;
	rr_setup();
	return 1;
}

/* ---------- the output images */

static void output_free(struct dlss_output *image)
{
	if (image->view)
		dlss.vkDestroyImageView(dlss.device, image->view, NULL);
	if (image->image)
		dlss.vkDestroyImage(dlss.device, image->image, NULL);
	if (image->memory)
		dlss.vkFreeMemory(dlss.device, image->memory, NULL);
	memset(image, 0, sizeof(*image));
}

static void garbage_collect(int all)
{
	uint64_t completed = all ? (uint64_t)-1 : xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < dlss.garbage_count)
	{
		if (dlss.garbage[index].serial <= completed)
		{
			if (dlss.garbage[index].feature && dlss.calls)
				dlss.calls->feature_release(dlss.garbage[index].feature);
			output_free(&dlss.garbage[index].images[0]);
			output_free(&dlss.garbage[index].images[1]);
			output_free(&dlss.garbage[index].images[2]);
			dlss.garbage[index] = dlss.garbage[--dlss.garbage_count];
		}
		else
		{
			index++;
		}
	}
}

/* (a feature and its images let go of, once the work that used them is
done) */
static void garbage_add(void *feature, const struct dlss_output *first, const struct dlss_output *second,
	const struct dlss_output *third)
{
	if (!feature && !first->image && (!second || !second->image) && (!third || !third->image))
		return;
	if (dlss.garbage_count == GARBAGE)
	{
		xgpu_vulkan_wait(dlss.garbage[0].serial);
		garbage_collect(0);
	}
	if (dlss.garbage_count < GARBAGE)
	{
		memset(&dlss.garbage[dlss.garbage_count], 0, sizeof(dlss.garbage[0]));
		dlss.garbage[dlss.garbage_count].feature = feature;
		dlss.garbage[dlss.garbage_count].images[0] = *first;
		if (second)
			dlss.garbage[dlss.garbage_count].images[1] = *second;
		if (third)
			dlss.garbage[dlss.garbage_count].images[2] = *third;
		dlss.garbage[dlss.garbage_count].serial = xgpu_vulkan_serial();
		dlss.garbage_count++;
	}
}

/* (a window's feature and image let go of) */
static void context_release(struct dlss_context *context)
{
	garbage_add(context->feature, &context->output_image, NULL, NULL);
	memset(context, 0, sizeof(*context));
}

/* an image of format the feature or the composition writes (storage) and
a blit or the feature reads, into the GENERAL layout with the work
recorded */
static int image_make(VkCommandBuffer commands, struct dlss_output *image, uint32_t width, uint32_t height,
	VkFormat format)
{
	VkImageCreateInfo info;
	VkImageViewCreateInfo view;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageMemoryBarrier transition;
	uint32_t type;

	memset(image, 0, sizeof(*image));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (dlss.vkCreateImage(dlss.device, &info, NULL, &image->image) != VK_SUCCESS)
	{
		image->image = VK_NULL_HANDLE;
		return 0;
	}
	dlss.vkGetImageMemoryRequirements(dlss.device, image->image, &requirements);
	for (type = 0; type < dlss.memory.memoryTypeCount; type++)
	{
		if ((requirements.memoryTypeBits & (1u << type)) &&
			(dlss.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			break;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (type == dlss.memory.memoryTypeCount ||
		dlss.vkAllocateMemory(dlss.device, &allocate, NULL, &image->memory) != VK_SUCCESS ||
		dlss.vkBindImageMemory(dlss.device, image->image, image->memory, 0) != VK_SUCCESS)
	{
		output_free(image);
		return 0;
	}
	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = image->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = info.format;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	if (dlss.vkCreateImageView(dlss.device, &view, NULL, &image->view) != VK_SUCCESS)
	{
		image->view = VK_NULL_HANDLE;
		output_free(image);
		return 0;
	}
	image->width = width;
	image->height = height;
	image->format = format;
	memset(&transition, 0, sizeof(transition));
	transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	transition.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.image = image->image;
	transition.subresourceRange = view.subresourceRange;
	dlss.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
		NULL, 0, NULL, 1, &transition);
	return 1;
}

/* ---------- a frame */

static void blit(VkCommandBuffer commands, VkImage from, const int32_t source[4], VkImage to,
	const int32_t destination[4], VkFilter filter)
{
	VkImageBlit region;

	memset(&region, 0, sizeof(region));
	region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
	region.srcOffsets[0].x = source[0];
	region.srcOffsets[0].y = source[1];
	region.srcOffsets[1].x = source[2];
	region.srcOffsets[1].y = source[3];
	region.srcOffsets[1].z = 1;
	region.dstOffsets[0].x = destination[0];
	region.dstOffsets[0].y = destination[1];
	region.dstOffsets[1].x = destination[2];
	region.dstOffsets[1].y = destination[3];
	region.dstOffsets[1].z = 1;
	dlss.vkCmdBlitImage(commands, from, VK_IMAGE_LAYOUT_GENERAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region, filter);
}

/* (a frame DLSS cannot do: the view scaled into the destination as it is,
linearly, rather than nothing) */
static void fallback_blit(const struct xgpu_vulkan_fsr_frame *frame)
{
	struct xgpu_vulkan_image *read[1], *written[1];
	VkCommandBuffer commands;

	if (!dlss.vkCmdBlitImage || frame->color == frame->destination)
		return;
	read[0] = frame->color;
	written[0] = frame->destination;
	commands = xgpu_vulkan_outside_begin(read, 1, written, 1);
	blit(commands, xgpu_vulkan_image_handle(frame->color), frame->source, xgpu_vulkan_image_handle(frame->destination),
		frame->destination_rectangle, VK_FILTER_LINEAR);
	xgpu_vulkan_outside_end(commands);
}

static struct openh1_dlss_image image_of(struct xgpu_vulkan_image *image, VkImageAspectFlags aspect)
{
	struct openh1_dlss_image made;

	made.image = xgpu_vulkan_image_handle(image);
	made.view = xgpu_vulkan_image_view(image);
	made.format = xgpu_vulkan_image_format(image);
	made.width = xgpu_vulkan_image_description(image)->width;
	made.height = xgpu_vulkan_image_description(image)->height;
	made.aspect = aspect;
	return made;
}

static int upscale(int window, const struct xgpu_vulkan_fsr_frame *frame)
{
	struct dlss_context *context = window >= 0 && window < XGPU_VULKAN_FSR_WINDOWS ? &dlss.contexts[window] : NULL;
	const struct xgpu_vulkan_image_description *source, *destination;
	struct xgpu_vulkan_image *read[3], *written[1];
	struct openh1_dlss_frame evaluate;
	VkCommandBuffer commands;
	VkMemoryBarrier memory;
	uint32_t render[2], output[2];
	int32_t whole[4];
	int hdr = frame->hdr != 0, ok;

	if (!dlss.ready || !context || !frame->color || !frame->depth || !frame->motion || !frame->destination)
		return 0;
	source = xgpu_vulkan_image_description(frame->color);
	destination = xgpu_vulkan_image_description(frame->destination);
	if (frame->source[2] - frame->source[0] < 8 || frame->source[3] - frame->source[1] < 8 ||
		frame->destination_rectangle[2] - frame->destination_rectangle[0] < 8 ||
		frame->destination_rectangle[3] - frame->destination_rectangle[1] < 8 || frame->source[0] < 0 ||
		frame->source[1] < 0 || frame->source[2] > (int32_t)source->width || frame->source[3] > (int32_t)source->height ||
		frame->destination_rectangle[0] < 0 || frame->destination_rectangle[1] < 0 ||
		frame->destination_rectangle[2] > (int32_t)destination->width ||
		frame->destination_rectangle[3] > (int32_t)destination->height ||
		xgpu_vulkan_image_description(frame->motion)->width != source->width ||
		xgpu_vulkan_image_description(frame->motion)->height != source->height ||
		xgpu_vulkan_image_description(frame->depth)->width != source->width ||
		xgpu_vulkan_image_description(frame->depth)->height != source->height)
		return 0;
	render[0] = (uint32_t)(frame->source[2] - frame->source[0]);
	render[1] = (uint32_t)(frame->source[3] - frame->source[1]);
	output[0] = (uint32_t)(frame->destination_rectangle[2] - frame->destination_rectangle[0]);
	output[1] = (uint32_t)(frame->destination_rectangle[3] - frame->destination_rectangle[1]);
	garbage_collect(0);
	xgpu_vulkan_pass_owner("DLSS");
	read[0] = frame->color;
	read[1] = frame->depth;
	read[2] = frame->motion;
	written[0] = frame->destination;
	commands = xgpu_vulkan_outside_begin(read, 3, written, 1);
	if (!context->feature || context->render[0] != render[0] || context->render[1] != render[1] ||
		context->output[0] != output[0] || context->output[1] != output[1] || context->hdr != hdr)
	{
		struct openh1_dlss_create create;

		context_release(context);
		memset(&create, 0, sizeof(create));
		create.render[0] = render[0];
		create.render[1] = render[1];
		create.output[0] = output[0];
		create.output[1] = output[1];
		create.hdr = hdr;
		if (!image_make(commands, &context->output_image, output[0], output[1], VK_FORMAT_R16G16B16A16_SFLOAT) ||
			!(context->feature = dlss.calls->feature_create(commands, &create)))
		{
			if (!dlss.failed_said)
				host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"dlss\": a window's feature cannot be made");
			dlss.failed_said = 1;
			context_release(context);
			xgpu_vulkan_outside_end(commands);
			xgpu_vulkan_pass_owner(NULL);
			return 0;
		}
		memcpy(context->render, render, sizeof(render));
		memcpy(context->output, output, sizeof(output));
		context->hdr = hdr;
	}
	memset(&evaluate, 0, sizeof(evaluate));
	evaluate.color = image_of(frame->color, VK_IMAGE_ASPECT_COLOR_BIT);
	evaluate.depth = image_of(frame->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
	evaluate.motion = image_of(frame->motion, VK_IMAGE_ASPECT_COLOR_BIT);
	evaluate.output.image = context->output_image.image;
	evaluate.output.view = context->output_image.view;
	evaluate.output.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	evaluate.output.width = output[0];
	evaluate.output.height = output[1];
	evaluate.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	evaluate.corner[0] = (uint32_t)frame->source[0];
	evaluate.corner[1] = (uint32_t)frame->source[1];
	memcpy(evaluate.render, render, sizeof(render));
	evaluate.jitter[0] = frame->jitter[0];
	evaluate.jitter[1] = frame->jitter[1];
	/* (the motion a fraction of the view: the render's pixels) */
	evaluate.motion_scale[0] = (float)render[0];
	evaluate.motion_scale[1] = (float)render[1];
	evaluate.reset = frame->reset != 0;
	ok = dlss.calls->feature_evaluate(commands, context->feature, &evaluate);
	if (ok)
	{
		memset(&memory, 0, sizeof(memory));
		memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		dlss.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
			&memory, 0, NULL, 0, NULL);
		whole[0] = whole[1] = 0;
		whole[2] = (int32_t)output[0];
		whole[3] = (int32_t)output[1];
		blit(commands, context->output_image.image, whole, xgpu_vulkan_image_handle(frame->destination),
			frame->destination_rectangle, VK_FILTER_NEAREST);
	}
	else if (!dlss.failed_said)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"dlss\": a frame cannot be recorded");
		dlss.failed_said = 1;
	}
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	return ok;
}

/* ---------- Ray Reconstruction */

/* the composition's sampler, set layout (the sampler immutable), pipeline
layout and pipeline, made where the module has Ray Reconstruction */
static void rr_setup(void)
{
	VkSamplerCreateInfo sampler;
	VkDescriptorSetLayoutBinding bindings[6];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkShaderModuleCreateInfo module_info;
	VkComputePipelineCreateInfo info;
	VkShaderModule shader = VK_NULL_HANDLE;
	VkResult result;
	uint32_t index;

	if (dlss.calls->abi < 2 || !dlss.calls->rr_available(dlss.rr_version, sizeof(dlss.rr_version)))
	{
		host_logf(HOST_LOG_INFO, "vulkan: DLSS Ray Reconstruction: not available (%s)",
			dlss.calls->abi < 2 ? "the add-on's module is older" : "the GPU, its driver or the runtime");
		return;
	}
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	if (dlss.vkCreateSampler(dlss.device, &sampler, NULL, &dlss.point) != VK_SUCCESS)
	{
		dlss.point = VK_NULL_HANDLE;
		goto failed;
	}
	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < 6; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = index < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bindings[index].pImmutableSamplers = index < 4 ? &dlss.point : NULL;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = 6;
	set_info.pBindings = bindings;
	if (dlss.vkCreateDescriptorSetLayout(dlss.device, &set_info, NULL, &dlss.set_layout) != VK_SUCCESS)
	{
		dlss.set_layout = VK_NULL_HANDLE;
		goto failed;
	}
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.offset = 0;
	push.size = 5 * sizeof(int32_t);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &dlss.set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (dlss.vkCreatePipelineLayout(dlss.device, &layout_info, NULL, &dlss.pipeline_layout) != VK_SUCCESS)
	{
		dlss.pipeline_layout = VK_NULL_HANDLE;
		goto failed;
	}
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(dlss_compose_spirv);
	module_info.pCode = dlss_compose_spirv;
	if (dlss.vkCreateShaderModule(dlss.device, &module_info, NULL, &shader) != VK_SUCCESS)
		goto failed;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = shader;
	info.stage.pName = "main";
	info.layout = dlss.pipeline_layout;
	result = dlss.vkCreateComputePipelines(dlss.device, VK_NULL_HANDLE, 1, &info, NULL, &dlss.compose);
	dlss.vkDestroyShaderModule(dlss.device, shader, NULL);
	if (result != VK_SUCCESS)
	{
		dlss.compose = VK_NULL_HANDLE;
		goto failed;
	}
	dlss.rr_ready = 1;
	host_logf(HOST_LOG_INFO, "vulkan: DLSS Ray Reconstruction: available, its runtime %s", dlss.rr_version);
	lt_setup();
	return;
failed:
	host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: its composition pass could not be made");
	rr_teardown();
}

static void rr_context_release(struct dlss_rr_context *context)
{
	garbage_add(context->feature, &context->color, &context->hit_distance, &context->output_image);
	memset(context, 0, sizeof(*context));
}

/* (the device idle) */
static void rr_teardown(void)
{
	int index;

	for (index = 0; index <= XGPU_VULKAN_FSR_WINDOWS; index++)
		rr_context_release(&dlss.rr_contexts[index]);
	lt_release();
	if (dlss.lt_gather)
		dlss.vkDestroyPipeline(dlss.device, dlss.lt_gather, NULL);
	if (dlss.lt_pipeline_layout)
		dlss.vkDestroyPipelineLayout(dlss.device, dlss.lt_pipeline_layout, NULL);
	if (dlss.lt_set_layout)
		dlss.vkDestroyDescriptorSetLayout(dlss.device, dlss.lt_set_layout, NULL);
	dlss.lt_gather = VK_NULL_HANDLE;
	dlss.lt_pipeline_layout = VK_NULL_HANDLE;
	dlss.lt_set_layout = VK_NULL_HANDLE;
	smoke_resources_free();
	for (index = 0; index < RR_POOLS; index++)
	{
		if (dlss.pools[index].pool)
			dlss.vkDestroyDescriptorPool(dlss.device, dlss.pools[index].pool, NULL);
	}
	memset(dlss.pools, 0, sizeof(dlss.pools));
	if (dlss.compose)
		dlss.vkDestroyPipeline(dlss.device, dlss.compose, NULL);
	if (dlss.pipeline_layout)
		dlss.vkDestroyPipelineLayout(dlss.device, dlss.pipeline_layout, NULL);
	if (dlss.set_layout)
		dlss.vkDestroyDescriptorSetLayout(dlss.device, dlss.set_layout, NULL);
	if (dlss.point)
		dlss.vkDestroySampler(dlss.device, dlss.point, NULL);
	dlss.compose = VK_NULL_HANDLE;
	dlss.pipeline_layout = VK_NULL_HANDLE;
	dlss.set_layout = VK_NULL_HANDLE;
	dlss.point = VK_NULL_HANDLE;
	dlss.rr_ready = 0;
}

/* a set for the submission being recorded (its pool reset once the GPU is
done with the submission it last served) */
static VkDescriptorSet rr_set(VkDescriptorSetLayout layout)
{
	uint64_t serial = xgpu_vulkan_serial(), completed = xgpu_vulkan_completed();
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSet set = VK_NULL_HANDLE;
	int attempt, found = -1;

	if (dlss.pools[dlss.pool].pool && dlss.pools[dlss.pool].serial == serial)
		found = dlss.pool;
	for (attempt = 0; found < 0 && attempt < RR_POOLS; attempt++)
	{
		int index = (dlss.pool + 1 + attempt) % RR_POOLS;

		if (!dlss.pools[index].pool)
		{
			VkDescriptorPoolSize sizes[2];
			VkDescriptorPoolCreateInfo info;

			sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			sizes[0].descriptorCount = RR_POOL_SETS * 4;
			sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			sizes[1].descriptorCount = RR_POOL_SETS * 4;
			memset(&info, 0, sizeof(info));
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			info.maxSets = RR_POOL_SETS;
			info.poolSizeCount = 2;
			info.pPoolSizes = sizes;
			if (dlss.vkCreateDescriptorPool(dlss.device, &info, NULL, &dlss.pools[index].pool) != VK_SUCCESS)
			{
				dlss.pools[index].pool = VK_NULL_HANDLE;
				return VK_NULL_HANDLE;
			}
		}
		else if (dlss.pools[index].serial > completed)
			continue;
		else
			dlss.vkResetDescriptorPool(dlss.device, dlss.pools[index].pool, 0);
		dlss.pools[index].serial = serial;
		dlss.pool = found = index;
	}
	if (found < 0)
		return VK_NULL_HANDLE;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = dlss.pools[found].pool;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &layout;
	if (dlss.vkAllocateDescriptorSets(dlss.device, &allocate, &set) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return set;
}

/* what a Ray Reconstruction frame reads, as the module's images */
struct rr_inputs
{
	struct openh1_dlss_image diffuse, specular, diffuse_albedo, specular_albedo, normal_roughness, depth, motion;
	int32_t source[4];
	uint32_t output[2];
	int demodulated, reset;
	/* (the context made, nothing recorded: the smoke test's first frame) */
	int create_only;
	float jitter[2];
	const float *world_to_view, *view_to_clip;
};

static int rr_barrier(VkCommandBuffer commands, VkPipelineStageFlags from, VkAccessFlags written,
	VkPipelineStageFlags to, VkAccessFlags read)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = written;
	memory.dstAccessMask = read;
	dlss.vkCmdPipelineBarrier(commands, from, to, 0, 1, &memory, 0, NULL, 0, NULL);
	return 1;
}

/* the composition and the feature's frame recorded in commands, the
context (re)made for the sizes; the output in context->output_image. 1
done */
static int rr_record(VkCommandBuffer commands, struct dlss_rr_context *context, const struct rr_inputs *in)
{
	VkDescriptorImageInfo images[6];
	VkWriteDescriptorSet writes[6];
	struct openh1_dlss_rr_frame frame;
	VkDescriptorSet set;
	uint32_t render[2], index;
	int32_t push[5];

	render[0] = (uint32_t)(in->source[2] - in->source[0]);
	render[1] = (uint32_t)(in->source[3] - in->source[1]);
	if (!context->feature || context->render[0] != render[0] || context->render[1] != render[1] ||
		context->output[0] != in->output[0] || context->output[1] != in->output[1] ||
		context->inputs[0] != in->diffuse.width || context->inputs[1] != in->diffuse.height)
	{
		struct openh1_dlss_rr_create create;

		rr_context_release(context);
		memset(&create, 0, sizeof(create));
		memcpy(create.render, render, sizeof(render));
		memcpy(create.output, in->output, sizeof(create.output));
		if (!image_make(commands, &context->color, in->diffuse.width, in->diffuse.height,
				VK_FORMAT_R16G16B16A16_SFLOAT) ||
			!image_make(commands, &context->hit_distance, in->diffuse.width, in->diffuse.height,
				VK_FORMAT_R32_SFLOAT) ||
			!image_make(commands, &context->output_image, in->output[0], in->output[1],
				VK_FORMAT_R16G16B16A16_SFLOAT) ||
			!(context->feature = dlss.calls->rr_create(commands, &create)))
		{
			if (!dlss.rr_failed_said)
				host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: a window's feature cannot be made");
			dlss.rr_failed_said = 1;
			rr_context_release(context);
			return 0;
		}
		memcpy(context->render, render, sizeof(render));
		memcpy(context->output, in->output, sizeof(context->output));
		context->inputs[0] = in->diffuse.width;
		context->inputs[1] = in->diffuse.height;
	}
	if (in->create_only)
		return 1;
	/* (the one noisy colour and the hit distance, at the inputs' texels) */
	if (!(set = rr_set(dlss.set_layout)))
		return 0;
	memset(images, 0, sizeof(images));
	memset(writes, 0, sizeof(writes));
	images[0].imageView = in->diffuse.view;
	images[1].imageView = in->specular.view;
	images[2].imageView = in->diffuse_albedo.view;
	images[3].imageView = in->specular_albedo.view;
	images[4].imageView = context->color.view;
	images[5].imageView = context->hit_distance.view;
	for (index = 0; index < 6; index++)
	{
		images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = set;
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
		writes[index].descriptorType = index < 4 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[index].pImageInfo = &images[index];
	}
	dlss.vkUpdateDescriptorSets(dlss.device, 6, writes, 0, NULL);
	push[0] = in->source[0];
	push[1] = in->source[1];
	push[2] = (int32_t)render[0];
	push[3] = (int32_t)render[1];
	push[4] = in->demodulated != 0;
	/* (the feature's last frame done reading the composed images) */
	rr_barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT);
	dlss.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.compose);
	dlss.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.pipeline_layout, 0, 1, &set, 0, NULL);
	dlss.vkCmdPushConstants(commands, dlss.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
	dlss.vkCmdDispatch(commands, (render[0] + 7) / 8, (render[1] + 7) / 8, 1);
	rr_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
	memset(&frame, 0, sizeof(frame));
	frame.color.image = context->color.image;
	frame.color.view = context->color.view;
	frame.color.format = context->color.format;
	frame.color.width = context->color.width;
	frame.color.height = context->color.height;
	frame.color.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	frame.specular_hit_distance = frame.color;
	frame.specular_hit_distance.image = context->hit_distance.image;
	frame.specular_hit_distance.view = context->hit_distance.view;
	frame.specular_hit_distance.format = context->hit_distance.format;
	frame.diffuse_albedo = in->diffuse_albedo;
	frame.specular_albedo = in->specular_albedo;
	frame.normal_roughness = in->normal_roughness;
	frame.depth = in->depth;
	frame.motion = in->motion;
	frame.output.image = context->output_image.image;
	frame.output.view = context->output_image.view;
	frame.output.format = context->output_image.format;
	frame.output.width = in->output[0];
	frame.output.height = in->output[1];
	frame.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
	frame.corner[0] = (uint32_t)in->source[0];
	frame.corner[1] = (uint32_t)in->source[1];
	memcpy(frame.render, render, sizeof(render));
	frame.jitter[0] = in->jitter[0];
	frame.jitter[1] = in->jitter[1];
	/* (the motion a fraction of the view: the render's pixels) */
	frame.motion_scale[0] = (float)render[0];
	frame.motion_scale[1] = (float)render[1];
	memcpy(frame.world_to_view, in->world_to_view, sizeof(frame.world_to_view));
	memcpy(frame.view_to_clip, in->view_to_clip, sizeof(frame.view_to_clip));
	frame.reset = in->reset != 0;
	if (!dlss.calls->rr_evaluate(commands, context->feature, &frame))
	{
		if (!dlss.rr_failed_said)
			host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: a frame cannot be recorded");
		dlss.rr_failed_said = 1;
		return 0;
	}
	rr_barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
	return 1;
}

int xgpu_vulkan_dlss_rr_available(void)
{
	return dlss.ready && dlss.rr_ready;
}

int xgpu_vulkan_dlss_reconstruct(int window, const struct xgpu_vulkan_dlss_rr_frame *frame)
{
	struct dlss_rr_context *context = window >= 0 && window < XGPU_VULKAN_FSR_WINDOWS ? &dlss.rr_contexts[window] :
		NULL;
	struct xgpu_vulkan_image *read[7], *written[1];
	const struct xgpu_vulkan_image_description *source, *destination;
	struct rr_inputs in;
	VkCommandBuffer commands;
	int32_t whole[4];
	int index, ok;

	if (!dlss.ready || !dlss.rr_ready || !context || !frame->diffuse || !frame->specular || !frame->diffuse_albedo ||
		!frame->specular_albedo || !frame->normal_roughness || !frame->depth || !frame->motion || !frame->destination)
		return 0;
	read[0] = frame->diffuse;
	read[1] = frame->specular;
	read[2] = frame->diffuse_albedo;
	read[3] = frame->specular_albedo;
	read[4] = frame->normal_roughness;
	read[5] = frame->depth;
	read[6] = frame->motion;
	source = xgpu_vulkan_image_description(frame->diffuse);
	destination = xgpu_vulkan_image_description(frame->destination);
	/* (every input the same size, the rectangles in them) */
	for (index = 1; index < 7; index++)
	{
		if (xgpu_vulkan_image_description(read[index])->width != source->width ||
			xgpu_vulkan_image_description(read[index])->height != source->height)
			return 0;
	}
	if (frame->source[2] - frame->source[0] < 8 || frame->source[3] - frame->source[1] < 8 || frame->source[0] < 0 ||
		frame->source[1] < 0 || frame->source[2] > (int32_t)source->width || frame->source[3] > (int32_t)source->height ||
		frame->destination_rectangle[2] - frame->destination_rectangle[0] < 8 ||
		frame->destination_rectangle[3] - frame->destination_rectangle[1] < 8 || frame->destination_rectangle[0] < 0 ||
		frame->destination_rectangle[1] < 0 || frame->destination_rectangle[2] > (int32_t)destination->width ||
		frame->destination_rectangle[3] > (int32_t)destination->height)
		return 0;
	memset(&in, 0, sizeof(in));
	in.diffuse = image_of(frame->diffuse, VK_IMAGE_ASPECT_COLOR_BIT);
	in.specular = image_of(frame->specular, VK_IMAGE_ASPECT_COLOR_BIT);
	in.diffuse_albedo = image_of(frame->diffuse_albedo, VK_IMAGE_ASPECT_COLOR_BIT);
	in.specular_albedo = image_of(frame->specular_albedo, VK_IMAGE_ASPECT_COLOR_BIT);
	in.normal_roughness = image_of(frame->normal_roughness, VK_IMAGE_ASPECT_COLOR_BIT);
	in.depth = image_of(frame->depth, VK_IMAGE_ASPECT_DEPTH_BIT);
	in.motion = image_of(frame->motion, VK_IMAGE_ASPECT_COLOR_BIT);
	memcpy(in.source, frame->source, sizeof(in.source));
	in.output[0] = (uint32_t)(frame->destination_rectangle[2] - frame->destination_rectangle[0]);
	in.output[1] = (uint32_t)(frame->destination_rectangle[3] - frame->destination_rectangle[1]);
	in.demodulated = frame->demodulated;
	in.reset = frame->reset;
	in.jitter[0] = frame->jitter[0];
	in.jitter[1] = frame->jitter[1];
	in.world_to_view = frame->world_to_view;
	in.view_to_clip = frame->view_to_clip;
	garbage_collect(0);
	xgpu_vulkan_pass_owner("DLSS RR");
	written[0] = frame->destination;
	commands = xgpu_vulkan_outside_begin(read, 7, written, 1);
	ok = rr_record(commands, context, &in);
	if (ok)
	{
		whole[0] = whole[1] = 0;
		whole[2] = (int32_t)in.output[0];
		whole[3] = (int32_t)in.output[1];
		blit(commands, context->output_image.image, whole, xgpu_vulkan_image_handle(frame->destination),
			frame->destination_rectangle, VK_FILTER_NEAREST);
	}
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	return ok;
}

/* ---------- the light-transport pass's denoiser (HALO_LT_DENOISER=rr) */

/* the gather's push constants (tools/vulkan_dlss_shaders.py's LT_GATHER) */
struct lt_gather_push
{
	float camera[4][4], previous[4][4];
	int32_t corners[4], previous_corners[4];
	float control[4];
};
_Static_assert(sizeof(struct lt_gather_push) == 176, "LT_GATHER's push constants");
/* the depth's near plane (world units: about 3 mm), of the matrices given */
#define LT_NEAR 0.001f
#define LT_GATHER_BINDINGS 7

/* (its pipeline, once Ray Reconstruction is there; without it the pass
keeps ours) */
static void lt_setup(void)
{
	VkDescriptorSetLayoutBinding bindings[LT_GATHER_BINDINGS];
	VkDescriptorSetLayoutCreateInfo set_info;
	VkPushConstantRange push;
	VkPipelineLayoutCreateInfo layout_info;
	VkShaderModuleCreateInfo module_info;
	VkComputePipelineCreateInfo info;
	VkShaderModule shader = VK_NULL_HANDLE;
	VkResult result;
	uint32_t index;

	memset(bindings, 0, sizeof(bindings));
	for (index = 0; index < LT_GATHER_BINDINGS; index++)
	{
		bindings[index].binding = index;
		bindings[index].descriptorType = index < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
			VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		bindings[index].pImmutableSamplers = index < 3 ? &dlss.point : NULL;
	}
	memset(&set_info, 0, sizeof(set_info));
	set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_info.bindingCount = LT_GATHER_BINDINGS;
	set_info.pBindings = bindings;
	if (dlss.vkCreateDescriptorSetLayout(dlss.device, &set_info, NULL, &dlss.lt_set_layout) != VK_SUCCESS)
	{
		dlss.lt_set_layout = VK_NULL_HANDLE;
		goto failed;
	}
	push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	push.offset = 0;
	push.size = sizeof(struct lt_gather_push);
	memset(&layout_info, 0, sizeof(layout_info));
	layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout_info.setLayoutCount = 1;
	layout_info.pSetLayouts = &dlss.lt_set_layout;
	layout_info.pushConstantRangeCount = 1;
	layout_info.pPushConstantRanges = &push;
	if (dlss.vkCreatePipelineLayout(dlss.device, &layout_info, NULL, &dlss.lt_pipeline_layout) != VK_SUCCESS)
	{
		dlss.lt_pipeline_layout = VK_NULL_HANDLE;
		goto failed;
	}
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = sizeof(dlss_lt_gather_spirv);
	module_info.pCode = dlss_lt_gather_spirv;
	if (dlss.vkCreateShaderModule(dlss.device, &module_info, NULL, &shader) != VK_SUCCESS)
		goto failed;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	info.stage.module = shader;
	info.stage.pName = "main";
	info.layout = dlss.lt_pipeline_layout;
	result = dlss.vkCreateComputePipelines(dlss.device, VK_NULL_HANDLE, 1, &info, NULL, &dlss.lt_gather);
	dlss.vkDestroyShaderModule(dlss.device, shader, NULL);
	if (result == VK_SUCCESS)
		return;
	dlss.lt_gather = VK_NULL_HANDLE;
failed:
	host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: the light-transport pass's gather could not be made");
}

/* (the context's feature and images let go of, once the work that used
them is done; its result kept for the pass's sets to let go of first) */
static void lt_release(void)
{
	struct dlss_lt_context *context = &dlss.lt;
	struct xgpu_vulkan_image *result = context->result;

	garbage_add(context->feature, &context->color, &context->normal_roughness, &context->depth);
	garbage_add(NULL, &context->motion, &context->diffuse_albedo, &context->specular_albedo);
	garbage_add(NULL, &context->output_image, NULL, NULL);
	memset(context, 0, sizeof(*context));
	context->result = result;
}

int xgpu_vulkan_dlss_lt_selected(void)
{
	if (!dlss.lt_selected)
	{
		const char *given = getenv("HALO_LT_DENOISER");

		dlss.lt_selected = -1;
		if (given && !strcmp(given, "rr"))
		{
			if (dlss.ready && dlss.rr_ready && dlss.lt_gather)
				dlss.lt_selected = 1;
			host_logf(dlss.lt_selected > 0 ? HOST_LOG_INFO : HOST_LOG_WARN,
				"vulkan: light transport: HALO_LT_DENOISER=rr: %s", dlss.lt_selected > 0 ?
				"DLSS Ray Reconstruction denoises the pass (our denoiser not run)" :
				"DLSS Ray Reconstruction is not available here: our denoiser");
		}
	}
	return dlss.lt_selected > 0;
}

/* (a camera's basis: right, down, forward, unit, and the screen's half
extents over the forward's length) */
static void lt_basis(const float camera[4][4], float axes[3][3], float half[2])
{
	float length[3];
	int axis, k;

	for (axis = 0; axis < 3; axis++)
	{
		const float *v = camera[axis == 2 ? 1 : axis == 0 ? 2 : 3];

		length[axis] = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		for (k = 0; k < 3; k++)
			axes[axis][k] = length[axis] > 0.0f ? v[k] / length[axis] : 0.0f;
	}
	half[0] = length[2] > 0.0f ? length[0] / length[2] : 1.0f;
	half[1] = length[2] > 0.0f ? length[1] / length[2] : 1.0f;
}

struct xgpu_vulkan_image *xgpu_vulkan_dlss_lt_denoise(struct xgpu_vulkan_image *light,
	struct xgpu_vulkan_image *inputs, struct xgpu_vulkan_image *guides, const struct xgpu_vulkan_dlss_lt_cameras *pass,
	const int32_t rectangle[4], int reset)
{
	struct dlss_lt_context *context = &dlss.lt;
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *read[3], *written[1];
	VkDescriptorImageInfo images[LT_GATHER_BINDINGS];
	VkWriteDescriptorSet writes[LT_GATHER_BINDINGS];
	struct openh1_dlss_rr_frame frame;
	struct lt_gather_push push;
	VkCommandBuffer commands;
	VkDescriptorSet set = VK_NULL_HANDLE;
	float axes[3][3], half[2], world_to_view[16], view_to_clip[16];
	uint32_t render[2], index;
	int32_t whole[4];
	int ok = 1, remake;

	if (!xgpu_vulkan_dlss_lt_selected() || !light || !inputs || !pass)
		return NULL;
	size = xgpu_vulkan_image_description(inputs);
	if (rectangle[2] - rectangle[0] < 8 || rectangle[3] - rectangle[1] < 8 || rectangle[0] < 0 || rectangle[1] < 0 ||
		rectangle[2] > (int32_t)size->width || rectangle[3] > (int32_t)size->height ||
		xgpu_vulkan_image_description(light)->width != size->width ||
		xgpu_vulkan_image_description(light)->height != size->height ||
		(guides && (xgpu_vulkan_image_description(guides)->width != size->width ||
		xgpu_vulkan_image_description(guides)->height != size->height)))
		return NULL;
	render[0] = (uint32_t)(rectangle[2] - rectangle[0]);
	render[1] = (uint32_t)(rectangle[3] - rectangle[1]);
	garbage_collect(0);
	remake = !context->feature || context->render[0] != render[0] || context->render[1] != render[1] ||
		context->size[0] != size->width || context->size[1] != size->height;
	/* (a feature just made: its submission not done yet, the light as it is) */
	if (!remake && xgpu_vulkan_completed() < context->made_serial)
		return NULL;
	/* (the result the pass is given, the inputs' size: made again with them,
	never while the pass's sets still name it) */
	if (!context->result || xgpu_vulkan_image_description(context->result)->width != size->width ||
		xgpu_vulkan_image_description(context->result)->height != size->height)
	{
		struct xgpu_vulkan_image_description description;

		if (context->result && xgpu_vulkan_rt_bounce_bound(context->result))
			xgpu_vulkan_rt_bounce_set(NULL);
		xgpu_vulkan_image_destroy(context->result);
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA16F;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED | XGPU_VULKAN_USAGE_TARGET;
		description.width = size->width;
		description.height = size->height;
		description.depth_or_layers = 1;
		description.levels = 1;
		context->result = xgpu_vulkan_image_create(&description);
		if (!context->result)
			return NULL;
	}
	read[0] = light;
	read[1] = inputs;
	read[2] = guides ? guides : inputs;
	written[0] = context->result;
	xgpu_vulkan_pass_owner("DENOISE (DLSS RR)");
	commands = xgpu_vulkan_outside_begin(read, 3, written, 1);
	/* (the feature and its images, made again for new sizes; its frames
	from the next whose submission is after this one's) */
	if (remake)
	{
		struct openh1_dlss_rr_create create;
		VkClearColorValue value;
		VkImageSubresourceRange range;

		lt_release();
		memset(&create, 0, sizeof(create));
		memcpy(create.render, render, sizeof(render));
		memcpy(create.output, render, sizeof(render));
		ok = image_make(commands, &context->color, size->width, size->height, VK_FORMAT_R16G16B16A16_SFLOAT) &&
			image_make(commands, &context->normal_roughness, size->width, size->height,
				VK_FORMAT_R16G16B16A16_SFLOAT) &&
			image_make(commands, &context->depth, size->width, size->height, VK_FORMAT_R32_SFLOAT) &&
			image_make(commands, &context->motion, size->width, size->height, VK_FORMAT_R16G16_SFLOAT) &&
			image_make(commands, &context->diffuse_albedo, size->width, size->height, VK_FORMAT_R8G8B8A8_UNORM) &&
			image_make(commands, &context->specular_albedo, size->width, size->height, VK_FORMAT_R8G8B8A8_UNORM) &&
			image_make(commands, &context->output_image, render[0], render[1], VK_FORMAT_R16G16B16A16_SFLOAT) &&
			(context->feature = dlss.calls->rr_create(commands, &create)) != NULL;
		if (ok)
		{
			/* (the albedos: the light alone is denoised, diffuse, white) */
			memset(&range, 0, sizeof(range));
			range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			range.levelCount = 1;
			range.layerCount = 1;
			value.float32[0] = value.float32[1] = value.float32[2] = value.float32[3] = 1.0f;
			dlss.vkCmdClearColorImage(commands, context->diffuse_albedo.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
				&range);
			value.float32[0] = value.float32[1] = value.float32[2] = value.float32[3] = 0.0f;
			dlss.vkCmdClearColorImage(commands, context->specular_albedo.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1,
				&range);
			memcpy(context->render, render, sizeof(render));
			context->size[0] = size->width;
			context->size[1] = size->height;
			context->made_serial = xgpu_vulkan_serial();
			context->fresh = 1;
			ok = 0;
		}
		else
		{
			/* (not tried again: our denoiser from the next frame) */
			lt_release();
			host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: the light-transport pass's feature cannot be "
				"made (%ux%u): our denoiser", render[0], render[1]);
			dlss.lt_selected = -1;
		}
	}
	if (ok && !(set = rr_set(dlss.lt_set_layout)))
		ok = 0;
	if (ok)
	{
		/* 1: the gather */
		memset(images, 0, sizeof(images));
		memset(writes, 0, sizeof(writes));
		images[0].imageView = xgpu_vulkan_image_view(light);
		images[1].imageView = xgpu_vulkan_image_view(inputs);
		images[2].imageView = xgpu_vulkan_image_view(guides ? guides : inputs);
		images[3].imageView = context->color.view;
		images[4].imageView = context->normal_roughness.view;
		images[5].imageView = context->depth.view;
		images[6].imageView = context->motion.view;
		for (index = 0; index < LT_GATHER_BINDINGS; index++)
		{
			images[index].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[index].dstSet = set;
			writes[index].dstBinding = index;
			writes[index].descriptorCount = 1;
			writes[index].descriptorType = index < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER :
				VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[index].pImageInfo = &images[index];
		}
		dlss.vkUpdateDescriptorSets(dlss.device, LT_GATHER_BINDINGS, writes, 0, NULL);
		memset(&push, 0, sizeof(push));
		memcpy(push.camera, pass->camera, sizeof(push.camera));
		memcpy(push.previous, pass->previous, sizeof(push.previous));
		memcpy(push.corners, rectangle, sizeof(push.corners));
		memcpy(push.previous_corners, pass->previous_corners, sizeof(push.previous_corners));
		push.control[0] = LT_NEAR;
		push.control[1] = guides ? 1.0f : 0.0f;
		push.control[2] = getenv("HALO_LT_DENOISE_CAMERA") && strcmp(getenv("HALO_LT_DENOISE_CAMERA"), "0") ? 1.0f :
			0.0f;
		/* (the feature's last frame done reading the gathered images) */
		rr_barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
			VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_ACCESS_SHADER_WRITE_BIT);
		dlss.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.lt_gather);
		dlss.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, dlss.lt_pipeline_layout, 0, 1, &set, 0,
			NULL);
		dlss.vkCmdPushConstants(commands, dlss.lt_pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push),
			&push);
		dlss.vkCmdDispatch(commands, (render[0] + 7) / 8, (render[1] + 7) / 8, 1);
		rr_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
		/* 2: the camera's matrices (NGX's: column-major, the translation in
		12 to 14): view x right, y down, z forward; the clip reversed with
		an infinite far plane (depth = near / z, the gather's) */
		lt_basis(pass->camera, axes, half);
		memset(world_to_view, 0, sizeof(world_to_view));
		for (index = 0; index < 3; index++)
		{
			world_to_view[0 * 4 + index] = axes[index][0];
			world_to_view[1 * 4 + index] = axes[index][1];
			world_to_view[2 * 4 + index] = axes[index][2];
			world_to_view[12 + index] = -(axes[index][0] * pass->camera[0][0] + axes[index][1] * pass->camera[0][1] +
				axes[index][2] * pass->camera[0][2]);
		}
		world_to_view[15] = 1.0f;
		memset(view_to_clip, 0, sizeof(view_to_clip));
		view_to_clip[0] = 1.0f / half[0];
		view_to_clip[5] = 1.0f / half[1];
		view_to_clip[11] = 1.0f;
		view_to_clip[14] = LT_NEAR;
		/* 3: the feature's frame, its output into the result at the pass's
		rectangle */
		memset(&frame, 0, sizeof(frame));
		frame.color.image = context->color.image;
		frame.color.view = context->color.view;
		frame.color.format = context->color.format;
		frame.color.width = size->width;
		frame.color.height = size->height;
		frame.color.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		frame.normal_roughness = frame.depth = frame.motion = frame.diffuse_albedo = frame.specular_albedo =
			frame.color;
		frame.normal_roughness.image = context->normal_roughness.image;
		frame.normal_roughness.view = context->normal_roughness.view;
		frame.normal_roughness.format = context->normal_roughness.format;
		frame.depth.image = context->depth.image;
		frame.depth.view = context->depth.view;
		frame.depth.format = context->depth.format;
		frame.motion.image = context->motion.image;
		frame.motion.view = context->motion.view;
		frame.motion.format = context->motion.format;
		frame.diffuse_albedo.image = context->diffuse_albedo.image;
		frame.diffuse_albedo.view = context->diffuse_albedo.view;
		frame.diffuse_albedo.format = context->diffuse_albedo.format;
		frame.specular_albedo.image = context->specular_albedo.image;
		frame.specular_albedo.view = context->specular_albedo.view;
		frame.specular_albedo.format = context->specular_albedo.format;
		/* (no specular yet: no hit distance) */
		frame.output.image = context->output_image.image;
		frame.output.view = context->output_image.view;
		frame.output.format = context->output_image.format;
		frame.output.width = render[0];
		frame.output.height = render[1];
		frame.output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
		frame.corner[0] = (uint32_t)rectangle[0];
		frame.corner[1] = (uint32_t)rectangle[1];
		memcpy(frame.render, render, sizeof(render));
		/* (the motion in the render's pixels already) */
		frame.motion_scale[0] = frame.motion_scale[1] = 1.0f;
		memcpy(frame.world_to_view, world_to_view, sizeof(world_to_view));
		memcpy(frame.view_to_clip, view_to_clip, sizeof(view_to_clip));
		frame.reset = reset || context->fresh;
		ok = dlss.calls->rr_evaluate(commands, context->feature, &frame);
		context->fresh = 0;
		if (ok)
		{
			rr_barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
			whole[0] = whole[1] = 0;
			whole[2] = (int32_t)render[0];
			whole[3] = (int32_t)render[1];
			blit(commands, context->output_image.image, whole, xgpu_vulkan_image_handle(context->result), rectangle,
				VK_FILTER_NEAREST);
		}
		else if (!dlss.lt_failed_said)
		{
			host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction: a light-transport frame cannot be recorded");
			dlss.lt_failed_said = 1;
		}
	}
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	if (ok && !dlss.lt_said)
	{
		host_logf(HOST_LOG_INFO, "vulkan: DLSS Ray Reconstruction: the light-transport pass's light denoised "
			"(%ux%u, runtime %s)", render[0], render[1], dlss.rr_version);
		dlss.lt_said = 1;
	}
	return ok ? context->result : NULL;
}

/* ---------- HALO_DLSS_RR_SMOKE: Ray Reconstruction on a synthetic noisy
frame, once (640x360 to 1280x720, four frames, the first a reset): made,
recorded, and its output read back once the GPU is done, which must be
finite and not black */

#define SMOKE_RENDER_WIDTH 640
#define SMOKE_RENDER_HEIGHT 360
#define SMOKE_IMAGES 7

static struct
{
	/* 0 to begin, 3 the feature made (its submission awaited), 1 the
	frames recorded (their submission awaited), 2 done */
	int state;
	uint64_t serial;
	struct rr_inputs in;
	struct dlss_output images[SMOKE_IMAGES];
	VkBuffer buffers[2];
	VkDeviceMemory memories[2];
	VkDeviceSize sizes[2];
} smoke;

static int smoke_buffer(int which, VkDeviceSize size, VkBufferUsageFlags usage)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	uint32_t type;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage;
	if (dlss.vkCreateBuffer(dlss.device, &info, NULL, &smoke.buffers[which]) != VK_SUCCESS)
		return 0;
	dlss.vkGetBufferMemoryRequirements(dlss.device, smoke.buffers[which], &requirements);
	for (type = 0; type < dlss.memory.memoryTypeCount; type++)
	{
		if ((requirements.memoryTypeBits & (1u << type)) &&
			(dlss.memory.memoryTypes[type].propertyFlags & wanted) == wanted)
			break;
	}
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (type == dlss.memory.memoryTypeCount ||
		dlss.vkAllocateMemory(dlss.device, &allocate, NULL, &smoke.memories[which]) != VK_SUCCESS)
		return 0;
	smoke.sizes[which] = size;
	return dlss.vkBindBufferMemory(dlss.device, smoke.buffers[which], smoke.memories[which], 0) == VK_SUCCESS;
}

static void smoke_resources_free(void)
{
	int index;

	for (index = 0; index < SMOKE_IMAGES; index++)
		output_free(&smoke.images[index]);
	for (index = 0; index < 2; index++)
	{
		if (smoke.buffers[index])
			dlss.vkDestroyBuffer(dlss.device, smoke.buffers[index], NULL);
		if (smoke.memories[index])
			dlss.vkFreeMemory(dlss.device, smoke.memories[index], NULL);
		smoke.buffers[index] = VK_NULL_HANDLE;
		smoke.memories[index] = VK_NULL_HANDLE;
	}
}

static void smoke_free(void)
{
	smoke_resources_free();
	rr_context_release(&dlss.rr_contexts[XGPU_VULKAN_FSR_WINDOWS]);
}

/* (a float as a half, for the noise) */
static uint16_t smoke_half(float value)
{
	union
	{
		float f;
		uint32_t u;
	} bits;
	int32_t exponent;

	bits.f = value;
	exponent = (int32_t)((bits.u >> 23) & 0xff) - 127 + 15;
	if (value <= 0.0f || exponent <= 0)
		return 0;
	if (exponent >= 31)
		return 0x7bff;
	return (uint16_t)((exponent << 10) | ((bits.u >> 13) & 0x3ff));
}

static void smoke_view(struct openh1_dlss_image *made, const struct dlss_output *image)
{
	made->image = image->image;
	made->view = image->view;
	made->format = image->format;
	made->width = image->width;
	made->height = image->height;
	made->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
}

/* the images, the noise and the feature (the first frame), then, once
that submission is done, the four frames and the output's copy */
static void smoke_record(int frames)
{
	/* the diffuse, the specular (noise), the albedos, the normals with
	roughness, the depth (R32F: read as a texture, as the device's depth
	is), the motion */
	static const float clears[SMOKE_IMAGES][4] = { { 0 }, { 0 }, { 0.6f, 0.5f, 0.4f, 1 }, { 0.04f, 0.04f, 0.04f, 1 },
		{ 0, 0, 1, 0.5f }, { 0.5f, 0, 0, 0 }, { 0 } };
	static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
	/* (a perspective of 70 degrees, 16:9, reversed depth, near 0.1) */
	static const float projection[16] = { 0.802f, 0, 0, 0, 0, 1.428f, 0, 0, 0, 0, 0, -1, 0, 0, 0.1f, 0 };
	struct rr_inputs in;
	struct dlss_rr_context *context = &dlss.rr_contexts[XGPU_VULKAN_FSR_WINDOWS];
	VkCommandBuffer commands;
	VkImageSubresourceRange range;
	VkBufferImageCopy copy;
	VkClearColorValue clear;
	uint16_t *noise;
	uint32_t seed = 12345u, index, texels = SMOKE_RENDER_WIDTH * SMOKE_RENDER_HEIGHT;
	int frame, ok = 1;

	smoke.state = 2;
	commands = xgpu_vulkan_outside_begin(NULL, 0, NULL, 0);
	if (frames)
		goto record;
	for (index = 0; index < SMOKE_IMAGES && ok; index++)
		ok = image_make(commands, &smoke.images[index], SMOKE_RENDER_WIDTH, SMOKE_RENDER_HEIGHT,
			index == 5 ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16G16B16A16_SFLOAT);
	ok = ok && smoke_buffer(0, (VkDeviceSize)texels * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT) &&
		smoke_buffer(1, (VkDeviceSize)1280 * 720 * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT) &&
		dlss.vkMapMemory(dlss.device, smoke.memories[0], 0, smoke.sizes[0], 0, (void **)&noise) == VK_SUCCESS;
	if (!ok)
	{
		host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction smoke test: its images could not be made");
		xgpu_vulkan_outside_end(commands);
		return;
	}
	/* (noise: a few bright texels in a dim field, as one sample a pixel
	gives; the alpha the hit distance) */
	for (index = 0; index < texels; index++)
	{
		float value;

		seed = seed * 1664525u + 1013904223u;
		value = (seed >> 8 & 0xff) < 16 ? 8.0f : (float)(seed >> 16 & 0xff) / 512.0f;
		noise[index * 4 + 0] = noise[index * 4 + 1] = noise[index * 4 + 2] = smoke_half(value);
		noise[index * 4 + 3] = smoke_half(2.0f);
	}
	dlss.vkUnmapMemory(dlss.device, smoke.memories[0]);
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = range.layerCount = 1;
	rr_barrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_ACCESS_TRANSFER_WRITE_BIT);
	memset(&copy, 0, sizeof(copy));
	copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	copy.imageSubresource.layerCount = 1;
	copy.imageExtent.width = SMOKE_RENDER_WIDTH;
	copy.imageExtent.height = SMOKE_RENDER_HEIGHT;
	copy.imageExtent.depth = 1;
	dlss.vkCmdCopyBufferToImage(commands, smoke.buffers[0], smoke.images[0].image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	dlss.vkCmdCopyBufferToImage(commands, smoke.buffers[0], smoke.images[1].image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	for (index = 2; index < SMOKE_IMAGES; index++)
	{
		memcpy(clear.float32, clears[index], sizeof(clear.float32));
		dlss.vkCmdClearColorImage(commands, smoke.images[index].image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
	}
	rr_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT);
	memset(&in, 0, sizeof(in));
	smoke_view(&in.diffuse, &smoke.images[0]);
	smoke_view(&in.specular, &smoke.images[1]);
	smoke_view(&in.diffuse_albedo, &smoke.images[2]);
	smoke_view(&in.specular_albedo, &smoke.images[3]);
	smoke_view(&in.normal_roughness, &smoke.images[4]);
	smoke_view(&in.depth, &smoke.images[5]);
	smoke_view(&in.motion, &smoke.images[6]);
	in.source[2] = SMOKE_RENDER_WIDTH;
	in.source[3] = SMOKE_RENDER_HEIGHT;
	in.output[0] = 1280;
	in.output[1] = 720;
	in.demodulated = 1;
	in.world_to_view = identity;
	in.view_to_clip = projection;
	in.create_only = 1;
	ok = rr_record(commands, context, &in);
	in.create_only = 0;
	smoke.in = in;
	smoke.serial = xgpu_vulkan_serial();
	smoke.state = ok ? 3 : 2;
	if (!ok)
		host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction smoke test: no feature made");
	xgpu_vulkan_outside_end(commands);
	return;
record:
	in = smoke.in;
	for (frame = 0; frame < 4 && ok; frame++)
	{
		in.reset = frame == 0;
		in.jitter[0] = frame & 1 ? 0.25f : -0.25f;
		in.jitter[1] = frame & 2 ? 0.25f : -0.25f;
		ok = rr_record(commands, context, &in);
	}
	if (ok)
	{
		memset(&copy, 0, sizeof(copy));
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent.width = 1280;
		copy.imageExtent.height = 720;
		copy.imageExtent.depth = 1;
		dlss.vkCmdCopyImageToBuffer(commands, context->output_image.image, VK_IMAGE_LAYOUT_GENERAL, smoke.buffers[1],
			1, &copy);
		smoke.serial = xgpu_vulkan_serial();
		smoke.state = 1;
	}
	host_logf(ok ? HOST_LOG_INFO : HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction smoke test: %s (runtime %s)",
		ok ? "4 frames recorded" : "not recorded", dlss.rr_version);
	xgpu_vulkan_outside_end(commands);
}

/* (a half as a float) */
static float smoke_float(uint16_t half)
{
	union
	{
		float f;
		uint32_t u;
	} bits;
	uint32_t exponent = half >> 10 & 0x1f, mantissa = half & 0x3ff;

	if (exponent == 31)
		return mantissa ? NAN : (half & 0x8000 ? -1e30f : 1e30f);
	if (!exponent)
		return (half & 0x8000 ? -1.0f : 1.0f) * (float)mantissa / 16777216.0f;
	bits.u = (uint32_t)(half & 0x8000) << 16 | (exponent - 15 + 127) << 23 | mantissa << 13;
	return bits.f;
}

static void rr_smoke(void)
{
	uint16_t *texels;
	double sum = 0.0, squares = 0.0;
	uint32_t index, count = 1280 * 720, bad = 0;

	if (smoke.state == 0)
	{
		if (!dlss.ready || !dlss.rr_ready)
		{
			host_logf(HOST_LOG_WARN, "vulkan: DLSS Ray Reconstruction smoke test: not available");
			smoke.state = 2;
			return;
		}
		smoke_record(0);
		if (smoke.state == 2)
			smoke_free();
		return;
	}
	if (smoke.state == 3 && xgpu_vulkan_completed() >= smoke.serial)
	{
		smoke_record(1);
		if (smoke.state == 2)
			smoke_free();
		return;
	}
	if (smoke.state != 1 || xgpu_vulkan_completed() < smoke.serial)
		return;
	smoke.state = 2;
	if (dlss.vkMapMemory(dlss.device, smoke.memories[1], 0, smoke.sizes[1], 0, (void **)&texels) != VK_SUCCESS)
	{
		smoke_free();
		return;
	}
	for (index = 0; index < count; index++)
	{
		float value = smoke_float(texels[index * 4 + 1]);

		if (!(value > -1e4f && value < 1e4f))
		{
			bad++;
			continue;
		}
		sum += value;
		squares += (double)value * value;
	}
	dlss.vkUnmapMemory(dlss.device, smoke.memories[1]);
	sum /= count;
	host_logf(bad || sum <= 0.0 ? HOST_LOG_WARN : HOST_LOG_INFO, "vulkan: DLSS Ray Reconstruction smoke test: %s, "
		"output mean %.4f, deviation %.4f, %u texels not finite (runtime %s)", bad || sum <= 0.0 ? "FAILED" : "passed",
		sum, sqrt(squares / count - sum * sum > 0.0 ? squares / count - sum * sum : 0.0), bad, dlss.rr_version);
	smoke_free();
}

int xgpu_vulkan_dlss_upscale(int window, const struct xgpu_vulkan_fsr_frame *frame)
{
	double started = host_pass_times > 0 ? host_load_trace_clock(CLOCK_MONOTONIC) : 0.0;
	int done;

	if (getenv("HALO_DLSS_RR_SMOKE"))
		rr_smoke();
	done = upscale(window, frame);

	if (!done && frame->color && frame->destination)
		fallback_blit(frame);
	/* (debug.pass_times: its CPU's share too, every 300 frames) */
	if (host_pass_times > 0)
	{
		dlss.recording += host_load_trace_clock(CLOCK_MONOTONIC) - started;
		if (++dlss.recorded == 300)
		{
			host_logf(HOST_LOG_INFO, "vulkan: display.temporal_aa \"dlss\": %.3f ms of the render thread a window's "
				"frame, recording its work", dlss.recording * 1000.0 / dlss.recorded);
			dlss.recording = 0.0;
			dlss.recorded = 0;
		}
	}
	return done;
}

/* (the device idle) */
void xgpu_vulkan_dlss_destroy(void)
{
	const struct openh1_dlss *calls = dlss.calls;
	int index;

	if (dlss.ready)
	{
		for (index = 0; index < XGPU_VULKAN_FSR_WINDOWS; index++)
			context_release(&dlss.contexts[index]);
		rr_teardown();
		garbage_collect(1);
		calls->shutdown(dlss.device);
	}
	memset(&dlss, 0, sizeof(dlss));
	/* (the module stays loaded: a device made again asks it again) */
	dlss.calls = calls;
}
