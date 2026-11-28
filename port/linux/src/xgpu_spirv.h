/*
XGPU_SPIRV.H

SPIR-V from the shader intermediate form (xgpu_shader.h), for the Vulkan
backend (port/linux/vulkan): xgpu_spirv.c. The programs are OpenCE's GLSL
emitter's, written as SPIR-V words directly rather than as GLSL for a
compiler: the shaders are made from pipeline keys
as the game first draws with them, and a GLSL compiler in the host would
cost a library of megabytes and milliseconds a shader there, where these
programs (no control flow but discarding, a few dozen kinds of operation)
are a walk over the form. spirv-val checks the words
(tools/spirv_corpus).

The conventions (SPIR-V 1.3, Vulkan 1.1):

- clip space is Vulkan's (z 0 to 1, the first row at the top of the
  framebuffer, y down): the game's position (Direct3D's, y up) has its y
  negated, so that the targets' first row is the Xbox's top one;
- vertex attributes are at location n for v<n>. A D3DCOLOR is
  VK_FORMAT_B8G8R8A8_UNORM (read as RGBA); NORMPACKED3
  is VK_FORMAT_R32_UINT, unpacked here; the unnormalized shorts
  (integer_attribute_mask) are VK_FORMAT_R16*_SINT, read as integers and
  converted here; everything else is read as a vec4, the missing components
  0, 0, 0, 1;
- the draw's values are uniform buffers in descriptor set
  XGPU_SPIRV_SET_DRAW: the vertex constants (192 vec4) at
  XGPU_SPIRV_BINDING_CONSTANTS, struct xgpu_spirv_uniforms at
  XGPU_SPIRV_BINDING_UNIFORMS (both stages), the enhanced dynamic lights'
  block and cells (struct xgpu_dynamic_lights_block, a usampler3D) and the
  visibility tests' counters (a storage buffer of uints) at theirs;
- each stage's texture is a combined image sampler at binding n of
  XGPU_SPIRV_SET_TEXTURES; the samplers have D3DTSS_MIPMAPLODBIAS
  (mipLodBias) and the border colors (VK_EXT_custom_border_color), or the
  shader draws the border as the Metal emitter's does
  (xgpu_spirv_options border);
- the stages hand on, at locations XGPU_SPIRV_LOCATION_*: the colors, the
  texture coordinates, fog, and with lighting the world normal, position,
  binormal and tangent (or, lit for each vertex with the ambient occlusion,
  the ambient and the point lights' light);
- the VR mode uses Vulkan multiview, with ViewIndex relative to the
  attachment view's first layer. A draw of one eye uses that eye's layer
  (its two layers when foveated) as the attachment view. The eyes and all
  other targets have their first row at the top: neither the eye positions
  nor layered texture lookups flip rows.
*/

#ifndef __HALO_LINUX_XGPU_SPIRV_H
#define __HALO_LINUX_XGPU_SPIRV_H

