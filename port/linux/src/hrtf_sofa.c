/*
HRTF_SOFA.C

A SOFA file's measured head-related impulse responses (hrtf_sofa.h), for
audio.hrtf_file. A SOFA file is netCDF-4, which is HDF5; this reads the part
of HDF5 that SOFA files use, and no more, rather than the HDF5 library:
	- the superblock, versions 0 to 3;
	- object headers, versions 1 and 2, with their continuations;
	- a group's links: a symbol table (version 1 B-tree, symbol nodes, local
	  heap), link messages in the header, or dense (a fractal heap and its
	  version 2 B-tree of names), as netCDF-4 writes a group of more than
	  eight;
	- attributes in the header or dense, of fixed-length strings;
	- datasets of integers or floats (either byte order), compact,
	  contiguous or chunked (a version 1 B-tree of chunks; or, layout
	  version 4, a single chunk or implicit), through deflate (the port's
	  zlib), shuffle and Fletcher-32.
Checksums are not checked; every read is bounded by the file. From the root
group: Data.IR (M x R x N, R 2), Data.SamplingRate, SourcePosition (M x 3,
spherical or, its Type attribute "cartesian", x y z) and Data.Delay (1 or M
x R, if there is one). A file of another SOFAConventions is refused.
*/

#include "hrtf_sofa.h"

#include "../../third_party/zlib/zlib_prefixed.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the largest file read (bytes), and the most of Data.IR (values) */
#define SOFA_LARGEST_FILE (128u << 20)
#define SOFA_LARGEST_RESPONSES (8u << 20)
/* how deep B-trees, heaps and continuations may nest, and the most of a
dataset's dimensions */
#define SOFA_DEPTH 16
#define SOFA_RANK 8
#define SOFA_FILTERS 8
#define SOFA_CONTINUATIONS 64

struct hdf
{
	const uint8_t *data;
	uint64_t size, base;
	int offset_size, length_size;
	char *why;
	size_t why_size;
	int failed;
};

/* a cursor in the file: from at, to end (file positions) */
struct cursor
{
	struct hdf *file;
	uint64_t at, end;
};

static int sofa_fail(struct hdf *file, const char *format, ...)
{
	if (!file->failed && file->why && file->why_size)
	{
		va_list arguments;

		va_start(arguments, format);
		vsnprintf(file->why, file->why_size, format, arguments);
		va_end(arguments);
	}
	file->failed = 1;
	return 0;
}

static struct cursor cursor_at(struct hdf *file, uint64_t position, uint64_t length)
{
	struct cursor c;

	c.file = file;
	c.at = position;
	c.end = position <= file->size && length <= file->size - position ? position + length : file->size;
	if (position > file->size || length > file->size - position)
		sofa_fail(file, "it is cut short or damaged (a read past its end)");
	return c;
}

/* the file position of an address (relative to the base); 0 if it is
undefined (all ones) or past the end */
static int address_valid(struct hdf *file, uint64_t address)
{
	uint64_t undefined = file->offset_size >= 8 ? ~(uint64_t)0 : (((uint64_t)1 << (file->offset_size * 8)) - 1);

	return address != undefined && file->base + address < file->size;
}

static const uint8_t *take_bytes(struct cursor *c, uint64_t bytes)
{
	const uint8_t *p;

	if (c->file->failed || bytes > c->end - c->at)
	{
		sofa_fail(c->file, "it is cut short or damaged (a read past a structure's end)");
		c->at = c->end;
		return NULL;
	}
	p = c->file->data + c->at;
	c->at += bytes;
	return p;
}

static uint64_t take(struct cursor *c, int bytes)
{
	const uint8_t *p = take_bytes(c, (uint64_t)bytes);
	uint64_t value = 0;
	int i;

	if (!p)
		return 0;
	for (i = bytes - 1; i >= 0; i--)
		value = value << 8 | p[i];
	return value;
}

static uint64_t take_offset(struct cursor *c) { return take(c, c->file->offset_size); }
static uint64_t take_length(struct cursor *c) { return take(c, c->file->length_size); }

static void skip(struct cursor *c, uint64_t bytes) { take_bytes(c, bytes); }

static int signature(struct cursor *c, const char *expected)
{
	const uint8_t *p = take_bytes(c, 4);

	if (!p || memcmp(p, expected, 4))
		return sofa_fail(c->file, "it is damaged (no %s where one should be)", expected);
	return 1;
}

/* a cursor at an address, its length the rest of the file */
static struct cursor cursor_address(struct hdf *file, uint64_t address)
{
	if (!address_valid(file, address))
	{
		sofa_fail(file, "it is damaged (an address past its end)");
		return cursor_at(file, file->size, 0);
	}
	return cursor_at(file, file->base + address, file->size - (file->base + address));
}

/* bytes enough for a count up to limit (HDF5's H5VM_limit_enc_size) */
static int limit_bytes(uint64_t limit)
{
	int bits = 0;

	while (limit >> 1)
	{
		limit >>= 1;
		bits++;
	}
	return bits / 8 + 1;
}

static int log2_floor(uint64_t value)
{
	int bits = 0;

	while (value >> 1)
	{
		value >>= 1;
		bits++;
	}
	return bits;
}

/* ---------- object headers */

/* each message of the object header at address: visit(context, type, the
message's cursor, its flags); a visit returning nonzero stops (and that is
returned) */
typedef int (*message_visit)(void *context, int type, struct cursor *message, int flags);

