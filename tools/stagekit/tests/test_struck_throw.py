"""scenarios/struck_throw.c: the player fires one slow shot at a wall 20 units ahead, which bounces straight back and
strikes it as it throws a grenade, the grenade still in its hand. The blow pings it hard, which lets go of the grenade
inside the shot's update (units.c's unit_ping_animation, from the shot's damage).

At 120 the grenade then steps at once, within the shot's step (projectiles.c's projectile_substep_launch_on_tick),
so with debug.timings on the projectile update's profile section is entered again while it is open. That was a
failed assertion (profile.c's #597, which a debug build halts on), and is now counted as a call of the section.
Played here at 120 with the timings on and assertions fatal (runner.launch): it must play to the end."""

import pytest
import runner
from conftest import SLOTS_PER_WORKER, worker

# (aimed level along x at the wall; the shot comes back in 4 seconds, the throw's hold of the grenade the third of a
# second before it lets go, StageKit's prop_grenades)
SCRIPT = "script:0.2 aim=0,0; 0.5 fire; THROW throw"
THROW = 4.2
SECONDS = 6.0
PLAY = dict(seconds=6, input=SCRIPT.replace("THROW", str(THROW)))


def traced(folder, map_path, slot):
    """the scenario played at 120 with debug.timings, traced (debug.trace): its lines, and the game's exit"""
    import shutil

    (folder / "custom_maps").mkdir(parents=True)
    shutil.copy(map_path, folder / "custom_maps" / "void.map")
    runner.prepare(folder)
    game = runner.launch(folder, PLAY["input"], *runner.addresses(slot), "log", init="map_name custom_maps\\void\n",
                         HALO_FIXED_STEP="1", HALO_SIM_RATE="120", HALO_TIMINGS="1", HALO_TRACE="trace.txt",
                         HALO_TRACE_SECONDS=str(SECONDS), **runner.cue_settings([], 1))
    try:
        code = game.wait(timeout=runner.LOADING_SECONDS + SECONDS + 60)
    except Exception:
        game.kill()
        game.wait()
        raise
    trace = folder / "trace.txt"
    return (trace.read_text().splitlines() if trace.exists() else []), code, runner.Session(
        folder, 0, -1, [], {}, "", "")


@pytest.fixture(scope="module")
def game(stagekit, tmp_path_factory):
    map_path = runner.generate(stagekit, "struck_throw", tmp_path_factory.mktemp("struck-throw-map"))
    return traced(tmp_path_factory.mktemp("struck-throw-120"), map_path, SLOTS_PER_WORKER * worker() + SLOTS_PER_WORKER - 6)


def steps(lines):
    """each step: (t, its lines)"""
    found = []
    for line in lines[1:]:
        words = line.split()
        if words[0] == "s":
            found.append((float(words[1]), []))
        elif found:
            found[-1][1].append(words)
    return found


def test_plays_to_the_end(game):
    lines, code, session = game
    assert code == 0 and lines and "rate 120" in lines[0], session.tails()
    assert steps(lines)[-1][0] >= SECONDS - 1e-6, session.tails()


def test_struck_with_the_grenade_in_hand(game):
    """the shot's second strike (off the wall, then the player) a sub-step of the fast phase, before the throw let
    go of the grenade at its key frame: the throw ended there (the hard ping), the player hurt and alive"""
    lines, _, session = game
    played = steps(lines)
    thrown = next(t for t, words in played if ["e", "act", "0", "throw"] in words)
    struck = [t for t, words in played if any(w[:2] == ["e", "hit"] and float(w[4]) < 1 for w in words)]
    ended = next(t for t, words in played if t > thrown and ["e", "act", "0", "-"] in words)
    assert struck and thrown < struck[0] < thrown + 1 / 3, (thrown, struck)
    assert ended == struck[0], (ended, struck)
    body = [float(w[12]) for t, words in played if t > struck[0] for w in words if w[:2] == ["p", "0"]]
    assert body and 0 < body[-1] < 1, body[-3:]
