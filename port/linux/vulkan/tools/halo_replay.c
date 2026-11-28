/*
HALO_REPLAY.C

The renderer's command stream replayed from a recording with the Vulkan
backend (HALO_RENDER_RECORD: port/android/host/host_render_record.h), on
the Linux builds' host: the replay of port/tools/replay/host_render_replay.h,
its options and its service (--serve), offscreen (no window: the backend
draws the frames and reads them back, as the game's does with SDL's
offscreen driver). ninja linux_replay builds it as build/<name>/halo_replay;
ninja windows_replay, with the Windows host's renderer and its POSIX
functions (tools/windows_build.py), as build/windows/halo_replay.exe.

The pipeline cache is <file>.vkpipelines (HALO_VULKAN_PIPELINE_CACHE names
another file, or none: empty); HALO_VULKAN_VALIDATION=1 turns on the
validation layers. HALO_LT_SHADER_DIR (debug) reads the light-transport
pass's kernels from that folder's SPIR-V (tools/rt2_scorecard.py shader),
again at each "reload" request.
*/

#include "host.h"
#include "host_render_record.h"
#include "host_render_replay.h"
#include "host_render_stream.h"
#include "xgpu_vulkan.h"
#include "xgpu_vulkan_denoise.h"
#include "xgpu_vulkan_lt.h"
#include "xgpu_vulkan_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- what the host gives the backend, here */

/* (no window: the backend asks for none) */
void *host_sdl_handle_get(uint32_t handle, int type)
{
	(void)handle;
	(void)type;
	return NULL;
}

/* (no display: a 60 Hz one's) */
uint64_t host_vulkan_display_period(void)
{
	return 16666667;
}

/* ---------- the platform */

static int vulkan_initialize(int window, int width, int height)
{
	struct xgpu_vulkan_options options;
	static struct host_render_backend backend;
	const char *validation = getenv("HALO_VULKAN_VALIDATION"), *cache = getenv("HALO_VULKAN_PIPELINE_CACHE");

	(void)window;
	memset(&options, 0, sizeof(options));
	options.cache_path = cache && *cache ? cache : NULL;
	options.validation = validation && atoi(validation);
	/* (its frames' GPU times) */
	options.statistics = 1;
	if (!xgpu_vulkan_initialize(&options))
		return 0;
	xgpu_vulkan_window_resized((uint32_t)width, (uint32_t)height);
	backend = *xgpu_vulkan_stream_backend();
	/* (a world pipeline waited for, not its stand-in drawn: the frames
	played from the start have no map preparation's pipelines) */
	xgpu_vulkan_stream_wait_world(1);
	host_render_stream_set_backend(&backend);
	host_render_record_set_shooter(xgpu_vulkan_stream_shot);
	return 1;
}

static void vulkan_frame_statistics(struct host_render_replay_statistics *to)
{
	struct xgpu_vulkan_statistics statistics;

	xgpu_vulkan_statistics(&statistics);
	to->gpu_ms = statistics.gpu_frames ? statistics.gpu_milliseconds / statistics.gpu_frames : 0.0;
	to->compile_ms = statistics.pipeline_seconds * 1000.0;
	to->draws = statistics.draws;
	to->compiles = statistics.pipelines_made;
}

static void vulkan_ids_draw(uint32_t index)
{
	xgpu_vulkan_stream_ids_draw(index);
}

/* ---------- the cost: each draw timed with timestamps about it
(xgpu_vulkan_timing_*); the service's subtraction needs only the frame's
and its passes' */

static struct host_render_replay_pass_time cost_passes[1024];

static int vulkan_cost_end(struct host_render_replay_cost *cost)
{
	struct xgpu_vulkan_timing timing;
	uint32_t index;

	if (!xgpu_vulkan_timing_wait(10.0, &timing))
		return 0;
	if (timing.pass_count > sizeof(cost_passes) / sizeof(cost_passes[0]))
		timing.pass_count = sizeof(cost_passes) / sizeof(cost_passes[0]);
	for (index = 0; index < timing.pass_count; index++)
	{
		memset(&cost_passes[index], 0, sizeof(cost_passes[index]));
		cost_passes[index].kind = timing.passes[index].kind;
		cost_passes[index].width = timing.passes[index].width;
		cost_passes[index].height = timing.passes[index].height;
		cost_passes[index].draws = timing.passes[index].draws;
		cost_passes[index].ms = timing.passes[index].ms;
		cost_passes[index].start_ms = timing.passes[index].start_ms;
	}
	cost->frame_ms = timing.frame_ms;
	cost->passes = cost_passes;
	cost->pass_count = timing.pass_count;
	cost->draw_pass = timing.draw_pass;
	cost->draw_count = timing.draw_count;
	cost->draw_ms = timing.draw_ms;
	cost->draw_method = "timestamps";
	cost->note = timing.note;
	return 1;
}

/* ---------- the CPU's time: the backend's calls of the driver, counted and
timed by kind (xgpu_vulkan_calls_watch) */

static void vulkan_calls_watch(uint64_t *ticks, uint64_t *counts)
{
	xgpu_vulkan_calls_watch(ticks, counts, ticks ? host_render_replay_ticks : NULL);
}

/* ---------- debug: the light pass's views (the service's rt_view) */

/* lt/denoise_temporal.frag's PROBE fields, its four windows' in turn */
static const char *const vulkan_denoise_view_names[16] = {
	"found", "cut", "kept", "count", "z_factor", "bounce_hold", "footprint", "flags",
	"grad_before", "grad_now", "lit_before", "lit_now", "relit", "cap", "z_score", "mean_luminance" };

