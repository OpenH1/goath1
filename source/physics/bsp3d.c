/*
BSP3D.C

symbols in this file:
00136600 0080:
	_bsp3d_test_point (0000)
00136680 0280:
	_bsp3d_clip_line_to_leaves (0000)
00136900 02d0:
	_code_00136900 (0000)
00136BD0 0030:
	_bsp3d_clip_polygon_to_leaves (0000)
0028AF28 000f:
	??_C@_0P@HJMKGDLK@t?$DO0?4f?5?$CG?$CG?5t?$DM1?4f?$AA@ (0000)
0028AF38 001f:
	??_C@_0BP@OECNNHLF@c?3?2halo?2SOURCE?2physics?2bsp3d?4c?$AA@ (0000)
0028AF58 0004:
	__real@39800000 (0000)
0028AF5C 0004:
	__real@b9800000 (0000)
0028AF60 0033:
	??_C@_0DD@IKBLKABF@clipped_counts?$FL0?$FN?$CB?$DNNONE?5?$CG?$CG?5clipp@ (0000)
0028AF94 0032:
	??_C@_0DC@PNNMCKKO@vertex_count?$DMMAXIMUM_VERTICES_PE@ (0000)
0028AFC8 002e:
	??_C@_0CO@JLDBGGIN@vertex_count?$DO?$DNNUMBER_OF_VERTICES@ (0000)
*/

/* ---------- headers */

#include "cseries.h"
#include "cseries/errors.h" /* port: error */
#include "bsp3d.h"
#include "math/geometry.h"

#include <stdio.h> /* port: (the walk's cells' measures) */
#include <stdlib.h> /* port: (the point walk's table) */
#include <string.h> /* port: (HALO_BSP3D_CELLS) */

void platform_log(char const *format, ...); /* port */

/* ---------- constants */

enum
{
	MAXIMUM_VERTICES_PER_CLIPPED_POLYGON = 64
};

#define CLIP_LINE_EPSILON (1.0f / 4096.0f)

/* ---------- macros */

/* ---------- structures */

typedef void (*bsp3d_line_leaf_proc)(
	real_point3d const *point0,
	real_point3d const *point1,
	long leaf_index,
	void *context);

typedef void (*bsp3d_polygon_leaf_proc)(
	real_point3d const *vertices,
	short vertex_count,
	long leaf_index,
	long plane_designator,
	void *context);

/* ---------- prototypes */

static long bsp3d_clip_polygon_to_leaves_recursive(
	struct bsp3d const *bsp,
	long node_index,
	long plane_designator,
	real_point3d const *vertices,
	short vertex_count,
	real epsilon,
	bsp3d_polygon_leaf_proc proc,
	void *context);

static __inline real bsp3d_plane_distance_to_point(
	real_plane3d const *plane,
	real_point3d const *point)
{
	return point->x*plane->n.i + point->y*plane->n.j + point->z*plane->n.k - plane->d;
}

static __inline real bsp3d_polygon_plane_distance_to_point(
	real_plane3d const *plane,
	real_point3d const *point)
{
	return plane->n.i*point->x + plane->n.j*point->y + plane->n.k*point->z - plane->d;
}

/* ---------- globals */

long node_count;

/* port: whether a map's malformed bsp3d was reported (once) */
static boolean warned_about_bsp3d_nodes;

/* port: the current structure bsp's nodes for bsp3d_test_point's walk, each
its plane beside its two children, so a step reads one record (24 bytes)
instead of a node and then its plane elsewhere in the tag data. Made as the
bsp loads (bsp3d_point_table_build, from scenario_switch_structure_bsp), and
only where every node's plane is one of the bsp's and both blocks are read
by tag_block_get_element_fast's own path: the walk on it then does the same
arithmetic on the same values and decides each step (on the plane too) as
the tag data's walk does. Any other bsp (an object's collision model) walks
the tag data */
/* (struct bsp3d_point_node: bsp3d.h) */

