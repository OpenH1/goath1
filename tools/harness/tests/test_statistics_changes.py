"""The host's players' statistics to its clients (distributed_send_statistics): to an OpenCE machine, and every machine
where debug.network_forms_before, each change at once and every player unchanged once a second, as before; to an
OpenH1 one each change at once and again on the next look not of its tick, and 2 players unchanged a second, round
them all. A change lost once heals within a look (half a second), lost twice within the refresh's round."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
CASES = ["quiet", "before", "kill", "lost", "lost-twice", "kill-twice-a-tick"]

NEGATIVE_CONTROLS = {
    # a change not sent again to an OpenH1 machine: one lost waits for the refresh's round
    "no-repeat": (("\t\t\trepeated[player_index] = TRUE;\n", "\t\t\trepeated[player_index] = FALSE;\n"), "lost"),
    # the repeat on the change's own tick (lost with it)
    "repeat-same-tick": (("distributed_statistics_repeat_times[player_index] != now && ", ""), "kill-twice-a-tick"),
    # an OpenH1 machine refreshed as an OpenCE one (16 players a second)
    "openh1-refresh-as-before": (("short most = rounds ? STATISTICS_OPENH1_REFRESH_PLAYERS : STATISTICS_REFRESH_PLAYERS;",
                                  "short most = STATISTICS_REFRESH_PLAYERS;"), "quiet"),
    # an OpenCE machine refreshed as an OpenH1 one
    "opence-as-openh1": (("boolean openh1 = !before && !network_legacy_machine(machine_index);",
                          "boolean openh1 = !before;"), "quiet"),
    # debug.network_forms_before not heard
    "before-ignored": (("boolean openh1 = !before && !network_legacy_machine(machine_index);",
                        "boolean openh1 = !network_legacy_machine(machine_index);"), "before"),
    # no refresh to an OpenH1 machine: a change lost twice never heals
    "openh1-no-refresh": (("boolean *marks = rounds ? openh1_refreshed : refreshed;", "boolean *marks = refreshed;"),
                          "lost-twice"),
}


def constant(source, name):
    return re.search(name + r" = ([^,\n]+),", source)[1]


def generated(fault=None):
    source = read(SOURCE)
    code = function(source, "distributed_send_statistics")
    if fault:
        code = mutated(code, *fault)
    config = "enum { STATISTICS_INTERVAL_TICKS = %s, STATISTICS_REFRESH_TICKS = %s, STATISTICS_REFRESH_PLAYERS = %s,\n" \
        "\tSTATISTICS_OPENH1_REFRESH_PLAYERS = %s, MAXIMUM_STATISTICS_PER_MESSAGE = %s };\n" % tuple(
            constant(source, name) for name in ("STATISTICS_INTERVAL_TICKS", "STATISTICS_REFRESH_TICKS",
                                                "STATISTICS_REFRESH_PLAYERS", "STATISTICS_OPENH1_REFRESH_PLAYERS",
                                                "MAXIMUM_STATISTICS_PER_MESSAGE"))
    config += structure(source, "distributed_player_statistics") + "\n"
    config += structure(source, "distributed_statistics_message") + "\n"
    for name in ("distributed_sent_statistics", "distributed_statistics_cursor", "distributed_statistics_openh1_cursor",
                 "distributed_statistics_repeat_times"):
        config += re.search(r"^static [^;\n]*\b" + name + r"\b[^;\n]*;", source, re.M)[0] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("statistics_changes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("statistics_changes", generated(fault)), case)
    assert status == CHECK_FAILED, output
