#!/usr/bin/env python3
"""The lighting referee: measured checks that any build's Enhanced lights and ray-traced shadows must pass before
anyone looks at them (tools/lighting_referee.md).

    python3 tools/lighting_referee.py <branch|sha|local/name> [--machine omarchy] [--timing omarchy,5900x]
    python3 tools/lighting_referee.py master --only parity,shadow     a part of it
    python3 tools/lighting_referee.py master --mutate shadows-everywhere   a negative control: must fail

Every scene is staged the same way each run: a campaign level on this machine (cinematics skipped), the frame's time
fixed (debug.frame_time, 1/30 s), the random seed fixed (goat-rig's), the player placed and objects placed by cues at
fixed ticks, the scripted controller on the simulation's clock, and the pictures taken at fixed seconds of it
(debug.screenshot_at), so two builds' pictures of a moment are of the same game state. The runs are goat-rig's
(--shared: three draw at once); the timing runs have the GPU alone and are played three times.

Each check is a number against a threshold; the table says which passed. Exit status 0 when every check passed, 1
when one failed, 2 when the referee could not run.
"""

import argparse
import concurrent.futures
import hashlib
import json
import math
import re
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "tools" / "rig" / "goat-rig"
FRAME_TIME = "0.0333333333"
WIDTH, HEIGHT = 1920, 1080

# ---------- the scenes

# c20's dark corridor: the player placed facing a Flood combat form that stands a step in front of the far wall, at
# its crease with the floor; the flashlight off, then on; then a fixed camera beside the player (debug_camera_load,
# the data folder's camera.txt), and the player turned so that the flashlight's beam moves
C20 = "HALO_NETWORK_TEST=campaign:levels\\c20\\c20"
STAGE = [C20, "HALO_NETWORK_TEST_START=2", "HALO_SKIP_CINEMATICS=1", f"HALO_FRAME_TIME={FRAME_TIME}",
         "HALO_NETWORK_TEST_PLACE=-26.65 15.71 -4.6 -70 -8@8"]
FLOOD = 'debug_object_place "characters\\floodcombat_human\\floodcombat_human -25.42 12.33 -4.5 110"'
SIDE_CAMERA = "-28.06 15.2 -3.9\\n0.676 -0.735 -0.026\\n0 0 1\\n1.2"
SCENES = {
    # (moments: 0 flashlight off, 1 on, 2 the side camera, 3 the side camera with the beam moved)
    "corridor": STAGE + [
        f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0|9 {FLOOD}|15.5 debug_camera_load",
        "HALO_TEST_INPUT=script:12.5-13 flashlight; 17 aim=-64,-8",
        "HALO_SCREENSHOT_AT=12,14.5,16.5,18.5,quit", f"@data/camera.txt={SIDE_CAMERA}"],
    # the same corridor, no Flood: a frag grenade thrown at the far wall's foot (moment 0 before it, 1 as it bursts)
    "blast": STAGE + [
        "HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0",
        "HALO_TEST_INPUT=script:12.5 throw",
        "HALO_SCREENSHOT_AT=12,BLAST,quit"],
    # a30's beach in daylight: no dynamic light in sight, so Enhanced must change nothing
    "daylight": ["HALO_NETWORK_TEST=campaign:levels\\a30\\a30", "HALO_NETWORK_TEST_START=2", "HALO_SKIP_CINEMATICS=1",
                 f"HALO_FRAME_TIME={FRAME_TIME}", "HALO_NETWORK_TEST_CUES=3 ai_erase_all",
                 "HALO_SCREENSHOT_AT=8,quit"],
}
# the second the frag grenade's burst is brightest (found once: tools/lighting_referee.md)
BLAST_SECOND = "13,13.4,13.8,14.2,14.6,15,15.4,15.8,16.2,16.6,17"

# the variants: Classic; Enhanced lights with every light's shadow off; the same with ray-traced shadows. The rest
# of the look is Classic's in each (display.graphics "classic"), so that only the lights differ
CLASSIC = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=classic", "HALO_SHADOWS=classic"]
ENHANCED = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=enhanced", "HALO_SHADOWS=classic"]
TRACED = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=enhanced", "HALO_SHADOWS=ray_traced"]
HDR = ENHANCED + ["HALO_HDR=on", "HALO_SCREENSHOT_HALVES=1"]
# the timing runs: the whole Enhanced look, ray-traced shadows, the flashlight on, each pass timed
TIMED = ["HALO_GRAPHICS=enhanced", "HALO_SHADOWS=ray_traced", "HALO_PASS_TIMES=1"]

