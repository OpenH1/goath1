#!/usr/bin/env python3
"""The emitter files' screen: every BSP with a solved lamp file (port/assets/emitters) looked at in the game, classic
against live, on lossless stills, and measured.

    python3 tools/rt2_emitter_screen.py --build local/my-branch --out DIR
    python3 tools/rt2_emitter_screen.py --build local/my-branch --bsps a50_bsp2,d20_bsp3 --out DIR
    python3 tools/rt2_emitter_screen.py --spots      the cameras picked again from the BSPs' meshes (tools/rt2_emitter_spots.json)

Each BSP gets two cameras inside rooms its lamps light (tools/rt2_emitter_spots.json): picked from the BSP's own
mesh (an rt2 scene export of it, tools/rt_truth/scenes.json's any_bsp recipe) beside its strongest lamp groups,
inside a closed room, looking where the lamps it can see are; the second at least 6 wu from the first (a more open room where a closed one gives too few). Each level is
played twice on the rig, as players run it (the menu setting alone, display.ray_traced_lighting off and on, the MCC
import linked, the HUD hidden), at a fixed frame time and the rig's fixed seed: after the level's opening (60 s on
the scenario's clock, past a skipped cutscene, which plays fast), each of its BSPs in turn by switch_bsp, the script's
camera at each spot and a lossless still 3 s later (debug.screenshot_at), so the denoiser's history has settled.

Measured per still pair (linear luminance, the sRGB stills decoded): the mean of the whole frame, of its lower 40%
(mostly floor) and of its middle band (mostly walls), live over classic; and the pixels that go near-black live where
classic shows them lit (live under 4% of white and under a third of classic, classic over 2%), as a share of the frame
and in the worst cell of a 4x4 grid. A BSP passes when at both spots the whole frame's ratio is within 0.6 to 1.6 and
no large region goes black (under 10% of the frame and under 50% of every cell), and something is in view
(classic's frame not flat (most of it one colour, as Halo's void outside the rooms is), nor the level's still
before it again: a camera outside the BSP's rooms, or a game
that stopped drawing new frames, is not a look); and no large
region blows out (live near white where classic is under 60% of white: under 3% of the frame). The lossless
stills stay on the rig in ~/goat-rig/rt2-emitter-screen/<build>/. It prints a table (the README's
rows), and copies results.json, a still pair per spot and a contact sheet of the pairs to --out. Runs on the 5900X by
default (its owner's own game checked before every run, as goat-rig does too); the rig's runs are removed.
"""

import argparse
import concurrent.futures
import json
import math
import shlex
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "tools" / "rig" / "goat-rig"
EMITTERS = ROOT / "port" / "assets" / "emitters"
SPOTS = ROOT / "tools" / "rt2_emitter_spots.json"
REMOTE = "goat-rig/rt2-emitter-screen"
OWNER_GAME = "^$HOME/Applications/OpenH1[^/ ]*/bin/openh1"
SIZE = "1280x720"
# each BSP's scene export on the rig (tools/rt_truth/scenes.json, any_bsp); a10's and c20's first from their scenes
EXPORTS = "goat-rig/rt2-emitters-more/{bsp}"
EXPORT_OTHERS = {"a10_bsp0": "goat-rig/rt2-scenes/a10_interior", "c20_bsp0": "goat-rig/rt2-scenes/c20_corner"}
OPENING = 60.0  # the scenario's second the first BSP is switched to (past every level's skipped opening)
EACH = 10.0  # seconds a BSP
PASS_RATIO = (0.6, 1.6)
BLACK_FRAME, BLACK_CELL = 0.10, 0.50
BLOWN = 0.03
LOCK = threading.Lock()


def say(*words):
    with LOCK:
        print(*words, file=sys.stderr, flush=True)


def target(host):
    hosts = {"omarchy": "omarchy.fabric", "5900x": "5900x.fabric"}
    path = Path.home() / ".config/goat-rig/hosts"
    if path.exists():
        for line in path.read_text().splitlines():
            if len(line.split()) == 2 and not line.startswith("#"):
                hosts[line.split()[0]] = line.split()[1]
    return hosts.get(host, host)


