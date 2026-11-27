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
# its crease with the floor. Moments: 0 the flashlight off, 1 on, 2 three frames later (stability). The "crease"
# scenes add a second local player as a light off the view's axis (debug.network_test_place's second pose).
C20 = "HALO_NETWORK_TEST=campaign:levels\\c20\\c20"
PLAYER_ONE = "-26.65 15.71 -4.6 -70 -8"
STAGE = [C20, "HALO_NETWORK_TEST_START=2", "HALO_SKIP_CINEMATICS=1", f"HALO_FRAME_TIME={FRAME_TIME}",
         f"HALO_NETWORK_TEST_PLACE={PLAYER_ONE}@8"]
FLOOD = 'debug_object_place "characters\\floodcombat_human\\floodcombat_human -25.42 12.33 -4.5 110"'
SHORT_AT = "HALO_SCREENSHOT_AT=12,14.5,14.6,quit"
SCENES = {
    "corridor": STAGE + [
        f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0|9 {FLOOD}",
        "HALO_TEST_INPUT=script:12.5-13 flashlight", SHORT_AT],
    # the off-axis light: a second local player (split screen: the first one's view is the top half) placed beside
    # the Flood, its flashlight aimed across it at the slanted wall beyond; and the same with that player a step
    # aside (the light moved)
    "crease": [item for item in STAGE if not item.startswith("HALO_NETWORK_TEST_PLACE")] + [
        f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0|9 {FLOOD}", "HALO_NETWORK_TEST_PLAYERS=2",
        f"HALO_NETWORK_TEST_PLACE={PLAYER_ONE};-23.71 13.48 -4.6 -146 -25@8",
        "HALO_TEST_INPUT=script:12.5-13 p2 flashlight", SHORT_AT],
    "crease-moved": [item for item in STAGE if not item.startswith("HALO_NETWORK_TEST_PLACE")] + [
        f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0|9 {FLOOD}", "HALO_NETWORK_TEST_PLAYERS=2",
        f"HALO_NETWORK_TEST_PLACE={PLAYER_ONE};-23.71 13.98 -4.6 -152 -25@8",
        "HALO_TEST_INPUT=script:12.5-13 p2 flashlight", SHORT_AT],
    # the same corridor, no Flood: a frag grenade thrown at the far wall (moment 0 before it, 1 as it bursts,
    # 14.2 s: found once by a burst of moments)
    "blast": STAGE + [
        "HALO_NETWORK_TEST_CUES=5 ai_erase_all|5.1 render_shadows 0",
        "HALO_TEST_INPUT=script:12.5 throw",
        "HALO_SCREENSHOT_AT=12,14.2,quit"],
    # a10's cryo deck, a static camera on its reflective floor (the reflections' scene: the corridor has nothing
    # that reflects); the co-op skip voted, the camera posed every 4 s (moments 1 and 2, a tenth of a second apart, as the
# corridor's: the features' rows read those two)
    "cryo": ["HALO_NETWORK_TEST=local:levels\\a10\\a10:coop", "HALO_TEST_INPUT=script:1 jump; 2 jump; 3 jump",
             "HALO_SKIP_CINEMATICS=1", f"HALO_FRAME_TIME={FRAME_TIME}",
             "HALO_NETWORK_TEST_CUES=" + "|".join(f'{t} debug_camera_set "-55.4 0.8 0.62 0 -12 0 70"'
                                                  for t in range(2, 23, 4)),
             "HALO_SCREENSHOT_AT=21.9,22,22.1,quit"],
    # Blood Gulch in daylight, one machine's game: no dynamic light in sight, so Enhanced must change nothing
    "daylight": ["HALO_NETWORK_TEST=log", "@data/init.txt=map_name levels\\test\\bloodgulch\\bloodgulch",
                 f"HALO_FRAME_TIME={FRAME_TIME}", "HALO_NETWORK_TEST_CUES=1 ai_erase_all",
                 "HALO_NETWORK_TEST_PLACE=60 -120 2 30 -5@2", "HALO_SCREENSHOT_AT=5,quit",
                 # (the AR counter's panel light, an Enhanced extra, off here: the "panel glow" rows measure it)
                 "HALO_PANEL_LIGHTS_OFF=1"],
}

# the variants. The rest of the look is Classic's in each (display.graphics "classic"), so that only the lights
# differ: Classic; Enhanced lights with no shadows; Enhanced with ray-traced shadows (world ambient occlusion, which
# display.shadows "ray_traced", with world ambient occlusion as Enhanced draws it); that in HDR with its half floats
# kept. (World AO once dropped the structure's draws until a ray-traced scene existed, the clear colour in their
# place: "world AO: structure drawn" guards it)
CLASSIC = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=classic", "HALO_SHADOWS=classic"]
ENHANCED = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=enhanced", "HALO_SHADOWS=classic"]
TRACED = ["HALO_GRAPHICS=classic", "HALO_DYNAMIC_LIGHTS=enhanced", "HALO_SHADOWS=ray_traced",
          "HALO_AMBIENT_OCCLUSION=world"]
# (the reference for the shadows: the same build with its lights' shadows off, the knobs every branch keeps:
# HALO_LIGHT_SHADOWS_OFF for the dynamic lights, HALO_FLASHLIGHT_SHADOWS_OFF for a flashlight's own)
UNTRACED = TRACED + ["HALO_LIGHT_SHADOWS_OFF=1", "HALO_FLASHLIGHT_SHADOWS_OFF=1"]
HDR = TRACED + ["HALO_HDR=on", "HALO_SCREENSHOT_HALVES=1"]
# the timing runs: the whole Enhanced look, ray-traced shadows, the flashlight on, each pass timed
TIMED = ["HALO_GRAPHICS=enhanced", "HALO_SHADOWS=ray_traced", "HALO_PASS_TIMES=1"]
# the features landing on the ray-traced work, each a setting turned on (--feature NAME=VAR=VALUE adds or changes
# one): off, the pictures must equal the baseline's; on, no NaN or blown-out pixel, no flicker, within its cost
# (a feature's settings separated by spaces; its scene, the corridor unless named here)
FEATURE_SCENES = {"reflections": "cryo"}
FEATURES = {
    "bounce": "HALO_DYNAMIC_LIGHT_BOUNCE=true",
    "reflections": "HALO_RT_REFLECTIONS=true",
    "rt-bounce": "HALO_RT_BOUNCE=true HALO_AMBIENT_OCCLUSION=ray_traced",
}