# the negative controls: settings that break one check's subject on purpose (--mutate), each of which must fail
MUTATIONS = {
    # the Enhanced lights twice as bright (debug.dynamic_lights_look's first number, 1.7)
    "lights-brighter": {"variants": ("enhanced", "traced", "hdr"), "settings": ["HALO_DYNAMIC_LIGHTS_LOOK=3.4"]},
    # every ray-traced light shadow's ray sent back into its own surface: everything the lights reach shadowed
    "shadows-everywhere": {"variants": ("traced",), "settings": ["HALO_LIGHT_SHADOWS_VIEW=4"]},
    # the HDR frame's glows pushed past their ceilings (display.hdr_highlight_boost at its most)
    "hdr-boost": {"variants": ("hdr",), "settings": ["HALO_HDR_HIGHLIGHT_BOOST=4"]},
    # the Classic picture changed (FXAA over it)
    "classic-changed": {"variants": ("classic",), "settings": ["HALO_ANTI_ALIASING=fxaa"]},
    # the GPU's work made four times larger (twice the pixels each way)
    "heavy": {"variants": ("timed",), "settings": ["HALO_RENDER_SCALE=2.0"]},
}

# ---------- regions (pixels of the 1920x1080 picture: x0, y0, x1, y1)

# the player's view of the corridor (moments 0 and 1)
PLAYER = {
    "wall pool": (700, 200, 1100, 470),
    "floor": (450, 640, 1050, 900),
    "character": (900, 420, 1010, 590),
    "weapon barrel": (1100, 640, 1500, 900),
    "weapon counter": (1250, 620, 1390, 790),
}
# the HUD's own pixels (the meters, the motion tracker, the reticle), compared where the HUD covers the view
HUD = [(140, 80, 420, 190), (1480, 80, 1780, 180), (130, 800, 340, 1010)]
# the side camera (moments 2 and 3): the crease of the far wall and the floor as a line, the Flood's feet, and the
# area about the Flood that its shadow falls in
SIDE = {
    "crease": ((1040, 660), (1920, 1000)),
    "feet": (960, 700),
    "around": (760, 420, 1400, 980),
    "caster": (900, 540, 1030, 710),
}
# the blast's light, away from the burst's own sprites
BLAST = {"wall left": (300, 150, 700, 450), "wall right": (1300, 150, 1800, 450), "floor near": (400, 800, 1500, 1000)}

# ---------- thresholds

PARITY_RATIO = 1.25        # an Enhanced light's mean added light over Classic's, either way, per region
PARITY_SHAPE = 0.85        # the correlation of the added light's pattern (a beam's hot disk and ring) with Classic's
PARITY_FLOOR = 0.002       # added light (linear) under this in Classic is no light to compare
HUD_SAME = 0.995           # the HUD's own pixels within 2 codes of Classic's
DAYLIGHT_DIFFERENT = 0.001  # pixels of the daylight control that may differ by more than 2 codes
SHADOW_DARKER = 0.25       # a pixel is shadowed where the traced light is this much under the untraced
SHADOW_LEAST = 400         # shadowed pixels a side of the crease needs (1920x1080)
SHADOW_BEND = 12.0         # degrees: the shadow's direction on the wall and on the floor differ by at least this
SHADOW_CONTACT = 40        # pixels: the shadow comes this close to the caster's feet
SHADOW_SELF = 0.05         # at most this share of the caster's lit front shadowed (acne, self-shadowing)
SHADOW_FRONT = 0.01        # at most this share of the floor in front of the caster shadowed
SHADOW_MOVED = 0.2         # the shadow changes this share of its pixels when the beam moves
HDR_COUNTER = (1.05, 2.6)  # the counter's brightest pixel, times paper white (linear): past white, under the lights'
HDR_EMITTER = (1.0, 3.05)  # a light fixture's brightest pixel: at most the light of the world's ceiling, 3
# the GPU's milliseconds a frame at 1920x1080 (median of three), the whole frame and the passes named, per machine
BUDGETS = {
    "omarchy": {"frame": 12.0, "LIGHT SHADOWS": 1.5, "AO": 1.5},
    "5900x": {"frame": 3.0, "LIGHT SHADOWS": 0.4, "AO": 0.4},
}

