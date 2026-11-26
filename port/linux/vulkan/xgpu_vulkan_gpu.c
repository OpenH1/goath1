/*
XGPU_VULKAN_GPU.C

Which GPU the game draws with (xgpu_vulkan_gpu.h). Issue #1269's PC, an
RTX 5070 driving the monitor beside a Ryzen's integrated Radeon, drew with
the Radeon: AMD's switchable graphics layer had hidden the RTX from the
list (xgpu_vulkan.c's load_functions turns that layer off). The rule here
picks among what the loader lists.
*/

#include "xgpu_vulkan_gpu.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *xgpu_gpu_type_name(int type)
{
	switch (type)
	{
	case XGPU_GPU_DISCRETE:
		return "discrete";
	case XGPU_GPU_INTEGRATED:
		return "integrated";
	case XGPU_GPU_VIRTUAL:
		return "virtual";
	case XGPU_GPU_CPU:
		return "processor";
	default:
		return "other";
	}
}

/* discrete, integrated, virtual, other, a processor's */
static int type_rank(int type)
{
	switch (type)
	{
	case XGPU_GPU_DISCRETE:
		return 4;
	case XGPU_GPU_INTEGRATED:
		return 3;
	case XGPU_GPU_VIRTUAL:
		return 2;
	case XGPU_GPU_CPU:
		return 0;
	default:
		return 1;
	}
}

/* >0 if a comes before b; the first listed of equals */
static int better(const struct xgpu_gpu *a, const struct xgpu_gpu *b)
{
	if (type_rank(a->type) != type_rank(b->type))
		return type_rank(a->type) - type_rank(b->type);
	if (!a->drives_display != !b->drives_display)
		return a->drives_display ? 1 : -1;
	if (a->local_memory != b->local_memory)
		return a->local_memory > b->local_memory ? 1 : -1;
	return 0;
}

static int name_has(const char *name, const char *text)
{
	size_t length = strlen(text), index;

	if (!name)
		return 0;
	for (; *name; name++)
	{
		for (index = 0; index < length && name[index] &&
			tolower((unsigned char)name[index]) == tolower((unsigned char)text[index]); index++)
			;
		if (index == length)
			return 1;
	}
	return 0;
}

/* the best usable device whose name has text (NULL: any), else -1; the
runner-up in second */
static int best(const struct xgpu_gpu *gpus, int count, const char *text, int *second)
{
	int index, chosen = -1;

	*second = -1;
	for (index = 0; index < count; index++)
	{
		if (!gpus[index].usable || (text && !name_has(gpus[index].name, text)))
			continue;
		if (chosen < 0 || better(&gpus[index], &gpus[chosen]) > 0)
		{
			*second = chosen;
			chosen = index;
		}
		else if (*second < 0 || better(&gpus[index], &gpus[*second]) > 0)
			*second = index;
	}
	return chosen;
}

/* why chosen came before second */
static const char *reason(const struct xgpu_gpu *chosen, const struct xgpu_gpu *second, char *text, size_t size)
{
	if (!second)
		snprintf(text, size, "the only usable device");
	else if (type_rank(chosen->type) != type_rank(second->type))
		snprintf(text, size, "%s before %s", xgpu_gpu_type_name(chosen->type), xgpu_gpu_type_name(second->type));
	else if (chosen->drives_display != second->drives_display)
		snprintf(text, size, "it drives the window's display");
	else if (chosen->local_memory != second->local_memory)
		snprintf(text, size, "the most device-local memory");
	else
		snprintf(text, size, "the first listed of equals");
	return text;
}

int xgpu_gpu_choose(const struct xgpu_gpu *gpus, int count, const char *setting, char *why, size_t why_size)
{
	char because[96], fallback[160] = "";
	int chosen = -1, second = -1;

	if (!why_size)
		why = NULL;
	if (setting && *setting && strcmp(setting, "auto"))
	{
		char *end;
		long index = strtol(setting, &end, 10);

		if (*end == '\0' && end != setting)
		{
			if (index >= 0 && index < count && gpus[index].usable)
			{
				if (why)
					snprintf(why, why_size, "display.gpu %ld", index);
				return (int)index;
			}
			snprintf(fallback, sizeof(fallback), "display.gpu %ld is no usable device; ", index);
		}
		else
		{
			chosen = best(gpus, count, setting, &second);
			if (chosen >= 0)
			{
				if (why)
					snprintf(why, why_size, "display.gpu \"%s\"", setting);
				return chosen;
			}
			snprintf(fallback, sizeof(fallback), "display.gpu \"%s\" names no usable device; ", setting);
		}
	}
	chosen = best(gpus, count, NULL, &second);
	if (why)
	{
		if (chosen < 0)
			snprintf(why, why_size, "%sno usable device", fallback);
		else
			snprintf(why, why_size, "%sauto: %s", fallback,
				reason(&gpus[chosen], second >= 0 ? &gpus[second] : NULL, because, sizeof(because)));
	}
	return chosen;
}
