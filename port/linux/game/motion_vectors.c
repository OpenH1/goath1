/*
MOTION_VECTORS.C

display.temporal_aa (motion_vectors.h, docs/temporal-aa.md): each window's
motion since the frame before and its jitter, for a temporal technique, at
any render scale (native too).

The motion of a pixel is where its surface was drawn the frame before less
where it is drawn now. The window's objects' draws find it themselves: each
vertex program runs twice, on this frame's registers and on the frame
before's (xgpu_spirv.c's motion), so a skinned character's vertices move as
its bones did, whatever the program does with them. This file gives the
front end the previous frame's registers where they differ: the window's
camera (its projection, c[-96] to c[-93], as the frame before saw the
world) and each object's nodes (c[-36] on), kept by its datum handle as
drawn the frame before (render_interpolation's pose). Every other register
is this frame's. The rest of the window (the level, the sky, decals) moves
with the camera alone: the front end's resolve finds it from the depth by
the reprojection from this frame's clip positions to the frame before's.

No history, no motion: a window with no frame before (a new map, a cut, a
resize, a change of player) has none at all, and an object none of its
own (it appeared, respawned, was teleported, changed its nodes): the frame
before's registers are this frame's, so it moves with the camera.

The jitter: a Halton (2, 3) offset of the window's pixels each frame (its
sequence the upscaler's length: halo_motion_jitter_phases), while
a technique is on, given to the front end, which moves the draws' pixels
by it (the viewport's offset in the draws' uniforms, not the game's
projection): the motion leaves it out, the HUD and menus never have it.

Render-only: the window's and objects' history is this file's own memory
(allocated as a technique is first on), read from the drawn pose, never
from or into the game's state. Not the VR mode's eyes (each eye's
projection is the host's; explicitly unsupported), a mirror's view, a
feed's, or a multisampled view (the front end's).
*/

#include "cseries.h"
#include "render/render.h"
#include "render/render_cameras.h"

#include "lt_lights.h"
#include "motion_vectors.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *config_string(const char *name);
unsigned long halo_gpu_settings_taken(void);
int halo_temporal_dlss_drawn(void);

/* an object's nodes as the vertex shader has them (three registers each) */
typedef real motion_node[3][4];

struct motion_object
{
	/* its datum handle (NONE: free) and kind (0 the world's, else a
	first-person weapon's local player plus one) */
	long id;
	short kind;
	short count;
	/* the frame it was drawn in, and its nodes then; the frame before's,
	where it has them */
	unsigned long frame;
	boolean history;
	motion_node *current, *previous;
};

struct motion_window
{
	/* the frame its camera is of (0 none) */
	unsigned long frame;
	real world_to_view[4][3];
	real projection[4][4];
	real_point3d position;
	real_vector3d forward;
	rectangle2d viewport;
};

static struct
{
	boolean read, technique_valid;
	unsigned long read_at;
	int technique;
	unsigned long frame;
	/* the window drawing now: its slot (a local player's), whether it draws
	its motion and has a frame before, and its camera now and before */
	boolean active, history;
	short slot;
	struct motion_window now, before;
	/* (display.temporal_aa "fsr": the window's camera's near and far planes
	and vertical field of view, for the upscaler's depth) */
	float camera[3];
	/* the camera's change, a point of the world (x, y, z, 1) to where this
	frame's camera sees what the frame before's saw there: this frame's
	projection's registers' inverse after the frame before's (rows; the
	identity with no history) */
	double change[4][4];
	struct motion_window windows[4];
	/* the objects (NULL until a technique is first on) */
	struct motion_object *objects;
	motion_node *nodes;
	/* the object named now (its id, kind and nodes), and its entry once
	its nodes are given (the model drawing now) */
	long named_id;
	short named_kind;
	real_matrix4x3 const *named_nodes;
	real_matrix4x3 const *drawing_nodes;
	struct motion_object *drawing;
} motion_vectors = { FALSE };

int motion_vectors_technique(void)
{
	unsigned long changes = halo_gpu_settings_taken();

	if (!motion_vectors.read || motion_vectors.read_at != changes)
	{
		const char *setting = config_string("display.temporal_aa");

		motion_vectors.read = TRUE;
		motion_vectors.read_at = changes;
		motion_vectors.technique = !setting ? 0 : !strcmp(setting, "fsr") ? 1 : !strcmp(setting, "dlss") ? 2 :
			!strcmp(setting, "metalfx") ? 3 : 0;
	}
	/* ("dlss" where the renderer has no DLSS, "metalfx" where it has no
	MetalFX: off, no motion or jitter) */
	if ((motion_vectors.technique == 2 && !halo_temporal_dlss_drawn()) ||
		(motion_vectors.technique == 3 && !halo_temporal_metalfx_drawn()))
		return 0;
	return motion_vectors.technique;
}

