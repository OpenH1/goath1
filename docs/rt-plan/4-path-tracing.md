# 4. Path-traced "Ultra" mode, and Metal ray tracing

Goal: an optional `display.renderer "path_traced"` for high-end GPUs, beside the hybrid path (studies 1–3) and Classic,
plus ray tracing on Metal (M3 and later), starting with the hybrid's shadows and AO. Builds on study 1's scene
(render geometry in the acceleration structures); reuses study 2's reflection rays and study 3's GI caches as the
path tracer's fallbacks and its first bounce.

## What a path-traced Halo frame needs

- **Materials.** One PBR model a surface, read in the closest-hit code from per-instance material records (a bindless
  table: base color, normal, roughness, metallic, emissive, opacity, texture indices). Derived from Halo's shader
  tags (`shader_environment`'s base, detail and bump maps and specular tint; `shader_model`'s multipurpose map as
  ORM-ish masks; self-illumination as emissive) by fixed rules, overridden where a mod or the MCC texture import
  ships `_NORMAL`/`_METALLIC`/`_ROUGHNESS`/`_AO` maps (docs/mcc-texture-import-plan.md; the model normal/ORM maps of
  #1267). Note: this tree has no "Material maps" section in docs/mods.md and no docs/mcc-import.md yet, so the
  material-record format should be written once, here, and those docs point at it. Glass, water and chicago
  (scrolling, animated) shaders need hand-written rules each.
- **Many lights.** ReSTIR DI (reservoirs a pixel, temporal plus spatial reuse) over one light list: the enhanced
  dynamic lights (up to 128 a window, `dynamic_lights_rank`), `light_sources.c`'s extra lights, and emissive
  triangles (self-illuminated surfaces, plasma, lamps) sampled by power through a per-map emissive table built at
  load. The portal cluster masks still cull lights per cluster before sampling.
- **The sky as a light.** Each sky's cubemap (or the HDR sky of docs/hdr.md) importance-sampled with a small
  per-map CDF; the sky's sun direction (the lightmap's dominant light, as the RT shadows use today) as a delta
  light with a soft cone. Ray misses read the sky.
- **Lightmaps.** Not used for direct light. They stay (1) the fallback radiance where a path is cut off after the
  last bounce, scaled against study 3's GI cache, and (2) the art-direction check: an "Ultra" frame compared to the
  baked frame should keep each level's mood. Classic and hybrid keep them as they are.
- **Transparency and particles.** Alpha-tested foliage and grates as any-hit (or opacity micromaps where present);
  glass and water as one refraction/reflection event a path; particles, contrails, decals and the classic
  transparent pass rasterized on top of the denoised result with the hybrid's lighting (Quake II RTX and Portal RTX
  both do this). Emissive particles also feed the ReSTIR light list as short-lived lights.
- **HUD and first-person weapon.** HUD unchanged, after the denoiser. The first-person weapon traced like any
  instance (it needs study 1 to put it in the scene, flagged out of shadow rays to the player's own eye where it
  clips) with its own depth range for the denoiser's motion vectors.

## Denoising

- **NVIDIA: DLSS Ray Reconstruction**, added to our DLSS module (`libopenh1_dlss.so`, loaded by
  `port/linux/vulkan/xgpu_vulkan_dlss.c`): a second feature kind beside Super Resolution with the extra guide
  buffers it needs (diffuse albedo, specular albedo, normals, roughness, specular hit distance). Same per-window
  feature life and garbage handling as today. The add-on download grows by NVIDIA's RR library.
- **AMD and Intel: NRD** (ReBLUR/ReLAX for diffuse and specular, SIGMA for shadows), vendor-neutral, MIT, Vulkan
  compute; then FSR as the upscaler as today. AMD's own ray-tracing denoiser in FSR's newer releases is worth a
  look once its Vulkan path is public; NRD first because it runs everywhere we ship Vulkan.
- The same guide buffers serve both, so the G-buffer pass is written once.

## Performance targets and cutoff

