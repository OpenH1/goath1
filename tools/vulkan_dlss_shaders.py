#!/usr/bin/env python3
"""Build DLSS Ray Reconstruction's composition pass for Vulkan (port/linux/vulkan/xgpu_vulkan_dlss.c).

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_dlss_shaders.py
It writes port/linux/vulkan/xgpu_vulkan_dlss_shaders.inc, the SPIR-V of COMPOSE and LT_GATHER below, which
xgpu_vulkan_dlss.c includes: the game needs no shader compiler. --check verifies that the checked-in file still
matches.

COMPOSE turns the shared denoiser's inputs (xgpu_vulkan_denoise.h: diffuse and specular radiance apart, the
specular's alpha its hit distance) into what Ray Reconstruction reads: one noisy colour (the radiance times its
albedo where it was demodulated, else their sum) and the hit distance alone, at the same texels as the inputs.

LT_GATHER does the same for the light-transport pass (xgpu_vulkan_lt.c, HALO_LT_DENOISER=rr): from its 1-spp
demodulated irradiance, the pre-pass's inputs (distance, geometric normal, owner) and the guides (xgpu_spirv.h's
C1: the shaded normal, the roughness, the motion), Ray Reconstruction's colour (the irradiance: its albedo is 1),
the world's normals with the roughness in w, the depth (reversed, of the camera xgpu_vulkan_dlss.c gives it) and
the motion (pixels to where the point was the frame before: the guides' where they carry it, else the cameras').
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


# set 0: the light, the pre-pass's inputs and its guides (texelFetch, the sampler immutable); the outputs. The push
# constants: the pass's cameras (xgpu_stream.h's struct xgpu_world_ao_pass: position, forward, right, up; this
# frame's and the last's) and corners, and x the depth's near plane, y whether there are guides, z the cameras'
# motion alone (checking)
LT_GATHER = """#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D light;
layout(set = 0, binding = 1) uniform usampler2D inputs;
layout(set = 0, binding = 2) uniform usampler2D guides;
layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D color;
layout(set = 0, binding = 4, rgba16f) uniform writeonly image2D normal_roughness;
layout(set = 0, binding = 5, r32f) uniform writeonly image2D depth;
layout(set = 0, binding = 6, rg16f) uniform writeonly image2D motion;
layout(push_constant) uniform Gather
{
    vec4 camera[4];
    vec4 previous[4];
    ivec4 corners;
    ivec4 previous_corners;
    vec4 control;
} gather;

// (xgpu.h's XGPU_WORLD_AO_INPUT_PRESENT; xgpu_spirv.h's XGPU_SPIRV_GUIDES_ROUGHNESS_*)
const uint PRESENT = 0x400u;
const uint ROUGHNESS_SHIFT = 16u;
const uint ROUGHNESS_NONE = 255u;
// (the sky's distance: its motion the cameras' turn alone)
const float FAR = 1.0e5;

vec3 unfold(vec2 e)
{
    vec2 a = abs(e);
    float z = 1.0 - a.x - a.y;
    float t = max(-z, 0.0);
    vec2 f = e + vec2(e.x >= 0.0 ? -t : t, e.y >= 0.0 ? -t : t);
    return normalize(vec3(f, z));
}