/* port: the walk's start by cell (with the table above). The world is cut
into cells of 1/8 unit, and a cell keeps the deepest node of the walk that
every point in it reaches: the cell (its whole box, by a conservative test
of its corners' reach with a margin over the walk's own rounding) lies on
one side of each plane above that node, so the walk's decision there is the
same for every point of the cell. A cell touching or near a plane stops
above it, and the walk from the node it keeps is the walk above: the same
arithmetic, the same decisions and its depth counted from the node's, so
the leaf (and NONE) is the same for every point.

Not a grid of the level: a fixed table of BSP3D_CELL_SLOTS words, each a
cell's coordinates (their high bits: the low ones are its slot) beside its
node and depth, filled as the walk asks (a cell asked for again takes its
slot's word), made and cleared with the table, the walk from the root where
there is none or a point is outside the cells' reach. Each word is read and
written whole (an atomic 64-bit word), so a walk on any thread sees either
a cell's own word or another's, never a mixture.

HALO_BSP3D_CELLS=off walks without it; =verify walks both ways and counts
the differences (halo.log's "bsp3d cells:" lines, every 10 seconds);
=alternate takes turns with it and without it each second and logs each
way's time in the walk, the game thread's time and the frame rate */
enum
{
	BSP3D_CELL_SLOT_BITS_X = 5,
	BSP3D_CELL_SLOT_BITS_Y = 5,
	BSP3D_CELL_SLOT_BITS_Z = 4,
	BSP3D_CELL_SLOT_BITS = BSP3D_CELL_SLOT_BITS_X + BSP3D_CELL_SLOT_BITS_Y + BSP3D_CELL_SLOT_BITS_Z,
	BSP3D_CELL_SLOTS = 1 << BSP3D_CELL_SLOT_BITS,
	/* (a cell's coordinates in 16 bits each, offset by half) */
	BSP3D_CELL_COORDINATE_OFFSET = 1 << 15,
	/* (the word: valid in bit 63, the coordinates' high bits from bit 28,
	the depth's 7 bits from 21, the node's 21 bits) */
	BSP3D_CELL_NODE_BITS = 21,
	BSP3D_CELL_DEPTH_SHIFT = 21,
	BSP3D_CELL_TAG_SHIFT = 28,
	BSP3D_CELL_LOG_SECONDS = 10,
};

#define BSP3D_CELLS_PER_UNIT 8.f
#define BSP3D_CELL_VALID (1ULL << 63)
/* the walk's rounding (a distance's three products, two sums and the
difference, each within half a unit in the last place: 4 of 2^-24 of the
terms' magnitudes) taken as 16 of them, and below any underflow's */
#define BSP3D_CELL_RELATIVE_MARGIN (1.0 / 1048576.0)
#define BSP3D_CELL_ABSOLUTE_MARGIN 1e-30

typedef char bsp3d_cell_depth_check[MAXIMUM_BSP3D_TRAVERSAL_DEPTH <= 128 ? 1 : -1];

enum
{
	_bsp3d_cells_on,
	_bsp3d_cells_off,
	_bsp3d_cells_verify,
	_bsp3d_cells_alternate,
};

static struct
{
	struct bsp3d const *bsp;
	void const *nodes_address;
	void const *planes_address;
	long node_count;
	long plane_count;
	struct bsp3d_point_node *nodes;
	/* (each node's plane designator, for collision_bsp.c's walks; NULL:
	none, and they walk the tag data) */
	long *plane_designators;
	/* (the cells' words, BSP3D_CELL_SLOTS; NULL: none) */
	unsigned long long *cells;
} bsp3d_point_table;

/* (HALO_BSP3D_CELLS's measures) */
static struct
{
	boolean mode_read;
	short mode;
	/* (alternate: the turn's way, its time so far) */
	boolean turn_off;
	long long turn_micros;
	long long log_micros;
	/* (verify) */
	unsigned long long compared;
	unsigned long long differences;
	/* (alternate, by way: [0] with the cells, [1] without) */
	struct
	{
		unsigned long long calls;
		unsigned long long ticks;
		unsigned long long frames;
		long long micros;
		long long game_micros;
	} way[2];
	unsigned long long frame_ticks;
	long long frame_ticks_micros;
	unsigned long long last_ticks;
} bsp3d_cells_measure;

static __inline unsigned long long bsp3d_cells_ticks(void)
{
#if defined(__x86_64__) || defined(__i386__)
	return __builtin_ia32_rdtsc();
#else
	return 0;
#endif
}

