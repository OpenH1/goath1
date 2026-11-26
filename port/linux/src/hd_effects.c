/*
HD_EFFECTS.C

The HD effects made from the player's own maps and kept (hd_effects.h):
which are wanted, the thread that makes them, the map read, the recipe, and
the tag file written.

A map is read as map_unpack.c reads it to copy it: its 2 KB header, then one
zlib stream of the rest, inflated as it is read and never held whole. Its
tags come first (the header says where they are: the end of the map), then,
in a second pass, the stock bitmap's pages, which the tags place earlier.
The pages' CRC-32 must be the effect's: any other bitmap (a modded map's) is
not the stock one the recipe was made for, and the next map is tried.

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
*/

#include "hd_effects.h"
#include "platform.h"
#include "port_config.h"
#include "posix.h"
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

/* an Xbox map's (map_unpack.c's) */
#define MAP_HEADER_SIZE 0x800
#define MAP_HEADER_HEAD 0x68656164UL
#define MAP_HEADER_FOOT 0x666F6F74UL
#define MAP_VERSION 5
#define MAP_TAGS_BASE 0x803A6000UL
#define MAP_MOST_TAGS_SIZE (64UL << 20)
#define MAP_READ_SIZE (256UL << 10)
#define MAP_INFLATE_SIZE (1UL << 20)

/* the tags' (bitmap_group.h) */
#define GROUP_BITMAP 0x6269746DUL
#define BITMAP_ROOT_SIZE 0x6C
#define BITMAP_SEQUENCE_SIZE 0x40
#define BITMAP_SPRITE_SIZE 0x20
#define BITMAP_DATA_SIZE 0x30
#define BITMAP_LINEAR_FLAG 0x10
#define BITMAP_FORMAT_A1R5G5B5 8
#define BITMAP_FORMAT_A8R8G8B8 11
#define BITMAP_GROUP_FORMAT_32_BIT 4
#define BITMAP_POWER_OF_TWO_FLAG 1
#define TAG_FILE_HEADER_SIZE 0x40
#define TAG_FILE_BITMAP_VERSION 7

