/*
MCC_IMPORT_BAKE.C

The MCC import's texture bake (mcc_import_bake.h): tools/mcc_preview_bake.py
step for step, but for the fit, which is shipped (mcc_import.json) rather
than searched for, and the closest points, which are exact (a uniform grid
of MCC's triangles, searched only as far as "close") where the prototype's
k-d tree of samples is near enough.

- Our model: each region's first permutation at its highest detail, from
  the stock Xbox map the fit names (xbox_map_file.c); its parts' UVs scaled
  by the model's base map scales.
- Each bitmap: up to 4x its size (at most max_size); each texel's triangle
  and barycentrics (texel centers inside, plus a margin), its 3D point, the
  fit, the closest point on MCC's mesh.
- Diffuse: MCC's colors with each channel's mean and contrast set to ours
  over the texels baked, padded 8 texels out, DXT1 (DXT5 where ours has
  alpha that is used), mips to 1x1, a Halo PC bitmap tag file.
- Multipurpose (the Xbox's channel order): each of our masks with the fine
  detail of the MCC map that agrees with it best; change color ours.
- <bitmap>.normal.png and .orm.png beside the diffuse for the enhanced
  graphics (port/linux/game/mod_materials.c).
- A layout (mcc_import.json's "layouts": MCC's textures already in our
  bitmap's UV layout, as the Spartan's are): no mesh, MCC's texel at the same
  UV; the diffuse's channels matched to ours over our model's islands, our
  texels kept where MCC's color has its visor hole, our multipurpose map
  kept, the normal map's green flipped, the .orm.png from MCC's occlusion, its
  color's alpha (roughness) and metalness.
*/

#include "mcc_import_bake.h"
#include "ue4_pak.h"
#include "xbox_map_file.h"
#include "posix.h"
#include "memory/zlib/zlib.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mkdir(const char *path, unsigned int mode);

#define CUSTOMIZATION_ROOT "MCC/Content/UI/Features/Customization/"
#define MESH_ROOT CUSTOMIZATION_ROOT "Characters/H1/"
#define TEXTURE_ROOT CUSTOMIZATION_ROOT "SharedTextures/H1/"

#define MAXIMUM_PIECES 64
#define MAXIMUM_BITMAPS 32
#define MAXIMUM_SECTIONS 64
#define MAXIMUM_PATH 1100
#define PAD_STEPS 8
/* a run's least share of a bitmap's drawn texels that must be baked */
#define LEAST_COVERAGE 0.5

/* Xbox bitmap formats */
#define FORMAT_X8R8G8B8 10
#define FORMAT_A8R8G8B8 11
#define FORMAT_DXT1 14
#define FORMAT_DXT3 15
#define FORMAT_DXT5 16

#define GROUP_BITMAP 0x6269746DUL
#define GROUP_MODEL 0x6D6F6465UL
#define GROUP_SHADER_MODEL 0x736F736FUL

enum
{
	_role_diffuse,
	_role_multipurpose,
	_role_metallic,
	_role_roughness,
	_role_normal,
	_role_emissive,
	_role_ao,
	NUMBER_OF_ROLES
};

/* a texture's role by the end of its name (any case); none of them: the
diffuse. (-1: a role left out) */
static const struct
{
	const char *suffix;
	int role;
} role_suffixes[] =
{
	{ "_MULTIPURPOSE", _role_multipurpose }, { "_METALLIC", _role_metallic }, { "_ROUGHNESS", _role_roughness },
	{ "_NORMAL", _role_normal }, { "_NM", _role_normal }, { "_EMISSIVE", _role_emissive }, { "_AO", _role_ao },
	{ "_OPACITY", -1 },
};

/* ---------- small things */

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

static int i16_le(const unsigned char *bytes)
{
	int value = bytes[0] | bytes[1] << 8;

	return value >= 0x8000 ? value - 0x10000 : value;
}

static float f32_le(const unsigned char *bytes)
{
	union { unsigned long u; float f; } word;

	word.u = u32_le(bytes);
	return word.f;
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

static void put_be_float(unsigned char *bytes, float value)
{
	union { unsigned long u; float f; } word;

	word.f = value;
	put_be32(bytes, word.u & 0xFFFFFFFFUL);
}

static int toupper_ascii(int character)
{
	return character >= 'a' && character <= 'z' ? character - 32 : character;
}

static int strncasecmp_ascii(const char *a, const char *b, size_t length)
{
	size_t index;

	for (index = 0; index < length; index++)
	{
		int x = toupper_ascii((unsigned char)a[index]), y = toupper_ascii((unsigned char)b[index]);

		if (x != y)
			return x - y;
		if (!x)
			return 0;
	}
	return 0;
}

static int ends_with_case(const char *text, const char *suffix)
{
	size_t length = strlen(text), suffix_length = strlen(suffix), index;

	if (suffix_length > length)
		return 0;
	for (index = 0; index < suffix_length; index++)
		if (toupper_ascii(text[length - suffix_length + index]) != toupper_ascii(suffix[index]))
			return 0;
	return 1;
}

/* x rounded to the nearest whole number, halves to even (as NumPy's round) */
static double round_even(double x)
{
	double down = floor(x), part = x - down;

	if (part > 0.5)
		return down + 1;
	if (part < 0.5)
		return down;
	return fmod(down, 2.0) == 0 ? down : down + 1;
}

static double clampd(double value, double low, double high)
{
	return value < low ? low : value > high ? high : value;
}

static void unit3(double *v)
{
	double length = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length < 1e-9)
		length = 1e-9;
	v[0] /= length;
	v[1] /= length;
	v[2] /= length;
}