static short bsp3d_cells_mode(void)
{
	if (!bsp3d_cells_measure.mode_read)
	{
		char const *value = getenv("HALO_BSP3D_CELLS");

		bsp3d_cells_measure.mode_read = TRUE;
		bsp3d_cells_measure.mode = _bsp3d_cells_on;
		if (value && (!strcmp(value, "off") || !strcmp(value, "0")))
			bsp3d_cells_measure.mode = _bsp3d_cells_off;
		else if (value && !strcmp(value, "verify"))
			bsp3d_cells_measure.mode = _bsp3d_cells_verify;
		else if (value && !strcmp(value, "alternate"))
			bsp3d_cells_measure.mode = _bsp3d_cells_alternate;
	}
	return bsp3d_cells_measure.mode;
}

/* a coordinate's cell, offset into 16 bits; FALSE outside their reach (and
for a NaN) */
static __inline boolean bsp3d_cell_coordinate(
	real value,
	long *cell)
{
	real scaled = value * BSP3D_CELLS_PER_UNIT;
	long index;

	if (!(scaled > -(real)BSP3D_CELL_COORDINATE_OFFSET && scaled < (real)(BSP3D_CELL_COORDINATE_OFFSET - 1)))
		return FALSE;
	index = (long)scaled;
	if ((real)index > scaled)
		index--;
	*cell = index + BSP3D_CELL_COORDINATE_OFFSET;
	return TRUE;
}

/* which side of a node's plane every point of a cell's box is on, as the
walk decides (1: the distance >= 0, its second child; 0: < 0, its first),
or NONE where the cell touches or is near the plane */
static long bsp3d_cell_side(
	real_plane3d const *plane,
	double const center[3],
	double const reach[3])
{
	double i = plane->n.i, j = plane->n.j, k = plane->n.k, d = plane->d;
	double ai = i < 0 ? -i : i, aj = j < 0 ? -j : j, ak = k < 0 ? -k : k, ad = d < 0 ? -d : d;
	double distance = center[0] * i + center[1] * j + center[2] * k - d;
	double half = (0.5 / BSP3D_CELLS_PER_UNIT) * (ai + aj + ak);
	double margin = (reach[0] * ai + reach[1] * aj + reach[2] * ak + ad) * BSP3D_CELL_RELATIVE_MARGIN +
		BSP3D_CELL_ABSOLUTE_MARGIN;

	if (distance - half - margin > 0)
		return 1;
	if (distance + half + margin < 0)
		return 0;
	/* (and a NaN or an infinity) */
	return NONE;
}

/* a cell's word: the deepest node every point of it reaches from the root,
and the walk's depth there */
static unsigned long long bsp3d_cell_fill(
	long const cell[3],
	unsigned long long tag)
{
	struct bsp3d_point_node const *nodes = bsp3d_point_table.nodes;
	long node_count = bsp3d_point_table.node_count;
	double center[3], reach[3];
	long node_index = 0, depth = 0;
	short axis;

	for (axis = 0; axis < 3; axis++)
	{
		double low = (double)(cell[axis] - BSP3D_CELL_COORDINATE_OFFSET) / BSP3D_CELLS_PER_UNIT;
		double high = low + 1.0 / BSP3D_CELLS_PER_UNIT;

		center[axis] = low + 0.5 / BSP3D_CELLS_PER_UNIT;
		low = low < 0 ? -low : low;
		high = high < 0 ? -high : high;
		reach[axis] = low > high ? low : high;
	}
	for (;;)
	{
		long side = bsp3d_cell_side(&nodes[node_index].plane, center, reach);
		long child;

		if (side == NONE)
			break;
		child = nodes[node_index].children[side];
		/* (a leaf, or what the walk refuses, is the walk's own step) */
		if ((child & LONG_MIN) || child >= node_count || depth + 1 >= MAXIMUM_BSP3D_TRAVERSAL_DEPTH)
			break;
		node_index = child;
		depth++;
	}

	return BSP3D_CELL_VALID | (tag << BSP3D_CELL_TAG_SHIFT) |
		((unsigned long long)depth << BSP3D_CELL_DEPTH_SHIFT) | (unsigned long long)node_index;
}

