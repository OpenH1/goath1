/*
XGPU_COVERAGE.H

The share of a texture's texels an alpha test draws, for code that cannot
include xgpu.h (the game's: world_sdf.c, whose objects' occlusion fields
take an alpha-tested leaf's card as blocking that share of what crosses it),
and a texture's average color (dynamic_lights_bounce.c).
Plain types only. Defined in xbox_texture_decode.c.
*/

#ifndef XGPU_COVERAGE_H
#define XGPU_COVERAGE_H

/* the share of a 2D texture's texels (its D3D Format and Size words, its
texels at texels, levels after one another as they lie) whose alpha is over
reference, at its first level no larger than largest each way; 1 for one
that cannot be read (a cube map, a volume, a format not known, no memory):
taken as drawn whole */
float xgpu_texture_alpha_coverage(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned int reference, unsigned long largest);
/* the mean of a 2D texture's texels' colors in linear light (each channel
taken to the power 2.2: red, green, blue, 0 to 1), at its first level no
larger than largest each way (port/linux/game/dynamic_lights_bounce.c: a
shader's base map); 0 for one that cannot be read */
int xgpu_texture_average_color(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned long largest, float linear[3]);
/* a 2D swizzled or compressed texture's texels (ARGB, blue the lowest
byte) at its first level no larger than largest each way, and that level's
size: the caller frees them; NULL for one that cannot be read
(port/linux/game/world_rt.c's object lamps: a model's base and
multipurpose maps) */
unsigned long *xgpu_texture_texels(unsigned int format_word, unsigned int size_word, const unsigned char *texels,
	unsigned long largest, unsigned long *width, unsigned long *height);

#endif
