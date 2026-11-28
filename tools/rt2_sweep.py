#!/usr/bin/env python3
"""The ray-traced lighting's problem finder: tools/rt2_bench.py's triage on every scene and region of the scene
library (tools/rt2_scenes.json), ranked.

    python3 tools/rt2_sweep.py --build master --out DIR [--hosts 5900x,omarchy] [--scenes a,b] [--record-only]
    python3 tools/rt2_sweep.py --out DIR --rank          the table again from what DIR holds
    python3 tools/rt2_sweep.py --out DIR --frames        each scene's moment frame (classic | setting on) to
                                                         DIR/frames, for picking its regions
    python3 tools/rt2_sweep.py --sizes                   the library's recordings on each rig
    python3 tools/rt2_sweep.py compare --base master --head my-branch [--out DIR]
                                                         the merge-time check: only what changed beyond noise

Each scene is a workbench moment (rt2_clips.MOMENTS: its own moments, and the library's scene_* ones loaded into it
from tools/rt2_scenes.json) with a few named regions (x,y,w,h at 1280x720). For each scene, on one rig:
  1. recorded twice (rt2_bench.py record): the menu setting on, and --setting-off (the classic look), in
     ~/goat-rig/rt2-bench/sweep-<scene>[-classic], both at once (the rig's shared GPU slots);
  2. triaged (rt2_bench.py triage --classic, every region in one triage, --json): per region the ESTIMATOR gap
     (realtime against the converged reference: bias, motion, noise), the denoiser's history cut, the reference's
     light by term, and the DATA/LOOK gap (the reference against classic, presented).
Then ranked: the biggest estimator problems and the biggest data/look gaps, with a thumbnail of each region
(classic | setting on | reference | error), in DIR/sweep.md and DIR/sweep.html.

Unattended: one worker a rig (--hosts), each taking the next scene not done; a scene recorded on a rig stays with
it. Before each step the rig's owner is checked (the 5900X: its owner's own game; a worker waits while it runs, the
others go on) and its disk (--keep-free GB free at least, the library's recordings at most --cap GB; omarchy's
btrfs metadata too). Progress is kept in DIR/state.json and each scene's DIR/<scene>/triage.json: run it again and
it resumes where it stopped (a failed step is tried again, twice at most). bosgame (rt2_bench --host bosgame) can
record and view but not triage (numpy on the rig), so it is not a sweep host.

compare: the library's recordings (as the sweep left them, each on its rig) drawn by two builds' replays, base and
head (rt2_bench triage --build, the same recording): the light pass's GLSL and the backend's C are compared; a change
of the game's side (lt_lights.c, the object lamps' gather) needs the library recorded again with the head build
(a sweep of its own with --build head into another --out). Each build's triages are kept under DIR/compare/<commit>/, so a base already
measured (master at that commit) is reused. Printed: only the regions whose realtime light, estimator gap or
data/look gap moved by more than the noise threshold (CHANGE, below), each with a thumbnail pair (base | head:
reference, error), and one verdict line.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import html
import json
import math
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import rt2_clips  # noqa: E402

BENCH = ROOT / "tools" / "rt2_bench.py"
LIBRARY = ROOT / "tools" / "rt2_scenes.json"
REMOTE = "goat-rig/rt2-bench"
PREFIX = "sweep-"
LOCK = threading.Lock()
# a region is cut into regions of the picture by default when the manifest names none (thirds: floor/middle/top)
DEFAULT_REGIONS = {"floor": [440, 540, 400, 140], "middle": [440, 300, 400, 120], "top": [440, 60, 400, 120]}
LIMITS = {"5900x": (60, 120), "omarchy": (40, 80)}  # (free GB kept, the library's recordings GB at most)
RECORD_SECONDS = 200  # a recording's run at most (real seconds; the library's take about 60)
RECORDING_GB = 3.0  # a scene's two recordings, the most expected


def say(*words):
    with LOCK:
        print(time.strftime("%H:%M:%S"), *words, file=sys.stderr, flush=True)


def library():
    doc = json.loads(LIBRARY.read_text())
    scenes = {}
    for name, entry in {**doc["moments"], **doc["scenes"]}.items():
        if name not in rt2_clips.MOMENTS:
            raise SystemExit(f"rt2_sweep: {name}: not a moment of rt2_clips.MOMENTS")
        scenes[name] = {"regions": entry.get("regions") or DEFAULT_REGIONS, "tags": entry.get("tags", []),
                        "what": rt2_clips.MOMENTS[name][5], "named": bool(entry.get("regions"))}
    return scenes


def ssh(host, script, check=False):
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", rt2_clips.rig_target(host), "bash", "-s"], input=script,
                            text=True, capture_output=True)
    if check and result.returncode:
        raise RuntimeError(f"{host}: {result.stderr.strip()[-400:]}")
    return result.stdout


class State:
    """DIR/state.json: each scene's rig, its steps done, its failures"""

    def __init__(self, out):
        self.path = out / "state.json"
        self.data = json.loads(self.path.read_text()) if self.path.exists() else {"scenes": {}, "seconds": 0}

    def scene(self, name):
        return self.data["scenes"].setdefault(name, {"host": None, "recorded": False, "triaged": False,
                                                     "failures": 0})

    def save(self):
        with LOCK:
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps(self.data, indent=1))
            tmp.replace(self.path)


