/*
BSP3D.H

header included in hcex build.
*/

#ifndef __BSP3D_H
#define __BSP3D_H
#pragma once


/* ---------- headers */

#include "tag_files/tag_groups.h"
#include "math/real_math.h"

/* ---------- constants */

enum
{
	/* port: how many nodes deep a walk of a bsp3d (from the map) goes
	before it stops: the engine's MAXIMUM_BSP3D_DEPTH (the plane stacks of
	collision_bsp.c, the path render_debug_bsp prints), where the retail
	bsps are at most 66 deep. A cycle of nodes would never end */
	MAXIMUM_BSP3D_TRAVERSAL_DEPTH = 128,
};

/* ---------- macros */

/* ---------- structures */

struct bsp3d
{
	struct tag_block nodes;
	struct tag_block planes;
};

struct bsp3d_node
{
	long plane_designator;
	long children[2];
};

typedef char bsp3d_node_size_check[
	sizeof(struct bsp3d_node) == 0xC ? 1 : -1];

/* port: a node of the current structure bsp's packed table (bsp3d.c): its
plane beside its two children */
struct bsp3d_point_node
{
	real_plane3d plane;
	long children[2];
};

/* ---------- prototypes/BSP3D.C */

long bsp3d_test_point(struct bsp3d const *bsp, long node_index, union real_point3d const *point);
/* port: the current structure bsp's table for bsp3d_test_point's walk, made
again as a bsp loads (NULL: none, as it is let go of) */
void bsp3d_point_table_build(struct bsp3d const *bsp);
/* port: the current structure bsp's packed nodes when `bsp` is it as the
table was made (and each node's plane designator, in `plane_designators`),
else NULL: the walks of collision_bsp.c read them in place of the tag data */
struct bsp3d_point_node const *bsp3d_packed_nodes(struct bsp3d const *bsp, long const **plane_designators);
/* port: HALO_BSP3D_CELLS's measures, each frame presented (report.c) */
void bsp3d_cells_frame(long long micros, unsigned long waited);

/* ---------- globals */

/* ---------- public code */

__inline real_plane3d *bsp3d_get_plane_from_designator(
	struct bsp3d const *bsp,
	long plane_designator,
	real_plane3d *result)
{
	real_plane3d* plane = TAG_BLOCK_GET_ELEMENT(&bsp->planes, plane_designator & LONG_MAX, real_plane3d);

	if (plane_designator & LONG_MIN)
	{
		plane3d_negate(plane, result);
	}
	else
	{
		*result = *plane;
	}

	return result;
}

#endif // __BSP3D_H
