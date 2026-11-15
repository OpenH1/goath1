"""Royale's loot plan (port/linux/game/royale_loot.c): the rarity tiers' weights, the kinds a map has, the supply pods,
and determinism: the same seed gives every machine the same plan, another seed another."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ["determinism", "tiers", "available", "pods", "stable", "pod-clock", "kit", "sweep"]

# a fault each, and the case that must catch it
NEGATIVE_CONTROLS = {
    # an unseeded draw (each plan its own): two machines' plans differ
    "unseeded": (("unsigned long loot_seed = seed ^ 0x6C6F6F74;",
                  "static unsigned long calls; unsigned long loot_seed = seed ^ ++calls;"), "determinism"),
    # the spot's tier ignored: every spot weighted as a common one
    "tier-ignored": (("royale_loot_tier_weights[PIN(tier, 0, NUMBER_OF_ROYALE_LOOT_TIERS - 1)]",
                      "royale_loot_tier_weights[0]"), "tiers"),
    # the map's kinds ignored when picking
    "availability-ignored": (("if (royale_loot_kinds[kind].rarity != rarity || !TEST_FLAG(available, kind))",
                              "if (royale_loot_kinds[kind].rarity != rarity)"), "available"),
    # a pod's spot not taken out of the draw
    "pod-spots-repeat": (("taken[pick] = taken[index];", ";"), "pods"),
    # nothing drawn for an empty spot's kind: later spots shift
    "empty-takes-less": (("byte kind = royale_loot_roll_kind(seed, rarity == NONE ? ROYALE_LOOT_RARITY_COMMON : rarity, available);",
                          "byte kind = rarity == NONE ? ROYALE_LOOT_KIND_NONE : royale_loot_roll_kind(seed, rarity, available);"),
                         "stable"),
    # ENDGAME's start ignored: a pod waits on in the endgame
    "endgame-ignored": (("if (endgame_start != NONE && endgame_start < start)", "if (0)"), "pod-clock"),
    # the old kit check: a gulag fighter's unit never taken as theirs, so the winner is disarmed
    "gulag-unit-not-kept": (("return kitted ? ROYALE_LOOT_KIT_LEAVE : ROYALE_LOOT_KIT_KEEP;",
                             "return ROYALE_LOOT_KIT_LEAVE;"), "kit"),
    # a unit not kitted given the kit whatever the phase (a winner's new unit)
    "kit-in-play": (("return phase == ROYALE_PHASE_LOBBY || phase == ROYALE_PHASE_INSERTION ? ROYALE_LOOT_KIT_GIVE : ROYALE_LOOT_KIT_KEEP;",
                     "return ROYALE_LOOT_KIT_GIVE;"), "kit"),
    # the old sweep: only loot's spared
    "drops-swept": ((" || royale_loot_listed(held, held_count, object_index);", ";"), "sweep"),
}


def generated(fault=None):
    royale = read("port/linux/game/royale.h")
    header = read("port/linux/game/royale_loot.h")
    loot = read("port/linux/game/royale_loot.c")
    random = read("source/math/random_math.c")
    defines = "".join(f"#define {name} {constant(source, name)}\n" for source, name in (
        (royale, "ROYALE_MAXIMUM_LOOT_SPOTS"), (royale, "ROYALE_MAXIMUM_POD_SPOTS"),
        (header, "ROYALE_LOOT_POD_COUNT"), (header, "ROYALE_LOOT_POD_ITEMS"),
        (loot, "ROYALE_LOOT_POD_FIRST_SECOND"), (loot, "ROYALE_LOOT_POD_INTERVAL_SECONDS"),
        (loot, "ROYALE_LOOT_POD_JITTER_SECONDS")))
    tables = re.search(r"static struct royale_loot_kind_definition const royale_loot_kinds.*?\n\};\n.*?"
                       r"static byte const royale_loot_tier_weights.*?\n\};", loot, re.S).group(0)
    code = "\n".join([
        "enum { RANDOM_A = 1664525L, RANDOM_C = 1013904223L };",
        function(random, "seed_random"), function(random, "seed_random_range"),
        defines,
        enum_with(royale, "ROYALE_LOOT_COMMON = 0"),
        enum_with(header, "ROYALE_LOOT_RARITY_COMMON = 0"), enum_with(header, "ROYALE_LOOT_ASSAULT_RIFLE = 0"),
        structure(header, "royale_loot_pod_plan"), structure(header, "royale_loot_plan"),
        structure(loot, "royale_loot_kind_definition"),
        # (the tag groups, four-character constants, are nothing to the plan)
        tables.replace("'weap'", "0").replace("'eqip'", "1"),
        function(loot, "royale_loot_roll_rarity"), function(loot, "royale_loot_roll_kind"),
        function(loot, "royale_loot_roll"), function(loot, "royale_loot_plan_make"),
        enum_with(royale, "ROYALE_PHASE_NONE = -1"), enum_with(royale, "ROYALE_PLAYER_NONE = -1"),
        enum_with(header, "ROYALE_LOOT_KIT_LEAVE = 0"),
        function(loot, "royale_loot_pod_fall_start"), function(loot, "royale_loot_kit_action"),
        function(loot, "royale_loot_listed"), function(loot, "royale_loot_purge_spares")])
    if fault:
        code = mutated(code, *fault)
    return (("generated.inc", code),)


def test_random_constants():
    """the generator's constants are the game's"""
    random = read("source/math/random_math.c")
    assert "RANDOM_A= 1664525L" in random and "RANDOM_C= 1013904223L" in random


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_loot", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_loot", generated(fault)), case)
    assert status == CHECK_FAILED, f"royale_loot with a fault ({control}) passed '{case}': the test cannot see it"