def owner_playing(host):
    return rt2_clips.owner_playing(host) if host != "omarchy" else ""


def disk_ok(host):
    """the rig's free space above its floor and the library's recordings under their cap (GB); omarchy's btrfs
    metadata with room"""
    keep, cap = LIMITS.get(host, (40, 60))
    text = ssh(host, f"df -B1G --output=avail ~ | tail -1; du -s -B1G ~/{REMOTE}/{PREFIX}* 2>/dev/null | "
                     "awk '{s+=$1} END {print s+0}'; btrfs filesystem df ~ 2>/dev/null | "
                     "awk -F'[=,]' '/^Metadata/ {print $3, $5}'")
    lines = text.split("\n")
    free, used = float(lines[0]), float(lines[1])
    if free - RECORDING_GB < keep or used + RECORDING_GB > cap:
        return False, f"{free:.0f} GB free (keep {keep}), the library {used:.0f} GB (cap {cap})"
    if len(lines) > 2 and lines[2].strip():
        total, used_meta = (float(v.strip().rstrip("GiB").rstrip("MiB")) for v in lines[2].split())
        if total - used_meta < 0.3:
            return False, f"btrfs metadata {used_meta:.2f} of {total:.2f} GiB"
    return True, f"{free:.0f} GB free, the library {used:.0f} GB"


def bench(*argv, log=None):
    result = subprocess.run([sys.executable, str(BENCH), *argv], capture_output=True, text=True)
    if log:
        log.write_text(result.stdout + result.stderr)
    if result.returncode:
        raise RuntimeError((result.stdout + result.stderr).strip().splitlines()[-1:] or ["failed"])
    return result


def recorded(host, scene):
    text = ssh(host, f"for n in {PREFIX}{scene} {PREFIX}{scene}-classic; do test -s ~/{REMOTE}/$n/meta.json && "
                     "echo $n; done")
    return len(text.split()) == 2


def record(host, build, scene, out):
    def one(off):
        name = f"{PREFIX}{scene}" + ("-classic" if off else "")
        bench("record", "--moment", scene, "--build", build, "--host", host, "--name", name, "--seconds",
              str(RECORD_SECONDS), *(["--setting-off"] if off else []),
              log=out / scene / f"record{'-classic' if off else ''}.log")
    (out / scene).mkdir(parents=True, exist_ok=True)
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            for future in [pool.submit(one, False), pool.submit(one, True)]:
                future.result()
    except Exception:
        # (a moment that never reaches its second records on until the run's limit: gigabytes, removed)
        ssh(host, f"rm -rf ~/{REMOTE}/{PREFIX}{scene} ~/{REMOTE}/{PREFIX}{scene}-classic")
        raise
    fetch_frames(host, scene, out)


