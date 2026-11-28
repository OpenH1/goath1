#!/usr/bin/env python3
"""Build Vulkan's anti-aliasing passes.

Run with glslangValidator and spirv-val on PATH:
    python3 tools/vulkan_post_shaders.py
The generated C (port/linux/vulkan/xgpu_vulkan_post_shaders.c) embeds the
SPIR-V of display.anti_aliasing's passes and SMAA's lookup textures,
inflated; the game needs no shader compiler and no zlib for them.
--check verifies that the checked-in file still matches its sources.

The passes are the GLSL below: a vertex shader, FXAA, and the main
functions of SMAA's three passes after SMAA.hlsl (port/third_party/smaa) at
its HIGH preset. Each pass draws one triangle covering the rectangle (its
corners from gl_VertexID, with no vertex data) and finds its texels from
gl_FragCoord. FXAA is written here, after Timothy Lottes' description: the
direction of an edge from the luma around a pixel, the edge's ends found by
stepping along it, and the pixel blended across it by where it lies between
them, or by how much it stands out from its neighbours. FXAA and SMAA's
blend write each pixel's own alpha back (the game's value, unchanged), so
that a pass may draw the view whole into another image. Only the interface
changes when they are built: the uniforms become a block (set 0, binding 1,
as the backend's other draws have their uniforms), the samplers take the
texture units UNITS gives them as their bindings (set 1), the output a
location, and gl_VertexID is gl_VertexIndex. The rotate program
(ROTATE_SOURCE) is Vulkan's own: the present step's turn for a swapchain the
display turns. display.hdr's tone map and encoding are xgpu_hdr.c's GLSL
(hdr_programs), the same source tools/metal_post_shaders.py compiles for
Metal: the tone map reads the pixel it draws over as its input attachment
(set 2, binding 0: VK_KHR_dynamic_rendering_local_read), and again as each
sample's own for a multisampled target (hdr_tone_map_ms). The coordinates
stay: the passes' textures and targets have their first row at the top, as
Vulkan's images do, and gl_FragCoord counts rows from it. glslang 16.6.0 and
SPIRV-Tools 2026.4 (Homebrew's) produce the checked-in words.

The encoding is the frame's last pass in both backends (#1108): HDR's
encoding, display.gamma's curve over an SDR frame, and the dither.
display.panini's projection (PANINI) is a pass over a window's 3D view as
FXAA's is, in both backends; its uniforms add window, panini and tangents.

RESOLVE_LIGHT is the core's MSAA resolve in light (#1227, xgpu_vulkan.c's
resolve_in_light): drawn in the pass over a multisampled target, it reads
each of a pixel's samples (its input attachment, as the tone map's), makes
them linear (xgpu_hdr.c's hdr_linear) and writes their mean, encoded again,
to every sample, so that the hardware's resolve after it, of equal
samples, is the mean of their light, not of their codes. Only an edge's
pixels, whose samples differ, are written: one whose samples are alike is
its own mean already, and is discarded, unwritten and unconverted.

CONTACT_SHADOWS is display.contact_shadows' pass (xgpu_vulkan_contact_shadows.c,
docs/dynamic-shadows-plan.md's stage 2): drawn with the vertex shader above
over a window's 3D view after its lightmap pass, where display.shadows is
not "ray_traced", it finds each pixel's point from the depth and, for each
caster whose volume holds it (the ray-traced shadows' casters, a few less),
marches a short way toward the caster's light (its classic shadow's
direction, the lightmap's dominant light: the sun outdoors) in screen
space, a step at a time projected into the depth: a step behind what the
depth holds, by less than a guessed thickness, is in shadow. Its steps are
the same for every pixel (no noise, which no pass after it averages under
SMAA: #1290's dotted fringe), and its normal comes from the nearer of each
pair of neighbours, never across an object's outline. Drawn after
the classic receivers, it writes the light taken before them (its second
texture) times its shadow, and the blend keeps the lesser of that and the
pixel: the darker of the two shadows, not both. Its uniforms are a block of
their own (CONTACT_SHADOWS_UNIFORMS).

Beside them, CLEAR is the pixel shader of the core's clears of some of a
target's colour channels (xgpu_vulkan.c, xgpu_vulkan_clear), which
vkCmdClearAttachments cannot do: drawn with the vertex shader above,
through the channels' write mask, it writes the uniforms' colour.
"""

from __future__ import annotations

import argparse
import ast
from pathlib import Path
import re
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[1]
# display.hdr's tone map and encoding (their functions and main functions)
HDR_SOURCE = ROOT / "port/linux/src/xgpu_hdr.c"
SMAA = ROOT / "port/third_party/smaa"
OUTPUT = ROOT / "port/linux/vulkan/xgpu_vulkan_post_shaders.c"

STRINGS = re.compile(r'"(?:\\.|[^"\\])*"')
COMMENTS = re.compile(r"/\*.*?\*/", re.S)
UNIFORM = re.compile(r"^uniform vec4 (\w+);\n", re.M)
SAMPLER = re.compile(r"^uniform sampler2D (\w+);\n", re.M)
# the uniforms' block: each pass's vec4s, in this order (std140), as far as
# the last its program uses (hdr in display.hdr's passes, window, panini
# and tangents in display.panini's; gamut, display.hdr_gamut's, in the
# encoding's)
# (and display.temporal_aa's motion passes' reproject0 to reproject3 and motion)
UNIFORMS = ("metrics", "bounds", "hdr", "window", "panini", "tangents", "gamut", "reproject0", "reproject1",
            "reproject2", "reproject3", "motion")
# the fragment programs, in xgpu_vulkan_post_shaders.h's order
PROGRAMS = ("fxaa", "smaa_edges", "smaa_weights", "smaa_blend", "rotate", "hdr_tone_map", "hdr_tone_map_ms",
            "hdr_encode", "panini", "motion_resolve", "motion_view", "loading", "loading_caption",
            "loading_picture", "resolve_light")
