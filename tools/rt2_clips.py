#!/usr/bin/env python3
"""The ray-traced lighting's standard clips: the same moments, played live from a script, side by side.

    python3 tools/rt2_clips.py --build master --out ~/Desktop/clips
        each moment played twice by one build: Experimental > Ray-Traced Lighting off (classic) | on
    python3 tools/rt2_clips.py --build my-branch --base master --moments b30_sun,c20_flashlight --out DIR
        the base build | this build, both with the setting on
    python3 tools/rt2_clips.py --build master --moments a10_cryo --repeat --out DIR
        the build's side played twice: the frames compared (a moment must play the same every time)
    python3 tools/rt2_clips.py --list

Each moment is a live game on its own map, staged by the console's cues and the scripted controller
(debug.network_test_cues, debug.test_input "script:") on the scenario's clock, with the rig's fixed random seed and
a fixed frame time (goat-rig run --video 60: each frame is 1/60 s of game time). So a moment plays the same frames
every run and in every build that plays the game the same: no recording to go stale, and the game itself makes its
ray scene, lights and MCC textures in each build. Played as players run it: the menu setting only
(display.ray_traced_lighting, HALO_RAY_TRACED_LIGHTING), no HALO_LT_* settings, the MCC import linked (omarchy's
~/mcc-import-data/mcc_import), the HUD hidden (display.hide_hud; the PRACTICE mark stays, as for any staged game).

It runs on the rig (omarchy by default; --host 5900x checks that its owner is not playing before every run, as
goat-rig does too): builds there (goat-rig build), plays each side there (goat-rig run --shared: pictures, not
timings, up to 3 at once), puts the side by side there with ffmpeg (labels, the frame number and game second on
every frame), a contact sheet and three stills, and copies only those to --out. The full videos stay in
~/goat-rig/rt2-clips/<name>/ on the rig; the runs' own folders are removed.
"""

import argparse
import base64
import concurrent.futures
import os
import shlex
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "tools" / "rig" / "goat-rig"
REMOTE = "goat-rig/rt2-clips"  # (under the rig's $HOME)
SIZE = "1280x720"
FPS = 60
OWNER_GAME = "^$HOME/Applications/OpenH1[^/ ]*/bin/openh1"
MCC_IMPORT = "@link/mcc_import=~/mcc-import-data/mcc_import"

# every side of every moment: the menu setting's side is added (HALO_RAY_TRACED_LIGHTING)
COMMON = [
    "HALO_SKIP_CINEMATICS=1",
    "HALO_NETWORK_TEST_START=2",
    "HALO_HIDE_HUD=1",
    # (the Spartan link's key hint, shown once for a time on the real clock, taken as shown)
    "HALO_SPARTAN_LINK_HINT=true",
    MCC_IMPORT,
]


def cam(x, y, z, yaw, pitch, seconds=0):
    """a debug_camera_set line: the script's camera at a place (degrees, 70 degrees across), moved there over the
    seconds (whole) when given"""
    move = f" {seconds}" if seconds else ""
    return f'debug_camera_set "{x} {y} {z} {yaw} {pitch} 0 70{move}"'


PLAYER = "(unit (list_get (players) 0))"

