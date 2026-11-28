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
import concurrent.futures
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
    "b30_sun": (
        "levels\\b30\\b30", 8, 20,
        [(3, "ai_erase_all"),
         (4, 'debug_object_place "vehicles\\warthog\\warthog -63.5 24.0 1.4 200"'),
         (4.5, 'debug_object_place "characters\\elite\\elite -60.5 22.5 1.4 160"'),
         (7.5, cam(-72.0, 14.0, 3.2, 35, -9)),
         (8, cam(-70.0, 28.0, 3.6, -30, -11, 12))],
        "",
        "b30's beach in the sun: the Pelicans, a Warthog, an Elite, their shadows on the sand; a slow camera move"),
    "b30_fight": (
        "levels\\b30\\b30", 6, 18,
        [(1, "cheat_deathless_player 1")],
        "3-18 hunt; 9-9.4 throw; 13-13.4 throw",
        "b30's beach firefight in first person: Covenant plasma and needles, two grenades, the rifle's muzzle flashes"),
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
        [(2, "ai_erase_all"),
         (3, f"unit_set_desired_flashlight_state {PLAYER} 1")],
        "5-8 move=0,0.6; 8-14 look=-0.22,0; 8-14 move=0,0.4",
        "c20's Library in first person with the flashlight on (by the console: the scripted button toggles it each "
        "tick it is held); walking, then a slow turn"),
}


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
    target = rig_target(host)
    result = subprocess.run(["ssh", "-o", "BatchMode=yes", target, "bash", "-s"], input=script, text=True,
                            capture_output=capture)
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
    """the machine owner's own game running (any ~/Applications/OpenH1* install)"""
    result = ssh(host, f'pgrep -fa "{OWNER_GAME}" || true')
    return result.stdout.strip()


def settings(moment, lighting):
    """a side's settings after goat-rig run's --"""
    level, start, end, cues, script, _ = MOMENTS[moment]
    lines = [f"{second:g} {line}" for second, line in cues]
    env = COMMON + [
        f"HALO_RAY_TRACED_LIGHTING={'1' if lighting else '0'}",
        f"HALO_NETWORK_TEST=campaign:{level}",
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
  fi
done
rm -rf ~/goat-rig/runs/{left} ~/goat-rig/runs/{right}
ffmpeg -nostdin -loglevel error -y -i {moment}_left.mp4 -i {moment}_right.mp4 -filter_complex "{graph}" -map "[v]" \\
  -c:v libx264 -preset medium -crf 20 -pix_fmt yuv420p -movflags +faststart {moment}.mp4
ffmpeg -nostdin -loglevel error -y -i {moment}.mp4 -vf "select='not(mod(n\\,{max(1, frames // 12)}))',scale={width}:-1,tile=2x6" \\
  -frames:v 1 -q:v 3 {moment}_sheet.jpg
ffmpeg -nostdin -loglevel error -y -i {moment}.mp4 -vf "select='{select}'" -fps_mode vfr -q:v 2 {moment}_still%d.jpg
for side in left right; do ffmpeg -nostdin -loglevel error -i {moment}_$side.mp4 -f framemd5 - > {moment}_$side.framemd5; done
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
            print(f"{name:16} {moment[0]:18} {moment[1]:g}-{moment[2]:g} s  {moment[5]}")
        return 0
    if not args.build:
        parser.error("--build is needed")
    moments = list(MOMENTS) if args.moments == "all" else args.moments.split(",")
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
