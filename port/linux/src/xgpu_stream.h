/*
XGPU_STREAM.H

The renderer's command stream: a frame's backend calls (xgpu_backend.h)
written by the game as plain data into a ring in its own memory, and
played back by a backend on the 64-bit host, on a thread of its own, while
the game goes on to the next frame. On the Linux arm64 build, Android and
macOS the game is ILP32 guest code: one host call a frame hands the frame
over instead of a crossing for each GL call, the backend's driver work
leaves the game's thread, and in the VR mode the host can take the newest
head pose just before it submits.

Both sides include this file, so it holds only fixed-width integers and
floats, laid out alike by the guest's ILP32 compiler and the host's LP64
one: every field is 4 bytes or smaller and aligned to its size, every
record a multiple of 4 bytes (the sizes are checked at the end). The
interface's own structures cannot cross as they are: unsigned long and
DWORD are 4 bytes in the guest and 8 on the host.

The guest writes (xgpu_stream_encode.c), the host reads
(port/android/host/host_render_stream.c, which calls the backend's
functions: host_render_stream.h). Guest data is a guest address: the
host's view of it is host_address(address) (port/android/host/host.h). Data
the game rewrites while the frame waits (streamed vertices, indices,
immediate mode, constants, uniforms, mirror writes, the lights) is copied
into the ring after its record ("inline": its address is in the ring);
what is too large for the ring (more than an eighth of it) is copied into
a buffer of its own in guest memory, which the guest frees once the host
has released the record. Textures and palettes are read where they are, as
the Xbox's GPU reads them; their changes are the write tracking's
(memory_watch.c). Refer to "The renderer's command stream" in
port/linux/README.md.
*/

#ifndef __HALO_XGPU_STREAM_H
#define __HALO_XGPU_STREAM_H

#include <stdint.h>

#define XGPU_STREAM_MAGIC 0x53504758u /* "XGPS" */
/* 24: INITIALIZE's answer (xgpu_stream_capabilities) 128 bytes, its
temporal_metalfx before device: a guest and a host of 23 and 24 would read
each other's device and temporal_metalfx out of place. 25: each frame's
own GPU time (xgpu_stream_gpu_frames) after the pipelines' states, which a
host of 24 would not write and a guest of 24 has no room for. 26:
CONTACT_SHADOWS (display.contact_shadows), which a host of 25 would refuse
as a record it does not know. 27: GPUS (display.gpu's devices, Settings'
GRAPHICS CARD), which a host of 26 would refuse so. 28: LT_LIGHTS (the
light-transport pass's light list), which a host of 27 would refuse so */
#define XGPU_STREAM_VERSION 28
#define XGPU_STREAM_LOADING_ART_BYTES (640u * 480u * 4u)
/* and the picture at its own size after it, where there is one (loading_art.h's LOADING_ART_SOURCE_BYTES) */
#define XGPU_STREAM_LOADING_ART_SOURCE_BYTES (1600u * 1200u * 4u)

/* ---------- the ring

The guest's: a header and the ring's bytes, both in guest memory, the bytes
aligned to XGPU_STREAM_RING_ALIGNMENT and a power of two in size (so that a
Metal backend can wrap them in one buffer without copying:
newBufferWithBytesNoCopy). Positions are running counts of bytes written
since the stream began, modulo 2^32: byte position p is at buffer + (p &
(size - 1)). The guest writes records from committed on, at most to
released + size; it publishes them with host_render_stream_submit, which
also counts the frames; the host releases bytes once the GPU is done with
the work that read them (the backend's fence). */

#define XGPU_STREAM_RING_ALIGNMENT 0x4000u
/* records and inline data: records start at 4 bytes, inline data at 16
(vertex data's alignment in every API) */
#define XGPU_STREAM_RECORD_ALIGNMENT 4u
#define XGPU_STREAM_DATA_ALIGNMENT 16u

struct xgpu_stream_ring
{
	/* XGPU_STREAM_MAGIC, XGPU_STREAM_VERSION, sizeof(struct xgpu_stream_ring) */
	uint32_t magic, version, header_size;
	/* the ring's bytes: guest address and size (a power of two) */
	uint32_t buffer, size;
	/* the most frames submitted and not yet retired (the GPU done with
	them): 2, the frame the host draws and the one the game writes */
	uint32_t frames_in_flight;
	/* the host's answers, in guest memory the guest allocates:
	visibility: XGPU_STREAM_VISIBILITY_SLOTS counts (uint32_t), the latest
	known for each slot, each at most 0x7fffffff (the game reads them as
	longs and halts on a negative one: XGPU_VISIBILITY_COUNT_MOST), then
	XGPU_STREAM_VISIBILITY_SLOTS test numbers (uint32_t), each the number
	of the test whose count the slot's is (the visibility end record's
	test; 0: none yet), written after its count with a release store, so
	that a guest reading the number first (an acquire load) reads a count
	of that test or a later one; pipelines: XGPU_STREAM_PIPELINES states (uint8_t,
	XGPU_STREAM_PIPELINE_*) of the pipeline ids, then the frames' GPU times
	(struct xgpu_stream_gpu_frames: XGPU_STREAM_PIPELINES_AREA bytes in all) */
	uint32_t visibility, pipelines;

	/* the guest's (written before each host_render_stream_submit, read by
	the host in it) */
	uint32_t committed;
	uint32_t frames_submitted;

	/* the host's (read by the guest with acquire loads) */
	uint32_t released;
	uint32_t frames_retired;
	/* positions decoded; nonzero after a malformed record (then the host
	decodes no more: XGPU_STREAM_ERROR_*) and where it was */
	uint32_t decoded;
	uint32_t error, error_position;

	/* statistics: the guest's (bytes of the last frame, the most a frame
	has had, VERTICES records sent again for a draw in a later submit)
	and the host's (the times and nanoseconds the guest waited for room, a
	frame to retire or a synchronous record; nanoseconds the last frame
	took to play, the backend's calls included) */
	uint32_t frame_bytes, frame_bytes_most;
	uint32_t vertices_again;
	uint32_t waits, wait_nanoseconds;
	uint32_t decode_nanoseconds;
	/* the host's display (display.hdr: written by the host whenever it
	looks, read by the guest between frames): whether it shows HDR (its
	potential headroom past XGPU_HDR_DISPLAY_HEADROOM, or an HDR swapchain),
	and its headroom now (a float's bits, 1.0 for none); its SDR white in
	nits (a float's bits: the system's, or the one the encoding takes where
	it gives none) and whether the system gave it */
	uint32_t display_hdr;
	uint32_t display_headroom;
	uint32_t display_white;
	uint32_t display_white_reported;
	/* the guest's, with each frame: microseconds more than its frames take
	that the next frame will (a load zone's switch in it): the host starts
	that frame so much earlier (display_wait) */
	uint32_t frame_extra_microseconds;
	/* the host's, running counts of microseconds modulo 2^32 (read by the
	guest with acquire loads at each frame presented: a report's,
	port/linux/src/report.c): the GPU's time on the frames, at each frame's
	end (host_render_stream_gpu_time: Metal's, the time the GPU was busy;
	Vulkan's, from each frame's first work to its last), and nonzero once
	the backend measures it; the render thread's time playing records (the
	backend's calls included, but its waits for the display:
	host_render_stream_display_blocking); the guest's waits for the
	display's pacing, a part of wait_nanoseconds (display_wait's, and its
	waits for the render thread or the GPU while the display held the render
	thread). The GPU's running count's change from one frame presented to
	the next is not that frame's (its timestamps are read late): each
	frame's own is in xgpu_stream_gpu_frames */
	uint32_t gpu_microseconds;
	uint32_t gpu_measured;
	uint32_t render_microseconds;
	uint32_t paced_microseconds;
};

enum
{
	XGPU_STREAM_ERROR_NONE,
	XGPU_STREAM_ERROR_RECORD,   /* an unknown type, or a size too small for it */
	XGPU_STREAM_ERROR_BOUNDS,   /* a record past committed, or inline data outside its record */
	XGPU_STREAM_ERROR_ID,       /* a pipeline or layout id out of range or never defined */
};

/* host_render_stream_submit's flags */
enum
{
	/* the records end a frame (after XGPU_STREAM_FRAME_END): returns once
	no more than frames_in_flight frames are unretired */
	XGPU_STREAM_SUBMIT_FRAME = 1,
	/* returns once the host has played the records and the GPU has done
	their work: a synchronous record's answer is written, data it named in
	place may change again */
	XGPU_STREAM_SUBMIT_SYNC = 2,
};

#define XGPU_STREAM_VISIBILITY_SLOTS 4096
#define XGPU_STREAM_PIPELINES 16384
#define XGPU_STREAM_LAYOUTS 4096

/* the host's, after the pipelines' states: the GPU's time on each of the
last XGPU_STREAM_GPU_FRAMES frames, each its own (not a running count's
change, which puts two frames' in one frame and none in another when a
frame's timestamps are read late), in the slot of its number (frames_submitted's
count of it) modulo XGPU_STREAM_GPU_FRAMES. The host writes a slot's frame 0,
then its microseconds, then its frame (release stores); a guest reads the
frame, the microseconds and the frame again (acquire loads), and takes the
time only for the frame it asked of, read alike both times
(port/linux/src/frame_gpu.h) */
#define XGPU_STREAM_GPU_FRAMES 16
struct xgpu_stream_gpu_frames
{
	uint32_t frame[XGPU_STREAM_GPU_FRAMES];
	uint32_t microseconds[XGPU_STREAM_GPU_FRAMES];
};
#define XGPU_STREAM_PIPELINES_AREA (XGPU_STREAM_PIPELINES + sizeof(struct xgpu_stream_gpu_frames))

/* a pipeline id's state, written by the host when the backend first makes
it (the front end's lighting fallback reads it from the next frame) */
enum
{
	XGPU_STREAM_PIPELINE_UNKNOWN,
	XGPU_STREAM_PIPELINE_MADE,
	XGPU_STREAM_PIPELINE_FAILED,
};

/* ---------- records

Each starts with a word: its type in the low 8 bits, its size in bytes
(header and inline data included, a multiple of 4) in the high 24. A
record's inline data follows its fixed part, at the next 16-byte aligned
ring position, and lies inside the record. A record never wraps: one that
would not fit before the ring's end is preceded by XGPU_STREAM_SKIP of size
0, which skips to the end (any other SKIP skips its size).

Records named "synchronous" are submitted with XGPU_STREAM_SUBMIT_SYNC and
carry the guest address of their answer (reply). */

#define XGPU_STREAM_HEADER(type, size) ((uint32_t)(type) | ((uint32_t)(size) << 8))
#define XGPU_STREAM_HEADER_TYPE(header) ((header) & 0xffu)
#define XGPU_STREAM_HEADER_SIZE(header) ((header) >> 8)
#define XGPU_STREAM_RECORD_MAXIMUM 0xfffffcu

