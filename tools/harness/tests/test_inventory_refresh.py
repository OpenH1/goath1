"""The host's units' inventories to its clients (distributed_host_send_inventories): to an OpenCE machine, and every
machine where debug.network_forms_before, a change of weapons at once and every unit unchanged once a second, as
before; to an OpenH1 one a change of weapons at once and again the round after, its own unit's refresh once a second,
another's every INVENTORY_OPENH1_REFRESHES seconds. A change lost once heals the round after, lost twice by the slow
refresh."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_objects.c"
CASES = ["quiet", "before", "pickup", "lost", "lost-twice"]

NEGATIVE_CONTROLS = {
    # another's unit refreshed to an OpenH1 machine every second (as before)
    "openh1-refresh-as-before": (("kind = openh1 ? _host_inventory_own : _host_inventory_to_all;",
                                  "kind = _host_inventory_to_all;"), "quiet"),
    # an OpenH1 machine's own unit refreshed as seldom as another's
    "own-refresh-slow": (("if (kind != _host_inventory_ammunition && kind != _host_inventory_own)",
                          "if (kind != _host_inventory_ammunition)"), "quiet"),
    # debug.network_forms_before not heard
    "before-ignored": (("boolean openh1 = !before && !network_legacy_machine(machine_index);",
                        "boolean openh1 = !network_legacy_machine(machine_index);"), "before"),
    # a change of weapons not sent again: one lost waits for the slow refresh
    "no-repeat": (("\t\t\tobjects_host_inventories[absolute_index].repeat = TRUE;\n", ""), "lost"),
    # the repeat sent to the OpenCE machine too (more than it was sent)
    "repeat-to-opence": (("if (openh1 && openh1_sends[index])", "if (openh1_sends[index])"), "pickup"),
    # no slow refresh: a change lost twice never heals
    "no-slow-refresh": (("openh1_send = objects_host_inventories[absolute_index].refreshes % INVENTORY_OPENH1_REFRESHES == 0;",
                         "openh1_send = FALSE;"), "lost-twice"),
}

CONSTANTS = ("INVENTORY_INTERVAL_TICKS", "INVENTORY_REFRESH_TICKS", "INVENTORY_OPENH1_REFRESHES", "EMPTY_INVENTORY_TICKS",
             "MAXIMUM_ENTRIES_PER_MESSAGE", "MAXIMUM_OBJECT_PERIOD_TICKS")


def generated(fault=None):
    source = read(SOURCE)
    code = "\n".join(function(source, name) for name in ("distributed_checksum", "distributed_inventory_from_unit",
                                                          "distributed_host_send_inventories"))
    if fault:
        code = mutated(code, *fault)
    config = "enum {\n" + ",\n".join("\t%s = %s" % (name, re.search(r"\b" + name + r" = ([^,\n]+),", source)[1])
                                     for name in CONSTANTS) + "\n};\n"
    config += re.search(r"^#define AGE_SCALE .*$", source, re.M)[0] + "\n"
    config += structure(source, "distributed_inventory") + "\n"
    config += structure(source, "distributed_inventory_message") + "\n"
    config += enum_with(source, "_host_inventory_to_all") + "\n"
    for name in ("objects_host_inventories", "objects_host_viewers"):
        # (the global's definition: its anonymous structure, from the last "static struct" before its name)
        end = re.search(r"^\} " + name + r"(\[[^\]]*\])?;", source, re.M)
        config += source[source.rindex("static struct\n{", 0, end.start()):end.end()] + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("inventory_refresh", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("inventory_refresh", generated(fault)), case)
    assert status == CHECK_FAILED, output