# one triangle over the whole viewport
VERTEX_SOURCE = (
    "void main()\n"
    "{\n"
    "\tgl_Position = vec4(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0, 0.0, 1.0);\n"
    "}\n"
)
# (a pixel with no edge written back as it is, never discarded: the pass draws
# a whole target's view into a cleared spare, xgpu_vulkan_post.c)
FXAA_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "const float EDGE_THRESHOLD = 0.125;\n"
    "const float EDGE_THRESHOLD_MINIMUM = 0.0312;\n"
    "const float SUBPIXEL_QUALITY = 0.75;\n"
    "const int STEPS = 12;\n"
    "const float STEP_LENGTHS[12] = float[12](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);\n"
    "\n"
    "vec3 color_at(vec2 position)\n"
    "{\n"
    "\treturn textureLod(color_texture, clamp(position, bounds.xy, bounds.zw), 0.0).rgb;\n"
    "}\n"
    "\n"
    "float luma_at(vec2 position)\n"
    "{\n"
    "\treturn dot(color_at(position), vec3(0.299, 0.587, 0.114));\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 position = gl_FragCoord.xy * metrics.xy;\n"
    "\tfloat luma = luma_at(position);\n"
    "\tfloat up = luma_at(position + vec2(0.0, -metrics.y));\n"
    "\tfloat down = luma_at(position + vec2(0.0, metrics.y));\n"
    "\tfloat left = luma_at(position + vec2(-metrics.x, 0.0));\n"
    "\tfloat right = luma_at(position + vec2(metrics.x, 0.0));\n"
    "\tfloat highest = max(luma, max(max(up, down), max(left, right)));\n"
    "\tfloat range = highest - min(luma, min(min(up, down), min(left, right)));\n"
    "\n"
    "\t\n"
    "\tif (range < max(EDGE_THRESHOLD_MINIMUM, highest * EDGE_THRESHOLD))\n"
    "\t{\n"
    "\t\tresult = textureLod(color_texture, position, 0.0);\n"
    "\t\treturn;\n"
    "\t}\n"
    "\tfloat up_left = luma_at(position + vec2(-metrics.x, -metrics.y));\n"
    "\tfloat up_right = luma_at(position + vec2(metrics.x, -metrics.y));\n"
    "\tfloat down_left = luma_at(position + vec2(-metrics.x, metrics.y));\n"
    "\tfloat down_right = luma_at(position + vec2(metrics.x, metrics.y));\n"
    "\n"
    "\t\n"
    "\tfloat average = (2.0 * (up + down + left + right) + up_left + up_right + down_left + down_right) / 12.0;\n"
    "\tfloat subpixel = smoothstep(0.0, 1.0, clamp(abs(average - luma) / range, 0.0, 1.0));\n"
    "\tsubpixel = subpixel * subpixel * SUBPIXEL_QUALITY;\n"
    "\n"
    "\t\n"
    "\tbool horizontal = abs(up_left + down_left - 2.0 * left) + 2.0 * abs(up + down - 2.0 * luma) +\n"
    "\t\tabs(up_right + down_right - 2.0 * right) >=\n"
    "\t\tabs(up_left + up_right - 2.0 * up) + 2.0 * abs(left + right - 2.0 * luma) +\n"
    "\t\tabs(down_left + down_right - 2.0 * down);\n"
    "\tfloat before = horizontal ? up : left;\n"
    "\tfloat after = horizontal ? down : right;\n"
    "\tbool before_steeper = abs(before - luma) >= abs(after - luma);\n"
    "\tfloat gradient = 0.25 * max(abs(before - luma), abs(after - luma));\n"
    "\tfloat edge_luma = 0.5 * ((before_steeper ? before : after) + luma);\n"
    "\tvec2 across = (horizontal ? vec2(0.0, metrics.y) : vec2(metrics.x, 0.0)) * (before_steeper ? -1.0 : 1.0);\n"
    "\tvec2 along = horizontal ? vec2(metrics.x, 0.0) : vec2(0.0, metrics.y);\n"
    "\n"
    "\t\n"
    "\tvec2 end_before = position + 0.5 * across - along;\n"
    "\tvec2 end_after = position + 0.5 * across + along;\n"
    "\tfloat luma_before = luma_at(end_before) - edge_luma;\n"
    "\tfloat luma_after = luma_at(end_after) - edge_luma;\n"
    "\tbool reached_before = abs(luma_before) >= gradient;\n"
    "\tbool reached_after = abs(luma_after) >= gradient;\n"
    "\tfor (int index = 1; index < STEPS && !(reached_before && reached_after); index++)\n"
    "\t{\n"
    "\t\tif (!reached_before)\n"
    "\t\t{\n"
    "\t\t\tend_before -= along * STEP_LENGTHS[index];\n"
    "\t\t\tluma_before = luma_at(end_before) - edge_luma;\n"
    "\t\t\treached_before = abs(luma_before) >= gradient;\n"
    "\t\t}\n"
    "\t\tif (!reached_after)\n"
    "\t\t{\n"
    "\t\t\tend_after += along * STEP_LENGTHS[index];\n"
    "\t\t\tluma_after = luma_at(end_after) - edge_luma;\n"
    "\t\t\treached_after = abs(luma_after) >= gradient;\n"
    "\t\t}\n"
    "\t}\n"
    "\n"
    "\t\n"
    "\tfloat distance_before = horizontal ? position.x - end_before.x : position.y - end_before.y;\n"
    "\tfloat distance_after = horizontal ? end_after.x - position.x : end_after.y - position.y;\n"
    "\tfloat pixel_offset = 0.5 - min(distance_before, distance_after) / (distance_before + distance_after);\n"
    "\tbool varies = ((distance_before < distance_after ? luma_before : luma_after) < 0.0) != (luma < edge_luma);\n"
    "\tresult = vec4(color_at(position + max(varies ? pixel_offset : 0.0, subpixel) * across),\n"
    "\t\ttextureLod(color_texture, position, 0.0).a);\n"
    "}\n"
)
# the main functions of SMAA's passes, after SMAA.hlsl; its vertex functions'
# offsets are found for each pixel
SMAA_EDGES_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec4 offset[3];\n"
    "\tSMAAEdgeDetectionVS(texcoord, offset);\n"
    "\tresult = vec4(SMAALumaEdgeDetectionPS(texcoord, offset, color_texture), 0.0, 0.0);\n"
    "}\n"
)
SMAA_WEIGHTS_SOURCE = (
    "uniform sampler2D edges_texture;\n"
    "uniform sampler2D area_texture;\n"
    "uniform sampler2D search_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec2 pixcoord;\n"
    "\tvec4 offset[3];\n"
    "\tSMAABlendingWeightCalculationVS(texcoord, pixcoord, offset);\n"
    "\tresult = SMAABlendingWeightCalculationPS(texcoord, pixcoord, offset, edges_texture, area_texture,\n"
    "\t\tsearch_texture, vec4(0.0));\n"
    "}\n"
)
SMAA_BLEND_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "uniform sampler2D weights_texture;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 texcoord = gl_FragCoord.xy * metrics.xy;\n"
    "\tvec4 offset;\n"
    "\tSMAANeighborhoodBlendingVS(texcoord, offset);\n"
    "\tresult = vec4(SMAANeighborhoodBlendingPS(texcoord, offset, color_texture, weights_texture).rgb,\n"
    "\t\ttextureLod(color_texture, texcoord, 0.0).a);\n"
    "}\n"
)
SMAA_MAINS = (SMAA_EDGES_SOURCE, SMAA_WEIGHTS_SOURCE, SMAA_BLEND_SOURCE)
# what comes before SMAA.hlsl and a pass's main function
SMAA_PREFIX = (
    "#define SMAA_GLSL_4 1\n"
    "uniform vec4 metrics;\n"
    "#define SMAA_RT_METRICS metrics\n"
    "#define SMAA_PRESET_HIGH 1\n"
)
# each sampler's texture unit, which becomes its binding
UNITS = {"color_texture": 0, "edges_texture": 0, "area_texture": 1, "search_texture": 2, "weights_texture": 1,
         "objects_texture": 0, "depth_texture": 1,
         # the loading screen's (LOADING_SOURCE's, LOADING_CAPTION_SOURCE's and LOADING_PICTURE_SOURCE's)
         "trail_texture": 0, "still_texture": 1, "frame_texture": 1, "table_texture": 2, "word_texture": 3,
         "caption_texture": 3, "picture_texture": 0}
