/*
XGPU.H

What the renderer's parts share, in no GPU API's terms: the vertex
programs' and pixel shaders' keys and lighting (decoded by nv2a_vsh.c and
nv2a_psh.c), the enhanced dynamic lights' layout, the Xbox's texture
formats (xbox_textures.c) and generated source text. The front end
(d3d8_device.c) and every backend include it; the interface between front
end and backend is xgpu_backend.h.
*/

#ifndef __HALO_LINUX_XGPU_H
#define __HALO_LINUX_XGPU_H

#include "platform.h"

/* ---------- generated source text */

struct xgpu_text
{
	char *buffer;
	unsigned long length;
	unsigned long capacity;
};

void xgpu_text_append(struct xgpu_text *text, const char *format, ...) __attribute__((format(printf, 2, 3)));

/* ---------- vertex shaders */

#define XGPU_VERTEX_ATTRIBUTE_COUNT 16
#define XGPU_VERTEX_CONSTANT_COUNT 192
/* D3D constant register -96 is hardware register 0 */
#define XGPU_VERTEX_CONSTANT_BIAS 96

/* where one of the game's model lighting programs (d3d8_device.c
halo_vertex_shader_lighting) has the normal, and the world position, that it
lights the diffuse color by: the temporary register that holds each before
the instruction given */
struct nv2a_vertex_lighting
{
	/* 1 by the ambient and distant lights, 2 by the point lights too */
	int lights;
	unsigned long normal_instruction, normal_register;
	/* (with the point lights only) */
	unsigned long position_instruction, position_register;
	/* the world position the program projects to the screen (the
	temporary its clip position is computed from, before the instruction
	given), for the enhanced dynamic lights (display.dynamic_lights): with
	the point lights, the same as theirs; world_found FALSE if none */
	BOOL world_found;
	unsigned long world_instruction, world_register;
	/* (the backends': lit for each vertex, the ambient and distant lights'
	sum and the point lights' handed on apart, for the pixel shader's
	ambient occlusion: XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX) */
	BOOL split;
};

/* the lights value of a structure's (the environment's) lightmap pass lit
by the enhanced dynamic lights: its vertices' world position, normal,
binormal and tangent (v0 to v3) handed on as they are */
#define XGPU_ENVIRONMENT_LIGHTING 3

struct xgpu_vertex_program;

/* finds them in a model lighting program (decoded: xgpu_shader.h), checking
that it lights its diffuse color as xgpu_spirv.c does for each pixel; FALSE if
it does not */
BOOL nv2a_vertex_shader_lighting(const struct xgpu_vertex_program *program, struct nv2a_vertex_lighting *lighting);

/* the views a draw goes to (a vertex program's multiview variant): with
XGPU_MULTIVIEW_EYES, both of the VR mode's eyes at once (multiview): the
clip position goes through each eye's correction (xgpu_spirv.c,
halo_vr_set_eye_transforms); with XGPU_MULTIVIEW_LEFT_EYE or
XGPU_MULTIVIEW_RIGHT_EYE, that eye's layer alone */
enum
{
	XGPU_MULTIVIEW_NONE = 0,
	XGPU_MULTIVIEW_EYES,
	/* the eye pass's visibility tests: queries in a multiview pass would
	count in as many query slots as views (Vulkan), which Zink does not
	allow for, and the GPU writes past them */
	XGPU_MULTIVIEW_LEFT_EYE,
	/* a draw into each eye's layer in turn, each with its own screen
	correction (the crosshairs: halo_vr_draw_eye) */
	XGPU_MULTIVIEW_RIGHT_EYE,
};
/* The VR mode's eye pass is foveated (vr.foveation, host_vulkan_vr.c): its targets
have four layers, two for each eye (layer 2 eye + 0, the whole eye at the
periphery's resolution; 2 eye + 1, the inset about the gaze at full
resolution, as large in pixels). XGPU_MULTIVIEW_EYES then draws all four
(eye_correction[VIEW >> 1], then fovea_view[VIEW] scales and moves the
inset's views), XGPU_MULTIVIEW_LEFT_EYE and XGPU_MULTIVIEW_RIGHT_EYE that
eye's two (its visibility tests are counted by the pixel shaders instead),
and a layered target is sampled in an inset's layer through
fovea_sample[layer] (its pixels from the whole eye's), from the whole eye's
layer where the inset does not have the point. Set before the first VR
shader is made. */
extern int xgpu_foveated;

/* ---------- pixel shaders */

enum
{
	_xgpu_sampler_none = 0,
	_xgpu_sampler_2d,
	_xgpu_sampler_3d,
	_xgpu_sampler_cube,
	/* a 2-layer render target (the VR mode's eye pass; 4 foveated,
	xgpu_foveated): the view's layer */
	_xgpu_sampler_2d_layered,
};

