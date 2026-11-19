/*
XGPU_VULKAN_FSR.C

display.temporal_aa "fsr" in the Vulkan backend (docs/temporal-aa.md,
xgpu_vulkan_fsr.h): AMD FidelityFX Super Resolution 3.1's upscaler. Its
compute passes are the SDK's own (port/third_party/fidelityfx-fsr3, as
SPIR-V: xgpu_vulkan_fsr_shaders.c); what the SDK's host side does
(ffx_fsr3upscaler.cpp's fsr3upscalerCreate and fsr3upscalerDispatch, and
the reactive mask's ffxFsr3UpscalerContextGenerateReactiveMask) is here, in
C, on the core's command buffer:

- Each window (split screen's four) has its own history: the SDK's
  resources, made at its sizes (the source target's, the destination
  target's) and made again when they change. Those kept two frames are read
  from the frame before's and written to this frame's, in turn.
- The inputs are the window's 3D view as drawn: the colour (an SDR picture,
  or display.hdr's linear light: the passes' HDR build), the depth
  (reversed), the motion (the frame before's place less this one's, as a
  fraction of the view: a scale of 1), the jitter, and the reset. The
  passes read the inputs from their top left corner: a window elsewhere is
  copied there first.
- The exposure is fixed: 1 (the SDK's default, no pre-exposure); the frame
  time too, 1/60 s (it smooths the passes' own luminance alone).
- The reactive mask is the SDK's generated one: where the transparent draws
  changed the opaque view (xgpu_vulkan_fsr_opaque's copy of it), tone
  mapped, the largest channel's change, 0.9 past 0.2 (the SDK's suggested
  values). No transparency and composition mask.
- No sharpening (RCAS): the output is the accumulation's, RGBA16F, blitted
  into the destination's rectangle.

Render thread alone, as the core.
*/

