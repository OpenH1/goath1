/*
MOD_MATERIALS.C

The material maps of mods (docs/mods.md "Material maps"): beside a map's
bitmap tag, a mod's tags folder may have two PNGs in the bitmap's layout,

  tags/<bitmap>.normal.png  a tangent-space normal map (RGB the normal's x,
                            y and z, green toward the picture's top);
  tags/<bitmap>.orm.png     occlusion in red, roughness in green, metalness
                            in blue (glTF's packing), and where it has an
                            alpha channel, how much the part glows in alpha
                            (the MCC import's _EMISSIVE: with the ray-traced
                            lighting, the base map's colour at full light);
                            an RGB file glows nowhere.

A model's part drawn with a shader_model whose base map is that bitmap is
then drawn with them by the enhanced graphics (the Vulkan renderer's
per-pixel model lighting with the enhanced dynamic lights: xgpu.h's
XGPU_TEXTURE_MODES_MATERIAL). They are read and decoded as the mods are
played (as the map loads, mods_reload, the Mods menu's OK), once, into
images (xgpu_images.h) the renderer makes its levels of; everything else
draws as it did. They are drawn, not simulated: nothing a network game's
machines agree on reads them.

Each is 8-bit RGB or RGBA, not interlaced, each side a power of two from 4
to 2048 (MAXIMUM_MATERIAL_MAP_SIDE). A file that is not is left out, and
debug.txt says why.
*/

/* ---------- headers */

#include "cseries.h"
#include "errors.h"
#include "bitmaps/bitmaps.h"
#include "bitmaps/bitmap_group.h"
#include "cache/cache_files.h"
#include "tag_files/tag_groups.h"
#include "../src/port_mods.h" /* port: port/linux/src/port_mods.c */
#include "mods.h"
#include "mod_materials.h"

#include <stdlib.h>
#include <string.h>

/* the platform layer's (port/linux/src/hud_hires.c, xgpu_images.c,
d3d8_device.c) */
unsigned char *hud_hires_png_pixels(const void *png, unsigned long size, unsigned long *width, unsigned long *height);
unsigned int hud_hires_pixels_texture(unsigned char *pixels, unsigned long width, unsigned long height,
	unsigned long *levels);
void xgpu_image_delete(unsigned int image);
void halo_model_material_maps(unsigned int normal_map, unsigned int orm_map);
char *tag_get_name(long tag_index);

/* ---------- constants */

enum
{
	_material_map_normal,
	_material_map_orm,
	NUMBER_OF_MATERIAL_MAPS
};

#define MAXIMUM_MATERIAL_MAP_SIDE 2048
#define MINIMUM_MATERIAL_MAP_SIDE 4
/* the bitmaps that can have them (a power of two: the table's mask) */
#define MAXIMUM_MATERIALS 1024
/* the largest PNG read (a 2048 x 2048 RGBA's worst is 16 MB) */
#define MAXIMUM_MATERIAL_FILE_SIZE (24UL * 1024 * 1024)

static char const *const material_extensions[NUMBER_OF_MATERIAL_MAPS] = { ".normal.png", ".orm.png" };

/* ---------- structures */

struct mod_material
{
	/* the bitmap's tag (NONE: the slot is free) */
	long tag_index;
	unsigned int images[NUMBER_OF_MATERIAL_MAPS];
	unsigned long bytes[NUMBER_OF_MATERIAL_MAPS];
};

/* ---------- globals */

static struct
{
	struct mod_material materials[MAXIMUM_MATERIALS];
	long count;
	/* the last bitmap drawn's (most draws are of the one before's) */
	long last_tag_index;
	struct mod_material const *last;
} mod_materials_globals;

/* ---------- private code */

static unsigned long material_slot(
	long tag_index)
{
	unsigned long hash = (unsigned long)tag_index & 0xffffUL;

	return (hash * 2654435761UL >> 7) & (MAXIMUM_MATERIALS - 1);
}

static struct mod_material *material_find(
	long tag_index)
{
	unsigned long slot = material_slot(tag_index);
	long looked;

	for (looked = 0; looked < MAXIMUM_MATERIALS; looked++)
	{
		struct mod_material *material = &mod_materials_globals.materials[slot];

		if (material->tag_index == tag_index)
			return material;
		if (material->tag_index == NONE)
			return NULL;
		slot = (slot + 1) & (MAXIMUM_MATERIALS - 1);
	}

	return NULL;
}