static real halton(unsigned long index, unsigned long base)
{
	real fraction = 1.0f, result = 0.0f;

	while (index)
	{
		fraction /= (real)base;
		result += fraction * (real)(index % base);
		index /= base;
	}
	return result;
}

void motion_vectors_jitter(unsigned long frame, real *x, real *y)
{
	int phases = halo_motion_jitter_phases();
	unsigned long index = frame % (unsigned long)(phases > 0 ? phases : MOTION_JITTER_BASE_PHASES) + 1;

	*x = halton(index, 2) - 0.5f;
	*y = halton(index, 3) - 0.5f;
}

void motion_vectors_frame_begin(void)
{
	motion_vectors.frame++;
	/* (never 0: a window's or an object's none) */
	if (!motion_vectors.frame)
		motion_vectors.frame = 1;
}

/* the objects' table, made the first time (FALSE: it cannot be) */
static boolean objects_made(void)
{
	long index;

	if (motion_vectors.objects)
		return TRUE;
	motion_vectors.objects = (struct motion_object *)calloc(MOTION_OBJECTS, sizeof(*motion_vectors.objects));
	motion_vectors.nodes = (motion_node *)calloc((size_t)MOTION_OBJECTS * 2 * MOTION_NODES,
		sizeof(*motion_vectors.nodes));
	if (!motion_vectors.objects || !motion_vectors.nodes)
	{
		free(motion_vectors.objects);
		free(motion_vectors.nodes);
		motion_vectors.objects = NULL;
		motion_vectors.nodes = NULL;
		return FALSE;
	}
	for (index = 0; index < MOTION_OBJECTS; index++)
	{
		motion_vectors.objects[index].id = NONE;
		motion_vectors.objects[index].current = &motion_vectors.nodes[index * 2 * MOTION_NODES];
		motion_vectors.objects[index].previous = &motion_vectors.nodes[(index * 2 + 1) * MOTION_NODES];
	}
	return TRUE;
}

/* ---------- the camera */

/* the projection's registers (rasterizer_set_frustum_z's): each the clip
position's component's weights of x, y, z, 1 */
static void projection_registers(real const world_to_view[4][3], real const projection[4][4], double registers[4][4])
{
	short column, row;

	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			registers[column][row] = (double)world_to_view[row][0] * projection[0][column] +
				(double)world_to_view[row][1] * projection[1][column] +
				(double)world_to_view[row][2] * projection[2][column];
		}
		registers[column][3] += projection[3][column];
	}
}

static void window_camera(struct motion_window *window, struct render_camera const *camera,
	struct render_frustum const *frustum)
{
	short row, column;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 3; column++)
			window->world_to_view[row][column] = frustum->world_to_view.n[row][column];
	}
	memcpy(window->projection, frustum->projection_matrix, sizeof(window->projection));
	window->position = camera->position;
	window->forward = camera->forward;
	window->viewport = camera->viewport_bounds;
	window->frame = motion_vectors.frame;
}

/* whether the camera cut between the frame before and now */
static boolean camera_cut(struct motion_window const *before, struct motion_window const *now)
{
	real dx = now->position.x - before->position.x;
	real dy = now->position.y - before->position.y;
	real dz = now->position.z - before->position.z;
	real turned = now->forward.i * before->forward.i + now->forward.j * before->forward.j +
		now->forward.k * before->forward.k;

	return dx * dx + dy * dy + dz * dz > MOTION_TELEPORT_DISTANCE * MOTION_TELEPORT_DISTANCE ||
		turned < MOTION_CUT_COSINE ||
		memcmp(&now->viewport, &before->viewport, sizeof(now->viewport)) != 0;
}

static boolean inverse4(double const matrix[4][4], double inverse[4][4]);