# ---------- pictures


def read_png(path):
    """an 8-bit RGB PNG as goat-rig writes them (tools/capture.py: unfiltered rows): (width, height, bytes)"""
    data = Path(path).read_bytes()
    at, idat, width, height = 8, b"", 0, 0
    while at < len(data):
        length, = struct.unpack(">I", data[at:at + 4])
        kind, body = data[at + 4:at + 8], data[at + 8:at + 8 + length]
        at += 12 + length
        if kind == b"IHDR":
            width, height = struct.unpack(">II", body[:8])
            if body[8:10] != b"\x08\x02":
                raise ValueError(f"{path}: not 8-bit RGB")
        elif kind == b"IDAT":
            idat += body
    raw = zlib.decompress(idat)
    stride = width * 3 + 1
    if any(raw[row * stride] for row in range(height)):
        raise ValueError(f"{path}: filtered rows")
    return width, height, b"".join(raw[row * stride + 1:(row + 1) * stride] for row in range(height))


def read_halves(path):
    """a debug.screenshot_halves file: (width, height, [r, g, b] of floats per pixel, the frame's values)"""
    data = Path(path).read_bytes()
    if data[:8] != b"H1HALF16":
        raise ValueError(f"{path}: no H1HALF16 header")
    width, height = struct.unpack("<II", data[8:16])
    values = struct.unpack(f"<{width * height * 4}e", data[16:16 + width * height * 8])
    return width, height, values


LINEAR = [(c / 255 / 12.92) if c / 255 <= 0.04045 else ((c / 255 + 0.055) / 1.055) ** 2.4 for c in range(256)]


def linear(value):
    """an encoded value (sRGB, extended past 1 as the HDR frame keeps it) in linear light"""
    sign = -1.0 if value < 0 else 1.0
    value = abs(value)
    return sign * (value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4)


class Picture:
    def __init__(self, path):
        self.path = Path(path)
        self.width, self.height, self.rgb = read_png(path)

    def luminance(self, box, step=1):
        """the linear luminance of each pixel of the box, row by row: [(x, y, value)]"""
        x0, y0, x1, y1 = box
        out = []
        for y in range(y0, y1, step):
            row = y * self.width * 3
            for x in range(x0, x1, step):
                at = row + x * 3
                out.append((x, y, 0.2126 * LINEAR[self.rgb[at]] + 0.7152 * LINEAR[self.rgb[at + 1]] +
                            0.0722 * LINEAR[self.rgb[at + 2]]))
        return out

    def digest(self):
        return hashlib.sha256(self.rgb).hexdigest()[:16]


def added(on, off, box, step=2):
    """the light added between two pictures of the same moment's scene, pixel by pixel (linear luminance)"""
    return [b[2] - a[2] for a, b in zip(off.luminance(box, step), on.luminance(box, step))]


def correlation(a, b):
    if len(a) < 2:
        return 0.0
    ma, mb = statistics.fmean(a), statistics.fmean(b)
    sa = math.sqrt(sum((x - ma) ** 2 for x in a))
    sb = math.sqrt(sum((y - mb) ** 2 for y in b))
    return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (sa * sb) if sa > 0 and sb > 0 else 0.0


# ---------- the runs


class Run:
    def __init__(self, scene, variant, build, settings, machine, shared=True, label=None, halves=False):
        self.scene, self.variant, self.build, self.machine, self.shared = scene, variant, build, machine, shared
        self.settings, self.halves = settings, halves
        self.label = label
        self.folder = None
        self.summary = ""

    def shot(self, moment, kind="png"):
        return self.folder / "game" / "shots" / f"at{moment}.{kind}"


def slug(text):
    return re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()[:40]


def rig(*arguments, machine=None, capture=True):
    command = [str(RIG)] + ([f"@{machine}"] if machine else []) + [str(a) for a in arguments]
    return subprocess.run(command, capture_output=capture, text=True)


