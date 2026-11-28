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
        [--checkpoint K | --exact | --warm N] [--set HALO_LT_SUN=0] [--settings FILE] [--region x,y,w,h] [--watch]
        [--repeat 2] [--out DIR]
        the workbench: the light pass's kernels compiled here from port/linux/vulkan/lt (only those whose source
        changed: tools/vulkan_lt_shaders.py's compiler), sent to the rig, and the moment's frames drawn by the
        build's replay with them (HALO_LT_SHADER_DIR) and the settings; each version's numbers and its last
        picture (lossless PNG) in --out. --watch: again on each saved change of the GLSL or of --settings (VAR=value
        lines: the light pass's HALO_LT_* settings), until Ctrl-C. --repeat 2: unchanged code twice, compared

    python3 tools/rt2_bench.py view cut_share,history,disocclusion --moment a50_shields --build local/my-branch
        [--region x,y,w,h] [--hold 60 | --count 30] [--warm N] [--set ...]
        the shortcuts' internals at the moment's frame (the replay's rt_view): the denoiser's temporal stage drawn
        again from the same inputs (lt/denoise_temporal.frag's PROBE, HALO_LT_DENOISE_PROBE=1), what it decided at
        each pixel. Pictures (grey = value / --scale, or the 99th percentile; dark purple: not drawn or no value)
        of any of: found (the history's count found where it is the same surface), cut (the gradients' share cut,
        changed()), kept (the count after both cuts), count (the count used this frame: the history length),
        cut_share (1 - kept / found: the history cut), disocclusion, z_factor / z_score (the sample's own cut),
        bounce_hold, footprint, grad_before / grad_now / grad_change (the stochastic gradient's inputs about the
        pixel), lit_before / lit_now / lit_change (the deterministic half's), relit, cap, mean_luminance; and each
        --region's means and the shares of its pixels found, disoccluded, cut by the gradients, by the sample, held
        by the bounce, moving, relit

    python3 tools/rt2_bench.py reference|terms|triage --moment a50_shields --build local/my-branch --region x,y,w,h
        [--classic NAME] [--plays 2000] [--term-plays 600]
        reference: the same kernels on the held frame with the real-time shortcuts off (REFERENCE_ENV: no ReSTIR
        reuse, the bounce traced without the radiance cache; the denoiser bypassed, its input sample accumulated),
        converged when the mean's error from 8 batches is under 1%, against the real-time frame: error = bias (the
        held real-time mean less the reference) + motion (as recorded less held), and the held noise. terms: the
        reference's light by source (TERMS: each with the others masked, less all masked) and the bounce. triage:
        both, the denoiser's history cut, and the reference against the classic look (a record --setting-off
        recording): estimator problem or data/look problem, per region

What is drawn is the game's frame sequence as recorded: the camera, the objects and the lights moving as they did,
the light pass's histories (the denoiser's, the radiance cache's, the reservoirs') made by the same frames as in the
game. The light pass's state comes from an exact play kept at a checkpoint (below); --exact plays every frame from
the recording's start whole in a replay of its own (the game's frames bit for bit, about 32 s for a50_shields' 1360
frames on omarchy); --warm N plays only the N frames before whole (faster, but the histories start cold there;
verify --converge measures its error). --hold instead holds the moment's
frame (played exactly first) as though the world stood still while time goes on: the noise and the convergence.

Checkpoints (the default): one replay serves every try. It plays the recording once, exactly, to --checkpoint
frames before the first measured frame and keeps the light pass's whole state there (rt_checkpoint: its images and
histories, the bounce's, the denoiser's, the pre-pass's occlusion's). Each try then reloads the kernels and settings
(rt_reload), plays the frames before the checkpoint without their GPU work, puts the state back, and plays the last
--checkpoint frames and the measured ones whole with the new code: a couple of seconds, not a full replay. With
unchanged code a try is the full replay bit for bit (verify checks it); with new code the frames before the
checkpoint were drawn by the code the run started with, so its effect needs --checkpoint frames to reach the
histories (verify --change measures how far a try strays by k). --exact: each try a fresh replay with every frame
whole (the old way, ~35 s); --warm N: a fresh replay, the N frames before whole and the histories cold.

The numbers are tools/rt2_stability.py's measures on the presented picture's sRGB decoded to linear light (Rec. 709
luminance): the mean, the colour's means, the temporal standard deviation of each pixel over the last --last plays
(p50, p99; relative to the pixel's mean where it is above 0.002) and each --region's mean and deviation. They are
computed by the replay (rt_frames), next to the pictures: no picture crosses the network but the last.

What a try reaches: the GLSL of port/linux/vulkan/lt and the HALO_LT_* settings; with --unit, how the light list
is built (port/linux/game/lt_lights_build.c, from the recording's LT_INPUTS). Not reached without recording again:
what feeds the list's inputs (lt_lights.c's gather, lt_emitters.c, world_rt.c's object lamps, dynamic_lights.c). The backend's C (xgpu_vulkan_lt*.c, the denoiser's and the bounce's C): push the branch to the rig's
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
import rt2_bench_cache  # noqa: E402
import rt2_clips  # noqa: E402
import rt2_scorecard  # noqa: E402
import vulkan_lt_shaders  # noqa: E402

REMOTE = "goat-rig/rt2-bench"  # (under the rig's $HOME)
SIZE = rt2_clips.SIZE
SHOT_EVERY = 20
WINDOWS_BUILDS = "goat-rig/builds"  # (bosgame: the replays in the VM, under its home)
# run's default --checkpoint: the frames played whole with the new code before the measured ones. verify --change
# HALO_LT_DENOISE_CAP=16 (a50_shields, 5900X): 8.6% of pixels off at 20, 1.7% (at most 4 of 255) at 60, then about
# 0.8% (at most 3) from 100 on, the reservoirs' noise; warm 120's cold start strays 16% (at most 11)
CHECKPOINT_FRAMES = 60


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
        # (--setting-off: the classic look, the menu setting's switch alone changed)
        if args.setting_off:
            env = ["HALO_RAY_TRACED_LIGHTING=0" if e.startswith("HALO_RAY_TRACED_LIGHTING=") else e for e in env]
    else:
        env = rt2_clips.settings(args.moment, not args.setting_off)
    env = [e for e in env if not e.startswith(("HALO_SCREENSHOT_", "HALO_TRACE"))]
    env += [f"HALO_RENDER_RECORD={home}/{folder}/moment.hrpl", "HALO_RENDER_MARKS=1",
            f"HALO_RENDER_SHOTS={home}/{folder}/game", f"HALO_RENDER_SHOT_FRAMES=0-1000000/{SHOT_EVERY}",
            # (rt2_clips' fixed frame time, without its video: 1/60 s of game time a frame)
            f"HALO_FRAME_TIME={1 / rt2_clips.FPS:.10f}",
            "HALO_TRACE=bench.trace", f"HALO_TRACE_SECONDS={at + 0.1:g}"]
    # (--set: the game's own settings for the recording, a measuring log's)
    env += args.set
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
            "lighting": "classic" if args.setting_off else "ray traced",
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
        reply = service.ask(request)
        if any(e.startswith("HALO_LT_UNIT=") for e in env):
            reply["lt_unit"] = service.ask({"cmd": "info"}).get("lt_unit")
        return reply
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
                                                           "last": 1}, env=args.set, out=f"{out}/raw")
        if unit_line(reply):
            say("    " + unit_line(reply))
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
    # 4. checkpoints: restore plus k frames, unchanged code, against the exact frame (bit for bit)
    if args.checkpoint > 0:
        ok &= checkpoint_check(args, folder, replay, last)
    # 5. a change's reach from a checkpoint: the change's exact frames against a try's, by the frames since it
    if args.change:
        checkpoint_reach(args, folder, replay, last)
    # 6. the light-list unit (--unit): every list it built the recorded one, and a second play from the start in the
    # same replay too (none of the first play's inputs reaching the second's lists)
    if args.unit:
        service = open_service(args.host, args.build, folder, args.set)
        try:
            for _ in range(2):
                service.ask({"cmd": "rt_frames", "n": last, "count": 1, "warm": -1, "last": 1})
            info = service.ask({"cmd": "info"}).get("lt_unit") or {}
        finally:
            service.close()
        same = bool(info.get("loaded")) and info.get("rebuilt", 0) > 0 and not info.get("differ")
        say(f"rt2_bench: UNIT (two plays from the start in one replay): {unit_line({'lt_unit': info})}")
        ok &= same
    say("rt2_bench: VERIFY " + ("PASS: the replay draws the game's frames, the same every time" if ok else "FAIL"))
    return 0 if ok else 1