# Vulkan's own program: the frame turned for a swapchain the display turns
# (a phone held sideways: xgpu_vulkan_post_rotate). Each pixel takes the
# texel the bounds' matrix (its rows xy and zw) turns it to, about the
# image's centre.
ROTATE_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "/* 1 / width, 1 / height, width, height of the turned image */\n"
    "uniform vec4 metrics;\n"
    "/* the turn: rows of a 2x2 matrix, from the turned image's coordinates to the frame's */\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 centred = gl_FragCoord.xy * metrics.xy - 0.5;\n"
    "\tvec2 coordinates = vec2(dot(bounds.xy, centred), dot(bounds.zw, centred)) + 0.5;\n"
    "\tresult = vec4(texture(color_texture, coordinates).rgb, 1.0);\n"
    "}\n"
)
# display.panini's projection (both backends': xgpu_vulkan_post_panini,
# xgpu_metal_post_panini; port/linux/include/panini_projection.h has the
# arithmetic, which this is the same as). Its uniforms: metrics, the
# target's; bounds, the rectangle's outermost texel centres (no texel of
# another window is read); window, the 3D view's corners in the target's
# pixels (x0, y0, width, height); panini, the projection's d, s and
# magnification at the middle; tangents, the rectilinear view's at the
# view's left, right, top and bottom. Each pixel takes the copy's pixel the
# rectilinear view saw its direction at, sampled over the pixels it covers
# there (the sides are drawn smaller): a bilinear tap for each two pixels
# each way, so that the taps' filters together cover them (up to 4 taps, 8
# pixels)
PANINI_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 window;\n"
    "uniform vec4 panini;\n"
    "uniform vec4 tangents;\n"
    "out vec4 result;\n"
    "\n"
    "/* the rectilinear tangents a point of the screen at tangents p shows */\n"
    "vec2 rectilinear(vec2 p)\n"
    "{\n"
    "\tfloat d = panini.x;\n"
    "\tvec2 projected = p / panini.z;\n"
    "\tfloat k = projected.x * projected.x / ((d + 1.0) * (d + 1.0));\n"
    "\tfloat c = max((sqrt(1.0 + k * (1.0 - d * d)) - k * d) / (k + 1.0), 1.0e-6);\n"
    "\tfloat S = (d + 1.0) / (d + c);\n"
    "\treturn vec2(projected.x / (S * c), projected.y / (S * (panini.y + (1.0 - panini.y) * c)));\n"
    "}\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 place = (gl_FragCoord.xy - window.xy) / window.zw;\n"
    "\tvec2 seen = rectilinear(vec2(mix(tangents.x, tangents.y, place.x), mix(tangents.z, tangents.w, place.y)));\n"
    "\tvec2 source = window.xy + window.zw * (seen - tangents.xz) / (tangents.yw - tangents.xz);\n"
    "\tvec2 across = dFdx(source);\n"
    "\tvec2 down = dFdy(source);\n"
    "\tivec2 taps = ivec2(clamp(ceil(vec2(length(across), length(down)) * 0.5 - 0.01), 1.0, 4.0));\n"
    "\tvec4 sum = vec4(0.0);\n"
    "\tfor (int row = 0; row < taps.y; row++)\n"
    "\t{\n"
    "\t\tfor (int column = 0; column < taps.x; column++)\n"
    "\t\t{\n"
    "\t\t\tvec2 offset = (vec2(column, row) + 0.5) / vec2(taps) - 0.5;\n"
    "\t\t\tvec2 at = (source + across * offset.x + down * offset.y) * metrics.xy;\n"
    "\t\t\tsum += textureLod(color_texture, clamp(at, bounds.xy, bounds.zw), 0.0);\n"
    "\t\t}\n"
    "\t}\n"
    "\tresult = sum / float(taps.x * taps.y);\n"
    "}\n"
)
# display.temporal_aa's motion (xgpu_vulkan_stream.c's MOTION records, docs/temporal-aa.md): each pixel's movement
# from where it was the frame before, as a fraction of the view (previous less current, x right, y down; the
# camera's jitter not in it). Its uniforms: window, the view's corners in the target's pixels (x0, y0, width,
# height); reproject0 to 3, the rows of the matrix from a point's clip position this frame (unjittered: the
# game's own projection, depth 0 near to 1 far) to its clip position the frame before, the camera's alone;
# motion, the offset of the drawn pixels from the clip positions (the jitter and the screen's half pixel, as
# normalized coordinates), whether the depth is reversed, and whether the window has a frame before (0: a cut,
# none). The objects' own (objects_texture, written by their draws as the second attachment: xgpu_spirv.c's
# motion) are kept where they are the surface the depth has (their depth the same); elsewhere the camera's.
MOTION_RESOLVE_SOURCE = (
    "uniform sampler2D objects_texture;\n"
    "uniform sampler2D depth_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 window;\n"
    "uniform vec4 reproject0;\n"
    "uniform vec4 reproject1;\n"
    "uniform vec4 reproject2;\n"
    "uniform vec4 reproject3;\n"
    "uniform vec4 motion;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tivec2 texel = ivec2(gl_FragCoord.xy);\n"
    "\tvec4 object = texelFetch(objects_texture, texel, 0);\n"
    "\tfloat depth = texelFetch(depth_texture, texel, 0).r;\n"
    "\tif (object.w > 0.5 && abs(object.z - depth) <= max(1.5e-7, depth * 1.0e-6))\n"
    "\t{\n"
    "\t\tresult = vec4(object.xy, 0.0, 1.0);\n"
    "\t\treturn;\n"
    "\t}\n"
    "\tif (motion.w == 0.0)\n"
    "\t{\n"
    "\t\tresult = vec4(0.0);\n"
    "\t\treturn;\n"
    "\t}\n"
    "\tvec2 place = (gl_FragCoord.xy - window.xy) / window.zw;\n"
    "\tvec2 ndc = vec2(place.x * 2.0 - 1.0, 1.0 - place.y * 2.0) - motion.xy;\n"
    "\tvec4 current = vec4(ndc, motion.z != 0.0 ? 1.0 - depth : depth, 1.0);\n"
    "\tvec4 previous = vec4(dot(reproject0, current), dot(reproject1, current), dot(reproject2, current),\n"
    "\t\tdot(reproject3, current));\n"
    "\tvec2 before = previous.w > 0.0 ? previous.xy / previous.w : ndc;\n"
    "\tresult = vec4((before - ndc) * vec2(0.5, -0.5), 0.0, 0.0);\n"
    "}\n"
)
# debug.motion_vectors: the motion drawn over the view as colour, in the view's pixels: grey still, more red
# to the right and green downward (cyan and magenta the other ways), full at 8 pixels a frame
MOTION_VIEW_SOURCE = (
    "uniform sampler2D color_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 window;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tvec2 moved = texelFetch(color_texture, ivec2(gl_FragCoord.xy), 0).xy * window.zw;\n"
    "\tvec2 shown = clamp(moved / 8.0, -1.0, 1.0);\n"
    "\tresult = vec4(0.5 + 0.5 * shown.x, 0.5 + 0.5 * shown.y, 0.5 - 0.25 * (shown.x + shown.y), 1.0);\n"
    "}\n"
)
# the clears' pixel shader (its uniforms at the passes' binding)
# The loading screen's sharp frame, drawn by the GPU at the screen's rows
# (port/linux/src/loading_art.h, "the sharp frame drawn by the GPU"): each
# pixel as loading_art_hd_rows draws it, in its integer arithmetic, from the
# 480 rows' trail between the last two steps and the sweep's glint
# (trail_texture: the step before's rows 0 to 479, the last's 480 to 959,
# its alpha the glint), the scene's still layers packed (still_texture,
# loading_art_hd_pack's) or, without a scene, the picture resampled
# (LOADING_PICTURE_SOURCE's), the picture's hues and the sweep's mask
# (table_texture, loading_art_hd_table's) and the words (word_texture: their
# coverage, red, green, blue, over the box window gives). Every texture is
# read texel by texel (texelFetch), no filter.
LOADING_COMMON = """
ivec4 texel_of(sampler2D image, ivec2 place)
{
\treturn ivec4(round(texelFetch(image, place, 0) * 255.0));
}

/* the sweep's mask (loading_art_mask) */
int mask_at(sampler2D table, int x, int y)
{
\treturn x < 0 || x >= 128 ? 5 : texel_of(table, ivec2(x, 16 + y)).r;
}

/* the mask at a place of the 480 rows' columns (column, its 16 rows' row
in 256ths): loading_art_hd_compose's and loading_art_hd_rows' */
int mask_between(sampler2D table, float column, int mask_row)
{
\tfloat whole = floor(column);
\tint ix = int(clamp(whole, -1.0, 128.0));
\tint fx = int((column - whole) * 256.0);
\tint iy = mask_row >> 8, my = mask_row & 255, iy1 = min(iy + 1, 15);
\tint upper = mask_at(table, ix, iy) * (256 - fx) + mask_at(table, ix + 1, iy) * fx;
\tint lower = mask_at(table, ix, iy1) * (256 - fx) + mask_at(table, ix + 1, iy1) * fx;
\treturn (upper * (256 - my) + lower * my + 32768) >> 16;
}

/* (2 * pixel + 1) * size * 128 / shown - 128, without overflow */
int sample_place(int pixel, int size, int shown)
{
\tint a = (2 * pixel + 1) * size;
\treturn (a / shown) * 128 + ((a % shown) * 128) / shown - 128;
}
"""

