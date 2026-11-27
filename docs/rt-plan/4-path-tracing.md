# 4. Path-traced "Ultra" mode

Goal: an optional `display.path_tracing "on"` for fast RT GPUs that lights every pixel the eye sees with traced
light and true visibility, and still looks like Halo. It sits beside Classic (untouched) and the hybrid (sections
1–3). It builds on the foundation in progress: the render geometry as the RT scene (`rt/geometry-*`), the Enhanced
light model calibrated to Classic (POINT, SPOT with gel, PANEL), the reflections' denoise module, the GPU bounce
cache, and the lighting referee (`tools/lighting_referee.py`). Metal is later (end of this page).

On origin when this was written (2026-11-27): `rt/geometry-2`/`-3` (indexed render meshes, the hit table, 64 B
material records with lightmap UVs, cutouts, skinned refit, masks), `rt/bounce-2` (CPU bounce lights) and
`tools/lighting-referee` (capture plumbing only: `debug.screenshot_at`, `debug.screenshot_halves`). The light model,
reflections and GPU cache branches weren't pushed yet; this plan assumes only their stated goals.

## 1. What Ultra means for Halo CE

### The decision: trace what you see, keep the bake for what it bounces off

A Classic pixel is `base x detail x lightmap` (plus cube reflections, self-illumination, fog), in gamma space. The
lightmap is the art director's measured irradiance: the radiosity tool's sun, sky, fixtures and their bounces.
Ultra does this:

- **The primary surface (what you see) is lit by traced light:** the sky's sun lights and dome, the static emitters
  (shaders with radiosity power), and every dynamic light, each with a shadow ray against the real render scene.
  That is what Classic can't do: objects shadow the level and each other per pixel, bump maps answer the real light
  direction, highlights come from every light, shadows are sharp where the lightmap is blurry.
- **One bounce ray from it, ended at the bake.** The bounce's hit returns `albedo x lightmap` (its outgoing diffuse
  light, which is what the bake measured there), plus the GPU bounce cache's dynamic-light term, plus its emission.
  Object hits (not in the bake) return section 3's per-object SH lighting. Misses return the sky.
- Reason: this is a one-bounce final gather over the original's own radiosity solution. It gives every bounce of the
  static light for one ray, never counts light twice (section 3's rule: the bake holds static light, the cache
  dynamic light), and the indirect light *is* the artist's. A full multi-bounce path tracer would replace the mood
  with physics; this keeps the mood and changes only visibility and directness.
- A second bounce (glossy on glossy: glass seen in a mirror) is a high-tier option, ended the same way.

### Gamma space, and why parity is reachable