# name: (map, the clip's first and last second on the scenario's clock, cues (second, console line), the scripted
# controller (debug.test_input "script:" events), what it shows). The scenario's clock starts when the player has a
# unit; a cue runs at the first tick at or after its second.
MOMENTS = {
    # (Damnation, a multiplayer map in a local game: the script's camera held in the lower hall by the spotlights,
    # where the live picture was a tenth of the classic's)
    # (b40's canyon by a Covenant torch (levels\b40\scenery\b40_ctorch, a baked point light), BSP 1, a held camera)
    "b40_torch": (
        "levels\\b40\\b40", 44, 50,
        [(1, "object_cannot_take_damage (players)"), (2, "ai_erase_all"), (38, "switch_bsp 1"), (39, "ai_erase_all"),
         (40, cam(156, -398, 22.5, 215, -15))],
        "",
        "b40's canyon by a Covenant torch, a held camera"),
    "damnation_hall": (
        "local:damnation:slayer", 16, 22,
        [(15, cam(-2.0, -8.8, 6.5, 0, -10))],
        "",
        "Damnation's lower hall by its spotlights, a held camera"),
    # (b30 opens with the Pelican's ride, a cutscene skipped by playing it fast, undrawn: the beach from about 29 s,
    # the Pelicans landed until about 40 s)
    "b30_sun": (
        "levels\\b30\\b30", 30, 42,
        [(26, "ai_erase_all"),
         (27, 'debug_object_place "vehicles\\warthog\\warthog -63.0 24.0 1.6 200"'),
         (27.5, 'debug_object_place "characters\\elite\\elite -64.0 21.0 1.6 200"'),
         (29, cam(-73.0, 16.0, 3.4, 50, -9)),
         (30, cam(-70.0, 20.0, 3.4, 25, -9, 12))],
        "",
        "b30's beach in the sun: the Pelicans landing, a Warthog, an Elite, their shadows on the sand; a slow camera move"),
    # (the landing zone's Covenant camp, beach_lz, placed afresh as the player lands (about 36 s), the marines taken
    # away so that it fires at the player, who cannot die; the player turned toward it)
    "b30_fight": (
        "levels\\b30\\b30", 37, 49,
        [(26, "cheat_deathless_player true"),
         (28, "ai_erase beach_lz_marine"),
         (28.5, "ai_erase beach_lz"),
         (33, "ai_place beach_lz")],
        "36.5 aim=-73,-4; 37-49 hunt; 38-40 fire; 42-42.4 throw; 46-46.4 throw",
        "b30's landing zone in first person: the Covenant camp's plasma and needles, the rifle's muzzle flashes, two "
        "grenades"),
    # (the opening's Pelican flight, the player in its bay: the bay lit by the game's model lighting where no sky is
    # open above (the forward receivers' interim), its grates and the marines by the pass)
    "b30_bay": (
        "levels\\b30\\b30", 31, 43,
        [],
        "29-33 move=0,1; 33-43 hunt",
        "b30's opening in first person inside the Pelican's bay (its ceiling, its lamps, the marines), then the landing"),
    # (the landing zone's camp's two portable shields, scenery placed by the scenario at about (-53.6, 8.8) and
    # (-57.5, 6.3), the camp placed afresh, the marines taken away; the script's camera beside them, a slow pan)
    "b30_shields": (
        "levels\\b30\\b30", 37, 49,
        [(26, "cheat_deathless_player true"),
         (28, "ai_erase beach_lz_marine"),
         (28.5, "ai_erase beach_lz"),
         (33, "ai_place beach_lz"),
         (36.5, cam(-49.0, 12.0, 5.6, 205, -11)),
         (37, cam(-49.0, 12.0, 5.6, 225, -11, 12))],
        "",
        "b30's landing zone camp: the Covenant's portable shields (purple, drawn adding), the ground and the Covenant "
        "about them; a slow pan"),
    # (a50's opening at night on the cliff, after its cutscene (skipped by playing it fast, undrawn: the clip comes
    # after it): its two portable shields at about (0, -28.2) and (-3.8, -25.6), the camera close by, a slow pan)
    "a50_shields": (
        "levels\\a50\\a50", 40, 52,
        [(39.5, cam(3.0, -30.0, 88.8, 140, -18)),
         (40, cam(3.0, -30.0, 88.8, 165, -18, 12))],
        "",
        "a50's opening at night: the Covenant's portable shields on the cliff, the ground about them; a slow pan"),
    # (a30's BSP 1, the emitter screen's first spot: a lamp-lit room with no solved file, black live before the one
    # rule; the sweep's scene_a30_bsp1)
    "a30_bsp1": (
        "levels\\a30\\a30", 34, 40,
        [(30, "ai_erase_all"), (30.5, "switch_bsp 1"), (33, cam(-340.98, 43.52, 44.32, 255, -9)),
         (34, cam(-340.98, 43.52, 44.32, 285, -9, 6))],
        "",
        "a30's BSP 1: a lamp-lit room (the emitter screen's spot 1), a slow pan"),
    "a10_cryo": (
        "levels\\a10\\a10", 12, 24,
        [(11.5, cam(-52.0, 0.0, 0.6, 180, -12)),
         (12, cam(-51.0, -2.2, 1.5, 158, -16, 12))],
        "",
        "a10's cryo deck, live: the floor grates, the ceiling lamps, the pods and panels; a slow camera move"),
    "a10_shine": (
        "levels\\a10\\a10", 12, 24,
        [(11.5, cam(-54.0, 4.0, 1.6, -90, -15)),
         (12, cam(-54.0, -4.0, 1.6, -90, -15, 12))],
        "",
        "a10's cryo deck's shiny floor and metal at a low angle (the original's reflections); a slow sideways move"),
    "c20_flashlight": (
        "levels\\c20\\c20", 5, 17,
        # (its desired state set again: the first may come while the fly-in, a skipped cutscene, still holds the
        # player)
        [(2, "ai_erase_all")] + [(t, f"unit_set_desired_flashlight_state {PLAYER} 1") for t in (3, 4.5, 6)],
        "5-11 move=0,1; 11-17 look=0.15,0",
        "c20's Library in first person with the flashlight on (by the console: the scripted button toggles it each "
        "tick it is held); walking up to a wall, then a slow turn along it"),
    # (the emitter files' BSPs, indoors where the lamps light it: the start, then switch_bsp to the BSP and the
    # script's camera at a spot inside a lamp-lit room (picked from the BSP's mesh beside its strongest lamp groups); a
    # slow pan. The level's own scripts still run: it shows the BSP as lit, not a moment of play. b40 and c10
    # open with a cutscene, skipped by playing it fast, undrawn, which runs the game's clock past a clip in it: their
    # clips come after it)
    "b40_bsp8": (
        "levels\\b40\\b40", 50, 60,
        [(46, "ai_erase_all"), (47, "switch_bsp 8"),
         (49, cam(269.48, -577.3, 23.87, 240, -9)), (50, cam(269.48, -577.3, 23.87, 270, -9, 10))],
        "",
        "b40's BSP 8 (cells x1.12): a lamp-lit room, live from its emitter file; a slow pan"),
    "c10_bsp1": (
        "levels\\c10\\c10", 50, 60,
        [(46, "ai_erase_all"), (47, "switch_bsp 1"),
         (49, cam(-53.16, -6.05, 2.86, -15, -9)), (50, cam(-53.16, -6.05, 2.86, 15, -9, 10))],
        "",
        "c10's BSP 1 (the worst cells: x3.60): a lamp-lit room, live from its emitter file; a slow pan"),
    "d40_bsp4": (
        "levels\\d40\\d40", 7, 17,
        [(3, "ai_erase_all"), (4, "switch_bsp 4"),
         (6, cam(-92.43, -5.21, 8.11, 285, -9)), (7, cam(-92.43, -5.21, 8.11, 315, -9, 10))],
        "",
        "d40's BSP 4 (x1.24): a lamp-lit room, live from its emitter file; a slow pan"),
}