def ssh_target(machine):
    hosts = {"omarchy": "omarchy.fabric", "5900x": "5900x.fabric"}
    with_file = Path.home() / ".config/goat-rig/hosts"
    if with_file.exists():
        for line in with_file.read_text().splitlines():
            if len(line.split()) == 2 and not line.startswith("#"):
                hosts[line.split()[0]] = line.split()[1]
    return hosts.get(machine, machine)


def play(run, work, seconds=150):
    """one goat-rig run, its pictures (and half floats) fetched, its folder on the machine removed"""
    options = ["run", run.label, "--build", run.build, "--seconds", seconds] + (["--shared"] if run.shared else [])
    result = rig(*options, "--", *run.settings, machine=run.machine)
    run.summary = (result.stdout + result.stderr).strip()
    run.folder = work / run.label
    shutil.rmtree(run.folder, ignore_errors=True)
    include = ["--include=*/", "--include=*.png", "--include=*.txt", "--include=*.log", "--include=*.json"]
    if run.halves:
        include.append("--include=*.rgba16f")
    subprocess.run(["rsync", "-a", "--prune-empty-dirs", *include, "--exclude=*",
                    f"{ssh_target(run.machine)}:goat-rig/runs/{run.label}/", f"{run.folder}/"], capture_output=True)
    # (only this run's own folder)
    subprocess.run(["ssh", ssh_target(run.machine), f"rm -rf ~/goat-rig/runs/{run.label}"], capture_output=True)
    return run


def scene_settings(scene, variant_settings, extra):
    settings = []
    for item in SCENES[scene]:
        if item.startswith("HALO_SCREENSHOT_AT="):
            item = item.replace("BLAST", BLAST_SECOND)
        settings.append(item)
    return settings + variant_settings + extra


def plan(build, baseline, machine, timing, only, mutate):
    """the runs: each check's scenes and variants, the baseline's Classic pictures (the identity check), the timing
    runs"""
    tag = slug(build) + (f"-{mutate}" if mutate else "")
    mutation = MUTATIONS.get(mutate, {"variants": (), "settings": []})

    def extra(variant):
        return list(mutation["settings"]) if variant in mutation["variants"] else []

    variants = {"classic": CLASSIC, "enhanced": ENHANCED, "traced": TRACED, "hdr": HDR}
    wanted = set()
    if "parity" in only:
        wanted |= {("corridor", "classic"), ("corridor", "enhanced"), ("blast", "classic"), ("blast", "enhanced"),
                   ("daylight", "classic"), ("daylight", "enhanced")}
    if "shadow" in only:
        wanted |= {("corridor", "enhanced"), ("corridor", "traced")}
    if "hdr" in only:
        wanted |= {("corridor", "hdr")}
    if "identity" in only:
        wanted |= {("corridor", "classic"), ("blast", "classic"), ("daylight", "classic")}
    runs = []
    for scene, variant in sorted(wanted):
        runs.append(Run(scene, variant, build, scene_settings(scene, variants[variant], extra(variant)), machine,
                        label=f"lref-{tag}-{scene}-{variant}", halves=variant == "hdr"))
    if "identity" in only and baseline != build:
        for scene in ("corridor", "blast", "daylight"):
            runs.append(Run(scene, "baseline", baseline, scene_settings(scene, CLASSIC, []), machine,
                            label=f"lref-{slug(baseline)}-base-{scene}"))
    timed = []
    if "cost" in only:
        for host in timing:
            for repeat in range(3):
                settings = STAGE + [f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|9 {FLOOD}",
                                    "HALO_TEST_INPUT=script:12.5-13 flashlight",
                                    "HALO_SCREENSHOT_AT=22,quit"] + TIMED + extra("timed")
                timed.append(Run("corridor", "timed", build, settings, host, shared=False,
                                 label=f"lref-{tag}-timed-{repeat}"))
    return runs, timed


# ---------- the checks


