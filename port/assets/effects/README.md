# HD effects

`effects.json` lists the stock effect bitmaps the game redraws sharper (`display.hd_effects`,
`port/linux/src/hd_effects.h`): numbers only, never a picture. The game reads each bitmap out of the player's own
maps, redraws it once and keeps it in the data folder's `hd_effects` (beside `menu_pictures`), then draws it in the
map's bitmap's place as a mod's bitmap is drawn (`docs/mods.md`, Bitmaps). `tools/embed_assets.py` compiles the list
into the game.

| Key | Contents |
| --- | --- |
| `name` | The file's name in the cache (`<name>.bitmap`, and its note `<name>.note`) |
| `tag` | The bitmap tag it stands for |
| `recipe` | How it is redrawn: `flash` is `tools/hd_muzzle_flash.py`'s math (`hd_effects.c` does the same in C) |
| `scale`, `smallest` | The redraw's size over the map's, and its smallest mip level |
| `crc` | The CRC-32 of the stock bitmap's pixels, every page's as the Xbox maps store them (`python3 tools/hd_muzzle_flash.py --map maps/putput.map --crc` prints it). A map whose bitmap has another (a modded map) keeps its own |
| `maps` | The stock maps it is read from: the first one the player has with the stock bitmap |

| Effect | Stock | Redrawn | Pixels with mip levels |
| --- | --- | --- | --- |
| `flash_h_ar_fp`, the Assault Rifle's first-person muzzle flash | 2 pages of 256 x 256 A1R5G5B5 (0.34 MB) | 2 pages of 1024 x 1024 A8R8G8B8 | 5.3 MB a page, 11.2 MB in all |

The redrawn textures take that much more of the texture cache while a map draws them (the cache grows by it on the
builds whose Xbox maps' cache is smaller: Android and iOS). Making the flash takes about 44 MB of memory for a moment,
given back after.
