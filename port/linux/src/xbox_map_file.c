/*
XBOX_MAP_FILE.C

An Xbox map read from its file (xbox_map_file.h).
*/

#include "xbox_map_file.h"
#include "posix.h"
#include "memory/zlib/zlib.h"

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* the header's (map_unpack.c's) */
#define MAP_HEADER_SIZE 0x800
#define MAP_HEADER_HEAD 0x68656164UL
#define MAP_HEADER_FOOT 0x666F6F74UL
#define MAP_VERSION 5
#define MAP_READ_SIZE (256UL << 10)
#define MAP_INFLATE_SIZE (1UL << 20)

/* (zlib's allocations with the C library's: the game's zlib's own allocator
is the game's thread's only, as menu_pictures.c says) */
static voidpf map_zalloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void map_zfree(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static unsigned long word_le(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

int xbox_map_ranges_read(const char *path, struct xbox_map_range *ranges, int count)
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
		struct xbox_map_range *range = &ranges[index];

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
	stream.zalloc = map_zalloc;
	stream.zfree = map_zfree;
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
			struct xbox_map_range *range = &ranges[index];
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

unsigned char *xbox_map_tags_read(const char *path, unsigned long *size)
{
	unsigned char header[MAP_HEADER_SIZE];
	struct xbox_map_range range;
	unsigned char *tags;
	unsigned long offset;
	int descriptor = open(path, O_RDONLY | O_CLOEXEC);
	int whole = descriptor >= 0 && pread(descriptor, header, sizeof(header), 0) == (ssize_t)sizeof(header);

	if (descriptor >= 0)
		close(descriptor);
	if (!whole || word_le(header) != MAP_HEADER_HEAD || word_le(header + 0x7FC) != MAP_HEADER_FOOT ||
		word_le(header + 4) != MAP_VERSION)
	{
		return NULL;
	}
	offset = word_le(header + 0x10);
	*size = word_le(header + 0x14);
	if (offset < MAP_HEADER_SIZE || *size < 0x10 || *size > XBOX_MAP_MOST_TAGS_SIZE || !(tags = malloc(*size)))
		return NULL;
	range.offset = offset;
	range.size = *size;
	range.destination = tags;
	if (!xbox_map_ranges_read(path, &range, 1))
	{
		free(tags);
		return NULL;
	}
	return tags;
}

const unsigned char *xbox_map_tags_at(const unsigned char *tags, unsigned long tags_size, unsigned long address,
	unsigned long size)
{
	if (address < XBOX_MAP_TAGS_BASE || address - XBOX_MAP_TAGS_BASE > tags_size ||
		size > tags_size - (address - XBOX_MAP_TAGS_BASE))
	{
		return NULL;
	}
	return tags + (address - XBOX_MAP_TAGS_BASE);
}
