/*
WORLD_RT.H

display.ambient_occlusion "ray_traced" (docs/world-ao.md, "Hardware ray
queries", #602): the scene the GPU's ray queries trace the occlusion's rays
through, as the backend builds its acceleration structures of it. The
meshes: the loaded structure BSP's collision geometry, and each object
definition's nodes' collision geometry (the first permutation's, in the
node's space), a mesh for each node that has some, or for an item with
none (a grenade, a weapon or equipment on the ground) its model's render
mesh as drawn, else a box about its bounding sphere; made as they are
first drawn
and kept until the map unloads (the backend's bottom-level structures,
built once). The instances: each frame, the structure (which stops a ray
and occludes nothing: the lightmaps and the field hold its occlusion) and
each dynamic object's meshes on its nodes as drawn (the top-level
structure, built again each frame). Not the static scenery the field and
its pages hold, nor a held weapon (world_proxies_source.h's instances).

Render-only: nothing here is read by the simulation, sent between machines
or saved. world_rt.c is the scene (its arithmetic testable alone:
tools/harness/tests/world_rt.c); what it reads of the game is
world_rt_source_* (port/linux/game/world_rt_game.c) and
world_proxies_source.h's instances and nodes.
*/

#ifndef WORLD_RT_H
#define WORLD_RT_H

struct world_proxies_frame;

/* ---------- constants */

enum
{
	/* the most meshes and instances (xgpu_stream.h's
	XGPU_STREAM_WORLD_AO_MESHES less the none, _INSTANCES_MAXIMUM) */
	WORLD_RT_MESHES = 4095,
	WORLD_RT_INSTANCES = 4096,
	/* the most nodes of a definition taken (world_proxies_source.h's) */
	WORLD_RT_NODES = 64,
	/* the most collision permutations of a node taken
	(world_proxies_source.h's) */
	WORLD_RT_PERMUTATIONS = 4,
	/* the most triangles of a mesh (XGPU_STREAM_WORLD_AO_MESH_TRIANGLES) */
	WORLD_RT_MESH_TRIANGLES = 1 << 20,
	/* an instance's kind (xgpu_stream.h's XGPU_WORLD_AO_INSTANCE_*) */
	WORLD_RT_STRUCTURE = 1,
	WORLD_RT_OBJECT = 2,
	/* (as drawn) the first-person weapon and hands (their own
	mask: no world ray meets them), and the local player's own body in
	first person (its own: it casts shadows, and nothing else sees it) */
	WORLD_RT_FIRST_PERSON = 4,
	WORLD_RT_PLAYER = 8,
	/* (with the ray-traced lighting) an object that casts no shadow
	(world_proxies_source_instance's shadowless): its own mask, which
	every ray takes but the pass's sun's dynamic one */
	WORLD_RT_SHADOWLESS = 16,
	/* the first-person models' owners (the skinned structures' keys: not
	their unit's, whose body has its own) */
	WORLD_RT_FIRST_PERSON_OWNER = 0xFFFFF0,
	/* the structure's render surfaces by their shader
	(world_rt_source_structure_mesh): opaque (shader_environment), traced
	as drawn; alpha-tested (shader_environment's flag: grates, fences,
	foliage cards; and the transparent shaders drawn as cutouts), traced as a second geometry whose rays
	test the alpha map's alpha (docs/rt-plan/1-geometry.md, stage 3); and
	transparent (water, glass and the other transparent shaders), left out */
	WORLD_RT_SURFACES_OPAQUE = 0,
	WORLD_RT_SURFACES_ALPHA_TESTED = 1,
	WORLD_RT_SURFACES_TRANSPARENT = 2,
	/* (an object's lamps alone: its additive transparent parts,
	world_rt_source_model_lamps) */
	WORLD_RT_SURFACES_ADDITIVE = 3,
	/* a material's maps (struct world_rt_source_material's maps) */
	WORLD_RT_MAP_BASE = 0,
	WORLD_RT_MAP_DETAIL,
	WORLD_RT_MAP_MULTIPURPOSE,
	WORLD_RT_MAP_BUMP,
	WORLD_RT_MAP_LIGHTMAP,
	WORLD_RT_MAPS,
	/* the skinned objects posed a frame at most (xgpu_stream.h's
	XGPU_WORLD_RT_POSES) */
	WORLD_RT_POSES = 512,
	/* the materials and textures' slots of a map (xgpu_stream.h's
	XGPU_WORLD_RT_MATERIALS, _TEXTURES: slot 0 none) */
	WORLD_RT_MATERIALS = 4096,
	WORLD_RT_TEXTURES = 512,
	/* a material's flags (xgpu_stream.h's XGPU_WORLD_RT_MATERIAL_*) */
	WORLD_RT_MATERIAL_ALPHA_TESTED = 1,
	WORLD_RT_MATERIAL_TWO_SIDED = 2,
	WORLD_RT_MATERIAL_TRANSPARENT = 4,
	/* (its reflection: xgpu_stream.h's XGPU_WORLD_RT_MATERIAL_MASK_*,
	_REFLECTION_*) */
	WORLD_RT_MATERIAL_MASK_BUMP = 0x100,
	WORLD_RT_MATERIAL_MASK_MULTIPURPOSE = 0x200,
	WORLD_RT_MATERIAL_REFLECTION_PERPENDICULAR_SHIFT = 16,
	WORLD_RT_MATERIAL_REFLECTION_PARALLEL_SHIFT = 24,
	WORLD_RT_MATERIAL_ROUGHNESS_SHIFT = 10,
	WORLD_RT_MATERIAL_ROUGHNESS_MASK = 0x1f,
	/* the scene as drawn's memory (world_rt.c's budget): a triangle's
	bytes, estimated (its share of a bottom-level structure, about 160
	before compaction on RADV, with its corners, material and vertices'
	attributes in the hit table), and the budget where the backend gives
	none (MB) */
	WORLD_RT_TRIANGLE_BYTES = 200,
	WORLD_RT_BUDGET_DEFAULT = 128,
};

