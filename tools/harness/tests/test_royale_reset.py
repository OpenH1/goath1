"""Nothing of a Royale match is left for the next game (port/linux/game/royale.c's royale_reset), and Royale's hooks
in the game's code act in Royale alone: every lane core starts has a reset that royale_reset calls; the game engine
calls it as every game starts and ends, of any game type; the beam's render and the campaign level's map mode
(no AI, scripts or BSP switches) ask for Royale."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import function, mutated, read  # noqa: E402

def sources():
    game = Path(__file__).resolve().parents[3] / "port/linux/game"
    names = {path.name for path in game.glob("royale*.c")} | {"bots_royale.c"}
    files = {name: read(f"port/linux/game/{name}") for name in sorted(names)}
    files["game_engine.c"] = read("source/game/game_engine.c")
    return files


def problems(files):
    """What lets a match's state leak, or a Royale hook act in another game type: [] if nothing."""
    found = []
    royale = files["royale.c"]
    started = re.findall(r"royale_(\w+)_start\(config\);", function(royale, "royale_start_lanes"))
    reset = function(royale, "royale_reset")
    for lane in started:
        if f"royale_{lane}_reset();" not in reset:
            found.append(f"royale_reset does not reset the {lane} lane")
        # (a lane with a start of its own, so a state of its own: its own reset)
        own = [text for name, text in files.items() if name != "royale_lanes_stub.c"]
        if any(re.search(rf"void royale_{lane}_start\(\s*struct royale_map_config", text) for text in own) and \
                not any(re.search(rf"void royale_{lane}_reset\(\s*void\)\s*\{{", text) for text in own):
            found.append(f"the {lane} lane has no reset of its own")
    engine = files["game_engine.c"]
    for name in ("game_engine_initialize_for_new_map", "game_engine_dispose_from_old_map"):
        if "royale_reset();" not in function(engine, name):
            found.append(f"{name} does not call royale_reset")
    if "royale_active()" not in function(files["royale_beam.c"], "royale_beam_render"):
        found.append("royale_beam_render draws outside Royale")
    if "royale_active()" not in function(files["royale_map.c"], "royale_map_mode"):
        found.append("royale_map_mode holds in every game type")
    beam_reset = function(files["royale_beam.c"], "royale_beam_reset")
    for field in ("play_start", "contrail", "hum", "damage"):
        if f"royale_beam.{field} = NONE;" not in beam_reset:
            found.append(f"royale_beam_reset leaves {field}")
    return found


def test_nothing_leaks():
    assert problems(sources()) == []


# a fault, and the file it is in: each must be seen
NEGATIVE_CONTROLS = {
    "beam-not-reset": ("royale.c", "\troyale_beam_reset();\n", ""),
    "no-reset-at-end": ("game_engine.c", "\troyale_map_restore();\n\troyale_reset();\n", "\troyale_map_restore();\n"),
    "beam-render-ungated": ("royale_beam.c", "if (!royale_active() || !royale_beam_sweeping()",
                            "if (!royale_beam_sweeping()"),
    "map-mode-any-game": ("royale_map.c", "_scenario_type_solo && royale_active();",
                          "_scenario_type_solo && game_engine_running();"),
    "contrail-kept": ("royale_beam.c", "\troyale_beam.contrail = NONE;\n", ""),
    "loot-reset-missing": ("royale_loot.c", "void royale_loot_reset(", "void royale_loot_forget("),
}


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    name, before, after = NEGATIVE_CONTROLS[control]
    files = sources()
    files[name] = mutated(files[name], before, after)
    assert problems(files), f"a fault ({control}) went unseen"