# the negative controls: settings that break one check's subject on purpose (--mutate), each of which must fail
MUTATIONS = {
    # the Enhanced lights twice as bright (debug.dynamic_lights_look's first number, 1.7)
    "lights-brighter": {"variants": ("enhanced",), "settings": ["HALO_DYNAMIC_LIGHTS_LOOK=3.4"]},
    # every ray-traced light shadow's ray sent back into its own surface: everything the lights reach shadowed
    "shadows-everywhere": {"variants": ("traced",), "settings": ["HALO_LIGHT_SHADOWS_VIEW=4"]},
    # the lights' shadows gone (the flashlight's and the dynamic lights'): the shadow rows must fail
    "shadows-gone": {"variants": ("traced",), "settings": ["HALO_LIGHT_SHADOWS_OFF=1", "HALO_FLASHLIGHT_SHADOWS_OFF=1"]},
    # the HDR frame's glows pushed past their ceilings (display.hdr_highlight_boost at its most)
    "hdr-boost": {"variants": ("hdr",), "settings": ["HALO_HDR_HIGHLIGHT_BOOST=4"]},
    # the Classic picture changed (its anisotropic filtering raised, a quiet change)
    "classic-changed": {"variants": ("classic",), "settings": ["HALO_ANISOTROPIC_FILTERING=16"]},
    # the GPU's work made four times larger (twice the pixels each way)
    "heavy": {"variants": ("timed",), "settings": ["HALO_SUPERSAMPLING=2.0"]},
}

# ---------- regions (pixels of the 1920x1080 picture: x0, y0, x1, y1)

# the player's view of the corridor (moments 0 to 2)
PLAYER = {
    "wall pool": (700, 200, 1100, 470),
    "floor": (450, 640, 1050, 900),
    "character": (900, 420, 1010, 590),
    "weapon barrel": (1100, 640, 1500, 900),
    "weapon counter": (1250, 620, 1390, 790),
    "view": (0, 0, 1920, 1080),
}
# the HUD's own pixels (the meters, the motion tracker), compared where the HUD covers the view
HUD = [(140, 80, 420, 190), (1480, 80, 1780, 180), (130, 800, 340, 1010)]
# the player's view about the Flood: the crease of the far wall and the floor as a line, its feet, the area its
# shadow falls in, and the Flood itself (as placed, and as moved)
# the first player's view in the crease scene (the top half of the split screen): the slanted wall beyond the Flood
# as a polygon (the floor the rest), the Flood's feet, the area its shadow falls in, the Flood itself
NEAR = {
    "wall": [(994, 322), (1192, 285), (1192, 150), (994, 150)],
    "feet": (955, 325),
    "around": (880, 200, 1200, 380),
    "caster": (925, 240, 985, 330),
}
# the metal just under the AR's counter, which its panel light reaches
PANEL_UNDER = (1240, 780, 1400, 900)
# the blast's light on the walls about it, away from the burst's own sprites
BLAST = {"wall left": (400, 100, 800, 450), "wall above": (850, 40, 1200, 250), "wall right": (1150, 350, 1900, 650)}

# ---------- thresholds

PARITY_RATIO = 1.05        # an Enhanced light's mean added light over Classic's, either way, per region
PARITY_SHAPE = 0.85        # the correlation of the added light's pattern (a beam's hot disk and ring) with Classic's
PARITY_FLOOR = 0.002       # added light (linear) under this in Classic is no light to compare
HUD_SAME = 1.0             # the HUD's own pixels identical to Classic's, to a code (the view behind a translucent
HUD_CODES = 1              # meter rounds to 8 bits after the blend)
WORLD_AO_CLEAR = 0.2       # the commonest colour's share of a frame with world AO (the structure missing: 0.87)
PANEL_GLOW = (0.5, 3.0)    # the panel light under the counter over Classic, 8-bit luma: there, and a glow, not a
PANEL_BARREL = 1.0         # lamp (#1297: the rifle 31.1 against 30.2); the barrel beyond its reach within 1
DAYLIGHT_DIFFERENT = 0.001  # bytes of the daylight control that may differ by more than 2 codes
SHADOW_DARKER = 0.4        # shadowed: the traced picture's light this much under the untraced one's
SHADOW_LEAST = 150         # shadowed pixels a side of the crease needs (1920x1080)
SHADOW_SAME = 1.15         # the untraced reference's light, the frame's median, within this of the traced
SHADOW_BEND = 12.0         # degrees: the shadow's direction on the wall and on the floor differ by at least this
SHADOW_CONTACT = 40        # pixels: the shadow comes this close to the caster's feet
SHADOW_SELF = 0.05         # at most this share of the caster's lit front shadowed (acne, self-shadowing)
SHADOW_FRONT = 0.01        # at most this share of the floor in front of the caster shadowed
SHADOW_GUN = 0.01          # at most this share of the first-person weapon shadowed (its own lamp is on it)
SHADOW_MOVED = 0.2         # the shadow changes this share of its pixels when the beam moves
HDR_COUNTER = (1.05, 2.6)  # the counter's brightest pixel, times paper white (linear): past white, under the lights'
HDR_EMITTER = 3.05         # the frame's brightest pixel: at most the light of the world's ceiling, 3
BLOWN = 0.001              # a feature on: at most this share of the pixels past HDR_EMITTER
FLICKER = (1.5, 0.0005)    # a feature on: frame-to-frame change at most this times the feature off's, plus this
# three runs of one configuration whose frames differ by more than this are not a measurement (the GPU's clock, or
# work beside it: omarchy's 8060S has shown x1.9 between runs)
TIMING_SPREAD = 1.25
# the GPU's milliseconds a frame at 1920x1080 (median of three), the whole frame and the passes named, per machine;
# "+feature": what turning a feature on adds to the frame
BUDGETS = {
    "omarchy": {"frame": 12.0, "LIGHT SHADOWS": 1.5, "AO": 1.5, "+feature": 2.0},
    "5900x": {"frame": 3.0, "LIGHT SHADOWS": 0.4, "AO": 0.4, "+feature": 0.5},
    "bosgame": {"frame": 5.0, "LIGHT SHADOWS": 0.8, "AO": 0.8, "+feature": 1.0},
}
# the machines' goat-rig, where one is not this tree's (the Windows machine's kit lives in its own worktree)
RIGS = {"bosgame": Path.home() / "dev/goat-wt-bosgame-rig/tools/rig/goat-rig"}

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


