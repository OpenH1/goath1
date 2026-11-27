/*
XGPU_VULKAN_RT.C

display.ambient_occlusion "ray_traced" in the Vulkan backend (docs/world-ao.md,
"Hardware ray queries"; xgpu_stream.h's WORLD_AO_MESH and _INSTANCES): the
ray-traced scene's acceleration structures, which the occlusion's pass reads
with ray queries (VK_KHR_ray_query, VK_KHR_acceleration_structure) where the
device has them (the core asks for them as it makes the device and starts
this file: xgpu_vulkan_rt_create).

- Each mesh is a bottom-level structure, built once (fast to trace): its
  triangles kept as they come and built at the next frame's scene with any
  others that came, in one call, then let go of. The small ones live in
  large buffers of their own (a map's few hundred, never one allocation
  each), let go of together with the meshes (id 0, a map's end). One of
  RT_OWN_TRIANGLES or more (a level's) has a buffer of its own and is
  compacted once built (its compacted size read a few frames later, never
  waited for, then copied into a buffer of that size, the first let go
  of), so that it can be let go of alone (xgpu_vulkan_rt_mesh_release: the
  level's collision mesh once its render mesh is in the scene).
- The frame's instances are a top-level structure, built again (fast to
  build) before the pass that first reads them: three in turn, each written
  only once the GPU is done with the work that read it. An instance whose
  mesh never came, or whose owner, kind or transform is out of bounds, is
  left out. Its flags: the structure's triangles opaque and either way
  facing; an object's not opaque (so that the pass leaves a pixel's own
  out) and facing front where wound counterclockwise seen from outside
  (HALO_VULKAN_RT_FLIP=1 turns that, to check it).
- The builds go in the early commands (xgpu_vulkan_early_commands), before
  the submission's draws, which the main commands' first barrier orders
  after them; they never end a pass.
- Until a scene is built, the draws' occlusion set names an empty one.
- What a ray's hit reads (docs/rt-plan/1-geometry.md, "Per-hit data"):
  the hit table, one storage buffer (host-visible, its words kept beside
  it to grow it), and the textures' slots, a bindless array of
  XGPU_WORLD_RT_TEXTURES (their images found again each frame, which keeps
  them in the texture cache; the empty ones a white stand-in), both in the
  hit set (XGPU_VULKAN_SET_RT_HITS), written again only as they change.
  The table: each mesh's record (RT_HIT_MESH: its corners', attributes' and
  triangles' materials' words and its opaque triangles' count; a mesh of
  triangles' all 0), each material's (RT_HIT_MATERIAL: xgpu_stream.h's
  struct xgpu_world_rt_material, sixteen words), then the meshes' data. An
  instance names its mesh by its shader binding table offset, which ray
  queries have no other use for. Everything there is checked as it comes
  (every corner, material and slot in range), so the shaders read it
  unchecked. Nothing is taken out of it until the map's end, which makes
  another (the GPU may still read the last frames' meshes).
- A build with a mesh of RT_LOGGED_TRIANGLES or more (the level's) is
  logged: its meshes, triangles, structures' and scratch's bytes, the
  meshes' storage so far, and its GPU time where the queue has timestamps
  (read a few frames later, never waited for).

Render thread alone, as the core.
*/

#include "xgpu_spirv.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_textures.h"
#include "host.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---------- constants */

enum
{
	/* the scenes in turn */
	RT_SCENES = 3,
	/* the meshes' storage's buffers, each at least so large */
	RT_CHUNKS = 64,
};

#define RT_CHUNK_BYTES ((VkDeviceSize)16 << 20)
/* the hit table's records (words): each mesh's (four: its corners' first
word, its attributes', its triangles' materials' (two to a word, the
first low), its opaque triangles' count), each material's (sixteen), then
the meshes' data; and the hit sets in turn */
#define RT_HIT_MESH 0u
#define RT_HIT_MATERIAL (XGPU_STREAM_WORLD_AO_MESHES * 4u)
#define RT_HIT_DATA (RT_HIT_MATERIAL + XGPU_WORLD_RT_MATERIALS * 16u)
#define RT_HIT_SETS 4u
_Static_assert(RT_HIT_MATERIAL == XGPU_SPIRV_RT_HIT_MATERIALS && XGPU_STREAM_WORLD_AO_MESHES == XGPU_SPIRV_RT_HIT_MESHES &&
	XGPU_WORLD_RT_TEXTURES == XGPU_SPIRV_RT_HIT_TEXTURES, "the hit table as the shaders read it");
/* the new textures found a frame at most (each may be read from the guest) */
#define RT_TEXTURES_FOUND 16u
/* a mesh of so many triangles has a buffer of its own, and is compacted */
#define RT_OWN_TRIANGLES 1024u
/* the most meshes compacted at once (the compacted sizes' queries) */
#define RT_COMPACTED 64u
/* a build logged has a mesh of so many triangles */
#define RT_LOGGED_TRIANGLES 2048u
/* an acceleration structure's offset in its buffer (the specification's) */
#define RT_STRUCTURE_ALIGNMENT 256u

/* ---------- structures */

/* a buffer of its own memory, with its device address (mapped where it is
host-visible) */
struct rt_buffer
{
	VkBuffer buffer;
	VkDeviceMemory memory;
	VkDeviceAddress address;
	VkDeviceSize size;
	void *mapped;
};

struct rt_mesh
{
	VkAccelerationStructureKHR structure;
	VkDeviceAddress address;
	uint32_t count;
	/* its triangles until it is built (NULL after) */
	float *triangles;
	int built;
	/* (RT_OWN_TRIANGLES or more) its own buffer; its structure's bytes; its
	compacted size's query (its index plus one: asked, not yet read), and
	whether it was asked */
	struct rt_buffer own;
	VkDeviceSize bytes;
	uint32_t compacting;
	int compaction_asked;
	/* (an indexed one: xgpu_vulkan_rt_mesh_indexed) its positions and
	corners until it is built, its vertices, its opaque triangles (the
	first so many: the rest alpha-tested) */
	int indexed;
	float *positions;
	uint32_t *indices;
	uint32_t vertex_count, opaque_count;
};

struct rt_scene
{
	VkAccelerationStructureKHR structure;
	struct rt_buffer storage, instances;
	uint32_t capacity;
	/* the last submission that reads it */
	uint64_t used;
	int built;
};

struct rt_garbage
{
	uint64_t serial;
	struct rt_buffer buffer;
	VkAccelerationStructureKHR structure;
};

/* ---------- globals */