/* the half side of an item's box with no collision geometry, a share of
its bounding radius (a cube of that radius's sphere's volume would be
0.81; the shapes are smaller than their spheres), and the largest such
item's bounding radius (world units). Anything else with none has no mesh:
an emitter's, a wire's or the sky's ring's sphere is not its shape */
#define WORLD_RT_BOX_SHARE 0.6f
#define WORLD_RT_BOX_LARGEST 1.0f

/* ---------- structures */

/* a render mesh's vertex as the game has it (world_rt_source_*_mesh): its
position (the mesh's space), normal, tangent and binormal, its base map's
coordinates, its lightmap's (the level's, two shorts of 1/32767), and its
nodes and the first's weight (a model's) */
struct world_rt_source_vertex
{
	float position[3], normal[3], tangent[3], binormal[3], uv[2];
	short lightmap_uv[2];
	short nodes[2];
	float weight;
};

/* a map of a material: a bitmap's tag index and its bitmap (NONE none) */
struct world_rt_source_map
{
	long bitmap;
	short index;
};

/* a render mesh's material as the game has it: its shader (its tag index)
and type, its flags (xgpu_stream.h's XGPU_WORLD_RT_MATERIAL_*), its alpha
test's cutoff, which of its maps that tests (WORLD_RT_MAP_*: -1 none) at
what scale of the coordinates, its detail map's scale, its maps, its mods'
material maps (xgpu_images.h: 0 none) */
struct world_rt_source_material
{
	long shader;
	unsigned long kind, flags;
	float alpha_cutoff;
	int alpha_map;
	float alpha_scale[2], detail_scale;
	struct world_rt_source_map maps[WORLD_RT_MAPS];
	unsigned int normal_image, orm_image;
	/* its self-illumination's colour (RGBA8, R low; 0 none: a model's) */
	unsigned int self_illumination;
	/* (a model's part) the region of the model it is in, its first
	permutation's (-1: none, the structure's) */
	short region;
};

/* a render mesh as the game has it: its counts (the first opaque_count
triangles opaque, the rest alpha-tested; left_out the transparent ones not
in it), and where the arrays are given (room for the counts a call without
them gave) its vertices, triangles' corners and materials (of its own
materials, below) */
struct world_rt_source_mesh
{
	long vertex_count, triangle_count, opaque_count, material_count, left_out;
	struct world_rt_source_vertex *vertices;
	unsigned long (*triangles)[3];
	unsigned short *triangle_materials;
	struct world_rt_source_material *materials;
};