def rig_of(machine):
    path = RIGS.get(machine)
    return path if path and path.exists() else RIG


def rig(*arguments, machine=None):
    command = [str(rig_of(machine))] + ([f"@{machine}"] if machine else []) + [str(a) for a in arguments]
    return subprocess.run(command, capture_output=True, text=True)


def ssh_target(machine):
    hosts = {"omarchy": "omarchy.fabric", "5900x": "5900x.fabric"}
    with_file = Path.home() / ".config/goat-rig/hosts"
    if with_file.exists():
        for line in with_file.read_text().splitlines():
            if len(line.split()) == 2 and not line.startswith("#"):
                hosts[line.split()[0]] = line.split()[1]
    return hosts.get(machine, machine)


def play(run, work, seconds=150, reuse=False):
    """one goat-rig run, its pictures (and half floats) fetched, its folder on the machine removed; with reuse, a
    run already fetched is kept as it is"""
    if reuse and (work / run.label / "summary.txt").exists() and \
            "exit 0" in (work / run.label / "summary.txt").read_text():
        run.folder = work / run.label
        run.summary = (run.folder / "summary.txt").read_text()
        return run
    for attempt in range(3):
        if attempt:
            time.sleep(15)
        play_once(run, work, seconds)
        # (again when ssh was refused: every helper's runs open connections to the machines at once)
        if "exit 0" in run.summary or not re.search(r"kex_exchange|Connection (reset|closed)|ssh:", run.summary):
            break
    return run


def play_once(run, work, seconds):
    options = ["run", run.label, "--build", run.build, "--seconds", seconds] + (["--shared"] if run.shared else [])
    result = rig(*options, "--", *run.settings, machine=run.machine)
    run.summary = (result.stdout + result.stderr).strip()
    run.folder = work / run.label
    shutil.rmtree(run.folder, ignore_errors=True)
    if run.machine in ("omarchy", "5900x"):
        include = ["--include=*/", "--include=*.png", "--include=*.txt", "--include=*.log", "--include=*.json"]
        if run.halves:
            include.append("--include=*.rgba16f")
        subprocess.run(["rsync", "-a", "--prune-empty-dirs", *include, "--exclude=*",
                        f"{ssh_target(run.machine)}:goat-rig/runs/{run.label}/", f"{run.folder}/"],
                       capture_output=True)
        # (only this run's own folder)
        subprocess.run(["ssh", ssh_target(run.machine), f"rm -rf ~/goat-rig/runs/{run.label}"], capture_output=True)
    else:
        rig("fetch", f"{run.machine}:{run.label}", run.folder)
    return run


def mirror_ref(build, machine):
    """a local/ ref of a machine's mirror (goat-rig's repo.git) fetched here as refs/lref/<name>"""
    target = f"refs/lref/{slug(build)}"
    fetched = subprocess.run(["git", "-C", str(ROOT), "fetch", "-q", "-f", f"{ssh_target(machine)}:goat-rig/repo.git",
                              f"+refs/heads/{build}:{target}"], capture_output=True)
    if fetched.returncode == 0:
        return target
    # (else one fetched before, from any machine)
    known = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "-q", "--verify", target], capture_output=True)
    return target if known.returncode == 0 else None


# what the referee needs of a build's own code: the captures, and the second player's pose
NEEDED = [("port/linux/src/port_config.c", "debug.screenshot_at"), ("port/linux/game/network_test.c", "place_second")]


def has_capture(build, machine="omarchy"):
    """whether a build has debug.screenshot_at (the referee's pictures need it): a ref of origin, of this checkout,
    or (local/<name>) of the machine's mirror"""
    subprocess.run(["git", "-C", str(ROOT), "fetch", "-q", "origin"], capture_output=True)
    refs = [f"origin/{build}", build]
    if build.startswith("local/"):
        refs = [mirror_ref(build, machine) or build]
    for ref in refs:
        found = True
        for path, marker in NEEDED:
            shown = subprocess.run(["git", "-C", str(ROOT), "show", f"{ref}:{path}"], capture_output=True, text=True)
            if shown.returncode != 0:
                found = None
                break
            found = found and marker in shown.stdout
        if found is not None:
            return found
    return True


