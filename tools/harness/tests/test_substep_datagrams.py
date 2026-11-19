"""The host's datagrams of a sub-step (network_distributed_host_substep_ended, distributed_forward_flush and the
batches): one datagram to a machine as a sub-step ends, the forwarded controls with the sub-step's states whatever
the interval since the frame's own forward, a tick's last sub-step's with the tick's messages (and, where no tick
follows, with the next frame's), split only where the next entry does not fit, in order."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_distributed.c"
HEADER = "port/linux/game/network_distributed.h"
SUBSTEPS_HEADER = "port/linux/game/network_substeps.h"
CASES = ["substep", "substep-after-read", "step-due", "display-wait", "as-they-come", "tick-end", "split", "held", "react", "defer", "into-view", "compact", "opence"]

NEGATIVE_CONTROLS = {
    # what came read just before the host's step in a datagram of its own, beside the step's
    "step-due-alone": (("return step_milliseconds >= 0.0f && step_milliseconds < (real)FORWARDED_INPUT_INTERVAL_MILLISECONDS;",
                        "return FALSE;"), "step-due"),
    # in a drawing host's wait for the display, held for a step that runs only once the wait ends
    "display-wait-as-step": (("\tif (display_wait)\n", "\tif (FALSE)\n"), "display-wait"),
    # read as they come, the frame's interval kept: a press held up to 8 ms for the step or the last datagram
    "as-they-come-frame-interval": (("interval = halo_server_mode() || (distributed_reading_as_they_come &&",
                                     "interval = halo_server_mode() || (FALSE &&"), "as-they-come"),
    # ... or held for it though the wait's end is not known
    "display-wait-unknown-held": (("\t\tif (wait_left_milliseconds < 0.0f)\n\t\t\treturn FALSE;\n", ""),
                                  "display-wait"),
    # a tick's last sub-step's controls in a datagram of their own, before the tick's
    "tick-end-alone": (("if (occasion == _forward_at_tick_end)\n\t\t\t\tdistributed_forward_held = TRUE;\n"
                        "\t\t\telse\n\t\t\t\tdistributed_batch_flush((short)machine_index);",
                        "distributed_batch_flush((short)machine_index);"), "tick-end"),
    # the frame's interval kept at a sub-step's end: its controls held for a datagram of their own
    "interval-at-substep": (("if (occasion == _forward_at_read && now - last_sent", "if (now - last_sent"),
                            "substep-after-read"),
    # what a tick's last sub-step held, left when no tick follows
    "held-forgotten": (("\tif (distributed_forward_held)\n\t\tdistributed_batches_flush();\n", ""), "held"),
    # a batch sent whenever it has anything: a datagram split with room left
    "split-early": (("if (used + overhead + minimum > DATAGRAM_MAXIMUM_SIZE)", "if (batch->size)"),
                    "split"),
    # every far player's press held for the tick's datagram, those in sight too
    "defer-in-sight": (("distributed_forward_reacts(machine_index, player_index)))", "FALSE))"), "react"),
    # a far, unseen player's press at once, as before
    "never-defer": (("if (*owed && (occasion == _forward_at_tick_end ||", "if (*owed && (TRUE ||"), "defer"),
    # one owed waits for the tick's end though it came into sight
    # the next tick's message read as the batch's
    "later-ignored": (("buffer.header.game_time = game_time + (value & 1);", "buffer.header.game_time = game_time;"),
                      "compact"),
    # a message of another world packed with the batch's
    "world-mixed": (("header->checkpoint_epoch != batch->checkpoint_epoch ||", ""), "compact"),
    # the tick read against this machine's clock, not the sender's as it has it
    "own-clock": (("long clock = distributed_sender_clock(machine_index);", "long clock = game_time_get();"),
                  "compact"),
    # a compact batch to an OpenCE machine
    "compact-to-opence": (("return sender == HOST_SENDER ? network_standard_agreed_rate() != 0 :\n\t\tnetwork_standard_machine_is_openh1(sender);",
                           "return TRUE;"), "opence"),
    # a message's length taken past the datagram's end
    "unbounded": (("length > size - offset - 2 ||", ""), "opence"),
    "no-coming-into-view": (("if (!distributed_reacts_at(machine_index, &origin, distributed_object_cluster(unit_index)))",
                             "if (TRUE)"), "into-view"),
}

FUNCTIONS = ("distributed_far_owed_any", "distributed_forward_reacts", "distributed_batch_flush",
             "distributed_batch_compact", "distributed_compact_batch_handle", "distributed_batch_room", "distributed_batch_append", "distributed_batch_add",
             "distributed_batches_flush", "distributed_fill_header", "distributed_entry_size",
             "distributed_send_to_machine", "distributed_packer_begin", "distributed_packer_flush",
             "distributed_packer_add", "distributed_forward_waits_for_step", "distributed_forward_flush", "network_distributed_frame_received",
             "network_distributed_host_substep_ended")


def generated(fault=None):
    source = read(SOURCE)
    header = read(HEADER)
    substeps_header = read(SUBSTEPS_HEADER)
    code = "\n".join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    config = enum_with(header, "_distributed_message_batch") + "\n"
    config += "enum { DATAGRAM_MAXIMUM_SIZE = %d, MAXIMUM_SIM_SUBSTEPS = %d, LIVE_CONTROL_FORWARD_MAXIMUM_SIZE = %d };\n" % (
        constant(read("source/networking/network_connection.h"), "DATAGRAM_MAXIMUM_SIZE"),
        constant(read("port/linux/game/sim_rate.h"), "MAXIMUM_SIM_SUBSTEPS"),
        constant(substeps_header, "LIVE_CONTROL_FORWARD_MAXIMUM_SIZE"))
    config += "enum { FORWARDED_INPUT_INTERVAL_MILLISECONDS = %d, FORWARDED_INPUT_SERVER_INTERVAL_MILLISECONDS = %d };\n" % (
        constant(source, "FORWARDED_INPUT_INTERVAL_MILLISECONDS"),
        constant(source, "FORWARDED_INPUT_SERVER_INTERVAL_MILLISECONDS"))
    config += enum_with(source, "_forward_at_read") + "\n"
    config += structure(substeps_header, "live_control_far") + "\n"
    structures = structure(header, "distributed_message_header") + "\n"
    structures += structure(source, "distributed_batch") + "\n"
    structures += re.search(r"#define BATCH_MESSAGE_OVERHEAD \\\n[^\n]*\n", source)[0]
    structures += re.search(r"#define COMPACT_BATCH_MESSAGE_OVERHEAD [^\n]*\n", source)[0]
    structures += re.search(r"#define COMPACT_BATCH_HEADER_SIZE [^\n]*\n", source)[0]
    structures += re.search(r"#define DATAGRAM_IP_UDP_BYTES [^\n]*\n", source)[0]
    structures += structure(source, "distributed_packer") + "\n"
    return (("config.inc", config), ("structures.inc", structures), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("substep_datagrams", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("substep_datagrams", generated(fault)), case)
    assert status == CHECK_FAILED, output