- RTX 4090: 1440p native-ish (DLSS Quality) at 120 fps; 4K with DLSS Performance plus RR at 60–90 fps. Halo's scenes
  are small (tens of thousands of triangles), so the cost is per pixel: 1 path a pixel, 2 bounces, ReSTIR DI.
- RTX 5070 class: 1440p DLSS Balanced/Performance at 60 fps.
- Offered only on GPUs with hardware RT that meet a measured bar (a startup probe on the rig, then a device list):
  RTX 40/50 and RX 9000 class and up. Everything below keeps the hybrid path; no path tracing on the Deck, phones or
  Metal.

## Coexisting with hybrid and Classic

Three renderers, one setting. Classic is untouched (the original look, no RT). Hybrid is the default on RT
hardware (studies 1–3: raster plus RT shadows, AO, reflections, GI). Ultra swaps the opaque lighting pass for the
path tracer and keeps everything else (transparents, HUD, post, menus) shared with hybrid. All three share the
scene builder and material records, so a fix to the scene helps each. Split screen in Ultra: allowed, at the
upscaler's lowest setting, or fall back to hybrid when four windows are open.

## Metal ray tracing

Metal has nothing today (dynamic-shadows-plan.md stage 5). Needed: build `MTLAccelerationStructure`s from the
existing `WORLD_AO_MESH`/`WORLD_AO_INSTANCES` records in `port/macos/xgpu_metal_stream.m` (primitive structures once,
the instance structure each frame), bind them through the Metal stream (`xgpu_metal_stream.h`), and emit
`intersector<triangle_data, instancing>` ray queries in `port/linux/src/xgpu_msl.c` where the SPIR-V emitter emits
`rayQueryEXT`, so both stay operation for operation. Intersection functions are only needed later for alpha-tested
geometry. Gate on `supportsRaytracing`; on by default only for hardware RT (M3, A17 Pro and later). First users:
the hybrid's RT shadows and RT AO, then reflections; MetalFX stays the upscaler. No path tracing on Metal in this plan.

## Reality check (scope comparison only)

Quake II RTX (NVIDIA, built on Q2VKPT) took a small dedicated team well over a year: a new Vulkan renderer, a
material system with hand-made PBR textures for every surface, light rebalancing per map, and their own denoiser.
Portal RTX needed RTX Remix (a D3D9 interceptor, which we can't use) plus a remaster's worth of new art, and still
runs poorly below a 4080. Our advantage: we own the renderer, the scene builder and ray queries already exist, and
the scenes are tiny. Our gap is the same as theirs: art. Without real PBR maps, Halo's shaders path-traced look flat,
so the material rules and the MCC import decide how good Ultra looks more than the tracer does.

## Stages

1. **Metal RT for hybrid shadows and AO** (large). Independent of the rest; needs an M3 to test.
2. **Material records** (medium): the per-instance PBR table from Halo's shader tags plus the imported maps; hybrid
   reflections (study 2) use it first.
3. **Guide buffers plus NRD** (medium): albedo, normal, roughness, hit distance; denoise study 2/3's rays with it.
4. **Path tracer core** (large): 1 path a pixel, sky and sun lights, lightmap cutoff, opaque only, behind a setting.
5. **ReSTIR DI** over dynamic and emissive lights (large).
6. **DLSS Ray Reconstruction** in the DLSS module (medium).
7. **Transparents, first-person weapon, per-map tuning, device cutoff** (medium).

## Risks

- Art: flat-looking surfaces without real maps; per-shader rules may need per-map tweaks.
- Look: Ultra must not lose each level's mood; compare every map to its lightmapped frame.
- Upkeep: a third renderer to keep in step; keep it sharing everything but one pass.
- Licensing and download size of NVIDIA's RR library in the add-on; NRD's shaders need porting to our build.
- Metal: no M3 in the test fleet yet; Metal's RT API differs enough from Vulkan's that the stream needs its own
  records.

## First concrete step

Write the material record (one struct, the rules from each Halo shader kind to PBR) and have the Vulkan scene builder
attach its index to each instance in `xgpu_vulkan_rt.c`, unused by the shaders yet. Study 2 and the path tracer both
read it, and it shakes out which shaders need hand rules.