LOADING_SOURCE = (
    "uniform sampler2D trail_texture;\n"
    "uniform sampler2D still_texture;\n"
    "uniform sampler2D table_texture;\n"
    "uniform sampler2D word_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 hdr;\n"
    "uniform vec4 window;\n"
    "out vec4 result;\n"
    + LOADING_COMMON + """
/* a row of the trail between the steps (fraction of 256), at a column and
the next by f (of 256): red, green, blue and glint, of 65536 */
ivec4 trail_row(int row, int c0, int c1, int f, int fraction)
{
\tivec4 p0 = texel_of(trail_texture, ivec2(c0, row)), q0 = texel_of(trail_texture, ivec2(c0, row + 480));
\tivec4 p1 = texel_of(trail_texture, ivec2(c1, row)), q1 = texel_of(trail_texture, ivec2(c1, row + 480));
\tivec3 a = (p0.rgb * (256 - fraction) + q0.rgb * fraction + 128) >> 8;
\tivec3 b = (p1.rgb * (256 - fraction) + q1.rgb * fraction + 128) >> 8;
\treturn ivec4(a * (256 - f) + b * f, q0.a * (256 - f) + q1.a * f);
}

void main()
{
\tivec2 p = ivec2(gl_FragCoord.xy);
\tint width = int(metrics.x), height = int(metrics.y), frame_width = int(metrics.z);
\tint fraction = int(bounds.x), base = int(bounds.y), completion = int(bounds.z), light_scale = int(bounds.w);
\t/* (the 480 rows' column and row of the pixel's middle, in 256ths) */
\tint column = clamp(sample_place(p.x, frame_width, width), 0, (frame_width - 1) * 256 - 1);
\tint c0 = column >> 8, f = column & 255, c1 = min(c0 + 1, frame_width - 1);
\tint row = max(((2 * p.y + 1) * 480 * 128) / height - 128, 0);
\tint r0 = row >> 8, fy = row & 255;
\tif (r0 >= 479)
\t{
\t\tr0 = 479;
\t\tfy = 0;
\t}
\tint r1 = min(r0 + 1, 479);
\tivec4 upper = trail_row(r0, c0, c1, f, fraction), lower = trail_row(r1, c0, c1, f, fraction);
\tivec4 trail = (upper * (256 - fy) + lower * fy + 32768) >> 16;
\t/* the sweep's band at the pixel, at its brightness */
\tprecise float place = (float(p.x) + 0.5) * hdr.y - 0.5 + hdr.x;
\tint mask_row = clamp(((2 * p.y + 1) * 16 * 128) / height - 128, 0, 15 * 256);
\tint mask = mask_between(table_texture, place, mask_row);
\tint scale = (light_scale * mask + 128) >> 8;
\tivec4 still = texel_of(still_texture, p);
\tivec3 value = trail.rgb;
\tif (metrics.w > 0.5)
\t{
\t\t/* the scene: its colour and what the sweep lights of it (its glint
\t\tadded), the words over both */
\t\tint hue = still.a & 15, white = (still.a >> 4) & 1, sky = (still.a >> 5) & 1;
\t\tint light = still.g | ((still.a >> 6) << 8), edge = still.b;
\t\tint shown = sky != 0 ? base * 112 / 24 : base;
\t\tint lit = light + trail.a * (64 + edge) * 7 / 2048;
\t\tif (white == 0 && lit > 110)
\t\t\tlit = min(110 + (lit - 110) / 3, 190);
\t\tivec3 feed = texel_of(table_texture, ivec2(min(lit, 255), hue)).rgb;
\t\tivec3 scene = texel_of(table_texture, ivec2(still.r, hue)).rgb;
\t\tif (p.x >= int(window.x) && p.x < int(window.z) && p.y >= int(window.y) && p.y < int(window.w))
\t\t{
\t\t\tivec4 word = texel_of(word_texture, p - ivec2(window.xy));
\t\t\tif (word.r != 0)
\t\t\t{
\t\t\t\tscene = scene + (word.gba - scene) * word.r / 255;
\t\t\t\tfeed = feed + (word.gba - feed) * word.r / 255;
\t\t\t}
\t\t}
\t\tvalue = min(value + (feed * scale + 32512) / 65025 + ((scene * shown) >> 16), ivec3(255));
\t\t/* (kept in the picture's blues) */
\t\tvalue.r = min(value.r, (value.b * 196) >> 8);
\t\tvalue.g = min(value.g, (value.b * 228) >> 8);
\t}
\telse
\t\tvalue = min(value + (still.rgb * scale + 32512) / 65025, ivec3(255));
\tif (completion < 256)
\t\tvalue = (value * completion) >> 8;
\tresult = vec4(vec3(value) / 255.0, 1.0);
}
""")