#define XGPU_SPIRV_SET_DRAW 0
#define XGPU_SPIRV_BINDING_CONSTANTS 0
#define XGPU_SPIRV_BINDING_UNIFORMS 1
#define XGPU_SPIRV_BINDING_DYNAMIC_LIGHTS 2
#define XGPU_SPIRV_BINDING_DYNAMIC_LIGHT_CELLS 3
#define XGPU_SPIRV_BINDING_SAMPLES 4
/* (the enhanced flashlight's shadow map: xgpu.h's XGPU_DYNAMIC_LIGHTS_FLASHLIGHT) */
#define XGPU_SPIRV_BINDING_FLASHLIGHT_SHADOW 5
#define XGPU_SPIRV_SET_TEXTURES 1
/* (a model's material maps, past the four stages: xgpu.h's
XGPU_TEXTURE_MODES_NORMAL_MAP, _ORM_MAP; 2D, at texture 0's coordinates) */
#define XGPU_SPIRV_BINDING_NORMAL_MAP 4
#define XGPU_SPIRV_BINDING_ORM_MAP 5
/* display.hdr: the pixel a draw blends over in its shader (its tag's
XGPU_HDR_SHADER_BLENDS, xgpu_hdr.h), the colour attachment read as input
attachment 0 at binding 0 (VK_KHR_dynamic_rendering_local_read) */
#define XGPU_SPIRV_SET_INPUT 2
/* (its binding of the colour target as a storage image, for the draws the
interlock orders: xgpu_spirv_options interlock) */
#define XGPU_SPIRV_BINDING_STORE 1
/* display.ambient_occlusion "world" (xgpu_stream.h): in a set of its own
(binding 0 unused), the structure's distance field, its atlas of bricks (a
combined image sampler, 3D, linear, clamped to its edge) and its table of
cells (3D, RGBA8, read with texelFetch), and the window's block (a uniform
buffer) */
#define XGPU_SPIRV_SET_OCCLUSION 3
/* (with ray queries) what a ray's hit reads (xgpu_vulkan_rt.c's hit set):
the hit table, a storage buffer of words, and XGPU_WORLD_RT_TEXTURES
textures' slots, sampled at an alpha-tested candidate */
#define XGPU_SPIRV_SET_RT_HITS 4
#define XGPU_SPIRV_BINDING_RT_HIT_TABLE 0
#define XGPU_SPIRV_BINDING_RT_HIT_TEXTURES 1
/* (its layout: the meshes' records, four words each, then the materials'
from XGPU_SPIRV_RT_HIT_MATERIALS, sixteen each; the textures' slots; the
level an alpha map is sampled at) */
#define XGPU_SPIRV_RT_HIT_MESHES 4096u
#define XGPU_SPIRV_RT_HIT_MATERIALS (4096u * 4u)
#define XGPU_SPIRV_RT_HIT_TEXTURES 512u
#define XGPU_SPIRV_RT_HIT_LEVEL 1.0f
#define XGPU_SPIRV_BINDING_WORLD_SDF 1
#define XGPU_SPIRV_BINDING_WORLD_AO 2
#define XGPU_SPIRV_BINDING_WORLD_SDF_TABLE 3
/* (the static scenery's pages of the level's occlusion: a 2D array of
bytes, sampled at the lightmap pass's lightmap coordinates) */
#define XGPU_SPIRV_BINDING_WORLD_AO_SCENERY 4
/* (the vehicles' occlusion fields' atlas: a 3D image of four halves, t
and v.xyz, sampled at a field's voxels' place) */
#define XGPU_SPIRV_BINDING_WORLD_AO_OBJECTS 5
/* (found once a pixel: the occlusion's texture, a float a pixel, read with
texelFetch at the pixel by the draws with
XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED) */
#define XGPU_SPIRV_BINDING_WORLD_AO_PIXELS 6
/* the passes after the pre-pass (xgpu_spirv_world_ao_pass), drawn as the
backend's own draws: the pass's struct xgpu_world_ao_pass at the start of
the uniforms (XGPU_SPIRV_SET_DRAW, XGPU_SPIRV_BINDING_UNIFORMS); the
pre-pass's inputs (an unsigned 2D image, read with texelFetch) at this
binding of XGPU_SPIRV_SET_TEXTURES; set XGPU_SPIRV_SET_OCCLUSION as the
surfaces have it */
#define XGPU_SPIRV_WORLD_AO_PASS_INPUTS 0
/* (and the window's previous frame's occlusion, a 2D float image read with
texelFetch, and the distance-field scene's cascades, a 3D RG8 snorm one,
linear, clamped: the dynamic objects' rays step through them) */
#define XGPU_SPIRV_WORLD_AO_PASS_HISTORY 1
#define XGPU_SPIRV_WORLD_AO_PASS_CASCADES 2
/* (the distance-field scene's cascades: a 3D RG8 snorm image, linear,
clamped: xgpu.h's XGPU_WORLD_CASCADE*) */
#define XGPU_SPIRV_BINDING_WORLD_CASCADES 7
/* (display.ambient_occlusion "ray_traced": the ray-traced scene's top-level
acceleration structure, which the occlusion's pass made with ray queries
reads: xgpu_stream.h's WORLD_AO_MESH, _INSTANCES) */
#define XGPU_SPIRV_BINDING_WORLD_AO_SCENE 8
/* (display.shadows "ray_traced": the dynamic lights' shadows' mask, which
the surfaces found once a pixel read, a channel a light: a 2D image of
four bytes, read by texel; xgpu.h's XGPU_DYNAMIC_LIGHTS_SHADOWED) */
#define XGPU_SPIRV_BINDING_LIGHT_SHADOWS 9
/* (display.rt_bounce: the bounced light the surfaces found once a pixel
add, a 2D image of RGBA16F read by texel, a 1x1 of none where it is not
traced: XGPU_SPIRV_BINDING_RT_BOUNCE, xgpu_spirv_rt_bounce_pass) */

/* the locations of what the vertex shader hands the pixel shader */
enum
{
	XGPU_SPIRV_LOCATION_D0, XGPU_SPIRV_LOCATION_D1,
	XGPU_SPIRV_LOCATION_T0, XGPU_SPIRV_LOCATION_T1, XGPU_SPIRV_LOCATION_T2, XGPU_SPIRV_LOCATION_T3,
	XGPU_SPIRV_LOCATION_FOG,
	XGPU_SPIRV_LOCATION_WORLD_NORMAL, XGPU_SPIRV_LOCATION_WORLD_POSITION,
	XGPU_SPIRV_LOCATION_WORLD_BINORMAL, XGPU_SPIRV_LOCATION_WORLD_TANGENT,
	/* (a model lit for each vertex, its ambient light apart: xgpu.h's
	XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX) */
	XGPU_SPIRV_LOCATION_LIGHT_AMBIENT, XGPU_SPIRV_LOCATION_LIGHT_POINT,
	/* (display.temporal_aa: the position this frame and the frame before,
	x, y and w as the screen's conversion has them: xgpu_spirv_options'
	motion) */
	XGPU_SPIRV_LOCATION_MOTION_CURRENT, XGPU_SPIRV_LOCATION_MOTION_PREVIOUS,
};

