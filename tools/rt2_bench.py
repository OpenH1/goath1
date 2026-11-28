#!/usr/bin/env python3
"""The ray-traced lighting's workbench: a moment recorded once on a rig, then the light pass's shaders and settings
changed and its frames drawn again by the replay in seconds, with numbers, without the game.

    python3 tools/rt2_bench.py record --moment a50_shields --build local/my-branch [--at 46]
        plays tools/rt2_clips.py's moment on the rig (the menu setting, the MCC import, debug.frame_time) with
        HALO_RENDER_RECORD, until --at seconds on the scenario's clock (default the moment's end), the game's own
        pictures of every 20th frame kept beside it (HALO_RENDER_SHOTS), in ~/goat-rig/rt2-bench/<name>/
    python3 tools/rt2_bench.py verify --moment a50_shields --build local/my-branch
        fidelity: the replay's frames against the game's own (lossless, pixel for pixel), with every frame before
        played whole and with the default 8; and determinism: the same frames drawn twice by the replay
    python3 tools/rt2_bench.py run --moment a50_shields --build local/my-branch [--hold 60 | --count 30]
        [--set HALO_LT_SUN=0] [--settings FILE] [--region x,y,w,h] [--watch] [--repeat 2] [--out DIR]
        the workbench: the light pass's kernels compiled here from port/linux/vulkan/lt (only those whose source
        changed), sent to the replay, which keeps the recording and the GPU's state, and the frames drawn again;
        each version's numbers and its last picture (lossless PNG). --watch: again on each saved change of the GLSL or
        of --settings (VAR=value lines: the light pass's HALO_LT_* settings), until Ctrl-C

What is drawn is the game's frame sequence as recorded: the camera, the objects and the lights moving as they did,
the light pass's histories (the denoiser's, the radiance cache's, the reservoirs') made by the same frames as in the
game, from the recording's start (rt_frames: the frames before the moment played without their GPU work but the
last --warm, played whole; --warm -1 plays every frame whole, the game's exactly). --hold instead holds the moment's
frame (played exactly first) as though the world stood still while time goes on: the noise and the convergence.

The numbers are tools/rt2_stability.py's measures on the presented picture's sRGB decoded to linear light (Rec. 709
luminance): the mean, the colour's means, the temporal standard deviation of each pixel over the last --last plays
(p50, p99; relative to the pixel's mean where it is above 0.002) and each --region's mean and deviation. They are
computed by the replay (docs/replay_protocol.md, rt_frames), next to the pictures: no picture crosses the network
but the last.

What a reload reaches: the GLSL of port/linux/vulkan/lt (tools/vulkan_lt_shaders.py's kernels) and the settings
the light pass, the bounce and the denoiser read from the environment (xgpu_vulkan_lt_settings_generation). Not
reached without recording again: the game's side (lt_lights.c's light list, the object lamps' gather: they are in
the recording's LT_LIGHTS records) and the backend's C (xgpu_vulkan_lt*.c: rebuild the replay, about 30 s warm, and
run again). Recordings hold the game's art: they stay on the rig.
"""

from __future__ import annotations

import argparse
import base64
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import rt2_clips  # noqa: E402
import rt2_scorecard  # noqa: E402
import vulkan_lt_shaders  # noqa: E402

REMOTE = "goat-rig/rt2-bench"  # (under the rig's $HOME)
SIZE = rt2_clips.SIZE
SHOT_EVERY = 20


def say(*words):
    print(*words, file=sys.stderr, flush=True)


def ssh(host, script, check=True):
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", rt2_clips.rig_target(host), "bash", "-s"], input=script,
                            text=True, capture_output=True)
    if check and result.returncode:
        raise SystemExit(f"rt2_bench: {host}: {(result.stderr or result.stdout).strip()[-800:]}")
    return result.stdout


def owner_check(host):
    if host != "omarchy":
        who = rt2_clips.owner_playing(host)
        if who:
            raise SystemExit(f"rt2_bench: {host}'s owner is playing ({who}); not starting")


def name_of(args):
    return args.name or f"{args.moment}-{args.build.replace('/', '-')}"


