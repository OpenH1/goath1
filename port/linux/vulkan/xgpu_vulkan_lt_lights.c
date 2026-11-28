/*
XGPU_VULKAN_LT_LIGHTS.C

The light-transport pass's light list, received and kept
(xgpu_vulkan_lt_lights.h). HALO_LT_LIGHTS_RECEIVED=<file> also writes each
list as it comes, for checking that a recording's replay gives the same
lists: per list the game's count of lists sent, the window and the count
(uint32 each, little-endian), then the count records as the stream has them.
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
	int log_read;
	FILE *log;
} lt_lights;

void xgpu_vulkan_lt_lights_receive(void *context, const struct xgpu_stream_lt_lights *record)
{
	const void *lights = record->count ? host_pointer(record->lights) : NULL;
	int window = record->window;

	(void)context;
	if (window < 0 || window > 3 || record->count > XGPU_STREAM_LT_LIGHTS_MAXIMUM || (record->count && !lights))
		return;
	memcpy(lt_lights.lights[window], lights ? lights : (const void *)lt_lights.lights[window],
		lights ? record->count * sizeof(struct xgpu_stream_lt_light) : 0);
	lt_lights.counts[window] = (int)record->count;
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
		head[2] = record->count;
		fwrite(head, sizeof(head), 1, lt_lights.log);
		if (record->count)
			fwrite(lt_lights.lights[window], sizeof(struct xgpu_stream_lt_light), record->count, lt_lights.log);
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
