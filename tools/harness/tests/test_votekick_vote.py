"""A vote of network_votekick.c as an object with an identity and an end: a choice names its vote's serial, so one for
a vote that has ended is refused and never starts or counts toward another (a yes in flight across a map change
among them); a vote ended by a new game cools its starter down and keeps the gap after it; a client's choices name the
vote it heard; a machine of a build before serials (an OpenCE one) names none, and its no still counts."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

SOURCE = "port/linux/game/network_votekick.c"
START = "/* ---------- constants */"
END = "/* ---------- a client */"
CLIENT_FUNCTIONS = ("network_votekick_handle_status", "network_votekick_get_status", "votekick_send_request",
                    "network_votekick_request", "network_vote_choose")

CASES = ["ballot-for-ended-vote", "game-end-cooldown", "yes-across-map-change", "start-is-a-yes",
         "before-serials-no", "client-names-the-vote"]

# a fault (each the code before, in part), and the case that must catch it
NEGATIVE_CONTROLS = {
    "serial-ignored": (("if (serial && (!votekick.active || serial != votekick.serial))", "if (0)"),
                       "ballot-for-ended-vote"),
    "serial-ignored-across-map-change": (("if (serial && (!votekick.active || serial != votekick.serial))",
                                          "if (0)"), "yes-across-map-change"),
    "game-end-no-rules": (("\t\tdistributed_send_notice(\"The vote ended with the game\");\n\t\tvotekick_end_with_game();",
                           "\t\tdistributed_send_notice(\"The vote ended with the game\");"), "game-end-cooldown"),
    "game-end-no-cooldown": (("\tif (votekick.kind == _network_vote_kick)\n\t{\n\t\tvotekick_remember(votekick_cooling_starters",
                              "\tif (0)\n\t{\n\t\tvotekick_remember(votekick_cooling_starters"), "game-end-cooldown"),
    "start-may-be-no": (("if (!serial && choice != _votekick_choice_yes &&", "if (0 &&"), "start-is-a-yes"),
    "before-serials-no-refused": ((" || network_standard_machine_is_openh1(machine_index)))", " || 1))"),
                                  "before-serials-no"),
    "client-names-none": (("\tmessage.request.vote = (byte)serial;\n", ""), "client-names-the-vote"),
}


def generated(fault=None):
    source = read(SOURCE)
    if fault:
        source = mutated(source, *fault)
    config = read("port/linux/game/network_votekick.h")
    code = source[source.index(START):source.index(END)] + "\n"
    for name in CLIENT_FUNCTIONS:
        code += function(source, name) + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("votekick_vote", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("votekick_vote", generated(fault)), case)
    assert status == CHECK_FAILED, f"network_votekick.c with a fault ({control}) passed '{case}': the test cannot see it"
