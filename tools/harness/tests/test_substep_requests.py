"""The host's states at its sub-steps go to a client only for the players it asked for (network_distributed.c's
distributed_client_send_substep_requests, distributed_handle_substep_requests, distributed_substep_states_wanted,
network_distributed_host_substep_ended): a client asks where its copy went wrong (a correction past the tolerance,
a forwarded input missed), for a window renewed while it keeps going wrong and ending once it stops; none while
every input comes; debug.network_forms_before sends them as before."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
HEADER = "port/linux/game/network_distributed.h"
CASES = ["arrive", "dropped", "wrong", "renew", "lost", "always", "bounds"]

NEGATIVE_CONTROLS = {
    # every near player's states sent unasked, as before the asks
    "unasked": (("!distributed_substep_states_wanted(always, machine_index, player_index)", "FALSE"), "arrive"),
    # a window that never ends
    "never-ends": (("(until != NONE && game_time_get() - until < 0)", "(until != NONE)"), "dropped"),
    # a missed forwarded input not seen
    "gap-unseen": (("if (last_key != NONE && key != last_key + 1)", "if (FALSE)"), "dropped"),
    # asked every tick the copy goes wrong
    "ask-every-tick": (("now - asked_time < SUBSTEP_REQUEST_RENEW_TICKS", "now - asked_time < 0"), "renew"),
    # an ask's window not renewed by the next
    "not-renewed": (("if (*until == NONE || game_time_get() + ticks - *until > 0)", "if (*until == NONE)"),
                    "renew"),
    # a copy once wrong asked for ever after: the ask never expires
    "stays-wrong": (("\t\tdistributed_substep_wants[player_index].wrong = FALSE;\n"
                     "\t\tdistributed_substep_wants[player_index].asked_time = now;",
                     "\t\tdistributed_substep_wants[player_index].asked_time = now;"), "renew"),
    # the setting not read
    "always-ignored": (("return always ||", "return FALSE ||"), "always"),
    # a window as long as a client asks
    "unbounded": (("MIN((long)request->ticks, (long)SUBSTEP_REQUEST_MAXIMUM_TICKS)", "(long)request->ticks"),
                  "bounds"),
}

FUNCTIONS = ("distributed_substep_states_wanted", "distributed_handle_substep_requests",
             "distributed_client_send_substep_requests", "network_distributed_host_substep_ended")


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = enum_with(header, "_distributed_message_batch") + "\n"
    config += enum_with(header, "_distributed_to_host") + "\n"
    config += "enum { MAXIMUM_SIM_SUBSTEPS = %s };\n" % re.search(r"MAXIMUM_SIM_SUBSTEPS = (\d+)",
                                                                   read("port/linux/game/sim_rate.h"))[1]
    config += enum_with(source, "SUBSTEP_REQUEST_RENEW_TICKS") + "\n"
    config += enum_with(source, "_forward_at_read") + "\n"
    config += structure(header, "distributed_message_header") + "\n"
    config += structure(source, "distributed_substep_request") + "\n"
    config += re.search(r"^static long distributed_substep_requested_until\[[^;]*;", source, re.M)[0] + "\n"
    config += re.search(r"^static struct\s*\{[^}]*\} distributed_substep_wants\[[^;]*;", source, re.M)[0] + "\n"
    config += re.search(r"^static struct\s*\{[^}]*\} distributed_substep_request_counts;", source, re.M)[0] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("substep_requests", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("substep_requests", generated(fault)), case)
    assert status == CHECK_FAILED, output
