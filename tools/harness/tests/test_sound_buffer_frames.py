"""audio.buffer_frames' choice (sound_buffer_frames.c): automatic starts at a Windows device's smallest shared period
(SDL's WASAPI driver rounding the request to the engine's periods) and grows on a dropout; a number is kept."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

SOURCE = "port/linux/src/dsound_sdl.c"
CASES = ["smallest-period", "default-only", "number-kept"]
NEGATIVE_CONTROLS = {
    # Windows' automatic start at 512, as it was (#266): the engine's default period, 480, never its smallest
    "windows-512": (("#define BUFFER_FRAMES_AUTO 256", "#define BUFFER_FRAMES_AUTO 512"), "smallest-period"),
    # no growth on a dropout
    "no-growth": (("\t\tbuffer_frames *= 2;", "\t\tbuffer_frames *= 1;"), "smallest-period"),
}


def generated(fault=None):
    text = read(SOURCE)
    code = text[text.index("#define BUFFER_FRAMES_AUTO "):text.index("static void buffer_frames_choose")]
    code += function(text, "buffer_frames_choose") + "\n"
    code += text[text.index("#define TIMING_LOG_MS"):text.index("static void timing_check")]
    code += function(text, "timing_check") + "\n"
    if fault:
        code = mutated(code, *fault)
    return (("under_test.inc", code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("sound_buffer_frames", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build("sound_buffer_frames", generated(fault)), case)
    assert status == CHECK_FAILED, f"{control} escaped {case}: {output}"
