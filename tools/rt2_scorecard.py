#!/usr/bin/env python3
"""The ray tracing's scorecard: one command that holds the fixed recorded scenes in a build's replay, measures
them against the design's targets (~/dev/rt2-realtime-design.md, section 5: W3's stability, W2's truth error, the
section 4 budget) and prints one table, with a folder of pictures. Non-zero exit when a check fails.

    python3 tools/rt2_scorecard.py --build local/rt2-main            the mirrors' branch (built warm: linux_replay)
    python3 tools/rt2_scorecard.py --build local/my-try --push      this checkout's HEAD pushed there first
    python3 tools/rt2_scorecard.py --build local/rt2-main --mutate noise    a negative control: must fail
    python3 tools/rt2_scorecard.py shader --build local/rt2-main [--scene c20] [--watch]
        the light-transport kernels (port/linux/vulkan/lt/) compiled here and tried on a held frame without
        building the game (the replay's "reload"); --watch tries each saved change again

What it runs (all from recordings made once, ~/rt2-scorecard/<scene>.hrpl on omarchy and the 5900X):
- Stability, on omarchy's 8060S (goat-rig's GPU lock, shared): each scene's frame held 120 plays
  (tools/rt2_stability.py's hold and measures): the temporal standard deviation's relative p99 over plays 101-120,
  swelling (a 32x32 tile mean's largest change, plays 30 to 120) and play 1 against 120.
- Truth error, on the 5900X: each rt2 scene (~/goat-rig/rt2-scenes) with a truth and a realtime/irradiance.exr,
  compared by the truth renderer's own comparison (rt_truth compare: demodulated irradiance, the HUD, mask_rects
  and the first-person weapon left out): the 16x16 tile-mean bias and the per-pixel relMSE, with its signed error
  map.
- Cost, on both GPUs (the 4090, the 8060S; the GPU lock alone, one cost request each): the frame's passes the
  light transport and the denoiser own ("LIGHT TRANSPORT", "DENOISE"), against the 1080p budget.

The replay's environment is --env's (HALO_LT_E1=e2 by default: the flashlight's pass at 1 spp, counted). The
mutations: noise (2% of each pixel's luminance, random each play, added to the held pictures), bias (the realtime
irradiance 5% brighter), raw (HALO_LT_E1=raw: the pass's own 1-spp noise, no history).
"""

import argparse
import base64
import concurrent.futures
import json
import os
import shlex
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENV = Path.home() / ".cache" / "rt2-scorecard" / "venv"


def need_numpy():
    """numpy, from a venv of the scorecard's own made once when this Python has none"""
    try:
        import numpy  # noqa: F401
        return
    except ImportError:
        pass
    python = VENV / "bin" / "python"
    if os.environ.get("RT2_SCORECARD_VENV"):
        raise SystemExit("rt2_scorecard: no numpy in its venv")
    if not python.exists():
        print(f"rt2_scorecard: making {VENV} (numpy), once", file=sys.stderr)
        subprocess.run([sys.executable, "-m", "venv", str(VENV)], check=True)
        subprocess.run([str(python), "-m", "pip", "install", "-q", "numpy"], check=True)
    os.environ["RT2_SCORECARD_VENV"] = "1"
    os.execv(str(python), [str(python), *sys.argv])


need_numpy()
import numpy as np  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import rt2_stability  # noqa: E402

