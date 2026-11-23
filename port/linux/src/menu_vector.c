/*
MENU_VECTOR.C

The menus' vector art drawn into texels (menu_vector.h): each layer's SVG
read by plutosvg (port/third_party/plutosvg, with the layers its art needs:
clip paths, masks, filters and groups' opacity) and drawn on plutovg's
canvas (port/third_party/plutovg) at the size the frame is shown, over the
layer before, then made straight RGBA as the menus' PNGs are, its
transparent texels given their nearest covered texel's colour (as
tools/hud_assets.py's bleed does the PNGs'), so that filtering and the mip
levels blend no black in at its edges. The same file draws the heavy art
when the game is built (tools/menu_vector_bake.c), so that both draw alike.
*/

/* (the libraries linked in, not a DLL's) */
#ifndef PLUTOVG_BUILD_STATIC
#define PLUTOVG_BUILD_STATIC
#endif
#ifndef PLUTOSVG_BUILD_STATIC
#define PLUTOSVG_BUILD_STATIC
#endif
#include "plutosvg.h"

#include "menu_vector.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* the document drawn over the surface's rectangle at x, y, w by h pixels,
the document's rectangle sx, sy, sw by sh units drawn there */
static int slice_draw(const plutosvg_document_t *document, plutovg_surface_t *surface, int x, int y, int w, int h,
	float sx, float sy, float sw, float sh)
{
	plutovg_surface_t *cell;
	plutovg_canvas_t *canvas;
	const uint32_t *from;
	uint32_t *to;
	int row, stride = plutovg_surface_get_width(surface);

	if (w <= 0 || h <= 0 || sw <= 0.0f || sh <= 0.0f)
		return 1;
	cell = plutovg_surface_create(w, h);
	if (!cell)
		return 0;
	canvas = plutovg_canvas_create(cell);
	plutovg_canvas_scale(canvas, (float)w / sw, (float)h / sh);
	plutovg_canvas_translate(canvas, -sx, -sy);
	plutosvg_document_render(document, NULL, canvas, NULL, NULL, NULL);
	plutovg_canvas_destroy(canvas);
	from = (const uint32_t *)plutovg_surface_get_data(cell);
	to = (uint32_t *)plutovg_surface_get_data(surface);
	/* (premultiplied, over what the layers before drew) */
	for (row = 0; row < h; row++)
	{
		const uint32_t *in = from + (size_t)row * w;
		uint32_t *out = to + (size_t)(y + row) * stride + x;
		int column;

		for (column = 0; column < w; column++)
		{
			uint32_t over = in[column], under = out[column], inverse = 255 - (over >> 24), result = 0;
			int shift;

			if (!under || !inverse)
			{
				out[column] = over;
				continue;
			}
			for (shift = 0; shift < 32; shift += 8)
			{
				uint32_t value = (over >> shift & 0xFF) + ((under >> shift & 0xFF) * inverse + 127) / 255;

				result |= (value > 255 ? 255 : value) << shift;
			}
			out[column] = result;
		}
	}
	plutovg_surface_destroy(cell);
	return 1;
}

/* one layer's SVG drawn over the surface, w by h pixels at scale */
static int layer_draw(plutovg_surface_t *surface, const void *svg, unsigned long size, int w, int h, float scale,
	const float *slice)
{
	plutosvg_document_t *document;
	float document_width, document_height;
	int ok = 1;

	if (!svg || !size || size > 0x7FFFFFFFUL)
		return 0;
	document = plutosvg_document_load_from_data((const char *)svg, (int)size, -1.0f, -1.0f, NULL, NULL);
	if (!document)
		return 0;
	document_width = plutosvg_document_get_width(document);
	document_height = plutosvg_document_get_height(document);
	if (!slice)
	{
		ok = slice_draw(document, surface, 0, 0, w, h, 0.0f, 0.0f, document_width, document_height);
	}
	else
	{
		/* (the caps: in the document's units, at most all of it; in pixels,
		at the frame's scale, made smaller together if they would overlap) */
		float left = fminf(fmaxf(slice[0], 0.0f), document_width), top = fminf(fmaxf(slice[1], 0.0f), document_height);
		float right = fminf(fmaxf(slice[2], 0.0f), document_width - left);
		float bottom = fminf(fmaxf(slice[3], 0.0f), document_height - top);
		float across = (left + right) * scale > w ? w / ((left + right) * scale) : 1.0f;
		float down = (top + bottom) * scale > h ? h / ((top + bottom) * scale) : 1.0f;
		int x[4], y[4], column, row;
		float sx[4], sy[4];

		x[0] = 0, x[1] = (int)lroundf(left * scale * across), x[2] = w - (int)lroundf(right * scale * across), x[3] = w;
		y[0] = 0, y[1] = (int)lroundf(top * scale * down), y[2] = h - (int)lroundf(bottom * scale * down), y[3] = h;
		if (x[2] < x[1])
			x[2] = x[1];
		if (y[2] < y[1])
			y[2] = y[1];
		sx[0] = 0.0f, sx[1] = left, sx[2] = document_width - right, sx[3] = document_width;
		sy[0] = 0.0f, sy[1] = top, sy[2] = document_height - bottom, sy[3] = document_height;
		for (row = 0; row < 3 && ok; row++)
		{
			for (column = 0; column < 3 && ok; column++)
				ok = slice_draw(document, surface, x[column], y[row], x[column + 1] - x[column], y[row + 1] - y[row],
					sx[column], sy[row], sx[column + 1] - sx[column], sy[row + 1] - sy[row]);
		}
	}
	plutosvg_document_destroy(document);
	return ok;
}

