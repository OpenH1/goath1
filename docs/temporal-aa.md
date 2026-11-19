# Temporal AA groundwork: motion and jitter

`display.temporal_aa` picks a temporal technique: `"off"` (the default), `"fsr"`, `"dlss"` or `"metalfx"`. It works at
any render scale, the native one included. Milestone 1 (this) makes what every technique reads: each window's
per-pixel motion and a sub-pixel jitter. No SDK is drawn yet. With `"off"` nothing here runs: the frames, passes and
uploads are as before.

- **Jitter.** A Halton (2, 3) offset, 8 phases, in the target's pixels. The front end moves the draws' pixels by it
  through their uniforms' viewport offset (`d3d8_device.c`), not the game's projection. It covers the 3D view only and
  is taken away before the tone map, so the HUD and menus are never jittered.
- **Motion.** The value is the previous position less the current one, as a fraction of the view (x right, y down),
  with the jitter left out. It is written to RGBA16F, the target's size; the backend's `motion.motion` is the
  technique's input.
  - Objects (rigid, skinned, the first-person weapon and arms): each draw that writes depth and blends nothing writes
    its motion as the second color attachment of the objects' pass. The vertex program runs a second time on the
    previous frame's registers (`xgpu_spirv.c`'s `motion`). Those are constants 192 to 383: the game's previous
    projection and each object's previous nodes (`motion_vectors.c`), else this frame's.
  - The level, the sky and everything else: a resolve pass reprojects the depth by the camera's change. It keeps an
    object's motion only where the object's depth is still the pixel's.
- **History.** The previous frame's data is render-only memory in `port/linux/game/motion_vectors.c`, never game state:
  - per window: the camera;
  - per object, by datum handle and kind: the nodes as drawn.

  A window has no history at a cut (more than 4 world units or about 60 degrees in a frame), on a new map, after a
  resize, or when it was not drawn the frame before. An object has none when it appears, respawns, is teleported (its
  root moves more than 4 world units) or changes its node count. With no history the motion is the camera's alone. A
  window with no history has zero motion and its reset flag set (`motion.reset`). Split screen keeps each window apart.
- **Not supported:** VR eyes, mirrors, feeds and MSAA. Metal is not done: the Metal backend ignores the records and
  draws no motion.
- **Hook for masks:** transparent draws (particles, decals, glass, water) write no motion. The objects' pass and the
  resolve are where a reactive or transparency mask goes later.

`debug.motion_vectors` draws the motion over the view: grey when still, red to the right, green downward, full color
at 8 pixels a frame.