/* the camera's change (motion_vectors.change); FALSE where there is none */
static boolean camera_change(void)
{
	double now[4][4], before[4][4], inverse[4][4];
	short row, column, inner;

	projection_registers((real const (*)[3])motion_vectors.now.world_to_view,
		(real const (*)[4])motion_vectors.now.projection, now);
	projection_registers((real const (*)[3])motion_vectors.before.world_to_view,
		(real const (*)[4])motion_vectors.before.projection, before);
	if (!inverse4((double const (*)[4])now, inverse))
		return FALSE;
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			double sum = 0.0;

			for (inner = 0; inner < 4; inner++)
				sum += inverse[row][inner] * before[inner][column];
			motion_vectors.change[row][column] = sum;
		}
	}
	return TRUE;
}

boolean motion_vectors_window_begin(short local_player_index, boolean eligible, struct render_camera const *camera,
	struct render_frustum const *frustum)
{
	struct motion_window *slot;
	real jitter_x = 0.0f, jitter_y = 0.0f;
	int technique = motion_vectors_technique();

	motion_vectors.active = FALSE;
	motion_vectors.drawing = NULL;
	motion_vectors.drawing_nodes = motion_vectors.named_nodes = NULL;
	/* (no technique, the light-transport pass on: the frame before's
	registers alone, for the pre-pass's twins' motion, not jittered) */
	if ((!technique && !lt_lights_sent()) || !eligible || local_player_index < 0 || local_player_index >= 4 ||
		!frustum->projection_valid || !objects_made())
	{
		return FALSE;
	}
	if (technique)
		motion_vectors_jitter(motion_vectors.frame, &jitter_x, &jitter_y);
	if (!halo_motion_window(technique ? TRUE : HALO_MOTION_PREVIOUS, jitter_x, jitter_y))
		return FALSE;
	slot = &motion_vectors.windows[local_player_index];
	window_camera(&motion_vectors.now, camera, frustum);
	motion_vectors.history = slot->frame && slot->frame + 1 == motion_vectors.frame &&
		!camera_cut(slot, &motion_vectors.now);
	/* (none before: the frame before's camera this frame's) */
	motion_vectors.before = motion_vectors.history ? *slot : motion_vectors.now;
	*slot = motion_vectors.now;
	if (!motion_vectors.history || !camera_change())
	{
		short row, column;

		motion_vectors.history = FALSE;
		motion_vectors.before = motion_vectors.now;
		for (row = 0; row < 4; row++)
		{
			for (column = 0; column < 4; column++)
				motion_vectors.change[row][column] = row == column ? 1.0 : 0.0;
		}
	}
	motion_vectors.slot = local_player_index;
	motion_vectors.camera[0] = camera->z_near;
	motion_vectors.camera[1] = camera->z_far;
	motion_vectors.camera[2] = camera->vertical_field_of_view;
	motion_vectors.active = TRUE;
	return TRUE;
}

void motion_vectors_opaque(rectangle2d const *viewport_bounds)
{
	if (motion_vectors.active && viewport_bounds)
		halo_motion_opaque(viewport_bounds->x0, viewport_bounds->y0, viewport_bounds->x1, viewport_bounds->y1,
			motion_vectors.slot);
}

/* (the registers as they are now, whichever projection they are: the
first-person weapon's own field of view and near plane, a zoomed view's,
the camouflage's; the frame before's the same projection of the frame
before's view) */
void motion_vectors_projection(real const (*constants)[4])
{
	float previous[4][4];
	short column, row, inner;

	if (!motion_vectors.active)
		return;
	for (column = 0; column < 4; column++)
	{
		for (row = 0; row < 4; row++)
		{
			double sum = 0.0;

			for (inner = 0; inner < 4; inner++)
				sum += (double)constants[column][inner] * motion_vectors.change[inner][row];
			previous[column][row] = (float)sum;
		}
	}
	halo_motion_previous_constants(-96, (float const (*)[4])previous, 4);
}

/* a 4x4's inverse (rows), Gauss-Jordan with partial pivoting; FALSE where
it has none */
static boolean inverse4(double const matrix[4][4], double inverse[4][4])
{
	double work[4][8];
	short row, column, pivot, other;

	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			work[row][column] = matrix[row][column];
			work[row][column + 4] = row == column ? 1.0 : 0.0;
		}
	}
	for (column = 0; column < 4; column++)
	{
		double scale;

		pivot = column;
		for (row = column + 1; row < 4; row++)
		{
			if (fabs(work[row][column]) > fabs(work[pivot][column]))
				pivot = row;
		}
		if (fabs(work[pivot][column]) < 1.0e-12)
			return FALSE;
		if (pivot != column)
		{
			for (other = 0; other < 8; other++)
			{
				double swap = work[column][other];

				work[column][other] = work[pivot][other];
				work[pivot][other] = swap;
			}
		}
		scale = 1.0 / work[column][column];
		for (other = 0; other < 8; other++)
			work[column][other] *= scale;
		for (row = 0; row < 4; row++)
		{
			double factor = work[row][column];

			if (row == column || factor == 0.0)
				continue;
			for (other = 0; other < 8; other++)
				work[row][other] -= factor * work[column][other];
		}
	}
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
			inverse[row][column] = work[row][column + 4];
	}
	return TRUE;
}