/* everything a translated pixel shader depends on; the GLSL program cache
is keyed by these bytes */
struct nv2a_pixel_shader_key
{
	DWORD combiner_state[D3DRS_PS_MAX];
	/* D3DRS_PSTEXTUREMODES lies past D3DRS_PS_MAX: its four stages' 20
	bits, and past them the port's (XGPU_TEXTURE_MODES_*, below) */
	DWORD texture_modes;
	unsigned char sampler_type[4];
	unsigned char alpha_kill[4];
	/* D3DTSS_COLORSIGN: channels (bit 0 alpha ... bit 3 blue, as
	D3DTSIGN_*) that hold signed data in an unsigned texture format */
	unsigned char color_sign[4];
	/* D3DCMP_* function for the alpha test, or 0 when disabled */
	unsigned long alpha_test_function;
	unsigned char fog_enable;
	unsigned char fog_table_mode;
	/* inside a visibility test: count the samples that pass (Android) */
	unsigned char count_samples;
	/* a high-res HUD meter (hud_hires.h) drawn with the meter's blend (the
	destination kept by the source's alpha): that alpha is eased to 1 by the
	coverage texture 0's green holds, so that the meter darkens what is
	behind it only where it covers it (the Xbox's point-sampled meters stop
	at their texels' edges; filtered ones have a fringe of faint texels) */
	unsigned char coverage_alpha;
	/* Discrete meter thresholds in texture 0's red are read at level zero,
	without filtering. Coverage/brightness still use the filtered lookup. */
	unsigned char point_threshold;
	/* a model lighting program's draw lit for each pixel
	(display.per_pixel_lighting): nv2a_vertex_lighting's lights, or 0 for
	the diffuse color the vertex shader computed */
	unsigned char per_pixel_lighting;
	/* drawn into a multisampled target (display.anti_aliasing's
	multisampling), its samples a pixel: the alpha test covers samples in
	proportion to how far alpha is past the reference, not all of the pixel
	or none of it, so that cut-out edges (foliage, grates) are smoothed too */
	unsigned char alpha_test_samples;
	/* drawn into both eyes at once (the VR mode, d3d8_device.c, xgpu_vulkan.c):
	layered render targets are sampled at the view's layer
	(XGPU_MULTIVIEW_*) */
	unsigned char multiview;
	/* the source's blend factor, applied here instead (1 the blend color, 2
	one less it, 3 its alpha, 4 one less that; 0 none), for a blend of a
	constant color factor with a constant alpha one, as the meters'
	(shader_transparent_meter) */
	unsigned char source_blend;
	/* the enhanced dynamic lights (display.dynamic_lights, d3d8_device.c,
	xgpu_spirv.c) added for each pixel: XGPU_DYNAMIC_LIGHTS_ENVIRONMENT to a
	structure's lightmap pass (into the light the textures are then
	multiplied by, the bump map in texture 0), XGPU_DYNAMIC_LIGHTS_MODEL to a
	model's lighting lit for each pixel in place of its two point lights; 0
	none */
	unsigned char dynamic_lights;
	/* display.ambient_occlusion's occlusion multiplies the draw's ambient
	light alone, not the light its lights add (XGPU_AMBIENT_OCCLUSION_LIGHT_*,
	below); 0 none */
	unsigned char ambient_occlusion;
	/* the VR mode's effects (d3d8_device.c halo_vr_effect_depth, the
	backend's xgpu_spirv.h XGPU_SPIRV_EFFECT_*): an effect drawn into both
	eyes soft against the scene's depth (1), and thick (2); 0 none */
	unsigned char vr_effect;
	/* display.hdr: the draw's tag into the HDR back buffer (xgpu_hdr.h:
	XGPU_HDR_*); 0 into any other target, and without HDR */
	unsigned short hdr;
};

/* display.smooth_lightmaps (#1225): a structure's lightmap pass samples its
lightmap (texture 2, in 2D: rasterizer_xbox_environment.c) with a uniform
cubic B-spline over the 4x4 texels about the point, in four bilinear
lookups, instead of the hardware's bilinear filter over 2x2, so that the
level's coarse baked light shows no stair steps or diamonds at high
resolutions. The weights are positive and sum to 1 (a flat area is as it
was; the lightmap never brighter or darker). Each page is a texture of
its own (clamped at its edge: no lookup reaches another page), and its
charts are dilated into the gutters between them by the light's bake:
measured on bloodgulch's and a10's first structures, the cubic's weight on
another chart's texels is 0.006% of it at most (bilinear's 0.001%), and
its light off what the chart's own texels alone give 0.003 on average, 0.11
at the 99.9th percentile (bilinear's 0.001 and 0.09), so no chart's bounds
are needed (xgpu_spirv.c's, xgpu_msl.c's lightmap_cubic). The bit in the pixel key's texture_modes,
set by xgpu_draw_derive; nv2a_pixel_program_decode gives the program's
lightmap_filter */
#define XGPU_TEXTURE_MODES_LIGHTMAP_CUBIC 0x80000000u
/* a model's material maps (docs/mods.md "Material maps", d3d8_device.c's
halo_model_material_maps): a model lit for each pixel with the enhanced
dynamic lights samples a tangent-space normal map (XGPU_TEXTURE_MODES_NORMAL_MAP)
and an occlusion, roughness and metalness map (XGPU_TEXTURE_MODES_ORM_MAP) at
texture 0's coordinates, bound past the four stages (xgpu_spirv.h's
XGPU_SPIRV_BINDING_NORMAL_MAP, _ORM_MAP; xgpu_stream.h's
XGPU_STREAM_MATERIAL_STAGE). Without either bit the draw is as it was */
#define XGPU_TEXTURE_MODES_NORMAL_MAP 0x40000000u
#define XGPU_TEXTURE_MODES_ORM_MAP 0x20000000u
#define XGPU_TEXTURE_MODES_MATERIAL (XGPU_TEXTURE_MODES_NORMAL_MAP | XGPU_TEXTURE_MODES_ORM_MAP)
/* display.rt_reflections (docs/rt-plan/2-reflections.md, d3d8_device.c's
halo_rt_reflection): a draw whose texture 3 is the game's reflection cube
map (a CUBEMAP or DOT_REFLECT_SPECULAR lookup) has its texel traced through
the ray-traced scene instead, the cube map where the ray leaves it; the
class (XGPU_RT_REFLECTION_*) in these bits: the structure's reflection pass,
the level's glass and its water (their vertices in the world: the vertex
shader hands the position on as it is), or a model (lit for each pixel: its
lighting's world position). Without the bits the draw is as it was */
#define XGPU_TEXTURE_MODES_RT_REFLECTION 0x1c000000u
#define XGPU_TEXTURE_MODES_RT_REFLECTION_SHIFT 26
#define XGPU_RT_REFLECTION_NONE 0u
#define XGPU_RT_REFLECTION_STRUCTURE 1u
#define XGPU_RT_REFLECTION_MODEL 2u
#define XGPU_RT_REFLECTION_MIRROR 3u
/* (water: level, so a ray its waves turn below the horizon would meet the
water itself, and keeps the cube map's lower half, as the game drew it) */
#define XGPU_RT_REFLECTION_WATER 4u
#define XGPU_RT_REFLECTION_OF(texture_modes) \
	(((unsigned long)(texture_modes) & XGPU_TEXTURE_MODES_RT_REFLECTION) >> XGPU_TEXTURE_MODES_RT_REFLECTION_SHIFT)