# The caption over the sharp frame at the screen's pixels
# (loading_art_caption_shown's, loading_art_caption_gpu's uniforms): the
# frame (frame_texture) as the presenter's filter shows it, and where the
# caption's letters are (caption_texture's alpha; its colour the words' of
# the row) the words' light (loading_art_caption_lit's). Drawn over the
# caption's box (window: its top left in the shown frame; panini: the shown
# frame's top left in the target), its viewport the box.
LOADING_CAPTION_SOURCE = (
    "uniform sampler2D trail_texture;\n"
    "uniform sampler2D frame_texture;\n"
    "uniform sampler2D table_texture;\n"
    "uniform sampler2D caption_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "uniform vec4 hdr;\n"
    "uniform vec4 window;\n"
    "uniform vec4 panini;\n"
    "out vec4 result;\n"
    + LOADING_COMMON + """
int trail_bilinear(int x0, int x1, int y0, int y1, int fx, int fy, int channel)
{
\tint upper = texel_of(trail_texture, ivec2(x0, y0))[channel] * (256 - fx) +
\t\ttexel_of(trail_texture, ivec2(x1, y0))[channel] * fx;
\tint lower = texel_of(trail_texture, ivec2(x0, y1))[channel] * (256 - fx) +
\t\ttexel_of(trail_texture, ivec2(x1, y1))[channel] * fx;
\treturn (upper * (256 - fy) + lower * fy + 32768) >> 16;
}

void main()
{
\tivec2 shown = ivec2(metrics.xy), frame = ivec2(metrics.zw);
\tivec2 s = ivec2(gl_FragCoord.xy) - ivec2(panini.xy);
\tivec4 caption = texel_of(caption_texture, s - ivec2(window.xy));
\t/* the frame there, as the bilinear filter shows it */
\tint px = clamp(sample_place(s.x, frame.x, shown.x), 0, (frame.x - 1) * 256);
\tint py = clamp(sample_place(s.y, frame.y, shown.y), 0, (frame.y - 1) * 256);
\tint x0 = px >> 8, x1 = min(x0 + 1, frame.x - 1), across = px & 255;
\tint y0 = py >> 8, y1 = min(y0 + 1, frame.y - 1), down = py & 255;
\tivec3 above = texel_of(frame_texture, ivec2(x0, y0)).rgb * (256 - across) +
\t\ttexel_of(frame_texture, ivec2(x1, y0)).rgb * across;
\tivec3 below = texel_of(frame_texture, ivec2(x0, y1)).rgb * (256 - across) +
\t\ttexel_of(frame_texture, ivec2(x1, y1)).rgb * across;
\tivec3 value = (above * (256 - down) + below * down + 32768) >> 16;
\tif (caption.a != 0)
\t{
\t\tint frame_width = int(hdr.y), fraction = int(bounds.x), base = int(bounds.y);
\t\tint completion = int(bounds.z);
\t\tprecise float x = (float(s.x) + 0.5) * hdr.y / metrics.x - 0.5;
\t\tprecise float y = (float(s.y) + 0.5) * 480.0 / metrics.y - 0.5;
\t\tx = clamp(x, 0.0, float(frame_width - 1));
\t\ty = clamp(y, 0.0, 479.0);
\t\tint lx0 = int(x), ly0 = int(y);
\t\tint fx = int((x - float(lx0)) * 256.0), fy = int((y - float(ly0)) * 256.0);
\t\tint lx1 = min(lx0 + 1, frame_width - 1), ly1 = min(ly0 + 1, 479);
\t\tprecise float column = x + hdr.x;
\t\tint mask_row = clamp(int((y + 0.5) * 16.0 * 256.0 / 480.0) - 128, 0, 15 * 256);
\t\tint scale = (int(bounds.w) * mask_between(table_texture, column, mask_row) + 128) >> 8;
\t\tivec3 lit;
\t\tfor (int channel = 0; channel < 3; channel++)
\t\t{
\t\t\tint before = trail_bilinear(lx0, lx1, ly0, ly1, fx, fy, channel);
\t\t\tint after = trail_bilinear(lx0, lx1, ly0 + 480, ly1 + 480, fx, fy, channel);
\t\t\tint word = caption[channel];
\t\t\tlit[channel] = min(((before * (256 - fraction) + after * fraction + 128) >> 8) +
\t\t\t\t(word * scale + 32512) / 65025 + ((word * base) >> 16), 255);
\t\t}
\t\tif (hdr.z > 0.5)
\t\t{
\t\t\tlit.r = min(lit.r, (lit.b * 196) >> 8);
\t\t\tlit.g = min(lit.g, (lit.b * 228) >> 8);
\t\t}
\t\tif (completion < 256)
\t\t\tlit = (lit * completion) >> 8;
\t\tvalue = value + (lit - value) * caption.a / 255;
\t}
\tresult = vec4(vec3(value) / 255.0, 1.0);
}
""")

