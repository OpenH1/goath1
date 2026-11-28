# Static light sources, one file a BSP

`<map>_bsp<k>.json`: the level's own glowing shaders (lamps, strips, light cards, screens: radiosity
power > 0 in the shader tag) as area lights, at the tag's color and relative power, with one brightness
scale per room-sized cluster (a 12 wu cell), set so each cell's traced light matches the original lightmap's.
The ray-traced lighting traces them live instead of using the lightmap.

Made by `python -m rt_truth emitters <scene folder> --out <file>` (tools/rt_truth/README.md, "Solving the
emitters") from any rt2 scene export of that BSP. In short, each `lamps` entry has `radiance` (linear, the game's light units) and `triangles` (world
positions, lifted 0.01 wu off the surface, wound to glow toward their normal) to sample as area lights.

The build packs each file (tools/lt_emitters_data.py: about 950 KB for all 51, zlib) into openh1.assets beside
the game, and the game unpacks the loaded BSP's at map load; `python3 tools/lt_emitters_data.py --check` fails
where a file does not build or holds more than the game takes (8192 triangles, 1024 groups a BSP).

## The format
One file a BSP: `<map>_bsp<k>.json` (`port/assets/emitters/` in the repo, shipped beside the game like the
prepared records once the pass reads it; made by `python -m rt_truth emitters <scene> --out <file>`, any export
of that BSP will do). The truth renderer reads it with `render --static emitters --emitters <file>`; the
real-time pass traces the same sources live. Units: linear radiance in the game's light units (a white
Lambertian surface lit by radiance L over its whole hemisphere shows L; a lightmap texel v as shown is
v^2.2 of these units). Positions in world units, z up.
```json
{"format": "goat-h1 rt2 emitters 1", "map": "a10", "scenario": "levels\\a10\\a10", "bsp_index": 0,
 "solved_from": {"bsp_and_lightmaps_sha256_16": "...", "scene": "<scene folder>"},
 "fit": {"r2": 0.0, "r2_display_gamma": 0.0, "r2_tag_sources_alone": 0.0, "...": "how it was fitted"},
 "shader_emitters": [{"shader": "levels\\a10\\shaders\\halogen on", "radiance": [r, g, b],
                      "two_sided": false, "tag_power": 1200, "tag_color": [r, g, b], "scale": s}],
 "sky": {"ambient": {"radiance": [r, g, b], "scale": s}, "suns": [{"direction_to_sun": [x, y, z],
         "angular_diameter_deg": d, "irradiance": [r, g, b], "scale": s}]},
 "ambient_floor": {"radiance": [r, g, b]},
 "added_emitters": [{"id": 0, "shape": "triangles", "one_sided": true, "center": [x, y, z], "normal": [x, y, z],
                     "area_wu2": a, "radiance": [r, g, b], "power": p,
                     "triangles": [[x0, y0, z0, x1, y1, z1, x2, y2, z2], ...], "why": "..."}]}
```
- `shader_emitters`: every BSP surface drawn with that shader glows with `radiance` (uniform, Lambertian),
  toward its normal; `two_sided` (light cards) toward both. Matched by shader tag path, so no indices.
- `sky`: the sky's dome (`radiance`, uniform) and lights (`irradiance` on a surface facing the light, a disc
  `angular_diameter_deg` across). Outdoors the sky's own tags are traced apart.
- `ambient_floor`: a constant added to every lightmapped surface's light (albedo x this); no bounce.
- `added_emitters`: light the bake needs that no tag source gives: surface patches (BSP triangles lifted
  0.01 wu off the surface, wound to glow toward `normal`), one-sided, light only (not drawn as a glow).
- Everything here bounces: the fit traced every bounce off the BSP's albedos, so the pass must too (or its
  first bounce at least) for the powers to hold.
