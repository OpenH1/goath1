/*
XGPU_DYNAMIC_LIGHTS_WORLD.C

The enhanced dynamic lights' world (xgpu.h), as the hosts' renderers give it
to the GPU (xgpu_dynamic_lights_world.h). A pixel whose cell has a light
finds its cluster down the structure's 3D BSP, and the muzzle flash of the
player's own weapon puts nearly the whole screen in a cell with a light:
each pixel walked the game's BSP, up to 128 nodes deep, with two reads at
each node, the second (its plane) waiting on the first (the node).

- The walk's tree is smaller: a node whose two sides end in the same
  cluster, or both in none, is that cluster (or none) itself, for every
  point. Where the game's BSP parts empty space from solid (a leaf in no
  cluster, whose pixels keep every light) the node stays, so that every
  point ends where it ended.
- Each node's plane is beside it, so that a node's two reads do not wait on
  each other.
- The walk is skipped where it would take no light away (the shaders test
  the cell's lights against XGPU_DYNAMIC_LIGHTS_WALK, which
  xgpu_dynamic_lights_world_walked sets): a walk takes away only a light not
  in the pixel's cluster's mask, and only from a pixel within the light's
  sphere (the light reaches no other). Each cluster's leaves' boxes are
  found once, as the tree is made: a leaf's region is the convex polytope
  of the planes down to it, each a little moved out (more than the float
  error of the GPU's test against it), in a box the world's points are in
  (WORLD_REACH); its box holds that polytope. A light whose sphere (a little
  larger, for the walk's point off the surface) meets no box of a cluster
  whose mask lacks it cannot be taken from any pixel by its walk. Each
  cluster's boxes are kept in a tree of groups (at most six boxes to a
  group at the bottom, halved along the longest side of their box above),
  so that a light tests the boxes near it, not each of the cluster's: a
  group whose box the light's does not meet holds no box it meets. The
  light is walked for exactly as when each box was tested.

What a pixel draws is what it drew with the game's BSP walked.
*/

#include "xgpu_dynamic_lights_world.h"
#include "xgpu.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* the box the world's points are in, world units each way (a light's
sphere past it is walked) */
#define WORLD_REACH 65536.0
/* how much more than its radius a light's sphere is taken as: the walk's
point is XGPU_DYNAMIC_LIGHTS_CLUSTER_OFFSET off the pixel's (along a
normal of length 1), and the shaders' test of the distance is a float's */
#define SPHERE_SCALE 1.001f
#define SPHERE_MARGIN 0.05f

/* ---------- the tree */

struct convert_state
{
	const uint32_t (*in)[4];
	uint32_t in_count, node_count, plane_base, leaf_base, leaf_count;
	uint32_t (*out)[4];
	uint32_t out_nodes, room;
	/* for each of the game's nodes, what it was made into, the depth it was
	reached at, and whether the depth's limit cut it short; and a limit on
	the work (the game's BSP is a tree, each node reached once; a malformed
	one is not) */
	uint32_t *made;
	unsigned char *made_depth;
	unsigned char *made_cut;
	unsigned long work, work_limit;
	uint32_t deepest;
	int failed;
};

/* a leaf child (the top bit set): its cluster, or none */
static uint32_t convert_leaf(const struct convert_state *state, uint32_t child)
{
	uint32_t leaf = child & 0x7fffffffu, cluster;

	if (child == 0xffffffffu || leaf >= state->leaf_count)
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	cluster = state->in[state->leaf_base + leaf / 4][leaf % 4];
	return cluster < XGPU_DYNAMIC_LIGHTS_CLUSTERS ? XGPU_DYNAMIC_LIGHTS_WORLD_CLUSTER | cluster :
		XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
}