def graft(build, machines):
    """a build without the referee's captures: the capture commits (tools/lighting-referee's commits to port/)
    picked onto it in a scratch worktree, pushed to each machine's mirror as local/lref/<build>; that name"""
    commits = subprocess.run(["git", "-C", str(ROOT), "log", "--reverse", "--format=%H",
                              "origin/master..origin/tools/lighting-referee", "--", "port"],
                             capture_output=True, text=True).stdout.split()
    if not commits:
        commits = subprocess.run(["git", "-C", str(ROOT), "log", "--reverse", "--format=%H", "-i",
                                  "--grep=debug.screenshot_", "--", "port/linux/src/d3d8_device.c"],
                                 capture_output=True, text=True).stdout.split()
    name = f"local/lref/{slug(build)}"
    tree = Path("/tmp") / f"lref-graft-{slug(build)}"
    subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], capture_output=True)
    base = f"origin/{build}" if subprocess.run(["git", "-C", str(ROOT), "rev-parse", "-q", "--verify",
                                                f"origin/{build}"], capture_output=True).returncode == 0 else build
    if build.startswith("local/"):
        base = f"refs/lref/{slug(build)}"
    steps = [["git", "-C", str(ROOT), "worktree", "add", "-q", "--detach", str(tree), base],
             ["git", "-C", str(tree), "-c", "user.name=referee", "-c", "user.email=referee@localhost",
              "cherry-pick", "-x", *commits]]
    # (only the commits the build lacks: one that merged some of them already)
    base_ref = steps[0][-1]
    missing = subprocess.run(["git", "-C", str(ROOT), "cherry", base_ref, "origin/tools/lighting-referee"],
                             capture_output=True, text=True).stdout.split("\n")
    lacking = {line[2:] for line in missing if line.startswith("+ ")}
    steps[1] = steps[1][:-len(commits)] + [c for c in commits if c in lacking]
    if len(steps[1]) == 9:
        return None
    for step in steps:
        if subprocess.run(step, capture_output=True).returncode != 0:
            subprocess.run(["git", "-C", str(tree), "cherry-pick", "--abort"], capture_output=True)
            subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], capture_output=True)
            return None
    for machine in machines:
        if machine in ("omarchy", "5900x"):
            subprocess.run(["git", "-C", str(tree), "push", "-q", "-f",
                            f"{ssh_target(machine)}:goat-rig/repo.git", f"HEAD:refs/heads/{name}"],
                           capture_output=True)
    subprocess.run(["git", "-C", str(ROOT), "worktree", "remove", "--force", str(tree)], capture_output=True)
    return name


def build_on(machine, ref):
    """goat-rig build, tried again a few times: the machines' mirror is fetched by every helper at once"""
    for attempt in range(4):
        result = rig("build", ref, machine=machine)
        if result.returncode == 0 or "fetch" not in result.stdout + result.stderr:
            return result
        time.sleep(20 * (attempt + 1))
    return result


def feature_off(setting):
    """a feature's settings turned off: its on/off settings negated (the others, such as the AO it needs, kept)"""
    out = []
    for item in setting.split():
        key, _, value = item.partition("=")
        out.append(f"{key}=false" if value.lower() in ("true", "1", "on") else item)
    return out


def scene_settings(scene, variant_settings, extra):
    settings = list(SCENES[scene])
    # (a variant's own moments in place of the scene's)
    if any(item.startswith("HALO_SCREENSHOT_AT=") for item in variant_settings):
        settings = [item for item in settings if not item.startswith("HALO_SCREENSHOT_AT=")]
    return settings + variant_settings + extra


def plan(build, baseline, machine, timing, only, mutate, features, overrides=(), feature_cost=False,
         feature_baseline=None):
    """the runs: each check's scenes and variants, the baseline's pictures (identity), the timing runs"""
    tag = slug(build) + (f"-{mutate}" if mutate else "")
    mutation = MUTATIONS.get(mutate, {"variants": (), "settings": []})

    def extra(variant):
        return list(overrides) + (list(mutation["settings"]) if variant in mutation["variants"] else [])

    variants = {"classic": CLASSIC, "enhanced": ENHANCED, "traced": TRACED, "untraced": UNTRACED, "hdr": HDR}
    for name, setting in features.items():
        variants[f"{name}-on"] = HDR + setting.split()
    wanted = set()
    if "parity" in only:
        wanted |= {("corridor", "classic"), ("corridor", "enhanced"), ("blast", "classic"), ("blast", "enhanced"),
                   ("daylight", "classic"), ("daylight", "enhanced")}
    if "shadow" in only:
        wanted |= {("corridor", "untraced"), ("corridor", "traced"), ("crease", "untraced"), ("crease", "traced"),
                   ("crease-moved", "untraced"), ("crease-moved", "traced")}
    if "hdr" in only or features:
        wanted |= {("corridor", "hdr")}
    if "identity" in only:
        wanted |= {("corridor", "classic"), ("blast", "classic"), ("daylight", "classic")}
    if features and "features" in only:
        wanted |= {("corridor", "traced")}
        for name in features:
            scene = FEATURE_SCENES.get(name, "corridor")
            wanted |= {(scene, "hdr"), (scene, f"{name}-on")}
    runs = []
    for scene, variant in sorted(wanted):
        runs.append(Run(scene, variant, build, scene_settings(scene, variants[variant], extra(variant)), machine,
                        label=f"lref-{tag}-{scene}-{variant}", halves=variant == "hdr" or variant.endswith("-on")))
    if baseline != build:
        if "identity" in only:
            for scene in ("corridor", "blast", "daylight"):
                runs.append(Run(scene, "baseline", baseline, scene_settings(scene, CLASSIC, list(overrides)), machine,
                                label=f"lref-{tag}-base-{slug(baseline)}-{scene}"))
    if features and feature_baseline:
        runs.append(Run("corridor", "baseline-traced", feature_baseline,
                        scene_settings("corridor", TRACED, list(overrides)), machine,
                        label=f"lref-{tag}-base-{slug(feature_baseline)}-traced"))
    timed = []
    if "cost" in only:
        corridor = STAGE + [f"HALO_NETWORK_TEST_CUES=5 ai_erase_all|9 {FLOOD}", "HALO_TEST_INPUT=script:12.5-13 flashlight",
                            "HALO_SCREENSHOT_AT=16,quit"]
        staged = {"corridor": corridor, "cryo": SCENES["cryo"]}
        configurations = [("timed", corridor, [])]
        if feature_cost:
            # (each feature timed on and explicitly off in its scene: a preset may turn it on by itself)
            for name, setting in features.items():
                scene = FEATURE_SCENES.get(name, "corridor")
                configurations.append((f"timed-{name}-off", staged[scene], feature_off(setting)))
                configurations.append((f"timed-{name}", staged[scene], setting.split()))
        for host in timing:
            for variant, stage, more in configurations:
                for repeat in range(3):
                    settings = stage + TIMED + more + extra("timed")
                    timed.append(Run("corridor", variant, build, settings, host, shared=False,
                                     label=f"lref-{tag}-{host}-{variant}-{repeat}"))
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
        lines = [f"{'check':<{width}}  {'value':>12}  {'limit':<26} result"]
        for r in self.rows:
            value = r["value"] if isinstance(r["value"], str) else f"{r['value']:.3f}"
            result = "PASS" if r["passed"] else ("n/a" if r["passed"] is None else "FAIL")
            lines.append(f"{r['check'] + ': ' + r['item']:<{width}}  {value:>12}  {r['limit']:<26} {result}"
                         + (f"  ({r['note']})" if r["note"] else ""))
        return "\n".join(lines)


