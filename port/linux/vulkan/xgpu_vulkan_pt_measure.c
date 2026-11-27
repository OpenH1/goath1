/*
XGPU_VULKAN_PT_MEASURE.C

Path tracing's stage 0, "Measure" (docs/rt-plan/4-path-tracing.md): the ray
throughput of the ray-traced scene (xgpu_vulkan_rt.c's, with
debug.rt_render_geometry the render meshes) on this GPU, with
HALO_PT_MEASURE_RAYS set (its value the rays a pixel, 4 by default; it
needs display.ambient_occlusion "ray_traced", whose pre-pass finds each
pixel's surface and whose pass builds the scene). Measurement only: nothing
it computes is drawn.

At the pre-pass's end (xgpu_vulkan_stream.c), a compute pass of its own
(pt/measure_rays.comp, tools/vulkan_pt_shaders.py) casts that many rays
from each pixel's surface, in turn a frame each: incoherent diffuse rays
(cosine about the normal, the nearest hit, every candidate opaque), the
same with the non-opaque candidates confirmed in a loop (the objects', the
cutouts': the loop's cost), and shadow rays (the first hit ends them). Its
GPU time (timestamps about the dispatch alone) and the rays it cast
(counted on the GPU) are read once the GPU is past it, never waited for,
and every PT_MEASURE_LOGGED frames of a kind logged: rays a second (all
rays over all time; and the median frame's), the pixels, the time a frame.
The pass is dispatched twice and the second timed alone (a timestamp before
the first may be written while the passes before it still run).
A measuring run has the GPU alone (goat-rig run without --shared).

Render thread alone, as the core.
*/

#include "host.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"

#include <stdlib.h>
#include <string.h>

#define PT_MEASURE_SLOTS 4u
#define PT_MEASURE_MODES 3u
#define PT_MEASURE_LOGGED 120u

extern const uint32_t xgpu_vulkan_pt_measure_rays_words[];
extern const uint32_t xgpu_vulkan_pt_measure_rays_count;

struct pt_measure_push
{
	float camera[4][4];
	int32_t corners[4];
	uint32_t parameters[4];
};

static const char *const pt_measure_mode_names[PT_MEASURE_MODES] =
{
	"diffuse, opaque", "diffuse, candidates confirmed", "shadow, first hit"
};

static struct
{
	int ready, wanted, failed;
	uint32_t rays_per_pixel;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	float timestamp_period;
	VkDescriptorSetLayout set_layout;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkDescriptorPool pool;
	VkDescriptorSet sets[PT_MEASURE_SLOTS];
	VkBuffer counts;
	VkDeviceMemory counts_memory;
	uint32_t *counts_mapped;
	VkQueryPool times;
	VkSampler sampler;
	/* each slot's submission (0 free), mode and pixels */
	uint64_t slot_serial[PT_MEASURE_SLOTS];
	uint32_t slot_mode[PT_MEASURE_SLOTS], slot_pixels[PT_MEASURE_SLOTS];
	uint32_t next_slot, frame;
	/* each mode's frames read: rays, seconds, each frame's rate */
	double rays[PT_MEASURE_MODES], seconds[PT_MEASURE_MODES];
	float rates[PT_MEASURE_MODES][PT_MEASURE_LOGGED];
	uint32_t pixels[PT_MEASURE_MODES], frames[PT_MEASURE_MODES], logged;
	PFN_vkCreateDescriptorSetLayout create_set_layout;
	PFN_vkDestroyDescriptorSetLayout destroy_set_layout;
	PFN_vkCreatePipelineLayout create_layout;
	PFN_vkDestroyPipelineLayout destroy_layout;
	PFN_vkCreateShaderModule create_module;
	PFN_vkDestroyShaderModule destroy_module;
	PFN_vkCreateComputePipelines create_pipeline;
	PFN_vkDestroyPipeline destroy_pipeline;
	PFN_vkCreateDescriptorPool create_pool;
	PFN_vkDestroyDescriptorPool destroy_pool;
	PFN_vkAllocateDescriptorSets allocate_sets;
	PFN_vkUpdateDescriptorSets update_sets;
	PFN_vkCreateBuffer create_buffer;
	PFN_vkDestroyBuffer destroy_buffer;
	PFN_vkGetBufferMemoryRequirements requirements;
	PFN_vkAllocateMemory allocate;
	PFN_vkFreeMemory free_memory;
	PFN_vkBindBufferMemory bind;
	PFN_vkMapMemory map;
	PFN_vkCreateQueryPool create_queries;
	PFN_vkDestroyQueryPool destroy_queries;
	PFN_vkCmdResetQueryPool reset_queries;
	PFN_vkCmdWriteTimestamp timestamp;
	PFN_vkGetQueryPoolResults query_results;
	PFN_vkCmdBindPipeline bind_pipeline;
	PFN_vkCmdBindDescriptorSets bind_sets;
	PFN_vkCmdPushConstants push;
	PFN_vkCmdDispatch dispatch;
} pt_measure;