def fetch_frames(host, scene, out):
    """the moment's frame, the game's own pictures with the setting on and off, to DIR/<scene>/on.png, off.png"""
    meta = json.loads(ssh(host, f"cat ~/{REMOTE}/{PREFIX}{scene}/meta.json", check=True))
    classic = json.loads(ssh(host, f"cat ~/{REMOTE}/{PREFIX}{scene}-classic/meta.json", check=True))
    # (the classic recording's frame at the same game second, counted from the end as triage does: its nearest
    # game picture)
    aligned = meta["frame"] - (meta["frames"] - classic["frames"])
    meta["classic_shot"] = min(classic["shots"], key=lambda n: abs(n - aligned))
    for side, name, frame in (("on", f"{PREFIX}{scene}", meta["frame"]),
                              ("off", f"{PREFIX}{scene}-classic", meta["classic_shot"])):
        subprocess.run(["scp", "-q", "-o", "BatchMode=yes",
                        f"{rt2_clips.rig_target(host)}:{REMOTE}/{name}/game/frame_{frame:05d}.png",
                        str(out / scene / f"{side}.png")], capture_output=True)
    (out / scene / "meta.json").write_text(json.dumps(meta, indent=1))


def triage(host, build, scene, regions, out, plays, term_plays):
    folder = out / scene
    folder.mkdir(parents=True, exist_ok=True)
    argv = ["triage", "--moment", scene, "--build", build, "--host", host, "--name", f"{PREFIX}{scene}",
            "--classic", f"{PREFIX}{scene}-classic", "--json", "--out", str(folder),
            "--plays", str(plays), "--term-plays", str(term_plays)]
    for box in regions.values():
        argv += ["--region", ",".join(str(v) for v in box)]
    bench(*argv, log=folder / "triage.log")
    if not (folder / "triage.json").exists():
        raise RuntimeError("no triage.json")


def worker(host, args, scenes, state, out):
    while True:
        with LOCK:
            if args.wait_regions:
                # (the manifest read again: regions named while the sweep runs are taken up)
                fresh = library()
                for name in scenes:
                    scenes[name].update(fresh.get(name, {}))
            pick, waiting = None, False
            for name in scenes:
                s = state.scene(name)
                if s["triaged"] or (args.record_only and s["recorded"]) or s["failures"] >= 2:
                    continue
                if args.wait_regions and s["recorded"] and not scenes[name]["named"]:
                    waiting = waiting or s["host"] == host
                    continue
                if s["host"] == host:
                    pick = name
                    break
                if s["host"] is None and pick is None and not s.get("taken"):
                    pick = name
            if pick is None and not waiting:
                return
            if pick is not None:
                s = state.scene(pick)
                s["taken"] = True
        if pick is None:
            time.sleep(30)
            continue
        with LOCK:
            s = state.scene(pick)
            s["taken"] = True
        try:
            while owner_playing(host):
                say(f"{host}: its owner is playing; waiting")
                time.sleep(60)
            if not s["recorded"]:
                if s["host"] is None and recorded(host, pick):
                    s["host"] = host
                    s["recorded"] = True
                    fetch_frames(host, pick, out)
                else:
                    ok, why = disk_ok(host)
                    if not ok:
                        say(f"{host}: disk: {why}; this rig takes no more recordings")
                        s["taken"] = False
                        return
                    s["host"] = host
                    state.save()
                    started = time.monotonic()
                    say(f"{host}: recording {pick} ({why})")
                    record(host, args.build, pick, out)
                    s["recorded"] = True
                    s["record_seconds"] = round(time.monotonic() - started)
                state.save()
            if not args.record_only:
                started = time.monotonic()
                say(f"{host}: triage {pick} ({len(scenes[pick]['regions'])} regions)")
                triage(host, args.build, pick, scenes[pick]["regions"], out, args.plays, args.term_plays)
                s["triaged"] = True
                s["triage_seconds"] = round(time.monotonic() - started)
                s["regions"] = scenes[pick]["regions"]
                say(f"{host}: {pick} triaged in {s['triage_seconds']} s")
        except Exception as error:  # (a step failed: kept, tried again on the next pass or run)
            s["failures"] += 1
            s["error"] = str(error)[-400:]
            say(f"{host}: {pick} failed ({s['failures']}): {s['error']}")
        finally:
            s["taken"] = False
            state.save()