class Table:
    def __init__(self):
        self.rows = []

    def add(self, check, item, value, limit, passed, note=""):
        self.rows.append({"check": check, "item": item, "value": value, "limit": limit, "passed": passed,
                          "note": note})

    def text(self):
        width = max([len(f"{r['check']}: {r['item']}") for r in self.rows] + [20])
        lines = [f"{'check':<{width}}  {'value':>12}  {'limit':<22} result"]
        for r in self.rows:
            value = r["value"] if isinstance(r["value"], str) else f"{r['value']:.3f}"
            result = "PASS" if r["passed"] else ("n/a" if r["passed"] is None else "FAIL")
            lines.append(f"{r['check'] + ': ' + r['item']:<{width}}  {value:>12}  {r['limit']:<22} {result}"
                         + (f"  ({r['note']})" if r["note"] else ""))
        return "\n".join(lines)


def pictures(runs, scene, variant, moments):
    run = runs.get((scene, variant))
    if not run:
        return None
    try:
        return [Picture(run.shot(m)) for m in moments]
    except (OSError, ValueError):
        return None


def parity_region(table, name, on_c, off_c, on_e, off_e, box):
    c = added(on_c, off_c, box)
    e = added(on_e, off_e, box)
    mc, me = statistics.fmean(c), statistics.fmean(e)
    if mc < PARITY_FLOOR:
        table.add("parity", name, me, f"Classic adds {mc:.4f}", me < PARITY_FLOOR * PARITY_RATIO,
                  "no Classic light here: Enhanced must add none")
        return
    ratio = me / mc
    table.add("parity", name, ratio, f"x{1 / PARITY_RATIO:.2f}..x{PARITY_RATIO:.2f} of Classic",
              1 / PARITY_RATIO <= ratio <= PARITY_RATIO)
    shape = correlation(c, e)
    table.add("parity", name + " pattern", shape, f">= {PARITY_SHAPE}", shape >= PARITY_SHAPE)


def check_parity(table, runs):
    c = pictures(runs, "corridor", "classic", (0, 1))
    e = pictures(runs, "corridor", "enhanced", (0, 1))
    if not c or not e:
        table.add("parity", "corridor", "missing", "pictures", False, "a run made no pictures")
    else:
        for name in ("wall pool", "floor", "character", "weapon barrel"):
            parity_region(table, f"flashlight {name}", c[1], c[0], e[1], e[0], PLAYER[name])
        # (with the flashlight off: the weapon lit by nothing but its own displays, as Classic lights it)
        box = PLAYER["weapon barrel"]
        mc = statistics.fmean(v for _, _, v in c[0].luminance(box, 2))
        me = statistics.fmean(v for _, _, v in e[0].luminance(box, 2))
        ratio = me / mc if mc > 0 else float("inf")
        table.add("parity", "weapon unlit (flashlight off)", ratio, f"x{1 / PARITY_RATIO:.2f}..x{PARITY_RATIO:.2f}",
                  1 / PARITY_RATIO <= ratio <= PARITY_RATIO)
        same = total = 0
        for moment in (0, 1):
            for box in HUD:
                for a, b in zip(hud_pixels(c[moment], box), hud_pixels(e[moment], box)):
                    if a is None:
                        continue
                    total += 1
                    same += max(abs(p - q) for p, q in zip(a, b)) <= 2
        share = same / total if total else 0.0
        table.add("parity", "HUD identical", share, f">= {HUD_SAME} of its pixels", share >= HUD_SAME)
    c = pictures(runs, "blast", "classic", (0, 1))
    e = pictures(runs, "blast", "enhanced", (0, 1))
    if not c or not e:
        table.add("parity", "blast", "missing", "pictures", False, "a run made no pictures")
    else:
        for name, box in BLAST.items():
            parity_region(table, f"blast {name}", c[1], c[0], e[1], e[0], box)
    c = pictures(runs, "daylight", "classic", (0,))
    e = pictures(runs, "daylight", "enhanced", (0,))
    if not c or not e:
        table.add("parity", "daylight", "missing", "pictures", False, "a run made no pictures")
    else:
        different = sum(abs(a - b) > 2 for a, b in zip(c[0].rgb, e[0].rgb)) / len(c[0].rgb)
        table.add("parity", "daylight unchanged", different, f"<= {DAYLIGHT_DIFFERENT} differ", different <=
                  DAYLIGHT_DIFFERENT)


def hud_pixels(picture, box):
    """the HUD's own pixels of a box: bright, saturated ones (its blue meters and white text), else None"""
    x0, y0, x1, y1 = box
    out = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            at = (y * picture.width + x) * 3
            pixel = picture.rgb[at:at + 3]
            out.append(tuple(pixel) if max(pixel) >= 160 else None)
    return out


