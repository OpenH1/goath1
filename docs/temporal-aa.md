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

## Milestone 2: `"fsr"` (Vulkan)

AMD FidelityFX Super Resolution 3.1's upscaler, on Vulkan (Linux, Windows, Android). The setting is offered where the
renderer has it: Settings > Video > Frame Rate & Sync > Upscaler (`xgpu_backend_capabilities.temporal_upscale`).

- **Source.** `port/third_party/fidelityfx-fsr3`: the SDK's (v1.1.4) Vulkan GLSL entry points of the upscaler's passes
  and the GPU headers they include, unchanged, MIT. `tools/vulkan_fsr_shaders.py` compiles them with glslangValidator
  (as `tools/vulkan_post_shaders.py` does the post passes) into `xgpu_vulkan_fsr_shaders.c`, with each pass's bindings
  read back from its SPIR-V; `--check` verifies it. The game has no shader compiler. Options: no half floats, no
  subgroup operations, reversed depth, motion at the render resolution without the jitter, no sharpening (RCAS). An
  SDR and an HDR build of each pass (the same words but the accumulation's).
- **Host side.** `port/linux/vulkan/xgpu_vulkan_fsr.c` is the SDK's `ffx_fsr3upscaler.cpp` (create, dispatch, the
  generated reactive mask) in C, on the core's command buffer between wide barriers. Each window (split screen's four)
  has its own history, made at its sizes. A window not at the target's top left corner is copied there first.
- **Exposure.** Fixed at 1 (the SDK's default exposure, no pre-exposure). The frame time is fixed at 1/60 s: it only
  smooths FSR's own luminance.
- **Frame order.** The window's 3D view is drawn at `display.render_scale`'s fraction of the screen's scale (the front
  end's targets of that scale: `d3d8_device.c`'s `temporal`). Then its motion is resolved, and FSR upscales colour,
  depth, motion, jitter and reset into the window of the back buffer at the display's scale. The tone map
  (`display.hdr`'s, on FSR's output), the Panini pass, the HUD and the menus follow at the display's resolution.
  FXAA, SMAA, MSAA and supersampling are off while it is on. At the native scale the two targets are one.
- **Reactive mask.** The window's colour is copied after the level's transparent surfaces and fog
  (`motion_vectors_opaque`, `render.c`). Particles, effects, contrails, transparent objects, water decals, the dynamic
  lights' glows and lens flares come after that. The SDK's generated mask marks where they changed the colour (tone
  mapped, largest channel, 0.9 past 0.2). The first-person weapon and the objects write their own motion.
- **Preparation.** A map prepared with a technique on also makes the objects' pipelines' motion variants. The
  enumeration's models are drawn as the window's objects are, and FSR's compute pipelines are made then too.
- **Not done.** Sharpening and MetalFX. Android's render scale is its swapchain's (FSR there antialiases at the
  swapchain's size).

## Milestone 3: `"dlss"` (Vulkan, NVIDIA)

NVIDIA DLSS Super Resolution (DLAA at render scale 1) through NVIDIA's NGX, the DLSS SDK's own path on Vulkan for Linux
and Windows. It is offered where the renderer has it: Settings > Video > Frame Rate & Sync > Upscaler gains NVIDIA DLSS
(`xgpu_backend_capabilities.temporal_dlss`, its own row in the FSR row's place). Elsewhere `"dlss"` is off: no motion,
no jitter (`motion_vectors_technique`).

- **The module.** The game holds none of NVIDIA's code. `configure.py --dlss` downloads the SDK's NGX headers, its
  static library and the DLSS runtime (pinned by SHA-256, `tools/vulkan_build.py`), and builds
  `libopenh1_dlss.so` beside the game from `port/linux/vulkan/dlss/openh1_dlss.c`; it alone links NVIDIA's library.
  `libnvidia-ngx-dlss.so.310.9.16` (the DLSS runtime, NVIDIA's redistributable) and `NVIDIA-DLSS-LICENSE.txt` are
  staged beside it. The game loads the module at run time (`xgpu_vulkan_dlss.c`); without it, an NVIDIA GPU or the
  driver's NGX (`libnvidia-ngx.so`, which NVIDIA's Linux driver installs), nothing changes. `HALO_VULKAN_NO_DLSS=1`
  leaves it unloaded. Linux x86-64 for now; Windows's loader is written (`openh1_dlss.dll`) but not built.
- **Device.** Only with the module and an NVIDIA GPU: the instance's and the device's extensions NGX asks for
  (`GetFeature*ExtensionRequirements`) are added, and buffer device addresses. NGX is identified by a project ID
  and the custom engine type.
- **Inputs.** FSR's: the colour, the depth (reversed), the motion (scaled by the render's size into its pixels,
  at the render's resolution, not jittered), the jitter, the reset; read in place at the window's corner. Exposure
  1, no auto exposure; display.hdr's linear light is marked HDR. The output is an RGBA16F image blitted into the
  window of the back buffer, where the tone map, Panini and the HUD follow. No reactive mask (DLSS has none).
- **Quality.** Any render scale: the quality mode is the one whose range of render sizes (NGX's optimal settings)
  holds the render scale's, the nearest optimal; 1.0 is DLAA. The render preset is NGX's default.
- **License.** The NVIDIA RTX SDKs license: the runtime may be shipped inside an application, the SDK must not be
  made subject to an open-source license, NVIDIA's attribution is required (splash, credits) and NVIDIA is to be
  told before a commercial release. The repository holds none of the SDK.