/* the walk on the table from a node, the depth counted from it's */
static __inline long bsp3d_point_walk(
	long node_index,
	short depth,
	union real_point3d const *point)
{
	struct bsp3d_point_node const *nodes = bsp3d_point_table.nodes;
	long node_count = bsp3d_point_table.node_count;

	do
	{
		struct bsp3d_point_node const *node;

		if (node_index < 0 ||
			node_index >= node_count ||
			depth++ >= MAXIMUM_BSP3D_TRAVERSAL_DEPTH)
		{
			if (!warned_about_bsp3d_nodes)
			{
				error(_error_silent, "a bsp3d node is not one of the bsp's, or is more than %d deep",
					MAXIMUM_BSP3D_TRAVERSAL_DEPTH);
				warned_about_bsp3d_nodes = TRUE;
			}
			return NONE;
		}
		node = nodes + node_index;
		node_index = node->children[plane3d_distance_to_point(&node->plane, point) >= 0.f];
	}
	while (!(node_index & LONG_MIN));

	if (node_index != NONE)
		return node_index & LONG_MAX;

	return NONE;
}

/* the walk from the root, from its point's cell's node where it has one */
static __inline long bsp3d_point_walk_from_cell(
	union real_point3d const *point)
{
	unsigned long long *cells = bsp3d_point_table.cells;
	long cell[3];

	if (cells &&
		bsp3d_cell_coordinate(point->x, &cell[0]) &&
		bsp3d_cell_coordinate(point->y, &cell[1]) &&
		bsp3d_cell_coordinate(point->z, &cell[2]))
	{
		unsigned long slot = (unsigned long)(cell[0] & ((1 << BSP3D_CELL_SLOT_BITS_X) - 1)) |
			((unsigned long)(cell[1] & ((1 << BSP3D_CELL_SLOT_BITS_Y) - 1)) << BSP3D_CELL_SLOT_BITS_X) |
			((unsigned long)(cell[2] & ((1 << BSP3D_CELL_SLOT_BITS_Z) - 1)) <<
				(BSP3D_CELL_SLOT_BITS_X + BSP3D_CELL_SLOT_BITS_Y));
		unsigned long long tag = (unsigned long long)(cell[0] >> BSP3D_CELL_SLOT_BITS_X) |
			((unsigned long long)(cell[1] >> BSP3D_CELL_SLOT_BITS_Y) << (16 - BSP3D_CELL_SLOT_BITS_X)) |
			((unsigned long long)(cell[2] >> BSP3D_CELL_SLOT_BITS_Z) <<
				(32 - BSP3D_CELL_SLOT_BITS_X - BSP3D_CELL_SLOT_BITS_Y));
		unsigned long long word = __atomic_load_n(&cells[slot], __ATOMIC_RELAXED);

		if ((word >> BSP3D_CELL_TAG_SHIFT) != ((BSP3D_CELL_VALID >> BSP3D_CELL_TAG_SHIFT) | tag))
		{
			word = bsp3d_cell_fill(cell, tag);
			__atomic_store_n(&cells[slot], word, __ATOMIC_RELAXED);
		}

		return bsp3d_point_walk(
			(long)(word & ((1UL << BSP3D_CELL_NODE_BITS) - 1)),
			(short)((word >> BSP3D_CELL_DEPTH_SHIFT) & (MAXIMUM_BSP3D_TRAVERSAL_DEPTH - 1)),
			point);
	}

	return bsp3d_point_walk(0, 0, point);
}

/* ---------- public code */

void bsp3d_point_table_build(
	struct bsp3d const *bsp)
{
	long index;

