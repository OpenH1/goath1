"""The lights' shadow rays never meet the first-person weapon and hands.

They are drawn in a view of their own, floating in front of the eye, not
where they are in the world, so a shadow ray that met them would cast a
gun-shaped shadow somewhere in the level. The ray-traced scene gives them
their own instance mask (xgpu_stream.h's XGPU_WORLD_AO_INSTANCE_FIRST_PERSON),
and every ray a light's visibility is traced with (xgpu_spirv.c's
light_shadows_rays, the dynamic lights' screen mask; flashlight_map_main,
the flashlight's shadow map) must leave that bit out of its mask. The local
player's own body (XGPU_WORLD_AO_INSTANCE_PLAYER) casts shadows for other
lights, and leaves its own lights' rays as their owner.
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPIRV = (ROOT / "port/linux/src/xgpu_spirv.c").read_text()
HEADERS = (ROOT / "port/linux/src/xgpu_spirv.h").read_text() + (ROOT / "port/linux/src/xgpu_stream.h").read_text()


def value(name):
    return int(re.search(r"#define %s (0x[0-9A-Fa-f]+|\d+)u?\b" % name, HEADERS).group(1), 0)


def function(name):
    start = SPIRV.index("\nstatic void %s(" % name) if "\nstatic void %s(" % name in SPIRV else \
        SPIRV.index("\nstatic unsigned int %s(" % name)
    return SPIRV[start:SPIRV.index("\n}\n", start)]


def ray_masks(source):
    """the masks the function's ray queries are initialized with (operands[3])"""
    return re.findall(r"operands\[3\] = constant_uint\(m, ([A-Za-z0-9_]+)\)", source)


def test_no_light_ray_meets_the_first_person_view():
    first_person = value("XGPU_WORLD_AO_INSTANCE_FIRST_PERSON")
    for name in ("light_shadows_rays", "flashlight_map_main"):
        masks = ray_masks(function(name))
        assert masks, name
        for mask in masks:
            bits = int(mask.rstrip("u"), 0) if mask[0].isdigit() else value(mask)
            assert not bits & first_person, (name, mask)


def test_other_lights_shadowed_by_the_players_body():
    masks = ray_masks(function("light_shadows_rays"))
    assert all((value(m) if not m[0].isdigit() else int(m.rstrip("u"), 0)) & value("XGPU_WORLD_AO_INSTANCE_PLAYER")
               for m in masks)
