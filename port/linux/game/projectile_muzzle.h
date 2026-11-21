/*
PROJECTILE_MUZZLE.H

A local player's own shot drawn leaving the first-person weapon at a wide
field of view (projectile_muzzle.c). Drawing only: the game never reads it.
*/

#ifndef PROJECTILE_MUZZLE_H
#define PROJECTILE_MUZZLE_H

struct real_matrix4x3;

/* weapons.c, in the tick: a projectile a player's unit fired from its
camera (the trigger's first-person offset), made and placed, by the
weapon's trigger of this marker (its marker_index'th) */
void projectile_muzzle_fired(long projectile_index, long unit_index, long weapon_index, char const *marker_name,
	short marker_index);

/* render.c: the frame's views are drawn (TRUE) or done (FALSE) */
void projectile_muzzle_views(boolean drawing);

/* render_interpolation.c, while a frame is drawn: the nodes the object is
drawn with (NULL: its own), or the shot's moved toward where its firer's
weapon is seen */
struct real_matrix4x3 *projectile_muzzle_nodes(long object_index, struct real_matrix4x3 *nodes);

#endif