	/* (cseries.h's free refuses NULL) */
	if (bsp3d_point_table.nodes)
		free(bsp3d_point_table.nodes);
	if (bsp3d_point_table.cells)
		free(bsp3d_point_table.cells);
	if (bsp3d_point_table.plane_designators)
		free(bsp3d_point_table.plane_designators);
	csmemset(&bsp3d_point_table, 0, sizeof(bsp3d_point_table));
	if (!bsp || bsp->nodes.count <= 0 || !bsp->nodes.address || !bsp->planes.address ||
		(bsp->nodes.definition && bsp->nodes.definition->element_size != sizeof(struct bsp3d_node)) ||
		(bsp->planes.definition && bsp->planes.definition->element_size != sizeof(real_plane3d)))
	{
		return;
	}
	for (index = 0; index < bsp->nodes.count; index++)
	{
		long plane_index = ((struct bsp3d_node const *)bsp->nodes.address)[index].plane_designator;

		if (plane_index < 0 || plane_index >= bsp->planes.count)
			return;
	}
	bsp3d_point_table.nodes = malloc((size_t)bsp->nodes.count * sizeof(struct bsp3d_point_node));
	if (!bsp3d_point_table.nodes)
		return;
	for (index = 0; index < bsp->nodes.count; index++)
	{
		struct bsp3d_node const *node = (struct bsp3d_node const *)bsp->nodes.address + index;
		struct bsp3d_point_node *record = bsp3d_point_table.nodes + index;

		record->plane = ((real_plane3d const *)bsp->planes.address)[node->plane_designator];
		record->children[0] = node->children[0];
		record->children[1] = node->children[1];
	}
	bsp3d_point_table.plane_designators = malloc((size_t)bsp->nodes.count * sizeof(long));
	if (bsp3d_point_table.plane_designators)
	{
		for (index = 0; index < bsp->nodes.count; index++)
		{
			bsp3d_point_table.plane_designators[index] =
				((struct bsp3d_node const *)bsp->nodes.address)[index].plane_designator;
		}
	}
	/* (the cells' table, empty: none where the nodes are more than its
	words hold, or it was refused, or HALO_BSP3D_CELLS=off) */
	if (bsp->nodes.count <= (1L << BSP3D_CELL_NODE_BITS) && bsp3d_cells_mode() != _bsp3d_cells_off)
	{
		bsp3d_point_table.cells = malloc(BSP3D_CELL_SLOTS * sizeof(*bsp3d_point_table.cells));
		if (bsp3d_point_table.cells)
			csmemset(bsp3d_point_table.cells, 0, BSP3D_CELL_SLOTS * sizeof(*bsp3d_point_table.cells));
	}
	if (bsp3d_cells_mode() != _bsp3d_cells_on)
	{
		platform_log("bsp3d cells: %s, %d cells (%d KB) for the bsp's %ld nodes",
			bsp3d_point_table.cells ? "made" : "none", BSP3D_CELL_SLOTS,
			(int)(BSP3D_CELL_SLOTS * sizeof(*bsp3d_point_table.cells) / 1024), bsp->nodes.count);
	}
	bsp3d_point_table.bsp = bsp;
	bsp3d_point_table.nodes_address = bsp->nodes.address;
	bsp3d_point_table.planes_address = bsp->planes.address;
	bsp3d_point_table.node_count = bsp->nodes.count;
	bsp3d_point_table.plane_count = bsp->planes.count;

	return;
}

