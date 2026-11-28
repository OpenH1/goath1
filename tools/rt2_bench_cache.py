"""tools/rt2_bench.py's results cache: its key's parts, kept apart from the bench (no rigs, no numpy, nothing run at
import), so that tools/test_rt2_bench.py tests them as they are on fixtures. The comment above rt2_bench.py's
Cache says what the key covers."""

import hashlib
import json

# what the bench asks of a result (its files' layout); raised when that changes
CACHE_VERSION = 1

# (its parts: pure functions of files, which tools/test_rt2_bench.py runs on fixtures)
FINGERPRINT_LIB = r"""
import hashlib, json, os, re, subprocess, sys
def sha_file(path, blank=()):
    data = open(path, "rb").read()
    blank = [b"\0" + w.encode() + b"\0" for w in blank if w]
    # (the linker's build id: the note's header (name size 4, id size 20, NT_GNU_BUILD_ID) and "GNU", then the id)
    note = b"\x04\x00\x00\x00\x14\x00\x00\x00\x03\x00\x00\x00GNU\x00"
    at = data.find(note)
    if at >= 0:
        data = data[:at + 16] + bytes(20) + data[at + 36:]
    for word in blank:
        data = data.replace(word, b"\0" * len(word))
    return hashlib.sha256(data).hexdigest()
def recording(path):
    # (the recording's hash, kept beside it while its size, time and inode are the same)
    st = os.stat(path)
    stamp = f"{st.st_size} {st.st_mtime_ns} {st.st_ino}"
    kept = path + ".sha256"
    try:
        words = open(kept).read().split()
        if " ".join(words[:3]) == stamp:
            return words[3]
    except (OSError, IndexError):
        pass
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            digest.update(chunk)
    open(kept + ".part", "w").write(f"{stamp} {digest.hexdigest()}\n")
    os.replace(kept + ".part", kept)
    return digest.hexdigest()
def code_hash(replay, commits):
    # (the label is the commit configure.py last ran at, not always the tree's HEAD: any of the tree's recent
    # commits that the binary holds, as a whole string)
    held = set(m.decode() for m in re.findall(rb"\0([0-9a-f]{12})\0", open(replay, "rb").read()))
    return sha_file(replay, blank=[c[:12] for c in commits if c[:12] in held])
def folder_hash(path):
    digest = hashlib.sha256()
    for name in sorted(os.listdir(path)):
        full = os.path.join(path, name)
        if os.path.isfile(full):
            digest.update(name.encode() + b"\0" + open(full, "rb").read())
    return digest.hexdigest()
def fingerprint(job, home, commits, gpu):
    out = {"recordings": {f: recording(os.path.join(home, f, "moment.hrpl")) for f in job["folders"]}}
    out["code"] = code_hash(os.path.join(home, job["replay"]), commits)
    out["files"] = {}
    for name, path in job["paths"].items():
        path = os.path.expanduser(path)
        out["files"][name] = (folder_hash(path) if os.path.isdir(path) else sha_file(path) if os.path.exists(path)
                              else "missing")
    out["gpu"] = gpu
    return out
"""


def job_key(fingerprint, env, request):
    """a result's key: the inputs' hashes (Cache's fingerprint), the settings (in any order: the replay reads its
    environment, not its order) and the request"""
    settings = sorted(env)
    words = json.dumps({"inputs": fingerprint, "env": settings, "request": request}, sort_keys=True)
    return hashlib.sha256(words.encode()).hexdigest()[:32]
