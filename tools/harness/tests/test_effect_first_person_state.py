"""An effect on a first-person weapon: the effects and their locations (the game state the ticks keep) are the same
whether the renderer drew the weapon or not; a drawn weapon's particles still go at its first-person markers; the
first-person locations, outside the game state, put right when it is written over (a load: none of the state's before
walked, a shown weapon's back)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure

CASES = ['drawn-or-not', 'first-person', 'rewritten']

EFFECTS = 'source/effects/effects.c'
FIRST_PERSON_WEAPONS = 'source/interface/first_person_weapons.c'


def generated(fault=None):
    effects = read(EFFECTS)
    CAPACITY = read('port/linux/include/halo_port_capacity.h')
    definitions = read('source/effects/effect_definitions.h')
    config = '\n'.join([
        '#define HALO_PORT_MAXIMUM_EFFECTS %d' % constant(CAPACITY, 'HALO_PORT_MAXIMUM_EFFECTS'),
        '#define HALO_PORT_MAXIMUM_EFFECT_LOCATIONS %d' % constant(CAPACITY, 'HALO_PORT_MAXIMUM_EFFECT_LOCATIONS'),
        '#define MAXIMUM_EFFECT_FIRST_PERSON_LOCATIONS %d' % constant(effects, 'MAXIMUM_EFFECT_FIRST_PERSON_LOCATIONS'),
        '#define MAXIMUM_EFFECT_LOCATIONS %d' % constant(definitions, 'MAXIMUM_EFFECT_LOCATIONS'),
        '#define MAXIMUM_EFFECT_EVENTS %d' % constant(definitions, 'MAXIMUM_EFFECT_EVENTS'),
        enum_with(effects, '_effect_loop_bit'),
        enum_with(effects, 'MAXIMUM_EFFECT_INSTANCES'),
        enum_with(effects, '_effect_location_first_person_bit'),
        enum_with(effects, '_effect_camera_mode_first_person_only'),
        enum_with(effects, '_effect_definition_must_be_deterministic_bit'),
        structure(effects, 'effect_datum_header'),
        structure(effects, 'effect_vector_field'),
        structure(effects, 'effect_datum'),
        structure(effects, 'effect_location_definition'),
        structure(effects, 'effect_location_datum'),
        structure(effects, 'effect_first_person'),
    ]) + '\n'
    code = '\n'.join(function(effects, name) for name in (
        'effect_first_person_try_and_get', 'effect_first_person_of', 'effect_local_player_index',
        'effect_first_person_free', 'effect_first_person_new', 'effect_allocate', 'effect_build_location',
        'effect_build_locations', 'effect_delete', 'effect_new_from_object', 'effect_new_looping',
        'effects_start_on_first_person_weapon', 'effects_stop_on_first_person_weapon',
        'effect_camera_mode_first_person', 'effect_location_first_instance', 'effect_location_get_next_instance',
        'effects_initialize_for_new_map', 'effects_port_game_state_rewritten'))
    code += '\n' + function(read(FIRST_PERSON_WEAPONS), 'first_person_weapons_port_effects_restart')
    if fault:
        code = mutated(code, *fault)
    return (('config.inc', config), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_case(case):
    status, output = run(build('effect_first_person_state', generated()), case)
    assert status == 0, output


FAULTS = [
    # (the first-person locations taken from the effect's own array, as before: its identifiers move with the frames)
    ('struct data_array *data = first_person ? effect_first_person_location_data : effect_location_data;',
     'struct data_array *data = effect_location_data;'),
    # (the weapon's player kept in the effect itself, as before)
    ('\t\t\teffect_first_person_new(effect_index, local_player_index);\n\t\t\teffect_build_locations(effect, '
     'first_person_weapon_get_marker_by_name);\n\t\t}\n\n\t\tif (force_local_player_index',
     '\t\t\teffect->local_player_index = local_player_index;\n\t\t\teffect_first_person_new(effect_index, '
     'local_player_index);\n\t\t\teffect_build_locations(effect, first_person_weapon_get_marker_by_name);\n\t\t}\n\n'
     '\t\tif (force_local_player_index'),
]


@pytest.mark.parametrize('fault', FAULTS, ids=['own-array', 'own-player'])
def test_negative_control(fault):
    status, output = run(build('effect_first_person_state', generated(fault)), 'drawn-or-not')
    assert status == CHECK_FAILED, output


REWRITTEN_FAULTS = [
    # (nothing put right after a load: the slots' identifiers alone, which the loaded effects have too)
    ('if (!effect_first_persons || !effect_first_person_location_data)\n\t\treturn;', 'return;'),
    # (the slots freed, but a weapon the loaded state has shown already not given its locations again)
    ('\tfirst_person_weapons_port_effects_restart();\n', ''),
]


@pytest.mark.parametrize('fault', REWRITTEN_FAULTS, ids=['identifier-only', 'no-restart'])
def test_negative_control_rewritten(fault):
    status, output = run(build('effect_first_person_state', generated(fault)), 'rewritten')
    assert status == CHECK_FAILED, output
