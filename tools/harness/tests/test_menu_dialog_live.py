"""A pop-up's words and its choices' labels with bindings filled again each frame (menu_screen.c's dialog_frame,
docs/menu_screens.md's <dialog>): a countdown counts down while the pop-up is up, and a pop-up with no bindings (Quit,
Save Changes) is not touched. The fixture screen (menu_dialog_live.xml, here, not one of the game's) is the C case's
blocks."""

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, ROOT, build, function, mutated, read, run  # noqa: E402

CASES = ["counts", "unbound", "no-value", "plain"]

NEGATIVE_CONTROLS = {
    # the words never filled again (master before this change)
    "words-not-refilled": (("screen_text_change(open->dialog_text, label);", ";"), "counts"),
    # the labels set every frame, changed or not
    "set-every-frame": (
        ("screen_text_change(open->items[index], label);", "screen_text_set(open->items[index], label);"), "counts"),
    # every label set again, bound or not
    "every-label": (("item && item->label && strchr(item->label, '{') &&", "item && item->label &&"), "unbound"),
    # a binding with no value empties the words
    "emptied": (("sizeof(label)) && label[0])", "sizeof(label)) >= 0)"), "no-value"),
}

FIXTURE = Path(__file__).with_name("menu_dialog_live.xml")


def dialog_frame():
    """the game's dialog_frame, to its closing brace at the line's start (its '{' character literals would unbalance
    harness.function's brace count)"""
    return re.search(r"^static void dialog_frame\(.*?^\}$", read("port/linux/game/menu_screen.c"), re.M | re.S).group(0)


def generated(fault=None):
    code = dialog_frame()
    if fault:
        code = mutated(code, *NEGATIVE_CONTROLS[fault][0])
    return (("under_test.inc", code + "\n"),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("menu_dialog_live", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", sorted(NEGATIVE_CONTROLS))
def test_negative_control(control):
    status, output = run(build("menu_dialog_live", generated(control)), NEGATIVE_CONTROLS[control][1])
    assert status == CHECK_FAILED, f"{control} was not caught: {output}"


def test_fixture_matches_the_case():
    """the C case's words and labels are the fixture's, and the fixture is a pop-up of the screens' format"""
    text = FIXTURE.read_text()
    case = read("tools/harness/tests/menu_dialog_live.c")
    assert re.search(r'<dialog title="[^"]+" text="Reverting in \{test.left\} seconds"/>', text)
    for label in re.findall(r'<item id="\w+" label="([^"]+)"', text):
        assert f'"{label}"' in case, label
    assert not (ROOT / "port/assets/menus/screens" / FIXTURE.name).exists()


def test_called_each_frame():
    """menu_screen_frame runs it for a pop-up's screen"""
    frame = function(read("port/linux/game/menu_screen.c"), "menu_screen_frame")
    assert re.search(r"if \(open->dialog_text\)\s+dialog_frame\(open\);", frame)
