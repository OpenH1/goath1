"""display.temporal_aa's motion on Metal (docs/temporal-aa.md): the Metal emitter writes the objects' motion as
the SPIR-V emitter does (xgpu_msl_options' motion: the vertex program run again on the previous frame's
registers, the motion written as the second colour attachment), only in the motion's variants; the shaders
of every other draw are as they were.

tools/temporal_metal/check.c, built with the shader decoders and the Metal emitter (as tools/msl_corpus is),
writes the game's 67 vertex programs (unlit, lit for each pixel, a structure's) and the synthetic programs
with the motion and without, and checks the motion's parts are in the motion's variants alone (negative
control: the plain variants and a World AO pre-pass twin, which never has it). Where Apple's Metal compiler
is (macOS with Xcode), the motion's variants are compiled too.
"""
import shutil
import subprocess
from pathlib import Path

import pytest

from test_reversed_depth import CC, ROOT, SHARED, SOURCE, game_programs


@pytest.fixture(scope="module")
def check(tmp_path_factory):
    if not CC:
        pytest.skip("no C compiler")
    build = tmp_path_factory.mktemp("temporal_metal")
    (build / "src").mkdir()
    for name in SHARED:
        shutil.copy(SOURCE / name, build / "src" / name)
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    enums = pdb.split("/* ---------- enumerations", 1)[1].split("/* ---------- structures and unions", 1)[0]
    (build / "xdk_enums.h").write_text("/* ---------- enumerations" + enums)
    executable = build / "check"
    subprocess.run([CC, "-std=gnu99", "-O1", "-Wall", f"-I{build}", f"-I{ROOT / 'tools/msl_corpus/stub'}",
                    f"-I{ROOT / 'tools/msl_corpus'}", f"-I{build / 'src'}", "-o", str(executable),
                    str(ROOT / "tools/temporal_metal/check.c")] +
                   [str(build / "src" / name) for name in SHARED if name.endswith(".c")] + ["-lm"], check=True)
    return executable


@pytest.fixture(scope="module")
def written(check, tmp_path_factory):
    captures = tmp_path_factory.mktemp("captures")
    count = game_programs(captures)
    out = tmp_path_factory.mktemp("shaders")
    result = subprocess.run([str(check), str(captures), str(out)], capture_output=True, text=True)
    return count, out, result


def test_motion_only_in_its_variants(written):
    count, out, result = written
    assert result.returncode == 0, result.stdout
    assert "failures 0" in result.stdout
    # (each game program three ways, with the motion and without)
    assert len(list(out.glob("vs*_motion.metal"))) == 3 * count
    plain = [path.read_text() for path in out.glob("*_plain.metal")]
    assert plain and not any("xMotion" in text or "[[color(1)]]" in text for text in plain)


def metal_compiler():
    xcrun = shutil.which("xcrun")
    if not xcrun:
        return None
    found = subprocess.run([xcrun, "-sdk", "macosx", "--find", "metal"], capture_output=True, text=True)
    return [xcrun, "-sdk", "macosx", "metal"] if found.returncode == 0 else None


def test_motion_variants_compile(written, tmp_path):
    compiler = metal_compiler()
    if not compiler:
        pytest.skip("no Metal compiler (macOS with Xcode)")
    _, out, _ = written
    sources = sorted(out.glob("vs*_motion.metal")) + sorted(out.glob("ps_*_motion.metal"))[:40]
    failed = []
    for source in sources:
        result = subprocess.run(compiler + ["-std=metal3.0", "-c", str(source), "-o", str(tmp_path / "out.air")],
                                capture_output=True, text=True)
        if result.returncode:
            failed.append(f"{source.name}: {result.stderr[:400]}")
    assert not failed, "\n".join(failed[:5])
