# 3. Ray-traced global illumination (bounced light)

## Where we start

- **Static light is done well already.** The lightmaps carry the baked sun, fixtures, static self-illumination and their
  bounces. This plan never re-lights anything static.
- **Dynamic light has no bounce.** The enhanced dynamic lights (`port/linux/game/dynamic_lights.c`, `light_sources.c`,
  `source/objects/object_lights.c`) are up to 128 a view (`XGPU_DYNAMIC_LIGHTS_MAXIMUM`), with cluster masks so they
  don't shine through walls. They are added to the lightmap pass and the model pass (`xgpu_spirv.c`, `xgpu_msl.c`)
  and light only the surface they hit directly.
- **Objects get one sample.** `object_get_cached_render_lighting` (`source/render/render_objects.c`) gives each object
  one `render_lighting`: an ambient color, two distant lights, point lights and a shadow vector, all read from the
  lightmap at one point. A Warthog half in shade is lit as one color.
- **RT plumbing exists, on Vulkan only.** `world_rt.c` builds the ray-query scene, which `rt_shadows.c` and the RTAO
  (`world_ao_shading.c`) trace. Metal has no ray queries yet (`halo_world_ao_ray_queries_missing`). FSR3, the DLSS
  add-on and MetalFX are in (`docs/temporal-aa.md`), with motion vectors and jitter.

## The rule that stops double-counting

The GI term is **the bounce of dynamic light only**: `indirect = bounce(dynamic lights' direct light x hit albedo)`.
It is added in the same place as the enhanced dynamic lights' direct term, multiplied by the diffuse texture like the
lightmap. The lightmap stays as it is. A ray's hit shades with dynamic light only, never with the hit's lightmap
value, because that bounce is already baked. The one exception is dynamic objects (below). They are not in the bake,
so reading the lightmapped surroundings for them counts nothing twice. Static self-illumination is baked too, so only
moving emitters count as new light sources. Classic dynamic lights get no GI.

## Options for the bounce

| | DDGI probes | World-space hash-grid cache (SHaRC-style) | ReSTIR GI (per pixel, screen reuse) |
| --- | --- | --- | --- |
| Quality | coarse, leaks at thin walls | per-surface, no probe leaks | best, sharp contact bounce |
| Reaction to a 0.3 s explosion | slow (hysteresis lags) | fast with short cell decay | fast |
| Stability | very stable | stable after a denoise | noisy, needs a strong denoiser |
| Objects | lit for free from the probes | needs a separate probe | per pixel, costly |
| Cost | low, fixed | medium, scales with resolution | high |

Halo's dynamic lights are small, short-lived and fast (plasma, needles, explosions). DDGI's lag and coarseness suit
them badly. The pick is a **hash-grid cache** fed by one short ray per pixel (half resolution on low-end GPUs), with
ReSTIR GI as a high-end option on top. A cache cell resets when its lights change a lot, keyed on the stable light
ids from `dynamic_lights_rank`.

## Stages

1. **Bounce lights, no RT GPU needed (small).** Each frame, the 16 strongest ranked lights each cast about 8 fixed
   rays on the CPU through the collision BSP. Each hit adds a dim, wide, bounce light: no glow, no specular, colored
   by the hit shader's average base-map color (collision material → its shader, averaged once at map load). These are
   ordinary enhanced dynamic lights, so they carry cluster masks and run on Vulkan, Metal and any GPU. A share of the
   128 slots (about 32) is kept for them, ranked below real lights. It gives the colored plasma spill on nearby walls.
2. **Separate indirect slot (small).** An "indirect dynamic" input in the emitters next to the direct dynamic term,
   with an albedo-demodulated output that later stages denoise on their own.
3. **GPU one-bounce with a hash-grid cache (medium, Vulkan RT).** It uses study 1's render geometry, with UVs and the
   shader id per triangle. Cache cells sit on hit surfaces. Each cell's radiance is dynamic direct light (grid lights,
   plus a shadow ray) times albedo. This replaces stage 1 where ray queries exist.
4. **Better object lighting (medium).** Per object, about 64 rays against the scene. They read the lightmap at each hit
   (study 1 must carry lightmap UVs and page) plus the stage 3 cache for the dynamic part, projected to L2 spherical
   harmonics (SH). Large vehicles get one per node group. The result is evaluated per pixel with the pixel's normal,
   so shade falls across a vehicle and a lit wall's color reaches the side facing it. It is kept in a port-side table,
   never in object memory, and `shadow_vector` stays for the shadows.
5. **Moving emitters (small to medium).** Animated self-illumination on objects, energy weapons and bright additive
   effects become stage 1 or 3 sources, as `light_sources.c` already does for the needle and fuel rod.
6. **High end (large).** ReSTIR GI plus DLSS Ray Reconstruction, and the Metal port once Metal has ray tracing
   (`docs/dynamic-shadows-plan.md`, stage 5).

## Denoising

The indirect term gets its own temporal pass and an a-trous filter on demodulated irradiance before it is composed. FSR,
TAA and MetalFX then see a mostly stable image. Light-change resets limit ghosting, because DLSS takes no reactive mask.
On NVIDIA, DLSS Ray Reconstruction can replace our denoiser and the upscaler together. It needs the RR model in the
add-on's pin (`dlss_addon_pin.h`) and guide buffers: diffuse and specular albedo, normals, roughness, depth and motion.
Only the last two exist today.

## Cost (targets to measure, not promises)

- **4090:** stages 1-5 at 4K are well under a few ms. ReSTIR GI with RR is a few ms more.
- **AMD iGPU (780M/890M, the Deck):** stage 1 costs about 128 CPU rays a frame, nothing on the GPU. Stage 4 costs about
  64 rays x 100 objects, which is small. Stage 3 runs at half or quarter resolution, or stays off.
- **Multipliers:** split screen and VR multiply the per-pixel parts by each window. The cache and the object probes
  are shared across windows.

## Risks

- Leaks through walls: bounce lights without a shadow test light rooms behind the hit. Cluster masks limit this.
  Stage 3 adds visibility rays.
- Explosions flicker or lag in the cache's history.
- The look drifts from the original (brighter interiors). It ships off by default as its own setting, enhanced lights
  only.
- The 128-light cap: bounce lights could crowd out real lights.
- Study 1 must carry lightmap UVs and shader ids. Collision geometry alone is not enough for stages 3 and 4.
- Metal and RADV iGPUs limit stages 3 and up.
- It must stay render-only: nothing simulated, nothing sent between machines.

## First concrete step

Stage 1: a new `port/linux/game/dynamic_lights_bounce.c`, called from `dynamic_lights_submit` after
`dynamic_lights_rank` (`port/linux/game/dynamic_lights.c`). It casts fixed rays with the game's collision vector test
and makes the per-shader average-albedo table at map load. Bounce lights go through the existing submit path with a
"bounce" flag (no glow, no specular), behind a new `display.dynamic_light_bounce` setting in `port_config.c`.