/* the game's node reached at depth (its walk's iteration) as the GPU's
child: a node made, a cluster or none; *cut set if the depth's limit ended
a walk below it. As the game's BSP was walked (xgpu.h): a node past the
count, or one whose plane is past the planes, or one reached at the
depth's limit, ends it in none. */
static uint32_t convert_node(struct convert_state *state, uint32_t node, uint32_t depth, int *cut)
{
	const uint32_t *entry;
	uint32_t sides[2], made;
	int side, below_cut = 0;

	if (depth >= XGPU_DYNAMIC_LIGHTS_WORLD_DEPTH)
	{
		*cut = 1;
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	}
	if (node >= state->node_count)
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	/* (what was made of it at the same depth, or higher where nothing was
	cut, is what it makes here) */
	if (state->made[node] != XGPU_DYNAMIC_LIGHTS_WORLD_NONE - 1 &&
		(state->made_depth[node] == depth || (!state->made_cut[node] && depth < state->made_depth[node])))
	{
		*cut |= state->made_cut[node];
		return state->made[node];
	}
	if (++state->work > state->work_limit)
	{
		state->failed = 1;
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	}
	entry = state->in[1 + node];
	if (depth + 1 > state->deepest)
		state->deepest = depth + 1;
	if (entry[0] >= state->leaf_base - state->plane_base)
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	for (side = 0; side < 2; side++)
	{
		uint32_t child = entry[1 + side];

		sides[side] = child & 0x80000000u ? convert_leaf(state, child) :
			convert_node(state, child, depth + 1, &below_cut);
		if (state->failed)
			return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	}
	if (sides[0] == sides[1])
	{
		/* both sides the same cluster, none, or node: that */
		made = sides[0];
	}
	else if (2 + 2 * state->out_nodes + 1 > state->room)
	{
		state->failed = 1;
		return XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	}
	else
	{
		made = state->out_nodes++;
		memcpy(state->out[1 + 2 * made], state->in[state->plane_base + entry[0]], 16);
		state->out[2 + 2 * made][0] = sides[0];
		state->out[2 + 2 * made][1] = sides[1];
		state->out[2 + 2 * made][2] = 0;
		state->out[2 + 2 * made][3] = 0;
	}
	state->made[node] = made;
	state->made_depth[node] = (unsigned char)depth;
	state->made_cut[node] = (unsigned char)below_cut;
	*cut |= below_cut;
	return made;
}

/* ---------- the clusters' boxes */

/* a convex polytope: its faces' vertices, face after face, and each face's
plane (outward: its points x have plane . x <= plane[3]) */
struct polytope
{
	int faces, vertices;
	int *face_size;
	double (*vertex)[3];
	double (*plane)[4];
};

static void polytope_free(struct polytope *polytope)
{
	free(polytope->face_size);
	free(polytope->vertex);
	free(polytope->plane);
	memset(polytope, 0, sizeof(*polytope));
}

static int polytope_box(struct polytope *polytope, double reach)
{
	static const int faces[6][4] =
	{
		{ 0, 2, 3, 1 }, { 4, 5, 7, 6 }, { 0, 1, 5, 4 }, { 2, 6, 7, 3 }, { 0, 4, 6, 2 }, { 1, 3, 7, 5 },
	};
	int face, corner;

	polytope->faces = 6;
	polytope->vertices = 24;
	polytope->face_size = malloc(6 * sizeof(int));
	polytope->vertex = malloc(24 * sizeof(*polytope->vertex));
	polytope->plane = malloc(6 * sizeof(*polytope->plane));
	if (!polytope->face_size || !polytope->vertex || !polytope->plane)
		return 0;
	for (face = 0; face < 6; face++)
	{
		/* (faces in pairs, -x and +x, -y and +y, -z and +z) */
		int axis = face / 2;

		polytope->face_size[face] = 4;
		polytope->plane[face][0] = polytope->plane[face][1] = polytope->plane[face][2] = 0.0;
		polytope->plane[face][axis] = face & 1 ? 1.0 : -1.0;
		polytope->plane[face][3] = reach;
		for (corner = 0; corner < 4; corner++)
		{
			int bits = faces[face][corner];
			double *v = polytope->vertex[face * 4 + corner];

			v[0] = bits & 4 ? reach : -reach;
			v[1] = bits & 2 ? reach : -reach;
			v[2] = bits & 1 ? reach : -reach;
		}
	}
	return 1;
}

static int angle_compare(const void *a, const void *b)
{
	double x = ((const double *)a)[3], y = ((const double *)b)[3];

	return x < y ? -1 : x > y;
}

