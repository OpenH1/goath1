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
- A skinned mesh (a biped's: its joints blend two nodes) has no structure
  of its own: its rest vertices go into the rest buffer, and each object of
  it posed in a frame (its owner's) has its own, skinned in the early
  commands by a compute pass (xgpu_spirv_rt_skin: the game's linear blend of
  two nodes) into the output buffer, then built once (allowing updates) and
  refit from then on, never built again while its mesh is the same. An
  object whose pose is what it was last skinned in is neither skinned nor
  refit. One not posed for RT_SKINNED_IDLE frames is let go of. The
  skinning's and refits' GPU time is logged every RT_SKIN_LOGGED frames.
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
#define RT_HIT_SETS 8u
/* (as the shaders read it: xgpu_spirv.h's XGPU_SPIRV_RT_HIT_*, which
xgpu_vulkan_stream.c checks against these) */
/* the skinning (xgpu_vulkan_rt_skin): its frames in turn, the skinned
objects kept at most (one not posed for RT_SKINNED_IDLE frames let go of),
the rest vertices' and the skinned positions' words, a frame's jobs and
matrices' words, and the frames its GPU time is logged over */
#define RT_SKIN_FRAMES 3u
#define RT_SKINNED 1024u
#define RT_SKINNED_IDLE 300u
#define RT_SKIN_RESTS (4u << 20)
#define RT_SKIN_OUTS (4u << 20)
/* (the rest and output buffers start at so many words, and double as they
must, up to RT_SKIN_RESTS and _OUTS: a map's few characters need a few
hundred thousand) */
#define RT_SKIN_FIRST_WORDS (1u << 18)
#define RT_SKIN_FRAME_WORDS (XGPU_WORLD_RT_POSES * 4u + (1u << 19))
#define RT_SKIN_LOGGED 600u
/* the scene's memory is logged every so many frames (rt_memory_log) */
#define RT_MEMORY_LOGGED 1800u
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
	/* (a skinned one: xgpu_vulkan_rt_skin) its rest vertices' first word in
	the rest buffer, its corners for its objects' builds */
	int skinned;
	uint32_t rest_at;
	struct rt_buffer corners;
};

/* a skinned object (its owner's): its mesh, its structure (refit from the
second frame it is posed), where its skinned positions are in the skinning's
output, and the pose it was last skinned in (not skinned again while it
holds it: a biped standing still) */
struct rt_skinned
{
	uint32_t owner, mesh;
	VkAccelerationStructureKHR structure;
	VkDeviceAddress address;
	struct rt_buffer own;
	VkDeviceSize bytes, update_scratch;
	uint32_t out_at, vertex_count;
	uint64_t posed_frame;
	int built;
	float last[XGPU_WORLD_RT_POSE_NODES][3][4];
	uint32_t node_count;
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
	/* the skinning: its pipeline (made at its first use), the rest
	vertices' buffer (words used), the output's (its slots: bump, then the
	freed ones taken again by size), the frames' jobs and matrices in turn
	(each its last submission), the refits' scratch, the frame's poses, the
	skinned objects, and (debug.pass_times-like) its GPU time's timestamps */
	PFN_vkCreateShaderModule create_module;
	PFN_vkDestroyShaderModule destroy_module;
	PFN_vkCreatePipelineLayout create_pipeline_layout;
	PFN_vkDestroyPipelineLayout destroy_pipeline_layout;
	PFN_vkCreateComputePipelines create_compute;
	PFN_vkDestroyPipeline destroy_pipeline;
	PFN_vkCmdBindPipeline bind_pipeline;
	PFN_vkCmdBindDescriptorSets bind_sets;
	PFN_vkCmdDispatch dispatch;
	int skin_failed;
	VkDescriptorSetLayout skin_layout;
	VkPipelineLayout skin_pipeline_layout;
	VkPipeline skin_pipeline;
	VkDescriptorPool skin_pool;
	VkDescriptorSet skin_sets[RT_SKIN_FRAMES];
	struct rt_buffer skin_frames[RT_SKIN_FRAMES];
	uint64_t skin_frame_used[RT_SKIN_FRAMES];
	struct rt_buffer rests, outs, skin_scratch;
	uint32_t rests_used, outs_used, outs_wanted;
	struct rt_slot { uint32_t at, count; } free_outs[RT_SKINNED];
	uint32_t free_out_count;
	struct xgpu_world_rt_pose *poses;
	uint32_t pose_count, posed_count;
	VkDeviceAddress pose_addresses[XGPU_WORLD_RT_POSES];
	struct rt_skinned *skinned[RT_SKINNED];
	uint32_t skinned_count;
	uint64_t skin_frame;
	VkQueryPool skin_times;
	int skin_timed;
	uint64_t skin_timed_serial;
	double skin_ms;
	uint32_t skin_samples, skin_objects, skin_refits, skin_counted;
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
	uint32_t memory_frames;
} rt;

/* ---------- memory */

static void rt_skinned_release(uint32_t index);
static int rt_skinned_mesh_keep(struct rt_mesh *mesh, const float *positions, const uint32_t *skin);

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

/* the scene's memory logged (when: "now", or "at the map's end, let go
of"): its meshes' structures (the bottom levels: the most this map, and
now), the scenes' (the top levels and their instances), the hit table's
and the skinning's buffers */
static void rt_memory_log(const char *when)
{
	VkDeviceSize scenes = 0, chunks = 0;
	int which;
	uint32_t index;

	for (which = 0; which < RT_SCENES; which++)
		scenes += rt.scenes[which].storage.size + rt.scenes[which].instances.size;
	for (index = 0; index < rt.chunk_count; index++)
		chunks += rt.chunks[index].size;
	host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's memory %s: bottom levels at most %.2f MB (%.2f MB now, "
		"%.2f MB of storage), top levels %.2f MB, hit table %.2f MB (%.2f MB used), skinning %.2f MB", when,
		(double)rt.live_most / 1048576.0, (double)rt.live_bytes / 1048576.0, (double)chunks / 1048576.0,
		(double)scenes / 1048576.0, (double)rt.hit_buffer.size / 1048576.0, (double)rt.hit_used * 4.0 / 1048576.0,
		(double)(rt.rests.size + rt.outs.size + rt.skin_scratch.size) / 1048576.0);
}