enum
{
	XGPU_STREAM_SKIP,
	/* the backend's life */
	XGPU_STREAM_INITIALIZE,            /* synchronous: xgpu_stream_capabilities */
	XGPU_STREAM_INVALIDATE,
	XGPU_STREAM_FLUSH,
	XGPU_STREAM_ANTI_ALIASING_PREPARE,
	XGPU_STREAM_ANTI_ALIAS,
	/* render targets */
	XGPU_STREAM_TARGET_CREATE,
	XGPU_STREAM_TARGET_DELETE,
	XGPU_STREAM_TARGET_MULTISAMPLE,
	XGPU_STREAM_TARGET_RESOLVE,
	XGPU_STREAM_SET_TARGETS,
	XGPU_STREAM_CLEAR,
	XGPU_STREAM_CLEAR_TARGET,
	XGPU_STREAM_READ_TARGET,           /* synchronous */
	/* textures */
	XGPU_STREAM_SET_TEXTURES,
	/* pipelines */
	XGPU_STREAM_CONSTANTS,
	XGPU_STREAM_VERTEX_PROGRAM,
	XGPU_STREAM_VERTEX_LIGHTING,
	XGPU_STREAM_PIPELINE_DEFINE,
	XGPU_STREAM_PIPELINE,
	XGPU_STREAM_DRAW_STATE,
	/* vertices and draws */
	XGPU_STREAM_MIRROR_WRITE,
	XGPU_STREAM_LAYOUT_DEFINE,
	XGPU_STREAM_VERTICES,
	XGPU_STREAM_DRAW,
	XGPU_STREAM_DRAW_INDICES,
	XGPU_STREAM_DRAW_INDEXED,
	XGPU_STREAM_DRAW_IMMEDIATE,
	/* visibility tests */
	XGPU_STREAM_VISIBILITY_BEGIN,
	XGPU_STREAM_VISIBILITY_END,
	XGPU_STREAM_VISIBILITY_FRAME,
	/* frames */
	XGPU_STREAM_PRESENT,
	XGPU_STREAM_FRAME_END,
	/* the enhanced dynamic lights */
	XGPU_STREAM_DYNAMIC_LIGHTS_PREPARE,
	XGPU_STREAM_DYNAMIC_LIGHTS_UPLOAD,
	XGPU_STREAM_DYNAMIC_LIGHTS_HALOS,
	/* the VR mode */
	XGPU_STREAM_VR_SUPPORTED,          /* synchronous: uint32_t */
	XGPU_STREAM_VR_INITIALIZE,         /* synchronous: xgpu_stream_vr_info, in and out */
	XGPU_STREAM_VR_EYES,
	XGPU_STREAM_VR_PASS,
	XGPU_STREAM_VR_GROUPS_RESET,
	XGPU_STREAM_VR_TIMING,
	XGPU_STREAM_VR_COMPOSE,
	XGPU_STREAM_VR_SUBMIT,
	/* the shaders and pipelines made */
	XGPU_STREAM_PREPARED,              /* synchronous: uint32_t, the milliseconds waited */
	XGPU_STREAM_PREPARE,
	/* the VR mode's state its composition reads, and what vr.c sets in the
	backend as the frame is drawn (vr.h) */
	XGPU_STREAM_VR_STATE,
	XGPU_STREAM_VR_EYE_TRANSFORMS,
	XGPU_STREAM_VR_SCREEN_CORRECTIONS,
	XGPU_STREAM_VR_SKY,
	XGPU_STREAM_VR_EYE_FEED,
	XGPU_STREAM_VR_EYE_TINT,
	XGPU_STREAM_LOADING,
	XGPU_STREAM_LOADING_OWNED,          /* synchronous: current presenter ownership */
	XGPU_STREAM_LOADING_ART,            /* value: byte count, followed by owned 640x480 RGBA pixels, and
	                                       the picture's own 1600x1200 RGBA after them where it has them */
	XGPU_STREAM_LOADING_SCENE,          /* value: byte count, followed by the menu's scene (loading_scene.h) */
	XGPU_STREAM_LOADING_PROGRESS,       /* value: the load's progress, a float's bits (0 to 1) */
	XGPU_STREAM_LOADING_MODE,           /* value: 1 the sweep follows the progress (the Xbox's), 0 it loops */
	XGPU_STREAM_LOADING_CAPTION,        /* value: byte count, followed by a width and a height (uint32_t)
	                                       and that many bytes of a line's coverage; 0: none */
	/* display.hdr (xgpu_hdr.h) */
	XGPU_STREAM_HDR,
	XGPU_STREAM_TONE_MAP,
	/* the screenshot key's picture (controls.screenshot) */
	XGPU_STREAM_SCREENSHOT,            /* synchronous */
	/* display.picture_position: where a picture of another shape than the
	window sits in it (value: 0 centered, n + 1 n pixels from the top, no
	lower than the bottom) */
	XGPU_STREAM_PICTURE_POSITION,
	/* display.gamma: an SDR frame's curve as it is presented (value: the
	setting, a float's bits; 1 none) */
	XGPU_STREAM_GAMMA,
	/* display.panini (xgpu_backend_panini) */
	XGPU_STREAM_PANINI,
	/* display.ambient_occlusion "world" (xgpu_backend_world_ao_window,
	_field; docs/world-ao.md) */
	XGPU_STREAM_WORLD_AO_WINDOW,
	XGPU_STREAM_WORLD_AO_FIELD,
	/* the pipeline bound's stand-in (xgpu_backend_pipeline_stand_in) */
	XGPU_STREAM_PIPELINE_STAND_IN,
	/* the game's renderer's category of the draws from now on, for the
	backends' timing alone (xgpu_backend_category; value: XGPU_STREAM_CATEGORY_*) */
	XGPU_STREAM_CATEGORY,
	/* the enhanced dynamic lights' world (xgpu_backend_dynamic_lights_world) */
	XGPU_STREAM_DYNAMIC_LIGHTS_WORLD,
	/* display.ambient_occlusion "world" found once a pixel
	(xgpu_backend_world_ao_prepass) */
	XGPU_STREAM_WORLD_AO_PREPASS,
	/* display.ambient_occlusion "world"'s distance-field scene: a box of
	its cascades (xgpu_backend_world_ao_cascades) */
	XGPU_STREAM_WORLD_AO_CASCADES,
	/* display.ambient_occlusion "ray_traced": a mesh of the scene the GPU's
	ray queries trace, and the frame's instances of the meshes
	(xgpu_backend_world_ao_mesh, _instances) */
	XGPU_STREAM_WORLD_AO_MESH,
	XGPU_STREAM_WORLD_AO_INSTANCES,
	/* debug.render_marks: what the draws from now on belong to, for the
	inspector's replay (port/macos/tools/halo_replay.m); the backends play
	nothing for them (xgpu_stream_mark_*) */
	XGPU_STREAM_MARK_STRING,
	XGPU_STREAM_MARK_VIEW,
	XGPU_STREAM_MARK_OWNER,
	/* the enhanced dynamic lights' cells that changed
	(xgpu_backend_dynamic_lights_upload; DYNAMIC_LIGHTS_UPLOAD's records,
	of the whole, are the stream's version 22's) */
	XGPU_STREAM_DYNAMIC_LIGHTS_CHANGE,
	/* display.render_scale: a frame smaller than the window scaled up
	sharply (value 1: the render scale's) or smoothly (0: "original"
	resolution scaling's 640x480, as it always was) */
	XGPU_STREAM_SHARP_UPSCALE,
	/* display.hdr_sdr_curve: the curve the HDR encoding takes the frame's
	values to light by (value: xgpu_hdr.h's XGPU_HDR_SDR_CURVE_*, AUTO the
	backend's platform's) */
	XGPU_STREAM_HDR_SDR_CURVE,
	/* display.hdr_peak_nits (#1122): the display's peak in nits, of the
	player's (value; 0 the system's) */
	XGPU_STREAM_HDR_PEAK_NITS,
	/* display.hdr_gamut (#1124): the highlights' colours expanded past
	sRGB's (value 1; 0 sRGB's) */
	XGPU_STREAM_HDR_GAMUT,
	/* debug.timings: each pass timed on the GPU (value: every so many
	frames, 0 none), as the setting changes (xgpu_backend_pass_times) */
	XGPU_STREAM_PASS_TIMES,
	/* display.temporal_aa: a window's motion (xgpu_backend_motion) */
	XGPU_STREAM_MOTION,
	/* display.shadows "ray_traced": a window's casters' shadows
	(xgpu_backend_rt_shadows) */
	XGPU_STREAM_RT_SHADOWS,
	/* display.contact_shadows: a window's casters' contact shadows, from
	the depth (xgpu_backend_contact_shadows; RT_SHADOWS' record: of no
	casters, in play, the light taken before the classic receivers) */
	XGPU_STREAM_CONTACT_SHADOWS,
	/* the devices the backend can draw with (synchronous:
	xgpu_stream_gpus; xgpu_backend_gpus) */
	XGPU_STREAM_GPUS,
	/* display.rt_bounce: a window's lights that bounce
	(xgpu_backend_rt_bounce) */
	XGPU_STREAM_RT_BOUNCE,
	/* the light-transport pass's light list: every dynamic light as drawn
	and the suns (port/linux/game/lt_lights.c; xgpu_backend_lt_lights) */
	XGPU_STREAM_LT_LIGHTS,
	/* the light list's inputs, before the frame's LT_LIGHTS: what
	port/linux/game/lt_lights_build.c reads of the game (lt_lights_inputs.h),
	sent while the frames are recorded, for the replay's light-list unit
	(xgpu_backend_lt_inputs); the backend takes no notice of it */
	XGPU_STREAM_LT_INPUTS,
	XGPU_STREAM_RECORD_TYPES,
};

/* a record of no more than its header: SKIP, INVALIDATE, FLUSH,
VISIBILITY_BEGIN, VISIBILITY_FRAME, DYNAMIC_LIGHTS_PREPARE,
VR_GROUPS_RESET, PREPARE */
struct xgpu_stream_record
{
	uint32_t header;
};

/* FRAME_END: when the game read the frame's input, nanoseconds of its
monotonic clock (0 unknown; a recording's older FRAME_END is its header
alone), for the time from it to the frame on screen (frame_pacing.h) */
struct xgpu_stream_frame_end
{
	uint32_t header;
	uint32_t input_low, input_high;
};

/* a record of one value: ANTI_ALIASING_PREPARE (smaa), TARGET_DELETE,
TARGET_RESOLVE, PRESENT (the back buffer's target), PICTURE_POSITION, GAMMA, SHARP_UPSCALE, HDR_SDR_CURVE, HDR_PEAK_NITS, HDR_GAMUT, PASS_TIMES, VR_PASS, VR_TIMING
(mark), PIPELINE (the id, XGPU_STREAM_PIPELINE_DEFINE's), VR_SKY (sky) */
struct xgpu_stream_value
{
	uint32_t header;
	uint32_t value;
};

/* CATEGORY's value: the category (rasterizer.h's _rasterizer_profile_*,
XGPU_STREAM_CATEGORY_NONE outside them) */
#define XGPU_STREAM_CATEGORY_NONE 255u

/* ---------- debug.render_marks (port/linux/game/render_marks.c)

Written only with the setting on. A string a mark names is a number: the
first mark that names it is preceded by MARK_STRING, its number and its
bytes (inline, NUL-terminated); 0 names none. Numbers count from 1 since
the stream began and are never given again. A VIEW mark says where the
camera of the window drawn from now on is; an OWNER mark says what the
draws from now on, until the next OWNER mark, belong to. */

/* an owner's kind (the replay's names: halo_replay.m's mark_kind_names) */
enum
{
	XGPU_STREAM_MARK_OTHER,
	XGPU_STREAM_MARK_WORLD_OBJECT,
	XGPU_STREAM_MARK_FIRST_PERSON,
	XGPU_STREAM_MARK_BSP,
	XGPU_STREAM_MARK_SKY,
	XGPU_STREAM_MARK_DECAL,
	XGPU_STREAM_MARK_EFFECT,
	XGPU_STREAM_MARK_PARTICLE,
	XGPU_STREAM_MARK_CONTRAIL,
	XGPU_STREAM_MARK_LIGHTNING,
	XGPU_STREAM_MARK_WEATHER,
	XGPU_STREAM_MARK_WATER,
	XGPU_STREAM_MARK_HUD,
	XGPU_STREAM_MARK_TEXT,
	XGPU_STREAM_MARK_MENU_WIDGET,
	XGPU_STREAM_MARK_CINEMATIC_LETTERBOX,
	XGPU_STREAM_MARK_DEBUG,
	XGPU_STREAM_MARK_DETAIL_OBJECT,
	XGPU_STREAM_MARK_LENS_FLARE,
	XGPU_STREAM_MARK_FOG,
	XGPU_STREAM_MARK_SCREEN_EFFECT,
	XGPU_STREAM_MARK_LIGHT_VOLUME,
	XGPU_STREAM_MARK_SHADOW,
	XGPU_STREAM_MARK_CLEAR,
	XGPU_STREAM_MARK_KINDS,
};

#define XGPU_STREAM_MARK_NONE 0xffffffffu