def build(host, ref, game=True):
    """the replay built warm on the rig (and the game, for a recording: goat-rig build's default targets)"""
    for extra in ([[]] if game else []) + [["linux_replay"]]:
        result = subprocess.run([sys.executable, str(rt2_clips.RIG), f"@{host}", "build", ref, *extra],
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"rt2_bench: building {ref} failed:\n{(result.stdout + result.stderr)[-2000:]}")


# ---------- record

def record(args):
    level, start, end, _, _, what = rt2_clips.MOMENTS[args.moment]
    at = args.at if args.at is not None else end
    folder = f"{REMOTE}/{name_of(args)}"
    home = ssh(args.host, "echo $HOME").strip()
    owner_check(args.host)
    build(args.host, args.build)
    say(f"rt2_bench: {args.build} built on {args.host}; recording {args.moment} to {at:g} s ({what})")
    ssh(args.host, f"rm -rf ~/{folder} && mkdir -p ~/{folder}/game")
    env = [e for e in rt2_clips.settings(args.moment, True) if not e.startswith(("HALO_SCREENSHOT_", "HALO_TRACE"))]
    env += [f"HALO_RENDER_RECORD={home}/{folder}/moment.hrpl", "HALO_RENDER_MARKS=1",
            f"HALO_RENDER_SHOTS={home}/{folder}/game", f"HALO_RENDER_SHOT_FRAMES=0-1000000/{SHOT_EVERY}",
            # (rt2_clips' fixed frame time, without its video: 1/60 s of game time a frame)
            f"HALO_FRAME_TIME={1 / rt2_clips.FPS:.10f}",
            "HALO_TRACE=bench.trace", f"HALO_TRACE_SECONDS={at + 0.1:g}"]
    label = f"bench-{name_of(args)}"[:80]
    started = time.monotonic()
    result = subprocess.run([sys.executable, str(rt2_clips.RIG), f"@{args.host}", "run", label, "--build",
                             args.build, "--shared", "--size", SIZE, "--seconds", str(args.seconds), "--", *env],
                            capture_output=True, text=True)
    tail = (result.stdout + result.stderr).strip().splitlines()
    ssh(args.host, f"cp ~/goat-rig/runs/{label}/game/halo.log ~/{folder}/game.log 2>/dev/null; "
        f"cp ~/goat-rig/runs/{label}/summary.txt ~/{folder}/ 2>/dev/null; rm -rf ~/goat-rig/runs/{label}", check=False)
    if result.returncode:
        raise SystemExit("rt2_bench: the recording's run failed:\n" + "\n".join(tail[-12:]))
    sha = ssh(args.host, f"git -C ~/goat-rig/builds/{rt2_scorecard.build_folder(args.build)} rev-parse --short HEAD")
    shots = sorted(int(m) for m in re.findall(r"frame_(\d+)\.png",
                                              ssh(args.host, f"ls ~/{folder}/game")))
    meta = {"moment": args.moment, "build": args.build, "sha": sha.strip(), "at": at, "size": SIZE,
            "shots": shots, "shot_every": SHOT_EVERY, "recorded": time.strftime("%Y-%m-%d %H:%M")}
    service = open_service(args.host, args.build, folder)
    try:
        meta["frames"] = service.ask({"cmd": "info"})["frames"]
    finally:
        service.close()
    # (the moment's frame: the last game picture a few frames before the recording's end)
    usable = [n for n in shots if n < meta["frames"] - 2]
    meta["frame"] = usable[-1] if usable else meta["frames"] - 3
    ssh(args.host, f"cat > ~/{folder}/meta.json <<'EOF'\n{json.dumps(meta, indent=1)}\nEOF\n"
        f"du -sh ~/{folder} | cut -f1")
    say(f"rt2_bench: recorded {meta['frames']} frames in {time.monotonic() - started:.0f} s, the moment's frame "
        f"{meta['frame']}, {len(shots)} game pictures: {args.host}:~/{folder}")
    return 0


def meta_of(host, folder):
    text = ssh(host, f"cat ~/{folder}/meta.json", check=False)
    if not text.strip():
        raise SystemExit(f"rt2_bench: no recording at {host}:~/{folder} (rt2_bench.py record first)")
    return json.loads(text)


def open_service(host, ref, folder, env=()):
    owner_check(host)
    return rt2_scorecard.Service(host, rt2_scorecard.replay_path(ref), f"{folder}/moment.hrpl", list(env), "shared")


