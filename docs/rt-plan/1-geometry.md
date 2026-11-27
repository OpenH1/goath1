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

## Memory and build time (estimates, to measure in stage 1)

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