# Without a scene, the picture at its own size (picture_texture: 1600x1200,
# or the 640x480 one) resampled once a load to the sharp frame's pixels, as
# loading_art_source_resample does on the processor: Catmull-Rom's cubic
# each way, widened by as many of the picture's pixels as a pixel covers
# where it covers more than one, edges clamped (metrics: the picture's
# width, height and its pixels to the 480 rows'; bounds: a and b across
# and down, loading_art_hd_picture_uniforms').
LOADING_PICTURE_SOURCE = (
    "uniform sampler2D picture_texture;\n"
    "\n"
    "uniform vec4 metrics;\n"
    "uniform vec4 bounds;\n"
    "out vec4 result;\n"
    """
float cubic(float t)
{
\tt = abs(t);
\treturn t < 1.0 ? (1.5 * t - 2.5) * t * t + 1.0 : t < 2.0 ? ((-0.5 * t + 2.5) * t - 4.0) * t + 2.0 : 0.0;
}

void main()
{
\tivec2 p = ivec2(gl_FragCoord.xy), size = ivec2(metrics.xy);
\tfloat units = metrics.z;
\tvec2 step = bounds.xz * units;
\tvec2 wide = max(step, vec2(1.0));
\tvec2 middle = (vec2(p) + 0.5) * step + bounds.yw * units - 0.5;
\tint left = int(floor(middle.x - 2.0 * wide.x)) + 1, right = int(floor(middle.x + 2.0 * wide.x));
\tint top = int(floor(middle.y - 2.0 * wide.y)) + 1, bottom = int(floor(middle.y + 2.0 * wide.y));
\tfloat total_x = 0.0, total_y = 0.0;
\tfor (int i = left; i <= right; i++)
\t\ttotal_x += cubic((float(i) - middle.x) / wide.x);
\tvec3 sum = vec3(0.0);
\tfor (int j = top; j <= bottom; j++)
\t{
\t\tfloat weight_y = cubic((float(j) - middle.y) / wide.y);
\t\tvec3 across = vec3(0.0);
\t\ttotal_y += weight_y;
\t\tfor (int i = left; i <= right; i++)
\t\t\tacross += cubic((float(i) - middle.x) / wide.x) *
\t\t\t\ttexelFetch(picture_texture, clamp(ivec2(i, j), ivec2(0), size - 1), 0).rgb * 255.0;
\t\tsum += weight_y * (total_x != 0.0 ? across / total_x : vec3(0.0));
\t}
\tvec3 value = total_y != 0.0 ? sum / total_y : vec3(0.0);
\tresult = vec4(clamp(floor(value + 0.5), 0.0, 255.0) / 255.0, 1.0);
}
""")

# the samples of a pixel (metrics.x of them) made one: the mean of their
# light (and of their alpha), encoded again, written to each of them; a
# pixel whose samples are all alike (inside a surface, most of the view)
# is that mean already, and is left as it is, unwritten
RESOLVE_LIGHT_SOURCE = (
    "layout(input_attachment_index = 0, set = 2, binding = 0) uniform subpassInputMS frame_input;\n"
    "uniform vec4 metrics;\n"
    "out vec4 result;\n"
    "\n"
    "void main()\n"
    "{\n"
    "\tint samples = max(int(metrics.x), 1);\n"
    "\tvec4 first = subpassLoad(frame_input, 0);\n"
    "\tbool edge = false;\n"
    "\tfor (int index = 1; index < samples; index++)\n"
    "\t\tedge = edge || any(notEqual(subpassLoad(frame_input, index), first));\n"
    "\tif (!edge)\n"
    "\t\tdiscard;\n"
    "\tvec4 sum = vec4(0.0);\n"
    "\tfor (int index = 0; index < samples; index++)\n"
    "\t{\n"
    "\t\tvec4 value = subpassLoad(frame_input, index);\n"
    "\t\tsum += vec4(hdr_linear(value.rgb), value.a);\n"
    "\t}\n"
    "\tsum /= float(samples);\n"
    "\tresult = vec4(hdr_encoded(sum.rgb), sum.a);\n"
    "}\n"
)