/* the rows of the matrix from this frame's clip positions (x, y, depth, 1)
to the frame before's, the camera's motion alone; FALSE where there is none */
static boolean reprojection(float rows[4][4])
{
	double now[4][4], before[4][4], inverse[4][4];
	short row, column, inner;

	projection_registers((real const (*)[3])motion_vectors.now.world_to_view,
		(real const (*)[4])motion_vectors.now.projection, now);
	projection_registers((real const (*)[3])motion_vectors.before.world_to_view,
		(real const (*)[4])motion_vectors.before.projection, before);
	if (!inverse4((double const (*)[4])now, inverse))
		return FALSE;
	for (row = 0; row < 4; row++)
	{
		for (column = 0; column < 4; column++)
		{
			double sum = 0.0;

			for (inner = 0; inner < 4; inner++)
				sum += before[row][inner] * inverse[inner][column];
			rows[row][column] = (float)sum;
		}
	}
	return TRUE;
}

void motion_vectors_window_end(rectangle2d const *viewport_bounds)
{
	float rows[4][4];
	boolean history;

	if (!motion_vectors.active)
		return;
	motion_vectors.active = FALSE;
	motion_vectors.drawing = NULL;
	motion_vectors.drawing_nodes = motion_vectors.named_nodes = NULL;
	if (!viewport_bounds)
	{
		halo_motion_objects(FALSE);
		halo_motion_window(FALSE, 0.0f, 0.0f);
		return;
	}
	history = motion_vectors.history && reprojection(rows);
	if (!history)
		memset(rows, 0, sizeof(rows));
	halo_motion_resolve(viewport_bounds->x0, viewport_bounds->y0, viewport_bounds->x1, viewport_bounds->y1,
		(float const (*)[4])rows, history, motion_vectors.slot, motion_vectors.camera);
	halo_motion_window(FALSE, 0.0f, 0.0f);
}

/* ---------- the objects */

static void node_registers(real_matrix4x3 const *matrix, motion_node registers)
{
	real scale = matrix->scale;

	registers[0][0] = scale * matrix->forward.i;
	registers[0][1] = scale * matrix->left.i;
	registers[0][2] = scale * matrix->up.i;
	registers[0][3] = matrix->position.x;
	registers[1][0] = scale * matrix->forward.j;
	registers[1][1] = scale * matrix->left.j;
	registers[1][2] = scale * matrix->up.j;
	registers[1][3] = matrix->position.y;
	registers[2][0] = scale * matrix->forward.k;
	registers[2][1] = scale * matrix->left.k;
	registers[2][2] = scale * matrix->up.k;
	registers[2][3] = matrix->position.z;
}

void motion_vectors_objects(boolean begin)
{
	/* (whether or not the motion is drawn: the span is the draws' input,
	journaled, so that a record made with the setting off makes their
	motion's variants as it plays with it on; d3d8_device.c draws the
	motion only in an active window) */
	halo_motion_objects(begin);
}

/* an object's entry: its own, or a free one, or the one drawn longest ago */
static struct motion_object *object_entry(long id, short kind)
{
	unsigned long hash = ((unsigned long)id * 2654435761UL) ^ ((unsigned long)kind * 40503UL);
	struct motion_object *oldest = NULL;
	short probe;

	for (probe = 0; probe < 8; probe++)
	{
		struct motion_object *entry = &motion_vectors.objects[(hash + (unsigned long)probe) & (MOTION_OBJECTS - 1)];

		if (entry->id == id && entry->kind == kind)
			return entry;
		if (entry->id == NONE || entry->frame + 1 < motion_vectors.frame)
		{
			entry->id = NONE;
			if (!oldest || oldest->id != NONE)
				oldest = entry;
			continue;
		}
		if (!oldest || (oldest->id != NONE && entry->frame < oldest->frame))
			oldest = entry;
	}
	oldest->id = NONE;
	return oldest;
}