#include "xgpu_vulkan_fsr.h"
#include "xgpu_vulkan_fsr_shaders.h"
#include "host.h"
#include "host_load_trace.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* the internal images (the SDK's FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_*
it makes), the shared ones it asks the application for (the dilated depth
and motion, the previous frame's nearest depth), and the port's own: the
output, the generated reactive mask, the inputs copied to the top left
corner and the opaque view */
enum
{
	IMAGE_ACCUMULATION_1,
	IMAGE_ACCUMULATION_2,
	IMAGE_LUMA_1,
	IMAGE_LUMA_2,
	IMAGE_INTERMEDIATE,
	IMAGE_SHADING_CHANGE,
	IMAGE_NEW_LOCKS,
	IMAGE_UPSCALED_1,
	IMAGE_UPSCALED_2,
	IMAGE_SPD_MIPS,
	IMAGE_FARTHEST_DEPTH_MIP1,
	IMAGE_LUMA_HISTORY_1,
	IMAGE_LUMA_HISTORY_2,
	IMAGE_SPD_ATOMIC,
	IMAGE_DILATED_REACTIVE_MASKS,
	IMAGE_DEFAULT_REACTIVITY,
	IMAGE_DEFAULT_EXPOSURE,
	IMAGE_FRAME_INFO,
	IMAGE_DILATED_MOTION_VECTORS,
	IMAGE_DILATED_DEPTH,
	IMAGE_RECONSTRUCTED_DEPTH,
	IMAGE_OUTPUT,
	IMAGE_REACTIVE,
	IMAGE_COLOR,
	IMAGE_DEPTH,
	IMAGE_MOTION,
	IMAGE_OPAQUE,
	IMAGE_COUNT
};

#define SPD_LEVELS 6
/* the descriptor pools in turn, each a submission's */
#define POOLS 8
#define POOL_SETS (XGPU_VULKAN_FSR_WINDOWS * XGPU_VULKAN_FSR_PASS_COUNT)
#define GARBAGE 64

struct fsr_image
{
	VkImage image;
	VkDeviceMemory memory;
	/* the whole image's view, and each level's (the downsampler's) */
	VkImageView view, levels[SPD_LEVELS];
	VkFormat format;
	uint32_t width, height, level_count;
	VkImageAspectFlags aspect;
};

/* the SDK's Fsr3UpscalerConstants (the passes' cbFSR3Upscaler, std140) */
struct fsr_constants
{
	int32_t render_size[2], previous_render_size[2];
	int32_t upscale_size[2], previous_upscale_size[2];
	int32_t max_render_size[2], max_upscale_size[2];
	float device_to_view_depth[4];
	float jitter[2], previous_jitter[2];
	float motion_vector_scale[2], downscale_factor[2];
	float motion_vector_jitter_cancellation[2];
	float tan_half_fov, jitter_phase_count;
	float delta_time, delta_pre_exposure, view_space_to_meters, frame_index;
	float velocity_factor, reactiveness_scale, shading_change_scale, accumulation_added_per_frame,
		min_disocclusion_accumulation;
};

/* cbSPD */
struct fsr_spd_constants
{
	uint32_t mips, work_groups, work_group_offset[2], render_size[2];
};

/* cbGenerateReactive (the reactive mask's) */
struct fsr_reactive_constants
{
	float scale, threshold, binary_value;
	uint32_t flags;
};

/* the SDK's FFX_FSR3UPSCALER_AUTOREACTIVEFLAGS_* */
#define AUTOREACTIVE_APPLY_TONEMAP 1u
#define AUTOREACTIVE_APPLY_THRESHOLD 4u
#define AUTOREACTIVE_USE_COMPONENTS_MAX 8u

struct fsr_context
{
	int made;
	/* the sizes it was made for: the source's (the SDK's maxRenderSize),
	the destination's (maxUpscaleSize); the colour's and depth's formats */
	uint32_t source_width, source_height, destination_width, destination_height;
	VkFormat color_format, depth_format;
	struct fsr_image images[IMAGE_COUNT];
	struct fsr_constants constants;
	/* the frames dispatched (their parity: which of a pair is read); none
	yet; the submission whose opaque view was copied (0 none) and its
	rectangle's size */
	uint32_t frame;
	int first;
	uint64_t opaque_serial;
	int32_t opaque_size[2];
};

static struct
{
	int ready, failed_said;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	VkPipelineCache cache;
	VkDescriptorSetLayout set_layouts[XGPU_VULKAN_FSR_PASS_COUNT];
	VkPipelineLayout layouts[XGPU_VULKAN_FSR_PASS_COUNT];
	/* [hdr][pass], made as first dispatched (0: not tried, -1 failed) */
	VkPipeline pipelines[2][XGPU_VULKAN_FSR_PASS_COUNT];
	int pipelines_made[2];
	VkSampler point, linear;
	struct
	{
		VkDescriptorPool pool;
		uint64_t serial;
	} pools[POOLS];
	int pool;
	struct fsr_context contexts[XGPU_VULKAN_FSR_WINDOWS];
	/* images let go of, destroyed once the submission that last used them
	is done */
	struct
	{
		struct fsr_image image;
		uint64_t serial;
	} garbage[GARBAGE];
	uint32_t garbage_count;
	/* (debug.pass_times: the render thread's time recording the frames'
	work, and the frames) */
	double recording;
	uint32_t recorded;
#define FSR_FUNCTIONS(F) \
	F(vkCreateImage) F(vkDestroyImage) F(vkGetImageMemoryRequirements) F(vkAllocateMemory) F(vkFreeMemory) \
	F(vkBindImageMemory) F(vkCreateImageView) F(vkDestroyImageView) F(vkCreateSampler) F(vkDestroySampler) \
	F(vkCreateShaderModule) F(vkDestroyShaderModule) F(vkCreateDescriptorSetLayout) \
	F(vkDestroyDescriptorSetLayout) F(vkCreatePipelineLayout) F(vkDestroyPipelineLayout) \
	F(vkCreateComputePipelines) F(vkDestroyPipeline) F(vkCreateDescriptorPool) F(vkDestroyDescriptorPool) \
	F(vkResetDescriptorPool) F(vkAllocateDescriptorSets) F(vkUpdateDescriptorSets) F(vkCmdPipelineBarrier) \
	F(vkCmdBindPipeline) F(vkCmdBindDescriptorSets) F(vkCmdDispatch) F(vkCmdClearColorImage) F(vkCmdCopyImage) \
	F(vkCmdBlitImage)
#define DECLARE(name) PFN_##name name;
	FSR_FUNCTIONS(DECLARE)
#undef DECLARE
} fsr;

/* ---------- images */

static int memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < fsr.memory.memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (fsr.memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static void image_free(struct fsr_image *image)
{
	uint32_t level;

	for (level = 0; level < SPD_LEVELS; level++)
	{
		if (image->levels[level])
			fsr.vkDestroyImageView(fsr.device, image->levels[level], NULL);
	}
	if (image->view)
		fsr.vkDestroyImageView(fsr.device, image->view, NULL);
	if (image->image)
		fsr.vkDestroyImage(fsr.device, image->image, NULL);
	if (image->memory)
		fsr.vkFreeMemory(fsr.device, image->memory, NULL);
	memset(image, 0, sizeof(*image));
}

/* (once the work that used it is done) */
static void image_release(struct fsr_image *image)
{
	if (!image->image)
		return;
	if (fsr.garbage_count == GARBAGE)
	{
		/* (full: the oldest waited for) */
		xgpu_vulkan_wait(fsr.garbage[0].serial);
		image_free(&fsr.garbage[0].image);
		memmove(&fsr.garbage[0], &fsr.garbage[1], (GARBAGE - 1) * sizeof(fsr.garbage[0]));
		fsr.garbage_count--;
	}
	fsr.garbage[fsr.garbage_count].image = *image;
	fsr.garbage[fsr.garbage_count].serial = xgpu_vulkan_serial();
	fsr.garbage_count++;
	memset(image, 0, sizeof(*image));
}

static void garbage_collect(void)
{
	uint64_t completed = xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < fsr.garbage_count)
	{
		if (fsr.garbage[index].serial <= completed)
		{
			image_free(&fsr.garbage[index].image);
			fsr.garbage[index] = fsr.garbage[--fsr.garbage_count];
		}
		else
		{
			index++;
		}
	}
}

static VkImageView view_make(struct fsr_image *image, uint32_t level, uint32_t count)
{
	VkImageViewCreateInfo info;
	VkImageView view = VK_NULL_HANDLE;

	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	info.image = image->image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = image->format;
	info.subresourceRange.aspectMask = image->aspect;
	info.subresourceRange.baseMipLevel = level;
	info.subresourceRange.levelCount = count;
	info.subresourceRange.layerCount = 1;
	if (fsr.vkCreateImageView(fsr.device, &info, NULL, &view) != VK_SUCCESS)
		return VK_NULL_HANDLE;
	return view;
}

/* an image of a format and size, levels of it (0: as many as it has), into
the GENERAL layout, cleared to zero, with the work recorded so far */
static int image_make(VkCommandBuffer commands, struct fsr_image *image, VkFormat format, uint32_t width,
	uint32_t height, uint32_t levels)
{
	VkImageCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo allocate;
	VkImageMemoryBarrier transition;
	int depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
		format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM;
	int stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
	/* (the inputs' copies are sampled; the rest written too) */
	int storage = !depth && format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R16G16B16A16_UNORM &&
		format != VK_FORMAT_A2B10G10R10_UNORM_PACK32;
	int type;
	uint32_t level;

	memset(image, 0, sizeof(*image));
	if (!width)
		width = 1;
	if (!height)
		height = 1;
	if (!levels)
	{
		uint32_t largest = width > height ? width : height;

		levels = 1;
		while ((largest >>= 1) != 0)
			levels++;
	}
	image->format = format;
	image->width = width;
	image->height = height;
	image->level_count = levels;
	image->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = format;
	info.extent.width = width;
	info.extent.height = height;
	info.extent.depth = 1;
	info.mipLevels = levels;
	info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
		(storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (fsr.vkCreateImage(fsr.device, &info, NULL, &image->image) != VK_SUCCESS)
	{
		image->image = VK_NULL_HANDLE;
		return 0;
	}
	fsr.vkGetImageMemoryRequirements(fsr.device, image->image, &requirements);
	type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0)
		type = memory_type(requirements.memoryTypeBits, 0);
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || fsr.vkAllocateMemory(fsr.device, &allocate, NULL, &image->memory) != VK_SUCCESS ||
		fsr.vkBindImageMemory(fsr.device, image->image, image->memory, 0) != VK_SUCCESS)
	{
		image->memory = VK_NULL_HANDLE;
		image_free(image);
		return 0;
	}
	image->view = view_make(image, 0, levels);
	if (!image->view)
	{
		image_free(image);
		return 0;
	}
	for (level = 0; level < SPD_LEVELS && levels > 1; level++)
	{
		image->levels[level] = view_make(image, level < levels ? level : levels - 1, 1);
		if (!image->levels[level])
		{
			image_free(image);
			return 0;
		}
	}
	memset(&transition, 0, sizeof(transition));
	transition.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	transition.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	transition.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	transition.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	transition.srcQueueFamilyIndex = transition.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	transition.image = image->image;
	transition.subresourceRange.aspectMask = image->aspect | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
	transition.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
	transition.subresourceRange.layerCount = 1;
	fsr.vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL,
		0, NULL, 1, &transition);
	if (!depth)
	{
		VkClearColorValue zero;

		memset(&zero, 0, sizeof(zero));
		fsr.vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1,
			&transition.subresourceRange);
	}
	return 1;
}

