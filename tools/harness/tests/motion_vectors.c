/* display.temporal_aa's motion (port/linux/game/motion_vectors.c,
test_motion_vectors.py): the whole file against a small fake renderer, and
the vertex shader's arithmetic (xgpu_spirv.c's motion: the program run on
this frame's registers and on the frame before's) done here on what it
hands the front end. A point's motion is where it was drawn the frame
before less where it is now: the camera's, a rigid object's, a skinned one's
(two nodes, and a palette's), the first-person weapon's own projection; a
teleport and a camera cut have none of their own. */

#include "harness.h"

#include <math.h>

typedef struct { short x0, y0, x1, y1; } rectangle2d;
typedef struct real_matrix4x3
{
	real scale;
	union
	{
		real n[4][3];
		struct
		{
			real_vector3d forward;
			real_vector3d left;
			real_vector3d up;
			real_point3d position;
		};
	};
} real_matrix4x3;

struct render_camera
{
	real_point3d position;
	real_vector3d forward;
	real vertical_field_of_view;
	rectangle2d viewport_bounds;
	real z_near, z_far;
};

struct render_frustum
{
	real_matrix4x3 world_to_view;
	boolean projection_valid;
	real projection_matrix[4][4];
};

/* config.toml's display.temporal_aa */
static const char *technique = "fsr";

const char *config_string(const char *name)
{
	return !strcmp(name, "display.temporal_aa") ? technique : NULL;
}

unsigned long halo_gpu_settings_taken(void)
{
	return 0;
}

/* the renderer has NVIDIA DLSS (its module, an NVIDIA GPU) */
static int dlss_drawn;

int halo_temporal_dlss_drawn(void)
{
	return dlss_drawn;
}

/* (and Metal's MetalFX: none here) */
int halo_temporal_metalfx_drawn(void)
{
	return 0;
}

/* (lt_lights.c's: the light-transport pass off, so the previous frame's
registers are kept only with a technique on) */
boolean lt_lights_sent(void)
{
	return FALSE;
}

/* the front end, as the backend would have it: this frame's registers and
the frame before's (each this frame's but where set apart) */
static float registers[192][4], previous[192][4];
static int window_on, resolved, resolved_history, previous_sets;
static float resolved_rows[4][4];

int halo_motion_window(int on, float jitter_x, float jitter_y)
{
	CHECK(!on || (fabsf(jitter_x) < 0.5f && fabsf(jitter_y) < 0.5f), "jitter %f %f", jitter_x, jitter_y);
	window_on = on;
	return on;
}

/* (the jitter's phases at a render scale of 0.67) */
int halo_motion_jitter_phases(void)
{
	return 17;
}

void halo_motion_previous_constants(int reg, float const (*values)[4], unsigned long count)
{
	memcpy(previous[reg + 96], values, count * sizeof(values[0]));
	previous_sets++;
}

void halo_motion_objects(int begin)
{
	(void)begin;
}

void halo_motion_opaque(short x0, short y0, short x1, short y1, int window)
{
	(void)x0; (void)y0; (void)x1; (void)y1; (void)window;
}

void halo_motion_resolve(short x0, short y0, short x1, short y1, float const reproject[4][4], int history,
	int window, float const camera[3])
{
	(void)x0; (void)y0; (void)x1; (void)y1; (void)window; (void)camera;
	memcpy(resolved_rows, reproject, sizeof(resolved_rows));
	resolved_history = history;
	resolved++;
}

#include "motion_vectors.h"
#include "under_test.inc"

/* ---------- the fake renderer */

/* (the game's: the shader's registers set, as the game sets them: this
frame's into both, then the frame before's apart) */
static void set_registers(int reg, float const (*values)[4], int count)
{
	memcpy(registers[reg + 96], values, (size_t)count * sizeof(values[0]));
	memcpy(previous[reg + 96], values, (size_t)count * sizeof(values[0]));
}

/* a camera at position looking along yaw (radians), its frustum's view
(rows forward, left, up, position: view = x n[0] + y n[1] + z n[2] + n[3])
and a projection of this focal length (clip x = -left, y = up, w = forward,
depth (near 1, far 100) */
static void camera_make(float x, float y, float z, float yaw, float focal, struct render_camera *camera,
	struct render_frustum *frustum)
{
	float forward[3] = { cosf(yaw), sinf(yaw), 0.0f }, left[3] = { -sinf(yaw), cosf(yaw), 0.0f };
	float up[3] = { 0.0f, 0.0f, 1.0f }, position[3] = { x, y, z };
	int row;

