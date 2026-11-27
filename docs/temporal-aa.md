# Temporal AA groundwork: motion and jitter

`display.temporal_aa` picks a temporal technique: `"off"` (the default; `"metalfx"` on a Mac, iPhone or iPad whose GPU has MetalFX's temporal scaler: Apple silicon, an A14 or later; an Intel Mac's GPU without it, or a device with the spatial scaler alone, stays off), `"fsr"`, `"dlss"` or `"metalfx"`. It works at
any render scale, the native one included. Milestone 1 (this) makes what every technique reads: each window's
per-pixel motion and a sub-pixel jitter. No SDK is drawn yet. With `"off"` nothing here runs: the frames, passes and
uploads are as before.

- **Jitter.** A Halton (2, 3) offset, in the target's pixels. Its sequence is AMD's length
  (`ffxFsr3GetJitterPhaseCount`): 8 times the display's pixels per the render's, squared (8 at the native scale, 17
  at 0.67), for each technique (`halo_motion_jitter_phases`). The front end moves the draws' pixels by it
  through their uniforms' viewport offset (`d3d8_device.c`), not the game's projection. It covers the 3D view only and
  is taken away before the tone map, so the HUD and menus are never jittered.
- **Texture LOD bias.** While the window's 3D view is upscaled, its draws into the screen's targets sample their
  textures' mip levels with the upscaler's bias added to the game's, so the render fetches the display's detail:
  FSR's log2(render / display) - 1 (AMD's: -1 at the native scale, -1.58 at 0.67), DLSS's and MetalFX's
  log2(render / display) (0 at DLAA). Vulkan's samplers have it (`mipLodBias`), Metal's lookups (`texture_lod_bias`).
  Not the HUD, menus, shadow maps or mirrors; off, nothing changes (`d3d8_device.c`'s `draw_lod_bias`).
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
- **Not supported:** VR eyes, mirrors, feeds and MSAA.
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
  subgroup operations, reversed depth, motion at the render resolution without the jitter; the accumulation built
  again with the SDK's sharpening option, and RCAS (`fsr1/ffx_fsr1.h`). An SDR and an HDR build of each pass (the same
  words but the accumulations').
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
- **Sharpening.** `display.temporal_aa_sharpness` (0 to 1, default 0.8; Settings > Video > Frame Rate & Sync >
  Sharpness) is the SDK's sharpness: FSR's own RCAS pass at 2 - 2 × sharpness stops below its strongest
  (`FsrRcasCon`). As in the SDK it is part of the upscale, so it runs before the tone map, on `display.hdr`'s linear
  light too: the accumulation's sharpening build leaves the output to RCAS, which reads this frame's upscaled colour.
  0 dispatches neither (the frame as without it). MetalFX's output goes through the same RCAS, ported to MSL
  (`xgpu_metal_temporal.m`), in place of its copy into the back buffer; 0 is the plain copy. DLSS ignores it.
- **Phones (#1228).** `display.temporal_aa` `"fsr"` on Android or `"metalfx"` on iOS (FSR off by default, MetalFX on where the GPU has
  its temporal scaler; the Upscaler row on both). With it the host draws the screen's pixels (the swapchain or drawable at full size) and gives the game the
  render scale (`HALO_UPSCALE_RENDER_SCALE`): the 3D view is drawn at it and upscaled, with the LOD bias, as on a
  computer. Otherwise the host draws the render scale and the system stretches it, as before. The host chooses at
  the start; a change in the game takes full effect at the next one. An iPhone whose GPU has MetalFX's spatial
  scaler but not its temporal one upscales with the spatial one, unjittered (`temporal_metalfx` 2).

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
  leaves it unloaded. Windows: `openh1_dlss.dll`, built with the game's clang and lld-link from `nvsdk_ngx_s.lib`
  (plain MSVC objects of the static runtime, clang's default), with `nvngx_dlss.dll` and the license beside it.
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
- **Ray Reconstruction.** The module's second feature (its ABI 2; an ABI 1 module still gives Super Resolution): NGX's
  DLSS-D, the ray-traced passes' denoiser and upscaler on RTX (docs/rt-plan/4-path-tracing.md, "Denoising"). The SDK's
  310.9.16 has its runtime, `libnvidia-ngx-dlssd.so.310.9.16` / `nvngx_dlssd.dll`, staged and packed beside Super
  Resolution's. `xgpu_vulkan_dlss_reconstruct` takes the shared denoiser's inputs (diffuse and specular radiance,
  the specular's alpha its hit distance; the albedos; normals with roughness in w; depth; motion; jitter; the camera's
  matrices; reset); a compute pass (`tools/vulkan_dlss_shaders.py`) makes the one noisy colour and the hit distance
  RR reads. `HALO_DLSS_RR_SMOKE=1` (with `"dlss"`) runs it once on a synthetic noisy frame and logs the result and
  the runtime's version (read from NGX's log of its loading).

### The add-on

The game's packages hold none of NVIDIA's files (`tools/dlss_addon.py check-package`, run by `linux_package.py` and
`ci_build.py`'s Windows staging). DLSS is an add-on the player downloads from inside the game:

- **Package.** `configure.py --dlss` then `ninja dlss_addon` writes `build/dlss-addon/`:
  `openh1-dlss-<platform>-<version>.zip` (linux-x86_64: `libopenh1_dlss.so`, `libnvidia-ngx-dlss.so.310.9.16`,
  `libnvidia-ngx-dlssd.so.310.9.16`; windows-x64: `openh1_dlss.dll`, `nvngx_dlss.dll`, `nvngx_dlssd.dll`; and `NVIDIA-DLSS-LICENSE.txt`, `OPENH1-DLSS-LICENSE.txt`), its
  manifest (`.json`: version, platform, size, SHA-256, each file's) and `openh1-dlss-license-<version>.txt`. The
  version is the SDK's and the module's source's (`310.9.16-<8 hex>`). The same inputs make the same bytes.
  `openh1_dlss.c` is MIT licensed (its header; the interface it includes, `xgpu_vulkan_dlss.h`, may be used so too):
  only the add-on links NVIDIA's NGX.
- **Pin.** The game downloads only what it is built with: `port/linux/src/dlss_addon_pin.h`, written by
  `tools/dlss_addon.py pin --tag <release> <manifests>` (each platform's URL,
  `https://github.com/OpenH1/goath1/releases/download/<tag>/<file>`, its size and SHA-256, and the license's). The
  committed file pins nothing: such a build offers no download.
- **In the game** (`port/linux/src/dlss_addon.c`). On an NVIDIA GPU with Vulkan (`dlss_gpu` in the renderer's
  capabilities) where the pinned add-on is not installed, Frame Rate & Sync's Upscaler row has GET DLSS; choosing it
  opens `screens/dlss_addon.xml`: NVIDIA's license (fetched and checked against the pin), ACCEPT, the download's
  progress. The archive is checked (size, SHA-256), unpacked into `<add-ons>/dlss.partial`, its files checked, then
  renamed into place as `<add-ons>/dlss` with `installed.txt` last; any failure (no network, a refused download, a
  full disk, an archive that is not the add-on's, a GPU without DLSS) says why and deletes `dlss.partial`. The
  add-ons' folder is the player's (`dlss_addon_folder.h`: `$XDG_DATA_HOME/openh1/addons`,
  `%LOCALAPPDATA%\OpenH1\addons`), never the game's.
- **Restart.** NGX's Vulkan extensions are the instance's and the device's from their making, and the backend does
  not make them again, so the module (the add-on's first, else a `--dlss` build's beside the executable:
  `xgpu_vulkan_dlss.c`) is loaded at the next start.
- **Remove.** Mods > Add-ons > NVIDIA DLSS ADD-ON (and the screen's REMOVE) deletes it. Where its module is loaded and
  cannot be deleted (Windows) a `remove` file marks it; the next start deletes it before the renderer loads anything.
- **Credit.** While it is installed, NVIDIA's attribution is in the add-on's screen and in Profiles' ABOUT (GOAT H1)
  panel. NVIDIA's logo is not shown: its marks' use needs NVIDIA's approval of a sample.
- **Tests.** `HALO_DLSS_ADDON_URL` (a folder's address, `http://127.0.0.1:<port>/` allowed) serves the pinned files in
  place of the release; `HALO_DLSS_ADDON_FOLDER` is another add-ons' folder.
- **The release workflow** (`.github/workflows/build.yml`, a release run's alone): `dlss-addon-linux` and
  `dlss-addon-windows` build with `--dlss` and `ninja dlss_addon`; `dlss-pin` runs `tools/dlss_addon.py pin --tag
  build-<N>` (the changes job's number, the release's tag) with both manifests; `linux-x86_64` and `windows` wait for
  it and build the game with that pin; the release job checks the packages hold none of NVIDIA's files, then publishes
  each `openh1-dlss-*` file beside them, the very bytes pinned. Other runs build none of it and keep the committed pin.
  DLSS never holds a release back: a platform whose add-on fails is left out of the pin, and with no pin the game keeps
  the committed one and the release publishes no add-on.

## Milestone 4: Metal's motion and `"metalfx"` (macOS, iOS)

- **Motion.** As Vulkan's. The MSL emitter writes the objects' motion as the SPIR-V emitter does
  (`xgpu_msl_options.motion`): the vertex program runs again on the previous frame's registers, 192 to 383, which the
  backend keeps past the 192 and binds as one 6 KB buffer from the frame's upload ring (`setVertexBytes` takes 4 KB at
  most). The pixel shader writes `[[color(1)]]`. The passes into the window's target have the objects' RGBA32F
  texture as their second attachment, cleared by the first of them (`xgpu_metal_set_motion_target`). Every pipeline
  drawn in them has it in its description, the writers' mask all channels and the others' none (clears too).
  `xgpu_metal_temporal.m` holds the resolve and `debug.motion_vectors`' view as MSL. Off, nothing changes: the
  emitter's text is the same for every other shader (the 746 of `tools/msl_corpus`), and no pass has the attachment.
- **MetalFX.** Offered where Metal has `MTLFXTemporalScaler` (`supportsDevice`, macOS 13 and iOS 16 on; the build
  targets macOS 14.4 and iOS 17.4): the Upscaler row's "APPLE METALFX" (`temporal_metalfx`, its own row in the FSR
  row's place). It takes the colour before its tone map, the reversed depth (`depthReversed`), the motion (a
  fraction of the view, previous less current, scaled by the render's size into its pixels: MetalFX's own sense),
  the jitter, the reset and a fixed exposure of 1. Its output is a texture of the display window's size, blitted into
  the back buffer, where the tone map, Panini and the HUD follow. Each window has a scaler of its own; a window not
  at its target's corner is copied there first. The iOS simulator's SDK has no MetalFX: none there.
- **Reactive mask.** FSR's generated one (the colour before and after the transparent draws, tone mapped, its largest
  channel's change, 0.9 past 0.2), drawn into an R8 texture, MetalFX's `reactiveMaskTexture` (macOS 14.4, iOS 17.4).
- **Preparation.** As Vulkan's: a map prepared with a technique on makes the motion variants and the passes'
  pipelines. A scaler is made off the render thread when a window first needs one or its sizes or formats change
  (100 to 300 ms each on an M1 Max, over a second the first time after a system update); until it is ready the view
  is stretched into the window (linear filtering), and its history begins with it. The first upscale's frame went
  from 183 ms of the render thread to under 1 ms (M1 Max, 1280x960 to 2560x1920).
- **Cost.** `HALO_METALFX_TIMES=<n>` logs MetalFX's GPU time, its own command buffer, every n upscales, and each
  window's first upscale's time on the render thread.