/* each transparent texel given the colour of its nearest covered texel (by
an 8-neighbour chamfer walk there and back): RGBA texels */
static void bleed(unsigned char *texels, int width, int height)
{
	uint32_t *distance = malloc((size_t)width * height * sizeof(*distance));
	int pass, x, y;

	if (!distance)
		return;
	for (y = 0; y < height * width; y++)
		distance[y] = texels[(size_t)y * 4 + 3] ? 0 : UINT32_MAX;
	for (pass = 0; pass < 2; pass++)
	{
		int step = pass ? -1 : 1;
		int y0 = pass ? height - 1 : 0, x0 = pass ? width - 1 : 0;

		for (y = y0; y >= 0 && y < height; y += step)
		{
			for (x = x0; x >= 0 && x < width; x += step)
			{
				/* (the neighbours walked already: behind on the row, and the row behind) */
				static const int dx[4] = { -1, -1, 0, 1 }, dy[4] = { 0, -1, -1, -1 }, cost[4] = { 2, 3, 2, 3 };
				size_t at = (size_t)y * width + x;
				int k;

				if (!distance[at])
					continue;
				for (k = 0; k < 4; k++)
				{
					int nx = x + dx[k] * step, ny = y + dy[k] * step;
					size_t from;

					if (nx < 0 || nx >= width || ny < 0 || ny >= height)
						continue;
					from = (size_t)ny * width + nx;
					if (distance[from] != UINT32_MAX && distance[from] + cost[k] < distance[at])
					{
						distance[at] = distance[from] + cost[k];
						memcpy(texels + at * 4, texels + from * 4, 3);
					}
				}
			}
		}
	}
	free(distance);
}

unsigned char *menu_vector_pixels(int layers, const void *const *svgs, const unsigned long *sizes, long width,
	long height, float scale, const float *slices, unsigned long *pixel_width, unsigned long *pixel_height)
{
	plutovg_surface_t *surface;
	unsigned char *texels;
	int w, h, layer, ok = 1;

	if (layers < 1 || layers > MENU_VECTOR_LAYERS || width <= 0 || height <= 0 || !(scale > 0.0f))
		return NULL;
	if (width * scale > MENU_VECTOR_LARGEST)
		scale = MENU_VECTOR_LARGEST / (float)width;
	if (height * scale > MENU_VECTOR_LARGEST)
		scale = MENU_VECTOR_LARGEST / (float)height;
	w = (int)ceilf(width * scale);
	h = (int)ceilf(height * scale);
	if (w < 1 || h < 1)
		return NULL;
	surface = plutovg_surface_create(w, h);
	if (!surface)
		return NULL;
	for (layer = 0; layer < layers && ok; layer++)
		ok = layer_draw(surface, svgs[layer], sizes[layer], w, h, scale, slices ? slices + layer * 4 : NULL);
	texels = ok ? malloc((size_t)w * h * 4) : NULL;
	if (texels)
	{
		plutovg_convert_argb_to_rgba(texels, plutovg_surface_get_data(surface), w, h, plutovg_surface_get_stride(surface));
		bleed(texels, w, h);
		*pixel_width = (unsigned long)w;
		*pixel_height = (unsigned long)h;
	}
	plutovg_surface_destroy(surface);
	return texels;
}
