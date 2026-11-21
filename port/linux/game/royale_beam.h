/* royale_beam.h: Royale's glassing beam (port/linux/game/royale_beam.c)

The lane's calls core makes (royale_beam_start, _tick, _draw) and the bots'
queries (royale_beam_side, _front, _target, _ticks_to_move) are royale.h's.
These are the beam's own: the world's drawing, called from render.c with
the other transparent geometry. */
#ifndef HALO_ROYALE_BEAM_H
#define HALO_ROYALE_BEAM_H

#include "cseries.h"
#include "math/real_math.h"

/* the beam's columns, the glow along its front and the haze over the
glassed ground, in the view being drawn (nothing when no beam runs) */
void royale_beam_render(void);

/* how far a point is behind the front, in world units (negative: ahead of
it, safe); out of the map's bounds counts as behind */
real royale_beam_depth(real_point3d const *point);

/* the HUD's (royale_hud.c): the beam's stage as every machine has it (from
the synced match clock); FALSE while no beam sweeps */
struct royale_beam_hud_state
{
	boolean moving; /* the front moving now */
	boolean holding; /* waiting, a stage still to come */
	long ticks_to_move; /* while holding */
	real ticks_per_second;
	/* the point's (if given): behind the front now, behind where this
	stage's move ends (world units; negative ahead of it, safe) */
	real depth;
	real target_depth;
	real_vector2d safe; /* toward safety, across the front */
};
boolean royale_beam_hud_state(real_point3d const *point, struct royale_beam_hud_state *state);

#endif