RIG = ROOT / "tools" / "rig" / "goat-rig"
RECORDINGS = "rt2-scorecard"  # (under the remote $HOME)
# the held scenes: name, recording, frame held, what it is
SCENES = [
# (the *_refl recordings, made from local/rt2-indirect@68b9b6de1 with HALO_LT_LIGHTS=1: the material records carry each
# shader's reflection strength; the same ticks and cameras as the *_lights ones: c20 598, a10 372, b30 898,
# combat 1369, c20walk 497)
    ("c20", "c20_refl.hrpl", 745, "c20's Flood corner, the flashlight"),
    ("a10", "a10_refl.hrpl", 925, "a10's cryo deck, a still camera on the reflective floor"),
    ("b30", "b30_refl.hrpl", 282, "b30's Pelican passing the beach rock (b30_exterior's view)"),
    ("combat", "combat_refl.hrpl", 593, "b30's beach in combat: plasma lights"),
    ("c20walk", "c20_walk_refl.hrpl", 595, "c20's corridor walked toward a Flood, the flashlight on (motion)"),
    # (made from local/rt2-fixes with HALO_LT_E1=denoise and HALO_LT_LIGHTS=1, b30's campaign at a fixed frame time:
    # the progress clip's two moments, the light list and motion recorded)
    ("b30sun", "b30_sun.hrpl", 650, "b30's beach in the sun: the Pelican, a Warthog, an Elite, the player (sun shadows)"),
    ("b30fight", "b30_fight.hrpl", 600, "b30's beach firefight: plasma bolts, needles, a grenade by a Warthog"),
]
# the visual checks (each found escaped once, by the progress clip of f7223c72e): on omarchy, shared, the pictures
# of the presented frame (sRGB decoded): (name, recording, frame, what)
# - sun shadow: the share of the frame the pass's sun shadows darken by 15% or more (the pass against itself with
#   HALO_LT_SUN=0), at least SHADOW_LEAST: the dynamic objects' shadows on the sunlit beach are there;
# - sun clamp: of those pixels, the share where the suns' share taken off is more than the lightmap has in a
#   channel (HALO_LT_SUN_CLAMP_VIEW=1's magenta), at most CLAMP_MOST: the subtraction tints nothing;
# - afterglow: AFTER frames after a light is gone (its last frame, its id), the largest 32x32 tile change of the
#   picture against the same frames played without that light (HALO_LT_DROP=id:), at most AFTERGLOW_MOST; and
#   while it was there the same at least AFTERGLOW_SEEN (the check sees the light at all)
VISUAL_SUN = ("b30sun", "b30_sun.hrpl", 650)
VISUAL_AFTERGLOW = ("b30fight", "b30_fight.hrpl", 660, 702, "ea94000c", "the grenade's explosion")
AFTER = 8
SHADOW_LEAST = 0.005
CLAMP_MOST = 0.01
AFTERGLOW_MOST = 0.02
AFTERGLOW_SEEN = 0.05
STABILITY_HOST = "omarchy"
TRUTH_HOST = "5900x"
COST_HOSTS = {"5900x": "RTX 4090", "omarchy": "Radeon 8060S"}
COST_SCENE = "c20"
TRUTH_SCENES = "goat-rig/rt2-scenes"
TRUTH_CODE = "goat-rig/rt2-truth-code"
TRUTH_PYTHON = "goat-rig/rt2-truth-venv/bin/python"
# the passes the new light transport owns (xgpu_vulkan_pass_owner's names; the cost's pass labels with
# HALO_PASS_TIMES)
OWNED_PASSES = ("LIGHT TRANSPORT", "DENOISE")
# ~/dev/rt2-realtime-design.md: section 5's W3 (stability) and W2 (direct light against the truth); section 4's
# 1080p budget
TARGETS = {
    "temporal std rel p99": 0.005,
    "swelling 30-120": 0.01,
    "play 1 vs 120": 0.03,
    "tile16 bias p95": 0.02,
    "relMSE": 0.01,
    "cost 5900x": 1.5,
    "cost omarchy": 5.0,
}
DEFAULT_ENV = ["HALO_LT_E1=e2"]
NOISE = 0.02
BIAS = 1.05
lock_print = threading.Lock()


def say(*words):
    with lock_print:
        print(*words, file=sys.stderr, flush=True)


# ---------- the machines

def target(host):
    return rt2_stability.ssh_target(host)


