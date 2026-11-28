/*
LT_LIGHTS.H

The one light-transport pass's light list (~/dev/rt2-realtime-design.md,
W4, contract C4): every dynamic light as the game draws it this frame, and
the sky's suns, one record each, handed to the backend (xgpu_stream.h's
LT_LIGHTS) for the pass to light by. port/linux/game/lt_lights.c.

The record is the game's copy of xgpu_stream.h's struct
xgpu_stream_lt_light, member for member (the game does not include the
backend's headers): change both. Every member is 4 bytes, so it lays out
alike in the guest and on the host.
*/

#ifndef LT_LIGHTS_H
#define LT_LIGHTS_H

#ifndef XGPU_STREAM_LT_LIGHTS_MAXIMUM

/* the most records a frame */
#define XGPU_STREAM_LT_LIGHTS_MAXIMUM 1024

/* a light's kind */
enum
{
	/* a sphere emitting every way */
	XGPU_LT_LIGHT_POINT,
	/* a disk facing forward, in a cone (inner and outer cosines), its gel
	(if any) the angular profile */
	XGPU_LT_LIGHT_SPOT,
	/* a one-sided Lambertian disk facing forward (a lit display:
	light_sources.c's panels) */
	XGPU_LT_LIGHT_PANEL,
	/* a sun: forward is toward it, at no distance; its angular diameter */
	XGPU_LT_LIGHT_DIRECTIONAL,
	/* one triangle of the level's static light sources (lt_emitters.c),
	not a light of the list: id[0] its index in the set, id[1] the source
	and the set's generation; object the set's triangles, owner its groups,
	gel_slot its group; position, forward and up its corners (wound to glow
	toward their normal); power its group's radiance */
	XGPU_LT_LIGHT_EMITTER,
	/* one triangle of an object's own lamps this frame (world_rt.c's
	world_rt_lamps: a model's self-illuminated triangles, where they glow,
	moved with it), not a light of the list: id[0] its index this frame,
	object the frame's count, owner its object's ray-traced owner;
	position, forward and up its corners (wound to glow toward their
	normal); power its radiance (linear, the pass's units) */
	XGPU_LT_LIGHT_OBJECT_EMITTER,
};
/* an XGPU_LT_LIGHT_OBJECT_EMITTER record's flags (xgpu_stream.h's): an
additive part drawn from both sides, which glows toward both */
#define XGPU_LT_LAMP_TWO_SIDED 1u
/* (an additive part: a glow drawn over its object, not shadowed by it) */
#define XGPU_LT_LAMP_ADDITIVE 2u

/* its flags: the first five are the enhanced lights' own
(halo_dynamic_lights.h's HALO_DYNAMIC_LIGHT_*), the same bits */
enum
{
	XGPU_LT_LIGHT_MODELS_ONLY = 1 << 0,
	XGPU_LT_LIGHT_HALO = 1 << 1,
	XGPU_LT_LIGHT_PROJECTILE = 1 << 2,
	XGPU_LT_LIGHT_FLASHLIGHT = 1 << 3,
	XGPU_LT_LIGHT_PANEL_FLAG = 1 << 4,
	/* lit through a gel (gel_tag; gel_slot where the backend has it) */
	XGPU_LT_LIGHT_GEL = 1 << 8,
	/* a glow standing in for light the lightmap already holds
	(light_sources.c's baked) */
	XGPU_LT_LIGHT_BAKED = 1 << 9,
	/* on the first-person weapon (the light datum's flag) */
	XGPU_LT_LIGHT_FIRST_PERSON = 1 << 10,
	/* the static lightmap already holds its direct light on the structure
	(a sun): the pass darkens only its direct share where a dynamic object
	blocks it */
	XGPU_LT_LIGHT_LIGHTMAP_DIRECT = 1 << 11,
	/* the tag's "no specular" */
	XGPU_LT_LIGHT_NO_SPECULAR = 1 << 12,
	/* the tag's "don't light own object" */
	XGPU_LT_LIGHT_NOT_OWN_OBJECT = 1 << 13,
	/* its id is this frame's alone (a light the game gave no handle) */
	XGPU_LT_LIGHT_UNSTABLE_ID = 1 << 14,
	/* (xgpu_stream.h's: a sky light of a level lit live, the sky's dome) */
	XGPU_LT_LIGHT_REAL_SKY = 1 << 15,
	XGPU_LT_LIGHT_SKY_DOME = 1 << 16,
	/* (an EMITTER's: its set solved from the lightmap, the level lit live) */
	XGPU_LT_LIGHT_EMITTER_SOLVED = 1 << 17,
};