- **`lamps` (mode "rooms", the form shipped):** the level's own glowing shaders (radiosity
  power > 0: lamps, strips, light cards, screens) at the tag's color x relative power, split into lamp groups
  by `region` (a `cell_wu` grid cell holding the surfaces' middles), each group with one `scale`, set so that
  in every cell the traced mean light equals the bake's. Each entry: `{"id", "shader", "region": {"min",
  "max"}, "radiance": [r, g, b], "scale", "scale_over_level", "tag_power", "tag_color", "two_sided",
  "area_wu2", "triangles"}`; `triangles` are that shader's surfaces in the region lifted 0.01 wu toward
  their normals (both faces for a light card), so the pass can sample them as area lights without matching
  BSP indices. In this mode `shader_emitters`, `added_emitters`, the sky and the floor are empty: the sky is
  traced from its own tags apart; nothing is invented.

## The files (solved 2026-11-28, 2048 paths a sample)
Each BSP's lamp groups, its total traced lamp light over the bake's (the bake less the sky's share, over the
cells the lamps light; 1 = all of it), and its typical cell ratio (the area-weighted geometric spread of each
12 wu cell's traced mean light over the bake's: x1.3 means a typical cell is within 30%). "Sky" is the bake's
share the sky's own tags give (traced too, fitted where only the sky lights, then taken off the bake; not in
the file). 2048 paths: c20 bsp1's cells match the same at 4096 (x2.95 both before the sky rule), while 128
(the first two files' count) gave x4.05; a10 and c20 bsp0 are solved again at 2048 here.

| BSP | lamp groups | triangles | lamps / bake | typical cell | sky |
|---|---|---|---|---|---|
| a10_bsp0 | 32 | 726 | 0.99 | x1.33 | 0% |
| a10_bsp1 | 44 | 888 | 0.98 | x1.37 | 44% |
| a10_bsp2 | 21 | 629 | 0.98 | x1.27 | 0% |
| a10_bsp3 | 19 | 505 | 0.98 | x1.29 | 0% |
| a10_bsp4 | 26 | 636 | 0.95 | x1.31 | 0% |
| a10_bsp5 | 41 | 1031 | 0.99 | x1.16 | 0% |
| a10_bsp6 | 31 | 828 | 0.99 | x1.27 | 0% |
| a10_bsp8 | 67 | 399 | 0.92 | x1.61 | 0% |
| a30_bsp1 | 46 | 1236 | 0.90 | x1.89 | 87% |
| a50_bsp1 | 60 | 1634 | 0.99 | x2.63 | 0% |
| a50_bsp2 | 46 | 1176 | 0.99 | x3.04 | 3% |
| a50_bsp3 | 48 | 850 | 1.00 | x1.03 | 3% |
| b40_bsp0 | 134 | 3304 | 0.88 | x1.64 | 62% |
| b40_bsp3 | 162 | 1818 | 0.86 | x1.88 | 0% |
| b40_bsp4 | 142 | 3456 | 1.00 | x1.07 | 0% |
| b40_bsp5 | 41 | 1048 | 1.00 | x1.26 | 0% |
| b40_bsp8 | 181 | 3556 | 1.00 | x1.12 | 0% |
| b40_bsp9 | 145 | 2416 | 1.00 | x1.10 | 0% |
| b40_bsp10 | 144 | 2840 | 0.99 | x1.23 | 0% |
| b40_bsp11 | 164 | 3432 | 1.00 | x1.24 | 0% |
| c10_bsp1 | 148 | 4100 | 0.98 | x3.60 | 0% |
| c10_bsp2 | 131 | 2944 | 1.00 | x1.06 | 0% |
| c10_bsp3 | 130 | 3252 | 0.99 | x1.35 | 0% |
| c10_bsp4 | 191 | 4108 | 0.99 | x1.84 | 0% |
| c10_bsp5 | 41 | 908 | 0.90 | x3.19 | 65% |
| c20_bsp0 | 325 | 3794 | 0.96 | x1.82 | 0% |
| c20_bsp1 | 411 | 4546 | 0.97 | x1.88 | 0% |
| c20_bsp2 | 365 | 4736 | 0.97 | x2.01 | 0% |
| c20_bsp3 | 292 | 3200 | 0.96 | x2.20 | 0% |
| c40_bsp0 | 134 | 3304 | 0.86 | x2.18 | 25% |
| c40_bsp5 | 41 | 1048 | 1.00 | x1.03 | 0% |
| c40_bsp6 | 42 | 1048 | 1.00 | x1.11 | 0% |
| c40_bsp7 | 83 | 2020 | 1.00 | x1.21 | 0% |
| c40_bsp8 | 181 | 3556 | 1.00 | x1.20 | 0% |
| c40_bsp9 | 145 | 2416 | 1.00 | x1.10 | 0% |
| c40_bsp10 | 144 | 2840 | 0.99 | x1.22 | 0% |
| c40_bsp11 | 164 | 3432 | 1.00 | x1.29 | 0% |
| c40_bsp12 | 47 | 1048 | 1.00 | x1.08 | 0% |
| d20_bsp0 | 43 | 736 | 0.97 | x2.71 | 2% |
| d20_bsp1 | 52 | 5528 | 0.87 | x2.25 | 61% |
| d20_bsp2 | 65 | 1274 | 0.98 | x1.10 | 8% |
| d20_bsp3 | 49 | 1160 | 0.98 | x3.52 | 61% |
| d20_bsp4 | 24 | 294 | 0.98 | x1.14 | 1% |
| d40_bsp0 | 15 | 262 | 0.99 | x1.16 | 84% |
| d40_bsp1 | 30 | 294 | 0.97 | x1.30 | 92% |
| d40_bsp2 | 27 | 653 | 0.98 | x1.53 | 0% |
| d40_bsp3 | 16 | 360 | 1.00 | x1.20 | 0% |
| d40_bsp4 | 56 | 1010 | 1.00 | x1.24 | 0% |
| d40_bsp5 | 101 | 1508 | 0.96 | x1.89 | 42% |
| d40_bsp6 | 52 | 900 | 0.98 | x1.52 | 74% |
| d40_bsp7 | 190 | 2737 | 0.97 | x2.52 | 20% |

Not shipped (an outdoor BSP's lamps cannot be matched apart from the sky, or nothing is left to them):
- a10_bsp7: all sky (space outside the ship): no lamp-lit cell.
- a30_bsp0: outdoors (sky 74%); the lamp cells left match only within x13.
- a50_bsp0: outdoors (sky 66%); lamps 0.83 of the rest, x4.2.
- b40_bsp1: outdoors (sky 60%); lamps 0.61 of the rest, x4.7.
- b40_bsp2: outdoors (sky 51%); lamps 0.78 of the rest (held at 20x).
- c10_bsp0: outdoors (sky 45%); lamps 0.70 of the rest, x3.4.
- c40_bsp1: outdoors (sky 35%); lamps 0.67 of the rest, x4.0.
- c40_bsp2: lamps reach 0.71 of the bake (held at 20x), x2.6.
- d40_bsp8: all sky: no lamp-lit cell.
- No lightmap in that level (nothing to solve from): b40 bsp6, bsp7 and bsp12, c40 bsp3 and bsp4; d40 bsp9 has
  no surfaces.

The scenes: one export a BSP (`tools/rt_truth/scenes.json`, "any_bsp"), kept on the 5900X in
`~/goat-rig/rt2-emitters-more/`.
