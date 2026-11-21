/* The host's plan of what each client is sent of each player
(distributed_host_plan_players, #287): a player aimed at from afar is in the
client's forwarded set (period 1 or 2) on every tick of the aim, and its
state goes on the ticks it went before; and which players' states go at the
host's sub-steps (distributed_substep_state_due, stage 2d). Machine 0 has
player 0 at the origin, machine 1 player 1, 100 world units along x (sent
every third tick by its distance). No map: every cluster is in sight; but
for the reacts case, whose map has two clusters, the second out of the
first's sight. */
#include "harness.h"
typedef unsigned short word;
enum { MAXIMUM_TRACKED_PLAYERS = 4, HALO_PORT_MAXIMUM_NETWORK_MACHINES = 4, MAXIMUM_LOCAL_PLAYERS = 1,
    DISTRIBUTED_INPUT_HISTORY = 4 };
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define SET_FLAG(flags, bit, value) ((value) ? ((flags) |= FLAG(bit)) : ((flags) &= ~FLAG(bit)))
#define BIT_VECTOR_TEST_FLAG(vector, bit) (((vector)[(bit) >> 5] & (1UL << ((bit) & 31))) != 0)
#define csmemset memset
struct player_action { int unused; };
struct distributed_relayed_action { int unused; };
struct structure_bsp { struct { long count; } clusters; };
struct player_datum { boolean quit_out_of_game; long unit_index; };
struct unit_datum { struct { real_vector3d aiming_vector; char current_zoom_level; } unit; };
struct distributed_unit_state { byte flags; long unit_index; long vehicle_index; short seat_index; boolean deck_present; };
#include "config.inc"
static struct distributed_unit_state distributed_host_states[MAXIMUM_TRACKED_PLAYERS];
static struct distributed_relayed_action distributed_host_actions[MAXIMUM_TRACKED_PLAYERS];
static long distributed_host_update_number;
static byte distributed_sends[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static long distributed_player_machines[MAXIMUM_TRACKED_PLAYERS];
static byte distributed_forward_periods[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static boolean distributed_forward_immediate[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static long distributed_budget_balances[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
static short distributed_budget_stretches[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
/* (network.client_budget, KB a second) */
static long client_budget;
static long config_integer(char const *name) { (void)name; return client_budget; }
static boolean distributed_seen[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static struct { byte flags; long unit_index; long vehicle_index; short seat_index; }
    distributed_sent_units[MAXIMUM_TRACKED_PLAYERS];

/* the fake world */
static long now_time;
static struct player_datum players[2] = { { FALSE, 0 }, { FALSE, 1 } };
static real_point3d origins[2] = { { 0.0f, 0.0f, 0.0f }, { 100.0f, 0.0f, 0.0f } };
static struct unit_datum units[2];
static long player_lists[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_LOCAL_PLAYERS] = { { 0 }, { 1 }, { NONE }, { NONE } };
static boolean at_tick_start[MAXIMUM_TRACKED_PLAYERS];
static long game_time_get(void) { return now_time; }
static short distributed_client_machines(long *indices, short maximum) {
    (void)maximum; indices[0] = 0; indices[1] = 1; return 2;
}
/* (the reacts case's map: cluster 0 sees itself alone, cluster 1 both) */
static boolean have_map;
static struct structure_bsp map = { { 2 } };
static unsigned long pvs_rows[2][1] = { { 1 }, { 3 } };
static short unit_clusters[2];
static struct structure_bsp *global_structure_bsp_get(void) { return have_map ? &map : NULL; }
static unsigned long *structure_bsp_get_cluster_pvs(struct structure_bsp *bsp, short cluster) {
    (void)bsp; return have_map ? pvs_rows[cluster] : NULL;
}
static long update_server_ticked_update_number(void) { return NONE; }
static struct player_action const *update_server_update_actions(long update, short *count) {
    (void)update; *count = 0; return NULL;
}
static boolean distributed_relayed_action_from(short player_index, struct player_action const **recent,
    short const *counts, struct distributed_relayed_action *action) {
    (void)player_index; (void)recent; (void)counts; (void)action; return FALSE;
}
static long *machine_get_player_list(long machine_index) { return player_lists[machine_index]; }
static struct player_datum *distributed_player(short player_index) {
    return player_index >= 0 && player_index < 2 ? &players[player_index] : NULL;
}
static long distributed_living_unit(struct player_datum const *player) { return player ? player->unit_index : NONE; }
static void distributed_state_from_player(short player_index, long time, struct distributed_unit_state *state) {
    (void)time;
    state->flags = FLAG(_distributed_unit_alive_bit) | FLAG(_distributed_unit_placed_bit);
    state->unit_index = player_index; state->vehicle_index = NONE; state->seat_index = NONE;
}
static void object_get_origin(long unit_index, real_point3d *origin) { *origin = origins[unit_index]; }
static short distributed_object_cluster(long unit_index) { return unit_clusters[unit_index]; }
static struct unit_datum *unit_get(long unit_index) { return &units[unit_index]; }
static boolean distributed_forward_covers(long machine_index, short player_index) {
    (void)machine_index; (void)player_index; return FALSE;
}
static boolean distributed_prediction_at_tick_start(short player_index) { return at_tick_start[player_index]; }
/* (no input in this world: relayed on a change only, which its own test checks) */
static struct { boolean covered; } distributed_relayed_sent[HALO_PORT_MAXIMUM_NETWORK_MACHINES][MAXIMUM_TRACKED_PLAYERS];
static boolean network_legacy_machine(long machine_index) { (void)machine_index; return FALSE; }
static boolean network_substeps_host_relays(short player_index) { (void)player_index; return FALSE; }
static boolean distributed_relayed_action_due(long machine_index, short player_index, boolean pressed,
    boolean substeps, boolean planned, long now) {
    (void)machine_index; (void)player_index; (void)pressed; (void)substeps; (void)planned; (void)now; return FALSE;
}
#include "under_test.inc"

static void aim(real_vector3d direction, char zoom) {
    units[0].unit.aiming_vector = direction;
    units[0].unit.current_zoom_level = zoom;
}

int main(int argc, char **argv) {
    char const *name = argc > 1 ? argv[1] : "";
    real_vector3d at = { 1.0f, 0.0f, 0.0f };
    real_vector3d away = { 0.0f, 1.0f, 0.0f };
    long tick;

    units[1].unit.aiming_vector = away;
    units[1].unit.current_zoom_level = (char)NONE;
    distributed_player_machines[0] = 0;
    if (!strcmp(name, "aimed") || !strcmp(name, "legacy-sends") || !strcmp(name, "unaimed") ||
        !strcmp(name, "scoped")) {
        aim(!strcmp(name, "unaimed") ? away : at, !strcmp(name, "scoped") ? 0 : (char)NONE);
        /* (the first tick: player 1 comes into sight and is new, sent at once) */
        now_time = 1000;
        distributed_host_plan_players();
        for (tick = 1001; tick < 1013; tick++) {
            byte period;
            boolean sent;

            now_time = tick;
            distributed_host_plan_players();
            period = distributed_forward_periods[0][1];
            sent = TEST_FLAG(distributed_sends[0][1], _distributed_send_state_bit);
            if (!strcmp(name, "aimed")) {
                /* (forwarded on every tick of the aim, not on some) */
                CHECK(period == 2, "tick %ld: the player aimed at planned every %d ticks", tick, period);
            } else if (!strcmp(name, "legacy-sends")) {
                /* (its state on the ticks of its distance and of the aim, as
                before the fix: a Legacy game's bytes unchanged) */
                boolean before = (tick + 1) % 3 == 0 || (tick + 1) % 2 == 0;
                CHECK(sent == before, "tick %ld: state %s, before %s", tick, sent ? "sent" : "not sent",
                    before ? "sent" : "not sent");
            } else if (!strcmp(name, "unaimed")) {
                CHECK(period == 3, "tick %ld: a player not aimed at planned every %d ticks", tick, period);
                CHECK(sent == ((tick + 1) % 3 == 0), "tick %ld: a player not aimed at sent off its ticks", tick);
            } else {
                CHECK(period == 1 && sent, "tick %ld: a player in the scope planned every %d ticks", tick, period);
            }
            /* (its own player never forwarded to it) */
            CHECK(distributed_forward_periods[0][0] == 0, "a client's own player in its forwarded set");
        }
        return 0;
    }
    if (!strcmp(name, "reacts")) {
        /* (whether machine 0 can react to player 1: in sight, or near though
        unseen; the plan's word, and the same test of where it is now) */
        static struct { real x; short cluster; boolean reacts; } const steps[] = {
            { 100.0f, 0, TRUE },  /* far, in sight */
            { 100.0f, 1, FALSE }, /* far, unseen */
            { 40.0f, 1, TRUE },   /* unseen but near */
            { 59.0f, 1, TRUE },
            { 61.0f, 1, FALSE },
        };
        short step;

        have_map = TRUE;
        aim(away, (char)NONE);
        for (step = 0; step < (short)(sizeof(steps) / sizeof(steps[0])); step++) {
            origins[1].x = steps[step].x;
            unit_clusters[1] = steps[step].cluster;
            now_time = 2000 + step;
            distributed_host_plan_players();
            CHECK(distributed_forward_immediate[0][1] == steps[step].reacts, "step %d: the plan has it %s", step,
                distributed_forward_immediate[0][1] ? "at once" : "for the tick's end");
            CHECK(distributed_reacts_at(0, &origins[1], unit_clusters[1]) == steps[step].reacts,
                "step %d: the test of where it is now differs from the plan's", step);
            CHECK(!distributed_forward_immediate[0][0], "its own player");
        }
        return 0;
    }
    if (!strcmp(name, "budget")) {
        /* (machine 0 over its byte budget: player 1 far, then unseen; then
        unseen but near. Each tick spends past the balance until tick 2010) */
        static struct { real x; short cluster; byte period; } const places[] = {
            { 100.0f, 0, 3 },  /* far, in sight: as its distance says, whatever the budget */
            { 100.0f, 1, 0 },  /* far, unseen: stretched */
            { 40.0f, 1, 6 },   /* unseen but near: as before */
        };
        short place;

        have_map = TRUE;
        aim(away, (char)NONE);
        client_budget = 16;
        for (place = 0; place < 3; place++) {
            short stretches[3] = { 0, 0, 0 };

            origins[1].x = places[place].x;
            unit_clusters[1] = places[place].cluster;
            memset(distributed_budget_balances, 0, sizeof(distributed_budget_balances));
            memset(distributed_budget_stretches, 0, sizeof(distributed_budget_stretches));
            for (tick = 2000; tick < 2100; tick++) {
                long first_sent = NONE;
                long sent_tick;

                /* (the last tick's datagrams: twice the tick's share, then none) */
                if (tick < 2010)
                    distributed_budget_balances[0] -= 2 * client_budget * 1024 / TICKS_PER_SECOND;
                now_time = tick;
                distributed_host_plan_players();
                CHECK(distributed_budget_stretch(0) >= 1 && distributed_budget_stretch(0) <= CLIENT_BUDGET_MAXIMUM_STRETCH,
                    "tick %ld: a stretch of %d", tick, distributed_budget_stretch(0));
                if (tick == 2003)
                    stretches[0] = distributed_budget_stretch(0);
                if (tick == 2099)
                    stretches[1] = distributed_budget_stretch(0);
                /* (the state's ticks over the next while: the period it goes at) */
                if (tick == 2005) {
                    for (sent_tick = 2005; sent_tick < 2005 + 2 * CLIENT_BUDGET_MAXIMUM_STRETCH * HIDDEN_PLAYER_PERIOD_TICKS; sent_tick++) {
                        now_time = sent_tick;
                        distributed_budget_balances[0] = -1;
                        distributed_host_plan_players();
                        if (TEST_FLAG(distributed_sends[0][1], _distributed_send_state_bit)) {
                            if (first_sent == NONE)
                                first_sent = sent_tick;
                            else {
                                stretches[2] = (short)(sent_tick - first_sent);
                                break;
                            }
                        }
                    }
                    tick = sent_tick;
                }
            }
            CHECK(stretches[0] == CLIENT_BUDGET_MAXIMUM_STRETCH, "place %d: over budget, a stretch of %d", place,
                stretches[0]);
            CHECK(stretches[1] == 1, "place %d: back under budget, a stretch of %d", place, stretches[1]);
            if (places[place].period)
                CHECK(stretches[2] == places[place].period, "place %d: sent every %d ticks over budget, not %d", place,
                    stretches[2], places[place].period);
            else
                CHECK(stretches[2] == HIDDEN_PLAYER_PERIOD_TICKS * CLIENT_BUDGET_MAXIMUM_STRETCH,
                    "place %d: the far and unseen sent every %d ticks over budget", place, stretches[2]);
        }
        /* (no budget: no stretch) */
        client_budget = 0;
        distributed_budget_balances[0] = -100000;
        distributed_host_plan_players();
        distributed_host_plan_players();
        CHECK(distributed_budget_stretch(0) == 1, "no budget, a stretch of %d", distributed_budget_stretch(0));
        return 0;
    }
    if (!strcmp(name, "substep-due")) {
        struct distributed_unit_state state = { FLAG(_distributed_unit_alive_bit) | FLAG(_distributed_unit_placed_bit),
            1, NONE, NONE };
        struct distributed_unit_state riding = state;
        struct distributed_unit_state dead = state;
        struct distributed_unit_state rider = state;

        riding.flags = FLAG(_distributed_unit_alive_bit);
        dead.flags = 0;
        rider.deck_present = TRUE;
        /* player 1 of an OpenH1 client (machine 1), whose sub-steps the
        host runs as they come; player 2 the host's own; player 3 of an
        OpenCE machine (2) */
        distributed_player_machines[1] = 1;
        distributed_player_machines[2] = NONE;
        distributed_player_machines[3] = 2;
        at_tick_start[1] = TRUE;
        distributed_forward_periods[0][1] = 1;
        CHECK(distributed_substep_state_due(0, 1, &state), "a near client's player not due");
        distributed_forward_periods[0][1] = 2;
        CHECK(distributed_substep_state_due(0, 1, &state), "a player aimed at (every second tick) not due");
        distributed_forward_periods[0][1] = 3;
        CHECK(!distributed_substep_state_due(0, 1, &state), "a far player due");
        distributed_forward_periods[0][1] = 0;
        CHECK(!distributed_substep_state_due(0, 1, &state), "a player the plan sends nothing of due");
        distributed_forward_periods[1][1] = 1;
        CHECK(!distributed_substep_state_due(1, 1, &state), "a client's own player due to it");
        distributed_forward_periods[0][1] = 1;
        CHECK(!distributed_substep_state_due(0, 1, &riding), "a riding player due");
        CHECK(!distributed_substep_state_due(0, 1, &dead), "a dead player due");
        CHECK(!distributed_substep_state_due(0, 1, &rider), "a player on a vehicle's deck due");
        distributed_forward_periods[0][2] = 1;
        CHECK(distributed_substep_state_due(0, 2, &state), "the host's own player (or a bot) not due");
        distributed_forward_periods[0][3] = 1;
        CHECK(!distributed_substep_state_due(0, 3, &state), "an OpenCE player due (its prediction taken at the tick's end)");
        return 0;
    }
    fprintf(stderr, "no case %s\n", name);
    return 2;
}