static struct
{
	int ready, flip;
	VkDevice device;
	VkPhysicalDeviceMemoryProperties memory;
	uint32_t scratch_alignment;
	PFN_vkCreateAccelerationStructureKHR create;
	PFN_vkDestroyAccelerationStructureKHR destroy;
	PFN_vkGetAccelerationStructureBuildSizesKHR sizes;
	PFN_vkCmdBuildAccelerationStructuresKHR build;
	PFN_vkGetAccelerationStructureDeviceAddressKHR structure_address;
	PFN_vkGetBufferDeviceAddress buffer_address;
	PFN_vkCreateBuffer create_buffer;
	PFN_vkDestroyBuffer destroy_buffer;
	PFN_vkGetBufferMemoryRequirements requirements;
	PFN_vkAllocateMemory allocate;
	PFN_vkFreeMemory free_memory;
	PFN_vkBindBufferMemory bind;
	PFN_vkMapMemory map;
	PFN_vkCmdPipelineBarrier barrier;
	PFN_vkCreateQueryPool create_queries;
	PFN_vkDestroyQueryPool destroy_queries;
	PFN_vkCmdResetQueryPool reset_queries;
	PFN_vkCmdWriteTimestamp timestamp;
	PFN_vkGetQueryPoolResults query_results;
	/* (a logged build's) its GPU time's two timestamps (VK_NULL_HANDLE: not
	timed), whether they are yet to be read (once the GPU is done with its
	submission), and a timestamp's nanoseconds; the meshes' structures'
	bytes so far */
	VkQueryPool times;
	int timed;
	uint64_t timed_serial;
	float timestamp_period;
	VkDeviceSize structure_bytes;
	/* the meshes' structures' bytes now (less those let go of and what
	compaction saved), and the most they have been this map */
	VkDeviceSize live_bytes, live_most;
	/* the compacted sizes asked (the meshes' ids; 0 one let go of since),
	read once the GPU is past the submission that asked */
	PFN_vkCmdWriteAccelerationStructuresPropertiesKHR properties;
	PFN_vkCmdCopyAccelerationStructureKHR copy;
	VkQueryPool compacted_sizes;
	uint32_t compacted_ids[RT_COMPACTED], compacted_count;
	uint64_t compacted_serial;
	/* what a hit reads: the hit table (its words, those used, the buffer
	they are copied into), the materials written, the textures' slots (as
	described, and the view each was last found as), the stand-in, the
	sampler; the hit sets' layout, pool and sets (each its contents'
	version and the last submission that read it), the version now, the
	last set handed out, and the submission the textures were last found
	for */
	PFN_vkCreateDescriptorSetLayout create_layout;
	PFN_vkDestroyDescriptorSetLayout destroy_layout;
	PFN_vkCreateDescriptorPool create_pool;
	PFN_vkDestroyDescriptorPool destroy_pool;
	PFN_vkAllocateDescriptorSets allocate_sets;
	PFN_vkUpdateDescriptorSets update_sets;
	uint32_t *hit_words;
	uint32_t hit_used, hit_capacity;
	struct rt_buffer hit_buffer;
	unsigned char hit_materials[XGPU_WORLD_RT_MATERIALS];
	struct xgpu_stream_texture_stage textures[XGPU_WORLD_RT_TEXTURES];
	VkImageView texture_views[XGPU_WORLD_RT_TEXTURES];
	struct xgpu_vulkan_image *stand_in;
	VkSampler sampler;
	VkDescriptorSetLayout hit_layout;
	VkDescriptorPool hit_pool;
	VkDescriptorSet hit_sets[RT_HIT_SETS];
	uint64_t hit_set_versions[RT_HIT_SETS], hit_set_used[RT_HIT_SETS];
	uint64_t hit_version, textures_serial;
	int hit_current;
	/* the meshes (by id) and those not yet built */
	struct rt_mesh *meshes[XGPU_STREAM_WORLD_AO_MESHES];
	uint32_t pending;
	/* the meshes' storage: its buffers, and what of the last is taken */
	struct rt_buffer chunks[RT_CHUNKS];
	uint32_t chunk_count;
	VkDeviceSize chunk_used;
	/* the instances given (the frame's), and whether they were built since */
	struct xgpu_world_ao_instance *given;
	uint32_t given_count, given_capacity;
	int given_new;
	/* the scenes in turn (current: the latest built, -1 none) and the empty
	one */
	struct rt_scene scenes[RT_SCENES], empty;
	int current;
	struct rt_garbage *garbage;
	uint32_t garbage_count, garbage_capacity;
	int failed_said;
} rt;

/* ---------- memory */

static int rt_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted)
{
	uint32_t index;

	for (index = 0; index < rt.memory.memoryTypeCount; index++)
	{
		if ((bits & (1u << index)) && (rt.memory.memoryTypes[index].propertyFlags & wanted) == wanted)
			return (int)index;
	}
	return -1;
}

/* a buffer of size bytes for usage (and its device address), in memory of
its own, host-visible or the device's; 0 if it cannot be */
static int rt_buffer_make(VkDeviceSize size, VkBufferUsageFlags usage, int host, struct rt_buffer *buffer)
{
	VkBufferCreateInfo info;
	VkMemoryRequirements requirements;
	VkMemoryAllocateFlagsInfo flags;
	VkMemoryAllocateInfo allocate;
	VkBufferDeviceAddressInfo address;
	int type;

	memset(buffer, 0, sizeof(*buffer));
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	info.size = size;
	info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
	info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	if (rt.create_buffer(rt.device, &info, NULL, &buffer->buffer) != VK_SUCCESS)
		return 0;
	rt.requirements(rt.device, buffer->buffer, &requirements);
	type = rt_memory_type(requirements.memoryTypeBits, host ?
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type < 0 && !host)
		type = rt_memory_type(requirements.memoryTypeBits, 0);
	memset(&flags, 0, sizeof(flags));
	flags.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
	flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &flags;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = (uint32_t)type;
	if (type < 0 || rt.allocate(rt.device, &allocate, NULL, &buffer->memory) != VK_SUCCESS)
	{
		rt.destroy_buffer(rt.device, buffer->buffer, NULL);
		memset(buffer, 0, sizeof(*buffer));
		return 0;
	}
	if (rt.bind(rt.device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
		(host && rt.map(rt.device, buffer->memory, 0, VK_WHOLE_SIZE, 0, &buffer->mapped) != VK_SUCCESS))
	{
		rt.destroy_buffer(rt.device, buffer->buffer, NULL);
		rt.free_memory(rt.device, buffer->memory, NULL);
		memset(buffer, 0, sizeof(*buffer));
		return 0;
	}
	memset(&address, 0, sizeof(address));
	address.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	address.buffer = buffer->buffer;
	buffer->address = rt.buffer_address(rt.device, &address);
	buffer->size = size;
	return 1;
}

static void rt_buffer_free(struct rt_buffer *buffer)
{
	if (buffer->buffer)
		rt.destroy_buffer(rt.device, buffer->buffer, NULL);
	if (buffer->memory)
		rt.free_memory(rt.device, buffer->memory, NULL);
	memset(buffer, 0, sizeof(*buffer));
}

/* ---------- garbage: let go of once the GPU is past the work recorded now */

static void rt_garbage_collect(void)
{
	uint64_t done = xgpu_vulkan_completed();
	uint32_t index = 0;

	while (index < rt.garbage_count)
	{
		struct rt_garbage *item = &rt.garbage[index];

		if (item->serial > done)
		{
			index++;
			continue;
		}
		if (item->structure)
			rt.destroy(rt.device, item->structure, NULL);
		rt_buffer_free(&item->buffer);
		rt.garbage[index] = rt.garbage[--rt.garbage_count];
	}
}

static void rt_garbage_add(const struct rt_buffer *buffer, VkAccelerationStructureKHR structure)
{
	struct rt_garbage *item;

	if (rt.garbage_count == rt.garbage_capacity)
	{
		uint32_t capacity = rt.garbage_capacity ? rt.garbage_capacity * 2 : 64;
		struct rt_garbage *grown = realloc(rt.garbage, capacity * sizeof(*grown));

		/* (none: waited for, then let go of now) */
		if (!grown)
		{
			xgpu_vulkan_wait(xgpu_vulkan_fence());
			if (structure)
				rt.destroy(rt.device, structure, NULL);
			if (buffer)
			{
				struct rt_buffer copy = *buffer;

				rt_buffer_free(&copy);
			}
			return;
		}
		rt.garbage = grown;
		rt.garbage_capacity = capacity;
	}
	item = &rt.garbage[rt.garbage_count++];
	memset(item, 0, sizeof(*item));
	item->serial = xgpu_vulkan_serial();
	if (buffer)
		item->buffer = *buffer;
	item->structure = structure;
}

/* ---------- the meshes */

/* room for a mesh's structure in the meshes' storage: its buffer and
offset; 0 if none can be had */
static int rt_storage_take(VkDeviceSize size, VkBuffer *buffer, VkDeviceSize *offset)
{
	VkDeviceSize at = (rt.chunk_used + RT_STRUCTURE_ALIGNMENT - 1) & ~(VkDeviceSize)(RT_STRUCTURE_ALIGNMENT - 1);

	if (!rt.chunk_count || at + size > rt.chunks[rt.chunk_count - 1].size)
	{
		if (rt.chunk_count == RT_CHUNKS || !rt_buffer_make(size > RT_CHUNK_BYTES ? size : RT_CHUNK_BYTES,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, &rt.chunks[rt.chunk_count]))
			return 0;
		rt.chunk_count++;
		at = 0;
	}
	*buffer = rt.chunks[rt.chunk_count - 1].buffer;
	*offset = at;
	rt.chunk_used = at + size;
	return 1;
}

static void rt_meshes_release(void)
{
	uint32_t id;

	for (id = 0; id < XGPU_STREAM_WORLD_AO_MESHES; id++)
	{
		struct rt_mesh *mesh = rt.meshes[id];

		if (!mesh)
			continue;
		if (mesh->structure)
			rt_garbage_add(mesh->own.buffer ? &mesh->own : NULL, mesh->structure);
		free(mesh->triangles);
		free(mesh->positions);
		free(mesh->indices);
		free(mesh);
		rt.meshes[id] = NULL;
	}
	rt.compacted_count = 0;
	rt.live_bytes = rt.live_most = 0;
	/* (a new hit table: the GPU may still read the old) */
	if (rt.hit_buffer.buffer)
		rt_garbage_add(&rt.hit_buffer, VK_NULL_HANDLE);
	memset(&rt.hit_buffer, 0, sizeof(rt.hit_buffer));
	free(rt.hit_words);
	rt.hit_words = NULL;
	rt.hit_used = rt.hit_capacity = 0;
	memset(rt.hit_materials, 0, sizeof(rt.hit_materials));
	rt.hit_version++;
	for (id = 0; id < rt.chunk_count; id++)
		rt_garbage_add(&rt.chunks[id], VK_NULL_HANDLE);
	memset(rt.chunks, 0, sizeof(rt.chunks));
	rt.chunk_count = 0;
	rt.chunk_used = 0;
	rt.pending = 0;
	rt.structure_bytes = 0;
}

/* (a logged build) its GPU time, once the GPU has written it */
static void rt_build_time_read(void)
{
	uint64_t stamps[2];

	if (!rt.timed || xgpu_vulkan_completed() < rt.timed_serial ||
		rt.query_results(rt.device, rt.times, 0, 2, sizeof(stamps), stamps, sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT) !=
			VK_SUCCESS)
	{
		return;
	}
	rt.timed = 0;
	host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's meshes built in %.3f ms of GPU time",
		(double)(stamps[1] - stamps[0]) * (double)rt.timestamp_period * 1.0e-6);
}

