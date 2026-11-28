"""display.temporal_aa "fsr"'s frame order (docs/temporal-aa.md): the window's 3D view drawn at the render scale, its
motion resolved, the view upscaled into the display's back buffer, and the tone map and the HUD drawn over it there, at
the display's scale (port/linux/src/d3d8_device.c's own functions against fake targets and a fake backend); at the
native scale the same order in one target; off below it, the view and its tone map at the render scale, stretched into
the display's back buffer before the HUD; off at it, nothing moved. And render.c's window: the opaque view kept after
the fog and before the particles, the upscale (the motion's end) before the tone map, the anti-aliasing, the Panini
pass and the HUD. The upscaler's texture LOD bias (each vendor's) on the view's draws alone, and the jitter's phases
at the render scale."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, enum_with, function, mutated, read, run, structure  # noqa: E402

DEVICE = "port/linux/src/d3d8_device.c"
STREAM = "port/linux/src/xgpu_stream.h"
RENDER = "source/render/render.c"

CONTROLS = [
    # the upscale's destination taken at the view's scale: the tone map and the HUD drawn there, not over it
    ("\ttemporal_scale_display();\n\tdestination = render_target_get(&device.back_buffer);",
     "\tdestination = render_target_get(&device.back_buffer);", "upscaled"),
    # the view drawn at the display's scale, the render scale never taken
    ("\tif (eligible && device.gl_ready && !screen_view)\n\t\ttemporal_scale_render();\n", "", "upscaled"),
    # display.render_scale off the upscaler: the screen's targets all at it (the HUD too), the view never stretched
    ("\tif (eligible && device.gl_ready && !screen_view)\n\t\ttemporal_scale_render();\n", "", "stretched"),
    # the stretch's destination taken at the view's scale: the HUD drawn into the view's target
    ("\tsource = drawn && device.gl_ready ? render_target_get(&device.back_buffer) : NULL;\n\ttemporal_scale_display();\n",
     "\tsource = drawn && device.gl_ready ? render_target_get(&device.back_buffer) : NULL;\n", "stretched"),
    # the upscale sent before the motion it reads is resolved
    ("\txgpu_backend_motion(XGPU_MOTION_RESOLVE, target->target.number, &pass);\n"
     "\t/* (display.temporal_aa \"fsr\": then upscaled, debug.motion_vectors' view\n\tof it too) */\n"
     "\tif (temporal_upscaling())\n\t\ttemporal_upscale(target, x0, y0, x1, y1, history, window, camera);",
     "\tif (temporal_upscaling())\n\t\ttemporal_upscale(target, x0, y0, x1, y1, history, window, camera);\n"
     "\txgpu_backend_motion(XGPU_MOTION_RESOLVE, target->target.number, &pass);", "native"),
    # display.temporal_aa_sharpness sent as set, past 1 or below 0 too
    ("\tpass.sharpness = pass.sharpness > 0.0f ? (pass.sharpness < 1.0f ? pass.sharpness : 1.0f) : 0.0f;\n", "",
     "sharpness"),
    # the upscaler's LOD bias on every draw of the window, a shadow map's too
    ("motion.on && device.render_target == &device.back_buffer ? motion.lod_bias : 0.0f",
     "motion.on ? motion.lod_bias : 0.0f", "lod_bias"),
    # FSR's bias without AMD's extra stop
    ("log2f(fraction) - (motion_vectors_technique() == 1 ? 1.0f : 0.0f)", "log2f(fraction)", "lod_bias"),
    # a phone's render scale the host's alone, 1 to the game: the system stretches it, nothing upscales it (#1228)
    ("#elif defined(HALO_ANDROID) || defined(HALO_IOS)\n\t/* (a phone's host", "#elif 0\n\t/* (a phone's host",
     "phone_scale"),
]


def generated(fault=None):
    device = read(DEVICE)
    if fault:
        device = mutated(device, fault[0], fault[1])
    stream = read(STREAM)
    states = "\n".join(re.search(pattern, device, re.M | re.S).group(0) for pattern in (
        r"^static struct\n\{\n\tfloat factor;.*?^\} temporal = .*?;\n", r"^static struct\n\{\n\tBOOL on, objects;.*?^\} motion;\n"))
    # (halo_motion_window's light-transport value, defined beside it)
    defines = re.search(r"^#define HALO_MOTION_PREVIOUS .*$", device, re.M).group(0)
    code = "\n\n".join([states, defines] + [function(device, name) for name in (
        "scaled_pixel", "view_corners", "temporal_scale_render", "temporal_scale_display", "temporal_lod_bias",
        "draw_lod_bias", "halo_screen_view_begin", "halo_screen_view_end", "halo_motion_window", "temporal_upscale",
        "halo_motion_resolve", "halo_screen_tone_map", "halo_motion_jitter_phases", "render_scale_configured")])
    types = "\n\n".join([enum_with(stream, "XGPU_MOTION_RESOLVE"), structure(stream, "xgpu_motion_pass"),
                         structure(stream, "xgpu_temporal_pass")])
    return (("stream.inc", types), ("under_test.inc", code))


def window_order(render):
    """render_window's calls in the order the frame needs; the first out of order, or None"""
    text = function(render, "render_window")
    order = ["structure_render_fog();", "motion_vectors_opaque(&rasterizer_camera->viewport_bounds);",
             "render_particles();", "rasterizer_lens_flares_draw();",
             "motion_vectors_window_end(&rasterizer_camera->viewport_bounds);", "halo_screen_tone_map(",
             "halo_screen_anti_alias(", "panini_window_draw();", "halo_screen_view_end(TRUE,",
             "interface_draw_screen();"]
    at = 0
    for call in order:
        found = text.find(call, at)
        if found < 0:
            return call
        at = found + len(call)
    return None


@pytest.mark.parametrize("case", ["upscaled", "native", "off", "stretched", "sharpness", "lod_bias", "phone_scale"])
def test_case(case):
    status, output = run(build("temporal_order", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("temporal_order", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_render_window_order():
    assert window_order(read(RENDER)) is None


def test_render_window_order_negative_control():
    # (the tone map moved before the upscale: over the view at the render scale)
    render = read(RENDER)
    block = re.search(r"\t\t\thalo_screen_tone_map\(\n(?:\t\t\t\trasterizer_camera->viewport_bounds\.\w\d[,)];?\n){4}",
                      render[render.index("motion_vectors_window_end(&"):])
    assert block
    moved = mutated(render, "\t\t\tmotion_vectors_window_end(&rasterizer_camera->viewport_bounds);\n" + block.group(0),
                    block.group(0) + "\t\t\tmotion_vectors_window_end(&rasterizer_camera->viewport_bounds);\n")
    assert window_order(moved) is not None
