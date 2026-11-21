"""The host's one state for each client machine (network_server_manager.c): every event in every state moves it as the
table says or is refused; every state that waits on the machine times it out (the lobby and the postgame too, so a dead
machine no longer holds the lobby's start); a load of another game, or a second one, is ignored, not a reason to drop
the machine; a joiner of another network version is told why (network_server_message_handler.c)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, constant, enum_with, function, mutated, read, run, structure  # noqa: E402

CASES = ['table', 'timeouts', 'loading-wait', 'mcc-loading', 'downloading', 'local', 'lobby-silent', 'lobby-heard',
         'dead-machine-countdown', 'postgame-silent', 'round-reset-grace', 'loaded-wrong-game', 'loaded-unnamed',
         'loaded-twice-in-game', 'loaded-late', 'version-old', 'version-new', 'version-same', 'phase',
         'drop-ends-party', 'late-replaced', 'late-changed']

SERVER = 'source/networking/network_server_manager.c'
HANDLER = 'source/networking/network_server_message_handler.c'
START = "/* ---------- each client machine's state"
END = "/* ---------- (the client machines' states end"
SERVER_FUNCTIONS = ('network_game_server_client_machine_is_joined_to_game',
                    'network_game_server_client_machine_is_loaded', 'network_game_server_machine_phase',
                    'network_game_server_client_machine_timed_out', 'network_game_server_client_machine_is_precached',
                    'network_game_server_have_all_machines_have_precached',
                    'network_game_server_same_player', 'network_game_server_player_among',
                    'network_game_server_player_told',
                    'network_game_server_client_machine_game_loading_complete',
                    'network_game_server_late_joiner_loaded', 'network_game_server_client_machine_loaded')
HANDLER_FUNCTIONS = ('network_game_server_handle_message_client_loaded', 'network_game_server_refuse_other_version')

# a fault, the file it is in, and the case that must catch it (each case that is new here fails on the code before)
NEGATIVE_CONTROLS = {
    # the lobby's machines never timed out (the code before)
    'lobby-no-timeout': ((SERVER, '\t\treturn silence > NETWORK_GAME_SERVER_LOBBY_TIMEOUT;', '\t\treturn FALSE;'),
                         'lobby-silent'),
    'lobby-no-timeout-countdown': ((SERVER, '\t\treturn silence > NETWORK_GAME_SERVER_LOBBY_TIMEOUT;',
                                    '\t\treturn FALSE;'), 'dead-machine-countdown'),
    'postgame-no-timeout': ((SERVER, 'TEST_FLAG(machine->flags, _network_client_machine_answers_postgame_bit) &&',
                             'FALSE &&'), 'postgame-silent'),
    'postgame-answer-not-taken': ((SERVER, 'network_game_server_machine_event(client_machine, '
                                           '_machine_event_answered_postgame);', ';'), 'postgame-silent'),
    'postgame-times-out-silent-builds': ((SERVER, 'TEST_FLAG(machine->flags, '
                                                  '_network_client_machine_answers_postgame_bit) &&', 'TRUE &&'),
                                         'timeouts'),
    'no-reset-grace': ((SERVER, '\t\tif (next != _machine_state_connected)\n\t\t\tmachine->last_heard_time = now;',
                        ';'), 'round-reset-grace'),
    'held-no-timeout': ((SERVER, '\t\treturn silence > NETWORK_GAME_SERVER_LATE_JOINER_TIMEOUT;\n\tcase '
                                 '_machine_state_loading:', '\t\treturn FALSE;\n\tcase _machine_state_loading:'),
                        'timeouts'),
    'loading-no-timeout': ((SERVER, '\t\t\treturn silence > NETWORK_GAME_SERVER_LOADING_TIMEOUT;',
                            '\t\t\treturn FALSE;'), 'timeouts'),
    'loading-wait-ignored': ((SERVER, 'if (server->time_of_first_client_loading_completion ||', 'if ('),
                             'loading-wait'),
    'game-number-ignored': ((SERVER, 'game_number != NONE && game_number != server->game.number_of_games_played',
                             '0'), 'loaded-wrong-game'),
    'game-number-misread': ((HANDLER, 'loaded.unused ? (long)((unsigned long)loaded.unused - 1) : NONE',
                             'NONE'), 'loaded-wrong-game'),
    'version-silent': ((HANDLER, '\tif (reply && !network_game_server_write(', '\tif (0 && !network_game_server_write('),
                       'version-old'),
    'version-reason-flipped': ((HANDLER, 'network_version < HALO_PORT_NETWORK_VERSION ?',
                                'network_version > HALO_PORT_NETWORK_VERSION ?'), 'version-new'),
    # a cell of the table changed: a connection's messages before it joins counted, a second load taken
    'connected-heard': ((SERVER, '\t/* heard (a connection that has not joined is timed from when it\n\tconnected: '
                                 'what it sends before its join does not count) */\n\t{ REFUSED, REFUSED,',
                         '\t/* heard */\n\t{ REFUSED, _machine_state_connected,'), 'table'),
    'loaded-twice': ((SERVER, '{ REFUSED, REFUSED, REFUSED, REFUSED, REFUSED, _machine_state_in_game, REFUSED },',
                      '{ REFUSED, REFUSED, REFUSED, REFUSED, REFUSED, _machine_state_in_game, _machine_state_in_game },'),
                     'table'),
    'phase-loading-as-loaded': ((SERVER, '\tcase _machine_state_loading:\n\t\treturn _network_machine_phase_loading;',
                                 '\tcase _machine_state_loading:\n\t\treturn _network_machine_phase_loaded;'), 'phase'),
    # the code before: a late joiner's players compared by machine, controller and slot alone
    'replaced-unseen': ((SERVER, '!csmemcmp(&told[index], player, sizeof(*player))',
                         'network_game_server_same_player(&told[index], player)'), 'late-replaced'),
    'changed-unseen': ((SERVER, '!csmemcmp(&told[index], player, sizeof(*player))',
                        'network_game_server_same_player(&told[index], player)'), 'late-changed'),
    'own-player-removed': ((SERVER, '\tif (player->machine_index == machine_index)\n'
                                    '\t\treturn network_game_server_player_among(player, told, count);\n', ''),
                           'late-changed'),
    # the code before: nothing told of a drop (the party module guessed it from the machines loaded)
    'drop-party-kept': ((SERVER, '\t\t\tnetwork_party_machine_dropped(machine->machine_index);', '\t\t\t;'),
                        'drop-ends-party'),
}


def generated(fault=None):
    server = read(SERVER)
    handler = read(HANDLER)
    if fault:
        path, before, after = fault
        if path == SERVER:
            server = mutated(server, before, after)
        else:
            handler = mutated(handler, before, after)
    limits = read('port/linux/include/halo_port_limits.h')
    config = ''
    for name in ('HALO_PORT_MAXIMUM_NETWORK_MACHINES', 'HALO_PORT_MAXIMUM_NETWORK_PLAYERS',
                 'HALO_PORT_MAXIMUM_NETWORK_MESSAGE_SIZE', 'HALO_PORT_NETWORK_VERSION'):
        config += f'#define {name} {constant(limits, name)}\n'
    config += f'#define MILLISECONDS_PER_SECOND {constant(read("source/cseries/cseries.h"), "MILLISECONDS_PER_SECOND")}\n'
    config += f'#define MAXIMUM_LOCAL_PLAYERS {constant(read("source/game/players.h"), "MAXIMUM_LOCAL_PLAYERS")}\n'
    for member in ('_network_client_machine_precached_bit', '_network_game_server_state_pregame',
                   'NETWORK_GAME_SERVER_PLAYERLESS_MACHINE_TIMEOUT'):
        config += enum_with(server, member) + '\n'
    config += enum_with(read('source/networking/network_server_manager.h'), '_network_machine_phase_none') + '\n'
    config += enum_with(read('source/networking/network_messages.h'), '_message_server_machine_rejected') + '\n'
    config += enum_with(read('source/networking/network_game_protocol.h'), '_rejection_code_version_too_old') + '\n'
    structures = structure(server, 'network_game_server_client_machine') + '\n'
    structures += structure(server, 'message_server_remove_player_ingame') + '\n'
    for name in ('message_client_loaded', 'message_server_machine_rejected'):
        structures += structure(handler, name) + '\n'
    code = server[server.index(START):server.index(END)] + '\n'
    for name in SERVER_FUNCTIONS:
        code += function(server, name) + '\n'
    for name in HANDLER_FUNCTIONS:
        code += function(handler, name) + '\n'
    return (('config.inc', config), ('structures.inc', structures), ('under_test.inc', code))


@pytest.mark.parametrize('case', CASES)
def test_machine_lifecycle(case):
    status, output = run(build('server_machine_lifecycle', generated()), case)
    assert status == 0, output


@pytest.mark.parametrize('control', NEGATIVE_CONTROLS)
def test_negative_control(control):
    fault, case = NEGATIVE_CONTROLS[control]
    status, output = run(build('server_machine_lifecycle', generated(fault)), case)
    assert status == CHECK_FAILED, f'the fault ({control}) passed {case!r}: the test cannot see it'
