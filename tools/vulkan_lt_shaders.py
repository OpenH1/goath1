#!/usr/bin/env python3
"""Build the light-transport kernels for Vulkan (port/linux/vulkan/xgpu_vulkan_lt.c).

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_lt_shaders.py
It writes port/linux/vulkan/lt/xgpu_vulkan_lt_shaders.inc, the SPIR-V of the GLSL in port/linux/vulkan/lt/, which
xgpu_vulkan_lt.c includes: the game needs no shader compiler. --check verifies that the checked-in file still matches.

E1/E2 (the flashlight's direct light, accumulated by count): lt/e1.vert, a triangle over the window, and lt/e1.frag.
W3, the denoiser (xgpu_vulkan_denoise.c): lt/denoise_gradient.frag, denoise_temporal.frag, denoise_guide.frag and
denoise_atrous.frag,
with lt/denoise_common.glsl and lt/gradient_pixel.glsl included.
The indirect light and reflections (xgpu_vulkan_lt_indirect.c): lt/e1.frag again with LT_INDIRECT (its lights' functions,
lt/indirect.glsl's main, lt/indirect_cache.glsl's cells).
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FOLDER = ROOT / "port/linux/vulkan/lt"
OUTPUT = FOLDER / "xgpu_vulkan_lt_shaders.inc"
# (the array's name, the source, its defines)
KERNELS = [("lt_e1_vertex_spirv", "e1.vert", []), ("lt_e1_spirv", "e1.frag", []),
           ("lt_denoise_gradient_spirv", "denoise_gradient.frag", []),
           ("lt_denoise_temporal_spirv", "denoise_temporal.frag", []),
           ("lt_denoise_atrous_spirv", "denoise_atrous.frag", []), ("lt_denoise_guide_spirv", "denoise_guide.frag", []),
           ("lt_indirect_spirv", "e1.frag", ["LT_INDIRECT"])]


def kernel_name(source: str, defines=()) -> str:
    """the kernel's name in the backend (xgpu_vulkan_lt_kernel's; a reload's <name>.spv): its source's, a variant's
    with its define (e1.frag's LT_INDIRECT: e1_indirect.frag)"""
    if "LT_INDIRECT" in defines:
        return source.replace(".frag", "_indirect.frag")
    return source


def spirv(source: Path, defines=()) -> bytes:
    with tempfile.TemporaryDirectory() as folder:
        output = Path(folder) / "out.spv"
        # (Vulkan 1.2's SPIR-V 1.5: ray queries; the backend asks for Vulkan 1.3)
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.2", *[f"-D{d}" for d in defines], "-o",
                        str(output), str(source)], check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.2", str(output)], check=True)
        return output.read_bytes()


def text() -> str:
    lines = [
        "/*",
        "XGPU_VULKAN_LT_SHADERS.INC",
        "",
        "Written by tools/vulkan_lt_shaders.py: the SPIR-V of the light-transport",
        "kernels (xgpu_vulkan_lt.c) from the GLSL beside it. Change the GLSL, then run it.",
        "*/",
        "",
    ]
    for name, source, defines in KERNELS:
        words = spirv(FOLDER / source, defines)
        values = struct.unpack(f"<{len(words) // 4}I", words)
        lines.append(f"static const uint32_t {name}[{len(values)}] = {{")
        for start in range(0, len(values), 8):
            lines.append("\t" + ", ".join(f"0x{value:08x}" for value in values[start:start + 8]) + ",")
        lines += ["};", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the checked-in file differs")
    args = parser.parse_args()
    made = text()
    if args.check:
        if OUTPUT.read_text() != made:
            print(f"{OUTPUT} is not what tools/vulkan_lt_shaders.py makes", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(made)
    print(f"{OUTPUT.relative_to(ROOT)} written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
