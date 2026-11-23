/*
MENU_VECTOR.H

The menus' vector art (port/assets/menus/vector): a frame's SVGs drawn into
texels by plutosvg on plutovg's canvas (port/third_party/plutosvg,
port/third_party/plutovg). The light ones (panels, button frames, focus
outlines: a few shapes) are drawn at the size they are shown, as a screen
opens or the display's size changes, on the menus' art threads
(menu_files.c). The heavy ones (with filters, or hundreds of paths) are
drawn by this same file when the game is built, at the size 4K's display
shows them (tools/menu_vector_bake.c, tools/embed_assets.py), and shipped
as a PNG beside the game.
*/

#ifndef __HALO_LINUX_MENU_VECTOR_H
#define __HALO_LINUX_MENU_VECTOR_H

/* the largest a picture is drawn, across or down */
#define MENU_VECTOR_LARGEST 4096
/* the most SVGs a frame layers, each drawn over the one before */
#define MENU_VECTOR_LAYERS 2

/* a frame's texels (8-bit RGBA, not premultiplied, rows from the top, each
transparent texel given its nearest covered texel's colour; malloc's, the
caller's to free) drawn from its layers' SVGs, each stretched to the frame,
of width by height of the menus' units at scale pixels each, and their
size; NULL if one could not be drawn. With slices (four for each layer: its
left, top, right and bottom, in the SVG's own units), a layer's edges that
far in keep their size and its middle stretches (the element library's
nine-slice sizing) */
unsigned char *menu_vector_pixels(int layers, const void *const *svgs, const unsigned long *sizes, long width,
	long height, float scale, const float *slices, unsigned long *pixel_width, unsigned long *pixel_height);

#endif
