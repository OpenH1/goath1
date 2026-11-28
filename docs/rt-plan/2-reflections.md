# 2. Ray-traced reflections (Vulkan)

**Today.** Reflections are the game's fixed cube maps, sampled by the translated NV2A texture stages in
`port/linux/src/xgpu_spirv.c`: `XGPU_TEXTURE_CUBEMAP` (a model's or environment's reflection, the vector from the
vertex program) and `XGPU_TEXTURE_DOT_REFLECT_SPECULAR[_CONSTANT]` (bumped environment: `r = 2n(n.e)/(n.n) - e`).
The combiners then weight that texel by the shader's masks (specular/reflection mask, perpendicular/parallel color),
and models add their dynamic-light highlights to it (`shine_variable`, `XGPU_DYNAMIC_LIGHTS_HIGHLIGHT`). The only RT
today is `display.ambient_occlusion "ray_traced"` (`port/linux/vulkan/xgpu_vulkan_rt.c`, ray queries over
**collision** meshes, no attributes). Note: there is no "Material maps" section in `docs/mods.md` and no roughness
path in the code yet; PBR roughness exists only as a plan (`docs/mcc-texture-import-plan.md`, MCC `_ROUGHNESS`).

**Core idea: replace the cube texel, keep the combiners.** The traced result stands in for the cube sample in the
reflection stage only; every mask, tint, fresnel and blend after it stays Halo's. So the art direction is unchanged
and a surface with a zero mask costs a trace we can skip.

## Assumptions about study 1 (geometry)
- Render geometry (not collision) in the BLASes: BSP per material cluster, objects per region/permutation, skinned
  bipeds refit each frame; first-person weapon and hands **not** in the TLAS.
- A hit record reachable from a ray query (instance custom index + geometry index + primitive id): vertex/index
  buffer addresses, UV and lightmap UV, base-bitmap index into a bindless texture array, lightmap page, alpha-test
  flag, and per-object lighting (ambient + directional, as the game computes it).
- Instance mask bits we can use: world, objects, reflection-only (see below). AO keeps its own mask.

## Which surfaces trace
The game side tags each draw (a new small per-draw record, like the AO `world_ao` key; today's `MARK_OWNER` is
debug-only) with a reflection class and the stage index holding the **reflection** cube. That matters: environment
bump shaders also use normalization cubes, which must never be traced. Classes: environment with reflection or bump;
Covenant metals (model shaders with a reflection mask in the multipurpose map); `shader_transparent_water`;
`shader_transparent_glass`. Everything else, and any draw the game doesn't tag, keeps its cube.

## Shading the hit (cheap path)
Base texel x lightmap sample (BSP) or the object's own lighting (models), plus the existing dynamic-light sum (the
`dynamic_light_sum` code reused in the trace shader). Alpha-tested hits walk candidates in the ray query; transparent
hits are skipped. No second bounce: a reflective hit returns its own cube texel. Misses return the original cube
(sky). Past a max distance (setting, default ~25 wu) the ray stops and blends to the cube over the last 20%.