def remote_script(command, lock):
    """a bash script on a rig: the owner's own game not running, the GPU lock as goat-rig takes it ("shared":
    gpu.lock shared and one of its 3 picture slots; "alone": gpu.lock, waited for at most 2 minutes), in a private
    network (loopback only) where the machine allows it"""
    lines = ['set -e', 'if pgrep -f "^$HOME/Applications/OpenH1/bin/openh1" >/dev/null; then '
             'echo "the machine owner is playing" >&2; exit 3; fi', 'mkdir -p ~/goat-rig/locks']
    if lock == "shared":
        lines += ['exec 9>>~/goat-rig/locks/gpu.lock', 'flock -s 9',
                  'while :; do for s in 0 1 2; do exec 8>>~/goat-rig/locks/gpu-shared-$s.lock; '
                  'if flock -n 8; then break 2; fi; done; sleep 0.5; done']
    elif lock == "alone":
        lines += ['exec 9>>~/goat-rig/locks/gpu.lock',
                  'flock -w 120 9 || { echo "the GPU lock is busy (2 minutes)" >&2; exit 4; }']
    lines.append('if unshare -rn true 2>/dev/null; then exec unshare -rn sh -c \'ip link set lo up 2>/dev/null; '
                 'exec "$@"\' sh ' + command + '; else exec ' + command + '; fi')
    return "\n".join(lines)


def ssh(host, script, **kwargs):
    """a bash script run on host (the 5900X's login shell is fish: the script goes to bash whole)"""
    return ["ssh", "-C", "-o", "BatchMode=yes", target(host), "bash -c " + shlex.quote(script)]


class Service(rt2_stability.Service):
    """the replay's service (--serve) on a rig over ssh, under the GPU lock"""

    def __init__(self, host, replay, recording, env, lock):
        command = " ".join(["env", *[shlex.quote(e) for e in env], shlex.quote(replay), shlex.quote(recording),
                            "--serve"])
        self.host = host
        self.errors = tempfile.TemporaryFile()
        self.process = subprocess.Popen(ssh(host, remote_script(command, lock)), stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.errors, text=True, bufsize=1)

    def ask(self, request):
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            self.errors.seek(0)
            tail = self.errors.read().decode(errors="replace").strip().splitlines()[-3:]
            raise RuntimeError(f"{self.host}: the replay stopped (exit {self.process.wait()}): {' / '.join(tail)}")
        reply = json.loads(line)
        if not reply.get("ok"):
            raise RuntimeError(f"{self.host}: {request['cmd']}: {reply.get('error')}")
        return reply

    def close(self):
        try:
            self.process.stdin.write('{"cmd":"quit"}\n')
            self.process.stdin.close()
        except (BrokenPipeError, ValueError):
            pass
        self.process.wait()


def build_folder(ref):
    """goat-rig's worktree name for a ref (builds/<name>)"""
    return ref.replace("/", "-")


def replay_path(ref):
    return f"goat-rig/builds/{build_folder(ref)}/build/linux_x86_64/halo_replay"


