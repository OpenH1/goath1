/*
EFFECT_COUNTDOWN.C

The player effects' countdowns (the screen's flash, the camera's impulse and
shake: player_effects.c) as the ticks count them (player_effect_count_down,
once a tick as it begins) and the frames draw them (player_effect_drawn,
through render_interpolation_ticks_left), at game.sim_rate 120: four
sub-steps a tick, the tick open after its first three. Frames are drawn at
every sub-step's phase and at times between them. A flash is drawn through
its ticks and the rest of the tick it ran out at, and never again after it:
its 0 ticks left read in an open tick are a tick more, which drew a sliver
of the flash in every open tick's frames and none in the others', a strobe
as long as the map ran.
*/

#include "harness.h"

#define SET_FLAG(f, b, v) ((v) ? ((f)|=(unsigned)FLAG(b)) : ((f)&=(unsigned)~FLAG(b)))
#define SUBSTEPS 4

enum
{
	_just_started_bit,
	_ran_out_bit
};

/* the frames' interpolation (render_interpolation.c's state) */
static boolean interpolation_rendering = TRUE;
static real interpolation_fraction;
static short fake_substep_next;

static short sim_substep_next(void)
{
	return fake_substep_next;
}

#include "under_test.inc"

/* a frame at the sub-step phase next (0 once the tick's four have run),
part way to the next sub-step */
static boolean frame(short ticks_left, real duration, byte flags, short next, real part, real *time_left)
{
	fake_substep_next = next;
	interpolation_fraction = ((real)next + part) / SUBSTEPS;
	return player_effect_drawn(ticks_left, duration, TEST_FLAG(flags, _just_started_bit),
		TEST_FLAG(flags, _ran_out_bit), time_left);
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";
	static short const phases[] = { 1, 2, 3, 0 };
	static real const parts[] = { 0.0f, 0.3f, 0.7f, 0.99f };

	CASE("flash")
	{
		short const duration = 10;
		short ticks_left = duration;
		byte flags = FLAG(_just_started_bit);
		long tick;
		long ran_out_tick = NONE;

		for (tick = 0; tick < 60; tick++)
		{
			short p;
			short q;

			player_effect_count_down(&ticks_left, &flags, _just_started_bit, _ran_out_bit);
			if (ticks_left <= 0 && ran_out_tick == NONE)
				ran_out_tick = tick;
			for (p = 0; p < 4; p++)
			{
				for (q = 0; q < 4; q++)
				{
					real time_left;
					boolean drawn = frame(ticks_left, duration, flags, phases[p], parts[q], &time_left);

					if (ran_out_tick == NONE)
						CHECK(drawn, "tick %ld: %d left, not drawn", tick, ticks_left);
					else if (tick > ran_out_tick)
						CHECK(!drawn, "tick %ld (ran out at %ld), phase %d part %.2f: drawn again, %f left",
							tick, ran_out_tick, phases[p], parts[q], time_left);
					else if (phases[p] && parts[q] < 0.99f)
						CHECK(drawn && time_left > 0.0f && time_left <= 1.0f,
							"the tick it ran out at, phase %d: %f left, drawn %d", phases[p], time_left, drawn);
				}
			}
		}
		CHECK(ran_out_tick == duration - 1, "ran out at tick %ld", ran_out_tick);
	}
	CASE("restarted")
	{
		/* run out, then started again: drawn through its ticks again */
		short ticks_left = 2;
		byte flags = FLAG(_just_started_bit);
		real time_left;
		long tick;

		for (tick = 0; tick < 5; tick++)
			player_effect_count_down(&ticks_left, &flags, _just_started_bit, _ran_out_bit);
		CHECK(!frame(ticks_left, 2, flags, 1, 0.5f, &time_left), "drawn long after");
		ticks_left = 3;
		SET_FLAG(flags, _just_started_bit, TRUE);
		CHECK(frame(ticks_left, 3, flags, 1, 0.5f, &time_left), "not drawn as it starts");
		player_effect_count_down(&ticks_left, &flags, _just_started_bit, _ran_out_bit);
		CHECK(frame(ticks_left, 3, flags, 2, 0.5f, &time_left) && time_left > 2.0f, "%f left", time_left);
	}
	return 0;
}
