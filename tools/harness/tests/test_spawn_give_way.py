"""A player put in the slot the host chose (network_game_manager.c's network_game_spawn_player): the datum there gives
way if it quit, is another machine's or controller's, or is at the same machine and controller with its quit heard (a
late joiner told a player gone and the one at his machine, controller and slot together); a live one never does."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run  # noqa: E402

CASES = ['quit-heard', 'other-machine', 'live']

# a fault, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the code before: a quit heard but not come yet kept the slot from a player at the same machine and controller
    'quit-heard-kept': (('\t\t\t\t\tquitter->quit_out_of_game_time != NONE ||\n', ''), 'quit-heard'),
    'live-gives-way': (('\t\t\t\t\tquitter->quit_out_of_game_time != NONE ||\n', '\t\t\t\t\tTRUE ||\n'), 'live'),
}


def generated(fault=None):
    source = read('source/networking/network_game_manager.c')
    code = function(source, 'network_game_player_slot_reusable') + '\n'
    code += function(source, 'network_game_spawn_player').replace('player_data->valid', 'TRUE') + '\n'
    if fault:
        code = mutated(code, *fault)
    return (('under_test.inc', code),)


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('spawn_give_way', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build('spawn_give_way', generated(fault)), case)
    assert status == CHECK_FAILED, f'the fault ({control}) passed {case!r}: the test cannot see it'
