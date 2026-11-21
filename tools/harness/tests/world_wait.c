/*
WORLD_WAIT.C (test)

display.ambient_occlusion "world": a world pipeline whose shaders the map's
preparation did not make waits for them after the load (port/linux/vulkan/
xgpu_vulkan_stream.c's world_entry, world_wait, world_make_some), its
stand-in drawn meanwhile. Its draws into the window's motion
(display.temporal_aa) need the motion's variant (motion.entries'), not its
own: that variant is the one made, so the draw finds it the next frame and
the miss is logged once, not again each frame. One whose shaders cannot be
made draws its stand-in, logged no more. test_world_wait.py takes the code.
*/

#include "harness.h"

#include <stdint.h>

#define XGPU_STREAM_PIPELINES 64
#define WORLD_WAITING_MOST 1024
#define WORLD_MADE_A_FRAME 2

enum
{
	XGPU_STREAM_PIPELINE_UNKNOWN,
	XGPU_STREAM_PIPELINE_MADE,
	XGPU_STREAM_PIPELINE_FAILED,
};

typedef void *VkShaderModule;

struct xgpu_stream_pipeline_define
{
	uint32_t key;
};

#include "under_test_types.inc"

static struct
{
	struct stream_pipeline pipelines[XGPU_STREAM_PIPELINES];
	const struct xgpu_stream_pipeline_define *definitions[XGPU_STREAM_PIPELINES];
	const struct xgpu_stream_pipeline_define *definition;
	uint16_t stand_ins[XGPU_STREAM_PIPELINES];
	struct
	{
		uint16_t id;
		struct variant variant;
		uint8_t motion;
	} world_waiting[WORLD_WAITING_MOST];
	uint32_t world_waiting_count;
	int prepared, preparing;
} stream;

static struct
{
	int making;
	struct stream_pipeline *entries;
	struct xgpu_stream_pipeline_define *definitions;
} motion;

static struct stream_pipeline motion_entries[XGPU_STREAM_PIPELINES];
static struct xgpu_stream_pipeline_define motion_definitions[XGPU_STREAM_PIPELINES];

static int motion_tables(void)
{
	motion.entries = motion_entries;
	motion.definitions = motion_definitions;
	return 1;
}

static int misses, builds, motion_builds, failing;

static void shaders_missed(const char *whose, const struct xgpu_stream_pipeline_define *definition)
{
	(void)whose;
	(void)definition;
	misses++;
}

/* (its shaders made at once, the motion's where motion.making; or none can be) */
static int pipeline_shaders(struct stream_pipeline *entry, const struct xgpu_stream_pipeline_define *definition,
	const struct variant *variant)
{
	(void)definition;
	builds++;
	motion_builds += motion.making != 0;
	if (failing)
	{
		entry->state = XGPU_STREAM_PIPELINE_FAILED;
		return 0;
	}
	entry->state = XGPU_STREAM_PIPELINE_MADE;
	entry->variant = *variant;
	return 1;
}

#include "under_test.inc"

static const struct xgpu_stream_pipeline_define world = { 0x81 };

/* a frame: the world pipeline's draw (draw_input's, apart: its own or the
motion's variant made in this variant, else it waits), then the frame's
making (world_make_some) */
static void frame(uint32_t id, int motion_variant)
{
	struct stream_pipeline *entry = motion_variant ? &motion_entries[id] : &stream.pipelines[id];
	struct variant variant;

	memset(&variant, 0, sizeof(variant));
	variant.integer_attributes = 3;
	stream.definition = &world;
	if (entry->state != XGPU_STREAM_PIPELINE_MADE || memcmp(&entry->variant, &variant, sizeof(variant)) ||
		(motion_variant && memcmp(&motion_definitions[id], &world, sizeof(world))))
	{
		world_wait(id, &variant, motion_variant);
	}
	world_make_some();
}

static void played(int motion_variant)
{
	int index;

	(void)motion_tables();
	stream.prepared = 1;
	stream.definitions[5] = &world;
	stream.stand_ins[5] = 6;
	/* (its own made as the map was prepared: the motion's is what is missed) */
	if (motion_variant)
	{
		struct variant variant;

		memset(&variant, 0, sizeof(variant));
		variant.integer_attributes = 3;
		stream.pipelines[5].state = XGPU_STREAM_PIPELINE_MADE;
		stream.pipelines[5].variant = variant;
	}
	for (index = 0; index < 120; index++)
		frame(5, motion_variant);
}

int main(int argc, char **argv)
{
	const char *which = argc > 1 ? argv[1] : "";

	if (!strcmp(which, "motion"))
	{
		played(1);
		CHECK(misses == 1, "the motion's variant missed %d times in 120 frames, not once", misses);
		CHECK(builds == 1 && motion_builds == 1, "%d made (%d the motion's), not the motion's once", builds,
			motion_builds);
		CHECK(motion_entries[5].state == XGPU_STREAM_PIPELINE_MADE, "the motion's variant not made");
		return 0;
	}
	if (!strcmp(which, "own"))
	{
		played(0);
		CHECK(misses == 1, "its own missed %d times in 120 frames, not once", misses);
		CHECK(builds == 1 && motion_builds == 0, "%d made (%d the motion's), not its own once", builds,
			motion_builds);
		return 0;
	}
	if (!strcmp(which, "failed"))
	{
		failing = 1;
		played(1);
		CHECK(misses == 1, "a variant that cannot be made missed %d times in 120 frames, not once", misses);
		CHECK(builds == 1, "a variant that cannot be made tried %d times, not once", builds);
		return 0;
	}
	fprintf(stderr, "no case %s\n", which);
	return 2;
}
