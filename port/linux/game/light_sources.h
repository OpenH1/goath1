/*
LIGHT_SOURCES.H

The enhanced dynamic lights' own lights (light_sources.c), which
dynamic_lights.c draws after the game's.
*/

#ifndef LIGHT_SOURCES_H
#define LIGHT_SOURCES_H

/* a light the frame draws: where, how far it reaches, its color (the
game's lights', 0 to 1), whether a glow is drawn about it (a projectile's
with no model), and how much of the view it lights (its rank) */
struct light_source
{
	real_point3d position;
	real radius;
	real_rgb_color color;
	boolean halo;
	real score;
};

void light_sources_new_map(void);
short light_sources_gather(struct light_source const **sources, short room);

#endif