static void clear(VkCommandBuffer commands, struct fsr_image *image, float r, float g)
{
	VkClearColorValue value;
	VkImageSubresourceRange range;

	memset(&value, 0, sizeof(value));
	value.float32[0] = r;
	value.float32[1] = g;
	/* (an integer image's: its bits) */
	if (image->format == VK_FORMAT_R32_UINT)
		value.uint32[0] = (uint32_t)r;
	memset(&range, 0, sizeof(range));
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = VK_REMAINING_MIP_LEVELS;
	range.layerCount = 1;
	fsr.vkCmdClearColorImage(commands, image->image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
}

static void memory_barrier(VkCommandBuffer commands, VkPipelineStageFlags from, VkPipelineStageFlags to)
{
	VkMemoryBarrier memory;

	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	fsr.vkCmdPipelineBarrier(commands, from, to, 0, 1, &memory, 0, NULL, 0, NULL);
}

/* ---------- a window's context (the SDK's fsr3upscalerCreate) */

static void context_free(struct fsr_context *context)
{
	int index;

	for (index = 0; index < IMAGE_COUNT; index++)
		image_release(&context->images[index]);
	memset(context, 0, sizeof(*context));
}

static int context_make(VkCommandBuffer commands, struct fsr_context *context, uint32_t source_width,
	uint32_t source_height, uint32_t destination_width, uint32_t destination_height, VkFormat color_format,
	VkFormat depth_format)
{
	static const struct
	{
		uint8_t image;
		/* 0 the source's size, 1 half of it, 2 the destination's, 3 one
		texel */
		uint8_t size;
		uint8_t levels;
		VkFormat format;
	} made[] = {
		{ IMAGE_ACCUMULATION_1, 0, 1, VK_FORMAT_R8_UNORM },
		{ IMAGE_ACCUMULATION_2, 0, 1, VK_FORMAT_R8_UNORM },
		{ IMAGE_LUMA_1, 0, 1, VK_FORMAT_R16_SFLOAT },
		{ IMAGE_LUMA_2, 0, 1, VK_FORMAT_R16_SFLOAT },
		{ IMAGE_INTERMEDIATE, 0, 1, VK_FORMAT_R16_SFLOAT },
		{ IMAGE_SHADING_CHANGE, 1, 1, VK_FORMAT_R8_UNORM },
		{ IMAGE_NEW_LOCKS, 2, 1, VK_FORMAT_R8_UNORM },
		{ IMAGE_UPSCALED_1, 2, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ IMAGE_UPSCALED_2, 2, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ IMAGE_SPD_MIPS, 1, 0, VK_FORMAT_R16G16_SFLOAT },
		{ IMAGE_FARTHEST_DEPTH_MIP1, 1, 1, VK_FORMAT_R16_SFLOAT },
		{ IMAGE_LUMA_HISTORY_1, 0, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ IMAGE_LUMA_HISTORY_2, 0, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ IMAGE_SPD_ATOMIC, 3, 1, VK_FORMAT_R32_UINT },
		{ IMAGE_DILATED_REACTIVE_MASKS, 0, 1, VK_FORMAT_R8G8B8A8_UNORM },
		{ IMAGE_DEFAULT_REACTIVITY, 3, 1, VK_FORMAT_R8_UNORM },
		{ IMAGE_DEFAULT_EXPOSURE, 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT },
		{ IMAGE_FRAME_INFO, 3, 1, VK_FORMAT_R32G32B32A32_SFLOAT },
		{ IMAGE_DILATED_MOTION_VECTORS, 0, 1, VK_FORMAT_R16G16_SFLOAT },
		{ IMAGE_DILATED_DEPTH, 0, 1, VK_FORMAT_R32_SFLOAT },
		{ IMAGE_RECONSTRUCTED_DEPTH, 0, 1, VK_FORMAT_R32_UINT },
		{ IMAGE_OUTPUT, 2, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
		{ IMAGE_REACTIVE, 0, 1, VK_FORMAT_R32_SFLOAT },
		{ IMAGE_MOTION, 0, 1, VK_FORMAT_R16G16B16A16_SFLOAT },
	};
	uint32_t index;

	context_free(context);
	for (index = 0; index < sizeof(made) / sizeof(made[0]); index++)
	{
		uint32_t width = made[index].size == 0 ? source_width : made[index].size == 1 ? source_width / 2 :
			made[index].size == 2 ? destination_width : 1;
		uint32_t height = made[index].size == 0 ? source_height : made[index].size == 1 ? source_height / 2 :
			made[index].size == 2 ? destination_height : 1;

		if (!image_make(commands, &context->images[made[index].image], made[index].format, width, height,
			made[index].levels))
			break;
	}
	if (index == sizeof(made) / sizeof(made[0]) &&
		image_make(commands, &context->images[IMAGE_COLOR], color_format, source_width, source_height, 1) &&
		image_make(commands, &context->images[IMAGE_OPAQUE], color_format, source_width, source_height, 1) &&
		image_make(commands, &context->images[IMAGE_DEPTH], depth_format, source_width, source_height, 1))
	{
		context->made = 1;
		context->first = 1;
		context->source_width = source_width;
		context->source_height = source_height;
		context->destination_width = destination_width;
		context->destination_height = destination_height;
		context->color_format = color_format;
		context->depth_format = depth_format;
		context->constants.max_upscale_size[0] = (int32_t)destination_width;
		context->constants.max_upscale_size[1] = (int32_t)destination_height;
		context->constants.velocity_factor = 1.0f;
		context->constants.reactiveness_scale = 1.0f;
		context->constants.shading_change_scale = 1.0f;
		context->constants.accumulation_added_per_frame = 1.0f / 3.0f;
		context->constants.min_disocclusion_accumulation = -1.0f / 3.0f;
		host_logf(HOST_LOG_INFO, "vulkan: display.temporal_aa \"fsr\": a window's history made, %ux%u to %ux%u",
			source_width, source_height, destination_width, destination_height);
		return 1;
	}
	context_free(context);
	return 0;
}

/* ---------- the passes */

static int pipelines_make(int hdr)
{
	int pass;

	if (fsr.pipelines_made[hdr])
		return fsr.pipelines_made[hdr] > 0;
	fsr.pipelines_made[hdr] = -1;
	for (pass = 0; pass < XGPU_VULKAN_FSR_PASS_COUNT; pass++)
	{
		const struct xgpu_vulkan_fsr_program *program = &xgpu_vulkan_fsr_programs[hdr][pass];
		VkShaderModuleCreateInfo module_info;
		VkComputePipelineCreateInfo info;
		VkShaderModule module;
		VkResult result;

		memset(&module_info, 0, sizeof(module_info));
		module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		module_info.codeSize = program->count * 4;
		module_info.pCode = program->words;
		if (fsr.vkCreateShaderModule(fsr.device, &module_info, NULL, &module) != VK_SUCCESS)
			return 0;
		memset(&info, 0, sizeof(info));
		info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
		info.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		info.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		info.stage.module = module;
		info.stage.pName = "main";
		info.layout = fsr.layouts[pass];
		result = fsr.vkCreateComputePipelines(fsr.device, fsr.cache, 1, &info, NULL, &fsr.pipelines[hdr][pass]);
		fsr.vkDestroyShaderModule(fsr.device, module, NULL);
		if (result != VK_SUCCESS)
		{
			fsr.pipelines[hdr][pass] = VK_NULL_HANDLE;
			return 0;
		}
	}
	fsr.pipelines_made[hdr] = 1;
	return 1;
}

void xgpu_vulkan_fsr_prepare(void)
{
	if (fsr.ready)
	{
		(void)pipelines_make(0);
		(void)pipelines_make(1);
	}
}

static VkDescriptorPool pool_get(void)
{
	uint64_t serial = xgpu_vulkan_serial(), completed = xgpu_vulkan_completed();
	int attempt;

	if (fsr.pools[fsr.pool].pool && fsr.pools[fsr.pool].serial == serial)
		return fsr.pools[fsr.pool].pool;
	for (attempt = 0; attempt < POOLS; attempt++)
	{
		int index = (fsr.pool + 1 + attempt) % POOLS;

		if (!fsr.pools[index].pool)
		{
			VkDescriptorPoolSize sizes[4];
			VkDescriptorPoolCreateInfo info;

			sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
			sizes[0].descriptorCount = POOL_SETS * 16;
			sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			sizes[1].descriptorCount = POOL_SETS * 16;
			sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			sizes[2].descriptorCount = POOL_SETS * 2;
			sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLER;
			sizes[3].descriptorCount = POOL_SETS * 2;
			memset(&info, 0, sizeof(info));
			info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
			info.maxSets = POOL_SETS;
			info.poolSizeCount = 4;
			info.pPoolSizes = sizes;
			if (fsr.vkCreateDescriptorPool(fsr.device, &info, NULL, &fsr.pools[index].pool) != VK_SUCCESS)
			{
				fsr.pools[index].pool = VK_NULL_HANDLE;
				return VK_NULL_HANDLE;
			}
		}
		else if (fsr.pools[index].serial > completed)
		{
			continue;
		}
		else
		{
			fsr.vkResetDescriptorPool(fsr.device, fsr.pools[index].pool, 0);
		}
		fsr.pools[index].serial = serial;
		fsr.pool = index;
		return fsr.pools[index].pool;
	}
	return VK_NULL_HANDLE;
}

/* what a frame's dispatches bind: the window's context, which of each pair
is read (odd), the inputs (copies or the target's own), the uniform blocks */
struct dispatch_state
{
	struct fsr_context *context;
	int odd, reactive;
	VkImageView color, depth, motion;
	struct xgpu_vulkan_range blocks[3];
};

static VkImageView resource_view(const struct dispatch_state *state, int kind, int resource)
{
	struct fsr_context *context = state->context;
	struct fsr_image *images = context->images;
	int odd = state->odd;

	switch (resource)
	{
	case XGPU_VULKAN_FSR_INPUT_COLOR: return state->color;
	case XGPU_VULKAN_FSR_INPUT_DEPTH: return state->depth;
	case XGPU_VULKAN_FSR_INPUT_MOTION_VECTORS: return state->motion;
	case XGPU_VULKAN_FSR_INPUT_OPAQUE_ONLY: return images[IMAGE_OPAQUE].view;
	/* (no auto exposure: the default, 0, which the passes take as 1) */
	case XGPU_VULKAN_FSR_INPUT_EXPOSURE: return images[IMAGE_DEFAULT_EXPOSURE].view;
	case XGPU_VULKAN_FSR_FRAME_INFO: return images[IMAGE_FRAME_INFO].view;
	case XGPU_VULKAN_FSR_REACTIVE_MASK:
		return images[state->reactive ? IMAGE_REACTIVE : IMAGE_DEFAULT_REACTIVITY].view;
	case XGPU_VULKAN_FSR_TRANSPARENCY_AND_COMPOSITION_MASK: return images[IMAGE_DEFAULT_REACTIVITY].view;
	case XGPU_VULKAN_FSR_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH: return images[IMAGE_RECONSTRUCTED_DEPTH].view;
	case XGPU_VULKAN_FSR_DILATED_MOTION_VECTORS: return images[IMAGE_DILATED_MOTION_VECTORS].view;
	case XGPU_VULKAN_FSR_DILATED_DEPTH: return images[IMAGE_DILATED_DEPTH].view;
	/* (the pairs: the frame before's read, this frame's written) */
	case XGPU_VULKAN_FSR_INTERNAL_UPSCALED:
		return images[(kind == XGPU_VULKAN_FSR_TEXTURE) == odd ? IMAGE_UPSCALED_2 : IMAGE_UPSCALED_1].view;
	case XGPU_VULKAN_FSR_ACCUMULATION:
		return images[(kind == XGPU_VULKAN_FSR_TEXTURE) == odd ? IMAGE_ACCUMULATION_2 : IMAGE_ACCUMULATION_1].view;
	case XGPU_VULKAN_FSR_LUMA_HISTORY:
		return images[(kind == XGPU_VULKAN_FSR_TEXTURE) == odd ? IMAGE_LUMA_HISTORY_2 : IMAGE_LUMA_HISTORY_1].view;
	case XGPU_VULKAN_FSR_CURRENT_LUMA: return images[odd ? IMAGE_LUMA_2 : IMAGE_LUMA_1].view;
	case XGPU_VULKAN_FSR_PREVIOUS_LUMA: return images[odd ? IMAGE_LUMA_1 : IMAGE_LUMA_2].view;
	case XGPU_VULKAN_FSR_SPD_MIPS: return images[IMAGE_SPD_MIPS].view;
	case XGPU_VULKAN_FSR_SPD_MIP0: case XGPU_VULKAN_FSR_SPD_MIP1: case XGPU_VULKAN_FSR_SPD_MIP2:
	case XGPU_VULKAN_FSR_SPD_MIP3: case XGPU_VULKAN_FSR_SPD_MIP4: case XGPU_VULKAN_FSR_SPD_MIP5:
		return images[IMAGE_SPD_MIPS].levels[resource - XGPU_VULKAN_FSR_SPD_MIP0];
	case XGPU_VULKAN_FSR_SPD_ATOMIC: return images[IMAGE_SPD_ATOMIC].view;
	case XGPU_VULKAN_FSR_DILATED_REACTIVE_MASKS: return images[IMAGE_DILATED_REACTIVE_MASKS].view;
	case XGPU_VULKAN_FSR_NEW_LOCKS: return images[IMAGE_NEW_LOCKS].view;
	/* (the farthest depth and the luma's instability share one image, as
	the SDK's do) */
	case XGPU_VULKAN_FSR_FARTHEST_DEPTH: case XGPU_VULKAN_FSR_LUMA_INSTABILITY:
		return images[IMAGE_INTERMEDIATE].view;
	case XGPU_VULKAN_FSR_FARTHEST_DEPTH_MIP1: return images[IMAGE_FARTHEST_DEPTH_MIP1].view;
	case XGPU_VULKAN_FSR_SHADING_CHANGE: return images[IMAGE_SHADING_CHANGE].view;
	case XGPU_VULKAN_FSR_UPSCALED_OUTPUT: return images[IMAGE_OUTPUT].view;
	case XGPU_VULKAN_FSR_AUTOREACTIVE: case XGPU_VULKAN_FSR_AUTOCOMPOSITION: return images[IMAGE_REACTIVE].view;
	default: return VK_NULL_HANDLE;
	}
}

static int dispatch(VkCommandBuffer commands, const struct dispatch_state *state, int hdr, int pass, uint32_t x,
	uint32_t y)
{
	const struct xgpu_vulkan_fsr_program *program = &xgpu_vulkan_fsr_programs[hdr][pass];
	VkDescriptorImageInfo images[24];
	VkDescriptorBufferInfo buffers[4];
	VkWriteDescriptorSet writes[24];
	VkDescriptorSetAllocateInfo allocate;
	VkDescriptorSet set;
	uint32_t index, image_count = 0, buffer_count = 0;

	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = pool_get();
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &fsr.set_layouts[pass];
	if (!allocate.descriptorPool || program->binding_count > 24 ||
		fsr.vkAllocateDescriptorSets(fsr.device, &allocate, &set) != VK_SUCCESS)
		return 0;
	memset(writes, 0, sizeof(writes));
	memset(images, 0, sizeof(images));
	for (index = 0; index < program->binding_count; index++)
	{
		const struct xgpu_vulkan_fsr_binding *binding = &program->bindings[index];
		VkWriteDescriptorSet *write = &writes[index];

		write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write->dstSet = set;
		write->dstBinding = binding->binding;
		write->descriptorCount = 1;
		if (binding->kind == XGPU_VULKAN_FSR_BUFFER)
		{
			int block = binding->resource == XGPU_VULKAN_FSR_CONSTANTS ? 0 :
				binding->resource == XGPU_VULKAN_FSR_SPD_CONSTANTS ? 1 : 2;

			if (buffer_count == 4)
				return 0;
			buffers[buffer_count].buffer = state->blocks[block].buffer;
			buffers[buffer_count].offset = state->blocks[block].offset;
			buffers[buffer_count].range = 256;
			write->descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			write->pBufferInfo = &buffers[buffer_count++];
			continue;
		}
		images[image_count].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
		if (binding->kind == XGPU_VULKAN_FSR_SAMPLER)
		{
			images[image_count].sampler = binding->resource == XGPU_VULKAN_FSR_POINT_SAMPLER ? fsr.point : fsr.linear;
			write->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
		}
		else
		{
			images[image_count].imageView = resource_view(state, binding->kind, binding->resource);
			if (!images[image_count].imageView)
			{
				if (!fsr.failed_said)
					host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"fsr\": pass %d's binding %u has no "
						"resource (%u)", pass, (unsigned)binding->binding, (unsigned)binding->resource);
				fsr.failed_said = 1;
				return 0;
			}
			write->descriptorType = binding->kind == XGPU_VULKAN_FSR_TEXTURE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
				VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		}
		write->pImageInfo = &images[image_count++];
	}
	fsr.vkUpdateDescriptorSets(fsr.device, (uint32_t)program->binding_count, writes, 0, NULL);
	fsr.vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.pipelines[hdr][pass]);
	fsr.vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.layouts[pass], 0, 1, &set, 0, NULL);
	fsr.vkCmdDispatch(commands, x ? x : 1, y ? y : 1, 1);
	memory_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	return 1;
}

