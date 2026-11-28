/*
STRUCK_THROW.C

The player at the middle of the floor with a rifle of one slow shot a second
and four grenades in hand, facing a wall 20 units ahead (+x). Its shot flies
level at 10 units a second, bounces straight back off the wall (it keeps all
its speed) and strikes the player 4 seconds later, as it throws a grenade:
the blow, two fifths of its body, pings it hard with the grenade still in
its hand, which lets go of the grenade (units.c's unit_ping_animation),
inside the shot's update. At a rate above 30 the grenade let go of then steps at once
(projectiles.c's projectile_substep_launch_on_tick), within the shot's step
(tests/test_struck_throw.py: debug.timings' sections, entered again there).
*/

#include "../stagekit.h"

static void struck_throw_build(
	struct stage *stage)
{
	struct projectile_shape shot;
	struct projectile_shape grenade;
	real wall = stage_parameter("wall", 20.f);
	/* (shoot.c's near wall: its face toward the player, 2 high) */
	real_rectangle3d across_x = { { 0.f, 0.5f, -1.f, 1.f, 0.f, 2.f } };
	real_point3d start = { { 0.f, 0.f, 0.f } };
	real_point3d wall_position = { { 0.f, 0.f, 0.f } };

	/* (StageKit's own round numbers: a slow shot, no gravity, a fuse long
	enough to come back, a bounce that keeps its speed; a blow of 30 of the
	body's 75, over its hard ping's tenth) */
	shot.speed = stage_parameter("shot_speed", 10.f);
	shot.gravity_scale = 0.f;
	shot.fuse = 20.f;
	shot.bounce = 1.f;
	shot.friction = 0.f;
	/* (throw.c's grenade) */
	grenade.speed = 9.f;
	grenade.gravity_scale = 1.f;
	grenade.fuse = 6.f;
	grenade.bounce = 0.4f;
	grenade.friction = 0.5f;
	stage_chamber_material(stage, _material_stone);
	player_loadout(stage, prop_weapon("stagekit\\rifle",
		prop_damaging_projectile("stagekit\\returning_shot", &shot, 30.f), 1.f));
	prop_grenades(stage, prop_projectile("stagekit\\grenade", &grenade), 4, 9.f);
	prop_biped_struck(stage, 0.1f);
	/* (shield.c's body, with no shield) */
	prop_biped_flesh(stage, 75.f);
	wall_position.x = wall;
	prop_obstacle(stage, "stagekit\\wall", &across_x, &wall_position);
	player_start(stage, &start);
}

struct stagekit_scenario const scenario_struck_throw = {
	"struck_throw", "a shot that comes back off a wall and strikes the player as it throws a grenade",
	struck_throw_build };
