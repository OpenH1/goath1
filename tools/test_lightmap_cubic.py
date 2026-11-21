"""display.smooth_lightmaps (#1225, port/linux/src/xgpu.h's XGPU_TEXTURE_MODES_LIGHTMAP_CUBIC): the SPIR-V emitter's
lightmap pass with the cubic filter, run on the CPU (tools/world_ao_light/spirv_run.py, its four lookups bilinear
and clamped to the edge, as the GPU's), is the uniform cubic B-spline over the 4x4 texels about the point: across a
step its profile is the B-spline's (a ramp two texels wider than bilinear's one, never under 0 or past 1), and a flat
lightmap stays exactly as it is (the weights sum to 1). The negative control is the same pass without the filter
(the key's bit not set): bilinear's ramp, which the B-spline's profile check rejects. The Metal emitter's has the
function only with the filter (Classic's shader is as before)."""
import math
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "port/linux/src"
sys.path.insert(0, str(ROOT / "tools/world_ao_light"))
import spirv_run  # noqa: E402

CC = shutil.which("cc") or shutil.which("clang")
SHARED = ["xgpu.h", "xgpu_backend.h", "xgpu_hdr.h", "xgpu_shader.h", "xgpu_msl.h", "xgpu_spirv.h", "xgpu_stream.h",
          "nv2a_vsh.c", "nv2a_psh.c", "xgpu_text.c", "xgpu_msl.c", "xgpu_spirv.c"]
U_TEXTURE_SCALE = 14  # (xgpu_spirv.c's member)
WIDTH, HEIGHT = 8, 4


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    directory = tmp_path_factory.mktemp("lightmap_cubic")
    (directory / "src").mkdir()
    for name in SHARED:
        shutil.copy(SOURCE / name, directory / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (directory / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = directory / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{directory}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{directory / 'src'}", "-o", str(executable), str(ROOT / "tools/lightmap_cubic/check.c")] +
                   [str(directory / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    subprocess.run([str(executable), "spirv", str(directory)], check=True)
    return directory


def module(check, name):
    data = (check / f"{name}.spv").read_bytes()
    return spirv_run.Module([int.from_bytes(data[at:at + 4], "little") for at in range(0, len(data), 4)])


def lightmap(texel):
    """sample(name, uv) for the runner: texture 2 is the lightmap (texel(x, y)), bilinear, clamped to its edge"""
    def sample(name, uv):
        if uv is None:
            return [WIDTH, HEIGHT]
        result = 0.0
        tx, ty = uv[0] * WIDTH - 0.5, uv[1] * HEIGHT - 0.5
        x0, y0 = math.floor(tx), math.floor(ty)
        fx, fy = tx - x0, ty - y0
        for j, wy in ((0, 1 - fy), (1, fy)):
            for i, wx in ((0, 1 - fx), (1, fx)):
                result += wx * wy * texel(min(max(x0 + i, 0), WIDTH - 1), min(max(y0 + j, 0), HEIGHT - 1))
        return [spirv_run.f32(result)] * 3 + [1.0]
    return sample


def run(shader, texel, u, v=0.5):
    inputs = {"u": {U_TEXTURE_SCALE: [[1.0, 1.0, 0.0, 0.0]] * 4}, "xT2": [u, v, 0.0, 1.0]}
    return spirv_run.run(shader, inputs, lightmap(texel))[0]


def bspline_step(x):
    """the uniform cubic B-spline's profile across a step at texel 4 (0 before, 1 from it), x in texels"""
    t = x - 0.5
    i = math.floor(t)
    f = t - i
    weights = [(1 - f) ** 3 / 6, (3 * f ** 3 - 6 * f ** 2 + 4) / 6, (-3 * f ** 3 + 3 * f ** 2 + 3 * f + 1) / 6, f ** 3 / 6]
    return sum(w for k, w in enumerate(weights) if min(max(i - 1 + k, 0), WIDTH - 1) >= 4)


def step(x, y):
    return 1.0 if x >= 4 else 0.0


POINTS = [1.5 + 0.125 * k for k in range(41)]  # (texels 1.5 to 6.5 across the step)


def profile_is_bspline(shader):
    return all(abs(run(shader, step, x / WIDTH) - bspline_step(x)) < 2e-5 for x in POINTS)


def test_cubic_is_the_bspline_across_a_step(check):
    cubic = module(check, "cubic")
    values = [run(cubic, step, x / WIDTH) for x in POINTS]
    assert profile_is_bspline(cubic)
    assert all(0.0 <= value <= 1.0 for value in values)
    assert all(b >= a - 1e-6 for a, b in zip(values, values[1:]))
    # (wider than bilinear's ramp, 3.5 to 4.5: already off 0 at texel 3.25, short of 1 at 4.75)
    assert run(cubic, step, 3.25 / WIDTH) > 0.01 and run(cubic, step, 4.75 / WIDTH) < 0.99
    # (a flat lightmap as it is, anywhere, its edge too)
    for u, v in ((0.0, 0.0), (0.37, 0.61), (0.999, 0.2), (0.5, 1.0)):
        assert abs(run(cubic, lambda x, y: 0.37, u, v) - spirv_run.f32(0.37)) < 1e-6


def test_negative_control_bilinear_is_not(check):
    bilinear = module(check, "bilinear")
    assert not profile_is_bspline(bilinear)
    assert run(bilinear, step, 3.25 / WIDTH) == 0.0


def test_msl_has_the_function_only_with_the_filter(check):
    text = subprocess.run([str(check / "check"), "msl"], check=True, capture_output=True, text=True).stdout
    cubic, bilinear = text.split("==== bilinear\n")
    assert "lightmap_cubic(tex2, tex2_sampler" in cubic and "gradient2d(" in cubic
    assert "lightmap_cubic" not in bilinear