# the moments not yet frame-identical run to run (on-demand pipeline compiles still land on different frames there:
# about 15% and 96% of frames differ); played only when named, not by "all"
FOLLOW_UP = {"damnation_hall", "b40_torch", "a30_bsp1"}


def say(*words):
    with LOCK:
        print(*words, file=sys.stderr, flush=True)


LOCK = threading.Lock()


def rig(host, *argv, capture=False):
    """goat-rig on the rig (tools/rig/goat-rig @host)"""
    result = subprocess.run([sys.executable, str(RIG), f"@{host}", *argv], text=True,
                            capture_output=capture)
    return result


def ssh(host, script, capture=True, check=True):
    if windows(host):
        result = subprocess.run(remote_argv(host, script), stdin=subprocess.DEVNULL, text=True,
                                capture_output=capture)
    else:
        result = subprocess.run(["ssh", "-o", "BatchMode=yes", rig_target(host), "bash", "-s"], input=script,
                                text=True, capture_output=capture)
    if check and result.returncode:
        raise SystemExit(f"rt2_clips: {host}: {(result.stderr or '').strip()[-400:]}")
    return result


def rig_target(host):
    hosts = {"omarchy": "omarchy.fabric", "5900x": "5900x.fabric"}
    path = Path.home() / ".config/goat-rig/hosts"
    if path.exists():
        for line in path.read_text().splitlines():
            if len(line.split()) == 2 and not line.startswith("#"):
                hosts[line.split()[0]] = line.split()[1]
    return hosts.get(host, host)


