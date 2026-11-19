"""display.soft_particles: a first-person weapon's effect over the weapon's own pixels (the transparent groups' first
pass, whose stencil accepts only them) is soft against the weapon's depth, and where it is soft its fade is its depth
test (xgpu.h's xgpu_effect_untested), so that a flat sprite (the plasma pistol's shot) meets the gun without a
straight cut. The rule, built alone from d3d8_device.c's draw and xgpu.h: with the setting off (the key's vr_effect 0)
every draw keeps its depth test, the Xbox's; on (the negative control) only the first-person group's draw of a pixel
program the fade is sure for tests none, never a world effect's nor one that samples texture 2 or 3 or a cube. The
game gives 2 only in a first-person group's first pass. No GPU or game data."""
import re
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DEVICE = ROOT / "port" / "linux" / "src" / "d3d8_device.c"
XGPU = ROOT / "port" / "linux" / "src" / "xgpu.h"
MANIFEST = ROOT / "port" / "linux" / "src" / "xgpu_manifest.c"
GEOMETRY = ROOT / "source" / "rasterizer" / "xbox" / "rasterizer_xbox_transparent_geometry.c"

RULE = ("\tif (key.vr_effect == 1 && inputs.soft_group == 2 && xgpu_effect_untested(key.texture_modes, key.sampler_type[0]))\n"
        "\t\tpipeline_key.depth_test = 0;\n")


def helper_source() -> str:
    text = XGPU.read_text(encoding="utf-8")
    start = text.index("static __inline int xgpu_effect_untested(")
    return text[start:text.index("\n}\n", start) + 3]


def soft_source() -> str:
    text = DEVICE.read_text(encoding="utf-8")
    start = text.index("void halo_effect_soft(int soft)")
    return text[start:text.index("\n}\n", start) + 3]


def run_rule(tmp_path: Path, vr_effect: int) -> str:
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a C compiler")
    source = tmp_path / "untested.c"
    source.write_text(r"""
#include <stdio.h>
typedef unsigned long DWORD;
enum { _xgpu_sampler_none, _xgpu_sampler_2d, _xgpu_sampler_3d, _xgpu_sampler_cube };
struct nv2a_pixel_shader_key { DWORD texture_modes; unsigned char sampler_type[4]; unsigned char vr_effect; };
static struct { unsigned int soft_group; } inputs;
""" + helper_source() + soft_source() + r"""
static int depth_test(int group, DWORD modes, int sampler)
{
    struct nv2a_pixel_shader_key key = { 0 };
    struct { int depth_test; } pipeline_key = { 1 };

    halo_effect_soft(group);
    key.texture_modes = modes;
    key.sampler_type[0] = (unsigned char)sampler;
    key.vr_effect = inputs.soft_group ? VR_EFFECT : 0;
""" + RULE + r"""    return pipeline_key.depth_test;
}

int main(void)
{
    /* the groups: hard, a world effect's, a first-person one's first pass; the programs: an effect's (texture 0
    2D, a secondary 2D map or none), one sampling texture 2, one with a cube in texture 0, texture 0 projected 3D */
    static const DWORD modes[] = { 1, 1 | 1 << 5, 1 | 1 << 10, 1, 2 };
    static const int samplers[] = { _xgpu_sampler_2d, _xgpu_sampler_2d, _xgpu_sampler_2d, _xgpu_sampler_cube,
        _xgpu_sampler_3d };
    int group, program;

    for (group = 0; group < 3; group++)
        for (program = 0; program < 5; program++)
            printf("%d", depth_test(group, modes[program], samplers[program]));
    printf("\n");
    return 0;
}
""")
    binary = tmp_path / "untested"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", f"-DVR_EFFECT={vr_effect}", str(source),
                    "-o", str(binary)], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True, check=True).stdout.strip()


def test_off_every_draw_keeps_its_depth_test(tmp_path):
    # (display.soft_particles off, or a backend without it: soft_effect_key's 0)
    assert run_rule(tmp_path, 0) == "11111" * 3


def test_on_only_the_first_person_effect_tests_none(tmp_path):
    # (the negative control: on, the first-person group's effect programs alone change)
    assert run_rule(tmp_path, 1) == "11111" + "11111" + "00111"


def test_the_rule_is_the_draws_and_the_manifests():
    text = DEVICE.read_text(encoding="utf-8")
    assert text.count(RULE) == 1
    # (after the basis is noted: the manifest's recording derives it, xgpu_manifest_derive)
    assert text.index("manifest_note_basis(program, &pipeline_key") < text.index(RULE)
    manifest = MANIFEST.read_text(encoding="utf-8")
    assert re.search(r"context->soft == 2 && xgpu_effect_untested\(derived->key\.pixel\.texture_modes,\s+"
                     r"derived->key\.pixel\.sampler_type\[0\]\)\)\n\t+derived->key\.depth_test = 0;", manifest)


def test_the_game_gives_2_only_in_the_first_persons_first_pass():
    text = GEOMETRY.read_text(encoding="utf-8")
    call = text[text.index("halo_effect_soft(\n"):]
    call = call[:call.index(");") + 2]
    assert re.sub(r"\s+", " ", call) == (
        "halo_effect_soft( TEST_FLAG(group->geometry_flags, _rasterizer_geometry_no_zbuffer_bit) ? 0 : "
        "!TEST_FLAG(group->geometry_flags, _rasterizer_geometry_first_person_bit) ? 1 : !pass ? 2 : 0);")
    # (its first pass is the stencil's accept, the weapon's own pixels)
    first = text[text.index("else if (!pass)\n"):]
    first = first[:first.index("else\n")]
    assert "rasterizer_set_stencil_mode(RASTERIZER_STENCIL_MODE_ACCEPT);" in first
