"""display.dynamic_resolution's controller (port/linux/src/dynamic_resolution.h, #1226) on made-up GPU times
(port/linux/tests/dynamic_resolution_test.c): steps down to what fits, never under the floor or over the
ceiling, held in its band, back up a step at a time. Its negative control builds it with no band between the
marks, which must fail. No GPU or game data."""
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent


def build_and_run(tmp_path: Path, name: str, defines: list[str]) -> subprocess.CompletedProcess:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a C compiler")
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'port/linux/src'}", *defines,
                    str(ROOT / "port/linux/tests/dynamic_resolution_test.c"), "-o", str(binary), "-lm"], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def test_dynamic_resolution_steps(tmp_path):
    run = build_and_run(tmp_path, "dynamic_resolution_test", [])
    assert run.returncode == 0, run.stdout + run.stderr


def test_dynamic_resolution_without_its_band_fails(tmp_path):
    # (the negative control: no band between the marks, so a scale in it is not held)
    run = build_and_run(tmp_path, "dynamic_resolution_test_no_band", ["-DDYNAMIC_RESOLUTION_TEST_NO_BAND"])
    assert run.returncode != 0
    assert "scale in the band not held" in run.stdout
