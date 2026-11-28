#!/usr/bin/env python3
"""The ray-traced lighting's point lights against the classic look: one command that measures each kind of point light
the pass draws (muzzle flashes, explosions, projectiles, the port's own lights) against what the setting-off look
adds there, on held frames of the recordings, and prints the k each one needs beside the k the build gives it
(lt_lights.c's lt_point_power_scale: the classic's light matched at half the reach).

    python3 tools/rt2_effect_k.py --build local/my-branch                  measure, the table printed
    python3 tools/rt2_effect_k.py --build local/my-branch --json out.json  the results written there too
    python3 tools/rt2_effect_k.py --table                                  the build's rule alone, by reach

With display.ray_traced_lighting off, the enhanced dynamic lights add each point light LINEARLY (under a shoulder
near white): BRIGHTNESS x color^2.2 x W(d/r) / (1 + core d^2), core = max(CORE / r^2, 1 / CORE_MOST^2), W the range
window (1 - (d/r)^4)^2, times the receiver's cosine (d3d8_device.c's look[0], xgpu_spirv.c's
dynamic_light_sum_classic and dynamic_light_add_classic); no shadows. The pass adds k x color^2.2 x W(d/r) /
max(d^2, emitter^2) x cosine, shadowed (lt/e1.frag). So at every pixel the classic's light is the pass's unshadowed
light at k = 1 times R(d) = BRIGHTNESS x max(d^2, emitter^2) / (1 + core d^2), and the measure compares LIGHT
(demodulated irradiance, Erin's rule for the truth comparisons), not finished pictures:

- each light alone in a held frame (HALO_LT_ONLY=ids:<id>, the sun, the bounce off, k = 1 by HALO_LT_KIND_SCALE,
  HALO_LT_E1=accumulate PLAYS plays: converged), its irradiance and each pixel's d/r (HALO_LT_E1_DUMP,
  HALO_LT_E1_DUMP_REACH), once unshadowed (the classic's stand-in, by R(d)) and once shadowed (the pass as drawn);
  these are debugging settings, said so: the scorecard and clips use the menu setting alone;
- k needed = the classic's light summed over the frame's pixels / the pass's at k = 1: the total light the light
  adds to the scene as seen, overall, by the base under it (the frame's presented luminance with the light: dark
  shade/night, dim dusk/indoor, lit sun) and over the old band (0.4 to 0.6 of the reach, how the first k were taken);
  with the shadows (the pass as drawn) and without (the light it sends, which the rule matches);
- the reach: the distance (in reaches) within which half and 90% of the light added lands, classic and pass at the
  build's k.

The lights' rows (id, kind, flags, range, emitter radius, power) from the pass's table log (HALO_LT_TABLE_LOG) over
each recording's window; for each kind (first-person muzzle flash, third-person flash, explosion, projectile, own
projectile light, own effect flash), the brightest few, a frame each. It runs on the 5900X by default (the owner's
game checked first, the GPU lock shared), the analysis there; only the results come back.
"""

import argparse
import base64
import json
import math
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "tools" / "rig" / "goat-rig"
REMOTE = "goat-rig/rt2-effect-k"
OWNER_GAME = "^$HOME/Applications/OpenH1[^/ ]*/bin/openh1"
# the classic light model's look[0] (d3d8_device.c's defaults) and the soft core's largest (xgpu.h)
BRIGHTNESS, CORE, CORE_MOST = 1.7, 12.0, 0.6
STEPS = 64
DISK = (0.05, 0.1, 0.5)  # lt_lights.c's emitter rule: 5% of the reach, 0.1 to 0.5 (a projectile's 0.05)

# the recordings (made with the light list, HALO_LT_LIGHTS=1, the pass on): name, path under the rig's $HOME, the
# frames searched, what they show. Recordings made after the outdoor live sky or the exposure land go here too.
RECORDINGS = {
    "w2_stage": ("rt2-scorecard/w2_stage.hrpl", 520, 730,
                 "b30's beach (W2's staging): the assault rifle's flashes in first person, a frag grenade"),
    "combat": ("rt2-scorecard/combat_lights.hrpl", 460, 620,
               "b30's beach in combat: Elites' plasma pistol flashes (third person), plasma bolts"),
    "b30_fight": ("goat-rig/rt2-effect-k/b30_fight_lt.hrpl", 430, 720,
                  "b30's beach firefight (rt2/fixes' b30_fight): plasma bolts, needles, a grenade by a Warthog"),
}

def say(*words):
    print(*words, file=sys.stderr, flush=True)


