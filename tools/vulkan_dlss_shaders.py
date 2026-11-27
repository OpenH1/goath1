#!/usr/bin/env python3
"""Build DLSS Ray Reconstruction's composition pass for Vulkan (port/linux/vulkan/xgpu_vulkan_dlss.c).

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_dlss_shaders.py
It writes port/linux/vulkan/xgpu_vulkan_dlss_shaders.inc, the SPIR-V of COMPOSE below, which xgpu_vulkan_dlss.c
includes: the game needs no shader compiler. --check verifies that the checked-in file still matches.

The pass turns the shared denoiser's inputs (xgpu_vulkan_denoise.h: diffuse and specular radiance apart, the
specular's alpha its hit distance) into what Ray Reconstruction reads: one noisy colour (the radiance times its
albedo where it was demodulated, else their sum) and the hit distance alone, at the same texels as the inputs.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_dlss_shaders.inc"

# set 0: the inputs (texelFetch, the sampler immutable), the outputs; the push constants: the rectangle's corner
# and size, and whether the radiance is demodulated
COMPOSE = """#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D diffuse;
layout(set = 0, binding = 1) uniform sampler2D specular;
layout(set = 0, binding = 2) uniform sampler2D diffuse_albedo;
layout(set = 0, binding = 3) uniform sampler2D specular_albedo;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D color;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D hit_distance;
layout(push_constant) uniform Compose
{
    ivec2 corner;
    ivec2 size;
    int demodulated;
} compose;

void main()
{
    ivec2 at = ivec2(gl_GlobalInvocationID.xy);
    vec4 d, s;
    vec3 lit;

    if (any(greaterThanEqual(at, compose.size)))
        return;
    at += compose.corner;
    d = texelFetch(diffuse, at, 0);
    s = texelFetch(specular, at, 0);
    lit = d.rgb + s.rgb;
    if (compose.demodulated != 0)
        lit = d.rgb * texelFetch(diffuse_albedo, at, 0).rgb + s.rgb * texelFetch(specular_albedo, at, 0).rgb;
    imageStore(color, at, vec4(max(lit, vec3(0.0)), 1.0));
    imageStore(hit_distance, at, vec4(s.a, 0.0, 0.0, 0.0));
}
"""


def spirv() -> bytes:
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / "compose.comp"
        output = Path(folder) / "compose.spv"
        source.write_text(COMPOSE)
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1", "-o", str(output), str(source)],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(output)], check=True)
        return output.read_bytes()


def text(words: bytes) -> str:
    values = struct.unpack(f"<{len(words) // 4}I", words)
    lines = [
        "/*",
        "XGPU_VULKAN_DLSS_SHADERS.INC",
        "",
        "Written by tools/vulkan_dlss_shaders.py: the SPIR-V of Ray Reconstruction's",
        "composition pass (xgpu_vulkan_dlss.c). Change the GLSL there, then run it.",
        "*/",
        "",
        f"static const uint32_t dlss_compose_spirv[{len(values)}] = {{",
    ]
    for start in range(0, len(values), 8):
        lines.append("\t" + ", ".join(f"0x{value:08x}" for value in values[start:start + 8]) + ",")
    lines += ["};", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the checked-in file differs")
    args = parser.parse_args()
    made = text(spirv())
    if args.check:
        if OUTPUT.read_text() != made:
            print(f"{OUTPUT} is not what tools/vulkan_dlss_shaders.py makes", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(made)
    print(f"{OUTPUT.relative_to(ROOT)} written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