/* the draw's values both stages read, laid out alike in C and in the
shaders' uniform block (std140: vec4s, and floats in fours): the backend's
draw uniforms, the per-pixel lighting's model lights, the blend color the
source factor takes in the shader, and each stage's border color */
struct xgpu_spirv_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size, screen_offset, alpha_reference, screen_points;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float blend_constant[4];
	/* each stage's border color (RGBA, 0 to 1), and whether its sampler
	filters linearly (1) or takes the nearest texel (0), for the stages
	whose border the shader draws (xgpu_spirv_options border) */
	float texture_border[4][4];
	float texture_border_linear[4];
	float model_lights[XGPU_MODEL_LIGHT_COUNT][4];
	/* the clip and screen corrections, as the stream delivers them;
	the matrices are column-major (four consecutive vec4 columns) */
	float eye_correction[2][16];
	float screen_correction[2][4];
	/* each foveated layer's scale and offset: fovea_view in Vulkan clip
	space (x/y down from the top left after the y negation), fovea_sample
	in texture space (its offset in texels). The whole eyes' are identity. */
	float fovea_view[4][4];
	float fovea_sample[4][4];
	/* this draw's counter in the visibility tests' uint storage buffer;
	its constant depth bias, where depth is reversed (xgpu_spirv_options
	reversed_depth: xgpu_reversed_depth_bias) */
	float visibility_counter, depth_bias, vr_unused[2];
	/* the VR mode's effects (xgpu_spirv_options effect): for each view, the
	world point (homogeneous) at a pixel and depth, as columns (the point is
	unproject[0] x + unproject[1] y + unproject[2] depth + unproject[3],
	gl_FragCoord's x and y), and its eye in the world (port/linux/vulkan/
	xgpu_vulkan_effects.h) */
	float effect_unproject[4][4][4];
	float effect_eye[4][4];
	/* display.ambient_occlusion "world" (xgpu.h): the draw's object, its
	owner slot among the window's proxies (-1 none) and its reference, then
	two spare; declared in the pixel shaders that read it alone, past the
	others, so that their words are as they were */
	float world_ao[4];
	/* display.dynamic_lights "enhanced": the lights' tint in the
	structure's lightmap pass, ps_final_c0's color in linear light (to the
	2.2), which the backend works out; declared (past world_ao) in those
	pixel shaders alone */
	float dynamic_light_tint[4];
};

/* what the SPIR-V is written for (xgpu_spirv.c):
- count_samples: the pixel shaders of a visibility test count their samples
  themselves, an atomic counter at XGPU_SPIRV_BINDING_SAMPLES (else the
  backend counts with occlusion queries, and the shaders do nothing);
- border: for each stage, the axes (bit 0 u, 1 v, 2 w) whose border color
  the shader draws (its sampler clamping to the edge) where the sampler
  cannot (no VK_EXT_custom_border_color): part of the pipeline, as the
  pixel key is;
- multiview: the vertex shader's XGPU_MULTIVIEW_* mode (the pixel
  program carries its own); foveated: four layers, periphery and inset
  for each eye, rather than the two eyes alone;
- the pixel shaders' debugging settings, as the GLSL's;
- effect: the VR mode's effects (XGPU_SPIRV_EFFECT_*, below), and
  effect_fade how their fade is drawn under the pipeline's blend
  (XGPU_SPIRV_FADE_*: the backend's choice); for the glows' shaders, any effect draws them
  as the light the bolt scatters toward each eye;
- input_samples: the samples of the colour target a display.hdr draw that
  blends in its shader reads (XGPU_SPIRV_SET_INPUT): more than 1, each
  sample its own (sample shading);
- reversed_depth: depth reversed (xgpu_backend.h, "Reversed depth"): the
  vertex shaders write w - z (from the projection's rows where the program
  has them: xgpu_vertex_program_depth_rows; in an eye's view, with its
  correction's rows) and add the uniforms' depth_bias, a z-sprite writes
  its depth reversed, the glows and the VR mode's effects read the scene's
  depth and their own reversed (their unprojections stay the game's). */
enum { XGPU_SPIRV_INTERLOCK_NONE, XGPU_SPIRV_INTERLOCK_PIXEL, XGPU_SPIRV_INTERLOCK_SAMPLE };