/* the part of in where sign (n . x - d) >= -slack, into out (empty: no
faces); 0 if there was no memory */
static int polytope_clip(const struct polytope *in, const double n[3], double d, double sign, double slack,
	struct polytope *out)
{
	int face, first = 0, cap_count = 0, index;
	double (*cap)[4];
	double normal_length = sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), length;

	memset(out, 0, sizeof(*out));
	/* (at most each face's vertices and one more for each, and the cap) */
	out->face_size = malloc((size_t)(in->faces + 1) * sizeof(int));
	out->vertex = malloc((size_t)(in->vertices * 2 + in->faces * 2 + 8) * sizeof(*out->vertex));
	out->plane = malloc((size_t)(in->faces + 1) * sizeof(*out->plane));
	cap = malloc((size_t)(in->faces * 2 + 8) * sizeof(*cap));
	if (!out->face_size || !out->vertex || !out->plane || !cap)
	{
		free(cap);
		polytope_free(out);
		return 0;
	}
	for (face = 0; face < in->faces; face++)
	{
		int size = in->face_size[face], start = out->vertices, corner;

		for (corner = 0; corner < size; corner++)
		{
			const double *a = in->vertex[first + corner], *b = in->vertex[first + (corner + 1) % size];
			double da = sign * (n[0] * a[0] + n[1] * a[1] + n[2] * a[2] - d) + slack;
			double db = sign * (n[0] * b[0] + n[1] * b[1] + n[2] * b[2] - d) + slack;

			if (da >= 0.0)
			{
				memcpy(out->vertex[out->vertices++], a, sizeof(double[3]));
				if (da == 0.0)
					memcpy(cap[cap_count++], a, sizeof(double[3]));
			}
			if ((da >= 0.0) != (db >= 0.0) && da != 0.0 && db != 0.0)
			{
				double t = da / (da - db), *v = out->vertex[out->vertices++];

				v[0] = a[0] + (b[0] - a[0]) * t;
				v[1] = a[1] + (b[1] - a[1]) * t;
				v[2] = a[2] + (b[2] - a[2]) * t;
				memcpy(cap[cap_count++], v, sizeof(double[3]));
			}
		}
		first += size;
		if (out->vertices - start >= 3)
		{
			memcpy(out->plane[out->faces], in->plane[face], sizeof(out->plane[0]));
			out->face_size[out->faces++] = out->vertices - start;
		}
		else
		{
			out->vertices = start;
		}
	}
	/* the cap: the points on the plane, in order about their middle */
	if (cap_count >= 3 && out->faces)
	{
		double middle[3] = { 0.0, 0.0, 0.0 }, u[3], v[3], axis[3];
		int kept = 0;

		for (index = 0; index < cap_count; index++)
		{
			middle[0] += cap[index][0];
			middle[1] += cap[index][1];
			middle[2] += cap[index][2];
		}
		middle[0] /= cap_count;
		middle[1] /= cap_count;
		middle[2] /= cap_count;
		axis[0] = n[0] / normal_length;
		axis[1] = n[1] / normal_length;
		axis[2] = n[2] / normal_length;
		/* (u across the normal, v across both) */
		if (fabs(axis[0]) < 0.6)
		{
			u[0] = 0.0; u[1] = axis[2]; u[2] = -axis[1];
		}
		else
		{
			u[0] = -axis[2]; u[1] = 0.0; u[2] = axis[0];
		}
		length = sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
		u[0] /= length; u[1] /= length; u[2] /= length;
		v[0] = axis[1] * u[2] - axis[2] * u[1];
		v[1] = axis[2] * u[0] - axis[0] * u[2];
		v[2] = axis[0] * u[1] - axis[1] * u[0];
		for (index = 0; index < cap_count; index++)
		{
			double x = cap[index][0] - middle[0], y = cap[index][1] - middle[1], z = cap[index][2] - middle[2];

			cap[index][3] = atan2(x * v[0] + y * v[1] + z * v[2], x * u[0] + y * u[1] + z * u[2]);
		}
		qsort(cap, (size_t)cap_count, sizeof(*cap), angle_compare);
		for (index = 0; index < cap_count; index++)
		{
			double *at = out->vertex[out->vertices + kept];

			/* (each point once) */
			if (kept && fabs(cap[index][0] - at[-3]) + fabs(cap[index][1] - at[-2]) + fabs(cap[index][2] - at[-1]) <
				1.0e-9 * WORLD_REACH)
			{
				continue;
			}
			memcpy(at, cap[index], sizeof(double[3]));
			kept++;
		}
		if (kept >= 3)
		{
			/* (its plane outward: -sign n . x <= -sign d + slack) */
			out->plane[out->faces][0] = -sign * n[0] / normal_length;
			out->plane[out->faces][1] = -sign * n[1] / normal_length;
			out->plane[out->faces][2] = -sign * n[2] / normal_length;
			out->plane[out->faces][3] = (-sign * d + slack) / normal_length;
			out->vertices += kept;
			out->face_size[out->faces++] = kept;
		}
	}
	free(cap);
	return 1;
}

struct bounds_state
{
	const uint32_t (*out)[4];
	/* each leaf reached: its box and its cluster */
	struct xgpu_dynamic_lights_world_box *boxes;
	uint32_t *clusters;
	uint32_t count, room;
	/* and its faces' planes (the box's first and count say which) */
	float (*planes)[4];
	uint32_t plane_count, plane_room;
	/* (a node made of one reached from more than one, a malformed BSP's,
	is walked again from each: a limit on the work) */
	unsigned long work, work_limit;
	int failed;
};

