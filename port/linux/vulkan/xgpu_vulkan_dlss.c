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

Render thread alone, as the core.
*/

#include "xgpu_vulkan_dlss.h"
#include "host.h"
#include "host_load_trace.h"
#include "dlss_addon_folder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif !defined(__ANDROID__) && !defined(__APPLE__)
#include <dlfcn.h>
#endif

#define GARBAGE 16

struct dlss_output
{
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	uint32_t width, height;
};

struct dlss_context
{
	void *feature;
	uint32_t render[2], output[2];
	int hdr;
	struct dlss_output output_image;
};

static struct
{
	const struct openh1_dlss *calls;
	int ready, failed_said;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	struct dlss_context contexts[XGPU_VULKAN_FSR_WINDOWS];
	/* features and images let go of, released once the submission that
	last used them is done */
	struct
	{
		void *feature;
		struct dlss_output image;
		uint64_t serial;
	} garbage[GARBAGE];
	uint32_t garbage_count;
	double recording;
	uint32_t recorded;
#define DLSS_FUNCTIONS(F) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) F(vkCmdPipelineBarrier) F(vkCmdBlitImage)
#define DECLARE(name) PFN_##name name;
	DLSS_FUNCTIONS(DECLARE)
#undef DECLARE
} dlss;

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
	if (!dlss.calls || dlss.calls->abi != OPENH1_DLSS_ABI)
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
			output_free(&dlss.garbage[index].image);
			dlss.garbage[index] = dlss.garbage[--dlss.garbage_count];
		}
		else
		{
			index++;
		}
	}
}

/* (a window's feature and image let go of, once the work that used them is
done) */
static void context_release(struct dlss_context *context)
{
	if (!context->feature && !context->output_image.image)
		return;
	if (dlss.garbage_count == GARBAGE)
	{
		xgpu_vulkan_wait(dlss.garbage[0].serial);
		garbage_collect(0);
	}
	if (dlss.garbage_count < GARBAGE)
	{
		dlss.garbage[dlss.garbage_count].feature = context->feature;
		dlss.garbage[dlss.garbage_count].image = context->output_image;
		dlss.garbage[dlss.garbage_count].serial = xgpu_vulkan_serial();
		dlss.garbage_count++;
	}
	memset(context, 0, sizeof(*context));
}

/* an RGBA16F image the feature writes (storage) and a blit reads, into the
GENERAL layout with the work recorded */
static int output_make(VkCommandBuffer commands, struct dlss_output *image, uint32_t width, uint32_t height)
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
	info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = 1;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
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
		if (!output_make(commands, &context->output_image, output[0], output[1]) ||
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

int xgpu_vulkan_dlss_upscale(int window, const struct xgpu_vulkan_fsr_frame *frame)
{
	double started = host_pass_times > 0 ? host_load_trace_clock(CLOCK_MONOTONIC) : 0.0;
	int done = upscale(window, frame);

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
		garbage_collect(1);
		calls->shutdown(dlss.device);
	}
	memset(&dlss, 0, sizeof(dlss));
	/* (the module stays loaded: a device made again asks it again) */
	dlss.calls = calls;
}