struct xgpu_spirv_options
{
	BOOL count_samples;
	unsigned char border[4];
	BOOL debug_texture0, debug_flat;
	int multiview;
	BOOL foveated;
	int effect, effect_fade;
	int input_samples;
	/* how a display.hdr draw that blends in its shader is ordered with the
	draw's own triangles over the same pixel (XGPU_SPIRV_INTERLOCK_*): by
	the device's fragment shader interlock (SPV_EXT_fragment_shader_
	interlock), the pixel's (PIXEL) or, with sample shading, each sample's
	(SAMPLE): the target read and written as a storage image
	(XGPU_SPIRV_BINDING_STORE) in the triangles' order, the channels of
	interlock_mask (the pipeline's colour mask, bit n channel n) written,
	the depth and stencil tests early (the draw must write neither, nor
	replace its depth), and the pipeline writing no colour itself; NONE as
	an input attachment, where the device orders the reads itself
	(rasterization order) or cannot */
	int interlock, interlock_mask;
	BOOL reversed_depth;
	/* the device's subgroups have their bitwise operations in pixel shaders
	(Vulkan 1.1's subgroup arithmetic in the fragment stage):
	display.ambient_occlusion "world"'s proxies walked a subgroup at a time
	(world_proxies_occlusion), each shape's place and kind then the same for
	all of its pixels */
	BOOL subgroups;
	/* display.ambient_occlusion "ray_traced": the occlusion's pass traces
	the dynamic objects' rays by the device's ray queries (SPV_KHR_ray_query:
	xgpu_spirv_world_ao_pass) through the scene at
	XGPU_SPIRV_BINDING_WORLD_AO_SCENE */
	BOOL ray_queries;
	/* display.temporal_aa: the draw writes its motion as the second colour
	attachment (location 1: the previous position less the current, as a
	fraction of the view, x right, y down; its depth; 1), its vertex
	program run again on the previous frame's registers, which the
	constants' block holds after the 192 (384 vec4s then); not with views */
	BOOL motion;
	/* the light-transport pass on (display.ray_traced_lighting, or HALO_LT_E1:
	port/linux/vulkan/xgpu_vulkan_lt.h): the surfaces add its light, the
	pre-pass's twins write its guides, and the enhanced dynamic lights take
	its light model (xgpu.h); off, all as before it */
	BOOL light_transport;
	/* display.rt_reflections: the vertex shader hands its position (input
	0, the level's vertices being in the world) on as the world position
	(XGPU_SPIRV_LOCATION_WORLD_POSITION), where its lighting does not
	(xgpu_spirv_vertex_world_position), for a pixel shader that traces its
	reflection (xgpu_shader.h's rt_reflection) */
	BOOL world_position_input;
};

/* ---------- the VR mode's effects

The game's effects (particles, contrails, weather: its effect shaders),
drawn into both eyes at once (the pixel key's vr_effect: d3d8_device.c,
halo_vr_effect_depth):

- XGPU_SPIRV_EFFECT_SOFT: soft. Each pixel finds the world point it draws
  and the scene's behind it (the depth buffer, texture
  XGPU_SPIRV_EFFECT_DEPTH_BINDING, an array of the views' layers sampled
  with texelFetch; the uniforms' effect_unproject) and fades the effect out
  as the two meet, over a distance scaled to the effect's sprite (its
  radius, the thickness map's green, in the world by the texels' size
  there), so that it meets walls and floors without a hard line.
- XGPU_SPIRV_EFFECT_THICK: soft and thick. The sprite's thickness (the map
  at XGPU_SPIRV_EFFECT_THICKNESS_BINDING, baked from its bitmap's alpha:
  xgpu_vulkan_effects.h) is a bulge toward the eyes: each eye's ray meets
  the bulge's front where the sprite's texel there is drawn (one parallax
  step: the bulge is smooth and the eyes' rays near the sprite's normal),
  so each eye samples other texels and the effect has depth inside it; the
  fade is measured from the bulge's front, through its thickness.

The pixel program must sample texture 0 in 2D (the effect's bitmap) and
not textures 2 and 3, whose bindings the depth and thickness take
(xgpu_spirv_effect_possible), drawn into both eyes, or on a flat screen
into no views (display.soft_particles: soft alone; the depth then one
layer, a 2D image, and the uniforms' first view). The fade is drawn as
the blend's own nothing (XGPU_SPIRV_FADE_*): alpha to 0 over an alpha
blend, colour to 0 when added, to white when multiplied, to grey when
multiplied twice. */
enum
{
	XGPU_SPIRV_EFFECT_NONE, XGPU_SPIRV_EFFECT_SOFT, XGPU_SPIRV_EFFECT_THICK,
};
enum
{
	XGPU_SPIRV_FADE_NONE, XGPU_SPIRV_FADE_ALPHA, XGPU_SPIRV_FADE_COLOR, XGPU_SPIRV_FADE_COLOR_ALPHA,
	XGPU_SPIRV_FADE_TO_WHITE, XGPU_SPIRV_FADE_TO_GREY,
};
#define XGPU_SPIRV_EFFECT_DEPTH_BINDING 2
#define XGPU_SPIRV_EFFECT_THICKNESS_BINDING 3
/* the bulge's height at a sprite's middle, of its radius (a sphere's is
1). Smoke is denser in its middle than at its silhouette, so the front of
what is seen of it lies nearer its middle than a ball's surface would: a
sphere's depth made puffs look like balloons, half of it keeps them round
without a hard surface. */
#define XGPU_SPIRV_EFFECT_THICKNESS 0.5f
/* the fade's distance: of the sprite's radius, and the least (world units,
6 cm, and of the distance from the eye: 24-bit depth's steps there, with the
game's near plane, are smaller), and the most an eye's parallax moves a
texel (of the radius) */
#define XGPU_SPIRV_EFFECT_SOFTNESS 0.5f
#define XGPU_SPIRV_EFFECT_SOFTNESS_LEAST 0.02f
#define XGPU_SPIRV_EFFECT_SOFTNESS_DISTANCE 0.002f
#define XGPU_SPIRV_EFFECT_PARALLAX_MOST 0.5f

struct xgpu_pixel_program;

/* the program can be drawn as an effect (above) */
BOOL xgpu_spirv_effect_possible(const struct xgpu_pixel_program *program);