static void bounds_leaf(struct bounds_state *state, const struct polytope *polytope, uint32_t child)
{
	struct xgpu_dynamic_lights_world_box *box;
	int index, axis;

	if (child == XGPU_DYNAMIC_LIGHTS_WORLD_NONE || !polytope->faces)
		return;
	if (state->count == state->room)
	{
		uint32_t room = state->room ? state->room * 2 : 1024;
		struct xgpu_dynamic_lights_world_box *boxes = realloc(state->boxes, room * sizeof(*boxes));
		uint32_t *clusters = boxes ? realloc(state->clusters, room * sizeof(*clusters)) : NULL;

		if (boxes)
			state->boxes = boxes;
		if (!boxes || !clusters)
		{
			state->failed = 1;
			return;
		}
		state->clusters = clusters;
		state->room = room;
	}
	if (state->plane_count + (uint32_t)polytope->faces > state->plane_room)
	{
		uint32_t room = (state->plane_room ? state->plane_room * 2 : 8192) + (uint32_t)polytope->faces;
		float (*planes)[4] = realloc(state->planes, room * sizeof(*planes));

		if (!planes)
		{
			state->failed = 1;
			return;
		}
		state->planes = planes;
		state->plane_room = room;
	}
	box = &state->boxes[state->count];
	box->plane_first = state->plane_count;
	box->plane_count = (uint32_t)polytope->faces;
	for (index = 0; index < polytope->faces; index++)
	{
		float *plane = state->planes[state->plane_count++];

		/* (in floats, its distance a little out: more than the rounding of
		its normal, at the reach) */
		plane[0] = (float)polytope->plane[index][0];
		plane[1] = (float)polytope->plane[index][1];
		plane[2] = (float)polytope->plane[index][2];
		plane[3] = (float)polytope->plane[index][3] + 0.02f + 1.0e-6f * fabsf((float)polytope->plane[index][3]);
	}
	for (axis = 0; axis < 3; axis++)
	{
		double lower = polytope->vertex[0][axis], upper = lower;

		for (index = 1; index < polytope->vertices; index++)
		{
			double value = polytope->vertex[index][axis];

			lower = value < lower ? value : lower;
			upper = value > upper ? value : upper;
		}
		/* (rounded out to floats) */
		box->lower[axis] = (float)lower;
		box->upper[axis] = (float)upper;
		if ((double)box->lower[axis] > lower)
			box->lower[axis] = nextafterf(box->lower[axis], -INFINITY);
		if ((double)box->upper[axis] < upper)
			box->upper[axis] = nextafterf(box->upper[axis], INFINITY);
	}
	state->clusters[state->count++] = child & 0x7fffffffu;
}

/* the leaves below the GPU's node, whose region is polytope */
static void bounds_node(struct bounds_state *state, const struct polytope *polytope, uint32_t node)
{
	const float *plane = (const float *)state->out[1 + 2 * node];
	double n[3], d, slack;
	int side;

	if (++state->work > state->work_limit)
	{
		state->failed = 1;
		return;
	}
	n[0] = plane[0];
	n[1] = plane[1];
	n[2] = plane[2];
	d = plane[3];
	/* (more than the float error of the GPU's n . q - d, with q in the
	reach: each product and sum to within a few of a float's units) */
	slack = 1.0e-6 * ((fabs(n[0]) + fabs(n[1]) + fabs(n[2])) * WORLD_REACH + fabs(d)) + 1.0e-4;
	for (side = 0; side < 2 && !state->failed; side++)
	{
		uint32_t child = state->out[2 + 2 * node][side];
		struct polytope part;

		/* (behind: n . q - d < 0; in front: >= 0) */
		if (!polytope_clip(polytope, n, d, side ? 1.0 : -1.0, slack, &part))
		{
			state->failed = 1;
			return;
		}
		if (part.faces)
		{
			if (child & XGPU_DYNAMIC_LIGHTS_WORLD_CLUSTER)
				bounds_leaf(state, &part, child);
			else
				bounds_node(state, &part, child);
		}
		polytope_free(&part);
	}
}

static void bounds_make(struct xgpu_dynamic_lights_world *world)
{
	struct bounds_state state;
	struct polytope box;
	uint32_t index, cluster;

	world->bounded = 0;
	if (world->count < 2 || !world->first || !world->box)
		return;
	memset(&state, 0, sizeof(state));
	state.out = (const uint32_t (*)[4])world->texels;
	state.work_limit = 4ul * world->nodes + 1024;
	if (!polytope_box(&box, WORLD_REACH))
	{
		polytope_free(&box);
		return;
	}
	bounds_node(&state, &box, world->texels[0][1]);
	polytope_free(&box);
	if (state.failed)
	{
		free(state.boxes);
		free(state.clusters);
		free(state.planes);
		return;
	}
	/* the leaves' boxes in their clusters' order, and each cluster's box */
	free(world->boxes);
	world->boxes = malloc((state.count ? state.count : 1) * sizeof(*world->boxes));
	if (!world->boxes)
	{
		free(state.boxes);
		free(state.clusters);
		free(state.planes);
		return;
	}
	memset(world->first, 0, (XGPU_DYNAMIC_LIGHTS_CLUSTERS + 1) * sizeof(*world->first));
	for (index = 0; index < state.count; index++)
		world->first[state.clusters[index] + 1]++;
	for (cluster = 0; cluster < XGPU_DYNAMIC_LIGHTS_CLUSTERS; cluster++)
		world->first[cluster + 1] += world->first[cluster];
	{
		uint32_t at[XGPU_DYNAMIC_LIGHTS_CLUSTERS];

		memcpy(at, world->first, sizeof(at));
		for (index = 0; index < state.count; index++)
			world->boxes[at[state.clusters[index]]++] = state.boxes[index];
	}
	for (cluster = 0; cluster < XGPU_DYNAMIC_LIGHTS_CLUSTERS; cluster++)
	{
		struct xgpu_dynamic_lights_world_box *all = &world->box[cluster];
		int axis;

		for (axis = 0; axis < 3; axis++)
		{
			all->lower[axis] = INFINITY;
			all->upper[axis] = -INFINITY;
		}
		for (index = world->first[cluster]; index < world->first[cluster + 1]; index++)
		{
			for (axis = 0; axis < 3; axis++)
			{
				all->lower[axis] = fminf(all->lower[axis], world->boxes[index].lower[axis]);
				all->upper[axis] = fmaxf(all->upper[axis], world->boxes[index].upper[axis]);
			}
		}
	}
	free(state.boxes);
	free(state.clusters);
	free(world->planes);
	world->planes = state.planes;
	world->bounded = 1;
}