def shadow_mask(lit, traced, box, step=2):
    """the pixels the traced shadows darken: (x, y) where the traced picture's light is SHADOW_DARKER under the
    untraced one's, of the pixels lit at all"""
    mask = set()
    lit_count = 0
    for (x, y, a), (_, _, b) in zip(lit.luminance(box, step), traced.luminance(box, step)):
        if a < 0.004:
            continue
        lit_count += 1
        if b < a * (1 - SHADOW_DARKER):
            mask.add((x, y))
    return mask, lit_count


def direction(points):
    """the principal direction of a set of points, degrees (0..180)"""
    if len(points) < 3:
        return None
    mx = statistics.fmean(p[0] for p in points)
    my = statistics.fmean(p[1] for p in points)
    sxx = sum((p[0] - mx) ** 2 for p in points)
    syy = sum((p[1] - my) ** 2 for p in points)
    sxy = sum((p[0] - mx) * (p[1] - my) for p in points)
    return (math.degrees(0.5 * math.atan2(2 * sxy, sxx - syy))) % 180.0


def side_of(line, point):
    (x0, y0), (x1, y1) = line
    return (x1 - x0) * (point[1] - y0) - (y1 - y0) * (point[0] - x0)


def check_shadow(table, runs):
    e = pictures(runs, "corridor", "enhanced", (1, 2, 3))
    t = pictures(runs, "corridor", "traced", (1, 2, 3))
    if not e or not t:
        table.add("shadow", "corridor", "missing", "pictures", False, "a run made no pictures")
        return
    # the side camera: the shadow on the wall and on the floor, bending at the crease, touching the feet
    mask, lit = shadow_mask(e[1], t[1], SIDE["around"])
    caster = SIDE["caster"]
    outside = {p for p in mask if not (caster[0] <= p[0] < caster[2] and caster[1] <= p[1] < caster[3])}
    wall = [p for p in outside if side_of(SIDE["crease"], p) < 0]
    floor = [p for p in outside if side_of(SIDE["crease"], p) >= 0]
    # (counted in full-size pixels: the mask is of every second pixel each way)
    table.add("shadow", "on the wall (side)", len(wall) * 4, f">= {SHADOW_LEAST} px", len(wall) * 4 >= SHADOW_LEAST)
    table.add("shadow", "on the floor (side)", len(floor) * 4, f">= {SHADOW_LEAST} px",
              len(floor) * 4 >= SHADOW_LEAST)
    a, b = direction(wall), direction(floor)
    bend = abs(a - b) if a is not None and b is not None else 0.0
    bend = min(bend, 180 - bend)
    table.add("shadow", "bends at the crease", bend, f">= {SHADOW_BEND} deg", bend >= SHADOW_BEND)
    feet = SIDE["feet"]
    near = min((math.hypot(p[0] - feet[0], p[1] - feet[1]) for p in outside), default=float("inf"))
    table.add("shadow", "touches the feet", near, f"<= {SHADOW_CONTACT} px", near <= SHADOW_CONTACT)
    # the player's view: the light from beside the eye, so what is seen is what is lit; the caster's front and the
    # floor before it must stay lit
    front, lit_front = shadow_mask(e[0], t[0], PLAYER["character"])
    share = len(front) / lit_front if lit_front else 1.0
    table.add("shadow", "caster's lit front kept", share, f"<= {SHADOW_SELF} shadowed", share <= SHADOW_SELF)
    before, lit_before = shadow_mask(e[0], t[0], PLAYER["floor"])
    share = len(before) / lit_before if lit_before else 1.0
    table.add("shadow", "nothing before the caster", share, f"<= {SHADOW_FRONT} shadowed", share <= SHADOW_FRONT)
    # the beam moved (the player turned): the shadow moves with it
    moved, _ = shadow_mask(e[2], t[2], SIDE["around"])
    union = mask | moved
    changed = len(mask ^ moved) / len(union) if union else 0.0
    table.add("shadow", "moves with the light", changed, f">= {SHADOW_MOVED} of it changed", changed >= SHADOW_MOVED
              and len(moved) * 4 >= SHADOW_LEAST)


