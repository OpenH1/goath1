/*
MENU_VECTOR.H

The menus' vector art (port/assets/menus/vector): an SVG drawn into
texels at the size it is shown, as a screen opens or the display's size
changes, by plutosvg on plutovg's canvas (port/third_party/plutosvg,
port/third_party/plutovg), on the menus' art threads (menu_files.c).
*/

#ifndef __HALO_LINUX_MENU_VECTOR_H
#define __HALO_LINUX_MENU_VECTOR_H

/* the largest a picture is drawn, across or down */
#define MENU_VECTOR_LARGEST 4096

/* an SVG's texels (8-bit RGBA, not premultiplied, rows from the top, each
transparent texel given its nearest covered texel's colour; malloc's, the
caller's to free) drawn for a frame of width by height of the menus' units
at scale pixels each, and their size; NULL if it could not be drawn. With
slice (left, top, right and bottom, in the SVG's own units), the SVG's
edges that far in keep their size and its middle stretches to the frame
(the element library's nine-slice sizing); without, the whole SVG is
stretched to the frame */
unsigned char *menu_vector_pixels(const void *svg, unsigned long size, long width, long height, float scale,
	const float *slice, unsigned long *pixel_width, unsigned long *pixel_height);

#endif