/* MARK_STRING: then length bytes and a NUL (inline: at data) */
struct xgpu_stream_mark_string
{
	uint32_t header;
	uint32_t number, length, data;
};

/* MARK_VIEW: the map, the game's tick, the window (XGPU_STREAM_MARK_NONE:
the frame's start, no camera) and its camera: position, forward and up
(the world's units), the horizontal field of view (radians) and the
viewport in the back buffer's pixels (x, y, width, height) */
struct xgpu_stream_mark_view
{
	uint32_t header;
	uint32_t map, tick, window;
	float position[3], forward[3], up[3];
	float field_of_view;
	int32_t viewport[4];
};

/* MARK_OWNER: the kind (XGPU_STREAM_MARK_*), an id (an object's or a
widget's handle, a tag's index; XGPU_STREAM_MARK_NONE), and strings: a
label, the tags that matter (the object's definition, its model, the
shader, a bitmap), a menu widget's tag and the generator source that wrote
it (tools/menu_sources.py's) */
struct xgpu_stream_mark_owner
{
	uint32_t header;
	uint32_t kind, id;
	uint32_t label;
	uint32_t object, model, shader, bitmap;
	uint32_t widget, source;
};

/* ---------- the backend's life */

/* INITIALIZE's answer: xgpu_backend_capabilities, and the backend's own
(the enhanced dynamic lights can be made: xgpu_backend_dynamic_lights_prepare) */
struct xgpu_stream_capabilities
{
	int32_t largest_target;
	int32_t most_samples;
	int32_t most_anisotropy;
	uint8_t sample_counting, bgra_vertex_colors, base_vertex, sample_mask;
	uint8_t mixed_constant_blend, dynamic_lights, dynamic_lights_preparable, hdr;
	/* display.ambient_occlusion "ray_traced": the backend traces the
	occlusion's rays by the GPU's ray queries (xgpu_backend_world_ao_mesh,
	_instances: Vulkan's VK_KHR_ray_query) */
	/* (and xgpu_backend_capabilities' clear_alone and draw_bounds, and in
	cutout_prepass its alpha_test_drop, 1, and cutout_prepass, 2) */
	uint8_t ray_queries, cutout_prepass, clear_alone, draw_bounds;
	/* display.output_depth: the SDR back buffer can be RGBA16
	(xgpu_backend.h's XGPU_FORMAT_RGBA16) */
	uint8_t deep_sdr;
	/* display.soft_particles: an effect drawn with no views can be soft
	(the pixel key's vr_effect: xgpu_backend_capabilities' soft_effects) */
	uint8_t soft_effects;
	/* display.temporal_aa "fsr": the backend upscales a window's view with
	AMD FSR (MOTION's _UPSCALE: xgpu_backend_capabilities' temporal_upscale) */
	uint8_t temporal_upscale;
	/* display.temporal_aa "dlss": the backend upscales a window's view with
	NVIDIA DLSS (MOTION's _UPSCALE, technique 2: xgpu_backend_capabilities'
	temporal_dlss) */
	uint8_t temporal_dlss;
	/* display.temporal_aa "metalfx": ... with MetalFX's temporal scaler
	(technique 3: xgpu_backend_capabilities' temporal_metalfx); 2 with its
	spatial one alone (iOS, #1228) */
	uint8_t temporal_metalfx;
	/* the GPU an NVIDIA one where the DLSS add-on could run
	(xgpu_backend_capabilities' dlss_gpu; a pad byte before, 0 from an
	older host: the size and the version are as they were) */
	uint8_t dlss_gpu;
	/* the ray-traced scene's render geometry's budget for this GPU, in 8 MB
	(xgpu_backend_capabilities' rt_budget; a pad byte before, 0 from an
	older host: the game's default then) */
	uint8_t rt_budget, pad;
	/* the GPU, its API and driver, in a few words (a report's:
	port/linux/src/report.c), NUL terminated */
	char device[96];
};

/* GPUS' answer: the usable devices the backend found as it started, in
the order it lists them (a backend that chooses none: count 0), the one it
draws with (-1 none), and for each display.gpu's value that chooses it
(xgpu_vulkan_gpu.h's xgpu_gpu_setting) and its name, NUL terminated */
#define XGPU_STREAM_GPUS_MOST 8
#define XGPU_STREAM_GPU_NAME 64
struct xgpu_stream_gpus
{
	int32_t count, chosen;
	char values[XGPU_STREAM_GPUS_MOST][XGPU_STREAM_GPU_NAME];
	char names[XGPU_STREAM_GPUS_MOST][XGPU_STREAM_GPU_NAME];
};

struct xgpu_stream_reply_record
{
	uint32_t header;
	/* where the answer goes (guest address) */
	uint32_t reply;
};

struct xgpu_stream_anti_alias
{
	uint32_t header;
	uint32_t target;
	uint32_t smaa;
	int32_t corners[4];
};

/* PANINI: a window's 3D view drawn again through the Panini projection
(xgpu_backend_panini): its target, corners, and the projection (d, s, the
magnification at the middle, 0, and the rectilinear view's tangents at the
corners' left, right, top and bottom) */
struct xgpu_stream_panini
{
	uint32_t header;
	uint32_t target;
	int32_t corners[4];
	float projection[8];
};

/* HDR: display.hdr's state (xgpu_backend_hdr): drawn in HDR, the mode
(XGPU_HDR_MODE_*), paper white (in nits; 0: the system's SDR white, in
percent of it: xgpu_hdr_paper_white_nits), the UI's white (in nits; 0:
paper white's) */
struct xgpu_stream_hdr
{
	uint32_t header;
	uint32_t active, mode, paper_white_nits, paper_white, ui_nits;
};

/* TONE_MAP: a window's 3D view tone mapped (xgpu_backend_tone_map) */
struct xgpu_stream_tone_map
{
	uint32_t header;
	uint32_t target;
	int32_t corners[4];
};

/* ---------- display.ambient_occlusion "world" (docs/world-ao.md, #578)

As the GPU sees it, alike in the guest's front end and the hosts' backends.

- The field: one 3D texture of the current structure BSP's signed distance
  (world_sdf.h), R8 signed normalized (VK_FORMAT_R8_SNORM,
  MTLPixelFormatR8Snorm), linear filtering, clamped to its edge; a texel's
  value times the block's range is the distance in world units at the
  voxel's centre.
- The block: the field's place and the window's proxies (world_proxies.h's
  struct world_proxy), a uniform buffer (std140: vec4s only).

Bindings (the shaders: workstream D; the backends' descriptors: B):
- SPIR-V: XGPU_SPIRV_SET_OCCLUSION (a set of its own, so that a draw
  still binds at most four sets), bindings XGPU_SPIRV_BINDING_WORLD_SDF and
  XGPU_SPIRV_BINDING_WORLD_AO (xgpu_spirv.h);
- MSL: [[texture(XGPU_MSL_TEXTURE_WORLD_SDF)]] with a constexpr sampler in
  the shader, [[buffer(XGPU_MSL_BUFFER_WORLD_AO)]] (xgpu_msl.h).
*/

#define XGPU_WORLD_AO_PROXIES_MAXIMUM 64
/* a proxy's kind (its [0].w) */
#define XGPU_WORLD_AO_PROXY_SPHERE 1
#define XGPU_WORLD_AO_PROXY_CAPSULE 2
#define XGPU_WORLD_AO_PROXY_BOX 3
/* (a vehicle's occlusion field, over this proxy and the next two: [0]
the reach's centre, kind; [1] row 0 of the world to its voxels' place,
owner; [2] the three rows' translations, its reach (world units); then [0]
row 1, 0; [1] row 2, owner; [2] its last voxel's place (its sides less
one), 0; then [0] its place in the objects' atlas plus a half texel, 0;
[1] one over the atlas's size, owner; [2] its geometry's box's half
extents (voxels, about the grid's middle), 0: the box's bound
(world_proxy_box_fade's U) bounds the field's occlusion) */
#define XGPU_WORLD_AO_PROXY_FIELD 4

/* the uniform block: [0] the field's origin (its first voxel's centre) and
one over the voxel's size; [1] its width, height, depth (in voxels) and
range (world units); [2] the strength on objects, the strength on the level,
the field's fade (0 none yet: the proxies alone), the proxies' count; [3]
the reference's weight (how much of the occlusion at the object's lighting
point its ambient sample already holds) and three spare; then the
proxies, three vec4s each (world_proxies.h's struct world_proxy); then
their grid about the camera (world_ao.h's world_ao_grid): [0] the camera,
[1] each axis's scale and one over the cell, and the cells' 64-bit masks of
the proxies that can reach them, two cells a uvec4 */
#define XGPU_WORLD_AO_GRID_CELLS_MAXIMUM (16 * 16 * 8)
struct xgpu_world_ao_block
{
	float field[2][4];
	float parameters[2][4];
	float proxies[XGPU_WORLD_AO_PROXIES_MAXIMUM][3][4];
	float grid[2][4];
	uint32_t cells[XGPU_WORLD_AO_GRID_CELLS_MAXIMUM / 2][4];
};

/* WORLD_AO_WINDOW: display.ambient_occlusion "world"'s block for the
window's opaque surfaces from here on (xgpu_backend_world_ao_window):
active 0 none (the block's contents unused) */
struct xgpu_stream_world_ao_window
{
	uint32_t header;
	uint32_t active;
	uint32_t pad[2];
	struct xgpu_world_ao_block block;
};

/* ---------- display.ambient_occlusion "world" found once a pixel

The window's opaque surfaces are drawn twice (source/render/render.c): first
their inputs to the occlusion alone (the pre-pass: each draw with the
world's light, XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED, drawn by the backend
with its twin, the same pipeline but for its pixel key's
XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS and its target: xgpu_world_ao_twin),
into a target of the inputs (XGPU_FORMAT_WORLD_AO_INPUTS: four 32-bit
words a pixel, xgpu.h's) and a depth target of its own; then the
occlusion found once from them, at each 2x2 block's nearest pixel (a
quarter of the pixels: the pass's camera[3].w 2; 1 each pixel's), into a
texture of four floats a texel (the occlusion, the pixel's distance from
the eye, its normal octahedral); then the surfaces drawn, each reading its
pixel's (XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED), upsampled by distance and
normal. Nothing else is drawn in the pre-pass: the draws without the
world's light are skipped, its clears too.

WORLD_AO_PREPASS: phase XGPU_WORLD_AO_PREPASS_BEGIN (the draws from here
on are the pre-pass's, into targets of target's size), _END (the pre-pass
done: the occlusion found over the window's corners (left, top, right,
bottom, in target's pixels) as the camera saw it (struct
xgpu_world_ao_pass)), or _NONE (the occlusion found no more: the surfaces
read 1 from here on). While a map is prepared (the others then ignored):
_TWINS (the draws from here on would be drawn in the pre-pass in play:
each one's twin made with it, xgpu_stream_world_ao_twin) and _NONE (no
more).

replay (BEGIN and END): the walk played twice. BEGIN's 1: the records
from it to the END are the window's opaque surfaces' own draws, walked once
by the game (render.c), which the decoder plays first as the pre-pass's
(each draw's twin, as above; their MOTION records not played), and, at an
END of 1, again from the state as it was at the BEGIN, as the surfaces'
(after the occlusion is found: host_render_stream.c's walk_replay). Only
their draws and the state they are drawn with are played again; what the
walk sent once (definitions, writes to memory, uploads) is had already.
An END of 0 after a BEGIN of 1: the walk was handed over in parts (the
ring's bytes of its start may be released), not played again: the game
walks the surfaces a second time instead. 0 at both: as before, the game
walks the pre-pass and then the surfaces. */
enum
{
	XGPU_WORLD_AO_PREPASS_NONE,
	XGPU_WORLD_AO_PREPASS_BEGIN,
	XGPU_WORLD_AO_PREPASS_END,
	XGPU_WORLD_AO_PREPASS_TWINS,
};
/* the camera, as the occlusion's pass finds a pixel's world position
(world_ao.h's world_ao_pass_position): [0] its position and its near
plane; [1] the direction (one unit along the view's axis) at the screen's
middle, and its far plane; [2] and [3] the direction's change a unit of
the screen's x (-1 its left edge, 1 its right) and y (-1 its top, 1 its
bottom), [2].w 0, [3].w the pixels a side of the blocks the occlusion is
found once for (1 or 2); the window's corners in its target's pixels
(left, top, right, bottom). A pixel's position: the eye (the block's
grid[0]) + its distance (from the pre-pass) along normalize([1] + x [2] +
y [3]). Then the distance-field scene's cascades, which the dynamic
objects' rays step through (world_cascades.h: each the first voxel's
centre and the voxel, 0 not ready); the window's previous frame's camera
and corners as these, where its occlusion is reprojected from; and history:
[0] 1 where the previous frame's occlusion is this window's (one window a
frame, its corners alike: the backend's previous texture of it), else 0;
[1] the frame's number (the rays' turn); [2] the share of the new frame's
occlusion blended into what is reprojected (1: none of the history); [3] 1
where the dynamic objects' occlusion is traced, 0 where their shapes are
walked instead (HALO_WORLD_AO_NO_TRACE, for measuring against), 2 where
it is traced by the GPU's ray queries ("ray_traced": WORLD_AO_MESH, below;
a backend without them, or before its scene is built, traces it as 1) */
struct xgpu_world_ao_pass
{
	float camera[4][4];
	int32_t corners[4];
	float cascades[4][4];
	float previous[4][4];
	int32_t previous_corners[4];
	float history[4];
};
struct xgpu_stream_world_ao_prepass
{
	uint32_t header;
	uint32_t phase;
	uint32_t target;
	uint32_t replay;
	struct xgpu_world_ao_pass pass;
};

