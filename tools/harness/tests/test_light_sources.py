"""The enhanced dynamic lights' new lights (source/objects/object_lights.c's flare-only lights of units and items,
port/linux/game/light_sources.c's lights for what has none)."""
import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, build, function, inline, mutated, read, run, structure

CASES = ["flare-units-items", "weapon-glow", "room-kept", "flash-fades", "magazine-strength", "stream-per-firer",
         "emitter-rows", "weapon-glow-rounds"]
CONTROLS = {
    # an empty needler's glow as bright as a full one's (#1309)
    "rounds-ignored": (("\t\t\tshare *= PIN(object->object.incoming_function_values[incoming], 0.0f, 1.0f);",
                        "\t\t\tshare *= 1.0f;"), "weapon-glow-rounds"),
    # (a machine's flares lit again over its lightmap's light)
    "scenery-lit": (("object->object.type == _object_type_weapon || object->object.type == _object_type_equipment;",
                     "object->object.type == _object_type_weapon || object->object.type == _object_type_equipment ||\n"
                     "\t\tobject->object.type == _object_type_machine;"), "flare-units-items"),
    "copies-each-whole": (("/ (real)MAX(copies, 1);", "/ (real)MAX(copies - copies, 1);"), "weapon-glow"),
    "room-ignored": (("return MIN(light_sources.candidate_count, MIN(room, LIGHT_SOURCES_MAXIMUM));",
                      "return MIN(light_sources.candidate_count, LIGHT_SOURCES_MAXIMUM);"), "room-kept"),
    "farthest-first": (("return left->score > right->score ? -1 : 1;", "return left->score > right->score ? 1 : -1;"),
                       "room-kept"),
    "flash-gone-with-effect": (("\t\t\tflash->position = position;\n\t\t\tcontinue;",
                                "\t\t\tflash->position = position;\n\t\t\tlight_sources_add(&position, entry->radius, "
                                "&entry->color, 1.0f, FALSE, 0);\n\t\t\tcontinue;"), "flash-fades"),
    # (#1248: an effect lasting past its flash flashed again each time it was over)
    "flash-repeats": (("\t\tif (fraction >= 1.0f && flash->seen)\n\t\t\tcontinue;\n", ""), "flash-fades"),
    "flash-restarted": (("\t\t\t\tflash->start = time;\n\t\t\t}\n", "\t\t\t}\n\t\t\tflash->start = time;\n"),
                        "flash-fades"),
    "magazine-ignored": (("return 0.35f + 0.65f * PIN(fraction, 0.0f, 1.0f);", "return 1.0f;"), "magazine-strength"),
    # (a light a flame, not a firer: dozens of lights for one flamethrower)
    "stream-each-flame": (("if (light_sources.streams[index].owner_index == owner_index && "
                           "light_sources.streams[index].row == row)\n\t\t\tbreak;",
                           "if (FALSE)\n\t\t\tbreak;"), "stream-per-firer"),
}


def generated(control=None):
    lights = read("source/objects/object_lights.c")
    sources = read("port/linux/game/light_sources.c")
    header = read("port/linux/game/light_sources.h")
    math = read("source/math/real_math.h")
    helpers = "\n".join(inline(math, name) for name in ["vector_from_points3d", "dot_product3d", "magnitude_squared3d",
                                                         "magnitude3d"])
    glow = "\n".join(re.findall(r"^#define LIGHT_PORT_WEAPON_GLOW_\w+ .+$", lights, re.M))
    defines = "\n".join(re.findall(r"^#define LIGHT_SOURCES_(?:RANGE .+|KEY\((?:.*\\\n)*.*)$", sources, re.M))
    enums = "\n".join(re.findall(r"^enum\n\{\n(?:\t[^\n]*\n)*?\};", sources, re.M))
    tables = "\n".join(re.search(r"^static struct light_source_" + name + r" const light_source_" + name +
                                 r"s\[\] =\n\{.*?^\};", sources, re.M | re.S).group(0)
                       for name in ["object", "effect"])
    state = re.search(r"^static struct\n\{\n\t/\* whether the map's rows.*?^\} light_sources;", sources,
                      re.M | re.S).group(0)
    code = "\n".join([
        glow, function(lights, "light_port_flare_lights"), function(lights, "light_port_weapon_glow_rounds"),
        function(lights, "light_port_weapon_glow"),
        defines, enums, structure(header, "light_source"), structure(sources, "light_source_object"),
        structure(sources, "light_source_effect"), tables, structure(sources, "light_source_flash"), state,
        "static void light_sources_find(void);", "static void light_sources_objects(real time);",
        function(sources, "light_sources_flicker"), function(sources, "light_sources_pulse"),
        function(sources, "light_sources_magazine_strength"), function(sources, "light_sources_add"),
        function(sources, "light_sources_effect_position"), function(sources, "light_sources_effects"),
        function(sources, "light_sources_stream_add"), function(sources, "light_sources_streams"),
        function(sources, "light_sources_compare"), function(sources, "light_sources_gather")])
    if control:
        code = mutated(code, *CONTROLS[control][0])
    return (("under_test.inc", helpers + "\n" + code),)


@pytest.mark.parametrize("case", CASES)
def test_case(case):
    status, output = run(build("light_sources", generated()), case)
    assert status == 0, output


@pytest.mark.parametrize("control", CONTROLS)
def test_negative_control(control):
    status, output = run(build("light_sources", generated(control)), CONTROLS[control][1])
    assert status == CHECK_FAILED and "check failed:" in output, f"{control} escaped its CHECK: {status}: {output}"


def test_enhanced_only():
    """Every new light is the enhanced dynamic lights' alone: classic draws the game's lights as ever."""
    lights = read("source/objects/object_lights.c")
    dynamic = read("port/linux/game/dynamic_lights.c")
    promotion = re.search(r"projectile_lights = .*?;", lights, re.S).group(0)
    assert "halo_dynamic_lights_enhanced()" in promotion
    assert "if (halo_dynamic_lights_enhanced())\n\t\tdynamic_lights_submit();" in lights
    assert "light_sources_gather(" in function(dynamic, "dynamic_lights_sources")