static int pt_measure_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < pt_measure.memory.memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (pt_measure.memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

static int pt_measure_make(void)
{
	VkDescriptorSetLayoutBinding bindings[3];
	VkDescriptorSetLayoutCreateInfo set_layout;
	VkPushConstantRange range;
	VkPipelineLayoutCreateInfo layout;
	VkShaderModuleCreateInfo module_info;
	VkShaderModule module = VK_NULL_HANDLE;
	VkComputePipelineCreateInfo pipeline;
	VkDescriptorPoolSize sizes[3];
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetLayout layouts[PT_MEASURE_SLOTS];
	VkDescriptorSetAllocateInfo allocate;
	VkBufferCreateInfo buffer;
	VkMemoryRequirements requirements;
	VkMemoryAllocateInfo memory;
	VkQueryPoolCreateInfo queries;
	struct xgpu_vulkan_sampler_description sampler;
	uint32_t index;
	int type;

	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[2].binding = 2;
	bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	for (index = 0; index < 3; index++)
	{
		bindings[index].descriptorCount = 1;
		bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&set_layout, 0, sizeof(set_layout));
	set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	set_layout.bindingCount = 3;
	set_layout.pBindings = bindings;
	if (pt_measure.create_set_layout(pt_measure.device, &set_layout, NULL, &pt_measure.set_layout) != VK_SUCCESS)
		return 0;
	memset(&range, 0, sizeof(range));
	range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	range.size = sizeof(struct pt_measure_push);
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &pt_measure.set_layout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &range;
	if (pt_measure.create_layout(pt_measure.device, &layout, NULL, &pt_measure.layout) != VK_SUCCESS)
		return 0;
	memset(&module_info, 0, sizeof(module_info));
	module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	module_info.codeSize = xgpu_vulkan_pt_measure_rays_count * 4u;
	module_info.pCode = xgpu_vulkan_pt_measure_rays_words;
	if (pt_measure.create_module(pt_measure.device, &module_info, NULL, &module) != VK_SUCCESS)
		return 0;
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline.stage.module = module;
	pipeline.stage.pName = "main";
	pipeline.layout = pt_measure.layout;
	if (pt_measure.create_pipeline(pt_measure.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &pt_measure.pipeline) !=
		VK_SUCCESS)
	{
		pt_measure.destroy_module(pt_measure.device, module, NULL);
		return 0;
	}
	pt_measure.destroy_module(pt_measure.device, module, NULL);
	sizes[0].type = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = sizes[1].descriptorCount = sizes[2].descriptorCount = PT_MEASURE_SLOTS;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = PT_MEASURE_SLOTS;
	pool.poolSizeCount = 3;
	pool.pPoolSizes = sizes;
	if (pt_measure.create_pool(pt_measure.device, &pool, NULL, &pt_measure.pool) != VK_SUCCESS)
		return 0;
	for (index = 0; index < PT_MEASURE_SLOTS; index++)
		layouts[index] = pt_measure.set_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = pt_measure.pool;
	allocate.descriptorSetCount = PT_MEASURE_SLOTS;
	allocate.pSetLayouts = layouts;
	if (pt_measure.allocate_sets(pt_measure.device, &allocate, pt_measure.sets) != VK_SUCCESS)
		return 0;
	memset(&buffer, 0, sizeof(buffer));
	buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	buffer.size = PT_MEASURE_SLOTS * 256u;
	buffer.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (pt_measure.create_buffer(pt_measure.device, &buffer, NULL, &pt_measure.counts) != VK_SUCCESS)
		return 0;
	pt_measure.requirements(pt_measure.device, pt_measure.counts, &requirements);
	type = pt_measure_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (type < 0)
		return 0;
	memset(&memory, 0, sizeof(memory));
	memory.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	memory.allocationSize = requirements.size;
	memory.memoryTypeIndex = (uint32_t)type;
	if (pt_measure.allocate(pt_measure.device, &memory, NULL, &pt_measure.counts_memory) != VK_SUCCESS ||
		pt_measure.bind(pt_measure.device, pt_measure.counts, pt_measure.counts_memory, 0) != VK_SUCCESS ||
		pt_measure.map(pt_measure.device, pt_measure.counts_memory, 0, VK_WHOLE_SIZE, 0,
			(void **)&pt_measure.counts_mapped) != VK_SUCCESS)
		return 0;
	memset(pt_measure.counts_mapped, 0, PT_MEASURE_SLOTS * 256u);
	memset(&queries, 0, sizeof(queries));
	queries.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
	queries.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queries.queryCount = PT_MEASURE_SLOTS * 2u;
	if (pt_measure.create_queries(pt_measure.device, &queries, NULL, &pt_measure.times) != VK_SUCCESS)
		return 0;
	memset(&sampler, 0, sizeof(sampler));
	sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_POINT;
	sampler.mip = XGPU_VULKAN_MIP_NONE;
	sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_CLAMP;
	sampler.anisotropy = 1;
	pt_measure.sampler = xgpu_vulkan_sampler(&sampler);
	return pt_measure.sampler != VK_NULL_HANDLE;
}

int xgpu_vulkan_pt_measure_create(const struct xgpu_vulkan_rt_device *device)
{
	const char *wanted = getenv("HALO_PT_MEASURE_RAYS");

	memset(&pt_measure, 0, sizeof(pt_measure));
	if (!wanted || !device || !device->device || !(device->timestamp_period > 0.0f))
		return 0;
	pt_measure.wanted = 1;
	pt_measure.rays_per_pixel = atoi(wanted) > 0 ? (uint32_t)atoi(wanted) : 4u;
	pt_measure.device = device->device;
	pt_measure.memory = device->memory;
	pt_measure.timestamp_period = device->timestamp_period;
#define LOAD(name, field) pt_measure.field = (PFN_##name)device->get_device_proc_addr(device->device, #name);
	LOAD(vkCreateDescriptorSetLayout, create_set_layout)
	LOAD(vkDestroyDescriptorSetLayout, destroy_set_layout)
	LOAD(vkCreatePipelineLayout, create_layout)
	LOAD(vkDestroyPipelineLayout, destroy_layout)
	LOAD(vkCreateShaderModule, create_module)
	LOAD(vkDestroyShaderModule, destroy_module)
	LOAD(vkCreateComputePipelines, create_pipeline)
	LOAD(vkDestroyPipeline, destroy_pipeline)
	LOAD(vkCreateDescriptorPool, create_pool)
	LOAD(vkDestroyDescriptorPool, destroy_pool)
	LOAD(vkAllocateDescriptorSets, allocate_sets)
	LOAD(vkUpdateDescriptorSets, update_sets)
	LOAD(vkCreateBuffer, create_buffer)
	LOAD(vkDestroyBuffer, destroy_buffer)
	LOAD(vkGetBufferMemoryRequirements, requirements)
	LOAD(vkAllocateMemory, allocate)
	LOAD(vkFreeMemory, free_memory)
	LOAD(vkBindBufferMemory, bind)
	LOAD(vkMapMemory, map)
	LOAD(vkCreateQueryPool, create_queries)
	LOAD(vkDestroyQueryPool, destroy_queries)
	LOAD(vkCmdResetQueryPool, reset_queries)
	LOAD(vkCmdWriteTimestamp, timestamp)
	LOAD(vkGetQueryPoolResults, query_results)
	LOAD(vkCmdBindPipeline, bind_pipeline)
	LOAD(vkCmdBindDescriptorSets, bind_sets)
	LOAD(vkCmdPushConstants, push)
	LOAD(vkCmdDispatch, dispatch)
#undef LOAD
	return 1;
}

void xgpu_vulkan_pt_measure_destroy(void)
{
	if (!pt_measure.wanted)
		return;
	/* (the caller has waited for the device) */
	if (pt_measure.pipeline)
		pt_measure.destroy_pipeline(pt_measure.device, pt_measure.pipeline, NULL);
	if (pt_measure.layout)
		pt_measure.destroy_layout(pt_measure.device, pt_measure.layout, NULL);
	if (pt_measure.pool)
		pt_measure.destroy_pool(pt_measure.device, pt_measure.pool, NULL);
	if (pt_measure.set_layout)
		pt_measure.destroy_set_layout(pt_measure.device, pt_measure.set_layout, NULL);
	if (pt_measure.counts)
		pt_measure.destroy_buffer(pt_measure.device, pt_measure.counts, NULL);
	if (pt_measure.counts_memory)
		pt_measure.free_memory(pt_measure.device, pt_measure.counts_memory, NULL);
	if (pt_measure.times)
		pt_measure.destroy_queries(pt_measure.device, pt_measure.times, NULL);
	memset(&pt_measure, 0, sizeof(pt_measure));
}

static int pt_measure_compare(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

/* a slot the GPU is past read (its time and rays), and every mode's
numbers logged once each has PT_MEASURE_LOGGED frames */
static void pt_measure_read(uint32_t slot)
{
	uint64_t stamps[2];
	uint32_t mode = pt_measure.slot_mode[slot], *counts = pt_measure.counts_mapped + slot * 64u;
	double seconds;

	if (!pt_measure.slot_serial[slot])
		return;
	pt_measure.slot_serial[slot] = 0;
	if (pt_measure.query_results(pt_measure.device, pt_measure.times, slot * 2u, 2, sizeof(stamps), stamps,
		sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS || stamps[1] <= stamps[0] || !counts[0])
		return;
	seconds = (double)(stamps[1] - stamps[0]) * pt_measure.timestamp_period * 1e-9;
	/* (the second dispatch's rays: half those counted, both the same
	pixels' and rays a pixel) */
	if (pt_measure.frames[mode] < PT_MEASURE_LOGGED)
	{
		pt_measure.rates[mode][pt_measure.frames[mode]++] = (float)(counts[0] * 0.5 / seconds);
		pt_measure.rays[mode] += counts[0] * 0.5;
		pt_measure.seconds[mode] += seconds;
		pt_measure.pixels[mode] = pt_measure.slot_pixels[slot];
	}
	if (pt_measure.frames[0] >= PT_MEASURE_LOGGED && pt_measure.frames[1] >= PT_MEASURE_LOGGED &&
		pt_measure.frames[2] >= PT_MEASURE_LOGGED)
	{
		for (mode = 0; mode < PT_MEASURE_MODES; mode++)
		{
			qsort(pt_measure.rates[mode], PT_MEASURE_LOGGED, sizeof(float), pt_measure_compare);
			host_logf(HOST_LOG_INFO, "vulkan: pt measure %u: %s rays: %.0f Mrays/s (median frame %.0f, p10 %.0f, "
				"p90 %.0f), %u px x %u rays a frame, %.0f rays traced a frame, %.3f ms a frame", pt_measure.logged, pt_measure_mode_names[mode],
				pt_measure.rays[mode] / pt_measure.seconds[mode] * 1e-6,
				pt_measure.rates[mode][PT_MEASURE_LOGGED / 2] * 1e-6, pt_measure.rates[mode][PT_MEASURE_LOGGED / 10] * 1e-6,
				pt_measure.rates[mode][PT_MEASURE_LOGGED * 9 / 10] * 1e-6, pt_measure.pixels[mode],
				pt_measure.rays_per_pixel, pt_measure.rays[mode] / PT_MEASURE_LOGGED,
				pt_measure.seconds[mode] * 1e3 / PT_MEASURE_LOGGED);
			pt_measure.frames[mode] = 0;
			pt_measure.rays[mode] = pt_measure.seconds[mode] = 0.0;
		}
		pt_measure.logged++;
	}
}

void xgpu_vulkan_pt_measure_draw(struct xgpu_vulkan_image *inputs, const struct xgpu_world_ao_pass *pass)
{
	const struct xgpu_vulkan_image_description *size;
	struct xgpu_vulkan_image *read[1];
	struct pt_measure_push push;
	VkAccelerationStructureKHR scene;
	VkWriteDescriptorSetAccelerationStructureKHR scene_write;
	VkDescriptorImageInfo image;
	VkDescriptorBufferInfo buffer;
	VkWriteDescriptorSet writes[3];
	VkCommandBuffer commands;
	uint32_t slot, index, width, height;
	int axis;

	if (!pt_measure.wanted || pt_measure.failed || !inputs || !pass)
		return;
	if (!pt_measure.ready)
	{
		if (!pt_measure_make())
		{
			host_logf(HOST_LOG_WARN, "vulkan: pt measure: its pass cannot be made");
			pt_measure.failed = 1;
			return;
		}
		pt_measure.ready = 1;
		host_logf(HOST_LOG_INFO, "vulkan: pt measure: ray throughput measured, %u rays a pixel", pt_measure.rays_per_pixel);
	}
	/* (the slots the GPU is past read; the next one used, if it is free) */
	for (index = 0; index < PT_MEASURE_SLOTS; index++)
	{
		if (pt_measure.slot_serial[index] && xgpu_vulkan_completed() >= pt_measure.slot_serial[index])
			pt_measure_read(index);
	}
	slot = pt_measure.next_slot;
	if (pt_measure.slot_serial[slot] || !xgpu_vulkan_rt_build())
		return;
	scene = xgpu_vulkan_rt_scene();
	if (!scene)
		return;
	size = xgpu_vulkan_image_description(inputs);
	memset(&push, 0, sizeof(push));
	memcpy(push.camera, pass->camera, sizeof(push.camera));
	for (axis = 0; axis < 2; axis++)
	{
		int32_t extent = (int32_t)(axis ? size->height : size->width);
		int32_t low = pass->corners[axis] < 0 ? 0 : pass->corners[axis] > extent ? extent : pass->corners[axis];
		int32_t high = pass->corners[axis + 2] < low ? low : pass->corners[axis + 2] > extent ? extent :
			pass->corners[axis + 2];

		push.corners[axis] = low;
		push.corners[axis + 2] = high;
	}
	width = (uint32_t)(push.corners[2] - push.corners[0]);
	height = (uint32_t)(push.corners[3] - push.corners[1]);
	if (!width || !height)
		return;
	push.parameters[0] = pt_measure.frame;
	push.parameters[1] = pt_measure.rays_per_pixel;
	push.parameters[2] = pt_measure.frame % PT_MEASURE_MODES;
	memset(pt_measure.counts_mapped + slot * 64u, 0, 16);
	memset(&scene_write, 0, sizeof(scene_write));
	scene_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
	scene_write.accelerationStructureCount = 1;
	scene_write.pAccelerationStructures = &scene;
	image.sampler = pt_measure.sampler;
	image.imageView = xgpu_vulkan_image_view(inputs);
	image.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	buffer.buffer = pt_measure.counts;
	buffer.offset = slot * 256u;
	buffer.range = 16;
	memset(writes, 0, sizeof(writes));
	for (index = 0; index < 3; index++)
	{
		writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[index].dstSet = pt_measure.sets[slot];
		writes[index].dstBinding = index;
		writes[index].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
	writes[0].pNext = &scene_write;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &image;
	writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[2].pBufferInfo = &buffer;
	pt_measure.update_sets(pt_measure.device, 3, writes, 0, NULL);
	read[0] = inputs;
	xgpu_vulkan_pass_owner("PT MEASURE");
	commands = xgpu_vulkan_outside_begin(read, 1, NULL, 0);
	/* (twice, the second timed: a timestamp after the first dispatch is
	after it, where one before it may be written while the passes before
	still run, which measured them too) */
	pt_measure.reset_queries(commands, pt_measure.times, slot * 2u, 2);
	pt_measure.bind_pipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt_measure.pipeline);
	pt_measure.bind_sets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pt_measure.layout, 0, 1, &pt_measure.sets[slot], 0, NULL);
	pt_measure.push(commands, pt_measure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	pt_measure.dispatch(commands, (width + 7u) / 8u, (height + 7u) / 8u, 1);
	pt_measure.timestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pt_measure.times, slot * 2u);
	push.parameters[0] = pt_measure.frame + 0x10000u;
	pt_measure.push(commands, pt_measure.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
	pt_measure.dispatch(commands, (width + 7u) / 8u, (height + 7u) / 8u, 1);
	pt_measure.timestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pt_measure.times, slot * 2u + 1u);
	xgpu_vulkan_outside_end(commands);
	xgpu_vulkan_pass_owner(NULL);
	pt_measure.slot_serial[slot] = xgpu_vulkan_serial();
	pt_measure.slot_mode[slot] = push.parameters[2];
	pt_measure.slot_pixels[slot] = width * height;
	pt_measure.next_slot = (slot + 1u) % PT_MEASURE_SLOTS;
	pt_measure.frame++;
}