`x^(1/2.2) * y^(1/2.2) = (x*y)^(1/2.2)`: a product survives a pure power-law decode. So Ultra decodes textures and
lightmaps with a pure 2.2 (not sRGB's piecewise curve), lights in linear and encodes with 1/2.2. Wherever Classic
only multiplies (base, detail, lightmap, multiply decals), Ultra with the lightmap as its irradiance gives Classic's
value exactly. Where Classic adds (self-illumination before the multiply, dynamic lights, specular), the light
model's calibration to Classic's math is reused. Blends after lighting (fog, decals, transparents) stay in gamma
space, as the HDR frame keeps them (docs/hdr.md, "The frame").

### Faithful: fit the lights to the lightmap, then correct the rest

Traced direct light only matches the bake if the light powers do, and Halo's units don't say how bright its sun is:

1. **Fit** (at load, on the GPU, spread over frames). With no dynamic lights or objects, trace the static light at a
   sample of lightmap texels (jittered over each texel's footprint, many samples a texel): each source group's
   contribution `D_k` (each sky light, each emitter shader) and the gathered bounce `G`. Solve least squares
   `L ≈ G + sum(e_k * D_k)` for the few powers `e_k` (one to four sky lights, 10 to 50 emitter shaders a map).
2. **Correct** with a ratio map `R = L / T` per lightmap texel (T: the fitted traced static light there, blurred as
   the lightmap is), RGB, smoothed, clamped to 0.25..4, RGBA16F at lightmap resolution. At run time the primary's
   static light is `traced x R(lightmap UV)`: the lightmap sets the low-frequency energy and color (tinted rooms,
   hand-boosted areas, art cheats), the tracing supplies the visibility. A Warthog's sun shadow scales with R, so it
   keeps the right darkness. Until R is ready it is 1.
3. **Objects' anchor.** Classic lights an object by one lightmap sample (`render_lighting`). Per object, compare the
   traced light at its lighting point with Classic's ambient plus distant lights and keep a smoothed, clamped gain,
   so the Chief is as bright as the art expects while his shading becomes per pixel.

`display.path_tracing_faithful` (0 to 1, default 1) blends R and the anchor toward 1 ("relit": fitted powers alone).
How far R is from 1, per map, says how physically consistent each bake is; it is the first thing to measure.

### The original's art, mapped

| Halo | Ultra |
| --- | --- |
| Lightmap color | Bounce terminator and fit target; never direct light at the primary |
| Lightmap incident direction | Not needed: bump maps use the real light directions |
| `shader_environment` base, detail (2x multiply), bump | Albedo and normal from the draw's own program (G-buffer, below); material records for bounce hits |
| `shader_model` multipurpose (change color, self-illumination, specular masks), #1267 normal/ORM maps | The same; ORM roughness where present, per-class roughness otherwise |
| Reflection cube, its masks, perpendicular/parallel colors | Specular albedo `mask x lerp(parallel, perpendicular)`; the traced reflection replaces the cube texel (section 2's rule) |
| Self-illumination | Emission `base x color x mask`, under hdr.md's 2x ceiling; animated or object self-illumination is a dynamic emitter (not in the bake) |
| Shaders with radiosity power (fixtures, Covenant lights) | Static emissive triangles in the light list, power from the fit |
| Sky model | Rendered once into an HDR cube at load (again when the sky changes); misses and dome sampling read it |
| Sky lights (the sun) | Delta lights with a small cone, from the sky tag's lights block (not decoded yet: `sky_definitions.h` stops at the fog), power from the fit |
| Atmospheric and planar fog, fog screens | Classic's math over the lit opaque, after denoising; fog screens drawn as today. No volumetrics (Halo's fog is flat by design) |
| Glass, water | One reflection/refraction event a path; water keeps its wave normals |
| Transparents, particles, decals, HUD | Rasterized as today (multiply decals commute with light; additive ones are light) |

### The referee's faithful target

Added to `tools/lighting_referee.py`, each a pass/fail number per map (Blood Gulch, a10, a30, b30, c10):

- **Composition:** the G-buffer's albedo times the lightmap, encoded, equals Classic's opaque frame to quantization.
  It proves the guides before a ray is cast.
- **Faithful static:** Ultra (faithful 1, dynamic lights off) vs Classic per region: mean linear luminance within
  10%, mean Oklab difference small; shadow-edge pixels left out by a mask from the visibility buffer.
- **Visibility off:** shadow and bounce rays off: Ultra's dynamic-light term equals Enhanced, which equals Classic.
- **Shadow correctness:** the referee's checks, now on sun and fixture shadows too.
- **HDR:** no surface past paper white except hdr.md's categories and ceilings.
- **Bias and stability:** the denoised frame vs a 1,024-sample accumulated reference of the same frame; flicker on a
  still camera (frame-to-frame variance) under a bound.
- **Cost:** pass times against the tier budgets below.

## 2. Architecture

### The frame

1. **G-buffer** at render resolution, written by the opaque draws' own translated programs in a demodulated variant
   (lightmap input 1 gives albedo, 0 gives emission), plus normal (after bump), roughness, specular albedo, motion,
   view depth, and each pixel's instance and primitive (a visibility buffer). Reason: what you see must be what
   Classic draws (its LOD, scrolling and animated textures, the first-person weapon's projection); traced primaries
   would see the ray scene's single LOD and the weapon a frame late. The same buffers serve RR, our denoiser and
   section 2.
2. **Trace** (compute): ReSTIR DI for the primary, the sun ray, one bounce ray, the bounce's shadow ray. About 4
   rays a pixel.
3. **Denoise** (RR, or our denoiser then FSR), then **compose** (fog, transparents, HUD as today).

### Ray queries, and SER later

Everything today is ray queries (`VK_KHR_ray_query`) in compute and fragment passes; there is no ray tracing
pipeline. Ultra keeps ray queries: Halo has five material kinds, so shading inline in a switch is cheap; AMD runs
queries as well as or better than pipelines; and the scene, hit table and candidate loop are already written.

SER (`VK_EXT_ray_tracing_invocation_reorder`, multi-vendor since Vulkan 1.4.333, or the older NV one) works only in a
ray tracing pipeline's raygen shader (hit objects, then a reorder). So the trace pass is written as trace / hit
record / shade functions, and an NVIDIA variant puts the same functions in a minimal pipeline (one raygen, one miss,
no hit shaders) that reorders on material and hit/miss. It ships only if it measures at least 15% faster on a 40/50
card; with so few materials the gain is mostly ray coherence and may be small.

### Shaders

Not hand-emitted like the NV2A translations in `xgpu_spirv.c`: Ultra's kernels are fixed code, written in GLSL,
compiled offline with glslangValidator and checked in as generated C, as FSR's are (`tools/vulkan_fsr_shaders.py`,
`xgpu_vulkan_fsr_shaders.c`). New files: `port/linux/vulkan/pt/*.glsl`, `tools/vulkan_pt_shaders.py`,
`port/linux/vulkan/xgpu_vulkan_pt.c`.

### Licenses decide what's ours

| SDK | License | Use |
| --- | --- | --- |
| RTXDI | NVIDIA RTX SDKs License | Ideas only (ReSTIR is published: Bitterli et al. 2020; Wyman et al.'s course notes); no code in the tree |
| RTXGI 2 (SHaRC, NRC) | NVIDIA RTX SDKs License | Ideas only; SHaRC's hash grid is what section 3's cache is |
| NRD (ReLAX, ReBLUR, SIGMA) | NVIDIA RTX SDKs License | Not in the tree (conflicts with the AGPL). Ours: the ReLAX-style `xgpu_vulkan_denoise.c` (owned by the GI work) |
| DLSS SR and RR (NGX) | NVIDIA RTX SDKs License | Only in the DLSS add-on, as today |
| Streamline | MIT core; NVIDIA's plugins under their license | Only for frame generation on Windows, in the add-on |

The RTX SDKs License forbids putting its code under an open-source license, so nothing of RTXDI, RTXGI or NRD goes
in the repository. The DLSS add-on already solves the rest: the game's package holds no NVIDIA file
(`tools/dlss_addon.py check-package`); each release carries `openh1-dlss-<platform>-310.9.16-<hash>.zip` (the MIT
module `openh1_dlss.c`, NVIDIA's runtime, NVIDIA's license), which the game downloads after showing the license.

### Light sampling: our own ReSTIR DI

- **One light list** a frame: the Enhanced lights (the light model's POINT, SPOT with gel, PANEL; Ultra lifts the
  128 cap to everything ranked in view, say 1,024), the static emissive triangles (built at load from the BSP's
  materials with radiosity power; power: area x fitted power), and moving emitters (`light_sources.c`, animated
  self-illumination). Cluster masks cull per cluster first.
- **The sun isn't in ReSTIR:** one or two sky lights get a shadow ray each, noise-free and cheap. The dome is
  sampled by the bounce ray (BRDF sampling that misses into the sky cube).
- Per pixel: 8 candidates from presampled light tiles (RTXDI's idea: tiles of 1,024 lights sampled by power each
  frame), one visibility ray, temporal reuse (history 20, reset per light id when a light dies), one spatial pass
  over 4 neighbors, final shading with one shadow ray. Biased mode (cheaper; the referee's bias check catches drift).
- **ReSTIR GI: not at first.** The bounce ends on smooth sources (bake, cache, SH), so one bounce ray at render
  resolution should denoise well. Added only if the referee's noise number fails on the top tier.

### Radiance cache: our hash grid, not NRC

The terminator for dynamic light is section 3's GPU hash-grid cache, unchanged. A neural radiance cache (RTXGI 2's
NRC) is NVIDIA-only, binary-only and licensed against us, and Halo's scenes are small enough for a hash grid at fine
resolution. No second cache.

### Opacity micromaps

`VK_EXT_opacity_micromap` is NVIDIA Ada and later (not RADV today). Halo's cutouts are few (a10: 504 triangles, a30:
582) and the candidate loop already handles them everywhere. Built later only if cutout candidates are over 10% of
trace time on foliage (b30's trees): 4-state micromaps from the cutout map at level 1, subdivision 4 to 6, the
"unknown" state still running the loop, so the result is exact.

### Denoising

- **RTX: DLSS Ray Reconstruction** (denoise and upscale in one), a second feature kind in the add-on module beside
  SR; the add-on also packs NVIDIA's RR runtime (`nvngx_dlssd.dll`, `libnvidia-ngx-dlssd.so.*`). Inputs: the noisy
  linear color, diffuse albedo, specular albedo, normals, roughness, depth, motion, and optionally specular hit
  distance. Because RR also upscales, everything goes in before it: fog and transparents are composed into the
  noisy color and flagged in RR's bias-current-color mask; the HUD comes after.
- **Everyone else: our denoiser** on demodulated diffuse and specular radiance (with hit distance, normal,
  roughness, view depth, motion, disocclusion), before transparents; then FSR as today.
- The guides are written once (G-buffer) for both.

### Upscaling and frame generation

- Ultra always upscales: RR on RTX, FSR elsewhere; native (DLAA) only on the top tier at 1080p.
- **Frame generation is separate and last.** DLSS FG goes through Streamline, which for us is Windows only, so it
  would be a Windows part of the add-on; FSR 3's frame generation (MIT, its source already in
  `port/third_party/fidelityfx-fsr3`) covers the rest. It needs the HUD-less color and the HUD apart and a base of at
  least 60 fps, and changes no budget below (budgets are base frames). Native Linux DLSS FG is an unknown.

### Memory (extra, at 4K output, 1080p render)

G-buffer guides about 40 B a pixel (80 MB), ReSTIR DI reservoirs for two frames at 32 B (130 MB), the light list and
tiles (under 10 MB), the hash grid (section 3's, shared), R maps (twice the lightmaps), the sky cube (24 MB),
denoiser history (about 60 MB), RR's own (a few hundred MB, NVIDIA's). About 0.5 to 0.8 GB in all: fine on the 8 GB
and larger cards of the Ultra tiers. The scene is small (a10's level BLAS 6.9 MB).

## 3. Tiers and budgets

The base frame is about 1 ms on the 8060S (median 1.16 ms at 1080p with RT shadows, docs/dynamic-shadows-plan.md)
and 0.64 ms on the 4090 at 4K. Ultra's cost is per pixel; these are ceilings the referee checks, Ultra's passes only.

| Tier | Has | 1080p output | 4K output | Offered |
| --- | --- | --- | --- | --- |
| RTX 4080/4090, 5080/5090 | SER, OMM, RR | 3 ms (DLAA) | 6 ms (RR Performance: 1080p render) | Yes |
| RTX 4060–4070, 5060–5070 | SER, RR | 4 ms (RR Quality) | 8 ms (RR Ultra Performance) | Yes |
| RTX 30, RX 7800/7900, RX 9000 | no SER; RX: no RR (our denoiser, FSR) | 6 ms (Quality) | 10 ms (Performance) | Yes; RTX 30 and RX 7000 as "experimental" |
| RX 6000, Steam Deck, 8060S/780M class | RDNA 2/3.5 ray rates | runs, for development | — | No: hybrid instead |

The split to aim for: about 40% tracing, 25% ReSTIR passes, 25% denoise and upscale, the rest G-buffer and
composition. Split screen: Ultra up to two windows at the upscaler's lowest mode, hybrid at three or four. No VR. The
8060S stays the rig's Ultra machine for correctness at a 720p render (the referee runs there); its times are logged,
not judged.

## 4. Build order

Each stage is behind `display.path_tracing` (a `debug.` switch until stage 8), shippable alone, with its referee
checks. Letters are lanes that can run at once without touching the same files.

0. **Measure** (small) [A]: ray throughput on our scenes (incoherent diffuse rays a second, 4090 and 8060S), and the
   static fit study (accumulated traced static light vs the lightmap at Blood Gulch, a10, b30: R's spread). If R is
   far off on most texels, faithful leans on R and the fit is rethought before stage 3.
1. **Guides** (medium) [B, after reflections-1 lands: same files, `d3d8_device.c`, `xgpu_spirv.c`]: the demodulated
   G-buffer and visibility buffer, debug views. Referee: composition.
2. **Core** (large) [A]: `xgpu_vulkan_pt.c` and its GLSL: primary from the G-buffer, sun rays, the sky cube, one
   bounce to bake/SH/sky, an accumulation mode for still frames (the reference). Powers 1. Referee: relit numbers
   recorded; the bias reference exists.
3. **Fit and anchor** (medium) [C: game side, `port/linux/game/pt_lights.c` and the sky lights decoded in
   `sky_definitions.h`; the GPU fit in A's files after 2]: the emitter table, sky lights, power fit, R maps, object
   anchor. Referee: faithful static.
4. **Denoise path** (medium) [A]: feed `xgpu_vulkan_denoise.c`, then FSR. Referee: bias and stability; cost on RX 7000.
5. **ReSTIR DI** (large) [A]: dynamic lights and emitters. Referee: visibility off, shadow correctness.
6. **Specular** (medium) [A]: the primary's specular from DI lights and section 2's reflection ray through Halo's
   masks; the optional second bounce. Referee: faithful static (specular regions), HDR.
7. **Ray Reconstruction** (medium) [D: `openh1_dlss.c`, `xgpu_vulkan_dlss.c`, `tools/dlss_addon.py`; any time after
   1]: the RR feature and its runtime in the add-on. Referee: bias and stability on RTX.
8. **Shipping** (medium) [A]: fog and transparents for both denoise paths, the first-person weapon's guides (world
   depth from its reconstructed position), split screen, the device gate (a startup probe on the rig, then a device
   list), the menu row. Referee: all checks, at each tier's budget.
9. **SER variant** (medium) [A]: the pipeline twin of the trace pass; kept only at 15% or more.
10. **Opacity micromaps** (small, optional) [E: `xgpu_vulkan_rt.c`, when no geometry work is open].
11. **Frame generation** (later) [D]: Streamline on Windows in the add-on, FSR 3 FG elsewhere.

The referee lane runs beside all of them: each stage's checks land in `tools/lighting_referee.py` with the stage.
The hot files (`xgpu_spirv.c`, `xgpu_vulkan_rt.c`, `d3d8_device.c`) take one helper at a time.

## 5. Risks, unknowns, what to measure first

Measure first, in this order:

1. **The fit residual** (stage 0): how close traced static light gets to each map's lightmap. "Faithful" rests on it.
   Unknown: whether the radiosity tool's powers and tints behave linearly enough for one power per emitter shader.
2. **Ray throughput** on Halo's scenes, on the 4090, an RTX 30 card and the 8060S: it fixes the tier table.
3. **RR through our module:** that NGX 310.x's RR runtime loads on Linux and Windows Vulkan, and the add-on's size.
4. **SER's gain** on a 40/50 card with five materials: decides stage 9.

Risks:

- Albedo: Halo's base maps carry painted shading, so traced light on them can darken crevices twice. R absorbs the
  average; contrast may need a per-class albedo curve.
- The bake terminator ignores dynamic occluders at the bounce's hit (a Warthog doesn't darken the wall a bounce
  reaches): small, and the hybrid never had it either.
- Lightmap leaks and seams now show through the bounce as well; R spikes near cracks (clamped).
- ReSTIR reuse ghosting behind fast plasma; reset per light id, check on Blood Gulch with bots.
- Three renderers to keep in step: Ultra shares the scene, materials, light model, cache, composition and post with
  the hybrid, and owns only the G-buffer variant and the trace pass.
- NVIDIA's terms: anything under the RTX SDKs License stays in the add-on, and a future version could limit Linux or
  open-source hosts. The in-tree path (our ReSTIR, cache, denoiser, FSR) never depends on it.

## Later: Metal ray tracing

Out of scope for now (decided 2026-11-26). Notes kept for when there is an M3 to test on: build
`MTLAccelerationStructure`s from the same `WORLD_AO_MESH`/`WORLD_AO_INSTANCES` records in
`port/macos/xgpu_metal_stream.m` (primitive structures once, the instance structure each frame), bind them through
the Metal stream (`xgpu_metal_stream.h`), and emit `intersector<triangle_data, instancing>` ray queries in
`port/linux/src/xgpu_msl.c` where the SPIR-V emitter emits `rayQueryEXT`, so both stay operation for operation. Gate
on `supportsRaytracing`, on by default only for hardware RT (M3, A17 Pro and later). First users: the hybrid's RT
shadows and AO, then reflections; MetalFX stays the upscaler. Ultra's GLSL kernels would go through SPIRV-Cross as
the post shaders do (`tools/metal_post_shaders.py`). No path tracing on Metal in this plan.