/* the rectangle (left, top, right, bottom) of an image copied to another's
top left corner */
static void copy_to_corner(VkCommandBuffer commands, VkImage from, VkImageAspectFlags aspect, const int32_t rectangle[4],
	struct fsr_image *to)
{
	VkImageCopy copy;

	memset(&copy, 0, sizeof(copy));
	copy.srcSubresource.aspectMask = copy.dstSubresource.aspectMask = aspect;
	copy.srcSubresource.layerCount = copy.dstSubresource.layerCount = 1;
	copy.srcOffset.x = rectangle[0];
	copy.srcOffset.y = rectangle[1];
	copy.extent.width = (uint32_t)(rectangle[2] - rectangle[0]);
	copy.extent.height = (uint32_t)(rectangle[3] - rectangle[1]);
	copy.extent.depth = 1;
	fsr.vkCmdCopyImage(commands, from, VK_IMAGE_LAYOUT_GENERAL, to->image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
}

/* the SDK's setupDeviceDepthToViewSpaceDepthParams: the depth reversed, its
far plane not infinite */
static void depth_parameters(struct fsr_constants *constants, const struct xgpu_vulkan_fsr_frame *frame, float aspect)
{
	float nearest = frame->near_plane < frame->far_plane ? frame->near_plane : frame->far_plane;
	float farthest = frame->near_plane < frame->far_plane ? frame->far_plane : frame->near_plane;
	/* (reversed: the two swapped) */
	float minimum = farthest, maximum = nearest;
	float q = maximum / (minimum - maximum);
	float cotangent = cosf(0.5f * frame->field_of_view) / sinf(0.5f * frame->field_of_view);

	constants->device_to_view_depth[0] = -q;
	constants->device_to_view_depth[1] = q * minimum;
	constants->device_to_view_depth[2] = aspect / cotangent;
	constants->device_to_view_depth[3] = 1.0f / cotangent;
}

/* (a frame FSR cannot do: the view scaled into the destination as it is,
linearly, rather than nothing) */
static void fallback_blit(const struct xgpu_vulkan_fsr_frame *frame)
{
	struct xgpu_vulkan_image *read[1], *written[1];
	VkCommandBuffer commands;
	VkImageBlit blit;

	if (!fsr.vkCmdBlitImage)
		return;
	read[0] = frame->color;
	written[0] = frame->destination;
	commands = xgpu_vulkan_outside_begin(read, 1, written, 1);
	memset(&blit, 0, sizeof(blit));
	blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
	blit.srcOffsets[0].x = frame->source[0];
	blit.srcOffsets[0].y = frame->source[1];
	blit.srcOffsets[1].x = frame->source[2];
	blit.srcOffsets[1].y = frame->source[3];
	blit.srcOffsets[1].z = 1;
	blit.dstOffsets[0].x = frame->destination_rectangle[0];
	blit.dstOffsets[0].y = frame->destination_rectangle[1];
	blit.dstOffsets[1].x = frame->destination_rectangle[2];
	blit.dstOffsets[1].y = frame->destination_rectangle[3];
	blit.dstOffsets[1].z = 1;
	/* (the same image at the native scale: nothing to do) */
	if (frame->color != frame->destination)
		fsr.vkCmdBlitImage(commands, xgpu_vulkan_image_handle(frame->color), VK_IMAGE_LAYOUT_GENERAL,
			xgpu_vulkan_image_handle(frame->destination), VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
	xgpu_vulkan_outside_end(commands);
}

static struct fsr_context *context_for(int window)
{
	return window >= 0 && window < XGPU_VULKAN_FSR_WINDOWS ? &fsr.contexts[window] : NULL;
}

void xgpu_vulkan_fsr_opaque(int window, struct xgpu_vulkan_image *color, const int32_t rectangle[4])
{
	struct fsr_context *context = context_for(window);
	struct xgpu_vulkan_image *read[1];
	VkCommandBuffer commands;

	read[0] = color;
	if (!fsr.ready || !context || !context->made || !color || rectangle[2] <= rectangle[0] ||
		rectangle[3] <= rectangle[1] || xgpu_vulkan_image_format(color) != context->color_format ||
		xgpu_vulkan_image_description(color)->width != context->source_width ||
		xgpu_vulkan_image_description(color)->height != context->source_height)
		return;
	xgpu_vulkan_pass_owner("FSR");
	commands = xgpu_vulkan_outside_begin(read, 1, NULL, 0);
	copy_to_corner(commands, xgpu_vulkan_image_handle(color), VK_IMAGE_ASPECT_COLOR_BIT, rectangle,
		&context->images[IMAGE_OPAQUE]);
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	context->opaque_serial = xgpu_vulkan_serial();
	context->opaque_size[0] = rectangle[2] - rectangle[0];
	context->opaque_size[1] = rectangle[3] - rectangle[1];
}

static int upscale(int window, const struct xgpu_vulkan_fsr_frame *frame);

int xgpu_vulkan_fsr_upscale(int window, const struct xgpu_vulkan_fsr_frame *frame)
{
	double started = host_pass_times > 0 ? host_load_trace_clock(CLOCK_MONOTONIC) : 0.0;
	int done = upscale(window, frame);

	if (!done && frame->color && frame->destination)
		fallback_blit(frame);
	/* (debug.pass_times: its CPU's share too, every 300 frames) */
	if (host_pass_times > 0)
	{
		fsr.recording += host_load_trace_clock(CLOCK_MONOTONIC) - started;
		if (++fsr.recorded == 300)
		{
			host_logf(HOST_LOG_INFO, "vulkan: display.temporal_aa \"fsr\": %.3f ms of the render thread a window's "
				"frame, recording its passes", fsr.recording * 1000.0 / fsr.recorded);
			fsr.recording = 0.0;
			fsr.recorded = 0;
		}
	}
	return done;
}

static int upscale(int window, const struct xgpu_vulkan_fsr_frame *frame)
{
	struct fsr_context *context = context_for(window);
	struct fsr_constants *constants;
	struct dispatch_state state;
	struct fsr_spd_constants spd;
	struct fsr_reactive_constants reactive;
	struct xgpu_vulkan_image *read[3], *written[1];
	const struct xgpu_vulkan_image_description *source, *destination;
	VkCommandBuffer commands;
	VkImageBlit blit;
	int32_t render[2], upscale[2];
	uint32_t groups_x, groups_y, largest, mips, phases;
	int staged, reset, hdr = frame->hdr != 0, ok = 1;
	void *bytes;
	int index;

	if (!fsr.ready || !context || !frame->color || !frame->depth || !frame->motion || !frame->destination)
		return 0;
	source = xgpu_vulkan_image_description(frame->color);
	destination = xgpu_vulkan_image_description(frame->destination);
	render[0] = frame->source[2] - frame->source[0];
	render[1] = frame->source[3] - frame->source[1];
	upscale[0] = frame->destination_rectangle[2] - frame->destination_rectangle[0];
	upscale[1] = frame->destination_rectangle[3] - frame->destination_rectangle[1];
	if (render[0] < 8 || render[1] < 8 || upscale[0] < 8 || upscale[1] < 8 || frame->source[0] < 0 ||
		frame->source[1] < 0 || frame->source[2] > (int32_t)source->width || frame->source[3] > (int32_t)source->height ||
		frame->destination_rectangle[0] < 0 || frame->destination_rectangle[1] < 0 ||
		frame->destination_rectangle[2] > (int32_t)destination->width ||
		frame->destination_rectangle[3] > (int32_t)destination->height ||
		xgpu_vulkan_image_description(frame->motion)->width != source->width ||
		xgpu_vulkan_image_description(frame->motion)->height != source->height ||
		xgpu_vulkan_image_description(frame->depth)->width != source->width ||
		xgpu_vulkan_image_description(frame->depth)->height != source->height)
		return 0;
	garbage_collect();
	if (!pipelines_make(hdr))
	{
		if (!fsr.failed_said)
			host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"fsr\": its passes cannot be made");
		fsr.failed_said = 1;
		return 0;
	}
	xgpu_vulkan_pass_owner("FSR");
	read[0] = frame->color;
	read[1] = frame->depth;
	read[2] = frame->motion;
	written[0] = frame->destination;
	commands = xgpu_vulkan_outside_begin(read, 3, written, 1);
	if (!context->made || context->source_width != source->width || context->source_height != source->height ||
		context->destination_width != destination->width || context->destination_height != destination->height ||
		context->color_format != xgpu_vulkan_image_format(frame->color) ||
		context->depth_format != xgpu_vulkan_image_format(frame->depth))
	{
		if (!context_make(commands, context, source->width, source->height, destination->width, destination->height,
			xgpu_vulkan_image_format(frame->color), xgpu_vulkan_image_format(frame->depth)))
		{
			if (!fsr.failed_said)
				host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"fsr\": a window's history cannot be made");
			fsr.failed_said = 1;
			xgpu_vulkan_outside_end(commands);
			xgpu_vulkan_pass_owner(NULL);
			return 0;
		}
		memory_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT |
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	}
	memset(&state, 0, sizeof(state));
	state.context = context;
	state.odd = (int)(context->frame & 1);
	/* (the inputs: from the top left corner, where the passes read them) */
	staged = frame->source[0] != 0 || frame->source[1] != 0;
	if (staged)
	{
		copy_to_corner(commands, xgpu_vulkan_image_handle(frame->color), VK_IMAGE_ASPECT_COLOR_BIT, frame->source,
			&context->images[IMAGE_COLOR]);
		copy_to_corner(commands, xgpu_vulkan_image_handle(frame->depth), VK_IMAGE_ASPECT_DEPTH_BIT, frame->source,
			&context->images[IMAGE_DEPTH]);
		copy_to_corner(commands, xgpu_vulkan_image_handle(frame->motion), VK_IMAGE_ASPECT_COLOR_BIT, frame->source,
			&context->images[IMAGE_MOTION]);
		state.color = context->images[IMAGE_COLOR].view;
		state.depth = context->images[IMAGE_DEPTH].view;
		state.motion = context->images[IMAGE_MOTION].view;
	}
	else
	{
		state.color = xgpu_vulkan_image_view(frame->color);
		state.depth = xgpu_vulkan_image_view(frame->depth);
		state.motion = xgpu_vulkan_image_view(frame->motion);
	}
	/* (the reactive mask where this frame's opaque view was kept, of the
	same window) */
	state.reactive = context->opaque_serial == xgpu_vulkan_serial() && context->opaque_size[0] == render[0] &&
		context->opaque_size[1] == render[1];
	context->opaque_serial = 0;

