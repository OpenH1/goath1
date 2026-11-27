/*
LIGHT_SOURCES.H

The enhanced dynamic lights' own lights (light_sources.c), which
dynamic_lights.c draws after the game's.
*/

#ifndef LIGHT_SOURCES_H
#define LIGHT_SOURCES_H

/* a light the frame draws: where, how far it reaches, its color (the
game's lights', 0 to 1), whether a glow is drawn about it (a projectile's
with no model), how much of the view it lights (its rank), and what it is
from frame to frame (its effect's or its object's and its row, its
marker's; dynamic_lights_shadowed's: how long it has lasted), and whether
it is a glow standing in for what the lightmap lights (a powerup's, a
teleporter's: no shadow) */
struct light_source
{
	real_point3d position;
	real radius;
	real_rgb_color color;
	boolean halo;
	real score;
	unsigned long long key;
	boolean baked;
};

void light_sources_new_map(void);
short light_sources_gather(struct light_source const **sources, short room);

#endif