/* the meshes' structures' bytes now, after a change */
static void rt_live_bytes(VkDeviceSize added, VkDeviceSize taken)
{
	rt.live_bytes += added;
	rt.live_bytes -= taken < rt.live_bytes ? taken : rt.live_bytes;
	if (rt.live_bytes > rt.live_most)
		rt.live_most = rt.live_bytes;
}

void xgpu_vulkan_rt_mesh_release(uint32_t id)
{
	struct rt_mesh *mesh;
	uint32_t index;

	if (!rt.ready || !id || id >= XGPU_STREAM_WORLD_AO_MESHES || !rt.meshes[id])
		return;
	rt_garbage_collect();
	mesh = rt.meshes[id];
	if (mesh->structure)
	{
		rt_garbage_add(mesh->own.buffer ? &mesh->own : NULL, mesh->structure);
		rt_live_bytes(0, mesh->bytes);
		/* (the scene built again: the instances may still name it) */
		rt.given_new = 1;
	}
	if (!mesh->built && rt.pending)
		rt.pending--;
	for (index = 0; index < rt.compacted_count; index++)
	{
		if (rt.compacted_ids[index] == id)
			rt.compacted_ids[index] = 0;
	}
	host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's mesh %u let go of (%.2f MB); %.2f MB of meshes now", id,
		(double)mesh->bytes / 1048576.0, (double)rt.live_bytes / 1048576.0);
	free(mesh->triangles);
	free(mesh->positions);
	free(mesh->indices);
	free(mesh);
	rt.meshes[id] = NULL;
}

void xgpu_vulkan_rt_mesh(uint32_t id, uint32_t count, const float *triangles)
{
	struct rt_mesh *mesh;

	if (!rt.ready)
		return;
	rt_garbage_collect();
	if (!id)
	{
		rt_meshes_release();
		/* (the instances named them) */
		rt.given_count = 0;
		rt.given_new = 1;
		return;
	}
	if (id >= XGPU_STREAM_WORLD_AO_MESHES || !count || count > XGPU_STREAM_WORLD_AO_MESH_TRIANGLES || !triangles ||
		rt.meshes[id])
		return;
	mesh = calloc(1, sizeof(*mesh));
	if (!mesh)
		return;
	mesh->triangles = malloc((size_t)count * 36u);
	if (!mesh->triangles)
	{
		free(mesh);
		return;
	}
	memcpy(mesh->triangles, triangles, (size_t)count * 36u);
	mesh->count = count;
	rt.meshes[id] = mesh;
	rt.pending++;
}

/* ---------- what a hit reads */

/* room for count more words in the hit table (made with its records the
first time: a larger one copied from its words, the old let go of once
the GPU is done with it); where they start, 0 if there is none */
static uint32_t rt_hit_room(uint32_t count)
{
	uint32_t capacity, at;
	uint32_t *grown;
	struct rt_buffer buffer;

	if (!rt.hit_words)
		rt.hit_used = RT_HIT_DATA;
	if ((uint64_t)rt.hit_used + count <= rt.hit_capacity)
	{
		at = rt.hit_used;
		rt.hit_used += count;
		return at;
	}
	for (capacity = rt.hit_capacity ? rt.hit_capacity : 1u << 20; (uint64_t)capacity < (uint64_t)rt.hit_used + count;
		capacity *= 2)
	{
		if (capacity >= 1u << 28)
			return 0;
	}
	grown = realloc(rt.hit_words, (size_t)capacity * 4u);
	if (!grown)
		return 0;
	if (!rt.hit_words)
		memset(grown, 0, (size_t)RT_HIT_DATA * 4u);
	rt.hit_words = grown;
	if (!rt_buffer_make((VkDeviceSize)capacity * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &buffer))
		return 0;
	memcpy(buffer.mapped, rt.hit_words, (size_t)rt.hit_used * 4u);
	if (rt.hit_buffer.buffer)
		rt_garbage_add(&rt.hit_buffer, VK_NULL_HANDLE);
	rt.hit_buffer = buffer;
	rt.hit_capacity = capacity;
	rt.hit_version++;
	at = rt.hit_used;
	rt.hit_used += count;
	return at;
}

/* words written into the hit table, and its buffer */
static void rt_hit_write(uint32_t at, const void *words, uint32_t count)
{
	memcpy(rt.hit_words + at, words, (size_t)count * 4u);
	memcpy((uint32_t *)rt.hit_buffer.mapped + at, words, (size_t)count * 4u);
}

/* a material's slots in range (FALSE: not) */
static int rt_material_checked(const struct xgpu_world_rt_material *material)
{
	return material->index < XGPU_WORLD_RT_MATERIALS && isfinite(material->alpha_cutoff) &&
		isfinite(material->alpha_scale[0]) && isfinite(material->alpha_scale[1]) && isfinite(material->detail_scale) &&
		material->alpha_map < XGPU_WORLD_RT_TEXTURES && material->base_map < XGPU_WORLD_RT_TEXTURES &&
		material->detail_map < XGPU_WORLD_RT_TEXTURES && material->multipurpose_map < XGPU_WORLD_RT_TEXTURES &&
		material->bump_map < XGPU_WORLD_RT_TEXTURES && material->normal_map < XGPU_WORLD_RT_TEXTURES &&
		material->orm_map < XGPU_WORLD_RT_TEXTURES && material->lightmap < XGPU_WORLD_RT_TEXTURES;
}

