/*
LT_EMITTERS_ASSETS.C

The loaded BSP's static light sources unpacked from openh1.assets
(lt_emitters_assets.h).
*/

#include "lt_emitters_assets.h"
#include "embedded_assets.h"

#include <stdlib.h>
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

void *lt_emitters_asset_read(char const *scenario, int bsp_index, unsigned int *size)
{
	unsigned int index;

	*size = 0;
	for (index = 0; scenario && index < lt_emitters_asset_count; index++)
	{
		struct lt_emitters_asset const *asset = &lt_emitters_assets[index];
		void const *packed;
		unsigned char *unpacked;
		uLongf length = asset->unpacked_size;

		if (asset->bsp_index != bsp_index || !lt_emitters_same_path(asset->scenario, scenario))
			continue;
		packed = embedded_assets_data(asset->offset, asset->size);
		unpacked = packed && asset->unpacked_size ? malloc(asset->unpacked_size) : NULL;
		if (!unpacked || uncompress(unpacked, &length, packed, asset->size) != Z_OK || length != asset->unpacked_size)
		{
			platform_log("lt emitters: %s bsp %d's light sources cannot be read from %s", scenario, bsp_index,
				EMBEDDED_ASSETS_FILE);
			free(unpacked);
			return NULL;
		}
		*size = asset->unpacked_size;
		return unpacked;
	}
	return NULL;
}