static int each_message(struct hdf *file, uint64_t address, message_visit visit, void *context)
{
	uint64_t blocks[SOFA_CONTINUATIONS][2];
	int block_count = 0, block, version, header_flags = 0;
	struct cursor c = cursor_address(file, address);
	const uint8_t *first;

	if (file->failed)
		return 0;
	first = c.file->data + c.at;
	if (c.end - c.at >= 4 && !memcmp(first, "OHDR", 4))
	{
		uint64_t chunk;

		skip(&c, 4);
		version = (int)take(&c, 1);
		header_flags = (int)take(&c, 1);
		if (version != 2)
			return sofa_fail(file, "it is damaged (an object header of version %d)", version);
		if (header_flags & 0x20)
			skip(&c, 16);
		if (header_flags & 0x10)
			skip(&c, 4);
		chunk = take(&c, 1 << (header_flags & 3));
		blocks[block_count][0] = c.at;
		blocks[block_count++][1] = chunk;
	}
	else
	{
		uint64_t size;

		version = (int)take(&c, 1);
		if (version != 1)
			return sofa_fail(file, "it is damaged (an object header of version %d)", version);
		skip(&c, 3);
		skip(&c, 4);
		size = take(&c, 4);
		skip(&c, 4);
		blocks[block_count][0] = c.at;
		blocks[block_count++][1] = size;
	}
	for (block = 0; block < block_count && !file->failed; block++)
	{
		struct cursor m = cursor_at(file, blocks[block][0], blocks[block][1]);

		if (version == 2 && block > 0)
		{
			if (!signature(&m, "OCHK"))
				return 0;
			/* (less its checksum) */
			m.end = m.end >= m.at + 4 ? m.end - 4 : m.at;
		}
		while (!file->failed)
		{
			int type, flags, result;
			uint64_t size;
			struct cursor message;

			if (m.end - m.at < (uint64_t)(version == 2 ? 4 + (header_flags & 0x04 ? 2 : 0) : 8))
				break;
			if (version == 2)
			{
				type = (int)take(&m, 1);
				size = take(&m, 2);
				flags = (int)take(&m, 1);
				if (header_flags & 0x04)
					skip(&m, 2);
			}
			else
			{
				type = (int)take(&m, 2);
				size = take(&m, 2);
				flags = (int)take(&m, 1);
				skip(&m, 3);
			}
			if (size > m.end - m.at)
				return sofa_fail(file, "it is damaged (a message past its object header)");
			message = cursor_at(file, m.at, size);
			m.at += size;
			if (type == 0x10)
			{
				uint64_t at = take_offset(&message), length = take_length(&message);

				if (block_count == SOFA_CONTINUATIONS)
					return sofa_fail(file, "it is damaged (too many object header continuations)");
				if (!address_valid(file, at))
					return sofa_fail(file, "it is damaged (a continuation past its end)");
				blocks[block_count][0] = file->base + at;
				blocks[block_count++][1] = length;
				continue;
			}
			result = visit(context, type, &message, flags);
			if (result)
				return result;
		}
	}
	return 0;
}

/* ---------- v2 B-trees and fractal heaps (dense links and attributes) */

struct heap
{
	struct hdf *file;
	int id_length, width, max_direct_rows, offset_bytes, length_bytes, flags;
	uint64_t start_block, max_direct, root, max_managed;
	int root_rows;
};

static int heap_open(struct hdf *file, uint64_t address, struct heap *heap)
{
	struct cursor c = cursor_address(file, address);
	int filters, max_heap_bits;

	memset(heap, 0, sizeof(*heap));
	heap->file = file;
	if (!signature(&c, "FRHP"))
		return 0;
	if (take(&c, 1) != 0)
		return sofa_fail(file, "it is damaged (a fractal heap of an unknown version)");
	heap->id_length = (int)take(&c, 2);
	filters = (int)take(&c, 2);
	heap->flags = (int)take(&c, 1);
	heap->max_managed = take(&c, 4);
	take_length(&c); /* next huge object id */
	take_offset(&c); /* huge objects' B-tree */
	take_length(&c); /* free space */
	take_offset(&c); /* free space manager */
	take_length(&c); /* managed space */
	take_length(&c); /* allocated */
	take_length(&c); /* iterator */
	take_length(&c); /* managed objects */
	take_length(&c); /* huge objects' size */
	take_length(&c); /* huge objects */
	take_length(&c); /* tiny objects' size */
	take_length(&c); /* tiny objects */
	heap->width = (int)take(&c, 2);
	heap->start_block = take_length(&c);
	heap->max_direct = take_length(&c);
	max_heap_bits = (int)take(&c, 2);
	take(&c, 2); /* starting rows */
	heap->root = take_offset(&c);
	heap->root_rows = (int)take(&c, 2);
	if (file->failed)
		return 0;
	if (filters)
		return sofa_fail(file, "it holds a filtered fractal heap, which is not read");
	if (heap->width < 1 || heap->start_block < 1 || heap->max_direct < heap->start_block || max_heap_bits < 1 ||
		max_heap_bits > 64 || (heap->start_block & (heap->start_block - 1)) || (heap->max_direct & (heap->max_direct - 1)))
		return sofa_fail(file, "it is damaged (a fractal heap's table)");
	heap->max_direct_rows = log2_floor(heap->max_direct) - log2_floor(heap->start_block) + 2;
	heap->offset_bytes = (max_heap_bits + 7) / 8;
	heap->length_bytes = (log2_floor(heap->max_direct) + 7) / 8;
	if (limit_bytes(heap->max_managed) < heap->length_bytes)
		heap->length_bytes = limit_bytes(heap->max_managed);
	return 1;
}

static uint64_t row_block_size(const struct heap *heap, int row)
{
	return row < 2 ? heap->start_block : heap->start_block << (row - 1);
}

/* the file position of the managed object at offset (in the heap) in the
indirect block at address of rows rows, which starts at block_offset */
static int heap_locate(const struct heap *heap, uint64_t address, int rows, uint64_t block_offset, uint64_t offset,
	int depth, uint64_t *position)
{
	struct hdf *file = heap->file;
	struct cursor c;
	uint64_t row_start = block_offset;
	int row, direct_rows = rows < heap->max_direct_rows ? rows : heap->max_direct_rows;

	if (depth > SOFA_DEPTH)
		return sofa_fail(file, "it is damaged (a fractal heap too deep)");
	if (rows == 0)
	{
		/* (the root a direct block) */
		if (!address_valid(file, address) || offset >= heap->start_block)
			return sofa_fail(file, "it is damaged (a heap object outside its block)");
		*position = file->base + address + offset;
		return 1;
	}
	c = cursor_address(file, address);
	if (!signature(&c, "FHIB"))
		return 0;
	skip(&c, 1 + (uint64_t)file->offset_size + (uint64_t)heap->offset_bytes);
	for (row = 0; row < rows; row++)
	{
		uint64_t size = row_block_size(heap, row), span = size * (uint64_t)heap->width;

		if (offset < row_start + span)
		{
			int column = (int)((offset - row_start) / size), entry = row * heap->width + column;
			uint64_t child_offset = row_start + (uint64_t)column * size, child;
			struct cursor e = c;

			if (row < heap->max_direct_rows)
			{
				skip(&e, (uint64_t)entry * (uint64_t)file->offset_size);
				child = take_offset(&e);
				if (file->failed || !address_valid(file, child) || offset - child_offset >= size)
					return sofa_fail(file, "it is damaged (a heap object in no block)");
				*position = file->base + child + (offset - child_offset);
				return 1;
			}
			skip(&e, ((uint64_t)direct_rows * (uint64_t)heap->width + (uint64_t)(entry - direct_rows * heap->width)) *
				(uint64_t)file->offset_size);
			child = take_offset(&e);
			if (file->failed)
				return 0;
			return heap_locate(heap, child, log2_floor(size) - (log2_floor(heap->start_block) +
				log2_floor((uint64_t)heap->width)) + 1, child_offset, offset, depth + 1, position);
		}
		row_start += span;
	}
	return sofa_fail(file, "it is damaged (a heap object past its heap)");
}