static double dot3(const double *a, const double *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void lower_into(char *out, size_t size, const char *in)
{
	size_t index;

	for (index = 0; in[index] && index + 1 < size; index++)
		out[index] = (char)(in[index] >= 'A' && in[index] <= 'Z' ? in[index] + 32 : in[index]);
	out[index] = 0;
}

/* every folder of `path` (a file's) made */
static void make_folders(const char *path)
{
	char folder[MAXIMUM_PATH];
	size_t index;

	snprintf(folder, sizeof(folder), "%s", path);
	for (index = 1; folder[index]; index++)
	{
		if (folder[index] == '/' || folder[index] == '\\')
		{
			char kept = folder[index];

			folder[index] = 0;
			mkdir(folder, 0755);
			folder[index] = kept;
		}
	}
}

/* a file written whole or not at all (a .new beside it, renamed over it) */
static int write_file(const char *path, const void *data, size_t size)
{
	char temporary[MAXIMUM_PATH + 8];
	FILE *file;
	int written;

	make_folders(path);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fwrite(data, 1, size, file) == size;
	written = fclose(file) == 0 && written;
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

/* the path of our tag's file (its backslashes slashes) under out/tags */
static void tag_path(char *path, size_t size, const char *out_folder, const char *tag, const char *extension)
{
	size_t index, length;

	snprintf(path, size, "%s/tags/", out_folder);
	length = strlen(path);
	for (index = 0; tag[index] && length + 1 < size; index++)
		path[length++] = tag[index] == '\\' ? '/' : tag[index];
	path[length] = 0;
	snprintf(path + length, size - length, "%s", extension);
}

/* ---------- a PNG (8-bit RGB), for the material maps */

/* (zlib's allocations with the C library's: off the game's thread) */
static voidpf png_zalloc(voidpf opaque, uInt items, uInt size)
{
	(void)opaque;
	return calloc(items, size);
}

static void png_zfree(voidpf opaque, voidpf address)
{
	(void)opaque;
	free(address);
}

static void png_chunk(unsigned char *out, size_t *at, const char *type, const unsigned char *data, size_t size)
{
	unsigned long crc;

	put_be32(out + *at, (unsigned long)size);
	memcpy(out + *at + 4, type, 4);
	if (size)
		memcpy(out + *at + 8, data, size);
	crc = crc32(0, out + *at + 4, (uInt)(size + 4));
	put_be32(out + *at + 8 + size, crc);
	*at += 12 + size;
}

static int png_write(const char *path, const unsigned char *rgb, long width, long height)
{
	size_t raw_size = (size_t)height * (size_t)(width * 3 + 1);
	unsigned long packed_size = (unsigned long)(raw_size + raw_size / 1000 + 64);
	unsigned char *raw = malloc(raw_size), *out = malloc(packed_size + 128);
	unsigned char header[13];
	size_t at = 8;
	long y;
	int written = 0;

	if (!raw || !out)
		goto finished;
	for (y = 0; y < height; y++)
	{
		raw[(size_t)y * (size_t)(width * 3 + 1)] = 0;
		memcpy(raw + (size_t)y * (size_t)(width * 3 + 1) + 1, rgb + (size_t)y * (size_t)width * 3, (size_t)width * 3);
	}
	memcpy(out, "\x89PNG\r\n\x1a\n", 8);
	put_be32(header, (unsigned long)width);
	put_be32(header + 4, (unsigned long)height);
	header[8] = 8;
	header[9] = 2;
	header[10] = header[11] = header[12] = 0;
	png_chunk(out, &at, "IHDR", header, sizeof(header));
	{
		z_stream stream;
		int result;

		memset(&stream, 0, sizeof(stream));
		stream.zalloc = png_zalloc;
		stream.zfree = png_zfree;
		if (deflateInit(&stream, 6) != Z_OK)
			goto finished;
		stream.next_in = (Bytef *)raw;
		stream.avail_in = (uInt)raw_size;
		stream.next_out = (Bytef *)(out + at + 8);
		stream.avail_out = (uInt)packed_size;
		result = deflate(&stream, Z_FINISH);
		packed_size = stream.total_out;
		deflateEnd(&stream);
		if (result != Z_STREAM_END)
			goto finished;
	}
	/* (compressed in place: the chunk's header and CRC around it) */
	{
		unsigned long crc;

		put_be32(out + at, (unsigned long)packed_size);
		memcpy(out + at + 4, "IDAT", 4);
		crc = crc32(0, out + at + 4, (uInt)(packed_size + 4));
		put_be32(out + at + 8 + packed_size, crc);
		at += 12 + packed_size;
	}
	png_chunk(out, &at, "IEND", NULL, 0);
	written = write_file(path, out, at);

finished:
	free(raw);
	free(out);
	return written;
}

/* ---------- DXT: decoding ours, encoding the bake */

static void rgb565(unsigned int color, float out[3])
{
	unsigned int r = color >> 11 & 31, g = color >> 5 & 63, b = color & 31;

	out[0] = (float)(r << 3 | r >> 2);
	out[1] = (float)(g << 2 | g >> 4);
	out[2] = (float)(b << 3 | b >> 2);
}

/* DXT1, DXT3 or DXT5 blocks as RGBA floats of 0 to 255 (width x height x 4) */
static void dxt_decode(const unsigned char *data, long width, long height, int format, float *out)
{
	int block_size = format == FORMAT_DXT1 ? 8 : 16;
	long bx, by;

	for (by = 0; by < height / 4; by++)
	{
		for (bx = 0; bx < width / 4; bx++)
		{
			const unsigned char *block = data + ((size_t)by * (size_t)(width / 4) + (size_t)bx) * (size_t)block_size;
			const unsigned char *color = block + block_size - 8;
			unsigned int c0 = color[0] | color[1] << 8, c1 = color[2] | color[3] << 8;
			unsigned long bits = u32_le(color + 4);
			float palette[4][4];
			int four = c0 > c1 || block_size == 16, index, channel;
			float alpha[16];

			rgb565(c0, palette[0]);
			rgb565(c1, palette[1]);
			for (channel = 0; channel < 3; channel++)
			{
				palette[2][channel] = four ? (2 * palette[0][channel] + palette[1][channel]) / 3 :
					(palette[0][channel] + palette[1][channel]) / 2;
				palette[3][channel] = four ? (palette[0][channel] + 2 * palette[1][channel]) / 3 : 0;
			}
			palette[0][3] = palette[1][3] = palette[2][3] = 255;
			palette[3][3] = four ? 255 : 0;
			for (index = 0; index < 16; index++)
				alpha[index] = palette[bits >> (2 * index) & 3][3];
			if (format == FORMAT_DXT3)
			{
				for (index = 0; index < 16; index++)
					alpha[index] = (float)((block[index / 2] >> (4 * (index & 1))) & 15) * 17;
			}
			else if (format == FORMAT_DXT5)
			{
				float a0 = block[0], a1 = block[1], apal[8];
				unsigned long long abits = 0;
				int step;

				for (index = 0; index < 6; index++)
					abits |= (unsigned long long)block[2 + index] << (8 * index);
				apal[0] = a0;
				apal[1] = a1;
				for (step = 2; step < 8; step++)
				{
					if (a0 > a1)
						apal[step] = ((8 - step) * a0 + (step - 1) * a1) / 7;
					else
						apal[step] = step == 6 ? 0 : step == 7 ? 255 : ((6 - step) * a0 + (step - 1) * a1) / 5;
				}
				for (index = 0; index < 16; index++)
					alpha[index] = apal[abits >> (3 * index) & 7];
			}
			for (index = 0; index < 16; index++)
			{
				float *pixel = out + (((size_t)(by * 4 + index / 4) * (size_t)width) + (size_t)(bx * 4 + index % 4)) * 4;
				const float *entry = palette[bits >> (2 * index) & 3];

				pixel[0] = entry[0];
				pixel[1] = entry[1];
				pixel[2] = entry[2];
				pixel[3] = alpha[index];
			}
		}
	}
}

static unsigned int quantize565(const float color[3])
{
	float r = color[0] < 0 ? 0 : color[0] > 255 ? 255 : color[0];
	float g = color[1] < 0 ? 0 : color[1] > 255 ? 255 : color[1];
	float b = color[2] < 0 ? 0 : color[2] > 255 ? 255 : color[2];

	return (unsigned int)(float)round_even(r * 31 / 255) << 11 | (unsigned int)(float)round_even(g * 63 / 255) << 5 |
		(unsigned int)(float)round_even(b * 31 / 255);
}

static const float dxt_weights[4] = { 0.0f, 1.0f, 1.0f / 3.0f, 2.0f / 3.0f };

/* endpoints quantized (the larger first), each pixel's index and the error */
static float dxt_fit(const float pixels[16][3], const float e0[3], const float e1[3], unsigned int *q0,
	unsigned int *q1, int index[16])
{
	float p0[3], p1[3], palette[4][3], error = 0;
	int k, i, channel;

	*q0 = quantize565(e0);
	*q1 = quantize565(e1);
	if (*q0 < *q1)
	{
		unsigned int swap = *q0;

		*q0 = *q1;
		*q1 = swap;
	}
	rgb565(*q0, p0);
	rgb565(*q1, p1);
	for (i = 0; i < 4; i++)
		for (channel = 0; channel < 3; channel++)
			palette[i][channel] = p0[channel] * (1 - dxt_weights[i]) + p1[channel] * dxt_weights[i];
	for (k = 0; k < 16; k++)
	{
		float best = 0;
		int best_index = 0;

		for (i = 0; i < 4; i++)
		{
			float distance = 0;

			for (channel = 0; channel < 3; channel++)
				distance += (pixels[k][channel] - palette[i][channel]) * (pixels[k][channel] - palette[i][channel]);
			if (i == 0 || distance < best)
			{
				best = distance;
				best_index = i;
			}
		}
		if (*q0 == *q1)
		{
			float distance = 0;

			for (channel = 0; channel < 3; channel++)
				distance += (pixels[k][channel] - palette[0][channel]) * (pixels[k][channel] - palette[0][channel]);
			best = distance;
			best_index = 0;
		}
		index[k] = best_index;
		error += best;
	}
	return error;
}

/* a 4x4 block of RGB(A) floats (16 x 4) as DXT1's 8 bytes: its endpoints
along its colors' main axis, refined once by least squares */
static void dxt1_block(const float rgba[16][4], unsigned char out[8])
{
	float pixels[16][3], mean[3] = { 0, 0, 0 }, covariance[3][3], axis[3] = { 1, 1, 1 }, lo = 0, hi = 0;
	float e0[3], e1[3], r0[3], r1[3], error, error_b;
	unsigned int q0, q1, q0b, q1b;
	int index[16], index_b[16], k, i, j, iteration;
	unsigned long bits = 0;
	float aa = 0, ab = 0, bb = 0, ax[3] = { 0, 0, 0 }, bx[3] = { 0, 0, 0 }, det;

	for (k = 0; k < 16; k++)
		for (i = 0; i < 3; i++)
		{
			pixels[k][i] = rgba[k][i];
			mean[i] += rgba[k][i];
		}
	for (i = 0; i < 3; i++)
		mean[i] /= 16;
	for (i = 0; i < 3; i++)
		for (j = 0; j < 3; j++)
		{
			covariance[i][j] = 0;
			for (k = 0; k < 16; k++)
				covariance[i][j] += (pixels[k][i] - mean[i]) * (pixels[k][j] - mean[j]);
		}
	for (iteration = 0; iteration < 6; iteration++)
	{
		float next[3], length;

		for (i = 0; i < 3; i++)
			next[i] = covariance[i][0] * axis[0] + covariance[i][1] * axis[1] + covariance[i][2] * axis[2];
		length = sqrtf(next[0] * next[0] + next[1] * next[1] + next[2] * next[2]);
		if (length < 1e-6f)
			length = 1e-6f;
		for (i = 0; i < 3; i++)
			axis[i] = next[i] / length;
	}
	for (k = 0; k < 16; k++)
	{
		float t = (pixels[k][0] - mean[0]) * axis[0] + (pixels[k][1] - mean[1]) * axis[1] +
			(pixels[k][2] - mean[2]) * axis[2];

		if (k == 0 || t < lo)
			lo = t;
		if (k == 0 || t > hi)
			hi = t;
	}
	for (i = 0; i < 3; i++)
	{
		e0[i] = mean[i] + hi * axis[i];
		e1[i] = mean[i] + lo * axis[i];
	}
	error = dxt_fit(pixels, e0, e1, &q0, &q1, index);
	for (k = 0; k < 16; k++)
	{
		float w = dxt_weights[index[k]], a = 1 - w;

		aa += a * a;
		ab += a * w;
		bb += w * w;
		for (i = 0; i < 3; i++)
		{
			ax[i] += a * pixels[k][i];
			bx[i] += w * pixels[k][i];
		}
	}
	det = aa * bb - ab * ab;
	for (i = 0; i < 3; i++)
	{
		if (fabsf(det) > 1e-6f)
		{
			r0[i] = (bb * ax[i] - ab * bx[i]) / det;
			r1[i] = (aa * bx[i] - ab * ax[i]) / det;
		}
		else
		{
			r0[i] = e0[i];
			r1[i] = e1[i];
		}
	}
	error_b = dxt_fit(pixels, r0, r1, &q0b, &q1b, index_b);
	if (error_b < error)
	{
		q0 = q0b;
		q1 = q1b;
		memcpy(index, index_b, sizeof(index));
	}
	for (k = 0; k < 16; k++)
		bits |= (unsigned long)index[k] << (2 * k);
	out[0] = (unsigned char)q0;
	out[1] = (unsigned char)(q0 >> 8);
	out[2] = (unsigned char)q1;
	out[3] = (unsigned char)(q1 >> 8);
	out[4] = (unsigned char)bits;
	out[5] = (unsigned char)(bits >> 8);
	out[6] = (unsigned char)(bits >> 16);
	out[7] = (unsigned char)(bits >> 24);
}

/* DXT5's alpha block: between the block's smallest and largest, 8 steps */
static void dxt5_alpha_block(const float rgba[16][4], unsigned char out[8])
{
	float alpha[16], a0 = 0, a1 = 255, palette[8];
	unsigned long long bits = 0;
	int k, step;

	for (k = 0; k < 16; k++)
	{
		alpha[k] = (float)round_even(rgba[k][3]);
		alpha[k] = alpha[k] < 0 ? 0 : alpha[k] > 255 ? 255 : alpha[k];
		if (alpha[k] > a0)
			a0 = alpha[k];
		if (alpha[k] < a1)
			a1 = alpha[k];
	}
	for (step = 0; step < 8; step++)
		palette[step] = step == 0 ? a0 : step == 1 ? a1 : ((8 - step) * a0 + (step - 1) * a1) / 7;
	for (k = 0; k < 16; k++)
	{
		int best = 0;

		for (step = 1; step < 8 && a0 != a1; step++)
			if (fabsf(alpha[k] - palette[step]) < fabsf(alpha[k] - palette[best]))
				best = step;
		bits |= (unsigned long long)best << (3 * k);
	}
	out[0] = (unsigned char)a0;
	out[1] = (unsigned char)a1;
	for (k = 0; k < 6; k++)
		out[2 + k] = (unsigned char)(bits >> (8 * k));
}

/* an RGBA float image (both sides multiples of 4) as DXT1 or DXT5 blocks into out */
static void dxt_encode(const float *image, long width, long height, int dxt5, unsigned char *out)
{
	long bx, by;
	int block_size = dxt5 ? 16 : 8;

	for (by = 0; by < height / 4; by++)
	{
		for (bx = 0; bx < width / 4; bx++)
		{
			float block[16][4];
			unsigned char *at = out + ((size_t)by * (size_t)(width / 4) + (size_t)bx) * (size_t)block_size;
			int k;

			for (k = 0; k < 16; k++)
				memcpy(block[k], image + (((size_t)(by * 4 + k / 4) * (size_t)width) + (size_t)(bx * 4 + k % 4)) * 4,
					sizeof(block[k]));
			if (dxt5)
			{
				dxt5_alpha_block(block, at);
				dxt1_block(block, at + 8);
			}
			else
			{
				dxt1_block(block, at);
			}
		}
	}
}

/* ---------- our side: an Xbox map's tags */

struct xbox_map
{
	char name[64];
	unsigned char *tags;
	unsigned long size;
	char path[MAXIMUM_PATH];
};

static const unsigned char *tags_at(const struct xbox_map *map, unsigned long address, unsigned long size)
{
	return xbox_map_tags_at(map->tags, map->size, address, size);
}

/* the instance's group, name and definition; 0 outside the tags */
static int instance_get(const struct xbox_map *map, long index, unsigned long *group, const char **name,
	unsigned long *definition)
{
	unsigned long instances = u32_le(map->tags), count = u32_le(map->tags + 0xC);
	const unsigned char *instance;
	const unsigned char *text;

	if (index < 0 || (unsigned long)index >= count ||
		!(instance = tags_at(map, instances + (unsigned long)index * 0x20, 0x20)))
	{
		return 0;
	}
	*group = u32_le(instance);
	*definition = u32_le(instance + 0x14);
	text = tags_at(map, u32_le(instance + 0x10), 1);
	*name = text && memchr(text, 0, map->size - (unsigned long)(text - map->tags)) ? (const char *)text : "";
	return 1;
}

static long instance_count(const struct xbox_map *map)
{
	return (long)u32_le(map->tags + 0xC);
}

/* a tag block at definition + offset: its count and its first element's address */
static long block_get(const struct xbox_map *map, unsigned long address, unsigned long offset, unsigned long size,
	unsigned long *elements)
{
	const unsigned char *field = tags_at(map, address + offset, 8);
	long count;

	if (!field)
		return 0;
	count = i32_le(field);
	*elements = u32_le(field + 4);
	if (count < 0 || count > 65536 || (count && !tags_at(map, *elements, (unsigned long)count * size)))
		return 0;
	return count;
}

/* the tag index a tag reference at definition + offset names, or -1 */
static long reference_get(const struct xbox_map *map, unsigned long address, unsigned long offset)
{
	const unsigned char *field = tags_at(map, address + offset + 0xC, 4);
	long index;

	if (!field)
		return -1;
	index = i32_le(field);
	return index == -1 ? -1 : index & 0xFFFF;
}

/* ---------- our side: a model's parts */

struct piece
{
	/* its shader_model's base map's and multipurpose map's tag indexes (-1: none) */
	long base;
	long multipurpose;
	long vertex_count;
	double *uv;
	double *xyz;
	double *normals;
	long triangle_count;
	long *triangles;
	/* the fit (into MCC's space) */
	double scale;
	double rotation[9];
	double shift[3];
	double near;
	/* the map it is from (the index into the run's maps) */
	int map;
};

static void piece_free(struct piece *piece)
{
	free(piece->uv);
	free(piece->xyz);
	free(piece->normals);
	free(piece->triangles);
	memset(piece, 0, sizeof(*piece));
}

/* a part's geometry into piece (its texcoords not yet scaled); 0 if it has none */
static int part_geometry(const struct xbox_map *map, unsigned long part, struct piece *piece)
{
	const unsigned char *field = tags_at(map, part, 0x68), *hardware, *raw, *index_bytes;
	unsigned long vertices, indices, index_total;
	long vertex_type, vertex_count, triangle_type, index_count, vertex_size, k, kept = 0;

	if (!field)
		return 0;
	vertex_type = i16_le(field + 0x54);
	vertex_count = i32_le(field + 0x58);
	triangle_type = i16_le(field + 0x44);
	index_count = i32_le(field + 0x48);
	if (!u32_le(field + 0x64) || !u32_le(field + 0x50) || vertex_count <= 0 || vertex_count > 65536 ||
		index_count <= 0 || index_count > 1000000)
	{
		return 0;
	}
	if (!(hardware = tags_at(map, u32_le(field + 0x64), 8)))
		return 0;
	vertices = u32_le(hardware + 4) | 0x80000000UL;
	if (!(hardware = tags_at(map, u32_le(field + 0x50), 8)))
		return 0;
	indices = u32_le(hardware + 4) | 0x80000000UL;
	vertex_size = vertex_type == 5 ? 32 : vertex_type == 4 ? 68 : 0;
	index_total = triangle_type == 0 ? 3UL * (unsigned long)index_count : (unsigned long)index_count + 2;
	if (!vertex_size || !(raw = tags_at(map, vertices, (unsigned long)(vertex_count * vertex_size))) ||
		!(index_bytes = tags_at(map, indices, index_total * 2)))
	{
		return 0;
	}
	piece->vertex_count = vertex_count;
	piece->uv = malloc(sizeof(double) * 2 * (size_t)vertex_count);
	piece->xyz = malloc(sizeof(double) * 3 * (size_t)vertex_count);
	piece->normals = malloc(sizeof(double) * 3 * (size_t)vertex_count);
	piece->triangles = malloc(sizeof(long) * 3 * (index_total + 1));
	if (!piece->uv || !piece->xyz || !piece->normals || !piece->triangles)
		return 0;
	for (k = 0; k < vertex_count; k++)
	{
		const unsigned char *vertex = raw + k * vertex_size;
		int axis;

		for (axis = 0; axis < 3; axis++)
			piece->xyz[3 * k + axis] = f32_le(vertex + 4 * axis);
		if (vertex_type == 5)
		{
			unsigned long packed = u32_le(vertex + 12);
			long x = (long)(packed & 0x7FF), y = (long)(packed >> 11 & 0x7FF), z = (long)(packed >> 22 & 0x3FF);

			x = x >= 0x400 ? x - 0x800 : x;
			y = y >= 0x400 ? y - 0x800 : y;
			z = z >= 0x200 ? z - 0x400 : z;
			piece->normals[3 * k] = (float)(x / 1023.0);
			piece->normals[3 * k + 1] = (float)(y / 1023.0);
			piece->normals[3 * k + 2] = (float)(z / 511.0);
			piece->uv[2 * k] = (float)((i16_le(vertex + 24) * 2.0f + 1.0f) / 65535.0f);
			piece->uv[2 * k + 1] = (float)((i16_le(vertex + 26) * 2.0f + 1.0f) / 65535.0f);
		}
		else
		{
			for (axis = 0; axis < 3; axis++)
				piece->normals[3 * k + axis] = f32_le(vertex + 12 + 4 * axis);
			piece->uv[2 * k] = f32_le(vertex + 48);
			piece->uv[2 * k + 1] = f32_le(vertex + 52);
		}
		/* (the prototype's float32 normals, normalized) */
		{
			double *n = piece->normals + 3 * k;
			float length = sqrtf((float)(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]));

			if (length < 1e-6f)
				length = 1e-6f;
			n[0] = (float)(n[0] / length);
			n[1] = (float)(n[1] / length);
			n[2] = (float)(n[2] / length);
		}
	}
	for (k = 0; k + 2 < (long)index_total; k += triangle_type == 0 ? 3 : 1)
	{
		long a = index_bytes[2 * k] | index_bytes[2 * k + 1] << 8;
		long b = index_bytes[2 * k + 2] | index_bytes[2 * k + 3] << 8;
		long c = index_bytes[2 * k + 4] | index_bytes[2 * k + 5] << 8;

		if (a == b || b == c || a == c || a >= vertex_count || b >= vertex_count || c >= vertex_count)
			continue;
		piece->triangles[3 * kept] = a;
		piece->triangles[3 * kept + 1] = b;
		piece->triangles[3 * kept + 2] = c;
		kept++;
	}
	piece->triangle_count = kept;
	return kept > 0;
}

/* the model's parts (each region's first permutation at its highest
detail) appended to pieces; how many */
static int model_pieces(const struct xbox_map *map, unsigned long address, struct piece *pieces, int room)
{
	const unsigned char *root = tags_at(map, address, 0xE8);
	unsigned long shaders_at, geometries_at, regions_at;
	long shader_count, geometry_count, region_count, region, geometry;
	long bases[256], multipurposes[256];
	unsigned char wanted[1024];
	float u_scale, v_scale;
	int count = 0;

	if (!root)
		return 0;
	u_scale = f32_le(root + 0x30);
	v_scale = f32_le(root + 0x34);
	u_scale = u_scale ? u_scale : 1.0f;
	v_scale = v_scale ? v_scale : 1.0f;
	shader_count = block_get(map, address, 0xDC, 0x20, &shaders_at);
	if (shader_count > 256)
		return 0;
	for (region = 0; region < shader_count; region++)
	{
		long tag = reference_get(map, shaders_at + (unsigned long)region * 0x20, 0);
		unsigned long group = 0, definition = 0;
		const char *name;

		bases[region] = multipurposes[region] = -1;
		if (tag >= 0 && instance_get(map, tag, &group, &name, &definition) && group == GROUP_SHADER_MODEL)
		{
			bases[region] = reference_get(map, definition, 0xA4);
			multipurposes[region] = reference_get(map, definition, 0xBC);
		}
	}
	geometry_count = block_get(map, address, 0xD0, 0x30, &geometries_at);
	if (geometry_count > (long)sizeof(wanted))
		return 0;
	memset(wanted, 0, sizeof(wanted));
	region_count = block_get(map, address, 0xC4, 0x4C, &regions_at);
	for (region = 0; region < region_count; region++)
	{
		unsigned long permutations_at;
		long permutation_count = block_get(map, regions_at + (unsigned long)region * 0x4C, 0x40, 0x58, &permutations_at);
		const unsigned char *levels;
		long last = -1;
		int level;

		if (permutation_count < 1 || !(levels = tags_at(map, permutations_at + 0x40, 10)))
			continue;
		for (level = 0; level < 5; level++)
			if (i16_le(levels + 2 * level) != -1)
				last = i16_le(levels + 2 * level);
		if (last >= 0 && last < geometry_count)
			wanted[last] = 1;
	}
	for (geometry = 0; geometry < geometry_count; geometry++)
	{
		unsigned long parts_at;
		long part_count, part;

		if (!wanted[geometry])
			continue;
		part_count = block_get(map, geometries_at + (unsigned long)geometry * 0x30, 0x24, 0x68, &parts_at);
		for (part = 0; part < part_count && count < room; part++)
		{
			const unsigned char *field = tags_at(map, parts_at + (unsigned long)part * 0x68, 0x68);
			struct piece *piece = &pieces[count];
			long shader, k;

			if (!field)
				continue;
			shader = i16_le(field + 4);
			memset(piece, 0, sizeof(*piece));
			if (!part_geometry(map, parts_at + (unsigned long)part * 0x68, piece))
			{
				piece_free(piece);
				continue;
			}
			for (k = 0; k < piece->vertex_count; k++)
			{
				piece->uv[2 * k] = (float)(piece->uv[2 * k] * u_scale);
				piece->uv[2 * k + 1] = (float)(piece->uv[2 * k + 1] * v_scale);
			}
			piece->base = shader >= 0 && shader < shader_count ? bases[shader] : -1;
			piece->multipurpose = shader >= 0 && shader < shader_count ? multipurposes[shader] : -1;
			count++;
		}
	}
	return count;
}

/* ---------- our side: a bitmap */

struct bitmap_source
{
	char name[256];
	int map;
	long tag;
	unsigned long address;
	/* its first bitmap's */
	long count;
	long width;
	long height;
	int type;
	int format;
	int flags;
	unsigned long pixels_offset;
	unsigned long pixels_size;
	unsigned char *pixels;
	unsigned long crc;
	/* the pieces that draw it (indexes into the run's pieces) */
	int pieces[MAXIMUM_PIECES];
	int piece_count;
	int multipurpose;
};

static int bitmap_source_read(const struct xbox_map *map, long tag, struct bitmap_source *source)
{
	unsigned long group, bitmaps_at;
	const char *name;
	const unsigned char *data;

	if (!instance_get(map, tag, &group, &name, &source->address))
		return 0;
	snprintf(source->name, sizeof(source->name), "%s", name);
	source->tag = tag;
	source->count = block_get(map, source->address, 0x60, 0x30, &bitmaps_at);
	if (source->count < 1 || !(data = tags_at(map, bitmaps_at, 0x30)))
		return 0;
	source->width = i16_le(data + 4);
	source->height = i16_le(data + 6);
	source->type = i16_le(data + 0xA);
	source->format = i16_le(data + 0xC);
	source->flags = data[0xE] | data[0xF] << 8;
	source->pixels_offset = u32_le(data + 0x18);
	source->pixels_size = u32_le(data + 0x1C);
	return source->width > 0 && source->height > 0 && source->pixels_size > 0 && source->pixels_size < (64UL << 20);
}

/* for each linear texel (row major), its index in the Xbox's swizzled order */
static unsigned long swizzled(long x, long y, long width, long height)
{
	unsigned long index = 0;
	int bit = 0, level, x_bits = 0, y_bits = 0;

	while ((1L << x_bits) < width)
		x_bits++;
	while ((1L << y_bits) < height)
		y_bits++;
	for (level = 0; level < (x_bits > y_bits ? x_bits : y_bits); level++)
	{
		if (level < x_bits)
			index |= (unsigned long)((x >> level) & 1) << bit++;
		if (level < y_bits)
			index |= (unsigned long)((y >> level) & 1) << bit++;
	}
	return index;
}

/* the bitmap's top mip as RGBA floats (malloc'd); NULL for a format not read */
static float *bitmap_rgba(const struct bitmap_source *source)
{
	long width = source->width, height = source->height, x, y;
	float *out = malloc(sizeof(float) * 4 * (size_t)width * (size_t)height);

	if (!out)
		return NULL;
	if (source->format == FORMAT_DXT1 || source->format == FORMAT_DXT3 || source->format == FORMAT_DXT5)
	{
		size_t need = (size_t)(width / 4) * (size_t)(height / 4) * (source->format == FORMAT_DXT1 ? 8 : 16);

		if (width % 4 || height % 4 || need > source->pixels_size)
		{
			free(out);
			return NULL;
		}
		dxt_decode(source->pixels, width, height, source->format, out);
		return out;
	}
	if ((source->format == FORMAT_A8R8G8B8 || source->format == FORMAT_X8R8G8B8) &&
		(size_t)width * (size_t)height * 4 <= source->pixels_size)
	{
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
			{
				unsigned long at = source->flags & 8 ? swizzled(x, y, width, height) : (unsigned long)(y * width + x);
				const unsigned char *bgra = source->pixels + 4 * at;
				float *pixel = out + 4 * ((size_t)y * (size_t)width + (size_t)x);

				pixel[0] = bgra[2];
				pixel[1] = bgra[1];
				pixel[2] = bgra[0];
				pixel[3] = source->format == FORMAT_X8R8G8B8 ? 255.0f : bgra[3];
			}
		return out;
	}
	free(out);
	return NULL;
}

static int bitmap_has_alpha(const struct bitmap_source *source)
{
	return source->format == FORMAT_DXT3 || source->format == FORMAT_DXT5 || source->format == FORMAT_A8R8G8B8;
}

/* ---------- MCC's side: the mesh, its materials and textures */

struct mcc_textures
{
	/* every texture under TEXTURE_ROOT: its name (no extension) */
	long count;
	char **names;
};

static void mcc_textures_free(struct mcc_textures *textures)
{
	long index;

	for (index = 0; index < textures->count; index++)
		free(textures->names[index]);
	free(textures->names);
	memset(textures, 0, sizeof(*textures));
}

static int mcc_textures_list(const struct ue4_pak *pak, struct mcc_textures *textures)
{
	long entry, room = 0;

	memset(textures, 0, sizeof(*textures));
	for (entry = 0; entry < ue4_pak_count(pak); entry++)
	{
		const char *path = ue4_pak_name(pak, entry);
		size_t length = strlen(path);
		const char *slash;

		if (strncmp(path, TEXTURE_ROOT, strlen(TEXTURE_ROOT)) || length < 7 || strcmp(path + length - 7, ".uasset"))
			continue;
		if (textures->count == room)
		{
			char **grown = realloc(textures->names, sizeof(char *) * (size_t)(room = room ? room * 2 : 256));

			if (!grown)
				return 0;
			textures->names = grown;
		}
		slash = strrchr(path, '/');
		textures->names[textures->count] = malloc(length);
		if (!textures->names[textures->count])
			return 0;
		snprintf(textures->names[textures->count], length, "%.*s", (int)(path + length - 7 - slash - 1), slash + 1);
		textures->count++;
	}
	return 1;
}

static int ascii_case_equal(const char *a, const char *b)
{
	for (; *a && *b; a++, b++)
		if (toupper_ascii(*a) != toupper_ascii(*b))
			return 0;
	return *a == *b;
}

/* the texture's spelling in the pak (any case), or NULL */
static const char *texture_named(const struct mcc_textures *textures, const char *name)
{
	long index;

	for (index = 0; index < textures->count; index++)
		if (ascii_case_equal(textures->names[index], name))
			return textures->names[index];
	return NULL;
}

static int texture_role(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(role_suffixes) / sizeof(role_suffixes[0]); index++)
		if (ends_with_case(name, role_suffixes[index].suffix))
			return role_suffixes[index].role;
	return _role_diffuse;
}