def ssh(host, script, check=True):
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", target(host), "bash", "-s"], input=script, text=True,
                            capture_output=True)
    if check and result.returncode:
        raise SystemExit(f"rt2_emitter_screen: {host}: {(result.stderr or result.stdout).strip()[-800:]}")
    return result.stdout


def bsp_files():
    return {path.stem: json.loads(path.read_text()) for path in sorted(EMITTERS.glob("*_bsp*.json"))}


# ---------- on the rig (numpy and PIL: the system's python3)

def rig_ply(path):
    import numpy as np
    data = open(path, "rb").read()
    end = data.index(b"end_header\n") + len(b"end_header\n")
    head = data[:end].decode().splitlines()
    nv = nf = props = 0
    cur, face_at = "", 0
    for index, line in enumerate(head):
        if line.startswith("element vertex"):
            nv, cur = int(line.split()[2]), "v"
        elif line.startswith("element face"):
            nf, cur = int(line.split()[2]), "f"
        elif line.startswith("property list"):
            face_at = index
        elif line.startswith("property") and cur == "v":
            props += 1
    v = np.frombuffer(data, "<f4", nv * props, end).reshape(nv, props)[:, :3].astype(np.float64)
    extra = [line for line in head[face_at + 1:] if line.startswith("property")]
    stride = 13 + 4 * len(extra)
    off = end + nv * props * 4
    faces = np.frombuffer(data, np.uint8, nf * stride, off).reshape(nf, stride)
    return v[faces[:, 1:13].copy().view("<i4").reshape(nf, 3)]


def rig_cast(tris, o, d):
    """the nearest hit's distance along d (inf: none)"""
    import numpy as np
    v0, e1, e2 = tris[:, 0], tris[:, 1] - tris[:, 0], tris[:, 2] - tris[:, 0]
    p = np.cross(d, e2)
    det = (e1 * p).sum(1)
    ok = np.abs(det) > 1e-9
    inv = np.where(ok, 1 / np.where(ok, det, 1), 0)
    t0 = o - v0
    u = (t0 * p).sum(1) * inv
    q = np.cross(t0, e1)
    w = (q * d).sum(1) * inv
    t = (q * e2).sum(1) * inv
    hit = ok & (u >= 0) & (w >= 0) & (u + w <= 1) & (t > 1e-3)
    return float(t[hit].min()) if hit.any() else math.inf