/* port: HALO_BSP3D_CELLS=verify's and =alternate's measures, each frame
presented (report.c): micros, its time, waited, its waits */
void bsp3d_cells_frame(
	long long micros,
	unsigned long waited)
{
	short mode = bsp3d_cells_measure.mode_read ? bsp3d_cells_measure.mode : bsp3d_cells_mode();
	unsigned long long now;
	short way;

	if (mode != _bsp3d_cells_verify && mode != _bsp3d_cells_alternate)
		return;
	now = bsp3d_cells_ticks();
	if (!bsp3d_point_table.cells || micros <= 0)
	{
		bsp3d_cells_measure.last_ticks = now;
		return;
	}
	if (bsp3d_cells_measure.last_ticks)
	{
		bsp3d_cells_measure.frame_ticks += now - bsp3d_cells_measure.last_ticks;
		bsp3d_cells_measure.frame_ticks_micros += micros;
	}
	bsp3d_cells_measure.last_ticks = now;
	if (mode == _bsp3d_cells_alternate)
	{
		way = bsp3d_cells_measure.turn_off ? 1 : 0;
		bsp3d_cells_measure.way[way].frames++;
		bsp3d_cells_measure.way[way].micros += micros;
		bsp3d_cells_measure.way[way].game_micros += micros - ((long long)waited < micros ? (long long)waited : micros);
		bsp3d_cells_measure.turn_micros += micros;
		if (bsp3d_cells_measure.turn_micros >= 1000000)
		{
			bsp3d_cells_measure.turn_micros = 0;
			bsp3d_cells_measure.turn_off = !bsp3d_cells_measure.turn_off;
		}
	}
	bsp3d_cells_measure.log_micros += micros;
	if (bsp3d_cells_measure.log_micros < BSP3D_CELL_LOG_SECONDS * 1000000LL)
		return;
	bsp3d_cells_measure.log_micros = 0;
	if (mode == _bsp3d_cells_verify)
	{
		platform_log("bsp3d cells: verify: %llu queries compared, %llu differences",
			bsp3d_cells_measure.compared, bsp3d_cells_measure.differences);
	}
	else
	{
		double ticks_per_ns = bsp3d_cells_measure.frame_ticks_micros > 0 ?
			(double)bsp3d_cells_measure.frame_ticks / ((double)bsp3d_cells_measure.frame_ticks_micros * 1000.0) : 0;
		char line[2][160];

		for (way = 0; way < 2; way++)
		{
			double frames = (double)bsp3d_cells_measure.way[way].frames;
			double ns = ticks_per_ns > 0 ? (double)bsp3d_cells_measure.way[way].ticks / ticks_per_ns : 0;

			snprintf(line[way], sizeof(line[way]),
				"%llu calls, %.1f ns a call, %.3f ms a frame in the walk, game thread %.3f ms, %.1f fps",
				bsp3d_cells_measure.way[way].calls,
				bsp3d_cells_measure.way[way].calls ? ns / (double)bsp3d_cells_measure.way[way].calls : 0,
				frames > 0 ? ns / 1e6 / frames : 0,
				frames > 0 ? (double)bsp3d_cells_measure.way[way].game_micros / 1000.0 / frames : 0,
				bsp3d_cells_measure.way[way].micros > 0 ?
					frames * 1e6 / (double)bsp3d_cells_measure.way[way].micros : 0);
		}
		platform_log("bsp3d cells: alternate: with cells %s; without %s", line[0], line[1]);
	}
}

struct bsp3d_point_node const *bsp3d_packed_nodes(
	struct bsp3d const *bsp,
	long const **plane_designators)
{
	if (bsp == bsp3d_point_table.bsp && bsp3d_point_table.nodes && bsp3d_point_table.plane_designators &&
		bsp->nodes.address == bsp3d_point_table.nodes_address &&
		bsp->planes.address == bsp3d_point_table.planes_address &&
		bsp->nodes.count == bsp3d_point_table.node_count &&
		bsp->planes.count == bsp3d_point_table.plane_count)
	{
		*plane_designators = bsp3d_point_table.plane_designators;
		return bsp3d_point_table.nodes;
	}

	return NULL;
}

long bsp3d_test_point(
	struct bsp3d const *bsp,
	long node_index,
	union real_point3d const *point)
{
	short depth = 0;

	/* port: the current structure bsp's walk, on its table (above), from
	its point's cell's node when it starts at the root */
	if (bsp == bsp3d_point_table.bsp && bsp3d_point_table.nodes &&
		bsp->nodes.address == bsp3d_point_table.nodes_address &&
		bsp->planes.address == bsp3d_point_table.planes_address &&
		bsp->nodes.count == bsp3d_point_table.node_count &&
		bsp->planes.count == bsp3d_point_table.plane_count)
	{
		if (node_index != 0 || !bsp3d_point_table.cells)
			return bsp3d_point_walk(node_index, 0, point);
		switch (bsp3d_cells_measure.mode)
		{
		case _bsp3d_cells_verify:
		{
			long leaf_index = bsp3d_point_walk(0, 0, point);

			bsp3d_cells_measure.compared++;
			if (bsp3d_point_walk_from_cell(point) != leaf_index)
				bsp3d_cells_measure.differences++;
			return leaf_index;
		}
		case _bsp3d_cells_alternate:
		{
			short way = bsp3d_cells_measure.turn_off ? 1 : 0;
			unsigned long long started = bsp3d_cells_ticks();
			long leaf_index = way ? bsp3d_point_walk(0, 0, point) : bsp3d_point_walk_from_cell(point);

			bsp3d_cells_measure.way[way].ticks += bsp3d_cells_ticks() - started;
			bsp3d_cells_measure.way[way].calls++;
			return leaf_index;
		}
		default:
			return bsp3d_point_walk_from_cell(point);
		}
	}

