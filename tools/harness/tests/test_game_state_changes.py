"""The host's game type's state to an OpenH1 client as its changes since the state last sent it
(distributed_game_state_changes_write, distributed_game_state_changes_read, distributed_game_state_due): nothing while
unchanged, a few bytes for a field's change, the whole state to a joiner, and a client that missed a change healed by
the whole state within GAME_STATE_REFRESH_TICKS."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["unchanged", "one-field", "joiner", "lost", "most-changed", "exact"]

NEGATIVE_CONTROLS = {
    # the whole state on every change (as before)
    "always-whole": (("return machine->shared && changes ? _game_state_send_changes : _game_state_send_whole;",
                      "return _game_state_send_whole;"), "one-field"),
    # changes to a client that has not the state last sent
    "joiner-changes": (("return machine->shared && changes ?", "return changes ?"), "joiner"),
    # no refresh: a client that missed a change never heals
    "no-refresh": (("if (machine->changed_since_whole && now - machine->whole_time >= GAME_STATE_REFRESH_TICKS)",
                    "if (FALSE)"), "lost"),
    # the refresh on every look: an unchanged state sent
    "refresh-always": (("if (machine->changed_since_whole && now - machine->whole_time >= GAME_STATE_REFRESH_TICKS)",
                        "if (TRUE)"), "unchanged"),
    # changes taken onto a state they were not written against
    "no-checksum": (("(distributed_checksum(made, size) & 0xFFFFFFFFUL) != checksum", "(checksum & 0)"), "lost"),
    # a run cut a byte short
    "run-short": (("buffer[written + sizeof(offset_word)] = (byte)(end - offset);",
                   "buffer[written + sizeof(offset_word)] = (byte)(end - offset - (end - offset > 1));"), "exact"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "distributed_checksum") + "\n"
    code += function(source, "distributed_game_state_changes_write") + "\n"
    code += function(source, "distributed_game_state_changes_read") + "\n"
    code += re.search(r"enum\s*\{\s*_game_state_send_none = 0,.*?\};", source, re.S)[0] + "\n"
    code += function(source, "distributed_game_state_due") + "\n"
    if fault:
        code = mutated(code, *fault)
    config = "enum {\n"
    for name in ("GAME_STATE_INTERVAL_TICKS", "MAXIMUM_GAME_STATE_SIZE", "GAME_STATE_CHANGES",
                 "GAME_STATE_REFRESH_TICKS", "GAME_STATE_RUN_HEADER_SIZE", "GAME_STATE_MAXIMUM_RUN",
                 "GAME_STATE_CHANGES_HEADER_SIZE"):
        config += "\t%s = %s,\n" % (name, re.search(r"\b%s = ([^,]+)," % name, source)[1])
    config += "};\n" + structure(source, "distributed_game_state_machine") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("game_state_changes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("game_state_changes", generated(fault)), case)
    assert status == CHECK_FAILED, output