def owner_playing(host):
    """the machine owner's own game running (any ~/Applications/OpenH1* install; in the Windows VM any game)"""
    if windows(host):
        result = ssh(host, WINDOWS_GAME_RUNNING)
    else:
        result = ssh(host, f'pgrep -fa "{OWNER_GAME}" || true')
    return result.stdout.strip()


# ---------- bosgame: the Windows VM with the RTX 3090 (tools/rig/README.md's "bosgame"), the same scripts there
#
# The VM refuses a Mac's ssh: it is reached in hops, each hop's ssh running the next with the rest quoted, by
# default through omarchy (which builds for it), then bosgame's host (where its locks are), then the VM, whose ssh
# shell, PowerShell, starts Git Bash (C:\Program Files\Git) on the script. So the scripts the Linux rigs run (bash,
# coreutils, ~ for the home) run there too, MSYS handing the native programs Windows paths. The script goes inline
# as base64 (nothing quoted through PowerShell's native arguments), leaving standard input to the command (the
# replay's protocol, a tar stream). RT2_BOSGAME_ROUTE replaces the hops, e.g.
# "<user>@<another machine>,bosgame,halo@192.168.122.119" for a route through another machine bosgame admits.

WINDOWS_HOSTS = ("bosgame",)
WINDOWS_ROUTE = "omarchy,bosgame.fabric,halo@192.168.122.119"
WINDOWS_BUILDER = "omarchy"  # (goat-rig @bosgame build: cross-compiled there, in builds/<name>.win)
VM_BASH = r"& 'C:\Program Files\Git\bin\bash.exe'"
# on the VM's host, around a replay: goat-rig's bosgame lock and winvm's own, shared (a game run there waits)
WINDOWS_LOCKS = "flock -s ~/goat-rig/bosgame.lock flock -s ~/winvm/.lock "
# a game in the VM (a player's or a run's: OpenH1, goath1 or the bench's halo.exe), not a replay
WINDOWS_GAME_RUNNING = ("tasklist //NH //FO CSV | tr -d '\\r' | grep -iE '^\"(halo|openh1|goath1)[^\"]*\\.exe\"' "
                        "| grep -vi '^\"halo_replay' || true")
# the MCC import's data in the VM (copied once from omarchy's), linked into each recording's data folder
WINDOWS_MCC_IMPORT = "goat-rig/mcc_import"
WINDOWS_MAPS = r"C:\openh1\data\maps"


def windows(host):
    return host in WINDOWS_HOSTS


def windows_route():
    hops = [h for h in os.environ.get("RT2_BOSGAME_ROUTE", WINDOWS_ROUTE).split(",") if h]
    return [rig_target(hops[0])] + hops[1:]


def remote_argv(host, script, prefix=""):
    """the argv that runs a bash script on the host, standard input left to it: one ssh to a Linux rig, or the hops
    to the Windows VM (prefix: a command on the VM's host that the last ssh runs under)"""
    if not windows(host):
        return ["ssh", "-o", "BatchMode=yes", rig_target(host), "bash -c " + shlex.quote(script)]
    encoded = base64.b64encode(script.encode()).decode()
    words = f"{VM_BASH} -c '. <(base64 -d <<<{encoded})'"
    hops = windows_route()
    for i in range(len(hops) - 1, 0, -1):
        words = (prefix if i == len(hops) - 1 else "") + f"ssh -o BatchMode=yes {hops[i]} " + shlex.quote(words)
    return ["ssh", "-o", "BatchMode=yes", hops[0], words]