	/* the clears (fsr3upscalerDispatch's) */
	reset = frame->reset || context->first;
	if (context->first)
	{
		clear(commands, &context->images[IMAGE_ACCUMULATION_1], 0.0f, 0.0f);
		clear(commands, &context->images[IMAGE_ACCUMULATION_2], 0.0f, 0.0f);
		clear(commands, &context->images[IMAGE_LUMA_1], 0.0f, 0.0f);
		clear(commands, &context->images[IMAGE_LUMA_2], 0.0f, 0.0f);
	}
	context->first = 0;
	if (reset)
	{
		clear(commands, &context->images[state.odd ? IMAGE_ACCUMULATION_2 : IMAGE_ACCUMULATION_1], 0.0f, 0.0f);
		clear(commands, &context->images[IMAGE_FRAME_INFO], -1.0f, 1.0f);
	}
	/* (the reversed depth's nearest: 0) */
	clear(commands, &context->images[IMAGE_RECONSTRUCTED_DEPTH], 0.0f, 0.0f);
	clear(commands, &context->images[IMAGE_SPD_ATOMIC], 0.0f, 0.0f);
	clear(commands, &context->images[IMAGE_SPD_MIPS], 0.0f, 0.0f);
	memory_barrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

	/* the constants */
	constants = &context->constants;
	memcpy(constants->previous_jitter, constants->jitter, sizeof(constants->jitter));
	constants->jitter[0] = frame->jitter[0];
	constants->jitter[1] = frame->jitter[1];
	memcpy(constants->previous_render_size, constants->render_size, sizeof(constants->render_size));
	constants->render_size[0] = render[0];
	constants->render_size[1] = render[1];
	constants->max_render_size[0] = (int32_t)context->source_width;
	constants->max_render_size[1] = (int32_t)context->source_height;
	/* (the horizontal field of view's tangent from the vertical's) */
	constants->tan_half_fov = tanf(0.5f * frame->field_of_view) * (float)render[0] / (float)render[1];
	constants->view_space_to_meters = frame->meters > 0.0f ? frame->meters : 1.0f;
	depth_parameters(constants, frame, (float)render[0] / (float)render[1]);
	memcpy(constants->previous_upscale_size, constants->upscale_size, sizeof(constants->upscale_size));
	constants->upscale_size[0] = upscale[0];
	constants->upscale_size[1] = upscale[1];
	constants->downscale_factor[0] = (float)render[0] / (float)upscale[0];
	constants->downscale_factor[1] = (float)render[1] / (float)upscale[1];
	constants->delta_pre_exposure = 1.0f;
	/* (the motion a fraction of the view already: the SDK's scale, the
	view's pixels, over the render size) */
	constants->motion_vector_scale[0] = constants->motion_vector_scale[1] = 1.0f;
	constants->motion_vector_jitter_cancellation[0] = constants->motion_vector_jitter_cancellation[1] = 0.0f;
	phases = (uint32_t)(8.0f * ((float)upscale[0] / (float)render[0]) * ((float)upscale[0] / (float)render[0]));
	if (reset || constants->jitter_phase_count == 0.0f)
		constants->jitter_phase_count = (float)phases;
	else if ((float)phases > constants->jitter_phase_count)
		constants->jitter_phase_count += 1.0f;
	else if ((float)phases < constants->jitter_phase_count)
		constants->jitter_phase_count -= 1.0f;
	constants->delta_time = 1.0f / 60.0f;
	constants->frame_index = reset ? 0.0f : constants->frame_index + 1.0f;
	/* (the downsampler's: ffxSpdSetup over the render size) */
	groups_x = ((uint32_t)render[0] - 1) / 64 + 1;
	groups_y = ((uint32_t)render[1] - 1) / 64 + 1;
	largest = (uint32_t)(render[0] > render[1] ? render[0] : render[1]);
	for (mips = 0; largest > 1 && mips < 12; largest >>= 1)
		mips++;
	memset(&spd, 0, sizeof(spd));
	spd.mips = mips;
	spd.work_groups = groups_x * groups_y;
	spd.render_size[0] = (uint32_t)render[0];
	spd.render_size[1] = (uint32_t)render[1];
	memset(&reactive, 0, sizeof(reactive));
	reactive.scale = 1.0f;
	reactive.threshold = 0.2f;
	reactive.binary_value = 0.9f;
	reactive.flags = AUTOREACTIVE_APPLY_TONEMAP | AUTOREACTIVE_APPLY_THRESHOLD | AUTOREACTIVE_USE_COMPONENTS_MAX;
	for (index = 0; index < 3 && ok; index++)
	{
		bytes = xgpu_vulkan_upload(256, 256, &state.blocks[index]);
		if (!bytes)
		{
			ok = 0;
			break;
		}
		memset(bytes, 0, 256);
		memcpy(bytes, index == 0 ? (const void *)constants : index == 1 ? (const void *)&spd : (const void *)&reactive,
			index == 0 ? sizeof(*constants) : index == 1 ? sizeof(spd) : sizeof(reactive));
	}

