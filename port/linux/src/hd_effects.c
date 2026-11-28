/*
HD_EFFECTS.C

The HD effects made from the player's own maps and kept (hd_effects.h):
which are wanted, the thread that makes them, the map read, the recipe, and
the tag file written.

A map is read with xbox_map_file.c (inflated as it is read, never held
whole), once for all the effects wanted that list it: its tags first (the
header says where they are: the end of the map), then, in a second pass,
every one of those effects' stock pages, which the tags place earlier.
Each effect's pages' CRC-32 must be its own: any other bitmap (a modded
map's) is not the stock one the recipe was made for, and the effect's next
map is tried. The maps' effects are made on up to BUILD_WORKERS threads.

The flash recipe is tools/hd_muzzle_flash.py's, step for step, on planes of
32-bit floats (the script's NumPy has 64): each page upscaled (bicubic, as
Pillow's, held within each texel's 3 x 3 neighbors' colors), its banding
smoothed (twice, Gaussian blurs as SciPy's: reflected at the edges, cut at
4 sigma), its cores sharpened (an unsharp mask held within the 5 x 5
neighborhood) and its glow softened, its alpha each sprite's box with soft
edges, each sprite scaled back to the map's light (its summed color), and
mip levels (2 x 2 boxes) down to the map's smallest. A 256 x 256 page takes
about 44 MB of planes while it is redrawn, given back after; the tag file
is the bitmap's values, sequences and sprites as the map has them, its
pages 32-bit (A8R8G8B8), at the scale (the AR's flash: two 1024 x 1024
pages, 5.3 MB each with their mip levels, 11.2 MB in all).

The splat recipe (the blood) is tools/hd_blood.py's, step for step, on
planes of 32-bit floats: each DXT1 or DXT3 page decoded, then redrawn whole
and each sprite's cell redrawn on its own over it (its edges clamped, so
no cell bleeds into the next): each plane upscaled (Catmull-Rom, held
within each texel's 3 x 3 neighbors' range), deblocked (within about a
format's step of its blur, that blur) and its edges pushed toward their
9 x 9 neighborhood's least or most (a soft shock filter), each cell's mean
color given back the map's; a decal's alpha the map's (1 bit, each texel
4 x 4), a particle's redrawn as its color is, its color under no alpha its
neighbors'. A 128 x 128 sheet takes about 15 MB of planes while it is
redrawn and becomes 512 x 512 A8R8G8B8 with mip levels down to the map's
smallest (1.3 MB).
*/

#include "hd_effects.h"
#include "platform.h"
#include "port_config.h"
#include "posix.h"
#include "xbox_map_file.h"
#include "memory/zlib/zlib.h"

#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* (the C library's: the game's <sys/stat.h> is MSVC's, as install_layout.c says) */
int mkdir(const char *path, unsigned int mode);
int halo_server_mode(void);

/* ---------- constants */

#define MAXIMUM_EFFECTS 64
#define MAXIMUM_EFFECT_MAPS 32
#define MAXIMUM_PATH 1100
#define MAXIMUM_PAGES 8
#define MAXIMUM_PAGE_SIDE 512

/* the tags' (bitmap_group.h) */
#define GROUP_BITMAP 0x6269746DUL
#define BITMAP_ROOT_SIZE 0x6C
#define BITMAP_SEQUENCE_SIZE 0x40
#define BITMAP_SPRITE_SIZE 0x20
#define BITMAP_DATA_SIZE 0x30
#define BITMAP_LINEAR_FLAG 0x10
#define BITMAP_FORMAT_A1R5G5B5 8
#define BITMAP_FORMAT_A8R8G8B8 11
#define BITMAP_FORMAT_DXT1 14
#define BITMAP_FORMAT_DXT3 15
#define BITMAP_GROUP_FORMAT_32_BIT 4
#define BITMAP_POWER_OF_TWO_FLAG 1
#define TAG_FILE_HEADER_SIZE 0x40
#define TAG_FILE_BITMAP_VERSION 7

/* the flash recipe's: one of the map's color steps (five bits a channel) */
#define FLASH_STEP (1.0f / 31.0f)
/* Gaussians are cut at 4 sigma, as SciPy's (sigma at most 14: 57 texels) */
#define MAXIMUM_RADIUS 64

/* the splat recipe's (tools/hd_blood.py's): the deblocking's steps (five
bits of color, DXT3's four of alpha) and blur (map texels); the edges'
reach (map texels each way), the contrast from which a texel is on an edge
and at which fully, and how far it is pushed; the particles' color under no
alpha, their neighbors' (map texels) */
#define SPLAT_COLOR_STEP (1.0f / 31.0f)
#define SPLAT_ALPHA_STEP (1.0f / 15.0f)
#define SPLAT_DEBLOCK_SIGMA 1.0f
#define SPLAT_EDGE_REACH 1
#define SPLAT_EDGE_LOW 0.06f
#define SPLAT_EDGE_HIGH 0.2f
#define SPLAT_EDGE_STRENGTH 0.75f
#define SPLAT_BLEED_SIGMA 2.0f
/* the most sprites' cells a page is redrawn by */
#define MAXIMUM_CELLS 64
/* the most threads a map's effects are made on (each about 15 MB of planes
while it redraws a 128 x 128 sheet) */
#define BUILD_WORKERS 4

/* ---------- globals */

static struct
{
	pthread_mutex_t lock;
	/* the effects asked for on the thread, and its progress */
	pthread_t thread;
	int thread_started;
	int running;
	float progress;
	int queued[MAXIMUM_EFFECTS];
	/* each effect's maps' paths on this machine (found on the game's thread) */
	char *map_paths[MAXIMUM_EFFECTS];
	/* each effect's cache: whether it was looked at, and is whole; whether
	it failed this run (not made again until the next) */
	int checked[MAXIMUM_EFFECTS];
	int valid[MAXIMUM_EFFECTS];
	int failed[MAXIMUM_EFFECTS];
	unsigned long made;
	char folder[1024];
} hd_effects = { PTHREAD_MUTEX_INITIALIZER };

/* ---------- private code */

static long effect_count(void)
{
	return hd_effects_embedded_count < MAXIMUM_EFFECTS ? (long)hd_effects_embedded_count : MAXIMUM_EFFECTS;
}

static unsigned long word_le(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static unsigned int half_le(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8;
}

static void put_be32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)(value >> 24);
	bytes[1] = (unsigned char)(value >> 16);
	bytes[2] = (unsigned char)(value >> 8);
	bytes[3] = (unsigned char)value;
}

static void put_be16(unsigned char *bytes, unsigned int value)
{
	bytes[0] = (unsigned char)(value >> 8);
	bytes[1] = (unsigned char)value;
}

/* `count` little-endian values of `size` bytes from `in` written big-endian to `out` */
static void swap_into(unsigned char *out, const unsigned char *in, int size, int count)
{
	int index, byte;

	for (index = 0; index < count; index++)
		for (byte = 0; byte < size; byte++)
			out[index * size + byte] = in[index * size + size - 1 - byte];
}

static const char *cache_folder(void)
{
	if (!hd_effects.folder[0])
		snprintf(hd_effects.folder, sizeof(hd_effects.folder), "%s/hd_effects", platform_data_root());
	return hd_effects.folder;
}

static void effect_path(char *path, size_t size, long effect, const char *extension)
{
	snprintf(path, size, "%s/%s.%s", cache_folder(), hd_effects_embedded[effect].name, extension);
}

/* the note a whole file made now has (its size: the bitmap file's) */
static void note_text(char *text, size_t size, long effect, unsigned long file_size)
{
	const struct hd_effect_embedded *embedded = &hd_effects_embedded[effect];

	snprintf(text, size, "openh1-hd-effects %d %08x %u %u %lu\n",
		embedded->recipe == _hd_effect_recipe_splat ? HD_EFFECTS_SPLAT_VERSION : HD_EFFECTS_RECIPE_VERSION, embedded->crc,
		embedded->scale, embedded->smallest, file_size);
}

/* whether the effect's file is whole and this build's (its note says so) */
static int effect_cached(long effect)
{
	char path[MAXIMUM_PATH], expected[160], note[160];
	struct posix_file_information information;
	size_t length;
	FILE *file;

	effect_path(path, sizeof(path), effect, "bitmap");
	if (posix_stat(path, &information) != 0 || information.size_high)
		return 0;
	note_text(expected, sizeof(expected), effect, information.size_low);
	effect_path(path, sizeof(path), effect, "note");
	file = fopen(path, "rb");
	if (!file)
		return 0;
	length = fread(note, 1, sizeof(note) - 1, file);
	fclose(file);
	note[length] = 0;
	return strcmp(note, expected) == 0;
}

