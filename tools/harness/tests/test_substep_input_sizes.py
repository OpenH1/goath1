"""A client's sub-steps to the host sized to the game's count of them (substep_input_size,
network_substeps_handle_inputs): one later sub-step's controls at 60, three at 120, each record read at its size."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure

SOURCE = "port/linux/game/network_substeps.c"
CASES = ["sizes", "read", "read-120", "short"]

NEGATIVE_CONTROLS = {
    # every record the room of three later sub-steps (as before)
    "fixed-size": (("return (word)(SUBSTEP_INPUT_HEADER_SIZE + (substep_count - 1) * sizeof(struct distributed_substep_input_control));",
                    "(void)substep_count; return (word)sizeof(struct distributed_substep_input);"), "sizes"),
    # a record read past the message's end
    "unbounded": (("\t\t\tif (end - data < (long)size)\n\t\t\t\treturn;\n", ""), "short"),
    # the next record read where the last began
    "not-advanced": (("\t\tdata += size;\n", ""), "read"),
}


def generated(fault=None):
    source = read(SOURCE)
    code = re.search(r"#define SUBSTEP_INPUT_HEADER_SIZE \(.*?\n.*?\n", source)[0]
    # (the packed records' reader, of their own test: test_controls_packing.py)
    code += "static word substep_input_unpack(byte const *data, byte const *end, long time, " \
            "struct distributed_substep_input *input) { (void)data; (void)end; (void)time; (void)input; return 0; }\n"
    code += function(source, "substep_input_size") + "\n"
    code += function(source, "substep_pin") + "\n"
    code += function(source, "network_substeps_handle_inputs") + "\n"
    if fault:
        code = mutated(code, *fault)
    config = structure(source, "substep_control") + "\n"
    config += structure(source, "substep_record") + "\n"
    config += structure(source, "distributed_substep_input_control") + "\n"
    config += structure(source, "distributed_substep_input") + "\n"
    return (("config.inc", config), ("under_test.inc", code))


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("substep_input_sizes", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("substep_input_sizes", generated(fault)), case)
    assert status == CHECK_FAILED, output