/* display.temporal_aa's motion (docs/temporal-aa.md): a window's pixels'
movement since the frame before, for a temporal technique (FSR, DLSS,
MetalFX; at any render scale, the native one too).

MOTION: phase XGPU_MOTION_OBJECTS_BEGIN (the draws from here on into
target, the window's objects, draw their motion too: the second colour
attachment, xgpu_spirv_options' motion, those that write depth and blend
nothing writing it, the others drawn through it; the constants' registers
192 to 383 are the previous frame's, CONSTANTS's past the 192: the backend
keeps a copy of the 192 there, which a record of the first 192 writes too),
_OBJECTS_END (no more), _RESOLVE (the window whole: its motion found at
each pixel of its corners, from its objects' where their depth is still
the pixel's, else from the camera's, struct xgpu_motion_pass). The
transparent draws (particles, decals, glass, water) draw none: their pixels
have what is behind them. _OPAQUE (the window's colour before its
transparent draws, over the pass's corners, kept for the technique's
reactive mask: where they changed it; the pass's motion[0] the window, 0 to
3), _UPSCALE (the technique: the window's view, resolved, into pad, the
window's target at the display's scale: the pass is a struct
xgpu_temporal_pass, its bytes the same). */
enum
{
	XGPU_MOTION_NONE,
	XGPU_MOTION_OBJECTS_BEGIN,
	XGPU_MOTION_OBJECTS_END,
	XGPU_MOTION_RESOLVE,
	XGPU_MOTION_OPAQUE,
	XGPU_MOTION_UPSCALE,
};
/* the previous frame's registers' first, in CONSTANTS */
#define XGPU_STREAM_PREVIOUS_CONSTANTS 192u
/* the window's corners in its target's pixels (left, top, right, bottom);
the rows of the matrix from a pixel's clip position (unjittered, the game's
projection: x, y -1 to 1 up, depth 0 near to 1 far, w 1) to where it was
the frame before, the camera's movement alone; motion: [0] and [1] the
drawn pixels' offset from their clip positions (the jitter and the screen's
half pixel, in the same units), [2] 1, [3] 1 where the window has its frame
before (0: a cut, a new map, a resize: no motion, and the technique's
history reset); jitter: the frame's jitter in the target's pixels (x right,
y down), and [2] 1 where debug.motion_vectors draws the motion over the
view */
struct xgpu_motion_pass
{
	int32_t corners[4];
	float reproject[4][4];
	float motion[4];
	float jitter[4];
};
/* the window's corners in the source's pixels (the target its view was
drawn into) and in the destination's (the target it is upscaled into: the
HUD's, at the display's scale); the window (0 to 3), the technique (1
"fsr", 2 "dlss"), whether the colour is display.hdr's linear light (else an SDR
picture), whether its history begins again (a cut, a new map, a resize);
the depth target's number (the view's); the camera's near and far planes
(world units), vertical field of view (radians) and meters in a world unit;
display.temporal_aa_sharpness (0 to 1, 0 none: FSR's RCAS); the frame's
jitter in the source's pixels (x right, y down) */
struct xgpu_temporal_pass
{
	int32_t corners[4];
	int32_t destination[4];
	uint32_t window, technique, hdr, reset;
	uint32_t depth, unused[3];
	float camera[4];
	float sharpness, pad[3];
	float jitter[4];
};
struct xgpu_stream_motion
{
	uint32_t header;
	uint32_t phase;
	uint32_t target;
	uint32_t pad;
	struct xgpu_motion_pass pass;
};

/* PIPELINE_STAND_IN: pipeline id (one with the world's light) has a stand-in
(an id, 0 none), the pipeline the draw would have without the world: the
backend may draw with it until id's is made, and never makes id's while a
map is prepared (xgpu_backend_pipeline_stand_in) */
struct xgpu_stream_pipeline_stand_in
{
	uint32_t header;
	uint32_t id, stand_in;
};

/* WORLD_AO_FIELD: slices z_first to z_first + z_count - 1 of a part of
the field (a width * height * depth volume, x fastest:
xgpu_backend_world_ao_field): its table of cells (part 0, four bytes a
texel: an atlas brick's x, y, z and the cell's kind, read as RGBA8) or its
atlas of bricks (part 1, signed bytes: R8 snorm), or the static
scenery's pages of the level's occlusion (part 2, bytes, R8 unorm: a 2D
array, depth its layers, sent before the table), or the vehicles'
occlusion fields' atlas (part 3, four half floats a texel: RGBA16F, a 3D
volume, sent before the table), its bytes at data (size:
width * height * z_count * the texel's); width 0 releases the field, every
part */
#define XGPU_STREAM_WORLD_AO_TABLE 0
#define XGPU_STREAM_WORLD_AO_ATLAS 1
#define XGPU_STREAM_WORLD_AO_SCENERY 2
#define XGPU_STREAM_WORLD_AO_OBJECTS 3
#define XGPU_STREAM_WORLD_AO_TEXEL(part) ((part) == XGPU_STREAM_WORLD_AO_TABLE ? 4u : \
	(part) == XGPU_STREAM_WORLD_AO_OBJECTS ? 8u : 1u)
struct xgpu_stream_world_ao_field
{
	uint32_t header;
	uint32_t part;
	uint32_t width, height, depth;
	uint32_t z_first, z_count;
	uint32_t data, size;
	uint32_t pad;
};

/* WORLD_AO_CASCADES: a box of the distance-field scene's cascades'
texture (xgpu.h's XGPU_WORLD_CASCADE*: XGPU_WORLD_CASCADE_SPAN texels
square, XGPU_WORLD_CASCADE_SPAN * XGPU_WORLD_CASCADES deep, two signed bytes
a texel, RG8 snorm; xgpu_backend_world_ao_cascades): its first texel x, y,
z and its size, its bytes at data (size: width * height * depth * 2, x
fastest); width 0 releases the texture */
#define XGPU_STREAM_WORLD_AO_CASCADES_SIDE 65u
#define XGPU_STREAM_WORLD_AO_CASCADES_DEPTH (65u * 4u)
struct xgpu_stream_world_ao_cascades
{
	uint32_t header;
	uint32_t x, y, z;
	uint32_t width, height, depth;
	uint32_t data, size;
	uint32_t pad;
};

/* ---------- render targets */

struct xgpu_stream_target_create
{
	uint32_t header;
	uint32_t target;
	uint32_t width, height;
	/* (format: XGPU_FORMAT_* of a colour target: BGRA8, RGBA16F the back
	buffer with display.hdr, or RGBA16 with display.output_depth) */
	uint8_t depth, layers, format, pad;
};

struct xgpu_stream_target_multisample
{
	uint32_t header;
	uint32_t target;
	int32_t samples;
};

struct xgpu_stream_set_targets
{
	uint32_t header;
	uint32_t color, depth;
	/* vr.h's HALO_VR_HUD_GROUP_* or -1; the layers drawn at once (1, 2, 4,
	or -1, -2 one layer alone) from base; their multisampled attachments' samples */
	int8_t hud_group, views;
	uint8_t base, samples;
};

/* the rectangles (left, top, width, height) follow, count of them */
struct xgpu_stream_clear
{
	uint32_t header;
	uint32_t flags;
	float color[4];
	float depth;
	uint32_t stencil;
	uint32_t count;
	int32_t rectangles[][4];
};

struct xgpu_stream_clear_target
{
	uint32_t header;
	uint32_t target;
	float color[4];
};

/* synchronous: the host writes the target's (or its layer's, -1 none)
pixels as BGRA rows from its first into pixels (guest memory of
width * height * 4 bytes, as the target was made), then the reply.
display.hdr's RGBA16F back buffer is clamped to [0, 1], as an SDR display
shows it, or with tone_map, tone mapped into it (xgpu_hdr_sdr_pixel). With
tone_map XGPU_STREAM_READ_HALVES, an RGBA16F target's half floats as they
are instead (RGBA, 8 bytes a pixel, pixels width * height * 8 bytes: the
debug capture of debug.screenshot_halves); any other target fails */
#define XGPU_STREAM_READ_HALVES 2u
struct xgpu_stream_read_target
{
	uint32_t header;
	uint32_t target;
	int32_t layer;
	uint32_t tone_map;
	uint32_t pixels, size;
	/* (guest address of a uint32_t: 1 written, 0 failed) */
	uint32_t reply;
};

/* synchronous: the screenshot key's picture. The host reads the target as
READ_TARGET does, tone mapped, into pixels (guest memory of width * height
* 4 bytes), and writes it as a PNG file in the folder the record ends with
(a UTF-8 path and its NUL, made if it is missing), named for the local date
and time; then the file's name (without its folder, NUL-terminated) into
name (XGPU_STREAM_SCREENSHOT_NAME bytes of guest memory), and the reply (a
uint32_t: 1 written, 0 failed) */
#define XGPU_STREAM_SCREENSHOT_NAME 64u
struct xgpu_stream_screenshot
{
	uint32_t header;
	uint32_t target;
	uint32_t width, height;
	uint32_t pixels, size;
	uint32_t name;
	uint32_t reply;
	char directory[];
};

/* ---------- textures */

/* xgpu_backend.h's struct xgpu_sampler, which is these fields */
struct xgpu_stream_sampler
{
	uint8_t minify, mip, magnify;
	uint8_t address[3];
	uint8_t pad[2];
	float lod_bias;
	uint32_t first_level;
	uint32_t anisotropy;
	uint32_t border_color;
};

#define XGPU_STREAM_MIP_LEVELS 16

/* a stage's xgpu_texture_source; resource_format and resource_size are
the texture header's Format and Size words as they were when the call was
made (the header is at resource). For an Xbox texture (XGPU_SOURCE_TEXTURE)
target is the channels its texels are sampled from: 0 as they are, else
0x100 and two bits for each of red, green, blue and alpha (red's lowest),
the channel of the texels it is taken from (a Custom Edition map's model
masks and meters: xbox_textures.c); of one sampled as it is, the copy
drawn whose levels keep an alpha test's coverage in XGPU_TEXTURE_COVERAGE_MASK
(xgpu.h: the least alpha the test passes, 0 the texture as stored).

A stage may instead draw an image (XGPU_STREAM_SOURCE_IMAGE: a high-res
replacement for the Xbox texture, xgpu_images.h): resource is its number,
resource_format its pixels' kind (XGPU_IMAGE_*: RGBA, 4 bytes each),
resource_size a serial that
changes with its pixels (no two images have the same), data its first
level's pixels, width by height, rows from the first; the host makes its
levels (levels of them) from the first. A stage's source on the wire:
none, an Xbox texture, a render target (xgpu_backend.h's XGPU_SOURCE_*, the
same values: xgpu_stream_encode.c checks them) or an image */
#define XGPU_STREAM_SOURCE_NONE 0
#define XGPU_STREAM_SOURCE_TEXTURE 1
#define XGPU_STREAM_SOURCE_TARGET 2
#define XGPU_STREAM_SOURCE_IMAGE 3

