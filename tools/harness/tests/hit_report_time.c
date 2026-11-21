/*
HIT_REPORT_TIME.C (test)

The host's check of a client's hit report against the time the report is
about (network_damage.c's distributed_report_valid, its age check and look
back, with distributed_report_earliest and distributed_target_seen): a
report says the host's tick it was made at, which the host believes only as
far back as it measures that client's round trip and a slack, and checks
the target against where the history had it from then on. The history holds
every tick a report it takes can be about, so a high-ping shooter's hit on
a running target is taken, while a report that claims an older time than
its machine's delay allows is refused, whatever its target. Ticks are the
game's (30 a second at every rate): a 60 or 120 Hz shooter's sub-step pose
is taken alike. Also the per-slot reset (distributed_note_player_slot): a
joiner in a quitter's slot does not inherit the quitter's recent weapons.
The round trip that bounds a report is the worst the host measured for
its machine over the last ROUND_TRIP_WINDOW_TICKS (noted each tick,
distributed_note_round_trips), so a low-ping client's reports delayed by a
lag spike are taken after its smoothed round trip has fallen back, the
bound tightens again once the window has passed, a claim no measurement
supports is refused, and a reused slot's window starts empty.
test_hit_report_time.py takes the code (under_test.inc) and the constants
(config.inc).
*/

#include "harness.h"
#include <math.h>

#ifndef NUMBEROF
#define NUMBEROF(array) (sizeof(array) / sizeof((array)[0]))
#endif

typedef unsigned short word;
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#include "config.inc"

enum
{
	MAXIMUM_TRACKED_PLAYERS = 4,
	MAXIMUM_RECENT_WEAPONS = 8,
	MAXIMUM_GRENADE_THROWS = 4,
	MAXIMUM_STUCK_PROJECTILES = 4,
	PLAYER_TRAIL_POINTS = 8,
	RECENT_WEAPON_TICKS = 300,
	HIT_REPORT_BURST_SECONDS = 3,
	TARGET = 0x10001,
	TARGET_PLAYER = 1,
	SHOOTER = 0,
	ROCKET = 0x20002,
};

#define csmemset memset

static long now;
static real round_trip;

static long game_time_get(void)
{
	return now;
}

static real distributed_machine_round_trip_ticks(long machine_index)
{
	(void)machine_index;
	return round_trip;
}

/* each player's machine (the shooter's 0, another client's 1; NONE: none) */
static long player_machines[MAXIMUM_TRACKED_PLAYERS] = { 0, 1, 0, 0 };

static long distributed_player_machine(short player_index)
{
	return player_machines[player_index];
}

/* the host's history (network_damage.c's damage_history) */
static struct damage_history_tick
{
	long time;
	struct
	{
		long object_index;
		real_point3d position;
		real speed;
		boolean driving;
	} objects[MAXIMUM_TRACKED_PLAYERS][2];
} damage_history[TARGET_HISTORY_TICKS];

/* the slots' per-player state the reset touches (network_damage.c's) */
struct damage_stuck_projectile
{
	long time;
};
static struct
{
	long definition_indices[MAXIMUM_RECENT_WEAPONS];
	long times[MAXIMUM_RECENT_WEAPONS];
	long throw_times[MAXIMUM_GRENADE_THROWS];
	struct damage_stuck_projectile stuck[MAXIMUM_STUCK_PROJECTILES];
	long driven_time;
	long unarmed_melee_damage_index;
	real hit_seconds;
	long identifier;
} damage_players[MAXIMUM_TRACKED_PLAYERS];
static struct { long time; short count; } damage_explosions[MAXIMUM_TRACKED_PLAYERS];
static struct { long time; } damage_trails[MAXIMUM_TRACKED_PLAYERS][PLAYER_TRAIL_POINTS];
static struct { long time; } damage_fire_origins[MAXIMUM_TRACKED_PLAYERS];
static struct damage_round_trip
{
	long time;
	real ticks;
} damage_round_trips[MAXIMUM_TRACKED_PLAYERS][ROUND_TRIP_WINDOW_TICKS];

