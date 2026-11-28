/*
UE4_PAK.C

An Unreal Engine 4 pak and its cooked packages, read in place (ue4_pak.h).

The pak: a 44-byte footer at its end (magic, version 7, the index's offset
and size, its SHA-1), the index (the mount point, then each entry's name,
offset, size, uncompressed size, compression method, SHA-1, a block list
when compressed, an encrypted flag and a block size), and each entry's data
after a copy of its index record (53 bytes for one stored whole).

The packages are read as tools/mcc_preview_bake.py reads them. A static
mesh's LOD0 buffers are found by their headers rather than by walking every
property: the position buffer is `stride 12, count n, element 12, count n`;
the sections come right before it (a count, 7 int32s each, then
MaxDeviation); then the vertex buffer (strip flags, UV count, n, full
precision UVs, high precision tangents, the tangents, the UVs), the color
buffer and the index buffer (a 32-bit flag, then its bytes). The material
slots are the MaterialInterface object properties, in order.
*/

#include "ue4_pak.h"
#include "posix.h"
#include "memory/zlib/zlib.h"

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAK_MAGIC 0x5A6F12E1UL
#define PAK_VERSION 7
#define PAK_FOOTER_SIZE 44
#define PAK_ENTRY_HEADER_SIZE 53
#define PAK_MOST_INDEX_SIZE (256UL << 20)
#define PAK_MOST_ENTRY_SIZE (64UL << 20)
#define PAK_MOUNT_PREFIX "../../../"

struct pak_entry
{
	/* (into the names) */
	size_t name;
	unsigned long long offset;
	unsigned long long size;
};

struct ue4_pak
{
	int descriptor;
	long count;
	struct pak_entry *entries;
	char *names;
	unsigned long index_crc;
};

/* ---------- bytes */