struct xgpu_stream_texture_stage
{
	uint8_t source, sampler_type, sampled;
	/* the level_targets and level_written that follow it in the record
	(rendered_levels of them, at most XGPU_STREAM_MIP_LEVELS) */
	uint8_t level_count;
	uint32_t resource, palette;
	uint32_t resource_format, resource_size;
	uint32_t target, drawn;
	uint32_t data, width, height, levels, rendered_levels;
	struct xgpu_stream_sampler sampler;
};

struct xgpu_stream_texture_level
{
	uint32_t target, written;
};

/* the stages whose bits are set in changed follow, in order, each an
xgpu_stream_texture_stage and its level_count levels; the others are as
they were. Past the four stages, a model's material maps
(XGPU_STREAM_MATERIAL_STAGE: its normal map, then its occlusion, roughness
and metalness map; xgpu.h's XGPU_TEXTURE_MODES_MATERIAL), images
(XGPU_STREAM_SOURCE_IMAGE) or none, which only the draws whose pixel key
asks for them sample: a backend without them leaves them out */
#define XGPU_STREAM_MATERIAL_STAGE 4
#define XGPU_STREAM_TEXTURE_STAGES 6
struct xgpu_stream_set_textures
{
	uint32_t header;
	uint32_t changed;
};

/* ---------- pipelines */

/* the vertex constants first .. first + count, count vec4s following */
struct xgpu_stream_constants
{
	uint32_t header;
	uint32_t first, count;
	float values[][4];
};

/* the bytes of struct xgpu_vertex_instruction (xgpu_shader.h, all
unsigned char: alike on both sides), count of them, following */
#define XGPU_STREAM_VERTEX_INSTRUCTION_SIZE 36
#define XGPU_STREAM_VERTEX_PROGRAM_MAXIMUM 136

struct xgpu_stream_vertex_program
{
	uint32_t header;
	/* the vertex shader's number (for the logs); its microcode's hash
	(low word first), the key's vertex_program */
	uint32_t id;
	uint32_t hash[2];
	uint32_t instruction_count;
	uint32_t projects;
	uint8_t instructions[][XGPU_STREAM_VERTEX_INSTRUCTION_SIZE];
};

/* xgpu.h's struct nv2a_vertex_lighting */
struct xgpu_stream_vertex_lighting
{
	uint32_t header;
	uint32_t hash[2];
	int32_t lights;
	uint32_t normal_instruction, normal_register;
	uint32_t position_instruction, position_register;
	uint32_t world_found, world_instruction, world_register;
};

/* xgpu.h's struct nv2a_pixel_shader_key */
#define XGPU_STREAM_COMBINER_STATES 57

struct xgpu_stream_pixel_key
{
	uint32_t combiner_state[XGPU_STREAM_COMBINER_STATES];
	uint32_t texture_modes;
	uint8_t sampler_type[4];
	uint8_t alpha_kill[4];
	uint8_t color_sign[4];
	uint32_t alpha_test_function;
	uint8_t fog_enable, fog_table_mode, count_samples, coverage_alpha;
	uint8_t point_threshold, per_pixel_lighting, alpha_test_samples, multiview;
	uint8_t source_blend, dynamic_lights, vr_effect, ambient_occlusion;
	/* display.hdr's tag (xgpu_hdr.h) */
	uint16_t hdr;
};

/* xgpu_backend.h's struct xgpu_pipeline_key */
struct xgpu_stream_pipeline_key
{
	uint32_t vertex_program[2];
	uint32_t packed_attributes;
	uint32_t stencil_read_mask, stencil_write_mask;
	uint8_t immediate, lighting, views;
	uint8_t blend, blend_source, blend_destination, blend_operation;
	uint8_t color_mask;
	uint8_t depth_test, depth_function, depth_write;
	uint8_t stencil_test, stencil_function, stencil_fail, stencil_depth_fail, stencil_pass;
	uint8_t cull, front_counter_clockwise, cull_front;
	uint8_t fill, depth_bias;
	uint8_t color_format, depth_format, samples;
	struct xgpu_stream_pixel_key pixel;
};

/* xgpu_shader.h's struct xgpu_pixel_program: its parts of unsigned chars
alone (alike on both sides) as their bytes, its unsigned longs as 32 bits */
#define XGPU_STREAM_TEXTURE_STAGE_SIZE 9
#define XGPU_STREAM_COMBINER_STAGE_SIZE 44
#define XGPU_STREAM_FINAL_COMBINER_SIZE 25

struct xgpu_stream_pixel_program
{
	uint8_t stages[4][XGPU_STREAM_TEXTURE_STAGE_SIZE];
	uint8_t mux_msb;
	uint8_t final[XGPU_STREAM_FINAL_COMBINER_SIZE];
	uint8_t fog, fog_mode;
	uint32_t combiner_count;
	uint8_t combiners[8][XGPU_STREAM_COMBINER_STAGE_SIZE];
	uint32_t alpha_test;
	uint8_t alpha_test_samples;
	uint8_t count_samples, coverage_alpha, point_threshold, per_pixel_lighting, dynamic_lights, multiview;
	uint8_t source_blend;
	uint8_t ambient_occlusion;
	/* display.smooth_lightmaps (xgpu_shader.h's lightmap_filter) */
	uint8_t lightmap_filter;
	/* display.hdr's tag (xgpu_hdr.h) */
	uint16_t hdr;
};

/* a pipeline key the guest numbered (from 1, below XGPU_STREAM_PIPELINES),
sent once; XGPU_STREAM_PIPELINE names it from then on. With its pixel
shader decoded (nv2a_pixel_program_decode), so that the host needs no NV2A
decoding. */
struct xgpu_stream_pipeline_define
{
	uint32_t header;
	uint32_t id;
	struct xgpu_stream_pipeline_key key;
	struct xgpu_stream_pixel_program pixel_program;
};

/* a pipeline's twin in the pre-pass, made of its definition in place
(nonzero; 0 none, the draw not drawn in the pre-pass):
- of one with the world's light read from its pixel
  (XGPU_AMBIENT_OCCLUSION_WORLD_SAMPLED), its pixel key's occlusion
  XGPU_AMBIENT_OCCLUSION_WORLD_INPUTS (its light kept): it writes its
  pixel's inputs;
- of another opaque one that writes the depth where it is nearer or as
  near (an occluder: the first-person weapon, a surface lighting itself),
  _INPUTS alone, its test made strictly nearer (so that it never takes the
  pixel of a surface it draws over at its own depth): it writes none, so
  that no surface it hides has its occlusion found;
into the inputs' target (xgpu_backend.h's XGPU_FORMAT_WORLD_AO_INPUTS:
RGBA32Uint), one sample, nothing blended, every channel written, no HDR tag,
no enhanced dynamic lights, no samples counted; its depth, stencil, raster,
vertex program and alpha test as they are. The backends, and the Mac's
library's build, make it alike */
#define XGPU_STREAM_WORLD_AO_SAMPLED 0x50u
#define XGPU_STREAM_WORLD_AO_INPUTS 0x90u
#define XGPU_STREAM_WORLD_AO_OCCLUDER 0x80u
#define XGPU_STREAM_FORMAT_WORLD_AO_INPUTS 4u
/* (xgpu_backend.h's XGPU_COMPARE_LESS and _LESS_EQUAL) */
#define XGPU_STREAM_COMPARE_LESS 1u
#define XGPU_STREAM_COMPARE_LESS_EQUAL 3u
static inline int xgpu_stream_world_ao_twin(struct xgpu_stream_pipeline_define *definition)
{
	uint8_t occlusion = definition->pixel_program.ambient_occlusion;

	if (occlusion & 0x80u)
		return 0;
	if ((occlusion & XGPU_STREAM_WORLD_AO_SAMPLED) == XGPU_STREAM_WORLD_AO_SAMPLED)
	{
		occlusion = (uint8_t)((occlusion & 0x0fu) | XGPU_STREAM_WORLD_AO_INPUTS);
	}
	else
	{
		if (occlusion || !definition->key.depth_test || !definition->key.depth_write || definition->key.blend ||
			!definition->key.color_mask || definition->key.pixel.source_blend ||
			(definition->key.depth_function != XGPU_STREAM_COMPARE_LESS &&
			definition->key.depth_function != XGPU_STREAM_COMPARE_LESS_EQUAL))
			return 0;
		occlusion = (uint8_t)XGPU_STREAM_WORLD_AO_OCCLUDER;
		definition->key.depth_function = (uint8_t)XGPU_STREAM_COMPARE_LESS;
	}
	definition->key.pixel.ambient_occlusion = definition->pixel_program.ambient_occlusion = occlusion;
	definition->key.blend = definition->key.blend_source = definition->key.blend_destination = 0;
	definition->key.blend_operation = 0;
	definition->key.color_mask = 0x0f;
	definition->key.color_format = (uint8_t)XGPU_STREAM_FORMAT_WORLD_AO_INPUTS;
	definition->key.samples = 0;
	definition->key.pixel.alpha_test_samples = definition->pixel_program.alpha_test_samples = 0;
	definition->key.pixel.hdr = definition->pixel_program.hdr = 0;
	definition->key.pixel.source_blend = definition->pixel_program.source_blend = 0;
	definition->key.pixel.count_samples = definition->pixel_program.count_samples = 0;
	definition->key.pixel.dynamic_lights = definition->pixel_program.dynamic_lights = 0;
	return 1;
}


/* xgpu_backend.h's struct xgpu_dynamic_state */
struct xgpu_stream_dynamic_state
{
	int32_t viewport[4];
	int32_t scissor[4];
	float depth_range[2];
	uint32_t stencil_reference;
	float blend_color[4];
	float depth_bias[2];
};

