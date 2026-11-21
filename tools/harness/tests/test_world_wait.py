"""display.ambient_occlusion "world": a world pipeline missed by the map's preparation is made once after the load and
its miss logged once. Its draws into display.temporal_aa's motion wait for, and get, the motion's variant (they once
waited for its own, made already, so the miss was logged every frame and the stand-in drawn for good)."""

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run, structure  # noqa: E402

STREAM = "port/linux/vulkan/xgpu_vulkan_stream.c"

NEGATIVE_CONTROLS = {
    # the motion's draws' wait making the pipeline's own variant (the old code)
    "own-for-motion": ("motion", "\tif (!motion_variant)\n\t\treturn &stream.pipelines[id];",
                       "\tif (1)\n\t\treturn &stream.pipelines[id];"),
    # the motion's variant made without its motion
    "not-motion": ("motion", "\t\tmotion.making = motion_variant;\n", ""),
    # a variant that cannot be made waited for again each frame
    "failed-again": ("failed", "\tif (!entry || entry->state == XGPU_STREAM_PIPELINE_FAILED)\n\t\treturn;\n",
                     "\tif (!entry)\n\t\treturn;\n"),
}


def generated(fault=None):
    stream = read(STREAM)
    types = "\n".join((structure(stream, "variant"), structure(stream, "stream_pipeline")))
    code = "\n\n".join(function(stream, name) for name in ("world_entry", "world_wait", "world_make_some"))
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][1:])
    return (("under_test_types.inc", types + "\n"), ("under_test.inc", code + "\n"))


@pytest.mark.parametrize("case", ["motion", "own", "failed"])
def test_case(case):
    status, output = run(build("world_wait", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    status, output = run(build("world_wait", generated(control)), NEGATIVE_CONTROLS[control][0])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"