static void rt_meshes_release(void)
{
	uint32_t id;

	/* (a map's: logged once, as it is let go of) */
	if (rt.live_most || rt.hit_used)
		rt_memory_log("at the map's end, let go of");

	for (id = 0; id < XGPU_STREAM_WORLD_AO_MESHES; id++)
	{
		struct rt_mesh *mesh = rt.meshes[id];

		if (!mesh)
			continue;
		if (mesh->structure)
			rt_garbage_add(mesh->own.buffer ? &mesh->own : NULL, mesh->structure);
		if (mesh->corners.buffer)
			rt_garbage_add(&mesh->corners, VK_NULL_HANDLE);
		free(mesh->triangles);
		free(mesh->positions);
		free(mesh->indices);
		free(mesh);
		rt.meshes[id] = NULL;
	}
	rt.compacted_count = 0;
	rt.live_bytes = rt.live_most = 0;
	/* (the skinned objects, their meshes' rest vertices and positions: new
	ones, the GPU may still read the old) */
	while (rt.skinned_count)
		rt_skinned_release(rt.skinned_count - 1);
	rt.free_out_count = 0;
	if (rt.rests.buffer)
		rt_garbage_add(&rt.rests, VK_NULL_HANDLE);
	if (rt.outs.buffer)
		rt_garbage_add(&rt.outs, VK_NULL_HANDLE);
	memset(&rt.rests, 0, sizeof(rt.rests));
	memset(&rt.outs, 0, sizeof(rt.outs));
	rt.rests_used = rt.outs_used = rt.outs_wanted = 0;
	rt.pose_count = 0;
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
	if (mesh->corners.buffer)
		rt_garbage_add(&mesh->corners, VK_NULL_HANDLE);
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

void xgpu_vulkan_rt_mesh_indexed(uint32_t id, const void *data, uint32_t size, int skinned)
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
	/* (a skinned one's skinning after: two words a vertex) */
	if (skinned)
		expected += (uint64_t)vertex_count * 8u;
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
	if (skinned)
	{
		const uint32_t *skin = (const uint32_t *)(materials + header->material_count);

		for (index = 0; index < vertex_count; index++)
		{
			float weight;

			memcpy(&weight, &skin[index * 2u + 1u], 4);
			if ((skin[index * 2u] & 0xFFFFu) != skin[index * 2u] || (skin[index * 2u] & 0xFFu) >= XGPU_WORLD_RT_POSE_NODES ||
				(skin[index * 2u] >> 8) >= XGPU_WORLD_RT_POSE_NODES || !(weight >= 0.0f && weight <= 1.0f))
				return;
		}
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
	/* (a skinned one: its rest vertices for the skinning, its corners kept
	for its objects' builds; no structure of its own) */
	if (skinned && !rt_skinned_mesh_keep(mesh, positions, (const uint32_t *)(materials + header->material_count)))
	{
		free(mesh->positions);
		free(mesh->indices);
		free(mesh);
		return;
	}
	/* (a material once a map: the GPU may be reading it) */
	for (index = 0; index < header->material_count; index++)
	{
		if (rt.hit_materials[materials[index].index])
			continue;
		rt.hit_materials[materials[index].index] = 1;
		rt_hit_write(RT_HIT_MATERIAL + materials[index].index * 16u, &materials[index], 16);
	}
	rt.meshes[id] = mesh;
	if (!mesh->skinned)
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

/* the hit sets' stand-in texture (white, 1x1) and their sampler, made
once: what a set names in every slot that has no image of its own, so a
set can be made whether or not the scene has been built (its draws are
drawn from the first frame: xgpu_vulkan_rt_hit_set); FALSE if out of
memory */
static int rt_stand_in_make(void)
{
	static const unsigned char white[4] = { 255, 255, 255, 255 };
	struct xgpu_vulkan_image_description description;
	struct xgpu_vulkan_sampler_description sampler;

	if (rt.stand_in && rt.sampler)
		return 1;
	if (!rt.stand_in)
	{
		memset(&description, 0, sizeof(description));
		description.type = XGPU_VULKAN_IMAGE_2D;
		description.format = XGPU_VULKAN_FORMAT_RGBA8;
		description.usage = XGPU_VULKAN_USAGE_SAMPLED;
		description.width = description.height = description.depth_or_layers = description.levels = 1;
		rt.stand_in = xgpu_vulkan_image_create(&description);
		if (!rt.stand_in)
			return 0;
		xgpu_vulkan_image_upload(rt.stand_in, 0, 0, 0, 0, 0, 1, 1, 1, white, 4, 4);
	}
	if (!rt.sampler)
	{
		memset(&sampler, 0, sizeof(sampler));
		sampler.minify = sampler.magnify = XGPU_VULKAN_FILTER_LINEAR;
		sampler.mip = XGPU_VULKAN_MIP_LINEAR;
		sampler.address[0] = sampler.address[1] = sampler.address[2] = XGPU_VULKAN_ADDRESS_WRAP;
		sampler.anisotropy = 1;
		rt.sampler = xgpu_vulkan_sampler(&sampler);
	}
	rt.hit_version++;
	return rt.sampler != VK_NULL_HANDLE;
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
	if (!rt_stand_in_make())
		return;
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

void xgpu_vulkan_rt_textures_again(void)
{
	/* (a replay played from its start again: the slots as at the stream's
	start, none given, until the stream gives them again; each image then
	found, RT_TEXTURES_FOUND a frame) */
	memset(rt.textures, 0, sizeof(rt.textures));
	memset(rt.texture_views, 0, sizeof(rt.texture_views));
	rt.textures_serial = 0;
	rt.hit_version++;
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
	/* (a table and the stand-in made, so a set has something to name: here,
	not only as the scene's textures are found, which a frame with no scene
	never does; without a set every draw of display.ambient_occlusion
	"world" was dropped, the level's structure with them, until a scene was
	first built) */
	if (!rt.hit_buffer.buffer && !rt_hit_room(0))
		return VK_NULL_HANDLE;
	if (!rt_stand_in_make())
		return VK_NULL_HANDLE;
	stand_in = xgpu_vulkan_image_view(rt.stand_in);
	if (!stand_in || !rt.sampler)
		return VK_NULL_HANDLE;
	if (rt.hit_current >= 0 && rt.hit_set_versions[rt.hit_current] == rt.hit_version)
	{
		rt.hit_set_used[rt.hit_current] = serial;
		return rt.hit_sets[rt.hit_current];
	}
	/* (one no work not yet done reads; none: the oldest waited for, an
	earlier submission's, never a set kept as it was: the table it names
	may have grown since and the old one be let go of once the GPU is done
	with the submissions that read it, so a later one must not; the last,
	as it was, where every other is this submission's, whose table is let
	go of after it) */
	for (which = 0; which < RT_HIT_SETS; which++)
	{
		if ((int)which != rt.hit_current && rt.hit_set_used[which] < serial && rt.hit_set_used[which] <= done)
			break;
	}
	if (which == RT_HIT_SETS)
	{
		uint32_t oldest = RT_HIT_SETS;

		for (which = 0; which < RT_HIT_SETS; which++)
		{
			if ((int)which != rt.hit_current && rt.hit_set_used[which] < serial &&
				(oldest == RT_HIT_SETS || rt.hit_set_used[which] < rt.hit_set_used[oldest]))
				oldest = which;
		}
		if (oldest == RT_HIT_SETS)
		{
			if (rt.hit_current < 0)
				return VK_NULL_HANDLE;
			rt.hit_set_used[rt.hit_current] = serial;
			return rt.hit_sets[rt.hit_current];
		}
		xgpu_vulkan_wait(rt.hit_set_used[oldest]);
		which = oldest;
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

/* ---------- the skinning */

/* the words a skin buffer of so many used needs room for more, doubled
from RT_SKIN_FIRST_WORDS (0: past most) */
static uint32_t rt_skin_words(uint32_t used, uint32_t more, uint32_t most)
{
	uint64_t words = RT_SKIN_FIRST_WORDS;

	while (words < (uint64_t)used + more)
		words *= 2u;
	return words > most ? 0 : (uint32_t)words;
}

/* the rest buffer made, or made larger, with room for more words (its
words copied: it is host-visible; the old let go of once the GPU is done
with it). FALSE: no room */
static int rt_rests_room(uint32_t more)
{
	uint32_t words = rt_skin_words(rt.rests_used, more, RT_SKIN_RESTS);
	struct rt_buffer grown;

	if (!words)
		return 0;
	if (rt.rests.buffer && (VkDeviceSize)words * 4u <= rt.rests.size)
		return 1;
	if (!rt_buffer_make((VkDeviceSize)words * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &grown))
		return 0;
	if (rt.rests.buffer)
	{
		memcpy(grown.mapped, rt.rests.mapped, (size_t)rt.rests_used * 4u);
		rt_garbage_add(&rt.rests, VK_NULL_HANDLE);
	}
	rt.rests = grown;
	return 1;
}

/* a skinned mesh's rest vertices (position, nodes, weight) into the rest
buffer and its corners into a buffer of their own (FALSE: no room) */
static int rt_skinned_mesh_keep(struct rt_mesh *mesh, const float *positions, const uint32_t *skin)
{
	uint32_t index, *words;

	if (!rt_rests_room(mesh->vertex_count * 5u) || !rt_buffer_make((VkDeviceSize)mesh->count * 12u, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 1,
			&mesh->corners))
	{
		host_logf(HOST_LOG_WARN, "vulkan: the ray-traced scene's skinned meshes have no room: one left out");
		return 0;
	}
	memcpy(mesh->corners.mapped, mesh->indices, (size_t)mesh->count * 12u);
	words = (uint32_t *)rt.rests.mapped + rt.rests_used;
	for (index = 0; index < mesh->vertex_count; index++)
	{
		memcpy(&words[index * 5u], &positions[index * 3u], 12);
		words[index * 5u + 3u] = skin[index * 2u];
		words[index * 5u + 4u] = skin[index * 2u + 1u];
	}
	mesh->rest_at = rt.rests_used;
	rt.rests_used += mesh->vertex_count * 5u;
	mesh->skinned = 1;
	mesh->built = 1;
	free(mesh->positions);
	free(mesh->indices);
	mesh->positions = NULL;
	mesh->indices = NULL;
	return 1;
}

void xgpu_vulkan_rt_poses(const struct xgpu_world_rt_pose *poses, uint32_t count)
{
	if (!rt.ready || !poses)
		return;
	if (!rt.poses)
	{
		rt.poses = malloc(sizeof(*rt.poses) * XGPU_WORLD_RT_POSES);
		if (!rt.poses)
			return;
	}
	if (count > XGPU_WORLD_RT_POSES)
		count = XGPU_WORLD_RT_POSES;
	memcpy(rt.poses, poses, count * sizeof(*poses));
	rt.pose_count = count;
}

/* the skinning's pipeline, set layout, pool and sets (FALSE: none: no
skinned object is traced) */
static int rt_skin_pipeline_make(void)
{
	uint32_t *words = NULL, word_count = 0;
	VkShaderModuleCreateInfo shader;
	VkShaderModule shader_module = VK_NULL_HANDLE;
	VkDescriptorSetLayoutBinding bindings[3];
	VkDescriptorSetLayoutCreateInfo layout;
	VkPipelineLayoutCreateInfo pipeline_layout;
	VkComputePipelineCreateInfo pipeline;
	VkDescriptorPoolSize size;
	VkDescriptorPoolCreateInfo pool;
	VkDescriptorSetLayout layouts[RT_SKIN_FRAMES];
	VkDescriptorSetAllocateInfo allocate;
	uint32_t which;
	int made = 0;

	if (rt.skin_pipeline)
		return 1;
	if (rt.skin_failed || !rt.create_module || !rt.destroy_module || !rt.create_pipeline_layout || !rt.create_compute ||
		!rt.bind_pipeline || !rt.bind_sets || !rt.dispatch || !xgpu_vulkan_rt_skin_shader(&words, &word_count))
	{
		rt.skin_failed = 1;
		return 0;
	}
	memset(&shader, 0, sizeof(shader));
	shader.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	shader.codeSize = word_count * 4u;
	shader.pCode = words;
	if (rt.create_module(rt.device, &shader, NULL, &shader_module) != VK_SUCCESS)
		goto done;
	memset(bindings, 0, sizeof(bindings));
	for (which = 0; which < 3; which++)
	{
		bindings[which].binding = which;
		bindings[which].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[which].descriptorCount = 1;
		bindings[which].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	memset(&layout, 0, sizeof(layout));
	layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
	layout.bindingCount = 3;
	layout.pBindings = bindings;
	if (rt.create_layout(rt.device, &layout, NULL, &rt.skin_layout) != VK_SUCCESS)
		goto done;
	memset(&pipeline_layout, 0, sizeof(pipeline_layout));
	pipeline_layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	pipeline_layout.setLayoutCount = 1;
	pipeline_layout.pSetLayouts = &rt.skin_layout;
	if (rt.create_pipeline_layout(rt.device, &pipeline_layout, NULL, &rt.skin_pipeline_layout) != VK_SUCCESS)
		goto done;
	memset(&pipeline, 0, sizeof(pipeline));
	pipeline.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	pipeline.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	pipeline.stage.module = shader_module;
	pipeline.stage.pName = "main";
	pipeline.layout = rt.skin_pipeline_layout;
	if (rt.create_compute(rt.device, VK_NULL_HANDLE, 1, &pipeline, NULL, &rt.skin_pipeline) != VK_SUCCESS)
	{
		rt.skin_pipeline = VK_NULL_HANDLE;
		goto done;
	}
	size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	size.descriptorCount = RT_SKIN_FRAMES * 3u;
	memset(&pool, 0, sizeof(pool));
	pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool.maxSets = RT_SKIN_FRAMES;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	if (rt.create_pool(rt.device, &pool, NULL, &rt.skin_pool) != VK_SUCCESS)
		goto done;
	for (which = 0; which < RT_SKIN_FRAMES; which++)
		layouts[which] = rt.skin_layout;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
	allocate.descriptorPool = rt.skin_pool;
	allocate.descriptorSetCount = RT_SKIN_FRAMES;
	allocate.pSetLayouts = layouts;
	made = rt.allocate_sets(rt.device, &allocate, rt.skin_sets) == VK_SUCCESS;
done:
	if (shader_module)
		rt.destroy_module(rt.device, shader_module, NULL);
	free(words);
	if (!made)
	{
		rt.skin_failed = 1;
		host_logf(HOST_LOG_WARN, "vulkan: the ray-traced scene's skinning cannot be made: skinned objects left out");
		if (rt.skin_pipeline)
			rt.destroy_pipeline(rt.device, rt.skin_pipeline, NULL);
		rt.skin_pipeline = VK_NULL_HANDLE;
	}
	return made;
}

/* a skinned object let go of (its structure once the GPU is past the work
recorded so far, its output's slot taken again) */
static void rt_skinned_release(uint32_t index)
{
	struct rt_skinned *object = rt.skinned[index];

	if (object->structure)
		rt_garbage_add(&object->own, object->structure);
	else if (object->own.buffer)
		rt_garbage_add(&object->own, VK_NULL_HANDLE);
	rt_live_bytes(0, object->bytes);
	if (object->vertex_count && rt.free_out_count < RT_SKINNED)
	{
		rt.free_outs[rt.free_out_count].at = object->out_at;
		rt.free_outs[rt.free_out_count].count = object->vertex_count;
		rt.free_out_count++;
	}
	free(object);
	rt.skinned[index] = rt.skinned[--rt.skinned_count];
}

/* the skinned object of an owner with a mesh (made, its output's slot
taken: NULL none can be) */
static struct rt_skinned *rt_skinned_of(uint32_t owner, uint32_t mesh_id, const struct rt_mesh *mesh)
{
	struct rt_skinned *object;
	uint32_t index, slot;

	for (index = 0; index < rt.skinned_count; index++)
	{
		if (rt.skinned[index]->owner == owner)
		{
			if (rt.skinned[index]->mesh == mesh_id)
				return rt.skinned[index];
			/* (another mesh's: an object of the owner's index made again) */
			rt_skinned_release(index);
			break;
		}
	}
	if (rt.skinned_count >= RT_SKINNED)
		return NULL;
	object = calloc(1, sizeof(*object));
	if (!object)
		return NULL;
	object->owner = owner;
	object->mesh = mesh_id;
	/* (its output's slot: a freed one of its size, else the next) */
	for (slot = 0; slot < rt.free_out_count && rt.free_outs[slot].count != mesh->vertex_count; slot++)
		;
	if (slot < rt.free_out_count)
	{
		object->out_at = rt.free_outs[slot].at;
		rt.free_outs[slot] = rt.free_outs[--rt.free_out_count];
	}
	else if ((uint64_t)rt.outs_used + (uint64_t)mesh->vertex_count * 3u <= rt.outs.size / 4u)
	{
		object->out_at = rt.outs_used;
		rt.outs_used += mesh->vertex_count * 3u;
	}
	else
	{
		/* (the output made larger at the next frame's skinning: this one
		traced from then) */
		if (rt.outs_used + mesh->vertex_count * 3u > rt.outs_wanted)
			rt.outs_wanted = rt.outs_used + mesh->vertex_count * 3u;
		free(object);
		return NULL;
	}
	object->vertex_count = mesh->vertex_count;
	rt.skinned[rt.skinned_count++] = object;
	return object;
}

/* a skinned object's structure's geometries and build (its own buffer made
the first time: building, else refitting) */
static void rt_skinned_geometry(const struct rt_mesh *mesh, const struct rt_skinned *object,
	VkAccelerationStructureGeometryKHR geometries[2], VkAccelerationStructureBuildRangeInfoKHR ranges[2],
	VkAccelerationStructureBuildGeometryInfoKHR *info, uint32_t primitives[2])
{
	uint32_t index, count = mesh->opaque_count < mesh->count ? 2 : 1;

	primitives[0] = mesh->opaque_count;
	primitives[1] = mesh->count - mesh->opaque_count;
	memset(geometries, 0, sizeof(*geometries) * 2);
	memset(ranges, 0, sizeof(*ranges) * 2);
	for (index = 0; index < count; index++)
	{
		geometries[index].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
		geometries[index].geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
		geometries[index].geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
		geometries[index].geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
		geometries[index].geometry.triangles.vertexData.deviceAddress = rt.outs.address + (VkDeviceSize)object->out_at * 4u;
		geometries[index].geometry.triangles.vertexStride = 12;
		geometries[index].geometry.triangles.maxVertex = mesh->vertex_count - 1;
		geometries[index].geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		geometries[index].geometry.triangles.indexData.deviceAddress = mesh->corners.address;
		geometries[index].flags = VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR |
			(index == 0 ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0);
		ranges[index].primitiveCount = primitives[index];
		ranges[index].primitiveOffset = index ? primitives[0] * 12u : 0;
	}
	memset(info, 0, sizeof(*info));
	info->sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	info->type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	info->flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
		VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	info->mode = object->built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR :
		VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	info->srcAccelerationStructure = object->built ? object->structure : VK_NULL_HANDLE;
	info->dstAccelerationStructure = object->structure;
	info->geometryCount = count;
	info->pGeometries = geometries;
}

/* (the GPU time of the last frame's skinning, once written: averaged and
logged every RT_SKIN_LOGGED frames) */
static void rt_skin_time_read(void)
{
	uint64_t stamps[2];

	if (!rt.skin_timed || xgpu_vulkan_completed() < rt.skin_timed_serial ||
		rt.query_results(rt.device, rt.skin_times, 0, 2, sizeof(stamps), stamps, sizeof(stamps[0]), VK_QUERY_RESULT_64_BIT) !=
			VK_SUCCESS)
		return;
	rt.skin_timed = 0;
	rt.skin_ms += (double)(stamps[1] - stamps[0]) * (double)rt.timestamp_period * 1.0e-6;
	if (++rt.skin_samples >= RT_SKIN_LOGGED / 4u)
	{
		host_logf(HOST_LOG_INFO, "vulkan: the ray-traced scene's skinned objects: %.1f a frame, %.1f skinned and refit, "
			"%.3f ms of GPU time a frame they are (%u kept)", (double)rt.skin_objects / (double)rt.skin_counted,
			(double)rt.skin_refits / (double)rt.skin_counted, rt.skin_ms / (double)rt.skin_samples, rt.skinned_count);
		rt.skin_ms = 0.0;
		rt.skin_samples = rt.skin_objects = rt.skin_refits = rt.skin_counted = 0;
	}
}

/* the frame's posed skinned objects skinned (those whose pose changed) and
their structures built or refit, in the early commands, before the scene
that names them; those idle too long let go of */
static void rt_skin(VkCommandBuffer commands)
{
	VkAccelerationStructureBuildGeometryInfoKHR infos[XGPU_WORLD_RT_POSES];
	VkAccelerationStructureGeometryKHR geometries[XGPU_WORLD_RT_POSES][2];
	VkAccelerationStructureBuildRangeInfoKHR ranges[XGPU_WORLD_RT_POSES][2];
	const VkAccelerationStructureBuildRangeInfoKHR *range_pointers[XGPU_WORLD_RT_POSES];
	VkDeviceSize scratch_offsets[XGPU_WORLD_RT_POSES], scratch_bytes = 0;
	uint32_t pose_index, jobs = 0, most = 0, matrices_at = XGPU_WORLD_RT_POSES * 4u, frame_slot, index, posed = 0;
	uint32_t *words;
	uint64_t serial = xgpu_vulkan_serial(), done = xgpu_vulkan_completed();
	VkMemoryBarrier barrier;
	int timed = 0;

	rt.skin_frame++;
	rt_skin_time_read();
	memset(rt.pose_addresses, 0, sizeof(rt.pose_addresses));
	rt.posed_count = rt.pose_count;
	/* (the idle ones let go of) */
	for (index = 0; index < rt.skinned_count;)
	{
		if (rt.skinned[index]->posed_frame + RT_SKINNED_IDLE < rt.skin_frame)
			rt_skinned_release(index);
		else
			index++;
	}
	if (!rt.pose_count || !rt_skin_pipeline_make())
		return;
	/* (the output made, or made larger for the objects that found no room:
	nothing of the old is copied, as an object's positions are read only by
	the refit that follows its skinning in the same frame; the old let go of
	once the GPU is done with it) */
	if (!rt.outs.buffer || (VkDeviceSize)rt.outs_wanted * 4u > rt.outs.size)
	{
		uint32_t words = rt_skin_words(rt.outs_wanted, 0, RT_SKIN_OUTS);
		struct rt_buffer grown;

		if (words && rt_buffer_make((VkDeviceSize)words * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, 0, &grown))
		{
			if (rt.outs.buffer)
				rt_garbage_add(&rt.outs, VK_NULL_HANDLE);
			rt.outs = grown;
		}
		rt.outs_wanted = 0;
		if (!rt.outs.buffer)
			return;
	}
	/* (a frame's jobs and matrices no work not yet done reads; the oldest
	waited for where none is) */
	for (frame_slot = 0; frame_slot < RT_SKIN_FRAMES; frame_slot++)
	{
		if (rt.skin_frame_used[frame_slot] < serial && rt.skin_frame_used[frame_slot] <= done)
			break;
	}
	if (frame_slot == RT_SKIN_FRAMES)
	{
		frame_slot = 0;
		for (index = 1; index < RT_SKIN_FRAMES; index++)
		{
			if (rt.skin_frame_used[index] < rt.skin_frame_used[frame_slot])
				frame_slot = index;
		}
		if (rt.skin_frame_used[frame_slot] >= serial)
			return;
		xgpu_vulkan_wait(rt.skin_frame_used[frame_slot]);
	}
	if (!rt.skin_frames[frame_slot].buffer && !rt_buffer_make((VkDeviceSize)RT_SKIN_FRAME_WORDS * 4u,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1, &rt.skin_frames[frame_slot]))
		return;
	words = rt.skin_frames[frame_slot].mapped;
	for (pose_index = 0; pose_index < rt.pose_count; pose_index++)
	{
		const struct xgpu_world_rt_pose *pose = &rt.poses[pose_index];
		struct rt_mesh *mesh = pose->mesh < XGPU_STREAM_WORLD_AO_MESHES ? rt.meshes[pose->mesh] : NULL;
		struct rt_skinned *object;
		uint32_t node, component, primitives[2];
		const float *values = &pose->transforms[0][0][0];
		VkAccelerationStructureBuildSizesInfoKHR sizes;

		if (!mesh || !mesh->skinned || !pose->node_count || pose->node_count > XGPU_WORLD_RT_POSE_NODES ||
			pose->owner >= XGPU_WORLD_AO_INSTANCE_OWNERS)
			continue;
		for (component = 0; component < pose->node_count * 12u; component++)
		{
			if (!isfinite(values[component]) || fabsf(values[component]) > 1.0e6f)
				break;
		}
		if (component < pose->node_count * 12u)
			continue;
		object = rt_skinned_of(pose->key, pose->mesh, mesh);
		if (!object)
			continue;
		/* (one pose an object a frame: a second of the same key as the
		first, never built twice in one call) */
		if (object->posed_frame == rt.skin_frame)
		{
			rt.pose_addresses[pose_index] = object->built ? object->address : 0;
			continue;
		}
		object->posed_frame = rt.skin_frame;
		rt.pose_addresses[pose_index] = object->built ? object->address : 0;
		posed++;
		/* (in the pose it was last skinned in: as it is) */
		if (object->built && object->node_count == pose->node_count &&
			!memcmp(object->last, pose->transforms, pose->node_count * 48u))
			continue;
		if (matrices_at + XGPU_WORLD_RT_POSE_NODES * 12u > RT_SKIN_FRAME_WORDS)
			break;
		/* its structure, the first time */
		rt_skinned_geometry(mesh, object, geometries[jobs], ranges[jobs], &infos[jobs], primitives);
		memset(&sizes, 0, sizeof(sizes));
		sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
		rt.sizes(rt.device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &infos[jobs], primitives, &sizes);
		if (!object->structure)
		{
			VkAccelerationStructureCreateInfoKHR create;
			VkAccelerationStructureDeviceAddressInfoKHR address;

			if (!rt_buffer_make(sizes.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, 0,
				&object->own))
				continue;
			memset(&create, 0, sizeof(create));
			create.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
			create.buffer = object->own.buffer;
			create.size = sizes.accelerationStructureSize;
			create.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
			if (rt.create(rt.device, &create, NULL, &object->structure) != VK_SUCCESS)
			{
				object->structure = VK_NULL_HANDLE;
				rt_buffer_free(&object->own);
				continue;
			}
			memset(&address, 0, sizeof(address));
			address.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
			address.accelerationStructure = object->structure;
			object->address = rt.structure_address(rt.device, &address);
			object->bytes = sizes.accelerationStructureSize;
			rt_live_bytes(object->bytes, 0);
			infos[jobs].dstAccelerationStructure = object->structure;
		}
		scratch_offsets[jobs] = scratch_bytes;
		scratch_bytes += ((object->built ? sizes.updateScratchSize : sizes.buildScratchSize) + rt.scratch_alignment - 1) &
			~(VkDeviceSize)(rt.scratch_alignment - 1);
		range_pointers[jobs] = ranges[jobs];
		/* its job and matrices (every node's to the most a pose has: unused
		ones never read past a vertex's nodes, which were checked) */
		words[jobs * 4u] = mesh->rest_at;
		words[jobs * 4u + 1u] = mesh->vertex_count;
		words[jobs * 4u + 2u] = matrices_at;
		words[jobs * 4u + 3u] = object->out_at;
		memset(&words[matrices_at], 0, XGPU_WORLD_RT_POSE_NODES * 48u);
		memcpy(&words[matrices_at], pose->transforms, pose->node_count * 48u);
		for (node = pose->node_count; node < XGPU_WORLD_RT_POSE_NODES; node++)
			memcpy(&words[matrices_at + node * 12u], pose->transforms[0], 48u);
		matrices_at += XGPU_WORLD_RT_POSE_NODES * 12u;
		memcpy(object->last, pose->transforms, pose->node_count * 48u);
		object->node_count = pose->node_count;
		if (mesh->vertex_count > most)
			most = mesh->vertex_count;
		/* (built now, refit from the next) */
		infos[jobs].mode = object->built ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR :
			VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		object->built = 1;
		rt.pose_addresses[pose_index] = object->address;
		jobs++;
	}
	rt.skin_objects += posed;
	rt.skin_refits += jobs;
	rt.skin_counted++;
	rt.pose_count = 0;
	if (!jobs)
		return;
	/* (none of them traced where its build cannot be recorded) */
	/* (the refits' scratch, made larger as it must be; its last use ordered
	before this by the barrier below, which takes in all earlier work) */
	if (scratch_bytes + rt.scratch_alignment > rt.skin_scratch.size)
	{
		if (rt.skin_scratch.buffer)
			rt_garbage_add(&rt.skin_scratch, VK_NULL_HANDLE);
		if (!rt_buffer_make((scratch_bytes + rt.scratch_alignment) * 2u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 0,
			&rt.skin_scratch))
		{
			/* (the objects built for the first time now have no structure
			to trace; the rest keep their last) */
			memset(&rt.skin_scratch, 0, sizeof(rt.skin_scratch));
			memset(rt.pose_addresses, 0, sizeof(rt.pose_addresses));
			return;
		}
	}
	for (index = 0; index < jobs; index++)
	{
		infos[index].scratchData.deviceAddress = ((rt.skin_scratch.address + rt.scratch_alignment - 1) &
			~(VkDeviceAddress)(rt.scratch_alignment - 1)) + scratch_offsets[index];
	}
	if (rt.skin_times && !rt.skin_timed)
	{
		rt.reset_queries(commands, rt.skin_times, 0, 2);
		rt.timestamp(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, rt.skin_times, 0);
		timed = 1;
	}
	/* the skinning: its set (this frame's jobs) written, a group of
	vertices of each job */
	{
		VkDescriptorBufferInfo buffers[3];
		VkWriteDescriptorSet write;

		memset(buffers, 0, sizeof(buffers));
		buffers[0].buffer = rt.rests.buffer;
		buffers[1].buffer = rt.skin_frames[frame_slot].buffer;
		buffers[2].buffer = rt.outs.buffer;
		for (index = 0; index < 3; index++)
			buffers[index].range = VK_WHOLE_SIZE;
		memset(&write, 0, sizeof(write));
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.dstSet = rt.skin_sets[frame_slot];
		write.dstBinding = 0;
		write.descriptorCount = 3;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		write.pBufferInfo = buffers;
		rt.update_sets(rt.device, 1, &write, 0, NULL);
	}
	/* (the output and scratch no earlier build reads or writes still) */
	memset(&barrier, 0, sizeof(barrier));
	barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR |
		VK_ACCESS_SHADER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	rt.barrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL,
		0, NULL);
	rt.bind_pipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.skin_pipeline);
	rt.bind_sets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, rt.skin_pipeline_layout, 0, 1, &rt.skin_sets[frame_slot], 0,
		NULL);
	rt.dispatch(commands, (most + XGPU_VULKAN_RT_SKIN_GROUP - 1) / XGPU_VULKAN_RT_SKIN_GROUP, jobs, 1);
	rt.skin_frame_used[frame_slot] = serial;
	/* (skinned before the builds read the positions) */
	barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	rt.barrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1,
		&barrier, 0, NULL, 0, NULL);
	rt.build(commands, jobs, infos, range_pointers);
	/* (built before the scene that names them) */
	barrier.srcAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	barrier.dstAccessMask = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	rt.barrier(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR,
		VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, 0, 1, &barrier, 0, NULL, 0, NULL);
	if (timed)
	{
		rt.timestamp(commands, VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, rt.skin_times, 1);
		rt.skin_timed = 1;
		rt.skin_timed_serial = serial;
	}
}

/* a posed instance's skinned object's structure's address (0: none):
its pose's, this frame */
static VkDeviceAddress rt_skinned_address(const struct xgpu_world_ao_instance *given)
{
	return given->pad && given->pad <= rt.posed_count ? rt.pose_addresses[given->pad - 1] : 0;
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
	VkDeviceAddress address;
	int row, column;

	if (!mesh || given->owner >= XGPU_WORLD_AO_INSTANCE_OWNERS ||
		(given->kind != XGPU_WORLD_AO_INSTANCE_STRUCTURE && given->kind != XGPU_WORLD_AO_INSTANCE_OBJECT &&
			given->kind != XGPU_WORLD_AO_INSTANCE_FIRST_PERSON && given->kind != XGPU_WORLD_AO_INSTANCE_PLAYER &&
			given->kind != XGPU_WORLD_AO_INSTANCE_SHADOWLESS))
		return 0;
	/* (a skinned mesh's: its object's, skinned and refit this frame; a
	posed one's alone) */
	address = mesh->skinned ? (given->pad ? rt_skinned_address(given) : 0) : mesh->structure ? mesh->address : 0;
	if (!address)
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
	instance->accelerationStructureReference = address;
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
	/* (its memory, every RT_MEMORY_LOGGED frames' instances, with meshes) */
	if (rt.live_bytes && ++rt.memory_frames >= RT_MEMORY_LOGGED)
	{
		rt.memory_frames = 0;
		rt_memory_log("now");
	}
	rt_garbage_collect();
	commands = xgpu_vulkan_early_commands();
	rt_meshes_build(commands);
	rt_compaction_ask(commands);
	rt_skin(commands);
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
	LOAD(vkCreateShaderModule, create_module)
	LOAD(vkDestroyShaderModule, destroy_module)
	LOAD(vkCreatePipelineLayout, create_pipeline_layout)
	LOAD(vkDestroyPipelineLayout, destroy_pipeline_layout)
	LOAD(vkCreateComputePipelines, create_compute)
	LOAD(vkDestroyPipeline, destroy_pipeline)
	LOAD(vkCmdBindPipeline, bind_pipeline)
	LOAD(vkCmdBindDescriptorSets, bind_sets)
	LOAD(vkCmdDispatch, dispatch)
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
		if (rt.create_queries(rt.device, &create, NULL, &rt.skin_times) != VK_SUCCESS)
			rt.skin_times = VK_NULL_HANDLE;
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
	while (rt.skinned_count)
	{
		struct rt_skinned *object = rt.skinned[--rt.skinned_count];

		if (object->structure)
			rt.destroy(rt.device, object->structure, NULL);
		rt_buffer_free(&object->own);
		free(object);
	}
	for (which = 0; which < (int)RT_SKIN_FRAMES; which++)
		rt_buffer_free(&rt.skin_frames[which]);
	rt_buffer_free(&rt.rests);
	rt_buffer_free(&rt.outs);
	rt_buffer_free(&rt.skin_scratch);
	free(rt.poses);
	if (rt.skin_times)
		rt.destroy_queries(rt.device, rt.skin_times, NULL);
	if (rt.skin_pipeline)
		rt.destroy_pipeline(rt.device, rt.skin_pipeline, NULL);
	if (rt.skin_pipeline_layout)
		rt.destroy_pipeline_layout(rt.device, rt.skin_pipeline_layout, NULL);
	if (rt.skin_pool)
		rt.destroy_pool(rt.device, rt.skin_pool, NULL);
	if (rt.skin_layout)
		rt.destroy_layout(rt.device, rt.skin_layout, NULL);
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