void xgpu_vulkan_rt_mesh_indexed(uint32_t id, const void *data, uint32_t size)
{
	const struct xgpu_world_rt_mesh *header = data;
	const float *positions;
	const uint32_t *indices;
	const struct xgpu_world_rt_vertex *attributes;
	const uint16_t *triangle_materials;
	const struct xgpu_world_rt_material *materials;
	struct rt_mesh *mesh;
	uint32_t vertex_count, triangle_count, index, record[4], corners_at, attributes_at, materials_at;
	uint64_t expected;

	if (!rt.ready || !id || id >= XGPU_STREAM_WORLD_AO_MESHES || rt.meshes[id] || !data || size < sizeof(*header))
		return;
	rt_garbage_collect();
	vertex_count = header->vertex_count;
	triangle_count = header->triangle_count;
	if (!vertex_count || vertex_count > XGPU_WORLD_RT_VERTICES || !triangle_count ||
		triangle_count > XGPU_STREAM_WORLD_AO_MESH_TRIANGLES || header->opaque_count > triangle_count ||
		header->material_count > XGPU_WORLD_RT_MATERIALS)
		return;
	expected = sizeof(*header) + (uint64_t)vertex_count * 12u + (uint64_t)triangle_count * 12u +
		(uint64_t)vertex_count * sizeof(struct xgpu_world_rt_vertex) + (((uint64_t)triangle_count * 2u + 3u) & ~(uint64_t)3u) +
		(uint64_t)header->material_count * sizeof(struct xgpu_world_rt_material);
	if (expected != size)
		return;
	positions = (const float *)(header + 1);
	indices = (const uint32_t *)(positions + (size_t)vertex_count * 3u);
	attributes = (const struct xgpu_world_rt_vertex *)(indices + (size_t)triangle_count * 3u);
	triangle_materials = (const uint16_t *)(attributes + vertex_count);
	materials = (const struct xgpu_world_rt_material *)((const unsigned char *)triangle_materials +
		(((size_t)triangle_count * 2u + 3u) & ~(size_t)3u));
	/* (every position, corner, material and slot in range: the shaders
	read them unchecked) */
	for (index = 0; index < vertex_count * 3u; index++)
	{
		if (!isfinite(positions[index]) || fabsf(positions[index]) > 1.0e6f)
			return;
	}
	for (index = 0; index < triangle_count * 3u; index++)
	{
		if (indices[index] >= vertex_count)
			return;
	}
	for (index = 0; index < vertex_count; index++)
	{
		if (!isfinite(attributes[index].uv[0]) || !isfinite(attributes[index].uv[1]))
			return;
	}
	for (index = 0; index < triangle_count; index++)
	{
		if (triangle_materials[index] >= XGPU_WORLD_RT_MATERIALS)
			return;
	}
	for (index = 0; index < header->material_count; index++)
	{
		if (!rt_material_checked(&materials[index]))
			return;
	}
	mesh = calloc(1, sizeof(*mesh));
	if (!mesh)
		return;
	mesh->positions = malloc((size_t)vertex_count * 12u);
	mesh->indices = malloc((size_t)triangle_count * 12u);
	corners_at = rt_hit_room(triangle_count * 3u + vertex_count * 6u + (triangle_count + 1u) / 2u);
	if (!mesh->positions || !mesh->indices || !corners_at)
	{
		free(mesh->positions);
		free(mesh->indices);
		free(mesh);
		return;
	}
	memcpy(mesh->positions, positions, (size_t)vertex_count * 12u);
	memcpy(mesh->indices, indices, (size_t)triangle_count * 12u);
	mesh->indexed = 1;
	mesh->count = triangle_count;
	mesh->vertex_count = vertex_count;
	mesh->opaque_count = header->opaque_count;
	/* its data and record in the hit table */
	attributes_at = corners_at + triangle_count * 3u;
	materials_at = attributes_at + vertex_count * 6u;
	rt_hit_write(corners_at, indices, triangle_count * 3u);
	rt_hit_write(attributes_at, attributes, vertex_count * 6u);
	rt_hit_write(materials_at, triangle_materials, triangle_count / 2u);
	if (triangle_count & 1u)
	{
		uint32_t last = triangle_materials[triangle_count - 1];

		rt_hit_write(materials_at + triangle_count / 2u, &last, 1);
	}
	record[0] = corners_at;
	record[1] = attributes_at;
	record[2] = materials_at;
	record[3] = header->opaque_count;
	rt_hit_write(RT_HIT_MESH + id * 4u, record, 4);
	/* (a material once a map: the GPU may be reading it) */
	for (index = 0; index < header->material_count; index++)
	{
		if (rt.hit_materials[materials[index].index])
			continue;
		rt.hit_materials[materials[index].index] = 1;
		rt_hit_write(RT_HIT_MATERIAL + materials[index].index * 16u, &materials[index], 16);
	}
	rt.meshes[id] = mesh;
	rt.pending++;
}

void xgpu_vulkan_rt_textures(const struct xgpu_world_rt_texture *textures, uint32_t count)
{
	uint32_t index;

	if (!rt.ready || !textures)
		return;
	for (index = 0; index < count && index < XGPU_WORLD_RT_TEXTURES; index++)
	{
		uint32_t slot = textures[index].slot;

		if (!slot || slot >= XGPU_WORLD_RT_TEXTURES)
			continue;
		rt.textures[slot] = textures[index].stage;
		/* (found again at the next frame) */
		rt.texture_views[slot] = VK_NULL_HANDLE;
	}
}

/* the textures' slots' images found for this submission (each frame: so
the texture cache keeps them, and one made again is seen), at most
RT_TEXTURES_FOUND new ones; a slot whose view changed changes the hit
sets' version */
static void rt_textures_find(void)
{
	uint32_t slot, found = 0;
	uint64_t serial = xgpu_vulkan_serial();

	if (rt.textures_serial == serial || !rt.hit_layout)
		return;
	rt.textures_serial = serial;
	if (!rt.stand_in)
	{
		static const unsigned char white[4] = { 255, 255, 255, 255 };
		struct xgpu_vulkan_image_description description;
		struct xgpu_vulkan_sampler_description sampler;

		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA8;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED;
		description.width = description.height = description.depth_or_layers = description.levels = 1;
		rt.stand_in = xgpu_vulkan_image_create(&description);
		if (!rt.stand_in)
			return;
		xgpu_vulkan_image_upload(rt.stand_in, 0, 0, 0, 0, 0, 1, 1, 1, white, 4, 4);
		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
		sampler.mip = XGPU_VULKAN_MIP_LINEAR;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_WRAP;
		sampler.anisotropy = 1;
		rt.sampler = xgpu_vulkan_sampler(&sampler);
		rt.hit_version++;
	}
	for (slot = 1; slot < XGPU_WORLD_RT_TEXTURES; slot++)
	{
		const struct xgpu_stream_texture_stage *stage = &rt.textures[slot];
		struct xgpu_vulkan_image *image = NULL;
		VkImageView view;

		if (stage->source == XGPU_STREAM_SOURCE_NONE)
			continue;
		if (!rt.texture_views[slot])
		{
			if (found >= RT_TEXTURES_FOUND)
				continue;
			found++;
		}
		if (stage->source == XGPU_STREAM_SOURCE_TEXTURE)
			image = xgpu_vulkan_textures_header(stage->resource);
		else if (stage->source == XGPU_STREAM_SOURCE_IMAGE)
			image = xgpu_vulkan_textures_get(stage);
		/* (a 2D one alone: the array is of 2D images) */
		view = image && xgpu_vulkan_image_description(image)->type == XGPU_VULKAN_IMAGE_2D ?
			xgpu_vulkan_image_view(image) : VK_NULL_HANDLE;
		if (!view)
			view = xgpu_vulkan_image_view(rt.stand_in);
		if (view != rt.texture_views[slot])
		{
			rt.texture_views[slot] = view;
			rt.hit_version++;
		}
	}
}

VkDescriptorSetLayout xgpu_vulkan_rt_hit_layout(void)
{
	return rt.ready ? rt.hit_layout : VK_NULL_HANDLE;
}