/* a file written whole or not at all (a .new beside it, renamed over it:
menu_pictures.c's picture_write_file) */
static int write_file(const char *path, const void *data, unsigned long size)
{
	char temporary[MAXIMUM_PATH + 8];
	FILE *file;
	int written;

	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fwrite(data, 1, size, file) == size;
	written = fclose(file) == 0 && written;
	/* (Windows renames over nothing) */
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

/* ---------- the map read */

/* the stock bitmap read out of a map */
struct effect_source
{
	/* the map's tags (the batch's, held while its effects are made) */
	const unsigned char *tags;
	unsigned long tags_size;
	/* its definition, sequences and pages (where the tags have them) */
	const unsigned char *root;
	long sequence_count;
	const unsigned char *sequences;
	long page_count;
	const unsigned char *pages;
	/* each page's pixels, as the map stores them */
	unsigned char *pixels[MAXIMUM_PAGES];
	unsigned long pixels_size[MAXIMUM_PAGES];
};

static void source_free(struct effect_source *source)
{
	long index;

	for (index = 0; index < MAXIMUM_PAGES; index++)
		free(source->pixels[index]);
	memset(source, 0, sizeof(*source));
}

/* the tags' bytes at `address` (`size` of them), or NULL outside them */
static const unsigned char *tags_at(const struct effect_source *source, unsigned long address, unsigned long size)
{
	return xbox_map_tags_at(source->tags, source->tags_size, address, size);
}

/* whether a page (its format, side and bytes) is one the effect's recipe
redraws: the flash's, 2D A1R5G5B5 squares; the splat's, DXT1 or DXT3 ones */
static int page_taken(unsigned int recipe, const unsigned char *page)
{
	unsigned int width = half_le(page + 4), height = half_le(page + 6), format = half_le(page + 0xC);
	unsigned long size = word_le(page + 0x1C), least;

	if (half_le(page + 0xA) != 0 || width != height || width < 8 || width > MAXIMUM_PAGE_SIDE || (width & (width - 1)))
		return 0;
	switch (recipe)
	{
	case _hd_effect_recipe_flash:
		least = width * height * 2UL;
		return format == BITMAP_FORMAT_A1R5G5B5 && size >= least && size <= 2 * least;
	case _hd_effect_recipe_splat:
		least = format == BITMAP_FORMAT_DXT1 ? width * height / 2UL : width * height;
		return (format == BITMAP_FORMAT_DXT1 || format == BITMAP_FORMAT_DXT3) && size >= least && size <= 2 * least;
	default:
		return 0;
	}
}

/* Finds the effect's bitmap in a map's tags (`tags`, `tags_size` bytes)
and the ranges of the map its pages are at (into `ranges`): their count,
or 0 if the map has none, or not one its recipe takes. */
static int source_find(const unsigned char *tags, unsigned long tags_size, long effect, struct effect_source *source,
	struct xbox_map_range *ranges)
{
	const struct hd_effect_embedded *embedded = &hd_effects_embedded[effect];
	const unsigned char *root = NULL;
	unsigned long instances, count, index;
	size_t length = strlen(embedded->tag);

	memset(source, 0, sizeof(*source));
	source->tags = tags;
	source->tags_size = tags_size;

	/* the tag of the effect's name, a bitmap */
	instances = word_le(tags);
	count = word_le(tags + 0xC);
	for (index = 0; index < count && !root; index++)
	{
		const unsigned char *instance = tags_at(source, instances + index * 0x20, 0x20);
		const unsigned char *name;

		if (!instance)
			goto refused;
		if (word_le(instance) != GROUP_BITMAP)
			continue;
		name = tags_at(source, word_le(instance + 0x10), (unsigned long)length + 1);
		if (name && !memcmp(name, embedded->tag, length + 1))
			root = tags_at(source, word_le(instance + 0x14), BITMAP_ROOT_SIZE);
	}
	if (!root)
		goto refused;
	source->root = root;
	source->sequence_count = (long)word_le(root + 0x54);
	source->page_count = (long)word_le(root + 0x60);
	source->sequences = tags_at(source, word_le(root + 0x58), (unsigned long)source->sequence_count * BITMAP_SEQUENCE_SIZE);
	source->pages = tags_at(source, word_le(root + 0x64), (unsigned long)source->page_count * BITMAP_DATA_SIZE);
	if (source->sequence_count < 0 || source->sequence_count > 256 || !source->sequences ||
		source->page_count < 1 || source->page_count > MAXIMUM_PAGES || !source->pages)
	{
		goto refused;
	}
	for (index = 0; index < (unsigned long)source->sequence_count; index++)
	{
		const unsigned char *sequence = source->sequences + index * BITMAP_SEQUENCE_SIZE;

		if (word_le(sequence + 0x34) > 256 ||
			!tags_at(source, word_le(sequence + 0x38), word_le(sequence + 0x34) * BITMAP_SPRITE_SIZE))
		{
			goto refused;
		}
	}

	/* its pages, as the recipe has them */
	for (index = 0; index < (unsigned long)source->page_count; index++)
	{
		const unsigned char *page = source->pages + index * BITMAP_DATA_SIZE;
		unsigned long size = word_le(page + 0x1C);

		if (!page_taken(embedded->recipe, page) || !(source->pixels[index] = malloc(size)))
			goto refused;
		source->pixels_size[index] = size;
		ranges[index].offset = word_le(page + 0x18);
		ranges[index].size = size;
		ranges[index].destination = source->pixels[index];
	}
	return (int)source->page_count;

refused:
	source_free(source);
	return 0;
}

/* whether the pages read are the stock ones (their CRC-32 the effect's) */
static int source_stock(const char *path, long effect, const struct effect_source *source)
{
	unsigned long crc = 0;
	long index;

	for (index = 0; index < source->page_count; index++)
		crc = crc32(crc, source->pixels[index], (uInt)source->pixels_size[index]);
	if (crc != hd_effects_embedded[effect].crc)
	{
		platform_log("hd effects: %s's %s is not the stock one (CRC %08lx): passed", path,
			hd_effects_embedded[effect].name, crc);
		return 0;
	}
	return 1;
}

/* ---------- the flash recipe: planes of floats, width x height */

static float smoothstep(float value, float low, float high)
{
	float t = (value - low) / (high - low);

	t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static float clampf(float value, float low, float high)
{
	return value < low ? low : value > high ? high : value;
}

/* 0 to 255 rounded to the nearest, halves to the even (NumPy's round; the
guests' C library has no rounding modes for nearbyintf) */
static unsigned char round_byte(float value)
{
	long whole = (long)floorf(value);
	float part = value - (float)whole;

	if (part > 0.5f || (part == 0.5f && (whole & 1)))
		whole++;
	return (unsigned char)(whole < 0 ? 0 : whole > 255 ? 255 : whole);
}

/* SciPy's reflect mode: d c b a | a b c d | d c b a */
static long reflect(long index, long count)
{
	while (index < 0 || index >= count)
		index = index < 0 ? -index - 1 : 2 * count - index - 1;
	return index;
}

/* A Gaussian blur of `in` into `out` (which may be `in`), through `work`
(a plane), as SciPy's gaussian_filter: separable, reflected, cut at 4 sigma. */
static void blur(const float *in, float *out, float *work, long width, long height, float sigma)
{
	float kernel[2 * MAXIMUM_RADIUS + 1];
	float row[MAXIMUM_PAGE_SIDE * 4 + 2 * MAXIMUM_RADIUS];
	long radius = (long)(4.0f * sigma + 0.5f), x, y, k;
	float sum = 0.0f;

	if (radius > MAXIMUM_RADIUS)
		radius = MAXIMUM_RADIUS;
	for (k = -radius; k <= radius; k++)
		sum += kernel[k + radius] = expf(-0.5f * (float)(k * k) / (sigma * sigma));
	for (k = 0; k <= 2 * radius; k++)
		kernel[k] /= sum;
	for (y = 0; y < height; y++)
	{
		const float *line = in + y * width;
		float *target = work + y * width;

		for (x = 0; x < width + 2 * radius; x++)
			row[x] = line[reflect(x - radius, width)];
		for (x = 0; x < width; x++)
		{
			float total = 0.0f;

			for (k = 0; k <= 2 * radius; k++)
				total += kernel[k] * row[x + k];
			target[x] = total;
		}
	}
	for (y = 0; y < height; y++)
	{
		float *target = out + y * width;

		memset(target, 0, (size_t)width * sizeof(*target));
		for (k = 0; k <= 2 * radius; k++)
		{
			const float *line = work + reflect(y + k - radius, height) * width;
			float weight = kernel[k];

			for (x = 0; x < width; x++)
				target[x] += weight * line[x];
		}
	}
}

/* the least (or most) of each texel's (2 radius + 1)-square neighborhood,
reflected, into `out` (not `in`), through `work` */
static void extreme_filter(const float *in, float *out, float *work, long width, long height, long radius, int most)
{
	long x, y, k;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
		{
			float value = in[y * width + x];

			for (k = -radius; k <= radius; k++)
			{
				float other = in[y * width + reflect(x + k, width)];

				value = most ? (other > value ? other : value) : (other < value ? other : value);
			}
			work[y * width + x] = value;
		}
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
		{
			float value = work[y * width + x];

			for (k = -radius; k <= radius; k++)
			{
				float other = work[reflect(y + k, height) * width + x];

				value = most ? (other > value ? other : value) : (other < value ? other : value);
			}
			out[y * width + x] = value;
		}
}

/* Pillow's bicubic (a = -0.5) */
static float bicubic(float x)
{
	const float a = -0.5f;

	if (x < 0.0f)
		x = -x;
	if (x < 1.0f)
		return ((a + 2.0f) * x - (a + 3.0f)) * x * x + 1.0f;
	if (x < 2.0f)
		return (((x - 5.0f) * x + 8.0f) * x - 4.0f) * a;
	return 0.0f;
}

/* Pillow's taps for an output texel of an upscale by `scale` (at most 5 of
them, from *first): their weights, normalized */
static int bicubic_taps(long output, long size, long scale, long *first, float weights[5])
{
	float center = ((float)output + 0.5f) / (float)scale, total = 0.0f;
	long low = (long)(center - 2.0f + 0.5f), high = (long)(center + 2.0f + 0.5f), index;
	int count;

	if (low < 0)
		low = 0;
	if (high > size)
		high = size;
	count = (int)(high - low);
	if (count > 5)
		count = 5;
	for (index = 0; index < count; index++)
		total += weights[index] = bicubic(((float)(index + low) - center + 0.5f));
	for (index = 0; index < count && total != 0.0f; index++)
		weights[index] /= total;
	*first = low;
	return count;
}

/* `in` (side x side) upscaled `scale` times into `out`, through `work` (side
x side x scale), held within each texel's 3 x 3 neighbors' range (lowest
and highest, planes of side x side: `low`, `high`) */
static void upscale(const float *in, float *out, float *work, float *low, float *high, long side, long scale)
{
	long big = side * scale, x, y;
	int k;

	for (y = 0; y < side; y++)
		for (x = 0; x < big; x++)
		{
			float weights[5], total = 0.0f;
			long first;
			int count = bicubic_taps(x, side, scale, &first, weights);

			for (k = 0; k < count; k++)
				total += weights[k] * in[y * side + first + k];
			work[y * big + x] = total;
		}
	for (y = 0; y < big; y++)
	{
		float weights[5];
		long first;
		int count = bicubic_taps(y, side, scale, &first, weights);

		for (x = 0; x < big; x++)
		{
			float total = 0.0f;

			for (k = 0; k < count; k++)
				total += weights[k] * work[(first + k) * big + x];
			out[y * big + x] = total;
		}
	}
	/* (the range through out's last plane-sized work: `work` holds side x
	side x scale floats, enough for one) */
	extreme_filter(in, low, work, side, side, 1, 0);
	extreme_filter(in, high, work, side, side, 1, 1);
	for (y = 0; y < big; y++)
		for (x = 0; x < big; x++)
		{
			long source = (y / scale) * side + x / scale;

			out[y * big + x] = clampf(out[y * big + x], low[source], high[source]);
		}
}

/* a sprite's box in its page (texels) */
struct box
{
	long left, top, right, bottom;
};

/* each sprite's box: its alpha's 4-connected regions' bounds, in the order
SciPy's label finds them (a region's first texel, row by row) */
static long sprite_boxes(const unsigned char *alpha, long side, struct box *boxes, long most)
{
	long *stack = malloc((size_t)side * side * sizeof(*stack));
	unsigned char *seen = calloc((size_t)side * side, 1);
	long count = 0, start;

	if (!stack || !seen)
	{
		free(stack);
		free(seen);
		return -1;
	}
	for (start = 0; start < side * side; start++)
	{
		struct box box;
		long depth = 0;

		if (!alpha[start] || seen[start])
			continue;
		box.left = box.right = start % side;
		box.top = box.bottom = start / side;
		seen[start] = 1;
		stack[depth++] = start;
		while (depth)
		{
			long texel = stack[--depth], x = texel % side, y = texel / side;
			long neighbors[4] = { x > 0 ? texel - 1 : -1, x < side - 1 ? texel + 1 : -1, y > 0 ? texel - side : -1,
				y < side - 1 ? texel + side : -1 };
			int k;

			box.left = x < box.left ? x : box.left;
			box.right = x > box.right ? x : box.right;
			box.top = y < box.top ? y : box.top;
			box.bottom = y > box.bottom ? y : box.bottom;
			for (k = 0; k < 4; k++)
				if (neighbors[k] >= 0 && alpha[neighbors[k]] && !seen[neighbors[k]])
				{
					seen[neighbors[k]] = 1;
					stack[depth++] = neighbors[k];
				}
		}
		box.right++;
		box.bottom++;
		if (count < most)
			boxes[count] = box;
		count++;
	}
	free(stack);
	free(seen);
	return count < most ? count : most;
}

/* The page's levels as the tag file has them (A8R8G8B8: blue, green, red
and alpha bytes), redrawn from its stock pixels (A1R5G5B5, swizzled unless
linear); NULL if out of memory. *size: their bytes, *levels: how many. */
static unsigned char *flash_page(const unsigned char *pixels, long side, int linear, long scale, long smallest,
	unsigned long *size, long *levels)
{
	enum { _up = 0, _original = 3, _smooth = 6, _extra = 9, _work = 10, _planes = 11 };
	long big = side * scale, count = big * big, x, y, index, box_count;
	float *plane[_planes] = { 0 }, *color = NULL, *alpha = NULL, *low = NULL, *high = NULL;
	unsigned char *bytes = NULL, *alpha_bits = NULL;
	struct box boxes[256];
	long level_count = 1, level_side;
	unsigned long total = 0;
	int c;

	*size = 0;
	*levels = 0;
	/* (the levels down to the smallest, as the script's mipmaps) */
	for (level_side = big; level_side > smallest && level_side > 1; level_side /= 2)
		level_count++;
	for (index = 0, level_side = big; index < level_count; index++, level_side /= 2)
		total += (unsigned long)level_side * level_side * 4;
	for (index = 0; index < _planes; index++)
		if (!(plane[index] = malloc((size_t)count * sizeof(float))))
			goto finished;
	color = malloc((size_t)side * side * 3 * sizeof(float));
	alpha = malloc((size_t)side * side * sizeof(float));
	low = malloc((size_t)side * side * sizeof(float));
	high = malloc((size_t)side * side * sizeof(float));
	alpha_bits = malloc((size_t)side * side);
	bytes = malloc(total);
	if (!color || !alpha || !low || !high || !alpha_bits || !bytes)
		goto finished;

	/* the stock texels: color 0 to 1, alpha 0 or 1 */
	{
		int x_bits = 0, y_bits = 0;

		while ((1L << x_bits) < side)
			x_bits++;
		y_bits = x_bits;
		for (y = 0; y < side; y++)
			for (x = 0; x < side; x++)
			{
				unsigned long stored = 0;
				unsigned int value;

				if (linear)
				{
					stored = (unsigned long)(y * side + x);
				}
				else
				{
					int bit = 0, shift_x = 0, shift_y = 0;

					while (shift_x < x_bits || shift_y < y_bits)
					{
						if (shift_x < x_bits)
							stored |= (unsigned long)((x >> shift_x++) & 1) << bit++;
						if (shift_y < y_bits)
							stored |= (unsigned long)((y >> shift_y++) & 1) << bit++;
					}
				}
				value = half_le(pixels + stored * 2);
				color[(y * side + x) * 3 + 0] = (float)((value >> 10) & 31) / 31.0f;
				color[(y * side + x) * 3 + 1] = (float)((value >> 5) & 31) / 31.0f;
				color[(y * side + x) * 3 + 2] = (float)(value & 31) / 31.0f;
				alpha[y * side + x] = (float)(value >> 15);
				alpha_bits[y * side + x] = (unsigned char)(value >> 15);
			}
	}
	box_count = sprite_boxes(alpha_bits, side, boxes, (long)(sizeof(boxes) / sizeof(boxes[0])));
	if (box_count < 0)
		goto finished;

	/* upscaled, each channel (the stock channel first in the smooth planes) */
	for (c = 0; c < 3; c++)
	{
		for (index = 0; index < side * side; index++)
			plane[_extra][index] = color[index * 3 + c];
		upscale(plane[_extra], plane[_up + c], plane[_work], low, high, side, scale);
	}

	/* the banding smoothed: twice, the second wider */
	for (c = 0; c < 3; c++)
		memcpy(plane[_original + c], plane[_up + c], (size_t)count * sizeof(float));
	{
		const float sigmas[2] = { 1.5f, 3.5f };
		int pass;

		for (pass = 0; pass < 2; pass++)
		{
			for (c = 0; c < 3; c++)
				blur(plane[_up + c], plane[_smooth + c], plane[_work], big, big, sigmas[pass] * (float)scale);
			for (index = 0; index < count; index++)
			{
				float difference = 0.0f, brightest = 0.0f, keep;

				for (c = 0; c < 3; c++)
				{
					float d = fabsf(plane[_up + c][index] - plane[_smooth + c][index]);

					difference = d > difference ? d : difference;
					brightest = plane[_smooth + c][index] > brightest ? plane[_smooth + c][index] : brightest;
				}
				keep = smoothstep(difference / FLASH_STEP, 1.0f, 2.5f);
				if (smoothstep(brightest, 0.25f, 0.5f) > keep)
					keep = smoothstep(brightest, 0.25f, 0.5f);
				for (c = 0; c < 3; c++)
					plane[_up + c][index] = plane[_smooth + c][index] * (1.0f - keep) + plane[_up + c][index] * keep;
			}
		}
		for (c = 0; c < 3; c++)
			for (index = 0; index < count; index++)
			{
				float original = plane[_original + c][index];
				float least = original - 0.75f * FLASH_STEP;

				plane[_up + c][index] = clampf(plane[_up + c][index], least > 0.0f ? least : 0.0f,
					original + 0.5f * FLASH_STEP);
			}
	}

	/* the cores crisp, the glow soft: into the smooth planes (the original
	planes: the core's weight, the sharp and the soft) */
	for (index = 0; index < count; index++)
	{
		float brightest = plane[_up][index];

		for (c = 1; c < 3; c++)
			brightest = plane[_up + c][index] > brightest ? plane[_up + c][index] : brightest;
		plane[_original][index] = smoothstep(brightest, 0.5f, 0.9f);
	}
	for (c = 0; c < 3; c++)
	{
		float *up = plane[_up + c], *sharp = plane[_original + 1], *soft = plane[_original + 2];
		float *core = plane[_original], *out = plane[_smooth + c];

		blur(up, sharp, plane[_work], big, big, (float)scale / 2.0f);
		extreme_filter(up, soft, plane[_work], big, big, (scale + 1) / 2, 0);
		extreme_filter(up, plane[_extra], plane[_work], big, big, (scale + 1) / 2, 1);
		for (index = 0; index < count; index++)
			sharp[index] = clampf(up[index] + 0.7f * (up[index] - sharp[index]), soft[index], plane[_extra][index]);
		blur(up, soft, plane[_work], big, big, 0.6f * (float)scale);
		for (index = 0; index < count; index++)
			out[index] = clampf(sharp[index] * core[index] + soft[index] * (1.0f - core[index]), 0.0f, 1.0f);
	}

	/* the alpha, each sprite's box with soft edges, into the extra plane;
	nothing outside it */
	upscale(alpha, plane[_up], plane[_work], low, high, side, scale);
	blur(plane[_up], plane[_extra], plane[_work], big, big, 0.5f * (float)scale);
	for (index = 0; index < count; index++)
	{
		plane[_extra][index] = clampf(plane[_extra][index], 0.0f, 1.0f);
		for (c = 0; c < 3; c++)
			plane[_smooth + c][index] *= plane[_extra][index];
	}

	/* each sprite as bright as the map's (its summed color) */
	for (index = 0; index < box_count; index++)
	{
		const struct box *box = &boxes[index];
		double stock = 0.0, made = 0.0;

		for (y = box->top; y < box->bottom; y++)
			for (x = box->left; x < box->right; x++)
				for (c = 0; c < 3; c++)
					stock += color[(y * side + x) * 3 + c];
		for (y = box->top * scale; y < box->bottom * scale; y++)
			for (x = box->left * scale; x < box->right * scale; x++)
				for (c = 0; c < 3; c++)
					made += plane[_smooth + c][y * big + x];
		if (made > 0.0)
		{
			float factor = (float)(stock * (double)(scale * scale) / made);

			for (y = box->top * scale; y < box->bottom * scale; y++)
				for (x = box->left * scale; x < box->right * scale; x++)
					for (c = 0; c < 3; c++)
						plane[_smooth + c][y * big + x] = clampf(plane[_smooth + c][y * big + x] * factor, 0.0f, 1.0f);
		}
	}

	/* the levels: each a 2 x 2 box of the last (as floats), each rounded */
	{
		/* (the red, green and blue planes, then the alpha's, each level
		made in place at the plane's start) */
		float *channels[4] = { plane[_smooth + 2], plane[_smooth + 1], plane[_smooth], plane[_extra] };
		unsigned char *level = bytes;
		long level_index;

		level_side = big;
		for (level_index = 0; level_index < level_count; level_index++)
		{
			if (level_index)
			{
				long next = level_side / 2;

				for (c = 0; c < 4; c++)
					for (y = 0; y < next; y++)
						for (x = 0; x < next; x++)
						{
							const float *last = channels[c];

							channels[c][y * next + x] = 0.25f * (last[(2 * y) * level_side + 2 * x] +
								last[(2 * y) * level_side + 2 * x + 1] + last[(2 * y + 1) * level_side + 2 * x] +
								last[(2 * y + 1) * level_side + 2 * x + 1]);
						}
				level_side = next;
			}
			for (index = 0; index < level_side * level_side; index++)
				for (c = 0; c < 4; c++)
					*level++ = round_byte(clampf(channels[c][index], 0.0f, 1.0f) * 255.0f);
		}
	}
	*size = total;
	*levels = level_count;

finished:
	for (index = 0; index < _planes; index++)
		free(plane[index]);
	free(color);
	free(alpha);
	free(low);
	free(high);
	free(alpha_bits);
	if (!*levels)
	{
		free(bytes);
		bytes = NULL;
	}
	return bytes;
}

/* ---------- the splat recipe (tools/hd_blood.py): planes of floats, the
edges clamped */

static long clamp_index(long index, long count)
{
	return index < 0 ? 0 : index >= count ? count - 1 : index;
}

/* a Gaussian blur of `in` into `out` (not `in`), through `work`, as the
script's: separable, cut at 4 sigma, the edges clamped */
static void splat_blur(const float *in, float *out, float *work, long width, long height, float sigma)
{
	float kernel[2 * MAXIMUM_RADIUS + 1];
	float row[MAXIMUM_PAGE_SIDE * 4 + 2 * MAXIMUM_RADIUS];
	long radius = (long)(4.0f * sigma + 0.5f), x, y, k;
	double sum = 0.0;

	if (radius > MAXIMUM_RADIUS)
		radius = MAXIMUM_RADIUS;
	for (k = -radius; k <= radius; k++)
		sum += kernel[k + radius] = (float)exp(-0.5 * (double)(k * k) / ((double)sigma * sigma));
	for (k = 0; k <= 2 * radius; k++)
		kernel[k] = (float)(kernel[k] / sum);
	for (y = 0; y < height; y++)
	{
		const float *line = in + y * width;
		float *target = work + y * width;

		/* (the row with its clamped edges) */
		for (x = 0; x < width + 2 * radius; x++)
			row[x] = line[clamp_index(x - radius, width)];
		for (x = 0; x < width; x++)
		{
			float total = 0.0f;

			for (k = 0; k <= 2 * radius; k++)
				total += kernel[k] * row[x + k];
			target[x] = total;
		}
	}
	for (y = 0; y < height; y++)
	{
		float *target = out + y * width;

		memset(target, 0, (size_t)width * sizeof(*target));
		for (k = -radius; k <= radius; k++)
		{
			const float *line = work + clamp_index(y + k, height) * width;
			float weight = kernel[k + radius];

			for (x = 0; x < width; x++)
				target[x] += weight * line[x];
		}
	}
}

/* the least (or most) of each texel's (2 radius + 1)-square neighborhood,
the edges clamped, into `out` (not `in`), through `work` */
static void splat_extreme(const float *in, float *out, float *work, long width, long height, long radius, int most)
{
	float row[MAXIMUM_PAGE_SIDE * 4 + 2 * MAXIMUM_RADIUS];
	long x, y, k;

	for (y = 0; y < height; y++)
	{
		/* (the row with its clamped edges) */
		for (x = 0; x < width + 2 * radius; x++)
			row[x] = in[y * width + clamp_index(x - radius, width)];
		for (x = 0; x < width; x++)
		{
			float value = row[x + radius];

			for (k = 0; k <= 2 * radius; k++)
				value = most ? (row[x + k] > value ? row[x + k] : value) : (row[x + k] < value ? row[x + k] : value);
			work[y * width + x] = value;
		}
	}
	for (y = 0; y < height; y++)
	{
		float *target = out + y * width;

		memcpy(target, work + y * width, (size_t)width * sizeof(*target));
		for (k = -radius; k <= radius; k++)
		{
			const float *line = work + clamp_index(y + k, height) * width;

			for (x = 0; x < width; x++)
				target[x] = most ? (line[x] > target[x] ? line[x] : target[x]) : (line[x] < target[x] ? line[x] : target[x]);
		}
	}
}

/* Catmull-Rom's four weights for each of the `scale` texels a map texel i
becomes (phase p, centered at (p + 0.5) / scale - 0.5 map texels from i),
on the map's texels from i + first[p] */
static void splat_cubic_weights(long scale, long first[16], float weights[16][4])
{
	long phase;

	for (phase = 0; phase < scale && phase < 16; phase++)
	{
		double position = ((double)phase + 0.5) / (double)scale - 0.5, f = position - floor(position);

		first[phase] = (long)floor(position) - 1;
		weights[phase][0] = (float)(((-0.5 * f + 1.0) * f - 0.5) * f);
		weights[phase][1] = (float)((1.5 * f - 2.5) * f * f + 1.0);
		weights[phase][2] = (float)(((-1.5 * f + 2.0) * f + 0.5) * f);
		weights[phase][3] = (float)((0.5 * f - 0.5) * f * f);
	}
}

/* `in` (width x height) upscaled `scale` times into `out`, bicubic,
through `work` (width x height x scale), held within each texel's 3 x 3
neighbors' range (planes of width x height: `low`, `high`, `small`) */
static void splat_upscale(const float *in, float *out, float *work, float *low, float *high, float *small, long width,
	long height, long scale)
{
	long wide = width * scale, tall = height * scale, first[16], x, y;
	float weights[16][4];
	int k;

	splat_cubic_weights(scale, first, weights);
	for (y = 0; y < height; y++)
		for (x = 0; x < wide; x++)
		{
			long phase = x % scale, i = x / scale;
			float total = 0.0f;

			for (k = 0; k < 4; k++)
				total += weights[phase][k] * in[y * width + clamp_index(i + first[phase] + k, width)];
			work[y * wide + x] = total;
		}
	for (y = 0; y < tall; y++)
	{
		long phase = y % scale, i = y / scale;

		for (x = 0; x < wide; x++)
		{
			float total = 0.0f;

			for (k = 0; k < 4; k++)
				total += weights[phase][k] * work[clamp_index(i + first[phase] + k, height) * wide + x];
			out[y * wide + x] = total;
		}
	}
	splat_extreme(in, low, small, width, height, 1, 0);
	splat_extreme(in, high, small, width, height, 1, 1);
	for (y = 0; y < tall; y++)
		for (x = 0; x < wide; x++)
		{
			long source = (y / scale) * width + x / scale;

			out[y * wide + x] = clampf(out[y * wide + x], low[source], high[source]);
		}
}

/* the DXT blocks and banding smoothed: within about `step` of the
neighborhood's smooth value, that; beyond it (the paint's detail) the
texel as it is; never moved more than a step (`smooth`, `work`: planes) */
static void splat_deblock(float *plane, float *smooth, float *work, long width, long height, long scale, float step)
{
	long index;

	splat_blur(plane, smooth, work, width, height, SPLAT_DEBLOCK_SIGMA * (float)scale);
	for (index = 0; index < width * height; index++)
	{
		float value = plane[index];
		float keep = smoothstep(fabsf(value - smooth[index]) / step, 1.0f, 2.5f);

		plane[index] = clampf(smooth[index] * (1.0f - keep) + value * keep, value - step, value + step);
	}
}

/* the edges pushed toward their neighborhood's least or most, as far as
its contrast says it is an edge (a soft shock filter; `low`, `high`,
`work`: planes) */
static void splat_crisp(float *plane, float *low, float *high, float *work, long width, long height, long scale)
{
	long index;

	splat_extreme(plane, low, work, width, height, SPLAT_EDGE_REACH * scale, 0);
	splat_extreme(plane, high, work, width, height, SPLAT_EDGE_REACH * scale, 1);
	for (index = 0; index < width * height; index++)
	{
		float value = plane[index], contrast = high[index] - low[index];
		float t = (value - low[index]) / (contrast > 1e-6f ? contrast : 1e-6f);
		float target = low[index] + contrast * smoothstep(t, 0.25f, 0.75f);
		float edge = smoothstep(contrast, SPLAT_EDGE_LOW, SPLAT_EDGE_HIGH) * SPLAT_EDGE_STRENGTH;

		plane[index] = value + (target - value) * edge;
	}
}

/* A cell of a page (its planes: red, green, blue and alpha, `width` x
`height`, 0 to 1; changed: a particle's color bled under its alpha)
redrawn at `scale` into `out` (four planes of width x height x scale x
scale): 0 if out of memory. A decal's alpha is the map's (each texel
scale x scale), a particle's redrawn as its color is. */
static int splat_cell(float *in[4], long width, long height, long scale, int decal, float *out[4])
{
	enum { _smooth, _low, _high, _work, _small_low, _small_high, _small_work, _small_product, _small_blurred, _planes };
	long wide = width * scale, tall = height * scale, count = wide * tall, small = width * height, index;
	float *plane[_planes] = { 0 };
	int c, made = 0;

	for (index = 0; index < _planes; index++)
		if (!(plane[index] = malloc((size_t)(index < _small_low ? count : small) * sizeof(float))))
			goto finished;

	/* a particle's color where its alpha is 0: its neighbors', weighted by
	their alpha (the map's where none has any) */
	if (!decal)
	{
		float *weight = plane[_small_low];

		splat_blur(in[3], weight, plane[_small_work], width, height, SPLAT_BLEED_SIGMA);
		for (c = 0; c < 3; c++)
		{
			for (index = 0; index < small; index++)
				plane[_small_product][index] = in[c][index] * in[3][index];
			splat_blur(plane[_small_product], plane[_small_blurred], plane[_small_work], width, height, SPLAT_BLEED_SIGMA);
			for (index = 0; index < small; index++)
				if (in[3][index] <= 0.0f && weight[index] > 1e-4f)
					in[c][index] = plane[_small_blurred][index] / (weight[index] > 1e-4f ? weight[index] : 1e-4f);
		}
	}

	/* each plane upscaled, deblocked and its edges made crisp */
	for (c = 0; c < 4; c++)
	{
		if (c == 3 && decal)
		{
			long x, y;

			for (y = 0; y < tall; y++)
				for (x = 0; x < wide; x++)
					out[3][y * wide + x] = in[3][(y / scale) * width + x / scale];
			continue;
		}
		splat_upscale(in[c], out[c], plane[_work], plane[_small_low], plane[_small_high], plane[_small_work], width, height,
			scale);
		splat_deblock(out[c], plane[_smooth], plane[_work], wide, tall, scale, c == 3 ? SPLAT_ALPHA_STEP : SPLAT_COLOR_STEP);
		splat_crisp(out[c], plane[_low], plane[_high], plane[_work], wide, tall, scale);
	}
	for (c = 0; c < 4; c++)
		for (index = 0; index < count; index++)
			out[c][index] = clampf(out[c][index], 0.0f, 1.0f);

	/* the map's color: each channel's mean (weighted by the alpha, a
	particle's), moved toward white or black, whichever it is off from */
	{
		double weight_in = 0.0, weight_out = 0.0;

		for (index = 0; index < small; index++)
			weight_in += decal ? 1.0 : (double)in[3][index];
		for (index = 0; index < count; index++)
			weight_out += decal ? 1.0 : (double)out[3][index];
		for (c = 0; c < 3 && weight_in > 0.0 && weight_out > 0.0; c++)
		{
			double stock = 0.0, drawn = 0.0;

			for (index = 0; index < small; index++)
				stock += (double)in[c][index] * (decal ? 1.0 : (double)in[3][index]);
			for (index = 0; index < count; index++)
				drawn += (double)out[c][index] * (decal ? 1.0 : (double)out[3][index]);
			stock /= weight_in;
			drawn /= weight_out;
			if (drawn < stock)
			{
				double factor = (1.0 - stock) / (1.0 - drawn > 1e-6 ? 1.0 - drawn : 1e-6);

				for (index = 0; index < count; index++)
					out[c][index] = (float)(1.0 - (1.0 - (double)out[c][index]) * factor);
			}
			else if (drawn > stock)
			{
				double factor = stock / drawn;

				for (index = 0; index < count; index++)
					out[c][index] = (float)((double)out[c][index] * factor);
			}
		}
	}
	made = 1;

finished:
	for (index = 0; index < _planes; index++)
		free(plane[index]);
	return made;
}

/* a page's DXT1 or DXT3 blocks (row by row) decoded into four planes
(red, green, blue, alpha: 0 to 1), as hud_assets.py's decode_dxt */
static void dxt_decode(const unsigned char *pixels, int dxt3, long side, float *planes[4])
{
	long blocks = side / 4, block;
	int block_size = dxt3 ? 16 : 8;

	for (block = 0; block < blocks * blocks; block++)
	{
		const unsigned char *chunk = pixels + block * block_size, *color = chunk + block_size - 8;
		unsigned int c0 = half_le(color), c1 = half_le(color + 2), palette[4][4], i, k;
		unsigned long bits = word_le(color + 4);
		long left = block % blocks * 4, top = block / blocks * 4;

		for (k = 0; k < 2; k++)
		{
			unsigned int value = k ? c1 : c0;

			palette[k][0] = ((value >> 11) & 31) * 255 / 31;
			palette[k][1] = ((value >> 5) & 63) * 255 / 63;
			palette[k][2] = (value & 31) * 255 / 31;
			palette[k][3] = 255;
		}
		for (k = 0; k < 3; k++)
		{
			if (dxt3 || c0 > c1)
			{
				palette[2][k] = (2 * palette[0][k] + palette[1][k]) / 3;
				palette[3][k] = (palette[0][k] + 2 * palette[1][k]) / 3;
			}
			else
			{
				palette[2][k] = (palette[0][k] + palette[1][k]) / 2;
				palette[3][k] = 0;
			}
		}
		palette[2][3] = 255;
		palette[3][3] = dxt3 || c0 > c1 ? 255 : 0;
		for (i = 0; i < 16; i++)
		{
			const unsigned int *texel = palette[(bits >> (2 * i)) & 3];
			long at = (top + (long)(i / 4)) * side + left + (long)(i % 4);
			unsigned int alpha = dxt3 ? ((chunk[i / 2] >> (4 * (i & 1))) & 15) * 17 : texel[3];

			for (k = 0; k < 3; k++)
				planes[k][at] = (float)texel[k] / 255.0f;
			planes[3][at] = (float)alpha / 255.0f;
		}
	}
}

/* a float from the tags (little-endian) */
static float float_le(const unsigned char *bytes)
{
	union
	{
		unsigned int word;
		float value;
	} cast;

	cast.word = (unsigned int)word_le(bytes);
	return cast.value;
}

/* each of the page's sprites' boxes, in texels, once each (as the script's
sprite_cells): their count */
static long splat_cells(const struct effect_source *source, long page, long side, struct box *cells, long most)
{
	long sequence, count = 0;

	for (sequence = 0; sequence < source->sequence_count; sequence++)
	{
		const unsigned char *in = source->sequences + sequence * BITMAP_SEQUENCE_SIZE;
		const unsigned char *sprites = tags_at(source, word_le(in + 0x38), word_le(in + 0x34) * BITMAP_SPRITE_SIZE);
		unsigned long sprite;

		for (sprite = 0; sprites && sprite < word_le(in + 0x34); sprite++)
		{
			const unsigned char *at = sprites + sprite * BITMAP_SPRITE_SIZE;
			struct box cell;
			long index;

			if ((long)(short)half_le(at) != page)
				continue;
			cell.left = (long)floor((double)float_le(at + 0x08) * (double)side + 1e-3);
			cell.right = (long)ceil((double)float_le(at + 0x0C) * (double)side - 1e-3);
			cell.top = (long)floor((double)float_le(at + 0x10) * (double)side + 1e-3);
			cell.bottom = (long)ceil((double)float_le(at + 0x14) * (double)side - 1e-3);
			cell.left = cell.left < 0 ? 0 : cell.left;
			cell.top = cell.top < 0 ? 0 : cell.top;
			cell.right = cell.right > side ? side : cell.right;
			cell.bottom = cell.bottom > side ? side : cell.bottom;
			if (cell.right <= cell.left || cell.bottom <= cell.top)
				continue;
			for (index = 0; index < count; index++)
				if (!memcmp(&cells[index], &cell, sizeof(cell)))
					break;
			if (index == count && count < most)
				cells[count++] = cell;
		}
	}
	return count;
}

/* The page's levels as the tag file has them (A8R8G8B8), redrawn from its
stock pixels (DXT1 or DXT3): the page whole, then each sprite's cell on its
own over it; NULL if out of memory. *size: their bytes, *levels: how many. */
static unsigned char *splat_page(const struct effect_source *source, long page, long scale, long smallest,
	unsigned long *size, long *levels)
{
	const unsigned char *data = source->pages + page * BITMAP_DATA_SIZE;
	long side = (long)half_le(data + 4), big = side * scale, count = big * big, cell_count, index, level_count = 1;
	long level_side;
	int decal = half_le(data + 0xC) == BITMAP_FORMAT_DXT1, c;
	float *stock[4] = { 0 }, *drawn[4] = { 0 }, *cell_in[4] = { 0 }, *cell_out[4] = { 0 };
	struct box cells[MAXIMUM_CELLS + 1];
	unsigned char *bytes = NULL;
	unsigned long total = 0;

	*size = 0;
	*levels = 0;
	for (level_side = big; level_side > smallest && level_side > 1; level_side /= 2)
		level_count++;
	for (index = 0, level_side = big; index < level_count; index++, level_side /= 2)
		total += (unsigned long)level_side * level_side * 4;
	for (c = 0; c < 4; c++)
	{
		stock[c] = malloc((size_t)side * side * sizeof(float));
		drawn[c] = malloc((size_t)count * sizeof(float));
		cell_in[c] = malloc((size_t)side * side * sizeof(float));
		cell_out[c] = malloc((size_t)count * sizeof(float));
		if (!stock[c] || !drawn[c] || !cell_in[c] || !cell_out[c])
			goto finished;
	}
	if (!(bytes = malloc(total)))
		goto finished;
	dxt_decode(source->pixels[page], !decal, side, stock);

	/* the page whole (for the texels between the cells), then each cell */
	cells[0].left = cells[0].top = 0;
	cells[0].right = cells[0].bottom = side;
	cell_count = 1 + splat_cells(source, page, side, cells + 1, MAXIMUM_CELLS);
	for (index = 0; index < cell_count; index++)
	{
		const struct box *cell = &cells[index];
		long width = cell->right - cell->left, height = cell->bottom - cell->top, x, y;

		for (c = 0; c < 4; c++)
			for (y = 0; y < height; y++)
				memcpy(cell_in[c] + y * width, stock[c] + (cell->top + y) * side + cell->left, (size_t)width * sizeof(float));
		if (!splat_cell(cell_in, width, height, scale, decal, index ? cell_out : drawn))
			goto finished;
		if (!index)
			continue;
		for (c = 0; c < 4; c++)
			for (y = 0; y < height * scale; y++)
				for (x = 0; x < width * scale; x++)
					drawn[c][(cell->top * scale + y) * big + cell->left * scale + x] = cell_out[c][y * width * scale + x];
	}

	/* the levels: each a 2 x 2 box of the last (as floats), each rounded; a
	decal's alpha 0 or 1 at each */
	{
		float *channels[4] = { drawn[2], drawn[1], drawn[0], drawn[3] };
		unsigned char *level = bytes;
		long level_index, x, y;

		level_side = big;
		for (level_index = 0; level_index < level_count; level_index++)
		{
			if (level_index)
			{
				long next = level_side / 2;

				for (c = 0; c < 4; c++)
					for (y = 0; y < next; y++)
						for (x = 0; x < next; x++)
						{
							const float *last = channels[c];

							channels[c][y * next + x] = 0.25f * (last[(2 * y) * level_side + 2 * x] +
								last[(2 * y) * level_side + 2 * x + 1] + last[(2 * y + 1) * level_side + 2 * x] +
								last[(2 * y + 1) * level_side + 2 * x + 1]);
						}
				level_side = next;
			}
			for (index = 0; index < level_side * level_side; index++)
				for (c = 0; c < 4; c++)
				{
					float value = channels[c][index];

					if (c == 3 && decal)
						value = value >= 0.5f ? 1.0f : 0.0f;
					*level++ = round_byte(clampf(value, 0.0f, 1.0f) * 255.0f);
				}
		}
	}
	*size = total;
	*levels = level_count;

finished:
	for (c = 0; c < 4; c++)
	{
		free(stock[c]);
		free(drawn[c]);
		free(cell_in[c]);
		free(cell_out[c]);
	}
	if (!*levels)
	{
		free(bytes);
		bytes = NULL;
	}
	return bytes;
}

/* ---------- the tag file */

/* The effect made from its source and written to the cache, with its note:
0 if it cannot be. */
static int effect_write(long effect, const struct effect_source *source)
{
	const struct hd_effect_embedded *embedded = &hd_effects_embedded[effect];
	unsigned char *levels[MAXIMUM_PAGES] = { 0 };
	unsigned long level_sizes[MAXIMUM_PAGES] = { 0 }, pixels_size = 0, sprite_count = 0, size, cursor;
	long level_counts[MAXIMUM_PAGES] = { 0 }, page, sequence;
	unsigned char *file = NULL;
	char path[MAXIMUM_PATH], note[160];
	int written = 0;

	for (page = 0; page < source->page_count; page++)
	{
		const unsigned char *data = source->pages + page * BITMAP_DATA_SIZE;
		long side = (long)half_le(data + 4);

		if (embedded->recipe == _hd_effect_recipe_splat)
		{
			levels[page] = splat_page(source, page, (long)embedded->scale, (long)embedded->smallest, &level_sizes[page],
				&level_counts[page]);
		}
		else
		{
			levels[page] = flash_page(source->pixels[page], side, (half_le(data + 0xE) & BITMAP_LINEAR_FLAG) != 0,
				(long)embedded->scale, (long)embedded->smallest, &level_sizes[page], &level_counts[page]);
		}
		if (!levels[page])
			goto finished;
		pixels_size += level_sizes[page];
	}
	for (sequence = 0; sequence < source->sequence_count; sequence++)
		sprite_count += word_le(source->sequences + sequence * BITMAP_SEQUENCE_SIZE + 0x34);
	size = TAG_FILE_HEADER_SIZE + BITMAP_ROOT_SIZE + pixels_size + (unsigned long)source->sequence_count *
		BITMAP_SEQUENCE_SIZE + sprite_count * BITMAP_SPRITE_SIZE + (unsigned long)source->page_count * BITMAP_DATA_SIZE;
	if (!(file = calloc(1, size)))
		goto finished;

	/* the header (its group, its size and its group's version) and the
	bitmap's values, the map's (little-endian) written big-endian */
	memcpy(file + 0x24, "bitm", 4);
	put_be32(file + 0x2C, TAG_FILE_HEADER_SIZE);
	put_be16(file + 0x38, TAG_FILE_BITMAP_VERSION);
	memcpy(file + 0x3C, "blam", 4);
	cursor = TAG_FILE_HEADER_SIZE;
	swap_into(file + cursor, source->root, 2, 4);
	put_be16(file + cursor + 2, BITMAP_GROUP_FORMAT_32_BIT);
	swap_into(file + cursor + 0x08, source->root + 0x08, 4, 3);
	swap_into(file + cursor + 0x14, source->root + 0x14, 2, 4);
	put_be32(file + cursor + 0x30, pixels_size);
	swap_into(file + cursor + 0x44, source->root + 0x44, 4, 2);
	swap_into(file + cursor + 0x4C, source->root + 0x4C, 2, 3);
	put_be32(file + cursor + 0x54, (unsigned long)source->sequence_count);
	put_be32(file + cursor + 0x60, (unsigned long)source->page_count);
	cursor += BITMAP_ROOT_SIZE;
	for (page = 0; page < source->page_count; page++)
	{
		memcpy(file + cursor, levels[page], level_sizes[page]);
		cursor += level_sizes[page];
	}
	/* the sequences, then their sprites, the map's */
	for (sequence = 0; sequence < source->sequence_count; sequence++)
	{
		const unsigned char *in = source->sequences + sequence * BITMAP_SEQUENCE_SIZE;

		memcpy(file + cursor, in, 32);
		file[cursor + 31] = 0;
		swap_into(file + cursor + 0x20, in + 0x20, 2, 2);
		put_be32(file + cursor + 0x34, word_le(in + 0x34));
		cursor += BITMAP_SEQUENCE_SIZE;
	}
	for (sequence = 0; sequence < source->sequence_count; sequence++)
	{
		const unsigned char *in = source->sequences + sequence * BITMAP_SEQUENCE_SIZE;
		const unsigned char *sprites = tags_at(source, word_le(in + 0x38), word_le(in + 0x34) * BITMAP_SPRITE_SIZE);
		unsigned long sprite;

		for (sprite = 0; sprite < word_le(in + 0x34); sprite++)
		{
			swap_into(file + cursor, sprites + sprite * BITMAP_SPRITE_SIZE, 2, 1);
			swap_into(file + cursor + 0x08, sprites + sprite * BITMAP_SPRITE_SIZE + 0x08, 4, 6);
			cursor += BITMAP_SPRITE_SIZE;
		}
	}
	/* the pages, 32-bit, at the scale */
	{
		unsigned long offset = 0;

		for (page = 0; page < source->page_count; page++)
		{
			const unsigned char *in = source->pages + page * BITMAP_DATA_SIZE;
			unsigned int side = half_le(in + 4) * embedded->scale;

			memcpy(file + cursor, "bitm", 4);
			put_be16(file + cursor + 4, side);
			put_be16(file + cursor + 6, side);
			put_be16(file + cursor + 8, 1);
			put_be16(file + cursor + 0xA, 0);
			put_be16(file + cursor + 0xC, BITMAP_FORMAT_A8R8G8B8);
			put_be16(file + cursor + 0xE, BITMAP_POWER_OF_TWO_FLAG);
			put_be16(file + cursor + 0x14, (unsigned int)(level_counts[page] - 1));
			put_be32(file + cursor + 0x18, offset);
			offset += level_sizes[page];
			cursor += BITMAP_DATA_SIZE;
		}
	}

	mkdir(cache_folder(), 0755);
	effect_path(path, sizeof(path), effect, "note");
	remove(path);
	effect_path(path, sizeof(path), effect, "bitmap");
	if (cursor == size && write_file(path, file, size))
	{
		note_text(note, sizeof(note), effect, size);
		effect_path(path, sizeof(path), effect, "note");
		written = write_file(path, note, (unsigned long)strlen(note));
	}

finished:
	for (page = 0; page < MAXIMUM_PAGES; page++)
		free(levels[page]);
	free(file);
	return written;
}

/* the effects being made: which still are, and which of their maps were tried */
struct build_state
{
	int pending[MAXIMUM_EFFECTS];
	unsigned char tried[MAXIMUM_EFFECTS][MAXIMUM_EFFECT_MAPS];
	long total, done;
};

/* the effect's `index`th map's path, or NULL past its last */
static const char *map_path_at(long effect, long index)
{
	const char *path = hd_effects.map_paths[effect];

	for (; path && *path && index > 0; index--)
		path += strlen(path) + 1;
	return path && *path ? path : NULL;
}

/* (with no lock) the effect made or not, and the progress */
static void effect_finish(struct build_state *state, long effect, int made)
{
	pthread_mutex_lock(&hd_effects.lock);
	state->pending[effect] = 0;
	hd_effects.queued[effect] = 0;
	hd_effects.checked[effect] = made;
	hd_effects.valid[effect] = made;
	hd_effects.failed[effect] = !made;
	hd_effects.progress = (float)++state->done / (float)(state->total ? state->total : 1);
	pthread_mutex_unlock(&hd_effects.lock);
}

static double seconds_since(const struct timespec *began)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (double)(now.tv_sec - began->tv_sec) + (double)(now.tv_nsec - began->tv_nsec) / 1e9;
}

/* the effects of a map made on several threads */
struct build_work
{
	struct build_state *state;
	struct effect_source *sources;
	const char *path;
	long effects[MAXIMUM_EFFECTS];
	long count, next;
};

static void *build_worker(void *context)
{
	struct build_work *work = context;

	for (;;)
	{
		struct timespec began;
		long effect;
		int made;

		pthread_mutex_lock(&hd_effects.lock);
		effect = work->next < work->count ? work->effects[work->next++] : -1;
		pthread_mutex_unlock(&hd_effects.lock);
		if (effect < 0)
			return NULL;
		clock_gettime(CLOCK_MONOTONIC, &began);
		made = effect_write(effect, &work->sources[effect]);
		platform_log("hd effects: %s %s from %s in %.2f s", hd_effects_embedded[effect].name,
			made ? "made" : "could not be written", work->path, seconds_since(&began));
		effect_finish(work->state, effect, made);
	}
}

/* Every effect still wanted that lists the map at `path` and has not tried
it made from it, if the map has its stock bitmap: the map's tags read
once, and all their pages in one more pass (a campaign map is over 300 MB
inflated). */
static void map_batch_make(struct build_state *state, const char *path)
{
	static struct effect_source sources[MAXIMUM_EFFECTS];
	static struct xbox_map_range ranges[MAXIMUM_EFFECTS * MAXIMUM_PAGES];
	unsigned char *tags;
	unsigned long tags_size = 0;
	int found[MAXIMUM_EFFECTS] = { 0 }, range_count = 0, read = 0;
	long effect, index;
	struct timespec began;

	clock_gettime(CLOCK_MONOTONIC, &began);
	tags = xbox_map_tags_read(path, &tags_size);
	for (effect = 0; effect < effect_count(); effect++)
	{
		const char *other;

		if (!state->pending[effect])
			continue;
		for (index = 0; (other = map_path_at(effect, index)) != NULL && index < MAXIMUM_EFFECT_MAPS; index++)
			if (!state->tried[effect][index] && !strcmp(other, path))
				break;
		if (!other || index >= MAXIMUM_EFFECT_MAPS)
			continue;
		state->tried[effect][index] = 1;
		if (tags && (found[effect] = source_find(tags, tags_size, effect, &sources[effect], ranges + range_count)) > 0)
			range_count += found[effect];
	}
	if (range_count)
		read = xbox_map_ranges_read(path, ranges, range_count);
	platform_log("hd effects: %s read for %d pages in %.2f s", path, range_count, seconds_since(&began));

	/* those with the stock bitmap made, on up to BUILD_WORKERS threads */
	{
		struct build_work work;
		pthread_t workers[BUILD_WORKERS];
		int started = 0;

		memset(&work, 0, sizeof(work));
		work.state = state;
		work.sources = sources;
		work.path = path;
		for (effect = 0; effect < effect_count(); effect++)
			if (found[effect] && read && source_stock(path, effect, &sources[effect]))
				work.effects[work.count++] = effect;
		while (started < BUILD_WORKERS - 1 && started < work.count - 1 &&
			pthread_create(&workers[started], NULL, build_worker, &work) == 0)
		{
			started++;
		}
		build_worker(&work);
		while (started > 0)
			pthread_join(workers[--started], NULL);
	}
	for (effect = 0; effect < effect_count(); effect++)
		if (found[effect])
			source_free(&sources[effect]);
	free(tags);
}

static void *build_thread(void *context)
{
	static struct build_state state;
	long effect;
	struct timespec began;

	(void)context;
	clock_gettime(CLOCK_MONOTONIC, &began);
	memset(&state, 0, sizeof(state));
	for (effect = 0; effect < effect_count(); effect++)
	{
		state.pending[effect] = hd_effects.queued[effect];
		state.total += state.pending[effect];
	}
	for (;;)
	{
		const char *path = NULL;

		/* the next map: the first not tried of the first effect still wanted
		(one with none left is not made) */
		for (effect = 0; effect < effect_count() && !path; effect++)
		{
			long index;

			if (!state.pending[effect])
				continue;
			for (index = 0; index < MAXIMUM_EFFECT_MAPS && (path = map_path_at(effect, index)) != NULL; index++)
				if (!state.tried[effect][index])
					break;
			if (index >= MAXIMUM_EFFECT_MAPS)
				path = NULL;
			if (!path)
			{
				platform_log("hd effects: %s not made: no map of it has its stock bitmap", hd_effects_embedded[effect].name);
				effect_finish(&state, effect, 0);
			}
		}
		if (!path)
			break;
		map_batch_make(&state, path);
	}
	platform_log("hd effects: %ld done in %.2f s", state.done, seconds_since(&began));
	pthread_mutex_lock(&hd_effects.lock);
	hd_effects.made++;
	hd_effects.running = 0;
	pthread_mutex_unlock(&hd_effects.lock);
	return NULL;
}

/* the paths of the effect's maps on this machine, each ended by a 0, the
last by two (found on the game's thread: platform_translate_path) */
static char *map_paths_find(long effect, const char *maps_folder)
{
	const char *names = hd_effects_embedded[effect].maps;
	char *paths = calloc(MAXIMUM_EFFECT_MAPS, MAXIMUM_PATH), *cursor = paths;
	int count = 0;

	while (paths && names && *names && count < MAXIMUM_EFFECT_MAPS - 1)
	{
		char name[64], xbox_path[512];
		size_t length = strcspn(names, " ");

		if (length && length < sizeof(name))
		{
			memcpy(name, names, length);
			name[length] = 0;
			snprintf(xbox_path, sizeof(xbox_path), "%s%s.map", maps_folder, name);
			platform_translate_path(xbox_path, cursor, MAXIMUM_PATH);
			if (*cursor)
			{
				cursor += strlen(cursor) + 1;
				count++;
			}
		}
		names += length;
		while (*names == ' ')
			names++;
	}
	return paths;
}

/* (with the lock) whether the effect is to be made */
static int effect_wanted(long effect)
{
	if (!hd_effects.checked[effect])
	{
		hd_effects.valid[effect] = effect_cached(effect);
		hd_effects.checked[effect] = 1;
	}
	return !hd_effects.valid[effect] && !hd_effects.failed[effect] && !hd_effects.queued[effect];
}

/* (the thread, if one was begun and has ended, joined) */
static void build_join(int wait)
{
	int running;

	pthread_mutex_lock(&hd_effects.lock);
	running = hd_effects.running;
	pthread_mutex_unlock(&hd_effects.lock);
	if (hd_effects.thread_started && (wait || !running))
	{
		pthread_join(hd_effects.thread, NULL);
		hd_effects.thread_started = 0;
	}
}

/* ---------- public code */

int hd_effects_enabled(void)
{
	return effect_count() > 0 && config_boolean("display.hd_effects") && !halo_server_mode() &&
		!config_boolean("debug.null_renderer");
}

int hd_effects_wanted(void)
{
	long effect;
	int wanted = 0;

	if (!hd_effects_enabled())
		return 0;
	pthread_mutex_lock(&hd_effects.lock);
	for (effect = 0; effect < effect_count() && !wanted; effect++)
		wanted = effect_wanted(effect);
	pthread_mutex_unlock(&hd_effects.lock);
	return wanted;
}

int hd_effects_build_begin(const char *maps_folder)
{
	long effect;
	int any = 0;

	build_join(1);
	if (!hd_effects_enabled() || !maps_folder)
		return 0;
	pthread_mutex_lock(&hd_effects.lock);
	for (effect = 0; effect < effect_count(); effect++)
	{
		if (!effect_wanted(effect))
			continue;
		free(hd_effects.map_paths[effect]);
		hd_effects.map_paths[effect] = map_paths_find(effect, maps_folder);
		hd_effects.queued[effect] = hd_effects.map_paths[effect] != NULL;
		any |= hd_effects.queued[effect];
	}
	hd_effects.progress = 0.0f;
	hd_effects.running = any;
	pthread_mutex_unlock(&hd_effects.lock);
	if (any)
	{
		if (pthread_create(&hd_effects.thread, NULL, build_thread, NULL) == 0)
		{
			hd_effects.thread_started = 1;
		}
		else
		{
			/* (no thread: made here) */
			build_thread(NULL);
		}
	}
	return any;
}

int hd_effects_build_running(float *progress)
{
	int running;

	pthread_mutex_lock(&hd_effects.lock);
	running = hd_effects.running;
	if (progress)
		*progress = hd_effects.progress;
	pthread_mutex_unlock(&hd_effects.lock);
	if (!running)
		build_join(0);
	return running;
}

void hd_effects_build_now(const char *maps_folder)
{
	build_join(1);
	if (hd_effects_wanted() && hd_effects_build_begin(maps_folder))
		build_join(1);
}

unsigned long hd_effects_changes(void)
{
	unsigned long made;

	pthread_mutex_lock(&hd_effects.lock);
	made = hd_effects.made;
	pthread_mutex_unlock(&hd_effects.lock);
	return made << 1 | (hd_effects_enabled() ? 1UL : 0UL);
}

int hd_effects_bitmap_file(const char *tag, char *path, unsigned long size, unsigned int *stock_crc)
{
	long effect;
	int found = 0;

	if (!tag || !hd_effects_enabled())
		return 0;
	pthread_mutex_lock(&hd_effects.lock);
	for (effect = 0; effect < effect_count() && !found; effect++)
	{
		if (strcmp(hd_effects_embedded[effect].tag, tag) != 0 || hd_effects.queued[effect])
			continue;
		if (!hd_effects.checked[effect])
		{
			hd_effects.valid[effect] = effect_cached(effect);
			hd_effects.checked[effect] = 1;
		}
		if (hd_effects.valid[effect])
		{
			effect_path(path, size, effect, "bitmap");
			*stock_crc = hd_effects_embedded[effect].crc;
			found = 1;
		}
	}
	pthread_mutex_unlock(&hd_effects.lock);
	return found;
}
