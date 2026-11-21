"""The enhanced dynamic lights' highlights in models' reflections (xgpu.h
XGPU_DYNAMIC_LIGHTS_HIGHLIGHT): a model lit by them with its reflection's cube
map in stage 3 gets them, in SPIR-V (valid for Vulkan 1.1) and in Metal; one
lit for each pixel without them, and one lit by them without a cube map, get
none (their shaders as before). No GPU, display or game data is needed.
"""

import os
from pathlib import Path
import shutil
import struct
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent
OP_DPDX = 207


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def corpus(tmp_path, tool):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    build = tmp_path / f"{tool}-build"
    subprocess.run(["sh", str(ROOT / f"tools/{tool}/build.sh"), str(ROOT), str(build)], check=True,
                   env={**os.environ, "CC": cc}, capture_output=True)
    captures = tmp_path / "captures"
    captures.mkdir(exist_ok=True)
    out = tmp_path / tool
    subprocess.run([str(build / tool), str(captures), str(out)], check=True, capture_output=True)
    files = {}
    for line in (out / "index.txt").read_text().splitlines():
        row = line.split("\t")
        files.setdefault(row[0], row[1])
    return out, files


def opcodes(path):
    data = path.read_bytes()
    words = struct.unpack(f"<{len(data) // 4}I", data)
    index, found = 5, set()
    while index < len(words):
        found.add(words[index] & 0xFFFF)
        index += max(words[index] >> 16, 1)
    return found


def test_spirv_highlights(tmp_path):
    out, files = corpus(tmp_path, "spirv_corpus")
    lit = [out / f"{files[f'synthetic-lighting-reflection{i}']}.spv" for i in (0, 1)]
    unlit = [out / f"{files[name]}.spv" for name in
             ("synthetic-lighting-reflection2", "synthetic-lighting-reflection3", "synthetic-lighting2")]
    assert all(OP_DPDX in opcodes(path) for path in lit)
    # (the negative control: no highlights without the dynamic lights or a cube map)
    assert not any(OP_DPDX in opcodes(path) for path in unlit)
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for path in lit + unlit:
        subprocess.run([validator, "--target-env", "vulkan1.1", str(path)], check=True)


def test_metal_highlights(tmp_path):
    out, files = corpus(tmp_path, "msl_corpus")
    text = {name: (out / f"{files[name]}.metal").read_text() for name in
            ("synthetic-lighting-reflection0", "synthetic-lighting-reflection2", "synthetic-lighting2")}
    assert "t3.rgb = dynamic_light_add(dynamic_lights, t3.rgb, shine * highlight_scale);" in \
        text["synthetic-lighting-reflection0"]
    assert "reflection.w = 2.0 / widened - 2.0;" in text["synthetic-lighting-reflection0"]
    assert "highlight_scale" not in text["synthetic-lighting-reflection2"]
    assert "highlight_scale" not in text["synthetic-lighting2"]