## Roughness, denoise, resolution
- **Roughness per class** (Halo has none): water/glass ~0 (water's wave normal does the work), Covenant metal low,
  bumped environment medium, scaled by the cube's mip bias. MCC `_ROUGHNESS` maps plug in here when that import lands.
- **Tracing at half resolution** (half each axis), one ray per pixel, GGX sampling with per-frame blue-noise jitter.
  A pre-pass writes normal, roughness and class (extend the world-AO pre-pass, `world_ao_prepass_begin` in
  `d3d8_device.c`); a compute pass traces into an RGBA16F image plus hit distance.
- **Denoise:** temporal reprojection on the motion vectors DLSS/FSR already get (`xgpu_vulkan_dlss.c`), using hit
  distance for the reflected point's parallax; then an edge-aware spatial filter scaled by roughness; then a
  depth/normal-guided upsample to full res. TAA/FSR/DLSS clean what's left. Near-mirror pixels skip the blur.
- **DLSS Ray Reconstruction** is a later option: it needs albedo/specular-albedo/normal/roughness guides a forward
  renderer doesn't have, and it replaces our DLSS SR path. Not the first target.
- The reflection stage samples the resolved image at the pixel's screen position instead of the cube.

## First-person weapon and the player
- The FP weapon is drawn in its own projection (`xgpu_depth_remap`), so it is never in the TLAS (it would float at the
  wrong scale). Its own shiny parts keep the cube in stage 1; tracing from them (world position from the remap) is a
  later stage.
- The player's third-person body joins the TLAS as a **reflection-only** instance (mask bit not set on AO or primary
  rays) so you see yourself in water and glass. Needs the game to keep posing it in first person
  (`render_interpolation.c`'s pose).

## Cost (estimates, to be measured)
Halo's frame is tiny (4090, a10: 0.64 ms GPU at 4K with AO off; RT AO pass 0.37 ms, `docs/world-ao.md`).
- **RTX 4090, 4K:** half-res trace ~2M rays, about 0.5 ms trace+shade, 0.4 ms denoise/upsample: ~1 ms total,
  ~1.5 ms on water-heavy views.
- **Radeon 8060S, 1080p internal + FSR:** ~0.5M rays, about 1.5-2 ms trace+shade (RDNA 3.5 ray rate is a fraction of
  Ada's), 0.7 ms denoise: ~2.5 ms. Default there: water, glass and Covenant only; bumped environment keeps cubes.

## Stages
1. **Small:** game-side reflection-class record + stage index, passed to `xgpu_spirv.c`; debug view tinting tagged
   pixels. No tracing yet.
2. **Medium:** mirror-only trace (water, glass) at half res, cheap hit shading, cube fallback, upsample. Needs study 1.
3. **Medium:** roughness classes, jitter, temporal + spatial denoise; Covenant metals and bumped environment.
4. **Small:** player reflection-only instance; per-GPU defaults and a setting (`off` / `mirrors` / `all`).
5. **Large (optional):** DLSS Ray Reconstruction guides; tracing from the FP weapon.

## Risks
- Telling reflection cubes from normalization cubes, and models whose cube carries baked tint: tagging must come from
  the shader tag, not guessed from texture-stage state.
- Cube maps are hand-authored mood (often brighter than the scene); a straight swap can look flatter. Keep a per-class
  blend toward the cube.
- Hit shading of objects is approximate (no per-pixel lightmap); bipeds may look flat in reflections.
- Reprojection ghosting on moving water and fast turns; skinned-mesh refit cost if study 1 doesn't solve it.
- Metal/macOS has no path here; it keeps cubes.

## First step
Add the per-draw reflection record: in `d3d8_device.c` (where draws get their `world_ao_key`) carry the shader
class and the reflection stage from the game's rasterizer shader setup into the draw key; read it in
`xgpu_spirv.c` at the `XGPU_TEXTURE_CUBEMAP` / `DOT_REFLECT_SPECULAR` cases; add a debug tint
(`HALO_VULKAN_RT_REFLECT_DEBUG=1`) to confirm on a10, Blood Gulch and a Covenant ship that exactly the right
surfaces light up.

## Prototype: mirror reflections (rt/reflections-1, 2026-11-27)

`display.rt_reflections` (off by default; Vulkan with ray queries only, the cube maps elsewhere). It replaces the
cube map's texel and nothing else, so the masks, fresnel and tints after it stay Halo's.

- **Where the original reflects.** Always texture stage 3. The structure's reflection pass
  (`_rasterizer_environment_reflection_draw`: bumped `DOT_RFLCT_SPEC` or flat `CUBEMAP`, with normalization cubes
  in stages 1 and 2). It is drawn depth-equal, added onto the target scaled by the destination alpha (the
  lightmap reflection mask drawn just before it), with combiners `lerp(T3^8, T3, fresnel tint) * fresnel
  brightness * specular mask`. Model shaders put the reflection cube in stage 3 of their one pass. Water
  (`rasterizer_xbox_water.c`) and glass (`rasterizer_xbox_transparent_geometry.c`) use stage 3 `DOT_RFLCT_SPEC`.
- **Tagging.** The rasterizer calls `halo_rt_reflection(class)` around those draws: structure, model (not first
  person or sky), glass (the level's, not a dynamic mirror) and water. `d3d8_device.c` puts the class in the pixel
  key's spare `texture_modes` bits (`XGPU_TEXTURE_MODES_RT_REFLECTION`) only when the setting is on and stage 3
  is a reflection lookup. With it off, every key is byte-identical to master.
- **Tracing.** `xgpu_spirv.c`'s `rt_reflection_texel` runs one inline ray query per pixel in the draw's own pixel
  shader. The ray starts at the world position, which the vertex shader hands on (the lighting's for models,
  input 0 for the level's world-space vertices: `world_position_input`), and follows Halo's own reflection vector
  (the bump map's `DOT_RFLCT_SPEC` result, so ripples bend it), out to 25 world units. It uses mask 0x0B (the
  player's body included), and alpha-tested candidates go through `rt_candidate_commits`. A hit is shaded from the
  hit table: base map (at the model's map scale), times 2x the detail map for environments (biased multiply),
  times the lightmap page at the hit's lightmap UV (objects: a flat 0.6). It fades to the cube over the last 20%.
  A miss, a hit with no attributes, or a water ray below the horizon keeps the cube texel. A model's ORM roughness
  blends back to the cube from 0.2 to 0.35. `HALO_RT_REFLECTIONS_VIEW=1` paints traced texels green and kept ones
  red.
- **Not done here (deliberately):** half resolution and denoising. Mirror rays along a fixed vector don't
  jitter, so they don't shimmer, and the cost is one ray per reflective pixel. The GI helper owns the shared denoiser
  (`xgpu_vulkan_denoise.c`; its inputs are a superset of DLSS RR's).

Measured (1080p, a10's cryo deck, a static camera, `debug.pass_times`):

| GPU | off | on | of which |
|---|---|---|---|
| Radeon 8060S (omarchy) | 1.32 ms | 2.50 ms | REFLECTIONS pass 0.10 to 1.25 ms; models 0.05 to 0.45 ms |
| RTX 4090 (5900X) | 0.50 ms | 0.77 ms | REFLECTIONS pass 0.03 to 0.26 ms; models 0.04 to 0.09 ms |

Pictures (Classic cube map against traced, same pose): a10's cryo-deck floor now shows the door and hazard
stripes faintly, at the strength Halo's lightmap mask allows. b30's shore water and Blood Gulch's base walls barely
change, because their reflection vectors mostly point at the sky, where the cube is already right. Water's waves
used to send some rays below the horizon into the sea floor (dark speckles), so water rays below the horizon now
keep the cube. A Covenant close-up (Ghost, Elites) wasn't captured: the posed camera didn't find them. Validation
layers on (8060S, a10 traced): 0 messages. Classic is unchanged: with the setting off, no key bit, scene or shader
code differs, and identity and the scorecard pass.

### What's next

1. **Glossy (needs the shared denoiser):** per-class roughness (bumped environment medium, Covenant metal low),
   GGX-jittered rays at half resolution into an RGBA16F image plus hit distance, the denoiser's temporal and
   spatial passes, then the surfaces sample the resolved image at their pixel instead of tracing inline. Same
   tagging and the same stage-3 replacement.
2. **Hit lighting for objects:** the hit table needs each object's ambient and directional light (the game has
   it per object). Today a reflected object gets a flat 0.6.
3. **Cost:** on the 8060S, trace only water, glass and Covenant metal by default, and leave the structure's
   bumped floors on the cube (1.15 of the 1.2 ms is the full-screen structure pass). Skip pixels whose
   destination-alpha mask is 0 (read it as an input attachment) and save their rays.
4. **Map preparation:** the tag is kept beside the draw's inputs, not in `struct draw_inputs`, so the shipped
   prepared records keep their form. A record's draw traces nothing, and the traced pipelines are made at their
   first draw (a hitch once a map with the setting on). Move it into `draw_inputs` the next time the records are
   made again (`tools/prepared_records.py`).
5. Glass on objects (vehicle windows), the first-person weapon's own reflections, and dynamic mirrors.
