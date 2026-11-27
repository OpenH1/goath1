/*
MOTION_VECTORS.H

display.temporal_aa (docs/temporal-aa.md): each window's motion and jitter
for a temporal technique (AMD FSR, NVIDIA DLSS, Apple MetalFX), at any
render scale, the native one too. port/linux/game/motion_vectors.c keeps
the previous frame's camera of each window and nodes of each object drawn,
render-only: nothing here is read by the simulation, sent between machines
or written to a saved game.
*/

#ifndef MOTION_VECTORS_H
#define MOTION_VECTORS_H

struct render_camera;
struct render_frustum;

enum
{
	/* the frames of the jitter's Halton (2, 3) sequence before it repeats
	at the native scale; more below it (halo_motion_jitter_phases) */
	MOTION_JITTER_BASE_PHASES = 8,
	/* the objects whose nodes are kept (a power of two) */
	MOTION_OBJECTS = 1024,
	/* the most nodes an object's motion is kept for (MAXIMUM_NODES_PER_MODEL) */
	MOTION_NODES = 64,
};

/* (no history where a node's root moved more than this between two frames,
in world units: a teleporter, a respawn in place, a vehicle entered; the
camera's where it moved more than this, or turned more than about 60
degrees: a cut) */
#define MOTION_TELEPORT_DISTANCE 4.0f
#define MOTION_CUT_COSINE 0.5f

/* display.temporal_aa's technique: 0 "off", else 1 "fsr", 2 "dlss", 3
"metalfx" (read again as the front end takes the settings) */
int motion_vectors_technique(void);

/* render_frame: a frame begun (its number) */
void motion_vectors_frame_begin(void);

/* render_window, before rasterizer_window_begin: the window's camera (the
rasterizer's) and whether its motion is drawn (the screen's 3D view: not a
mirror's, a feed's, the VR mode's eyes); TRUE where it is */
boolean motion_vectors_window_begin(short local_player_index, boolean eligible, struct render_camera const *camera,
	struct render_frustum const *frustum);
/* rasterizer_set_frustum_z: the projection's constants just set (c[-96] to
c[-93]: each the clip position's component's weights of x, y, z, 1): the
previous frame's beside them */
void motion_vectors_projection(real const (*constants)[4]);
/* render_objects, around the window's own draw of its objects (not
display.ambient_occlusion "world"'s pre-pass): their motion drawn too */
void motion_vectors_objects(boolean begin);
/* an object (its datum handle) about to be drawn with these nodes (a
first-person weapon's or arms': kind its local player's plus one, plus 4
for the arms; 0 the world's objects) */
void motion_vectors_object(long id, short kind, real_matrix4x3 const *nodes);
/* render_model: the nodes an object was named with (node_matrices) given
to the rasterizer as relative (each times its default's inverse): the
relative ones are the object's from here on */
void motion_vectors_model_relative(real_matrix4x3 const *nodes, real_matrix4x3 const *relative);
/* rasterizer_set_model_skinning: a model's nodes, count of them, given
(the object's, where they are the nodes it was named with: kept for the
next frame) */
void motion_vectors_model_nodes(real_matrix4x3 const *nodes, short count);
/* the same, the nodes already packed as the vertex shader's registers
(three each, as node_registers packs them; NULL: packed here) */
void motion_vectors_model_nodes_packed(real_matrix4x3 const *nodes, short count, real const (*packed)[3][4]);
/* rasterizer_set_model_skinning, rasterizer_model_part_skinning: the nodes
just set as the vertex shader's (c[-36] on, three each), count of them from
nodes, each the palette's (NULL: in order; past palette_most the last):
their previous frame's beside them */
void motion_vectors_skinning(real_matrix4x3 const *nodes, unsigned char const *palette, short count,
	short palette_most);
/* render_view, once its opaque surfaces are drawn and before its
transparent ones (particles, effects, glass, water, lens flares): the
window's colour kept for display.temporal_aa "fsr"'s reactive mask, which is
where they change it (its view, the game's units of the screen) */
void motion_vectors_opaque(rectangle2d const *viewport_bounds);
/* render_window, before the tone map: the window's motion resolved over
its view (the game's units of the screen); the jitter taken away (the HUD
is drawn still); with display.temporal_aa "fsr", the view upscaled into the
screen's targets at the display's scale, where the tone map, panini and
HUD are drawn. NULL: the window ended with none resolved */
void motion_vectors_window_end(rectangle2d const *viewport_bounds);

/* the jitter of a frame, in pixels (x right, y down: each -0.5 to 0.5) */
void motion_vectors_jitter(unsigned long frame, real *x, real *y);

/* ---------- the front end's (port/linux/src/d3d8_device.c) */

/* the window's draws jittered by so many of the target's pixels (0 0
none), and whether its motion can be drawn (the back buffer, one sample);
on HALO_MOTION_PREVIOUS (the light-transport pass without a technique):
the previous frame's registers alone, kept for the pre-pass's twins'
motion (xgpu_stream.h's MOTION phase NONE), no jitter, no objects' motion
drawn and none resolved */
#define HALO_MOTION_PREVIOUS 2
int halo_motion_window(int on, float jitter_x, float jitter_y);
/* the previous frame's registers beside c[reg] .. c[reg + count) */
/* the frames of the jitter's sequence before it repeats: the upscaler's
at the render scale, 8 times the display's pixels per the render's,
squared (17 at 0.67), MOTION_JITTER_BASE_PHASES at the native scale */
int halo_motion_jitter_phases(void);
void halo_motion_previous_constants(int reg, float const (*values)[4], unsigned long count);
/* the window's objects' span begun or ended, whatever the setting (the
draws' input, journaled); their motion drawn while the window's is */
void halo_motion_objects(int begin);
/* the window's colour before its transparent draws (window: 0 to 3) */
void halo_motion_opaque(short x0, short y0, short x1, short y1, int window);
/* the window's motion over its view: the reprojection's rows (this frame's
clip positions to the frame before's, the camera's), whether it has a
frame before; with display.temporal_aa "fsr", the view upscaled (the window,
0 to 3, and its camera's near and far planes and vertical field of view) */
void halo_motion_resolve(short x0, short y0, short x1, short y1, float const reproject[4][4], int history,
	int window, float const camera[3]);

#endif
