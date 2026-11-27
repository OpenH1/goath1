"""The players' pings (port/linux/game/network_markers.c): a point the host takes from another machine (inside the
world, never NaN or infinite), a ping that takes the player's last away (the same object, or a point near the last;
an object and a point never the same), the pings a player sends at once and over time, where a ping of an object
is drawn in a frame between the ticks: over it as drawn, a biped's head (read from the pose drawn) not moved again;
and whose pings a viewer is shown: a team's, none of a player this machine muted (the scoreboard's mute); and that
the host measures a ping's range from where the pinger is in the world, seated in a vehicle or not; that the aim
reaches a small item (a health pack) near its line or lying just past where it lands, and nothing past a wall; and
that a ping of an item another player's ping is on is a claim of it (a unit's or a point's never)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

SOURCE = "port/linux/game/network_markers.c"
FUNCTIONS = ("network_markers_point_valid", "network_markers_same_spot", "markers_rate_take",
             "markers_object_drawn_point", "markers_network_game", "markers_sees", "markers_muted_here",
             "markers_shown_to", "markers_unit_near", "network_markers_pick", "network_markers_decided_kind")
CASES = ["point", "same-spot", "rate-burst", "rate-refill", "rate-wrap", "object-point", "muted", "seated", "pick",
         "claim"]

# a fault in the code under test and the case that must catch it
NEGATIVE_CONTROLS = {
    # an unbounded z: a point past the world (or an infinite one) taken
    "z-unbounded": (("z > -MARKER_WORLD_LIMIT && z < MARKER_WORLD_LIMIT", "z == z"), "point"),
    # a NaN taken: the bounds as a negated test
    "nan-taken": (("return x > -MARKER_WORLD_LIMIT && x < MARKER_WORLD_LIMIT &&",
                   "return !(x <= -MARKER_WORLD_LIMIT || x >= MARKER_WORLD_LIMIT) &&"), "point"),
    # objects compared by place, as points are
    "objects-by-place": (("if (object_index != NONE || last_object_index != NONE)", "if (0)"), "same-spot"),
    # no burst limit
    "no-burst-limit": (("if (rate->tokens <= 0)\n\t\treturn FALSE;", ""), "rate-burst"),
    # refilled past the burst
    "refill-unbounded": (("while (rate->tokens < MARKER_BURST &&", "while ("), "rate-refill"),
    # (as it was) the biped's drawn head moved with the object again: a tick's rise further, up and down each tick
    "head-moved-twice": (("position->z = head_position.z + MARKER_HEAD_HEIGHT;",
                          "position->z += head_position.z - at->z + MARKER_HEAD_HEIGHT;"), "object-point"),
    # the tick's middle, not moved with the object as drawn
    "not-interpolated": (("\trender_interpolation_object_point(object_index, position);\n", ""), "object-point"),
    # the mute not asked: a muted player's pings drawn and heard
    "mute-ignored": (("&& (pinger == viewer || !markers_muted_here(pinger))", ""), "muted"),
    # the mute asked of the viewer's machine, not the pinger's
    "mute-of-viewer": (("!markers_muted_here(pinger))", "!markers_muted_here(viewer))"), "muted"),
    # the mute asked in a game of this machine alone too (a network game's left over)
    "mute-local-game": (("return pinger && markers_network_game() &&", "return pinger &&"), "muted"),
    # (as it was) measured from the unit's own position: a seated one's is its seat's in the vehicle
    "seat-position": (("object_get_origin(unit_index, &origin);", "origin = object_get(unit_index)->object.position;"),
                      "seated"),
    # (as it was) a small item reached by its own radius alone: a health pack just off the aim missed
    "pick-own-radius": (("MAX(radius, MARKER_PICK_SMALLEST_RADIUS)", "radius"), "pick"),
    # (as it was) nothing past where the aim hit: a health pack on the floor just past where it lands missed
    "pick-not-past-hit": (("along - radius > hit_distance + reach_across", "along - radius > hit_distance"), "pick"),
    # no bound past the hit: an item behind a wall taken
    "pick-through-walls": (("along - radius > hit_distance + reach_across ||", ""), "pick"),
    # a claim of anything another pinged (a unit, a point)
    "claim-any": (("kind == _marker_kind_object && item && pinged_by_another", "pinged_by_another"), "claim"),
    # a claim not taken away by the claimer's ping again
    "claim-sticks": (("\tif (same_as_last)\n\t\treturn _marker_kind_clear;\n", ""), "claim"),
    # no claims: a second marker stacked, as before
    "claim-never": (("\t\treturn _marker_kind_claim;", "\t\treturn kind;"), "claim"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = re.search(r"(enum\s*\{\s*/\* the message's format.*?\};)", source, re.S)[1] + "\n"
    config = re.sub(r"\n\s*MAXIMUM_MARKER_PLAYERS.*?\n\s*/\* the aim's line.*?_bit\),\n", "\n", config, flags=re.S)
    config += "\n".join(re.findall(r"^#define MARKER_\w+ .*$", source, re.M)) + "\n"
    config += re.search(r"(struct marker_rate\s*\{.*?\};)", source, re.S)[1] + "\n"
    config += re.search(r"/\* the message's kinds \*/\s*(enum\s*\{.*?\};)", source, re.S)[1] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("markers", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("markers", generated(fault)), case)
    assert status == CHECK_FAILED, output
