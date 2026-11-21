"""The co-op cutscene skip vote and the host's reverts: a refused or unanswered skip leaves the vote usable, skips
are not failures, and the failure reverts' limit is the map's own."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure

CASES = ['refused', 'unanswered', 'skips-not-failures', 'failures-then-skip', 'failure-loop', 'failure-new-map']

# a fault, the file and function it is in, and the case that must catch it
NEGATIVE_CONTROLS = {
    'refusal-not-answered': (('\tif (main_coop_host())\n\t\tnetwork_coop_skip_answered(skipped);\n', ''),
                             'main', 'main_skip_cinematic_private', 'refused'),
    'never-answered-waits': (('\t\tskip_vote_answer(FALSE);\n\t}', '\t\treturn;\n\t}'),
                             'coop', 'host_count_skip_votes', 'unanswered'),
    'skips-counted': (('skipped = main_coop_host_revert();', 'skipped = main_coop_host_failure_revert();'),
                      'main', 'main_skip_cinematic_private', 'skips-not-failures'),
    'kept-across-maps': (('main_coop_failure_reverts[0] = main_coop_failure_reverts[1] = NONE;', ';'),
                         'main', 'main_coop_failure_reverts_reset', 'failure-new-map'),
}


def expression(source, name):
    """An enum constant as its source writes it, with its expression"""
    match = re.search(r'^\s*' + re.escape(name) + r'\s*=\s*([^,\n]+),', source, re.M)
    assert match, f'{name} not found'
    return f'#define {name} ({match.group(1)})\n'


def generated(fault=None):
    coop = read('port/linux/game/network_coop.c')
    main = read('source/main/main.c')
    types = structure(coop, 'distributed_coop_skip_vote') + '\n'
    types += enum_with(coop, '_skip_vote_idle') + '\n'
    for name in ('SKIP_VOTE_HELD_TICKS', 'SKIP_COOLDOWN_TICKS', 'SKIP_ANSWER_TICKS'):
        types += expression(coop, name)
    types += expression(main, 'MAIN_COOP_FAILURE_REVERT_WINDOW_TICKS')
    vote = re.search(r'^static struct\n\{\n\tboolean voted;.*?^\} skip_vote;\n', coop, re.M | re.S)
    assert vote, 'skip_vote not found in network_coop.c'
    types += vote.group(0)
    reverts = re.search(r'^static long main_coop_failure_reverts\[2\].*\n', main, re.M)
    assert reverts, 'main_coop_failure_reverts not found in main.c'
    types += reverts.group(0)
    code = ''
    for file, source, names in (
            ('coop', coop, ('skip_vote_clear', 'skip_vote_answer', 'skip_vote_reset', 'host_count_skip_votes',
                            'network_coop_skip_answered', 'network_coop_handle_skip_vote')),
            ('main', main, ('main_coop_host_revert', 'main_coop_failure_reverts_reset',
                            'main_coop_host_failure_revert', 'main_skip_cinematic_private'))):
        for name in names:
            text = function(source, name)
            if fault and fault[1] == file and fault[2] == name:
                text = mutated(text, *fault[0])
            code += text + '\n'
    return (('types.inc', types), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_skip_vote(case):
    status, output = run(build('coop_skip_vote', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault = NEGATIVE_CONTROLS[control]
    status, output = run(build('coop_skip_vote', generated(fault)), fault[3])
    assert status == CHECK_FAILED, f'a fault ({control}) passed {fault[3]!r}: the test cannot see it'