def rig_spots(bsp, emitters, ply):
    """two cameras (x, y, z, yaw) in rooms the BSP's lamps light"""
    import numpy as np
    tris = rig_ply(ply)
    doc = json.load(open(emitters))
    groups = []
    for entry in doc["lamps"]:
        t = np.array(entry["triangles"], float).reshape(-1, 3, 3)
        cross = np.cross(t[:, 1] - t[:, 0], t[:, 2] - t[:, 0])
        area = np.linalg.norm(cross, axis=1)
        r = entry["radiance"]
        power = (0.2126 * r[0] + 0.7152 * r[1] + 0.0722 * r[2]) * area.sum()
        centre = (t.mean(1) * area[:, None]).sum(0) / max(area.sum(), 1e-9)
        normal = cross.sum(0)
        normal /= np.linalg.norm(normal) + 1e-12
        groups.append((power, centre, normal))
    groups.sort(key=lambda g: -g[0])
    golden = (1 + 5 ** 0.5) / 2
    around = [np.array([math.cos(2 * math.pi * i / golden) * math.sqrt(1 - (1 - 2 * (i + .5) / 40) ** 2),
                        math.sin(2 * math.pi * i / golden) * math.sqrt(1 - (1 - 2 * (i + .5) / 40) ** 2),
                        1 - 2 * (i + .5) / 40]) for i in range(40)]
    def candidates_for(closed_least, ceiling_most, reach_least, ahead_least):
        found = []
        for power, centre, normal in groups[:30]:
            start = centre + normal * 0.3
            reach = rig_cast(tris, start, normal)
            if not math.isfinite(reach) or reach < reach_least:
                continue
            cam = start + normal * min(reach * 0.5, 2.0)
            if sum(1 for d in around if rig_cast(tris, cam, d) < 40) / len(around) < closed_least:
                continue
            if rig_cast(tris, cam, np.array([0, 0, 1.0])) > ceiling_most:
                continue
            down, up = rig_cast(tris, cam, np.array([0, 0, -1.0])), rig_cast(tris, cam, np.array([0, 0, 1.0]))
            if math.isfinite(down) and math.isfinite(up) and down + up > 1.2:
                cam = cam + np.array([0, 0, min(0.7, (down + up) / 2) - down])
            best = None
            for yaw in range(0, 360, 15):
                d = np.array([math.cos(math.radians(yaw)), math.sin(math.radians(yaw)), -0.15])
                d /= np.linalg.norm(d)
                ahead = rig_cast(tris, cam, d)
                if not math.isfinite(ahead) or ahead < ahead_least:
                    continue
                seen = 0.0
                for gp, gc, _ in groups[:80]:
                    v = gc - cam
                    dist = np.linalg.norm(v)
                    if dist < 1 or dist > 30 or (v / dist) @ d < math.cos(math.radians(35)):
                        continue
                    if rig_cast(tris, cam, v / dist) >= dist - 0.5:
                        seen += gp
                score = min(ahead, 25) * (1 + seen)
                if best is None or score > best[0]:
                    behind = rig_cast(tris, cam, -d)
                    step = min(behind * 0.7, 5) if math.isfinite(behind) else 0
                    place = cam - d * step
                    best = (score, [round(float(c), 2) for c in place], yaw)
            if best:
                found.append(best)
        return found

    # (inside a closed room first; where that finds too few, a more open or smaller one: a10's corridors are under
    # 2 wu tall)
    candidates = candidates_for(0.9, 12, 1.5, 4)
    if len(candidates) < 2:
        candidates += candidates_for(0.7, 30, 0.6, 2.5)
    candidates.sort(key=lambda c: -c[0])
    chosen = []
    for score, place, yaw in candidates:
        if all(math.dist(place, other["at"]) >= 6 for other in chosen):
            chosen.append({"at": place, "yaw": yaw})
        if len(chosen) == 2:
            break
    print(json.dumps({"bsp": bsp, "spots": chosen}))


def linear(picture):
    import numpy as np
    c = np.asarray(picture.convert("RGB"), np.float64) / 255.0
    c = np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)
    return 0.2126 * c[..., 0] + 0.7152 * c[..., 1] + 0.0722 * c[..., 2]