VkDescriptorSet xgpu_vulkan_rt_hit_set(void)
{
	VkDescriptorBufferInfo table;
	VkDescriptorImageInfo images[XGPU_WORLD_RT_TEXTURES];
	VkWriteDescriptorSet writes[2];
	uint64_t serial = xgpu_vulkan_serial(), done = xgpu_vulkan_completed();
	uint32_t which, slot;
	VkImageView stand_in;

	if (!rt.ready || !rt.hit_pool)
		return VK_NULL_HANDLE;
	/* (a table and the stand-in made, so a set has something to name) */
	if (!rt.hit_buffer.buffer && !rt_hit_room(0))
		return VK_NULL_HANDLE;
	stand_in = xgpu_vulkan_image_view(rt.stand_in);
	if (!stand_in || !rt.sampler)
		return VK_NULL_HANDLE;
	if (rt.hit_current >= 0 && rt.hit_set_versions[rt.hit_current] == rt.hit_version)
	{
		rt.hit_set_used[rt.hit_current] = serial;
		return rt.hit_sets[rt.hit_current];
	}
	/* (one no work not yet done reads; none: the last, as it was) */
	for (which = 0; which < RT_HIT_SETS; which++)
	{
		if ((int)which != rt.hit_current && rt.hit_set_used[which] < serial && rt.hit_set_used[which] <= done)
			break;
	}
	if (which == RT_HIT_SETS)
	{
		if (rt.hit_current < 0)
			return VK_NULL_HANDLE;
		rt.hit_set_used[rt.hit_current] = serial;
		return rt.hit_sets[rt.hit_current];
	}
	memset(&table, 0, sizeof(table));
	table.buffer = rt.hit_buffer.buffer;
	table.range = VK_WHOLE_SIZE;
	for (slot = 0; slot < XGPU_WORLD_RT_TEXTURES; slot++)
	{
		images[slot].sampler = rt.sampler;
		images[slot].imageView = rt.texture_views[slot] ? rt.texture_views[slot] : stand_in;
		images[slot].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	}
	memset(writes, 0, sizeof(writes));
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = rt.hit_sets[which];
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[0].pBufferInfo = &table;
	writes[1] = writes[0];
	writes[1].dstBinding = 1;
	writes[1].descriptorCount = XGPU_WORLD_RT_TEXTURES;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pBufferInfo = NULL;
	writes[1].pImageInfo = images;
	rt.update_sets(rt.device, 2, writes, 0, NULL);
	rt.hit_set_versions[which] = rt.hit_version;
	rt.hit_set_used[which] = serial;
	rt.hit_current = (int)which;
	return rt.hit_sets[which];
}

/* the hit sets' layout, pool and sets (FALSE: none can be made) */
static int rt_hit_sets_make(void)
{
	VkDescriptorSetLayoutBinding bindings[2];
	VkDescriptorSetLayoutCreateInfo layout;
	VkDescriptorPoolSize sizes[2];
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetLayout layouts[RT_HIT_SETS];
	VkDescriptorSetAllocateInfo allocate;
	uint32_t which;

	if (!rt.create_layout || !rt.destroy_layout || !rt.create_pool || !rt.destroy_pool || !rt.allocate_sets ||
		!rt.update_sets)
		return 0;
	memset(bindings, 0, sizeof(bindings));
	bindings[0].binding = 0;
	bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	bindings[0].descriptorCount = 1;
	bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	bindings[1].binding = 1;
	bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	bindings[1].descriptorCount = XGPU_WORLD_RT_TEXTURES;
	bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout.bindingCount = 2;
	layout.pBindings = bindings;
	if (rt.create_layout(rt.device, &layout, NULL, &rt.hit_layout) != VK_SUCCESS)
	{
		rt.hit_layout = VK_NULL_HANDLE;
		return 0;
	}
	sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	sizes[0].descriptorCount = RT_HIT_SETS;
	sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	sizes[1].descriptorCount = RT_HIT_SETS * XGPU_WORLD_RT_TEXTURES;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = RT_HIT_SETS;
	pool.poolSizeCount = 2;
	pool.pPoolSizes = sizes;
	if (rt.create_pool(rt.device, &pool, NULL, &rt.hit_pool) != VK_SUCCESS)
	{
		rt.hit_pool = VK_NULL_HANDLE;
		return 0;
	}
	for (which = 0; which < RT_HIT_SETS; which++)
		layouts[which] = rt.hit_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = rt.hit_pool;
	allocate.descriptorSetCount = RT_HIT_SETS;
	allocate.pSetLayouts = layouts;
	if (rt.allocate_sets(rt.device, &allocate, rt.hit_sets) != VK_SUCCESS)
	{
		rt.destroy_pool(rt.device, rt.hit_pool, NULL);
		rt.hit_pool = VK_NULL_HANDLE;
		return 0;
	}
	rt.hit_current = -1;
	return 1;
}

/* a mesh's build's input: its triangles, or an indexed one's positions
and corners (each aligned) */
static VkDeviceSize rt_mesh_input_bytes(const struct rt_mesh *mesh)
{
	if (!mesh->indexed)
		return ((VkDeviceSize)mesh->count * 36u + 15u) & ~(VkDeviceSize)15u;
	return (((VkDeviceSize)mesh->vertex_count * 12u + 15u) & ~(VkDeviceSize)15u) +
		(((VkDeviceSize)mesh->count * 12u + 15u) & ~(VkDeviceSize)15u);
}