/* ---------- the clusters' boxes' groups */

/* (a group of more than this many boxes is halved) */
#define GROUP_BOXES 6

static void groups_free(struct xgpu_dynamic_lights_world *world)
{
	free(world->groups);
	free(world->root);
	world->groups = NULL;
	world->root = NULL;
	world->group_count = world->group_bytes = 0;
}

/* (boxes by their middles along an axis) */
#define BOX_ORDER(axis) \
	static int box_order_##axis(const void *a, const void *b) \
	{ \
		const struct xgpu_dynamic_lights_world_box *x = a, *y = b; \
		float p = x->lower[axis] + x->upper[axis], q = y->lower[axis] + y->upper[axis]; \
		\
		return (p > q) - (p < q); \
	}
BOX_ORDER(0)
BOX_ORDER(1)
BOX_ORDER(2)

/* the group of boxes[begin] to boxes[end - 1], and its groups after it
(the boxes put in their order) */
static uint32_t group_make(struct xgpu_dynamic_lights_world *world, uint32_t begin, uint32_t end)
{
	static int (*const order[3])(const void *, const void *) = { box_order_0, box_order_1, box_order_2 };
	uint32_t made = world->group_count++, index;
	struct xgpu_dynamic_lights_world_group *group = &world->groups[made];
	int axis, longest = 0;

	for (axis = 0; axis < 3; axis++)
	{
		group->lower[axis] = INFINITY;
		group->upper[axis] = -INFINITY;
	}
	for (index = begin; index < end; index++)
	{
		const struct xgpu_dynamic_lights_world_box *box = &world->boxes[index];

		for (axis = 0; axis < 3; axis++)
		{
			group->lower[axis] = fminf(group->lower[axis], box->lower[axis]);
			group->upper[axis] = fmaxf(group->upper[axis], box->upper[axis]);
		}
	}
	group->first = begin;
	group->count = end - begin;
	if (end - begin > GROUP_BOXES)
	{
		for (axis = 1; axis < 3; axis++)
		{
			if (group->upper[axis] - group->lower[axis] > group->upper[longest] - group->lower[longest])
				longest = axis;
		}
		qsort(&world->boxes[begin], end - begin, sizeof(world->boxes[0]), order[longest]);
		group->count = 0;
		group_make(world, begin, begin + (end - begin) / 2);
		group_make(world, begin + (end - begin) / 2, end);
	}
	group->past = world->group_count;
	return made;
}

int xgpu_dynamic_lights_world_group(struct xgpu_dynamic_lights_world *world)
{
	uint32_t boxes, cluster;

	groups_free(world);
	if (!world->bounded)
		return 0;
	boxes = world->first[XGPU_DYNAMIC_LIGHTS_CLUSTERS];
	/* (fewer groups than two for each box: each at the bottom has one or
	more, each above two groups) */
	world->groups = malloc(((size_t)boxes * 2 + 1) * sizeof(*world->groups));
	world->root = malloc(XGPU_DYNAMIC_LIGHTS_CLUSTERS * sizeof(*world->root));
	if (!world->groups || !world->root)
	{
		groups_free(world);
		return 0;
	}
	for (cluster = 0; cluster < XGPU_DYNAMIC_LIGHTS_CLUSTERS; cluster++)
	{
		world->root[cluster] = world->first[cluster] < world->first[cluster + 1] ?
			group_make(world, world->first[cluster], world->first[cluster + 1]) : XGPU_DYNAMIC_LIGHTS_WORLD_NONE;
	}
	world->group_bytes = (uint32_t)(world->group_count * sizeof(*world->groups) +
		XGPU_DYNAMIC_LIGHTS_CLUSTERS * sizeof(*world->root));
	return 1;
}

