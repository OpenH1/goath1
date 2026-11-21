"""A host that draws reads its datagrams on a thread of its own while its frame waits for the display
(network_receive.c): each datagram handled once, in order, never while the game's thread works, in the game's
floating-point environment, none between games (a map change's datagrams queued for the next), only in main.c's
present; which machines read so; and, built with ThreadSanitizer, no data race between the two threads."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, sanitizer_available

SOURCE = "port/linux/game/network_receive.c"
CASES = ["handoff", "load-present", "who"]
FUNCTIONS = ("network_receive_main", "network_receive_start", "network_receive_wanted",
             "network_receive_frame_wait_begin", "network_receive_frame_wait_end", "network_receive_frame_presenting",
             "network_receive_in_wait", "network_receive_stop")

NEGATIVE_CONTROLS = {
    # the thread reads on after the game's thread ended its wait
    "read-after-wait": (("\t\tif (!receive.open || receive.stopping)\n\t\t\tcontinue;\n", ""), "handoff"),
    # the wait ended without the lock: the game's thread goes on while the thread still reads
    "end-unlocked": (("\tpthread_mutex_lock(&receive_lock);\n\treceive.open = FALSE;\n", "\treceive.open = FALSE;\n"),
                     "handoff"),
    # the thread reads in its own floating-point environment, not the game's
    "own-rounding": (("\t\tnetwork_receive_environment_give(&receive.environment);\n", ""), "handoff"),
    # a wait opened between games: what is queued for the next read as there is none
    "between-games": (("game_in_progress() && network_standard_agreed_rate()", "network_standard_agreed_rate()"),
                      "handoff"),
    # any present's wait opened, a load's progress screen's too
    "any-present": (("if (!receive.presenting || receive.waiting", "if (receive.waiting"), "load-present"),
    # debug.network_timing_before not looked at
    "no-before-setting": ((' && !config_boolean("debug.network_timing_before")', ""), "who"),
    # a client reads so too
    "client-too": (("game_connection() == _game_connection_network_server && ", ""), "who"),
}


def generated(fault=None):
    source = read(SOURCE)
    globals_ = re.search(r"/\* ---------- constants \*/\n(.*?)/\* ---------- the thread \*/", source, re.S)[1]
    code = globals_ + "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("network_receive", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("network_receive", generated(fault)), case)
    assert status == CHECK_FAILED, output


def test_no_data_race():
    if not sanitizer_available("thread"):
        pytest.skip("no ThreadSanitizer here")
    status, output = run(build("network_receive", generated(), sanitizer="thread"), "handoff")
    assert status == 0 and "ThreadSanitizer" not in output, output


def test_data_race_found():
    """(the negative control of the one before: the wait ended without the lock races)"""
    if not sanitizer_available("thread"):
        pytest.skip("no ThreadSanitizer here")
    fault = NEGATIVE_CONTROLS["end-unlocked"][0]
    status, output = run(build("network_receive", generated(fault), sanitizer="thread"), "handoff")
    assert "ThreadSanitizer: data race" in output, output
