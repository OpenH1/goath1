"""Each frame's own GPU time for the frame rows (port/linux/src/frame_gpu.h; debug.frame_log, the performance
overlay, Save Report): on made-up frames whose timestamps are read late (port/linux/tests/frame_gpu_test.c), every
row has its own frame's time, none 0 and none two frames'. Its negative control holds no rows (FRAME_GPU_HOLD 0),
which must fail. No GPU or game data."""
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
                    str(ROOT / "port/linux/tests/frame_gpu_test.c"), "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def test_frame_gpu_rows_their_own(tmp_path):
    run = build_and_run(tmp_path, "frame_gpu_test", [])
    assert run.returncode == 0, run.stdout + run.stderr


def test_frame_gpu_rows_not_held_fail(tmp_path):
    # (the negative control: rows written as the frames are presented, late times missed)
    run = build_and_run(tmp_path, "frame_gpu_test_no_hold", ["-DFRAME_GPU_HOLD=0"])
    assert run.returncode != 0
    assert "a row not its own frame's time" in run.stdout