/* a material instance's textures by role ("" for none): the H1S2 textures
its package imports, and where it imports none of a role, the texture named
as its diffuse's with that role's suffix; 0 if it has none */
static int material_roles(const struct ue4_pak *pak, const struct mcc_textures *textures, const char *material,
	char roles[NUMBER_OF_ROLES][128])
{
	long asset = ue4_pak_find_asset(pak, MESH_ROOT, material);
	struct ue4_package package;
	unsigned char *uasset;
	size_t size = 0, index;
	int any = 0, role;

	for (role = 0; role < NUMBER_OF_ROLES; role++)
		roles[role][0] = 0;
	if (asset < 0 || !(uasset = ue4_pak_read(pak, asset, &size)))
		return 0;
	if (ue4_package_read(uasset, size, &package))
	{
		long import;

		for (import = 0; import < package.import_count; import++)
		{
			const char *kind = package.names[package.import_class[import]];
			const char *name = package.names[package.import_object[import]];
			const char *spelled;

			if (strcmp(kind, "Texture2D") || strncasecmp_ascii(name, "H1S2_", 5) ||
				!(spelled = texture_named(textures, name)))
			{
				continue;
			}
			role = texture_role(name);
			if (role >= 0 && !roles[role][0])
				snprintf(roles[role], sizeof(roles[role]), "%s", spelled);
		}
		ue4_package_free(&package);
	}
	free(uasset);
	if (roles[_role_diffuse][0])
	{
		for (index = 0; index < sizeof(role_suffixes) / sizeof(role_suffixes[0]); index++)
		{
			char name[256];
			const char *spelled;

			role = role_suffixes[index].role;
			if (role < 0 || roles[role][0])
				continue;
			snprintf(name, sizeof(name), "%s%s", roles[_role_diffuse], role_suffixes[index].suffix);
			if ((spelled = texture_named(textures, name)))
				snprintf(roles[role], sizeof(roles[role]), "%s", spelled);
		}
	}
	for (role = 0; role < NUMBER_OF_ROLES; role++)
		any |= roles[role][0] != 0;
	return any;
}

/* a texture asset's top mip as 8-bit RGBA; NULL if not read (or asset < 0) */
static unsigned char *texture_load(const struct ue4_pak *pak, long asset, long *width, long *height)
{
	struct ue4_texture texture;
	const char *reason;
	unsigned char *rgba;

	if (asset < 0 || !ue4_texture_read(pak, asset, &texture, &reason))
		return NULL;
	rgba = ue4_texture_rgba(&texture);
	*width = texture.width;
	*height = texture.height;
	ue4_texture_free(&texture);
	return rgba;
}

/* ---------- MCC's side: closest points (a uniform grid of its triangles) */

struct grid
{
	const struct ue4_mesh *mesh;
	double origin[3];
	double cell;
	long dims[3];
	/* (cells + 1: each cell's first triangle in list) */
	long *starts;
	long *list;
};

static void grid_free(struct grid *grid)
{
	free(grid->starts);
	free(grid->list);
	memset(grid, 0, sizeof(*grid));
}

static long grid_coordinate(const struct grid *grid, double value, int axis)
{
	long cell = (long)floor((value - grid->origin[axis]) / grid->cell);

	return cell < 0 ? 0 : cell >= grid->dims[axis] ? grid->dims[axis] - 1 : cell;
}

static int grid_make(struct grid *grid, const struct ue4_mesh *mesh, double near)
{
	double low[3], high[3], longest = 0;
	long triangle, total = 0, cells;
	int axis, pass;

	memset(grid, 0, sizeof(*grid));
	grid->mesh = mesh;
	for (axis = 0; axis < 3; axis++)
	{
		long vertex;

		low[axis] = high[axis] = mesh->positions[axis];
		for (vertex = 1; vertex < mesh->vertex_count; vertex++)
		{
			double value = mesh->positions[3 * vertex + axis];

			low[axis] = value < low[axis] ? value : low[axis];
			high[axis] = value > high[axis] ? value : high[axis];
		}
		if (high[axis] - low[axis] > longest)
			longest = high[axis] - low[axis];
	}
	grid->cell = near > longest / 160 ? near : longest / 160;
	if (grid->cell <= 0)
		grid->cell = 1;
	for (axis = 0; axis < 3; axis++)
	{
		grid->origin[axis] = low[axis];
		grid->dims[axis] = (long)((high[axis] - low[axis]) / grid->cell) + 1;
	}
	cells = grid->dims[0] * grid->dims[1] * grid->dims[2];
	grid->starts = calloc((size_t)cells + 1, sizeof(long));
	if (!grid->starts)
		return 0;
	/* (counted, then filled) */
	for (pass = 0; pass < 2; pass++)
	{
		if (pass == 1)
		{
			long cell, at = 0;

			for (cell = 0; cell <= cells; cell++)
			{
				long count = grid->starts[cell];

				grid->starts[cell] = at;
				at += count;
			}
			total = at;
			grid->list = malloc(sizeof(long) * (size_t)(total + 1));
			if (!grid->list)
				return 0;
		}
		for (triangle = 0; triangle < mesh->triangle_count; triangle++)
		{
			long from[3], to[3], x, y, z;

			for (axis = 0; axis < 3; axis++)
			{
				double a = mesh->positions[3 * mesh->triangles[3 * triangle] + axis];
				double b = mesh->positions[3 * mesh->triangles[3 * triangle + 1] + axis];
				double c = mesh->positions[3 * mesh->triangles[3 * triangle + 2] + axis];
				double lo = a < b ? (a < c ? a : c) : (b < c ? b : c), hi = a > b ? (a > c ? a : c) : (b > c ? b : c);

				from[axis] = grid_coordinate(grid, lo, axis);
				to[axis] = grid_coordinate(grid, hi, axis);
			}
			for (z = from[2]; z <= to[2]; z++)
				for (y = from[1]; y <= to[1]; y++)
					for (x = from[0]; x <= to[0]; x++)
					{
						long cell = (z * grid->dims[1] + y) * grid->dims[0] + x;

						if (pass == 0)
							grid->starts[cell]++;
						else
							grid->list[grid->starts[cell]++] = triangle;
					}
		}
		if (pass == 1)
		{
			/* (each start moved to its cell's end: put back) */
			long cell;

			for (cell = cells; cell > 0; cell--)
				grid->starts[cell] = grid->starts[cell - 1];
			grid->starts[0] = 0;
		}
	}
	(void)total;
	return 1;
}

/* the closest point on triangle abc to p: its barycentrics (Ericson, Real-Time
Collision Detection 5.1.5, as the prototype has it) */
static void closest_on_triangle(const double *p, const double *a, const double *b, const double *c, double bary[3])
{
	double ab[3], ac[3], ap[3], bp[3], cp[3], d1, d2, d3, d4, d5, d6, va, vb, vc, v, w, denom;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		ab[axis] = b[axis] - a[axis];
		ac[axis] = c[axis] - a[axis];
		ap[axis] = p[axis] - a[axis];
		bp[axis] = p[axis] - b[axis];
		cp[axis] = p[axis] - c[axis];
	}
	d1 = dot3(ab, ap);
	d2 = dot3(ac, ap);
	d3 = dot3(ab, bp);
	d4 = dot3(ac, bp);
	d5 = dot3(ab, cp);
	d6 = dot3(ac, cp);
	va = d3 * d6 - d5 * d4;
	vb = d5 * d2 - d1 * d6;
	vc = d1 * d4 - d3 * d2;
	if (d1 <= 0 && d2 <= 0)
	{
		bary[0] = 1, bary[1] = 0, bary[2] = 0;
	}
	else if (d3 >= 0 && d4 <= d3)
	{
		bary[0] = 0, bary[1] = 1, bary[2] = 0;
	}
	else if (d6 >= 0 && d5 <= d6)
	{
		bary[0] = 0, bary[1] = 0, bary[2] = 1;
	}
	else if (vc <= 0 && d1 >= 0 && d3 <= 0)
	{
		v = d1 / (d1 - d3);
		bary[0] = 1 - v, bary[1] = v, bary[2] = 0;
	}
	else if (vb <= 0 && d2 >= 0 && d6 <= 0)
	{
		w = d2 / (d2 - d6);
		bary[0] = 1 - w, bary[1] = 0, bary[2] = w;
	}
	else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
	{
		w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
		bary[0] = 0, bary[1] = 1 - w, bary[2] = w;
	}
	else
	{
		denom = va + vb + vc;
		if (fabs(denom) < 1e-300)
		{
			bary[0] = 1, bary[1] = 0, bary[2] = 0;
			return;
		}
		v = vb / denom;
		w = vc / denom;
		bary[0] = 1 - v - w, bary[1] = v, bary[2] = w;
	}
	for (axis = 0; axis < 3; axis++)
		if (!isfinite(bary[axis]))
			bary[0] = bary[1] = bary[2] = 0;
}