def windows_settings(moment, data):
    """a moment's settings for a game in the VM (goat-rig @bosgame run's, as settings() gives a Linux rig's): its
    own data folder (data, a Windows path: the maps and the MCC import linked there by windows_data_folder, as
    goat-rig run's @link does), no bots, the window at SIZE"""
    env = [e for e in settings(moment, True) if not e.startswith("@")]
    return env + [f"HALO_DATA_ROOT={data}", "HALO_BOTS=", f"HALO_WINDOW_SIZE={SIZE}"]


def windows_data_folder(host, folder):
    """~/<folder> in the VM made a game's data folder: the maps and the MCC import linked (junctions), the import
    copied from omarchy's first when the VM has none yet"""
    have = ssh(host, f"test -e ~/{WINDOWS_MCC_IMPORT}/.copied && echo yes || true").stdout.strip()
    if have != "yes":
        say(f"rt2_clips: copying the MCC import's data from {WINDOWS_BUILDER} into the VM (once)")
        source = subprocess.Popen(["ssh", "-o", "BatchMode=yes", rig_target(WINDOWS_BUILDER),
                                   "tar -C ~/mcc-import-data/mcc_import -cf - ."], stdout=subprocess.PIPE)
        to = f"~/{WINDOWS_MCC_IMPORT}"
        sink = subprocess.run(remote_argv(host, f"set -e; rm -rf {to}; mkdir -p {to}; tar -xf - -C {to}; "
                                                f"touch {to}/.copied"),
                              stdin=source.stdout, capture_output=True, text=True)
        source.stdout.close()
        if source.wait() or sink.returncode:
            raise SystemExit(f"rt2_clips: the MCC import's copy failed: {sink.stderr.strip()[-400:]}")
    ssh(host, f"""set -e
mkdir -p ~/{folder}
{windows_unlink(folder)}
cd ~/{folder}
cmd //c mklink //J maps '{WINDOWS_MAPS}' >/dev/null
cmd //c mklink //J mcc_import "$(cygpath -w ~/{WINDOWS_MCC_IMPORT})" >/dev/null
""")


def windows_unlink(folder):
    """a bash line removing a data folder's junctions alone (before an rm -rf of it, to be sure of their targets)"""
    return (f"for link in ~/{folder}/maps ~/{folder}/mcc_import; do if [ -e $link ]; then "
            f"cmd //c rmdir \"$(cygpath -w $link)\" >/dev/null; fi; done")


def settings(moment, lighting):
    """a side's settings after goat-rig run's --"""
    level, start, end, cues, script, _ = MOMENTS[moment]
    lines = [f"{second:g} {line}" for second, line in cues]
    env = COMMON + [
        f"HALO_RAY_TRACED_LIGHTING={'1' if lighting else '0'}",
        # (a level named with its game, a multiplayer map's "local:<map>:<game>", as it is)
        f"HALO_NETWORK_TEST={level if ':' in level else 'campaign:' + level}",
        "HALO_NETWORK_TEST_CUES=" + "|".join(lines),
        f"HALO_SCREENSHOT_FROM={start:g}",
        f"HALO_SCREENSHOT_UNTIL={end:g}",
        # (the game quits once the clip is played: debug.trace_seconds, the trace written into the run's data)
        "HALO_TRACE=clip.trace",
        f"HALO_TRACE_SECONDS={end + 0.25:g}",
    ]
    if script:
        env.append(f"HALO_TEST_INPUT=script:{script}")
    return env


def play(host, label, build, moment, lighting, seconds):
    """one side played on the rig: its video kept as ~/goat-rig/rt2-clips/<label>.mp4"""
    if host != "omarchy":
        who = owner_playing(host)
        if who:
            raise SystemExit(f"rt2_clips: {host}'s owner is playing ({who}); not starting")
    say(f"rt2_clips: playing {label} ({build}, ray-traced lighting {'on' if lighting else 'off'})")
    result = rig(host, "run", label, "--build", build, "--shared", "--size", SIZE, "--video", str(FPS),
                 "--seconds", str(seconds), "--", *settings(moment, lighting), capture=True)
    tail = (result.stdout + result.stderr).strip().splitlines()
    if result.returncode or not any("video" in line for line in tail):
        raise SystemExit(f"rt2_clips: {label}: the run failed:\n" + "\n".join(tail[-12:]))
    return label