/* the heap object of id: a cursor over it */
static int heap_object(const struct heap *heap, const uint8_t *id, struct cursor *object)
{
	struct hdf *file = heap->file;
	int type = id[0] >> 4 & 3, i;

	if (type == 0)
	{
		uint64_t offset = 0, length = 0, position;

		if (1 + heap->offset_bytes + heap->length_bytes > heap->id_length)
			return sofa_fail(file, "it is damaged (a heap id too short)");
		for (i = heap->offset_bytes - 1; i >= 0; i--)
			offset = offset << 8 | id[1 + i];
		for (i = heap->length_bytes - 1; i >= 0; i--)
			length = length << 8 | id[1 + heap->offset_bytes + i];
		if (!heap_locate(heap, heap->root, heap->root_rows, 0, offset, 0, &position))
			return 0;
		*object = cursor_at(file, position, length);
		return !file->failed;
	}
	if (type == 2)
	{
		/* (tiny: the object in the id itself) */
		uint64_t start = (uint64_t)(id - file->data);
		int extended = heap->id_length > 18;
		uint64_t length = extended ? ((uint64_t)(id[0] & 0x0F) << 8 | id[1]) + 1 : (uint64_t)(id[0] & 0x0F) + 1;

		if (length + (extended ? 2 : 1) > (uint64_t)heap->id_length)
			return sofa_fail(file, "it is damaged (a tiny heap object too long)");
		*object = cursor_at(file, start + (extended ? 2 : 1), length);
		return !file->failed;
	}
	return sofa_fail(file, "it holds a huge heap object, which is not read");
}

/* each record of the v2 B-tree at address: visit(context, record) */
typedef int (*record_visit)(void *context, const uint8_t *record);

struct btree2
{
	struct hdf *file;
	int record_size, max_nrec_size;
	int cum_size[SOFA_DEPTH + 1];
	record_visit visit;
	void *context;
};

static int btree2_node(struct btree2 *tree, uint64_t address, uint64_t records, int depth)
{
	struct hdf *file = tree->file;
	struct cursor c = cursor_address(file, address);
	uint64_t r;
	int result;

	if (!signature(&c, depth ? "BTIN" : "BTLF"))
		return 0;
	skip(&c, 2);
	for (r = 0; r < records && !file->failed; r++)
	{
		const uint8_t *record = take_bytes(&c, (uint64_t)tree->record_size);

		if (record && (result = tree->visit(tree->context, record)) != 0)
			return result;
	}
	if (depth)
	{
		for (r = 0; r <= records && !file->failed; r++)
		{
			uint64_t child = take_offset(&c), child_records = take(&c, tree->max_nrec_size);

			if (depth > 1)
				take(&c, tree->cum_size[depth - 1]);
			if (file->failed)
				return 0;
			if ((result = btree2_node(tree, child, child_records, depth - 1)) != 0)
				return result;
		}
	}
	return 0;
}

static int btree2_each(struct hdf *file, uint64_t address, record_visit visit, void *context)
{
	struct cursor c = cursor_address(file, address);
	struct btree2 tree;
	uint64_t node_size, root, root_records, max_nrec[SOFA_DEPTH + 1], cum[SOFA_DEPTH + 1];
	int depth, u;

	memset(&tree, 0, sizeof(tree));
	tree.file = file;
	tree.visit = visit;
	tree.context = context;
	if (!signature(&c, "BTHD"))
		return 0;
	skip(&c, 2);
	node_size = take(&c, 4);
	tree.record_size = (int)take(&c, 2);
	depth = (int)take(&c, 2);
	skip(&c, 2);
	root = take_offset(&c);
	root_records = take(&c, 2);
	if (file->failed)
		return 0;
	if (depth > SOFA_DEPTH || tree.record_size < 1 || node_size < 10 + (uint64_t)tree.record_size)
		return sofa_fail(file, "it is damaged (a B-tree's header)");
	/* (the bytes of the counts in an internal node's pointers, as HDF5
	works them out: H5B2__hdr_init) */
	max_nrec[0] = (node_size - 10) / (uint64_t)tree.record_size;
	cum[0] = max_nrec[0];
	tree.cum_size[0] = 0;
	tree.max_nrec_size = limit_bytes(max_nrec[0]);
	for (u = 1; u <= depth; u++)
	{
		uint64_t pointer = (uint64_t)file->offset_size + (uint64_t)tree.max_nrec_size +
			(u > 1 ? (uint64_t)tree.cum_size[u - 1] : 0);

		if (node_size < 10 + pointer)
			return sofa_fail(file, "it is damaged (a B-tree's node size)");
		max_nrec[u] = (node_size - (10 + pointer)) / ((uint64_t)tree.record_size + pointer);
		cum[u] = (max_nrec[u] + 1) * cum[u - 1] + max_nrec[u];
		tree.cum_size[u] = limit_bytes(cum[u]);
	}
	if (!address_valid(file, root))
		return 0; /* (empty) */
	return btree2_node(&tree, root, root_records, depth);
}

/* ---------- groups */

struct link_search
{
	struct hdf *file;
	const char *name;
	uint64_t found;
	struct heap heap;
	int offset_in_record;
};

/* a link message: whether it is name's (a hard link), its object's
address in found */
static int link_message(struct link_search *search, struct cursor *c)
{
	int flags, type = 0;
	uint64_t length;
	const uint8_t *name;

	if (take(c, 1) != 1)
		return sofa_fail(search->file, "it is damaged (a link of an unknown version)");
	flags = (int)take(c, 1);
	if (flags & 0x08)
		type = (int)take(c, 1);
	if (flags & 0x04)
		skip(c, 8);
	if (flags & 0x10)
		skip(c, 1);
	length = take(c, 1 << (flags & 3));
	name = take_bytes(c, length);
	if (!name || type != 0 || length != strlen(search->name) || memcmp(name, search->name, (size_t)length))
		return 0;
	search->found = take_offset(c);
	return !search->file->failed;
}