/* reversed depth (xgpu_backend.h, "Reversed depth"): the depth test's
function that passes on reversed depth what the key's (xgpu_backend.h's
XGPU_COMPARE_*: never, less, equal, less or equal, greater, not equal,
greater or equal, always) passes on the game's */
static __inline uint8_t xgpu_reversed_depth_function(uint8_t function)
{
	static const uint8_t reversed[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

	return function < 8 ? reversed[function] : function;
}

/* a draw's constant depth bias (its dynamic state's depth_bias[1], in the
24-bit depth buffer's units; the pipeline's depth_bias set) as the shaders
add it to reversed depth: toward the camera, positive */
static __inline float xgpu_reversed_depth_bias(int biased, float offset)
{
	return biased ? -offset / 16777215.0f : 0.0f;
}

/* ---------- the depth buffer's two projections

The depth buffer holds two projections' depths: the first-person weapon's
(its own near plane and field of view: rasterizer_xbox_models.c's
STENCIL_MODE_WRITE, which marks its pixels with the stencil's first bit)
and the world's (everything else). Both are perspective along the same
camera's forward, so each writes the game's depth (as the game has it, not
reversed) d = a + b / w of a point w ahead of the camera, with its own a
and b; a pixel's depth read back is the world's only where the stencil's
first bit is clear. Where it is set, a' + b' / w of the world's is
a + (b / b') (d' - a') of the weapon's: what a reader that unprojects with
the world's projection must read there instead (xgpu_depth_remap). */

/* a projection's (c[-96] to c[-93], then the screen conversion c[-38] and
c[-37], rows), drawn in a viewport of the draw's scale and offset
(xgpu_draw_uniforms') and depth range, line: the game's depth a + b / w
of a point w ahead (line[0] a, line[1] b). 0 where the projection has no
forward (its w row none) or no depth (b 0). */
static __inline int xgpu_depth_line(const float projection[6][4], const float scale[4], const float offset[4],
	const float depth_range[2], float line[2])
{
	/* (z = A w + B over the world: its z row the w row's A times, plus B) */
	float w_w = projection[3][0] * projection[3][0] + projection[3][1] * projection[3][1] +
		projection[3][2] * projection[3][2];
	float a, b, scale_z, near_z, far_z;

	if (!(w_w > 0.0f))
		return 0;
	a = (projection[2][0] * projection[3][0] + projection[2][1] * projection[3][1] +
		projection[2][2] * projection[3][2]) / w_w;
	b = projection[2][3] - a * projection[3][3];
	scale_z = scale[2] != 0.0f ? scale[2] : 1.0f;
	near_z = depth_range[0] >= 0.0f && depth_range[0] <= 1.0f ? depth_range[0] : 0.0f;
	far_z = depth_range[1] >= 0.0f && depth_range[1] <= 1.0f ? depth_range[1] : 1.0f;
	line[0] = near_z + (far_z - near_z) * (a * projection[4][2] + projection[5][2] - offset[2]) / scale_z;
	line[1] = (far_z - near_z) * b * projection[4][2] / scale_z;
	return line[1] != 0.0f;
}

/* a weapon's pixel's depth (the game's) as the world's projection would
have written it at the same distance: world[0] + world[1] / w, its w from
the weapon's line, remap[0] * depth + remap[1]. 0 where either has no
depth (nothing to remap: the world's own) */
static __inline int xgpu_depth_remap(const float world[2], const float first_person[2], float remap[2])
{
	if (world[1] == 0.0f || first_person[1] == 0.0f)
		return 0;
	remap[0] = world[1] / first_person[1];
	remap[1] = world[0] - remap[0] * first_person[0];
	return 1;
}

/* xgpu_backend.h's struct xgpu_draw_uniforms (floats alone: alike on both
sides) */
struct xgpu_stream_draw_uniforms
{
	float viewport_scale[4];
	float viewport_offset[4];
	float point_size;
	float ps_c0[8][4];
	float ps_c1[8][4];
	float ps_final_c0[4];
	float ps_final_c1[4];
	float fog_color[4];
	float fog_parameters[4];
	float alpha_reference;
	float bump_matrix[4][4];
	float bump_luminance[4][4];
	float texture_scale[4][4];
	float screen_offset;
	float texture_lod_bias[4];
	float world_ao[4];
};

/* xgpu_backend_draw_state: what changed since the last follows, the
dynamic state (XGPU_STREAM_DRAW_DYNAMIC), then the uniforms' pieces that
changed (XGPU_STREAM_DRAW_UNIFORMS: the uniforms as 16-byte pieces, the
last of 4, those whose bits are set in pieces, low word first, in order);
a call that changes neither sends nothing */
enum
{
	XGPU_STREAM_DRAW_DYNAMIC = 1,
	XGPU_STREAM_DRAW_UNIFORMS = 2,
};

#define XGPU_STREAM_UNIFORM_PIECES ((sizeof(struct xgpu_stream_draw_uniforms) + 15) / 16)

struct xgpu_stream_draw_state
{
	uint32_t header;
	uint32_t parts;
	uint32_t uniforms_serial;
	uint32_t pieces[2];
};

/* ---------- vertices and draws */

/* inline: data is in the ring */
struct xgpu_stream_mirror_write
{
	uint32_t header;
	uint32_t segment, offset, size;
	uint32_t unused;
	uint32_t data;
};

/* xgpu_backend.h's struct xgpu_vertex_element */
struct xgpu_stream_vertex_element
{
	uint8_t reg, stream, format, bytes;
	uint16_t offset;
};

/* a vertex layout the guest numbered (from 1, below XGPU_STREAM_LAYOUTS):
xgpu_vertex_input's declaration, elements and packed mask, sent once */
struct xgpu_stream_layout_define
{
	uint32_t header;
	uint32_t id;
	uint32_t declaration;
	uint32_t element_count;
	uint32_t packed_mask;
	struct xgpu_stream_vertex_element elements[];
};

struct xgpu_stream_vertex_stream
{
	/* the mirror's segment and the offset in it, or -1 and the vertices
	(inline) at data, bytes of them */
	int32_t segment;
	uint32_t offset;
	uint32_t data;
	uint32_t bytes;
	uint32_t stride;
};

/* xgpu_backend_set_vertices: the layout, then an xgpu_stream_vertex_stream
for each stream present (bits of streams, in order), then the values of
the inputs that changed of those no stream feeds (bits of attributes, a
vec4 each, in order); then the streamed vertices' data, inline. A draw is
in the same submit as the VERTICES record whose data it reads (the guest
sends the record again before a draw in a later submit), so the data is
the backend's until the fence after that draw. */
struct xgpu_stream_vertices
{
	uint32_t header;
	uint32_t layout;
	uint16_t streams, attributes;
};

struct xgpu_stream_draw
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
};

/* the indices (inline) at indices, count of them (16 bits each) */
struct xgpu_stream_draw_indices
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint32_t indices;
};

/* indices from the mirror's segment at offset, or (segment -1) inline at
indices, temporary if the front end made them for this draw alone (a
backend that keeps index arrays must not keep them) */
struct xgpu_stream_draw_indexed
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint32_t minimum;
	int32_t segment;
	uint32_t offset;
	uint32_t indices;
	uint32_t temporary;
};

/* immediate mode: count vertices of the registers written (bits of
written), a vec4 each in register order (inline at vertices: no room for
those not written); the values of the registers not written that changed
(bits of attributes, a vec4 each following); the indices (inline, 0
none) */
struct xgpu_stream_draw_immediate
{
	uint32_t header;
	uint32_t primitive;
	uint32_t count;
	uint16_t written, attributes;
	uint32_t vertices;
	uint32_t indices, index_count;
};

/* ---------- visibility tests */

struct xgpu_stream_visibility_end
{
	uint32_t header;
	uint32_t slot;
	float area;
	int32_t views;
	int32_t samples;
	/* the guest's number for the test (never 0; one more each test, modulo
	2^32): published with its count, so that the guest knows which test a
	slot's count is of (a count reaches the guest frames after its test) */
	uint32_t test;
};

/* ---------- the enhanced dynamic lights */

/* xgpu.h's struct xgpu_dynamic_lights_block (floats alone) */
#define XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM 128