# ---------- the rule (lt_lights.c's lt_point_power_scale, operation for operation)

def window(x):
    w = max(0.0, 1.0 - x ** 4)
    return w * w


def classic_core(r):
    return max(CORE / (r * r), 1.0 / (CORE_MOST * CORE_MOST))


def point_k(r, emitter):
    """(the other rule tried: the classic's flux over the reach over the pass's at k = 1)"""
    if r <= 0:
        return 0.0
    core, r2, classic, passed = classic_core(r), emitter * emitter, 0.0, 0.0
    for step in range(STEPS):
        x = (step + 0.5) / STEPS
        d2 = (x * r) ** 2
        classic += window(x) * d2 / (1 + core * d2)
        passed += window(x) * d2 / max(d2, r2)
    return BRIGHTNESS * classic / passed if passed else 0.0


def mid_k(r, emitter):
    """the build's rule (lt_lights.c's lt_point_power_scale): the classic matched at half the reach"""
    d = r / 2
    return BRIGHTNESS * max(d * d, emitter * emitter) / (1 + classic_core(r) * d * d)


def disk(r, projectile=False):
    return 0.05 if projectile else min(max(DISK[0] * r, DISK[1]), DISK[2])


def table():
    rows = []
    for r, projectile, what in [(1.0, True, "needle (own light, on a projectile)"),
                                (1.5, True, "plasma bolt (projectile)"), (2.0, False, "muzzle flash"),
                                (3.0, False, "a longer flash"), (5.0, False, "a fuel rod / mortar flash"),
                                (10.0, False, "grenade explosion")]:
        e = 0.05 if projectile else disk(r)
        rows.append({"what": what, "reach": r, "emitter": e, "k_flux": round(point_k(r, e), 4),
                     "k_mid": round(mid_k(r, e), 4)})
    return rows


# ---------- the rig

def rig_target(host):
    hosts = {"omarchy": "omarchy.fabric", "5900x": "5900x.fabric"}
    path = Path.home() / ".config/goat-rig/hosts"
    if path.exists():
        for line in path.read_text().splitlines():
            if len(line.split()) == 2 and not line.startswith("#"):
                hosts[line.split()[0]] = line.split()[1]
    return hosts.get(host, host)


def ssh(host, script, capture=True, check=True):
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", rig_target(host), "bash", "-s"], input=script, text=True,
                            capture_output=capture)
    if check and result.returncode:
        raise SystemExit(f"rt2_effect_k: {host}: {(result.stderr or '').strip()[-600:]}")
    return result


def owner_check(host):
    who = ssh(host, f'pgrep -fa "{OWNER_GAME}" || true').stdout.strip()
    if who:
        raise SystemExit(f"rt2_effect_k: {host}'s owner is playing ({who}); not starting")


def rig(host, *argv):
    return subprocess.run([sys.executable, str(RIG), f"@{host}", *argv], text=True, capture_output=True)


# ---------- on the rig: the measure (--remote)

FIRST_PERSON, PROJECTILE, NO_OBJECT = 1 << 10, 1 << 2, 0x80000000


def kind_of(row):
    """a point light's kind, as lt_lights.c's records tell it: its source (the id's high word's top byte: 1 to 3 the
    port's own lights, 4 a game light, 5 a copy), its flags, whether it is on an object"""
    source, flags = row["id1"] >> 24, row["flags"]
    if source in (1, 2, 3):
        if flags & PROJECTILE:
            return "own projectile light (a needle)"
        return "own effect flash" if source == 1 else "own light on an object"
    if flags & PROJECTILE:
        return "projectile (a plasma bolt)"
    if flags & NO_OBJECT:
        return "explosion / effect (no object)"
    if flags & FIRST_PERSON:
        return "muzzle flash, first person"
    return "muzzle flash / light on an object, third person"