static float vulkan_half(uint16_t half)
{
	uint32_t sign = (uint32_t)(half >> 15) << 31, exponent = (half >> 10) & 31, mantissa = half & 1023, bits;
	float value;

	if (!exponent)
	{
		value = (float)mantissa * (1.0f / 16777216.0f);
		return sign ? -value : value;
	}
	bits = sign | (exponent == 31 ? 0x7f800000u | (mantissa << 13) : ((exponent + 112) << 23) | (mantissa << 13));
	memcpy(&value, &bits, sizeof(value));
	return value;
}

/* the "sample" and "light" views' fields */
static const char *const vulkan_light_view_names[4] = { "r", "g", "b", "a" };

static const char *vulkan_view_setting(const char *name)
{
	return name && (!strcmp(name, "denoise") || !strcmp(name, "sample") || !strcmp(name, "light")) ?
		"HALO_LT_DENOISE_PROBE=1" : NULL;
}

/* "denoise": the denoiser's temporal stage (lt/denoise_temporal.frag's PROBE fields); "sample": the diffuse sample
it took (rgb, a the bounce's count); "light": the denoised diffuse irradiance (rgb, a its deviation) */
static int vulkan_view_read(const char *name, struct host_render_replay_view *view)
{
	int which = !vulkan_view_setting(name) ? -1 : !strcmp(name, "sample") ? 1 : !strcmp(name, "light") ? 2 : 0;
	/* (the sample: the probe's fifth window) */
	uint32_t width, height, x, y, field, fields = which ? 4 : 16, windows = which == 2 ? 1 : 5;
	uint32_t from = which == 1 ? 16 : 0;
	uint16_t *halves;

	memset(view, 0, sizeof(*view));
	if (which < 0 || !xgpu_vulkan_denoise_probe_read(which, &halves, &width, &height))
		return 0;
	view->values = malloc((size_t)width * height * fields * sizeof(float));
	if (!view->values)
	{
		free(halves);
		return 0;
	}
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			for (field = 0; field < fields; field++)
				view->values[((size_t)y * width + x) * fields + field] =
					vulkan_half(halves[(((size_t)y * windows * width) + ((from + field) / 4) * width + x) * 4 +
						(from + field) % 4]);
	free(halves);
	view->width = width;
	view->height = height;
	view->fields = fields;
	view->names = which ? vulkan_light_view_names : vulkan_denoise_view_names;
	return 1;
}

static int vulkan_lt_checkpoint(int what)
{
	if (what)
		xgpu_vulkan_lt_checkpoint_ask(what);
	return (xgpu_vulkan_lt_checkpoint_pending() ? 2 : 0) | (xgpu_vulkan_lt_checkpoint_valid() ? 1 : 0);
}

static const struct host_render_replay_platform vulkan_platform =
{
	.backend = "vulkan",
	.initialize = vulkan_initialize,
	.shutdown = xgpu_vulkan_shutdown,
	.frame_statistics = vulkan_frame_statistics,
	.read_target = xgpu_vulkan_stream_read,
	.ids_begin = xgpu_vulkan_stream_ids_begin,
	.ids_draw = vulkan_ids_draw,
	.ids_read = xgpu_vulkan_stream_ids_read,
	.ids_end = xgpu_vulkan_stream_ids_end,
	.cost_begin = xgpu_vulkan_timing_begin,
	.cost_draw_method = "timestamps",
	.cost_draw = xgpu_vulkan_timing_draw,
	.cost_end = vulkan_cost_end,
	.call_kinds = XGPU_VULKAN_CALL_KINDS,
	.call_kind_names = xgpu_vulkan_call_kind_names,
	.calls_watch = vulkan_calls_watch,
	/* (debug: the light-transport pass's kernels from HALO_LT_SHADER_DIR's
	SPIR-V, read again on "reload") */
	.shaders_reload = xgpu_vulkan_lt_shaders_reload,
	/* (debug: the light pass's views, rt_view) */
	.view_read = vulkan_view_read,
	.view_setting = vulkan_view_setting,
	.lt_checkpoint = vulkan_lt_checkpoint,
};

int main(int argc, char *argv[])
{
	char cache[4200];
	int index;

	/* (debug.pass_times, as the game reads it: each pass's GPU time logged,
	every so many frames; port/tools/replay's host_pass_times) */
	if (getenv("HALO_PASS_TIMES") && atoi(getenv("HALO_PASS_TIMES")) > 0)
		host_pass_times = atoi(getenv("HALO_PASS_TIMES"));
	/* (the pipeline cache beside the recording, as Metal's archive is) */
	for (index = 1; index < argc; index++)
	{
		if (!strcmp(argv[index], "--compare"))
			break;
		if (argv[index][0] != '-' && (index == 1 || argv[index - 1][0] != '-' || !strcmp(argv[index - 1], "--serve") ||
			!strcmp(argv[index - 1], "--quiet") || !strcmp(argv[index - 1], "--verbose") ||
			!strcmp(argv[index - 1], "--unpaced")))
		{
			snprintf(cache, sizeof(cache), "%s.vkpipelines", argv[index]);
#ifdef _WIN32
			if (!getenv("HALO_VULKAN_PIPELINE_CACHE"))
				_putenv_s("HALO_VULKAN_PIPELINE_CACHE", cache);
#else
			setenv("HALO_VULKAN_PIPELINE_CACHE", cache, 0);
#endif
			break;
		}
	}
	return host_render_replay_main(argc, argv, &vulkan_platform);
}