# ---------- the ranking

def rows_of(scene, regions, folder):
    """one row a region from DIR/<scene>/triage.json (the measures as triage prints them)"""
    doc = json.loads((folder / "triage.json").read_text())
    rows = []
    names = list(regions)
    for i, r in enumerate(doc["regions"][1:]):
        ref = r["reference"]["mean"] or 1e-12
        rt, held = r["realtime"]["mean"], r["held"]
        none = r["term_none"]["mean"]
        terms = {t: r[f"term_{t}"]["mean"] - none for t in ("sun", "sky", "lights", "flashlight", "solved_lamps",
                                                             "object_lamps")}
        terms["bounce"] = r["term_all"]["mean"] - r["term_direct"]["mean"]
        terms["unmasked"] = none
        total = r["term_all"]["mean"] or 1e-12
        top = sorted(terms.items(), key=lambda kv: -abs(kv[1]))
        d = doc["denoiser"][i]
        row = {"scene": scene, "region": names[i] if i < len(names) else f"region {i + 1}",
               "box": r["box"], "estimator": rt / ref - 1, "bias": held["mean"] / ref - 1,
               "motion": (rt - held["mean"]) / ref, "noise": held["noise"] / ref,
               "ref_err": (r["reference"]["stderr"] or 0) / ref, "cut_share": d["mean"].get("cut_share") or 0,
               "history": d["mean"].get("count"), "terms": [(k, v / total) for k, v in top[:3]],
               "reference": ref}
        if doc.get("classic") and doc.get("game"):
            c, g = doc["classic"][i]["mean"], doc["game"][i]["mean"]
            row["classic"], row["setting_on"] = c, g
            # (where the setting's light is black the game's own picture against classic: -100% when it is)
            row["data"] = ((g * ref / rt) / c - 1 if rt > 0 else g / c - 1) if c > 0 else None
            row["game_vs_classic"] = g / c - 1 if c else None
        rows.append(row)
    return rows


def estimator_score(row):
    """how far the real-time light strays from the converged reference: the larger of its error and its noise,
    less what the reference's own uncertainty allows (as shares of the reference)"""
    return max(abs(row["estimator"]) - 2 * row["ref_err"], row["noise"], 0)


def estimator_kind(row):
    parts = {"bias": abs(row["bias"]), "motion": abs(row["motion"]), "noise": row["noise"]}
    return max(parts, key=parts.get)


def data_score(row):
    d = row.get("data")
    return abs(math.log(max(1 + d, 0.01))) if d is not None else 0


def thumbnail(folder, row, index):
    """classic | setting on | reference | error, the region's crop (a little around it), side by side"""
    x, y, w, h = (int(v) for v in row["box"])
    pad = 16
    x0, y0 = max(0, x - pad), max(0, y - pad)
    w0, h0 = min(1280 - x0, w + 2 * pad), min(720 - y0, h + 2 * pad)
    sources = [folder / n for n in ("off.png", "on.png", "reference.png", "error.png")]
    sources = [s for s in sources if s.exists()]
    if not sources:
        return None
    name = f"thumb_{index}.png"
    scale = max(1, round(160 / max(h0, 1)))
    inputs, graph = [], []
    for i, s in enumerate(sources):
        inputs += ["-i", str(s)]
        graph.append(f"[{i}:v]scale=1280:720,crop={w0}:{h0}:{x0}:{y0},scale=iw*{scale}:ih*{scale}:flags=neighbor,"
                     f"pad=iw+4:ih:0:0:white[v{i}]")
    graph.append("".join(f"[v{i}]" for i in range(len(sources))) + f"hstack=inputs={len(sources)}[out]")
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", *inputs, "-filter_complex", ";".join(graph),
                    "-map", "[out]", str(folder / name)], capture_output=True)
    return name if (folder / name).exists() else None


def pct(value):
    return "-" if value is None else f"{value * 100:+.0f}%"