/* the deepest walk of the GPU's tree, in nodes (each node's children were
made before it) */
static uint32_t tree_depth(const uint32_t (*texels)[4], uint32_t nodes)
{
	unsigned char *depth = malloc(nodes ? nodes : 1);
	uint32_t node, deepest = 0;
	int side;

	if (!depth)
		return 0;
	for (node = 0; node < nodes; node++)
	{
		depth[node] = 1;
		for (side = 0; side < 2; side++)
		{
			uint32_t child = texels[2 + 2 * node][side];

			if (!(child & XGPU_DYNAMIC_LIGHTS_WORLD_CLUSTER) && child < node && depth[child] + 1 > depth[node])
				depth[node] = (unsigned char)(depth[child] + 1);
		}
		deepest = depth[node] > deepest ? depth[node] : deepest;
	}
	free(depth);
	return deepest;
}

static void job_abandon(struct xgpu_dynamic_lights_world *world);

/* (convert's and convert_later's) the GPU's tree, its boxes not yet found */
static int convert_tree(struct xgpu_dynamic_lights_world *world, const uint32_t (*texels)[4], uint32_t count)
{
	struct convert_state state;
	uint32_t root, index;
	int cut = 0;

	world->bounded = 0;
	groups_free(world);
	if (!world->texels)
		world->texels = malloc(XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS * sizeof(*world->texels));
	if (!world->first)
		world->first = malloc((XGPU_DYNAMIC_LIGHTS_CLUSTERS + 1) * sizeof(*world->first));
	if (!world->box)
		world->box = malloc(XGPU_DYNAMIC_LIGHTS_CLUSTERS * sizeof(*world->box));
	if (!world->texels)
	{
		world->count = 0;
		return 0;
	}
	memset(world->texels[0], 0, sizeof(world->texels[0]));
	world->count = 1;
	world->game_nodes = world->game_depth = world->nodes = world->depth = 0;
	if (!texels || count < 1 || count > XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS)
		return 0;
	/* (no nodes: no world) */
	if (!texels[0][0])
		return 1;
	memset(&state, 0, sizeof(state));
	state.in = texels;
	state.in_count = count;
	state.node_count = texels[0][0];
	state.plane_base = texels[0][1];
	state.leaf_base = texels[0][2];
	state.leaf_count = texels[0][3];
	/* (the game's layout: dynamic_lights.c's, each part within the count) */
	if (state.node_count > count - 1 || state.plane_base < 1 + state.node_count || state.leaf_base < state.plane_base ||
		state.leaf_base > count || state.leaf_count > (count - state.leaf_base) * 4)
	{
		return 0;
	}
	state.out = world->texels;
	state.room = XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS;
	state.made = malloc(state.node_count * sizeof(*state.made));
	state.made_depth = malloc(state.node_count);
	state.made_cut = malloc(state.node_count);
	state.work_limit = 4ul * state.node_count + 1024;
	if (!state.made || !state.made_depth || !state.made_cut)
	{
		free(state.made);
		free(state.made_depth);
		free(state.made_cut);
		return 0;
	}
	for (index = 0; index < state.node_count; index++)
		state.made[index] = XGPU_DYNAMIC_LIGHTS_WORLD_NONE - 1;
	root = convert_node(&state, 0, 0, &cut);
	free(state.made);
	free(state.made_depth);
	free(state.made_cut);
	if (state.failed)
		return 0;
	world->game_nodes = state.node_count;
	world->game_depth = state.deepest;
	if (root == XGPU_DYNAMIC_LIGHTS_WORLD_NONE)
	{
		/* (every point in none: every light, as with no world) */
		return 1;
	}
	if (root & XGPU_DYNAMIC_LIGHTS_WORLD_CLUSTER)
	{
		/* (every point in one cluster: a node both of whose sides are it) */
		memset(world->texels[1], 0, sizeof(world->texels[1]));
		world->texels[2][0] = world->texels[2][1] = root;
		world->texels[2][2] = world->texels[2][3] = 0;
		root = 0;
		state.out_nodes = 1;
	}
	world->texels[0][0] = state.out_nodes;
	world->texels[0][1] = root;
	world->count = 1 + 2 * state.out_nodes;
	world->nodes = state.out_nodes;
	world->depth = tree_depth((const uint32_t (*)[4])world->texels, state.out_nodes);
	return 2;
}

int xgpu_dynamic_lights_world_convert(struct xgpu_dynamic_lights_world *world, const uint32_t (*texels)[4],
	uint32_t count)
{
	int made;

	job_abandon(world);
	made = convert_tree(world, texels, count);
	if (made == 2)
		bounds_make(world);
	return made != 0;
}

/* ---------- the boxes on a thread of their own (convert_later) */