/* the closest point on the mesh to p within `near`: its distance (HUGE_VAL
if none is that close), triangle and barycentrics; stamp is the caller's
(triangle_count longs) to see each triangle once */
static double grid_closest(const struct grid *grid, const double *p, double near, long *stamp, long query,
	long *triangle_out, double bary_out[3])
{
	const struct ue4_mesh *mesh = grid->mesh;
	long from[3], to[3], x, y, z;
	double best = HUGE_VAL;
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		if (p[axis] + near < grid->origin[axis] || p[axis] - near > grid->origin[axis] + grid->cell * grid->dims[axis])
			return HUGE_VAL;
		from[axis] = grid_coordinate(grid, p[axis] - near, axis);
		to[axis] = grid_coordinate(grid, p[axis] + near, axis);
	}
	for (z = from[2]; z <= to[2]; z++)
		for (y = from[1]; y <= to[1]; y++)
			for (x = from[0]; x <= to[0]; x++)
			{
				long cell = (z * grid->dims[1] + y) * grid->dims[0] + x, at;

				for (at = grid->starts[cell]; at < grid->starts[cell + 1]; at++)
				{
					long triangle = grid->list[at];
					const double *a, *b, *c;
					double bary[3], point[3], distance;

					if (stamp[triangle] == query)
						continue;
					stamp[triangle] = query;
					a = mesh->positions + 3 * mesh->triangles[3 * triangle];
					b = mesh->positions + 3 * mesh->triangles[3 * triangle + 1];
					c = mesh->positions + 3 * mesh->triangles[3 * triangle + 2];
					closest_on_triangle(p, a, b, c, bary);
					for (axis = 0; axis < 3; axis++)
						point[axis] = bary[0] * a[axis] + bary[1] * b[axis] + bary[2] * c[axis] - p[axis];
					distance = sqrt(dot3(point, point));
					if (distance < best)
					{
						best = distance;
						*triangle_out = triangle;
						memcpy(bary_out, bary, sizeof(bary));
					}
				}
			}
	return best <= near ? best : HUGE_VAL;
}

/* ---------- the lookup: our texels to MCC's surface */

enum
{
	_texel_none,
	_texel_found,
	/* drawn, but nothing of MCC's is close */
	_texel_far,
};

struct lookup
{
	long width;
	long height;
	unsigned char *state;
	double *best;
	/* MCC's triangle and barycentrics (the third is 1 less the two) */
	long *mcc_triangle;
	float *mcc_bary;
	/* ours: the piece, its triangle and the raster's barycentrics */
	unsigned char *our_piece;
	long *our_triangle;
	float *our_bary;
};

static void lookup_free(struct lookup *look)
{
	free(look->state);
	free(look->best);
	free(look->mcc_triangle);
	free(look->mcc_bary);
	free(look->our_piece);
	free(look->our_triangle);
	free(look->our_bary);
	memset(look, 0, sizeof(*look));
}

/* for each texel of a width x height texture: the piece's triangle drawn
there (-1 for none) and its barycentrics (texel centers inside, plus a margin
of about three quarters of a texel; UVs wrapped) */
static void raster(const struct piece *piece, long width, long height, long *tri_of, float *bary)
{
	long triangle, texel;

	for (texel = 0; texel < width * height; texel++)
		tri_of[texel] = -1;
	for (triangle = 0; triangle < piece->triangle_count; triangle++)
	{
		double tri[3][2], shift[2], area, eps;
		long x0, y0, x1, y1, x, y;
		int corner;

		for (corner = 0; corner < 3; corner++)
		{
			const double *uv = piece->uv + 2 * piece->triangles[3 * triangle + corner];

			tri[corner][0] = uv[0] * width;
			tri[corner][1] = uv[1] * height;
		}
		shift[0] = floor((tri[0][0] + tri[1][0] + tri[2][0]) / 3 / width) * width;
		shift[1] = floor((tri[0][1] + tri[1][1] + tri[2][1]) / 3 / height) * height;
		for (corner = 0; corner < 3; corner++)
		{
			tri[corner][0] -= shift[0];
			tri[corner][1] -= shift[1];
		}
		area = (tri[1][0] - tri[0][0]) * (tri[2][1] - tri[0][1]) - (tri[1][1] - tri[0][1]) * (tri[2][0] - tri[0][0]);
		if (fabs(area) < 1e-12)
			continue;
		x0 = (long)floor(fmin(tri[0][0], fmin(tri[1][0], tri[2][0])));
		y0 = (long)floor(fmin(tri[0][1], fmin(tri[1][1], tri[2][1])));
		x1 = (long)ceil(fmax(tri[0][0], fmax(tri[1][0], tri[2][0])));
		y1 = (long)ceil(fmax(tri[0][1], fmax(tri[1][1], tri[2][1])));
		eps = 0.75 / fmax(1e-9, sqrt(fabs(area)));
		for (y = y0; y <= y1; y++)
		{
			for (x = x0; x <= x1; x++)
			{
				double xs = x + 0.5, ys = y + 0.5, w0, w1, w2, c0, c1, c2, sum;
				long gx, gy, at;
				int strict;

				w0 = ((tri[1][0] - xs) * (tri[2][1] - ys) - (tri[1][1] - ys) * (tri[2][0] - xs)) / area;
				w1 = ((tri[2][0] - xs) * (tri[0][1] - ys) - (tri[2][1] - ys) * (tri[0][0] - xs)) / area;
				w2 = 1 - w0 - w1;
				if (w0 < -eps || w1 < -eps || w2 < -eps)
					continue;
				strict = w0 >= 0 && w1 >= 0 && w2 >= 0;
				gx = ((x % width) + width) % width;
				gy = ((y % height) + height) % height;
				at = gy * width + gx;
				/* (a strictly inside texel always wins over a margin one there) */
				if (tri_of[at] >= 0 && !strict)
					continue;
				c0 = w0 > 0 ? w0 : 0;
				c1 = w1 > 0 ? w1 : 0;
				c2 = w2 > 0 ? w2 : 0;
				sum = c0 + c1 + c2;
				tri_of[at] = triangle;
				bary[3 * at] = (float)(c0 / sum);
				bary[3 * at + 1] = (float)(c1 / sum);
				bary[3 * at + 2] = (float)(c2 / sum);
			}
		}
	}
}

struct query_job
{
	const struct grid *grid;
	const struct piece *piece;
	const long *texels;
	const long *tri_of;
	const float *bary;
	long first;
	long last;
	double *distance;
	long *triangle;
	double *mcc_bary;
};

static void *query_thread(void *context)
{
	struct query_job *job = context;
	const struct piece *piece = job->piece;
	long *stamp = malloc(sizeof(long) * (size_t)(job->grid->mesh->triangle_count + 1));
	long index;

	if (!stamp)
		return NULL;
	for (index = 0; index < job->grid->mesh->triangle_count; index++)
		stamp[index] = -1;
	for (index = job->first; index < job->last; index++)
	{
		long texel = job->texels[index];
		const long *corners = piece->triangles + 3 * job->tri_of[texel];
		const float *w = job->bary + 3 * texel;
		double point[3], moved[3];
		int axis;

		for (axis = 0; axis < 3; axis++)
			point[axis] = w[0] * piece->xyz[3 * corners[0] + axis] + w[1] * piece->xyz[3 * corners[1] + axis] +
				w[2] * piece->xyz[3 * corners[2] + axis];
		for (axis = 0; axis < 3; axis++)
			moved[axis] = piece->scale * (piece->rotation[3 * axis] * point[0] + piece->rotation[3 * axis + 1] * point[1] +
				piece->rotation[3 * axis + 2] * point[2]) + piece->shift[axis];
		job->distance[index] = grid_closest(job->grid, moved, piece->near, stamp, index, &job->triangle[index],
			job->mcc_bary + 3 * index);
	}
	free(stamp);
	return NULL;
}

struct run;
static int run_progress(struct run *run, float within);

/* our pieces' texels of a width x height texture on MCC's surface; 0 if out
of memory or canceled */
static int lookup_make(struct run *run, const struct grid *grid, const struct piece *pieces, const int *which,
	int count, long width, long height, int threads, struct lookup *look)
{
	size_t texels = (size_t)width * (size_t)height;
	long *tri_of = malloc(sizeof(long) * texels), *list = malloc(sizeof(long) * texels);
	float *bary = malloc(sizeof(float) * 3 * texels);
	double *distance = NULL, *mcc_bary = NULL;
	long *triangle = NULL;
	int number, made = 0;

	memset(look, 0, sizeof(*look));
	look->width = width;
	look->height = height;
	look->state = calloc(texels, 1);
	look->best = malloc(sizeof(double) * texels);
	look->mcc_triangle = calloc(texels, sizeof(long));
	look->mcc_bary = calloc(texels * 2, sizeof(float));
	look->our_piece = calloc(texels, 1);
	look->our_triangle = calloc(texels, sizeof(long));
	look->our_bary = calloc(texels * 3, sizeof(float));
	if (!tri_of || !list || !bary || !look->state || !look->best || !look->mcc_triangle || !look->mcc_bary ||
		!look->our_piece || !look->our_triangle || !look->our_bary)
	{
		goto finished;
	}
	{
		size_t texel;

		for (texel = 0; texel < texels; texel++)
			look->best[texel] = INFINITY;
	}
	for (number = 0; number < count; number++)
	{
		const struct piece *piece = &pieces[which[number]];
		pthread_t workers[64];
		struct query_job jobs[64];
		unsigned char started[64];
		long found = 0, index;
		int thread;

		raster(piece, width, height, tri_of, bary);
		for (index = 0; index < (long)texels; index++)
			if (tri_of[index] >= 0)
				list[found++] = index;
		free(distance);
		free(triangle);
		free(mcc_bary);
		distance = malloc(sizeof(double) * (size_t)(found + 1));
		triangle = calloc((size_t)(found + 1), sizeof(long));
		mcc_bary = malloc(sizeof(double) * 3 * (size_t)(found + 1));
		if (!distance || !triangle || !mcc_bary)
			goto finished;
		threads = threads < 1 ? 1 : threads > 64 ? 64 : threads;
		for (thread = 0; thread < threads; thread++)
		{
			jobs[thread].grid = grid;
			jobs[thread].piece = piece;
			jobs[thread].texels = list;
			jobs[thread].tri_of = tri_of;
			jobs[thread].bary = bary;
			jobs[thread].first = found * thread / threads;
			jobs[thread].last = found * (thread + 1) / threads;
			jobs[thread].distance = distance;
			jobs[thread].triangle = triangle;
			jobs[thread].mcc_bary = mcc_bary;
		}
		for (thread = 1; thread < threads; thread++)
			started[thread] = pthread_create(&workers[thread], NULL, query_thread, &jobs[thread]) == 0;
		query_thread(&jobs[0]);
		for (thread = 1; thread < threads; thread++)
		{
			/* (a thread that could not start: its share done here) */
			if (started[thread])
				pthread_join(workers[thread], NULL);
			else
				query_thread(&jobs[thread]);
		}
		for (index = 0; index < found; index++)
		{
			long texel = list[index];
			/* (nothing within close: farther than every close point) */
			double d = distance[index] == HUGE_VAL ? 1e300 : distance[index];

			if (!(d < look->best[texel]))
				continue;
			look->best[texel] = d;
			look->state[texel] = d <= piece->near ? _texel_found : _texel_far;
			look->mcc_triangle[texel] = triangle[index];
			look->mcc_bary[2 * texel] = (float)mcc_bary[3 * index];
			look->mcc_bary[2 * texel + 1] = (float)mcc_bary[3 * index + 1];
			look->our_piece[texel] = (unsigned char)which[number];
			look->our_triangle[texel] = tri_of[texel];
			memcpy(look->our_bary + 3 * texel, bary + 3 * texel, sizeof(float) * 3);
		}
		if (run_progress(run, (float)(number + 1) / (float)count * 0.6f))
			goto finished;
	}
	made = 1;

finished:
	free(tri_of);
	free(list);
	free(bary);
	free(distance);
	free(triangle);
	free(mcc_bary);
	if (!made)
		lookup_free(look);
	return made;
}

/* ---------- sampling MCC's textures */

enum
{
	/* RGBA */
	_sample_rgba,
	/* the red channel */
	_sample_red,
	/* the mean of red, green and blue */
	_sample_mean,
};

/* the image at continuous pixel coordinates (texel i's center at i + 0.5), wrapped */
static void bilinear_wrap(const unsigned char *image, long width, long height, double x, double y, double out[4])
{
	long x0, y0, xa, xb, ya, yb;
	double fx, fy;
	int channel;

	x -= 0.5;
	y -= 0.5;
	x0 = (long)floor(x);
	y0 = (long)floor(y);
	fx = x - x0;
	fy = y - y0;
	xa = ((x0 % width) + width) % width;
	xb = (xa + 1) % width;
	ya = ((y0 % height) + height) % height;
	yb = (ya + 1) % height;
	for (channel = 0; channel < 4; channel++)
	{
		double top = image[4 * (ya * width + xa) + channel] * (1 - fx) + image[4 * (ya * width + xb) + channel] * fx;
		double bottom = image[4 * (yb * width + xa) + channel] * (1 - fx) + image[4 * (yb * width + xb) + channel] * fx;

		out[channel] = top * (1 - fy) + bottom * fy;
	}
}

struct mcc_mesh
{
	struct ue4_mesh mesh;
	char roles[MAXIMUM_SECTIONS][NUMBER_OF_ROLES][128];
};

/* each found texel's MCC texel of a role's texture into out (channels a
texel: 4 for _sample_rgba, else 1), NaN where its section has none; 0 if
none was sampled */
static int sample_role(const struct ue4_pak *pak, const struct mcc_mesh *mcc, const struct lookup *look, int role,
	int mode, float *out)
{
	size_t texels = (size_t)look->width * (size_t)look->height, texel;
	int channels = mode == _sample_rgba ? 4 : 1, any = 0;
	long section, done[MAXIMUM_SECTIONS];
	long section_count = mcc->mesh.section_count < MAXIMUM_SECTIONS ? mcc->mesh.section_count : MAXIMUM_SECTIONS;

	for (texel = 0; texel < texels * (size_t)channels; texel++)
		out[texel] = NAN;
	for (section = 0; section < section_count; section++)
		done[section] = 0;
	for (section = 0; section < section_count; section++)
	{
		const char *name = mcc->roles[section][role];
		unsigned char *image;
		long width = 0, height = 0, other;

		if (!name[0] || done[section])
			continue;
		image = texture_load(pak, ue4_pak_find_asset(pak, TEXTURE_ROOT, name), &width, &height);
		/* (every section of the same texture sampled from this one load) */
		for (other = section; other < section_count; other++)
		{
			if (strcmp(mcc->roles[other][role], name))
				continue;
			done[other] = 1;
			if (!image)
				continue;
			for (texel = 0; texel < texels; texel++)
			{
				long triangle = look->mcc_triangle[texel];
				const long *corners;
				double b0, b1, b2, u, v, value[4];

				if (look->state[texel] != _texel_found || mcc->mesh.section_of[triangle] != other)
					continue;
				corners = mcc->mesh.triangles + 3 * triangle;
				b0 = look->mcc_bary[2 * texel];
				b1 = look->mcc_bary[2 * texel + 1];
				b2 = 1.0 - b0 - b1;
				u = b0 * mcc->mesh.uvs[2 * corners[0]] + b1 * mcc->mesh.uvs[2 * corners[1]] + b2 * mcc->mesh.uvs[2 * corners[2]];
				v = b0 * mcc->mesh.uvs[2 * corners[0] + 1] + b1 * mcc->mesh.uvs[2 * corners[1] + 1] +
					b2 * mcc->mesh.uvs[2 * corners[2] + 1];
				bilinear_wrap(image, width, height, u * width, v * height, value);
				if (mode == _sample_rgba)
				{
					out[4 * texel] = (float)value[0];
					out[4 * texel + 1] = (float)value[1];
					out[4 * texel + 2] = (float)value[2];
					out[4 * texel + 3] = (float)value[3];
				}
				else
				{
					out[texel] = (float)(mode == _sample_red ? value[0] : (value[0] + value[1] + value[2]) / 3);
				}
				any = 1;
			}
		}
		free(image);
	}
	return any;
}