void motion_vectors_object(long id, short kind, real_matrix4x3 const *nodes)
{
	if (!motion_vectors.active)
		return;
	motion_vectors.named_id = id;
	motion_vectors.named_kind = kind;
	motion_vectors.named_nodes = id != NONE ? nodes : NULL;
	motion_vectors.drawing = NULL;
	motion_vectors.drawing_nodes = NULL;
}

void motion_vectors_model_relative(real_matrix4x3 const *nodes, real_matrix4x3 const *relative)
{
	if (!motion_vectors.active)
		return;
	/* (another model's: none named from here on) */
	motion_vectors.named_nodes = nodes && nodes == motion_vectors.named_nodes ? relative : NULL;
}

void motion_vectors_model_nodes(real_matrix4x3 const *nodes, short count)
{
	motion_vectors_model_nodes_packed(nodes, count, NULL);
}

void motion_vectors_model_nodes_packed(real_matrix4x3 const *nodes, short count, real const (*packed)[3][4])
{
	struct motion_object *entry;
	long id = motion_vectors.named_id;
	short node;

	if (!motion_vectors.active)
		return;
	/* (another model's: its registers this frame's) */
	if (!nodes || nodes != motion_vectors.named_nodes || count <= 0)
	{
		motion_vectors.drawing = NULL;
		motion_vectors.drawing_nodes = NULL;
		return;
	}
	if (motion_vectors.drawing && motion_vectors.drawing_nodes == nodes && motion_vectors.drawing->count ==
		(count > MOTION_NODES ? MOTION_NODES : count))
		return;
	motion_vectors.drawing = NULL;
	motion_vectors.drawing_nodes = NULL;
	if (count > MOTION_NODES)
		count = MOTION_NODES;
	entry = object_entry(id, motion_vectors.named_kind);
	/* (drawn already this frame: another window's, or the pre-pass's) */
	if (entry->id == id && entry->frame == motion_vectors.frame)
	{
		if (entry->count == count)
		{
			motion_vectors.drawing = entry;
			motion_vectors.drawing_nodes = nodes;
		}
		return;
	}
	if (entry->id == id && entry->frame + 1 == motion_vectors.frame && entry->count == count)
	{
		motion_node *swap = entry->previous;

		entry->previous = entry->current;
		entry->current = swap;
		entry->history = TRUE;
	}
	else
	{
		entry->history = FALSE;
	}
	entry->id = id;
	entry->kind = motion_vectors.named_kind;
	entry->count = count;
	entry->frame = motion_vectors.frame;
	/* (packed already as the draw's registers: those very bytes) */
	if (packed)
		memcpy(entry->current, packed, (size_t)count * sizeof(motion_node));
	else
		for (node = 0; node < count; node++)
			node_registers(&nodes[node], entry->current[node]);
	/* (teleported: its root moved too far to have moved) */
	if (entry->history)
	{
		real dx = entry->current[0][0][3] - entry->previous[0][0][3];
		real dy = entry->current[0][1][3] - entry->previous[0][1][3];
		real dz = entry->current[0][2][3] - entry->previous[0][2][3];

		if (dx * dx + dy * dy + dz * dz > MOTION_TELEPORT_DISTANCE * MOTION_TELEPORT_DISTANCE)
			entry->history = FALSE;
	}
	motion_vectors.drawing = entry;
	motion_vectors.drawing_nodes = nodes;
}

void motion_vectors_skinning(real_matrix4x3 const *nodes, unsigned char const *palette, short count,
	short palette_most)
{
	struct motion_object *entry = motion_vectors.drawing;
	float previous[MOTION_NODES * 3][4];
	short node;

	/* (an object of no history moves with the camera: its registers this
	frame's, as the front end keeps them) */
	if (!motion_vectors.active || !entry || !entry->history || nodes != motion_vectors.drawing_nodes || count <= 0)
		return;
	if (count > MOTION_NODES)
		count = MOTION_NODES;
	for (node = 0; node < count; node++)
	{
		short index = palette ? (short)palette[node] : node;

		if (palette && index > palette_most)
			index = palette_most;
		if (index < 0 || index >= entry->count)
			return;
		memcpy(previous[node * 3], entry->previous[index], sizeof(motion_node));
	}
	halo_motion_previous_constants(-36, (float const (*)[4])previous, (unsigned long)count * 3);
}
