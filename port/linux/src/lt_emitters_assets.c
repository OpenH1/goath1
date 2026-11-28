/*
LT_EMITTERS_ASSETS.C

The loaded BSP's static light sources unpacked from openh1.assets
(lt_emitters_assets.h).
*/

#include "lt_emitters_assets.h"
#include "embedded_assets.h"

#include <stddef.h>
#include <zlib.h>

void platform_log(const char *format, ...);

/* the tag paths alike, in ASCII and whatever their case */
static int lt_emitters_same_path(char const *a, char const *b)
{
	for (;; a++, b++)
	{
		char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
		char y = *b >= 'A' && *b <= 'Z' ? (char)(*b - 'A' + 'a') : *b;

		if (x != y)
			return 0;
		if (!x)
			return 1;
	}
}

static struct lt_emitters_asset const *lt_emitters_asset_find(char const *scenario, int bsp_index)
{
	unsigned int index;

	for (index = 0; scenario && index < lt_emitters_asset_count; index++)
	{
		struct lt_emitters_asset const *asset = &lt_emitters_assets[index];

		if (asset->bsp_index == bsp_index && lt_emitters_same_path(asset->scenario, scenario))
			return asset;
	}
	return NULL;
}

unsigned int lt_emitters_asset_size(char const *scenario, int bsp_index)
{
	struct lt_emitters_asset const *asset = lt_emitters_asset_find(scenario, bsp_index);

	return asset ? asset->unpacked_size : 0;
}

int lt_emitters_asset_read(char const *scenario, int bsp_index, void *out, unsigned int size)
{
	struct lt_emitters_asset const *asset = lt_emitters_asset_find(scenario, bsp_index);
	void const *packed = asset ? embedded_assets_data(asset->offset, asset->size) : NULL;
	uLongf length = size;

	if (!asset)
		return 0;
	if (!packed || !out || size != asset->unpacked_size ||
		uncompress(out, &length, packed, asset->size) != Z_OK || length != asset->unpacked_size)
	{
		platform_log("lt emitters: %s bsp %d's light sources cannot be read from %s", scenario, bsp_index,
			EMBEDDED_ASSETS_FILE);
		return 0;
	}
	return 1;
}