static struct mod_material *material_new(
	long tag_index)
{
	unsigned long slot = material_slot(tag_index);

	if (mod_materials_globals.count >= MAXIMUM_MATERIALS / 2)
		return NULL;
	while (mod_materials_globals.materials[slot].tag_index != NONE)
		slot = (slot + 1) & (MAXIMUM_MATERIALS - 1);
	mod_materials_globals.count++;
	mod_materials_globals.materials[slot].tag_index = tag_index;

	return &mod_materials_globals.materials[slot];
}

static boolean side_valid(
	unsigned long side)
{
	return side >= MINIMUM_MATERIAL_MAP_SIDE && side <= MAXIMUM_MATERIAL_MAP_SIDE && !(side & (side - 1));
}

/* a mod's map of the bitmap's (its file, or with file NONE the file at
path: the MCC import's), as an image: 0 with *reason saying why not */
static unsigned int material_map_read(
	long file,
	char const *path,
	unsigned long *bytes,
	char const **reason,
	boolean alpha_kept)
{
	boolean has_alpha;
	unsigned long size, stamp[2], width = 0, height = 0, levels;
	unsigned char *data, *pixels;
	unsigned int image;

	*reason = "it cannot be read";
	if (file != NONE ? !port_mods_file_stamp(file, &size, stamp) : !port_mods_path_stamp(path, &size, stamp))
		return 0;
	if (size > MAXIMUM_MATERIAL_FILE_SIZE)
	{
		*reason = "it is larger than 24 MB";
		return 0;
	}
	data = (unsigned char *)malloc(size ? size : 1);
	if (!data)
		return 0;
	if (file != NONE ? !port_mods_file_read(file, 0, data, size) : !port_mods_path_read(path, 0, data, size))
	{
		free(data);
		return 0;
	}
	/* (the PNG's colour type, IHDR's byte 25: 6 RGBA, 4 grey and alpha) */
	has_alpha = size > 25 && (data[25] == 6 || data[25] == 4);
	pixels = hud_hires_png_pixels(data, size, &width, &height);
	free(data);
	if (!pixels)
	{
		*reason = "it is not an 8-bit RGB or RGBA PNG, not interlaced";
		return 0;
	}
	if (!side_valid(width) || !side_valid(height))
	{
		free(pixels);
		*reason = "its sides are not powers of two from 4 to 2048";
		return 0;
	}
	/* (a map whose alpha is not its own (the ORM's emissive) where the file
	has none: 0, not the decoder's opaque 255) */
	if (!alpha_kept && !has_alpha)
	{
		unsigned long pixel;

		for (pixel = 0; pixel < width * height; pixel++)
			pixels[pixel * 4 + 3] = 0;
	}
	*bytes = width * height * 4;
	if (!mods_memory_take(*bytes))
	{
		free(pixels);
		*reason = "the mods' memory is full";
		return 0;
	}
	/* (the image owns the pixels from here, made or not) */
	image = hud_hires_pixels_texture(pixels, width, height, &levels);
	if (!image)
	{
		mods_memory_give(*bytes);
		*reason = "there is no room for another image";
	}

	return image;
}

static void materials_let_go(
	void)
{
	long index;

	for (index = 0; index < MAXIMUM_MATERIALS; index++)
	{
		struct mod_material *material = &mod_materials_globals.materials[index];
		long map;

		if (material->tag_index != NONE)
		{
			for (map = 0; map < NUMBER_OF_MATERIAL_MAPS; map++)
			{
				if (material->images[map])
				{
					xgpu_image_delete(material->images[map]);
					mods_memory_give(material->bytes[map]);
				}
			}
		}
		memset(material, 0, sizeof(*material));
		material->tag_index = NONE;
	}
	mod_materials_globals.count = 0;
	mod_materials_globals.last_tag_index = NONE;
	mod_materials_globals.last = NULL;
	halo_model_material_maps(0, 0);

	return;
}

/* ---------- public code */