# ---------- verify

def verify(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    replay = rt2_scorecard.replay_path(args.build)
    if args.build != meta["build"]:
        say(f"rt2_bench: note: recorded by {meta['build']}, replayed by {args.build}: only the same build is "
            "expected to match")
    frames = [n for n in meta["shots"] if n <= meta["frame"]][-args.pictures:]
    service = open_service(args.host, args.build, folder)
    try:
        if "rt_frames" not in service.ask({"cmd": "info"}).get("capabilities", []):
            raise SystemExit("rt2_bench: this replay has no rt_frames")
        results = {}
        # (every frame before played whole: the game's sequence exactly, twice; the default 8: how far a short warm
        # strays)
        for warm, attempt in ((-1, 1), (-1, 2), (8, 1)):
            out = f"{folder}/verify/w{warm}_{attempt}"
            ssh(args.host, f"rm -rf ~/{out} && mkdir -p ~/{out}")
            started = time.monotonic()
            for n in frames:
                reply = service.ask({"cmd": "rt_frames", "n": n, "count": 1, "warm": warm, "last": 1,
                                     "dir": f"{service_home(args.host)}/{out}"})
                ssh(args.host, f"mv {shlex.quote(reply['pictures'][-1]['png'])} ~/{out}/frame_{n:05d}.png")
            seconds = time.monotonic() - started
            against = "game" if attempt == 1 else f"verify/w{warm}_1"
            lines = ssh(args.host, f"cd ~/{folder} && ~/{replay} --compare verify/w{warm}_{attempt} {against} "
                        "|| true").strip().splitlines()
            same = lines[-1] if lines else "?"
            results[(warm, attempt)] = same.endswith(" 0 differ") and not same.startswith("0 ")
            what = (f"warm {warm}: against the game's pictures" if attempt == 1 else
                    f"warm {warm}: against its own first run (determinism)")
            say(f"rt2_bench: {what}: {same} ({seconds:.1f} s for {len(frames)} frames)")
            for line in lines[:-1]:
                say("    " + line)
        ok = results[(-1, 1)] and results[(-1, 2)]
    finally:
        service.close()
    say("rt2_bench: VERIFY " + ("PASS: the replay draws the game's frames, the same every time" if ok else "FAIL"))
    return 0 if ok else 1


_homes = {}


def service_home(host):
    if host not in _homes:
        _homes[host] = ssh(host, "echo $HOME").strip()
    return _homes[host]


# ---------- run: the workbench's loop

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)


def sources_of(source, seen=None):
    """a kernel's GLSL file and those it includes, under port/linux/vulkan/lt"""
    seen = seen if seen is not None else []
    path = vulkan_lt_shaders.FOLDER / source
    if path in seen or not path.exists():
        return seen
    seen.append(path)
    for name in INCLUDE.findall(path.read_text(errors="replace")):
        sources_of(name, seen)
    return seen


class Kernels:
    """the kernels compiled here into one folder, each only when its sources changed"""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.keys = {}

    def key(self, source, defines):
        digest = hashlib.sha256(" ".join(defines).encode())
        for path in sources_of(source):
            digest.update(path.name.encode() + b"\0" + path.read_bytes())
        return digest.hexdigest()

    def compile(self):
        """the changed kernels compiled (in parallel): their names; a GLSL error raises CalledProcessError"""
        wanted = {}
        for _, source, defines in vulkan_lt_shaders.KERNELS:
            name = vulkan_lt_shaders.kernel_name(source, defines)
            key = self.key(source, defines)
            if self.keys.get(name) != key:
                wanted[name] = (source, defines, key)

        def one(item):
            name, (source, defines, key) = item
            words = vulkan_lt_shaders.spirv(vulkan_lt_shaders.FOLDER / source, defines)
            return name, key, words

        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
            for name, key, words in pool.map(one, wanted.items()):
                (self.folder / f"{name}.spv").write_bytes(words)
                self.keys[name] = key
        return sorted(wanted)


def settings_from(path, given):
    values = {}
    if path:
        for line in Path(path).read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                key, _, value = line.partition("=")
                values[key.strip()] = value.strip()
    for item in given:
        key, _, value = item.partition("=")
        values[key] = value
    return values