	do
	{
		struct bsp3d_node const *node;
		real_plane3d const *plane;

		/* port: a node (from the map) that is no node, or deeper than
		MAXIMUM_BSP3D_TRAVERSAL_DEPTH, is solid */
		if (node_index < 0 ||
			node_index >= bsp->nodes.count ||
			depth++ >= MAXIMUM_BSP3D_TRAVERSAL_DEPTH)
		{
			if (!warned_about_bsp3d_nodes)
			{
				error(_error_silent, "a bsp3d node is not one of the bsp's, or is more than %d deep",
					MAXIMUM_BSP3D_TRAVERSAL_DEPTH);
				warned_about_bsp3d_nodes = TRUE;
			}
			return NONE;
		}
		node = TAG_BLOCK_GET_ELEMENT(
			&bsp->nodes,
			node_index,
			struct bsp3d_node);
		plane = TAG_BLOCK_GET_ELEMENT(
			&bsp->planes,
			node->plane_designator,
			real_plane3d);

		node_index = node->children[plane3d_distance_to_point(plane, point) >= 0.f];
	}
	while (!(node_index & LONG_MIN));

	if (node_index != NONE)
		return node_index & LONG_MAX;

	return NONE;
}

long bsp3d_clip_line_to_leaves(
	struct bsp3d const *bsp,
	long node_index,
	real_point3d const *p0,
	real_point3d const *p1,
	bsp3d_line_leaf_proc handler,
	void *user_data)
{
	real_point3d intersection;
	boolean p0_flags[2];
	boolean p1_flags[2];
	short child_index;

	long intersected_leaf_count = 0;
	struct bsp3d_node const *node = TAG_BLOCK_GET_ELEMENT(&bsp->nodes, node_index, struct bsp3d_node);
	real_plane3d const *plane = TAG_BLOCK_GET_ELEMENT(&bsp->planes, node->plane_designator, real_plane3d);
	real p0_d = bsp3d_plane_distance_to_point(plane, p0);
	real p1_d = bsp3d_plane_distance_to_point(plane, p1);

	if (!node_index)
	{
		node_count = 0;
	}

	++node_count;

	p0_flags[0] = p0_d < -CLIP_LINE_EPSILON;
	p0_flags[1] = p0_d > CLIP_LINE_EPSILON;
	p1_flags[0] = p1_d < -CLIP_LINE_EPSILON;
	p1_flags[1] = p1_d > CLIP_LINE_EPSILON;

	if (p0_flags[0] && p1_flags[1] || p0_flags[1] && p1_flags[0])
	{
		real_vector3d p0p1;
		real t;

		vector_from_points3d(p0, p1, &p0p1);
		t = vector_intersect_plane3d(p0, &p0p1, plane);

		match_assert(
			"c:\\halo\\SOURCE\\physics\\bsp3d.c",
			73,
			t>0.f && t<1.f);

		point_from_line3d(p0, &p0p1, t, &intersection);
	}

	for (child_index = 0; child_index < 2; child_index++)
	{
		if (p0_flags[child_index] || p1_flags[child_index] ||
			!p0_flags[child_index==0] && !p1_flags[child_index==0])
		{
			real_point3d const *c0 = p0_flags[child_index == 0] ? &intersection : p0;
			real_point3d const *c1 = p1_flags[child_index == 0] ? &intersection : p1;
			long child = node->children[child_index];

			if (child & LONG_MIN)
			{
				if (child != NONE)
				{
					if (handler)
					{
						handler(c0, c1, child & LONG_MAX, user_data);
					}

					++intersected_leaf_count;
				}
			}
			else
			{
				intersected_leaf_count += bsp3d_clip_line_to_leaves(bsp, child, c0, c1, handler, user_data);
			}
		}
	}

	return intersected_leaf_count;
}

long bsp3d_clip_polygon_to_leaves(
	struct bsp3d const *bsp,
	long node_index,
	real_point3d const *vertices,
	short vertex_count,
	real epsilon,
	bsp3d_polygon_leaf_proc proc,
	void *context)
{
	long result = bsp3d_clip_polygon_to_leaves_recursive(
		bsp,
		node_index,
		NONE,
		vertices,
		vertex_count,
		epsilon,
		proc,
		context);

	return result;
}

