/*
MENU_VECTOR_BAKE.C

The menus' heavy vector art drawn when the game is built
(tools/embed_assets.py, which compiles this with the build computer's C
compiler beside port/linux/src/menu_vector.c and plutosvg, so that it draws
as the game does): an SVG drawn for a frame of width by height of the
menus' units at scale pixels each, written as its texels (straight RGBA,
rows from the top) after a header of "OHVR" and the width and height
(32-bit, little-endian).

    menu_vector_bake OUTPUT WIDTH HEIGHT SCALE SVG
*/

#include "menu_vector.h"

#include <stdio.h>
#include <stdlib.h>

static void put32(FILE *file, unsigned long value)
{
	unsigned char bytes[4] = { value & 0xFF, value >> 8 & 0xFF, value >> 16 & 0xFF, value >> 24 & 0xFF };

	fwrite(bytes, 1, 4, file);
}

int main(int argc, char **argv)
{
	FILE *file;
	unsigned char *svg, *texels;
	long length;
	unsigned long size, width = 0, height = 0;
	const void *layers[1];

	if (argc != 6)
	{
		fprintf(stderr, "usage: menu_vector_bake OUTPUT WIDTH HEIGHT SCALE SVG\n");
		return 2;
	}
	file = fopen(argv[5], "rb");
	if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET))
	{
		fprintf(stderr, "menu_vector_bake: cannot read %s\n", argv[5]);
		return 1;
	}
	svg = malloc((size_t)length);
	if (!svg || fread(svg, 1, (size_t)length, file) != (size_t)length)
		return 1;
	fclose(file);
	size = (unsigned long)length;
	layers[0] = svg;
	texels = menu_vector_pixels(1, layers, &size, atol(argv[2]), atol(argv[3]), (float)atof(argv[4]), NULL, &width,
		&height);
	if (!texels)
	{
		fprintf(stderr, "menu_vector_bake: cannot draw %s\n", argv[5]);
		return 1;
	}
	file = fopen(argv[1], "wb");
	if (!file)
		return 1;
	fwrite("OHVR", 1, 4, file);
	put32(file, width);
	put32(file, height);
	if (fwrite(texels, 4, (size_t)width * height, file) != (size_t)width * height || fclose(file))
		return 1;
	return 0;
}