static int dense_link(void *context, const uint8_t *record)
{
	struct link_search *search = context;
	struct cursor object;

	if (!heap_object(&search->heap, record + search->offset_in_record, &object))
		return 0;
	return link_message(search, &object);
}

static int symbol_node(struct link_search *search, uint64_t node, uint64_t heap_data, uint64_t heap_size, int depth)
{
	struct hdf *file = search->file;
	struct cursor c = cursor_address(file, node);
	int level, entries, i;

	if (depth > SOFA_DEPTH)
		return sofa_fail(file, "it is damaged (a group's B-tree too deep)");
	if (c.end - c.at >= 4 && !memcmp(file->data + c.at, "SNOD", 4))
	{
		skip(&c, 6);
		entries = (int)take(&c, 2);
		for (i = 0; i < entries && !file->failed; i++)
		{
			uint64_t name = take_offset(&c), header = take_offset(&c);
			size_t length = strlen(search->name);

			skip(&c, 24);
			if (name < heap_size && heap_size - name > length && !memcmp(file->data + heap_data + name, search->name, length) &&
				file->data[heap_data + name + length] == 0)
			{
				search->found = header;
				return 1;
			}
		}
		return 0;
	}
	if (!signature(&c, "TREE"))
		return 0;
	if (take(&c, 1) != 0)
		return sofa_fail(file, "it is damaged (a group's B-tree of chunks)");
	level = (int)take(&c, 1);
	entries = (int)take(&c, 2);
	skip(&c, 2 * (uint64_t)file->offset_size);
	(void)level;
	for (i = 0; i < entries && !file->failed; i++)
	{
		uint64_t child;

		take_length(&c);
		child = take_offset(&c);
		if (file->failed)
			return 0;
		if (symbol_node(search, child, heap_data, heap_size, depth + 1))
			return 1;
	}
	return 0;
}

static int group_message(void *context, int type, struct cursor *message, int flags)
{
	struct link_search *search = context;
	struct hdf *file = search->file;

	(void)flags;
	if (type == 0x06)
		return link_message(search, message);
	if (type == 0x11)
	{
		uint64_t tree = take_offset(message), heap = take_offset(message), data, size;
		struct cursor h = cursor_address(file, heap);

		if (!signature(&h, "HEAP"))
			return 0;
		skip(&h, 4);
		size = take_length(&h);
		take_length(&h);
		data = take_offset(&h);
		if (file->failed || !address_valid(file, data) || size > file->size - (file->base + data))
			return sofa_fail(file, "it is damaged (a group's heap)");
		return symbol_node(search, tree, file->base + data, size, 0);
	}
	if (type == 0x02)
	{
		uint64_t heap, names;
		int link_flags;

		skip(message, 1);
		link_flags = (int)take(message, 1);
		if (link_flags & 1)
			skip(message, 8);
		heap = take_offset(message);
		names = take_offset(message);
		if (file->failed || !address_valid(file, heap))
			return 0;
		if (!heap_open(file, heap, &search->heap))
			return 0;
		/* (the names' B-tree records: a hash, then the heap id) */
		search->offset_in_record = 4;
		return btree2_each(file, names, dense_link, search);
	}
	return 0;
}

/* the object header address of the link name in the group at address; 0 if
there is none */
static uint64_t group_find(struct hdf *file, uint64_t group, const char *name)
{
	struct link_search search;

	memset(&search, 0, sizeof(search));
	search.file = file;
	search.name = name;
	if (each_message(file, group, group_message, &search) > 0 && !file->failed)
		return search.found ? search.found : 0;
	return 0;
}

/* ---------- datatypes, dataspaces, attributes */

struct datatype
{
	int class, size, big_endian, is_signed;
};

static int datatype_read(struct hdf *file, struct cursor *c, struct datatype *type)
{
	int first = (int)take(c, 1), bits = (int)take(c, 3);

	type->class = first & 0x0F;
	type->size = (int)take(c, 4);
	type->big_endian = bits & 1;
	type->is_signed = (bits >> 3) & 1;
	if (file->failed)
		return 0;
	if (type->class == 1 && (bits & 0x40))
		return sofa_fail(file, "it holds floats of VAX byte order, which are not read");
	return 1;
}

struct dataspace
{
	int rank;
	uint64_t dims[SOFA_RANK];
};

static int dataspace_read(struct hdf *file, struct cursor *c, struct dataspace *space)
{
	int version = (int)take(c, 1), i, kind = 1;

	space->rank = (int)take(c, 1);
	take(c, 1);
	if (version == 1)
		skip(c, 5);
	else if (version == 2)
		kind = (int)take(c, 1);
	else
		return sofa_fail(file, "it is damaged (a dataspace of version %d)", version);
	if (space->rank > SOFA_RANK)
		return sofa_fail(file, "it holds a dataset of %d dimensions", space->rank);
	for (i = 0; i < space->rank; i++)
		space->dims[i] = take_length(c);
	if (kind == 2)
		space->rank = -1; /* (null: no elements) */
	return !file->failed;
}

static uint64_t elements(const struct dataspace *space)
{
	uint64_t count = 1;
	int i;

	if (space->rank < 0)
		return 0;
	for (i = 0; i < space->rank; i++)
		count = space->dims[i] && count > ~(uint64_t)0 / space->dims[i] ? ~(uint64_t)0 : count * space->dims[i];
	return count;
}

struct attribute_search
{
	struct hdf *file;
	const char *name;
	char *text;
	size_t text_size;
	int found;
	struct heap heap;
};