	memset(camera, 0, sizeof(*camera));
	memset(frustum, 0, sizeof(*frustum));
	camera->position.x = x;
	camera->position.y = y;
	camera->position.z = z;
	camera->forward.i = forward[0];
	camera->forward.j = forward[1];
	camera->viewport_bounds.x1 = 640;
	camera->viewport_bounds.y1 = 480;
	frustum->world_to_view.scale = 1.0f;
	for (row = 0; row < 3; row++)
	{
		frustum->world_to_view.n[row][0] = forward[row];
		frustum->world_to_view.n[row][1] = left[row];
		frustum->world_to_view.n[row][2] = up[row];
	}
	frustum->world_to_view.n[3][0] = -(forward[0] * position[0] + forward[1] * position[1] + forward[2] * position[2]);
	frustum->world_to_view.n[3][1] = -(left[0] * position[0] + left[1] * position[1] + left[2] * position[2]);
	frustum->world_to_view.n[3][2] = -(up[0] * position[0] + up[1] * position[1] + up[2] * position[2]);
	frustum->projection_valid = TRUE;
	frustum->projection_matrix[1][0] = -focal;
	frustum->projection_matrix[2][1] = focal * 4.0f / 3.0f;
	frustum->projection_matrix[0][2] = 100.0f / 99.0f;
	frustum->projection_matrix[3][2] = -100.0f / 99.0f;
	frustum->projection_matrix[0][3] = 1.0f;
}

/* rasterizer_set_frustum_z's registers of a frustum */
static void frustum_registers(struct render_frustum const *frustum, float out[4][4])
{
	int column, row;

	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			const real *n = frustum->world_to_view.n[row];

			out[column][row] = n[0] * frustum->projection_matrix[0][column] +
				n[1] * frustum->projection_matrix[1][column] + n[2] * frustum->projection_matrix[2][column];
		}
		out[column][3] += frustum->projection_matrix[3][column];
	}
}

/* a node: turned by yaw about z, at position */
static void node_make(real_matrix4x3 *node, float yaw, float x, float y, float z)
{
	memset(node, 0, sizeof(*node));
	node->scale = 1.0f;
	node->forward.i = cosf(yaw);
	node->forward.j = sinf(yaw);
	node->left.i = -sinf(yaw);
	node->left.j = cosf(yaw);
	node->up.k = 1.0f;
	node->position.x = x;
	node->position.y = y;
	node->position.z = z;
}

static void node_point(real_matrix4x3 const *node, float const p[3], float out[3])
{
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		out[axis] = node->n[0][axis] * p[0] * node->scale + node->n[1][axis] * p[1] * node->scale +
			node->n[2][axis] * p[2] * node->scale + node->n[3][axis];
	}
}

/* the vertex shader: a vertex of nodes' weights, its world point by the
node registers (c[-36] on) of a bank, its clip position by the projection's
(c[-96] on) of the same bank; its normalized x, y */
static void vertex_ndc(float (*bank)[4], const short *nodes, const float *weights, int count, float const p[3],
	float ndc[2])
{
	float world[3] = { 0.0f, 0.0f, 0.0f }, clip[4];
	int index, axis, component;

	for (index = 0; index < count; index++)
	{
		float (*node)[4] = &bank[60 + nodes[index] * 3];

		for (axis = 0; axis < 3; axis++)
			world[axis] += weights[index] * (node[axis][0] * p[0] + node[axis][1] * p[1] + node[axis][2] * p[2] +
				node[axis][3]);
	}
	for (component = 0; component < 4; component++)
	{
		float *row = bank[component];

		clip[component] = row[0] * world[0] + row[1] * world[1] + row[2] * world[2] + row[3];
	}
	ndc[0] = clip[0] / clip[3];
	ndc[1] = clip[1] / clip[3];
}

/* a world point as a frustum's projection draws it */
static void world_ndc(struct render_frustum const *frustum, float const world[3], float ndc[2], float *depth)
{
	float rows[4][4], clip[4];
	int component;

	frustum_registers(frustum, rows);
	for (component = 0; component < 4; component++)
		clip[component] = rows[component][0] * world[0] + rows[component][1] * world[1] +
			rows[component][2] * world[2] + rows[component][3];
	ndc[0] = clip[0] / clip[3];
	ndc[1] = clip[1] / clip[3];
	if (depth)
		*depth = clip[2] / clip[3];
}

#define NEAR(a, b) (fabsf((a) - (b)) < 1.0e-3f)

/* one window's frame: its camera, the projection set, the objects drawn */
struct object
{
	long id;
	short kind;
	int count;
	real_matrix4x3 nodes[2];
	const unsigned char *palette;
};