/* (its class's vertices are in the world) */
#define XGPU_RT_REFLECTION_WORLD_VERTICES(reflection) \
	((reflection) == XGPU_RT_REFLECTION_STRUCTURE || (reflection) == XGPU_RT_REFLECTION_MIRROR || \
	(reflection) == XGPU_RT_REFLECTION_WATER)
/* (the stages' own bits) */
#define XGPU_TEXTURE_MODES_STAGES 0x000fffffu
/* display.soft_particles: a first-person weapon's effect drawn over the
weapon's own pixels (d3d8_device.c's halo_effect_soft 2) tests no depth
where it is soft, its fade against the weapon's depth its test, so that a
flat sprite meets the gun without a straight cut. Only where the backend's
fade is sure to be drawn for the draw's pixel program: texture 0 sampled in
2D and textures 2 and 3 not at all (xgpu_spirv.c's
xgpu_spirv_effect_possible, which samples none in 2 and 3 more strictly);
else the draw keeps its test, as the Xbox drew it (its pixel key's
texture modes and texture 0's sampler) */
static __inline int xgpu_effect_untested(unsigned long texture_modes, unsigned int sampler_0)
{
	return (texture_modes & 0x1f) == 1 && sampler_0 == _xgpu_sampler_2d && !((texture_modes >> 10) & 0x3ff);
}

/* the vertex constants the per-pixel model lighting reads, in a uniform of
their own (the vertex shader's 192 would pass the least fragment uniform
space a GPU must have): [0] c[-82] (the translucency in z), [1] to [11] c[-79] to
c[-69] (rasterizer_set_model_lighting's two point lights, two distant
lights and the ambient light), [12] c[-57] (with the enhanced dynamic
lights, the lights the model is not lit by: its object's own that say so,
as four light indices, -1 none: halo_dynamic_lights.h) */
#define XGPU_MODEL_LIGHT_COUNT 13
#define XGPU_MODEL_LIGHT_OWN_LIGHTS 12

/* ---------- display.ambient_occlusion in the light

The occlusion (found in the world at the pixel: below) is the share of
the ambient light that reaches a pixel, so it multiplies that light alone,
in the shaders that light the surfaces, never what a light adds (the
flashlight, a plasma bolt's glow, the enhanced dynamic lights): the pixel
key's ambient_occlusion, the light it multiplies:

- XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL: a model lit for each pixel
  (per_pixel_lighting): its ambient and distant lights (the level's light
  about it, sampled from the lightmap) by the occlusion, its two point
  lights or the enhanced dynamic lights added after;
- XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX: a model lit for each vertex, as the
  Xbox lit it: the vertex shader lights it as its program does, but hands
  on the ambient and distant lights' sum and the point lights' apart
  (XGPU_LIGHTING_MODEL_SPLIT), and the pixel shader adds them, the first by
  the occlusion; the program's own sum clamps each vertex's light, which
  no pixel could take apart again;
- XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP: a structure's lightmap pass, its
  light (the lightmap, before the textures multiply it) by the occlusion,
  the enhanced dynamic lights added after. */
enum
{
	XGPU_AMBIENT_OCCLUSION_LIGHT_NONE,
	XGPU_AMBIENT_OCCLUSION_LIGHT_MODEL,
	XGPU_AMBIENT_OCCLUSION_LIGHT_VERTEX,
	XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP,
};

