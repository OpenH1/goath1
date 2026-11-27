# The lighting referee

`tools/lighting_referee.py` measures a build's Enhanced lights, ray-traced shadows, HDR and their cost, and says
PASS or FAIL for each check, so a lighting change reaches a person only after it passes. It runs on the test machines
through goat-rig, from a Mac checkout:

```sh
python3 tools/lighting_referee.py <branch|sha|local/name>          # everything, about 5 minutes
python3 tools/lighting_referee.py my-branch --only parity,shadow     # a part
python3 tools/lighting_referee.py my-branch --timing omarchy,5900x,bosgame
python3 tools/lighting_referee.py master --mutate shadows-everywhere # a negative control: must FAIL
```

It prints a table and writes `build/lighting_referee/<build>/table.txt` and `summary.json` (the verdict, every row,
the build and baseline as built); exit status 0 PASS, 1 FAIL, 2 could not run. The pictures stay beside them. A build
without `debug.screenshot_at` (older than this tool) gets the capture commits picked onto it and is built as
`local/lref/<build>` on the machines' mirrors only.

**Staging.** Every scene is a campaign level played on one machine with cinematics skipped, the frame time fixed
(`debug.frame_time` 1/30), the seed fixed, the player placed and objects placed by cues at fixed ticks, the scripted
controller on the simulation's clock, and pictures taken at fixed seconds (`debug.screenshot_at`), so two builds'
pictures of a moment show the same game state. Scenes: c20's dark corridor with a Flood combat form at the far
wall's crease (flashlight off, on, and a tenth of a second later); the same with the Flood a step aside; a frag
grenade bursting at that wall; a30's beach in daylight. Variants keep the rest of the look Classic
(`display.graphics "classic"`) so only the lights differ.

**Checks.**
- *parity*: Enhanced lights with shadows off against Classic, as the light added (light on minus off, linear
  luminance) per region: the wall pool, floor, character and first-person weapon for the flashlight, three wall
  patches for the blast; the weapon with the flashlight off (the old counter lamp); the HUD's own pixels; a daylight
  frame with no dynamic light in sight.
- *shadow*: the traced picture against the same build with its lights' shadows off (`HALO_LIGHT_SHADOWS_OFF=1`,
  `HALO_FLASHLIGHT_SHADOWS_OFF=1`: every branch must keep these meaning "no shadows, nothing else"; the first row
  checks the two pictures' overall level agrees). Rows: shadowed pixels on the wall and on the floor beside the
  Flood, the angle between the two parts (it bends at the crease), the distance to the feet, the shares of the
  Flood's lit front, the floor before it and the first-person weapon shadowed, and how much of the shadow changes
  when the Flood is placed a step aside.
- *hdr*: the HDR frame's half floats (`debug.screenshot_halves`, the view not tone mapped): the AR counter's
  brightest pixel, and the frame's.
- *features* (bounce, reflections; `--feature NAME=VAR=VALUE` adds or changes one): on, no NaN or infinite value,
  no blown-out pixels, no more flicker than off; off, the traced pictures equal the baseline's (meaningful for a
  branch that adds only that feature: give `--baseline` the point it branched from). "draws nothing" (n/a) means
  the setting changed nothing in this build. `--feature-cost` times each one on.
- *cost*: `debug.pass_times` on the whole Enhanced look with ray-traced shadows and the flashlight on, 1920x1080,
  each machine alone on its GPU, three runs, the median of the last summaries.
- *identity*: the Classic pictures of every scene byte-identical to the baseline's (master), and
  `tools/scorecard/identity.py` (game state tick by tick) against the baseline's game.

**Thresholds and why** (set from what each check means, not from any branch's numbers):
- Parity within 5% of Classic's added light per region, either way: Enhanced is the same light drawn per pixel, so
  only shading noise may differ; 5% is under what anyone sees side by side. The pattern's correlation >= 0.85: the
  beam's hot disk and ring must be where Classic's are (a missing ring drops it well below). Under 0.002 of linear
  light Classic adds nothing there, and Enhanced must not either.
- HUD exact and Classic identity exact: neither is touched by any light, so any byte that differs is a bug. (The
  HUD's translucent meters may differ by one 8-bit code, the rounding of the view blended behind them.) The
  daylight frame may differ in 0.1% of its bytes (8-bit rounding of the same light), no more.
- A pixel is shadowed when 40% darker than shadows-off: a real shadow takes most of a light; noise and soft edges
  stay under that. At least 150 such pixels on each side of the crease (a person's shadow at 3 m covers far more);
  the wall and floor parts' directions at least 12 degrees apart (a projection that ignores the crease keeps one
  direction); within 40 pixels of the feet (about 10 cm there: a contact shadow, not a floating one). At most 5% of
  the caster's lit front and 1% of the floor before it shadowed (acne, light leaking through); at most 1% of the
  first-person weapon (its own lamp never shadows it). The caster a step aside must change at least 20% of the
  shadow.
- HDR: the counter glows past paper white (1.05) and stays under the lights' ceiling (2.6, below 3); nothing in the
  frame past the world lights' ceiling, 3x paper white (docs/hdr.md). A feature may push at most 0.1% of pixels past
  that, and its frame-to-frame change may be at most 1.5x the feature off's plus 0.0005 of linear light.
- Cost budgets (ms a frame at 1080p): omarchy (Radeon 8060S, a 60 fps target with room for the game: 16.7 ms)
  frame 12, LIGHT SHADOWS 1.5, AO 1.5, a feature +2; 5900X (RTX 4090, a 240 Hz target: 4.2 ms) 3, 0.4, 0.4, +0.5;
  bosgame (RTX 3090, Windows, `--timing ...,bosgame`) 5, 0.8, 0.8, +1. Other passes are reported.

**Negative controls** (`--mutate`): `lights-brighter` (parity), `shadows-everywhere` (shadow), `hdr-boost` (hdr),
`classic-changed` (identity), `heavy` (cost). Each must fail its check; run them after changing a threshold.
Regions and thresholds are constants at the top of the script; regions are pixels of the 1920x1080 picture.
