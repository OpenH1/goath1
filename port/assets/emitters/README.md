# Static light sources, one file a BSP

`<map>_bsp<k>.json`: the level's own glowing shaders (lamps, strips, light cards, screens: radiosity
power > 0 in the shader tag) as area lights, at the tag's color and relative power, with one brightness
scale per room-sized cluster (a 12 wu cell), set so each cell's traced light matches the original lightmap's.
The ray-traced lighting traces them live instead of using the lightmap.

Made by `python -m rt_truth emitters <scene folder> --out <file>` (tools/rt_truth/README.md, "Solving the
emitters") from any rt2 scene export of that BSP. In short, each `lamps` entry has `radiance` (linear, the game's light units) and `triangles` (world
positions, lifted 0.01 wu off the surface, wound to glow toward their normal) to sample as area lights.

The build packs each file (tools/lt_emitters_data.py: about 384 KB for the 21 shipped, zlib) into openh1.assets beside
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
(the first two files' count) gave x4.05; a10 and c20 bsp0 were solved again at 2048 here, and again with
the fixtures (below), and both they and their first (128-path) files fail the screen. The table is every BSP solved;
the files shipped are the ones that passed the screen.

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

## Why BSPs failed the first screen (2026-11-28)
Traced on lossless stills, the pass's unshadowed debug view (HALO_LT_E1_UNSHADOWED, a debugging run) and the
solver's own transport at the screen's spots (texels near a spot, each lamp group's light, direct and every bounce):
- **The camera's contrast bent the fit a second time.** The live tone map's contrast (xgpu_hdr.h's
  XGPU_HDR_CAMERA_CONTRAST, 1.22 about middle grey, fitted on b30's traced sky) applied wherever the pass lit the
  level live, solved lamps too. The lamps are fitted to the bake's own light, so a dim room (light about 0.01)
  came out at about x0.55 and a bright one went brighter (c20's gold room blown). That was the uniform x0.5-0.6 of
  b40 bsp4/5, c40 bsp5/6, c10 bsp5 and d40 bsp0/6. The contrast now applies only where the sky lights the view
  live (xgpu_vulkan_lt_live); a view lit by solved lamps alone keeps the bake's spread, as the lightmap does.
- **Cells met by near-field peaks.** Where no lamp of the BSP gives the bake's light, the per-cell fixed point
  pushed the nearest glow strips to its 20x limit (d20 bsp4: 49% of the lamps' power at the limit, d20 bsp1 36%,
  d20 bsp2 10%, b40 bsp0 14%). A cell's mean light was then met by the few texels within a unit of a strip, its
  near-field peak, while the rest of the room got almost none: in the game those floors go black (d20 bsp4's spot,
  x0.13), and with the shadow rays off the light shows up (green from the strips). d20's and a50's rooms are lit in
  the bake by light_ceiling fixtures (scenery and light-fixture objects, no radiosity power in their tags), so the
  solve now takes the static baked objects' glowing parts (scenery and light fixtures: a model's self-illumination,
  a fixture's light cards and cones) as lamp groups too, and records the power held at the limit
  (`fit.power_share_at_limit`, warned on above 5%). Machines and controls (doors, lifts) move, so they are not
  in the file.
  d20 bsp4 with its fixtures: no group at the limit, its first spot from x0.13 (71% black) to x0.62 (6%), its
  second x0.30 to x0.45, still short. d20 bsp0-3 and a50 bsp1-3 with fixtures hold 14,000 to 40,000 triangles,
  past the game's 8192, and d20 bsp0/1/2 and a50 bsp1 still hold 8-48% of their power at the limit: not shipped.
- Not the cause at d20 bsp4's spot: the 32-group cell list. The level power mixed into the pick keeps every
  lamp drawable, and the strip that lit the cell there is 40% of the level's power. With the shadow rays off, its
  light arrives.

## Looked at in the game (the screen, 2026-11-28)
`python3 tools/rt2_emitter_screen.py --build <ref> --out DIR` plays each level twice on the 5900X as players run it
(the menu setting off and on, the MCC import, the HUD hidden, a fixed frame time), switches to each BSP with a
file and takes a lossless still at two cameras inside rooms its lamps light (tools/rt2_emitter_spots.json, picked
from the BSP's mesh beside its strongest lamp groups). Per still pair, live over classic in linear luminance: the
whole frame, its lower 40% (mostly floor) and middle band (mostly walls); "black": pixels near-black live where
classic shows them lit (live under 4% of white and under a third of classic), of the frame and of the worst cell
of a 4x4 grid; "blown": pixels near white live where classic is under 60% of white. A BSP ships when every still
of it has its whole-frame ratio within 0.6 to 1.6, black under 10% of the frame and under 50% of every cell, and
blown under 3% (c20 bsp2's library floor went fully white at 7%); a still that shows nothing (a camera outside the rooms) or repeats the level's still before it
(the game no longer drawing new frames) is not a look, and a BSP that is not looked at is not shipped.

Shipped, screened with the contrast fix (below; a10_bsp1 has one spot: no second camera inside a lit room was found):

| BSP | spot | overall | floor | walls | black | worst cell | blown | verdict |
|---|---|---|---|---|---|---|---|---|
| a10_bsp1 | 1 | x1.37 | x1.64 | x1.00 | 4% | 22% | 0% | pass |
| a10_bsp2 | 1 | x0.98 | x0.82 | x1.08 | 1% | 7% | 0% | pass |
| a10_bsp2 | 2 | x1.50 | x1.13 | x1.65 | 0% | 1% | 0% | pass |
| a10_bsp3 | 1 | x0.94 | x0.95 | x0.88 | 0% | 4% | 0% | pass |
| a10_bsp3 | 2 | x1.21 | x1.42 | x1.32 | 0% | 4% | 0% | pass |
| a10_bsp6 | 1 | x0.79 | x0.75 | x0.85 | 1% | 6% | 0% | pass |
| a10_bsp6 | 2 | x1.07 | x0.94 | x1.09 | 1% | 9% | 0% | pass |
| b40_bsp4 | 1 | x0.91 | x0.89 | x1.01 | 0% | 2% | 0% | pass |
| b40_bsp4 | 2 | x0.82 | x0.77 | x0.82 | 0% | 0% | 0% | pass |
| b40_bsp5 | 1 | x0.85 | x0.81 | x0.94 | 0% | 1% | 0% | pass |
| b40_bsp5 | 2 | x0.78 | x0.71 | x0.67 | 0% | 2% | 0% | pass |
| b40_bsp8 | 1 | x0.94 | x0.94 | x0.84 | 0% | 1% | 0% | pass |
| b40_bsp8 | 2 | x0.81 | x0.76 | x0.87 | 0% | 6% | 0% | pass |
| b40_bsp9 | 1 | x1.19 | x1.30 | x1.18 | 1% | 8% | 0% | pass |
| b40_bsp9 | 2 | x1.33 | x1.64 | x1.16 | 0% | 4% | 0% | pass |
| b40_bsp10 | 1 | x0.99 | x1.00 | x1.08 | 0% | 4% | 0% | pass |
| b40_bsp10 | 2 | x1.50 | x1.83 | x0.92 | 0% | 0% | 0% | pass |
| c10_bsp1 | 1 | x1.25 | x1.56 | x1.12 | 0% | 1% | 0% | pass |
| c10_bsp1 | 2 | x1.23 | x1.52 | x1.07 | 0% | 6% | 0% | pass |
| c10_bsp2 | 1 | x1.23 | x1.53 | x1.05 | 0% | 2% | 0% | pass |
| c10_bsp2 | 2 | x1.28 | x1.55 | x1.08 | 0% | 0% | 0% | pass |
| c10_bsp4 | 1 | x0.80 | x0.69 | x0.80 | 0% | 0% | 0% | pass |
| c10_bsp4 | 2 | x1.06 | x1.12 | x0.92 | 0% | 0% | 0% | pass |
| c40_bsp0 | 1 | x0.77 | x0.65 | x0.82 | 1% | 5% | 0% | pass |
| c40_bsp0 | 2 | x0.92 | x1.16 | x0.77 | 0% | 1% | 0% | pass |
| c40_bsp5 | 1 | x0.84 | x0.82 | x0.92 | 0% | 0% | 0% | pass |
| c40_bsp5 | 2 | x0.83 | x0.72 | x0.72 | 0% | 0% | 0% | pass |
| c40_bsp6 | 1 | x0.82 | x0.77 | x0.76 | 0% | 1% | 0% | pass |
| c40_bsp6 | 2 | x0.61 | x0.57 | x0.62 | 0% | 2% | 0% | pass |
| c40_bsp8 | 1 | x0.94 | x0.92 | x0.83 | 0% | 1% | 0% | pass |
| c40_bsp8 | 2 | x0.80 | x0.74 | x0.86 | 0% | 6% | 0% | pass |
| c40_bsp10 | 1 | x1.16 | x1.17 | x1.22 | 0% | 0% | 0% | pass |
| c40_bsp10 | 2 | x1.56 | x1.85 | x0.95 | 0% | 0% | 0% | pass |
| d40_bsp1 | 1 | x1.18 | x1.02 | x1.28 | 0% | 1% | 0% | pass |
| d40_bsp1 | 2 | x1.17 | x1.21 | x1.32 | 0% | 2% | 0% | pass |
| d40_bsp2 | 1 | x0.78 | x0.65 | x0.94 | 3% | 25% | 0% | pass |
| d40_bsp4 | 1 | x1.15 | x1.27 | x1.05 | 0% | 3% | 0% | pass |
| d40_bsp4 | 2 | x1.14 | x1.12 | x1.32 | 0% | 2% | 0% | pass |
| d40_bsp5 | 1 | x0.92 | x0.93 | x0.99 | 1% | 5% | 0% | pass |
| d40_bsp5 | 2 | x1.02 | x1.02 | x1.03 | 1% | 5% | 0% | pass |

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
- a50_bsp1 (60 groups, x2.63): looked at in game (tools/rt2_clips.py a50_bsp1), its lamp-lit room goes nearly black live
  where the bake lights its walls and floor green.
- d20_bsp0 (43 groups, x2.71): looked at in game (d20_bsp0), the first hall's lower floor goes black live where the
  bake lights it, while its walls come out brighter.
- Failed the screen, or not looked at ("nothing in view"). a10 bsp0, c20 bsp0 and c40 bsp7 shipped before the contrast fix. Their files fail now, re-solved too (a10's second spot blown, c20's library blown, c40's second spot x1.8), so they keep their lightmap:

| BSP | spot | overall | floor | walls | black | worst cell | blown | verdict |
|---|---|---|---|---|---|---|---|---|
| a10_bsp0 (re-solved with fixtures) | 1 | x0.75 | x0.56 | x1.00 | 2% | 10% | 0% | pass |
| a10_bsp0 (re-solved with fixtures) | 2 | x2.04 | x1.32 | x0.99 | 7% | 27% | 8% | FAIL |
| a10_bsp4 | 1 | x2.48 | x2.21 | x1.32 | 0% | 4% | 14% | FAIL |
| a10_bsp4 | 2 | x0.72 | x0.59 | x0.80 | 0% | 3% | 0% | pass |
| a10_bsp5 | 1 | x3.44 | x5.37 | x3.96 | 1% | 7% | 2% | FAIL |
| a10_bsp8 | 1 | x1.15 | x1.00 | x1.11 | 0% | 0% | 0% | nothing in view |
| a10_bsp8 | 2 | x1.15 | x1.00 | x1.11 | 0% | 0% | 0% | nothing in view |
| a30_bsp1 | 1 | x0.25 | x0.24 | x0.26 | 54% | 86% | 0% | FAIL |
| a30_bsp1 | 2 | x0.94 | x0.95 | x0.94 | 0% | 0% | 0% | pass |
| a50_bsp1 | 1 | x0.25 | x0.13 | x0.29 | 4% | 47% | 0% | FAIL |
| a50_bsp1 | 2 | x0.55 | x0.53 | x0.46 | 6% | 46% | 0% | FAIL |
| a50_bsp2 | 1 | x0.85 | x1.09 | x0.47 | 6% | 25% | 0% | pass |
| a50_bsp2 | 2 | x0.49 | x0.33 | x0.56 | 6% | 15% | 0% | FAIL |
| a50_bsp3 | 1 | x0.80 | x0.82 | x0.69 | 12% | 67% | 0% | FAIL |
| a50_bsp3 | 2 | x0.46 | x0.14 | x0.66 | 8% | 44% | 0% | FAIL |
| b40_bsp0 | 1 | x0.60 | x0.71 | x0.48 | 1% | 5% | 0% | FAIL |
| b40_bsp0 | 2 | x0.71 | x0.68 | x0.66 | 1% | 8% | 0% | pass |
| b40_bsp11 | 1 | x1.48 | x1.81 | x0.90 | 0% | 0% | 0% | nothing in view |
| b40_bsp11 | 2 | x1.48 | x1.81 | x0.90 | 0% | 0% | 0% | nothing in view |
| b40_bsp3 | 1 | x1.00 | x1.00 | x1.00 | 0% | 0% | 0% | nothing in view |
| b40_bsp3 | 2 | x0.72 | x0.84 | x0.76 | 1% | 6% | 0% | pass |
| c10_bsp3 | 1 | x0.86 | x0.78 | x0.88 | 0% | 2% | 0% | pass |
| c10_bsp3 | 2 | x0.40 | x0.32 | x0.54 | 19% | 59% | 0% | FAIL |
| c10_bsp5 | 1 | x0.48 | x0.38 | x0.49 | 2% | 21% | 0% | FAIL |
| c10_bsp5 | 2 | x0.72 | x0.77 | x0.84 | 1% | 14% | 0% | nothing in view |
| c20_bsp0 (re-solved with fixtures) | 1 | x1.29 | x1.20 | x1.50 | 0% | 0% | 9% | FAIL |
| c20_bsp0 (re-solved with fixtures) | 2 | x1.00 | x1.00 | x1.00 | 0% | 0% | 0% | nothing in view |
| c20_bsp1 | 1 | x1.40 | x1.08 | x1.67 | 0% | 0% | 6% | FAIL |
| c20_bsp1 | 2 | x1.03 | x1.03 | x1.06 | 0% | 0% | 0% | nothing in view |
| c20_bsp2 | 1 | x1.33 | x1.08 | x1.56 | 0% | 0% | 6% | FAIL |
| c20_bsp2 | 2 | x0.99 | x0.80 | x1.19 | 0% | 2% | 0% | pass |
| c20_bsp3 | 1 | x1.35 | x1.08 | x1.59 | 0% | 0% | 6% | FAIL |
| c20_bsp3 | 2 | x1.10 | x1.18 | x1.22 | 0% | 1% | 0% | nothing in view |
| c40_bsp11 | 1 | x1.55 | x1.84 | x0.93 | 0% | 1% | 0% | nothing in view |
| c40_bsp11 | 2 | x1.55 | x1.84 | x0.93 | 0% | 1% | 0% | nothing in view |
| c40_bsp12 | 1 | x1.55 | x1.83 | x0.93 | 0% | 1% | 0% | nothing in view |
| c40_bsp12 | 2 | x1.54 | x1.83 | x0.93 | 0% | 1% | 0% | nothing in view |
| c40_bsp7 (re-solved with fixtures) | 1 | x1.59 | x1.91 | x1.39 | 0% | 4% | 0% | pass |
| c40_bsp7 (re-solved with fixtures) | 2 | x1.81 | x2.17 | x1.83 | 0% | 0% | 0% | FAIL |
| c40_bsp9 | 1 | x1.38 | x1.70 | x1.22 | 0% | 4% | 0% | pass |
| c40_bsp9 | 2 | x1.03 | x0.49 | x1.70 | 45% | 100% | 0% | FAIL |
| d20_bsp0 | 1 | x1.39 | x0.54 | x1.44 | 8% | 38% | 0% | pass |
| d20_bsp0 | 2 | x0.68 | x0.46 | x0.97 | 14% | 67% | 0% | FAIL |
| d20_bsp1 | 1 | x1.07 | x0.96 | x1.04 | 0% | 2% | 0% | pass |
| d20_bsp1 | 2 | x0.73 | x0.95 | x0.58 | 14% | 51% | 0% | FAIL |
| d20_bsp2 | 1 | x0.32 | x0.24 | x0.34 | 20% | 84% | 0% | FAIL |
| d20_bsp2 | 2 | x0.35 | x0.12 | x0.60 | 44% | 98% | 0% | FAIL |
| d20_bsp3 | 1 | x1.84 | x3.42 | x1.26 | 4% | 21% | 0% | FAIL |
| d20_bsp3 | 2 | x1.09 | x0.97 | x1.32 | 4% | 14% | 0% | pass |
| d20_bsp4 (re-solved with fixtures) | 1 | x0.62 | x0.41 | x0.93 | 6% | 27% | 0% | pass |
| d20_bsp4 (re-solved with fixtures) | 2 | x0.45 | x0.32 | x0.40 | 19% | 72% | 0% | FAIL |
| d40_bsp0 | 1 | x0.45 | x0.50 | x0.47 | 22% | 95% | 0% | FAIL |
| d40_bsp0 | 2 | x0.54 | x0.46 | x0.56 | 1% | 7% | 0% | FAIL |
| d40_bsp3 | 1 | x0.99 | x1.00 | x1.05 | 2% | 11% | 0% | pass |
| d40_bsp3 | 2 | x1.43 | x1.16 | x1.48 | 12% | 63% | 0% | FAIL |
| d40_bsp6 | 1 | x0.56 | x0.36 | x0.60 | 10% | 57% | 0% | FAIL |
| d40_bsp6 | 2 | x0.60 | x0.54 | x0.63 | 5% | 54% | 0% | FAIL |
| d40_bsp7 | 1 | x0.60 | x0.53 | x0.63 | 5% | 55% | 0% | nothing in view |
| d40_bsp7 | 2 | x0.60 | x0.53 | x0.63 | 5% | 55% | 0% | nothing in view |

- No lightmap in that level (nothing to solve from): b40 bsp6, bsp7 and bsp12, c40 bsp3 and bsp4; d40 bsp9 has
  no surfaces.

The scenes: one export a BSP (`tools/rt_truth/scenes.json`, "any_bsp"), kept on the 5900X in
`~/goat-rig/rt2-emitters-more/`.
