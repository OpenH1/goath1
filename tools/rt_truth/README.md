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
- `truth.exr`: the light, in the game's units (1.0 = paper white). `mood.png`: the game's SDR picture of it.
  `compare.png`: the original on the left, the truth on the right.
- `truth_parts/`: `emission` (what the camera sees of glowing surfaces and the sky), `direct` (one bounce),
  `indirect` (the rest), which sum to `truth`; `static` (what Halo's lightmaps hold: sky, sun and baked
  glow, every dynamic light off); `direct_flashlight`; and `irradiance_direct*.exr`, the same light on
  white Lambertian surfaces with the same normal maps and cutouts: the demodulated irradiance the
  real-time comparison uses (`irradiance_direct_flashlight_geometric.exr` without normal maps). Bump maps
  shade in the game's frame (the exported vertex tangent and binormal, `n = normalize(T b.x + B b.y + N
  max(b.z, 0.05))`), their mip level chosen by the pixel's footprint as a GPU samples it.
- `static_fit_residual.png/.exr` (`--static fit`): the lightmap less the fitted static light.
- Renders save their sums (`_state.npz`) and resume from them, so they can run in short chunks.
- `mask.png`: the pixels comparisons count (not the HUD's `mask_rects`, not the first-person weapon/hands).
- `samples.txt`, `info.json`: samples, time, noise left, the fit's scales and R^2, every placeholder and
  every assumption.

## Static light, light groups and units
The static light is Halo's lightmap (`--static lightmap`, the default): each lightmapped BSP surface emits
radiance albedo x lightmap, the page's texel at the surface's lightmap UV decoded ^2.2 (the game's frame is
albedo x lightmap in gamma space, so its light is albedo_lin x lightmap^2.2: a white surface shows the
lightmap's value, which is irradiance / pi in the game's light units), and reflects none of it (the bake
holds its own bounces). Objects are lit by that light, bounce it and show it in reflections. The pages are
packed in one atlas texture. `--static fit` instead rebuilds the static light from the sky, the sky's lights
and the radiosity emitters, each fitted to the lightmap with one scale (R^2 reported): b30 0.566, and a10's
lightmap has no source in the tags, which is why the lightmap is the truth's static light.

Groups render separately and add (light adds linearly): `lightmap` (or, fitting, `ambient`, `sun`,
`emitters`), `flashlight`, `dynamic` (every other light, and unbaked objects' glow). Dynamic lights are in the
game's own units (color and attenuation table decoded ^2.2) unless a physical model is asked for:
- `--flashlight-scale k` (rt2-calibration.json): a forward-facing disk at lights.json's `emitter_position`
  (the lamp), cos at the emitter, 1/d^2 x (1 - (d/r)^4)^2, the gel (projected from the drawn light) as profile;
- `--dynamic-scale k`: the other lights the same way from their emitter, a point (no emitter cosine) or a
  spot's cone ramp. A light's intensity is pi x the game's value, so a surface shows albedo x value x cos / d^2.
`--flashlight-direct-only` / `--dynamic-direct-only [--light i]` render only that group's direct light (and
its irradiance), for comparing a real-time light before the whole truth.

## Solving the emitters (indoor static light, traced live)
The lightmap is the answer key for the level's static light sources: `python -m rt_truth emitters <scene>
--out <map>_bsp<k>.json [--report DIR]` (`emitters.py`) writes the level's emitter file (rt2-scene-format.md,
"Emitters"), and `render <scene> --static emitters --emitters <file>` traces the static light from it (group
`solved`: the BSP with its full materials and objects, lit only by the file's sources, every bounce traced),
writing `static_fit_residual.png` (the bake less the traced light at the lit BSP pixels) and `bake_bsp.png`.
Any export of the level will do (the whole BSP is used, not the view): `scenes.json` has the commands.
- Samples are the BSP's lightmap texels (centres inside each lightmapped triangle; a triangle without one
  gets one at its middle), weighted by world area, up to `--samples`; their target is the texel ^2.2, the
  radiance a white surface shows (see "Static light"). Glowing and transparent surfaces are left out.
- Transport is the lightmap tool's world: the BSP alone (no objects), diffuse surfaces of the BSP's albedos,
  cutouts passed, next-event estimation at each vertex, up to 8 vertices. Each light event is credited to its
  source (emitter registry id to source), so one pass gives every source's light at unit power.
- Sources, in order of preference: (1) the shaders with radiosity power (environment lamps, light cards:
  transparent ones glow on both faces), at the tag's color x power x one fitted scale each; (2) the sky's
  ambient dome and each sky light (other light tags are objects', which the bake never saw); (3) the indoor
  ambient floor (the sky's indoor ambient radiosity, a constant); (4) only for what those leave, added
  rectangles (`--max-added`, each kept only if it adds `--min-gain` of R^2) on the brightest texels near the
  largest positive residual, one-sided, light only (not drawn), RGB power free.
- The fit: area-weighted nonnegative least squares; the shaders' scales are pulled toward one shared scale
  (the tag's relative powers as the prior), the pull chosen by two-fold spatial cross-validation (8 wu cells),
  so nearly collinear sources (a lamp and its neighbour, sun and dome) do not trade light. R^2 is of the
  luminance over the samples; `--report` writes each page's residual and the bake beside the fit.

## The path tracer
`integrator.py` is Mitsuba's unidirectional path tracer (next event estimation, MIS, Russian roulette)
written out with symbolic loops so that shadow rays pass alpha cutouts (Mitsuba's `path` stops them there,
which gave 10^4 fireflies from the sun behind foliage). It splits each path's light into emission, direct and
indirect as it goes. It matches Mitsuba's `path` on the synthetic box (mean 0.020348 both) and passes the
analytic check below.

## Comparing a real-time render
`python -m rt_truth compare <scene> <exr> [--part direct_flashlight] [--quantity irradiance] [--out DIR]`:
16x16 tile bias, relMSE and a signed error map; definitions in `compare.py`.

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