/* a mesh as drawn as the renderer takes it (halo_world_rt_mesh; as
xgpu_stream.h's struct xgpu_world_rt_mesh, _vertex, _material, laid out
alike): the header, then the positions, corners, vertices, triangles'
materials (two bytes each, padded to four) and the materials named first */
struct world_rt_mesh_header
{
	unsigned int vertex_count, triangle_count, opaque_count, material_count;
	unsigned int pad[4];
};
struct world_rt_vertex
{
	float uv[2];
	unsigned int normal, tangent, lightmap_uv, flags;
};
struct world_rt_material
{
	unsigned int index, kind, flags;
	float alpha_cutoff;
	unsigned int alpha_map;
	float alpha_scale[2];
	unsigned int base_map, detail_map;
	float detail_scale;
	unsigned int multipurpose_map, bump_map, normal_map, orm_map, lightmap;
	unsigned int self_illumination;
};

/* a skinned object's pose this frame (halo_world_rt_poses; as
xgpu_stream.h's struct xgpu_world_rt_pose): its definition's skinned mesh,
its owner, its nodes, its key (the object's datum handle, whose skinned
structure the backend keeps: an owner may be 0, the baked scenery's), and
each node's
skinning transform, the model's space (the default pose) into the object's
first node's space (the instance's) */
struct world_rt_pose
{
	unsigned int mesh, owner, node_count, key;
	float transforms[WORLD_RT_NODES][3][4];
};

/* ---------- prototypes (port/linux/game/world_rt.c) */

/* each frame, from its first window (world_ao.c, with "ray_traced"): the
meshes made that the frame's objects need first, and its instances handed
to the renderer (halo_world_ao_mesh, _instances); frame the frame's number,
once a frame */
void world_rt_frame(unsigned long frame);
/* the map unloaded (or the setting turned off): every mesh let go of */
void world_rt_map_unloaded(void);
/* whether the last frame's scene held the object (a datum handle) with
meshes of its own (not the structure's, not an item's box): a ray meets its
shape, so the light-transport pass's sun shadow is cast by it */
int world_rt_object_traced(long object_index);
/* (display.ray_traced_lighting) the last frame's object lamps: each a
triangle of an object's self-illuminated surfaces where it glows, placed
with the object (world-space corners, wound to glow toward their normal),
its radiance (linear, the pass's units) and its object's owner; their
count */
long world_rt_lamps(float const (**corners)[9], float const (**radiance)[3], unsigned long const **owners);
/* the frame's lamps' sides, in world_rt_lamps' order: 1, or 2 where it glows
toward both (an additive part drawn from both sides) in the low bits (& 3),
and WORLD_RT_LAMP_ADDITIVE an additive part's (a glow drawn over its object);
NULL none */
#define WORLD_RT_LAMP_ADDITIVE 4
unsigned char const *world_rt_lamp_sides(void);

/* an object's owner among the instances (its datum index plus one; 0 none,
-1 an object index), which a pixel of its own leaves out */
unsigned long world_rt_owner(long object_index);
/* a mesh's triangles (nine floats each) turned to face out: each wound
counterclockwise seen from outside, by the sign of the volume they hold
(none flipped where it is under least, an open or flat mesh's); returns
that volume, after */
float world_rt_orient(float *triangles, long count, float least);
/* the box from -1 to 1 along each axis, its 12 triangles facing out */
void world_rt_box(float triangles[12 * 9]);
/* a node's frame as an instance's transform (a row-major 3x4 whose
columns are its axes, scaled, then its position) */
void world_rt_transform(struct world_proxies_frame const *frame, float transform[3][4]);
/* ... and the box's about a bounding sphere in that node's space (centre,
radius): its axes the frame's times radius * WORLD_RT_BOX_SHARE */
void world_rt_box_transform(struct world_proxies_frame const *frame, float const sphere[4], float transform[3][4]);
/* a unit vector as two snorm16s of its octahedral map (x low) */
unsigned long world_rt_octahedral(float const vector[3]);
/* a model triangle's node: its corners' major nodes' (two corners', else
the first's) */
int world_rt_triangle_node(struct world_rt_source_vertex const *a, struct world_rt_source_vertex const *b,
	struct world_rt_source_vertex const *c);
/* a row-major 3x4 transform's inverse (an affine one's: FALSE where it has
none), and two composed (out = a b: b first; out may be neither) */
int world_rt_inverse(float const in[3][4], float out[3][4]);
void world_rt_compose(float const a[3][4], float const b[3][4], float out[3][4]);
/* whether a model's vertices blend two nodes (its joints are skinned: a
vertex's weight of its first node well within 0 and 1, its nodes two) */
int world_rt_blended(struct world_rt_source_vertex const *vertices, long count);
/* a triangle's corners turned to face as its vertices' normals do
(counterclockwise seen from where they point) */
void world_rt_face_out(struct world_rt_source_vertex const *vertices, unsigned long triangle[3]);