def rank(out, scenes, state, top=15):
    rows = []
    for scene in scenes:
        folder = out / scene
        if (folder / "triage.json").exists():
            regions = state.scene(scene).get("regions") or scenes[scene]["regions"]
            rows += rows_of(scene, regions, folder)
    estimator = sorted(rows, key=estimator_score, reverse=True)
    data = sorted(rows, key=data_score, reverse=True)
    for i, row in enumerate(rows):
        row["thumb"] = thumbnail(out / row["scene"], row, i)
    (out / "rows.json").write_text(json.dumps(rows, indent=1))
    done = [s for s in scenes if state.scene(s)["triaged"]]
    seconds = state.data.get("seconds", 0)
    md = [f"# rt2 sweep: {len(done)} of {len(scenes)} scenes, {len(rows)} regions",
          f"Total runtime {seconds / 60:.0f} min (wall, all rigs). Estimator = realtime against the converged "
          "reference (the real-time shortcuts' error); data/look = the reference against the classic look "
          "(the lights' data and the models). Thumbnails: classic | setting on | reference | error "
          "(grey 128 = no error).", "",
          "## Biggest ESTIMATOR problems", "",
          "| # | scene | region | error | bias | motion | noise | history cut | history (frames) | top terms |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for i, r in enumerate(estimator[:top]):
        md.append(f"| {i + 1} | {r['scene']} | {r['region']} | {pct(r['estimator'])} | {pct(r['bias'])} | "
                  f"{pct(r['motion'])} | {r['noise'] * 100:.0f}% | {r['cut_share']:.2f} | "
                  f"{r['history'] if r['history'] is None else f'{r['history']:.1f}'} | "
                  + ", ".join(f"{k} {v * 100:.0f}%" for k, v in r["terms"]) + " |")
    md += ["", "## Biggest DATA/LOOK gaps", "",
           "| # | scene | region | reference vs classic | game vs classic | dominant term | top terms |",
           "|---|---|---|---|---|---|---|"]
    for i, r in enumerate(data[:top]):
        md.append(f"| {i + 1} | {r['scene']} | {r['region']} | {pct(r.get('data'))} | "
                  f"{pct(r.get('game_vs_classic'))} | {r['terms'][0][0]} | "
                  + ", ".join(f"{k} {v * 100:.0f}%" for k, v in r["terms"]) + " |")
    (out / "sweep.md").write_text("\n".join(md) + "\n")

    def table(title, ranked, cells):
        head = "".join(f"<th>{html.escape(c)}</th>" for c in ["#", "scene", "region", *cells[0], "thumbnail"])
        body = []
        for i, r in enumerate(ranked[:top]):
            thumb = f'<img src="{r["scene"]}/{r["thumb"]}">' if r.get("thumb") else ""
            body.append(f"<tr><td>{i + 1}</td><td>{html.escape(r['scene'])}<br><small>"
                        f"{html.escape(scenes[r['scene']]['what'])}</small></td><td>{html.escape(r['region'])}"
                        f"<br><small>{','.join(f'{v:g}' for v in r['box'])}</small></td>" +
                        "".join(f"<td>{html.escape(c)}</td>" for c in cells[1](r)) + f"<td>{thumb}</td></tr>")
        return f"<h2>{title}</h2><table><tr>{head}</tr>{''.join(body)}</table>"

    page = ["<!doctype html><meta charset=utf-8><title>rt2 sweep</title><style>body{font:14px sans-serif;"
            "background:#fff;color:#111;margin:16px}td,th{border:1px solid #ccc;padding:4px;vertical-align:top}"
            "table{border-collapse:collapse}img{max-width:720px}small{color:#555}</style>",
            f"<h1>rt2 sweep: {len(done)} of {len(scenes)} scenes, {len(rows)} regions</h1>",
            f"<p>{html.escape(md[1])}</p>",
            table("Biggest ESTIMATOR problems", estimator,
                  (["error", "kind", "bias", "motion", "noise", "history cut", "top terms"],
                   lambda r: [pct(r["estimator"]), estimator_kind(r), pct(r["bias"]), pct(r["motion"]),
                              f"{r['noise'] * 100:.0f}%", f"{r['cut_share']:.2f}",
                              ", ".join(f"{k} {v * 100:.0f}%" for k, v in r["terms"])])),
            table("Biggest DATA/LOOK gaps", data,
                  (["reference vs classic", "game vs classic", "dominant term", "top terms"],
                   lambda r: [pct(r.get("data")), pct(r.get("game_vs_classic")), r["terms"][0][0],
                              ", ".join(f"{k} {v * 100:.0f}%" for k, v in r["terms"])]))]
    (out / "sweep.html").write_text("\n".join(page) + "\n")
    say(f"rt2_sweep: {out / 'sweep.md'}, {out / 'sweep.html'}")
    return estimator, data


# ---------- compare

# a region changed when a measure moved by more than this (a share of the base's value): the triage of one build
# repeated on the same recording moves the realtime light 0 (the replay is exact), the reference by its stderr
# (under 1% converged), the held noise and the history cut a little; 3% leaves room for the reference's error at
# compare's shorter accumulation (see the PR: measured on repeat runs)
CHANGE = 0.03
COMPARE_PLAYS, COMPARE_TERM_PLAYS = 600, 60


def commit_of(host, ref):
    from rt2_scorecard import build_folder
    return ssh(host, f"git -C ~/goat-rig/builds/{build_folder(ref)} rev-parse --short=10 HEAD", check=True).strip()


def compare_worker(host, sides, scenes, state, out, args, results):
    """sides: (side, ref, repeat); each side's triages under DIR/compare/<commit>-p<plays>[-repeat]/<scene>, kept
    and reused while the scene's regions are the same"""
    import rt2_bench
    plays, term_plays = args.plays or COMPARE_PLAYS, args.term_plays or COMPARE_TERM_PLAYS
    folders = {}
    for side, ref, repeat in sides:
        rt2_bench.build(host, ref, game=False)
        folders[side] = (ref, out / "compare" / (f"{commit_of(host, ref)}-p{plays}" + ("-repeat" if repeat else "")))
    while True:
        with LOCK:
            pick = next((n for n in scenes if state.scene(n)["host"] == host and n not in results), None)
            if pick is None:
                return
            results[pick] = None
        regions = json.dumps(scenes[pick]["regions"])
        for side, (ref, folder) in folders.items():
            kept = folder / pick / "regions.json"
            if (folder / pick / "triage.json").exists() and kept.exists() and kept.read_text() == regions:
                continue
            while owner_playing(host):
                say(f"{host}: its owner is playing; waiting")
                time.sleep(60)
            started = time.monotonic()
            try:
                triage(host, ref, pick, scenes[pick]["regions"], folder, plays, term_plays)
                kept.write_text(regions)
                say(f"{host}: {pick} {side} ({ref}, {folder.name}) in {time.monotonic() - started:.0f} s")
            except Exception as error:
                say(f"{host}: {pick} {side} ({ref}) failed: {error}")
        results[pick] = {side: folder / pick for side, (ref, folder) in folders.items()}


def measures(b, h):
    """what a compare looks at, head against base: the realtime light (its mean, a share), the estimator gap and
    the data/look gap (their scores' change)"""
    return {"realtime light": (h["reference"] * (1 + h["estimator"])) / (b["reference"] * (1 + b["estimator"])) - 1,
            "estimator gap": estimator_score(h) - estimator_score(b),
            "data/look gap": data_score(h) - data_score(b)}


def pair_thumb(base, head, row, path):
    """base | head, each reference then error, the region's crop"""
    x, y, w, h = (int(v) for v in row["box"])
    pad = 16
    x0, y0 = max(0, x - pad), max(0, y - pad)
    w0, h0 = min(1280 - x0, w + 2 * pad), min(720 - y0, h + 2 * pad)
    sources = [base / "reference.png", base / "error.png", head / "reference.png", head / "error.png"]
    if not all(p.exists() for p in sources):
        return None
    scale = max(1, round(120 / max(h0, 1)))
    inputs, graph = [], []
    for i, src in enumerate(sources):
        inputs += ["-i", str(src)]
        graph.append(f"[{i}:v]scale=1280:720,crop={w0}:{h0}:{x0}:{y0},scale=iw*{scale}:ih*{scale}:flags=neighbor,"
                     f"pad=iw+{12 if i == 1 else 4}:ih:0:0:white[v{i}]")
    graph.append("[v0][v1][v2][v3]hstack=inputs=4[out]")
    subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", *inputs, "-filter_complex", ";".join(graph),
                    "-map", "[out]", str(path)], capture_output=True)
    return path if path.exists() else None