	/* the passes */
	if (ok && state.reactive)
		ok = dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_AUTOGEN_REACTIVE, ((uint32_t)render[0] + 7) / 8,
			((uint32_t)render[1] + 7) / 8);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_PREPARE_INPUTS, ((uint32_t)render[0] + 7) / 8,
		((uint32_t)render[1] + 7) / 8);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_LUMA_PYRAMID, groups_x, groups_y);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_SHADING_CHANGE_PYRAMID, groups_x, groups_y);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_SHADING_CHANGE,
		((uint32_t)(render[0] / 2) + 7) / 8, ((uint32_t)(render[1] / 2) + 7) / 8);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_PREPARE_REACTIVITY, ((uint32_t)render[0] + 7) / 8,
		((uint32_t)render[1] + 7) / 8);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_LUMA_INSTABILITY, ((uint32_t)render[0] + 7) / 8,
		((uint32_t)render[1] + 7) / 8);
	ok = ok && dispatch(commands, &state, hdr, XGPU_VULKAN_FSR_PASS_ACCUMULATE, ((uint32_t)upscale[0] + 7) / 8,
		((uint32_t)upscale[1] + 7) / 8);

	/* the output into the destination's rectangle (its format's: a blit) */
	if (ok)
	{
		memory_barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		memset(&blit, 0, sizeof(blit));
		blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
		blit.srcOffsets[1].x = upscale[0];
		blit.srcOffsets[1].y = upscale[1];
		blit.srcOffsets[1].z = 1;
		blit.dstOffsets[0].x = frame->destination_rectangle[0];
		blit.dstOffsets[0].y = frame->destination_rectangle[1];
		blit.dstOffsets[1].x = frame->destination_rectangle[2];
		blit.dstOffsets[1].y = frame->destination_rectangle[3];
		blit.dstOffsets[1].z = 1;
		fsr.vkCmdBlitImage(commands, context->images[IMAGE_OUTPUT].image, VK_IMAGE_LAYOUT_GENERAL,
			xgpu_vulkan_image_handle(frame->destination), VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
		context->frame++;
	}
	else if (!fsr.failed_said)
	{
		host_logf(HOST_LOG_WARN, "vulkan: display.temporal_aa \"fsr\": a frame's passes cannot be recorded");
		fsr.failed_said = 1;
	}
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	return ok;
}