/* a world's boxes and groups being made: a world of their own (its tree a
copy of the one it was made for), and whether it is done, or no longer
wanted (its thread then frees it) */
struct xgpu_dynamic_lights_world_job
{
	struct xgpu_dynamic_lights_world made;
	int done, abandoned;
};

static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;

static void job_free(struct xgpu_dynamic_lights_world_job *job)
{
	xgpu_dynamic_lights_world_free(&job->made);
	free(job);
}

static void job_make(struct xgpu_dynamic_lights_world_job *job)
{
	bounds_make(&job->made);
	xgpu_dynamic_lights_world_group(&job->made);
}

static void *job_thread(void *argument)
{
	struct xgpu_dynamic_lights_world_job *job = argument;
	int abandoned;

	job_make(job);
	pthread_mutex_lock(&job_lock);
	job->done = 1;
	abandoned = job->abandoned;
	pthread_mutex_unlock(&job_lock);
	if (abandoned)
		job_free(job);
	return NULL;
}

/* the world's job no longer wanted (a new world, or the world freed) */
static void job_abandon(struct xgpu_dynamic_lights_world *world)
{
	struct xgpu_dynamic_lights_world_job *job = world->pending;
	int done;

	if (!job)
		return;
	world->pending = NULL;
	pthread_mutex_lock(&job_lock);
	done = job->done;
	job->abandoned = 1;
	pthread_mutex_unlock(&job_lock);
	if (done)
		job_free(job);
}

int xgpu_dynamic_lights_world_convert_later(struct xgpu_dynamic_lights_world *world, const uint32_t (*texels)[4],
	uint32_t count)
{
	struct xgpu_dynamic_lights_world_job *job;
	pthread_attr_t attributes;
	pthread_t thread;
	int made, started = 0;

	job_abandon(world);
	made = convert_tree(world, texels, count);
	if (made != 2)
		return made != 0;
	/* (the job's tree and the room its boxes are counted in) */
	job = calloc(1, sizeof(*job));
	if (job)
	{
		job->made.texels = malloc((size_t)world->count * sizeof(*world->texels));
		job->made.first = malloc((XGPU_DYNAMIC_LIGHTS_CLUSTERS + 1) * sizeof(*job->made.first));
		job->made.box = malloc(XGPU_DYNAMIC_LIGHTS_CLUSTERS * sizeof(*job->made.box));
	}
	if (!job || !job->made.texels || !job->made.first || !job->made.box)
	{
		/* (no memory for it: made here, as convert makes it) */
		if (job)
			job_free(job);
		bounds_make(world);
		return 1;
	}
	memcpy(job->made.texels, world->texels, (size_t)world->count * sizeof(*world->texels));
	job->made.count = world->count;
	job->made.nodes = world->nodes;
	job->made.depth = world->depth;
	if (pthread_attr_init(&attributes) == 0)
	{
		pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
		started = pthread_create(&thread, &attributes, job_thread, job) == 0;
		pthread_attr_destroy(&attributes);
	}
	if (!started)
	{
		/* (no thread: made here) */
		job_make(job);
		job->done = 1;
	}
	world->pending = job;
	return 1;
}

int xgpu_dynamic_lights_world_take(struct xgpu_dynamic_lights_world *world)
{
	struct xgpu_dynamic_lights_world_job *job = world->pending;
	struct xgpu_dynamic_lights_world *made;
	int done;

	if (!job)
		return 0;
	pthread_mutex_lock(&job_lock);
	done = job->done;
	pthread_mutex_unlock(&job_lock);
	if (!done)
		return 0;
	world->pending = NULL;
	made = &job->made;
	groups_free(world);
	free(world->boxes);
	free(world->planes);
	free(world->first);
	free(world->box);
	world->boxes = made->boxes;
	world->planes = made->planes;
	world->first = made->first;
	world->box = made->box;
	world->groups = made->groups;
	world->root = made->root;
	world->group_count = made->group_count;
	world->group_bytes = made->group_bytes;
	world->bounded = made->bounded;
	made->boxes = NULL;
	made->planes = NULL;
	made->first = NULL;
	made->box = NULL;
	made->groups = NULL;
	made->root = NULL;
	job_free(job);
	return 1;
}

/* ---------- the frame's lights */

typedef char walk_texel[XGPU_DYNAMIC_LIGHTS_WORLD_WALK_TEXEL == XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y *
	XGPU_DYNAMIC_LIGHTS_GRID_Z + XGPU_DYNAMIC_LIGHTS_WALK ? 1 : -1];