def compare(args, scenes, state, out, hosts):
    # (--noise: the base against itself, a second triage of each scene: what a compare moves with no change)
    sides = [("base", args.base, False), ("head", args.base if args.noise else args.head, args.noise)]
    started = time.monotonic()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
        for future in [pool.submit(compare_worker, h, sides, scenes, state, out, args, results) for h in hosts]:
            future.result()
    took = time.monotonic() - started
    head_name = "noise" if args.noise else args.head
    report = out / "compare" / f"{args.base.replace('/', '-')}..{head_name.replace('/', '-')}"
    report.mkdir(parents=True, exist_ok=True)
    changed, better, worse, measured = [], 0.0, 0.0, 0
    for scene, folders in results.items():
        if not folders or not all((f / "triage.json").exists() for f in folders.values()):
            continue
        regions = scenes[scene]["regions"]
        base_rows = rows_of(scene, regions, folders["base"])
        head_rows = rows_of(scene, regions, folders["head"])
        for b, h in zip(base_rows, head_rows):
            measured += 1
            light = h["reference"] and b["reference"]
            moves = measures(b, h) if light else {}
            if not any(abs(v) > CHANGE for v in moves.values()):
                continue
            # (better: the gaps closed; the light's own change alone is neither)
            score = moves["estimator gap"] + moves["data/look gap"]
            better += max(0.0, -score)
            worse += max(0.0, score)
            thumb = pair_thumb(folders["base"], folders["head"], h, report / f"{scene}_{len(changed)}.png")
            changed.append((scene, h["region"], moves, score, thumb))
    if args.noise:
        largest = {}
        for scene, folders in results.items():
            if folders and all((f / "triage.json").exists() for f in folders.values()):
                for b, h in zip(rows_of(scene, scenes[scene]["regions"], folders["base"]),
                                rows_of(scene, scenes[scene]["regions"], folders["head"])):
                    for k, v in measures(b, h).items():
                        if abs(v) >= abs(largest.get(k, (0, ""))[0]):
                            largest[k] = (v, f"{scene} {h['region']}")
        print("rt2_sweep compare --noise (the base triaged twice): the largest move of each measure: " +
              "; ".join(f"{k} {v * 100:+.2f}% ({where})" for k, (v, where) in largest.items()))
    print(f"rt2_sweep compare {args.base} -> {head_name}: {measured} regions measured, {len(changed)} changed "
          f"beyond {CHANGE * 100:.0f}% ({took / 60:.1f} min)")
    for scene, region, moves, score, thumb in sorted(changed, key=lambda c: -abs(c[3])):
        words = ", ".join(f"{k} {v * 100:+.0f}%" for k, v in moves.items() if abs(v) > CHANGE)
        print(f"  {'better' if score < 0 else 'worse' if score > 0 else 'moved'}: {scene} {region}: {words}"
              + (f"  [{thumb}]" if thumb else ""))
    verdict = ("unchanged" if not changed else "better" if better > worse * 1.5 else
               "worse" if worse > better * 1.5 else "mixed")
    print(f"VERDICT: {verdict} (gaps closed {better * 100:.0f} points, opened {worse * 100:.0f}; thumbnails base | "
          f"head, each the reference then the error)")
    return 0 if verdict != "worse" else 1