def frames_named(host, folder):
    """rt_<n>_<k>.png in a rig folder renamed frame_<n+k-1>.png (halo_replay --compare's names)"""
    moves = []
    for name in ssh(host, f"ls ~/{folder}").split():
        match = re.match(r"rt_(\d+)_(\d+)\.png$", name)
        if match:
            moves.append(f"mv {name} frame_{int(match.group(1)) + int(match.group(2)) - 1:05d}.png")
    if moves:
        ssh(host, f"cd ~/{folder} && " + " && ".join(moves))


def checkpoint_check(args, folder, replay, last):
    """the moment's frame from a checkpoint args.checkpoint frames before it, unchanged code, in the replay that
    kept it, after a play of the whole: the same picture as the exact one, bit for bit"""
    k = args.checkpoint
    out = f"{folder}/verify/checkpoint"
    ssh(args.host, f"mkdir -p ~/{out}")
    service = open_service(args.host, args.build, folder)
    try:
        if "rt_checkpoint" not in service.ask({"cmd": "info"}).get("capabilities", []):
            say("rt2_bench: CHECKPOINT: this replay has none")
            return False
        started = time.monotonic()
        service.ask({"cmd": "rt_checkpoint", "n": last - k - 1})
        kept = time.monotonic() - started
        seconds = []
        for _ in range(2):
            started = time.monotonic()
            service.ask({"cmd": "rt_frames", "n": last, "count": 1, "last": 1, "checkpoint": True,
                         "dir": f"{service_home(args.host)}/{out}"})
            seconds.append(time.monotonic() - started)
    finally:
        service.close()
    frames_named(args.host, out)
    same, lines = compare(args.host, replay, out, f"{folder}/verify/exact1")
    say(f"rt2_bench: CHECKPOINT (frame {last} from a checkpoint {k} frames before, unchanged code, against the "
        f"exact frame): {lines[-1] if lines else '?'}; kept in {kept:.1f} s, tries "
        f"{', '.join(f'{x:.1f}' for x in seconds)} s")
    return same