/* a module's words (SPIR-V's own: 32 bits each), malloc'd, count of them;
the vertex constants it reads (bit n of word n / 64: c[n], counted from
c[-96]), every one where it indexes them with a0 */
struct xgpu_spirv_module
{
	unsigned int *words;
	unsigned long count;
	unsigned long long constants_read[3];
};

/* the vertex program (xgpu_shader.h) as a vertex shader, entry point
"main": packed_attribute_mask's attributes are NORMPACKED3,
integer_attribute_mask's unnormalized shorts; with lighting (else NULL), the
normal and world position go to the pixel shader too, as the GLSL's.
FALSE if out of memory. */
BOOL xgpu_spirv_vertex_shader(const struct xgpu_vertex_program *program, unsigned long packed_attribute_mask,
	unsigned long integer_attribute_mask, const struct nv2a_vertex_lighting *lighting,
	const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* the vertex inputs (bit n v<n>) the vertex shader of a program and its
lighting reads: the pipeline's vertex layout has these alone */
unsigned long xgpu_spirv_vertex_inputs(const struct xgpu_vertex_program *program,
	const struct nv2a_vertex_lighting *lighting);
/* the vertex shader of that lighting hands the pixel its world position
(XGPU_SPIRV_LOCATION_WORLD_POSITION) */
BOOL xgpu_spirv_vertex_world_position(const struct nv2a_vertex_lighting *lighting);

/* the pixel program (xgpu_shader.h) as a fragment shader, entry point
"main". FALSE if out of memory. */
BOOL xgpu_spirv_pixel_shader(const struct xgpu_pixel_program *program, const struct xgpu_spirv_options *options,
	struct xgpu_spirv_module *module);

/* display.ambient_occlusion "world" found once a pixel (xgpu_stream.h's
WORLD_AO_PREPASS): the passes after the pre-pass, entry point "main": the
vertex shader covering the viewport with one triangle (pass 0) and the
occlusion (1: a vec4 a texel, each 2x2 block's nearest pixel's at a quarter
of the pixels: the occlusion, the distance, the normal octahedral). Their bindings:
XGPU_SPIRV_WORLD_AO_PASS_*. options: subgroups and ray_queries alone (the
occlusion's rays through the ray-traced scene: the pass's history[3] 2).
FALSE if out of memory. */
enum
{
	XGPU_SPIRV_WORLD_AO_VERTEX,
	XGPU_SPIRV_WORLD_AO_OCCLUSION,
};
BOOL xgpu_spirv_world_ao_pass(int pass, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* display.shadows "ray_traced" (xgpu_backend.h's xgpu_backend_rt_shadows):
its pass's pixel shader, drawn with world_ao_pass's vertex shader (the
triangle over the viewport): the scene's depth at texture 0, a header of
8 vec4 at the uniforms' binding (the unprojection's rows from the
framebuffer's x, y and depth to the world's homogeneous point; the camera
and the casters' count), the casters (2 vec4 each) at the constants', the
ray-traced scene at the occlusion set's; its colour the light the pixel
keeps, which the blend multiplies it by */
BOOL xgpu_spirv_rt_shadows_pass(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* the ray-traced scene's instances' masks (xgpu_stream.h's
XGPU_WORLD_AO_INSTANCE_*): the structure, objects, the first-person weapon
(drawn in a view of its own: no world ray meets it), and the local
player's own body in first person (which casts shadows, and nothing else
sees); the rays' masks: the occlusion's (the structure and objects), the
shadows' (and the player's body) */
#define XGPU_SPIRV_RT_MASK_AO 0x03u
#define XGPU_SPIRV_RT_MASK_SHADOWS 0x0Bu
/* (display.rt_reflections' rays: the structure, objects and the player's
own body, who sees themself in a polished floor) */
#define XGPU_SPIRV_RT_MASK_REFLECTIONS 0x0Bu

/* the ray-traced scene's skinning (xgpu_vulkan_rt.c): a compute shader of
XGPU_SPIRV_RT_SKIN_GROUP invocations a group, one a vertex (x) of a job
(y), its set 0 three storage buffers of words: the skinned meshes' rest
vertices (five words each: position, nodes, weight), the frame's jobs
(four words each: the mesh's first rest word, its vertices, its matrices'
first word in the same buffer, the first word its positions go to) and
matrices (twelve words a node, at most XGPU_SPIRV_RT_SKIN_NODES), and the
skinned positions (three words a vertex) */
#define XGPU_SPIRV_RT_SKIN_GROUP 64u
#define XGPU_SPIRV_RT_SKIN_NODES 64u
#define XGPU_SPIRV_RT_SKIN_RESTS 0
#define XGPU_SPIRV_RT_SKIN_FRAME 1
#define XGPU_SPIRV_RT_SKIN_OUT 2
BOOL xgpu_spirv_rt_skin(struct xgpu_spirv_module *module);

/* display.shadows "ray_traced": the dynamic lights' shadows (xgpu.h's
XGPU_DYNAMIC_LIGHTS_SHADOWED), passes drawn with world_ao_pass's vertex
shader after the pre-pass, its uniforms (struct xgpu_world_ao_pass: the
camera, the corners, the frame) and its inputs at
XGPU_SPIRV_WORLD_AO_PASS_INPUTS, the enhanced lights' block and cells at the
draw set's, the occlusion set (its block's camera; the ray-traced scene).
_TRACE: each pixel's view of each shadowed light's disk, a few rays by ray
queries (options' ray_queries), a channel a light; _FILTER: the trace's mask
(at XGPU_SPIRV_WORLD_AO_PASS_HISTORY) averaged over the 3x3 pixels about
each of the same surface (where no temporal technique accumulates it);
_FLASHLIGHT: the enhanced flashlight's shadow map (xgpu.h's
XGPU_DYNAMIC_LIGHTS_FLASHLIGHT), a ray a texel from the light along the
texel's direction in its gel's frame, its red the distance to what the ray
met (the light's radius where none), the light's owner and the
first-person models and the local player's own body left out.
FALSE if out of memory. */
enum
{
	XGPU_SPIRV_LIGHT_SHADOWS_TRACE,
	XGPU_SPIRV_LIGHT_SHADOWS_FILTER,
	XGPU_SPIRV_LIGHT_SHADOWS_FLASHLIGHT,
};
BOOL xgpu_spirv_light_shadows_pass(int pass, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* display.rt_bounce (docs/rt-plan/3-gi.md, stage 3;
port/linux/vulkan/xgpu_vulkan_gi.c): the dynamic lights' bounced light,
passes drawn with world_ao_pass's vertex shader after the pre-pass, its
uniforms (struct xgpu_world_ao_pass) and its inputs at
XGPU_SPIRV_WORLD_AO_PASS_INPUTS, the passes' block (XGPU_RT_BOUNCE_BLOCK
vec4s, below) at the draw set's constants, the occlusion set (its block's
camera; the ray-traced scene) and the hit set. The cache: a grid of
XGPU_RT_BOUNCE_GRID_* cells of XGPU_RT_BOUNCE_BLOCK_ORIGIN's w world units
about the camera, its cells kept where they are in the world (each at its
cell's coordinates modulo the grid's), six faces a cell (+x, -x, +y, -y,
+z, -z), laid out as a 2D image of RGBA16F, a cell's face at (x + GRID_X
face, y + GRID_Y z): the light a surface facing that way there receives of
the bounce (its mean radiance over the cosine-weighted hemisphere, the
enhanced lights' units), and the weight of its history.
_TRACE: one pixel of each XGPU_RT_BOUNCE_TILE square (a different one
each frame) casts one cosine-weighted ray through the ray-traced scene
(options' ray_queries); the surface it meets, lit by one of the block's
lights (chosen by its share, its shadow ray through the structure) times
its base map's color, is the sample (its alpha 1 where the pixel is
drawn); into an image a texel a square. _RESOLVE: each cell's face, the
samples about where its middle is seen that lie in it, weighted by how
squarely they face its way, blended into its history (at
XGPU_SPIRV_WORLD_AO_PASS_HISTORY; the samples at XGPU_SPIRV_RT_BOUNCE_SAMPLES),
into the new cache. _GATHER: each pixel's light from the cache, its eight
cells' faces it faces, into an image of the window's size, which the
surfaces found once a pixel add to the dynamic lights' (the occlusion set's
XGPU_SPIRV_BINDING_RT_BOUNCE). FALSE if out of memory. */
enum
{
	XGPU_SPIRV_RT_BOUNCE_TRACE,
	XGPU_SPIRV_RT_BOUNCE_RESOLVE,
	XGPU_SPIRV_RT_BOUNCE_GATHER,
};
#define XGPU_SPIRV_BINDING_RT_BOUNCE 10