def env_words(now, before):
    words = [f"{k}={v}" for k, v in now.items() if before.get(k) != v]
    words += [f"-{k}" for k in before if k not in now]
    for word in words:
        if " " in word:
            raise SystemExit(f"rt2_bench: {word}: a setting's value has no spaces")
    return " ".join(words)


def shown(stats, base):
    def delta(value, was):
        if was is None or was == 0:
            return f"{value:.5f}"
        return f"{value:.5f} ({(value / was - 1) * 100:+.2f}%)"

    std = stats.get("temporal_std", {})
    bstd = (base or {}).get("temporal_std", {})
    parts = [f"mean {delta(stats['mean'], (base or {}).get('mean'))}",
             "rgb " + "/".join(f"{c:.4f}" for c in stats["rgb"])]
    if std:
        parts.append(f"std p50 {delta(std['p50'], bstd.get('p50'))} p99 {delta(std['p99'], bstd.get('p99'))} "
                     f"rel p99 {delta(std['relative_p99'], bstd.get('relative_p99'))}")
    for i, region in enumerate(stats.get("regions", [])):
        was = (base or {}).get("regions", [])
        was = was[i] if i < len(was) else {}
        parts.append(f"region {i + 1} mean {delta(region['mean'], was.get('mean'))} "
                     f"rel std {region['relative_std']:.4f}")
    return "; ".join(parts)