def build(ref, hosts, push):
    """the replay built warm on each host (goat-rig build <ref> linux_replay); the checkout's HEAD pushed to the
    mirrors first with --push"""
    if push:
        if not ref.startswith("local/"):
            raise SystemExit("rt2_scorecard: --push needs a local/<name> ref")
        for host in hosts:
            subprocess.run(["git", "push", "-q", "-f", f"{target(host)}:goat-rig/repo.git",
                            f"HEAD:refs/heads/{ref}"], cwd=ROOT, check=True)

    def one(host):
        result = subprocess.run([str(RIG), f"@{host}", "build", ref, "linux_replay"], capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(f"rt2_scorecard: {host}: the build failed:\n{(result.stdout + result.stderr)[-2000:]}")
        return host

    with concurrent.futures.ThreadPoolExecutor() as pool:
        list(pool.map(one, hosts))


# ---------- the checks

def check(rows, kind, scene, value, target_key, said=None):
    limit = TARGETS[target_key]
    verdict = "PASS" if value is not None and value <= limit else "FAIL"
    rows.append({"check": kind, "scene": scene, "value": value, "target": limit, "verdict": verdict,
                 "said": said})


def note(rows, kind, scene, said, verdict="—"):
    rows.append({"check": kind, "scene": scene, "value": None, "target": None, "verdict": verdict, "said": said})


def stability(scene, replay, env, out, mutate):
    name, recording, frame, _ = scene
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    started = time.time()
    service = Service(STABILITY_HOST, replay, f"{RECORDINGS}/{recording}", env, "shared")
    try:
        info, reply, width, height, mask, mask_said = rt2_stability.hold(service, frame, 120, reset=True)
    finally:
        service.close()
    perturb = None
    if mutate == "noise":
        generator = np.random.default_rng(7)
        perturb = lambda k, lum: lum * (1.0 + NOISE * generator.standard_normal(lum.shape))  # noqa: E731
    measures = rt2_stability.measure(reply, width, height, mask, 120, folder, perturb)
    measures.update({"recording": recording, "frame": frame, "build": info.get("build"), "hud_mask": mask_said,
                     "hold_ms": reply.get("ms"), "seconds": round(time.time() - started, 1)})
    (folder / "stability.json").write_text(json.dumps(measures, indent=2) + "\n")
    say(f"  stability {name}: {measures['seconds']} s")
    return measures


def stability_rows(rows, name, m):
    t = m["temporal_std"]
    check(rows, "temporal std rel p99", name, t["relative_p99"], "temporal std rel p99",
          f"abs p99 {t['p99']:.5f}")
    check(rows, "swelling 30-120", name, m["swelling_30_120"]["max_relative"], "swelling 30-120",
          f"median {m['swelling_30_120']['median_relative']:.2%}")
    check(rows, "play 1 vs 120", name, m["first_vs_120"]["max_relative"], "play 1 vs 120",
          f"median {m['first_vs_120']['median_relative']:.2%}")


def pictures(env, recording, frames, hold=None):
    """the presented pictures (linear luminance and the sRGB bytes, numpy) of frames played in order, or of a frame
    held so many plays (its last)"""
    service = Service(STABILITY_HOST, replay_path_for_visual[0], f"{RECORDINGS}/{recording}", env, "shared")
    try:
        if hold:
            reply = service.ask({"cmd": "hold", "n": frames[0], "frames": hold, "shots": [hold], "inline": True,
                                 "compress": True, "reset": True})
            datas = {frames[0]: base64.b64decode(reply["shots"][-1]["png_base64"])}
        else:
            datas = {}
            for n in frames:
                datas[n] = base64.b64decode(service.ask({"cmd": "frame", "n": n, "inline": True,
                                                         "compress": True})["png_base64"])
    finally:
        service.close()
    result = {}
    for n, data in datas.items():
        w, h, lum = rt2_stability.png_luminance(data)
        result[n] = (np.asarray(lum, dtype=np.float64).reshape(h, w), rt2_stability.png_rgb_numpy(data), data)
    return result


replay_path_for_visual = [None]


def tile_largest(a, b):
    """the largest 32x32 tile mean's change from b to a, relative to b's (tiles darker than 0.002 left out)"""
    h, w = a.shape
    th, tw = h // 32, w // 32
    ta = a[:th * 32, :tw * 32].reshape(th, 32, tw, 32).mean(axis=(1, 3))
    tb = b[:th * 32, :tw * 32].reshape(th, 32, tw, 32).mean(axis=(1, 3))
    keep = np.maximum(ta, tb) > 0.002
    return float((np.abs(ta - tb)[keep] / np.maximum(tb[keep], 1e-6)).max()) if keep.any() else 0.0


def visual(replay, env, out):
    """the visual checks (VISUAL_*): [(check, scene, value, target, at least?, said)]"""
    replay_path_for_visual[0] = replay
    found = []
    name, recording, frame = VISUAL_SUN
    lit = pictures(env, recording, [frame], hold=30)[frame]
    off = pictures(env + ["HALO_LT_SUN=0"], recording, [frame], hold=30)[frame]
    clamp = pictures(env + ["HALO_LT_SUN_CLAMP_VIEW=1"], recording, [frame], hold=30)[frame]
    shadowed = lit[0] < 0.85 * off[0]
    # (the view's magenta, which the surface's texture then multiplies: red and blue, no green)
    rgb = clamp[1].astype(np.int32)
    magenta = (rgb[..., 0] > 30) & (rgb[..., 2] > 30) & (rgb[..., 1] * 10 < np.minimum(rgb[..., 0], rgb[..., 2]))
    shadowed |= magenta
    share = float(shadowed.mean())
    clamped = float((magenta & shadowed).sum() / max(shadowed.sum(), 1))
    found.append(("sun shadow share", name, share, SHADOW_LEAST, True, f"{int(shadowed.sum())} px darkened 15%+"))
    found.append(("sun clamp share", name, clamped, CLAMP_MOST, False, f"{int(magenta.sum())} px clamped "
                  f"({magenta.mean():.3%} of the frame)"))
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    for label, picture in (("sun_on", lit), ("sun_off", off), ("sun_clamp_view", clamp)):
        (folder / f"{label}.png").write_bytes(picture[2])
    name, recording, start, last, light, what = VISUAL_AFTERGLOW
    frames = list(range(start, last + AFTER + 1))
    with_light = pictures(env, recording, frames)
    without = pictures(env + [f"HALO_LT_DROP=id:{light}"], recording, frames)
    folder = out / name
    folder.mkdir(parents=True, exist_ok=True)
    for n in (last, last + AFTER):
        (folder / f"afterglow_{n}_with.png").write_bytes(with_light[n][2])
        (folder / f"afterglow_{n}_without.png").write_bytes(without[n][2])
    seen = tile_largest(with_light[last][0], without[last][0])
    after = tile_largest(with_light[last + AFTER][0], without[last + AFTER][0])
    found.append((f"afterglow {AFTER} frames", name, after, AFTERGLOW_MOST, False,
                  f"{what} ({light}): {seen:.1%} while there (at least {AFTERGLOW_SEEN:.0%})"))
    if seen < AFTERGLOW_SEEN:
        found.append(("afterglow seen", name, seen, AFTERGLOW_SEEN, True, "the light is not seen: the check sees nothing"))
    return found


def visual_rows(rows, found):
    for kind, scene, value, limit, least, said in found:
        verdict = "PASS" if value is not None and (value >= limit if least else value <= limit) else "FAIL"
        rows.append({"check": kind, "scene": scene, "value": value, "target": limit, "verdict": verdict,
                     "said": said, "least": least})


def truth(out, mutate, run_id, part):
    """the truth comparisons on the 5900X: [(scene, result dict or None, what was said)]"""
    work = f"{RECORDINGS}/out/{run_id}"
    listing = subprocess.run(ssh(TRUTH_HOST, f'for d in ~/{TRUTH_SCENES}/*/; do [ -d "$d/truth" ] && '
                                              f'[ -f "$d/realtime/irradiance.exr" ] && basename "$d"; done; '
                                              f'[ -x ~/{TRUTH_PYTHON} ] && [ -f ~/{TRUTH_CODE}/rt_truth/compare.py ] '
                                              f'&& echo "compare: yes"; true'),
                             capture_output=True, text=True)
    names = [line for line in listing.stdout.split() if line and line not in ("compare:", "yes")]
    if "compare: yes" not in listing.stdout:
        return [(name, None, "pending: rt_truth compare is not on the 5900X yet") for name in names] or \
            [("-", None, "pending: rt_truth compare is not on the 5900X yet")]
    if not names:
        return [("-", None, "pending: no rt2 scene has both truth/ and realtime/irradiance.exr")]
    results = []
    for name in names:
        scene = f"~/{TRUTH_SCENES}/{name}"
        exr = f"{scene}/realtime/irradiance.exr"
        lines = [f"mkdir -p ~/{work}/{name}", f"cd ~/{TRUTH_CODE}"]
        if mutate == "bias":
            # (the realtime irradiance made brighter, beside the run's own results; its mask goes with it)
            lines.append(f"OPENCV_IO_ENABLE_OPENEXR=1 ~/{TRUTH_PYTHON} -c \"import cv2,sys; "
                         f"i=cv2.imread(sys.argv[1], cv2.IMREAD_UNCHANGED); cv2.imwrite(sys.argv[2], i*{BIAS})\" "
                         f"{exr} ~/{work}/{name}/irradiance.exr")
            lines.append(f"[ -f {scene}/realtime/mask.png ] && cp {scene}/realtime/mask.png ~/{work}/{name}/ || true")
            exr = f"~/{work}/{name}/irradiance.exr"
        lines.append(f"~/{TRUTH_PYTHON} -m rt_truth compare {scene} {exr} --part {part} "
                     f"--quantity irradiance --out ~/{work}/{name}/compare --json")
        result = subprocess.run(ssh(TRUTH_HOST, "\n".join(lines)), capture_output=True, text=True)
        if result.returncode:
            results.append((name, None, "compare failed: " + (result.stderr.strip().splitlines() or ["?"])[-1][:120]))
            continue
        try:
            data = json.loads(result.stdout[result.stdout.index("{"):])
        except ValueError:
            results.append((name, None, "compare said no JSON"))
            continue
        local = out / f"truth_{name}"
        local.mkdir(parents=True, exist_ok=True)
        subprocess.run(["rsync", "-a", f"{target(TRUTH_HOST)}:{work}/{name}/compare/", f"{local}/"],
                       capture_output=True)
        provenance = subprocess.run(ssh(TRUTH_HOST, f"echo; date -r {scene}/realtime/irradiance.exr +%H:%M"),
                                    capture_output=True, text=True).stdout.strip().splitlines()
        data["realtime_made"] = provenance[-1] if provenance else "?"
        (local / "compare.json").write_text(json.dumps(data, indent=2) + "\n")
        results.append((name, data, None))
    subprocess.run(ssh(TRUTH_HOST, f"rm -rf ~/{work}"), capture_output=True)
    return results


def truth_rows(rows, results):
    for name, data, said in results:
        if data is None:
            note(rows, "truth error", name, said)
            continue
        bias = data.get("tile16_bias", {})
        check(rows, "tile16 bias p95", name, bias.get("p95_abs"), "tile16 bias p95",
              f"max {bias.get('max_abs', float('nan')):.2%}, global {bias.get('global', float('nan')):+.2%}, "
              f"realtime of {data.get('realtime_made')}")
        check(rows, "relMSE", name, data.get("rel_mse"), "relMSE", f"eps {data.get('rel_eps', 0):.2g}")


def cost(host, replay, env):
    """one cost request of the cost scene's frame, the GPU alone: (owned passes' ms, frame ms, the passes owned)"""
    name, recording, frame, _ = next(s for s in SCENES if s[0] == COST_SCENE)
    # (HALO_PASS_TIMES keeps the passes' owners, which name them; its period long: nothing logged)
    service = Service(host, replay, f"{RECORDINGS}/{recording}", env + ["HALO_PASS_TIMES=100000"], "alone")
    try:
        reply = service.ask({"cmd": "cost", "n": frame, "repeats": 5})
    finally:
        service.close()
    owned = [p for p in reply.get("passes", []) if str(p.get("kind", "")).startswith(OWNED_PASSES)]
    return sum(p["ms"] for p in owned), reply.get("frame_ms"), owned


def cost_rows(rows, host, result):
    if isinstance(result, Exception):
        note(rows, f"cost {COST_HOSTS[host]}", COST_SCENE, f"not measured: {result}", "ERROR")
        return
    ms, frame_ms, owned = result
    if not owned:
        note(rows, f"cost {COST_HOSTS[host]}", COST_SCENE, f"no pass named {' or '.join(OWNED_PASSES)} "
             f"(frame {frame_ms:.2f} ms)", "ERROR")
        return
    check(rows, f"cost {COST_HOSTS[host]} (ms)", COST_SCENE, ms, f"cost {host}",
          f"frame {frame_ms:.2f} ms, {len(owned)} passes")


def table(rows):
    def shown(value, kind):
        if value is None:
            return ""
        return f"{value:.2f}" if "cost" in kind else f"{value:.2%}"

    width = max(len(r["check"]) for r in rows)
    lines = [f"{'check':<{width}}  {'scene':<10} {'value':>8} {'target':>8}  verdict  notes"]
    for r in rows:
        bound = "≥" if r.get("least") else "≤"
        lines.append(f"{r['check']:<{width}}  {r['scene']:<10} {shown(r['value'], r['check']):>8} "
                     f"{bound + shown(r['target'], r['check']) if r['target'] is not None else '':>8}  "
                     f"{r['verdict']:<7}  {r['said'] or ''}")
    return "\n".join(lines)


def scorecard(args):
    started = time.time()
    env = DEFAULT_ENV + args.env
    if args.mutate == "raw":
        env = [e for e in env if not e.startswith("HALO_LT_E1=")] + ["HALO_LT_E1=raw"]
    hosts = sorted({STABILITY_HOST, *([] if args.no_truth else [TRUTH_HOST]), *([] if args.no_cost else COST_HOSTS)})
    if args.build:
        say(f"building {args.build} (linux_replay) on {', '.join(hosts)}")
        build(args.build, hosts, args.push)
    replays = {host: getattr(args, f"replay_{host}") or (replay_path(args.build) if args.build else None)
               for host in hosts}
    if not all(replays.values()):
        raise SystemExit("rt2_scorecard: --build, or a replay for each machine (--replay-omarchy, --replay-5900x)")
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    run_id = f"{time.strftime('%H%M%S')}-{os.getpid()}"
    rows, errors = [], []
    say(f"measuring ({time.time() - started:.0f} s in)")
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        held = {s[0]: pool.submit(stability, s, replays[STABILITY_HOST], env, out, args.mutate)
                for s in SCENES if not args.scenes or s[0] in args.scenes}
        compared = pool.submit(truth, out, args.mutate, run_id, args.truth_part) if not args.no_truth else None
        seen = pool.submit(visual, replays[STABILITY_HOST], env, out) if not args.no_visual else None
        costs = {}
        if not args.no_cost:
            # (the 5900X's at once; omarchy's once its pictures are taken: it needs the GPU alone)
            costs["5900x"] = pool.submit(cost, "5900x", replays["5900x"], env)
            concurrent.futures.wait(list(held.values()))
            costs["omarchy"] = pool.submit(cost, "omarchy", replays["omarchy"], env)
        for name, future in held.items():
            try:
                stability_rows(rows, name, future.result())
            except Exception as error:  # (one scene's failure is the table's, not the run's)
                note(rows, "stability", name, str(error)[:160], "ERROR")
                errors.append(error)
        try:
            if compared:
                truth_rows(rows, compared.result())
        except Exception as error:
            note(rows, "truth error", "-", str(error)[:160], "ERROR")
            errors.append(error)
        if seen:
            try:
                visual_rows(rows, seen.result())
            except Exception as error:
                note(rows, "visual", "-", str(error)[:160], "ERROR")
                errors.append(error)
        for host, future in costs.items():
            try:
                cost_rows(rows, host, future.result())
            except Exception as error:
                cost_rows(rows, host, error)
                errors.append(error)
    print(f"rt2 scorecard: {args.build or 'given replays'}  env {' '.join(env)}"
          + (f"  MUTATION {args.mutate}" if args.mutate else ""))
    print(table(rows))
    failed = [r for r in rows if r["verdict"] in ("FAIL", "ERROR")]
    print(f"{len(rows) - len(failed)} of {len(rows)} rows passed or pending; {len(failed)} failed. Pictures: {out} "
          f"(per scene: heat.png, hold_1/30/120.png; truth_<scene>: signed_error.png, tile_bias.png, mask.png). "
          f"{time.time() - started:.0f} s.")
    (out / "scorecard.json").write_text(json.dumps({"build": args.build, "env": env, "mutate": args.mutate,
                                                     "rows": rows, "seconds": time.time() - started}, indent=2,
                                                    default=float) + "\n")
    return 1 if failed else 0


# ---------- the kernels from files: a change tried on a held frame

def kernels_compiled(folder):
    """port/linux/vulkan/lt's kernels compiled here (tools/vulkan_lt_shaders.py's list and compiler) into
    folder/<source>.spv"""
    import vulkan_lt_shaders
    names = []
    for _, source, defines in vulkan_lt_shaders.KERNELS:
        name = vulkan_lt_shaders.kernel_name(source, defines)
        (folder / f"{name}.spv").write_bytes(vulkan_lt_shaders.spirv(vulkan_lt_shaders.FOLDER / source, defines))
        names.append(name)
    return names


def shader(args):
    """a held frame drawn with the kernels as they are in this checkout, again on each change with --watch"""
    import vulkan_lt_shaders
    host = args.host
    replay = args.replay or (replay_path(args.build) if args.build else None)
    if not replay:
        raise SystemExit("rt2_scorecard shader: --build or --replay")
    if args.build and not args.replay:
        build(args.build, [host], args.push)
    name, recording, frame, _ = next(s for s in SCENES if s[0] == args.scene)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    remote = f"{RECORDINGS}/spv/{os.getpid()}"
    env = DEFAULT_ENV + args.env
    service = None
    version = 0

    def changed_at():
        return max(path.stat().st_mtime for path in vulkan_lt_shaders.FOLDER.glob("*.*") if path.suffix != ".inc")

    try:
        service = Service(host, replay, f"{RECORDINGS}/{recording}", env, "shared")
        home = subprocess.run(["ssh", target(host), f"mkdir -p ~/{remote} && echo $HOME"], capture_output=True,
                              text=True).stdout.strip()
        if "reload" not in service.ask({"cmd": "info"}).get("capabilities", []):
            raise SystemExit("rt2_scorecard shader: this replay has no reload")
        seen = None
        while True:
            stamp = changed_at()
            if stamp != seen:
                seen = stamp
                started = time.time()
                version += 1
                with tempfile.TemporaryDirectory() as folder:
                    try:
                        kernels_compiled(Path(folder))
                    except subprocess.CalledProcessError as error:
                        say(f"version {version}: the GLSL does not compile ({error}); kept the last")
                        if not args.watch:
                            return 1
                        continue
                    subprocess.run(["rsync", "-a", f"{folder}/", f"{target(host)}:{remote}/v{version}/"],
                                   check=True, capture_output=True)
                service.ask({"cmd": "reload", "dir": f"{home}/{remote}/v{version}"})
                reply = service.ask({"cmd": "hold", "n": frame, "frames": args.frames, "shots": [args.frames],
                                     "inline": True, "compress": True, "reset": True})
                picture = out / f"{name}_v{version}.png"
                picture.write_bytes(base64.b64decode(reply["shots"][-1]["png_base64"]))
                say(f"version {version}: {picture} ({args.frames} plays held, {time.time() - started:.1f} s from "
                    f"the saved GLSL)")
            if not args.watch:
                return 0
            time.sleep(0.25)
    except KeyboardInterrupt:
        return 0
    finally:
        if service:
            service.close()
        subprocess.run(["ssh", target(host), f"rm -rf ~/{remote}"], capture_output=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", nargs="?", default="score", choices=["score", "shader"])
    parser.add_argument("--build", help="a mirrors' ref (local/<name>), built warm on each machine (linux_replay)")
    parser.add_argument("--push", action="store_true", help="this checkout's HEAD pushed to --build's ref first")
    parser.add_argument("--replay-omarchy", help="a replay binary on omarchy instead (its path there)")
    parser.add_argument("--replay-5900x", help="a replay binary on the 5900X instead (its path there)")
    parser.add_argument("--env", action="append", default=[], help="VAR=value for the replays (again for more)")
    parser.add_argument("--mutate", choices=["noise", "bias", "raw"], help="a negative control: must fail")
    parser.add_argument("--scenes", nargs="*", help="only these held scenes (c20 a10 b30 combat c20walk b30sun "
                        "b30fight)")
    # (E1's realtime irradiance has the geometric normal's cosine until the pre-pass carries the bump normal)
    parser.add_argument("--truth-part", default="direct_flashlight_geometric",
                        help="the truth's part compared (rt_truth compare --part)")
    parser.add_argument("--no-cost", action="store_true", help="no cost runs")
    parser.add_argument("--no-visual", action="store_true", help="no visual checks (sun shadow, clamp, afterglow)")
    parser.add_argument("--no-truth", action="store_true", help="no truth comparisons")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "rt2_scorecard", help="the pictures' folder")
    # shader
    parser.add_argument("--scene", default="c20", help="shader: the held scene")
    parser.add_argument("--host", default=STABILITY_HOST, help="shader: the machine")
    parser.add_argument("--replay", help="shader: a replay binary on --host (its path there)")
    parser.add_argument("--frames", type=int, default=30, help="shader: the plays held")
    parser.add_argument("--watch", action="store_true", help="shader: again on each saved change, until Ctrl-C")
    args = parser.parse_args()
    if args.what == "shader":
        return shader(args)
    try:
        return scorecard(args)
    except SystemExit:
        raise
    except Exception as error:
        print(f"rt2_scorecard: could not run: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