def box_peak(width, values, box):
    x0, y0, x1, y1 = box
    best = 0.0
    for y in range(y0, y1):
        for x in range(x0, x1):
            at = (y * width + x) * 4
            best = max(best, values[at], values[at + 1], values[at + 2])
    return linear(best)


def check_hdr(table, runs):
    run = runs.get(("corridor", "hdr"))
    path = run.shot(1, "rgba16f") if run else None
    if not path or not path.exists():
        table.add("hdr", "counter", "missing", "half floats", False,
                  "no debug.screenshot_halves frame: a build without it, or no HDR frame")
        return
    width, height, values = read_halves(path)
    peak = box_peak(width, values, PLAYER["weapon counter"])
    low, high = HDR_COUNTER
    table.add("hdr", "AR counter peak", peak, f"{low}..{high} x paper white", low <= peak <= high)
    peak = box_peak(width, values, (0, 0, width, height))
    low, high = HDR_EMITTER
    table.add("hdr", "brightest in the frame", peak, f"<= {high} x paper white", peak <= high)


PASS_GROUPS = re.compile(r"vulkan: pass groups: draws [0-9.]+(.*)")


def pass_groups(log):
    """the last ten summaries' milliseconds a frame by owner (the flashlight on, the scene still)"""
    sums = []
    for match in PASS_GROUPS.finditer(log):
        groups = {}
        for name, value in re.findall(r",? ([A-Z][A-Z ]*?) ([0-9.]+) ms", match.group(1)):
            groups[name.strip()] = float(value)
        if groups:
            sums.append(groups)
    sums = sums[-10:]
    if not sums:
        return {}
    names = set().union(*sums)
    out = {name: statistics.fmean(s.get(name, 0.0) for s in sums) for name in names}
    out["frame"] = statistics.fmean(sum(s.values()) for s in sums)
    return out


def check_cost(table, timed):
    by_machine = {}
    for run in timed:
        log = ""
        for name in ("halo.log", "debug.txt"):
            path = run.folder / "game" / name if run.folder else None
            if path and path.exists():
                log += path.read_text(errors="replace")
        by_machine.setdefault(run.machine, []).append(pass_groups(log))
    for machine, results in sorted(by_machine.items()):
        results = [r for r in results if r]
        budget = BUDGETS.get(machine, {})
        if not results:
            table.add("cost", f"{machine}", "missing", "pass times", False, "no debug.pass_times lines")
            continue
        names = sorted(set().union(*results) - {"frame"}, key=lambda n: -statistics.median(r.get(n, 0) for r in
                                                                                             results))
        for name in ["frame"] + names:
            value = statistics.median(r.get(name, 0.0) for r in results)
            if name in budget:
                table.add("cost", f"{machine} {name}", value, f"<= {budget[name]} ms", value <= budget[name])
            elif value >= 0.05:
                table.add("cost", f"{machine} {name}", value, "(reported)", None)


def check_identity(table, runs, build, baseline, identity):
    for scene in ("corridor", "blast", "daylight"):
        mine = runs.get((scene, "classic"))
        theirs = runs.get((scene, "baseline")) if baseline != build else mine
        if not mine or not theirs:
            continue
        moments = sorted(int(p.stem[2:]) for p in (mine.folder / "game" / "shots").glob("at*.png"))
        same = bool(moments)
        for m in moments:
            try:
                same = same and Picture(mine.shot(m)).digest() == Picture(theirs.shot(m)).digest()
            except (OSError, ValueError):
                same = False
        table.add("identity", f"Classic frames: {scene}", "same" if same else "differ", f"= {baseline}", same)
    if identity is not None:
        table.add("identity", "game state (identity.py)", "same" if identity == 0 else f"exit {identity}",
                  f"= {baseline}", identity == 0 if identity in (0, 1) else None,
                  "" if identity in (0, 1) else "could not run")


def run_identity(build, baseline, machine):
    """tools/scorecard/identity.py: this build's game state tick by tick against the baseline's game"""
    path = rig("path", baseline, machine=machine)
    binary = path.stdout.strip().splitlines()[-1] if path.returncode == 0 and path.stdout.strip() else ""
    if not binary:
        return 2
    result = rig("exec", build, "--", "python3", "tools/scorecard/identity.py", "--baseline-game", binary,
                 "--work", f"/tmp/lref-identity-{slug(build)}", machine=machine)
    return result.returncode


