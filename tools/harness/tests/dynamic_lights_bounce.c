/*
DYNAMIC_LIGHTS_BOUNCE.C

Bounced light (port/linux/game/dynamic_lights_bounce.c): where a bounce
light goes off the wall a ray met and how bright, how it eases from frame to
frame (steady, fading with its light, starting dark on another wall), which
track a light takes again, and which lights bounce.
*/

#include "harness.h"
#include <math.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#include "under_test.inc"

static int near(real a, real b)
{
	return fabsf(a - b) < 1.0e-4f;
}

static struct halo_dynamic_light light_at(real x, real radius, real intensity, long flags)
{
	struct halo_dynamic_light light;

	memset(&light, 0, sizeof(light));
	light.position[0] = x;
	light.radius = radius;
	light.color[0] = light.color[1] = light.color[2] = intensity;
	light.cone_offset = 1.0f;
	light.flags = flags;
	return light;
}

int main(int argc, char **argv)
{
	static real const white[3] = { 1.0f, 1.0f, 1.0f };
	static real const grey[3] = { 0.5f, 0.5f, 0.5f };
	real_point3d wall = { 2.0f, 0.0f, 0.0f };
	real_vector3d facing_x = { -1.0f, 0.0f, 0.0f };
	struct bounce_target target;
	char const *case_name;

	CHECK(argc == 2, "one named case");
	case_name = argv[1];
	memset(&target, 0, sizeof(target));
	if (!strcmp(case_name, "placed-off-wall"))
	{
		/* a light of reach 4 at the origin; a wall 2 away facing it squarely */
		real falloff = (1.0f - 0.0625f) * (1.0f - 0.0625f);

		CHECK(dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 1.0f, grey, &target), "a lit wall bounces");
		CHECK(target.have, "found");
		CHECK(near(target.point.x, 2.0f - MIN(2.0f * BOUNCE_OFFSET_SCALE, BOUNCE_OFFSET_MOST)) && near(target.point.y, 0.0f) &&
			near(target.point.z, 0.0f), "off the wall, on the light's side: %f %f %f", target.point.x, target.point.y,
			target.point.z);
		CHECK(target.point.x > 0.0f, "never past its light");
		CHECK(near(target.normal.i, -1.0f), "facing out of the wall");
		CHECK(near(target.radius, 4.0f * BOUNCE_RADIUS_SCALE), "its reach of the light's: %f", target.radius);
		CHECK(near(target.color[0], falloff * 0.5f * BOUNCE_ALBEDO_SCALE),
			"the light's falloff at the wall times its albedo: %f", target.color[0]);
		/* nearer, brighter; at a slant, dimmer */
		{
			struct bounce_target nearer, slanted;

			dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 1.0f, 1.0f, grey, &nearer);
			dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 0.5f, grey, &slanted);
			CHECK(nearer.color[0] > target.color[0], "a nearer wall bounces more");
			CHECK(near(slanted.color[0], target.color[0] * 0.5f), "a wall at 60 degrees half as much: %f",
				slanted.color[0]);
		}
		return 0;
	}
	if (!strcmp(case_name, "out-of-reach"))
	{
		CHECK(!dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 4.0f, 1.0f, grey, &target),
			"a wall at the light's reach is not lit");
		CHECK(!dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 0.0f, grey, &target),
			"a wall edge on is not lit");
		CHECK(!dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, -0.5f, grey, &target),
			"a wall facing away is not lit");
		{
			static real const black[3] = { 0.0f, 0.0f, 0.0f };

			CHECK(!dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 1.0f, black, &target),
				"a black wall sends nothing on");
		}
		CHECK(!target.have, "none found");
		return 0;
	}
	if (!strcmp(case_name, "albedo-tints"))
	{
		static real const red[3] = { 0.8f, 0.05f, 0.05f };
		static real const blue_light[3] = { 0.1f, 0.2f, 1.0f };

		CHECK(dynamic_lights_bounce_place(4.0f, blue_light, &wall, &facing_x, 1.0f, 1.0f, red, &target), "lit");
		CHECK(target.color[0] > target.color[1] && target.color[2] > target.color[1],
			"a blue light off a red wall: purple: %f %f %f", target.color[0], target.color[1], target.color[2]);
		return 0;
	}
	if (!strcmp(case_name, "radius-clamped"))
	{
		CHECK(dynamic_lights_bounce_place(0.6f, white, &wall, &facing_x, 0.1f, 1.0f, white, &target), "lit");
		CHECK(near(target.radius, BOUNCE_RADIUS_LEAST), "a small light's bounce at least: %f", target.radius);
		CHECK(dynamic_lights_bounce_place(40.0f, white, &wall, &facing_x, 2.0f, 1.0f, white, &target), "lit");
		CHECK(near(target.radius, BOUNCE_RADIUS_MOST), "a wide light's at most: %f", target.radius);
		return 0;
	}
	if (!strcmp(case_name, "steady"))
	{
		struct bounce_slot slot;
		real before[3];
		int frame;

		memset(&slot, 0, sizeof(slot));
		dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 1.0f, grey, &target);
		dynamic_lights_bounce_ease(&slot, &target, 1.0f);
		CHECK(slot.have && near(slot.color[0], target.color[0]), "a new light's bounce at once (eased by 1)");
		for (frame = 0; frame < 120; frame++)
		{
			memcpy(before, slot.color, sizeof(before));
			dynamic_lights_bounce_ease(&slot, &target, 0.3f);
			CHECK(slot.have && slot.color[0] == before[0] && slot.color[1] == before[1] && slot.color[2] == before[2],
				"a steady light's bounce does not change: frame %d %f %f", frame, slot.color[0], before[0]);
		}
		/* the wall's point drifting a little is the same wall */
		target.point.y += 0.1f;
		dynamic_lights_bounce_ease(&slot, &target, 0.3f);
		CHECK(near(slot.color[0], target.color[0]), "a small move keeps its brightness: %f", slot.color[0]);
		return 0;
	}
	if (!strcmp(case_name, "fades-with-source"))
	{
		struct bounce_slot slot;
		struct bounce_target dimmer, none;
		int frame;

		memset(&slot, 0, sizeof(slot));
		memset(&none, 0, sizeof(none));
		dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 2.0f, 1.0f, grey, &target);
		dynamic_lights_bounce_ease(&slot, &target, 1.0f);
		dimmer = target;
		dimmer.color[0] *= 0.25f;
		dynamic_lights_bounce_ease(&slot, &dimmer, 0.5f);
		CHECK(slot.color[0] < target.color[0] && slot.color[0] > dimmer.color[0],
			"a fading light's bounce eases down after it: %f", slot.color[0]);
		for (frame = 0; frame < 4; frame++)
			dynamic_lights_bounce_ease(&slot, &none, 0.5f);
		CHECK(slot.color[0] < dimmer.color[0], "a wall no longer met fades: %f", slot.color[0]);
		for (frame = 0; frame < 60 && slot.have; frame++)
			dynamic_lights_bounce_ease(&slot, &none, 0.5f);
		CHECK(!slot.have, "and is gone once dim");
		return 0;
	}
	if (!strcmp(case_name, "new-wall-starts-dark"))
	{
		struct bounce_slot slot;
		struct bounce_target other;

		memset(&slot, 0, sizeof(slot));
		dynamic_lights_bounce_place(4.0f, white, &wall, &facing_x, 1.0f, 1.0f, grey, &target);
		dynamic_lights_bounce_ease(&slot, &target, 1.0f);
		/* the ray passes a corner: its wall now 2 units further */
		other = target;
		other.point.x += 2.0f;
		other.color[0] = target.color[0] * 0.5f;
		dynamic_lights_bounce_ease(&slot, &other, 0.25f);
		CHECK(near(slot.point.x, other.point.x), "moved to the new wall");
		CHECK(near(slot.color[0], other.color[0] * 0.25f), "beginning dark there, not at the old wall's: %f",
			slot.color[0]);
		/* turned to face another way: another wall too */
		slot.color[0] = 1.0f;
		other.normal.i = 0.0f;
		other.normal.k = 1.0f;
		dynamic_lights_bounce_ease(&slot, &other, 0.25f);
		CHECK(near(slot.color[0], other.color[0] * 0.25f), "a wall facing elsewhere begins dark: %f", slot.color[0]);
		return 0;
	}
	if (!strcmp(case_name, "match-nearest"))
	{
		struct bounce_track tracks[BOUNCE_SOURCES];
		boolean taken[BOUNCE_SOURCES];
		real_point3d at = { 0.0f, 0.0f, 0.0f };

		memset(tracks, 0, sizeof(tracks));
		memset(taken, 0, sizeof(taken));
		tracks[0].used = TRUE;
		tracks[0].position.x = 1.5f;
		tracks[2].used = TRUE;
		tracks[2].position.x = 0.5f;
		tracks[3].used = TRUE;
		tracks[3].position.x = 50.0f;
		CHECK(dynamic_lights_bounce_match(tracks, taken, &at, 1.0f) == 2, "the nearest track");
		taken[2] = TRUE;
		CHECK(dynamic_lights_bounce_match(tracks, taken, &at, 1.0f) == 0, "the nearest not yet taken");
		taken[0] = TRUE;
		CHECK(dynamic_lights_bounce_match(tracks, taken, &at, 1.0f) == NONE, "none too far");
		CHECK(dynamic_lights_bounce_match(tracks, taken, &at, 200.0f) == 3, "a wide light's reach goes further");
		return 0;
	}
	if (!strcmp(case_name, "choose-strongest"))
	{
		struct halo_dynamic_light lights[10];
		real_point3d camera = { 0.0f, 0.0f, 0.0f };
		short chosen[BOUNCE_SOURCES], count, index;

		for (index = 0; index < 10; index++)
			lights[index] = light_at(20.0f, 2.0f, 0.5f, 0);
		lights[3] = light_at(2.0f, 2.0f, 1.0f, 0);
		lights[5] = light_at(1.0f, 4.0f, 1.0f, HALO_DYNAMIC_LIGHT_MODELS_ONLY);
		lights[6] = light_at(1.0f, 0.2f, 1.0f, 0);
		lights[7] = light_at(1.0f, 4.0f, 0.01f, 0);
		count = dynamic_lights_bounce_choose(lights, 10, &camera, chosen);
		CHECK(count == BOUNCE_SOURCES, "at most %d: %d", BOUNCE_SOURCES, count);
		CHECK(chosen[0] == 3, "the strongest to the view first: %d", chosen[0]);
		for (index = 0; index < count; index++)
		{
			CHECK(chosen[index] != 5 && chosen[index] != 6 && chosen[index] != 7,
				"no spotlight's gel, no small or dim light: %d", chosen[index]);
		}
		for (index = 2; index < count; index++)
			CHECK(chosen[index] > chosen[index - 1], "the earlier among equals");
		return 0;
	}
	CHECK(FALSE, "unknown case %s", case_name);
	return 1;
}