/* the meshes not yet built, built in one call into the early commands (a
mesh that cannot be is left out from then on) */
static void rt_meshes_build(VkCommandBuffer commands)
{
	VkAccelerationStructureBuildGeometryInfoKHR *infos;
	VkAccelerationStructureGeometryKHR *geometries;
	VkAccelerationStructureBuildRangeInfoKHR *ranges;
	const VkAccelerationStructureBuildRangeInfoKHR **range_pointers;
	struct rt_buffer vertices, scratch;
	VkDeviceSize vertex_bytes = 0, scratch_bytes = 0, *scratch_offsets, built_bytes = 0, largest_bytes = 0;
	uint32_t id, count = 0, index, triangles = 0, largest = 0;
	VkMemoryBarrier barrier;
	int logged, own;
	uint32_t geometry_count = 0;

	if (!rt.pending)
		return;
	infos = calloc(rt.pending, sizeof(*infos));
	/* (two geometries a mesh at most: an indexed one's opaque and
	alpha-tested triangles) */
	geometries = calloc(rt.pending * 2, sizeof(*geometries));
	ranges = calloc(rt.pending * 2, sizeof(*ranges));
	range_pointers = calloc(rt.pending, sizeof(*range_pointers));
	scratch_offsets = calloc(rt.pending, sizeof(*scratch_offsets));
	memset(&vertices, 0, sizeof(vertices));
	memset(&scratch, 0, sizeof(scratch));
	if (!infos || !geometries || !ranges || !range_pointers || !scratch_offsets)
		goto done;
	for (id = 1; id < XGPU_STREAM_WORLD_AO_MESHES; id++)
	{
		if (rt.meshes[id] && !rt.meshes[id]->built)
			vertex_bytes += rt_mesh_input_bytes(rt.meshes[id]);
	}
	if (!rt_buffer_make(vertex_bytes, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1, &vertices))
		goto done;
	vertex_bytes = 0;
	for (id = 1; id < XGPU_STREAM_WORLD_AO_MESHES && count < rt.pending; id++)
	{
		struct rt_mesh *mesh = rt.meshes[id];
		VkAccelerationStructureBuildSizesInfoKHR sizes;
		VkAccelerationStructureCreateInfoKHR create;
		VkAccelerationStructureDeviceAddressInfoKHR address;
		VkAccelerationStructureGeometryKHR *geometry = &geometries[geometry_count];
		VkAccelerationStructureBuildGeometryInfoKHR *info = &infos[count];
		uint32_t primitives[2], geometry_index, mesh_geometries;

		if (!mesh || mesh->built)
			continue;
		/* (left out from now on, whether it is built or not) */
		mesh->built = 1;
		/* its geometries: a mesh of triangles' one, an indexed one's opaque
		triangles (geometry 0, opaque: never a ray's candidate) and, where
		it has them, its alpha-tested ones (geometry 1, which the pass's
		candidate loop tests), their corners after its positions */
		mesh_geometries = mesh->indexed && mesh->opaque_count < mesh->count ? 2 : 1;
		primitives[0] = mesh->indexed ? mesh->opaque_count : mesh->count;
		primitives[1] = mesh->count - primitives[0];
		for (geometry_index = 0; geometry_index < mesh_geometries; geometry_index++)
		{
			VkAccelerationStructureGeometryKHR *each = &geometry[geometry_index];

			each->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
			each->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			each->geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			each->geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			each->geometry.triangles.vertexData.deviceAddress = vertices.address + vertex_bytes;
			each->geometry.triangles.vertexStride = 12;
			if (mesh->indexed)
			{
				each->geometry.triangles.maxVertex = mesh->vertex_count - 1;
				each->geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
				each->geometry.triangles.indexData.deviceAddress = vertices.address + vertex_bytes +
					(((VkDeviceSize)mesh->vertex_count * 12u + 15u) & ~(VkDeviceSize)15u);
			}
			else
			{
				each->geometry.triangles.maxVertex = mesh->count * 3 - 1;
				each->geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
			}
			each->flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR |
				(geometry_index == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0);
			ranges[geometry_count + geometry_index].primitiveCount = primitives[geometry_index];
			/* (the alpha-tested ones' corners after the opaque ones', in bytes) */
			ranges[geometry_count + geometry_index].primitiveOffset = geometry_index ? primitives[0] * 12u : 0;
		}
		if (mesh->indexed)
		{
			memcpy((unsigned char *)vertices.mapped + vertex_bytes, mesh->positions, (size_t)mesh->vertex_count * 12u);
			memcpy((unsigned char *)vertices.mapped + vertex_bytes + (((VkDeviceSize)mesh->vertex_count * 12u + 15u) &
				~(VkDeviceSize)15u), mesh->indices, (size_t)mesh->count * 12u);
		}
		else
			memcpy((unsigned char *)vertices.mapped + vertex_bytes, mesh->triangles, (size_t)mesh->count * 36u);
		vertex_bytes += rt_mesh_input_bytes(mesh);
		own = mesh->count >= RT_OWN_TRIANGLES;
		info->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
		info->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		info->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
			(own && rt.compacted_sizes ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR : 0);
		info->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		info->geometryCount = mesh_geometries;
		info->pGeometries = geometry;
		memset(&sizes, 0, sizeof(sizes));
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		rt.sizes(rt.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, info, primitives, &sizes);
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		create.size = sizes.accelerationStructureSize;
		create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		if (own)
		{
			if (rt_buffer_make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0,
				&mesh->own))
				create.buffer = mesh->own.buffer;
		}
		else if (!rt_storage_take(sizes.accelerationStructureSize, &create.buffer, &create.offset))
			create.buffer = VK_NULL_HANDLE;
		if (!create.buffer || rt.create(rt.device, &create, NULL, &mesh->structure) != VK_SUCCESS)
		{
			mesh->structure = VK_NULL_HANDLE;
			if (mesh->own.buffer)
				rt_buffer_free(&mesh->own);
			continue;
		}
		mesh->bytes = sizes.accelerationStructureSize;
		rt_live_bytes(mesh->bytes, 0);
		memset(&address, 0, sizeof(address));
		address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		address.accelerationStructure = mesh->structure;
		mesh->address = rt.structure_address(rt.device, &address);
		info->dstAccelerationStructure = mesh->structure;
		built_bytes += sizes.accelerationStructureSize;
		triangles += mesh->count;
		if (mesh->count > largest)
		{
			largest = mesh->count;
			largest_bytes = sizes.accelerationStructureSize;
		}
		scratch_offsets[count] = scratch_bytes;
		scratch_bytes += (sizes.buildScratchSize + rt.scratch_alignment - 1) & ~(VkDeviceSize)(rt.scratch_alignment - 1);
		range_pointers[count] = &ranges[geometry_count];
		geometry_count += mesh_geometries;
		count++;
	}
	if (!count)
		goto done;
	if (!rt_buffer_make(scratch_bytes + rt.scratch_alignment, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &scratch))
	{
		/* (none built: their structures let go of, never named) */
		for (index = 0; index < count; index++)
		{
			for (id = 1; id < XGPU_STREAM_WORLD_AO_MESHES; id++)
			{
				if (rt.meshes[id] && rt.meshes[id]->structure == infos[index].dstAccelerationStructure)
				{
					rt_garbage_add(rt.meshes[id]->own.buffer ? &rt.meshes[id]->own : NULL, rt.meshes[id]->structure);
					memset(&rt.meshes[id]->own, 0, sizeof(rt.meshes[id]->own));
					rt.meshes[id]->structure = VK_NULL_HANDLE;
					rt_live_bytes(0, rt.meshes[id]->bytes);
				}
			}
		}
		goto done;
	}
	for (index = 0; index < count; index++)
	{
		VkDeviceAddress at = (scratch.address + rt.scratch_alignment - 1) & ~(VkDeviceAddress)(rt.scratch_alignment - 1);

		infos[index].scratchData.deviceAddress = at + scratch_offsets[index];
	}
	logged = largest >= RT_LOGGED_TRIANGLES;
	if (logged && rt.times && !rt.timed)
	{
		rt.reset_queries(commands, rt.times, 0, 2);
		rt.timestamp(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, rt.times, 0);
	}
	rt.build(commands, count, infos, range_pointers);
	rt.structure_bytes += built_bytes;
	if (logged)
	{
		if (rt.times && !rt.timed)
		{
			rt.timestamp(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, rt.times, 1);
			rt.timed = 1;
			rt.timed_serial = xgpu_vulkan_serial();
		}
		host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's meshes built: %u, %u triangles, %.2f MB of structures "
			"(the largest %u triangles, %.2f MB, %.1f B a triangle), %.2f MB of scratch; %.2f MB of meshes built so far, "
			"%.2f MB now (at most %.2f MB)", count, triangles, (double)built_bytes / 1048576.0, largest,
			(double)largest_bytes / 1048576.0, (double)largest_bytes / (double)largest, (double)scratch_bytes / 1048576.0,
			(double)rt.structure_bytes / 1048576.0, (double)rt.live_bytes / 1048576.0, (double)rt.live_most / 1048576.0);
	}
	/* (the meshes built before the scene that names them) */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	rt.barrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);
done:
	/* (every mesh tried: their triangles let go of) */
	for (id = 1; id < XGPU_STREAM_WORLD_AO_MESHES; id++)
	{
		if (rt.meshes[id] && rt.meshes[id]->built)
		{
			free(rt.meshes[id]->triangles);
			free(rt.meshes[id]->positions);
			free(rt.meshes[id]->indices);
			rt.meshes[id]->triangles = rt.meshes[id]->positions = NULL;
			rt.meshes[id]->indices = NULL;
		}
	}
	rt.pending = 0;
	if (vertices.buffer)
		rt_garbage_add(&vertices, VK_NULL_HANDLE);
	if (scratch.buffer)
		rt_garbage_add(&scratch, VK_NULL_HANDLE);
	free(infos);
	free(geometries);
	free(ranges);
	free(range_pointers);
	free(scratch_offsets);
}

/* the large meshes built (their builds' barrier recorded) and not yet
compacted: their compacted sizes asked, where none are being, read a few
frames later */
static void rt_compaction_ask(VkCommandBuffer commands)
{
	VkAccelerationStructureKHR structures[RT_COMPACTED];
	uint32_t id, count = 0;

	if (!rt.compacted_sizes || rt.compacted_count)
		return;
	for (id = 1; id < XGPU_STREAM_WORLD_AO_MESHES && count < RT_COMPACTED; id++)
	{
		struct rt_mesh *mesh = rt.meshes[id];

		if (!mesh || !mesh->built || !mesh->structure || !mesh->own.buffer || mesh->compaction_asked)
			continue;
		mesh->compaction_asked = 1;
		mesh->compacting = count + 1;
		structures[count] = mesh->structure;
		rt.compacted_ids[count++] = id;
	}
	if (!count)
		return;
	rt.reset_queries(commands, rt.compacted_sizes, 0, count);
	rt.properties(commands, count, structures, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
		rt.compacted_sizes, 0);
	rt.compacted_count = count;
	rt.compacted_serial = xgpu_vulkan_serial();
}

