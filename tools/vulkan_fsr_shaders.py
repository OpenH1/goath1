#!/usr/bin/env python3
"""Build display.temporal_aa "fsr"'s compute passes for Vulkan.

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_fsr_shaders.py
The generated C (port/linux/vulkan/xgpu_vulkan_fsr_shaders.c) embeds the
SPIR-V of AMD FidelityFX Super Resolution 3.1's upscaler passes, compiled
from port/third_party/fidelityfx-fsr3 (the SDK's Vulkan GLSL entry points
and the GPU headers they include, unchanged), and each pass's bindings, read
back from its SPIR-V (the names the SDK's own binding tables match): the game
needs no shader compiler. --check verifies that the checked-in file still
matches its sources.

The options are the port's, the same for every pass: no half floats (any
Vulkan 1.1 device), no subgroup operations in the downsampler
(FFX_SPD_NO_WAVE_OPERATIONS: no quad operations needed), the depth reversed
(XGPU_VULKAN_REVERSED_DEPTH), the motion at the render resolution and
without the jitter. The accumulation is built a second time with the SDK's
sharpening option (FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING: it leaves the
output to the RCAS pass, display.temporal_aa_sharpness's). Each pass is built
twice, for an SDR and an HDR (linear, display.hdr's) colour input; where both
are the same words, once. glslang 16.6.0 and SPIRV-Tools 2026.4 (Homebrew's)
produce the checked-in words.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FSR = ROOT / "port/third_party/fidelityfx-fsr3"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_fsr_shaders.c"

# the passes, in xgpu_vulkan_fsr_shaders.h's order: each one's name, the SDK's
# entry point it is built from and the options it adds to OPTIONS
PASSES = (("prepare_inputs", "prepare_inputs", {}), ("luma_pyramid", "luma_pyramid", {}),
          ("shading_change_pyramid", "shading_change_pyramid", {}), ("shading_change", "shading_change", {}),
          ("prepare_reactivity", "prepare_reactivity", {}), ("luma_instability", "luma_instability", {}),
          ("accumulate", "accumulate", {}), ("autogen_reactive", "autogen_reactive", {}),
          ("accumulate_sharpen", "accumulate", {"FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING": 1}),
          ("rcas", "rcas", {}))
OPTIONS = {
    "FFX_GPU": 1,
    "FFX_GLSL": 1,
    "FFX_HALF": 0,
    "FFX_SPD_NO_WAVE_OPERATIONS": 1,
    "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS": 1,
    "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS": 0,
    "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH": 1,
    "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING": 0,
}
# the shaders' variables (the SDK's binding tables' names) and the
# resources they are (xgpu_vulkan_fsr_shaders.h's XGPU_VULKAN_FSR_*): a
# texture read (r_), an image written (rw_), a block, a sampler
RESOURCES = {
    "r_input_color_jittered": "INPUT_COLOR",
    "r_input_opaque_only": "INPUT_OPAQUE_ONLY",
    "r_input_motion_vectors": "INPUT_MOTION_VECTORS",
    "r_input_depth": "INPUT_DEPTH",
    "r_input_exposure": "INPUT_EXPOSURE",
    "r_frame_info": "FRAME_INFO",
    "rw_frame_info": "FRAME_INFO",
    "r_reactive_mask": "REACTIVE_MASK",
    "r_transparency_and_composition_mask": "TRANSPARENCY_AND_COMPOSITION_MASK",
    "r_reconstructed_previous_nearest_depth": "RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH",
    "rw_reconstructed_previous_nearest_depth": "RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH",
    "r_dilated_motion_vectors": "DILATED_MOTION_VECTORS",
    "rw_dilated_motion_vectors": "DILATED_MOTION_VECTORS",
    "r_dilated_depth": "DILATED_DEPTH",
    "rw_dilated_depth": "DILATED_DEPTH",
    "r_internal_upscaled_color": "INTERNAL_UPSCALED",
    "rw_internal_upscaled_color": "INTERNAL_UPSCALED",
    "r_accumulation": "ACCUMULATION",
    "rw_accumulation": "ACCUMULATION",
    "r_luma_history": "LUMA_HISTORY",
    "rw_luma_history": "LUMA_HISTORY",
    "r_lanczos_lut": "LANCZOS_LUT",
    "r_spd_mips": "SPD_MIPS",
    "rw_spd_mip0": "SPD_MIP0",
    "rw_spd_mip1": "SPD_MIP1",
    "rw_spd_mip2": "SPD_MIP2",
    "rw_spd_mip3": "SPD_MIP3",
    "rw_spd_mip4": "SPD_MIP4",
    "rw_spd_mip5": "SPD_MIP5",
    "rw_spd_global_atomic": "SPD_ATOMIC",
    "r_dilated_reactive_masks": "DILATED_REACTIVE_MASKS",
    "rw_dilated_reactive_masks": "DILATED_REACTIVE_MASKS",
    "r_new_locks": "NEW_LOCKS",
    "rw_new_locks": "NEW_LOCKS",
    "r_farthest_depth": "FARTHEST_DEPTH",
    "rw_farthest_depth": "FARTHEST_DEPTH",
    "r_farthest_depth_mip1": "FARTHEST_DEPTH_MIP1",
    "rw_farthest_depth_mip1": "FARTHEST_DEPTH_MIP1",
    "r_shading_change": "SHADING_CHANGE",
    "rw_shading_change": "SHADING_CHANGE",
    "r_current_luma": "CURRENT_LUMA",
    "rw_current_luma": "CURRENT_LUMA",
    "r_previous_luma": "PREVIOUS_LUMA",
    "r_luma_instability": "LUMA_INSTABILITY",
    "rw_luma_instability": "LUMA_INSTABILITY",
    "rw_upscaled_output": "UPSCALED_OUTPUT",
    "rw_output_autoreactive": "AUTOREACTIVE",
    "rw_output_autocomposition": "AUTOCOMPOSITION",
    "r_rcas_input": "RCAS_INPUT",
    "cbFSR3Upscaler": "CONSTANTS",
    "cbSPD": "SPD_CONSTANTS",
    "cbGenerateReactive": "REACTIVE_CONSTANTS",
    "cbRCAS": "RCAS_CONSTANTS",
    "s_PointClamp": "POINT_SAMPLER",
    "s_LinearClamp": "LINEAR_SAMPLER",
}

OP_NAME, OP_DECORATE, OP_TYPE_IMAGE, OP_TYPE_SAMPLER, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_VARIABLE = \
    5, 71, 25, 26, 30, 32, 59
DECORATION_BINDING, DECORATION_SET = 33, 34


def string(words: tuple[int, ...]) -> str:
    data = b"".join(struct.pack("<I", word) for word in words)
    return data[:data.index(0)].decode()


def bindings(spirv: bytes) -> list[tuple[int, str, str]]:
    """Each resource variable's binding, kind and resource, from the
    module's names, decorations and types, by binding."""
    words = struct.unpack(f"<{len(spirv) // 4}I", spirv)
    names, binding, sets, types, pointers, variables = {}, {}, {}, {}, {}, {}
    index = 5
    while index < len(words):
        opcode, count = words[index] & 0xFFFF, words[index] >> 16
        operands = words[index + 1:index + count]
        if opcode == OP_NAME:
            names[operands[0]] = string(operands[1:])
        elif opcode == OP_DECORATE and operands[1] == DECORATION_BINDING:
            binding[operands[0]] = operands[2]
        elif opcode == OP_DECORATE and operands[1] == DECORATION_SET:
            sets[operands[0]] = operands[2]
        elif opcode == OP_TYPE_IMAGE:
            types[operands[0]] = "TEXTURE" if operands[6] == 1 else "IMAGE"
        elif opcode == OP_TYPE_SAMPLER:
            types[operands[0]] = "SAMPLER"
        elif opcode == OP_TYPE_STRUCT:
            types[operands[0]] = "BUFFER"
        elif opcode == OP_TYPE_POINTER:
            pointers[operands[0]] = operands[2]
        elif opcode == OP_VARIABLE:
            # (its type: through its pointer)
            variables[operands[1]] = pointers[operands[0]]
        index += count
    result = []
    for variable, pointee in variables.items():
        if variable not in binding:
            continue
        name = names.get(variable, "")
        if sets.get(variable, 0) != 0 or name not in RESOURCES:
            raise ValueError(f"a resource the port does not know: {name!r}")
        result.append((binding[variable], types[pointee], RESOURCES[name]))
    return sorted(result)