def pictures(runs, scene, variant, moments):
    run = runs.get((scene, variant))
    if not run or not run.folder:
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
        # (with the flashlight off: the counter's panel light, an Enhanced extra, glows on the metal just under it,
        # a glow and not a lamp; the barrel beyond it as Classic lights it)
        glow = luma(e[0], PANEL_UNDER) - luma(c[0], PANEL_UNDER)
        table.add("parity", "panel glow under the counter", glow, f"+{PANEL_GLOW[0]}..+{PANEL_GLOW[1]} luma",
                  PANEL_GLOW[0] <= glow <= PANEL_GLOW[1])
        glow = luma(e[0], PLAYER["weapon barrel"]) - luma(c[0], PLAYER["weapon barrel"])
        table.add("parity", "panel glow on the barrel", glow, f"within {PANEL_BARREL} luma", abs(glow) <= PANEL_BARREL)
        same = total = 0
        for moment in (0, 1):
            for box in HUD:
                for (a, hud_a), (b, hud_b) in zip(hud_pixels(c[moment], box), hud_pixels(e[moment], box)):
                    if not hud_a and not hud_b:
                        continue
                    total += 1
                    same += max(abs(p - q) for p, q in zip(a, b)) <= HUD_CODES
        differ = total - same
        table.add("parity", "HUD identical", differ, f"0 of {total} px differ > {HUD_CODES} code", differ == 0)
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
        table.add("parity", "daylight unchanged (panel lights off)", different, f"<= {DAYLIGHT_DIFFERENT} differ",
                  different <= DAYLIGHT_DIFFERENT)


def clear_share(rgb):
    """the share of a picture's pixels of its commonest colour (the clear colour where nothing was drawn)"""
    counts = {}
    for at in range(0, len(rgb), 3 * 97):
        key = rgb[at:at + 3]
        counts[key] = counts.get(key, 0) + 1
    return max(counts.values()) / sum(counts.values())


def check_whole(table, runs):
    """every picture whole: no frame mostly one colour (the clear colour where the structure was not drawn); with
    world AO (the traced variants), at most WORLD_AO_CLEAR of any picture (void frames read 0.87, whole ones
    0.03 to 0.07)"""
    broken, worst = [], 0.0
    for run in runs:
        # (the build's own pictures: a baseline's are its own business)
        if not run.folder or run.variant.startswith("baseline"):
            continue
        for path in sorted((run.folder / "game" / "shots").glob("at*.png")):
            try:
                rgb = Picture(path).rgb
            except (OSError, ValueError):
                continue
            share = clear_share(rgb)
            world_ao = "HALO_AMBIENT_OCCLUSION=world" in run.settings
            if world_ao:
                worst = max(worst, share)
            if share > (WORLD_AO_CLEAR if world_ao else 0.4):
                broken.append(f"{run.scene}-{run.variant} {path.stem} {share:.2f}")
    table.add("runs", "frames whole", len(broken), "= 0 mostly one colour", not broken, ", ".join(broken[:4]))
    if any("HALO_AMBIENT_OCCLUSION=world" in run.settings for run in runs):
        table.add("runs", "world AO: structure drawn", worst, f"< {WORLD_AO_CLEAR} clear colour, every frame",
                  worst < WORLD_AO_CLEAR)


def luma(picture, box):
    """the mean 8-bit luma (Rec. 601) of a box: #1297's measure of the barrel"""
    x0, y0, x1, y1 = box
    total = count = 0
    for y in range(y0, y1):
        row = y * picture.width * 3
        for x in range(x0, x1):
            at = row + x * 3
            total += 0.299 * picture.rgb[at] + 0.587 * picture.rgb[at + 1] + 0.114 * picture.rgb[at + 2]
            count += 1
    return total / count if count else 0.0


def hud_pixels(picture, box):
    """the pixels of a box, each with whether it is the HUD's own (bright: its blue meters and white text)"""
    x0, y0, x1, y1 = box
    out = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            at = (y * picture.width + x) * 3
            pixel = tuple(picture.rgb[at:at + 3])
            out.append((pixel, max(pixel) >= 160))
    return out


def shadow_mask(untraced, traced, box, step=2):
    """the pixels the traced shadows darken: where the traced picture's light is SHADOW_DARKER under the same
    build's with its shadows off; and the count of pixels lit at all"""
    mask, lit = set(), 0
    for (x, y, a), (_, _, b) in zip(untraced.luminance(box, step), traced.luminance(box, step)):
        if a < 0.004:
            continue
        lit += 1
        if b < a * (1 - SHADOW_DARKER):
            mask.add((x, y))
    return mask, lit


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


def inside(polygon, point):
    x, y = point
    within = False
    for index in range(len(polygon)):
        (x0, y0), (x1, y1) = polygon[index], polygon[index - 1]
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            within = not within
    return within


def side_of(line, point):
    (x0, y0), (x1, y1) = line
    return (x1 - x0) * (point[1] - y0) - (y1 - y0) * (point[0] - x0)


# (the floor and bend rows: the second player's beam lights too little floor behind the Flood for a floor shadow
# of any size in this view, a thin contact line; until the staging lights more of it, they report their numbers
# as n/a)
STAGED = None
UNSTAGED = "n/a: the staging lights too little floor behind the Flood"