/* our bitmap enlarged to width x height (bilinear, wrapped), RGBA floats */
static float *classic_enlarged(const float *classic, long classic_width, long classic_height, long width, long height)
{
	float *out = malloc(sizeof(float) * 4 * (size_t)width * (size_t)height);
	long x, y;

	if (!out)
		return NULL;
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
		{
			double sx = (x + 0.5) * classic_width / width - 0.5, sy = (y + 0.5) * classic_height / height - 0.5, fx, fy;
			long x0 = (long)floor(sx), y0 = (long)floor(sy), xa, xb, ya, yb;
			int channel;

			fx = sx - x0;
			fy = sy - y0;
			xa = ((x0 % classic_width) + classic_width) % classic_width;
			xb = (xa + 1) % classic_width;
			ya = ((y0 % classic_height) + classic_height) % classic_height;
			yb = (ya + 1) % classic_height;
			for (channel = 0; channel < 4; channel++)
			{
				double top = classic[4 * (ya * classic_width + xa) + channel] * (1 - fx) +
					classic[4 * (ya * classic_width + xb) + channel] * fx;
				double bottom = classic[4 * (yb * classic_width + xa) + channel] * (1 - fx) +
					classic[4 * (yb * classic_width + xb) + channel] * fx;

				out[4 * (y * width + x) + channel] = (float)(top * (1 - fy) + bottom * fy);
			}
		}
	return out;
}

/* an image's (channels a texel) texels where `have` is not set taken from
their neighbors where it is, `steps` texels out (wrapped); have grows */
static int dilate(float *image, unsigned char *have, long width, long height, int channels, int steps)
{
	size_t texels = (size_t)width * (size_t)height;
	unsigned char *next = malloc(texels);
	float *grown = malloc(sizeof(float) * (size_t)channels * texels);
	int step;

	if (!next || !grown)
	{
		free(next);
		free(grown);
		return 0;
	}
	for (step = 0; step < steps; step++)
	{
		long x, y;
		int any = 0;

		memcpy(next, have, texels);
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
			{
				long at = y * width + x, neighbors[4];
				int count = 0, k, channel;
				double sum[4] = { 0, 0, 0, 0 };

				if (have[at])
					continue;
				neighbors[0] = ((y + 1) % height) * width + x;
				neighbors[1] = ((y + height - 1) % height) * width + x;
				neighbors[2] = y * width + (x + 1) % width;
				neighbors[3] = y * width + (x + width - 1) % width;
				for (k = 0; k < 4; k++)
				{
					if (!have[neighbors[k]])
						continue;
					count++;
					for (channel = 0; channel < channels; channel++)
						sum[channel] += image[(size_t)neighbors[k] * (size_t)channels + (size_t)channel];
				}
				if (!count)
					continue;
				for (channel = 0; channel < channels; channel++)
					grown[(size_t)at * (size_t)channels + (size_t)channel] = (float)(sum[channel] / count);
				next[at] = 1;
				any = 1;
			}
		if (!any)
			break;
		for (y = 0; y < (long)texels; y++)
			if (next[y] && !have[y])
			{
				int channel;

				for (channel = 0; channel < channels; channel++)
					image[(size_t)y * (size_t)channels + (size_t)channel] = grown[(size_t)y * (size_t)channels + (size_t)channel];
			}
		memcpy(have, next, texels);
	}
	free(next);
	free(grown);
	return 1;
}

/* a box filter 2r+1 wide along each axis (wrapped), in place */
static void box_wrap(double *values, double *work, long width, long height, long radius)
{
	long x, y, k, size = 2 * radius + 1;

	for (y = 0; y < height; y++)
	{
		double *row = values + y * width, sum = 0;

		for (k = -radius; k <= radius; k++)
			sum += row[((k % width) + width) % width];
		for (x = 0; x < width; x++)
		{
			work[x] = sum / size;
			sum += row[(x + radius + 1) % width] - row[((x - radius) % width + width) % width];
		}
		memcpy(row, work, sizeof(double) * (size_t)width);
	}
	for (x = 0; x < width; x++)
	{
		double sum = 0;

		for (k = -radius; k <= radius; k++)
			sum += values[(((k % height) + height) % height) * width + x];
		for (y = 0; y < height; y++)
		{
			work[y] = sum / size;
			sum += values[((y + radius + 1) % height) * width + x] - values[(((y - radius) % height + height) % height) * width + x];
		}
		for (y = 0; y < height; y++)
			values[y * width + x] = work[y];
	}
}

/* values blurred by a box radius texels across, three times, over mask only */
static double *masked_blur(const double *values, const unsigned char *mask, long width, long height, long radius)
{
	size_t texels = (size_t)width * (size_t)height, texel;
	double *total = malloc(sizeof(double) * texels), *weight = malloc(sizeof(double) * texels);
	double *work = malloc(sizeof(double) * (size_t)(width > height ? width : height));
	int pass;

	if (!total || !weight || !work)
	{
		free(total);
		free(weight);
		free(work);
		return NULL;
	}
	for (texel = 0; texel < texels; texel++)
	{
		weight[texel] = mask[texel] ? 1.0 : 0.0;
		total[texel] = mask[texel] ? values[texel] : 0.0;
	}
	for (pass = 0; pass < 3; pass++)
	{
		box_wrap(total, work, width, height, radius);
		box_wrap(weight, work, width, height, radius);
	}
	for (texel = 0; texel < texels; texel++)
		total[texel] /= weight[texel] > 1e-6 ? weight[texel] : 1e-6;
	free(weight);
	free(work);
	return total;
}

/* values scaled and shifted to best match target (least squares) over the
texels of ok: their slope, means and correlation; 0 if they cannot be */
struct fit
{
	double slope;
	double values_mean;
	double target_mean;
	double correlation;
};

static int fit_to(const float *values, const float *target, int target_stride, const unsigned char *ok, size_t texels,
	struct fit *fit)
{
	double n = 0, sv = 0, st = 0, svv = 0, stt = 0, svt = 0, sd_v, sd_t;
	size_t texel;

	for (texel = 0; texel < texels; texel++)
	{
		double v, t;

		if (!ok[texel])
			continue;
		v = values[texel];
		t = target[texel * (size_t)target_stride];
		n++;
		sv += v;
		st += t;
	}
	if (n < 100)
		return 0;
	fit->values_mean = sv / n;
	fit->target_mean = st / n;
	for (texel = 0; texel < texels; texel++)
	{
		double v, t;

		if (!ok[texel])
			continue;
		v = values[texel] - fit->values_mean;
		t = target[texel * (size_t)target_stride] - fit->target_mean;
		svv += v * v;
		stt += t * t;
		svt += v * t;
	}
	sd_v = sqrt(svv / n);
	sd_t = sqrt(stt / n);
	if (sd_v < 1e-3 || sd_t < 1e-3)
		return 0;
	fit->correlation = svt / sqrt(svv * stt);
	fit->slope = fit->correlation * sd_t / sd_v;
	return 1;
}

static int compare_floats(const void *a, const void *b)
{
	float x = *(const float *)a, y = *(const float *)b;

	return x < y ? -1 : x > y;
}

/* the q (0 to 1) percentile of values, linearly between the nearest */
static double percentile(float *values, size_t count, double q)
{
	double at;
	size_t low;

	if (!count)
		return 0;
	qsort(values, count, sizeof(float), compare_floats);
	at = q * (double)(count - 1);
	low = (size_t)floor(at);
	if (low + 1 >= count)
		return values[count - 1];
	return values[low] + (values[low + 1] - values[low]) * (at - (double)low);
}

/* ---------- the run */

struct run
{
	const struct ue4_pak *pak;
	const struct mcc_bake_options *options;
	struct mcc_textures textures;
	struct xbox_map maps[8];
	int map_count;
	struct piece pieces[MAXIMUM_PIECES];
	int piece_count;
	struct bitmap_source bitmaps[MAXIMUM_BITMAPS];
	int bitmap_count;
	long written;
	long bitmaps_written;
	int canceled;
	/* the progress: this mesh's share of the whole and where it starts */
	float progress_base;
	float progress_share;
	float within_bitmap_base;
	float within_bitmap_share;
};

static void run_log(struct run *run, const char *format, ...)
{
	char line[1024];
	va_list arguments;

	if (!run->options->say)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	run->options->say(line);
}

static int run_progress(struct run *run, float within)
{
	float fraction = run->progress_base + run->progress_share * (run->within_bitmap_base + run->within_bitmap_share * within);

	if (run->canceled)
		return 1;
	if (run->options->progress && run->options->progress(run->options->context, fraction))
		run->canceled = 1;
	return run->canceled;
}

static void note_add(struct run *run, const char *format, const char *a, unsigned long crc)
{
	size_t length;

	if (!run->options->note || !run->options->note_size)
		return;
	length = strlen(run->options->note);
	snprintf(run->options->note + length, run->options->note_size - length, format, crc, a);
}

/* the map of that name, read (its tags, kept for the run; at most 8) */
static int map_get(struct run *run, const char *name)
{
	struct xbox_map *map;
	int index;

	for (index = 0; index < run->map_count; index++)
		if (!strcmp(run->maps[index].name, name))
			return run->maps[index].tags ? index : -1;
	if (run->map_count == (int)(sizeof(run->maps) / sizeof(run->maps[0])))
		return -1;
	map = &run->maps[run->map_count++];
	snprintf(map->name, sizeof(map->name), "%s", name);
	snprintf(map->path, sizeof(map->path), "%s%s.map", run->options->maps_folder, name);
	map->tags = xbox_map_tags_read(map->path, &map->size);
	if (!map->tags)
	{
		run_log(run, "  %s: not read", map->path);
		return -1;
	}
	note_add(run, "map %08lx %s\n", name, crc32(0, map->tags, (uInt)map->size));
	return run->map_count - 1;
}

/* the tag of a group and name (any case) with a definition, or -1 */
static long tag_find(const struct xbox_map *map, unsigned long wanted_group, const char *name)
{
	long index;

	for (index = 0; index < instance_count(map); index++)
	{
		unsigned long group, definition;
		const char *found;

		if (instance_get(map, index, &group, &found, &definition) && group == wanted_group && definition &&
			ascii_case_equal(found, name))
		{
			return index;
		}
	}
	return -1;
}

/* the bitmap of the map's tag (the first map that has it is the one read) */
static struct bitmap_source *bitmap_get(struct run *run, int map, long tag, int multipurpose)
{
	unsigned long group, definition;
	const char *name;
	int index;

	if (tag < 0 || !instance_get(&run->maps[map], tag, &group, &name, &definition))
		return NULL;
	for (index = 0; index < run->bitmap_count; index++)
		if (!strcmp(run->bitmaps[index].name, name) && run->bitmaps[index].multipurpose == multipurpose)
			return &run->bitmaps[index];
	if (run->bitmap_count == MAXIMUM_BITMAPS)
		return NULL;
	memset(&run->bitmaps[run->bitmap_count], 0, sizeof(run->bitmaps[0]));
	if (!bitmap_source_read(&run->maps[map], tag, &run->bitmaps[run->bitmap_count]))
		return NULL;
	run->bitmaps[run->bitmap_count].map = map;
	run->bitmaps[run->bitmap_count].multipurpose = multipurpose;
	return &run->bitmaps[run->bitmap_count++];
}

static void bake_size(long width, long height, long max_size, long *out_width, long *out_height)
{
	long factor = 1;

	while (factor < 4 && (width > height ? width : height) * factor * 2 <= max_size)
		factor *= 2;
	*out_width = width * factor;
	*out_height = height * factor;
}

/* the mips of an RGBA float image to 1x1 (box filtered), each padded to 4x4
at least and encoded; the pixels' bytes (malloc'd) */
static unsigned char *mips_encode(const float *image, long width, long height, int dxt5, size_t *size, long *levels)
{
	float *current = malloc(sizeof(float) * 4 * (size_t)width * (size_t)height);
	long w = width, h = height;
	size_t total = 0, room = 0;
	unsigned char *pixels = NULL;

	*levels = 0;
	if (!current)
		return NULL;
	memcpy(current, image, sizeof(float) * 4 * (size_t)width * (size_t)height);
	for (;;)
	{
		long pw = w < 4 ? 4 : w, ph = h < 4 ? 4 : h, x, y;
		float *padded = malloc(sizeof(float) * 4 * (size_t)pw * (size_t)ph);
		size_t bytes = (size_t)(pw / 4) * (size_t)(ph / 4) * (dxt5 ? 16 : 8);

		if (!padded)
			goto refused;
		for (y = 0; y < ph; y++)
			for (x = 0; x < pw; x++)
				memcpy(padded + 4 * (y * pw + x), current + 4 * ((y < h ? y : h - 1) * w + (x < w ? x : w - 1)),
					sizeof(float) * 4);
		if (total + bytes > room)
		{
			unsigned char *grown = realloc(pixels, room = (total + bytes) * 2);

			if (!grown)
			{
				free(padded);
				goto refused;
			}
			pixels = grown;
		}
		dxt_encode(padded, pw, ph, dxt5, pixels + total);
		free(padded);
		total += bytes;
		(*levels)++;
		if (w == 1 && h == 1)
			break;
		{
			long nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
			float *next = malloc(sizeof(float) * 4 * (size_t)nw * (size_t)nh);

			if (!next)
				goto refused;
			for (y = 0; y < nh; y++)
				for (x = 0; x < nw; x++)
				{
					int channel;

					for (channel = 0; channel < 4; channel++)
					{
						long x0 = w > 1 ? 2 * x : x, y0 = h > 1 ? 2 * y : y;
						long x1 = w > 1 ? x0 + 1 : x0, y1 = h > 1 ? y0 + 1 : y0;
						/* (rows first, then columns, as the prototype halves them) */
						float top = h > 1 ? (current[4 * (y0 * w + x0) + channel] + current[4 * (y1 * w + x0) + channel]) / 2 :
							current[4 * (y0 * w + x0) + channel];
						float bottom = h > 1 ? (current[4 * (y0 * w + x1) + channel] + current[4 * (y1 * w + x1) + channel]) / 2 :
							current[4 * (y0 * w + x1) + channel];

						next[4 * (y * nw + x) + channel] = w > 1 ? (top + bottom) / 2 : top;
					}
				}
			free(current);
			current = next;
			w = nw;
			h = nh;
		}
	}
	free(current);
	*size = total;
	return pixels;

refused:
	free(current);
	free(pixels);
	return NULL;
}

