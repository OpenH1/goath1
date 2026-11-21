"""The real Royale rules (port/linux/game/royale_rules.c): squads of two drawn from the seed, a lethal hit downing
a player with a squadmate standing, a squadmate's held revive (and only theirs, and one let go starting over), a
bleed-out credited to the downer, the gulag asked of every death, squads placed as they go out, the endgame, the
last squad's win and the results' end."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["squads", "revive", "bleed-out", "gulag", "results", "downed-rule", "lane-states", "clock", "network",
         "take"]

# a fault in each rule, and the case that must catch it
NEGATIVE_CONTROLS = {
    "revive-not-reset": (("if (entry->reviver != (byte)by)\n\t\t\t\tentry->revive_ticks = 0;", ";"), "revive"),
    "anyone-revives": (("by != mate || ", ""), "revive"),
    "placed-one-low": (("rules->placement[squad] = (byte)(alive + 1);", "rules->placement[squad] = alive;"),
                       "results"),
    "last-of-squad-downed": (("return mate != NONE && royale_rules_standing(rules->slots[mate].state) &&",
                              "return mate != NONE &&"), "bleed-out"),
    # (the client's old guess: any squadmate) a down when the squadmate is in the gulag
    "mate-in-gulag-revives": (("rules->slots[mate].state != ROYALE_RULES_GULAG;\n}", "TRUE;\n}"), "downed-rule"),
    # a landing that stands the downed (the lanes' moves unchecked)
    "landing-stands-downed": ((" || rules->slots[slot].state == ROYALE_RULES_DOWNED))", "))"), "lane-states"),
    # a client whose clock stands still
    "client-clock-frozen": (("\trules->match_time++;\n\trules->phase_time = rules->match_time - rules->phase_start;",
                             "\trules->phase_time = rules->match_time - rules->phase_start;"), "clock"),
    # a client bleeding out while revived
    "client-bleeds-in-revive": (("\t\telse if (entry->downed_ticks > 0)\n\t\t\tentry->downed_ticks--;",
                                 "\t\tif (entry->downed_ticks > 0)\n\t\t\tentry->downed_ticks--;"), "clock"),
    # ENDGAME's start taken as PLAY's
    "play-start-moves": (("rules->play_start == NONE)\n\t\trules->play_start = rules->match_time;",
                          "TRUE)\n\t\trules->play_start = rules->match_time;"), "clock"),
    # the clock in the discrete part: it changes every tick, resent all the time
    "clock-in-discrete": (("cursor = royale_rules_put_long(cursor, rules->phase_start);\n\tcursor = royale_rules_put_long(cursor, rules->play_start);\n\t*cursor++ = (byte)count;",
                           "cursor = royale_rules_put_long(cursor, rules->phase_time);\n\tcursor = royale_rules_put_long(cursor, rules->play_start);\n\t*cursor++ = (byte)count;"),
                          "network"),
    # a squad past the count taken
    "squad-unchecked": (("(entry->squad != SLOT_NONE && entry->squad >= rules->squad_count) ||", ""), "network"),
    # the Reclaimer's result not sent: every client shows none
    "reclaimer-not-sent": (("\t\t*cursor++ = entry->reclaimer;\n", "\t\t*cursor++ = 0;\n"), "network"),
    # a win not recorded
    "reclaimer-win-lost": (("ROYALE_RULES_RECLAIMER_LOST : ROYALE_RULES_RECLAIMER_WON;",
                            "ROYALE_RULES_RECLAIMER_LOST : ROYALE_RULES_RECLAIMER_NONE;"), "gulag"),
    # every state snaps a client's clock (it steps)
    "take-always-snaps": (("return snap || local < host - slack || local > host + slack ? host : local;",
                           "return host;"), "take"),
    "gulag-not-told": (("if (gulag && gulag(slot, killer_slot) && !in_gulag)",
                        "if (!in_gulag && gulag && gulag(slot, killer_slot))"), "gulag"),
}


def generated(fault=None):
    header = read("port/linux/game/royale_rules.h")
    body = re.sub(r'^#include "(royale_rules|cseries)\.h"$', "", read("port/linux/game/royale_rules.c"), flags=re.M)
    if fault:
        body = mutated(body, *fault)
    return (("royale_rules.inc", header + "\n" + body),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_rules", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_rules", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_rules with a fault ({control}) passed '{case}': the test cannot see it"
