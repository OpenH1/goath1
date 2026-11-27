/*
XGPU_VULKAN_TEXTURES.H

The Vulkan backend's cache of the textures the command stream's stages draw
that are not render targets (xgpu_vulkan_textures.c): the Xbox's textures,
read from the guest's memory, and the high-res replacements' images. Called
on the render thread.
*/

#ifndef __HALO_LINUX_XGPU_VULKAN_TEXTURES_H
#define __HALO_LINUX_XGPU_VULKAN_TEXTURES_H

#include <stdint.h>

struct xgpu_stream_texture_stage;
struct xgpu_vulkan_image;

/* the image a stage draws: an Xbox texture's (XGPU_SOURCE_TEXTURE), made
from the guest's memory and made again once its pages have been written,
or an image's (XGPU_STREAM_SOURCE_IMAGE); NULL if there is none */
struct xgpu_vulkan_image *xgpu_vulkan_textures_get(const struct xgpu_stream_texture_stage *stage);

/* the VR mode's effects and a flat screen's soft ones: the thickness of
the Xbox texture a stage draws (an effect's bitmap), RGBA8
(xgpu_vulkan_effects.h), baked from its first level on the effects'
workers once it is first asked for, and kept with the texture (baked again
with its texels). NULL until it has come back (it is made between frames,
xgpu_vulkan_textures_frame_end, and the epoch moves on), and for one it
cannot be (a cube, a volume, too large, an image, one whose texels could
not be read yet): never baked on the calling thread */
struct xgpu_vulkan_image *xgpu_vulkan_textures_thickness(const struct xgpu_stream_texture_stage *stage);
/* changes whenever a thickness image is destroyed (baked again, its
texture let go of by xgpu_vulkan_textures_frame_end, or read with another
palette) or made (a bake come back): what xgpu_vulkan_textures_thickness
gave, an image or none, may be drawn with only while this is the same as
when it was looked up */
uint32_t xgpu_vulkan_textures_thickness_epoch(void);
/* the map's preparation's end (behind the loading screen): the bakes asked
for waited for, up to seconds_most, and every one come back made; the
count made */
uint32_t xgpu_vulkan_textures_thickness_settle(double seconds_most);

/* A texture whose header is named by the VR stream (the visor's plasma
noise), checked and decoded through the same cache as a draw's stage. */
struct xgpu_vulkan_image *xgpu_vulkan_textures_header(uint32_t header);

/* debug.texture_dump_directory: the folder the first level of each 2D Xbox
texture is written into as it is read (again, once its texels change):
tex<number>_fmt<format>_<width>x<height>.tga, 32-bit BGRA (NULL or empty:
none; the string is kept) */
void xgpu_vulkan_textures_dump(const char *folder);

/* the folder the ASTC encoder keeps the DXT textures' encoded blocks in
(port/linux/astc/xgpu_astc.h; NULL or empty: none; the string is copied),
where DXT textures are ASTC (HALO_ASTC builds) */
void xgpu_vulkan_textures_astc_folder(const char *folder);

/* a frame has been played: the textures no stage has drawn for half a
minute of time are let go (but those with a thickness baked from texels the
guest still has) */
void xgpu_vulkan_textures_frame_end(void);

/* statistics since the last call: textures made, and made again from pages
that were written; images; their bytes read from the guest; thicknesses
made, the seconds their bakes took on the workers, and the render thread's
seconds making their images */
struct xgpu_vulkan_texture_statistics
{
	uint32_t made, remade, images, thickness;
	uint64_t bytes;
	double thickness_seconds, thickness_landing_seconds;
	/* the copies read whose levels keep an alpha test's coverage (xgpu.h),
	and the time reading them took */
	uint32_t coverage;
	double coverage_seconds;
};
void xgpu_vulkan_textures_statistics(struct xgpu_vulkan_texture_statistics *statistics);
void xgpu_vulkan_textures_statistics_peek(struct xgpu_vulkan_texture_statistics *statistics);

#endif
