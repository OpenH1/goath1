/*
XGPU_VULKAN_LT_LIGHTS.C

The light-transport pass's light list, received and kept
(xgpu_vulkan_lt_lights.h). HALO_LT_LIGHTS_RECEIVED=<file> also writes each
list as it comes, for checking that a recording's replay gives the same
lists: per list the game's count of lists sent, the window and the count
(uint32 each, little-endian), then the count records as the stream has them.

The level's static light sources' triangles ride in the lists as records of
kind XGPU_LT_LIGHT_EMITTER, a share of them a frame (port/linux/game/
lt_emitters.c): taken out of the list here and gathered by their set's
generation into one set, whole once every triangle has come
(xgpu_vulkan_lt_emitters).
*/

#include "host.h"
#include "xgpu_stream.h"
#include "xgpu_vulkan_lt_lights.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct
{
	struct xgpu_stream_lt_light lights[4][XGPU_STREAM_LT_LIGHTS_MAXIMUM];
	int counts[4];
	uint32_t frames[4];
	int received, last_window;
	/* the pass on: a list received, none saying it is off since */
	int on;
	int log_read;
	FILE *log;
	/* the light sources' set: its generation (0 none), triangles and
	groups, whether solved, each triangle (had: come), how many have come,
	and its number (counted up each time a set is whole) */
	uint32_t emitter_generation;
	int emitter_count, emitter_groups, emitter_solved, emitter_had_count, emitter_version, emitter_whole;
	struct xgpu_vulkan_lt_emitter emitters[XGPU_VULKAN_LT_EMITTERS_MAXIMUM];
	/* each window's last list's object lamps (its own frame's whole) */
	struct xgpu_vulkan_lt_object_lamp lamps[4][XGPU_VULKAN_LT_OBJECT_LAMPS_MAXIMUM];
	int lamp_counts[4];
	unsigned char emitter_had[XGPU_VULKAN_LT_EMITTERS_MAXIMUM];
} lt_lights;

/* a light source's triangle into the set (a new generation starts it again) */
static void lt_emitter_take(const struct xgpu_stream_lt_light *light)
{
	uint32_t generation = light->id[1] & 0xffffffu, index = light->id[0];
	int count = (int)light->object, groups = (int)light->owner;
	struct xgpu_vulkan_lt_emitter *out;

	if (count <= 0 || count > XGPU_VULKAN_LT_EMITTERS_MAXIMUM || groups <= 0 || index >= (uint32_t)count ||
		light->gel_slot < 0 || light->gel_slot >= groups)
		return;
	if (generation != lt_lights.emitter_generation || count != lt_lights.emitter_count ||
		groups != lt_lights.emitter_groups)
	{
		lt_lights.emitter_generation = generation;
		lt_lights.emitter_count = count;
		lt_lights.emitter_groups = groups;
		lt_lights.emitter_had_count = 0;
		lt_lights.emitter_whole = 0;
		memset(lt_lights.emitter_had, 0, sizeof(lt_lights.emitter_had));
	}
	lt_lights.emitter_solved = (light->flags & XGPU_LT_LIGHT_EMITTER_SOLVED) != 0;
	out = &lt_lights.emitters[index];
	memcpy(out->corners, light->position, sizeof(float) * 3);
	memcpy(out->corners + 3, light->forward, sizeof(float) * 3);
	memcpy(out->corners + 6, light->up, sizeof(float) * 3);
	memcpy(out->radiance, light->power, sizeof(out->radiance));
	out->group = light->gel_slot;
	if (!lt_lights.emitter_had[index])
	{
		lt_lights.emitter_had[index] = 1;
		if (++lt_lights.emitter_had_count == count)
		{
			lt_lights.emitter_whole = 1;
			lt_lights.emitter_version++;
			host_logf(HOST_LOG_INFO, "vulkan: light transport: the level's light sources received (%d triangles, "
				"%d groups, %s)", count, groups, lt_lights.emitter_solved ? "solved" : "the tags'");
		}
	}
}