/* ---------- the device's */

int xgpu_vulkan_fsr_create(const struct xgpu_vulkan_fsr_device *device)
{
	VkSamplerCreateInfo sampler;
	int pass;

	memset(&fsr, 0, sizeof(fsr));
	fsr.device = device->device;
	fsr.memory = device->memory;
	fsr.cache = device->cache;
#define LOAD(name) fsr.name = (PFN_##name)device->get_device_proc_addr(device->device, #name); \
	if (!fsr.name) return 0;
	FSR_FUNCTIONS(LOAD)
#undef LOAD
	memset(&sampler, 0, sizeof(sampler));
	sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sampler.maxLod = 1000.0f;
	if (fsr.vkCreateSampler(fsr.device, &sampler, NULL, &fsr.point) != VK_SUCCESS)
		return 0;
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	if (fsr.vkCreateSampler(fsr.device, &sampler, NULL, &fsr.linear) != VK_SUCCESS)
	{
		xgpu_vulkan_fsr_destroy();
		return 0;
	}
	for (pass = 0; pass < XGPU_VULKAN_FSR_PASS_COUNT; pass++)
	{
		const struct xgpu_vulkan_fsr_program *program = &xgpu_vulkan_fsr_programs[0][pass];
		VkDescriptorSetLayoutBinding bindings[24];
		VkDescriptorSetLayoutCreateInfo set_info;
		VkPipelineLayoutCreateInfo layout_info;
		uint32_t index;

		if (program->binding_count > 24)
		{
			xgpu_vulkan_fsr_destroy();
			return 0;
		}
		memset(bindings, 0, sizeof(bindings));
		for (index = 0; index < program->binding_count; index++)
		{
			int kind = program->bindings[index].kind;

			bindings[index].binding = program->bindings[index].binding;
			bindings[index].descriptorType = kind == XGPU_VULKAN_FSR_TEXTURE ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE :
				kind == XGPU_VULKAN_FSR_IMAGE ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE :
				kind == XGPU_VULKAN_FSR_BUFFER ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_SAMPLER;
			bindings[index].descriptorCount = 1;
			bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
		}
		memset(&set_info, 0, sizeof(set_info));
		set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		set_info.bindingCount = (uint32_t)program->binding_count;
		set_info.pBindings = bindings;
		memset(&layout_info, 0, sizeof(layout_info));
		layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		layout_info.setLayoutCount = 1;
		layout_info.pSetLayouts = &fsr.set_layouts[pass];
		if (fsr.vkCreateDescriptorSetLayout(fsr.device, &set_info, NULL, &fsr.set_layouts[pass]) != VK_SUCCESS ||
			fsr.vkCreatePipelineLayout(fsr.device, &layout_info, NULL, &fsr.layouts[pass]) != VK_SUCCESS)
		{
			xgpu_vulkan_fsr_destroy();
			return 0;
		}
	}
	fsr.ready = 1;
	return 1;
}

