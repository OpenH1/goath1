/*
PROJECTILE_MUZZLE.C

A local player's own shot seen leaving the first-person weapon's muzzle.

A player's shot starts at the camera plus its trigger's first-person offset
(weapons.c), for most weapons on the view's own line, a few inches ahead of
the eye: at the stock angle its first frames were drawn at the crosshair,
under a big sprite, and with display.fov wider than the stock angle, small
at the crosshair, well away from the gun, which is drawn at its own angle
(display.viewmodel_fov, or the stock one: view_fov.c).

So, as later games do, the firing player's own view draws it from where its
weapon's muzzle is seen: the trigger's marker on the first-person weapon as
last drawn (first_person_weapons.c), moved across the view by the ratio of
the world's and the weapon's tangents (where the weapon's projection draws
it, in the world's), less where the shot starts. That offset is drawn whole
for its first two ticks and blended to its true path over 3 more (a tenth of
a second, smoothstep): by then a shot of any speed is far enough out that
it is drawn nearly where it is, and the time is the same whether it is
drawn between the ticks or from its newest sub-steps (render_interpolation.c,
the same game clock), at every speed (a rocket's or a needle's). Its nodes
are moved, so what hangs off its markers (its light, contrail, particles:
objects.c's object_get_node_matrices) is drawn moved with it.

Only the firer's own view, in the first person with the weapon drawn: not
another player's window, nor a Spartan link feed (render.local_player_index
NONE), nor its firer's in the third person. The effects moved once a frame
(game_frame, between the views) take it when the game has one local player.
Kept here for the frames only: the simulation (where it is, what it hits)
never reads it.
*/

#include "cseries.h"
#include "projectile_muzzle.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "objects/objects.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "units/units.h"
#include "interface/first_person_weapons.h"
#include "view_fov.h"
#include "spartan_link.h"

#include <math.h>

real render_interpolation_fraction(void);

enum
{
	MAXIMUM_MUZZLE_SHOTS = 16,
	MAXIMUM_MUZZLE_SHOT_NODES = 8,
	MAXIMUM_MUZZLE_MARKERS = 4
};

/* (ticks: the first drawn whole, then blended away) */
#define MUZZLE_SHOT_WHOLE_TICKS 2.0f
#define MUZZLE_SHOT_BLEND_TICKS 3.0f

struct muzzle_shot
{
	boolean in_use;
	long object_index;
	short local_player_index;
	long fired_time;
	/* how far it is drawn from where it is, whole (world units) */
	real_vector3d offset;
	real_matrix4x3 nodes[MAXIMUM_MUZZLE_SHOT_NODES];
};

static struct muzzle_shot shots[MAXIMUM_MUZZLE_SHOTS];
static short shots_live;
static short shot_next;
static boolean views_drawing;

static void shot_forget(struct muzzle_shot *shot)
{
	if (shot->in_use)
	{
		shot->in_use = FALSE;
		shots_live--;
	}
}

/* how much of its offset it is drawn with (0: done) */
static real shot_weight(struct muzzle_shot const *shot)
{
	real age = (real)(game_time_get() - shot->fired_time) + render_interpolation_fraction();
	real part;

	/* (a game state loaded, a new map: another clock) */
	if (!(age >= 0.0f) || age >= MUZZLE_SHOT_WHOLE_TICKS + MUZZLE_SHOT_BLEND_TICKS)
		return 0.0f;
	if (age <= MUZZLE_SHOT_WHOLE_TICKS)
		return 1.0f;
	part = (age - MUZZLE_SHOT_WHOLE_TICKS) / MUZZLE_SHOT_BLEND_TICKS;
	return 1.0f - part * part * (3.0f - 2.0f * part);
}