void xgpu_vulkan_lt_lights_receive(void *context, const struct xgpu_stream_lt_lights *record)
{
	const void *lights = record->count ? host_pointer(record->lights) : NULL;
	int window = record->window;

	(void)context;
	/* (window -1: the pass turned off, port/linux/game/lt_lights.c) */
	if (window == -1)
	{
		if (lt_lights.on)
			host_logf(HOST_LOG_INFO, "vulkan: the light-transport pass turned off");
		lt_lights.on = 0;
		return;
	}
	if (!lt_lights.on)
		host_logf(HOST_LOG_INFO, "vulkan: the light-transport pass turned on (the game's light lists)");
	lt_lights.on = 1;
	if (window < 0 || window > 3 || record->count > XGPU_STREAM_LT_LIGHTS_MAXIMUM || (record->count && !lights))
		return;
	{
		/* (the lights kept; the light sources' triangles, after them, taken
		into their set) */
		const struct xgpu_stream_lt_light *in = lights;
		uint32_t index;
		int kept = 0;

		int lamps = 0;

		for (index = 0; in && index < record->count; index++)
		{
			if (in[index].kind == XGPU_LT_LIGHT_EMITTER)
				lt_emitter_take(&in[index]);
			else if (in[index].kind == XGPU_LT_LIGHT_OBJECT_EMITTER)
			{
				struct xgpu_vulkan_lt_object_lamp *out;

				if (lamps >= XGPU_VULKAN_LT_OBJECT_LAMPS_MAXIMUM)
					continue;
				out = &lt_lights.lamps[window][lamps++];
				memcpy(out->corners, in[index].position, sizeof(float) * 3);
				memcpy(out->corners + 3, in[index].forward, sizeof(float) * 3);
				memcpy(out->corners + 6, in[index].up, sizeof(float) * 3);
				memcpy(out->radiance, in[index].power, sizeof(out->radiance));
				out->owner = in[index].owner;
				out->two_sided = (in[index].flags & XGPU_LT_LAMP_TWO_SIDED) != 0;
				out->additive = (in[index].flags & XGPU_LT_LAMP_ADDITIVE) != 0;
			}
			else
				lt_lights.lights[window][kept++] = in[index];
		}
		lt_lights.counts[window] = kept;
		lt_lights.lamp_counts[window] = lamps;
	}
	lt_lights.frames[window] = record->frame;
	lt_lights.last_window = window;
	if (!lt_lights.received++)
		host_logf(HOST_LOG_INFO, "vulkan: light list received (%u lights, window %d)", record->count, window);
	if (!lt_lights.log_read)
	{
		const char *path = getenv("HALO_LT_LIGHTS_RECEIVED");

		lt_lights.log_read = 1;
		if (path && *path)
		{
			lt_lights.log = fopen(path, "wb");
			if (!lt_lights.log)
				host_logf(HOST_LOG_WARN, "vulkan: %s could not be written", path);
		}
	}
	if (lt_lights.log)
	{
		uint32_t head[3];

		head[0] = record->frame;
		head[1] = (uint32_t)window;
		head[2] = (uint32_t)lt_lights.counts[window];
		fwrite(head, sizeof(head), 1, lt_lights.log);
		if (lt_lights.counts[window])
			fwrite(lt_lights.lights[window], sizeof(struct xgpu_stream_lt_light), (size_t)lt_lights.counts[window],
				lt_lights.log);
		fflush(lt_lights.log);
	}
}

const struct xgpu_stream_lt_light *xgpu_vulkan_lt_lights(int window, int *count, uint32_t *frame)
{
	if (window < 0 || window > 3 || !lt_lights.counts[window])
	{
		*count = 0;
		if (frame)
			*frame = window >= 0 && window <= 3 ? lt_lights.frames[window] : 0;
		return NULL;
	}
	*count = lt_lights.counts[window];
	if (frame)
		*frame = lt_lights.frames[window];
	return lt_lights.lights[window];
}

int xgpu_vulkan_lt_lights_window(void)
{
	return lt_lights.received ? lt_lights.last_window : -1;
}

int xgpu_vulkan_lt_lights_on(void)
{
	return lt_lights.on;
}

const struct xgpu_vulkan_lt_emitter *xgpu_vulkan_lt_emitters(int *count, int *groups, int *solved, int *version)
{
	if (!lt_lights.on || !lt_lights.emitter_whole)
	{
		*count = *groups = *solved = 0;
		*version = 0;
		return NULL;
	}
	*count = lt_lights.emitter_count;
	*groups = lt_lights.emitter_groups;
	*solved = lt_lights.emitter_solved;
	*version = lt_lights.emitter_version;
	return lt_lights.emitters;
}

const struct xgpu_vulkan_lt_object_lamp *xgpu_vulkan_lt_object_lamps(int window, int *count)
{
	if (!lt_lights.on || window < 0 || window > 3)
	{
		*count = 0;
		return NULL;
	}
	*count = lt_lights.lamp_counts[window];
	return lt_lights.lamps[window];
}
