# 1. Geometry: the render meshes in the ray-traced scene

Today the scene (`world_rt.c`, `world_rt_game.c`, `xgpu_vulkan_rt.c`; docs/world-ao.md "Hardware ray queries") holds
collision geometry only: the BSP's collision surfaces fanned by `world_scene_fan` (5,468 triangles at Blood Gulch,
27,914 at a30, 41,354 at a10), each object node's collision BSP as a rigid BLAS, a box for items. It is sent as
non-indexed float triangles (`WORLD_AO_MESH`, 9 floats a triangle) with no UVs, normals or materials, and the
backend forces every instance opaque or not-opaque. Static scenery, held and first-person weapons, grates and foliage
cards are missing; invisible clip walls are in. That is enough for 2 wu AO rays and object shadows, not for
reflections, GI or path tracing, which need what the eye sees and what it is made of.

## The level: one static BLAS per BSP

- **Source.** `structure_bsp.lightmaps[]` → `materials[]` (`structure_material`, structure_bsp_definitions.h): each
  has its shader, `first_surface_index`/`surface_count` into the BSP's 16-bit index triangles, and its environment
  vertices (position, normal, binormal, tangent, UV) followed by its lightmap vertices (normal, lightmap UV). The
  lightmap index gives the lightmap page. Custom Edition maps keep these uncompressed (`custom_edition_geometry.c`).
- **Build.** One BLAS per BSP, two geometries: opaque materials (`VK_GEOMETRY_OPAQUE_BIT`) and alpha-tested ones (no
  flag, any-hit). Indexed, position-only stream for the build (12 B a vertex); attributes kept beside it. Built
  `PREFER_FAST_TRACE` + `ALLOW_COMPACTION`, compacted after. A BSP switch builds the new one; the old is kept until
  the switch settles (b30, c10 and others switch BSPs mid-mission).
