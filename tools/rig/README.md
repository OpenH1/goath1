# goat-rig: the test machines' shared kit

Build and run the game on **omarchy.fabric** (32 cores, has the MCC maps) and **5900x.fabric** (24 cores) without
cloning, building from zero or writing run scripts. Everything lives in `~/goat-rig` on each machine.

From a Mac checkout, `tools/rig/goat-rig @omarchy <command>` (or `@5900x`) runs the command on that machine over
ssh. The 5900X's fish shell never sees the arguments, so quote them as you would for bash. On the machine itself,
run `~/goat-rig/bin/goat-rig <command>`. The ssh target is `<name>.fabric` unless `~/.config/goat-rig/hosts` on the
Mac gives a `<name> <user@host>` line for it.

```sh
tools/rig/goat-rig @omarchy build my-branch          # prints the binary's path
tools/rig/goat-rig @omarchy run look --build my-branch --shots at:12 -- HALO_NETWORK_TEST=local:bloodgulch:slayer
tools/rig/goat-rig fetch omarchy:look ~/Desktop/look  # pictures, video, logs back to the Mac
```

## Recipes

**1. One game, one picture** (12 s into the game; `at:12+4` keeps 5 frames):
```sh
goat-rig run shot --build my-branch --seconds 40 --shots at:12 -- \
  HALO_NETWORK_TEST=local:bloodgulch:slayer HALO_NETWORK_TEST_START=2
```

**2. Host and join, two instances** (Slayer; `bot:N` plays each one):
```sh
goat-rig run join --pair host:my-branch client:my-branch --seconds 70 --shots every:1200 -- \
  HALO_NETWORK_TEST=host:bloodgulch:slayer HALO_NETWORK_TEST_START=15 HALO_NETWORK_TEST_NAME=Hosty HALO_TEST_INPUT=bot:1 -- \
  HALO_NETWORK_TEST=join HALO_NETWORK_TEST_NAME=Joiny HALO_TEST_INPUT=bot:2
```
The settings after the first `--` are the host's, after the second the client's (`--env VAR=…` sets one for all).

**3. Split screen with player 2's input** (`p2 ` in `script:`, `p2:` in `menu:`): player 2 walks, then pauses.
```sh
goat-rig run split-pause --seconds 30 --shots at:10 -- HALO_NETWORK_TEST=local:bloodgulch:slayer \
  HALO_NETWORK_TEST_PLAYERS=2 HALO_NETWORK_TEST_START=2 "HALO_TEST_INPUT=script:5-7 p2 move=0,1; 8 p2 start"
```