/* (the device idle) */
void xgpu_vulkan_fsr_destroy(void)
{
	int index, hdr;

	if (!fsr.device || !fsr.vkDestroyImage)
	{
		memset(&fsr, 0, sizeof(fsr));
		return;
	}
	for (index = 0; index < XGPU_VULKAN_FSR_WINDOWS; index++)
	{
		int image;

		for (image = 0; image < IMAGE_COUNT; image++)
			image_free(&fsr.contexts[index].images[image]);
	}
	for (index = 0; index < (int)fsr.garbage_count; index++)
		image_free(&fsr.garbage[index].image);
	for (index = 0; index < POOLS; index++)
	{
		if (fsr.pools[index].pool)
			fsr.vkDestroyDescriptorPool(fsr.device, fsr.pools[index].pool, NULL);
	}
	for (hdr = 0; hdr < 2; hdr++)
	{
		for (index = 0; index < XGPU_VULKAN_FSR_PASS_COUNT; index++)
		{
			if (fsr.pipelines[hdr][index])
				fsr.vkDestroyPipeline(fsr.device, fsr.pipelines[hdr][index], NULL);
		}
	}
	for (index = 0; index < XGPU_VULKAN_FSR_PASS_COUNT; index++)
	{
		if (fsr.layouts[index])
			fsr.vkDestroyPipelineLayout(fsr.device, fsr.layouts[index], NULL);
		if (fsr.set_layouts[index])
			fsr.vkDestroyDescriptorSetLayout(fsr.device, fsr.set_layouts[index], NULL);
	}
	if (fsr.point)
		fsr.vkDestroySampler(fsr.device, fsr.point, NULL);
	if (fsr.linear)
		fsr.vkDestroySampler(fsr.device, fsr.linear, NULL);
	memset(&fsr, 0, sizeof(fsr));
}