- **Never lengthen a load.** Build after the first frame, a material group at a time if needed; until it is ready
  the collision BLAS stays in the scene (it is what's there today), so nothing waits.
- Sky, water (`shader_transparent_water`) and the level's fog planes are left out; glass is tagged non-opaque.

## Objects

- **Rigid parts first.** Most CE model vertices are weighted wholly to one node. Split each render model's parts by
  node: a vertex's major node, its triangles a BLAS for that node, instanced on the drawn node transform, exactly as
  the collision meshes are today (no refit, no skinning, the TLAS alone moves). Vehicles, scenery, weapons,
  machines and most of a biped are then done.
- **One LOD for rays,** chosen per definition and not per view (the second-highest, or a triangle cap), so the scene
  is the same for every window and eye and a BLAS is built once per definition a map.
- **Skinned vertices** (2-node blends at a biped's joints): first leave them on their major node (small seams the
  rays rarely notice). Then a compute pass that skins them with the same math the vertex programs use
  (`nv2a_vsh.c`'s skinned position and normal) into a per-object buffer, and **refit** that object's skinned BLAS
  each frame; rebuild only when the LOD or permutation changes. Feeding the BLAS from the draw's own vertex output
  (transform feedback) is not worth it: draws are per window and eye, views cull them, and Metal has no equivalent.
- **Static scenery** (Blood Gulch's rocks and trees) as instances built once a map; **held weapons** on their hand
  marker; the **first-person weapon** as its own instance with its own cull-mask bit, off for AO and shadows from the
  world, on for its own self-shadow and reflections (it is drawn with a different projection: its world transform is
  the drawn camera's inverse times its view transform).

## Alpha-tested and transparent surfaces

- Shader classes: `shader_environment` with alpha-test, `shader_model` with its alpha-tested flag, and
  `shader_transparent_chicago`/`_generic` used as cutouts go in the non-opaque geometry. Glass and other blended
  transparents are non-opaque too, skipped by shadow and AO rays at first (tinting later).
- With ray queries there is no any-hit shader: the candidate loop (`rayQueryProceedEXT`) fetches the triangle's UVs
  by barycentrics, samples the base map's alpha (one bilinear, lowest useful mip) and commits only above the
  shader's cutoff. The backend must stop forcing `FORCE_OPAQUE`/`FORCE_NO_OPAQUE` per instance and use the
  geometry flags; the owner skip moves into the same candidate loop (it already compares the owner).
- **Opacity micromaps** (`VK_EXT_opacity_micromap`) only later and optional: NVIDIA Ada and later have them, AMD and
  Metal don't. The UV fetch is the path everywhere.

## Per-hit data

One table the shaders reach by buffer device address, keyed by the hit's instance custom index → geometry →
primitive:

- **Per vertex** (a stream beside the positions, about 24 B): UV (half2), octahedral normal and tangent (with its
  sign), lightmap UV for the level (half2).
- **Per triangle:** its material index (16 bit), from the BSP material or model part.
- **Per material** (a small record): shader kind and flags, alpha cutoff, base map, detail/multipurpose map, the mod
  material maps (normal and ORM, `feat/object-material-maps`' docs/mods.md "Material maps"), the lightmap page, and
  self-illumination color. Textures are named by a slot in one bindless array (`VK_EXT_descriptor_indexing`).
- Collision material ids stay as today for gameplay-like uses (footsteps don't need them).

## Measured in stage 1 (2026-11-26)

`debug.rt_render_geometry` on omarchy (Radeon 8060S, RADV), the logs' `as drawn` and `meshes built` lines. The
level's render triangles are almost exactly its collision count: Halo's BSP render and collision meshes come from
the same source, so the "1.5 to 3 times" guess below was wrong. The BLAS is 158.7 B a triangle uncompacted (non-indexed,
`PREFER_FAST_TRACE`, no compaction yet), the same on every map, about 0.78 times the scratch.

| Map (BSP) | Collision tris | Render tris, opaque | Alpha-tested (stage 3) | Transparent | BLAS | GPU build | CPU read |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Blood Gulch | 5,468 | 5,371 | 0 | 132 | 0.81 MB | 0.41 ms | 0.07 ms |
| a10 (BSP 0) | 41,238 | 40,430 | 504 | 12 | 6.12 MB | 0.71 ms (1.52 ms with 16 object meshes) | 0.45 ms |
| a10 (BSP 7) | 7,922 | 7,788 | 24 | 378 | 1.18 MB | 0.56 ms | 0.07 ms |
| b30 (BSP 0) | 32,960 | 32,806 | 48 | 2,251 | 4.96 MB | 0.85 ms (with 16 object meshes) | 0.37 ms |

The GPU time is the whole batch built that frame (timestamps around `vkCmdBuildAccelerationStructuresKHR`); the CPU
read is the game thread's walk of the materials. The render mesh comes the frame after the collision one, so no
load waits for it. With the switch, the level costs twice its BLAS (the collision mesh is kept, unused, until the
map's end), and a BSP switch back (a10's start goes 0, 7, 0) makes both again: a10 reached 27.9 MB of meshes in
60 s. Keeping each BSP's meshes by index, or letting a mesh go alone, belongs in stage 2's new stream record.

Clip walls: the render geometry has none (nothing draws them), so they are out by construction.

## Measured in stages 2 and 3 (2026-11-26)

On branch `rt/geometry-2`, still behind `debug.rt_render_geometry`.

- **Memory and BSP switches.** Each BSP's mesh is kept by index (switching back builds nothing), the collision BLAS
  is let go of once the render mesh is in, and a mesh of 1,024 or more triangles has its own buffer and is compacted
  (to 0.68 to 0.70 of its size on RADV, about 0.45 on the 4090). a10's 0, 7, 0 start peaks at 6.94 MB and ends at
  5.9 MB of meshes (it was 27.9 MB).
- **Per-hit data.** Indexed meshes (`WORLD_AO_MESH` kind `_INDEXED`: positions, corners, 24 B a vertex, a material
  per triangle, the map's 64 B material records with the #1267 normal and ORM maps) go into one hit table. Textures
  sit in a 512-slot bindless array, set 4 (`xgpu_vulkan_rt.c`). An instance names its mesh by its SBT offset. a10
  BSP 0: 40,934 triangles, 67,305 vertices, 178 materials, 2.9 MB sent, read in 2.75 ms.
- **Cutouts.** Alpha-tested surfaces are each mesh's geometry 1, which is non-opaque. All three candidate loops take
  a geometry-1 candidate only where its alpha map, sampled at level 1, passes the cutoff
  (`xgpu_spirv.c`'s `rt_candidate_commits`). The environment's map is its bump map, a model's is its base map.
  a10: 504 alpha-tested triangles, a30: 582. On a10 the validation layer reports nothing and every module passes
  spirv-val.
- **Objects.** Each model's render mesh, at the second most detailed LOD, is split by node and instanced as the
  collision meshes were. b30: about 1,700 instances a frame, 195 meshes, 85k triangles.
- **GPU time**, Blood Gulch, 5 bots: 2.34 ms a frame on and off (AO 0.97 ms) on the 8060S; 0.76 ms on the 4090,
  with no hang in 30 s.
- **Not done yet:** TLAS refit.

## Measured in stage 5 (2026-11-26)

- **Skinned refit.** A model whose vertices blend two nodes becomes one skinned mesh (rest pose, nodes and weights).
  Each object of it has its own BLAS (`ALLOW_UPDATE`), which a compute pass skins into (`xgpu_spirv_rt_skin`, the
  game's two-node linear blend) and which is then refit. It is never rebuilt, and an unchanged pose is skipped.
  GPU time for skinning plus refits: Blood Gulch with 5 bots, 7 objects, 0.045 ms on the 8060S and 0.085 ms on the
  4090; b30 with Marines, about 185 objects (about 62 refit a frame), 0.07 ms.
- **Masks.** Instance kinds: structure 1, object 2, first-person weapon and hands 4, the local player's own body in
  first person 8. The AO rays use 0x03 and the object-shadow rays 0x0B. The light-shadow rays (`light_shadows_rays`,
  still 0xFF) should use 0x0B.
- **First-person weapon and hands** go in as drawn (world-space nodes from `first_person_weapons.c`), one frame
  late.
- **The player's own body** is the game's third-person biped, posed by its own node matrices whatever
  `display.first_person_legs` says. In first person those nodes kept moving (b30: 519 to 600 of every 600 frames).

## The default (2026-11-27)

`debug.rt_render_geometry` is now `"auto"` (the default), `"on"` or `"off"` (an older `true`/`false` reads as on/off).
Wherever ray queries are available, the rays hit the render geometry. Collision meshes are only the fallback: until a
render mesh is made (the frame after its collision one), and past the GPU class's budget, which the backend sets from
the device type and device-local memory (discrete: 512 MB at 12 GB or more, 256 MB at 6 GB, else 128 MB; integrated:
256 MB at 8 GB, 128 MB at 2 GB, else 64 MB, the Steam Deck class). The game estimates 200 B a triangle for the
kept meshes (level, definitions), then for the skinned objects posed in a frame. Past the budget, the rest becomes
collision (a skinned definition's collision meshes are made then), logged once a map. `"on"` ignores the budget.
`HALO_RT_GEOMETRY_BUDGET_MB` overrides it for checking (b30 at 8 MB: one skinned object as drawn, the rest
collision, validation clean). Transparent generic and chicago shaders drawn as cutouts (alpha-tested,
alpha-blended, not decals) are alpha-tested geometry on their first map. The backend logs its memory every 1800
frames and once more as a map's end lets it go.

Checked with the validation layer (0 messages, no device loss): Blood Gulch, a10 (BSPs 0, 7, 0), b30, MCC
mp_plains, a co-op client (ui, a10, ui), and b30 past an 8 MB budget.

Measured: median of 3 runs, 1920x1080, AO and shadows `ray_traced`, enhanced lights, `debug.pass_times`. Frame = the
GPU's whole frame, AO = the AO pass (where the shadow rays are traced too). Memory = peak bottom levels / top
levels / hit table used (the table's buffer is 4 MB either way). The render path adds the skinning's fixed buffers
(about 32 MB, 43 MB at b30), which are not in the budget.

| GPU, map | Frame on / off (ms) | AO on / off (ms) | Memory on (MB) | Memory off (MB) |
| --- | --- | --- | --- | --- |
| 8060S, Blood Gulch 5 bots | 2.21 / 1.65 | 1.11 / 0.62 | 2.75 / 0.17 / 1.00 | 1.02 / 0.17 / 0.31 |
| 8060S, a10 | 3.82 / 3.37 | 1.92 / 1.79 | 14.5 / 1.33 / 3.04 | 6.94 / 2.67 / 0.31 |
| 8060S, b30 Marines | 6.40 / 5.73 | 1.53 / 1.46 | 21.0 / 1.33 / 2.79 | 5.34 / 2.67 / 0.31 |
| 8060S, MCC mp_plains 5 bots | 2.48 / 3.00 | 1.04 / 1.22 | 13.5 / 0.67 / 3.86 | 8.13 / 0.67 / 0.31 |
| 4090, Blood Gulch 5 bots | 0.74 / 0.76 | 0.31 / 0.32 | 1.13 / 0.10 / 1.00 | 0.48 / 0.20 / 0.31 |
| 4090, a10 | 1.01 / 1.03 | 0.40 / 0.39 | 5.78 / 0.79 / 3.42 | 2.80 / 1.88 / 0.31 |
| 4090, b30 Marines | 1.92 / 1.85 | 0.43 / 0.41 | 8.85 / 0.94 / 2.79 | 2.05 / 1.57 / 0.31 |
| 3090 (Windows), Blood Gulch 5 bots | 2.41 / 2.40 | 0.95 / 1.05 | 2.1 / 0.12 / 1.00 | 0.48 / 0.24 / 0.31 |

On NVIDIA it costs nothing measurable. On the 8060S it costs 0.4 to 0.7 ms a frame, most of it in the AO pass at
Blood Gulch. A likely cause, to check next: every `shader_model` without "not alpha-tested" is alpha-tested geometry
(the game's own rule), so most objects go through the candidate loop's texture fetch even where their base map has
no alpha. The Steam Deck was not measured.

## Memory and build time (estimates before stage 1)

Render BSPs are roughly 1.5 to 3 times their collision triangle count (cutouts, detail, decorations). Compacted BLAS
size is about 60 to 150 B a triangle depending on the vendor, plus about 24 B a vertex of attributes.

| Map | Level tris (est.) | Object tris built (est.) | Scene memory (est.) |
| --- | --- | --- | --- |
| Blood Gulch | 10k to 20k | 50k to 100k (vehicles, scenery, weapons) | 10 to 25 MB |
| a10 | 80k to 120k | 150k to 300k | 30 to 70 MB |
| b30 | 60k to 120k | 150k to 300k | 30 to 70 MB |

Static BLAS builds are a few ms of GPU time each, once, spread over frames. Per frame: the TLAS rebuild stays (931
instances at a10 is about 0.14 ms on a 4090; per-node render instances may double the count, so refit it where only
transforms changed). Skinned refits: 60 bipeds at about 1k skinned triangles each is about 60k triangles a frame,
roughly a third of a rebuild's cost; rebuilding them each frame is the fallback if refit quality drops. The current
16 MB backend buffers and the 1M-triangle per-mesh cap (`XGPU_STREAM_WORLD_AO_MESH_TRIANGLES`) hold all of this.

## What changes for today's RT shadows and AO

Shadows and AO keep their passes, rays and rules (the structure opaque and stopping a ray, the owner skipped); they
just hit the real shapes. Gains: biped and vehicle shadows match their models, grates and trees cast cut-out
shadows, invisible clip walls stop casting, static scenery shades and shadows. Costs: more triangles to trace and
the alpha fetch in the candidate loop (only on non-opaque hits). The SDF bake and `"world"` mode keep collision.
Shadow rays from a model's own surface need a small normal offset (render meshes sit exactly on the surface).

## Stages

1. **Small.** Fan the BSP's render triangles (opaque materials only) in `world_rt_game.c` as a new
   `world_rt_source_structure` variant, behind a debug switch, sent through today's `WORLD_AO_MESH`; log counts.
   Gives the real numbers for the table above and fixes clip walls at once.
2. **Medium.** Indexed meshes with attributes and materials: a new stream record (positions, indices, attribute
   stream, material table), the bindless texture array, per-geometry flags in `xgpu_vulkan_rt.c`.
3. **Medium.** Alpha-tested geometry and the candidate-loop UV fetch.
4. **Medium.** Render models per node (rigid split), static scenery, held weapons; collision meshes retired.
5. **Large.** Skinned refit by compute, the first-person weapon, TLAS refit.
6. **Large, later.** Metal: the same records into `MTLAccelerationStructure`s (dynamic-shadows-plan stage 5).

## Risks

- Texture residency: the bindless array needs every hit-able material's base map resident; the texture cache may
  drop maps the view no longer draws. Pin alpha-tested materials' maps at least.
- Render meshes are not closed: rays leak at cracks and through one-sided cards (the backend's facing rules and
  "structure stops a ray" need checking against two-sided materials).
- Self-intersection on the drawn surface itself (shadows, AO from the pixel) now that the traced and drawn
  triangles match.
- Mesh and instance counts: rigid per-node splits push toward the 4096-mesh cap on heavy campaign maps.
- BSP switches and Custom Edition maps with odd vertex layouts.
- Mobile and the Deck: memory and refit cost measured first (#432's rule).

## First step

In `port/linux/game/world_rt_game.c`, add `world_rt_source_structure_render(bsp_index, triangles, capacity)` that
walks `structure_bsp->lightmaps` → `structure_material` → its surfaces and environment vertices (layout as
`custom_edition_geometry.c` reads them), skipping alpha-tested and transparent shaders; in `world_rt.c`'s
`world_rt_structure`, use it when `debug.rt_render_geometry` is on and log its triangle count next to the
collision count at Blood Gulch, a10 and b30.