/* ---------- display.ambient_occlusion "world" in the light

The same three lights (XGPU_AMBIENT_OCCLUSION_LIGHT_*, the key's low bits:
XGPU_AMBIENT_OCCLUSION_LIGHT_OF) multiplied by the occlusion found in the
world at the pixel, where the key has
XGPU_AMBIENT_OCCLUSION_WORLD (docs/world-ao.md, "Shading";
port/linux/game/world_ao.c). The pixel's world position p and normal n
(the lighting's: a model's, or the lightmap pass's own, XGPU_LIGHTING_
ENVIRONMENT) read the window's block (xgpu_stream.h's struct
xgpu_world_ao_block: XGPU_SPIRV_BINDING_WORLD_AO, XGPU_MSL_BUFFER_WORLD_AO)
and the structure's distance field (XGPU_SPIRV_BINDING_WORLD_SDF,
XGPU_MSL_TEXTURE_WORLD_SDF), and the draw's object its uniforms' world_ao
(its owner slot among the proxies', -1 none; its reference):

  A_sdf   = 1 - clamp(K sum_i 2^-i clamp((h_i - d(p + n h_i)) / h_i, 0, 1), 0, 1),
            h_i = max(STEP, voxel) 2^(i-1), i = 1 to TAPS
  A_obj   = mix(A_sdf, clamp(A_sdf / max(reference, REFERENCE_LEAST), 0, 1), reference_weight)
  A_prox  = prod over the proxies of other owners within reach (1 - occlusion_i fade_i),
            fade_i = clamp((reach_i - |p - c_i|) / (REACH_FADE reach_i), 0, 1), c_i its
            centre (a sphere's and a box's a, a capsule's segment's middle); a box's
            fade_i its bound's (world_proxy_box_fade), and none where that is 0;
            the proxies those of the pixel's cell of the block's grid alone
            (XGPU_WORLD_AO_GRID_*), lowest first: the same product
  models:   A = mix(1, max(mix(1, A_obj, field_fade) A_prox, FLOOR), strength_objects)
  lightmap: A = mix(1, max(A_prox, FLOOR), strength_level)          (never the field)

d is the field's distance, read as world_sdf_sample reads it
(world_sdf_bake.c): the field is sparse (world_sdf.h), a table of cells
(XGPU_SPIRV_BINDING_WORLD_SDF_TABLE, XGPU_MSL_TEXTURE_WORLD_SDF_TABLE: RGBA8,
an atlas brick's x, y, z and the cell's kind, XGPU_WORLD_SDF_CELL_*) of
XGPU_WORLD_SDF_BRICK voxels' steps a side, and an atlas of bricks of
XGPU_WORLD_SDF_BRICK_SPAN voxels a side, XGPU_WORLD_SDF_ATLAS by
XGPU_WORLD_SDF_ATLAS a layer (the block's field[1].w layers); at p, in
voxels c = clamp((p - field[0].xyz) field[0].w, 0, 8 field[1].xyz), its cell
min(floor(c / 8), field[1].xyz - 1): a cell outside is +range, inside
-range, a brick its texel at its place times 9 plus c - 8 cell plus a half,
filtered, times range = RANGE_VOXELS / field[0].w. occlusion_i is
world_proxy_occlusion's (world_proxies.c), reach_i world_proxy_reach's;
world_ao.c's world_ao_occlusion is the whole of it on the CPU, line for
line. With XGPU_AMBIENT_OCCLUSION_WORLD_VIEW too (debug.world_ao_view) the
draw shows A in grey. */
#define XGPU_AMBIENT_OCCLUSION_WORLD 0x10
#define XGPU_AMBIENT_OCCLUSION_WORLD_VIEW 0x20
/* found once a pixel (xgpu_stream.h's WORLD_AO_PREPASS): with
XGPU_AMBIENT_OCCLUSION_WORLD, the draw reads its pixel's occlusion from the
backend's texture of it (XGPU_SPIRV_BINDING_WORLD_AO_PIXELS,
XGPU_MSL_TEXTURE_WORLD_AO_PIXELS) where it would find it, upsampled where it
was found at a quarter of the pixels (the block's parameters[1].w 2: each
2x2 block's at its nearest pixel; world_ao.h's world_ao_upsample); its twin
in the pre-pass (_INPUTS in its place) writes its pixel's inputs to the
occlusion instead of its colour, its alpha test as it is: four words, the
normal octahedral, two signed 16-bit halves (x low); the owner slot plus 2
in its low byte, the XGPU_WORLD_AO_INPUT_* bits, and in its high half the
reference (a model's) or the static scenery's page's texel (the lightmap
pass's: 1 where it has none) as a half float; its distance from the eye
(the block's grid[0]), a float's bits; and a spare word. The occlusion's
pass reads them (world_ao_shading.c's world_ao_pass_*). _CARDS: the draw
is a two-sided model shader's cards (its uniforms' world_ao.w not 0: a
plant's leaves), whose occlusion is found about their most open
direction, the sky's (up) or either face's (world_ao_shading.c's
world_ao_cards_occlusion) */
#define XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED 0x40
#define XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS 0x80
#define XGPU_WORLD_AO_INPUT_LIGHTMAP 0x100u
#define XGPU_WORLD_AO_INPUT_ALL_SHAPES 0x200u
#define XGPU_WORLD_AO_INPUT_PRESENT 0x400u
#define XGPU_WORLD_AO_INPUT_CARDS 0x800u
/* (the surfaces' upsampling of the occlusion found at a quarter of the
pixels: world_ao.h's WORLD_AO_UPSAMPLE_*) */
#define XGPU_WORLD_AO_UPSAMPLE_DEPTH 0.02f
#define XGPU_WORLD_AO_UPSAMPLE_FACING 0.2f
#define XGPU_WORLD_AO_UPSAMPLE_LEAST 1.0e-4f
#define XGPU_AMBIENT_OCCLUSION_LIGHT_OF(value) ((value) & 0x0f)
#define XGPU_WORLD_AO_K 1.6f
#define XGPU_WORLD_AO_STEP 0.15f
#define XGPU_WORLD_AO_TAPS 5
#define XGPU_WORLD_AO_REFERENCE_LEAST 0.25f
#define XGPU_WORLD_AO_REACH_FADE 0.25f
#define XGPU_WORLD_AO_FLOOR 0.2f
/* the lightmap's indirect light, linear, which the ambient occlusion takes
from a lightmap pass's light (XGPU_AMBIENT_OCCLUSION_LIGHT_LIGHTMAP): the
occlusion is of the indirect light alone, never the sun's direct light an
outdoor bake mixes in. Under a sky with an outdoor ambient (Blood Gulch,
b30) the bakes' shade sits at 3 to 5% linear and their sun at 80 to 95%,
18 to 26 times over it (docs/rt-plan/4-path-tracing.md "Measured"): a
texel brighter than their geometric middle, sqrt(0.04 x 0.9), is sunlit,
its light past that the sun's: light -= (1 - occlusion^2.2) * min(light, this)
(the occlusion as it multiplied the gamma light),
sunlit sand darkened by a fifth of the occlusion at most. An interior's
bake (a10's: no outdoor ambient, its median texel 1.3%) is its emitters'
and their bounce, all of it occluded. The level is the block's grid[0].w
(port/linux/game/world_ao.c's WORLD_AO_INDIRECT; 0: all of the light) */
#define XGPU_AMBIENT_OCCLUSION_INDIRECT 0.19f
/* the field's (world_sdf.h's WORLD_SDF_*: d3d8_device.c checks they agree) */
#define XGPU_WORLD_SDF_BRICK 8
#define XGPU_WORLD_SDF_BRICK_SPAN 9
#define XGPU_WORLD_SDF_ATLAS 16
#define XGPU_WORLD_SDF_RANGE_VOXELS 12
#define XGPU_WORLD_SDF_CELL_OUTSIDE 0
#define XGPU_WORLD_SDF_CELL_INSIDE 1
#define XGPU_WORLD_SDF_CELL_BRICK 2
/* (world_proxies.c's WORLD_PROXIES_PI) */
#define XGPU_WORLD_AO_PI 3.14159265358979f
/* the block as the emitters declare it (xgpu_stream.h's struct
xgpu_world_ao_block, its XGPU_WORLD_AO_PROXIES_MAXIMUM and _PROXY_*: the
emitters' copies of these headers have not that one; d3d8_device.c checks
they agree): its members' offsets, the most proxies, their kinds */
#define XGPU_WORLD_AO_BLOCK_FIELD 0
#define XGPU_WORLD_AO_BLOCK_PARAMETERS 32
#define XGPU_WORLD_AO_BLOCK_PROXIES 64
#define XGPU_WORLD_AO_BLOCK_GRID 3136
#define XGPU_WORLD_AO_BLOCK_CELLS 3168
#define XGPU_WORLD_AO_SHAPES 64
/* the proxies' grid (world_ao.h's WORLD_AO_GRID_*: world_ao_grid) and the
least box occlusion computed, one over (world_proxies.c's
WORLD_PROXIES_LEAST: world_proxy_box_fade) */
#define XGPU_WORLD_AO_GRID_X 16
#define XGPU_WORLD_AO_GRID_Y 16
#define XGPU_WORLD_AO_GRID_Z 8
#define XGPU_WORLD_AO_GRID_CELLS (XGPU_WORLD_AO_GRID_X * XGPU_WORLD_AO_GRID_Y * XGPU_WORLD_AO_GRID_Z)
#define XGPU_WORLD_AO_LEAST 510.0f
/* (world_proxies.c's WORLD_PROXIES_FAR: a box as its faces' patches past
it, in half diagonals) */
#define XGPU_WORLD_AO_FAR 6.0f
#define XGPU_WORLD_AO_SPHERE 1
#define XGPU_WORLD_AO_CAPSULE 2
#define XGPU_WORLD_AO_BOX 3
/* (a vehicle's occlusion field: its shape and the next two, read from the
objects' atlas) */
#define XGPU_WORLD_AO_FIELD 4

