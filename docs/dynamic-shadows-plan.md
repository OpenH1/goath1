# Dynamic shadows and moving lights: plan

Goal: moving lights (the flashlight, plasma, muzzle flashes, rockets, explosions, fixtures) cast shadows, and the
level shadows the objects, done the way our renderer does best: ray queries on Vulkan, a screen-space fallback
elsewhere, Metal's ray tracing later. Inspired by GulchCE's "Modern Dynamic Shadows" (read as a reference only).

## Where we start

- **Lights.** `display.dynamic_lights "enhanced"`: up to 128 lights a window (`dynamic_lights_rank` keeps the 128
  that matter most), lit per pixel through a log-spaced 32x32x16 grid of 128-bit light masks, each light kept to the
  clusters its sphere floods through the portals (no light through walls between clusters). Spotlights and gels (the
  flashlight, headlights) light the models in the grid but the structure in the game's own pass
  (`light_port_lights_structure`). `light_sources.c` adds 32 lights the tags lack; `shield_glow.c` the shields.
- **Ray-traced scene** (`world_rt.c`, `xgpu_vulkan_rt.c`). The level **is already in it**: the loaded BSP's
  collision geometry (5,468 triangles at Blood Gulch, 41,354 at a10), opaque, built once a map. With it, every
  dynamic object's collision meshes, one BLAS each, instanced on its drawn nodes (a10's start: 154 meshes, 931
  instances; the TLAS rebuilt each frame). **Not in it:** static scenery (Blood Gulch's rocks and trees), held and
  first-person weapons, render-only detail (grates, foliage cards). Collision surfaces are fanned unfiltered, so
  invisible player-clip walls would cast shadows.
- **`display.shadows "ray_traced"`**: the classic casters' shadows on the level, 4 cone rays a pixel inside each
  caster's volume, one direction a caster (the lightmap's dominant light), no history.
- **Measured** (omarchy, Radeon 8060S, master's default build, Blood Gulch, one player, 1920x1080, ray-traced
  shadows and enhanced lights on): the whole frame's GPU time 0.53 to 1.39 ms, median 1.16 ms (16 windows of 30
  frames). The 5900X run gave no pass times (no window surface under the rig); world-ao.md's 4090 numbers stand
  (a10: RT AO pass 0.115 ms at 1080p, TLAS build of 931 instances about 0.14 ms). Ray work is cheap here; the
  fixed costs are the TLAS build and any extra pass over the opaque surfaces.
- Also there: a pre-pass of the opaque surfaces (normal, owner, depth) when world AO is on; motion vectors and
  jitter for FSR, DLSS and MetalFX; HDR; normal/ORM maps on models (#1267).

GulchCE does it with OpenGL depth maps: 8 shadowed lights a view (128 to 2048 px), the flashlight plus two nearby
fixtures, one orthographic sun map, and a CPU "visibility field" (16^3 cells from collision rays) to stop light
through doors. Our ray queries replace all of that; our cluster masks already handle the doors.

## Stage 1: ray-traced shadows for the dynamic lights (large)

- **Budget.** Keep the grid for lighting; shadow only the top **K = 4** lights of `dynamic_lights_rank` a window
  (8 on high-end, a setting), nearest and brightest first, the flashlight always one of them. The rest stay
  unshadowed, as today. A pixel traces only for the shadowed lights its cell's mask has, so typical cost is 0 to 2
  rays a pixel, at most K.
- **The pass.** One full-screen pass before the lit surfaces, from the pre-pass's depth, normal and owner: for each
  shadowed light reaching the pixel, one ray to a point on the light's disk; the structure and objects block it; the
  light's owner (the bolt, the rocket, the flashlight's wearer) is skipped by instance owner, as `rt_shadows` skips
  a pixel's own owner. It writes an RGBA8 visibility mask (one channel a light; two for K = 8). The enhanced light
  loop in `xgpu_spirv.c` multiplies those lights by their channel. Ray queries stay in one pass, not in every
  surface pipeline (pipeline count, phones). Needs the pre-pass on whenever this is on (its cost is already paid
  with world AO).
- **Receivers.** The level and the models (classic shadows have no model receivers). The flashlight's and gels'
  structure lighting is the game's pass, so either that pass reads the mask too, or the flashlight moves to the
  enhanced path (preferred: one code path, and its gel is a cone we already have).
- **Soft shadows.** Disk radius by kind: bolts and needles small (about 0.05 wu), explosions and fires larger
  (0.3 to 0.5), the flashlight a point at the weapon (GulchCE moves it to the "primary trigger" marker so the
  shadow isn't lost behind the eye; worth copying the idea). Noise rotated each frame.
- **Denoise.** With FSR, DLSS or MetalFX on, 1 ray a light and the upscaler's accumulation is the denoiser (mask
  applied before it, so the noise is jittered with the frame). Without them, 2 to 4 rays and the spatial dither
  `rt_shadows` uses. A temporal filter of our own (motion vectors exist) only if that shimmers: bolts move fast,
  so history must reset per light id. No DLSS Ray Reconstruction (needs a full G-buffer).