def words_array(name: str, data: bytes) -> str:
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"static const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def compile_pass(args, name: str, source: str, options: dict[str, int], hdr: int) -> bytes:
    build = args.build_dir
    build.mkdir(parents=True, exist_ok=True)
    spv = build / f"{name}_{'hdr' if hdr else 'sdr'}.spv"
    spv.unlink(missing_ok=True)
    defines = dict(OPTIONS, **options, FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=hdr)
    command = [args.compiler, "-V", "--target-env", "vulkan1.1", "-S", "comp", "-Os",
               *[f"-D{key}={value}" for key, value in defines.items()], f"-I{FSR / 'gpu'}",
               "-o", str(spv), str(FSR / "vk" / f"ffx_fsr3upscaler_{source}_pass.glsl")]
    subprocess.run(command, check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    subprocess.run([args.validator, "--target-env", "vulkan1.1", str(spv)], check=True)
    return spv.read_bytes()


def generate(args):
    text = [
        "/* Generated by tools/vulkan_fsr_shaders.py from port/third_party/fidelityfx-fsr3 (AMD FidelityFX "
        "Super Resolution 3.1, MIT). Do not edit. */\n",
        '#include "xgpu_vulkan_fsr_shaders.h"\n',
    ]
    programs: dict[int, list[str]] = {0: [], 1: []}
    total = 0
    for name, source, options in PASSES:
        modules = {hdr: compile_pass(args, name, source, options, hdr) for hdr in (0, 1)}
        found = bindings(modules[0])
        if bindings(modules[1]) != found:
            raise ValueError(f"{name}'s bindings differ between its SDR and HDR builds")
        text.append(f"static const struct xgpu_vulkan_fsr_binding {name}_bindings[] =\n{{")
        for binding, kind, resource in found:
            text.append(f"\t{{ {binding}, XGPU_VULKAN_FSR_{kind}, XGPU_VULKAN_FSR_{resource} }},")
        text.append("};\n")
        same = modules[0] == modules[1]
        for hdr in (0, 1):
            if hdr and same:
                programs[1].append(programs[0][-1])
                continue
            array = f"{name}_{'hdr' if hdr else 'sdr'}"
            text.append(words_array(array, modules[hdr]))
            total += len(modules[hdr])
            programs[hdr].append(f"\t{{ {array}, sizeof({array}) / 4, {name}_bindings, "
                                 f"sizeof({name}_bindings) / sizeof({name}_bindings[0]) }},")
        print(f"{name}: {len(modules[0])} / {len(modules[1])} bytes of SPIR-V{' (the same)' if same else ''}, "
              f"{len(found)} bindings")
    text.append("const struct xgpu_vulkan_fsr_program "
                "xgpu_vulkan_fsr_programs[2][XGPU_VULKAN_FSR_PASS_COUNT] =\n{")
    for hdr in (0, 1):
        text.append("\t{")
        text += ["\t" + line for line in programs[hdr]]
        text.append("\t},")
    text.append("};\n")
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{2 * len(PASSES)} modules validated for Vulkan 1.1, {total} bytes of SPIR-V")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_fsr_shaders")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        generate(args)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout.decode())
        raise


if __name__ == "__main__":
    main()