/* the large meshes whose compacted sizes the GPU has written, copied into
structures of those sizes (in the early commands, before the scene that
names them), the first let go of */
static void rt_meshes_compact(VkCommandBuffer commands)
{
	VkDeviceSize sizes[RT_COMPACTED];
	VkDeviceSize before = 0, after = 0;
	uint32_t index, copied = 0;

	if (rt.query_results(rt.device, rt.compacted_sizes, 0, rt.compacted_count, sizeof(sizes), sizes, sizeof(sizes[0]),
		VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
	{
		rt.compacted_count = 0;
		return;
	}
	for (index = 0; index < rt.compacted_count; index++)
	{
		uint32_t id = rt.compacted_ids[index];
		struct rt_mesh *mesh = id ? rt.meshes[id] : NULL;
		VkAccelerationStructureCreateInfoKHR create;
		VkCopyAccelerationStructureInfoKHR copy;
		VkAccelerationStructureDeviceAddressInfoKHR address;
		VkAccelerationStructureKHR compact = VK_NULL_HANDLE;
		struct rt_buffer buffer;

		if (!mesh || !mesh->structure || mesh->compacting != index + 1)
			continue;
		mesh->compacting = 0;
		if (!sizes[index] || sizes[index] >= mesh->bytes ||
			!rt_buffer_make(sizes[index], VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0, &buffer))
			continue;
		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
		create.buffer = buffer.buffer;
		create.size = sizes[index];
		create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		if (rt.create(rt.device, &create, NULL, &compact) != VK_SUCCESS)
		{
			rt_buffer_free(&buffer);
			continue;
		}
		memset(&copy, 0, sizeof(copy));
		copy.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
		copy.src = mesh->structure;
		copy.dst = compact;
		copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
		rt.copy(commands, &copy);
		rt_garbage_add(&mesh->own, mesh->structure);
		mesh->own = buffer;
		mesh->structure = compact;
		memset(&address, 0, sizeof(address));
		address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
		address.accelerationStructure = compact;
		mesh->address = rt.structure_address(rt.device, &address);
		before += mesh->bytes;
		after += sizes[index];
		rt_live_bytes(sizes[index], mesh->bytes);
		mesh->bytes = sizes[index];
		copied++;
	}
	rt.compacted_count = 0;
	if (copied)
	{
		VkMemoryBarrier barrier;

		memset(&barrier, 0, sizeof(barrier));
		barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
		barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		rt.barrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
			VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);
		/* (the scene built again, naming the compacted ones) */
		rt.given_new = 1;
		host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's meshes compacted: %u, %.2f MB to %.2f MB; %.2f MB of "
			"meshes now", copied, (double)before / 1048576.0, (double)after / 1048576.0, (double)rt.live_bytes / 1048576.0);
	}
}

/* ---------- the scenes */

void xgpu_vulkan_rt_instances(const struct xgpu_world_ao_instance *instances, uint32_t count)
{
	if (!rt.ready)
		return;
	if (count > rt.given_capacity)
	{
		struct xgpu_world_ao_instance *grown = realloc(rt.given, count * sizeof(*grown));

		if (!grown)
			return;
		rt.given = grown;
		rt.given_capacity = count;
	}
	if (count)
		memcpy(rt.given, instances, count * sizeof(*instances));
	rt.given_count = count;
	rt.given_new = 1;
}

/* a scene's structure, with room for count instances (made again where it
has not: the old let go of once its work is done); 0 if it cannot be */
static int rt_scene_room(struct rt_scene *scene, uint32_t count)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildSizesInfoKHR sizes;
	VkAccelerationStructureCreateInfoKHR create;
	uint32_t capacity;

	if (scene->structure && count <= scene->capacity)
		return 1;
	if (scene->structure)
		rt_garbage_add(&scene->storage, scene->structure);
	if (scene->instances.buffer)
		rt_garbage_add(&scene->instances, VK_NULL_HANDLE);
	memset(&scene->storage, 0, sizeof(scene->storage));
	memset(&scene->instances, 0, sizeof(scene->instances));
	scene->structure = VK_NULL_HANDLE;
	scene->built = 0;
	for (capacity = 64; capacity < count; capacity *= 2)
		;
	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	rt.sizes(rt.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &capacity, &sizes);
	/* (its scratch after its structure, in the same buffer) */
	if (!rt_buffer_make(((sizes.accelerationStructureSize + rt.scratch_alignment - 1) &
		~(VkDeviceSize)(rt.scratch_alignment - 1)) + sizes.buildScratchSize + rt.scratch_alignment,
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0, &scene->storage))
		return 0;
	if (!rt_buffer_make((VkDeviceSize)capacity * sizeof(VkAccelerationStructureInstanceKHR),
		VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1, &scene->instances))
	{
		rt_buffer_free(&scene->storage);
		return 0;
	}
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	create.buffer = scene->storage.buffer;
	create.size = sizes.accelerationStructureSize;
	create.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	if (rt.create(rt.device, &create, NULL, &scene->structure) != VK_SUCCESS)
	{
		rt_buffer_free(&scene->storage);
		rt_buffer_free(&scene->instances);
		scene->structure = VK_NULL_HANDLE;
		return 0;
	}
	scene->capacity = capacity;
	return 1;
}

/* an instance as the device takes it (0: left out) */
static int rt_instance_of(const struct xgpu_world_ao_instance *given, VkAccelerationStructureInstanceKHR *instance)
{
	const struct rt_mesh *mesh = given->mesh < XGPU_STREAM_WORLD_AO_MESHES ? rt.meshes[given->mesh] : NULL;
	int row, column;

	if (!mesh || !mesh->structure || given->owner >= XGPU_WORLD_AO_INSTANCE_OWNERS ||
		(given->kind != XGPU_WORLD_AO_INSTANCE_STRUCTURE && given->kind != XGPU_WORLD_AO_INSTANCE_OBJECT))
		return 0;
	memset(instance, 0, sizeof(*instance));
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 4; column++)
		{
			float value = given->transform[row][column];

			if (!isfinite(value) || fabsf(value) > 1.0e6f)
				return 0;
			instance->transform.matrix[row][column] = value;
		}
	}
	instance->instanceCustomIndex = given->owner;
	instance->mask = given->kind;
	/* (its mesh, for the hit table: ray queries have no other use for it) */
	instance->instanceShaderBindingTableRecordOffset = given->mesh;
	/* (the structure's opaque geometry stops a ray at once, its
	alpha-tested one is a candidate: the meshes' geometries say which; an
	object's every triangle a candidate, so that the pass leaves a pixel's
	own out) */
	instance->flags = given->kind == XGPU_WORLD_AO_INSTANCE_STRUCTURE ?
		VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR :
		VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR | (rt.flip ? VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR : 0);
	instance->accelerationStructureReference = mesh->address;
	return 1;
}

/* a scene's instances built into its structure, in the commands */
static void rt_scene_build(VkCommandBuffer commands, struct rt_scene *scene, uint32_t count)
{
	VkAccelerationStructureGeometryKHR geometry;
	VkAccelerationStructureBuildGeometryInfoKHR info;
	VkAccelerationStructureBuildRangeInfoKHR range;
	const VkAccelerationStructureBuildRangeInfoKHR *ranges = &range;
	VkDeviceSize structure_bytes;
	VkMemoryBarrier barrier;
	VkAccelerationStructureBuildSizesInfoKHR sizes;

	memset(&geometry, 0, sizeof(geometry));
	geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geometry.geometry.instances.data.deviceAddress = scene->instances.address;
	memset(&info, 0, sizeof(info));
	info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info.geometryCount = 1;
	info.pGeometries = &geometry;
	info.dstAccelerationStructure = scene->structure;
	memset(&sizes, 0, sizeof(sizes));
	sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	rt.sizes(rt.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &scene->capacity, &sizes);
	structure_bytes = (sizes.accelerationStructureSize + rt.scratch_alignment - 1) &
		~(VkDeviceSize)(rt.scratch_alignment - 1);
	info.scratchData.deviceAddress = ((scene->storage.address + rt.scratch_alignment - 1) &
		~(VkDeviceAddress)(rt.scratch_alignment - 1)) + structure_bytes;
	memset(&range, 0, sizeof(range));
	range.primitiveCount = count;
	rt.build(commands, 1, &info, &ranges);
	/* (built before the pass's ray queries read it) */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	rt.barrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0,
		NULL, 0, NULL);
	scene->built = 1;
}

/* the empty scene, made and built once (0 if it cannot be) */
static int rt_empty_scene(void)
{
	if (rt.empty.built)
		return 1;
	if (!rt.empty.structure && !rt_scene_room(&rt.empty, 0))
		return 0;
	rt_scene_build(xgpu_vulkan_early_commands(), &rt.empty, 0);
	return 1;
}