/* an attribute message: if it is name's and a string, its text */
static int attribute_message(struct attribute_search *search, struct cursor *c)
{
	struct hdf *file = search->file;
	int version = (int)take(c, 1);
	uint64_t name_size, type_size, space_size, length;
	const uint8_t *name, *data;
	struct cursor part;
	struct datatype type;
	struct dataspace space;

	take(c, 1);
	name_size = take(c, 2);
	type_size = take(c, 2);
	space_size = take(c, 2);
	if (version == 3)
		take(c, 1);
	else if (version != 1 && version != 2)
		return sofa_fail(file, "it is damaged (an attribute of version %d)", version);
	name = take_bytes(c, version == 1 ? (name_size + 7) / 8 * 8 : name_size);
	if (!name)
		return 0;
	length = strlen(search->name);
	if (name_size != length + 1 || memcmp(name, search->name, (size_t)length))
		return 0;
	part = cursor_at(file, c->at, type_size);
	skip(c, version == 1 ? (type_size + 7) / 8 * 8 : type_size);
	if (!datatype_read(file, &part, &type))
		return 0;
	part = cursor_at(file, c->at, space_size);
	skip(c, version == 1 ? (space_size + 7) / 8 * 8 : space_size);
	if (!dataspace_read(file, &part, &space))
		return 0;
	if (type.class != 3 || elements(&space) != 1)
		return 0;
	data = take_bytes(c, (uint64_t)type.size);
	if (!data)
		return 0;
	length = (uint64_t)type.size < search->text_size - 1 ? (uint64_t)type.size : search->text_size - 1;
	memcpy(search->text, data, (size_t)length);
	search->text[length] = 0;
	search->found = 1;
	return 1;
}

static int dense_attribute(void *context, const uint8_t *record)
{
	struct attribute_search *search = context;
	struct cursor object;

	if (!heap_object(&search->heap, record, &object))
		return 0;
	return attribute_message(search, &object);
}

static int attribute_visit(void *context, int type, struct cursor *message, int flags)
{
	struct attribute_search *search = context;
	struct hdf *file = search->file;

	if (type == 0x0C && !(flags & 0x02))
		return attribute_message(search, message);
	if (type == 0x15)
	{
		uint64_t heap, names;
		int info_flags;

		skip(message, 1);
		info_flags = (int)take(message, 1);
		if (info_flags & 1)
			skip(message, 2);
		heap = take_offset(message);
		names = take_offset(message);
		if (file->failed || !address_valid(file, heap))
			return 0;
		if (!heap_open(file, heap, &search->heap))
			return 0;
		return btree2_each(file, names, dense_attribute, search);
	}
	return 0;
}

/* the string attribute name of the object at address, in text; 0 if it has
none (or it is not a string) */
static int attribute_string(struct hdf *file, uint64_t object, const char *name, char *text, size_t text_size)
{
	struct attribute_search search;

	memset(&search, 0, sizeof(search));
	search.file = file;
	search.name = name;
	search.text = text;
	search.text_size = text_size;
	text[0] = 0;
	each_message(file, object, attribute_visit, &search);
	return search.found && !file->failed;
}

/* ---------- datasets */

struct dataset
{
	struct hdf *file;
	struct datatype type;
	struct dataspace space;
	int have_type, have_space, layout, chunk_index, chunk_rank;
	uint64_t address, size, chunk_size_filtered;
	uint32_t chunk[SOFA_RANK + 1], chunk_mask;
	const uint8_t *compact;
	int filters;
	int filter_id[SOFA_FILTERS];
	uint32_t filter_value[SOFA_FILTERS];
};

static int dataset_message(void *context, int type, struct cursor *message, int flags)
{
	struct dataset *set = context;
	struct hdf *file = set->file;
	int version, i;

	if ((type == 0x01 || type == 0x03 || type == 0x08 || type == 0x0B) && (flags & 0x02))
		return sofa_fail(file, "it holds a shared message, which is not read");
	if (type == 0x01)
		set->have_space = dataspace_read(file, message, &set->space);
	else if (type == 0x03)
		set->have_type = datatype_read(file, message, &set->type);
	else if (type == 0x08)
	{
		version = (int)take(message, 1);
		if (version != 3 && version != 4)
			return sofa_fail(file, "it holds a dataset of layout version %d, which is not read", version);
		set->layout = (int)take(message, 1);
		if (set->layout == 0)
		{
			set->size = take(message, 2);
			set->compact = take_bytes(message, set->size);
		}
		else if (set->layout == 1)
		{
			set->address = take_offset(message);
			set->size = take_length(message);
		}
		else if (set->layout == 2 && version == 3)
		{
			set->chunk_rank = (int)take(message, 1);
			set->address = take_offset(message);
			if (set->chunk_rank < 1 || set->chunk_rank > SOFA_RANK + 1)
				return sofa_fail(file, "it is damaged (a chunk of %d dimensions)", set->chunk_rank);
			for (i = 0; i < set->chunk_rank; i++)
				set->chunk[i] = (uint32_t)take(message, 4);
			set->chunk_index = 0;
		}
		else if (set->layout == 2)
		{
			int layout_flags = (int)take(message, 1), bytes;

			set->chunk_rank = (int)take(message, 1);
			bytes = (int)take(message, 1);
			if (set->chunk_rank < 1 || set->chunk_rank > SOFA_RANK + 1 || bytes < 1 || bytes > 8)
				return sofa_fail(file, "it is damaged (a chunk of %d dimensions)", set->chunk_rank);
			for (i = 0; i < set->chunk_rank; i++)
				set->chunk[i] = (uint32_t)take(message, bytes);
			set->chunk_index = (int)take(message, 1);
			if (set->chunk_index == 1)
			{
				if (layout_flags & 0x02)
				{
					set->chunk_size_filtered = take_length(message);
					set->chunk_mask = (uint32_t)take(message, 4);
				}
				set->address = take_offset(message);
			}
			else if (set->chunk_index == 2)
				set->address = take_offset(message);
			else
				return sofa_fail(file, "it holds a dataset of chunk index %d, which is not read", set->chunk_index);
		}
		else
			return sofa_fail(file, "it holds a dataset of layout %d, which is not read", set->layout);
	}
	else if (type == 0x0B)
	{
		int count;

		version = (int)take(message, 1);
		count = (int)take(message, 1);
		if (version == 1)
			skip(message, 6);
		if (count > SOFA_FILTERS || (version != 1 && version != 2))
			return sofa_fail(file, "it is damaged (a filter pipeline)");
		set->filters = count;
		for (i = 0; i < count && !file->failed; i++)
		{
			int id = (int)take(message, 2), name_length = 0, values, v;

			if (version == 1 || id >= 256)
				name_length = (int)take(message, 2);
			take(message, 2);
			values = (int)take(message, 2);
			skip(message, version == 1 ? (uint64_t)(name_length + 7) / 8 * 8 : (uint64_t)name_length);
			set->filter_id[i] = id;
			set->filter_value[i] = 0;
			for (v = 0; v < values; v++)
			{
				uint32_t value = (uint32_t)take(message, 4);

				if (v == 0)
					set->filter_value[i] = value;
			}
			if (version == 1 && (values & 1))
				skip(message, 4);
			if (id != 1 && id != 2 && id != 3)
				return sofa_fail(file, "it is compressed with filter %d, which is not read (deflate, shuffle and "
					"Fletcher-32 are)", id);
		}
	}
	return file->failed ? -1 : 0;
}

