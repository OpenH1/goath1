# The campaign bot

A benchmark driver for the campaign: with `debug.campaign_bot` on, code plays the first player of a campaign level
on this computer alone. Its input depends only on the game's state, so a level plays the same every run, at any
frame rate. Its input goes into the demo like any player's, so a run recorded with `demo.record` and
`debug.demo_perf_log` lines up with its performance log tick by tick ([demos.md](demos.md)). It is a test tool.
Off (the default), nothing of it runs. It has no menu row.

The code is the "campaign bot" section of `port/linux/game/bots.c`. It uses the bots' own parts:

- It fights with the scripted `hunt` input's target choice and aim (`bots_hunt`): the nearest enemy in sight within
  40 units and 50 degrees, firing once on target. It has no randomness. A vehicle counts as an enemy only with an
  enemy in it.
- It walks the route's places with the bots' path search (`bots_set_goal`, `bots_follow_path`). A place is reached
  within 1.5 units across, from 4.5 below to 3 above. It gives up a place after 60 seconds of walking and goes on
  to the next. After 10 seconds of fighting without getting nearer, it walks on while still firing.
- In a seat that hasn't moved for 5 seconds (a landed Pelican, a10's cryo tube), it presses the action button
  every 2 seconds. When it's stuck (under 1 unit moved in 2 seconds), it jumps and steps aside for a second,
  alternating sides.
- With no route, it fights and otherwise walks straight ahead.

## Settings

| Setting | Environment | |
| --- | --- | --- |
| `debug.campaign_bot` | `HALO_CAMPAIGN_BOT` | on |
| `debug.campaign_bot_route` | `HALO_CAMPAIGN_BOT_ROUTE` | the route file, relative to the data folder (the folder with `maps/`) |
| `debug.campaign_bot_seconds` | `HALO_CAMPAIGN_BOT_SECONDS` | quit after this many seconds of play (game time); 0 never |

## Route files

Routes are kept in `tools/campaign_bot/`, one per level (`b30.txt`: The Silent Cartographer's landing). Copy one
into the data folder to use it. A route is plain text, one place per line, walked in order. Lines starting with `#`
are comments.

```
place <name> <x> <y> <z>
hold  <name> <x> <y> <z> <yaw> <pitch> <seconds>
```

Points are in world units. A `hold` place is walked to like any other. There the bot stops firing and turns to the
yaw and pitch given (degrees; yaw 0 faces +x, 90 faces +y), holds that pose for the seconds given, then walks on.
A route that ends with a `hold` gives a fixed view for repeatable captures. After the last place, the bot fights
where it stands.

## The log

`halo.log` gets a `campaign bot:` line for each of these, each with its game tick:

- each place reached, held or given up, and the route's end
- its position every 5 seconds, with its target, body and shield
- each checkpoint saved or reverted to
- the quit at `debug.campaign_bot_seconds`

## A run

On a test machine (`tools/rig/README.md`), The Silent Cartographer's landing, rendered, with the determinism log:

```sh
goat-rig run b30 --build my-branch --map 'levels\b30\b30' --seconds 200 -- \
  HALO_CAMPAIGN_BOT=1 HALO_CAMPAIGN_BOT_SECONDS=120 HALO_CAMPAIGN_BOT_ROUTE=campaign_bot/b30.txt \
  @copy/campaign_bot/b30.txt=<tree>/tools/campaign_bot/b30.txt \
  HALO_SKIP_CINEMATICS=1 HALO_SIM_RATE=30 HALO_DETERMINISM_LOG=determinism.txt
```

On the beach the bot reaches the route's six places by about tick 2950. The first checkpoint is saved at tick
1112, after the Pelican lands. Two runs at different frame rates (uncapped, and `display.max_fps` 60) log the same
places, checkpoints and random seeds at the same ticks. Their game states don't yet compare alike with
`tools/determinism_compare.py --game --simulation`: the campaign's HUD, effects, cinematic and sound state still
follow the frames, with or without the bot.
