"""The host's object states to its clients (network_objects.c's distributed_host_send_states): a moving object goes by
its distance, every HIDDEN_PLAYER_PERIOD_TICKS where a client's players cannot see its cluster (the player plan's test,
distributed_machine_sees_cluster), and at once as it comes into their sight or starts moving; an object at rest goes to
every client as it comes to rest or changes at rest, and unchanged only as its slow refresh (RESTING_REFRESH_TICKS)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_objects.c"
CASES = ["resting-unchanged", "resting-changed", "resting-jitter", "came-to-rest", "unseen", "comes-into-view", "starts-moving", "budget"]

NEGATIVE_CONTROLS = {
    # the resting round-robin as before: unchanged objects at rest sent as often as the scan comes round
    "resting-round-robin": (("(objects_host_rest_sent_times[absolute_index] != NONE &&\n"
                             "\t\t\t\tnow - objects_host_rest_sent_times[absolute_index] < RESTING_REFRESH_TICKS)",
                             "FALSE"), "resting-unchanged"),
    # a change at rest not seen: it waits for the refresh
    "change-at-rest-ignored": (("return was_moving || (checksum != objects_host_rest_checksums[absolute_index] &&",
                                "return was_moving || (FALSE &&"), "resting-changed"),
    # a change at rest restarting its sends every tick (an object that keeps changing sent every tick)
    "change-every-tick": (("game_time_get() - objects_host_rest_sent_times[absolute_index] >= REST_STATE_REPEAT_TICKS",
                           "TRUE"), "resting-jitter"),
    # no sight test: by distance alone, as before
    # the budget's stretch on near objects out of sight too
    "budget-near": (("(period > 2 ? distributed_budget_stretch(machine_index) : 1)",
                     "distributed_budget_stretch(machine_index)"), "budget"),
    # no stretch: the far and unseen as often over budget
    "budget-ignored": (("(period > 2 ? distributed_budget_stretch(machine_index) : 1)", "1"), "budget"),
    "no-sight-test": (("if (!seen)\n", "if (FALSE)\n"), "unseen"),
    # one come into sight waits for its period
    "sight-waits": (("return started || came_into_sight ||", "return started ||"), "comes-into-view"),
    # one that starts moving waits for its period
    "start-waits": (("return started || came_into_sight ||", "return came_into_sight ||"), "starts-moving"),
}

FUNCTIONS = ("distributed_checksum", "distributed_host_rest_state_due", "distributed_host_rest_changed",
             "distributed_host_moving_state_due", "distributed_host_object_period", "distributed_host_send_states")


def generated(fault=None):
    source = read(SOURCE)
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = structure(source, "distributed_object_state") + "\n"
    config += enum_with(source, "_distributed_object_predicted_bit") + "\n"
    config += re.search(r"static (struct\n\{\n\tshort count;.*?\n\} objects_host_viewers;)", source, re.S)[1] + "\n"
    config += "enum { %s };\n" % ", ".join(f"{name} = {constant(source, name)}" for name in
                                           ("RESTING_STATES_PER_TICK", "REST_STATE_REPEATS", "REST_STATE_REPEAT_TICKS",
                                            "MAXIMUM_OBJECT_PERIOD_TICKS", "OWN_VEHICLE_PERIOD_TICKS"))
    config += "enum { RESTING_REFRESH_TICKS = %s, VEHICLE_PREDICTION_REFERENCE_TICKS = 60 };\n" % re.search(
        r"RESTING_REFRESH_TICKS = ([^,]+),", source)[1]
    config += "enum { HIDDEN_PLAYER_PERIOD_TICKS = %d };\n" % constant(
        read("port/linux/game/network_distributed.h"), "HIDDEN_PLAYER_PERIOD_TICKS")
    for name in ("NEAR_OBJECT_DISTANCE", "MIDDLE_OBJECT_DISTANCE", "FAR_OBJECT_DISTANCE"):
        config += f"#define {name} " + re.search(rf"^#define {name}\s+(\S+)", source, re.M)[1] + "\n"
    config += "enum { _host_state_moving, _host_state_to_all };\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("object_state_sends", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("object_state_sends", generated(fault)), case)
    assert status == CHECK_FAILED, output
