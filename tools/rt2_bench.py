#!/usr/bin/env python3
"""The ray-traced lighting's workbench: a moment recorded once on a rig, then the light pass's shaders and settings
changed and its frames drawn again by the replay in seconds, with numbers, without the game.

    python3 tools/rt2_bench.py record --moment a50_shields --build local/my-branch [--at 46]
        plays tools/rt2_clips.py's moment on the rig (the menu setting, the MCC import, debug.frame_time) with
        HALO_RENDER_RECORD, until --at seconds on the scenario's clock (default the moment's end), the game's own
        pictures of every 20th frame kept beside it (HALO_RENDER_SHOTS), in ~/goat-rig/rt2-bench/<name>/
    python3 tools/rt2_bench.py verify --moment a50_shields --build local/my-branch
        the acceptance test: the replay's frames against the game's own pictures (lossless, pixel for pixel), every
        frame before played whole; the same frame by two replays (determinism); and how far a faster try strays
        (--converge: only so many frames before played whole, against the exact frame)
    python3 tools/rt2_bench.py run --moment a50_shields --build local/my-branch [--hold 60 | --count 30]
        [--warm N] [--set HALO_LT_SUN=0] [--settings FILE] [--region x,y,w,h] [--watch] [--repeat 2] [--out DIR]
        the workbench: the light pass's kernels compiled here from port/linux/vulkan/lt (only those whose source
        changed: tools/vulkan_lt_shaders.py's compiler), sent to the rig, and the moment's frames drawn by the
        build's replay with them (HALO_LT_SHADER_DIR) and the settings; each version's numbers and its last
        picture (lossless PNG) in --out. --watch: again on each saved change of the GLSL or of --settings (VAR=value
        lines: the light pass's HALO_LT_* settings), until Ctrl-C. --repeat 2: unchanged code twice, compared

What is drawn is the game's frame sequence as recorded: the camera, the objects and the lights moving as they did,
the light pass's histories (the denoiser's, the radiance cache's, the reservoirs') made by the same frames as in the
game. By default (--warm -1) every frame from the recording's start is played whole, by a replay of its own: the
game's frames bit for bit (verify), about 32 s for a50_shields' 1360 frames on omarchy, nearly all of it the light
pass's GPU time on the frames before. --warm N plays only the N frames before whole (the rest without their GPU work:
faster, but the histories start cold there; verify --converge measures its error). --hold instead holds the moment's
frame (played exactly first) as though the world stood still while time goes on: the noise and the convergence.

Each try is a replay of its own: within one replay a play from the start again does not yet draw what the first
play drew (the bounce keeps something from it; with HALO_LT_INDIRECT=0 or the pass off it does), so the replay's
rt_set/rt_reload (docs/replay_protocol.md) are for exploring, and the numbers come from fresh replays.

The numbers are tools/rt2_stability.py's measures on the presented picture's sRGB decoded to linear light (Rec. 709
luminance): the mean, the colour's means, the temporal standard deviation of each pixel over the last --last plays
(p50, p99; relative to the pixel's mean where it is above 0.002) and each --region's mean and deviation. They are
computed by the replay (rt_frames), next to the pictures: no picture crosses the network but the last.

What a try reaches: the GLSL of port/linux/vulkan/lt and the HALO_LT_* settings. Not reached without recording
again: the game's side (lt_lights.c's light list, the object lamps' gather: they are in the recording's LT_LIGHTS
records). The backend's C (xgpu_vulkan_lt*.c, the denoiser's and the bounce's C): push the branch to the rig's
mirror and rebuild the replay (goat-rig build <ref> linux_replay, about 30 s warm), then run again on the same
recording. Recordings hold the game's art: they stay on the rig.

--host bosgame: the same on the Windows VM's RTX 3090 (rt2_clips.py's bosgame hosts: the hops through omarchy, Git
Bash in the VM), the Windows build and its replay (windows_replay) cross-compiled on omarchy, the replay copied
into the VM, the recordings in the VM's ~/goat-rig/rt2-bench.
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
WINDOWS_BUILDS = "goat-rig/builds"  # (bosgame: the replays in the VM, under its home)


def say(*words):
    print(*words, file=sys.stderr, flush=True)


def ssh(host, script, check=True):
    if rt2_clips.windows(host):
        result = subprocess.run(rt2_clips.remote_argv(host, script), stdin=subprocess.DEVNULL, text=True,
                                capture_output=True)
    else:
        result = subprocess.run(["ssh", "-o", "BatchMode=yes", rt2_clips.rig_target(host), "bash", "-s"],
                                input=script, text=True, capture_output=True)
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


def replay_path(host, ref):
    """the build's replay under the rig's home"""
    if rt2_clips.windows(host):
        return f"{WINDOWS_BUILDS}/{rt2_scorecard.build_folder(ref)}.win/halo_replay.exe"
    return rt2_scorecard.replay_path(ref)


def build(host, ref, game=True):
    """the replay built warm on the rig (and the game, for a recording: goat-rig build's default targets)"""
    if rt2_clips.windows(host):
        return build_windows(host, ref, game)
    for extra in ([[]] if game else []) + [["linux_replay"]]:
        result = subprocess.run([sys.executable, str(rt2_clips.RIG), f"@{host}", "build", ref, *extra],
                                capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"rt2_bench: building {ref} failed:\n{(result.stdout + result.stderr)[-2000:]}")


def build_windows(host, ref, game):
    """the Windows build cross-compiled on omarchy (goat-rig @bosgame build: the game, which goat-rig @bosgame run
    puts in the VM, and windows_replay), the replay and SDL3.dll copied into the VM's goat-rig/builds/<name>.win
    when they differ from those there"""
    targets = (["windows"] if game else []) + ["windows_replay"]
    result = subprocess.run([sys.executable, str(rt2_clips.RIG), f"@{host}", "build", ref, *targets],
                            capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f"rt2_bench: building {ref} failed:\n{(result.stdout + result.stderr)[-2000:]}")
    built = f"goat-rig/builds/{rt2_scorecard.build_folder(ref)}.win/build/windows"
    files = "halo_replay.exe SDL3.dll"
    builder = ["ssh", "-o", "BatchMode=yes", rt2_clips.rig_target(rt2_clips.WINDOWS_BUILDER)]
    there = os.path.dirname(replay_path(host, ref))
    digest = subprocess.run(builder + [f"cd ~/{built} && sha256sum {files} | cut -c1-64"], capture_output=True,
                            text=True, check=True).stdout
    if ssh(host, f"cd ~/{there} 2>/dev/null && sha256sum {files} | cut -c1-64 || true") == digest:
        return
    source = subprocess.Popen(builder + [f"tar -C ~/{built} -cf - {files}"], stdout=subprocess.PIPE)
    sink = subprocess.run(rt2_clips.remote_argv(host, f"set -e; mkdir -p ~/{there}; tar -xf - -C ~/{there}"),
                          stdin=source.stdout, capture_output=True, text=True)
    source.stdout.close()
    if source.wait() or sink.returncode:
        raise SystemExit(f"rt2_bench: copying the replay into the VM failed: {sink.stderr.strip()[-400:]}")


# ---------- record

def record(args):
    level, start, end, _, _, what = rt2_clips.MOMENTS[args.moment]
    at = args.at if args.at is not None else end
    folder = f"{REMOTE}/{name_of(args)}"
    home = service_home(args.host)
    on_windows = rt2_clips.windows(args.host)
    owner_check(args.host)
    build(args.host, args.build)
    say(f"rt2_bench: {args.build} built on {args.host}; recording {args.moment} to {at:g} s ({what})")
    ssh(args.host, (rt2_clips.windows_unlink(f"{folder}/data") + "; " if on_windows else "") +
        f"rm -rf ~/{folder} && mkdir -p ~/{folder}/game")
    if on_windows:
        # (the game's own data folder in the VM, the maps and the MCC import linked: goat-rig run's @link there)
        rt2_clips.windows_data_folder(args.host, f"{folder}/data")
        env = rt2_clips.windows_settings(args.moment, f"{home}/{folder}/data")
    else:
        env = rt2_clips.settings(args.moment, True)
    env = [e for e in env if not e.startswith(("HALO_SCREENSHOT_", "HALO_TRACE"))]
    env += [f"HALO_RENDER_RECORD={home}/{folder}/moment.hrpl", "HALO_RENDER_MARKS=1",
            f"HALO_RENDER_SHOTS={home}/{folder}/game", f"HALO_RENDER_SHOT_FRAMES=0-1000000/{SHOT_EVERY}",
            # (rt2_clips' fixed frame time, without its video: 1/60 s of game time a frame)
            f"HALO_FRAME_TIME={1 / rt2_clips.FPS:.10f}",
            "HALO_TRACE=bench.trace", f"HALO_TRACE_SECONDS={at + 0.1:g}"]
    label = f"bench-{name_of(args)}"[:80]
    started = time.monotonic()
    # (bosgame's run: one game in the VM through winvm, muted and on loopback, under bosgame's lock; its window
    # 1280x720, the run's own size there)
    options = ["--seconds", str(args.seconds)] if on_windows else ["--shared", "--size", SIZE, "--seconds",
                                                                   str(args.seconds)]
    result = subprocess.run([sys.executable, str(rt2_clips.RIG), f"@{args.host}", "run", label, "--build",
                             args.build, *options, "--", *env], capture_output=True, text=True)
    tail = (result.stdout + result.stderr).strip().splitlines()
    if on_windows:
        keep_windows_logs(args.host, label, folder)
    else:
        ssh(args.host, f"cp ~/goat-rig/runs/{label}/game/halo.log ~/{folder}/game.log 2>/dev/null; "
            f"cp ~/goat-rig/runs/{label}/summary.txt ~/{folder}/ 2>/dev/null; rm -rf ~/goat-rig/runs/{label}",
            check=False)
    if result.returncode:
        raise SystemExit("rt2_bench: the recording's run failed:\n" + "\n".join(tail[-12:]))
    tree = rt2_scorecard.build_folder(args.build) + (".win" if on_windows else "")
    sha = ssh(rt2_clips.WINDOWS_BUILDER if on_windows else args.host,
              f"git -C ~/goat-rig/builds/{tree} rev-parse --short HEAD")
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


def keep_windows_logs(host, label, folder):
    """bosgame's run kept on omarchy (goat-rig @bosgame run's ~/goat-rig/runs/<label>/bosgame): its game log and
    summary copied beside the recording in the VM, the run's folder there removed"""
    builder = ["ssh", "-o", "BatchMode=yes", rt2_clips.rig_target(rt2_clips.WINDOWS_BUILDER)]
    run = f"goat-rig/runs/{label}"
    source = subprocess.Popen(builder + [f"cd ~/{run}/bosgame 2>/dev/null && tar -cf - halo.log summary.txt "
                                         "2>/dev/null; true"], stdout=subprocess.PIPE)
    subprocess.run(rt2_clips.remote_argv(host, f"cd ~/{folder} && tar -xf - 2>/dev/null; "
                                               "[ -e halo.log ] && mv halo.log game.log; true"),
                   stdin=source.stdout, capture_output=True)
    source.stdout.close()
    source.wait()
    subprocess.run(builder + [f"rm -rf ~/{run}"], capture_output=True)


def meta_of(host, folder):
    text = ssh(host, f"cat ~/{folder}/meta.json", check=False)
    if not text.strip():
        raise SystemExit(f"rt2_bench: no recording at {host}:~/{folder} (rt2_bench.py record first)")
    return json.loads(text)


class WindowsService(rt2_scorecard.Service):
    """the replay's service in bosgame's VM (halo_replay.exe --serve, through the hops), under bosgame's locks held
    shared on its host (a game run there waits for it); refused while a game runs in the VM"""

    def __init__(self, host, replay, recording, env):
        command = " ".join(["exec", "env", *[shlex.quote(e) for e in env], shlex.quote(replay),
                            shlex.quote(recording), "--serve"])
        script = (f"cd ~\nif [ -n \"$({rt2_clips.WINDOWS_GAME_RUNNING})\" ]; then "
                  "echo 'a game is running in the VM' >&2; exit 3; fi\n" + command + "\n")
        self.host = host
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(rt2_clips.remote_argv(host, script, rt2_clips.WINDOWS_LOCKS),
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors, text=True,
                                        bufsize=1)


def open_service(host, ref, folder, env=()):
    owner_check(host)
    if rt2_clips.windows(host):
        return WindowsService(host, replay_path(host, ref), f"{folder}/moment.hrpl", list(env))
    return rt2_scorecard.Service(host, rt2_scorecard.replay_path(ref), f"{folder}/moment.hrpl", list(env), "shared")


# ---------- verify

def draw_once(host, ref, folder, request, env=(), out=None):
    """one try in a replay of its own (a fresh process: the backend as at the stream's start, so the frames are
    the game's; within one process a play from the start again still differs, see the module's doc): the reply"""
    service = open_service(host, ref, folder, env)
    try:
        if "rt_frames" not in service.ask({"cmd": "info"}).get("capabilities", []):
            raise SystemExit("rt2_bench: this replay has no rt_frames (build it from a branch with the workbench)")
        if out:
            ssh(host, f"mkdir -p ~/{out}")
            request = {**request, "dir": f"{service_home(host)}/{out}"}
        return service.ask(request)
    finally:
        service.close()


def compare(host, replay, first, second):
    """halo_replay --compare of two folders of frame_<n>.png on the rig: (all identical, its lines)"""
    lines = ssh(host, f"~/{replay} --compare ~/{first} ~/{second} || true").strip().splitlines()
    last = lines[-1] if lines else "?"
    return last.endswith(" 0 differ") and not last.startswith("0 "), lines


def verify(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    replay = replay_path(args.host, args.build)
    if args.build != meta["build"]:
        say(f"rt2_bench: note: recorded by {meta['build']}, replayed by {args.build}: only the same build is "
            "expected to match")
    frames = [n for n in meta["shots"] if n <= meta["frame"]][-args.pictures:]
    ssh(args.host, f"rm -rf ~/{folder}/verify")

    def drawn(tag, n, warm):
        out = f"{folder}/verify/{tag}"
        started = time.monotonic()
        reply = draw_once(args.host, args.build, folder, {"cmd": "rt_frames", "n": n, "count": 1, "warm": warm,
                                                           "last": 1}, out=f"{out}/raw")
        ssh(args.host, f"mv {shlex.quote(reply['pictures'][-1]['png'])} ~/{out}/frame_{n:05d}.png")
        return reply, time.monotonic() - started

    ok = True
    # 1. fidelity: each game picture against the replay's frame, every frame before it played whole
    exact = None
    for n in frames:
        exact, seconds = drawn("exact", n, -1)
        say(f"rt2_bench: frame {n} drawn exactly in {seconds:.1f} s (a replay of its own, every frame before whole)")
    same, lines = compare(args.host, replay, f"{folder}/verify/exact", f"{folder}/game")
    say("rt2_bench: FIDELITY (the replay's frames against the game's own pictures): " + lines[-1])
    for line in lines[:-1]:
        say("    " + line)
    ok &= same
    # 2. determinism: the moment's frame drawn again by another replay
    last = frames[-1]
    drawn("again", last, -1)
    ssh(args.host, f"mkdir -p ~/{folder}/verify/exact1 && cp ~/{folder}/verify/exact/frame_{last:05d}.png "
        f"~/{folder}/verify/exact1/")
    same, lines = compare(args.host, replay, f"{folder}/verify/again", f"{folder}/verify/exact1")
    say("rt2_bench: DETERMINISM (the same frame by two replays): " + lines[-1] +
        f"; the exact frame's mean {exact['stats']['mean']:.6f}")
    ok &= same
    # 3. the fast tries' error: only the warm frames before the moment played whole, against the exact frame
    for warm in [int(w) for w in args.converge.split(",") if w]:
        reply, seconds = drawn(f"warm{warm}", last, warm)
        same, lines = compare(args.host, replay, f"{folder}/verify/warm{warm}", f"{folder}/verify/exact1")
        say(f"rt2_bench: warm {warm} ({seconds:.1f} s): {lines[0] if lines else '?'}; mean "
            f"{reply['stats']['mean']:.6f}")
    say("rt2_bench: VERIFY " + ("PASS: the replay draws the game's frames, the same every time" if ok else "FAIL"))
    return 0 if ok else 1


_homes = {}


def service_home(host):
    if host not in _homes:
        # (in the Windows VM the home as the replay takes paths: C:/Users/<name>)
        _homes[host] = ssh(host, "cygpath -m ~" if rt2_clips.windows(host) else "echo $HOME").strip()
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
    say(f"rt2_bench: {args.moment} ({meta['build']} {meta['sha']}): {what}; "
        f"{'every frame before played whole (exact)' if args.warm < 0 else f'the {args.warm} before played whole'}; "
        f"the last {args.last} measured; replay {args.build}")
    scratch = tempfile.TemporaryDirectory()
    kernels = Kernels(scratch.name)
    base = None
    version = 0
    pictures = []

    def stamp():
        paths = [p for p in vulkan_lt_shaders.FOLDER.glob("*.*") if p.suffix != ".inc"]
        if args.settings:
            paths.append(Path(args.settings))
        return tuple(sorted((str(p), p.stat().st_mtime_ns) for p in paths if p.exists()))

    try:
        ssh(args.host, f"mkdir -p ~/{remote}/k")
        seen = None
        while True:
            now = stamp()
            if now != seen or len(pictures) < args.repeat:
                seen = now
                version += 1
                t0 = time.monotonic()
                try:
                    changed = kernels.compile() if not args.built_in else []
                except subprocess.CalledProcessError as error:
                    say(f"v{version}: the GLSL does not compile ({error}); the last version kept")
                    if not args.watch:
                        return 1
                    time.sleep(0.25)
                    continue
                t_compile = time.monotonic() - t0
                settings = settings_from(args.settings, args.set)
                env = [f"{k}={v}" for k, v in settings.items()]
                if not args.built_in:
                    if changed and rt2_clips.windows(args.host):
                        # (no rsync in the VM: the changed kernels as a tar stream through the hops)
                        names = [f"{name}.spv" for name in changed]
                        stream = subprocess.run(["tar", "--no-xattrs", "--no-mac-metadata", "-C", scratch.name,
                                                 "-cf", "-", *names], capture_output=True, check=True,
                                                env={**os.environ, "COPYFILE_DISABLE": "1"}).stdout
                        subprocess.run(rt2_clips.remote_argv(args.host, f"tar -xf - -C ~/{remote}/k"), check=True,
                                       capture_output=True, input=stream)
                    elif changed:
                        subprocess.run(["rsync", "-a", "--delete", f"{scratch.name}/",
                                        f"{rt2_clips.rig_target(args.host)}:{remote}/k/"], check=True,
                                       capture_output=True)
                    env.append(f"HALO_LT_SHADER_DIR={home}/{remote}/k")
                t1 = time.monotonic()
                # (each try a replay of its own: exact, and the same every time)
                reply = draw_once(args.host, args.build, folder, request, env)
                t_render = time.monotonic() - t1
                picture = out / f"v{version}.png"
                data = base64.b64decode(reply["pictures"][-1]["png_base64"])
                picture.write_bytes(data)
                (out / "latest.png").write_bytes(data)
                digest = hashlib.sha256(data).hexdigest()[:12]
                stats = reply["stats"]
                pictures.append((digest, json.dumps(stats, sort_keys=True)))
                base = base or stats
                total = time.monotonic() - t0
                say(f"v{version} [{', '.join(changed) or 'no kernel changed'}"
                    f"{'; ' + ' '.join(env[:-1] if not args.built_in else env) if settings else ''}]: "
                    f"{shown(stats, base)}")
                say(f"    {picture} (sha {digest}); edit to numbers {total:.1f} s: compile {t_compile:.1f}, "
                    f"replay {t_render:.1f} (its frames {reply['ms'] / 1000:.1f})")
                if args.json:
                    with open(out / "versions.jsonl", "a") as log:
                        log.write(json.dumps({"version": version, "changed": changed, "settings": settings,
                                              "stats": stats, "picture": str(picture), "sha": digest,
                                              "seconds": {"compile": t_compile, "replay": t_render}}) + "\n")
            if not args.watch and len(pictures) >= args.repeat:
                if args.repeat > 1:
                    same = len(set(pictures)) == 1
                    say(f"rt2_bench: {args.repeat} tries of unchanged code: "
                        f"{'identical pictures and numbers' if same else 'DIFFERENT'}")
                    return 0 if same else 1
                return 0
            time.sleep(0.2)
    except KeyboardInterrupt:
        return 0
    finally:
        ssh(args.host, f"rm -rf ~/{remote}", check=False)
        scratch.cleanup()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=["record", "verify", "run"])
    parser.add_argument("--moment", required=True, choices=list(rt2_clips.MOMENTS), help="tools/rt2_clips.py's")
    parser.add_argument("--build", required=True, help="the build (goat-rig build's ref: a branch, local/<name>)")
    parser.add_argument("--host", default="omarchy", choices=("omarchy", "5900x", *rt2_clips.WINDOWS_HOSTS),
                        help="the rig (bosgame: the Windows VM with the RTX 3090, through omarchy)")
    parser.add_argument("--name", help="the recording's folder ~/goat-rig/rt2-bench/<name> (default moment-build)")
    parser.add_argument("--at", type=float, help="record: the moment's second the recording ends at")
    parser.add_argument("--seconds", type=float, default=420, help="record: the run's limit in real seconds")
    parser.add_argument("--pictures", type=int, default=3, help="verify: the game's pictures compared (the last)")
    parser.add_argument("--converge", default="30,120,300,600",
                        help="verify: the warms whose frame is compared with the exact one (the fast tries' error)")
    parser.add_argument("--frame", type=int, help="run: the frame (default the moment's, record's)")
    parser.add_argument("--count", type=int, default=30, help="run: the frames played as recorded, to the moment's")
    parser.add_argument("--hold", type=int, default=0, help="run: the moment's frame held this many plays instead")
    parser.add_argument("--reset", action="store_true", help="run --hold: the histories begun again (the scorecard's)")
    parser.add_argument("--warm", type=int, default=-1, help="run: the frames before played whole (-1, the default: every one, exact; fewer is faster, verify --converge gives its error)")
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
