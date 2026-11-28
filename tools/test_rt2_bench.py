"""tools/rt2_bench.py's results cache never hands back a stale result: its key changes with every input a replay's
result depends on (the recording, the replay's code, the kernels and light-list unit it is pointed to, the GPU, the
settings, the request), and does not change with what only names a build (its commit label, the linker's build id).
The rig's fingerprint script (rt2_bench.FINGERPRINT) is run here on stand-in files."""

import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rt2_bench  # noqa: E402

NOTE = b"\x04\x00\x00\x00\x14\x00\x00\x00\x03\x00\x00\x00GNU\x00"


def fingerprint(home, paths=None):
    job = {"folders": ["rec"], "replay": "builds/b/build/linux_x86_64/halo_replay", "paths": paths or {}}
    out = subprocess.run([sys.executable, "-", json.dumps(job)], input=rt2_bench.FINGERPRINT, text=True,
                         capture_output=True, env={**os.environ, "HOME": str(home)}, check=True).stdout
    found = json.loads(out.strip().splitlines()[-1])
    return {"version": rt2_bench.CACHE_VERSION, "recording": found["recordings"]["rec"], "code": found["code"],
            "files": found["files"], "gpu": found["gpu"]}


def replay_binary(home, kernel=b"kernel-spirv-1", label="0123456789ab", build_id=b"\x11" * 20):
    tree = home / "builds" / "b"
    (tree / "build" / "linux_x86_64").mkdir(parents=True, exist_ok=True)
    if not (tree / ".git").exists():
        subprocess.run(["git", "init", "-q", str(tree)], check=True)
        subprocess.run(["git", "-C", str(tree), "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-q",
                        "--allow-empty", "-m", "t"], check=True)
    real = subprocess.run(["git", "-C", str(tree), "rev-parse", "--short=12", "HEAD"], capture_output=True,
                          text=True).stdout.strip()
    label = real if label == "head" else label
    (tree / "build" / "linux_x86_64" / "halo_replay").write_bytes(
        b"\x7fELF" + NOTE + build_id + b"code " + kernel + b" build\0" + label.encode() + b"\0")


def test_key_misses_on_every_input(tmp_path):
    home = tmp_path
    (home / "rec").mkdir()
    (home / "rec" / "moment.hrpl").write_bytes(b"recording one")
    (home / "spv").mkdir()
    (home / "spv" / "e1.spv").write_bytes(b"one")
    replay_binary(home, label="head")
    base = fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")})
    env, request = ["HALO_LT_REUSE=0", "HALO_LT_DENOISE_PROBE=1"], {"n": 1360, "hold": 2200, "accumulate": "sample"}
    key = rt2_bench.job_key(base, env, request)

    # the same inputs: the same key (and the recording's hash kept beside it is reused)
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) == key
    assert (home / "rec" / "moment.hrpl.sha256").exists()
    # the settings in another order: the same (the replay reads its environment); a changed setting: a miss
    assert rt2_bench.job_key(base, list(reversed(env)), request) == key
    assert rt2_bench.job_key(base, ["HALO_LT_REUSE=1", "HALO_LT_DENOISE_PROBE=1"], request) != key
    assert rt2_bench.job_key(base, env + ["HALO_LT_SUN=0"], request) != key
    # another request (more plays, another frame): a miss
    assert rt2_bench.job_key(base, env, {**request, "hold": 2201}) != key
    assert rt2_bench.job_key(base, env, {**request, "n": 1340}) != key

    # a kernel changed in the working tree's kernels the replay is pointed to: a miss
    (home / "spv" / "e1.spv").write_bytes(b"two")
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) != key
    (home / "spv" / "e1.spv").write_bytes(b"one")
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) == key

    # the replay rebuilt with a changed built-in kernel or C: a miss
    replay_binary(home, kernel=b"kernel-spirv-2", label="head")
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) != key
    # rebuilt from the same code, only its build id another: a hit
    replay_binary(home, label="head", build_id=b"\x22" * 20)
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) == key

    # the recording made again (other bytes): a miss
    (home / "rec" / "moment.hrpl").write_bytes(b"recording two")
    assert rt2_bench.job_key(fingerprint(home, {"HALO_LT_SHADER_DIR": str(home / "spv")}), env, request) != key

    # a light-list unit the replay is pointed to: part of the key
    (home / "unit.so").write_bytes(b"unit one")
    one = fingerprint(home, {"HALO_LT_UNIT": str(home / "unit.so")})
    (home / "unit.so").write_bytes(b"unit two")
    assert fingerprint(home, {"HALO_LT_UNIT": str(home / "unit.so")})["files"] != one["files"]

    # another GPU or driver: a miss
    assert rt2_bench.job_key({**base, "gpu": "other"}, env, request) != key
