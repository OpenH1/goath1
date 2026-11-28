/*
LT_LIGHTS_INPUTS.H

The light list's inputs: everything of the game's state the list's building
(port/linux/game/lt_lights_build.c) reads in a frame, gathered by
lt_lights.c from the game (its lights as drawn, what their tags, objects and
the level say of them, the sky, the object lamps and the camera), so that
the same building runs on them in the game and again, rebuilt and reloaded
alone, in the replay (xgpu_stream.h's LT_INPUTS, sent before the frame's
LT_LIGHTS; port/tools/replay's light-list unit, HALO_LT_UNIT).

Plain data: every member is 4 bytes and there are no pointers, so it lays
out alike in the guest and on the host. The record is the header, then
light_count struct lt_input_light, then lamp_count struct lt_input_lamp.
*/

#ifndef LT_LIGHTS_INPUTS_H
#define LT_LIGHTS_INPUTS_H

/* the layout's version: raised when a member changes (an older recording's
inputs are then not taken) */
#define LT_INPUTS_VERSION 1
/* the sky's lights taken (the sky tag's block) */
#define LT_INPUTS_SKY_LIGHTS 64
/* the frame's lights taken (dynamic_lights_submit's count; more, and the
frame's inputs are not built from) */
#define LT_INPUTS_LIGHTS_MOST 2048
/* the object lamps taken: every one up to the most lt_lights_build.c's
choice ranks (4096), else the first it sends (its LT_OBJECT_LAMPS_SENT) */
#define LT_INPUTS_LAMPS_MOST 4096

/* one of the frame's lights (dynamic_lights_submit's: halo_dynamic_light and
its key, object, owner and baked), and what the game says of it */
struct lt_input_light
{
	/* struct halo_dynamic_light's, as drawn */
	float position[3], axis[3], radius, color[3];
	unsigned int flags;
	float cone_scale, cone_offset;
	/* dynamic_lights.c's key (low, high), the object it is on (NONE -1),
	its owner as recorded, whether baked */
	unsigned int key[2];
	int object;
	unsigned int owner, baked;
	/* the rasterizer's light at this index (rasterizer_lights.lights[]),
	where there is one: its up, and its tag's runtime falloff and cutoff
	cosines, flags and gel bitmap (NONE -1); has_definition 0 none */
	unsigned int has_drawn;
	float drawn_up[3];
	unsigned int has_definition;
	float falloff_cosine, cutoff_cosine;
	unsigned int definition_flags;
	int gel_index;
	/* a game light's datum flagged first-person */
	unsigned int first_person;
	/* the gel pass's frame (rasterizer_environment_port_gel, the game's
	lights only), has_gel 0 none */
	unsigned int has_gel;
	float gel_forward[3], gel_up[3], gel_left[3];
	/* the first-person weapon's flashlight lamp on its object
	(first_person_weapon_flashlight_lamp), has_lamp 0 none */
	unsigned int has_lamp;
	float lamp[3];
	/* the object it is on (its own, else the one its key names: an own
	light's, a firer's flames'): there (object_try_and_get), a projectile,
	and its top parent's ray-traced owner (world_rt_owner) */
	unsigned int carrier_exists, carrier_projectile, carrier_owner;
};

/* one of the sky's lights, as the tag has it, its direction worked out (the
game's vector3d_from_euler_angles2d, HALO_LT_SUN_ANGLES applied) */
struct lt_input_sky_light
{
	float direction[3], up[3];
	float color[3], power, diameter;
	unsigned int flags;
};

/* one object lamp (world_rt.c's world_rt_lamps, world_rt_lamp_sides) */
struct lt_input_lamp
{
	float corners[9], radiance[3];
	unsigned int owner, sides;
};

struct lt_inputs
{
	unsigned int version, bytes;
	/* the frame's lights: count, and the first game_count the rasterizer's;
	the window */
	int light_count, game_count, window;
	/* the sky (the one the most BSP clusters name): there, its index, lit
	live (lt_lights_real_sky), its lights, its outdoor ambient color and
	power */
	int has_sky, sky_index, live, sky_light_count;
	struct lt_input_sky_light sky_lights[LT_INPUTS_SKY_LIGHTS];
	float ambient_color[3], ambient_power;
	/* the camera (render.camera.position), the object lamps: their count
	in the world, those taken, whether the sides are known */
	float camera[3];
	int lamps_in_world, lamp_count, has_sides;
};

/* (lt_lights_build.c) the lights' records and the suns' after them, every
float finite, into records (XGPU_STREAM_LT_LIGHTS_MAXIMUM of room): their
count; then the object lamps after count of them, no more than maximum: the
count with them */
struct xgpu_stream_lt_light;
int lt_lights_build_list(struct lt_inputs const *in, struct lt_input_light const *lights,
	struct xgpu_stream_lt_light *records);
int lt_lights_build_lamps(struct lt_inputs const *in, struct lt_input_lamp const *lamps,
	struct xgpu_stream_lt_light *lights, int count, int maximum);

#endif
