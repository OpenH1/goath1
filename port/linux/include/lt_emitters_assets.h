/*
LT_EMITTERS_ASSETS.H

The levels' static light sources solved from their lightmaps
(port/assets/emitters/<map>_bsp<k>.json), one pack a BSP: tools/
lt_emitters_data.py packs each (its form there), tools/embed_assets.py puts
it, zlib-compressed, in openh1.assets (embedded_assets.h) and lists it here
by its scenario's tag path and BSP index, so the game holds only this table
and unpacks the loaded BSP's when the BSP changes
(port/linux/game/lt_emitters.c).
*/

#ifndef LT_EMITTERS_ASSETS_H
#define LT_EMITTERS_ASSETS_H

struct lt_emitters_asset
{
	char const *scenario;
	int bsp_index;
	/* its compressed bytes in openh1.assets, and its size unpacked */
	unsigned int offset, size, unpacked_size;
};

extern struct lt_emitters_asset const lt_emitters_assets[];
extern unsigned int const lt_emitters_asset_count;

/* the scenario's BSP's pack's size unpacked (0: none built in) */
unsigned int lt_emitters_asset_size(char const *scenario, int bsp_index);
/* the pack unpacked into out (of size bytes, the size above; the caller's
memory, as the game's own allocator holds it): nonzero when it was */
int lt_emitters_asset_read(char const *scenario, int bsp_index, void *out, unsigned int size);

#endif