/* a Halo PC bitmap tag file (big-endian, version 7) of the map's bitmap with
one DXT1 or DXT5 bitmap of `image` and its mips, its sequences and sprites
the map's */
static int bitmap_write(struct run *run, const struct bitmap_source *source, float *image, long width, long height,
	int alpha_wanted)
{
	const struct xbox_map *map = &run->maps[source->map];
	const unsigned char *root = tags_at(map, source->address, 0x6C);
	unsigned long sequences_at;
	long sequence_count = block_get(map, source->address, 0x54, 0x40, &sequences_at), sequence, levels = 0;
	size_t texels = (size_t)width * (size_t)height, texel, pixels_size = 0, tail_size = 0, size, at;
	unsigned char *pixels, *file;
	float least_alpha = 255;
	int dxt5, written;
	static const unsigned int short_fields[] = { 0x00, 0x04, 0x06, 0x14, 0x16, 0x18, 0x1A, 0x4C, 0x4E, 0x50 };
	static const unsigned int float_fields[] = { 0x08, 0x0C, 0x10, 0x44, 0x48 };
	char path[MAXIMUM_PATH];
	size_t index;

	if (!root || sequence_count < 0)
		return 0;
	for (texel = 0; texel < texels * 4; texel++)
		image[texel] = image[texel] < 0 ? 0 : image[texel] > 255 ? 255 : image[texel];
	for (texel = 0; texel < texels; texel++)
		least_alpha = image[4 * texel + 3] < least_alpha ? image[4 * texel + 3] : least_alpha;
	dxt5 = alpha_wanted && least_alpha < 250;
	pixels = mips_encode(image, width, height, dxt5, &pixels_size, &levels);
	if (!pixels)
		return 0;
	for (sequence = 0; sequence < sequence_count; sequence++)
	{
		unsigned long sprites_at;

		tail_size += 0x40 + 0x20 * (size_t)block_get(map, sequences_at + (unsigned long)sequence * 0x40, 0x34, 0x20, &sprites_at);
	}
	size = 0x40 + 0x6C + pixels_size + tail_size + 0x30;
	file = calloc(size, 1);
	if (!file)
	{
		free(pixels);
		return 0;
	}
	/* the header */
	memcpy(file + 0x24, "bitm", 4);
	put_be32(file + 0x2C, 0x40);
	put_be16(file + 0x38, 7);
	memcpy(file + 0x3C, "blam", 4);
	/* the root */
	for (index = 0; index < sizeof(short_fields) / sizeof(short_fields[0]); index++)
		put_be16(file + 0x40 + short_fields[index], (unsigned int)(root[short_fields[index]] | root[short_fields[index] + 1] << 8));
	for (index = 0; index < sizeof(float_fields) / sizeof(float_fields[0]); index++)
		put_be_float(file + 0x40 + float_fields[index], f32_le(root + float_fields[index]));
	/* (the group's format: 0 compressed with color-key transparency, 2 with interpolated alpha) */
	put_be16(file + 0x40 + 0x02, dxt5 ? 2 : 0);
	put_be32(file + 0x40 + 0x30, (unsigned long)pixels_size);
	put_be32(file + 0x40 + 0x54, (unsigned long)sequence_count);
	put_be32(file + 0x40 + 0x60, 1);
	at = 0x40 + 0x6C;
	memcpy(file + at, pixels, pixels_size);
	at += pixels_size;
	free(pixels);
	/* the sequences, then each one's sprites */
	for (sequence = 0; sequence < sequence_count; sequence++)
	{
		const unsigned char *element = tags_at(map, sequences_at + (unsigned long)sequence * 0x40, 0x40);
		unsigned long sprites_at;

		memcpy(file + at, element, 32);
		put_be16(file + at + 0x20, (unsigned int)(element[0x20] | element[0x21] << 8));
		put_be16(file + at + 0x22, (unsigned int)(element[0x22] | element[0x23] << 8));
		put_be32(file + at + 0x34, (unsigned long)block_get(map, sequences_at + (unsigned long)sequence * 0x40, 0x34, 0x20,
			&sprites_at));
		at += 0x40;
	}
	for (sequence = 0; sequence < sequence_count; sequence++)
	{
		unsigned long sprites_at;
		long sprite_count = block_get(map, sequences_at + (unsigned long)sequence * 0x40, 0x34, 0x20, &sprites_at), sprite;

		for (sprite = 0; sprite < sprite_count; sprite++)
		{
			const unsigned char *element = tags_at(map, sprites_at + (unsigned long)sprite * 0x20, 0x20);
			int value;

			put_be16(file + at, (unsigned int)(element[0] | element[1] << 8));
			for (value = 0; value < 6; value++)
				put_be_float(file + at + 8 + 4 * value, f32_le(element + 8 + 4 * value));
			at += 0x20;
		}
	}
	/* the bitmap: 2D, power of two, compressed */
	memcpy(file + at, "bitm", 4);
	put_be16(file + at + 4, (unsigned int)width);
	put_be16(file + at + 6, (unsigned int)height);
	put_be16(file + at + 8, 1);
	put_be16(file + at + 0xA, 0);
	put_be16(file + at + 0xC, dxt5 ? FORMAT_DXT5 : FORMAT_DXT1);
	put_be16(file + at + 0xE, 3);
	put_be16(file + at + 0x14, (unsigned int)(levels - 1));
	put_be32(file + at + 0x18, 0);
	put_be32(file + at + 0x1C, (unsigned long)pixels_size);
	tag_path(path, sizeof(path), run->options->out_folder, source->name, ".bitmap");
	written = write_file(path, file, size);
	free(file);
	if (written)
	{
		run->written++;
		run->bitmaps_written++;
		note_add(run, "bitmap %08lx %s\n", source->name, source->crc);
	}
	return written ? (dxt5 ? 5 : 1) : 0;
}

/* MCC's normal map and its occlusion, roughness and metalness in our
layout: <bitmap>.normal.png (tangent space, red along +u, green toward the
picture's top) and <bitmap>.orm.png */
static void extras_write(struct run *run, const struct mcc_mesh *mcc, const struct lookup *look,
	const struct bitmap_source *source)
{
	long width = look->width, height = look->height;
	size_t texels = (size_t)width * (size_t)height, texel;
	float *normal = malloc(sizeof(float) * 4 * texels), *image = malloc(sizeof(float) * 3 * texels);
	float *values = malloc(sizeof(float) * texels), *raw = malloc(sizeof(float) * texels);
	unsigned char *ok = malloc(texels), *rgb = malloc(3 * texels);
	char path[MAXIMUM_PATH];
	long ok_count = 0;
	int role_index;
	static const int roles[3] = { _role_ao, _role_roughness, _role_metallic };

	if (!normal || !image || !values || !raw || !ok || !rgb)
		goto finished;
	sample_role(run->pak, mcc, look, _role_normal, _sample_rgba, normal);
	for (texel = 0; texel < texels; texel++)
	{
		const struct piece *piece = &run->pieces[look->our_piece[texel]];
		const double *rotation = piece->rotation;
		const long *ours, *theirs;
		const float *w;
		double nx, ny, nz, n[3], tangent[3], bitangent[3], model[3], frame[3][3], e1[3], e2[3], du1, dv1, du2, dv2, det;
		double b[3], tx[3], tz[3], ty[3], out[3];
		int axis, largest;

		ok[texel] = look->state[texel] == _texel_found && !isnan(normal[4 * texel]);
		if (!ok[texel])
			continue;
		ok_count++;
		nx = normal[4 * texel] / 127.5 - 1;
		ny = normal[4 * texel + 1] / 127.5 - 1;
		nz = sqrt(clampd(1 - nx * nx - ny * ny, 0, 1));
		/* MCC's frame: its vertices' tangent and normal, the bitangent
		cross(normal, tangent) times their sign, into our model's space */
		theirs = mcc->mesh.triangles + 3 * look->mcc_triangle[texel];
		b[0] = look->mcc_bary[2 * texel];
		b[1] = look->mcc_bary[2 * texel + 1];
		b[2] = 1.0 - b[0] - b[1];
		/* (the prototype's float32 barycentrics) */
		b[2] = (float)b[2];
		for (axis = 0; axis < 3; axis++)
		{
			tx[axis] = b[0] * mcc->mesh.tangents[3 * theirs[0] + axis] + b[1] * mcc->mesh.tangents[3 * theirs[1] + axis] +
				b[2] * mcc->mesh.tangents[3 * theirs[2] + axis];
			tz[axis] = b[0] * mcc->mesh.normals[3 * theirs[0] + axis] + b[1] * mcc->mesh.normals[3 * theirs[1] + axis] +
				b[2] * mcc->mesh.normals[3 * theirs[2] + axis];
		}
		largest = b[0] >= b[1] && b[0] >= b[2] ? 0 : b[1] >= b[2] ? 1 : 2;
		unit3(tx);
		unit3(tz);
		ty[0] = (tz[1] * tx[2] - tz[2] * tx[1]) * mcc->mesh.signs[theirs[largest]];
		ty[1] = (tz[2] * tx[0] - tz[0] * tx[2]) * mcc->mesh.signs[theirs[largest]];
		ty[2] = (tz[0] * tx[1] - tz[1] * tx[0]) * mcc->mesh.signs[theirs[largest]];
		for (axis = 0; axis < 3; axis++)
		{
			/* (a direction v of MCC's space is R^T v in ours) */
			model[axis] = 0;
			{
				int row;
				double v[3];

				for (row = 0; row < 3; row++)
					v[row] = nx * tx[row] + ny * ty[row] + nz * tz[row];
				for (row = 0; row < 3; row++)
					model[axis] += rotation[3 * row + axis] * v[row];
			}
		}
		/* ours: the vertex normal, the tangent along +u and the bitangent along +v */
		ours = piece->triangles + 3 * look->our_triangle[texel];
		w = look->our_bary + 3 * texel;
		for (axis = 0; axis < 3; axis++)
		{
			n[axis] = w[0] * piece->normals[3 * ours[0] + axis] + w[1] * piece->normals[3 * ours[1] + axis] +
				w[2] * piece->normals[3 * ours[2] + axis];
			e1[axis] = piece->xyz[3 * ours[1] + axis] - piece->xyz[3 * ours[0] + axis];
			e2[axis] = piece->xyz[3 * ours[2] + axis] - piece->xyz[3 * ours[0] + axis];
		}
		unit3(n);
		du1 = piece->uv[2 * ours[1]] - piece->uv[2 * ours[0]];
		dv1 = piece->uv[2 * ours[1] + 1] - piece->uv[2 * ours[0] + 1];
		du2 = piece->uv[2 * ours[2]] - piece->uv[2 * ours[0]];
		dv2 = piece->uv[2 * ours[2] + 1] - piece->uv[2 * ours[0] + 1];
		det = du1 * dv2 - du2 * dv1;
		if (fabs(det) < 1e-12)
			det = 1e-12;
		for (axis = 0; axis < 3; axis++)
		{
			tangent[axis] = (e1[axis] * dv2 - e2[axis] * dv1) / det;
			bitangent[axis] = (e2[axis] * du1 - e1[axis] * du2) / det;
		}
		{
			double along = dot3(n, tangent);

			for (axis = 0; axis < 3; axis++)
				tangent[axis] -= n[axis] * along;
			unit3(tangent);
			along = dot3(n, bitangent);
			{
				double across = dot3(tangent, bitangent);

				for (axis = 0; axis < 3; axis++)
					bitangent[axis] -= n[axis] * along + tangent[axis] * across;
			}
			unit3(bitangent);
		}
		memcpy(frame[0], tangent, sizeof(tangent));
		memcpy(frame[1], bitangent, sizeof(bitangent));
		memcpy(frame[2], n, sizeof(n));
		for (axis = 0; axis < 3; axis++)
			out[axis] = dot3(frame[axis], model);
		unit3(out);
		out[1] = -out[1];
		for (axis = 0; axis < 3; axis++)
			image[3 * texel + axis] = (float)((out[axis] * 0.5 + 0.5) * 255);
	}
	if (ok_count > 100)
	{
		dilate(image, ok, width, height, 3, PAD_STEPS);
		for (texel = 0; texel < texels; texel++)
		{
			int axis;

			for (axis = 0; axis < 3; axis++)
			{
				float value = ok[texel] ? image[3 * texel + axis] : (axis == 2 ? 255.0f : 128.0f);

				rgb[3 * texel + axis] = (unsigned char)clampd(round_even(value), 0, 255);
			}
		}
		tag_path(path, sizeof(path), run->options->out_folder, source->name, ".normal.png");
		if (png_write(path, rgb, width, height))
			run->written++;
	}
	/* the occlusion, roughness and metalness: the classic look (no
	occlusion, sharp reflection, no metal) as the baseline, MCC's maps only
	adding to it where they stand out */
	for (role_index = 0; role_index < 3; role_index++)
	{
		int role = roles[role_index];
		size_t count = 0;
		double low = 0, scale = 1;
		float fallback = role == _role_ao ? 255.0f : 0.0f;

		sample_role(run->pak, mcc, look, role, _sample_red, values);
		for (texel = 0; texel < texels; texel++)
		{
			ok[texel] = look->state[texel] == _texel_found && !isnan(values[texel]);
			if (ok[texel])
				raw[count++] = values[texel];
		}
		if (role == _role_ao)
		{
			double top = percentile(raw, count, 0.9);

			scale = 255.0 / (top > 1 ? top : 1);
		}
		else if (role == _role_roughness)
		{
			low = percentile(raw, count, 0.5);
			scale = 0.5;
		}
		else
		{
			low = 128;
			scale = 1.0;
		}
		for (texel = 0; texel < texels; texel++)
			if (ok[texel])
				values[texel] = (float)clampd((values[texel] - low) * scale, 0, 255);
		dilate(values, ok, width, height, 1, PAD_STEPS);
		for (texel = 0; texel < texels; texel++)
			rgb[3 * texel + role_index] = (unsigned char)clampd(round_even(ok[texel] ? values[texel] : fallback), 0, 255);
	}
	tag_path(path, sizeof(path), run->options->out_folder, source->name, ".orm.png");
	if (png_write(path, rgb, width, height))
		run->written++;

finished:
	free(normal);
	free(image);
	free(values);
	free(raw);
	free(ok);
	free(rgb);
}

static void bake_diffuse(struct run *run, const struct mcc_mesh *mcc, const struct grid *grid, struct bitmap_source *source)
{
	long width, height;
	size_t texels, texel;
	struct lookup look;
	float *diffuse = NULL, *out = NULL, *classic = NULL;
	unsigned char *baked = NULL, *have = NULL;
	long drawn = 0, baked_count = 0, far_count = 0;
	double mcc_mean[3] = { 0, 0, 0 }, ours_mean[3] = { 0, 0, 0 }, mcc_var[3] = { 0, 0, 0 }, ours_var[3] = { 0, 0, 0 };
	int channel, format;

	if (source->count != 1 || source->type != 0)
	{
		run_log(run, "    %s: %ld bitmaps of type %d, left out", source->name, source->count, source->type);
		return;
	}
	if (!(classic = bitmap_rgba(source)))
	{
		run_log(run, "    %s: its format %d is not read, left out", source->name, source->format);
		return;
	}
	bake_size(source->width, source->height, run->options->max_size, &width, &height);
	texels = (size_t)width * (size_t)height;
	if (!lookup_make(run, grid, run->pieces, source->pieces, source->piece_count, width, height, run->options->threads,
		&look))
	{
		free(classic);
		return;
	}
	diffuse = malloc(sizeof(float) * 4 * texels);
	baked = calloc(texels, 1);
	have = calloc(texels, 1);
	if (!diffuse || !baked || !have)
		goto finished;
	sample_role(run->pak, mcc, &look, _role_diffuse, _sample_rgba, diffuse);
	for (texel = 0; texel < texels; texel++)
	{
		baked[texel] = !isnan(diffuse[4 * texel]);
		drawn += look.state[texel] != _texel_none;
		baked_count += baked[texel];
		far_count += look.state[texel] == _texel_far;
	}
	drawn = drawn ? drawn : 1;
	if ((double)baked_count / drawn < LEAST_COVERAGE)
	{
		run_log(run, "    %s: %ldx%ld -> %ldx%ld, baked %.1f%% of %ld texels drawn (nothing close %.1f%%) -- too little, "
			"left out", source->name, source->width, source->height, width, height, 100.0 * baked_count / drawn, drawn,
			100.0 * far_count / drawn);
		goto finished;
	}
	if (run_progress(run, 0.7f) || !(out = classic_enlarged(classic, source->width, source->height, width, height)))
		goto finished;
	/* MCC's paint is lighter and greyer than ours by design: each channel's
	mean and contrast over the baked texels set to ours there */
	for (texel = 0; texel < texels; texel++)
		if (baked[texel])
			for (channel = 0; channel < 3; channel++)
			{
				mcc_mean[channel] += diffuse[4 * texel + channel];
				ours_mean[channel] += out[4 * texel + channel];
			}
	for (channel = 0; channel < 3; channel++)
	{
		mcc_mean[channel] /= baked_count;
		ours_mean[channel] /= baked_count;
	}
	for (texel = 0; texel < texels; texel++)
		if (baked[texel])
			for (channel = 0; channel < 3; channel++)
			{
				double m = diffuse[4 * texel + channel] - mcc_mean[channel], o = out[4 * texel + channel] - ours_mean[channel];

				mcc_var[channel] += m * m;
				ours_var[channel] += o * o;
			}
	for (texel = 0; texel < texels; texel++)
	{
		for (channel = 0; channel < 3; channel++)
		{
			double mcc_sd = sqrt(mcc_var[channel] / baked_count), ours_sd = sqrt(ours_var[channel] / baked_count);

			/* (the adjusted colors in diffuse, 0 where not baked) */
			diffuse[4 * texel + channel] = baked[texel] ? (float)((diffuse[4 * texel + channel] - mcc_mean[channel]) *
				(ours_sd / (mcc_sd > 1e-3 ? mcc_sd : 1e-3)) + ours_mean[channel]) : 0.0f;
		}
		diffuse[4 * texel + 3] = 0;
	}
	memcpy(have, baked, texels);
	dilate(diffuse, have, width, height, 4, PAD_STEPS);
	for (texel = 0; texel < texels; texel++)
		if (have[texel] && look.state[texel] != _texel_far && !(look.state[texel] == _texel_found && !baked[texel]))
			for (channel = 0; channel < 3; channel++)
				out[4 * texel + channel] = diffuse[4 * texel + channel];
	format = bitmap_write(run, source, out, width, height, bitmap_has_alpha(source));
	run_log(run, "    %s: %ldx%ld -> %ldx%ld, baked %.1f%% of %ld texels drawn (nothing close %.1f%%), %s", source->name,
		source->width, source->height, width, height, 100.0 * baked_count / drawn, drawn, 100.0 * far_count / drawn,
		format == 5 ? "DXT5" : format == 1 ? "DXT1" : "not written");
	if (format && run_progress(run, 0.8f) == 0)
		extras_write(run, mcc, &look, source);

finished:
	lookup_free(&look);
	free(diffuse);
	free(out);
	free(classic);
	free(baked);
	free(have);
}