/* whether the light's sphere (at position, of radius, in the box from
lower to upper) meets the leaf's box: unless their boxes do not meet, or
it is wholly past one of the leaf's faces */
static inline int leaf_meets(const struct xgpu_dynamic_lights_world *world,
	const struct xgpu_dynamic_lights_world_box *box, const float *position, float radius, const float lower[3],
	const float upper[3])
{
	uint32_t face;
	int meets = 1;

	if (box->lower[0] > upper[0] || box->upper[0] < lower[0] || box->lower[1] > upper[1] ||
		box->upper[1] < lower[1] || box->lower[2] > upper[2] || box->upper[2] < lower[2])
	{
		return 0;
	}
	for (face = 0; face < box->plane_count && meets; face++)
	{
		const float *plane = world->planes[box->plane_first + face];

		meets = plane[0] * position[0] + plane[1] * position[1] + plane[2] * position[2] - plane[3] <= radius;
	}
	return meets;
}

/* (xgpu_dynamic_lights_world_walked, with the boxes' groups or without) */
static void walked(const struct xgpu_dynamic_lights_world *world, const float (*block)[4], uint32_t count,
	const void *cells, uint32_t walk[8], int grouped)
{
	const uint32_t (*cluster_masks)[4] = (const uint32_t (*)[4])cells +
		XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y * XGPU_DYNAMIC_LIGHTS_GRID_Z;
	const float (*position)[4] = block + 3;
	const struct xgpu_dynamic_lights_world_group *groups = grouped ? world->groups : NULL;
	uint32_t light, cluster, index;

	memset(walk, 0, 8 * sizeof(walk[0]));
	/* (the frame's: past them the block's are an earlier frame's, in no
	cell) */
	if (block[0][3] >= 0.0f && block[0][3] < (float)count)
		count = (uint32_t)block[0][3];
	if (count > XGPU_DYNAMIC_LIGHTS_MAXIMUM)
		count = XGPU_DYNAMIC_LIGHTS_MAXIMUM;
	for (light = 0; light < count; light++)
	{
		uint32_t word = light >> 5, bit = 1u << (light & 31);
		float w = position[light][3], radius, lower[3], upper[3];
		int axis, needed = 0;

		if (!world->bounded)
			needed = 1;
		radius = w != 0.0f ? SPHERE_SCALE / fabsf(w) + SPHERE_MARGIN : INFINITY;
		for (axis = 0; axis < 3; axis++)
		{
			lower[axis] = position[light][axis] - radius;
			upper[axis] = position[light][axis] + radius;
			/* (past the reach, or not a number: walked) */
			if (!(lower[axis] > (float)-WORLD_REACH && upper[axis] < (float)WORLD_REACH))
				needed = 1;
		}
		for (cluster = 0; cluster < XGPU_DYNAMIC_LIGHTS_CLUSTERS && !needed; cluster++)
		{
			const struct xgpu_dynamic_lights_world_box *box = &world->box[cluster];

			if (cluster_masks[cluster][word] & bit || world->first[cluster] == world->first[cluster + 1] ||
				box->lower[0] > upper[0] || box->upper[0] < lower[0] || box->lower[1] > upper[1] ||
				box->upper[1] < lower[1] || box->lower[2] > upper[2] || box->upper[2] < lower[2])
			{
				continue;
			}
			if (groups)
			{
				/* (a group whose box the light's does not meet holds no box
				it meets: on past its groups) */
				uint32_t at = world->root[cluster], past = groups[at].past;

				while (at < past && !needed)
				{
					const struct xgpu_dynamic_lights_world_group *group = &groups[at];

					if (group->lower[0] > upper[0] || group->upper[0] < lower[0] || group->lower[1] > upper[1] ||
						group->upper[1] < lower[1] || group->lower[2] > upper[2] || group->upper[2] < lower[2])
					{
						at = group->past;
						continue;
					}
					for (index = group->first; index < group->first + group->count && !needed; index++)
						needed = leaf_meets(world, &world->boxes[index], position[light], radius, lower, upper);
					at++;
				}
				continue;
			}
			for (index = world->first[cluster]; index < world->first[cluster + 1] && !needed; index++)
				needed = leaf_meets(world, &world->boxes[index], position[light], radius, lower, upper);
		}
		/* (the structure's pixels pass over a light for the models alone:
		its walk takes nothing from them) */
		if (needed)
		{
			walk[word] |= bit;
			if (!(w < 0.0f))
				walk[4 + word] |= bit;
		}
	}
}

void xgpu_dynamic_lights_world_walked(const struct xgpu_dynamic_lights_world *world, const float (*block)[4],
	uint32_t count, const void *cells, uint32_t walk[8])
{
	walked(world, block, count, cells, walk, 1);
}

void xgpu_dynamic_lights_world_walked_scan(const struct xgpu_dynamic_lights_world *world, const float (*block)[4],
	uint32_t count, const void *cells, uint32_t walk[8])
{
	walked(world, block, count, cells, walk, 0);
}

void xgpu_dynamic_lights_world_free(struct xgpu_dynamic_lights_world *world)
{
	job_abandon(world);
	groups_free(world);
	free(world->texels);
	free(world->boxes);
	free(world->planes);
	free(world->first);
	free(world->box);
	memset(world, 0, sizeof(*world));
}