- **Risks.** Collision meshes are coarse: bipeds' shadows blocky, no alpha detail. Lights inside or touching their
  owner. The mask's resolution (try half-resolution, upsampled by depth). Split screen and VR eyes: one mask per
  view.
- **Before it** (small, part of stage 3): static scenery into the AS, invisible collision surfaces left out.

## Stage 2: contact shadows, the fallback (medium)

For GPUs without ray queries, the Deck if RT costs too much there, phones and Metal today: a screen-space march from
the pre-pass's depth toward each shadowed light, 8 to 16 steps over about 0.5 wu, at half resolution, the same mask
and channels as stage 1 so the surfaces don't know which made it. Cost: a few hundredths to a few tenths of a ms at
1080p for 4 lights (measure on the Deck first). Quality: good contact (a foot, a bolt past a wall), nothing from
off-screen occluders, guessed thickness, fails at screen edges. Not a replacement for long shadows, but no
light-leak cost.

## Stage 3: the level in the AS for objects (medium)

The BSP is already traced; what's missing is using it for the objects. Today a model is lit by the lightmap sampled
at one point (its lighting point): a Warthog half under a bridge is all lit or all dark. Plan: on model pixels, one
ray along each of the model's distant lights (the lightmap's dominant direction, `rasterizer_set_model_lighting`)
through the level and other objects; multiply that light's share, leave the ambient (world AO has it). This also
gives objects shadows on objects. Static BLAS work: add static scenery (once a map; its meshes are in the SDF pages
already) and filter invisible surfaces. Memory is small: collision BSPs are 5k to 41k triangles, a few MB with
compaction. Render-geometry BSP (more triangles, alpha grates) later and only if collision proves too coarse:
measure its triangle count and memory first. Risk: the direction is a lightmap estimate, not a sun; outdoors it
reads well, indoors (many lights) a single direction may look wrong, so fade it by how directional the sample is.

## Stage 4: GulchCE's extra emitters (small, can run beside the others)

| Theirs | Ours today | Worth it? |
| --- | --- | --- |
| Needle, Wraith mortar, rocket flight light | `light_sources.c` rows | Have |
| Covenant vehicle shots, fuel rod, explosions | `light_sources.c` effect rows | Have |
| Jackal shield, field generator, unit shields | `shield_glow.c` | Have |
| Ghost running/cockpit, dropship thrusters | `light_sources.c` | Have (Ghost MP and campaign) |
| AR ammo counter display | none | Yes: small, iconic |
| Held Needler crystals | none | Yes |
| Overshield, camo powerups; teleporters | none | Yes (MP maps) |
| Hunter fuel-rod arm glow; plasma cannon turret | none | Yes |
| Flamethrower (one light a firer) | none | Yes, for CE maps |
| Human weapon muzzle pulses | tags' own flashes | Check which tags lack one first |
| Authored lights scaled up (sword, headlights, Banshee wing, plasma pistol overcharge) | tags as authored | No by default: changes the look |
| Emissive lightmap patches as fixture sources | scenario fixtures in `world_scene` | Later, as stage 1/3 shadow sources |

How: new object rows in `light_sources.c` (marker plus a function for strength), a held-weapon row kind placed
through `dynamic_lights_first_person`, and a self-illumination strength source for the ammo counter. **Attribution:**
their code is AGPL-3.0-or-later OR GPL-3.0-or-later; we may take it under AGPL, but any reused code or data table
(tag lists, colors, radii) must keep their SPDX header and the credit and provenance notice from their README, note
our changes, and be credited in our credits. Simplest: write ours from the tags, and add the notice only where a
value is taken from them.

## Stage 5: Metal ray tracing (large, later)

Build the same records (`WORLD_AO_MESH`, `WORLD_AO_INSTANCES`) into `MTLAccelerationStructure`s in
`xgpu_metal.m` (primitive structures once, the instance structure each frame, refit later), and emit
`intersector<triangle_data, instancing>` in `xgpu_msl.c` for the AO, shadow and stage 1 passes. Gate on
`supportsRaytracing` and default on for hardware RT only (M3, A17 Pro and later); M1/M2 can run it but slowly,
off by default. Nothing exists in the Metal code today. Until then, stage 2 serves Metal.

## Order and first step

1. Stage 3's AS fix-ups (small): static scenery into the scene, invisible surfaces out. Improves today's ray-traced
   shadows and AO at once.
2. Stage 1 for plasma bolts, needles, explosions and rockets (K = 4), then the flashlight on the enhanced path.
3. Stage 3's sun on objects.
4. Stage 2, the contact-shadow fallback (Deck, phones, Metal).
5. Stage 4 any time beside them (one helper, small rows).
6. Stage 5 when there's an M3 to test on.

First concrete step: in `world_scene.c`'s `world_scene_fan`, skip collision surfaces with
`_collision_surface_invisible_bit` (bit 1 of `flags`; breakable glass, bit 3, needs its broken state too) and add the static
scenery's collision meshes to the frame's instances (once a map); then, for stage 1, have `dynamic_lights_rank`
hand the top 4 lights' indices and disk radii to `rt_shadows.c` with the frame's scene.