CLEAR = """#version 450
layout(std140, set = 0, binding = 1) uniform ClearUniforms
{
\tvec4 color;
};
layout(location = 0) out vec4 result;
void main()
{
\tresult = color;
}
"""
# display.contact_shadows' pass: its block (std140, set 0, binding 1; as
# xgpu_vulkan_contact_shadows.c's struct contact_header writes it) and the
# depth (set 1, binding 0). unproject's rows take the framebuffer's (x, y,
# depth, 1) to the world's homogeneous point; project's the world's to the
# framebuffer's, times w (the camera's distance ahead, times view.z's
# inverse); camera's w the casters' count; march the length, thickness,
# start and depth bias (its share of the camera's distance in w); view the
# fade's start and 1 / its length, 1 / project[3]'s length, the strength;
# bounds the viewport's pixels (x0, y0, x1, y1). Each caster two vec4s:
# its bounding sphere, and its direction's x and y, its darkness's bits
# (red lowest). The light before the classic receivers (set 1, binding 1),
# by texel, at the pixel's place.
CONTACT_SHADOWS = """#version 450
const int STEPS = 12;
const int CASTERS = 48;
layout(std140, set = 0, binding = 1) uniform ContactUniforms
{
\tvec4 unproject[4];
\tvec4 project[4];
\tvec4 camera;
\tvec4 march;
\tvec4 view;
\tvec4 bounds;
\tvec4 reserved[4];
\tvec4 casters[CASTERS * 2];
};
layout(set = 1, binding = 0) uniform sampler2D depth_texture;
layout(set = 1, binding = 1) uniform sampler2D light_texture;
layout(location = 0) out vec4 result;

float depth_at(vec2 pixel)
{
#if REVERSED_DEPTH
\treturn 1.0 - texelFetch(depth_texture, ivec2(pixel), 0).r;
#else
\treturn texelFetch(depth_texture, ivec2(pixel), 0).r;
#endif
}

// (a pixel's point in the world, from its depth; one of the sky's far away)
vec3 point_at(vec2 pixel)
{
\tfloat depth = depth_at(pixel);
\tif (depth >= 1.0)
\t\treturn vec3(1.0e30);
\tvec4 h = vec4(dot(unproject[0], vec4(pixel, depth, 1.0)), dot(unproject[1], vec4(pixel, depth, 1.0)),
\t\tdot(unproject[2], vec4(pixel, depth, 1.0)), dot(unproject[3], vec4(pixel, depth, 1.0)));
\treturn h.xyz / h.w;
}

// (the way along the surface toward a neighbour pixel, by the nearer of the
// two on either side: across an object's outline the other side's point is
// another surface's, far off)
vec3 along_surface(vec3 p, vec2 pixel, vec2 offset)
{
\tvec3 a = point_at(clamp(pixel + offset, bounds.xy + 0.5, bounds.zw - 0.5)) - p;
\tvec3 b = p - point_at(clamp(pixel - offset, bounds.xy + 0.5, bounds.zw - 0.5));
\treturn dot(a, a) < dot(b, b) ? a : b;
}

void main()
{
\tvec2 pixel = gl_FragCoord.xy;
\tfloat depth = depth_at(pixel);
\tif (depth >= 1.0)
\t\tdiscard;
\tvec3 p = point_at(pixel);
\t// (the surface's normal from its own neighbours, facing the camera: a
\t// pixel facing away from a light gets no shadow from it, as its own
\t// surface would be found in its way. Not dFdx and dFdy's, whose 2x2
\t// quads straddle an object's outline: the floor's pixels beside a body
\t// took a normal half the body's, a dotted fringe)
\tvec3 n = cross(along_surface(p, pixel, vec2(1.0, 0.0)), along_surface(p, pixel, vec2(0.0, 1.0)));
\tn *= dot(n, camera.xyz - p) < 0.0 ? -1.0 : 1.0;
\tn /= max(length(n), 1.0e-6);
\tfloat distance_ = length(camera.xyz - p);
\tfloat faded = 1.0 - clamp((distance_ - view.x) * view.y, 0.0, 1.0);
\tif (faded <= 0.0)
\t\tdiscard;
\tfloat bias = march.w * distance_;
\tint count = min(int(camera.w), CASTERS);
\tvec3 light = vec3(1.0);
\tfor (int i = 0; i < count; i++)
\t{
\t\tvec4 a = casters[i * 2], b = casters[i * 2 + 1];
\t\tvec3 v = vec3(b.xy, -sqrt(max(1.0 - dot(b.xy, b.xy), 0.0)));
\t\tvec3 rel = p - a.xyz;
\t\tfloat along = dot(rel, v);
\t\tvec3 side = rel - v * along;
\t\t// (its volume: half its radius toward the light, twice it away, and a
\t\t// quarter again to each side)
\t\tif (along < -0.5 * a.w || along > 2.0 * a.w || dot(side, side) > 1.5625 * a.w * a.w)
\t\t\tcontinue;
\t\tvec3 l = -v;
\t\tfloat facing = dot(n, l);
\t\tif (facing <= 0.05)
\t\t\tcontinue;
\t\tvec3 from = p + n * (march.z + bias);
\t\t// (the same steps for every pixel, no noise: no temporal pass
\t\t// averages a pixel's noise away under SMAA, FXAA or none, and noise
\t\t// tied to the screen crawls over the floor as the view moves. Where
\t\t// the march went behind the depth is found between the step before
\t\t// and the first behind, so the shadow's darkness follows the caster
\t\t// smoothly, not a step at a time)
\t\tfloat step_ = march.x / float(STEPS);
\t\tfloat hit = 0.0;
\t\tfloat t_before = 0.0, behind_before = 0.0;
\t\tfor (int k = 0; k < STEPS; k++)
\t\t{
\t\t\tfloat t = step_ * float(k + 1);
\t\t\tvec4 q = vec4(from + l * t, 1.0);
\t\t\tfloat w = dot(project[3], q);
\t\t\tif (w <= 0.0)
\t\t\t\tbreak;
\t\t\tvec2 s = vec2(dot(project[0], q), dot(project[1], q)) / w;
\t\t\tif (any(lessThan(s, bounds.xy)) || any(greaterThanEqual(s, bounds.zw)))
\t\t\t\tbreak;
\t\t\tfloat scene = depth_at(s);
\t\t\tif (scene >= 1.0)
\t\t\t{
\t\t\t\tt_before = t;
\t\t\t\tbehind_before = -1.0;
\t\t\t\tcontinue;
\t\t\t}
\t\t\t// (how far the step is behind what the depth holds there, in world
\t\t\t// units: a world point's 1 / w is its unprojection's w)
\t\t\tfloat behind = (w - 1.0 / dot(unproject[3], vec4(s, scene, 1.0))) * view.z;
\t\t\tif (behind > bias && behind < march.y)
\t\t\t{
\t\t\t\t// (where it crossed: between the step before, in front, and
\t\t\t\t// this one; after a step behind something thicker, here)
\t\t\t\tfloat crossed = behind_before <= bias ?
\t\t\t\t\tmix(t_before, t, clamp((bias - behind_before) / (behind - behind_before), 0.0, 1.0)) : t;
\t\t\t\t// (more the nearer it is, and none at the march's end: no edge
\t\t\t\t// where a caster leaves its reach)
\t\t\t\thit = (1.0 - 0.5 * crossed / march.x) * (1.0 - smoothstep(0.6 * march.x, march.x, crossed));
\t\t\t\tbreak;
\t\t\t}
\t\t\tt_before = t;
\t\t\tbehind_before = behind;
\t\t}
\t\tfloat ends = along >= 0.0 ? 1.0 - along / (2.0 * a.w) : 1.0 + along / (0.5 * a.w);
\t\tvec3 darkness = unpackUnorm4x8(floatBitsToUint(b.z)).rgb;
\t\tlight *= 1.0 - darkness * (hit * clamp(ends, 0.0, 1.0) * smoothstep(0.05, 0.3, facing) * faded * view.w);
\t}
\tif (all(greaterThanEqual(light, vec3(0.998))))
\t\tdiscard;
\t// (the light before the classic receivers, shadowed; the blend's MIN keeps
\t// the darker of it and what they left)
\tresult = vec4(texelFetch(light_texture, ivec2(pixel), 0).rgb * light, 1.0);
}
"""
# SMAA's lookup textures: their file, size and channels
TEXTURES = (("area", "area_tex.zlib", 160, 560, 2), ("search", "search_tex.zlib", 64, 16, 1))


def literal(text: str) -> str:
    """The C string literals of text (comments removed), joined."""
    return "".join(ast.literal_eval(value) for value in STRINGS.findall(COMMENTS.sub("", text)))


def declaration(source: str, name: str) -> str:
    match = re.search(r"(?:static )?const char " + name + r"\[\]\s*=\s*(.*?);\n", source, re.S)
    if not match:
        raise ValueError(f"{name} not found in {HDR_SOURCE.name}")
    return literal(match[1])


def smaa_hlsl() -> str:
    """SMAA.hlsl, its few characters beyond ASCII (in comments) spaces."""
    return bytes(byte if byte < 0x80 else 0x20 for byte in (SMAA / "SMAA.hlsl").read_bytes()).decode("ascii")


