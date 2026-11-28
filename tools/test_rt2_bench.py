"""tools/rt2_bench.py's results cache never hands back a stale result: its key changes with every input a replay's
result depends on (the recording, the replay's code, the kernels and light-list unit it is pointed to, the GPU, the
settings, the request), and does not change with what only names a build (its commit label, the linker's build id).
The rig's fingerprint functions (rt2_bench_cache.FINGERPRINT_LIB) run here, in this process, on stand-in files: no rig,
no ssh, no git, no subprocess."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import rt2_bench_cache as rt2_bench  # noqa: E402 (the cache's key alone: importing the bench needs numpy)

NOTE = b"\x04\x00\x00\x00\x14\x00\x00\x00\x03\x00\x00\x00GNU\x00"
LABEL = "0123456789ab"
COMMITS = [LABEL + "cdef0123456789abcdef0123", "fedcba987654" + "3210fedcba9876543210fedc"]
REPLAY = "builds/b/build/linux_x86_64/halo_replay"


def fingerprint_lib():
    names = {}
    exec(compile(rt2_bench.FINGERPRINT_LIB, "FINGERPRINT_LIB", "exec"), names)
    return names


def replay_binary(home, kernel=b"kernel-spirv-1", label=LABEL, build_id=b"\x11" * 20):
    path = home / REPLAY
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b"\x7fELF" + NOTE + build_id + b"code " + kernel + b" build\0" + label.encode() + b"\0")


def key_of(lib, home, paths, env, request, gpu="gpu one"):
    job = {"folders": ["rec"], "replay": REPLAY, "paths": paths}
    found = lib["fingerprint"](job, str(home), COMMITS, gpu)
    inputs = {"version": rt2_bench.CACHE_VERSION, "recording": found["recordings"]["rec"], "code": found["code"],
              "files": found["files"], "gpu": found["gpu"]}
    return rt2_bench.job_key(inputs, env, request)


def test_key_misses_on_every_input(tmp_path):
    lib = fingerprint_lib()
    home = tmp_path
    (home / "rec").mkdir()
    (home / "rec" / "moment.hrpl").write_bytes(b"recording one")
    (home / "spv").mkdir()
    (home / "spv" / "e1.spv").write_bytes(b"one")
    replay_binary(home)
    shaders = {"HALO_LT_SHADER_DIR": str(home / "spv")}
    env, request = ["HALO_LT_REUSE=0", "HALO_LT_DENOISE_PROBE=1"], {"n": 1360, "hold": 2200, "accumulate": "sample"}
    key = key_of(lib, home, shaders, env, request)

    # the same inputs: the same key (and the recording's hash kept beside it)
    assert key_of(lib, home, shaders, env, request) == key
    assert (home / "rec" / "moment.hrpl.sha256").exists()
    # the settings in another order: the same (the replay reads its environment); a changed setting: a miss
    assert key_of(lib, home, shaders, list(reversed(env)), request) == key
    assert key_of(lib, home, shaders, ["HALO_LT_REUSE=1", "HALO_LT_DENOISE_PROBE=1"], request) != key
    assert key_of(lib, home, shaders, env + ["HALO_LT_SUN=0"], request) != key
    # another request (more plays, another frame): a miss
    assert key_of(lib, home, shaders, env, {**request, "hold": 2201}) != key
    assert key_of(lib, home, shaders, env, {**request, "n": 1340}) != key

    # a kernel changed in the kernels the replay is pointed to: a miss; changed back: the same again
    (home / "spv" / "e1.spv").write_bytes(b"two")
    assert key_of(lib, home, shaders, env, request) != key
    (home / "spv" / "e1.spv").write_bytes(b"one")
    assert key_of(lib, home, shaders, env, request) == key

    # the replay rebuilt with a changed built-in kernel or C: a miss
    replay_binary(home, kernel=b"kernel-spirv-2")
    assert key_of(lib, home, shaders, env, request) != key
    # rebuilt from the same code at another commit (its label and build id another): the same
    replay_binary(home, label=COMMITS[1][:12], build_id=b"\x22" * 20)
    assert key_of(lib, home, shaders, env, request) == key

    # the recording made again (other bytes): a miss
    (home / "rec" / "moment.hrpl").write_bytes(b"recording two, longer")
    assert key_of(lib, home, shaders, env, request) != key
    (home / "rec" / "moment.hrpl").write_bytes(b"recording one")

    # a light-list unit the replay is pointed to: part of the key
    (home / "unit.so").write_bytes(b"unit one")
    unit = {"HALO_LT_UNIT": str(home / "unit.so")}
    one = key_of(lib, home, unit, env, request)
    (home / "unit.so").write_bytes(b"unit two")
    assert key_of(lib, home, unit, env, request) != one

    # another GPU or driver: a miss
    assert key_of(lib, home, shaders, env, request, gpu="gpu two") != key