/* The one light model's forward receivers (~/dev/rt2-realtime-design.md,
"One light model for every receiver"): with the light-transport pass on,
a draw lit by the enhanced dynamic lights that reads no pass light of its
own (the first-person weapon and hands, a blended or self-illuminated
model's or structure's pass: none drawn into the pre-pass) evaluates the
pass's direct light itself, from the pass's light table (xgpu_vulkan_lt.c's
lt_table, lt/e1.frag's light_table), bound to the occlusion set here: an
RGBA32F image XGPU_SPIRV_LT_TABLE_TEXELS wide, a row a light, the block's
lights' rows by their index in the block, the suns' from
XGPU_SPIRV_LT_SUN_ROW, and the view's shadowed flashlight (the pass's own
E1 light, which no block row carries for the pass) at
XGPU_SPIRV_LT_FLASHLIGHT_ROW. Texel 4's w is the row's kind (0 a point, 1 a
spot, 2 a panel, 3 a sun), less than 0 where the pass does not light it:
XGPU_SPIRV_LT_KIND_CLASSIC (the classic sum still adds it: a models-only
light) or XGPU_SPIRV_LT_KIND_FLASHLIGHT (the block's row of the view's
flashlight, lit from its own row). Texel 6's w is 1 where texel 6's xyz is
a gel's origin apart from the emitter (the flashlight's). */
#define XGPU_SPIRV_BINDING_LT_TABLE 11
#define XGPU_SPIRV_LT_TABLE_TEXELS 8
#define XGPU_SPIRV_LT_SUN_ROW 128
#define XGPU_SPIRV_LT_SUNS 8
#define XGPU_SPIRV_LT_FLASHLIGHT_ROW (XGPU_SPIRV_LT_SUN_ROW + XGPU_SPIRV_LT_SUNS)
#define XGPU_SPIRV_LT_TABLE_ROWS (XGPU_SPIRV_LT_FLASHLIGHT_ROW + 1)
#define XGPU_SPIRV_LT_KIND_CLASSIC (-1)
#define XGPU_SPIRV_LT_KIND_FLASHLIGHT (-2)