def run(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    frame = args.frame if args.frame is not None else meta["frame"]
    if args.count and not args.hold and args.frame is None:
        frame = max(0, meta["frame"] - args.count + 1)
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    regions = [float(v) for r in args.region for v in r.split(",")]
    if len(regions) % 4:
        raise SystemExit("rt2_bench: --region x,y,w,h")
    remote = f"{REMOTE}/spv/{os.getpid()}"
    home = service_home(args.host)
    request = {"cmd": "rt_frames", "n": frame, "warm": args.warm, "last": args.last, "inline": True,
               "compress": True}
    request.update({"hold": args.hold} if args.hold else {"count": args.count})
    if regions:
        request["regions"] = regions
    if args.reset:
        request["reset"] = True
    what = (f"frame {frame} held {args.hold} plays" if args.hold else
            f"frames {frame}-{frame + args.count - 1} as recorded")
    say(f"rt2_bench: {args.moment} ({meta['build']} {meta['sha']}): {what}, warm {args.warm}, the last {args.last} "
        f"measured; replay {args.build}")
    service = None
    scratch = tempfile.TemporaryDirectory()
    kernels = Kernels(scratch.name)
    settings = {}
    base = None
    version = 0
    pictures = set()

    def stamp():
        paths = [p for p in vulkan_lt_shaders.FOLDER.glob("*.*") if p.suffix != ".inc"]
        if args.settings:
            paths.append(Path(args.settings))
        return tuple(sorted((str(p), p.stat().st_mtime_ns) for p in paths if p.exists()))

    try:
        started = time.monotonic()
        service = open_service(args.host, args.build, folder)
        caps = service.ask({"cmd": "info"}).get("capabilities", [])
        if "rt_frames" not in caps:
            raise SystemExit("rt2_bench: this replay has no rt_frames (build it from a branch with the workbench)")
        ssh(args.host, f"mkdir -p ~/{remote}")
        say(f"rt2_bench: the replay up in {time.monotonic() - started:.1f} s")
        seen = None
        repeats = args.repeat
        while True:
            now = stamp()
            if now != seen or repeats > 1:
                if now == seen:
                    repeats -= 1
                seen = now
                version += 1
                timing = {}
                t0 = time.monotonic()
                try:
                    changed = kernels.compile() if not args.built_in else []
                except subprocess.CalledProcessError as error:
                    say(f"v{version}: the GLSL does not compile ({error}); the last version kept")
                    if not args.watch:
                        return 1
                    time.sleep(0.25)
                    continue
                timing["compile"] = time.monotonic() - t0
                t1 = time.monotonic()
                wanted = settings_from(args.settings, args.set)
                words = env_words(wanted, settings)
                if changed or version == 1:
                    if not args.built_in:
                        subprocess.run(["rsync", "-a", "--delete", f"{scratch.name}/",
                                        f"{rt2_clips.rig_target(args.host)}:{remote}/k/"], check=True,
                                       capture_output=True)
                    reply = service.ask({"cmd": "rt_reload", "env": words,
                                         **({} if args.built_in else {"dir": f"{home}/{remote}/k"})})
                elif words or repeats:
                    reply = service.ask({"cmd": "rt_set", "env": words})
                settings = wanted
                timing["send"] = time.monotonic() - t1
                t2 = time.monotonic()
                reply = service.ask(request)
                timing["render"] = time.monotonic() - t2
                picture = out / f"v{version}.png"
                data = base64.b64decode(reply["pictures"][-1]["png_base64"])
                picture.write_bytes(data)
                (out / "latest.png").write_bytes(data)
                digest = hashlib.sha256(data).hexdigest()[:12]
                pictures.add(digest)
                stats = reply["stats"]
                base = base or stats
                total = time.monotonic() - t0
                say(f"v{version} [{', '.join(changed) or 'no kernel changed'}"
                    f"{'; ' + words if words else ''}]: {shown(stats, base)}")
                say(f"    {picture} (sha {digest}); {total:.1f} s: compile {timing['compile']:.1f}, send "
                    f"{timing['send']:.1f}, replay {timing['render']:.1f} (its own {reply['ms'] / 1000:.1f})")
                if args.json:
                    with open(out / "versions.jsonl", "a") as log:
                        log.write(json.dumps({"version": version, "changed": changed, "settings": settings,
                                              "stats": stats, "picture": str(picture), "sha": digest,
                                              "seconds": timing}) + "\n")
            if not args.watch:
                if args.repeat > 1 and repeats <= 1:
                    same = len(pictures) == 1
                    say(f"rt2_bench: {args.repeat} runs of unchanged code: "
                        f"{'identical pictures and numbers' if same else 'DIFFERENT pictures'}")
                    return 0 if same else 1
                if repeats <= 1:
                    return 0
                continue
            time.sleep(0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        if service:
            service.close()
        ssh(args.host, f"rm -rf ~/{remote}", check=False)
        scratch.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=["record", "verify", "run"])
    parser.add_argument("--moment", required=True, choices=list(rt2_clips.MOMENTS), help="tools/rt2_clips.py's")
    parser.add_argument("--build", required=True, help="the build (goat-rig build's ref: a branch, local/<name>)")
    parser.add_argument("--host", default="omarchy", choices=("omarchy", "5900x"))
    parser.add_argument("--name", help="the recording's folder ~/goat-rig/rt2-bench/<name> (default moment-build)")
    parser.add_argument("--at", type=float, help="record: the moment's second the recording ends at")
    parser.add_argument("--seconds", type=float, default=420, help="record: the run's limit in real seconds")
    parser.add_argument("--pictures", type=int, default=3, help="verify: the game's pictures compared (the last)")
    parser.add_argument("--frame", type=int, help="run: the frame (default the moment's, record's)")
    parser.add_argument("--count", type=int, default=30, help="run: the frames played as recorded, to the moment's")
    parser.add_argument("--hold", type=int, default=0, help="run: the moment's frame held this many plays instead")
    parser.add_argument("--reset", action="store_true", help="run --hold: the histories begun again (the scorecard's)")
    parser.add_argument("--warm", type=int, default=60, help="run: the frames before played whole (-1: every one)")
    parser.add_argument("--last", type=int, default=20, help="run: the plays measured (the last)")
    parser.add_argument("--set", action="append", default=[], help="run: VAR=value, a light pass setting (again)")
    parser.add_argument("--settings", help="run: a file of VAR=value lines, watched with --watch")
    parser.add_argument("--region", action="append", default=[], help="run: x,y,w,h, a region measured (again)")
    parser.add_argument("--built-in", action="store_true", help="run: the build's own kernels (no GLSL compiled)")
    parser.add_argument("--watch", action="store_true", help="run: again on each saved change, until Ctrl-C")
    parser.add_argument("--repeat", type=int, default=1, help="run: unchanged code drawn this many times, compared")
    parser.add_argument("--json", action="store_true", help="run: each version's numbers into OUT/versions.jsonl")
    parser.add_argument("--out", default=str(ROOT / "build" / "rt2_bench"), help="run: the pictures' folder")
    args = parser.parse_args()
    return {"record": record, "verify": verify, "run": run}[args.what](args)


if __name__ == "__main__":
    sys.exit(main())