def check_shadow(table, runs):
    u = pictures(runs, "corridor", "untraced", (1,))
    t = pictures(runs, "corridor", "traced", (1,))
    if not u or not t:
        table.add("shadow", "corridor", "missing", "pictures", False, "a run made no pictures")
        return
    # (the reference: shadows off must change nothing but the shadows)
    ratios = sorted(b / a for (_, _, a), (_, _, b) in zip(u[0].luminance(PLAYER["view"], 8),
                                                          t[0].luminance(PLAYER["view"], 8)) if a > 0.004)
    level = ratios[len(ratios) // 2] if ratios else 0.0
    table.add("shadow", "shadows-off reference sound", level, f"x{1 / SHADOW_SAME:.2f}..x{SHADOW_SAME:.2f}",
              1 / SHADOW_SAME <= level <= SHADOW_SAME, "HALO_LIGHT_SHADOWS_OFF / HALO_FLASHLIGHT_SHADOWS_OFF")
    # the off-axis light (the second player's flashlight): the shadow on the wall and on the floor, bending at the
    # crease, touching the feet; moving when the light moves
    cu = pictures(runs, "crease", "untraced", (1,))
    ct = pictures(runs, "crease", "traced", (1,))
    if not cu or not ct:
        table.add("shadow", "crease scene", "missing", "pictures", False, "a run made no pictures")
        outside = set()
    else:
        mask, _ = shadow_mask(cu[0], ct[0], NEAR["around"], step=1)
        caster = NEAR["caster"]
        outside = {p for p in mask if not (caster[0] <= p[0] < caster[2] and caster[1] <= p[1] < caster[3])}
        wall = [p for p in outside if inside(NEAR["wall"], p)]
        floor = [p for p in outside if not inside(NEAR["wall"], p)]
        table.add("shadow", "on the wall", len(wall), f">= {SHADOW_LEAST} px", len(wall) >= SHADOW_LEAST)
        table.add("shadow", "on the floor", len(floor), f">= {SHADOW_LEAST} px", STAGED and len(floor) >= SHADOW_LEAST,
                  UNSTAGED)
        a, b = direction(wall), direction(floor)
        bend = abs(a - b) if a is not None and b is not None else 0.0
        bend = min(bend, 180 - bend)
        table.add("shadow", "bends at the crease", bend, f">= {SHADOW_BEND} deg", STAGED and bend >= SHADOW_BEND,
                  UNSTAGED)
        feet = NEAR["feet"]
        near = min((math.hypot(p[0] - feet[0], p[1] - feet[1]) for p in outside), default=float("inf"))
        table.add("shadow", "touches the feet", near, f"<= {SHADOW_CONTACT} px", near <= SHADOW_CONTACT)
    mu = pictures(runs, "crease-moved", "untraced", (1,))
    mt = pictures(runs, "crease-moved", "traced", (1,))
    if not mu or not mt:
        table.add("shadow", "moves with the light", "missing", "pictures", False)
    else:
        moved, _ = shadow_mask(mu[0], mt[0], NEAR["around"], step=1)
        caster = NEAR["caster"]
        moved = {p for p in moved if not (caster[0] <= p[0] < caster[2] and caster[1] <= p[1] < caster[3])}
        union = outside | moved
        changed = len(outside ^ moved) / len(union) if union else 0.0
        table.add("shadow", "moves with the light", changed, f">= {SHADOW_MOVED} changed",
                  changed >= SHADOW_MOVED and len(moved) >= SHADOW_LEAST)
    # the light from beside the eye, so what is seen is lit: the caster's front, the floor before it and the
    # first-person weapon (whose lamp it is) stay lit
    for box, limit, item in ((NEAR["caster"], SHADOW_SELF, "caster's lit front kept"),
                             (PLAYER["floor"], SHADOW_FRONT, "nothing before the caster"),
                             (PLAYER["weapon barrel"], SHADOW_GUN, "first-person weapon unshadowed")):
        shaded, lit = shadow_mask(u[0], t[0], box)
        share = len(shaded) / lit if lit else 0.0
        table.add("shadow", item, share, f"<= {limit} shadowed", share <= limit)


def halves_luminance(values, width, height, step=4):
    out = []
    for y in range(0, height, step):
        for x in range(0, width, step):
            at = (y * width + x) * 4
            out.append(0.2126 * linear(values[at]) + 0.7152 * linear(values[at + 1]) +
                       0.0722 * linear(values[at + 2]))
    return out


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
    path = run.shot(1, "rgba16f") if run and run.folder else None
    if not path or not path.exists():
        table.add("hdr", "counter", "missing", "half floats", False,
                  "no debug.screenshot_halves frame: a build without it, or no HDR frame")
        return
    width, height, values = read_halves(path)
    peak = box_peak(width, values, PLAYER["weapon counter"])
    low, high = HDR_COUNTER
    table.add("hdr", "AR counter peak", peak, f"{low}..{high} x paper white", low <= peak <= high)
    peak = box_peak(width, values, (0, 0, width, height))
    table.add("hdr", "brightest in the frame", peak, f"<= {HDR_EMITTER} x paper white", peak <= HDR_EMITTER)


def flicker(run):
    """the mean change of light between two frames of a still view (moments 1 and 2, a tenth of a second apart)"""
    a = read_halves(run.shot(1, "rgba16f"))
    b = read_halves(run.shot(2, "rgba16f"))
    la, lb = halves_luminance(a[2], a[0], a[1]), halves_luminance(b[2], b[0], b[1])
    return statistics.fmean(abs(x - y) for x, y in zip(la, lb))


def check_features(table, runs, features, baseline):
    for name in features:
        scene = FEATURE_SCENES.get(name, "corridor")
        off = runs.get((scene, "hdr"))
        on = runs.get((scene, f"{name}-on"))
        try:
            width, height, values = read_halves(on.shot(1, "rgba16f"))
        except (OSError, ValueError, AttributeError):
            table.add(name, "on", "missing", "half floats", False, "no HDR frame with the feature on")
            continue
        bad = sum(1 for v in values if v != v or v in (float("inf"), float("-inf")))
        table.add(name, "on: NaN or infinite values", bad, "= 0", bad == 0)
        ceiling = HDR_EMITTER
        blown = sum(1 for at in range(0, len(values), 4)
                    if max(linear(values[at]), linear(values[at + 1]), linear(values[at + 2])) > ceiling) / \
            (width * height)
        table.add(name, "on: blown-out pixels", blown, f"<= {BLOWN} past x{ceiling}", blown <= BLOWN)
        try:
            same = Path(on.shot(1)).read_bytes() == Path(off.shot(1)).read_bytes()
            mine, theirs = flicker(on), flicker(off)
        except (OSError, ValueError, AttributeError):
            table.add(name, "on: stable", "missing", "frames", False)
            continue
        if same:
            table.add(name, "on: changes the picture", "no", "a change", None,
                      f"{features[name]} draws nothing in this build")
        limit = mine <= theirs * FLICKER[0] + FLICKER[1]
        table.add(name, "on: frame-to-frame change", mine, f"<= {FLICKER[0]}x off's {theirs:.4f} + {FLICKER[1]}",
                  limit)
    mine = runs.get(("corridor", "traced"))
    theirs = runs.get(("corridor", "baseline-traced"))
    if mine and theirs and mine is not theirs:
        same = all(_digest(mine, m) == _digest(theirs, m) and _digest(mine, m) for m in range(3))
        table.add("features", "off: same as baseline", "same" if same else "differ", f"= {baseline}", same)


def _digest(run, moment):
    try:
        return Picture(run.shot(moment)).digest()
    except (OSError, ValueError):
        return None


PASS_GROUPS = re.compile(r"vulkan: pass groups: draws [0-9.]+(.*)")


def pass_groups(log):
    """the last summaries' milliseconds a frame by owner (the flashlight on, the scene still)"""
    sums = []
    for match in PASS_GROUPS.finditer(log):
        groups = {}
        for name, value in re.findall(r"[;,]\s*([A-Z][A-Z ]*[A-Z]) ([0-9.]+)", match.group(1)):
            groups[name.strip()] = float(value)
        if groups:
            sums.append(groups)
    sums = sums[-3:]
    if not sums:
        return {}
    names = set().union(*sums)
    out = {name: statistics.fmean(s.get(name, 0.0) for s in sums) for name in names}
    out["frame"] = statistics.fmean(sum(s.values()) for s in sums)
    return out


def check_cost(table, timed):
    results = {}
    for run in timed:
        log = ""
        for name in ("halo.log", "debug.txt"):
            path = run.folder / "game" / name if run.folder else None
            if path and path.exists():
                log += path.read_text(errors="replace")
        results.setdefault((run.machine, run.variant), []).append(pass_groups(log))
    medians, spreads = {}, {}
    for (machine, variant), found in sorted(results.items()):
        found = [r for r in found if r]
        if not found:
            table.add("cost", f"{machine} {variant}", "missing", "pass times", False, "no debug.pass_times lines")
            continue
        medians[(machine, variant)] = {name: statistics.median(r.get(name, 0.0) for r in found)
                                       for name in set().union(*found)}
        frames = [r.get("frame", 0.0) for r in found]
        spreads[(machine, variant)] = max(frames) / min(frames) if min(frames) > 0 else float("inf")
    for (machine, variant), median in sorted(medians.items()):
        budget = BUDGETS.get(machine, {})
        if variant == "timed":
            names = sorted(set(median) - {"frame"}, key=lambda n: -median[n])
            if spreads[(machine, variant)] > TIMING_SPREAD:
                table.add("cost", f"{machine} timings agree", spreads[(machine, variant)], f"<= x{TIMING_SPREAD}", None,
                          "the runs disagree (the GPU's clock, or work beside it): its rows are n/a; run again")
                continue
            for name in ["frame"] + names:
                if name in budget:
                    table.add("cost", f"{machine} {name}", median[name], f"<= {budget[name]} ms",
                              median[name] <= budget[name])
                elif median[name] >= 0.05:
                    table.add("cost", f"{machine} {name}", median[name], "(reported)", None)
        elif variant.endswith("-off"):
            continue
        else:
            base = (machine, variant + "-off")
            if base not in medians:
                continue
            more = median["frame"] - medians[base]["frame"]
            limit = budget.get("+feature", float("inf"))
            spread = max(spreads[(machine, variant)], spreads[base])
            if spread > TIMING_SPREAD:
                table.add("cost", f"{machine} {variant[6:]} adds", more, f"<= {limit} ms", None,
                          f"the runs disagree (x{spread:.2f}): run again")
                continue
            table.add("cost", f"{machine} {variant[6:]} adds", more, f"<= {limit} ms", more <= limit)


def check_identity(table, runs, build, baseline, identity):
    for scene in ("corridor", "blast", "daylight"):
        mine = runs.get((scene, "classic"))
        theirs = runs.get((scene, "baseline")) if baseline != build else mine
        if not mine or not theirs or not mine.folder:
            continue
        moments = sorted(int(p.stem[2:]) for p in (mine.folder / "game" / "shots").glob("at*.png"))
        same = bool(moments) and all(_digest(mine, m) and _digest(mine, m) == _digest(theirs, m) for m in moments)
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
    parser.add_argument("--timing", default="omarchy,5900x",
                        help="the machines timed (comma-separated; bosgame too where its kit is; '' none)")
    parser.add_argument("--only", default="parity,shadow,hdr,cost,identity,features", help="the checks run")
    parser.add_argument("--feature", action="append", default=[], metavar="NAME=VAR=VALUE",
                        help="a feature and the setting that turns it on (default: " +
                        ", ".join(f"{k}={v}" for k, v in FEATURES.items()) + ")")
    parser.add_argument("--set", action="append", default=[], metavar="VAR=VALUE",
                        help="a setting for every run (the baseline's too): HALO_RT_RENDER_GEOMETRY=true")
    parser.add_argument("--mutate", choices=sorted(MUTATIONS), help="a negative control: break one subject")
    parser.add_argument("--work", type=Path, default=ROOT / "build" / "lighting_referee",
                        help="where the pictures and the table are kept")
    parser.add_argument("--features", help="only these features (comma-separated names)")
    parser.add_argument("--feature-baseline", help="the build the features off must equal (the state before them); "
                        "no comparison without it")
    parser.add_argument("--feature-cost", action="store_true", help="time each feature on too (three runs each)")
    parser.add_argument("--reuse", action="store_true", help="the runs already fetched into --work kept, not run again")
    parser.add_argument("--no-game-state", action="store_true", help="identity: the Classic frames only, not "
                        "identity.py's game state")
    parser.add_argument("--no-build", action="store_true", help="the builds as the machines have them")
    args = parser.parse_args()
    only = {c.strip() for c in args.only.split(",") if c.strip()}
    features = dict(FEATURES)
    for item in args.feature:
        name, _, setting = item.partition("=")
        features[name] = setting
    if args.features:
        features = {k: v for k, v in features.items() if k in args.features.split(",")}
    if "features" not in only and "cost" not in only:
        features = {}
    timing = [m for m in args.timing.split(",") if m] if "cost" in only else []
    started = time.monotonic()
    work = args.work / (slug(args.build) + (f"-{args.mutate}" if args.mutate else ""))
    work.mkdir(parents=True, exist_ok=True)
    machines = sorted({args.machine, *timing})

    # (a build without the captures gets them picked onto it; the baseline too)
    build, baseline, feature_baseline = args.build, args.baseline, args.feature_baseline
    if not args.no_build:
        for which in ("build", "baseline", "feature"):
            ref = {"build": build, "baseline": baseline, "feature": feature_baseline}[which]
            if not ref:
                continue
            if ref.startswith("local/lref/") or has_capture(ref, args.machine):
                continue
            grafted = graft(ref, machines)
            if not grafted:
                print(f"referee: {ref} has no debug.screenshot_at and the capture commits do not pick onto it")
                sys.exit(2)
            print(f"referee: {ref} lacks the captures: built as {grafted}", flush=True)
            if which == "build":
                build = grafted
            elif which == "baseline":
                baseline = grafted
            else:
                feature_baseline = grafted
        builds = sorted({(m, b) for m in machines for b in (build, baseline, feature_baseline)
                         if b and (m == args.machine or b == build)})
        with concurrent.futures.ThreadPoolExecutor(len(builds)) as pool:
            for (machine, ref), result in zip(builds, pool.map(lambda mb: build_on(*mb), builds)):
                if result.returncode != 0:
                    print(f"referee: {ref} did not build on {machine}:\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
                    sys.exit(2)
        print(f"referee: built in {time.monotonic() - started:.0f} s", flush=True)

    runs, timed = plan(build, baseline, args.machine, timing, only, args.mutate, features, args.set,
                       args.feature_cost, feature_baseline)
    # (the timing runs wait for the GPU alone, each machine's in turn; the picture runs share it, three at once)
    by_machine = {}
    for run in timed:
        by_machine.setdefault(run.machine, []).append(run)
    with concurrent.futures.ThreadPoolExecutor(len(by_machine) + 2) as pool, \
            concurrent.futures.ThreadPoolExecutor(6) as pictures_pool:
        future_identity = pool.submit(run_identity, build, baseline, args.machine) \
            if "identity" in only and baseline != build and not args.no_game_state else None
        future_timed = [pool.submit(lambda group: [play(r, work, reuse=args.reuse) for r in group], group)
                        for machine, group in by_machine.items() if machine != args.machine]
        list(pictures_pool.map(lambda r: play(r, work, reuse=args.reuse), runs))
        print(f"referee: {len(runs)} picture runs in {time.monotonic() - started:.0f} s", flush=True)
        for run in by_machine.get(args.machine, []):
            play(run, work, reuse=args.reuse)
        for future in future_timed:
            future.result()
        identity = future_identity.result() if future_identity else None
    if timed:
        print(f"referee: {len(timed)} timing runs in {time.monotonic() - started:.0f} s", flush=True)

    keyed = {(r.scene, r.variant): r for r in runs}
    table = Table()
    for run in runs + timed:
        # (the run's own summary, fetched with it, where there is one: goat-rig's output on the way may carry an
        # ssh or rsync complaint about something else)
        fetched = run.folder / "summary.txt" if run.folder else None
        if fetched and fetched.exists():
            run.summary = fetched.read_text(errors="replace")
        if not re.search(r": exit 0\b", run.summary) or re.search(r"asserts [1-9][0-9]* failed", run.summary):
            table.add("runs", run.label, "trouble", "exit 0", False,
                      run.summary.splitlines()[0] if run.summary else "no summary")
    check_whole(table, runs)
    if "parity" in only:
        check_parity(table, keyed)
    if "shadow" in only:
        check_shadow(table, keyed)
    if "hdr" in only:
        check_hdr(table, keyed)
    if features and "features" in only:
        check_features(table, keyed, features, feature_baseline)
    if "cost" in only:
        check_cost(table, timed)
    if "identity" in only:
        check_identity(table, keyed, build, baseline, identity)
    text = table.text()
    failed = sum(r["passed"] is False for r in table.rows)
    verdict = "FAIL" if failed else "PASS"
    footer = (f"referee: {args.build}{' (mutation ' + args.mutate + ')' if args.mutate else ''}: {verdict}, "
              f"{sum(r['passed'] is True for r in table.rows)} passed, {failed} failed, in "
              f"{time.monotonic() - started:.0f} s; pictures in {work}")
    print(text + "\n" + footer)
    (work / "table.txt").write_text(text + "\n" + footer + "\n")
    (work / "summary.json").write_text(json.dumps(
        {"build": args.build, "built_as": build, "baseline": baseline, "mutation": args.mutate, "verdict": verdict,
         "passed": sum(r["passed"] is True for r in table.rows), "failed": failed,
         "seconds": round(time.monotonic() - started), "rows": table.rows}, indent=1))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
