"""display.shadows "ray_traced" (port/linux/include/rt_shadows.h, port/linux/game/rt_shadows.c, docs/world-ao.md
"Ray-traced shadows"): on only with the setting and the backend's ray queries; a caster packed as the backend's pass
reads it; the largest on screen kept past the most; the scene built before the casters are handed over. The classic
path's early-out where it is on: render_objects.c's render_object_shadows (checked below)."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, mutated, read, run  # noqa: E402

CASES = ["setting", "pack", "window"]
GAME = "port/linux/game/rt_shadows.c"
CONTROLS = [
    # on without the backend's ray queries
    ("\tif (halo_world_ao_ray_queries())\n\t\treturn 1;", "\treturn 1;", "setting"),
    # why not said every time
    ("\tif (!rt_shadows.missing_said && halo_world_ao_ray_queries_missing())",
     "\tif (halo_world_ao_ray_queries_missing())", "setting"),
    # the darkness the colour itself, not 1 - it
    ("rt_shadows_byte((1.0f - shadow_color[channel]) * strength)", "rt_shadows_byte(shadow_color[channel] * strength)",
     "pack"),
    # the child's owner left out (a vehicle's rider casts nothing)
    ("(child_index != -1 ? (unsigned int)(world_rt_owner(child_index) & 0xFFFF) << 16 : 0)", "0", "pack"),
    # the direction not made unit
    ("caster->direction[0] = length > 0.0f ? shadow_vector[0] / length : 0.0f;",
     "caster->direction[0] = shadow_vector[0];", "pack"),
    # past the most, a larger caster given up
    ("\t\tif (rt_shadows.pixels[slot] >= pixels)\n\t\t\treturn;", "\t\treturn;", "window"),
    # the scene not built for the window
    ("\tworld_ao_rt_frame();\n", "", "window"),
    # the casters kept from the window before
    ("\trt_shadows.count = 0;\n", "", "window"),
]


def generated(fault=None):
    game = read(GAME)
    if fault:
        game = mutated(game, *fault)
    headers = [(name, read(f"port/linux/include/{name}")) for name in ("rt_shadows.h", "world_ao.h", "world_rt.h")]
    return tuple(headers) + (("under_test.inc", game),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("rt_shadows", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("before,after,case", CONTROLS)
def test_negative_control(before, after, case):
    status, output = run(build("rt_shadows", generated((before, after))), case)
    assert status == CHECK_FAILED, output


def test_the_classic_path_is_skipped_where_it_is_on():
    # render_object_shadows returns before the atlas, its blur and the receivers when it is on
    source = read("source/render/render_objects.c")
    early = source.index("if (render_shadows && rt_shadows_on())")
    assert early < source.index("rasterizer_shadow_atlas_begin();") and early < source.index(
        "process_rendered_objects(&data);\n\n\t\t/* port: (as above) */")
    block = source[early:source.index("\tif (render_shadows)\n\t{", early)]
    assert "render_object_rt_shadows();" in block and "return;" in block