**4. Co-op campaign, intro skipped** (a10). Both players vote to skip the intro with A, at 35 s and 39 s
on the menus' clock. The host's debug.txt then logs `co-op: skipping the cutscene (2 of 2 voted)`. The cue runs
a console line at a game second, counted from when the host's player has a unit.
```sh
V="menu:$(printf 'wait %.0s' $(seq 35))a wait wait wait a $(printf 'wait %.0s' $(seq 70))"
goat-rig run coop --pair host:my-branch client:my-branch --seconds 110 --shots every:900 -- \
  'HALO_NETWORK_TEST=host:levels\a10\a10:coop' HALO_NETWORK_TEST_START=12 \
  'HALO_NETWORK_TEST_CUES=40 player_enable_input 1' "HALO_TEST_INPUT=$V" -- HALO_NETWORK_TEST=join "HALO_TEST_INPUT=$V"
```
After the skip, the host stays in the cryo pod and the client spectates it ("You join them when they're on
foot"), even with the cue. The cross-play check let the intro play out and gave input back with
`'HALO_NETWORK_TEST_CUES=128 player_enable_input 1'` and `HALO_NETWORK_TEST_CUE_PLAYERS=1`.
Friends, invites and party follow need `--online` (a local MQTT broker and STUN inside the run's network). Use
`{run:<instance>}` for another instance's folder: `"HALO_FRIENDS=3 add @{run:client}/friends.txt"`.

**5. Fast headless determinism**:
```sh
goat-rig exec my-branch -- python3 tools/scorecard/identity.py --repeat                       # must pass
goat-rig exec my-branch -- python3 tools/scorecard/identity.py --repeat --mutation unseeded   # must fail
goat-rig run soak --fast --map bloodgulch --seconds 20 -- HALO_TEST_INPUT=bot:1   # about 200x real time
```
`--fast` draws nothing and runs at a fixed step, so it holds no GPU lock and several runs can go at once. Only a
game of one machine (`--map`) runs faster than real time. A `local:` or networked game runs at real time.

**6. Video at 60 fps** (each frame is 1/60 s of game time; `HALO_SCREENSHOT_FROM/UNTIL` pick the seconds):
```sh
goat-rig run clip --seconds 25 --video 60 -- HALO_NETWORK_TEST=local:bloodgulch:slayer HALO_NETWORK_TEST_START=2 \
  HALO_SCREENSHOT_FROM=8 HALO_SCREENSHOT_UNTIL=12 "HALO_TEST_INPUT=script:6-14 move=0,1; 8-12 look=0.3,0"
```

`video.framemd5` beside it holds each frame's MD5 as the game drew it (before the lossy encoding), so two runs'
frames compare exactly; `tools/rt2_clips.py --repeat` does that for the ray tracing's standard moments.

**7. OpenCE 359 as one side** (omarchy only; the build must have cross-play):
```sh
goat-rig run xplay --pair host:opence client:my-branch --seconds 75 --shots every:1500 -- \
  HALO_NETWORK_TEST=host:bloodgulch:slayer HALO_NETWORK_TEST_START=15 -- HALO_NETWORK_TEST=join
```
OpenCE always uses port 5150, so a pair with it runs only inside the run's private network, which never reaches
the real one.

**8. MCC maps** (omarchy only; never copy `~/goat-rig/mcc-maps` off omarchy). `--mcc host` gives the first
instance the maps and the others only `_shared` (to test the download). `--mcc all` gives every instance the maps.
```sh
goat-rig run mcc --mcc host --seconds 70 --shots at:25 -- \
  'HALO_NETWORK_TEST=host:mcc_maps\3021510694\mp_plains:slayer' HALO_NETWORK_TEST_START=10
```

**9. menu: the main menu as a player sees it** (its 3D scene, the ring and the sky, drawn; nothing pressed):
```sh
goat-rig run menu --build my-branch --menu 20 --shots at:12
```
`--menu [SECONDS]` (20 by default) is one game with no network test, which every other recipe jumps past the
menu with, so the menu's own scene is drawn and prepared. The summary then says whether only the ui map loaded
and stayed up. Any renderer or menu change gets one `--menu` run as part of its proof.

**10. Stress test with a co-op partner** (5900X, 4K): the host runs the stress test (a30, `benchmark.c`) and
waits for the client, which joins as its co-op partner without drawing anything (null renderer, tiny window). The
host keeps the partner updated over loopback and draws their Spartan link feed.
```sh
goat-rig run stress-pair --pair host:my-branch client:my-branch --size 3840x2160 --seconds 230 -- \
  HALO_STRESS_TEST=uncapped HALO_STRESS_TEST_PARTNER=1 -- \
  HALO_NETWORK_TEST=join HALO_NULL_RENDERER=1 HALO_WINDOW_SIZE=320x180 HALO_MAX_FPS=60 HALO_SKIP_CINEMATICS=1
```
The host's `halo.log` has `the partner brought beside the host`, `stress test:` lines with `2 players` and the feeds
drawn so far, and the `benchmark:` line with the fps and the game thread's time. Without the partner flag (one
instance, `HALO_STRESS_TEST=uncapped`) it is the solo stress test.

## bosgame: Windows and the RTX 3090

**bosgame** is the Windows test machine: a Windows 11 VM (`halo-win`) with an RTX 3090 passed through, so NVIDIA's
Windows driver and ray queries (`display.shadows ray_traced`, `display.ambient_occlusion ray_traced`) get tested
there. `goat-rig @bosgame build <branch>` cross-compiles the Windows build on omarchy (`goat-rig build --windows`:
clang and lld against the MSVC toolchain in `~/goat-rig/xwin`, the shared ccache, its own worktree
`builds/<branch>.win`; about 20 s from nothing, seconds when cached). `goat-rig @bosgame run <label> --build <branch>
[--seconds S] [--shots at:12+1] [--map M] [--bots N] [--menu] [--release] -- VAR=value ...` builds it, copies it to
bosgame and puts it in the VM (only when it differs from the one there), plays one local Slayer game through
`winvm run` (muted, loopback, port 25150, broadcast 127.255.255.255, no updates or services; the window shows on the
VM's own screen only), then keeps `halo.log`, `debug.txt`, the pictures (PNG), `result.json` and `summary.txt`
(frame and GPU times, stalls, the `vulkan: GPU` and ray-tracing lines) in omarchy's `~/goat-rig/runs/<label>/bosgame`;
`goat-rig fetch bosgame:<label> <dest>` brings them to the Mac. One run at a time (`~/goat-rig/bosgame.lock` on
bosgame, around winvm's own lock). A VM that is off is started by winvm and left running afterwards: `winvm stop`
would hand the 3090 to the host, and giving it back to the VM takes a reboot. `goat-rig @bosgame status` shows the VM.
Setup: `tools/rig/install.sh omarchy --windows` (once; omarchy reaches `bosgame.fabric` directly).
```sh
tools/rig/goat-rig @bosgame run rt --build my-branch --seconds 30 --shots at:12+1 -- \
  HALO_SHADOWS=ray_traced HALO_AMBIENT_OCCLUSION=ray_traced
```

**11. The ray tracing's standard clips** (omarchy; `tools/rt2_clips.py`, run from a Mac checkout): the same staged
moments played live in each build, side by side, with the menu setting only and the MCC import linked.
```sh
python3 tools/rt2_clips.py --list                                   # the moments
python3 tools/rt2_clips.py --build my-branch --out ~/Desktop/clips  # classic | my-branch with the setting on
python3 tools/rt2_clips.py --build my-branch --base master --moments c20_flashlight --out DIR   # master | my-branch
python3 tools/rt2_clips.py --build my-branch --repeat               # played twice, the raw frames compared
```
Each moment is a campaign start with cues and a scripted controller on the scenario's clock, the fixed seed and a
fixed frame time, so no recording goes stale. Staging notes: `debug_camera_set "<x y z yaw pitch roll fov> <seconds>"`
moves the script's camera over the seconds; the flashlight goes on with
`unit_set_desired_flashlight_state (unit (list_get (players) 0)) 1` (the script's `flashlight` button toggles it
every tick it is held); a skipped cutscene (b30's Pelican ride, to about 29 s) is played fast and undrawn, so cues
in it land late and no frame is drawn there; console paths are relative to the run's data folder; `debug_scenario_places "<file>"` lists each actor's `<encounter>/<squad>` for `ai_place`. With `debug.frame_time` the game waits for each pipeline and times its score popups and particles' randomness by the frames, so a classic run plays the same frames every time.

**12. The lighting workbench** (`tools/rt2_bench.py`, ray-traced lighting): a `tools/rt2_clips.py` moment
recorded once (`HALO_RENDER_RECORD`, the menu setting, the MCC import, 1/60 s frames), then the light pass's GLSL
and `HALO_LT_*` settings tried on it by the replay, no game, with lossless pictures and numbers:
```sh
python3 tools/rt2_bench.py record --moment a50_shields --build local/my-branch --at 46   # once: ~/goat-rig/rt2-bench/<moment>-<build>
python3 tools/rt2_bench.py verify --moment a50_shields --build local/my-branch           # must PASS: the game's frames, bit for bit
python3 tools/rt2_bench.py run --moment a50_shields --build local/my-branch --watch \
  --region 600,300,80,80 --settings my.env                                              # edit lt/*.frag or my.env, save: numbers
python3 tools/rt2_bench.py run ... --hold 60          # the moment's frame held: noise (temporal std over the last 20)
python3 tools/rt2_bench.py run ... --checkpoint 120   # the new code's reach before the measured frames (default 60)
python3 tools/rt2_bench.py run ... --exact            # each try a fresh replay, every frame whole (~35 s)
```
One replay serves every try: it plays the moment once exactly and keeps the light pass's whole state a checkpoint
(`--checkpoint` frames) before the measured frames; each try reloads the changed kernels (compiled on the Mac, sent
to the rig) and settings, puts that state back and plays the rest whole, the last picture to
`build/rt2_bench/v<N>.png`. Unchanged code gives the full replay's frames bit for bit (`verify` checks it);
`verify --change VAR=value` shows how far a try strays by the frames since the checkpoint. The game's C (`lt_lights.c`) needs a new
recording; the backend's C (`xgpu_vulkan_lt*.c`) a replay rebuilt (`goat-rig build <ref> linux_replay`). The
replay's own requests are `rt_set`, `rt_reload`, `rt_frames` and `rt_view` (docs/replay_protocol.md).

Triage first: before changing anything, ask the bench which kind of problem it is.
```sh
python3 tools/rt2_bench.py record --moment a50_shields --build local/my-branch --setting-off --name a50-classic  # once
python3 tools/rt2_bench.py triage --moment a50_shields --build local/my-branch --classic a50-classic \
  --region 560,400,200,80          # per region: ESTIMATOR (realtime vs reference: bias, motion, noise), the terms,
                                   # the DENOISER's history cut, DATA/LOOK (reference vs classic), FIRST LOOK AT
python3 tools/rt2_bench.py view cut_share,history,cut,grad_change --region ...   # the denoiser's decisions (9 s)
python3 tools/rt2_bench.py reference --region ...  # the converged picture, error/bias pictures and numbers
python3 tools/rt2_bench.py terms --region ...      # sun, sky, lights, flashlight, solved lamps, object lamps, bounce
```
An ESTIMATOR answer (realtime off the reference by more than its noise) is the shortcuts': `view` says what the
denoiser cut, `run --set HALO_LT_REUSE=0` / `HALO_LT_INDIRECT=nocache` which shortcut moves it. A DATA/LOOK answer
(the reference itself off the classic look) is the lights' data: `terms` says whose light it is (a lamp too dim, a
light missing). The reference is the same kernels on the same held frame with the shortcuts off (no ReSTIR reuse,
the bounce traced, no denoiser: its input sample accumulated, 2000 plays after 200, converged when the mean's error
from 8 batches is under 1%); what stays is the data and the models (the sky, the materials, the lamps' power, one
bounce from the hits' direct light, the bounce at half resolution, the first-person and forward receivers' paths).
Everything stays on the rig; the 5900X (after the owner check) accumulates fastest.

`--host bosgame` runs the same three on the Windows VM's RTX 3090: the Windows build and `windows_replay`
cross-compiled on omarchy, the recording made by `goat-rig @bosgame run` (its own data folder in the VM, the maps and
the MCC import linked; the import copied from omarchy's once), the replay (`halo_replay.exe`) run in the VM, its
recordings in the VM's `~/goat-rig/rt2-bench`. The VM is reached through omarchy and bosgame's host
(`RT2_BOSGAME_ROUTE` changes the hops), where the replay holds bosgame's and winvm's locks shared; its scripts run
in Git Bash there. Nothing starts while a game runs in the VM.

## What it does for you

- **build** `<branch|sha|pr/N|local branch>` fetches `~/goat-rig/repo.git` (a bare clone of GitHub's
  OpenH1/goath1) and checks the commit out in `~/goat-rig/builds/<branch>`. It configures with the shared ccache
  (`--compiler-launcher ~/goat-rig/bin/rig-ccache`, 30 GB) and builds `halo` and `openh1.assets` under nice, with
  all cores but 2 and at most 2 builds per machine. A new worktree copies `build/*/third_party` from another, so
  configure downloads nothing again. Never edit inside `builds/`: each build resets the worktree to its commit.
  For a branch that must stay off GitHub, push it to the mirror:
  `git push <user@omarchy>:goat-rig/repo.git HEAD:refs/heads/local/<name>`, then `goat-rig build local/<name>`.
  `--clean` runs `ninja -t clean` first, `--release` adds configure.py's `--release`, and extra targets go after the
  ref.
- **run** starts 1 to 4 instances (`--players N`, or `--pair`), each in `~/goat-rig/runs/<label>/<instance>` with
  its own home, data, saves and config. Each is hidden and muted, with no clipboard, updates, Discord, UPnP,
  masters, public brokers or STUN. The instances share a private network (`unshare -rn`: only its loopback). Each
  has its own address, 127.0.0.200, .201 and so on (never 127.0.0.1, which the game takes as this computer only),
  and all share port 25150. The broadcast is 127.255.255.255 plus the peers. Map share and seed caps are lifted
  (20480 KB/s), and the random seed is fixed. A run that draws holds the machine's GPU lock
  (`~/goat-rig/locks/gpu.lock`; older scripts link their lock to it). A run for pictures, not timings, takes `--shared`: up to 3 such runs draw at once, holding the lock shared, while a measuring run (without it) still has the GPU alone. It never starts while the machine owner's
  own game (`~/Applications/OpenH1/bin/openh1`) runs, and stops if that game starts. Afterwards it keeps `halo.log`, `debug.txt`, the pictures (as PNG),
  `video.mp4` and `summary.txt` (exit codes, failed asserts, peak RSS) and deletes the rest unless you pass
  `--keep`. The summary (printed too) also gives the known failure signs from each game's log: `FAIL menus: not
  added` (the port's menus refused), the count of `pipeline missed map preparation` and of `a pipeline the map's
  preparation did not make`, each map's preparation (`shipped record`, `own record` or `walked`), and stalls over
  100 ms that are not loads.
- **Settings after `--`:** `VAR=value`; `-VAR` unsets; `@data/<file>=<text>` writes a file into the data folder
  (`@data/init.txt=…`); `@link/<path>=<target>` and `@copy/<path>=<source>` link or copy into it.
- **exec** `<branch> -- <command>` runs a tree's own tool in its worktree, niced, inside a private network.
- **fetch**, **clean** `[--older-than DAYS] [--builds]`, **status** (locks, disk, cache).

Install or update the kit from a checkout: `tools/rig/install.sh omarchy` (or `5900x`). It needs no root. It
pins ccache 4.14.2's static release, checked against its SHA-256.