/* ---------- private code */

static long bsp3d_clip_polygon_to_leaves_recursive(
	struct bsp3d const *bsp,
	long node_index,
	long plane_designator,
	real_point3d const *vertices,
	short vertex_count,
	real epsilon,
	bsp3d_polygon_leaf_proc proc,
	void *context)
{
	real_point3d clipped_polygon_storage[2][MAXIMUM_VERTICES_PER_CLIPPED_POLYGON];
	real_point3d const *clipped_polygons[2];
	short clipped_counts[2];

	short vertex_index;
	short child_index;

	long intersected_leaf_count = 0;
	struct bsp3d_node const *node = TAG_BLOCK_GET_ELEMENT(
		&bsp->nodes,
		node_index,
		struct bsp3d_node);
	real_plane3d const *plane = TAG_BLOCK_GET_ELEMENT(
		&bsp->planes,
		node->plane_designator,
		real_plane3d);

	match_assert(
		"c:\\halo\\SOURCE\\physics\\bsp3d.c",
		149,
		vertex_count>=NUMBER_OF_VERTICES_PER_TRIANGLE);
	match_assert(
		"c:\\halo\\SOURCE\\physics\\bsp3d.c",
		151,
		vertex_count<MAXIMUM_VERTICES_PER_CLIPPED_POLYGON);

	for (vertex_index = 0; vertex_index < vertex_count; vertex_index++)
	{
		if (!(fabs(bsp3d_polygon_plane_distance_to_point(plane, &vertices[vertex_index])-0.0f) < epsilon))
		{
			break;
		}
	}

	if (vertex_index == vertex_count)
	{
		real_vector3d p0p2;
		real_vector3d p0p1;
		real_vector3d n;
		boolean facing;
		real_vector3d *edge2 = &p0p2;

		vector_from_points3d(&vertices[0], &vertices[1], &p0p1);
		vector_from_points3d(&vertices[0], &vertices[2], edge2);
		cross_product3d(edge2, &p0p1, &n);

		facing = dot_product3d(&n, &plane->n) > 0.0f;
		clipped_polygons[facing] = vertices;
		clipped_counts[facing] = vertex_count;
		clipped_counts[!facing] = 0;

		if (facing)
		{
			plane_designator = LONG_MIN | node_index & LONG_MAX;
		}
		else
		{
			plane_designator = node_index & LONG_MAX;
		}
	}
	else
	{
		real_plane3d back_plane;

		plane3d_negate(plane, &back_plane);
		clipped_counts[0] = convex_polygon3d_clip_to_plane(
			vertex_count,
			vertices,
			&back_plane,
			NUMBEROF(clipped_polygon_storage[0]),
			clipped_polygon_storage[0],
			NULL,
			epsilon,
			FALSE);
		clipped_counts[1] = convex_polygon3d_clip_to_plane(
			vertex_count,
			vertices,
			plane,
			NUMBEROF(clipped_polygon_storage[1]),
			clipped_polygon_storage[1],
			NULL,
			epsilon,
			FALSE);
		match_assert(
			"c:\\halo\\SOURCE\\physics\\bsp3d.c",
			185,
			clipped_counts[0]!=NONE && clipped_counts[1]!=NONE);

		clipped_polygons[0] = clipped_polygon_storage[0];
		clipped_polygons[1] = clipped_polygon_storage[1];
	}

	for (child_index = 0; child_index < NUMBEROF(clipped_counts); child_index++)
	{
		if (clipped_counts[child_index])
		{
			if (node->children[child_index] & LONG_MIN)
			{
				if (node->children[child_index] != NONE)
				{
					if (proc)
					{
						proc(
							clipped_polygons[child_index],
							clipped_counts[child_index],
							node->children[child_index] & LONG_MAX,
							plane_designator,
							context);
					}

					intersected_leaf_count++;
				}
			}
			else
			{
				intersected_leaf_count += bsp3d_clip_polygon_to_leaves_recursive(
					bsp,
					node->children[child_index],
					plane_designator,
					clipped_polygons[child_index],
					clipped_counts[child_index],
					epsilon,
					proc,
					context);
			}
		}
	}

	return intersected_leaf_count;
}