def hdr_programs() -> dict[str, str]:
    """display.hdr's fragment programs (xgpu_hdr.c): the tone map, in place
    (the pixel drawn over is the pass's input attachment), and the encoding,
    each its main function after the functions they share."""
    source = HDR_SOURCE.read_text()
    functions = declaration(source, "xgpu_hdr_functions_source")
    return {name: functions + declaration(source, f"{name}_source") for name in ("tone_map", "encode")}


def resolve_light_program() -> str:
    """The MSAA resolve in light: display.hdr's functions (xgpu_hdr.c), its
    main Vulkan's own (RESOLVE_LIGHT_SOURCE)."""
    return declaration(HDR_SOURCE.read_text(), "xgpu_hdr_functions_source") + RESOLVE_LIGHT_SOURCE


def multisampled(code: str) -> str:
    """The tone map of a multisampled target: each sample's own pixel (which
    makes the pass run for each sample)."""
    changed = code.replace("uniform subpassInput ", "uniform subpassInputMS ").replace(
        "subpassLoad(frame_input)", "subpassLoad(frame_input, gl_SampleID)")
    if changed.count("subpassInputMS") != 1 or "gl_SampleID" not in changed:
        raise ValueError("the tone map's input attachment changed")
    return changed


def translate(stage: str, code: str, bindings: dict[str, int]) -> str:
    found = UNIFORM.findall(code)
    unknown = set(found) - set(UNIFORMS)
    if unknown:
        raise ValueError(f"uniforms {sorted(unknown)} are not in the block")
    code = UNIFORM.sub("", code)
    used = max([1] + [UNIFORMS.index(name) for name in found])

    def sampler(match):
        return f"layout(set = 1, binding = {bindings[match[1]]}) uniform sampler2D {match[1]};\n"

    code = SAMPLER.sub(sampler, code)
    code = code.replace("out vec4 result;\n", "layout(location = 0) out vec4 result;\n")
    code = code.replace("gl_VertexID", "gl_VertexIndex")
    block = ""
    if stage == "frag":
        block = "layout(std140, set = 0, binding = 1) uniform PostUniforms\n{\n"
        block += "".join(f"\tvec4 {name};\n" for name in UNIFORMS[:used + 1]) + "};\n"
    return "#version 450\n" + block + code


def words_array(name: str, data: bytes, public: bool = False) -> str:
    data += b"\0" * (-len(data) % 4)
    words = struct.unpack(f"<{len(data) // 4}I", data)
    lines = [f"{'' if public else 'static '}const uint32_t {name}[] =\n{{"]
    for index in range(0, len(words), 8):
        lines.append("\t" + ", ".join(f"0x{word:08x}" for word in words[index:index + 8]) + ",")
    lines.append("};\n")
    return "\n".join(lines)


def generate(args):
    build = args.build_dir
    build.mkdir(parents=True, exist_ok=True)
    hlsl = smaa_hlsl()
    programs = {
        "post": ("vert", VERTEX_SOURCE),
        "fxaa": ("frag", FXAA_SOURCE),
    }
    for name, main in zip(PROGRAMS[1:4], SMAA_MAINS):
        programs[name] = ("frag", SMAA_PREFIX + hlsl + "\n" + main)
    programs["rotate"] = ("frag", ROTATE_SOURCE)
    hdr = hdr_programs()
    programs["hdr_tone_map"] = ("frag", hdr["tone_map"])
    programs["hdr_tone_map_ms"] = ("frag", multisampled(hdr["tone_map"]))
    programs["hdr_encode"] = ("frag", hdr["encode"])
    programs["panini"] = ("frag", PANINI_SOURCE)
    programs["motion_resolve"] = ("frag", MOTION_RESOLVE_SOURCE)
    programs["motion_view"] = ("frag", MOTION_VIEW_SOURCE)
    programs["loading"] = ("frag", LOADING_SOURCE)
    programs["loading_caption"] = ("frag", LOADING_CAPTION_SOURCE)
    programs["loading_picture"] = ("frag", LOADING_PICTURE_SOURCE)
    programs["resolve_light"] = ("frag", resolve_light_program())
    programs["clear"] = ("frag", CLEAR)
    programs["contact_shadows"] = ("frag", CONTACT_SHADOWS.replace("#version 450\n", "#version 450\n"
                                                                   "#define REVERSED_DEPTH 1\n", 1))
    text = [
        "/* Generated by tools/vulkan_post_shaders.py from its GLSL, xgpu_hdr.c and port/third_party/smaa. "
        "Do not edit. */\n",
        '#include "xgpu_vulkan_post_shaders.h"\n',
    ]
    for name, (stage, code) in programs.items():
        path = build / f"{name}.{stage}"
        path.write_text(code if name in ("clear", "contact_shadows") else translate(stage, code, UNITS))
        spv = path.with_suffix(f".{stage}.spv")
        spv.unlink(missing_ok=True)
        subprocess.run([args.compiler, "-V", "--target-env", "vulkan1.1", "-o", str(spv), str(path)],
                       check=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        subprocess.run([args.validator, "--target-env", "vulkan1.1", str(spv)], check=True)
        text.append(words_array(f"{name}_{stage}", spv.read_bytes()))
        print(f"{name}: {spv.stat().st_size} bytes of SPIR-V")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_vertex =\n{")
    text.append("\tpost_vert, sizeof(post_vert) / 4,\n};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_programs[XGPU_VULKAN_POST_PROGRAM_COUNT] =\n{")
    for name in PROGRAMS:
        text.append(f"\t{{ {name}_frag, sizeof({name}_frag) / 4 }},")
    text.append("};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_clear =\n{")
    text.append("\tclear_frag, sizeof(clear_frag) / 4,\n};\n")
    text.append("const struct xgpu_vulkan_post_module xgpu_vulkan_post_contact_shadows =\n{")
    text.append("\tcontact_shadows_frag, sizeof(contact_shadows_frag) / 4,\n};\n")
    for name, file, width, height, channels in TEXTURES:
        texels = zlib.decompress((SMAA / file).read_bytes())
        if len(texels) != width * height * channels:
            raise ValueError(f"{file} is {len(texels)} bytes, not {width}x{height}x{channels}")
        text.append(words_array(f"xgpu_vulkan_post_{name}_texture", texels, public=True))
    generated = "\n".join(text)
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text() != generated:
            raise SystemExit(f"{OUTPUT.relative_to(ROOT)} differs; regenerate it")
    else:
        OUTPUT.write_text(generated)
    print(f"{len(programs)} modules validated for Vulkan 1.1")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compiler", default="glslangValidator")
    parser.add_argument("--validator", default="spirv-val")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/vulkan_post_shaders")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        generate(args)
    except subprocess.CalledProcessError as error:
        if error.stdout:
            print(error.stdout.decode())
        raise


if __name__ == "__main__":
    main()