/* ---------- the distance-field scene (port/linux/include/world_cascades.h)

The cascades about the camera, one 3D texture of two signed bytes a texel
(RG8 snorm: none, +1 everywhere; the objects' distance), each cascade
XGPU_WORLD_CASCADE_SPAN texels a side (its side and a guard), one above the
other along z; a cascade's block vec4 is its first voxel's centre and its
voxel (0: not ready). world_trace, the emitters' ray step through them
(xgpu_spirv.c's, xgpu_msl.c's; the structure's distance the level's field's
where the ray is), is world_cascades.c's world_trace_ray
operation for operation; these are world_cascades.h's WORLD_CASCADE* and
WORLD_TRACE_* */
#define XGPU_WORLD_CASCADES 4
#define XGPU_WORLD_CASCADE_SIDE 64
#define XGPU_WORLD_CASCADE_SPAN 65
#define XGPU_WORLD_CASCADE_RANGE_VOXELS 4
#define XGPU_WORLD_TRACE_STEPS 24
#define XGPU_WORLD_TRACE_HIT 0.25f
#define XGPU_WORLD_TRACE_LEAST_STEP 0.5f
#define XGPU_WORLD_TRACE_STRUCTURE 1
#define XGPU_WORLD_TRACE_OBJECTS 2
/* World AO's rays through it (the pass's world_ao_traced; world_ao.h's
WORLD_AO_RAYS and so on): rays a pixel, cosine-weighted about its normal;
their reach and their start off the surface (world units); and the share of
a frame's occlusion blended into the history reprojected (temporal
accumulation), and the history's tests (the distance's relative
difference, the normals' least cosine) */
#define XGPU_WORLD_AO_RAYS 4
#define XGPU_WORLD_AO_RAY_REACH 2.0f
/* (a pixel no object is within the reach of casts none: the objects'
distance at it read first in the finest cascade whose range, 4 voxels,
holds the reach, cascade 1's 2 wu) */
#define XGPU_WORLD_AO_RAY_CASCADE 1
#define XGPU_WORLD_AO_RAY_BIAS 0.1f
#define XGPU_WORLD_AO_HISTORY_NEW 0.2f
#define XGPU_WORLD_AO_HISTORY_DISTANCE 0.05f
#define XGPU_WORLD_AO_HISTORY_NORMAL 0.8f

/* ---------- enhanced dynamic lights

display.dynamic_lights "enhanced" (d3d8_device.c, halo_dynamic_lights_set):
the frame's dynamic lights (up to the game's 128 a window) light the
structure's lightmap pass and the models lit for each pixel, each pixel by
the lights near it alone. A grid about the camera, its cells boxes in the
world (eye-independent: the VR mode's eyes share it), holds for each cell a
128-bit mask of the lights whose spheres reach it (an integer 3D texture at
XGPU_DYNAMIC_LIGHTS_UNIT); the lights are a uniform block at
XGPU_DYNAMIC_LIGHTS_BINDING. The cells grow away from the camera: a cell's
index along each axis is log2(1 + |offset| / XGPU_DYNAMIC_LIGHTS_CELL) times
that axis's scale, from the middle, so that the grid reaches as far each
way as the frame's farthest light does (at most XGPU_DYNAMIC_LIGHTS_REACH
world units; its outer cells hold every point past it) with small cells near
the camera, where lights are seen large. A cell has the lights whose spheres
meet its box (d3d8_device.c's dynamic_lights_mark). */
#define XGPU_DYNAMIC_LIGHTS_MAXIMUM 128
/* a model lit by the enhanced dynamic lights where display.per_pixel_lighting
is off: its pixel key's per_pixel_lighting (the program's lights, 1 or 2)
with this bit, its ambient and distant lights the vertex's, as the game's
program lights them (XGPU_LIGHTING_MODEL_SPLIT's xLightAmbient), the lights
added for each pixel over them, so a model no light reaches is drawn as the
game draws it (xgpu_manifest.c's xgpu_draw_derive) */
#define XGPU_PER_PIXEL_VERTEX_AMBIENT 4
#define XGPU_DYNAMIC_LIGHTS_GRID_X 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Y 32
#define XGPU_DYNAMIC_LIGHTS_GRID_Z 16
#define XGPU_DYNAMIC_LIGHTS_CELL 0.5f
#define XGPU_DYNAMIC_LIGHTS_REACH 128.0f
/* the largest soft core of a light's falloff, world units (xgpu_spirv.c) */
#define XGPU_DYNAMIC_LIGHTS_CORE 0.6
/* a model's highlights of the lights (XGPU_DYNAMIC_LIGHTS_MODEL with a cube
map in texture 3, its reflection): added to the cube map's texel, so that the
model's own reflection mask, tint and fade show them where it shines. Their
strength at their peak (a normalized Phong lobe's, (exponent + 8) / 8, of the
light's diffuse on a surface facing it), and the sharpness of a light's
highlight on a smooth surface (a Phong exponent); where the surface's normal turns fast across the pixel
(small or far models), the highlight widens, as dim as it widens, so it does
not flicker (XGPU_DYNAMIC_LIGHTS_HIGHLIGHT_WIDEST at most added to the lobe's
width, 2 / (exponent + 2): an exponent no less than about 5.8) */
#define XGPU_DYNAMIC_LIGHTS_HIGHLIGHT 4.0f
#define XGPU_DYNAMIC_LIGHTS_HIGHLIGHT_EXPONENT 24.0f
#define XGPU_DYNAMIC_LIGHTS_HIGHLIGHT_WIDEST 0.18f
#define XGPU_DYNAMIC_LIGHTS_UNIT 12
#define XGPU_DYNAMIC_LIGHTS_BINDING 1