static void bake_multipurpose(struct run *run, const struct mcc_mesh *mcc, const struct grid *grid,
	struct bitmap_source *source)
{
	static const int channels[3] = { 0, 1, 3 };
	static const char *const labels[4] = { "spec", "self-illum", "change color", "detail" };
	long width, height;
	size_t texels, texel;
	struct lookup look;
	float *classic_small = NULL, *classic = NULL, *out = NULL, *mp = NULL, *metallic = NULL, *roughness = NULL;
	float *ao = NULL, *emissive = NULL, *values = NULL, *target = NULL;
	unsigned char *found = NULL, *ok = NULL;
	double *fitted = NULL;
	long found_count = 0, has_mp = 0;
	int index, any = 0;
	char notes[1024] = "";

	if (source->count != 1 || source->type != 0 || !(classic_small = bitmap_rgba(source)))
	{
		run_log(run, "    %s (multipurpose): left out", source->name);
		free(classic_small);
		return;
	}
	bake_size(source->width, source->height, run->options->max_size, &width, &height);
	texels = (size_t)width * (size_t)height;
	if (!lookup_make(run, grid, run->pieces, source->pieces, source->piece_count, width, height, run->options->threads,
		&look))
	{
		free(classic_small);
		return;
	}
	classic = classic_enlarged(classic_small, source->width, source->height, width, height);
	out = malloc(sizeof(float) * 4 * texels);
	mp = malloc(sizeof(float) * 4 * texels);
	metallic = malloc(sizeof(float) * texels);
	roughness = malloc(sizeof(float) * texels);
	ao = malloc(sizeof(float) * texels);
	emissive = malloc(sizeof(float) * texels);
	values = malloc(sizeof(float) * texels);
	target = malloc(sizeof(float) * texels);
	found = malloc(texels);
	ok = malloc(texels);
	fitted = malloc(sizeof(double) * texels);
	if (!classic || !out || !mp || !metallic || !roughness || !ao || !emissive || !values || !target || !found || !ok ||
		!fitted)
	{
		goto finished;
	}
	memcpy(out, classic, sizeof(float) * 4 * texels);
	sample_role(run->pak, mcc, &look, _role_multipurpose, _sample_rgba, mp);
	sample_role(run->pak, mcc, &look, _role_metallic, _sample_red, metallic);
	sample_role(run->pak, mcc, &look, _role_roughness, _sample_red, roughness);
	sample_role(run->pak, mcc, &look, _role_ao, _sample_red, ao);
	sample_role(run->pak, mcc, &look, _role_emissive, _sample_mean, emissive);
	for (texel = 0; texel < texels; texel++)
	{
		found[texel] = look.state[texel] == _texel_found;
		found_count += found[texel];
		has_mp += found[texel] && !isnan(mp[4 * texel]);
	}
	for (index = 0; index < 3; index++)
	{
		int channel = channels[index], candidate, best = -1;
		struct fit best_fit;
		float most = 0;
		long ok_count = 0, radius;
		double *blurred;
		size_t length = strlen(notes);

		for (texel = 0; texel < texels; texel++)
		{
			target[texel] = classic[4 * texel + channel];
			if (found[texel] && target[texel] > most)
				most = target[texel];
		}
		if (most < 16 && channel != 0)
		{
			snprintf(notes + length, sizeof(notes) - length, "; %s empty in ours, kept", labels[channel]);
			continue;
		}
		/* the candidates: MCC's multipurpose R, G and B; for the specular
		mask its roughness, metalness and occlusion; for self-illumination
		its emissive */
		for (candidate = 0; candidate < 4; candidate++)
		{
			struct fit fit;
			long count = 0;

			if (candidate < 3 && has_mp <= 100)
				continue;
			if (candidate == 3 && channel == 3)
				continue;
			for (texel = 0; texel < texels; texel++)
			{
				float value;

				if (candidate < 3)
					value = mp[4 * texel + candidate];
				else if (channel == 0)
				{
					float rough = isnan(roughness[texel]) ? NAN : 1 - roughness[texel] / 255;
					float metal = isnan(metallic[texel]) ? 0.0f : metallic[texel] / 255;
					float occlusion = isnan(ao[texel]) ? 1.0f : ao[texel] / 255;

					value = rough * (0.5f + 0.5f * metal) * occlusion;
				}
				else
					value = emissive[texel];
				ok[texel] = found[texel] && !isnan(value);
				count += ok[texel];
				values[texel] = value;
			}
			if (fit_to(values, target, 1, ok, texels, &fit) && fit.correlation > 0.3 && count >= 0.5 * found_count &&
				(best < 0 || fit.correlation > best_fit.correlation))
			{
				best = candidate;
				best_fit = fit;
			}
		}
		if (best < 0)
		{
			snprintf(notes + length, sizeof(notes) - length, "; %s kept ours (no MCC map agrees)", labels[channel]);
			continue;
		}
		/* our mask kept as it is, with MCC's fine detail (its fitted map
		less its own blur, about a classic texel and a half across) on top */
		for (texel = 0; texel < texels; texel++)
		{
			float value;

			if (best < 3)
				value = mp[4 * texel + best];
			else if (channel == 0)
			{
				float rough = isnan(roughness[texel]) ? NAN : 1 - roughness[texel] / 255;
				float metal = isnan(metallic[texel]) ? 0.0f : metallic[texel] / 255;
				float occlusion = isnan(ao[texel]) ? 1.0f : ao[texel] / 255;

				value = rough * (0.5f + 0.5f * metal) * occlusion;
			}
			else
				value = emissive[texel];
			ok[texel] = found[texel] && !isnan(value);
			ok_count += ok[texel];
			fitted[texel] = ok[texel] ? (value - best_fit.values_mean) * best_fit.slope + best_fit.target_mean : 0.0;
		}
		radius = (long)round_even(1.5 * (double)width / (double)source->width);
		radius = radius < 2 ? 2 : radius;
		blurred = masked_blur(fitted, ok, width, height, radius);
		if (!blurred)
			goto finished;
		for (texel = 0; texel < texels; texel++)
			values[texel] = (float)clampd(target[texel] + (ok[texel] ? fitted[texel] - blurred[texel] : 0.0), 0, 255);
		free(blurred);
		dilate(values, ok, width, height, 1, PAD_STEPS);
		for (texel = 0; texel < texels; texel++)
			if (ok[texel] && look.state[texel] != _texel_far)
				out[4 * texel + channel] = values[texel];
		snprintf(notes + length, sizeof(notes) - length, "; %s: ours with %s's detail (r %.2f)", labels[channel],
			best < 3 ? (best == 0 ? "MCC mp R" : best == 1 ? "MCC mp G" : "MCC mp B") :
			channel == 0 ? "(1-rough)(0.5+0.5 metal) AO" : "MCC emissive", best_fit.correlation);
		any = 1;
		(void)ok_count;
	}
	if (!any)
	{
		run_log(run, "    %s (multipurpose): %ldx%ld -> %ldx%ld%s -- nothing from MCC, left out", source->name,
			source->width, source->height, width, height, notes);
		goto finished;
	}
	{
		int format = bitmap_write(run, source, out, width, height, bitmap_has_alpha(source));

		run_log(run, "    %s (multipurpose): %ldx%ld -> %ldx%ld%s, %s", source->name, source->width, source->height,
			width, height, notes, format == 5 ? "DXT5" : format == 1 ? "DXT1" : "not written");
	}

finished:
	lookup_free(&look);
	free(classic_small);
	free(classic);
	free(out);
	free(mp);
	free(metallic);
	free(roughness);
	free(ao);
	free(emissive);
	free(values);
	free(target);
	free(found);
	free(ok);
	free(fitted);
}

static int wanted(const char *only, const char *mesh)
{
	char words[512], lower_mesh[128], *word, *rest;

	if (!only || !*only)
		return 1;
	snprintf(words, sizeof(words), "%s", only);
	lower_into(lower_mesh, sizeof(lower_mesh), mesh);
	for (word = words; word && *word; word = rest)
	{
		char lower_word[128];

		rest = strchr(word, ',');
		if (rest)
			*rest++ = 0;
		lower_into(lower_word, sizeof(lower_word), word);
		if (*lower_word && strstr(lower_mesh, lower_word))
			return 1;
	}
	return 0;
}

/* the pieces' bitmaps, each from the first map that has it, and its pixels */
static int bitmaps_read(struct run *run)
{
	int map;

	for (map = 0; map < run->map_count; map++)
	{
		struct xbox_map_range ranges[MAXIMUM_BITMAPS];
		int count = 0, index;

		for (index = 0; index < run->bitmap_count; index++)
		{
			struct bitmap_source *source = &run->bitmaps[index];

			if (source->map != map || source->pixels)
				continue;
			source->pixels = malloc(source->pixels_size);
			if (!source->pixels)
				return 0;
			ranges[count].offset = source->pixels_offset;
			ranges[count].size = source->pixels_size;
			ranges[count].destination = source->pixels;
			count++;
		}
		if (count && !xbox_map_ranges_read(run->maps[map].path, ranges, count))
			return 0;
	}
	for (map = 0; map < run->bitmap_count; map++)
		run->bitmaps[map].crc = crc32(0, run->bitmaps[map].pixels, (uInt)run->bitmaps[map].pixels_size);
	return 1;
}

static void run_mesh_free(struct run *run)
{
	int index;

	for (index = 0; index < run->piece_count; index++)
		piece_free(&run->pieces[index]);
	run->piece_count = 0;
	for (index = 0; index < run->bitmap_count; index++)
		free(run->bitmaps[index].pixels);
	run->bitmap_count = 0;
}

static int compare_names(const void *a, const void *b)
{
	const struct bitmap_source *x = a, *y = b;

	return strcmp(x->name, y->name);
}

/* one preview mesh's bitmaps: its fits are first .. last - 1 */
static void bake_mesh(struct run *run, unsigned int first, unsigned int last)
{
	const struct mcc_import_fit *fit = &mcc_import_fits[first];
	struct mcc_mesh *mcc = calloc(1, sizeof(*mcc));
	struct grid grid;
	const char *reason;
	long asset = ue4_pak_find_asset(run->pak, MESH_ROOT, fit->mesh), section;
	unsigned int model;
	double near = 0;
	int index;

	memset(&grid, 0, sizeof(grid));
	if (!mcc)
		return;
	if (asset < 0 || !ue4_mesh_read(run->pak, asset, &mcc->mesh, &reason))
	{
		run_log(run, "%s: not read (%s)", fit->mesh, asset < 0 ? "not in the pak" : reason);
		free(mcc);
		return;
	}
	if (mcc->mesh.vertex_count != fit->vertices || mcc->mesh.triangle_count != fit->triangles)
	{
		run_log(run, "%s: %ld vertices and %ld triangles, not the %ld and %ld its fit was made for: left out", fit->mesh,
			mcc->mesh.vertex_count, mcc->mesh.triangle_count, fit->vertices, fit->triangles);
		ue4_mesh_free(&mcc->mesh);
		free(mcc);
		return;
	}
	for (section = 0; section < mcc->mesh.section_count && section < MAXIMUM_SECTIONS; section++)
		if (!material_roles(run->pak, &run->textures, mcc->mesh.sections[section].material, mcc->roles[section]) &&
			fit->fallback[0])
		{
			material_roles(run->pak, &run->textures, fit->fallback, mcc->roles[section]);
		}
	run_log(run, "%s: %ld vertices, %ld triangles", fit->mesh, mcc->mesh.vertex_count, mcc->mesh.triangle_count);
	for (model = first; model < last; model++)
	{
		const struct mcc_import_fit *entry = &mcc_import_fits[model];
		int map = map_get(run, entry->map), count, piece;
		long tag;
		unsigned long group, definition;
		const char *name;

		if (map < 0 || (tag = tag_find(&run->maps[map], GROUP_MODEL, entry->model)) < 0 ||
			!instance_get(&run->maps[map], tag, &group, &name, &definition))
		{
			run_log(run, "  %s: not in %s", entry->model, entry->map);
			continue;
		}
		count = model_pieces(&run->maps[map], definition, run->pieces + run->piece_count, MAXIMUM_PIECES - run->piece_count);
		for (piece = run->piece_count; piece < run->piece_count + count; piece++)
		{
			struct piece *made = &run->pieces[piece];
			struct bitmap_source *source;

			made->scale = entry->scale;
			memcpy(made->rotation, entry->rotation, sizeof(made->rotation));
			memcpy(made->shift, entry->shift, sizeof(made->shift));
			made->near = entry->near;
			made->map = map;
			near = entry->near > near ? entry->near : near;
			if ((source = bitmap_get(run, map, made->base, 0)) && source->piece_count < MAXIMUM_PIECES)
				source->pieces[source->piece_count++] = piece;
			if ((source = bitmap_get(run, map, made->multipurpose, 1)) && source->piece_count < MAXIMUM_PIECES)
				source->pieces[source->piece_count++] = piece;
		}
		run->piece_count += count;
		run_log(run, "  %s (%s): %d parts", entry->model, entry->map, count);
	}
	qsort(run->bitmaps, (size_t)run->bitmap_count, sizeof(run->bitmaps[0]), compare_names);
	if (run->bitmap_count && bitmaps_read(run) && grid_make(&grid, &mcc->mesh, near))
	{
		int done = 0, pass;

		/* the diffuse maps first, then the multipurpose maps (as the prototype) */
		for (pass = 0; pass < 2 && !run->canceled; pass++)
			for (index = 0; index < run->bitmap_count && !run->canceled; index++)
			{
				struct bitmap_source *source = &run->bitmaps[index];

				if (source->multipurpose != pass)
					continue;
				run->within_bitmap_base = (float)done / (float)run->bitmap_count;
				run->within_bitmap_share = 1.0f / (float)run->bitmap_count;
				if (pass == 0)
					bake_diffuse(run, mcc, &grid, source);
				else
					bake_multipurpose(run, mcc, &grid, source);
				done++;
			}
	}
	grid_free(&grid);
	run_mesh_free(run);
	ue4_mesh_free(&mcc->mesh);
	free(mcc);
}

