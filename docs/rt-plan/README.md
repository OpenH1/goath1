# Proper ray tracing: the plan

Four parallel studies, one section each. This page gives the order and what each step unlocks. None of it changes how the game plays (render-only); Classic stays as it is.

| Section | What it gives | Needs |
|---|---|---|
| [1. Geometry](1-geometry.md) | The real visible shapes in the ray-traced scene (BSP render mesh, render models, skinned characters, cutouts like grates and foliage), with UVs, normals and a material per triangle. Today's RT shadows and AO hit the real shapes. | Nothing; it's the foundation. |
| [2. Reflections](2-reflections.md) | Traced reflections in place of the fixed cube maps, through Halo's own reflection masks, so the original look still decides what shines. | 1 (hit UVs, lightmap, base texture). |
| [3. Bounced light](3-gi.md) | Colored bounce from dynamic lights (plasma, explosions) on top of the baked lightmaps, and better lighting on characters and vehicles. | Its step 1 needs nothing; the GPU cache needs 1. |
| [4. Path tracing](4-path-tracing.md) | An optional "Ultra" mode for RTX 30 and later and RX 7000 and later: what you see lit by traced light with real shadows, its bounce ended at the original lightmaps (fitted to them, so each level keeps its look), our own ReSTIR DI, DLSS Ray Reconstruction on RTX and our denoiser elsewhere. Metal later. | 1–3, the light model, the referee. |

## Order

1. **Geometry, stage 1** (small): the BSP's opaque render triangles in the scene behind `debug.rt_render_geometry`, with triangle counts logged at Blood Gulch, a10 and b30. In parallel: **bounce lights, stage 1** (small, any GPU and Metal): CPU-placed bounce lights from dynamic lights, off by default.
2. **Geometry, stages 2–4** (medium): render models, skinned refit, cutouts, per-hit data and the material record (the shared first step of sections 2 and 4).
3. **Reflections** (medium): water, glass and Covenant surfaces first, at half resolution with denoising.
4. **Bounced light, the GPU cache** (medium), and per-object light from rays.
5. **Ultra path tracing** (large), last.

Metal ray tracing is out of scope for now (decided 2026-11-26); section 4's Metal notes stay for later.

## Corrections to the sections

Sections 2 and 3 say the model material maps don't exist; they read an outdated checkout. They're on master since #1267: `<bitmap>.normal.png` and `<bitmap>.orm.png` per base map (docs/mods.md, "Material maps"), and the MCC import (#1275) bakes them for weapons and vehicles. Reflections and path tracing should read roughness from the ORM maps where present, with the fixed per-type roughness as the fallback.

The dynamic-shadows plan (docs/dynamic-shadows-plan.md) is the first part of this work: its ray-traced light shadows and light selection carry over unchanged.
