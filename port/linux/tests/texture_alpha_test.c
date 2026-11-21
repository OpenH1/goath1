/*
TEXTURE_ALPHA_TEST.C

A texture's least alpha (xgpu_texture_minimum_alpha, xbox_texture_decode.c),
which the front end drops a draw's alpha test by (d3d8_device.c, "an alpha
test that cannot fail"), against the alphas the decoder gives the same
texels: never more than the least of them, and the least itself where the
format lets it be told. On the build machine, without a GPU or game data
(tools/test_texture_alpha.py). And the levels made to keep an alpha
test's coverage (xgpu_texture_coverage_level, display.alpha_coverage):
each level's share of texels passing within a tolerance of the first's,
where the stored levels lose it; a texture not asked for, and one passing
whole, as stored.
*/

#include "xgpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, ...) \
	do { if (!(condition)) { fprintf(stderr, "%s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); exit(1); } } while (0)

enum
{
	FORMAT_A4R4G4B4 = 0x04, FORMAT_A8R8G8B8 = 0x06, FORMAT_X8R8G8B8 = 0x07, FORMAT_DXT1 = 0x0c, FORMAT_DXT3 = 0x0e,
	FORMAT_DXT5 = 0x0f, FORMAT_LINEAR_A8R8G8B8 = 0x12
};

static uint32_t random_state = 2342;

static uint32_t random_next(void)
{
	random_state = random_state * 1664525u + 1013904223u;
	return random_state >> 8;
}

static struct xgpu_texture_description describe(DWORD format, unsigned long size, unsigned long levels)
{
	struct xgpu_texture_description description;

	memset(&description, 0, sizeof(description));
	description.format = format;
	description.width = description.height = size;
	description.depth = 1;
	description.levels = levels;
	description.linear = format == FORMAT_LINEAR_A8R8G8B8;
	description.compressed = format == FORMAT_DXT1 || format == FORMAT_DXT3 || format == FORMAT_DXT5;
	description.pitch = description.linear ? size * 4 : 0;
	return description;
}

/* the least alpha of the texels as the decoder reads them, every level's */
static int decoded_minimum(const struct xgpu_texture_description *description, const unsigned char *texels)
{
	unsigned long level, least = 255;

	for (level = 0; level < description->levels; level++)
	{
		unsigned long width = xgpu_texture_level_dimension(description->width, level);
		unsigned long height = xgpu_texture_level_dimension(description->height, level), index;
		const unsigned char *source = texels + xgpu_texture_level_offset(description, level);
		DWORD *pixels = calloc(width * height, sizeof(DWORD));

		CHECK(pixels, "no memory");
		if (description->compressed)
			xgpu_texture_decode_dxt(description, level, source, pixels);
		else
			CHECK(xgpu_texture_decode_level(description, level, source, NULL, pixels), "a level not decoded");
		for (index = 0; index < width * height; index++)
		{
			if (pixels[index] >> 24 < least)
				least = pixels[index] >> 24;
		}
		free(pixels);
	}
	return (int)least;
}

/* random texels: all of them, or (shaped) each block's alphas kept away
from nothing, so that the least is not always 0 */
static void fill(const struct xgpu_texture_description *description, unsigned char *texels, unsigned long bytes, int shaped)
{
	unsigned long offset;

	for (offset = 0; offset < bytes; offset++)
		texels[offset] = (unsigned char)random_next();
	if (!shaped)
		return;
	switch (description->format)
	{
	case FORMAT_DXT1:
		/* (four colours: the first the greater, no transparent index) */
		for (offset = 0; offset + 8 <= bytes; offset += 8)
		{
			texels[offset] = texels[offset + 1] = 0xff;
			texels[offset + 2] = texels[offset + 3] = 0x00;
		}
		break;
	case FORMAT_DXT3:
		for (offset = 0; offset + 16 <= bytes; offset += 16)
		{
			int byte;

			for (byte = 0; byte < 8; byte++)
				texels[offset + byte] |= 0x88;
		}
		break;
	case FORMAT_DXT5:
		/* (eight alphas, from the first down to the second) */
		for (offset = 0; offset + 16 <= bytes; offset += 16)
		{
			texels[offset] = (unsigned char)(200 + random_next() % 56);
			texels[offset + 1] = (unsigned char)(100 + random_next() % 100);
		}
		break;
	default:
		for (offset = 3; offset < bytes; offset += 4)
			texels[offset] |= 0x90;
		break;
	}
}

static void format_check(DWORD format, const char *name, int exact)
{
	int trial;

	for (trial = 0; trial < 400; trial++)
	{
		/* (a first level alone, of whole blocks: every texel read is one of
		the texture's; with the levels below, a level smaller than a block
		reads part of one) */
		unsigned long levels = format == FORMAT_LINEAR_A8R8G8B8 || trial % 2 ? 1 : 5;
		struct xgpu_texture_description description = describe(format, 16, levels);
		unsigned long bytes = xgpu_texture_face_size(&description);
		unsigned char *texels = malloc(bytes);
		int minimum, decoded;

		CHECK(texels && bytes, "%s: no texels", name);
		fill(&description, texels, bytes, trial % 4 >= 2);
		minimum = xgpu_texture_minimum_alpha(&description, texels);
		decoded = decoded_minimum(&description, texels);
		CHECK(minimum >= 0 && minimum <= decoded, "%s, trial %d: least alpha %d, the decoder's %d", name, trial, minimum,
			decoded);
		if (exact && levels == 1)
			CHECK(minimum == decoded, "%s, trial %d: least alpha %d, not the decoder's %d", name, trial, minimum, decoded);
		free(texels);
	}
}

/* ---------- levels that keep an alpha test's coverage (xgpu.h)

A size by size texture's first level (ARGB words), its levels below each
the box filter of the one before as a texture's tool makes them, as
stored; then the levels made again by xgpu_texture_coverage_level at a test
passing alphas of at least pass: the share of each level's texels passing,
stored (before) and made (after), into the shares; the count of levels.
made_out: level 2 as made. */
static unsigned long coverage_run(const DWORD *first, unsigned long size, unsigned int pass, double before[],
	double after[], DWORD **made_out)
{
	struct xgpu_texture_coverage coverage;
	DWORD *stored = malloc(size * size * sizeof(DWORD)), *level_texels = malloc(size * size * sizeof(DWORD));
	unsigned long level = 0, width = size, index;

	CHECK(stored && level_texels, "no memory");
	memcpy(stored, first, size * size * sizeof(DWORD));
	xgpu_texture_coverage_begin(&coverage, pass);
	for (;;)
	{
		unsigned long passed_before = 0, passed_after = 0;

		memcpy(level_texels, stored, width * width * sizeof(DWORD));
		xgpu_texture_coverage_level(&coverage, level, width, width, level_texels);
		for (index = 0; index < width * width; index++)
		{
			passed_before += stored[index] >> 24 >= pass;
			passed_after += level_texels[index] >> 24 >= pass;
		}
		before[level] = (double)passed_before / (double)(width * width);
		after[level] = (double)passed_after / (double)(width * width);
		if (made_out && level == 2)
		{
			*made_out = malloc(width * width * sizeof(DWORD));
			CHECK(*made_out, "no memory");
			memcpy(*made_out, level_texels, width * width * sizeof(DWORD));
		}
		if (width == 1)
			break;
		/* (the next stored level: each 2x2's mean, colour and alpha alike) */
		{
			unsigned long half = width / 2, x, y, channel;

			for (y = 0; y < half; y++)
			{
				for (x = 0; x < half; x++)
				{
					DWORD texel = 0;

					for (channel = 0; channel < 4; channel++)
					{
						unsigned long sum = ((stored[(2 * y) * width + 2 * x] >> (channel * 8)) & 0xff) +
							((stored[(2 * y) * width + 2 * x + 1] >> (channel * 8)) & 0xff) +
							((stored[(2 * y + 1) * width + 2 * x] >> (channel * 8)) & 0xff) +
							((stored[(2 * y + 1) * width + 2 * x + 1] >> (channel * 8)) & 0xff);

						texel |= (DWORD)((sum + 2) / 4) << (channel * 8);
					}
					stored[y * half + x] = texel;
				}
			}
			width = half;
		}
		level++;
	}
	xgpu_texture_coverage_end(&coverage);
	free(stored);
	free(level_texels);
	return level + 1;
}

/* each level's share passing within the tolerance of the first's (a level
of few texels cannot come nearer than one of them) */
static void coverage_shares_check(const char *name, const double after[], unsigned long levels, unsigned long size)
{
	unsigned long level, width;

	for (level = 1, width = size / 2; level < levels; level++, width /= 2)
	{
		double off = after[level] - after[0];

		CHECK((off < 0 ? -off : off) <= 0.1 + 1.0 / (double)(width * width), "%s: level %lu passes %.3f, the first %.3f",
			name, level, after[level], after[0]);
	}
}

static void coverage_check(void)
{
	enum { SIZE = 64 };
	static DWORD fence[SIZE * SIZE], noise[SIZE * SIZE], texels[SIZE * SIZE], copy[SIZE * SIZE];
	struct xgpu_texture_coverage coverage;
	double before[8], after[8];
	unsigned long levels, index;
	DWORD *made = NULL;

	/* leaves: three texels in ten opaque and white, the rest cut out and
	black (as DXT1 stores them), tested as the game tests (GREATER 0x7f) */
	for (index = 0; index < SIZE * SIZE; index++)
		fence[index] = random_next() % 10 < 3 ? 0xffffffffu : 0x00000000u;
	levels = coverage_run(fence, SIZE, 128, before, after, &made);
	CHECK(levels == 7, "the leaves' levels: %lu", levels);
	/* (as stored, the leaves are gone two levels down: the case it is for) */
	CHECK(before[2] < before[0] / 2, "the leaves' stored level 2 passes %.3f of the first's %.3f", before[2], before[0]);
	CHECK(after[0] == before[0], "the leaves' first level changed");
	coverage_shares_check("the leaves", after, levels, SIZE);
	/* (a texel passing two levels down is at least half white: one cut out
	as stored, its alpha and colour under half, takes the leaves' white,
	not the cut-out black) */
	for (index = 0; index < (SIZE / 4) * (SIZE / 4); index++)
		CHECK(made[index] >> 24 < 128 || ((made[index] & 0xff) >= 0x80 && (made[index] >> 8 & 0xff) >= 0x80 &&
			(made[index] >> 16 & 0xff) >= 0x80), "level 2's texel %lu is %08lx", index, (unsigned long)made[index]);
	free(made);

	/* random alphas, a test passing 0xc0 and over (GREATER 0xbf) */
	for (index = 0; index < SIZE * SIZE; index++)
		noise[index] = (random_next() & 0xffffff) | (DWORD)(random_next() % 256) << 24;
	levels = coverage_run(noise, SIZE, 0xc0, before, after, NULL);
	coverage_shares_check("the noise", after, levels, SIZE);

	/* the negative controls: a copy not asked for (pass 0: the stored
	texture, a blended draw's), and a texture whose first level passes
	whole, are as stored, texel for texel */
	memcpy(texels, noise, sizeof(texels));
	memcpy(copy, noise, sizeof(copy));
	xgpu_texture_coverage_begin(&coverage, 0);
	xgpu_texture_coverage_level(&coverage, 0, SIZE, SIZE, texels);
	xgpu_texture_coverage_level(&coverage, 1, SIZE / 2, SIZE / 2, texels);
	xgpu_texture_coverage_end(&coverage);
	CHECK(!memcmp(texels, copy, sizeof(texels)), "a texture with no test changed");
	for (index = 0; index < SIZE * SIZE; index++)
		texels[index] = copy[index] = noise[index] | 0xc0000000u;
	xgpu_texture_coverage_begin(&coverage, 0x80);
	xgpu_texture_coverage_level(&coverage, 0, SIZE, SIZE, texels);
	xgpu_texture_coverage_level(&coverage, 1, SIZE / 2, SIZE / 2, texels);
	xgpu_texture_coverage_end(&coverage);
	CHECK(!memcmp(texels, copy, sizeof(texels)), "a texture passing whole changed");
}

int main(void)
{
	struct xgpu_texture_description description;
	unsigned char block[16];

	format_check(FORMAT_DXT1, "DXT1", 1);
	format_check(FORMAT_DXT3, "DXT3", 1);
	format_check(FORMAT_DXT5, "DXT5", 0);
	format_check(FORMAT_A8R8G8B8, "A8R8G8B8", 1);
	format_check(FORMAT_LINEAR_A8R8G8B8, "linear A8R8G8B8", 1);

	/* a DXT5 block of one alpha: that alpha */
	description = describe(FORMAT_DXT5, 4, 1);
	memset(block, 0, sizeof(block));
	block[0] = block[1] = 200;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 200 && decoded_minimum(&description, block) == 200,
		"a DXT5 block of one alpha");
	/* ... of six alphas that uses its 0 (index 6): nothing */
	block[0] = 100;
	block[2] = 6;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 0 && decoded_minimum(&description, block) == 0,
		"a DXT5 block of six alphas and nothing");
	/* a DXT1 block of three colours that uses the transparent index */
	description = describe(FORMAT_DXT1, 4, 1);
	memset(block, 0, sizeof(block));
	block[4] = 0x03;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 0 && decoded_minimum(&description, block) == 0,
		"a DXT1 block with a transparent texel");
	/* ... and of four colours with the same index: opaque */
	block[0] = 1;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 255 && decoded_minimum(&description, block) == 255,
		"a DXT1 block of four colours");

	/* a format with no alpha passes whatever its texels; one not read, and a
	cube map, are not known */
	description = describe(FORMAT_X8R8G8B8, 4, 1);
	CHECK(xgpu_texture_minimum_alpha(&description, block) == 255, "a format with no alpha");
	description = describe(FORMAT_A4R4G4B4, 4, 1);
	CHECK(xgpu_texture_minimum_alpha(&description, block) == -1, "a format that is not read");
	description = describe(FORMAT_A8R8G8B8, 4, 1);
	description.cube_map = TRUE;
	CHECK(xgpu_texture_minimum_alpha(&description, block) == -1, "a cube map");
	coverage_check();
	printf("texture_alpha_test: passed\n");
	return 0;
}
