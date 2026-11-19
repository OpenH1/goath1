# AMD FidelityFX Super Resolution 3.1 (upscaler)

From the AMD FidelityFX SDK, MIT licensed (see `LICENSE.txt`).

Upstream: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v1.1.4, commit
c6efa6bf7f2027b3ec94f28578bb5965eabb9e55. Copied unchanged:

- `gpu/`: `sdk/include/FidelityFX/gpu/`'s core headers, `fsr3upscaler/` and `spd/ffx_spd.h`, the files the passes
  below include;
- `vk/`: `sdk/src/backends/vk/shaders/fsr3upscaler/`'s GLSL entry points of the passes the port dispatches
  (prepare inputs, luma pyramid, shading change pyramid, shading change, prepare reactivity, luma instability,
  accumulate, and the generated reactive mask).

`display.temporal_aa = "fsr"` draws them. `tools/vulkan_fsr_shaders.py` compiles them to SPIR-V with glslangValidator
into `port/linux/vulkan/xgpu_vulkan_fsr_shaders.c`, so the game builds without a shader compiler.
`port/linux/vulkan/xgpu_vulkan_fsr.c` is the SDK's host side (`ffx_fsr3upscaler.cpp`) written again in C.