long mod_materials_update(
	boolean remove_all,
	boolean at_load)
{
	struct tag_iterator iterator;
	long tag_index;
	long map_count = 0;
	long refused_count = 0;

	(void)at_load;
	/* (read again whole: the renderer is done with the images it has, they
	are freed after the frames that draw them) */
	materials_let_go();
	if (!mods_map_loaded())
		return 0;
	tag_iterator_new(&iterator, BITMAP_GROUP_TAG);
	while ((tag_index = tag_iterator_next(&iterator)) != NONE)
	{
		long files[NUMBER_OF_MATERIAL_MAPS];
		/* the MCC import's maps' paths ("" for none): only where no mod has
		a map of the bitmap's, nor the bitmap itself (a mod's own picture
		need not have the stock one's layout) */
		char imported[NUMBER_OF_MATERIAL_MAPS][1024];
		struct mod_material *material;
		long map;
		boolean any = FALSE;

		for (map = 0; map < NUMBER_OF_MATERIAL_MAPS; map++)
		{
			files[map] = remove_all ? NONE : mods_tag_file_find(tag_index, material_extensions[map]);
			imported[map][0] = 0;
		}
		if (files[_material_map_normal] == NONE && files[_material_map_orm] == NONE && mods_builtins_played() &&
			(remove_all || mods_tag_file_find(tag_index, ".bitmap") == NONE))
		{
			for (map = 0; map < NUMBER_OF_MATERIAL_MAPS; map++)
				if (!mcc_import_tag_file(tag_get_name(tag_index), material_extensions[map], imported[map],
					sizeof(imported[map])))
				{
					imported[map][0] = 0;
				}
		}
		for (map = 0; map < NUMBER_OF_MATERIAL_MAPS; map++)
			any |= files[map] != NONE || imported[map][0];
		if (!any)
			continue;
		material = material_new(tag_index);
		if (!material)
		{
			error(_error_silent, "mods: material maps of %s left out: more than %d bitmaps have them",
				tag_get_name(tag_index), MAXIMUM_MATERIALS / 2);
			refused_count++;
			continue;
		}
		for (map = 0; map < NUMBER_OF_MATERIAL_MAPS; map++)
		{
			char const *reason;

			if (files[map] == NONE && !imported[map][0])
				continue;
			material->images[map] = material_map_read(files[map], imported[map], &material->bytes[map], &reason,
				map != _material_map_orm);
			if (material->images[map])
			{
				map_count++;
			}
			else
			{
				error(_error_silent, "mods: %s left out: %s",
					files[map] != NONE ? port_mods_file_path(files[map]) : imported[map], reason);
				refused_count++;
			}
		}
	}
	if (map_count || refused_count)
		error(_error_silent, "mods: %ld material maps from their files, %ld left out", map_count, refused_count);

	return map_count;
}

unsigned long mod_materials_identity(
	void)
{
	unsigned long identity = 0;
	long index;

	/* (the same whatever the table's order: each bitmap's word mixed, then
	summed) */
	for (index = 0; index < MAXIMUM_MATERIALS; index++)
	{
		struct mod_material const *material = &mod_materials_globals.materials[index];
		unsigned long word;

		if (material->tag_index == NONE ||
			(!material->images[_material_map_normal] && !material->images[_material_map_orm]))
		{
			continue;
		}
		word = ((unsigned long)material->tag_index & 0xffffUL) << 2 |
			(material->images[_material_map_normal] ? 1UL : 0UL) | (material->images[_material_map_orm] ? 2UL : 0UL);
		word = (word ^ (word >> 16)) * 0x45d9f3bUL;
		word = (word ^ (word >> 16)) * 0x45d9f3bUL;
		identity += (word ^ (word >> 16)) & 0xffffffffUL;
	}

	return identity & 0xffffffffUL;
}

void mod_materials_tags_unloaded(
	void)
{
	materials_let_go();

	return;
}

void mod_materials_maps(
	long base_map_tag_index,
	unsigned int *normal_map,
	unsigned int *orm_map)
{
	struct mod_material const *material = mod_materials_globals.count && base_map_tag_index != NONE ?
		material_find(base_map_tag_index) : NULL;

	*normal_map = material ? material->images[_material_map_normal] : 0;
	*orm_map = material ? material->images[_material_map_orm] : 0;

	return;
}

void mod_materials_model_draw(
	long base_map_tag_index)
{
	struct mod_material const *material;

	if (!mod_materials_globals.count)
	{
		return;
	}
	if (base_map_tag_index == mod_materials_globals.last_tag_index)
	{
		material = mod_materials_globals.last;
	}
	else
	{
		material = base_map_tag_index == NONE ? NULL : material_find(base_map_tag_index);
		mod_materials_globals.last_tag_index = base_map_tag_index;
		mod_materials_globals.last = material;
	}
	if (material)
		halo_model_material_maps(material->images[_material_map_normal], material->images[_material_map_orm]);
	else
		halo_model_material_maps(0, 0);

	return;
}