int xgpu_vulkan_rt_build(void)
{
	uint64_t serial = xgpu_vulkan_serial(), done;
	VkCommandBuffer commands;
	struct rt_scene *scene = NULL;
	VkAccelerationStructureInstanceKHR *written;
	uint32_t index, count = 0;
	int which;

	if (!rt.ready)
		return 0;
	rt_build_time_read();
	rt_textures_find();
	if (rt.compacted_count && xgpu_vulkan_completed() >= rt.compacted_serial)
		rt_meshes_compact(xgpu_vulkan_early_commands());
	if (!rt.given_new)
		return rt.current >= 0;
	rt_garbage_collect();
	commands = xgpu_vulkan_early_commands();
	rt_meshes_build(commands);
	rt_compaction_ask(commands);
	rt.given_new = 0;
	rt.current = -1;
	xgpu_vulkan_world_ao_touch();
	if (!rt.given_count)
		return 0;
	/* (a scene no work recorded so far reads, the GPU done with it; the
	oldest waited for where none is) */
	done = xgpu_vulkan_completed();
	for (which = 0; which < RT_SCENES && !scene; which++)
	{
		if (rt.scenes[which].used < serial && rt.scenes[which].used <= done)
			scene = &rt.scenes[which];
	}
	if (!scene)
	{
		struct rt_scene *oldest = &rt.scenes[0];

		for (which = 1; which < RT_SCENES; which++)
		{
			if (rt.scenes[which].used < oldest->used)
				oldest = &rt.scenes[which];
		}
		if (oldest->used >= serial)
			return 0;
		xgpu_vulkan_wait(oldest->used);
		scene = oldest;
	}
	if (!rt_scene_room(scene, rt.given_count))
	{
		if (!rt.failed_said)
			host_logf(HOST_LOG_WARN, "vulkan: display.ambient_occlusion \"ray_traced\"'s scene cannot be made: drawn as \"world\"");
		rt.failed_said = 1;
		return 0;
	}
	written = scene->instances.mapped;
	for (index = 0; index < rt.given_count; index++)
		count += (uint32_t)rt_instance_of(&rt.given[index], &written[count]);
	rt_scene_build(commands, scene, count);
	scene->used = serial;
	rt.current = (int)(scene - rt.scenes);
	return 1;
}

VkAccelerationStructureKHR xgpu_vulkan_rt_scene(void)
{
	struct rt_scene *scene;

	if (!rt.ready)
		return VK_NULL_HANDLE;
	scene = rt.current >= 0 ? &rt.scenes[rt.current] : &rt.empty;
	if (scene == &rt.empty && !rt_empty_scene())
		return VK_NULL_HANDLE;
	scene->used = xgpu_vulkan_serial();
	return scene->structure;
}

int xgpu_vulkan_rt_ready(void)
{
	return rt.ready;
}

/* ---------- the start and end */

int xgpu_vulkan_rt_create(const struct xgpu_vulkan_rt_device *device)
{
	memset(&rt, 0, sizeof(rt));
	rt.device = device->device;
	rt.memory = device->memory;
	rt.scratch_alignment = device->scratch_alignment ? device->scratch_alignment : 256u;
	rt.current = -1;
	rt.flip = getenv("HALO_VULKAN_RT_FLIP") && atoi(getenv("HALO_VULKAN_RT_FLIP")) != 0;
#define LOAD(name, field) rt.field = (PFN_##name)device->get_device_proc_addr(device->device, #name);
	LOAD(vkCreateAccelerationStructureKHR, create)
	LOAD(vkDestroyAccelerationStructureKHR, destroy)
	LOAD(vkGetAccelerationStructureBuildSizesKHR, sizes)
	LOAD(vkCmdBuildAccelerationStructuresKHR, build)
	LOAD(vkGetAccelerationStructureDeviceAddressKHR, structure_address)
	LOAD(vkGetBufferDeviceAddress, buffer_address)
	LOAD(vkCreateBuffer, create_buffer)
	LOAD(vkDestroyBuffer, destroy_buffer)
	LOAD(vkGetBufferMemoryRequirements, requirements)
	LOAD(vkAllocateMemory, allocate)
	LOAD(vkFreeMemory, free_memory)
	LOAD(vkBindBufferMemory, bind)
	LOAD(vkMapMemory, map)
	LOAD(vkCmdPipelineBarrier, barrier)
	LOAD(vkCreateQueryPool, create_queries)
	LOAD(vkDestroyQueryPool, destroy_queries)
	LOAD(vkCmdResetQueryPool, reset_queries)
	LOAD(vkCmdWriteTimestamp, timestamp)
	LOAD(vkGetQueryPoolResults, query_results)
	LOAD(vkCmdWriteAccelerationStructuresPropertiesKHR, properties)
	LOAD(vkCmdCopyAccelerationStructureKHR, copy)
	LOAD(vkCreateDescriptorSetLayout, create_layout)
	LOAD(vkDestroyDescriptorSetLayout, destroy_layout)
	LOAD(vkCreateDescriptorPool, create_pool)
	LOAD(vkDestroyDescriptorPool, destroy_pool)
	LOAD(vkAllocateDescriptorSets, allocate_sets)
	LOAD(vkUpdateDescriptorSets, update_sets)
#undef LOAD
	rt.ready = rt.create && rt.destroy && rt.sizes && rt.build && rt.structure_address && rt.buffer_address &&
		rt.create_buffer && rt.destroy_buffer && rt.requirements && rt.allocate && rt.free_memory && rt.bind && rt.map &&
		rt.barrier;
	/* (the builds' GPU time, where the queue has timestamps) */
	rt.timestamp_period = device->timestamp_period;
	if (rt.ready && rt.timestamp_period > 0.0f && rt.create_queries && rt.destroy_queries && rt.reset_queries &&
		rt.timestamp && rt.query_results)
	{
		VkQueryPoolCreateInfo create;

		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		create.queryType = VK_QUERY_TYPE_TIMESTAMP;
		create.queryCount = 2;
		if (rt.create_queries(rt.device, &create, NULL, &rt.times) != VK_SUCCESS)
			rt.times = VK_NULL_HANDLE;
	}
	/* (what a hit reads: without its sets, no ray queries, as the shaders
	that trace bind them) */
	rt.hit_current = -1;
	if (rt.ready && !rt_hit_sets_make())
		rt.ready = 0;
	/* (the large meshes' compaction) */
	if (rt.ready && rt.properties && rt.copy && rt.create_queries && rt.reset_queries && rt.query_results)
	{
		VkQueryPoolCreateInfo create;

		memset(&create, 0, sizeof(create));
		create.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
		create.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
		create.queryCount = RT_COMPACTED;
		if (rt.create_queries(rt.device, &create, NULL, &rt.compacted_sizes) != VK_SUCCESS)
			rt.compacted_sizes = VK_NULL_HANDLE;
	}
	return rt.ready;
}

void xgpu_vulkan_rt_destroy(void)
{
	int which;

	if (!rt.ready)
		return;
	rt_meshes_release();
	for (which = 0; which < RT_SCENES; which++)
	{
		if (rt.scenes[which].structure)
			rt.destroy(rt.device, rt.scenes[which].structure, NULL);
		rt_buffer_free(&rt.scenes[which].storage);
		rt_buffer_free(&rt.scenes[which].instances);
	}
	if (rt.empty.structure)
		rt.destroy(rt.device, rt.empty.structure, NULL);
	rt_buffer_free(&rt.empty.storage);
	rt_buffer_free(&rt.empty.instances);
	/* (the caller has waited for the device) */
	while (rt.garbage_count)
	{
		struct rt_garbage *item = &rt.garbage[--rt.garbage_count];

		if (item->structure)
			rt.destroy(rt.device, item->structure, NULL);
		rt_buffer_free(&item->buffer);
	}
	if (rt.times)
		rt.destroy_queries(rt.device, rt.times, NULL);
	if (rt.compacted_sizes)
		rt.destroy_queries(rt.device, rt.compacted_sizes, NULL);
	rt_buffer_free(&rt.hit_buffer);
	free(rt.hit_words);
	if (rt.stand_in)
		xgpu_vulkan_image_destroy(rt.stand_in);
	if (rt.hit_pool)
		rt.destroy_pool(rt.device, rt.hit_pool, NULL);
	if (rt.hit_layout)
		rt.destroy_layout(rt.device, rt.hit_layout, NULL);
	free(rt.garbage);
	free(rt.given);
	memset(&rt, 0, sizeof(rt));
}
