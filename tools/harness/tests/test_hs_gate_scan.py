"""Co-op gates: whether a script's wait for every player could leave one behind (hs_runtime.c's gate scan).
a30's waits for the players to fall back only send the Marines back; bringing everyone into the volume for one
teleported a player back across the map."""
import re
import sys
from pathlib import Path
import pytest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import build, read, run, function, structure, mutated, CHECK_FAILED


def declared_enum(source, member):
    """the enum that declares a member (its line in the enum, not a mention before it)"""
    at = re.search(r'^\t' + re.escape(member) + r'\b', source, re.M).start()
    start = source.rindex('enum', 0, at)
    return source[start:source.index('};', at) + 2]


def generated(fault=None):
    runtime = read('source/hs/hs_runtime.c')
    definitions = read('source/hs/hs_scenario_definitions.h')
    hs = read('source/hs/hs.h')
    types = '\n'.join([
        declared_enum(runtime, '_hs_thread_type_script'), declared_enum(runtime, '_hs_syntax_node_primitive_bit'),
        declared_enum(hs, '_hs_function_begin'), declared_enum(definitions, '_hs_script_continuous'),
        structure(definitions, 'hs_script'), structure(definitions, 'hs_syntax_node'),
        structure(runtime, 'hs_runtime_globals'), structure(runtime, 'hs_stack_frame'),
        structure(runtime, 'hs_thread_datum')])
    scan = '\n'.join([declared_enum(runtime, '_hs_gate_scan_go_on')] +
                     re.findall(r'^#define HS_GATE_SCAN_\w+ \w+$', runtime, re.M) +
                     ['static short hs_gate_scan_expression(long expression_index, short depth, short *budget);'] +
                     [function(runtime, name) for name in ('hs_gate_scan_function_leaves_players_be',
                      'hs_gate_scan_statements', 'hs_gate_scan_expression', 'hs_runtime_waiting_call_is_a_gate')])
    if fault == 'sleep':
        scan = mutated(scan, 'if (node->function_index == _hs_function_sleep_until)',
                       'if (node->function_index == _hs_function_sleep_until || node->function_index == _hs_function_sleep)')
    if fault == 'arguments':
        scan = mutated(scan, 'return hs_gate_scan_statements(hs_syntax_get(node->data)->next_node_index, depth + 1, budget);',
                       'return _hs_gate_scan_go_on;')
    if fault == 'ending':
        scan = mutated(scan, '\t\treturn result == _hs_gate_scan_gate;\n\t}\n\n\treturn FALSE;',
                       '\t\treturn result == _hs_gate_scan_gate;\n\t}\n\n\treturn TRUE;')
    return (('types.inc', types), ('scan.inc', scan))


CASES = ['ai-only', 'a30-last-wave', 'door', 'sleep-then-door', 'arguments', 'static-script', 'continuous',
         'nested-block', 'wake', 'unknown', 'not-waiting', 'budget', 'recursion']


@pytest.mark.parametrize('case', CASES)
def test_scan(case):
    status, output = run(build('hs_gate_scan', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('fault,case', [('sleep', 'sleep-then-door'), ('arguments', 'arguments'),
                                        ('ending', 'a30-last-wave')])
def test_negative(fault, case):
    status, output = run(build('hs_gate_scan', generated(fault)), case)
    assert status == CHECK_FAILED, output