/* Which lights reach which of the structure's clusters: a light reaches the
clusters its sphere floods into through the structure's portals from where
it is (structure_clusters_in_sphere, as the game gathers a light's surfaces
for its own pass), and no other, so that a light on the far side of a wall
does not light this side of it. Past the cells, at z = GRID_Z, the cells'
texture has a slice of each cluster's 128-bit mask of the lights that reach
it (XGPU_DYNAMIC_LIGHTS_CLUSTERS of them: the game's most), uploaded with
the cells. Past it are the world's slices (XGPU_DYNAMIC_LIGHTS_WORLD_SLICES),
uploaded when the structure changes: the structure's 3D BSP, which says in
which leaf, and so cluster, a point is (bsp3d_test_point), as four words a
texel, texel t at x = t % 32, y = t / 32 % 32, z = GRID_Z + 1 + t / 1024:
texel 0 the header (the node count, the planes' first texel, the leaves'
first texel, the leaf count), from texel 1 a node each (its plane, its
children behind and in front: a child with the top bit set is a leaf, all
bits set none), then the planes (their floats' bits: the normal, and its
distance along it), then the leaves' clusters, four a texel (all bits set:
none). A pixel finds its cluster from its point a little off its surface,
along its normal, and its cell's lights are those its cluster's mask has; a
world with no nodes (none uploaded, or too large) leaves the cells' lights as
they are, as does a point the BSP puts in no cluster.
That is the world the game sends; the hosts' backends give the GPU another
made of it (xgpu_dynamic_lights_world.c and .h, which say how), in which
every point ends in the cluster it ended in, with each node's plane beside
it, and past the clusters' masks, at XGPU_DYNAMIC_LIGHTS_WALK in their slice,
the mask of the lights whose walk could take them from a pixel (which the
backend sets), a model's, and after it a structure's (which leaves out the
lights for the models alone): a pixel walks the world only where its cell
has one of them. */
#define XGPU_DYNAMIC_LIGHTS_CLUSTERS 512
#define XGPU_DYNAMIC_LIGHTS_WALK XGPU_DYNAMIC_LIGHTS_CLUSTERS
#define XGPU_DYNAMIC_LIGHTS_WORLD_SLICES 240
#define XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS (XGPU_DYNAMIC_LIGHTS_WORLD_SLICES * 1024)
/* the bytes of the cells and the clusters' masks, of which each frame's lights
send those that changed (the command stream's DYNAMIC_LIGHTS_CHANGE) */
#define XGPU_DYNAMIC_LIGHTS_CELLS_BYTES \
	(XGPU_DYNAMIC_LIGHTS_GRID_X * XGPU_DYNAMIC_LIGHTS_GRID_Y * (XGPU_DYNAMIC_LIGHTS_GRID_Z + 1) * 16)
/* how far off its surface a pixel's point is put, world units */
#define XGPU_DYNAMIC_LIGHTS_CLUSTER_OFFSET 0.02f
/* the most nodes a pixel walks down the BSP (bsp3d.c's
MAXIMUM_BSP3D_TRAVERSAL_DEPTH) */
#define XGPU_DYNAMIC_LIGHTS_WORLD_DEPTH 128

/* display.shadows "ray_traced"'s shadows of the dynamic lights
(docs/dynamic-shadows-plan.md, stage 1; port/linux/game/dynamic_lights.c
chooses them): the few lights that matter most cast shadows, found by one
pass of ray queries after display.ambient_occlusion "world"'s pre-pass
(port/linux/vulkan/xgpu_vulkan_light_shadows.c), from its pixels' distance,
normal and owner, into a mask of a channel a light (the light's share of
the pixel's view of its disk), which the surfaces drawn after it read in
their loop of the lights (xgpu_spirv.c's light_shadows_*). The lights are in
the cells' texture's clusters' slice, past the walk's two texels: at
XGPU_DYNAMIC_LIGHTS_SHADOWED, four words, each channel's light (its index
plus one; 0 none); at the next, their disks' radii (world units, a float's
bits); at the next, their owners among the ray-traced scene's instances
(world_rt.h's world_rt_owner: a ray leaves them out; 0 none); at the next,
the rays a light and whether the window's view is accumulated by a
temporal technique (FSR, DLSS: 1, the rays turned each frame and no filter
of the mask; 0, the rays the same each frame and the mask filtered). No
channel's light: the surfaces read none of it, and the backend draws no
pass. */
#define XGPU_DYNAMIC_LIGHTS_SHADOWED (XGPU_DYNAMIC_LIGHTS_WALK + 2)
#define XGPU_LIGHT_SHADOWS_CHANNELS 4
/* the most rays a light, a pixel; the rays' start off the surface (world
units); and how far short of the light's disk a ray ends (of the disk's
radius, and at least the least: the light's own glow, a bolt's body) */
#define XGPU_LIGHT_SHADOWS_RAYS 4
#define XGPU_LIGHT_SHADOWS_BIAS 0.04f
#define XGPU_LIGHT_SHADOWS_SHORT 1.5f
#define XGPU_LIGHT_SHADOWS_SHORT_LEAST 0.1f

