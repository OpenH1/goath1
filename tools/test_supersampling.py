"""display.supersampling's and display.render_scale's arithmetic
(d3d8_device.c's screen_mode_choose).

The screen's targets are drawn at display.supersampling's factor each way,
or an older "ssaa2x"'s 2, or display.panini's, whichever is most, at most
the GPU's largest target, and independently of display.anti_aliasing's pass
or samples. display.render_scale's fraction (0.25 to 1) is the 3D view's
alone (temporal.factor), in every mode: the screen's targets stay at the
display's pixels (times the supersampling), and the view is upscaled or
stretched into them. The front end's code is compiled on its own over stubs
of the settings, the display and the backend's capabilities, for the
desktop, the Mac (whose "ssaa2x" drew none), iOS and Android (no
supersampling; the render scale their hosts' HALO_UPSCALE_RENDER_SCALE).
"""
import os
import re
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def function_text(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def temporal_text(text):
    """display.temporal_aa "fsr"'s scale of the 3D view (struct temporal), as the file declares it"""
    return re.search(r"static struct\n\{\n\tfloat factor;.*?\n\} temporal = [^;]*;\n", text, re.S).group(0)


def settings_text(text):
    """the anti-aliasing's and the supersampling's code, as one run of the file"""
    start = text.index("enum\n{\n\t_anti_aliasing_off,")
    end = text.index("/* ---------- display.hdr")
    return text[start:end]


HARNESS = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define SCREEN_HEIGHT 480
#define SCREEN_MAXIMUM_WIDTH 1920
static float screen_supersampling = 1.0f, screen_supersampling_asked = 1.0f;
static struct { long largest_target; } xgpu_backend_capabilities = { 32768 };
static const char *anti_aliasing_setting = "smaa";
static double supersampling_setting = 1.0, render_scale_setting = 1.0, panini = 1.0;
static unsigned long changes;
static long display_width = 2560, display_height = 1440;
static BOOL original;
static int logs;
static unsigned long config_changes(void) { return changes; }
static const char *config_string(const char *name)
{
	if (!strcmp(name, "display.anti_aliasing"))
		return anti_aliasing_setting;
	assert(!strcmp(name, "display.picture_shape"));
	return "4:3";
}
static double config_real(const char *name)
{
	if (!strcmp(name, "display.render_scale"))
		return render_scale_setting;
	assert(!strcmp(name, "display.supersampling"));
	return supersampling_setting;
}
static void platform_log(const char *format, ...) { (void)format; logs++; }
static void anti_aliasing_prepare(void) {}
static float panini_supersampling(long width, long height) { (void)width; (void)height; return (float)panini; }
/* ("original" resolution scaling: no display size) */
static BOOL platform_screen_mode(long *width, long *height)
{
	if (original)
		return FALSE;
	*width = display_width;
	*height = display_height;
	return TRUE;
}
/* (display.temporal_aa "fsr" where the backend has it: upscaling) */
static BOOL upscaling;
static BOOL temporal_upscaling(void) { return upscaling; }
static int motion_vectors_technique(void) { return upscaling ? 1 : 0; }
%FUNCTIONS%
static long drawn[2];
static float factor;
static void choose(void)
{
	long width;
	float scale[2];

	/* (as halo_screen_commit: the settings read, then the mode chosen) */
	changes++;
	anti_aliasing_read();
	screen_mode_choose(&width, scale);
	drawn[0] = (long)(width * scale[0] + 0.5f);
	drawn[1] = (long)(SCREEN_HEIGHT * scale[1] + 0.5f);
	factor = screen_supersampling;
	(void)anti_aliasing;
}
#define DRAWN(w, h) (choose(), drawn[0] == (w) && drawn[1] == (h))
/* (the 3D view's pixels: the targets' times the render scale) */
#define VIEW(w, h) ((long)(drawn[0] * temporal.factor + 0.5f) == (w) && (long)(drawn[1] * temporal.factor + 0.5f) == (h))
int main(void)
{
	/* (read only in the supersampled builds' checks: newer clang's
	-Wunused-but-set-global in the others) */
	(void)logs;
	/* unset (1): the display's pixels, whatever the anti-aliasing */
	assert(DRAWN(2560, 1440) && factor == 1.0f);
#if SUPERSAMPLED
	/* 1.5 at 2560x1440: 3840x2160, with SMAA, MSAA or none */
	supersampling_setting = 1.5;
	assert(DRAWN(3840, 2160) && factor == 1.5f);
	anti_aliasing_setting = "msaa4x";
	assert(DRAWN(3840, 2160));
	anti_aliasing_setting = "off";
	assert(DRAWN(3840, 2160));
	supersampling_setting = 1.25;
	assert(DRAWN(3200, 1800));
	supersampling_setting = 1.75;
	assert(DRAWN(4480, 2520));
	supersampling_setting = 2.0;
	assert(DRAWN(5120, 2880));
	/* outside 1 to 2: brought inside */
	supersampling_setting = 3.0;
	assert(DRAWN(5120, 2880));
	supersampling_setting = 0.5;
	assert(DRAWN(2560, 1440));
	supersampling_setting = 0.0;
	assert(DRAWN(2560, 1440));
	/* the most of it, "ssaa2x"'s and Panini's */
	supersampling_setting = 1.25;
	anti_aliasing_setting = "ssaa2x";
#if LEGACY
	assert(DRAWN(5120, 2880));
#else
	assert(DRAWN(3200, 1800));
#endif
	anti_aliasing_setting = "fxaa";
	panini = 1.6;
	assert(DRAWN(4096, 2304));
	panini = 1.0;
	/* at most the GPU's largest target: 2 asked, 1.6 drawn, said */
	supersampling_setting = 2.0;
	xgpu_backend_capabilities.largest_target = 4096;
	assert(DRAWN(4096, 2304) && screen_supersampling_asked == 2.0f && factor < 2.0f);
	xgpu_backend_capabilities.largest_target = 32768;
	/* 1080p, 16:10, 4:3 */
	supersampling_setting = 1.5;
	display_width = 1920;
	display_height = 1080;
	assert(DRAWN(2880, 1620));
	display_width = 1920;
	display_height = 1200;
	assert(DRAWN(2880, 1800));
	display_width = 1600;
	display_height = 1200;
	assert(DRAWN(2400, 1800));
	/* "original": the Xbox's 640x480, none */
	original = TRUE;
	assert(DRAWN(640, 480) && factor == 1.0f);
	original = FALSE;
	/* (said once a change, not each frame) */
	logs = 0;
	assert(DRAWN(2400, 1800) && DRAWN(2400, 1800) && !logs);
	/* display.render_scale: the 3D view's alone, at half the pixels each
	way at 1080p and 1440p, the screen's targets (the HUD's and the menus')
	at the display's; 0.25 at least, 1 (or 0) at most; under the
	supersampling, the view a fraction of the supersampled targets; not with
	"original" */
	supersampling_setting = 1.0;
	render_scale_setting = 0.5;
	display_width = 1920;
	display_height = 1080;
	assert(DRAWN(1920, 1080) && factor == 1.0f && temporal.factor == 0.5f && VIEW(960, 540));
	display_width = 2560;
	display_height = 1440;
	assert(DRAWN(2560, 1440) && VIEW(1280, 720));
	render_scale_setting = 0.75;
	assert(DRAWN(2560, 1440) && VIEW(1920, 1080));
	render_scale_setting = 0.1;
	assert(DRAWN(2560, 1440) && factor == 1.0f && temporal.factor == 0.25f);
	render_scale_setting = 1.5;
	assert(DRAWN(2560, 1440) && factor == 1.0f && temporal.factor == 1.0f);
	render_scale_setting = 0.0;
	assert(DRAWN(2560, 1440) && factor == 1.0f && temporal.factor == 1.0f);
	render_scale_setting = 0.5;
	supersampling_setting = 1.5;
	assert(DRAWN(3840, 2160) && factor == 1.5f && VIEW(1920, 1080));
	supersampling_setting = 1.0;
	original = TRUE;
	assert(DRAWN(640, 480) && factor == 1.0f && temporal.factor == 1.0f);
	original = FALSE;
	/* (the same with display.temporal_aa's upscaler as without: it upscales
	the view where the stretch would) */
	display_width = 1920;
	display_height = 1080;
	upscaling = TRUE;
	assert(DRAWN(1920, 1080) && factor == 1.0f && temporal.factor == 0.5f);
	upscaling = FALSE;
	render_scale_setting = 1.0;
	assert(DRAWN(1920, 1080) && temporal.factor == 1.0f);
#else
	/* (the negative control) iOS and Android: display.render_scale is not
	read; the render scale is the host's, HALO_UPSCALE_RENDER_SCALE (run once
	without and once with it), the 3D view's alone as a computer's */
	render_scale_setting = 0.5;
	if (getenv("HALO_UPSCALE_RENDER_SCALE"))
		assert(DRAWN(2560, 1440) && factor == 1.0f && temporal.factor == 0.5f && VIEW(1280, 720));
	else
		assert(DRAWN(2560, 1440) && factor == 1.0f && temporal.factor == 1.0f);
	/* none: the display's pixels */
	supersampling_setting = 2.0;
	anti_aliasing_setting = "ssaa2x";
	assert(DRAWN(2560, 1440) && factor == 1.0f);
#endif
	return 0;
}
"""


@pytest.mark.parametrize("build,defines,supersampled,legacy", [
    ("desktop", [], 1, 1),
    ("macos", ["-DHALO_MACOS"], 1, 0),
    ("ios", ["-DHALO_MACOS", "-DHALO_IOS"], 0, 0),
    ("android", ["-DHALO_ANDROID"], 0, 0),
])
def test_supersampling(tmp_path, build, defines, supersampled, legacy):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    text = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    functions = temporal_text(text) + settings_text(text) + "".join(
        function_text(text, signature) for signature in (
            "static long screen_narrowest_width(", "static void screen_mode_choose(")
    )
    source = tmp_path / f"supersampling_{build}.c"
    source.write_text(HARNESS.replace("%FUNCTIONS%", functions))
    binary = tmp_path / f"supersampling_{build}"
    subprocess.run([cc, "-std=gnu89", "-Wall", "-Werror", "-Wno-unused-function", "-Wno-unused-variable", *defines,
                    f"-DSUPERSAMPLED={supersampled}", f"-DLEGACY={legacy}", str(source), "-o", str(binary)],
                   check=True)
    environment = {name: value for name, value in os.environ.items() if name != "HALO_UPSCALE_RENDER_SCALE"}
    run = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
    assert run.returncode == 0, run.stdout + run.stderr
    if not supersampled:
        run = subprocess.run([str(binary)], capture_output=True, text=True,
                             env={**environment, "HALO_UPSCALE_RENDER_SCALE": "0.5"})
        assert run.returncode == 0, run.stdout + run.stderr