def remote_main(args):
    import numpy as np

    sys.path.insert(0, str(Path(args.tools)))
    import rt2_stability

    class Service:
        def __init__(self, env):
            self.log = tempfile.NamedTemporaryFile(prefix="effectk-", suffix=".log", delete=False)
            command = ["env", *env, args.replay, args.recording, "--serve"]
            self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                            stderr=self.log, text=True, bufsize=1)

        def ask(self, request):
            self.process.stdin.write(json.dumps(request) + "\n")
            self.process.stdin.flush()
            line = self.process.stdout.readline()
            if not line:
                raise SystemExit(f"rt2_effect_k: the replay stopped: {Path(self.log.name).read_text()[-800:]}")
            reply = json.loads(line)
            if not reply.get("ok"):
                raise SystemExit(f"rt2_effect_k: {request['cmd']}: {reply.get('error')}")
            return reply

        def log_text(self):
            return Path(self.log.name).read_text(errors="replace")

        def close(self):
            try:
                self.process.stdin.write('{"cmd":"quit"}\n')
                self.process.stdin.close()
            except (BrokenPipeError, ValueError):
                pass
            self.process.wait()
            os.unlink(self.log.name)

    def read_dump(path):
        data = Path(path).read_bytes()
        magic, width, height, _ = data[:4], *np.frombuffer(data[4:16], dtype="<u4")
        if magic != b"LTE1":
            raise SystemExit(f"rt2_effect_k: {path}: not a dump")
        return np.frombuffer(data[16:16 + int(width) * int(height) * 8], dtype="<f2").astype(np.float64).reshape(
            int(height), int(width), 4)

    k1 = "HALO_LT_KIND_SCALE=point=1"
    quiet = ["HALO_LT_SUN=0", "HALO_LT_INDIRECT=0", "HALO_LT_SUN_CLAMP_VIEW=0"]

    # 1. the window's lights, from the pass's table log, frame by frame
    service = Service(["HALO_LT_E1=denoise", "HALO_LT_TABLE_LOG=1", k1] + quiet)
    frames = service.ask({"cmd": "info"}).get("frames", 0)
    first, last = args.first, min(args.last, frames - 1)
    pattern = re.compile(r"table frame (\d+) row (\d+) id (\w+) (\w+) kind (\d+) at (\S+) (\S+) (\S+) emitter (\S+) "
                         r"range (\S+) power (\S+) (\S+) (\S+) flags (\w+)")
    seen, lights = 0, {}
    for n in range(first, last + 1):
        service.ask({"cmd": "frame", "n": n})
        text = service.log_text()
        rows = []
        for m in pattern.finditer(text[seen:]):
            if int(m.group(5)) != 0:
                continue
            power = [float(m.group(i)) for i in (11, 12, 13)]
            rows.append({"id0": int(m.group(3), 16), "id1": int(m.group(4), 16), "flags": int(m.group(14), 16),
                         "at": [float(m.group(i)) for i in (6, 7, 8)], "emitter": float(m.group(9)),
                         "range": float(m.group(10)), "power": power,
                         "luminance": 0.2126 * power[0] + 0.7152 * power[1] + 0.0722 * power[2]})
        seen = len(text)
        lights[n] = rows
    service.close()

    # (each light's life: the frames it is in, its brightest)
    life = {}
    for n, rows in lights.items():
        for row in rows:
            entry = life.setdefault(row["id0"], {"frames": [], "peak": 0.0, "range": row["range"]})
            entry["frames"].append(n)
            entry["peak"] = max(entry["peak"], row["luminance"])

    # 2. each kind's brightest few (flux at k = 1 ~ luminance x reach), a frame each, apart
    best = {}
    for n, rows in lights.items():
        for row in rows:
            if row["luminance"] <= 0 or row["range"] <= 0:
                continue
            best.setdefault(kind_of(row), []).append((row["luminance"] * row["range"], n, row))
    chosen = []
    for kind, items in sorted(best.items()):
        items.sort(key=lambda item: -item[0])
        picked, ids = [], set()
        for item in items:
            if item[2]["id0"] in ids or any(abs(item[1] - other[1]) < 4 for other in picked):
                continue
            picked.append(item)
            ids.add(item[2]["id0"])
            if len(picked) >= args.per_kind:
                break
        chosen += [(kind, n, row) for _, n, row in picked]
    say(f"rt2_effect_k: {args.moment}: {len(chosen)} lights chosen ({', '.join(sorted(best))})")

    # 3. each alone, held: unshadowed and shadowed, with its reach; the frame as presented, for the base
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    results = []
    bins = [("dark", 0.0, 0.02), ("dim", 0.02, 0.1), ("lit", 0.1, 10.0)]
    for kind, n, row in chosen:
        hexid = f"{row['id0']:08x}"
        parts = {}
        for name, extra in (("unshadowed", ["HALO_LT_E1_UNSHADOWED=1"]), ("shadowed", [])):
            folder = out / f"{args.moment}_{n}_{hexid}_{name}"
            folder.mkdir(exist_ok=True)
            env = (["HALO_LT_E1=accumulate", k1, f"HALO_LT_ONLY=ids:{hexid}", f"HALO_LT_E1_DUMP={args.plays}",
                    f"HALO_LT_E1_DUMP_DIR={folder}", f"HALO_LT_E1_DUMP_REACH={hexid}"] + quiet + extra)
            service = Service(env)
            try:
                reply = service.ask({"cmd": "hold", "n": n, "frames": args.plays + 2, "shots": [args.plays + 2],
                                     "inline": True, "compress": True, "reset": True})
                png = base64.b64decode(reply["shots"][-1]["png_base64"])
            finally:
                service.close()
            mean = next(folder.glob(f"lt_e1_{args.plays}.rgba16f"), None)
            reach = next(folder.glob(f"lt_e1_{args.plays}_reach.rgba16f"), None)
            if not mean or not reach:
                say(f"rt2_effect_k: {args.moment} {n} {hexid}: no dump ({sorted(p.name for p in folder.iterdir())})")
                break
            parts[name] = (read_dump(mean), read_dump(reach), png)
        if len(parts) < 2:
            continue
        e_open = parts["unshadowed"][0][..., :3] @ np.array([0.2126, 0.7152, 0.0722])
        e_shadow = parts["shadowed"][0][..., :3] @ np.array([0.2126, 0.7152, 0.0722])
        x = parts["unshadowed"][1][..., 0]
        valid = (x >= 0) & (x < 1) & np.isfinite(e_open) & np.isfinite(e_shadow)
        r, emitter = row["range"], row["emitter"]
        d = x * r
        ratio = BRIGHTNESS * np.maximum(d * d, emitter * emitter) / (1 + classic_core(r) * d * d)
        classic = np.where(valid, np.maximum(e_open, 0) * ratio, 0.0)
        p_open = np.where(valid, np.maximum(e_open, 0), 0.0)
        p_shadow = np.where(valid, np.maximum(e_shadow, 0), 0.0)
        w, h, lum = rt2_stability.png_luminance(parts["shadowed"][2])
        base = np.asarray(lum, dtype=np.float64).reshape(h, w)
        if base.shape != classic.shape:
            base = np.zeros_like(classic)
        k_build = mid_k(r, emitter)

        def k_over(mask):
            c, p = float(classic[mask].sum()), float(p_open[mask].sum())
            return round(c / p, 4) if p > 0 and c > 0 and mask.sum() >= 500 else None

        def reach_of(light, share):
            weights = light[valid]
            order = np.argsort(x[valid])
            cumulative = np.cumsum(weights[order])
            if not len(cumulative) or cumulative[-1] <= 0:
                return None
            return round(float(x[valid][order][np.searchsorted(cumulative, share * cumulative[-1])]), 3)

        total_c, total_open, total_shadow = float(classic.sum()), float(p_open.sum()), float(p_shadow.sum())
        entry = life.get(row["id0"], {"frames": [n], "peak": row["luminance"]})
        result = {
            "recording": args.moment, "frame": n, "id": hexid, "kind": kind, "range": r, "emitter": emitter,
            "power_k1": [round(v, 5) for v in row["power"]], "lit_pixels": int((p_open > 0).sum()),
            "life_frames": len(entry["frames"]), "life_span": [min(entry["frames"]), max(entry["frames"])],
            "k_needed": round(total_c / total_open, 4) if total_open > 0 else None,
            "k_needed_with_shadows": round(total_c / total_shadow, 4) if total_shadow > 0 else None,
            "k_band_0.4_0.6": k_over(valid & (x >= 0.4) & (x <= 0.6)),
            "k_build": round(k_build, 4), "k_flux_rule": round(point_k(r, emitter), 4),
            "k_mid_rule": round(mid_k(r, emitter), 4),
            "by_base": {label: {"pixels": int((valid & (base >= lo) & (base < hi) & (p_open > 0)).sum()),
                                "k_needed": k_over(valid & (base >= lo) & (base < hi))} for label, lo, hi in bins},
            "reach_half_classic": reach_of(classic, 0.5), "reach_half_pass": reach_of(p_shadow * k_build, 0.5),
            "reach_90_classic": reach_of(classic, 0.9), "reach_90_pass": reach_of(p_shadow * k_build, 0.9),
            "build_over_classic": round(total_shadow * k_build / total_c, 3) if total_c > 0 else None,
        }
        results.append(result)
        (out / f"{args.moment}_{n}_{hexid}.png").write_bytes(parts["shadowed"][2])
        for name in parts:
            for path in (out / f"{args.moment}_{n}_{hexid}_{name}").glob("*.rgba16f"):
                path.unlink()
    print(json.dumps(results))
    return 0