/* a chunk's bytes (filtered: size of them) undone through the pipeline into
out (expected bytes) */
static int chunk_unfilter(struct dataset *set, const uint8_t *data, uint64_t size, uint32_t mask, uint8_t *out,
	uint64_t expected)
{
	struct hdf *file = set->file;
	uint8_t *buffer = malloc((size_t)(expected ? expected : 1)), *work = NULL;
	const uint8_t *now = data;
	uint64_t now_size = size;
	int i;

	if (!buffer)
		return sofa_fail(file, "there is not the memory to read it");
	for (i = set->filters - 1; i >= 0; i--)
	{
		if (mask & (1u << i))
			continue;
		if (set->filter_id[i] == 3)
		{
			if (now_size < 4)
				break;
			now_size -= 4;
		}
		else if (set->filter_id[i] == 1)
		{
			uLongf written = (uLongf)expected;
			uint8_t *into = (now == buffer) ? malloc((size_t)(expected ? expected : 1)) : buffer;

			if (!into)
				break;
			if (z_uncompress(into, &written, now, (uLong)now_size) != Z_OK)
			{
				if (into != buffer)
					free(into);
				free(buffer);
				free(work);
				return sofa_fail(file, "it is damaged (a chunk that does not inflate)");
			}
			if (into != buffer)
			{
				free(buffer);
				buffer = into;
			}
			now = buffer;
			now_size = written;
		}
		else if (set->filter_id[i] == 2)
		{
			uint64_t element = set->filter_value[i] ? set->filter_value[i] : (uint64_t)set->type.size, count, e, b;
			uint8_t *into;

			if (!element)
				break;
			count = now_size / element;
			into = malloc((size_t)(now_size ? now_size : 1));
			if (!into)
				break;
			for (b = 0; b < element; b++)
				for (e = 0; e < count; e++)
					into[e * element + b] = now[b * count + e];
			memcpy(into + count * element, now + count * element, (size_t)(now_size - count * element));
			free(work);
			work = into;
			now = work;
		}
	}
	if (i >= 0 || now_size != expected)
	{
		free(buffer);
		free(work);
		return sofa_fail(file, "it is damaged (a chunk of the wrong size)");
	}
	memcpy(out, now, (size_t)expected);
	free(buffer);
	free(work);
	return 1;
}

/* a chunk (at offsets, in elements) into the dataset's bytes (out) */
static int chunk_place(struct dataset *set, const uint8_t *data, uint64_t size, uint32_t mask, const uint64_t *offsets,
	uint8_t *out)
{
	int rank = set->space.rank, d;
	uint64_t element = (uint64_t)set->type.size, chunk_bytes = element, counter[SOFA_RANK], n, total;
	uint8_t *chunk;

	for (d = 0; d < rank; d++)
	{
		if (!set->chunk[d] || offsets[d] % set->chunk[d])
			return sofa_fail(set->file, "it is damaged (a chunk out of place)");
		chunk_bytes *= set->chunk[d];
	}
	if (chunk_bytes > (uint64_t)SOFA_LARGEST_FILE * 2)
		return sofa_fail(set->file, "it holds a chunk too large");
	chunk = malloc((size_t)(chunk_bytes ? chunk_bytes : 1));
	if (!chunk)
		return sofa_fail(set->file, "there is not the memory to read it");
	if (!chunk_unfilter(set, data, size, mask, chunk, chunk_bytes))
	{
		free(chunk);
		return 0;
	}
	/* (each element of the chunk within the dataset to its place) */
	total = chunk_bytes / (element ? element : 1);
	memset(counter, 0, sizeof(counter));
	for (n = 0; n < total; n++)
	{
		uint64_t at = 0;
		int inside = 1;

		for (d = 0; d < rank; d++)
		{
			uint64_t coordinate = offsets[d] + counter[d];

			if (coordinate >= set->space.dims[d])
				inside = 0;
			at = at * set->space.dims[d] + coordinate;
		}
		if (inside)
			memcpy(out + at * element, chunk + n * element, (size_t)element);
		for (d = rank - 1; d >= 0; d--)
		{
			if (++counter[d] < set->chunk[d])
				break;
			counter[d] = 0;
		}
	}
	free(chunk);
	return 1;
}

static int chunk_tree(struct dataset *set, uint64_t address, uint8_t *out, int depth)
{
	struct hdf *file = set->file;
	struct cursor c = cursor_address(file, address);
	int level, entries, i, d;

	if (depth > SOFA_DEPTH)
		return sofa_fail(file, "it is damaged (a chunks' B-tree too deep)");
	if (!signature(&c, "TREE"))
		return 0;
	if (take(&c, 1) != 1)
		return sofa_fail(file, "it is damaged (a chunks' B-tree of a group)");
	level = (int)take(&c, 1);
	entries = (int)take(&c, 2);
	skip(&c, 2 * (uint64_t)file->offset_size);
	for (i = 0; i < entries && !file->failed; i++)
	{
		uint64_t size = take(&c, 4), offsets[SOFA_RANK + 1], child;
		uint32_t mask = (uint32_t)take(&c, 4);

		for (d = 0; d < set->chunk_rank; d++)
			offsets[d] = take(&c, 8);
		child = take_offset(&c);
		if (file->failed)
			return 0;
		if (level > 0)
		{
			if (!chunk_tree(set, child, out, depth + 1))
				return 0;
		}
		else
		{
			struct cursor data = cursor_address(file, child);
			const uint8_t *bytes = take_bytes(&data, size);

			if (!bytes || !chunk_place(set, bytes, size, mask, offsets, out))
				return 0;
		}
	}
	return !file->failed;
}