struct player_datum
{
	word identifier;
};
static struct player_datum players[MAXIMUM_TRACKED_PLAYERS];

static struct player_datum *distributed_player(short player_index)
{
	return players[player_index].identifier ? &players[player_index] : NULL;
}

struct report
{
	long object_index;
	real_point3d target_position;
	long host_time;
};

#include "under_test.inc"

/* where the target is at a time in ticks (a fraction: a sub-step's end):
running straight along x at speed world units a tick, so that where it was
long ago is nowhere near where it has been lately */
static real_point3d target_at(real time, real speed)
{
	real_point3d point;

	point.x = speed * time;
	point.y = 5.0f;
	point.z = 0.5f;
	return point;
}

/* the host's history of the target up to its tick now */
static void history(real speed)
{
	long back;

	memset(damage_history, 0, sizeof(damage_history));
	for (back = 0; back < TARGET_HISTORY_TICKS; back++)
	{
		long time = now - back;
		struct damage_history_tick *tick = damage_history_at(time);
		short player;

		tick->time = time;
		for (player = 0; player < MAXIMUM_TRACKED_PLAYERS; player++)
		{
			tick->objects[player][0].object_index = NONE;
			tick->objects[player][1].object_index = NONE;
		}
		tick->objects[TARGET_PLAYER][0].object_index = TARGET;
		tick->objects[TARGET_PLAYER][0].position = target_at((real)time, speed);
		tick->objects[TARGET_PLAYER][0].speed = speed;
	}
}

/* a report saying it was made at the host's tick made, of the target where
the shooter saw it at seen (in ticks), checked by the host now: taken or not */
static boolean taken(long made, real seen, real speed)
{
	struct report report;
	real_point3d host_pose;

	report.object_index = TARGET;
	report.host_time = made;
	report.target_position = target_at(seen, speed);
	return report_checked(0, SHOOTER, &report, &host_pose);
}