struct xgpu_stream_dynamic_lights_block
{
	float grid[3][4];
	float position[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
	float color[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
	float cone[XGPU_STREAM_DYNAMIC_LIGHTS_MAXIMUM][4];
};

/* the block's lights past count are zero; the cells (inline) at cells,
size bytes */
struct xgpu_stream_dynamic_lights_upload
{
	uint32_t header;
	uint32_t count;
	uint32_t cells, size;
	struct xgpu_stream_dynamic_lights_block block;
};

/* the frame's lights: the block (its lights past count zero), and the
cells' bytes from offset that changed since the stream's last change (size
bytes, inline at cells; none: 0), the others as they were. The offset and
size are multiples of 16, inside XGPU_STREAM_DYNAMIC_LIGHTS_CELLS_BYTES;
the stream's first change is of all of them. */
struct xgpu_stream_dynamic_lights_change
{
	uint32_t header;
	uint32_t count;
	uint32_t cells, offset, size;
	struct xgpu_stream_dynamic_lights_block block;
};

/* the world's count texels (inline) at texels, count * 16 bytes; count
at most xgpu.h's XGPU_DYNAMIC_LIGHTS_WORLD_TEXELS */
#define XGPU_STREAM_DYNAMIC_LIGHTS_WORLD_TEXELS (240u * 1024u)
/* the cells' texture's slices of 32 x 32 texels (xgpu.h): the cells and the
clusters' masks, uploaded each frame, then the world's */
#define XGPU_STREAM_DYNAMIC_LIGHTS_CELL_SLICES 17u
/* their bytes (xgpu.h's XGPU_DYNAMIC_LIGHTS_CELLS_BYTES) */
#define XGPU_STREAM_DYNAMIC_LIGHTS_CELLS_BYTES (32u * 32u * XGPU_STREAM_DYNAMIC_LIGHTS_CELL_SLICES * 16u)
#define XGPU_STREAM_DYNAMIC_LIGHTS_WORLD_SLICES 240u
struct xgpu_stream_dynamic_lights_world
{
	uint32_t header;
	uint32_t count;
	uint32_t texels;
};

/* xgpu_backend.h's struct xgpu_dynamic_lights_halos but its halos, which
follow (count * 2 vec4s) */
struct xgpu_stream_dynamic_lights_halos
{
	uint32_t header;
	uint32_t color, depth;
	int32_t layers;
	int32_t count;
	float camera_right[3], camera_up[3];
	float projection[6][4];
	float viewport_scale[4], viewport_offset[4];
	float camera[3];
	float softness;
	float screen_offset;
	float halos[][4];
};

/* xgpu_backend.h's struct xgpu_rt_shadows but its casters, which follow
(count of them, 32 bytes each: the sphere, the direction's x and y, the
darkness's and owners' words); color and depth 0 (and count 0) while a map
is prepared: the pass's pipeline made for its targets; CONTACT_SHADOWS'
record too */
#define XGPU_STREAM_RT_SHADOWS_CASTERS 96
struct xgpu_stream_rt_shadow_caster
{
	float sphere[4];
	float direction[2];
	uint32_t darkness, owners;
};
struct xgpu_stream_rt_shadows
{
	uint32_t header;
	uint32_t color, depth;
	int32_t count;
	float projection[6][4];
	float viewport_scale[4], viewport_offset[4];
	float camera[3];
	float screen_offset;
	struct xgpu_stream_rt_shadow_caster casters[];
};

/* xgpu_backend.h's struct xgpu_rt_bounce (its lights, count of them
used, 48 bytes each) */
#define XGPU_STREAM_RT_BOUNCE_LIGHTS 8
struct xgpu_stream_rt_bounce_light
{
	float position[3];
	float radius;
	float color[3];
	float cone_offset;
	float axis[3];
	float cone_scale;
};
struct xgpu_stream_rt_bounce
{
	uint32_t header;
	int32_t count, window;
	float seconds;
	struct xgpu_stream_rt_bounce_light lights[XGPU_STREAM_RT_BOUNCE_LIGHTS];
};

/* LT_LIGHTS: a window's light list for the light-transport pass
(~/dev/rt2-realtime-design.md's contract C4; port/linux/game/lt_lights.c,
whose lt_lights.h has the game's copy of the light, the same: change both):
count lights (0 to XGPU_STREAM_LT_LIGHTS_MAXIMUM, 144 bytes each) inline at
lights (none: 0), the window (0 to 3) and the game's count of lists sent.
A list turns the pass on (display.ray_traced_lighting, where HALO_LT_E1 does
not say); window -1, with none, turns it off; window -2, with none, turns
it on before its first list (as a map is prepared, so that its shaders are
the pass's variant: lt_lights.c's lt_lights_announce) */
#define XGPU_STREAM_LT_LIGHTS_MAXIMUM 1024
enum
{
	XGPU_LT_LIGHT_POINT,
	XGPU_LT_LIGHT_SPOT,
	XGPU_LT_LIGHT_PANEL,
	XGPU_LT_LIGHT_DIRECTIONAL,
	/* a triangle of the level's static light sources, not a light
	(port/linux/game/lt_emitters.c, lt_lights.h has its members): the
	backend gathers a set's by its generation (xgpu_vulkan_lt_lights.c) */
	XGPU_LT_LIGHT_EMITTER,
	/* a triangle of an object's own lamps this frame (world_rt.c's
	world_rt_lamps), not a light: the backend takes the frame's whole
	(xgpu_vulkan_lt_lights.c's xgpu_vulkan_lt_object_lamps) */
	XGPU_LT_LIGHT_OBJECT_EMITTER,
};
/* an XGPU_LT_LIGHT_OBJECT_EMITTER record's flags: an additive part drawn
from both sides, which glows toward both */
#define XGPU_LT_LAMP_TWO_SIDED 1u
/* (an additive part: a glow drawn over its object, not shadowed by it) */
#define XGPU_LT_LAMP_ADDITIVE 2u
enum
{
	XGPU_LT_LIGHT_MODELS_ONLY = 1 << 0,
	XGPU_LT_LIGHT_HALO = 1 << 1,
	XGPU_LT_LIGHT_PROJECTILE = 1 << 2,
	XGPU_LT_LIGHT_FLASHLIGHT = 1 << 3,
	XGPU_LT_LIGHT_PANEL_FLAG = 1 << 4,
	XGPU_LT_LIGHT_GEL = 1 << 8,
	XGPU_LT_LIGHT_BAKED = 1 << 9,
	XGPU_LT_LIGHT_FIRST_PERSON = 1 << 10,
	XGPU_LT_LIGHT_LIGHTMAP_DIRECT = 1 << 11,
	XGPU_LT_LIGHT_NO_SPECULAR = 1 << 12,
	XGPU_LT_LIGHT_NOT_OWN_OBJECT = 1 << 13,
	XGPU_LT_LIGHT_UNSTABLE_ID = 1 << 14,
	/* a sky light of a level lit live (lt_lights.c's lt_lights_real_sky:
	the lightmap dropped, the sky's lights the whole light), and the sky's
	ambient dome (a directional record of the whole sphere, forward up) */
	XGPU_LT_LIGHT_REAL_SKY = 1 << 15,
	XGPU_LT_LIGHT_SKY_DOME = 1 << 16,
	XGPU_LT_LIGHT_EMITTER_SOLVED = 1 << 17,
};
enum
{
	XGPU_LT_LIGHT_SOURCE_OWN_FIRST = 1,
	XGPU_LT_LIGHT_SOURCE_GAME = 4,
	XGPU_LT_LIGHT_SOURCE_COPY = 5,
	XGPU_LT_LIGHT_SOURCE_SUN = 6,
	XGPU_LT_LIGHT_SOURCE_FRAME = 7,
	XGPU_LT_LIGHT_SOURCE_EMITTER = 8,
	XGPU_LT_LIGHT_SOURCE_FIXTURE = 9,
};
/* (each member as lt_lights.h describes it) */
struct xgpu_stream_lt_light
{
	uint32_t id[2];
	uint32_t kind, flags;
	uint32_t object, owner;
	int32_t gel_slot, gel_tag;
	float position[3], emitter_radius;
	float forward[3], range;
	float up[3], cone_inner;
	float left[3], cone_outer;
	float emitter[3], power_scale;
	float power[3], angular_diameter;
	float color[3], gel_width;
};
struct xgpu_stream_lt_lights
{
	uint32_t header;
	uint32_t count, lights;
	int32_t window;
	uint32_t frame;
};

/* LT_INPUTS: bytes of the inputs (lt_lights_inputs.h's struct lt_inputs,
its lights and lamps) at inputs, for window's list of frame */
#define XGPU_STREAM_LT_INPUTS_MOST (4u << 20)
struct xgpu_stream_lt_inputs
{
	uint32_t header;
	uint32_t bytes, inputs;
	int32_t window;
	uint32_t frame;
};

/* ---------- the VR mode

The backend composes each frame (the eyes' images, the HUD's and its
panels', the scope's) and ends it with OpenXR, on the host's render thread:
OpenXR's calls that use the device (the session made, the swapchains'
images acquired and released, the frame begun and ended) are made there
(port/linux/host/host_vr_stream.c); the game waits for the runtime's frames and
locates the views (vr.c). What the composition reads of the game's VR mode
(vr.h's and vr_visor.h's functions the guest's backend calls) comes in the
records: VR_STATE before each VR_COMPOSE, the gaze with VR_EYES, the layers
the game would submit with VR_SUBMIT; and what vr.c sets in the backend as
it draws (the eyes' transforms and corrections, the sky's depth clamping, the
feed and the tint) as records of their own, in their order. vr.h's and
vr_visor.h's structures are of 4-byte fields, alike on both sides, and are
carried as their words, but the visor image's noise textures (pointers:
guest addresses here). */

/* xgpu_backend.h's struct xgpu_vr_info (ints alone), in and out; and
vr.foveation's inset (halo_vr_foveation_inset, 0 off) and its name (for
the log), in; and out, the visibility tests counted by the pixel shaders
from now on (xgpu_backend_capabilities' sample_counting, which the VR
mode's eye pass needs) */
/* (statistics' bits: debug.gpu_stats, and with it debug.vr_gpu_time) */
#define XGPU_STREAM_VR_STATISTICS 1
#define XGPU_STREAM_VR_GPU_TIME 2
struct xgpu_stream_vr_info
{
	int32_t eye_width, eye_height;
	int32_t hud_width, hud_height;
	int32_t scope_size;
	int32_t statistics;
	int32_t foveated;
	int32_t fovea_width, fovea_height;
	float foveation_inset;
	char foveation_name[64];
	int32_t sample_counting;
};

/* vr_host.h's struct vr_host_latch, in words */
#define XGPU_STREAM_VR_LATCH_WORDS 52

/* the eye pass begins; with its insets' middles where foveated
(halo_vr_foveation: foveation 0, none this frame), and the poses the eyes
are drawn from for the late latch (halo_vr_latch's; latched 0: none) */
struct xgpu_stream_vr_eyes
{
	uint32_t header;
	uint32_t eye_target;
	uint32_t image;
	int32_t foveation;
	float centres[2][2];
	uint32_t latched;
	uint32_t latch[XGPU_STREAM_VR_LATCH_WORDS];
};

/* xgpu_backend.h's struct xgpu_vr_frame */
struct xgpu_stream_vr_frame
{
	int32_t eyes_drawn, hud_drawn, scope_drawn, hud_panels;
	uint32_t eye_target, eye_depth, scope, hud;
	int32_t eye_depth_layers;
};

/* VR_COMPOSE */
struct xgpu_stream_vr_frame_record
{
	uint32_t header;
	struct xgpu_stream_vr_frame frame;
	uint32_t screenshot;
};

/* vr_host.h's struct vr_host_layers and struct vr_host_pose, in words */
#define XGPU_STREAM_VR_LAYERS_WORDS 116
#define XGPU_STREAM_VR_POSE_WORDS 8

/* VR_SUBMIT: the frame record (screenshot: the eyes', HUD's and scope's
images written, numbered shot); the game waited for a frame of the
runtime's for it (host_vr_frame_wait: the host begins and ends one), and the
layers it would submit (halo_vr_present's, with every image taken as
acquired: the host leaves out those it did not draw) and the head they
were located with (halo_vr_submitted's) */
struct xgpu_stream_vr_submit
{
	struct xgpu_stream_vr_frame_record record;
	uint32_t waited;
	uint32_t shot;
	uint32_t layers[XGPU_STREAM_VR_LAYERS_WORDS];
	uint32_t head[XGPU_STREAM_VR_POSE_WORDS];
};

/* vr.h's and vr_visor.h's structures, in words (the visor image's up to its
noise textures, which the 64-bit host's lie 4 bytes further, aligned) */
#define XGPU_STREAM_VR_SCOPE_WINDOW_WORDS 32
#define XGPU_STREAM_VR_COCKPIT_WINDOW_WORDS 34
#define XGPU_STREAM_VR_COCKPIT_DISPLAY_WORDS 19
#define XGPU_STREAM_VR_HUD_IMAGE_WORDS 50
#define XGPU_STREAM_VR_PANEL_WORDS 20
#define XGPU_STREAM_VR_VISOR_IMAGE_WORDS 71
#define XGPU_STREAM_VR_RIM_WORDS 39

/* VR_STATE's answered: the functions whose result says whether there is
one (XGPU_STREAM_VR_STATE_*: the scope's smart-link, its window, a cockpit's
display, the HUD's panels, the helmet's rim, the eyes' depth wanted), and
whether a frame of the runtime's is being drawn (halo_vr_frame_waited: its
images may be acquired) */
enum
{
	XGPU_STREAM_VR_STATE_SMARTLINK = 1,
	XGPU_STREAM_VR_STATE_SCOPE_WINDOW = 2,
	XGPU_STREAM_VR_STATE_COCKPIT = 4,
	XGPU_STREAM_VR_STATE_PANELS = 8,
	XGPU_STREAM_VR_STATE_RIM = 16,
	XGPU_STREAM_VR_STATE_DEPTH = 32,
	XGPU_STREAM_VR_STATE_FRAME = 64,
};

/* what the frame's composition asks of vr.c and vr_visor.c (vr.h,
vr_visor.h), as they answered when the game composed it: the scope's
(halo_vr_scope_zoom, _smartlink, _window, _readout, _animation, _shape,
_gun), a
cockpit's display (halo_vr_cockpit_window), the vignette
(halo_vr_vignette), the visor's (vr_visor_hud_image, _panels, _image,
_rim) */
struct xgpu_stream_vr_state
{
	uint32_t header;
	uint32_t answered;
	float scope_zoom, scope_zoom_height;
	float scope_reticle[2];
	uint32_t scope_window[XGPU_STREAM_VR_SCOPE_WINDOW_WORDS];
	float scope_range;
	int32_t scope_rounds_loaded, scope_rounds_carried;
	float scope_animation[3];
	float scope_shape[3];
	int32_t scope_gun;
	uint32_t cockpit_window[XGPU_STREAM_VR_COCKPIT_WINDOW_WORDS];
	uint32_t cockpit_display[XGPU_STREAM_VR_COCKPIT_DISPLAY_WORDS];
	float vignette;
	float vignette_fov[2][4];
	uint32_t hud_image[XGPU_STREAM_VR_HUD_IMAGE_WORDS];
	uint32_t panels[4][XGPU_STREAM_VR_PANEL_WORDS];
	uint32_t visor_image[XGPU_STREAM_VR_VISOR_IMAGE_WORDS];
	uint32_t visor_noise[2];
	float visor_time;
	uint32_t rim[XGPU_STREAM_VR_RIM_WORDS];
};

/* halo_vr_eye_feed's (7) and halo_vr_eye_tint's (3) values */
struct xgpu_stream_vr_values
{
	uint32_t header;
	float values[];
};

/* The late latch. The host locates the head and the controllers again as
it begins the eye pass (VR_EYES), and moves what the frame draws by how far
they have moved since the game located them: the eyes' transforms
(VR_EYE_TRANSFORMS) for each projection the game sets, by the head's move
(the world), its turn alone (the sky, drawn about the eye), or the head's
and the weapon's controller's (the first-person weapon and the hands), and
the eyes' layers' poses, and the scope's with the weapon. */
enum
{
	XGPU_STREAM_VR_LATCH_NONE = 0,
	XGPU_STREAM_VR_LATCH_WORLD,
	XGPU_STREAM_VR_LATCH_SKY,
	XGPU_STREAM_VR_LATCH_HELD,
};

/* VR_EYE_TRANSFORMS: halo_vr_set_eye_transforms' (2 row-major 4x4s, the
centre's clip position to each eye's), what they move with in the late
latch (XGPU_STREAM_VR_LATCH_*), and each eye's clip transform from its own
space (row-major: OpenXR's axes, metres) for the projection
(halo_vr_eye_latch's) */
struct xgpu_stream_vr_eye_transforms
{
	uint32_t header;
	int32_t latch;
	float transforms[2][16];
	float clip_from_eye[2][16];
};

/* VR_SCREEN_CORRECTIONS: halo_vr_set_screen_corrections' (2 vec4s), and the
point they are for (halo_vr_screen_point's: its clip position in the
centre's view, w 0 none, and the viewport's width and height), from which
the late latch makes them again */
struct xgpu_stream_vr_screen_corrections
{
	uint32_t header;
	float corrections[2][4];
	float point[4];
	float viewport[2];
};

/* ---------- display.ambient_occlusion "ray_traced" (docs/world-ao.md,
"Hardware ray queries")

The scene the occlusion's pass traces its rays through with the GPU's ray
queries, where the backend has them (xgpu_stream_capabilities' ray_queries):
the dynamic objects' occlusion is then found by the hardware's rays against
their collision meshes, the structure's collision mesh stopping them, in
place of the distance-field scene's step (the pass's history[3] 2).

WORLD_AO_MESH: a mesh (a bottom-level acceleration structure, built once):
its id (1 to XGPU_STREAM_WORLD_AO_MESHES - 1) and count triangles, nine
floats each (each corner's x, y, z in the mesh's own space, the front faces
counterclockwise seen from outside), at data (size: count * 36); id 0 lets
go of every mesh (a map's end). An id is given once a map. Its kind
(XGPU_WORLD_AO_MESH_*): those triangles; (_RELEASE, no data) that id's
mesh let go of alone (a level's collision mesh once its render mesh is in
the scene: docs/rt-plan/1-geometry.md); (_INDEXED) a mesh as drawn, with
what a ray's hit reads of it (struct xgpu_world_rt_mesh, below), count its
triangles; (_TEXTURES, id 0) the textures' slots its materials name
(struct xgpu_world_rt_texture, count of them); (_SKINNED) a skinned mesh,
an indexed one's data in its model's default pose with each vertex's
skinning after it (count vertex_count of two words: its nodes, the first
in the low byte, the second in the next, each below
XGPU_WORLD_RT_POSE_NODES; its first node's weight, a float of 0 to 1),
which has no structure of its own: each object of it traces its own,
skinned by its pose and refit; or (_POSES, id 0) the frame's poses (struct
xgpu_world_rt_pose, count of them), which its instances name (their pad:
a pose's index plus one). */
#define XGPU_STREAM_WORLD_AO_MESHES 4096u
#define XGPU_STREAM_WORLD_AO_MESH_TRIANGLES (1u << 20)
#define XGPU_WORLD_AO_MESH_TRIANGLES 0u
#define XGPU_WORLD_AO_MESH_RELEASE 1u
#define XGPU_WORLD_AO_MESH_INDEXED 2u
#define XGPU_WORLD_AO_MESH_TEXTURES 3u
#define XGPU_WORLD_AO_MESH_SKINNED 4u
#define XGPU_WORLD_AO_MESH_POSES 5u

/* An indexed mesh's data (docs/rt-plan/1-geometry.md, "Per-hit data"):
this header, then its vertices' positions (vertex_count of three floats,
the mesh's own space), its triangles' corners (triangle_count of three
uint32_t, each below vertex_count, the front faces counterclockwise seen
from outside), its vertices' attributes (struct xgpu_world_rt_vertex),
each triangle's material (a uint16_t of the map's, below
XGPU_WORLD_RT_MATERIALS; padded to four bytes) and the materials it is the
first to name (material_count of struct xgpu_world_rt_material). Its first
opaque_count triangles are opaque (the structure's geometry 0, which stops
a ray at once); the rest alpha-tested (geometry 1: a ray's candidate
there is taken only where the material's alpha map passes its cutoff) */
#define XGPU_WORLD_RT_VERTICES (1u << 20)
#define XGPU_WORLD_RT_MATERIALS 4096u
#define XGPU_WORLD_RT_TEXTURES 512u
struct xgpu_world_rt_mesh
{
	uint32_t vertex_count, triangle_count, opaque_count, material_count;
	uint32_t pad[4];
};
/* a vertex's attributes (24 bytes): its texture coordinates (the base
map's, unscaled), its normal and tangent (octahedral, two snorm16: x low),
its lightmap coordinates (the level's: two snorm16, as its compressed
lightmap vertex has them; 0 a model's) and flags (XGPU_WORLD_RT_VERTEX_*) */
#define XGPU_WORLD_RT_VERTEX_BITANGENT_NEGATIVE 1u
struct xgpu_world_rt_vertex
{
	float uv[2];
	uint32_t normal, tangent, lightmap_uv, flags;
};
/* a material (64 bytes, sixteen words, read so by the shaders: the hit
table's records, xgpu_vulkan_rt.c): its index among the map's, its
shader's type (shaders.c's _shader_type_*) and flags
(XGPU_WORLD_RT_MATERIAL_*), the cutoff its alpha map's alpha must pass,
that map's texture slot and the scale of the coordinates it is sampled at;
then its maps' slots (0 none): base, detail (with its scale),
multipurpose, bump, the mods' material maps (normal; occlusion, roughness
and metalness: docs/mods.md "Material maps"), the lightmap page; and its
self-illumination colour (RGBA8, R low). Later stages (reflections, path
tracing) read the rest */
#define XGPU_WORLD_RT_MATERIAL_ALPHA_TESTED 1u
#define XGPU_WORLD_RT_MATERIAL_TWO_SIDED 2u
#define XGPU_WORLD_RT_MATERIAL_TRANSPARENT 4u
/* (the light transport's reflections, lt/indirect.glsl: how much a
surface reflects, from its shader, in the flags' high bits: its specular
mask the bump map's alpha (an environment's "bump map is specular mask"), or
the multipurpose map's (a model's), else the base map's; and its reflection's
strength head-on and at grazing view (luminance x 128, 0 to 255: the
specular brightness x its tint x the reflection's brightness, an
environment's; a model's brightness x tint)) */
#define XGPU_WORLD_RT_MATERIAL_MASK_BUMP 0x100u
#define XGPU_WORLD_RT_MATERIAL_MASK_MULTIPURPOSE 0x200u
#define XGPU_WORLD_RT_MATERIAL_REFLECTION_PERPENDICULAR_SHIFT 16
#define XGPU_WORLD_RT_MATERIAL_REFLECTION_PARALLEL_SHIFT 24
#define XGPU_WORLD_RT_MATERIAL_REFLECTION_SCALE 128.0f
/* (and its roughness, derived from its shader where it reflects, in bits
10 to 14: the code k (1 to 31) is a roughness of k x
XGPU_WORLD_RT_MATERIAL_ROUGHNESS_STEP, 0 none: world_rt_game.c's
material_roughness, ~/dev/rt2-realtime-design.md section 10) */
#define XGPU_WORLD_RT_MATERIAL_ROUGHNESS_SHIFT 10
#define XGPU_WORLD_RT_MATERIAL_ROUGHNESS_MASK 0x1fu
#define XGPU_WORLD_RT_MATERIAL_ROUGHNESS_STEP 0.02f
struct xgpu_world_rt_material
{
	uint32_t index, kind, flags;
	float alpha_cutoff;
	uint32_t alpha_map;
	float alpha_scale[2];
	uint32_t base_map, detail_map;
	float detail_scale;
	uint32_t multipurpose_map, bump_map, normal_map, orm_map, lightmap;
	uint32_t self_illumination;
};
/* a texture's slot (1 to XGPU_WORLD_RT_TEXTURES - 1) and what it is: an
Xbox texture (stage's source XGPU_SOURCE_TEXTURE, its header's guest
address) or an image (XGPU_STREAM_SOURCE_IMAGE), as a draw's stage
describes it; source XGPU_SOURCE_NONE empties the slot */
struct xgpu_world_rt_texture
{
	uint32_t slot, pad;
	struct xgpu_stream_texture_stage stage;
};
/* a skinned object's pose: its skinned mesh, its owner, its nodes, its
key (the object's: the structure the backend keeps skinned for it), each
node's
skinning transform (row-major 3x4: the model's space into the instance's,
the game's skinned vertex programs' blend of two of them) */
#define XGPU_WORLD_RT_POSES 512u
#define XGPU_WORLD_RT_POSE_NODES 64u
struct xgpu_world_rt_pose
{
	uint32_t mesh, owner, node_count, key;
	float transforms[XGPU_WORLD_RT_POSE_NODES][3][4];
};
struct xgpu_stream_world_ao_mesh
{
	uint32_t header;
	uint32_t id, count;
	uint32_t data, size;
	uint32_t kind;
	uint32_t pad[2];
};

/* WORLD_AO_INSTANCES: the frame's instances of the meshes (a top-level
acceleration structure of them, built before the next pass), count of
them at data (size: count * sizeof(struct xgpu_world_ao_instance)): each
its transform (a row-major 3x4, its mesh's space into the world), its
mesh's id, its owner (its object's datum index plus one, the instance's
custom index: a pixel of that object's own leaves it out; 0 the structure,
which stops a ray and occludes nothing: the lightmaps and the field hold
its occlusion) and its kind (XGPU_WORLD_AO_INSTANCE_*, the instance's mask:
the structure's facing either way, an object's front faces alone) */
#define XGPU_STREAM_WORLD_AO_INSTANCES_MAXIMUM 4096u
#define XGPU_WORLD_AO_INSTANCE_STRUCTURE 1u
#define XGPU_WORLD_AO_INSTANCE_OBJECT 2u
/* (with debug.rt_render_geometry) the first-person weapon and hands, drawn
in a view of their own (no ray from the world's pixels meets them: their
own mask bit, which the passes' rays leave out), and the local player's
own body in first person, which casts shadows and nothing else sees */
#define XGPU_WORLD_AO_INSTANCE_FIRST_PERSON 4u
#define XGPU_WORLD_AO_INSTANCE_PLAYER 8u
/* (with the ray-traced lighting) an object that casts no shadow (its
definition's "does not cast shadow", a device's: b30's ring overhead): an
object to every ray but the pass's sun's, which takes off the lightmap's
sun only where an object casting a shadow the lightmap lacks blocks it */
#define XGPU_WORLD_AO_INSTANCE_SHADOWLESS 16u
#define XGPU_WORLD_AO_INSTANCE_OWNERS (1u << 24)
struct xgpu_world_ao_instance
{
	float transform[3][4];
	uint32_t mesh, owner, kind, pad;
};
struct xgpu_stream_world_ao_instances
{
	uint32_t header;
	uint32_t count;
	uint32_t data, size;
};

/* ---------- the layouts' sizes, alike on both sides */

#define XGPU_STREAM_ASSERT_SIZE(type, size) _Static_assert(sizeof(type) == (size), #type "'s size")

XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_ring, 120);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_gpu_frames, 128);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_frame_end, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_capabilities, 128);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_gpus, 8 + 2 * XGPU_STREAM_GPUS_MOST * XGPU_STREAM_GPU_NAME);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_hdr, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_tone_map, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_panini, 56);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_window, 16 + 32 + 32 + XGPU_WORLD_AO_PROXIES_MAXIMUM * 48 + 32 +
	XGPU_WORLD_AO_GRID_CELLS_MAXIMUM * 8);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_field, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_prepass, 96 + 64 + 64 + 16 + 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_cascades, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_motion, 16 + 16 + 64 + 16 + 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_temporal_pass, sizeof(struct xgpu_motion_pass));
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_mesh, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_rt_mesh, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_rt_vertex, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_rt_material, 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_rt_texture, 80);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_rt_pose, 16 + 64 * 48);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_world_ao_instance, 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_world_ao_instances, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_stand_in, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_set_targets, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_string, 16);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_view, 72);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_mark_owner, 40);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_clear, 36);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_read_target, 28);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_screenshot, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_sampler, 24);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_texture_stage, 72);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pixel_key, 264);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_key, 308);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pixel_program, 436);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_pipeline_define, 752);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_state, 68);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_draw_uniforms, 588);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertex_element, 6);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_layout_define, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertex_stream, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vertices, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_draw_immediate, 28);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_block, 6192);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_halos, 192);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_rt_shadow_caster, 32);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_rt_shadows, 160);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_rt_bounce, 16 + 48 * XGPU_STREAM_RT_BOUNCE_LIGHTS);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_lt_light, 144);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_lt_lights, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_lt_inputs, 20);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_dynamic_lights_world, 12);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_info, 108);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_eyes, 36 + 4 * 52);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_eye_transforms, 8 + 4 * 64);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_screen_corrections, 4 + 4 * 14);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_frame_record, 44);
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_submit, 44 + 8 + 4 * (116 + 8));
XGPU_STREAM_ASSERT_SIZE(struct xgpu_stream_vr_state, 4 * (2 + 4 + 32 + 3 + 3 + 3 + 1 + 34 + 19 + 1 + 8 + 50 + 80 + 71 + 3 + 39));

#endif