/* the dataset name of the root group (at root) as doubles, its dimensions
in space; NULL if there is none (and the file is not failed) or it cannot be
read (failed) */
static double *dataset_read(struct hdf *file, uint64_t root, const char *name, struct dataspace *space,
	uint64_t largest)
{
	struct dataset set;
	uint64_t object = group_find(file, root, name), count, n, bytes;
	uint8_t *raw;
	double *values;
	int d;

	if (!object || file->failed)
		return NULL;
	memset(&set, 0, sizeof(set));
	set.file = file;
	set.layout = -1;
	each_message(file, object, dataset_message, &set);
	if (file->failed)
		return NULL;
	if (!set.have_type || !set.have_space || set.layout < 0 || set.space.rank < 0)
		return sofa_fail(file, "it is damaged (%s has no type, shape or layout)", name), NULL;
	if ((set.type.class != 0 && set.type.class != 1) || (set.type.class == 1 && set.type.size != 4 && set.type.size != 8) ||
		(set.type.class == 0 && set.type.size != 1 && set.type.size != 2 && set.type.size != 4 && set.type.size != 8))
		return sofa_fail(file, "%s is not of numbers", name), NULL;
	count = elements(&set.space);
	if (count > largest)
		return sofa_fail(file, "%s is too large (%llu values)", name, (unsigned long long)count), NULL;
	bytes = count * (uint64_t)set.type.size;
	raw = calloc((size_t)(bytes ? bytes : 1), 1);
	values = malloc((size_t)(count ? count : 1) * sizeof(double));
	if (!raw || !values)
	{
		free(raw);
		free(values);
		return sofa_fail(file, "there is not the memory to read it"), NULL;
	}
	if (set.layout == 0)
	{
		if (set.size < bytes || !set.compact)
			sofa_fail(file, "it is damaged (%s's data is short)", name);
		else
			memcpy(raw, set.compact, (size_t)bytes);
	}
	else if (set.layout == 1)
	{
		if (bytes && (!address_valid(file, set.address) || set.size < bytes ||
			bytes > file->size - (file->base + set.address)))
			sofa_fail(file, "it is damaged (%s's data is past its end)", name);
		else if (bytes)
			memcpy(raw, file->data + file->base + set.address, (size_t)bytes);
	}
	else
	{
		if (set.chunk_rank != set.space.rank + 1)
			sofa_fail(file, "it is damaged (%s's chunks are of %d dimensions)", name, set.chunk_rank);
		else if (set.chunk_index == 0)
		{
			if (address_valid(file, set.address))
				chunk_tree(&set, set.address, raw, 0);
		}
		else if (address_valid(file, set.address))
		{
			/* (a single chunk, the whole dataset; or implicit: each chunk
			in turn, unfiltered) */
			uint64_t chunk_bytes = (uint64_t)set.type.size, offsets[SOFA_RANK], chunks = 1, k;

			for (d = 0; d < set.space.rank; d++)
			{
				if (!set.chunk[d])
					break;
				chunk_bytes *= set.chunk[d];
				chunks *= (set.space.dims[d] + set.chunk[d] - 1) / set.chunk[d];
			}
			if (set.chunk_index == 1)
				chunks = 1;
			for (k = 0; k < chunks && !file->failed; k++)
			{
				uint64_t rest = k, size = set.chunk_index == 1 && set.chunk_size_filtered ? set.chunk_size_filtered : chunk_bytes;
				struct cursor data = cursor_at(file, file->base + set.address + k * chunk_bytes, size);
				const uint8_t *chunk_data = take_bytes(&data, size);

				for (d = set.space.rank - 1; d >= 0; d--)
				{
					uint64_t across = (set.space.dims[d] + set.chunk[d] - 1) / set.chunk[d];

					offsets[d] = rest % across * set.chunk[d];
					rest /= across;
				}
				if (chunk_data)
					chunk_place(&set, chunk_data, size, set.chunk_index == 1 ? set.chunk_mask : 0xFFFFFFFFu, offsets, raw);
			}
		}
	}
	if (file->failed)
	{
		free(raw);
		free(values);
		return NULL;
	}
	for (n = 0; n < count; n++)
	{
		const uint8_t *p = raw + n * (uint64_t)set.type.size;
		uint64_t bits = 0;
		int i, size = set.type.size;

		for (i = 0; i < size; i++)
			bits |= (uint64_t)p[set.type.big_endian ? size - 1 - i : i] << (8 * i);
		if (set.type.class == 1 && size == 4)
		{
			uint32_t word = (uint32_t)bits;
			float f;

			memcpy(&f, &word, 4);
			values[n] = f;
		}
		else if (set.type.class == 1)
		{
			double f;

			memcpy(&f, &bits, 8);
			values[n] = f;
		}
		else if (set.type.is_signed && size < 8 && (bits >> (size * 8 - 1)) & 1)
			values[n] = (double)(int64_t)(bits | (~(uint64_t)0 << (size * 8)));
		else
			values[n] = set.type.is_signed ? (double)(int64_t)bits : (double)bits;
	}
	free(raw);
	*space = set.space;
	return values;
}

/* ---------- the file */

static int superblock(struct hdf *file, uint64_t *root)
{
	static const uint8_t magic[8] = { 0x89, 'H', 'D', 'F', '\r', '\n', 0x1A, '\n' };
	uint64_t at;
	struct cursor c;
	int version;

	for (at = 0; at + 8 <= file->size; at = at ? at * 2 : 512)
		if (!memcmp(file->data + at, magic, 8))
			break;
	if (at + 8 > file->size)
		return sofa_fail(file, "it is not a SOFA file (not HDF5)");
	c = cursor_at(file, at + 8, file->size - (at + 8));
	version = (int)take(&c, 1);
	file->offset_size = 8;
	file->length_size = 8;
	if (version == 0 || version == 1)
	{
		skip(&c, 4);
		file->offset_size = (int)take(&c, 1);
		file->length_size = (int)take(&c, 1);
		skip(&c, 1 + 2 + 2 + 4 + (version == 1 ? 4 : 0));
	}
	else if (version == 2 || version == 3)
	{
		file->offset_size = (int)take(&c, 1);
		file->length_size = (int)take(&c, 1);
		skip(&c, 1);
	}
	else
		return sofa_fail(file, "it is HDF5 of an unknown superblock (version %d)", version);
	if ((file->offset_size != 2 && file->offset_size != 4 && file->offset_size != 8) ||
		(file->length_size != 2 && file->length_size != 4 && file->length_size != 8))
		return sofa_fail(file, "it is damaged (its superblock)");
	file->base = take_offset(&c);
	take_offset(&c); /* the free space's, or the superblock extension's */
	take_offset(&c); /* the end of the file */
	if (version < 2)
	{
		take_offset(&c); /* the driver's */
		take_offset(&c); /* the root's name */
	}
	*root = take_offset(&c);
	if (file->base >= file->size)
		file->base = at;
	return !file->failed;
}