/* ---------- a layout: MCC's textures already in our bitmap's UV layout */

/* the visor hole: where MCC's color's alpha is under this, grown this many
texels (at 2048) over its blended edge */
#define VISOR_ALPHA 16
#define VISOR_GROWTH 4

/* a texture under the layout's folder (of MCC's Customization folder), or -1 */
static long layout_asset(const struct ue4_pak *pak, const struct mcc_import_layout *layout, const char *name)
{
	char root[MAXIMUM_PATH];

	snprintf(root, sizeof(root), "%s%s", CUSTOMIZATION_ROOT, layout->root);
	return ue4_pak_find_asset(pak, root, name);
}

/* a texture (8-bit RGBA) at our bitmap's size, RGBA floats: each of our
texels takes MCC's at the same UV, the texels' centers lined up (at the same
size, a copy) */
static void layout_resample(const unsigned char *image, long image_width, long image_height, long width, long height,
	float *out)
{
	long x, y;

	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
		{
			double value[4];
			int channel;

			bilinear_wrap(image, image_width, image_height, (x + 0.5) * image_width / width,
				(y + 0.5) * image_height / height, value);
			for (channel = 0; channel < 4; channel++)
				out[4 * ((size_t)y * (size_t)width + (size_t)x) + channel] = (float)value[channel];
		}
}

/* our bitmap's islands: the texels the model's parts draw; 0 if out of memory */
static int layout_islands(const struct piece *pieces, int count, long width, long height, unsigned char *island)
{
	size_t texels = (size_t)width * (size_t)height, texel;
	long *tri_of = malloc(sizeof(long) * texels);
	float *bary = malloc(sizeof(float) * 3 * texels);
	int piece;

	if (!tri_of || !bary)
	{
		free(tri_of);
		free(bary);
		return 0;
	}
	memset(island, 0, texels);
	for (piece = 0; piece < count; piece++)
	{
		raster(&pieces[piece], width, height, tri_of, bary);
		for (texel = 0; texel < texels; texel++)
			island[texel] |= tri_of[texel] >= 0;
	}
	free(tri_of);
	free(bary);
	return 1;
}

/* the visor: MCC's color's alpha hole (MCC paints it white, where ours has
the visor's own texels), grown over its edge, on our islands only; how many
texels, or -1 if out of memory */
static long layout_visor(const float *color, const unsigned char *island, long width, long height, unsigned char *visor)
{
	size_t texels = (size_t)width * (size_t)height, texel;
	unsigned char *grown = malloc(texels);
	long steps = (long)round_even(VISOR_GROWTH * (double)width / 2048.0), step, x, y, count = 0;

	if (!grown)
		return -1;
	steps = steps < 1 ? 1 : steps;
	for (texel = 0; texel < texels; texel++)
		visor[texel] = color[4 * texel + 3] < VISOR_ALPHA;
	for (step = 0; step < steps; step++)
	{
		memcpy(grown, visor, texels);
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
			{
				size_t at = (size_t)y * (size_t)width + (size_t)x;

				if (!visor[at] && ((y > 0 && visor[at - (size_t)width]) || (y + 1 < height && visor[at + (size_t)width]) ||
					(x > 0 && visor[at - 1]) || (x + 1 < width && visor[at + 1])))
				{
					grown[at] = 1;
				}
			}
		memcpy(visor, grown, texels);
	}
	for (texel = 0; texel < texels; texel++)
	{
		visor[texel] = visor[texel] && island[texel];
		count += visor[texel];
	}
	free(grown);
	return count;
}

/* MCC's color (RGBA floats) into out (ours enlarged, RGBA floats): MCC's
paint is lighter and greyer than ours by design, so each channel's mean and
contrast over our islands (less the visor) set to ours there; our texels kept
on the visor; 0 if too few texels to match */
static int layout_color(const float *color, float *out, const unsigned char *island, const unsigned char *visor,
	size_t texels)
{
	double n = 0, mcc_mean[3] = { 0, 0, 0 }, ours_mean[3] = { 0, 0, 0 }, mcc_var[3] = { 0, 0, 0 }, ours_var[3] = { 0, 0, 0 };
	size_t texel;
	int channel;

	for (texel = 0; texel < texels; texel++)
		if (island[texel] && !visor[texel])
		{
			n++;
			for (channel = 0; channel < 3; channel++)
			{
				mcc_mean[channel] += color[4 * texel + channel];
				ours_mean[channel] += out[4 * texel + channel];
			}
		}
	if (n < 100)
		return 0;
	for (channel = 0; channel < 3; channel++)
	{
		mcc_mean[channel] /= n;
		ours_mean[channel] /= n;
	}
	for (texel = 0; texel < texels; texel++)
		if (island[texel] && !visor[texel])
			for (channel = 0; channel < 3; channel++)
			{
				double m = color[4 * texel + channel] - mcc_mean[channel], o = out[4 * texel + channel] - ours_mean[channel];

				mcc_var[channel] += m * m;
				ours_var[channel] += o * o;
			}
	for (channel = 0; channel < 3; channel++)
	{
		double mcc_sd = sqrt(mcc_var[channel] / n), ours_sd = sqrt(ours_var[channel] / n);
		double slope = ours_sd / (mcc_sd > 1e-3 ? mcc_sd : 1e-3);

		for (texel = 0; texel < texels; texel++)
			if (!visor[texel])
				out[4 * texel + channel] = (float)((color[4 * texel + channel] - mcc_mean[channel]) * slope + ours_mean[channel]);
	}
	return 1;
}

/* MCC's DirectX normal map (red and green; green toward the picture's
bottom) in our convention (green toward its top), flat on the visor: 8-bit
RGB */
static void layout_normal(const float *normal, const unsigned char *visor, size_t texels, unsigned char *rgb)
{
	size_t texel;

	for (texel = 0; texel < texels; texel++)
	{
		double n[3] = { 0, 0, 1 };
		int axis;

		if (!visor[texel])
		{
			n[0] = normal[4 * texel] / 127.5 - 1;
			n[1] = -(normal[4 * texel + 1] / 127.5 - 1);
			n[2] = sqrt(clampd(1 - n[0] * n[0] - n[1] * n[1], 0, 1));
			unit3(n);
		}
		for (axis = 0; axis < 3; axis++)
			rgb[3 * texel + axis] = (unsigned char)clampd(round_even((n[axis] * 0.5 + 0.5) * 255), 0, 255);
	}
}

/* the occlusion, roughness (the color's alpha) and metalness (RGBA floats,
red read; occlusion or metallic NULL: none) on the classic look's baseline
(no occlusion, sharp reflection, no metal) as the fitted bake makes them:
the occlusion's 90th percentile over the islands full, the roughness above
its median halved, the metalness above half; the visor the baseline. 8-bit
RGB; 0 if out of memory */
static int layout_orm(const float *color, const float *occlusion, const float *metallic, const unsigned char *island,
	const unsigned char *visor, size_t texels, unsigned char *rgb)
{
	float *raw = malloc(sizeof(float) * (texels ? texels : 1));
	size_t texel;
	int role;

	if (!raw)
		return 0;
	for (role = 0; role < 3; role++)
	{
		const float *values = role == 0 ? occlusion : role == 1 ? color : metallic;
		int channel = role == 1 ? 3 : 0;
		float fallback = role == 0 ? 255.0f : 0.0f;
		double low = 0, scale = 1;
		size_t count = 0;

		for (texel = 0; values && texel < texels; texel++)
			if (island[texel] && !visor[texel])
				raw[count++] = values[4 * texel + channel];
		if (role == 0)
		{
			double top = percentile(raw, count, 0.9);

			scale = 255.0 / (top > 1 ? top : 1);
		}
		else if (role == 1)
		{
			low = percentile(raw, count, 0.5);
			scale = 0.5;
		}
		else
		{
			low = 128;
		}
		for (texel = 0; texel < texels; texel++)
			rgb[3 * texel + role] = (unsigned char)(values && !visor[texel] ?
				clampd(round_even(clampd((values[4 * texel + channel] - low) * scale, 0, 255)), 0, 255) : fallback);
	}
	free(raw);
	return 1;
}

/* one layout's bitmap and its material maps (our multipurpose map is kept) */
static void bake_layout(struct run *run, const struct mcc_import_layout *layout)
{
	const char *names[4] = { layout->color, layout->normal, layout->occlusion, layout->metallic };
	float *textures[4] = { NULL, NULL, NULL, NULL }, *classic = NULL, *out = NULL;
	unsigned char *island = NULL, *visor = NULL, *rgb = NULL;
	struct bitmap_source *source = NULL;
	int map = map_get(run, layout->map), count, role, format;
	long tag, width, height, visor_count, island_count = 0;
	unsigned long group, definition;
	const char *name;
	size_t texels, texel;
	char path[MAXIMUM_PATH];

	if (map < 0 || (tag = tag_find(&run->maps[map], GROUP_MODEL, layout->model)) < 0 ||
		!instance_get(&run->maps[map], tag, &group, &name, &definition))
	{
		run_log(run, "%s: not in %s", layout->model, layout->map);
		return;
	}
	run->piece_count = count = model_pieces(&run->maps[map], definition, run->pieces, MAXIMUM_PIECES);
	run_log(run, "%s (%s): %d parts, MCC's textures in its layout (%s)", layout->model, layout->map, count, layout->root);
	if (!count || (tag = tag_find(&run->maps[map], GROUP_BITMAP, layout->bitmap)) < 0 ||
		!(source = bitmap_get(run, map, tag, 0)) || !bitmaps_read(run))
	{
		run_log(run, "  %s: not in %s", layout->bitmap, layout->map);
		goto finished;
	}
	if (source->count != 1 || source->type != 0 || !(classic = bitmap_rgba(source)))
	{
		run_log(run, "    %s: %ld bitmaps of type %d, format %d, left out", source->name, source->count, source->type,
			source->format);
		goto finished;
	}
	bake_size(source->width, source->height, run->options->max_size, &width, &height);
	texels = (size_t)width * (size_t)height;
	island = malloc(texels);
	visor = malloc(texels);
	rgb = malloc(3 * texels);
	if (!island || !visor || !rgb || !layout_islands(run->pieces, count, width, height, island))
		goto finished;
	for (role = 0; role < 4; role++)
	{
		unsigned char *image;
		long image_width = 0, image_height = 0;

		if (!names[role][0])
			continue;
		if (!(image = texture_load(run->pak, layout_asset(run->pak, layout, names[role]), &image_width, &image_height)))
		{
			run_log(run, "    %s: not read%s", names[role], role == 0 ? ", left out" : "");
			if (role == 0)
				goto finished;
			continue;
		}
		textures[role] = malloc(sizeof(float) * 4 * texels);
		if (textures[role])
			layout_resample(image, image_width, image_height, width, height, textures[role]);
		free(image);
		if (!textures[role] || run_progress(run, 0.15f * (float)(role + 1)))
			goto finished;
	}
	if (!textures[0] || (visor_count = layout_visor(textures[0], island, width, height, visor)) < 0 ||
		!(out = classic_enlarged(classic, source->width, source->height, width, height)))
	{
		goto finished;
	}
	for (texel = 0; texel < texels; texel++)
		island_count += island[texel];
	if (!layout_color(textures[0], out, island, visor, texels))
	{
		run_log(run, "    %s: too few texels on its islands (%ld), left out", source->name, island_count);
		goto finished;
	}
	format = bitmap_write(run, source, out, width, height, bitmap_has_alpha(source));
	run_log(run, "    %s: %ldx%ld -> %ldx%ld, islands %.1f%%, ours kept on the visor's %ld texels, %s", source->name,
		source->width, source->height, width, height, 100.0 * (double)island_count / (double)texels, visor_count,
		format == 5 ? "DXT5" : format == 1 ? "DXT1" : "not written");
	if (!format || run_progress(run, 0.8f))
		goto finished;
	if (textures[1])
	{
		layout_normal(textures[1], visor, texels, rgb);
		tag_path(path, sizeof(path), run->options->out_folder, source->name, ".normal.png");
		if (png_write(path, rgb, width, height))
			run->written++;
	}
	if (layout_orm(textures[0], textures[2], textures[3], island, visor, texels, rgb))
	{
		tag_path(path, sizeof(path), run->options->out_folder, source->name, ".orm.png");
		if (png_write(path, rgb, width, height))
			run->written++;
	}

finished:
	for (role = 0; role < 4; role++)
		free(textures[role]);
	free(classic);
	free(out);
	free(island);
	free(visor);
	free(rgb);
	run_mesh_free(run);
}

static int layout_wanted(const char *only, const struct mcc_import_layout *layout)
{
	return wanted(only, layout->model) || wanted(only, layout->root);
}

long mcc_bake_textures(const struct ue4_pak *pak, const struct mcc_bake_options *options, char *report,
	size_t report_size)
{
	struct run *run = calloc(1, sizeof(*run));
	unsigned int first, last, meshes = 0, mesh = 0;
	long written;
	int index;

	if (!run)
	{
		snprintf(report, report_size, "out of memory");
		return -1;
	}
	run->pak = pak;
	run->options = options;
	if (!mcc_textures_list(pak, &run->textures))
	{
		snprintf(report, report_size, "out of memory");
		free(run);
		return -1;
	}
	for (first = 0; first < mcc_import_fit_count; first = last)
	{
		for (last = first + 1; last < mcc_import_fit_count && !strcmp(mcc_import_fits[last].mesh, mcc_import_fits[first].mesh);
			last++)
		{
		}
		meshes += wanted(options->only, mcc_import_fits[first].mesh);
	}
	for (index = 0; index < (int)mcc_import_layout_count; index++)
		meshes += layout_wanted(options->only, &mcc_import_layouts[index]);
	for (first = 0; first < mcc_import_fit_count && !run->canceled; first = last)
	{
		for (last = first + 1; last < mcc_import_fit_count && !strcmp(mcc_import_fits[last].mesh, mcc_import_fits[first].mesh);
			last++)
		{
		}
		if (!wanted(options->only, mcc_import_fits[first].mesh))
			continue;
		run->progress_base = (float)mesh / (float)(meshes ? meshes : 1);
		run->progress_share = 1.0f / (float)(meshes ? meshes : 1);
		run->within_bitmap_base = 0;
		run->within_bitmap_share = 1;
		bake_mesh(run, first, last);
		mesh++;
	}
	for (index = 0; index < (int)mcc_import_layout_count && !run->canceled; index++)
	{
		if (!layout_wanted(options->only, &mcc_import_layouts[index]))
			continue;
		run->progress_base = (float)mesh / (float)(meshes ? meshes : 1);
		run->progress_share = 1.0f / (float)(meshes ? meshes : 1);
		run->within_bitmap_base = 0;
		run->within_bitmap_share = 1;
		bake_layout(run, &mcc_import_layouts[index]);
		mesh++;
	}
	for (index = 0; index < run->map_count; index++)
		free(run->maps[index].tags);
	mcc_textures_free(&run->textures);
	written = run->canceled ? -1 : run->written;
	snprintf(report, report_size, "%ld bitmaps (%ld files) from %u MCC models%s", run->bitmaps_written, run->written,
		meshes, run->canceled ? ", canceled" : "");
	free(run);
	return written;
}