/* The enhanced flashlight (port/linux/game/dynamic_lights.c,
d3d8_device.c's halo_dynamic_lights_flashlights): a unit's flashlight lights
the structure, the models and the first-person view in the one loop of the
lights (xgpu_spirv.c's, xgpu_msl.c's dynamic_light_sum), through its own
gel as the game's gel pass projects it, its own distance falloff, and one
shadow map in the gel's frame. Past the shadowed lights' texels, in the
clusters' slice:

- FLASHLIGHT: four words, each slot's light (its index plus one; 0 none);
- FLASHLIGHT + 1: the shadowed slot plus one (0: no map this frame), the
  gel's half width in the projection (tan, float bits), the cookie there
  (1), the map's bias (world units, float bits);
- FLASHLIGHT + 2 + 3 * slot, + 1, + 2: the slot's gel frame: its forward
  (the beam), up and left (floats' bits), the gel pass's rows;
- FLASHLIGHT + 14: the gel's tint, gamma (floats' bits);
- FLASHLIGHT + 15 to + 22 (ATTENUATION): every light's falloff on the
  structure, gamma: 32 floats' bits, from the light to its radius (the
  game's distance attenuation's);
- FLASHLIGHT + 23: where the shadowed slot's shadows are cast from (its
  lamp on the weapon in hand, floats' bits): the map's middle, in the same
  frame, a and b of v = p - lamp;
- FLASHLIGHT_COOKIE on: the gel's brightness (the game's, gamma, as the
  gel pass multiplies it), a byte a
  texel, FLASHLIGHT_COOKIE_SIZE square, rows from -up, columns from -left:
  texel (x, y) is the direction forward + a left + b up, a = u |u| width
  for u from -1 to 1 at the texels' middles (and b of v the same): each
  axis warped, so the texels are closer at the middle, where the gel's hot
  disk and ring are.

A pixel at p: v = p - light, a = dot(v, left) / dot(v, forward), b the
same with up, for the cookie; the map, from the lamp, (its own image, the draw set's binding
XGPU_SPIRV_BINDING_FLASHLIGHT_SHADOW, FLASHLIGHT_SHADOW_SIZE square, in
the same a, b as the cookie) holds, in red, the distance from the light
to what its ray first met (its radius where none). */
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT (XGPU_DYNAMIC_LIGHTS_SHADOWED + 4)
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHTS 4
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_FALLOFF 32
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE (XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 24)
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE 80
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_SHADOW_SIZE 512
/* The light model (port/linux/game/dynamic_lights.c): every enhanced light is a point,
a spot (the flashlight: its gel, above) or a panel, and each is added as
the game adds it for the surface it lights, in the display's gamma
(xgpu_spirv.c's dynamic_light_sum, dynamic_light_add):

- the structure (the game's gel pass): color * gel * T(d / r) *
  max(dot(n, l), 0) * saturate(8 dot(surface normal, l)), times the
  shader's material colour, T the game's distance attenuation (its 32
  values at ATTENUATION, the falloff above: every light's, read from the
  game's texture as it is, gamma) and the gel 1 for a point;
- a model and the first-person view (the game's model lighting): color *
  max(1 - d^2 / r^2, 0) * saturate(dot(n, l)) * the cone;
- a panel (PANELS: a mask of the lights that are, four words): a small
  lit display, a Lambertian disk, in linear light: color (linear) *
  a^2 / (a^2 + d^2) * (1 - d^2 / r^2)^2 * the cosine off its face (the
  cone's axis) * saturate(dot(n, l)), a^2 in the colour's w.

The sum is added to the light already there as the game's framebuffer
adds it, (base + sum)^2.2 linear, the panels' added there, under a
shoulder that leaves all below max(base, ADD_KNEE) (linear) as it is. */
#define XGPU_DYNAMIC_LIGHTS_ATTENUATION (XGPU_DYNAMIC_LIGHTS_FLASHLIGHT + 15)
#define XGPU_DYNAMIC_LIGHTS_PANELS (XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE + \
	XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE * XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_COOKIE_SIZE / 16)
#define XGPU_DYNAMIC_LIGHTS_ADD_KNEE 0.7f
/* the map's bias at the light's radius past its texel's width there, and
its least (world units) */
#define XGPU_DYNAMIC_LIGHTS_FLASHLIGHT_BIAS 0.03f
enum
{
	XGPU_DYNAMIC_LIGHTS_ENVIRONMENT = 1,
	XGPU_DYNAMIC_LIGHTS_MODEL,
};

