"""An OpenH1 client's input held while unchanged (distributed_input_held, distributed_input_checksum) and the host's take
of it (distributed_handle_held, distributed_handle_inputs): an idle client sends its input whole only after a change
and as the refresh, the host takes an input every tick and measures the round trip and the clock every tick, and a
change whose whole sends were all lost is taken within INPUT_HELD_REFRESH_TICKS."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["idle", "change", "lost"]

NEGATIVE_CONTROLS = {
    # sent whole every tick (as before)
    "always-whole": (("if (!sent->valid || sent->checksum != checksum)", "if (TRUE)"), "idle"),
    # never sent whole again: a change whose whole sends were lost never taken
    "no-refresh": (("now - sent->whole_time >= INPUT_HELD_REFRESH_TICKS ||", ""), "lost"),
    # the input's own parts not in its checksum: a held one repeats another
    "action-unchecked": (("\tcopy.tick = 0;\n", "\tcopy.tick = 0;\n\tcsmemset(&copy.action, 0, sizeof(copy.action));\n"),
                         "lost"),
    # the tick in the checksum: every input different, none held
    "tick-checked": (("\tcopy.tick = 0;\n", ""), "idle"),
    # a held input not had whole not measured: the round trip and the clock wait for a whole one
    "no-echo": (("\telse if (echo_count)\n\t\tdistributed_handle_inputs(machine_index, echoes, echo_count, 0);\n", ""),
                "lost"),
    # a held one taken whatever the host had whole
    "checksum-ignored": (("if (distributed_held_inputs[player_index].valid && distributed_held_inputs[player_index].checksum == held.checksum)",
                          "if (distributed_held_inputs[player_index].valid)"), "lost"),
}

FUNCTIONS = ("distributed_checksum", "distributed_input_held", "distributed_input_echo", "distributed_input_checksum",
             "distributed_handle_inputs")


def generated(fault=None):
    source = read(SOURCE)
    code = "\n".join(function(source, name) for name in FUNCTIONS) + "\n"
    held = function(source, "distributed_handle_held")
    # (its predictions' part: the host's predictions are another test's)
    held = held.replace(held[held.index("\t\tif (type == _distributed_message_player_prediction_held)"):
                             held.index("\t\tif (distributed_held_inputs[player_index].valid &&")],
                        "\t\t(void)type; (void)time;\n")
    code += held + "\n"
    if fault:
        code = mutated(code, *fault)
    config = structure(read("source/game/players.h"), "player_action") + "\n"
    config += structure(source, "distributed_player_input") + "\n"
    config += structure(source, "distributed_player_held") + "\n"
    config += re.search(r"enum\s*\{\s*INPUT_HELD_REPEATS = .*?\};", source, re.S)[0] + "\n"
    config += structure(source, "distributed_held_sent") + "\n"
    config += "static struct distributed_held_sent distributed_inputs_held_sent[MAXIMUM_LOCAL_PLAYERS];\n"
    config += re.search(r"static struct\s*\{\s*boolean valid;\s*unsigned long checksum;\s*struct distributed_player_input input;"
                        r"\s*\} distributed_held_inputs\[MAXIMUM_TRACKED_PLAYERS\];", source)[0] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("held_inputs", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("held_inputs", generated(fault)), case)
    assert status == CHECK_FAILED, output