def compose(host, folder, moment, left, right, left_label, right_label, start):
    """the side by side (labels; the frame number and the game second on every frame), the contact sheet and three
    stills, made on the rig from the two runs' videos, which are moved into the folder; the runs removed"""
    _, _, end, _, _, what = MOMENTS[moment]
    frames = int((end - start) * FPS)
    stills = [frames // 6, frames // 2, frames * 5 // 6]
    width, height = (int(n) for n in SIZE.split("x"))

    def label(text):
        return ("drawtext=text='" + text.replace(":", "\\:").replace("'", "") +
                "':x=12:y=10:fontsize=26:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=6")
    stamp = (f"drawtext=text='{moment}  frame %{{n}}  t %{{eif\\:{start}+n/{FPS}\\:d\\:2}}.%{{eif\\:mod(n*100/{FPS}\\,100)\\:d\\:2}} s'"
             ":x=(w-tw)/2:y=h-th-12:fontsize=24:fontcolor=white:box=1:boxcolor=black@0.55:boxborderw=6")
    graph = (f"[0:v]{label(left_label)}[a];[1:v]{label(right_label)}[b];[a][b]hstack=inputs=2,{stamp}[v]")
    select = "+".join(f"eq(n\\,{n})" for n in stills)
    script = f"""set -e
cd ~/{folder}
for pair in "{left} left" "{right} right"; do set -- $pair
  if [ -e ~/goat-rig/runs/$1 ]; then
    mv ~/goat-rig/runs/$1/game/video.mp4 {moment}_$2.mp4
    cp ~/goat-rig/runs/$1/game/halo.log {moment}_$2.log
    cp ~/goat-rig/runs/$1/summary.txt {moment}_$2.summary.txt
    cp ~/goat-rig/runs/$1/game/video.framemd5 {moment}_$2.framemd5
  fi
done
rm -rf ~/goat-rig/runs/{left} ~/goat-rig/runs/{right}
ffmpeg -nostdin -loglevel error -y -i {moment}_left.mp4 -i {moment}_right.mp4 -filter_complex "{graph}" -map "[v]" \\
  -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -movflags +faststart {moment}.mp4
ffmpeg -nostdin -loglevel error -y -i {moment}.mp4 -vf "select='not(mod(n\\,{max(1, frames // 12)}))',scale={width}:-1,tile=2x6" \\
  -frames:v 1 -q:v 3 {moment}_sheet.jpg
ffmpeg -nostdin -loglevel error -y -i {moment}.mp4 -vf "select='{select}'" -fps_mode vfr -q:v 2 {moment}_still%d.jpg
"""
    ssh(host, script)
    say(f"rt2_clips: {moment}: {what}")


def frames_alike(host, folder, moment):
    """(the frames of the two runs of --repeat) how many of the left's and right's frames are identical"""
    script = f"cd ~/{folder} && paste <(grep -v '^#' {moment}_left.framemd5 | awk -F, '{{print $6}}') " \
             f"<(grep -v '^#' {moment}_right.framemd5 | awk -F, '{{print $6}}') | " \
             "awk '{n++; if ($1==$2) same++} END {print same+0, n+0}'"
    same, total = ssh(host, script).stdout.split()
    return int(same), int(total)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build", help="the build (a branch, sha or pr/N goat-rig builds)")
    parser.add_argument("--base", help="the base build: base | build, both with the setting on (else classic | "
                        "the build with the setting on)")
    parser.add_argument("--moments", default="all", help="all, or names separated by commas (--list)")
    parser.add_argument("--repeat", action="store_true", help="the build's side played twice, its frames compared")
    parser.add_argument("--classic", action="store_true", help="with --repeat: the classic side (the setting off)")
    parser.add_argument("--host", default="omarchy", choices=("omarchy", "5900x"))
    parser.add_argument("--name", help="the rig's folder ~/goat-rig/rt2-clips/<name> (default from the builds)")
    parser.add_argument("--out", help="a local folder for the side-by-side MP4s, sheets and stills")
    parser.add_argument("--seconds", type=float, default=420, help="each run's limit in real seconds (default 420)")
    parser.add_argument("--list", action="store_true", help="the moments")
    args = parser.parse_args()
    if args.list:
        for name, moment in MOMENTS.items():
            print(f"{name:16} {moment[0]:18} {moment[1]:g}-{moment[2]:g} s  {moment[5]}"
                  + ("  (follow-up: not yet frame-identical)" if name in FOLLOW_UP else ""))
        return 0
    if not args.build:
        parser.error("--build is needed")
    moments = [m for m in MOMENTS if m not in FOLLOW_UP] if args.moments == "all" else args.moments.split(",")
    for moment in moments:
        if moment not in MOMENTS:
            parser.error(f"{moment}: not a moment ({', '.join(MOMENTS)})")
    if args.base and args.repeat:
        parser.error("--repeat plays one build: no --base")
    safe = args.build.replace("/", "-")
    name = args.name or (f"{safe}-repeat" if args.repeat else
                         f"{args.base.replace('/', '-')}-vs-{safe}" if args.base else f"{safe}")
    folder = f"{REMOTE}/{name}"

    for ref in dict.fromkeys(r for r in (args.base, args.build) if r):
        result = rig(args.host, "build", ref, capture=True)
        if result.returncode:
            raise SystemExit(f"rt2_clips: building {ref} failed:\n{result.stdout[-2000:]}{result.stderr[-2000:]}")
        say(f"rt2_clips: {ref} built on {args.host}")
    ssh(args.host, f"mkdir -p ~/{folder}")

    if args.repeat:
        lit = not args.classic
        sides = [(args.build, lit, f"{args.build} (run {n})") for n in (1, 2)]
    elif args.base:
        sides = [(args.base, True, f"{args.base} (ray-traced lighting on)"),
                 (args.build, True, f"{args.build} (ray-traced lighting on)")]
    else:
        sides = [(args.build, False, "classic (ray-traced lighting off)"),
                 (args.build, True, f"{args.build}: ray-traced lighting on")]
    started = time.monotonic()
    failed = []
    # (both sides of a moment at once: goat-rig's shared GPU slots hold 3)
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        for moment in moments:
            labels = [f"clips-{name}-{moment}-{i}"[:80] for i in (1, 2)]
            try:
                runs = [pool.submit(play, args.host, label, ref, moment, lighting, args.seconds)
                        for label, (ref, lighting, _) in zip(labels, sides)]
                left, right = (run.result() for run in runs)
                compose(args.host, folder, moment, left, right, sides[0][2], sides[1][2], MOMENTS[moment][1])
            except SystemExit as error:
                say(str(error))
                failed.append(moment)
                continue
            if args.repeat:
                same, total = frames_alike(args.host, folder, moment)
                say(f"rt2_clips: {moment}: {same} of {total} frames identical in both runs"
                    + ("" if same == total and total else "  NOT DETERMINISTIC"))
                if same != total or not total:
                    failed.append(moment)
    if args.out:
        out = Path(args.out).expanduser()
        out.mkdir(parents=True, exist_ok=True)
        wanted = [f"{folder}/{m}{suffix}" for m in moments if m not in failed or args.repeat
                  for suffix in (".mp4", "_sheet.jpg", "_still1.jpg", "_still2.jpg", "_still3.jpg")]
        if wanted:
            subprocess.run(["scp", "-q", "-o", "BatchMode=yes",
                            *[f"{rig_target(args.host)}:{path}" for path in wanted], str(out)])
        say(f"rt2_clips: copied to {out}")
    say(f"rt2_clips: {len(moments) - len(failed)} of {len(moments)} moments in {time.monotonic() - started:.0f} s; "
        f"the full videos in {args.host}:~/{folder}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
