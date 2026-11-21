"""Co-op one-shot events: over the reliable stream, so a loss burst loses none; each applied once and in order; a
moment that comes too late is not applied, a state always is."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

CASES = ['loss-burst', 'late', 'bound', 'chunks', 'old-host']

# a fault in network_coop.c, the function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    'datagrams': (('_distributed_to_clients_reliably);', '_distributed_to_clients);'), 'host_send_events', 'loss-burst'),
    'late-applied': (('if (late_ticks != NONE && age > late_ticks)', 'if (late_ticks != NONE && FALSE)'),
                     'network_coop_handle_events', 'late'),
    'states-dropped-late': (('\tdefault:\n\t\treturn NONE;', '\tdefault:\n\t\treturn EVENT_LATE_EFFECT_TICKS;'),
                            'event_late_ticks', 'late'),
    'bound-off-by-one': (('age > late_ticks', 'age >= late_ticks'), 'network_coop_handle_events', 'bound'),
}


def expression(source, name):
    """An enum constant as its source writes it, with its expression"""
    match = re.search(r'^\s*' + re.escape(name) + r'\s*=\s*([^,\n]+),', source, re.M)
    assert match, f'{name} not found'
    return f'#define {name} ({match.group(1)})\n'


def generated(fault=None):
    coop = read('port/linux/game/network_coop.c')
    header = read('port/linux/game/network_coop.h')
    types = ''
    for name in ('MAXIMUM_QUEUED_EVENTS', 'EVENT_LATE_SOUND_TICKS', 'EVENT_LATE_TEXT_TICKS', 'EVENT_LATE_EFFECT_TICKS'):
        types += expression(coop, name)
    types += structure(coop, 'distributed_coop_event') + '\n'
    types += structure(coop, 'distributed_coop_events_message') + '\n'
    types += enum_with(coop, '_coop_event_sound') + '\n'
    types += enum_with(read('port/linux/game/network_distributed.h'), '_coop_sound_impulse') + '\n'
    types += enum_with(header, '_coop_hud_help_text') + '\n'
    types += enum_with(header, '_coop_player_effect_translation') + '\n'
    events = re.search(r'^static struct\n\{\n\tstruct distributed_coop_event events\[MAXIMUM_QUEUED_EVENTS\];.*?'
                       r'^\} coop_events;\n', coop, re.M | re.S)
    assert events, 'coop_events not found in network_coop.c'
    types += events.group(0)
    code = ''
    for name in ('event_new', 'host_send_events', 'event_late_ticks', 'network_coop_handle_events'):
        text = function(coop, name)
        if fault and fault[1] == name:
            text = mutated(text, *fault[0])
        code += text + '\n'
    return (('types.inc', types), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_events(case):
    status, output = run(build('coop_events', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault = NEGATIVE_CONTROLS[control]
    status, output = run(build('coop_events', generated(fault)), fault[2])
    assert status == CHECK_FAILED, f'network_coop.c with a fault ({control}) passed {fault[2]!r}: the test cannot see it'