static void frame(struct render_camera *camera, struct render_frustum *frustum, struct object *objects, int count,
	struct render_frustum const *weapon_frustum)
{
	float rows[4][4];
	int index;

	previous_sets = 0;
	motion_vectors_frame_begin();
	CHECK(motion_vectors_window_begin(0, TRUE, camera, frustum), "the window draws no motion");
	frustum_registers(weapon_frustum ? weapon_frustum : frustum, rows);
	set_registers(-96, (float const (*)[4])rows, 4);
	motion_vectors_projection((real const (*)[4])rows);
	for (index = 0; index < count; index++)
	{
		struct object *object = &objects[index];
		float node_rows[6][4];
		int node;

		for (node = 0; node < object->count; node++)
		{
			real_matrix4x3 const *given = &object->nodes[object->palette ? object->palette[node] : node];

			node_rows[node * 3][0] = given->forward.i; node_rows[node * 3][1] = given->left.i;
			node_rows[node * 3][2] = given->up.i; node_rows[node * 3][3] = given->position.x;
			node_rows[node * 3 + 1][0] = given->forward.j; node_rows[node * 3 + 1][1] = given->left.j;
			node_rows[node * 3 + 1][2] = given->up.j; node_rows[node * 3 + 1][3] = given->position.y;
			node_rows[node * 3 + 2][0] = given->forward.k; node_rows[node * 3 + 2][1] = given->left.k;
			node_rows[node * 3 + 2][2] = given->up.k; node_rows[node * 3 + 2][3] = given->position.z;
		}
		motion_vectors_object(object->id, object->kind, object->nodes);
		/* (render_model: its relative nodes, here the same) */
		motion_vectors_model_relative(object->nodes, object->nodes);
		motion_vectors_model_nodes(object->nodes, (short)object->count);
		set_registers(-36, (float const (*)[4])node_rows, object->count * 3);
		motion_vectors_skinning(object->nodes, object->palette, (short)object->count, (short)(object->count - 1));
	}
	{
		/* (a window drawing its motion resolves it once as it ends; one
		not drawing it, never) */
		int drawing = window_on, resolves = resolved;

		motion_vectors_window_end(&camera->viewport_bounds);
		CHECK(resolved == resolves + (drawing != 0), "the window's motion resolved %d times",
			resolved - resolves);
	}
	CHECK(!window_on, "the jitter kept past the window");
}

/* ---------- the cases */

static float vertex[3] = { 0.5f, 0.2f, 0.3f };