/* The light-transport pass's guides (contract C1, ~/dev/rt2-realtime-design.md
section 1.1), with HALO_LT_E1 alone: the pre-pass's twins keep their
pipelines' dynamic lights (so they shade their bump or normal map's normal
as the surfaces do) and write, beside their inputs, a second RGBA32Uint
target (the pass's second colour attachment, location
XGPU_SPIRV_GUIDES_LOCATION), a pixel each:
- x: the shaded normal (the bump map's on the structure, the normal map's on
  a model with one, else the interpolated one), octahedral, packSnorm2x16;
- y: the geometric (interpolated vertex) normal, the same;
- z: the material id (low 16 bits; 0 not known yet), the roughness (bits
  16-23, 0 to 255; 255 where the draw has none) and the specular mask (bits
  24-31: 255 where the draw has a reflection stage, else 0);
- w: the motion: where the point is less where it was the frame before, as
  a fraction of the window (its corners' width and height), x right and y
  down, the jitter left out (both positions the vertex program's with this
  frame's, xgpu_spirv.c's motion_screen); stored as
  packSnorm2x16(motion / XGPU_SPIRV_GUIDES_MOTION_RANGE) ^
  XGPU_SPIRV_GUIDES_MOTION_FLIP (fixed point: a step of 1/131068 of the
  window, 0.015 px at 1920, the same for any motion; a half float's step
  grows with it). The frame before's from the previous frame's registers
  (192 to 383: its camera, a skinned or moving object's nodes; xgpu_stream.h's
  MOTION, phase NONE where only these are kept). XGPU_SPIRV_GUIDES_MOTION_NONE
  (0, which no motion stores: packSnorm2x16 never gives -32768): none (a
  point behind either eye, a move of more than the range, or a twin without
  motion: the cameras' reprojection stands for it).
A pixel the pre-pass drew no twin at has none (its inputs say so: their
PRESENT bit). */
#define XGPU_SPIRV_GUIDES_LOCATION 1
#define XGPU_SPIRV_GUIDES_MATERIAL_MASK 0xFFFFu
#define XGPU_SPIRV_GUIDES_ROUGHNESS_SHIFT 16
#define XGPU_SPIRV_GUIDES_SPECULAR_SHIFT 24
#define XGPU_SPIRV_GUIDES_ROUGHNESS_NONE 255u
#define XGPU_SPIRV_GUIDES_MOTION_NONE 0u
#define XGPU_SPIRV_GUIDES_MOTION_FLIP 0x80008000u
#define XGPU_SPIRV_GUIDES_MOTION_RANGE 0.25f
#define XGPU_SPIRV_RT_BOUNCE_SAMPLES 3
#define XGPU_RT_BOUNCE_GRID_X 64
#define XGPU_RT_BOUNCE_GRID_Y 64
#define XGPU_RT_BOUNCE_GRID_Z 32
#define XGPU_RT_BOUNCE_CACHE_WIDTH (XGPU_RT_BOUNCE_GRID_X * 6)
#define XGPU_RT_BOUNCE_CACHE_HEIGHT (XGPU_RT_BOUNCE_GRID_Y * XGPU_RT_BOUNCE_GRID_Z)
#define XGPU_RT_BOUNCE_CELL 0.25f
#define XGPU_RT_BOUNCE_TILE 4
/* a bounce ray's reach and its start off the surface (world units); the
base map's level a hit reads (its average, more or less), and its color
at most (none sends on all it gets) or where it has none */
#define XGPU_RT_BOUNCE_REACH 4.0f
#define XGPU_RT_BOUNCE_BIAS 0.02f
#define XGPU_RT_BOUNCE_ALBEDO_LEVEL 4.0f
#define XGPU_RT_BOUNCE_ALBEDO_MOST 0.9f
#define XGPU_RT_BOUNCE_ALBEDO_NONE 0.35f
/* the bounce's strength (HALO_RT_BOUNCE_STRENGTH for looking): 2, the gain
the level's own bake fits over one bounce off the same albedo indoors (a10's
2.0, docs/rt-plan/4-path-tracing.md's static fit; Blood Gulch 3.2 and b30
4.0 with their sky), so that a dynamic light bounces as much as the bake's
lights do: several bounces (1 / (1 - albedo), 1.4 to 2 for the levels'
0.3 to 0.5) and the detail maps this albedo leaves out */
#define XGPU_RT_BOUNCE_STRENGTH 2.0f
/* the passes' block (vec4s): the grid's first cell (its coordinates) and
the cell's size; the last frame's first cell and 1 where its cache is
this one's; the frame's number, the history's share kept, its most, and
a cell's fading where nothing is seen of it; the rows that take a point's
offset from the eye to (s, s x, s y) of the screen (the inverse of the
camera's [1], [2], [3] as columns); the eye and the lights' count; the
samples' square, the samples' image's width and height, the strength;
then each light's three: position and reach, linear color and cone
offset, axis and cone scale */
enum
{
	XGPU_RT_BOUNCE_BLOCK_ORIGIN,
	XGPU_RT_BOUNCE_BLOCK_PREVIOUS,
	XGPU_RT_BOUNCE_BLOCK_TIMES,
	XGPU_RT_BOUNCE_BLOCK_UNPROJECT,
	XGPU_RT_BOUNCE_BLOCK_EYE = XGPU_RT_BOUNCE_BLOCK_UNPROJECT + 3,
	XGPU_RT_BOUNCE_BLOCK_SIZES,
	XGPU_RT_BOUNCE_BLOCK_LIGHTS,
	XGPU_RT_BOUNCE_BLOCK = XGPU_RT_BOUNCE_BLOCK_LIGHTS + 3 * 8,
};
#define XGPU_RT_BOUNCE_BLOCK_LIGHTS_MOST 8
BOOL xgpu_spirv_rt_bounce_pass(int pass, const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* the enhanced dynamic lights' glows (xgpu_backend.h's
xgpu_backend_dynamic_lights_halos):
a camera-facing quad about each light, six vertices of a triangle list,
added to the 3D view and faded against the depth the scene was drawn with.
The vertices: v<XGPU_SPIRV_HALO_MIDDLE> the light's middle and the glow's
size, v<XGPU_SPIRV_HALO_COLOR> its color, v<XGPU_SPIRV_HALO_CORNER> the
corner (-1 or 1 each way). The vertex constants: the world's projection
and the screen conversion (c[-96] to c[-93], c[-38], c[-37]) at
XGPU_SPIRV_HALO_PROJECTION, the camera's right and up at
XGPU_SPIRV_HALO_CAMERA_AXES, and the depth for a distance and the softness
at XGPU_SPIRV_HALO_DEPTH (xgpu_vulkan_stream.c's halo_depth_parameters).
The uniforms are struct xgpu_spirv_uniforms (the viewport's, the screen
offset, the eyes' and insets' corrections); the scene's depth is texture 0,
sampled with texelFetch (an array of the views' layers with multiview).
options: multiview (XGPU_MULTIVIEW_EYES or none), foveated and effect alone.

With effect (the VR mode's eyes), each glow is the light its bolt
scatters toward each eye from a haze about it, worked out for each pixel
along that eye's ray: the haze's density falls off from the bolt as the
card's glow does across it (a wide Gaussian, less its value at the glow's
edge, and a narrow hot core), and its integral along the ray is closed
(each Gaussian's is an error function's difference), from the eye to the
scene's surface behind the pixel. Unhidden, a ray passing the bolt at a
fraction r of the glow's size gets exactly the card's glow at r; a wall
through the glow hides the part behind it, in each eye as it should. The
quad drawn covers the haze's outline from the head (a quad about the bolt
facing the head's centre, or, with the head in the haze, one across the
view). Its vertex constants, past the card's: each view's clip transform
(rows: XGPU_SPIRV_GLOW_CLIP + 4 view), its unprojection (columns:
XGPU_SPIRV_GLOW_UNPROJECT + 4 view) and eye (XGPU_SPIRV_GLOW_EYE + view),
the head's centre and the near distance the quad across the view is drawn
at (XGPU_SPIRV_GLOW_HEAD), and the head's right, up and forward
(XGPU_SPIRV_GLOW_AXES); port/linux/vulkan/xgpu_vulkan_effects.h. */
enum
{
	XGPU_SPIRV_HALO_MIDDLE, XGPU_SPIRV_HALO_COLOR, XGPU_SPIRV_HALO_CORNER,
};
#define XGPU_SPIRV_HALO_PROJECTION 0
#define XGPU_SPIRV_HALO_CAMERA_AXES 6
#define XGPU_SPIRV_HALO_DEPTH 8
#define XGPU_SPIRV_GLOW_CLIP 16
#define XGPU_SPIRV_GLOW_UNPROJECT 32
#define XGPU_SPIRV_GLOW_EYE 48
#define XGPU_SPIRV_GLOW_HEAD 52
#define XGPU_SPIRV_GLOW_AXES 53

BOOL xgpu_spirv_halo_vertex_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);
BOOL xgpu_spirv_halo_pixel_shader(const struct xgpu_spirv_options *options, struct xgpu_spirv_module *module);

/* the distance-field scene's ray step (port/linux/include/world_cascades.h's
world_trace_ray) alone, for tools/test_world_trace.py: a fragment shader,
entry point "main", tracing its ray from its inputs (location 0 the origin,
1 the direction, 2 t_max and the mask, 3 to 6 the cascades' vec4s, 7 and 8
the field's block vec4s) through the cascades' texture
(XGPU_SPIRV_SET_OCCLUSION, XGPU_SPIRV_BINDING_WORLD_CASCADES) and the
level's field (XGPU_SPIRV_BINDING_WORLD_SDF, _WORLD_SDF_TABLE), writing the
hit (1 or 0), t and kind.
FALSE if out of memory. */
BOOL xgpu_spirv_world_trace_test(struct xgpu_spirv_module *module);

#endif