/* the flash recipe's: one of the map's color steps (five bits a channel) */
#define FLASH_STEP (1.0f / 31.0f)
/* Gaussians are cut at 4 sigma, as SciPy's (sigma at most 14: 57 texels) */
#define MAXIMUM_RADIUS 64

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

	snprintf(text, size, "openh1-hd-effects %d %08x %u %u %lu\n", HD_EFFECTS_RECIPE_VERSION, embedded->crc,
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

/* a range of the map (its offset as inflated, header included) to copy out */
struct map_range
{
	unsigned long offset;
	unsigned long size;
	unsigned char *destination;
};

/* Inflates the map at `path` far enough to copy each of the ranges; 0 if
it cannot be read or ends first. */
static int map_ranges_read(const char *path, struct map_range *ranges, int count)
{
	unsigned char header[MAP_HEADER_SIZE];
	unsigned char *in = NULL, *out = NULL;
	unsigned long end = 0, done = MAP_HEADER_SIZE, length;
	off_t read_offset = MAP_HEADER_SIZE;
	z_stream stream;
	int source, inflating = 0, read = 0, index;

	for (index = 0; index < count; index++)
		if (ranges[index].offset + ranges[index].size > end)
			end = ranges[index].offset + ranges[index].size;
	source = open(path, O_RDONLY | O_CLOEXEC);
	if (source < 0 || pread(source, header, sizeof(header), 0) != (ssize_t)sizeof(header))
		goto finished;
	length = word_le(header + 8);
	if (end > length)
		goto finished;
	/* (the header's part of each range) */
	for (index = 0; index < count; index++)
	{
		struct map_range *range = &ranges[index];

		if (range->offset < MAP_HEADER_SIZE)
		{
			unsigned long size = MAP_HEADER_SIZE - range->offset < range->size ? MAP_HEADER_SIZE - range->offset :
				range->size;

			memcpy(range->destination, header + range->offset, size);
		}
	}
	in = malloc(MAP_READ_SIZE);
	out = malloc(MAP_INFLATE_SIZE);
	memset(&stream, 0, sizeof(stream));
	if (!in || !out || inflateInit(&stream) != Z_OK)
		goto finished;
	inflating = 1;
	while (done < end)
	{
		unsigned long produced;
		int result;

		if (!stream.avail_in)
		{
			ssize_t got = pread(source, in, MAP_READ_SIZE, read_offset);

			if (got <= 0)
				goto finished;
			read_offset += got;
			stream.next_in = in;
			stream.avail_in = (uInt)got;
		}
		stream.next_out = out;
		stream.avail_out = (uInt)MAP_INFLATE_SIZE;
		result = inflate(&stream, Z_NO_FLUSH);
		if (result != Z_OK && result != Z_STREAM_END)
			goto finished;
		produced = (unsigned long)(stream.next_out - out);
		for (index = 0; index < count; index++)
		{
			struct map_range *range = &ranges[index];
			unsigned long first = range->offset > done ? range->offset : done;
			unsigned long last = range->offset + range->size < done + produced ? range->offset + range->size :
				done + produced;

			if (first < last)
				memcpy(range->destination + (first - range->offset), out + (first - done), last - first);
		}
		done += produced;
		if (result == Z_STREAM_END)
			break;
	}
	read = done >= end;

finished:
	if (inflating)
		inflateEnd(&stream);
	free(in);
	free(out);
	if (source >= 0)
		close(source);
	return read;
}

/* the stock bitmap read out of a map */
struct effect_source
{
	unsigned char *tags;
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

	free(source->tags);
	for (index = 0; index < MAXIMUM_PAGES; index++)
		free(source->pixels[index]);
	memset(source, 0, sizeof(*source));
}

/* the tags' bytes at `address` (`size` of them), or NULL outside them */
static const unsigned char *tags_at(const struct effect_source *source, unsigned long address, unsigned long size)
{
	if (address < MAP_TAGS_BASE || address - MAP_TAGS_BASE > source->tags_size ||
		size > source->tags_size - (address - MAP_TAGS_BASE))
	{
		return NULL;
	}
	return source->tags + (address - MAP_TAGS_BASE);
}

/* Reads the effect's stock bitmap out of the map at `path`: 0 if the map has
none, or not the stock one (its pages' CRC), or cannot be read. */
static int source_read(const char *path, long effect, struct effect_source *source)
{
	const struct hd_effect_embedded *embedded = &hd_effects_embedded[effect];
	unsigned char header[MAP_HEADER_SIZE];
	struct map_range ranges[MAXIMUM_PAGES];
	unsigned long tags_offset, instances, count, index, crc = 0;
	const unsigned char *root = NULL;

	memset(source, 0, sizeof(*source));
	{
		int descriptor = open(path, O_RDONLY | O_CLOEXEC);
		int whole = descriptor >= 0 && pread(descriptor, header, sizeof(header), 0) == (ssize_t)sizeof(header);

		if (descriptor >= 0)
			close(descriptor);
		if (!whole)
			return 0;
	}
	if (word_le(header) != MAP_HEADER_HEAD || word_le(header + 0x7FC) != MAP_HEADER_FOOT ||
		word_le(header + 4) != MAP_VERSION)
	{
		return 0;
	}
	tags_offset = word_le(header + 0x10);
	source->tags_size = word_le(header + 0x14);
	if (tags_offset < MAP_HEADER_SIZE || source->tags_size < 0x10 || source->tags_size > MAP_MOST_TAGS_SIZE ||
		!(source->tags = malloc(source->tags_size)))
	{
		goto refused;
	}
	ranges[0].offset = tags_offset;
	ranges[0].size = source->tags_size;
	ranges[0].destination = source->tags;
	if (!map_ranges_read(path, ranges, 1))
		goto refused;

	/* the tag of the effect's name, a bitmap */
	instances = word_le(source->tags);
	count = word_le(source->tags + 0xC);
	for (index = 0; index < count && !root; index++)
	{
		const unsigned char *instance = tags_at(source, instances + index * 0x20, 0x20);
		const unsigned char *name;
		size_t length = strlen(embedded->tag);

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

	/* its pages: 2D A1R5G5B5 squares, as the recipe has them */
	for (index = 0; index < (unsigned long)source->page_count; index++)
	{
		const unsigned char *page = source->pages + index * BITMAP_DATA_SIZE;
		unsigned int width = half_le(page + 4), height = half_le(page + 6);
		unsigned long size = word_le(page + 0x1C);

		if (half_le(page + 0xA) != 0 || half_le(page + 0xC) != BITMAP_FORMAT_A1R5G5B5 || width != height ||
			width < 8 || width > MAXIMUM_PAGE_SIDE || (width & (width - 1)) || size < width * height * 2UL ||
			size > 4UL * width * height || !(source->pixels[index] = malloc(size)))
		{
			goto refused;
		}
		source->pixels_size[index] = size;
		ranges[index].offset = word_le(page + 0x18);
		ranges[index].size = size;
		ranges[index].destination = source->pixels[index];
	}
	if (!map_ranges_read(path, ranges, (int)source->page_count))
		goto refused;
	for (index = 0; index < (unsigned long)source->page_count; index++)
		crc = crc32(crc, source->pixels[index], (uInt)source->pixels_size[index]);
	if (crc != embedded->crc)
	{
		platform_log("hd effects: %s's %s is not the stock one (CRC %08lx): passed", path, embedded->name, crc);
		goto refused;
	}
	return 1;

refused:
	source_free(source);
	return 0;
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

		levels[page] = flash_page(source->pixels[page], side, (half_le(data + 0xE) & BITMAP_LINEAR_FLAG) != 0,
			(long)embedded->scale, (long)embedded->smallest, &level_sizes[page], &level_counts[page]);
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

/* the effect made from the first of its maps that has its stock bitmap */
static int effect_make(long effect, const char *map_paths)
{
	struct timespec began, ended;
	const char *path;

	clock_gettime(CLOCK_MONOTONIC, &began);
	for (path = map_paths; path && *path; path += strlen(path) + 1)
	{
		struct effect_source source;
		int made;

		if (!source_read(path, effect, &source))
			continue;
		made = effect_write(effect, &source);
		source_free(&source);
		clock_gettime(CLOCK_MONOTONIC, &ended);
		platform_log("hd effects: %s %s from %s in %.2f s", hd_effects_embedded[effect].name,
			made ? "made" : "could not be written", path,
			(double)(ended.tv_sec - began.tv_sec) + (double)(ended.tv_nsec - began.tv_nsec) / 1e9);
		return made;
	}
	platform_log("hd effects: %s not made: no map of it has its stock bitmap", hd_effects_embedded[effect].name);
	return 0;
}

static void *build_thread(void *context)
{
	long effect, total = 0, done = 0;

	(void)context;
	for (effect = 0; effect < effect_count(); effect++)
		total += hd_effects.queued[effect];
	for (effect = 0; effect < effect_count(); effect++)
	{
		int made;

		if (!hd_effects.queued[effect])
			continue;
		made = effect_make(effect, hd_effects.map_paths[effect]);
		pthread_mutex_lock(&hd_effects.lock);
		hd_effects.queued[effect] = 0;
		hd_effects.checked[effect] = made;
		hd_effects.valid[effect] = made;
		hd_effects.failed[effect] = !made;
		hd_effects.progress = (float)++done / (float)(total ? total : 1);
		pthread_mutex_unlock(&hd_effects.lock);
	}
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