def checkpoint_reach(args, folder, replay, last):
    """how far a try from a checkpoint strays with a change (args.change: VAR=value settings, the stand-in for an
    edit): the change's exact frames (a fresh replay, every frame whole) against a try's from a checkpoint kept with
    the build's code --span frames before, frame by frame: the error by the frames since the checkpoint"""
    span = args.span
    first = last - span + 1
    change = " ".join(args.change)
    out = f"{folder}/verify/reach"
    ssh(args.host, f"rm -rf ~/{out} && mkdir -p ~/{out}/exact ~/{out}/try")
    home = service_home(args.host)
    every = max(1, span // 24)
    started = time.monotonic()
    draw_once(args.host, args.build, folder, {"cmd": "rt_frames", "n": first, "count": span, "warm": -1,
                                               "last": 1, "every": every}, env=args.change, out=f"{out}/exact")
    exact_seconds = time.monotonic() - started
    service = open_service(args.host, args.build, folder)
    try:
        service.ask({"cmd": "rt_checkpoint", "n": first - 1})
        service.ask({"cmd": "rt_reload", "env": change})
        started = time.monotonic()
        service.ask({"cmd": "rt_frames", "n": first, "count": span, "last": 1, "every": every, "checkpoint": True,
                     "dir": f"{home}/{out}/try"})
        try_seconds = time.monotonic() - started
    finally:
        service.close()
    for side in ("exact", "try"):
        frames_named(args.host, f"{out}/{side}")
    _, lines = compare(args.host, replay, f"{out}/try", f"{out}/exact")
    # (the change's own effect: its exact frame against the build's, so that a change that does nothing shows)
    ssh(args.host, f"mkdir -p ~/{out}/own && cp ~/{out}/exact/frame_{last:05d}.png ~/{out}/own/")
    _, own = compare(args.host, replay, f"{out}/own", f"{folder}/verify/exact1")
    say(f"rt2_bench: REACH of {change} from a checkpoint (the build's code before it): the try against the "
        f"change's exact frames (exact {exact_seconds:.1f} s, the try's {span} frames {try_seconds:.1f} s); the "
        f"change itself at frame {last}: {own[0] if own else '?'}")
    for line in lines:
        match = re.match(r"frame_(\d+)\.png: \w+ \((\d+) of (\d+) pixels differ, by at most (\d+)\)", line)
        if match:
            n, differ, pixels, most = (int(g) for g in match.groups())
            say(f"    k {n - first + 1:4d} (frame {n}): {100.0 * differ / pixels:6.2f}% of pixels differ, "
                f"at most {most}")


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


def edited_kernels(args):
    """view/reference/terms/triage --kernels: the working tree's kernels as run sends them (compiled here, only those
    whose sources changed since the last time: build/rt2_bench/kernels), in a rig folder named by their contents
    (~/goat-rig/rt2-bench/spv/<hash>, kept for the cache's key and reused); the setting that points the replay
    there"""
    if not getattr(args, "kernels", False):
        return []
    local = ROOT / "build" / "rt2_bench" / "kernels"
    local.mkdir(parents=True, exist_ok=True)
    kept = local / "keys.json"
    kernels = Kernels(local)
    try:
        kernels.keys = json.loads(kept.read_text())
    except (OSError, ValueError):
        pass
    started = time.monotonic()
    changed = kernels.compile()
    kept.write_text(json.dumps(kernels.keys))
    names = sorted(f"{vulkan_lt_shaders.kernel_name(source, defines)}.spv"
                   for _, source, defines in vulkan_lt_shaders.KERNELS)
    digest = hashlib.sha256()
    for name in names:
        digest.update(name.encode() + b"\0" + (local / name).read_bytes())
    remote = f"{REMOTE}/spv/{digest.hexdigest()[:16]}"
    if rt2_clips.windows(args.host):
        stream = subprocess.run(["tar", "--no-xattrs", "--no-mac-metadata", "-C", str(local), "-cf", "-", *names],
                                capture_output=True, check=True, env={**os.environ, "COPYFILE_DISABLE": "1"}).stdout
        subprocess.run(rt2_clips.remote_argv(args.host, f"mkdir -p ~/{remote} && tar -xf - -C ~/{remote}"),
                       check=True, capture_output=True, input=stream)
    else:
        ssh(args.host, f"mkdir -p ~/{remote}")
        subprocess.run(["rsync", "-a", *[str(local / n) for n in names], f"{rt2_clips.rig_target(args.host)}:{remote}/"],
                       check=True, capture_output=True)
    say(f"rt2_bench: the working tree's kernels ({len(changed)} compiled, {time.monotonic() - started:.1f} s): "
        f"{args.host}:~/{remote}")
    return [f"HALO_LT_SHADER_DIR={service_home(args.host)}/{remote}"]


UNIT_SOURCES = [ROOT / "port/linux/game/lt_lights_build.c", ROOT / "port/linux/game/lt_lights.h",
                ROOT / "port/linux/game/lt_lights_inputs.h"]

# on the rig: the working tree's lt_lights_build.c compiled as the build's linux_lt_unit compiles it (its commands,
# ninja -t commands), linked with the build's musl objects, into the folder the sources came to
UNIT_COMPILE = r"""
set -e
tree=~/goat-rig/builds/$1; out=~/$2
cd "$tree"
[ -f build/linux_x86_64/lt_lights_unit.so ] || { echo "no lt_lights_unit.so in $1 (goat-rig build <ref> linux_lt_unit)" >&2; exit 5; }
ninja -t commands build/linux_x86_64/lt_lights_unit.so | grep -E 'lt_lights_build\.c\.o$|-shared .*lt_lights_unit\.so' |
  sed -e "s#port/linux/game/lt_lights_build.c#$out/lt_lights_build.c#" \
      -e "s#-MMD -MF [^ ]*##" \
      -e "s#-o build/linux_x86_64/lt_unit/lt_lights_build.c.o#-o $out/lt_lights_build.c.o#" \
      -e "s#-o build/linux_x86_64/lt_lights_unit.so#-o $out/lt_lights_unit.so.part#" \
      -e "s#build/linux_x86_64/lt_unit/lt_lights_build.c.o#$out/lt_lights_build.c.o#" > "$out/commands.sh"
bash "$out/commands.sh"
mv "$out/lt_lights_unit.so.part" "$out/lt_lights_unit.so"
"""


def unit_env(args):
    """--unit: the light list built again in the replay from the recording's inputs by the working tree's
    lt_lights_build.c (compiled on the rig in about a second, into ~/goat-rig/rt2-bench/unit/<its sources' hash>,
    reused while they are the same); the setting that loads it. --unit built: the build's own lt_lights_unit.so"""
    unit = getattr(args, "unit", None)
    if not unit:
        return []
    if rt2_clips.windows(args.host):
        raise SystemExit("rt2_bench: --unit runs on omarchy or the 5900X (the replay's unit is Linux's)")
    tree = rt2_scorecard.build_folder(args.build)
    if unit == "built":
        return [f"HALO_LT_UNIT={service_home(args.host)}/goat-rig/builds/{tree}/build/linux_x86_64/lt_lights_unit.so"]
    digest = hashlib.sha256(tree.encode())
    for path in UNIT_SOURCES:
        digest.update(path.name.encode() + b"\0" + path.read_bytes())
    remote = f"{REMOTE}/unit/{digest.hexdigest()[:16]}"
    started = time.monotonic()
    if ssh(args.host, f"test -f ~/{remote}/lt_lights_unit.so && echo kept", check=False).strip() != "kept":
        ssh(args.host, f"mkdir -p ~/{remote}")
        subprocess.run(["rsync", "-a", *[str(p) for p in UNIT_SOURCES], f"{rt2_clips.rig_target(args.host)}:{remote}/"],
                       check=True, capture_output=True)
        result = subprocess.run(rt2_scorecard.ssh(args.host, "bash -s " + tree + " " + remote), input=UNIT_COMPILE,
                                text=True, capture_output=True)
        if result.returncode:
            raise SystemExit(f"rt2_bench: lt_lights_build.c does not compile:\n{(result.stderr or result.stdout)[-2000:]}")
        say(f"rt2_bench: the working tree's light-list unit built on {args.host} in {time.monotonic() - started:.1f} s")
    return [f"HALO_LT_UNIT={service_home(args.host)}/{remote}/lt_lights_unit.so"]


def unit_line(info):
    """the replay's light-list unit's counts (info's lt_unit)"""
    unit = (info or {}).get("lt_unit")
    if not unit:
        return None
    if not unit.get("loaded"):
        return f"the light-list unit {unit.get('path')} NOT loaded"
    note = (f"{unit['differ']} other (the first at list {unit['first_differ']})" if unit["differ"] else
            "every one the same as recorded")
    return (f"light lists built again by the unit: {unit['rebuilt']} ({note}); {unit['as_recorded']} as recorded "
            "(no inputs recorded with them)")


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
    checkpointed = args.warm is None and not args.exact
    warm = -1 if args.exact or args.warm is None else args.warm
    request = {"cmd": "rt_frames", "n": frame, "warm": warm, "last": args.last, "inline": True,
               "compress": True}
    if checkpointed:
        request["checkpoint"] = True
        request.pop("warm")
    request.update({"hold": args.hold} if args.hold else {"count": args.count})
    if regions:
        request["regions"] = regions
    if args.reset:
        request["reset"] = True
    what = (f"frame {frame} held {args.hold} plays" if args.hold else
            f"frames {frame}-{frame + args.count - 1} as recorded")
    how = (f"from a checkpoint {args.checkpoint} frames before (one replay for every try)" if checkpointed else
           "every frame before played whole (exact, a replay a try)" if warm < 0 else
           f"the {warm} before played whole (a replay a try)")
    say(f"rt2_bench: {args.moment} ({meta['build']} {meta['sha']}): {what}; {how}; the last {args.last} measured; "
        f"replay {args.build}")
    scratch = tempfile.TemporaryDirectory()
    kernels = Kernels(scratch.name)
    base = None
    version = 0
    pictures = []

    def stamp():
        paths = [p for p in vulkan_lt_shaders.FOLDER.glob("*.*") if p.suffix != ".inc"]
        if args.settings:
            paths.append(Path(args.settings))
        if args.unit == "tree":
            paths += UNIT_SOURCES
        return tuple(sorted((str(p), p.stat().st_mtime_ns) for p in paths if p.exists()))

    service = None
    applied = {}
    try:
        ssh(args.host, f"mkdir -p ~/{remote}/k")
        if checkpointed:
            checkpoint_at = frame - args.checkpoint - 1
            if checkpoint_at < 0:
                raise SystemExit("rt2_bench: --checkpoint reaches before the recording's start")
            t0 = time.monotonic()
            service = open_service(args.host, args.build, folder)
            if "rt_checkpoint" not in service.ask({"cmd": "info"}).get("capabilities", []):
                raise SystemExit("rt2_bench: this replay has no rt_checkpoint (build it from a branch with it), "
                                 "or run with --exact")
            service.ask({"cmd": "rt_checkpoint", "n": checkpoint_at})
            say(f"rt2_bench: the checkpoint kept at frame {checkpoint_at} (an exact play, the build's own kernels) "
                f"in {time.monotonic() - t0:.1f} s")
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
                if args.unit == "tree":
                    try:
                        args.set = [e for e in args.set if not e.startswith("HALO_LT_UNIT=")] + unit_env(args)
                    except SystemExit as error:
                        say(f"v{version}: {error}; the last version kept")
                        if not args.watch:
                            return 1
                        time.sleep(0.25)
                        continue
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
                if service:
                    # (the kernels and settings read again by the one replay, its frames from the checkpoint)
                    given = dict(settings)
                    words = [f"{k}={v}" for k, v in given.items()] + [f"-{k}" for k in applied if k not in given]
                    if not args.built_in:
                        words.append(f"HALO_LT_SHADER_DIR={home}/{remote}/k")
                    service.ask({"cmd": "rt_reload", "env": " ".join(words),
                                 **({} if args.built_in else {"dir": f"{home}/{remote}/k"})})
                    applied = given
                    reply = service.ask(request)
                    if "HALO_LT_UNIT" in given:
                        reply["lt_unit"] = service.ask({"cmd": "info"}).get("lt_unit")
                else:
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
                if unit_line(reply):
                    say("    " + unit_line(reply))
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
        if service:
            service.close()
        ssh(args.host, f"rm -rf ~/{remote}", check=False)
        scratch.cleanup()


# ---------- view: the shortcuts' internals

VIEW_SHARES = ("found", "disoccluded", "gradient_cut", "sample_cut", "bounce_hold", "moving", "relit")
VIEW_MEANS = ("count", "found", "cut_share", "cut", "z_factor", "grad_before", "grad_now", "grad_change", "lit_before",
              "lit_now", "lit_change", "relit")


def view_lines(box, label):
    mean, share = box["mean"], box["share"]

    def number(value):
        return "-" if value is None else f"{value:.4g}"

    return [f"{label}: {box['drawn']} of {box['pixels']} px drawn",
            "    share: " + ", ".join(f"{k} {share[k] * 100:.1f}%" for k in VIEW_SHARES),
            "    mean:  " + ", ".join(f"{k} {number(mean.get(k))}" for k in VIEW_MEANS)]


def view(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    frame = args.frame if args.frame is not None else meta["frame"]
    if args.count and not args.hold and args.frame is None:
        frame = max(0, meta["frame"] - args.count + 1)
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    regions = [float(v) for r in args.region for v in r.split(",")]
    shows = args.show or "cut_share,history,disocclusion,cut"
    request = {"cmd": "rt_view", "view": "denoise", "show": shows, "n": frame, "warm": args.warm, "last": 1,
               "inline": True, "compress": True, "scale": args.scale}
    request.update({"hold": args.hold} if args.hold else {"count": args.count})
    if regions:
        request["regions"] = regions
    env = [f"{k}={v}" for k, v in settings_from(args.settings, args.set).items()] + ["HALO_LT_DENOISE_PROBE=1"]
    started = time.monotonic()
    service = open_service(args.host, args.build, folder, env)
    try:
        if "rt_view" not in service.ask({"cmd": "info"}).get("capabilities", []):
            raise SystemExit("rt2_bench: this replay has no rt_view (build it from a branch with the views)")
        reply = service.ask(request)
    finally:
        service.close()
    if not reply.get("ok"):
        raise SystemExit(f"rt2_bench: {reply.get('error', reply)}")
    seconds = time.monotonic() - started
    last = reply["pictures"][-1]["frame"]
    (out / "view_frame.png").write_bytes(base64.b64decode(reply["pictures"][-1]["png_base64"]))
    say(f"rt2_bench: the denoiser at frame {last} ({'held ' + str(args.hold) if args.hold else 'as recorded'}; "
        f"{'exact' if args.warm < 0 and reply.get('exact') else 'warm ' + str(args.warm)}) in {seconds:.1f} s")
    for picture in reply["view"]["pictures"]:
        path = out / f"view_{picture['show']}.png"
        path.write_bytes(base64.b64decode(picture["png_base64"]))
        say(f"    {picture['show']}: {path} (white = {picture['scale']:.4g})")
    say(f"    the frame itself: {out / 'view_frame.png'}")
    for line in view_lines(reply["view"]["whole"], "whole picture"):
        say(line)
    for i, box in enumerate(reply["view"]["regions"]):
        for line in view_lines(box, f"region {i + 1} {','.join(f'{v:g}' for v in box['box'])}"):
            say(line)
    if args.json:
        with open(out / "view.json", "w") as f:
            json.dump({"frame": last, "view": {k: v for k, v in reply["view"].items() if k != "pictures"},
                       "seconds": seconds}, f, indent=1)
    return 0

# ---------- the results' cache: a replay's accumulated views and replies kept by everything they depend on

# What a replay's result depends on, each part hashed on the rig (FINGERPRINT): the recording (moment.hrpl's
# bytes: every record the game sent, the lights' tables and their inputs among them), the light pass's code (the
# replay's binary: the backend's C and the kernels' SPIR-V built into it, with only the two parts that name the
# build rather than its code blanked: the commit label HALO_REPLAY_BUILD and the linker's build id), any kernels or
# light-list unit the settings point the replay to (HALO_LT_SHADER_DIR's files, HALO_LT_UNIT's), and the GPU and
# its driver; then the settings and the request themselves (job_key). The replay draws the same frame bit for bit
# for the same inputs (verify's DETERMINISM), so a result kept under that key is the result a replay would give.
# CACHE_VERSION: what the bench asks of a result (its files' layout); raised when that changes.
CACHE = "cache"  # (beside the recording: ~/goat-rig/rt2-bench/<name>/cache/<key>)
CACHE_GB = float(os.environ.get("RT2_BENCH_CACHE_GB", "20"))  # (each rig's whole cache, oldest used dropped first)
CACHE_FREE_GB = 60  # (never cached when the rig's disk would have less free)

from rt2_bench_cache import CACHE_VERSION, FINGERPRINT_LIB, job_key  # noqa: E402,F401

FINGERPRINT = FINGERPRINT_LIB + r"""
job = json.loads(sys.argv[1])
home = os.path.expanduser("~")
tree = os.path.join(home, job["replay"]).split("/build/")[0]
commits = subprocess.run(["git", "-C", tree, "rev-list", "--max-count=2000", "HEAD"], capture_output=True,
                         text=True).stdout.split()
gpu = subprocess.run("nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null || "
                     "(pacman -Q mesa vulkan-radeon 2>/dev/null; grep -h PCI_ID /sys/class/drm/card*/device/uevent "
                     "2>/dev/null | sort -u)", shell=True, capture_output=True, text=True).stdout.strip()
print(json.dumps(fingerprint(job, home, commits, gpu + " / " + os.uname().release)))
"""

# the settings that point the replay to files: their contents are part of the key
FILE_SETTINGS = ("HALO_LT_SHADER_DIR", "HALO_LT_UNIT")


class Cache:
    """the rig's kept results for one build's replay (lookups and saves by job_key); off with --no-cache"""

    def __init__(self, args, folders, env_paths=()):
        self.host = args.host
        self.on = not getattr(args, "no_cache", False) and not rt2_clips.windows(args.host)
        self.hits, self.misses = [], []
        self.fingerprints = {}
        if not self.on:
            return
        paths = {}
        for setting in env_paths:
            key, _, value = setting.partition("=")
            if key in FILE_SETTINGS:
                paths[key] = value
        job = {"folders": list(folders), "replay": replay_path(args.host, args.build), "paths": paths}
        started = time.monotonic()
        found = json.loads(ssh(args.host, f"python3 - {shlex.quote(json.dumps(job))} <<'PY'\n{FINGERPRINT}\nPY\n")
                           .strip().splitlines()[-1])
        self.seconds = time.monotonic() - started
        for folder in folders:
            self.fingerprints[folder] = {"version": CACHE_VERSION, "recording": found["recordings"][folder],
                                         "code": found["code"], "files": found["files"], "gpu": found["gpu"]}

    def key(self, folder, env, request):
        return job_key(self.fingerprints[folder], env, request)

    def place(self, folder, key):
        return f"{folder}/{CACHE}/{key}"

    def look(self, items):
        """the kept entries of these (folder, key)s read in one go (each one's use time renewed)"""
        if not self.on or not items:
            return
        script = "".join(f"e=~/{self.place(folder, key)}/entry.json; if [ -f $e ]; then touch $e; "
                         f"echo \"{key} $(base64 -w0 $e)\"; fi\n" for folder, key in items)
        self.known = getattr(self, "known", {})
        for folder, key in items:
            self.known.setdefault(key, None)
        for line in ssh(self.host, script, check=False).splitlines():
            key, _, data = line.partition(" ")
            if data:
                self.known[key] = json.loads(base64.b64decode(data))

    def get(self, folder, key, kind):
        """the kept result (the f32 file's rig path for "f32", else the reply), or None"""
        if not self.on:
            return None
        if key not in getattr(self, "known", {}):
            self.look([(folder, key)])
        entry = self.known.get(key)
        place = self.place(folder, key)
        if not entry:
            self.misses.append(key)
            return None
        self.hits.append(key)
        return f"{service_home(self.host)}/{place}/{entry['f32']}" if kind == "f32" else entry["reply"]

    def put(self, folder, key, save, reply, f32=None, inputs=None):
        """a fresh result kept: the save folder (an f32 accumulation's) moved under the key, or the reply alone"""
        if not self.on:
            return f32
        place = self.place(folder, key)
        entry = {"key": key, "inputs": inputs, "reply": reply, "made": time.strftime("%Y-%m-%d %H:%M:%S")}
        if f32:
            entry["f32"] = os.path.basename(f32)
        free = f"$(( $(df -Pk ~ | awk 'NR==2 {{print $4}}') / 1048576 ))"
        script = (f"set -e; if [ {free} -lt {CACHE_FREE_GB + 1} ]; then echo full; exit 0; fi\n"
                  f"mkdir -p ~/{folder}/{CACHE}; rm -rf ~/{place}.part ~/{place}\n" +
                  (f"mv ~/{save} ~/{place}.part\n" if f32 else f"mkdir -p ~/{place}.part\n") +
                  f"cat > ~/{place}.part/entry.json <<'EOF'\n{json.dumps(entry)}\nEOF\n"
                  f"mv ~/{place}.part ~/{place}; echo kept")
        if ssh(self.host, script).strip() != "kept":
            say(f"rt2_bench: the rig has under {CACHE_FREE_GB} GB free: not cached")
            return f32
        return f"{service_home(self.host)}/{place}/{entry['f32']}" if f32 else None

    def trim(self):
        """the rig's whole cache (every recording's) kept under CACHE_GB, the least recently used dropped first"""
        if not self.on or not self.misses:
            return
        script = (f"cd ~/{REMOTE} 2>/dev/null || exit 0\n"
                  f"ls -d */{CACHE}/*/ 2>/dev/null | grep -v '\\.part/$' | while read d; do "
                  "echo \"$(stat -c %Y \"$d/entry.json\" 2>/dev/null || echo 0) $(du -sk \"$d\" | cut -f1) $d\"; "
                  f"done | sort -rn | awk -v cap={int(CACHE_GB * 1048576)} '{{t += $2; if (t > cap) print $3}}' | "
                  "xargs -r rm -rf")
        ssh(self.host, script, check=False)

    def summary(self):
        if not self.on:
            return "the cache off"
        return (f"cache: {len(self.hits)} kept, {len(self.misses)} drawn (the inputs hashed in "
                f"{self.seconds:.1f} s)")


# ---------- reference, terms, triage: the shortcuts against the converged picture, the light by term

# the real-time shortcuts the reference turns off (the same kernels, the same frame; docs: tools/rig/README.md, recipe 12)
REFERENCE_ENV = ["HALO_LT_REUSE=0",           # no ReSTIR reuse: RIS alone each frame, no reservoirs kept (the lights,
                                              # the level's lamps)
                 "HALO_LT_INDIRECT=nocache"]  # the bounce traced: no radiance-cache lookups at its hits
# what each term keeps (the rest masked: HALO_LT_KIND_SCALE's table kinds, HALO_LT_MASK's lamps, HALO_LT_DROP's
# flashlight); every direct term with the bounce off, the bounce the whole less the direct
_NONE = "HALO_LT_KIND_SCALE=point=0,spot=0,panel=0,sun=0,fill=0"
TERMS = {
    "all": [],
    "direct": ["HALO_LT_INDIRECT=0"],
    "sun": ["HALO_LT_INDIRECT=0", "HALO_LT_KIND_SCALE=point=0,spot=0,panel=0,fill=0", "HALO_LT_MASK=emitters,objects",
            "HALO_LT_DROP=flashlight"],
    "sky": ["HALO_LT_INDIRECT=0", "HALO_LT_KIND_SCALE=point=0,spot=0,panel=0,sun=0", "HALO_LT_MASK=emitters,objects",
            "HALO_LT_DROP=flashlight"],
    "lights": ["HALO_LT_INDIRECT=0", "HALO_LT_KIND_SCALE=sun=0,fill=0", "HALO_LT_MASK=emitters,objects",
               "HALO_LT_DROP=flashlight"],
    "flashlight": ["HALO_LT_INDIRECT=0", _NONE, "HALO_LT_MASK=emitters,objects"],
    "solved_lamps": ["HALO_LT_INDIRECT=0", _NONE, "HALO_LT_MASK=objects", "HALO_LT_DROP=flashlight"],
    "object_lamps": ["HALO_LT_INDIRECT=0", _NONE, "HALO_LT_MASK=emitters", "HALO_LT_DROP=flashlight"],
    # (every source masked: what no mask reaches, each term's run less it)
    "none": ["HALO_LT_INDIRECT=0", _NONE, "HALO_LT_MASK=emitters,objects", "HALO_LT_DROP=flashlight"],
}
TERM_ORDER = ["sun", "sky", "lights", "flashlight", "solved_lamps", "object_lamps", "bounce", "unmasked"]
TERM_RUNS = ["sun", "sky", "lights", "flashlight", "solved_lamps", "object_lamps", "none", "all", "direct"]

# run on the rig (numpy there): the saved views' numbers by region, and small pictures
ANALYSE = r"""
import json, sys, zlib, struct
import numpy as np
job = json.loads(sys.argv[1])
W, H = job["width"], job["height"]
def load(path):
    a = np.fromfile(path, dtype=np.float32).reshape(H, W, 13)
    return a
def png(path, grey):
    g = np.clip(grey, 0, 255).astype(np.uint8)
    raw = b"".join(b"\x00" + bytes(row) for row in g)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0)) +
                          chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
images = {k: load(v) for k, v in job["files"].items()}
boxes = [[0, 0, W, H]] + job["regions"]
out = {"regions": []}
for box in boxes:
    x, y, w, h = [int(v) for v in box]
    r = {"box": box}
    for k, a in images.items():
        sub = a[y:y + h, x:x + w]
        batches = np.nanmean(sub[:, :, 5:].reshape(-1, 8), axis=0)
        good = batches[np.isfinite(batches)]
        r[k] = {"mean": float(sub[:, :, 3].mean()), "rgb": [float(sub[:, :, c].mean()) for c in range(3)],
                "noise": float(sub[:, :, 4].mean()),
                "stderr": float(good.std(ddof=1) / np.sqrt(len(good))) if len(good) > 1 else None}
    out["regions"].append(r)
def tone(l, white):
    return 255.0 * np.sqrt(np.clip(l / white, 0, 1))
pics = job.get("pictures", {})
for name, spec in pics.items():
    kind = spec[0]
    if kind == "light":
        l = images[spec[1]][:, :, 3]
        png(f"{job['dir']}/{name}.png", tone(l, spec[2]))
    elif kind == "error":
        a, b = images[spec[1]][:, :, 3], images[spec[2]][:, :, 3]
        rel = (a - b) / np.maximum(b, spec[3])
        png(f"{job['dir']}/{name}.png", 128.0 + 127.0 * np.clip(rel, -1, 1))
print(json.dumps(out))
"""


PROBE = "HALO_LT_DENOISE_PROBE=1"  # (the views' passes: only the workbench sets it)


def accumulate(args, folder, env, request, save):
    """one replay of its own: rt_view's accumulation of the views into the rig's save folder; the reply"""
    home = service_home(args.host)
    ssh(args.host, f"mkdir -p ~/{save}")
    service = open_service(args.host, args.build, folder, list(env) + [PROBE])
    try:
        if "rt_view" not in service.ask({"cmd": "info"}).get("capabilities", []):
            raise SystemExit("rt2_bench: this replay has no rt_view (build it from a branch with the views)")
        return service.ask({**request, "cmd": "rt_view", "save": f"{home}/{save}", "inline": True, "compress": True})
    finally:
        service.close()


def analyse(args, files, regions, pictures, save, out):
    """the saved views' numbers on the rig (whole picture first, then each region), their pictures copied to out"""
    job = {"width": 1280, "height": 720, "files": files, "regions": regions, "pictures": pictures,
           "dir": f"{service_home(args.host)}/{save}"}
    w, h = (int(v) for v in SIZE.split("x"))
    job["width"], job["height"] = w, h
    text = ssh(args.host, f"mkdir -p ~/{save} && python3 - {shlex.quote(json.dumps(job))} <<'PY'\n{ANALYSE}\nPY\n")
    result = json.loads(text.strip().splitlines()[-1])
    if pictures:
        out.mkdir(parents=True, exist_ok=True)
        subprocess.run(["scp", "-q", *[f"{rt2_clips.rig_target(args.host)}:{save}/{name}.png" for name in pictures],
                       str(out)], check=False)
    return result


def linux_only(args):
    """reference, terms and triage: their numbers worked out by numpy on the rig, their pictures copied with scp"""
    if rt2_clips.windows(args.host):
        raise SystemExit(f"rt2_bench: {args.what} runs on omarchy or the 5900X (numpy on the rig); view runs on "
                         f"{args.host} too")


def regions_of(args):
    linux_only(args)
    regions = [[float(v) for v in r.split(",")] for r in args.region]
    if any(len(r) != 4 for r in regions):
        raise SystemExit("rt2_bench: --region x,y,w,h")
    return regions


def realtime_and_reference(args, folder, meta, save, terms=(), cache=None):
    """the game's light at the moment's frame (as recorded, exact), the same frame held (its noise and its mean), the
    reference (the shortcuts off, held, accumulated), and each term's reference: their saved files and seconds"""
    frame = args.frame if args.frame is not None else meta["frame"]
    held = args.hold or 120
    plays = args.plays
    skip = args.skip
    jobs = {
        "realtime": ([*args.set], {"n": frame, "count": 1, "warm": -1, "last": 1, "accumulate": "light"}),
        "held": ([*args.set], {"n": frame, "hold": held, "warm": -1, "last": held - 20, "accumulate": "light"}),
        "reference": ([*args.set, *REFERENCE_ENV], {"n": frame, "hold": plays + skip, "warm": -1, "last": plays,
                                                    "accumulate": "sample"}),
    }
    for term in terms:
        if term in TERMS:
            jobs[f"term_{term}"] = ([*args.set, *REFERENCE_ENV, *TERMS[term]],
                                    {"n": frame, "hold": args.term_plays + skip, "warm": -1,
                                     "last": args.term_plays, "accumulate": "sample"})
    files, seconds = {}, {}
    cache = cache or Cache(args, [folder], args.set)
    keys = {name: cache.key(folder, [*env, PROBE], request) for name, (env, request) in jobs.items()}
    cache.look([(folder, key) for key in keys.values()])
    for name, key in keys.items():
        path = cache.get(folder, key, "f32")
        if path:
            files[name] = path
            seconds[name] = 0.0
    wanted = {name: job for name, job in jobs.items() if name not in files}

    def one(item):
        name, (env, request) = item
        started = time.monotonic()
        reply = accumulate(args, folder, env, request, f"{save}/{name}")
        if not reply.get("ok"):
            raise SystemExit(f"rt2_bench: {name}: {reply.get('error')}")
        view = reply["accumulated"][0]
        path = cache.put(folder, keys[name], f"{save}/{name}", {k: v for k, v in reply.items() if k != "accumulated"},
                         f32=view["f32"], inputs={"env": sorted([*env, PROBE]), "request": request})
        return name, path, time.monotonic() - started

    if wanted:
        owner_check(args.host)
    # (three replays at once share the GPU: goat-rig run --shared's limit; the longest first)
    order = sorted(wanted.items(), key=lambda item: -item[1][1].get("hold", 1))
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for name, path, took in pool.map(one, order):
            files[name] = path
            seconds[name] = took
    cache.trim()
    return files, seconds, frame


def number(value, digits=4):
    return "-" if value is None else f"{value:.{digits}g}"


def reference(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    save = f"goat-rig/rt2-bench-views/{name_of(args)}-{os.getpid()}"
    out = Path(args.out).expanduser()
    regions = regions_of(args)
    started = time.monotonic()
    try:
        cache = Cache(args, [folder], args.set)
        files, seconds, frame = realtime_and_reference(args, folder, meta, save, cache=cache)
        white = args.scale or 0.05
        pictures = {"reference": ["light", "reference", white], "realtime": ["light", "realtime", white],
                    "error": ["error", "realtime", "reference", white * 0.02],
                    "bias": ["error", "held", "reference", white * 0.02]}
        result = analyse(args, files, regions, pictures, save, out)
    finally:
        ssh(args.host, f"rm -rf ~/{save}", check=False)
    say(f"rt2_bench: reference at frame {frame}: {args.plays} plays (after {args.skip}) with "
        f"{' '.join(REFERENCE_ENV)}, the denoiser bypassed (its input sample accumulated); total "
        f"{time.monotonic() - started:.0f} s ({', '.join(f'{k} {v:.0f} s' for k, v in seconds.items())}; "
        f"{cache.summary()})")
    for name in pictures:
        say(f"    {name}: {out / (name + '.png')}")
    for i, r in enumerate(result["regions"]):
        say(reference_line(r, "whole picture" if i == 0 else f"region {i} {','.join(f'{v:g}' for v in r['box'])}"))
    if args.json:
        (out / "reference.json").write_text(json.dumps({"frame": frame, "seconds": seconds, **result}, indent=1))
    return 0


def reference_line(r, label):
    ref, rt, held = r["reference"], r["realtime"], r["held"]
    base = ref["mean"] or 1e-12
    converged = ref["stderr"] is not None and ref["stderr"] <= 0.01 * base
    return (f"{label}:\n    reference {number(ref['mean'])} +- {number(ref['stderr'], 2)} "
            f"({'converged' if converged else 'NOT converged'}: the mean's error from 8 batches, <= 1% wanted)\n"
            f"    realtime {number(rt['mean'])}: error {(rt['mean'] / base - 1) * 100:+.1f}% = bias "
            f"{(held['mean'] / base - 1) * 100:+.1f}% (the held frame's mean against the reference) + motion "
            f"{(rt['mean'] - held['mean']) / base * 100:+.1f}% (as recorded against held); noise "
            f"{held['noise'] / base * 100:.1f}% (each pixel's deviation over the held plays)")


def terms(args):
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    save = f"goat-rig/rt2-bench-views/{name_of(args)}-{os.getpid()}"
    out = Path(args.out).expanduser()
    regions = regions_of(args)
    started = time.monotonic()
    try:
        cache = Cache(args, [folder], args.set)
        files, seconds, frame = realtime_and_reference(args, folder, meta, save, TERM_RUNS, cache)
        white = args.scale or 0.05
        pictures = {f"term_{t}": ["light", f"term_{t}", white] for t in TERM_RUNS}
        result = analyse(args, files, regions, pictures, save, out)
    finally:
        ssh(args.host, f"rm -rf ~/{save}", check=False)
    say(f"rt2_bench: light by term at frame {frame} (each the reference's, {args.term_plays} plays, the others "
        f"masked, less the run with every source masked; bounce = all less direct, the sky's dome through the bounce included; unmasked = what no mask reaches); "
        f"{time.monotonic() - started:.0f} s; {cache.summary()}")
    for i, r in enumerate(result["regions"]):
        say(terms_line(r, "whole picture" if i == 0 else f"region {i} {','.join(f'{v:g}' for v in r['box'])}"))
    for name in pictures:
        say(f"    {name}: {out / (name + '.png')}")
    if args.json:
        (out / "terms.json").write_text(json.dumps({"frame": frame, "seconds": seconds, **result}, indent=1))
    return 0


def term_means(r):
    """each source's light: its run (the others masked, no bounce) less the run with every source masked; the
    bounce, the whole less the direct; unmasked, what no mask reaches (the run with every source masked)"""
    none = r["term_none"]["mean"]
    means = {t: r[f"term_{t}"]["mean"] - none for t in TERM_ORDER if t in TERMS}
    means["bounce"] = r["term_all"]["mean"] - r["term_direct"]["mean"]
    means["unmasked"] = none
    return means


def terms_line(r, label):
    means = term_means(r)
    total = r["term_all"]["mean"] or 1e-12
    parts = sorted(means.items(), key=lambda kv: -abs(kv[1]))
    summed = sum(means.values())
    return (f"{label}: all {number(total)} (the terms sum to {summed / total * 100:.0f}%)\n    " +
            ", ".join(f"{k} {number(v)} ({v / total * 100:.0f}%)" for k, v in parts))


def triage(args):
    """the first step: estimator (realtime against reference) or data/look (reference against classic)?"""
    folder = f"{REMOTE}/{name_of(args)}"
    meta = meta_of(args.host, folder)
    save = f"goat-rig/rt2-bench-views/{name_of(args)}-{os.getpid()}"
    out = Path(args.out).expanduser()
    regions = regions_of(args)
    if not regions:
        raise SystemExit("rt2_bench: triage --region x,y,w,h (one or more)")
    started = time.monotonic()
    frame = args.frame if args.frame is not None else meta["frame"]
    view_reply = {}
    classic_folder = f"{REMOTE}/{args.classic}" if args.classic else None
    cache = Cache(args, [folder] + ([classic_folder] if classic_folder else []), args.set)

    def kept(where, env, request, draw):
        """a replay's reply, from the cache or drawn (and kept)"""
        key = cache.key(where, env, request)
        reply = cache.get(where, key, "reply")
        if reply is None:
            reply = draw()
            cache.put(where, key, None, reply, inputs={"env": sorted(env), "request": request})
        return reply

    def denoiser():
        env = [*args.set, PROBE]
        request = {"cmd": "rt_view", "view": "denoise", "show": "cut_share", "n": frame, "count": 1, "warm": -1,
                   "last": 1, "regions": sum(regions, []), "inline": True, "compress": True}

        def draw():
            service = open_service(args.host, args.build, folder, env)
            try:
                reply = service.ask(request)
            finally:
                service.close()
            # (the numbers kept, not the pictures)
            return {"view": {k: v for k, v in reply["view"].items() if k != "pictures"}}

        view_reply["denoiser"] = kept(folder, env, request, draw)

    def presented(where, env, n=None):
        request = {"cmd": "rt_frames", "n": frame if n is None else n, "count": 1, "warm": -1, "last": 1, "inline": True,
                   "compress": True, "regions": sum(regions, [])}

        def draw():
            reply = draw_once(args.host, args.build, where, request, env)
            return {"stats": reply["stats"]}

        return kept(where, env, request, draw)["stats"]["regions"]

    def side():
        """the exact frames beside the accumulations: the denoiser's view, the classic and the game's pictures"""
        took = {}
        t = time.monotonic()
        denoiser()
        took["denoiser"] = time.monotonic() - t
        if args.classic:
            t = time.monotonic()
            # (the classic recording's frame at the same game second: both end at the moment's --at, 1/60 s a
            # frame, but the setting's loading draws a different number of frames first, so counted from the end)
            view_reply["classic_frame"] = frame - (meta["frames"] - meta_of(args.host, classic_folder)["frames"])
            view_reply["classic"] = presented(classic_folder, [], view_reply["classic_frame"])
            view_reply["game"] = presented(folder, list(args.set))
            took["classic"] = time.monotonic() - t
        return took

    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as aside:
            beside = aside.submit(side)
            files, seconds, _ = realtime_and_reference(args, folder, meta, save, TERM_RUNS, cache)
            seconds.update(beside.result())
        classic, game = view_reply.get("classic"), view_reply.get("game")
        classic_frame = view_reply.get("classic_frame")
        white = args.scale or 0.05
        pictures = {"reference": ["light", "reference", white], "error": ["error", "realtime", "reference",
                                                                          white * 0.02]}
        result = analyse(args, files, regions, pictures, save, out)
    finally:
        ssh(args.host, f"rm -rf ~/{save}", check=False)
    say(f"rt2_bench: TRIAGE at frame {frame} ({time.monotonic() - started:.0f} s: "
        f"{', '.join(f'{k} {v:.0f} s' for k, v in seconds.items())}; {cache.summary()})")
    report = []
    for i, r in enumerate(result["regions"][1:]):
        ref, rt = r["reference"]["mean"] or 1e-12, r["realtime"]["mean"]
        held = r["held"]
        means = term_means(r)
        total = r["term_all"]["mean"] or 1e-12
        top = sorted(means.items(), key=lambda kv: -abs(kv[1]))[:3]
        d = view_reply["denoiser"]["view"]["regions"][i]
        estimator = rt / ref - 1
        lines = [f"region {i + 1} {','.join(f'{v:g}' for v in r['box'])}:",
                 f"    ESTIMATOR (realtime against reference): {estimator * 100:+.1f}% "
                 f"(bias {(held['mean'] / ref - 1) * 100:+.1f}%, motion {(rt - held['mean']) / ref * 100:+.1f}%, "
                 f"noise {held['noise'] / ref * 100:.1f}%; the reference +- {number(r['reference']['stderr'], 2)})",
                 "    the reference's light by term: " +
                 ", ".join(f"{k} {v / total * 100:.0f}%" for k, v in top),
                 f"    DENOISER: history cut {d['mean'].get('cut_share') or 0:.2f} of what was found "
                 f"(gradients cut {d['share']['gradient_cut'] * 100:.0f}% of pixels, the sample's test "
                 f"{d['share']['sample_cut'] * 100:.0f}%, disoccluded {d['share']['disoccluded'] * 100:.0f}%); "
                 f"history used {number(d['mean'].get('count'), 3)} frames"]
        data = None
        if classic:
            c, g = classic[i]["mean"], game[i]["mean"]
            # (the presented pictures, sRGB decoded: the game's with the setting on and off; the reference's own
            # presented picture estimated as the game's scaled by reference / realtime irradiance there; none
            # where either is black)
            data = (g * ref / rt) / c - 1 if rt > 0 and c > 0 else None
            lines.append(f"    DATA/LOOK (reference against classic, presented): "
                         f"{'-' if data is None else f'{data * 100:+.1f}%'}"
                         f" (the game's {'-' if c <= 0 else f'{(g / c - 1) * 100:+.1f}%'}; classic {c:.4f}, "
                         f"setting on {g:.4f})")
        verdict = ("estimator" if abs(estimator) > 0.1 else "data/look" if data is not None and
                   abs(data) > 0.1 else "neither beyond 10%")
        lines.append(f"    FIRST LOOK AT: {verdict}")
        report += lines
    for line in report:
        say(line)
    say(f"    pictures: {out / 'reference.png'}, {out / 'error.png'} (grey 128 = no error, white +100%)")
    if args.json:
        (out / "triage.json").write_text(json.dumps({"frame": frame, "seconds": seconds, **result,
                                                     "denoiser": view_reply["denoiser"]["view"]["regions"],
                                                     "classic": classic, "game": game if classic else None,
                                                     "classic_frame": classic_frame if classic else None,
                                                     "cache": {"kept": len(cache.hits), "drawn": len(cache.misses)}},
                                                    indent=1))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", choices=["record", "verify", "run", "view", "reference", "terms", "triage"])
    parser.add_argument("show", nargs="?", help="view: the pictures, a comma between (see above)")
    parser.add_argument("--moment", required=True, choices=list(rt2_clips.MOMENTS), help="tools/rt2_clips.py's")
    parser.add_argument("--build", required=True, help="the build (goat-rig build's ref: a branch, local/<name>)")
    parser.add_argument("--host", default="omarchy", choices=("omarchy", "5900x", *rt2_clips.WINDOWS_HOSTS),
                        help="the rig (bosgame: the Windows VM with the RTX 3090, through omarchy)")
    parser.add_argument("--name", help="the recording's folder ~/goat-rig/rt2-bench/<name> (default moment-build)")
    parser.add_argument("--at", type=float, help="record: the moment's second the recording ends at")
    parser.add_argument("--setting-off", action="store_true",
                        help="record: the setting off (the classic look; triage's --classic recording)")
    parser.add_argument("--seconds", type=float, default=420, help="record: the run's limit in real seconds")
    parser.add_argument("--pictures", type=int, default=3, help="verify: the game's pictures compared (the last)")
    parser.add_argument("--converge", default="30,120,300,600",
                        help="verify: the warms whose frame is compared with the exact one (the fast tries' error)")
    parser.add_argument("--change", action="append", default=[],
                        help="verify: VAR=value, a setting standing in for an edit: its reach from a checkpoint, by "
                             "the frames since (again)")
    parser.add_argument("--span", type=int, default=240, help="verify --change: the frames after the checkpoint")
    parser.add_argument("--frame", type=int, help="run: the frame (default the moment's, record's)")
    parser.add_argument("--count", type=int, default=30, help="run: the frames played as recorded, to the moment's")
    parser.add_argument("--hold", type=int, default=0, help="run: the moment's frame held this many plays instead")
    parser.add_argument("--reset", action="store_true", help="run --hold: the histories begun again (the scorecard's)")
    parser.add_argument("--checkpoint", type=int, default=CHECKPOINT_FRAMES,
                        help="run: the frames played whole with the new code before the measured ones, from the "
                             f"checkpoint (default {CHECKPOINT_FRAMES}; verify --change gives the error by k)")
    parser.add_argument("--exact", action="store_true",
                        help="run: each try a fresh replay, every frame before played whole (no checkpoint)")
    parser.add_argument("--warm", type=int, default=None,
                        help="run: each try a fresh replay, only the N frames before played whole (-1: every one, "
                             "as --exact); verify --converge gives its error")
    parser.add_argument("--last", type=int, default=20, help="run: the plays measured (the last)")
    parser.add_argument("--set", action="append", default=[], help="run: VAR=value, a light pass setting (again); record: the game's environment")
    parser.add_argument("--settings", help="run: a file of VAR=value lines, watched with --watch")
    parser.add_argument("--region", action="append", default=[], help="run: x,y,w,h, a region measured (again)")
    parser.add_argument("--built-in", action="store_true", help="run: the build's own kernels (no GLSL compiled)")
    parser.add_argument("--watch", action="store_true", help="run: again on each saved change, until Ctrl-C")
    parser.add_argument("--repeat", type=int, default=1, help="run: unchanged code drawn this many times, compared")
    parser.add_argument("--json", action="store_true", help="run: each version's numbers into OUT/versions.jsonl")
    parser.add_argument("--plays", type=int, default=2000, help="reference/triage: the reference's plays accumulated")
    parser.add_argument("--term-plays", type=int, default=600, help="terms/triage: each term's reference plays")
    parser.add_argument("--skip", type=int, default=200,
                        help="reference: the held plays first left out (the bounce's own history filling)")
    parser.add_argument("--classic", help="triage: the classic recording's name (record --setting-off --name NAME)")
    parser.add_argument("--scale", type=float, default=0, help="view: white's value (0: the 99th percentile)")
    parser.add_argument("--out", default=str(ROOT / "build" / "rt2_bench"), help="run: the pictures' folder")
    parser.add_argument("--kernels", action="store_true",
                        help="view/reference/terms/triage: the working tree's kernels (as run sends them), not the "
                             "build's own")
    parser.add_argument("--unit", nargs="?", const="tree", choices=("tree", "built"),
                        help="the light list built again from the recording's inputs by the working tree's "
                             "lt_lights_build.c (compiled on the rig; run --watch: again on each saved change), or "
                             "the build's own (--unit built); a recording made by a build with the inputs")
    parser.add_argument("--no-cache", action="store_true",
                        help="reference/terms/triage: every result drawn again, none kept (the cache: ~/goat-rig/"
                             "rt2-bench/<name>/cache, keyed by the recording, the replay's code, the kernels, the "
                             "GPU, the settings and the request)")
    args = parser.parse_args()
    if args.what in ("view", "reference", "terms", "triage"):
        args.set = [*args.set, *edited_kernels(args)]
    if args.what in ("verify", "run", "view", "reference", "terms", "triage"):
        args.set = [*args.set, *unit_env(args)]
    return {"record": record, "verify": verify, "run": run, "view": view, "reference": reference, "terms": terms,
            "triage": triage}[args.what](args)


if __name__ == "__main__":
    sys.exit(main())