def rig_measure(folder):
    """results.json, the pairs and the sheet from the runs' stills (folder/plan.json names them)"""
    import numpy as np
    from PIL import Image, ImageDraw
    folder = Path(folder)
    plan = json.loads((folder / "plan.json").read_text())
    results, pairs = {}, []
    previous = {}
    for bsp, shots in plan["shots"].items():
        rows = []
        for spot, (classic_png, live_png) in enumerate(shots):
            classic_png, live_png = Path(classic_png).expanduser(), Path(live_png).expanduser()
            # (measured again from the copies kept here once the runs are gone)
            if not classic_png.exists():
                classic_png, live_png = (folder / f"{bsp}_spot{spot + 1}_{side}.png" for side in ("classic", "live"))
            if not classic_png.exists() or not live_png.exists():
                rows.append({"spot": spot, "missing": True})
                continue
            a, b = Image.open(classic_png), Image.open(live_png)
            ca, lb = linear(a), linear(b)
            h, w = ca.shape

            def ratio(rows_from, rows_to):
                c = ca[int(h * rows_from):int(h * rows_to)].mean()
                return float(lb[int(h * rows_from):int(h * rows_to)].mean() / c) if c > 1e-6 else 1.0
            black = (lb < 0.04) & (lb < ca / 3) & (ca > 0.02)
            cells = [float(black[i * h // 4:(i + 1) * h // 4, j * w // 4:(j + 1) * w // 4].mean())
                     for i in range(4) for j in range(4)]
            blown = (lb > 0.9) & (ca < 0.6)
            row = {"spot": spot, "overall": round(ratio(0, 1), 3), "floor": round(ratio(0.6, 1), 3),
                   "walls": round(ratio(0.3, 0.7), 3), "black": round(float(black.mean()), 3),
                   "blown": round(float(blown.mean()), 3),
                   "black_cell": round(max(cells), 3), "classic_mean": round(float(ca.mean()), 4)}
            # (a frame with nothing in view, the camera outside the BSP's rooms, or the run's last still again, the game
            # no longer drawing new frames: not a look at it)
            level = bsp.split("_bsp")[0]
            stale = level in previous and previous[level].shape == ca.shape and \
                float(np.abs(previous[level] - ca).mean()) < 0.002
            previous[level] = ca
            row["stale"] = bool(stale)
            # (Halo's void outside the BSP's rooms is one flat colour: most of the frame within a hair of its median)
            flat = float((np.abs(ca - np.median(ca)) < 0.002).mean()) > 0.6
            row["seen"] = bool(ca.std() > 0.003) and not flat and not stale
            row["pass"] = (row["seen"] and PASS_RATIO[0] <= row["overall"] <= PASS_RATIO[1]
                           and row["black"] < BLACK_FRAME and row["black_cell"] < BLACK_CELL and row["blown"] < BLOWN)
            rows.append(row)
            for side, picture, path in (("classic", a, classic_png), ("live", b, live_png)):
                if path.parent != folder:
                    picture.save(folder / f"{bsp}_spot{spot + 1}_{side}.png")
            pair = Image.new("RGB", (a.width * 2, a.height))
            pair.paste(a.convert("RGB"), (0, 0))
            pair.paste(b.convert("RGB"), (a.width, 0))
            draw = ImageDraw.Draw(pair)
            text = (f"{bsp} spot {spot + 1}: classic | live   overall x{row['overall']:.2f} floor x{row['floor']:.2f}"
                    f" walls x{row['walls']:.2f} black {row['black'] * 100:.0f}% (cell {row['black_cell'] * 100:.0f}%)"
                    f" blown {row['blown'] * 100:.0f}%"
                    f" {'PASS' if row['pass'] else 'FAIL' if row['seen'] else 'NOTHING IN VIEW'}")
            draw.rectangle((0, a.height - 34, pair.width, a.height), fill=(0, 0, 0))
            draw.text((10, a.height - 28), text, fill=(255, 255, 255))
            name = f"{bsp}_spot{spot + 1}.jpg"
            pair.save(folder / name, quality=90)
            pairs.append((bsp, name))
        ok = [r for r in rows if not r.get("missing")]
        # (every still of it a look that passes: one not seen, or not there, leaves the BSP not looked at)
        results[bsp] = {"spots": rows, "pass": bool(ok) and all(r["pass"] for r in ok)}
    (folder / "results.json").write_text(json.dumps(results, indent=1))
    # the contact sheet: each shipped (passing) BSP's pairs, 2 a row, at a third of their size
    shipped = [name for bsp, name in pairs if results[bsp]["pass"]]
    if shipped:
        tiles = [Image.open(folder / name) for name in shipped]
        tw, th = tiles[0].width // 3, tiles[0].height // 3
        sheet = Image.new("RGB", (tw * 2, th * math.ceil(len(tiles) / 2)))
        for index, tile in enumerate(tiles):
            sheet.paste(tile.resize((tw, th)), ((index % 2) * tw, (index // 2) * th))
        sheet.save(folder / "sheet_shipped.jpg", quality=88)
    print(json.dumps(results))


# ---------- on this computer

def owner_playing(host):
    return ssh(host, f'pgrep -fa "{OWNER_GAME}" || true').strip()


def push_script(host, folder):
    subprocess.run(["scp", "-q", "-o", "BatchMode=yes", str(Path(__file__)), f"{target(host)}:{folder}/screen.py"],
                   check=True)


def pick_spots(host, bsps):
    folder = f"{REMOTE}/spots"
    ssh(host, f"mkdir -p ~/{folder}")
    push_script(host, folder)
    for bsp in bsps:
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes", str(EMITTERS / f"{bsp}.json"),
                        f"{target(host)}:{folder}/{bsp}.json"], check=True)
    lines = []
    for bsp in bsps:
        export = EXPORT_OTHERS.get(bsp, EXPORTS.format(bsp=bsp))
        lines.append(f"nice /usr/bin/python3 screen.py --rig-spots {bsp} {bsp}.json ~/{export}/geometry/bsp.ply &")
    out = ssh(host, f"cd ~/{folder}\n" + "\n".join(lines) + "\nwait\n")
    ssh(host, f"rm -rf ~/{folder}")
    found = {}
    for line in out.splitlines():
        if line.startswith("{"):
            doc = json.loads(line)
            found[doc["bsp"]] = doc["spots"]
    return found


def cam(spot, pitch=-9):
    x, y, z = spot["at"]
    return f'debug_camera_set "{x} {y} {z} {spot["yaw"]} {pitch} 0 70"'


def level_plan(level, bsps, spots):
    """the cues and the stills' seconds of one level's run: [(bsp, spot index, still index)]"""
    # (the player kept alive: c40 opens with the player taking damage until it dies, which holds the game there)
    cues = [(1, "object_cannot_take_damage (players)"), (2, "ai_erase_all"),
            (OPENING - 3, "object_cannot_take_damage (players)"), (OPENING - 2, "ai_erase_all")]
    shots, order = [], []
    for index, bsp in enumerate(bsps):
        base = OPENING + index * EACH
        cues += [(base, "ai_erase_all"), (base + 0.5, f"switch_bsp {int(bsp.split('_bsp')[1])}")]
        for n, spot in enumerate(spots[bsp][:2]):
            at = base + 3 + n * 3.5
            cues.append((at, cam(spot)))
            shots.append(at + 3)
            order.append((bsp, n, len(shots) - 1))
    return cues, shots, order


def play(host, label, build, level, lighting, cues, shots, seconds):
    who = owner_playing(host)
    if who:
        raise SystemExit(f"rt2_emitter_screen: {host}'s owner is playing ({who}); not starting")
    env = ["HALO_SKIP_CINEMATICS=1", "HALO_NETWORK_TEST_START=2", "HALO_HIDE_HUD=1", "HALO_SPARTAN_LINK_HINT=true",
           "@link/mcc_import=~/mcc-import-data/mcc_import", f"HALO_RAY_TRACED_LIGHTING={1 if lighting else 0}",
           f"HALO_NETWORK_TEST=campaign:levels\\{level}\\{level}", "HALO_FRAME_TIME=0.0166666667",
           "HALO_NETWORK_TEST_CUES=" + "|".join(f"{t:g} {line}" for t, line in cues),
           "HALO_SCREENSHOT_AT=" + ",".join(f"{t:g}" for t in shots) + ",quit"]
    say(f"rt2_emitter_screen: playing {label} (ray-traced lighting {'on' if lighting else 'off'})")
    result = subprocess.run([sys.executable, str(RIG), f"@{host}", "run", label, "--build", build, "--shared",
                             "--size", SIZE, "--seconds", str(seconds), "--", *env], text=True, capture_output=True)
    if result.returncode:
        say(f"rt2_emitter_screen: {label}: exit {result.returncode}\n" + (result.stdout + result.stderr)[-1200:])
    return label


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", help="the build (a branch, sha or local/<name> goat-rig builds)")
    parser.add_argument("--bsps", help="only these BSPs (names as the files', separated by commas)")
    parser.add_argument("--host", default="5900x", choices=("5900x", "omarchy"))
    parser.add_argument("--out", help="a local folder for results.json, the pairs and the sheet")
    parser.add_argument("--spots", action="store_true", help="the cameras picked again (tools/rt2_emitter_spots.json)")
    parser.add_argument("--seconds", type=float, default=900, help="each run's limit in real seconds")
    parser.add_argument("--rig-spots", nargs=3, help=argparse.SUPPRESS)
    parser.add_argument("--rig-measure", help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.rig_spots:
        return rig_spots(*args.rig_spots)
    if args.rig_measure:
        return rig_measure(args.rig_measure)
    files = bsp_files()
    bsps = args.bsps.split(",") if args.bsps else list(files)
    for bsp in bsps:
        if bsp not in files:
            parser.error(f"{bsp}: no port/assets/emitters/{bsp}.json")
    spots = json.loads(SPOTS.read_text()) if SPOTS.exists() else {}
    missing = [b for b in bsps if b not in spots]
    if args.spots or missing:
        spots.update(pick_spots(args.host, bsps if args.spots else missing))
        SPOTS.write_text(json.dumps(dict(sorted(spots.items())), indent=1) + "\n")
        say(f"rt2_emitter_screen: spots written to {SPOTS.relative_to(ROOT)}")
    if not args.build:
        return 0
    result = subprocess.run([sys.executable, str(RIG), f"@{args.host}", "build", args.build], text=True,
                            capture_output=True)
    if result.returncode:
        raise SystemExit(f"rt2_emitter_screen: building {args.build} failed:\n{result.stdout[-1500:]}")
    folder = f"{REMOTE}/{args.build.replace('/', '-')}"
    ssh(args.host, f"rm -rf ~/{folder} && mkdir -p ~/{folder}")
    levels = {}
    for bsp in bsps:
        if spots.get(bsp):
            levels.setdefault(bsp.split("_bsp")[0], []).append(bsp)
        else:
            say(f"rt2_emitter_screen: {bsp}: no spot inside a lamp-lit room found; not looked at")
    plan, labels = {"shots": {}}, []
    jobs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
        for level, members in levels.items():
            members.sort(key=lambda b: int(b.split("_bsp")[1]))
            cues, shots, order = level_plan(level, members, spots)
            side = {}
            for lighting in (False, True):
                label = f"screen-{level}-{'live' if lighting else 'classic'}"
                side[lighting] = label
                labels.append(label)
                jobs.append(pool.submit(play, args.host, label, args.build, level, lighting, cues, shots,
                                        args.seconds))
            for bsp, n, still in order:
                plan["shots"].setdefault(bsp, []).append(
                    [f"~/goat-rig/runs/{side[False]}/game/shots/at{still}.png",
                     f"~/goat-rig/runs/{side[True]}/game/shots/at{still}.png"])
        for job in jobs:
            job.result()
    ssh(args.host, f"cat > ~/{folder}/plan.json <<'EOF'\n{json.dumps(plan)}\nEOF\n")
    push_script(args.host, folder)
    out = ssh(args.host, f"cd ~/{folder} && /usr/bin/python3 screen.py --rig-measure ~/{folder}")
    results = json.loads([line for line in out.splitlines() if line.startswith("{")][-1])
    ssh(args.host, "rm -rf " + " ".join(f"~/goat-rig/runs/{shlex.quote(label)}" for label in labels))
    if args.out:
        local = Path(args.out).expanduser()
        local.mkdir(parents=True, exist_ok=True)
        # (the pairs, the sheet and the numbers; the lossless stills stay on the rig, in its folder)
        subprocess.run(f"ssh -o BatchMode=yes {target(args.host)} 'cd ~/{folder} && tar cf - *.jpg results.json' | "
                       f"tar xf - -C {shlex.quote(str(local))}", shell=True)
    print("| BSP | spot | overall | floor | walls | black | worst cell | blown | verdict |")
    print("|---|---|---|---|---|---|---|---|---|")
    for bsp in bsps:
        for row in (results.get(bsp) or {}).get("spots", []):
            if row.get("missing"):
                print(f"| {bsp} | {row['spot'] + 1} | (no still) | | | | | | |")
                continue
            print(f"| {bsp} | {row['spot'] + 1} | x{row['overall']:.2f} | x{row['floor']:.2f} | x{row['walls']:.2f} | "
                  f"{row['black'] * 100:.0f}% | {row['black_cell'] * 100:.0f}% | {row['blown'] * 100:.0f}% | "
                  f"{'pass' if row['pass'] else 'FAIL' if row.get('seen', True) else 'nothing in view'} |")
    failed = [b for b in bsps if b in results and not results[b]["pass"]]
    say(f"rt2_emitter_screen: {len(results) - len(failed)} of {len(results)} BSPs pass; fail: {', '.join(failed) or 'none'}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
