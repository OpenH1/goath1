"""Royale's insertion (port/linux/game/royale_insertion.c): the Pelicans' paths from seeded random directions across
the play area, their jump windows, air control in the free fall, the landing rule, and the redeploy's safe area."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, mutated, read, run

CASES = ['crosses', 'seeded', 'point', 'window', 'air', 'landed', 'safe']
FUNCTIONS = ['path_length', 'line_box2d', 'royale_insertion_path_draw', 'royale_insertion_path_speed',
             'royale_insertion_path_point', 'royale_insertion_path_window', 'royale_insertion_air_control',
             'royale_insertion_landed', 'royale_insertion_safe_area', 'royale_insertion_redeploy_point']
CONTROLS = [
    # (no margin: the Pelicans start over the island)
    ('line_box2d(cx, cy, dx, dy, bounds, ROYALE_INSERTION_PATH_MARGIN, &t0, &t1)',
     'line_box2d(cx, cy, dx, dy, bounds, 0.0f, &t0, &t1)', 'crosses'),
    # (no acceleration limit: steering snaps)
    ('if (change > step)', 'if (0)', 'air'),
    # (steering with no throttle brakes the jump's carry)
    ('\tif (throttle <= 0.0f)\n\t\treturn;\n', '', 'air'),
    # (no settling: a brush of a ledge on the way down ends the fall and its immunity)
    ('return ticks_falling > ROYALE_INSERTION_LANDING_GRACE_TICKS && ground_ticks >= ROYALE_INSERTION_SETTLE_TICKS;',
     'return ticks_falling > ROYALE_INSERTION_LANDING_GRACE_TICKS && ground_ticks > 0;', 'landed'),
    # (the beam's side read backwards: redeploys into the glass)
    ('case ROYALE_SIDE_NORTH: safe->maximum.y = bounds->maximum.y - size_y * fraction;',
     'case ROYALE_SIDE_NORTH: safe->minimum.y = bounds->minimum.y + size_y * fraction;', 'safe'),
    # (the jump window past the island's edge)
    ('*leave_ticks = (long)floorf(t1 / per_tick);', '*leave_ticks = (long)floorf(t1 / per_tick) + 10;', 'window'),
]


def generated(fault=None):
    royale = read('port/linux/game/royale.h')
    header = read('port/linux/game/royale_insertion.h')
    config = '\n'.join([
        re.search(r'enum royale_side\n\{.*?\};', royale, re.S).group(0),
        re.search(r'struct royale_bounds\n\{.*?\};', royale, re.S).group(0),
        re.search(r'struct royale_flight_path\n\{.*?\};', royale, re.S).group(0),
        re.search(r'enum\n\{\n\t/\* the Pelicans\' passengers.*?\};', header, re.S).group(0),
        ''.join(re.findall(r'^#define ROYALE_INSERTION_\w+ .*\n', header, re.M)),
    ]) + '\n'
    source = read('port/linux/game/royale_insertion.c')
    code = '\n'.join(function(source, name) for name in FUNCTIONS)
    if fault:
        code = mutated(code, *fault)
    return (('config.inc', config), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('royale_insertion', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('before,after,case', CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build('royale_insertion', generated((before, after))), case)
    assert status == CHECK_FAILED, output
