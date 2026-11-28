# Static light sources, one file a BSP

`<map>_bsp<k>.json`: the level's own glowing shaders (lamps, strips, light cards, screens: radiosity
power > 0 in the shader tag) as area lights, at the tag's color and relative power, with one brightness
scale per room-sized cluster (a 12 wu cell), set so each cell's traced light matches the original lightmap's.
The ray-traced lighting traces them live instead of using the lightmap.

Made by `python -m rt_truth emitters <scene folder> --out <file>` (tools/rt_truth/README.md, "Solving the
emitters") from any rt2 scene export of that BSP. In short, each `lamps` entry has `radiance` (linear, the game's light units) and `triangles` (world
positions, lifted 0.01 wu off the surface, wound to glow toward their normal) to sample as area lights.

Not yet packaged into builds: that comes with the pass that reads them.

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