/* the camera turned and moved: the projection's previous registers are the
frame before's (the level's, and the first-person weapon's own field of
view), and the resolve's reprojection takes a point's clip position now to
the frame before's */
static void camera(void)
{
	struct render_camera cameras[2];
	struct render_frustum frustums[2], weapon[2];
	struct render_camera unused;
	float world[3] = { 20.0f, 3.0f, 1.0f }, before[2], now[2], depth, clip[4], back[4];
	int frame_index, component;

	camera_make(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, &cameras[0], &frustums[0]);
	camera_make(0.3f, 0.1f, 0.0f, 0.05f, 1.0f, &cameras[1], &frustums[1]);
	for (frame_index = 0; frame_index < 2; frame_index++)
		frame(&cameras[frame_index], &frustums[frame_index], NULL, 0, NULL);
	CHECK(resolved_history, "a frame before, none");
	/* the level's: a point of the world's motion from its depth */
	world_ndc(&frustums[0], world, before, NULL);
	world_ndc(&frustums[1], world, now, &depth);
	clip[0] = now[0]; clip[1] = now[1]; clip[2] = depth; clip[3] = 1.0f;
	for (component = 0; component < 4; component++)
		back[component] = resolved_rows[component][0] * clip[0] + resolved_rows[component][1] * clip[1] +
			resolved_rows[component][2] * clip[2] + resolved_rows[component][3];
	CHECK(NEAR(back[0] / back[3], before[0]) && NEAR(back[1] / back[3], before[1]),
		"reprojected %f %f, drawn %f %f the frame before", back[0] / back[3], back[1] / back[3], before[0], before[1]);
	/* ... and the previous projection's registers draw it there */
	{
		short identity_node = 0;
		float one = 1.0f, at[2];
		float rows[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

		memcpy(previous[60], rows, sizeof(rows));
		vertex_ndc(previous, &identity_node, &one, 1, world, at);
		CHECK(NEAR(at[0], before[0]) && NEAR(at[1], before[1]), "the previous projection draws %f %f, not %f %f",
			at[0], at[1], before[0], before[1]);
	}
	/* the first-person weapon's own projection (a wider field of view): its
	previous registers the frame before's view through it */
	camera_make(0.0f, 0.0f, 0.0f, 0.0f, 0.7f, &unused, &weapon[0]);
	camera_make(0.3f, 0.1f, 0.0f, 0.05f, 0.7f, &unused, &weapon[1]);
	frame(&cameras[0], &frustums[0], NULL, 0, &weapon[0]);
	frame(&cameras[1], &frustums[1], NULL, 0, &weapon[1]);
	world_ndc(&weapon[0], world, before, NULL);
	{
		short identity_node = 0;
		float one = 1.0f, at[2];
		float rows[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };

		memcpy(previous[60], rows, sizeof(rows));
		vertex_ndc(previous, &identity_node, &one, 1, world, at);
		CHECK(NEAR(at[0], before[0]) && NEAR(at[1], before[1]), "the weapon's projection draws %f %f, not %f %f",
			at[0], at[1], before[0], before[1]);
	}
}

/* an object's vertex: its motion (the frame before's registers' position
less this frame's) is its own pose's and the camera's */
static void object_motion(struct object frames[2], const short *nodes, const float *weights, int count)
{
	struct render_camera cameras[2];
	struct render_frustum frustums[2];
	float now[2], drawn_now[2], before[2], drawn_before[2], world[3] = { 0.0f, 0.0f, 0.0f }, part[3];
	int frame_index, index, axis;

	camera_make(0.0f, -0.5f, 0.0f, 0.0f, 1.0f, &cameras[0], &frustums[0]);
	camera_make(0.1f, -0.4f, 0.0f, 0.02f, 1.0f, &cameras[1], &frustums[1]);
	for (frame_index = 0; frame_index < 2; frame_index++)
		frame(&cameras[frame_index], &frustums[frame_index], &frames[frame_index], 1, NULL);
	CHECK(previous_sets == 2, "the frame before's nodes not given (%d sets)", previous_sets);
	vertex_ndc(registers, nodes, weights, count, vertex, now);
	vertex_ndc(previous, nodes, weights, count, vertex, before);
	/* (where the frame before's pose and camera drew it, and this frame's) */
	for (index = 0; index < count; index++)
	{
		const unsigned char *palette = frames[0].palette;

		node_point(&frames[0].nodes[palette ? palette[nodes[index]] : nodes[index]], vertex, part);
		for (axis = 0; axis < 3; axis++)
			world[axis] += weights[index] * part[axis];
	}
	world_ndc(&frustums[0], world, drawn_before, NULL);
	memset(world, 0, sizeof(world));
	for (index = 0; index < count; index++)
	{
		const unsigned char *palette = frames[1].palette;

		node_point(&frames[1].nodes[palette ? palette[nodes[index]] : nodes[index]], vertex, part);
		for (axis = 0; axis < 3; axis++)
			world[axis] += weights[index] * part[axis];
	}
	world_ndc(&frustums[1], world, drawn_now, NULL);
	CHECK(NEAR(now[0], drawn_now[0]) && NEAR(now[1], drawn_now[1]), "now %f %f, drawn %f %f", now[0], now[1],
		drawn_now[0], drawn_now[1]);
	CHECK(NEAR(before[0], drawn_before[0]) && NEAR(before[1], drawn_before[1]),
		"the frame before %f %f, drawn %f %f", before[0], before[1], drawn_before[0], drawn_before[1]);
	CHECK(!NEAR(now[0] - before[0], 0.0f) || !NEAR(now[1] - before[1], 0.0f), "no motion");
}

static void rigid(void)
{
	struct object frames[2];
	short node = 0;
	float one = 1.0f;

	memset(frames, 0, sizeof(frames));
	frames[0].id = frames[1].id = 0x12340005;
	frames[0].count = frames[1].count = 1;
	node_make(&frames[0].nodes[0], 0.0f, 10.0f, 1.0f, 0.0f);
	node_make(&frames[1].nodes[0], 0.3f, 10.5f, 0.6f, 0.2f);
	object_motion(frames, &node, &one, 1);
}

/* two nodes, each its own way (a biped's arm), and a part's palette */
static void skinned(void)
{
	static const unsigned char palette[2] = { 1, 0 };
	struct object frames[2];
	short nodes[2] = { 0, 1 };
	float weights[2] = { 0.3f, 0.7f };
	int with_palette;

	for (with_palette = 0; with_palette < 2; with_palette++)
	{
		memset(frames, 0, sizeof(frames));
		frames[0].id = frames[1].id = 0x22220007 + with_palette;
		frames[0].count = frames[1].count = 2;
		frames[0].palette = frames[1].palette = with_palette ? palette : NULL;
		node_make(&frames[0].nodes[0], 0.0f, 8.0f, 0.0f, 0.0f);
		node_make(&frames[0].nodes[1], 0.2f, 8.0f, 0.5f, 0.4f);
		node_make(&frames[1].nodes[0], 0.0f, 8.1f, 0.0f, 0.0f);
		node_make(&frames[1].nodes[1], -0.4f, 8.3f, 0.2f, 0.9f);
		object_motion(frames, nodes, weights, 2);
	}
}

/* a teleported object, and a camera cut: no history (the frame before's
registers this frame's: the object moves with the camera alone, the cut's
window not at all) */
static void cut(void)
{
	struct render_camera cameras[3];
	struct render_frustum frustums[3];
	struct object frames[2];
	int index;

	memset(frames, 0, sizeof(frames));
	frames[0].id = frames[1].id = 0x33330009;
	frames[0].count = frames[1].count = 1;
	node_make(&frames[0].nodes[0], 0.0f, 10.0f, 1.0f, 0.0f);
	node_make(&frames[1].nodes[0], 0.0f, 30.0f, 1.0f, 0.0f);
	camera_make(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, &cameras[0], &frustums[0]);
	camera_make(0.1f, 0.0f, 0.0f, 0.0f, 1.0f, &cameras[1], &frustums[1]);
	frame(&cameras[0], &frustums[0], &frames[0], 1, NULL);
	frame(&cameras[1], &frustums[1], &frames[1], 1, NULL);
	CHECK(resolved_history, "the camera's history lost");
	CHECK(previous_sets == 1, "a teleported object's nodes of the frame before given");
	for (index = 0; index < 3; index++)
		CHECK(!memcmp(previous[60 + index], registers[60 + index], sizeof(registers[0])), "its nodes not this frame's");
	/* the camera jumps: no history, its projection's previous registers
	this frame's */
	camera_make(50.0f, 0.0f, 0.0f, 0.0f, 1.0f, &cameras[2], &frustums[2]);
	frame(&cameras[2], &frustums[2], NULL, 0, NULL);
	CHECK(!resolved_history, "a cut kept the frame before");
	for (index = 0; index < 4; index++)
	{
		int part;

		for (part = 0; part < 4; part++)
			CHECK(fabsf(previous[index][part] - registers[index][part]) <= 1.0e-4f * (1.0f + fabsf(registers[index][part])),
				"a cut's previous projection %d %d: %f, not %f", index, part, previous[index][part], registers[index][part]);
	}
}

/* off: nothing, no jitter */
static void off(void)
{
	struct render_camera camera_now;
	struct render_frustum frustum;

	technique = "off";
	camera_make(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, &camera_now, &frustum);
	motion_vectors_frame_begin();
	CHECK(!motion_vectors_window_begin(0, TRUE, &camera_now, &frustum), "off draws the motion");
	CHECK(!window_on, "off jittered");
}

/* display.temporal_aa "dlss": on where the renderer has DLSS (the window's
motion and jitter made); where it has none, off (none of either) */
static void dlss(int drawn)
{
	struct render_camera camera_now;
	struct render_frustum frustum;
	boolean begun;

	technique = "dlss";
	dlss_drawn = drawn;
	camera_make(0.0f, 0.0f, 0.0f, 0.0f, 1.0f, &camera_now, &frustum);
	motion_vectors_frame_begin();
	begun = motion_vectors_window_begin(0, TRUE, &camera_now, &frustum);
	CHECK(motion_vectors_technique() == (drawn ? 2 : 0), "the technique %d", motion_vectors_technique());
	CHECK(begun == (drawn != 0) && window_on == (drawn != 0), "DLSS %s: the motion %s", drawn ? "drawn" : "not drawn",
		begun ? "made" : "not made");
}

int main(int argc, char **argv)
{
	const char *name = argc > 1 ? argv[1] : "";

	if (!strcmp(name, "camera"))
		camera();
	else if (!strcmp(name, "rigid"))
		rigid();
	else if (!strcmp(name, "skinned"))
		skinned();
	else if (!strcmp(name, "cut"))
		cut();
	else if (!strcmp(name, "off"))
		off();
	else if (!strcmp(name, "dlss"))
		dlss(1);
	else if (!strcmp(name, "dlss_unavailable"))
		dlss(0);
	else
	{
		fprintf(stderr, "unknown case %s\n", name);
		return 2;
	}
	return 0;
}