/* an id's source (its high word's top byte; dynamic_lights.c's keys, as
they are, and the suns') */
enum
{
	XGPU_LT_LIGHT_SOURCE_OWN_FIRST = 1, /* 1 to 3: light_sources.c's own lights */
	XGPU_LT_LIGHT_SOURCE_GAME = 4,      /* low word: the light datum's handle (index, salt << 16) */
	XGPU_LT_LIGHT_SOURCE_COPY = 5,      /* a game light's copy on another marker: low word its handle */
	XGPU_LT_LIGHT_SOURCE_SUN = 6,       /* low word: the sky's index << 8 | the light's */
	XGPU_LT_LIGHT_SOURCE_FRAME = 7,     /* low word: the light's place this frame (UNSTABLE_ID) */
	XGPU_LT_LIGHT_SOURCE_EMITTER = 8,   /* an EMITTER's (its high word's low 24 bits the set's generation) */
	XGPU_LT_LIGHT_SOURCE_FIXTURE = 9,   /* a light fixture's (dynamic_lights.c): low word its object's handle */
};

/* 144 bytes. Positions in world units; directions unit length. */
struct xgpu_stream_lt_light
{
	/* stable from frame to frame: id[0] low, id[1] high (source << 24) */
	unsigned int id[2];
	/* XGPU_LT_LIGHT_* kind and flags */
	unsigned int kind, flags;
	/* the object it is on (its handle; 0xffffffff none), and that object's
	owner among the ray-traced scene's instances (world_rt_owner: the rays
	leave it out; 0 none) */
	unsigned int object, owner;
	/* the gel: its slot in the backend's flashlight table (xgpu.h's
	XGPU_DYNAMIC_LIGHTS_FLASHLIGHT, the cells' cookie; -1 none), its bitmap
	tag (-1 none) */
	int gel_slot, gel_tag;
	/* where it is drawn (a gel's projection origin), and its emitter's
	radius (world units) */
	float position[3], emitter_radius;
	/* its frame, as drawn (a gel's turned by the gel's functions; a sun's
	forward toward the sun); range: the range window's radius,
	(1 - (d / range)^4)^2 (0 a sun) */
	float forward[3], range;
	/* up and the cone's inner cosine (full light; -1 none) */
	float up[3], cone_inner;
	/* left (forward x up) and the cone's outer cosine (none; -1 none) */
	float left[3], cone_outer;
	/* the emitter's middle (the flashlight's lamp on the weapon in hand;
	elsewhere position), and the power's scale k the power was given */
	float emitter[3], power_scale;
	/* linear power: the color as the game draws it, taken to linear
	(^2.2), times k (a sun: color x the tag's power x k); a sun's angular
	diameter (radians, 0 others) */
	float power[3], angular_diameter;
	/* the color as the game draws it (0 to 1, gamma; past 1 where the port
	brightens it), for checking; and the gel half-width (tan) where known */
	float color[3], gel_width;
};

typedef char lt_light_size_assert[sizeof(struct xgpu_stream_lt_light) == 144 ? 1 : -1];

#endif

/* the frame's lights, from dynamic_lights.c's dynamic_lights_submit (the
enhanced lights, after lights_preprocess_scene): its lights as drawn
(count of them, the first game_count the rasterizer's, then its copies and
its own), each one's key, object, owner and whether it is baked. Builds the
list when it is wanted (HALO_LT_LIGHTS, or the light-transport pass on:
HALO_LT_E1), sends it, and dumps it (HALO_LT_LIGHTS_DUMP) */
struct halo_dynamic_light;
void lt_lights_frame(struct halo_dynamic_light const *lights, unsigned long long const *keys, long const *objects,
	unsigned long const *owners, unsigned char const *baked, long game_count, long count);

/* whether the light-transport pass draws an object's sun shadow, so that
the game's own (render_objects.c's projected shadow) is not drawn too: the
pass on with its suns (HALO_LT_E1, HALO_LT_SUN not 0), the object not
scenery, its middle in the sky's sun (no structure toward it). Render-only:
a collision test, nothing changed */
boolean lt_lights_sun_shadow(long object_index);

/* whether the list is sent (HALO_LT_LIGHTS, or the light-transport pass on:
HALO_LT_E1): the frame before's registers are then kept for the pre-pass's
twins' motion too (motion_vectors.c) */
boolean lt_lights_sent(void);

/* whether the ray-traced lighting (the light-transport pass) is on:
display.ray_traced_lighting where the GPU has ray queries and the dynamic
lights are enhanced; HALO_LT_E1, where given, overrides it (debug). Off,
nothing of the pass or of the light model it brings runs: the dynamic
lights, the flashlight and the surfaces are master's */
int lt_lights_pass_on(void);
/* whether the level is lit live with the pass on: the lightmap dropped, the
sky's lights, its dome and their bounce the static light (its BSP's
clusters mostly exterior; HALO_LT_REAL_SKY overrides) */
int lt_lights_real_sky(void);
/* the level's static light sources (lt_emitters.c): whether the loaded
BSP has glowing surfaces by the one rule (the level then lit live); whether
they are the per-BSP solve from its lightmap instead (HALO_LT_EMITTERS=
solved, the check); and this frame's share of their records, after count
records of lights (at most maximum in all): the count of all */
int lt_emitters_live(void);
int lt_emitters_fitted(void);
int lt_emitters_append(struct xgpu_stream_lt_light *lights, int count, int maximum);
/* each frame: the backend told when the pass is turned off */
void lt_lights_update(void);

#endif