/* the host's ticks run on, its measure of the shooter's machine's round
trip (its smoothed one, average and jitter) trip all the while, noted each
tick; the target's history up to the last */
static void ticks_with(long count, real trip, real speed)
{
	long index;

	round_trip = trip;
	for (index = 0; index < count; index++)
	{
		now++;
		distributed_note_round_trips();
	}
	history(speed);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	real const speed = 0.25f;

	now = 5000;
	{
		short player;
		short index;

		for (player = 0; player < MAXIMUM_TRACKED_PLAYERS; player++)
		{
			players[player].identifier = (word)(player + 1);
			for (index = 0; index < ROUND_TRIP_WINDOW_TICKS; index++)
				damage_round_trips[player][index].time = NONE;
		}
	}

	/* a shooter on a slow link (the longest round trip the host measures)
	hits a running target where it saw it, at each age the link gives,
	older than a second among them: taken */
	CASE("high-ping-moving")
	{
		long const ages[] = { 0, 6, 20, 31, 32, 45, 60, 75 };
		short index;

		round_trip = 60.0f;
		history(speed);
		for (index = 0; index < (short)NUMBEROF(ages); index++)
		{
			long made = now - ages[index];

			CHECK(taken(made, (real)made, speed), "a hit made %ld ticks ago on a running target (round trip %.0f): refused",
				ages[index], round_trip);
			CHECK(taken(made, (real)(made - TARGET_HISTORY_SLACK_TICKS), speed),
				"a hit made %ld ticks ago, its frame drawn behind: refused", ages[index]);
		}
		return 0;
	}

	/* the oldest a report may be is its machine's round trip and the slack
	(no more than the history holds): one tick older is refused, even of a
	target still where the report has it; and none older than the longest
	age, however long the measured round trip */
	CASE("claimed-too-old")
	{
		real const trips[] = { 6.0f, 20.0f, 45.0f, 200.0f };
		short index;

		history(0.0f);
		for (index = 0; index < (short)NUMBEROF(trips); index++)
		{
			long oldest;

			round_trip = trips[index];
			oldest = MIN((long)round_trip + REPORT_DELAY_SLACK_TICKS, REPORT_MAXIMUM_AGE_TICKS);
			CHECK(taken(now - oldest, (real)now, 0.0f), "a report as old as a %.0f-tick round trip allows: refused",
				round_trip);
			CHECK(!taken(now - oldest - 1, (real)now, 0.0f),
				"a report claiming %ld ticks (a %.0f-tick round trip allows %ld): taken", oldest + 1, round_trip, oldest);
		}
		/* (a low-ping client that claims a second and more back, to have its
		hit checked against where the target was at a moment of its choosing) */
		round_trip = 6.0f;
		history(speed);
		CHECK(!taken(now - 60, (real)(now - 10), speed), "a 6-tick client's report claiming 60 ticks: taken");
		return 0;
	}

	/* a report said to be made after the host's now, or with no time: refused */
	CASE("claimed-future")
	{
		round_trip = 6.0f;
		history(0.0f);
		CHECK(!taken(now + 1, (real)now, 0.0f), "a report from the future: taken");
		CHECK(!taken(NONE, (real)now, 0.0f), "a report with no time: taken");
		return 0;
	}

	/* a target where the host never had it, within the age: refused */
	CASE("never-there")
	{
		round_trip = 60.0f;
		history(speed);
		CHECK(!taken(now - 45, (real)(now - 200), speed), "a target where it was long before the report: taken");
		CHECK(!taken(now - 10, (real)(now + 60), speed), "a target where it will be: taken");
		return 0;
	}

	/* a 30, 60 or 120 Hz shooter's sub-step pose (between two ticks'), at a
	high ping: taken alike, the windows being in the game's ticks */
	CASE("every-rate")
	{
		short substeps;

		round_trip = 50.0f;
		history(speed);
		for (substeps = 1; substeps <= 4; substeps *= 2)
		{
			long made;

			for (made = now - 50; made <= now - 40; made++)
			{
				short substep;

				for (substep = 0; substep < substeps; substep++)
				{
					real seen = (real)(made - 1) + (real)(substep + 1) / (real)substeps;

					CHECK(taken(made, seen, speed), "a %d Hz shot made %ld ticks ago: refused", 30 * substeps, now - made);
				}
			}
		}
		return 0;
	}

	/* a low-ping client (4 ticks) whose round trip jumps to 45 for half a
	second, then falls back (the smoothed measure within a second): its
	reports delayed by the spike, and those sent again on the stream after
	it, are taken while the spike's measurement is within the window, as
	old as that round trip and the slack allow and no older */
	CASE("spike-taken")
	{
		ticks_with(300, 4.0f, speed);
		CHECK(!taken(now - 45, (real)(now - 45), speed), "a 4-tick client's report 45 ticks old, no spike measured: taken");
		ticks_with(1, 45.0f, speed);
		CHECK(taken(now - 45, (real)(now - 45), speed), "a report delayed by the spike as it is measured: refused");
		ticks_with(14, 45.0f, speed);
		ticks_with(30, 5.0f, speed);
		CHECK(taken(now - 70, (real)(now - 70), speed),
			"a report made in the spike, sent again, 70 ticks old a second after it (round trip back to 5): refused");
		CHECK(!taken(now - 45 - REPORT_DELAY_SLACK_TICKS - 1, (real)(now - 76), speed),
			"a report older than the spike's round trip and the slack: taken");
		return 0;
	}

	/* once the window has passed with the round trip low again, the bound
	tightens back to it */
	CASE("spike-tightens")
	{
		ticks_with(300, 4.0f, speed);
		ticks_with(15, 45.0f, speed);
		ticks_with(ROUND_TRIP_WINDOW_TICKS - 1, 5.0f, speed);
		CHECK(taken(now - 70, (real)(now - 70), speed), "the spike forgotten before the window passed");
		ticks_with(1, 5.0f, speed);
		CHECK(!taken(now - 70, (real)(now - 70), speed), "a report 70 ticks old a window after the spike: taken");
		CHECK(taken(now - 5 - REPORT_DELAY_SLACK_TICKS, (real)(now - 35), speed),
			"a report as old as the low round trip and the slack: refused");
		return 0;
	}

	/* a client claiming an old time with no measured delay behind it
	(steady at 4 ticks; another client's spike is not its own): refused */
	CASE("unsupported-claim")
	{
		ticks_with(300, 4.0f, speed);
		CHECK(!taken(now - 70, (real)(now - 70), speed), "a 4-tick client claiming 70 ticks: taken");
		CHECK(!taken(now - 4 - REPORT_DELAY_SLACK_TICKS - 1, (real)(now - 35), speed), "a claim past its round trip: taken");
		CHECK(taken(now - 4 - REPORT_DELAY_SLACK_TICKS, (real)(now - 34), speed), "a claim within its round trip: refused");
		/* (the other client's machine's 60 ticks noted in its slot) */
		damage_round_trips[1][(now + 1) % ROUND_TRIP_WINDOW_TICKS].time = now;
		damage_round_trips[1][(now + 1) % ROUND_TRIP_WINDOW_TICKS].ticks = 60.0f;
		CHECK(!taken(now - 70, (real)(now - 70), speed), "another client's spike widened this one's: taken");
		return 0;
	}

	/* a joiner in a quitter's slot starts afresh (the quitter's rocket not
	theirs); the same player keeps theirs; one in an empty slot adopts it */
	CASE("slot-reused")
	{
		memset(damage_players, 0, sizeof(damage_players));
		players[2].identifier = 7;
		distributed_note_player_slot(2);
		damage_players[2].definition_indices[0] = ROCKET;
		damage_players[2].times[0] = now;
		distributed_note_player_slot(2);
		CHECK(damage_players[2].definition_indices[0] == ROCKET, "the same player's rocket forgotten");
		/* (the quitter's slot taken by a joiner within the tick) */
		players[2].identifier = 8;
		distributed_note_player_slot(2);
		CHECK(damage_players[2].definition_indices[0] == NONE, "a joiner inherited the quitter's rocket");
		CHECK(damage_players[2].identifier == 8, "the slot's player not noted");
		/* (the slot left empty, then taken) */
		damage_players[2].definition_indices[0] = ROCKET;
		players[2].identifier = 0;
		distributed_note_player_slot(2);
		CHECK(damage_players[2].definition_indices[0] == NONE, "a quitter's rocket kept in an empty slot");
		players[2].identifier = 9;
		distributed_note_player_slot(2);
		CHECK(damage_players[2].identifier == 9 && damage_players[2].hit_seconds == (real)HIT_REPORT_BURST_SECONDS,
			"a joiner in an empty slot not given a fresh slot");
		/* (another slot untouched) */
		CHECK(damage_players[1].identifier == 0, "another slot changed");
		/* (the quitter's lag spike not the joiner's) */
		memset(damage_players, 0, sizeof(damage_players));
		players[SHOOTER].identifier = 7;
		distributed_note_player_slot(SHOOTER);
		ticks_with(10, 60.0f, 0.0f);
		ticks_with(1, 4.0f, 0.0f);
		CHECK(taken(now - 80, (real)now, 0.0f), "the quitter's spike not taken into account");
		players[SHOOTER].identifier = 8;
		distributed_note_player_slot(SHOOTER);
		CHECK(!taken(now - 80, (real)now, 0.0f), "a joiner inherited the quitter's round trips");
		return 0;
	}

	fprintf(stderr, "no case '%s'\n", case_name);
	return 2;
}