/* the uniform block, as std140 lays it out: [0] the grid's middle (the
camera) and the light count, [1] each axis's cells a doubling of the offset
and 1 / XGPU_DYNAMIC_LIGHTS_CELL, [2] the look (xgpu_spirv.c); then each
light's position and 1 / radius (less than 0: models only, the structure
lit by the game's own pass: a spotlight's gel), its color (linear, the
look's brightness in it) and its cone's offset, its cone's axis and scale
(omni: 0, 0 and an offset of 1) */
struct xgpu_dynamic_lights_block
{
	float grid[3][4];
	float position[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float color[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
	float cone[XGPU_DYNAMIC_LIGHTS_MAXIMUM][4];
};

/* writes BGRA pixels as a BMP named <name><frame>.bmp in the screenshot
folder (debug.screenshot_directory): rows from the top, or from the bottom
(d3d8_device.c) */
void xgpu_write_bmp(const char *name, const unsigned char *pixels, unsigned long width, unsigned long height,
	BOOL bottom_up);
/* the <frame> of this frame's screenshots (where the host writes the VR
mode's, it is told it: xgpu_stream_encode.c) */
unsigned long xgpu_screenshot_number(void);

/* ---------- textures */

struct xgpu_texture_description
{
	DWORD format;       /* D3DFMT_* */
	unsigned long width, height, depth, levels;
	BOOL cube_map;
	BOOL linear;        /* not swizzled; addressed with texel coordinates */
	BOOL compressed;
	unsigned long pitch; /* linear textures */
	BOOL hires;         /* a high-res HUD texture drawn in the texture's place (hud_hires.h) */
	BOOL hires_coverage; /* ... whose green is its coverage (a meter's) */
	BOOL hires_point_threshold; /* ... whose red holds discrete segment data */
	BOOL hires_msdf;    /* the high-res text's distance fields (XGPU_ADDRESS_MSDF) */
};

/* (xbox_texture_decode.c) */
void xgpu_texture_describe(DWORD format_word, DWORD size_word, struct xgpu_texture_description *description);
/* bytes of one face, mip levels included (cube faces are padded) */
unsigned long xgpu_texture_face_size(const struct xgpu_texture_description *description);
unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_pitch(const struct xgpu_texture_description *description, unsigned long level);
/* bytes of a level (all of a volume's slices), and a level's width, height
or depth from the first level's */
unsigned long xgpu_texture_level_size(const struct xgpu_texture_description *description, unsigned long level);
unsigned long xgpu_texture_level_dimension(unsigned long base, unsigned long level);
/* the least alpha any texel of any level reads, 0 to 255, from its texels
(a 2D texture's): 255 for a format with no alpha; a DXT1 texture's 255, or
0 with a block of three colours that uses the fourth index, the
transparent one; a DXT3 texture's least alpha; a DXT5 texture's blocks'
lower end points (0 where a block of six alphas uses its 0); an
A8R8G8B8 texture's. Never more than the least: -1 where it is not known
(the other formats are not read) */
int xgpu_texture_minimum_alpha(const struct xgpu_texture_description *description, const unsigned char *texels);
/* the size is one D3DDevice_GetDeviceCaps allows (d3d8_device.c) */
BOOL xgpu_texture_size_supported(const struct xgpu_texture_description *description);
/* a level of a texture that is not compressed (a volume's slices one after
another), from its texels at source, as 32-bit BGRA (a DWORD each: ARGB),
rows from the first; FALSE when out of memory. palette: a P8 texture's 256
D3DCOLORs (NULL: grey) */
BOOL xgpu_texture_decode_level(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, const D3DCOLOR *palette, DWORD *destination);
/* a level of a DXT texture, as xgpu_texture_decode_level */
void xgpu_texture_decode_dxt(const struct xgpu_texture_description *description, unsigned long level,
	const unsigned char *source, DWORD *destination);

/* ---------- levels that keep an alpha test's coverage

display.alpha_coverage (Enhanced; the Xbox drew the levels as stored, #1223):
a 2D texture whose alpha a draw's alpha test reads alone (d3d8_device.c's
alpha_test_stage) is drawn from a copy of its own (the stage's
xgpu_stream_texture_stage target, XGPU_TEXTURE_COVERAGE_*: the texture of
any other draw is the one stored), whose levels past the first have the
share of texels the test passes that the first has. Averaged, a level's
alphas fall under the test's reference where the first's are thin (a
fence's wires, a tree's leaves), and far away those vanish and shimmer.
Each level's alpha is made from the one before's as it was made (not as
scaled), box-filtered, then scaled so that as many of its texels pass as
the first's share would have (I. Castaño's, "Computing Alpha Mipmaps",
2010); a texel the test passes now that it failed as stored takes the
colour the first level's passing texels average to there (a stored level's
cut-out texels are of any colour, DXT1's black). The colours of the rest
are the level's as stored. The backends decode such a copy to BGRA
(xgpu_vulkan_textures.c, xgpu_metal_textures.c), once as it is read. */
#define XGPU_TEXTURE_COVERAGE_SHIFT 16
#define XGPU_TEXTURE_COVERAGE_MASK 0x00ff0000u
/* the least alpha the test passes (2 to 255) in a stage's target */
#define XGPU_TEXTURE_COVERAGE_PASS(target) (((target) & XGPU_TEXTURE_COVERAGE_MASK) >> XGPU_TEXTURE_COVERAGE_SHIFT)

struct xgpu_texture_coverage
{
	/* the least alpha passing; the first level's share passing */
	unsigned int pass;
	double share;
	/* the level before as made, unscaled: its colour averaged over its
	alpha, and that (ARGB words) */
	DWORD *made;
	unsigned long width, height;
	BOOL failed;
};

void xgpu_texture_coverage_begin(struct xgpu_texture_coverage *coverage, unsigned int pass);
/* the texture's next level (0 first, then each in order), its texels
decoded (ARGB words, as xgpu_texture_decode_level), width by height: the
first's share taken from it; a later one's alpha (and colour where it is
passed now and was not) made as above, in place */
void xgpu_texture_coverage_level(struct xgpu_texture_coverage *coverage, unsigned long level, unsigned long width,
	unsigned long height, DWORD *texels);
void xgpu_texture_coverage_end(struct xgpu_texture_coverage *coverage);

/* ---------- MCC textures

An MCC map's textures (port/linux/game/mcc_texture_cache.c) are converted to
Xbox formats in memory of their own, outside the Xbox's contiguous memory
the host's texture cache reads texels from. Each has a header of its own
(mcc_texture_bridge.c) whose Data is XGPU_MCC_TEXTURE_DATA, never read as an
address, and is drawn as an image (xgpu_images.h) in its place: its first
level decoded to RGBA as it is first bound, or as the map loads
(xgpu_mcc_texture_prepare), and the levels past it made by the host
(xbox_textures.c). A cube map or a volume has no image (images are 2D), nor
does a P8 texture bound with no palette, and is not drawn. */
#define XGPU_MCC_TEXTURE_DATA 0xffffffffUL

/* (mcc_texture_bridge.c) for an MCC texture's header: its description (a
linear one's sides and pitch as its header could not hold them), its
texels and their size, and the high-res HUD texture drawn in its place
(hud_hires.h's hud_hires_embedded, -1 none); FALSE if the header is no MCC
texture's */
BOOL mcc_texture_bridge_texels(const DWORD *resource, struct xgpu_texture_description *description,
	const void **pixels, unsigned long *bytes, long *hud_asset);

/* (xbox_textures.c) the image drawn for the MCC texture whose header is at
resource (0 none: not an MCC texture's, or one with no image), and its
description as it is drawn; palette: a P8 texture's 256 D3DCOLORs (0 none) */
unsigned int xgpu_mcc_texture_image(DWORD resource, DWORD palette, struct xgpu_texture_description *description);
/* (xbox_textures.c) an MCC texture's image made ahead, as its map loads, if
its first level's RGBA is at most budget bytes: those bytes, 0 if it was not
made (or was already) */
unsigned long xgpu_mcc_texture_prepare(const DWORD *resource, unsigned long budget);
/* (xbox_textures.c) an MCC texture let go: its image deleted (once the
frames that drew it are done) */
void xgpu_mcc_texture_forget(const DWORD *resource);
/* (d3d8_device.c) a texture's header about to be freed: taken from every
stage it is set to, so that no draw reads it after */
void xgpu_mcc_texture_unbind(const DWORD *resource);

#endif