void hrtf_sofa_free(struct hrtf_sofa *sofa)
{
	free(sofa->responses);
	free(sofa->directions);
	free(sofa->delays);
	memset(sofa, 0, sizeof(*sofa));
}

int hrtf_sofa_read(const char *path, struct hrtf_sofa *sofa, char *why, size_t why_size)
{
	struct hdf file;
	struct dataspace ir_space, rate_space, position_space, delay_space;
	double *ir = NULL, *rate = NULL, *position = NULL, *delay = NULL;
	uint8_t *data = NULL;
	uint64_t root = 0, m, measurements = 0, samples = 0, n;
	char text[64];
	FILE *stream;
	long size;
	int cartesian = 0;

	memset(sofa, 0, sizeof(*sofa));
	memset(&file, 0, sizeof(file));
	file.why = why;
	file.why_size = why_size;
	if (why && why_size)
		why[0] = 0;
	stream = fopen(path, "rb");
	if (!stream)
		return sofa_fail(&file, "it cannot be opened");
	if (fseek(stream, 0, SEEK_END) || (size = ftell(stream)) < 0 || fseek(stream, 0, SEEK_SET))
	{
		fclose(stream);
		return sofa_fail(&file, "it cannot be read");
	}
	if ((unsigned long)size > SOFA_LARGEST_FILE)
	{
		fclose(stream);
		return sofa_fail(&file, "it is too large (%ld bytes; at most %u)", size, SOFA_LARGEST_FILE);
	}
	data = malloc((size_t)size + 1);
	if (!data || fread(data, 1, (size_t)size, stream) != (size_t)size)
	{
		fclose(stream);
		free(data);
		return sofa_fail(&file, data ? "it cannot be read" : "there is not the memory to read it");
	}
	fclose(stream);
	file.data = data;
	file.size = (uint64_t)size;
	if (!superblock(&file, &root))
		goto done;
	if (attribute_string(&file, root, "SOFAConventions", text, sizeof(text)) && strcmp(text, "SimpleFreeFieldHRIR"))
	{
		sofa_fail(&file, "it is a SOFA file of %s, not SimpleFreeFieldHRIR", text);
		goto done;
	}
	file.failed = 0; /* (an unreadable SOFAConventions: the data says) */
	if (why && why_size)
		why[0] = 0;
	ir = dataset_read(&file, root, "Data.IR", &ir_space, SOFA_LARGEST_RESPONSES);
	if (!ir)
	{
		if (!file.failed)
			sofa_fail(&file, "it has no Data.IR (not of measured impulse responses)");
		goto done;
	}
	if (ir_space.rank != 3 || ir_space.dims[1] != 2 || ir_space.dims[0] < 1 || ir_space.dims[2] < 8)
	{
		sofa_fail(&file, "its Data.IR is not of two ears' responses (M x 2 x N, N at least 8)");
		goto done;
	}
	measurements = ir_space.dims[0];
	samples = ir_space.dims[2];
	rate = dataset_read(&file, root, "Data.SamplingRate", &rate_space, 1u << 20);
	if (!rate || elements(&rate_space) < 1 || !(rate[0] >= 8000.0 && rate[0] <= 768000.0))
	{
		sofa_fail(&file, "its Data.SamplingRate is missing or not a rate");
		goto done;
	}
	if (elements(&rate_space) > 1)
		for (m = 1; m < elements(&rate_space); m++)
			if (rate[m] != rate[0])
			{
				sofa_fail(&file, "its responses are of more than one rate");
				goto done;
			}
	position = dataset_read(&file, root, "SourcePosition", &position_space, 1u << 24);
	if (!position || position_space.rank != 2 || position_space.dims[1] != 3 || position_space.dims[0] != measurements)
	{
		sofa_fail(&file, "its SourcePosition is missing or not one direction a response");
		goto done;
	}
	{
		uint64_t object = group_find(&file, root, "SourcePosition");

		if (object && attribute_string(&file, object, "Type", text, sizeof(text)))
			cartesian = !strcmp(text, "cartesian");
		file.failed = 0;
		if (why && why_size)
			why[0] = 0;
	}
	delay = dataset_read(&file, root, "Data.Delay", &delay_space, 1u << 24);
	if (file.failed)
		goto done;
	if (delay && (delay_space.rank != 2 || delay_space.dims[1] != 2 ||
		(delay_space.dims[0] != 1 && delay_space.dims[0] != measurements)))
	{
		sofa_fail(&file, "its Data.Delay is not one delay an ear (I or M x 2)");
		goto done;
	}
	sofa->measurements = (unsigned long)measurements;
	sofa->samples = (unsigned long)samples;
	sofa->rate = rate[0];
	sofa->responses = malloc((size_t)(measurements * 2 * samples) * sizeof(float));
	sofa->directions = malloc((size_t)(measurements * 2) * sizeof(double));
	sofa->delays = calloc((size_t)(measurements * 2), sizeof(double));
	if (!sofa->responses || !sofa->directions || !sofa->delays)
	{
		sofa_fail(&file, "there is not the memory to read it");
		goto done;
	}
	for (n = 0; n < measurements * 2 * samples; n++)
		sofa->responses[n] = (float)ir[n];
	for (m = 0; m < measurements; m++)
	{
		const double *p = position + m * 3;

		if (cartesian)
		{
			sofa->directions[m * 2] = atan2(p[1], p[0]) * (180.0 / 3.14159265358979323846);
			sofa->directions[m * 2 + 1] = atan2(p[2], sqrt(p[0] * p[0] + p[1] * p[1])) * (180.0 / 3.14159265358979323846);
		}
		else
		{
			sofa->directions[m * 2] = p[0];
			sofa->directions[m * 2 + 1] = p[1];
		}
		if (delay)
		{
			const double *d = delay + (delay_space.dims[0] == 1 ? 0 : m * 2);

			sofa->delays[m * 2] = d[0];
			sofa->delays[m * 2 + 1] = d[1];
		}
	}
done:
	free(ir);
	free(rate);
	free(position);
	free(delay);
	free(data);
	if (file.failed)
	{
		hrtf_sofa_free(sofa);
		return 0;
	}
	return 1;
}