void main()
{
    ivec2 at = ivec2(gl_GlobalInvocationID.xy) + gather.corners.xy;
    vec2 low, size, screen, was;
    vec3 f, r, u, ray, p, w, n, lit;
    float distance, along, roughness, depth_value;
    uvec4 words, guide;
    bool present;

    if (any(greaterThanEqual(at, gather.corners.zw)))
        return;
    words = texelFetch(inputs, at, 0);
    present = (words.y & PRESENT) != 0u;
    low = vec2(gather.corners.xy);
    size = vec2(gather.corners.zw) - low;
    screen = (vec2(at) + 0.5 - low) * 2.0 / size - 1.0;
    f = gather.camera[1].xyz;
    r = gather.camera[2].xyz;
    u = gather.camera[3].xyz;
    ray = normalize(f + r * screen.x + u * screen.y);
    distance = present ? max(uintBitsToFloat(words.z), 1.0e-3) : FAR;
    p = gather.camera[0].xyz + ray * distance;
    along = dot(p - gather.camera[0].xyz, f) / length(f);
    lit = present ? max(texelFetch(light, at, 0).rgb, vec3(0.0)) : vec3(0.0);
    n = vec3(0.0, 0.0, 1.0);
    roughness = 1.0;
    guide = uvec4(0u);
    if (present)
    {
        n = unfold(unpackSnorm2x16(words.x));
        if (gather.control.y != 0.0)
        {
            vec3 s;
            uint rough;

            guide = texelFetch(guides, at, 0);
            s = unfold(unpackSnorm2x16(guide.x));
            if (guide.x != 0u && dot(s, n) >= 0.0)
                n = s;
            rough = (guide.z >> ROUGHNESS_SHIFT) & 0xFFu;
            roughness = rough == ROUGHNESS_NONE ? 1.0 : float(rough) / 254.0;
        }
    }
    // (where the point was the frame before, in its pixels)
    if (guide.w != 0u && gather.control.z == 0.0)
        was = vec2(at) + 0.5 - unpackHalf2x16(guide.w);
    else
    {
        vec3 pf = gather.previous[1].xyz, pr = gather.previous[2].xyz, pu = gather.previous[3].xyz;
        float before;

        w = p - gather.previous[0].xyz;
        before = max(dot(w, pf) / dot(pf, pf), 1.0e-6);
        was = vec2(dot(w, pr) / dot(pr, pr), dot(w, pu) / dot(pu, pu)) / before;
        was = vec2(gather.previous_corners.xy) + (was + 1.0) * 0.5 *
            vec2(gather.previous_corners.zw - gather.previous_corners.xy);
    }
    depth_value = present && along > 0.0 ? min(gather.control.x / along, 1.0) : 0.0;
    imageStore(color, at, vec4(lit, 1.0));
    imageStore(normal_roughness, at, vec4(n, clamp(roughness, 0.0, 1.0)));
    imageStore(depth, at, vec4(depth_value, 0.0, 0.0, 0.0));
    imageStore(motion, at, vec4(was - (vec2(at) + 0.5), 0.0, 0.0));
}
"""


def spirv(code: str = COMPOSE, name: str = "compose") -> bytes:
    with tempfile.TemporaryDirectory() as folder:
        source = Path(folder) / f"{name}.comp"
        output = Path(folder) / f"{name}.spv"
        source.write_text(code)
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.1", "-o", str(output), str(source)],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.1", str(output)], check=True)
        return output.read_bytes()


def array(name: str, words: bytes) -> list:
    values = struct.unpack(f"<{len(words) // 4}I", words)
    lines = [f"static const uint32_t {name}[{len(values)}] = {{"]
    for start in range(0, len(values), 8):
        lines.append("\t" + ", ".join(f"0x{value:08x}" for value in values[start:start + 8]) + ",")
    return lines + ["};", ""]


def text() -> str:
    lines = [
        "/*",
        "XGPU_VULKAN_DLSS_SHADERS.INC",
        "",
        "Written by tools/vulkan_dlss_shaders.py: the SPIR-V of Ray Reconstruction's",
        "composition pass and the light-transport pass's gather (xgpu_vulkan_dlss.c).",
        "Change the GLSL there, then run it.",
        "*/",
        "",
    ]
    lines += array("dlss_compose_spirv", spirv(COMPOSE, "compose"))
    lines += array("dlss_lt_gather_spirv", spirv(LT_GATHER, "lt_gather"))
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true", help="fail if the checked-in file differs")
    args = parser.parse_args()
    made = text()
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
