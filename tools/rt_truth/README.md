# rt_truth: the truth renderer

Renders an rt2 scene folder (`rt2-scene-format.md`) into `truth/`: a converged, path-traced picture made with
Mitsuba 3 (an offline tool, not shipped). It defines what correct light is for the in-game ray tracing.

```sh
tools/rt_truth/run_5900x.sh render '~/goat-rig/rt2-scenes/c20_corner'      # on the 5900X's RTX 4090
tools/rt_truth/run_5900x.sh synthetic '~/goat-rig/rt2-truth-work/synthetic_box'
tools/rt_truth/run_5900x.sh check '~/goat-rig/rt2-truth-work/synthetic_box'   # the proof, with numbers
```
It runs in `~/goat-rig/rt2-truth-venv` (mitsuba, numpy, opencv-python-headless, pillow). `RT_TRUTH_VARIANT`
picks Mitsuba's variant (`cuda_ad_rgb`, else `llvm_ad_rgb`). While a player's game runs on the machine the
render waits; it holds the rig's GPU lock while it renders.

## Output
- `truth.exr`: the light, exposed (below). `mood.png`: the game's SDR picture of it. `compare.png`: the
  original on the left, the truth on the right.
- `truth_parts/`: `emission` (glowing surfaces and sky seen directly), `direct` (one bounce), `indirect` (the
  rest), which sum to `truth`; `static` (what Halo's lightmaps hold: the sky, sun and the BSP's glowing
  surfaces, every dynamic light off, all bounces); `direct_flashlight` (the flashlight's one bounce alone).
- `samples.txt`, `info.json`: samples, time, the noise left, the exposure, every placeholder and every
  choice made from free text in the JSON.

The lights are rendered in groups (static, flashlight, other dynamic lights and objects' glow) and summed;
light adds linearly, so the sum is the whole scene's. Parts by depth use the same seeds, so `indirect`
(full minus two-vertex paths) carries no extra noise.

## Units
Distances stay in world units. Every Halo source is converted so that a white Lambertian surface (BRDF
1/pi) shows radiance `power * color` where Halo's own lighting adds `power * color` to it:
- point and spot lights: intensity `pi * power * color`, radiance at a surface `albedo * power * color *
  cos / d^2` times the angular falloff and the gel (`scene.py`, `POINT_INTENSITY_PER_POWER`);
- the sun: irradiance `pi * power * color` facing it, a disc of its angular diameter;
- the sky's ambient: a constant radiance `power * color`; glowing surfaces: radiance `power * color`.

The one calibration of mood is a single global exposure per scene (`display.match_exposure`): the truth's SDR
picture shows, on average, the same light as `original.png` (mean luminance of both pictures made linear).
It is applied to `truth.exr` and every part, and recorded. Nothing local is calibrated.

## The display
`mood.png` is the game's SDR picture exactly (`display.py`): the frame's value is sRGB's encoding of the
light, extended past 1; a pixel with a channel past 1 goes through `xgpu_hdr_tone_map` at Faithful and a
peak of 1, then 8 bits, as `xgpu_hdr_sdr_pixel` (`port/linux/src/xgpu_hdr.h:887`, the tone map at `:592`,
the curves at `:410` and `:417`) writes the screenshot key's PNG.

## Mapping
- Albedo: the base map with the detail map tiled `detail_scale` times, combined as `albedo_note` says
  (double biased multiply `2 * base * detail`, multiply, biased add), clamped to [0, 1]. Textures are taken
  as stored (the format's textures are linear).
- Specular: when `specular.tint` is not black, Mitsuba's `roughplastic` (GGX) with the mask times the tint
  as its specular reflectance and alpha = roughness^2.
- Alpha test: a cutout at 0.5 of the base map's alpha. Two-sided where flagged. `transparent_*` shaders
  without alpha test pass light (null BSDF); their glow stays.
- Spot lights: the cone's `cone_deg` are full apex angles (`"cone_is": "half"` otherwise); the gel is laid
  across the outer cone like a slide in a projector, image right to the light's right, up toward the
  light's `up` (world z when not given). Distance and angular falloff follow `falloff` (a structured dict, or
  keywords in the tag's text; see `halo_plugins.py` for the modes) and the choice is logged.
- The sun's `direction` points from the scene toward the sun unless `"direction_is": "from_sun"`.

## The synthetic proof
`check` renders the spot's direct light alone in the synthetic box and holds every smooth pixel to the
analytic answer computed independently in numpy (shadow by a ray-box test). It passes when the median error
is under 0.5%, the fitted distance exponent is -2 within 0.01, the gel correlates above 0.999 with its
contrast kept, and the cube's shadow is dark on both the floor and the wall (it bends at the corner).