static unsigned long u32_le(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static long i32_le(const unsigned char *bytes)
{
	unsigned long value = u32_le(bytes);

	return value & 0x80000000UL ? (long)(value - 0x100000000ULL) : (long)value;
}

static unsigned long long u64_le(const unsigned char *bytes)
{
	return (unsigned long long)u32_le(bytes) | (unsigned long long)u32_le(bytes + 4) << 32;
}

static double half_float(unsigned int half)
{
	int exponent = (int)(half >> 10 & 31);
	double mantissa = (double)(half & 1023);
	double value;

	if (exponent == 0)
		value = ldexp(mantissa, -24);
	else if (exponent == 31)
		value = mantissa ? NAN : INFINITY;
	else
		value = ldexp(mantissa + 1024.0, exponent - 25);
	return half & 0x8000 ? -value : value;
}

/* an FString at *at in data (size bytes) into text (UTF-16 kept to its low
bytes), *at moved past it; 0 if it runs off the end */
static int fstring(const unsigned char *data, size_t size, size_t *at, char *text, size_t text_size)
{
	long length;
	size_t bytes, index, out = 0;
	int wide;

	if (*at + 4 > size)
		return 0;
	length = i32_le(data + *at);
	*at += 4;
	wide = length < 0;
	if (wide)
		length = -length;
	if (length > 65536)
		return 0;
	bytes = (size_t)length * (wide ? 2 : 1);
	if (bytes > size - *at)
		return 0;
	for (index = 0; index < (size_t)length; index++)
	{
		unsigned char character = data[*at + index * (wide ? 2 : 1)];

		if (!character)
			break;
		if (text && out + 1 < text_size)
			text[out++] = (char)character;
	}
	if (text && text_size)
		text[out] = 0;
	*at += bytes;
	return 1;
}

static int read_at(int descriptor, void *buffer, size_t size, unsigned long long offset)
{
	unsigned char *cursor = buffer;

	while (size)
	{
		ssize_t got = pread(descriptor, cursor, size, (off_t)offset);

		if (got <= 0)
			return 0;
		cursor += got;
		size -= (size_t)got;
		offset += (unsigned long long)got;
	}
	return 1;
}

/* ---------- the pak */

struct ue4_pak *ue4_pak_open(const char *path, const char **reason)
{
	unsigned char footer[PAK_FOOTER_SIZE];
	unsigned char *index = NULL;
	struct ue4_pak *pak = calloc(1, sizeof(*pak));
	unsigned long long index_offset, index_size;
	off_t end;
	size_t at = 0, names_size = 0, names_room;
	char mount[512], name[1024];
	long count, entry;

	*reason = "it cannot be read";
	if (!pak)
		return NULL;
	pak->descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (pak->descriptor < 0)
		goto refused;
	end = lseek(pak->descriptor, 0, SEEK_END);
	if (end < PAK_FOOTER_SIZE || !read_at(pak->descriptor, footer, sizeof(footer), (unsigned long long)end - PAK_FOOTER_SIZE))
		goto refused;
	*reason = "it is not a version 7 Unreal pak";
	if (u32_le(footer) != PAK_MAGIC || u32_le(footer + 4) != PAK_VERSION)
		goto refused;
	index_offset = u64_le(footer + 8);
	index_size = u64_le(footer + 16);
	if (index_size < 8 || index_size > PAK_MOST_INDEX_SIZE || index_offset + index_size > (unsigned long long)end ||
		!(index = malloc((size_t)index_size)) || !read_at(pak->descriptor, index, (size_t)index_size, index_offset))
	{
		goto refused;
	}
	pak->index_crc = crc32(0, index, (uInt)index_size);
	*reason = "its index is not of the layout read";
	if (!fstring(index, (size_t)index_size, &at, mount, sizeof(mount)) || at + 4 > index_size)
		goto refused;
	count = i32_le(index + at);
	at += 4;
	if (count < 0 || count > 4000000)
		goto refused;
	if (!strncmp(mount, PAK_MOUNT_PREFIX, strlen(PAK_MOUNT_PREFIX)))
		memmove(mount, mount + strlen(PAK_MOUNT_PREFIX), strlen(mount + strlen(PAK_MOUNT_PREFIX)) + 1);
	names_room = (size_t)index_size + (size_t)count * (strlen(mount) + 1);
	pak->entries = calloc((size_t)count + 1, sizeof(*pak->entries));
	pak->names = malloc(names_room);
	if (!pak->entries || !pak->names)
		goto refused;
	for (entry = 0; entry < count; entry++)
	{
		unsigned long compression;
		size_t length;

		if (!fstring(index, (size_t)index_size, &at, name, sizeof(name)) || at + 48 + 5 > index_size)
			goto refused;
		pak->entries[entry].offset = u64_le(index + at);
		pak->entries[entry].size = u64_le(index + at + 8);
		compression = u32_le(index + at + 24);
		at += 28 + 20;
		if (compression)
		{
			long blocks = i32_le(index + at);

			if (blocks < 0 || at + 4 + 16 * (size_t)blocks > index_size)
				goto refused;
			at += 4 + 16 * (size_t)blocks;
			/* (a compressed entry is never read: its size says it cannot be) */
			pak->entries[entry].size = ~0ULL;
		}
		if (at + 5 > index_size)
			goto refused;
		if (index[at])
			pak->entries[entry].size = ~0ULL;
		at += 1 + 4;
		length = strlen(mount) + strlen(name);
		if (names_size + length + 1 > names_room)
			goto refused;
		pak->entries[entry].name = names_size;
		memcpy(pak->names + names_size, mount, strlen(mount));
		memcpy(pak->names + names_size + strlen(mount), name, strlen(name) + 1);
		names_size += length + 1;
	}
	pak->count = count;
	free(index);
	*reason = NULL;
	return pak;

refused:
	free(index);
	ue4_pak_close(pak);
	return NULL;
}

void ue4_pak_close(struct ue4_pak *pak)
{
	if (!pak)
		return;
	if (pak->descriptor >= 0)
		close(pak->descriptor);
	free(pak->entries);
	free(pak->names);
	free(pak);
}

long ue4_pak_count(const struct ue4_pak *pak)
{
	return pak->count;
}

unsigned long ue4_pak_index_crc(const struct ue4_pak *pak)
{
	return pak->index_crc;
}

const char *ue4_pak_name(const struct ue4_pak *pak, long entry)
{
	return entry >= 0 && entry < pak->count ? pak->names + pak->entries[entry].name : "";
}

long ue4_pak_find(const struct ue4_pak *pak, const char *name)
{
	long entry;

	for (entry = 0; entry < pak->count; entry++)
		if (!strcmp(pak->names + pak->entries[entry].name, name))
			return entry;
	return -1;
}

long ue4_pak_find_asset(const struct ue4_pak *pak, const char *root, const char *name)
{
	size_t root_length = strlen(root), name_length = strlen(name);
	long entry, found = -1;

	for (entry = 0; entry < pak->count; entry++)
	{
		const char *path = pak->names + pak->entries[entry].name;
		size_t length = strlen(path);

		/* (<root><name>.uasset at the least: in the root's own folder too) */
		if (length < root_length + name_length + 7 || strncmp(path, root, root_length) ||
			strcmp(path + length - 7, ".uasset") || path[length - 7 - name_length - 1] != '/' ||
			strncmp(path + length - 7 - name_length, name, name_length))
		{
			continue;
		}
		if (found >= 0)
			return -1;
		found = entry;
	}
	return found;
}

unsigned char *ue4_pak_read(const struct ue4_pak *pak, long entry, size_t *size)
{
	unsigned char *data;

	if (entry < 0 || entry >= pak->count || pak->entries[entry].size > PAK_MOST_ENTRY_SIZE)
		return NULL;
	*size = (size_t)pak->entries[entry].size;
	data = malloc(*size ? *size : 1);
	if (data && !read_at(pak->descriptor, data, *size, pak->entries[entry].offset + PAK_ENTRY_HEADER_SIZE))
	{
		free(data);
		data = NULL;
	}
	return data;
}

/* the entry beside the .uasset `asset` with another extension, or -1 */
static long sibling(const struct ue4_pak *pak, long asset, const char *extension)
{
	char name[1100];
	const char *path = ue4_pak_name(pak, asset);
	size_t length = strlen(path);

	if (length < 7 || length - 7 + strlen(extension) + 1 > sizeof(name))
		return -1;
	memcpy(name, path, length - 7);
	strcpy(name + length - 7, extension);
	return ue4_pak_find(pak, name);
}

/* ---------- packages */

int ue4_package_read(const unsigned char *uasset, size_t size, struct ue4_package *package)
{
	size_t at = 20, name_at;
	long custom, index;
	unsigned long flags;
	long import_offset;

	memset(package, 0, sizeof(*package));
	if (size < 64 || u32_le(uasset) != 0x9E2A83C1UL)
		return 0;
	custom = i32_le(uasset + at);
	if (custom < 0 || custom > 4096)
		return 0;
	at += 4 + 20 * (size_t)custom + 4;
	if (!fstring(uasset, size, &at, NULL, 0) || at + 12 > size)
		return 0;
	flags = u32_le(uasset + at);
	package->name_count = i32_le(uasset + at + 4);
	name_at = (size_t)i32_le(uasset + at + 8);
	at += 12;
	if (!(flags & 0x80000000UL) && !fstring(uasset, size, &at, NULL, 0))
		return 0;
	if (at + 24 > size)
		return 0;
	package->import_count = i32_le(uasset + at + 16);
	import_offset = i32_le(uasset + at + 20);
	if (package->name_count < 0 || package->name_count > 100000 || package->import_count < 0 ||
		package->import_count > 100000 || import_offset < 0 ||
		(size_t)import_offset + 28 * (size_t)package->import_count > size)
	{
		package->name_count = package->import_count = 0;
		return 0;
	}
	package->names = calloc((size_t)package->name_count + 1, sizeof(char *));
	package->import_class = calloc((size_t)package->import_count + 1, sizeof(long));
	package->import_object = calloc((size_t)package->import_count + 1, sizeof(long));
	if (!package->names || !package->import_class || !package->import_object)
		goto refused;
	at = name_at;
	for (index = 0; index < package->name_count; index++)
	{
		char name[256];

		if (!fstring(uasset, size, &at, name, sizeof(name)) || !(package->names[index] = strdup(name)))
			goto refused;
		at += 4;
	}
	for (index = 0; index < package->import_count; index++)
	{
		const unsigned char *row = uasset + import_offset + 28 * index;
		long class_name = i32_le(row + 8), object_name = i32_le(row + 20);

		if (class_name < 0 || class_name >= package->name_count || object_name < 0 ||
			object_name >= package->name_count)
		{
			goto refused;
		}
		package->import_class[index] = class_name;
		package->import_object[index] = object_name;
	}
	return 1;

refused:
	ue4_package_free(package);
	return 0;
}

void ue4_package_free(struct ue4_package *package)
{
	long index;

	if (package->names)
		for (index = 0; index < package->name_count; index++)
			free(package->names[index]);
	free(package->names);
	free(package->import_class);
	free(package->import_object);
	memset(package, 0, sizeof(*package));
}

long ue4_package_name(const struct ue4_package *package, const char *name)
{
	long index, found = -1;

	/* (the last of that name, as the prototype's table has it) */
	for (index = 0; index < package->name_count; index++)
		if (!strcmp(package->names[index], name))
			found = index;
	return found;
}

/* ---------- textures */

static const unsigned char *find_bytes(const unsigned char *data, size_t size, const void *what, size_t length)
{
	size_t at;

	for (at = 0; length <= size && at <= size - length; at++)
		if (data[at] == *(const unsigned char *)what && !memcmp(data + at, what, length))
			return data + at;
	return NULL;
}

int ue4_texture_read(const struct ue4_pak *pak, long asset, struct ue4_texture *texture, const char **reason)
{
	static const char *const names[] = { "PF_DXT1", "PF_DXT5", "PF_BC5" };
	static const int block_sizes[] = { 8, 16, 16 };
	unsigned char *uasset = NULL, *uexp = NULL, *bulk = NULL;
	size_t uasset_size = 0, uexp_size = 0, bulk_size = 0, top;
	unsigned char marker[16];
	const unsigned char *found;
	int format;
	long bulk_entry;

	memset(texture, 0, sizeof(*texture));
	*reason = "it cannot be read";
	uasset = ue4_pak_read(pak, asset, &uasset_size);
	uexp = ue4_pak_read(pak, sibling(pak, asset, ".uexp"), &uexp_size);
	if (!uasset || !uexp)
		goto finished;
	*reason = "not a DXT1, DXT5 or BC5 texture";
	for (format = 0; format < 3; format++)
		if (find_bytes(uasset, uasset_size, names[format], strlen(names[format])))
			break;
	if (format == 3)
		goto finished;
	/* the platform data: SizeX, SizeY, NumSlices, then the format's name as
	an FString */
	marker[0] = (unsigned char)(strlen(names[format]) + 1);
	marker[1] = marker[2] = marker[3] = 0;
	memcpy(marker + 4, names[format], strlen(names[format]) + 1);
	found = find_bytes(uexp, uexp_size, marker, 4 + strlen(names[format]) + 1);
	*reason = "no platform data";
	if (!found || found - uexp < 12)
		goto finished;
	texture->width = i32_le(found - 12);
	texture->height = i32_le(found - 8);
	texture->format = format;
	*reason = "an odd size";
	if (texture->width < 4 || texture->height < 4 || texture->width > 8192 || texture->height > 8192)
		goto finished;
	top = (size_t)(texture->width / 4) * (size_t)(texture->height / 4) * (size_t)block_sizes[format];
	bulk_entry = sibling(pak, asset, ".ubulk");
	*reason = "its top mip is not in its .ubulk";
	if (bulk_entry < 0 || !(bulk = ue4_pak_read(pak, bulk_entry, &bulk_size)) || bulk_size < top)
		goto finished;
	texture->blocks = bulk;
	texture->size = top;
	bulk = NULL;
	*reason = NULL;

finished:
	free(uasset);
	free(uexp);
	free(bulk);
	return *reason == NULL;
}

void ue4_texture_free(struct ue4_texture *texture)
{
	free(texture->blocks);
	memset(texture, 0, sizeof(*texture));
}

/* a BC3/BC4-style alpha block's 16 values */
static void alpha_block(const unsigned char *block, unsigned char out[16])
{
	unsigned int a0 = block[0], a1 = block[1], palette[8], step;
	unsigned long long bits = 0;
	int index;

	for (index = 0; index < 6; index++)
		bits |= (unsigned long long)block[2 + index] << (8 * index);
	palette[0] = a0;
	palette[1] = a1;
	for (step = 2; step < 8; step++)
	{
		if (a0 > a1)
			palette[step] = (unsigned int)floor(0.5 + ((8.0 - step) * a0 + (step - 1.0) * a1) / 7.0);
		else if (step < 6)
			palette[step] = (unsigned int)floor(0.5 + ((6.0 - step) * a0 + (step - 1.0) * a1) / 5.0);
		else
			palette[step] = step == 6 ? 0 : 255;
	}
	for (index = 0; index < 16; index++)
		out[index] = (unsigned char)palette[bits >> (3 * index) & 7];
}

static void color_block(const unsigned char *block, int four, unsigned char out[16][4])
{
	unsigned int c0 = block[0] | block[1] << 8, c1 = block[2] | block[3] << 8;
	unsigned long bits = u32_le(block + 4);
	double palette[4][4];
	int index, channel;

	for (index = 0; index < 2; index++)
	{
		unsigned int c = index ? c1 : c0, r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;

		palette[index][0] = (r << 3 | r >> 2);
		palette[index][1] = (g << 2 | g >> 4);
		palette[index][2] = (b << 3 | b >> 2);
		palette[index][3] = 255;
	}
	four = four || c0 > c1;
	for (channel = 0; channel < 3; channel++)
	{
		palette[2][channel] = four ? (2 * palette[0][channel] + palette[1][channel]) / 3 :
			(palette[0][channel] + palette[1][channel]) / 2;
		palette[3][channel] = four ? (palette[0][channel] + 2 * palette[1][channel]) / 3 : 0;
	}
	palette[2][3] = 255;
	palette[3][3] = four ? 255 : 0;
	for (index = 0; index < 16; index++)
		for (channel = 0; channel < 4; channel++)
			out[index][channel] = (unsigned char)floor(0.5 + palette[bits >> (2 * index) & 3][channel]);
}

unsigned char *ue4_texture_rgba(const struct ue4_texture *texture)
{
	long width = texture->width, height = texture->height, bx, by;
	unsigned char *rgba = malloc((size_t)width * (size_t)height * 4);
	int block_size = texture->format == _ue4_format_dxt1 ? 8 : 16;

	if (!rgba)
		return NULL;
	for (by = 0; by < height / 4; by++)
	{
		for (bx = 0; bx < width / 4; bx++)
		{
			const unsigned char *block = texture->blocks + ((size_t)by * (size_t)(width / 4) + (size_t)bx) * (size_t)block_size;
			unsigned char pixels[16][4], alpha[16], green[16];
			int index;

			if (texture->format == _ue4_format_bc5)
			{
				alpha_block(block, alpha);
				alpha_block(block + 8, green);
				for (index = 0; index < 16; index++)
				{
					pixels[index][0] = alpha[index];
					pixels[index][1] = green[index];
					pixels[index][2] = 0;
					pixels[index][3] = 255;
				}
			}
			else
			{
				color_block(block + block_size - 8, block_size == 16, pixels);
				if (block_size == 16)
				{
					alpha_block(block, alpha);
					for (index = 0; index < 16; index++)
						pixels[index][3] = alpha[index];
				}
			}
			for (index = 0; index < 16; index++)
				memcpy(rgba + (((size_t)(by * 4 + index / 4) * (size_t)width) + (size_t)(bx * 4 + index % 4)) * 4,
					pixels[index], 4);
		}
	}
	return rgba;
}

/* ---------- static meshes */

void ue4_mesh_free(struct ue4_mesh *mesh)
{
	free(mesh->positions);
	free(mesh->uvs);
	free(mesh->tangents);
	free(mesh->normals);
	free(mesh->signs);
	free(mesh->triangles);
	free(mesh->section_of);
	free(mesh->sections);
	memset(mesh, 0, sizeof(*mesh));
}

int ue4_mesh_read(const struct ue4_pak *pak, long asset, struct ue4_mesh *mesh, const char **reason)
{
	unsigned char *uasset = NULL, *uexp = NULL;
	size_t uasset_size = 0, uexp_size = 0, at, position_at = 0;
	struct ue4_package package;
	long vertex_count = 0, k, index, uv_count, full_uvs, element, count, wide, bytes;
	long section_k = 0;
	char materials[64][128];
	long material_count = 0;

	memset(mesh, 0, sizeof(*mesh));
	memset(&package, 0, sizeof(package));
	*reason = "it cannot be read";
	uasset = ue4_pak_read(pak, asset, &uasset_size);
	uexp = ue4_pak_read(pak, sibling(pak, asset, ".uexp"), &uexp_size);
	if (!uasset || !uexp || !ue4_package_read(uasset, uasset_size, &package))
		goto finished;
	*reason = "no position buffer";
	for (at = 0; at + 16 <= uexp_size; at++)
	{
		long stride = i32_le(uexp + at), n = i32_le(uexp + at + 4);

		if (stride == 12 && i32_le(uexp + at + 8) == 12 && i32_le(uexp + at + 12) == n && n > 2)
		{
			position_at = at;
			vertex_count = n;
			break;
		}
	}
	if (!vertex_count || (size_t)vertex_count > (uexp_size - position_at) / 12)
		goto finished;
	*reason = "no section list before the position buffer";
	for (k = 1; k < 64 && !section_k; k++)
	{
		long start = (long)position_at - 4 - 28 * k - 4;
		int good = 1;

		if (start < 0)
			break;
		if (i32_le(uexp + start) != k)
			continue;
		for (index = 0; index < k && good; index++)
		{
			const unsigned char *row = uexp + start + 4 + 28 * index;
			long minimum = i32_le(row + 12), maximum = i32_le(row + 16), collision = i32_le(row + 20),
				shadow = i32_le(row + 24);

			good = !(index == 0 && i32_le(row + 4) != 0) && minimum >= 0 && minimum <= maximum &&
				maximum < vertex_count && (collision == 0 || collision == 1) && (shadow == 0 || shadow == 1);
		}
		if (good)
			section_k = k;
	}
	if (!section_k)
		goto finished;
	mesh->section_count = section_k;
	mesh->sections = calloc((size_t)section_k, sizeof(*mesh->sections));
	mesh->vertex_count = vertex_count;
	mesh->positions = malloc(sizeof(double) * 3 * (size_t)vertex_count);
	mesh->uvs = malloc(sizeof(double) * 2 * (size_t)vertex_count);
	mesh->tangents = malloc(sizeof(double) * 3 * (size_t)vertex_count);
	mesh->normals = malloc(sizeof(double) * 3 * (size_t)vertex_count);
	mesh->signs = malloc(sizeof(double) * (size_t)vertex_count);
	*reason = "out of memory";
	if (!mesh->sections || !mesh->positions || !mesh->uvs || !mesh->tangents || !mesh->normals || !mesh->signs)
		goto finished;
	for (index = 0; index < section_k; index++)
	{
		const unsigned char *row = uexp + position_at - 4 - 28 * section_k + 28 * index;

		mesh->sections[index].slot = i32_le(row);
		mesh->sections[index].first_index = i32_le(row + 4);
		mesh->sections[index].triangle_count = i32_le(row + 8);
	}
	for (index = 0; index < 3 * vertex_count; index++)
	{
		union { unsigned long u; float f; } word;

		word.u = u32_le(uexp + position_at + 16 + 4 * index);
		mesh->positions[index] = word.f;
	}
	*reason = "its vertex buffer is not of the layout read";
	at = position_at + 16 + 12 * (size_t)vertex_count + 2;
	if (at + 24 > uexp_size)
		goto finished;
	uv_count = i32_le(uexp + at);
	full_uvs = i32_le(uexp + at + 8);
	if (i32_le(uexp + at + 4) != vertex_count || uv_count < 1 || uv_count > 8)
		goto finished;
	at += 16;
	element = i32_le(uexp + at);
	count = i32_le(uexp + at + 4);
	if (element != 8 || count != vertex_count || at + 8 + 8 * (size_t)vertex_count > uexp_size)
		goto finished;
	for (index = 0; index < vertex_count; index++)
	{
		const signed char *packed = (const signed char *)(uexp + at + 8 + 8 * index);
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			mesh->tangents[3 * index + axis] = packed[axis] / 127.0;
			mesh->normals[3 * index + axis] = packed[4 + axis] / 127.0;
		}
		mesh->signs[index] = packed[7] < 0 ? -1.0 : 1.0;
	}
	at += 8 + (size_t)element * (size_t)count;
	if (at + 8 > uexp_size)
		goto finished;
	element = i32_le(uexp + at);
	count = i32_le(uexp + at + 4);
	/* (the UVs follow as the prototype reads them: every vertex's channels
	together, whatever the buffer's element size says) */
	if (element < 0 || count < 0 || at + 8 + (size_t)element * (size_t)count > uexp_size ||
		(size_t)element * (size_t)count < (size_t)uv_count * (full_uvs ? 8 : 4) * (size_t)vertex_count)
	{
		goto finished;
	}
	for (index = 0; index < vertex_count; index++)
	{
		const unsigned char *uv = uexp + at + 8 + (size_t)uv_count * (full_uvs ? 8 : 4) * (size_t)index;
		int axis;

		for (axis = 0; axis < 2; axis++)
		{
			if (full_uvs)
			{
				union { unsigned long u; float f; } word;

				word.u = u32_le(uv + 4 * axis);
				mesh->uvs[2 * index + axis] = word.f;
			}
			else
			{
				mesh->uvs[2 * index + axis] = half_float(uv[2 * axis] | uv[2 * axis + 1] << 8);
			}
		}
	}
	at += 8 + (size_t)element * (size_t)count;
	/* the color buffer: strip flags, stride, count, then its bulk */
	at += 2 + 8;
	if (at + 8 > uexp_size)
		goto finished;
	element = i32_le(uexp + at);
	count = i32_le(uexp + at + 4);
	if (element < 0 || count < 0 || at + 8 + (size_t)element * (size_t)count + 12 > uexp_size)
		goto finished;
	at += 8 + (size_t)element * (size_t)count;
	*reason = "its index buffer is not of the layout read";
	wide = i32_le(uexp + at);
	bytes = i32_le(uexp + at + 8);
	if (bytes < 0 || at + 12 + (size_t)bytes > uexp_size || bytes % (wide ? 12 : 6))
		goto finished;
	mesh->triangle_count = bytes / (wide ? 12 : 6);
	mesh->triangles = malloc(sizeof(long) * 3 * (size_t)mesh->triangle_count + 1);
	mesh->section_of = malloc(sizeof(long) * (size_t)mesh->triangle_count + 1);
	if (!mesh->triangles || !mesh->section_of)
		goto finished;
	for (index = 0; index < 3 * mesh->triangle_count; index++)
	{
		const unsigned char *value = uexp + at + 12 + (wide ? 4 : 2) * index;

		mesh->triangles[index] = wide ? (long)u32_le(value) : (long)(value[0] | value[1] << 8);
		if (mesh->triangles[index] >= vertex_count)
			goto finished;
	}
	/* the material slots, in order: each StaticMaterial's MaterialInterface */
	{
		long interface = ue4_package_name(&package, "MaterialInterface");
		long object = ue4_package_name(&package, "ObjectProperty");
		unsigned char tag[16];

		memset(tag, 0, sizeof(tag));
		tag[0] = (unsigned char)interface;
		tag[1] = (unsigned char)(interface >> 8);
		tag[2] = (unsigned char)(interface >> 16);
		tag[8] = (unsigned char)object;
		tag[9] = (unsigned char)(object >> 8);
		tag[10] = (unsigned char)(object >> 16);
		for (at = 0; interface >= 0 && object >= 0 && at + 16 + 8 + 1 + 4 <= uexp_size && material_count < 64; at++)
		{
			long value;

			if (memcmp(uexp + at, tag, 16))
				continue;
			value = i32_le(uexp + at + 16 + 8 + 1);
			if (value < 0 && -value - 1 < package.import_count)
				snprintf(materials[material_count], sizeof(materials[0]), "%s",
					package.names[package.import_object[-value - 1]]);
			else
				snprintf(materials[material_count], sizeof(materials[0]), "export %ld", value);
			material_count++;
		}
	}
	for (index = 0; index < mesh->triangle_count; index++)
		mesh->section_of[index] = -1;
	for (index = 0; index < section_k; index++)
	{
		struct ue4_mesh_section *section = &mesh->sections[index];
		long triangle;

		for (triangle = section->first_index / 3;
			triangle < section->first_index / 3 + section->triangle_count && triangle < mesh->triangle_count; triangle++)
		{
			if (triangle >= 0)
				mesh->section_of[triangle] = index;
		}
		snprintf(section->material, sizeof(section->material), "%s",
			section->slot >= 0 && section->slot < material_count ? materials[section->slot] : "?");
	}
	*reason = NULL;

finished:
	if (*reason)
		ue4_mesh_free(mesh);
	ue4_package_free(&package);
	free(uasset);
	free(uexp);
	return *reason == NULL;
}