/* ---------- what the scene reads of the game (port/linux/game/world_rt_game.c) */

/* the loaded structure BSP's index (-1 none) */
short world_rt_source_bsp(void);
/* its collision geometry's triangles, world space (nine floats each, into
triangles: NULL for the count alone, at most maximum): how many (0 none) */
long world_rt_source_structure(short bsp_index, float *triangles, long maximum);
/* its render mesh, world space: its lightmaps' materials' surfaces (no
invisible clip: nothing draws it, so the render geometry has none), the
opaque ones first, then the alpha-tested; each of its materials its own.
Its counts, then (arrays given) its data; its triangles (0 none) */
long world_rt_source_structure_mesh(short bsp_index, struct world_rt_source_mesh *mesh);
/* a definition's model's render mesh, its model's space (its default pose),
each vertex with its nodes: its regions' first permutations at one level of
detail (the second most detailed), the opaque parts first, then the
alpha-tested ones, the transparent ones counted (left_out). Its counts, then
(arrays given) its data; with inverses (room for WORLD_RT_NODES), each
node's default inverse, the model's space into the node's (a row-major
3x4); its triangles (0 none) */
long world_rt_source_model_mesh(long definition, struct world_rt_source_mesh *mesh, float (*inverses)[3][4]);
/* ... and its additive transparent parts alone (a Pelican's bay lights:
transparent shaders drawn adding to the frame), the ray-traced scene's
object lamps (world_rt.c), each material's base map the shader's first
map, its self-illumination white, its flags transparent; as
world_rt_source_model_mesh, counted where the arrays are not given */
long world_rt_source_model_lamps(long definition, struct world_rt_source_mesh *mesh);
/* a map's texture as the renderer has it now (its header: NULL while it is
not loaded, which this asks for) */
void const *world_rt_source_texture(struct world_rt_source_map const *map);
/* a map's 2D texture's texels (ARGB words) at its first level no larger
than largest each way, and that level's size: the caller frees them; NULL
where it cannot be read (world_rt.c's object lamps) */
unsigned long *world_rt_source_texels(struct world_rt_source_map const *map, unsigned long largest,
	unsigned long *width, unsigned long *height);
/* an object's shading as it is drawn this frame (render_interpolation's):
its function values and change colours A to D, the game's time (seconds),
and its regions' permutations and destroyed flags (bit per region) */
struct world_rt_source_shading
{
	float values[4], colours[4][3], time;
	unsigned char permutations[8];
	unsigned int destroyed;
};
/* (FALSE: no such object) */
int world_rt_source_shading(long id, struct world_rt_source_shading *shading);
/* a transparent generic shader (an object lamp's, drawn adding): its maps,
at most 4, and each one's coordinates' scale and offset (u scale, v scale,
u offset, v offset); 0 if its stages cannot be run here (none, or a cube
first map) */
int world_rt_source_glow_maps(long shader, struct world_rt_source_map maps[4], float transforms[4][4]);
/* ... what it adds at a point: its stages' combiners run as the pixel
shader runs them (shader_transparent_generic_preprocessor.c's registers),
on its maps' texels there (ARGB words), with an object's shading (NULL:
white change colours, functions and time 0); its colour, 0 to 1 */
void world_rt_source_glow(long shader, unsigned long const texels[4], struct world_rt_source_shading const *shading,
	float rgb[3]);
/* a definition's nodes (at most WORLD_RT_NODES), its bounding sphere in
its model's space (centre, radius) and whether it is an item (a weapon,
equipment or garbage); 0 if it has none */
short world_rt_source_definition(long definition, float sphere[4], int *item);
/* a node's collision geometry's triangles in its own space (as above), of
one of its permutations (the BSP of that index: collision_models.c): how
many (0 none); and how many permutations it has */
long world_rt_source_node(long definition, short node_index, short permutation, float *triangles, long maximum);
short world_rt_source_node_permutations(long definition, short node_index);
/* the first local player's first-person weapon (which 0) or hands (1) as
drawn since world_rt_source_first_person_next was last called: its model's
tag index (as a definition: world_rt_source_definition and _model_mesh take
it), the unit holding it, its nodes (world space, at most maximum); 0 none
drawn */
short world_rt_source_first_person(int which, long *model, long *unit, struct world_proxies_frame *frames, short maximum);
void world_rt_source_first_person_next(void);
/* a definition's tag's name (HALO_WORLD_AO_COUNTS' log) */
char const *world_rt_source_name(long definition);

#endif