# ---------- the Mac's side

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", help="the build (goat-rig build: a branch, local/<name>, sha or pr/N)")
    parser.add_argument("--host", default="5900x", choices=("5900x", "omarchy"))
    parser.add_argument("--recordings", default="all")
    parser.add_argument("--per-kind", type=int, default=3, help="lights measured of each kind a recording")
    parser.add_argument("--plays", type=int, default=160, help="the plays a held light accumulates")
    parser.add_argument("--json", help="also write the combined results to this file")
    parser.add_argument("--table", action="store_true", help="the rule's k by reach, and stop")
    parser.add_argument("--remote", action="store_true", help=argparse.SUPPRESS)
    for name in ("replay", "recording", "tools", "moment", "out"):
        parser.add_argument(f"--{name}", help=argparse.SUPPRESS)
    parser.add_argument("--first", type=int, default=0, help=argparse.SUPPRESS)
    parser.add_argument("--last", type=int, default=10 ** 9, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.remote:
        return remote_main(args)
    if args.table:
        print(json.dumps(table(), indent=1))
        return 0
    if not args.build:
        parser.error("--build is needed")
    names = list(RECORDINGS) if args.recordings == "all" else args.recordings.split(",")
    host = args.host
    owner_check(host)
    result = rig(host, "build", args.build, "linux_replay")
    if result.returncode:
        raise SystemExit(f"rt2_effect_k: building {args.build} failed:\n{(result.stdout + result.stderr)[-2000:]}")
    home = ssh(host, "echo $HOME").stdout.strip()
    tree = f"{home}/goat-rig/builds/{args.build.replace('/', '-')}"
    replay, tools = f"{tree}/build/linux_x86_64/halo_replay", f"{tree}/tools"
    say(f"rt2_effect_k: {args.build}'s replay built on {host}")
    work = f"{REMOTE}/measure-{args.build.replace('/', '-')}-{time.strftime('%Y%m%d-%H%M%S')}"
    ssh(host, f"mkdir -p ~/{work}")
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", __file__, f"{rig_target(host)}:{work}/rt2_effect_k.py"],
                   check=True)
    combined = {"build": args.build, "rule": table(), "results": []}
    for name in names:
        path, first, last, _ = RECORDINGS[name]
        owner_check(host)
        command = (f"cd ~/{work} && python3 rt2_effect_k.py --remote --replay {shlex.quote(replay)} "
                   f"--recording ~/{path} --tools {shlex.quote(tools)} --moment {name} --first {first} "
                   f"--last {last} --per-kind {args.per_kind} --plays {args.plays} --out ~/{work}/out")
        # (the GPU lock shared, one of its picture slots, as goat-rig run --shared; the owner's game not running)
        script = "\n".join([
            "set -e", "mkdir -p ~/goat-rig/locks", "exec 9>>~/goat-rig/locks/gpu.lock", "flock -s 9",
            "while :; do for s in 0 1 2; do exec 8>>~/goat-rig/locks/gpu-shared-$s.lock; "
            "if flock -n 8; then break 2; fi; done; sleep 0.5; done",
            f'if pgrep -f "{OWNER_GAME}" >/dev/null; then echo "the owner is playing" >&2; exit 3; fi',
            command])
        say(f"rt2_effect_k: measuring {name} on {host}")
        reply = ssh(host, script)
        combined["results"] += json.loads(reply.stdout.strip().splitlines()[-1])
    print(f"{'recording':10} {'frame':>5} {'kind':44} {'reach':>5} {'life':>4} {'k need':>7} {'k shad':>7} "
          f"{'band':>6} {'k build':>7} {'build/cl':>8} {'90% c/p':>11}  by base")
    for r in combined["results"]:
        bases = ", ".join(f"{label} {v['k_needed']}" for label, v in r["by_base"].items() if v["k_needed"])
        print(f"{r['recording']:10} {r['frame']:>5} {r['kind']:44} {r['range']:>5.2f} {r['life_frames']:>4} "
              f"{r['k_needed']!s:>7} {r['k_needed_with_shadows']!s:>7} {r['k_band_0.4_0.6']!s:>6} "
              f"{r['k_build']:>7} {r['build_over_classic']!s:>8} "
              f"{r['reach_90_classic']!s:>5}/{r['reach_90_pass']!s:<5}  {bases}")
    if args.json:
        Path(args.json).write_text(json.dumps(combined, indent=1) + "\n")
    ssh(host, f"rm -rf ~/{work}", check=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