def sizes(hosts):
    for host in hosts:
        text = ssh(host, f"du -sc -B1M ~/{REMOTE}/{PREFIX}* 2>/dev/null | tail -1 | cut -f1; "
                         f"ls -d ~/{REMOTE}/{PREFIX}* 2>/dev/null | wc -l; df -h --output=avail ~ | tail -1")
        mb, count, free = (text.split() + ["0", "0", "?"])[:3]
        print(f"{host}: {count} recordings, {int(mb) / 1024:.1f} GB; {free} free")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("what", nargs="?", choices=["sweep", "compare"], default="sweep")
    parser.add_argument("--base", help="compare: the base build (goat-rig build's ref)")
    parser.add_argument("--head", help="compare: the build compared with it")
    parser.add_argument("--noise", action="store_true", help="compare: the base against itself (the threshold's "
                        "measure: every move printed, the largest per measure)")
    parser.add_argument("--build", default="master", help="the build (goat-rig build's ref)")
    parser.add_argument("--out", default=str(ROOT / "build" / "rt2_sweep"), help="the results' folder")
    parser.add_argument("--hosts", default="5900x,omarchy", help="the rigs, a comma between")
    parser.add_argument("--scenes", help="only these scenes, a comma between")
    parser.add_argument("--wait-regions", action="store_true",
                        help="a recorded scene with no regions in the manifest waits for them (else the defaults)")
    parser.add_argument("--record-only", action="store_true", help="record the scenes, no triage")
    parser.add_argument("--rank", action="store_true", help="only the table, from what --out holds")
    parser.add_argument("--frames", action="store_true", help="only each recorded scene's moment frames")
    parser.add_argument("--sizes", action="store_true", help="the library's recordings on each rig")
    parser.add_argument("--retry", action="store_true", help="failed scenes tried again (their failures cleared)")
    parser.add_argument("--plays", type=int, help="triage's reference plays (2000; compare 600)")
    parser.add_argument("--term-plays", type=int, help="triage's plays for each term (600; compare 60)")
    parser.add_argument("--top", type=int, default=15, help="rows in each table")
    args = parser.parse_args()
    hosts = [h for h in args.hosts.split(",") if h]
    if any(rt2_clips.windows(h) for h in hosts):
        parser.error("bosgame records and views but cannot triage (numpy on the rig): not a sweep host")
    if args.sizes:
        sizes(hosts)
        return 0
    out = Path(args.out).expanduser()
    out.mkdir(parents=True, exist_ok=True)
    scenes = library()
    if args.scenes:
        wanted = args.scenes.split(",")
        missing = [s for s in wanted if s not in scenes]
        if missing:
            parser.error(f"not in the library: {', '.join(missing)}")
        scenes = {s: scenes[s] for s in wanted}
    state = State(out)
    if args.what == "compare":
        if not (args.base and (args.head or args.noise)):
            parser.error("compare --base REF --head REF")
        return compare(args, {n: v for n, v in scenes.items() if state.scene(n)["recorded"]}, state, out, hosts)
    args.plays = args.plays or 2000
    args.term_plays = args.term_plays or 600
    if args.frames:
        for name in scenes:
            s = state.scene(name)
            if s["recorded"]:
                fetch_frames(s["host"], name, out)
        return 0
    if not args.rank:
        for name in scenes:
            s = state.scene(name)
            s["taken"] = False
            if args.retry:
                s["failures"] = 0
            # (regions changed since the triage: triaged again)
            if s["triaged"] and s.get("regions") != scenes[name]["regions"]:
                s["triaged"] = False
        state.save()
        started = time.monotonic()
        with concurrent.futures.ThreadPoolExecutor(max_workers=len(hosts)) as pool:
            for future in [pool.submit(worker, h, args, scenes, state, out) for h in hosts]:
                future.result()
        took = time.monotonic() - started
        state.data["seconds"] = state.data.get("seconds", 0) + took
        state.save()
        left = [n for n in scenes if not state.scene(n)["triaged"]]
        say(f"rt2_sweep: this run {took / 60:.1f} min, all runs {state.data['seconds'] / 60:.1f} min; "
            f"{len(scenes) - len(left)} of {len(scenes)} scenes done" + (f"; left: {', '.join(left)}" if left else ""))
        if args.record_only:
            return 0
    rank(out, scenes, state, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