void projectile_muzzle_fired(long projectile_index, long unit_index, long weapon_index, char const *marker_name,
	short marker_index)
{
	struct object_datum *object;
	struct render_camera camera;
	struct muzzle_shot *shot;
	struct object_marker markers[MAXIMUM_MUZZLE_MARKERS];
	real weapon_vertical, ratio, along;
	real_vector3d from_camera, offset;
	real_point3d muzzle, seen;
	short marker_count;
	short local_player_index;
	short index;

	if (projectile_index == NONE || unit_index == NONE || weapon_index == NONE || !marker_name)
		return;
	/* (the done let go of first: a slot is free while any is) */
	if (shots_live)
	{
		for (index = 0; index < MAXIMUM_MUZZLE_SHOTS; index++)
		{
			if (shots[index].in_use &&
				(!object_try_and_get(shots[index].object_index) || shot_weight(&shots[index]) <= 0.0f))
			{
				shot_forget(&shots[index]);
			}
		}
	}
	local_player_index = unit_get_local_player_index(unit_index);
	if (local_player_index == NONE ||
		!viewmodel_projection_seen(local_player_index, &camera, &weapon_vertical) ||
		!(camera.vertical_field_of_view > 0.0f && camera.vertical_field_of_view < _pi) ||
		!(weapon_vertical > 0.0f && weapon_vertical < _pi))
	{
		return;
	}
	ratio = tanf(camera.vertical_field_of_view * 0.5f) / tanf(weapon_vertical * 0.5f);
	object = object_try_and_get(projectile_index);
	if (!object || !(ratio > 0.0f && ratio < 16.0f))
		return;
	/* the trigger's muzzle on the first-person weapon, as last drawn (in
	the world, by the camera: first_person_weapons.c) */
	marker_count = first_person_weapon_get_marker_by_name(weapon_index, marker_name, markers, MAXIMUM_MUZZLE_MARKERS);
	if (marker_count <= 0)
		return;
	muzzle = markers[marker_index < marker_count ? marker_index : marker_count - 1].matrix.position;
	/* (where the weapon's projection draws it, at its depth, in the world's:
	across the view by the tangents' ratio) */
	from_camera.i = muzzle.x - camera.position.x;
	from_camera.j = muzzle.y - camera.position.y;
	from_camera.k = muzzle.z - camera.position.z;
	along = dot_product3d(&from_camera, &camera.forward);
	seen.x = camera.position.x + along * camera.forward.i + (from_camera.i - along * camera.forward.i) * ratio;
	seen.y = camera.position.y + along * camera.forward.j + (from_camera.j - along * camera.forward.j) * ratio;
	seen.z = camera.position.z + along * camera.forward.k + (from_camera.k - along * camera.forward.k) * ratio;
	offset.i = seen.x - object->object.position.x;
	offset.j = seen.y - object->object.position.y;
	offset.k = seen.z - object->object.position.z;
	/* (a muzzle far from the shot: not this shot's, a camera cut) */
	if (!(along > 0.0f) || !(magnitude_squared3d(&offset) < 1.0f))
		return;
	for (index = 0; index < MAXIMUM_MUZZLE_SHOTS; index++)
	{
		if (!shots[shot_next].in_use)
			break;
		shot_next = (short)((shot_next + 1) % MAXIMUM_MUZZLE_SHOTS);
	}
	shot = &shots[shot_next];
	shot_next = (short)((shot_next + 1) % MAXIMUM_MUZZLE_SHOTS);
	shot_forget(shot);
	shot->offset = offset;
	shot->in_use = TRUE;
	shot->object_index = projectile_index;
	shot->local_player_index = local_player_index;
	shot->fired_time = game_time_get();
	shots_live++;
}

void projectile_muzzle_views(boolean drawing)
{
	views_drawing = drawing;
}

real_matrix4x3 *projectile_muzzle_nodes(long object_index, real_matrix4x3 *nodes)
{
	struct muzzle_shot *shot = NULL;
	struct object_datum *object;
	real_matrix4x3 const *from;
	real weight;
	short node_count;
	short index;

	if (!shots_live || object_index == NONE)
		return nodes;
	for (index = 0; index < MAXIMUM_MUZZLE_SHOTS; index++)
	{
		if (shots[index].in_use && shots[index].object_index == object_index)
		{
			shot = &shots[index];
			break;
		}
	}
	if (!shot)
		return nodes;
	/* (the firer's own view, with the weapon drawn; between the views,
	what moves once a frame, with one local player) */
	if (views_drawing ?
		render.local_player_index != shot->local_player_index || spartan_link_drawing() ||
			!viewmodel_projection_seen(shot->local_player_index, NULL, NULL) :
		players_globals->local_player_count != 1)
	{
		return nodes;
	}
	weight = shot_weight(shot);
	if (weight <= 0.0f)
		return nodes;
	object = object_get(object_index);
	node_count = (short)(object->object.node_matrices.size / (short)sizeof(real_matrix4x3));
	if (node_count <= 0 || node_count > MAXIMUM_MUZZLE_SHOT_NODES)
		return nodes;
	from = nodes ? nodes :
		(real_matrix4x3 const *)object_header_block_get(object_index, &object->object.node_matrices);
	for (index = 0; index < node_count; index++)
	{
		shot->nodes[index] = from[index];
		shot->nodes[index].position.x += weight * shot->offset.i;
		shot->nodes[index].position.y += weight * shot->offset.j;
		shot->nodes[index].position.z += weight * shot->offset.k;
	}
	return shot->nodes;
}
