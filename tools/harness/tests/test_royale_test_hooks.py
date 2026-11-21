"""Royale's test settings (port/linux/game/royale.c's royale_test_setting, network_test.c's network_test_hooks):
HALO_ROYALE_* work in a network test's run alone, never in a player's game, where one machine's would part its
simulation from the host's."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ["test_run", "player_game"]

# a fault in the gate, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the gate gone: the environment's settings taken in any game
    "no-gate": (("if (!network_test_hooks())", "if (FALSE)"), "player_game"),
    # the test's run refused too
    "never": (("return config_string(\"debug.network_test\")[0] != 0;", "return FALSE;"), "test_run"),
}


def generated(fault=None):
    royale = read("port/linux/game/royale.c")
    test = read("port/linux/game/network_test.c")
    code = "\n\n".join([function(test, "network_test_hooks"), function(royale, "royale_test_setting"),
                        function(royale, "royale_debug_seconds")])
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("royale_test_hooks", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("royale_test_hooks", generated(fault)), case)
    assert status == CHECK_FAILED, \
        f"royale_test_hooks with a fault ({control}) passed '{case}': the test cannot see it"