# ---------- main


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("build", help="the branch, sha or local/<name> goat-rig builds")
    parser.add_argument("--baseline", default="master", help="the build Classic must equal (default master)")
    parser.add_argument("--machine", default="omarchy", help="the machine the pictures are taken on")
    parser.add_argument("--timing", default="omarchy,5900x", help="the machines timed (comma-separated; '' none)")
    parser.add_argument("--only", default="parity,shadow,hdr,cost,identity", help="the checks run")
    parser.add_argument("--mutate", choices=sorted(MUTATIONS), help="a negative control: break one subject")
    parser.add_argument("--work", type=Path, default=ROOT / "build" / "lighting_referee",
                        help="where the pictures and the table are kept")
    parser.add_argument("--no-build", action="store_true", help="the builds as the machines have them")
    args = parser.parse_args()
    only = {c.strip() for c in args.only.split(",") if c.strip()}
    timing = [m for m in args.timing.split(",") if m] if "cost" in only else []
    started = time.monotonic()
    work = args.work / (slug(args.build) + (f"-{args.mutate}" if args.mutate else ""))
    work.mkdir(parents=True, exist_ok=True)

    machines = sorted({args.machine, *timing})
    if not args.no_build:
        builds = [(m, b) for m in machines for b in {args.build, args.baseline}
                  if m == args.machine or b == args.build]
        with concurrent.futures.ThreadPoolExecutor(len(builds)) as pool:
            for (machine, ref), result in zip(builds, pool.map(lambda mb: rig("build", mb[1], machine=mb[0]),
                                                              builds)):
                if result.returncode != 0:
                    print(f"referee: {ref} did not build on {machine}:\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
                    sys.exit(2)
        print(f"referee: built in {time.monotonic() - started:.0f} s", flush=True)

    runs, timed = plan(args.build, args.baseline, args.machine, timing, only, args.mutate)
    identity = None
    with concurrent.futures.ThreadPoolExecutor(len(runs) + 1) as pool:
        future_identity = pool.submit(run_identity, args.build, args.baseline, args.machine) \
            if "identity" in only and args.baseline != args.build else None
        done = list(pool.map(lambda r: play(r, work), runs))
        identity = future_identity.result() if future_identity else None
    print(f"referee: {len(done)} picture runs in {time.monotonic() - started:.0f} s", flush=True)
    # (the timing runs: the GPU alone, each machine's in turn, the machines at once)
    by_machine = {}
    for run in timed:
        by_machine.setdefault(run.machine, []).append(run)
    with concurrent.futures.ThreadPoolExecutor(max(1, len(by_machine))) as pool:
        list(pool.map(lambda group: [play(r, work) for r in group], by_machine.values()))
    if timed:
        print(f"referee: {len(timed)} timing runs in {time.monotonic() - started:.0f} s", flush=True)

    keyed = {(r.scene, r.variant): r for r in runs}
    table = Table()
    for run in runs + timed:
        if "exit 0" not in run.summary or "failed asserts" in run.summary and "no failed asserts" not in run.summary:
            table.add("runs", run.label, "trouble", "exit 0", False, run.summary.splitlines()[0] if run.summary
                      else "no summary")
    if "parity" in only:
        check_parity(table, keyed)
    if "shadow" in only:
        check_shadow(table, keyed)
    if "hdr" in only:
        check_hdr(table, keyed)
    if "cost" in only:
        check_cost(table, timed)
    if "identity" in only:
        check_identity(table, keyed, args.build, args.baseline, identity)
    text = table.text()
    failed = sum(r["passed"] is False for r in table.rows)
    footer = (f"referee: {args.build}{' (mutation ' + args.mutate + ')' if args.mutate else ''}: "
              f"{sum(r['passed'] is True for r in table.rows)} passed, {failed} failed, in "
              f"{time.monotonic() - started:.0f} s; pictures in {work}")
    print(text + "\n" + footer)
    (work / "table.txt").write_text(text + "\n" + footer + "\n")
    (work / "table.json").write_text(json.dumps(table.rows, indent=1))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
