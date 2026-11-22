"""debug.retained_draws: the states' hash a kept draw is found by follows the setters.

A draw kept (its hash noted) is found again while nothing it is made from
changes; a pipeline's state changed after it must not find it (the draw is
prepared, and the backend told, again); a uniform's state (made again for
every draw) leaves it found. The negative control writes a state past the
setters: the kept hash no longer says what the tables hold, which
debug.retained_draws_verify reports, until the tables are marked written.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def _between(text, start, end):
    first = text.index(start)
    return text[first:text.index(end, first)]


def _function(text, signature):
    start = text.index(signature)
    brace = text.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def _enums(names):
    pdb = (ROOT / "port/include/xdk/xdk_pdb.h").read_text()
    lines = []
    for name in names:
        value = re.search(r"\b%s = (\d+)," % name, pdb).group(1)
        lines.append("#define %s %s" % (name, value))
    return "\n".join(lines)


def test_retained_draws_state_hash(tmp_path):
    cc = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("needs a native C compiler")
    device = (ROOT / "port/linux/src/d3d8_device.c").read_text()
    block = _between(device, "/* ---------- the states' hash (debug.retained_draws)",
        "/* ---------- render and texture stage state */")
    simple = _function(device, "void D3DFASTCALL D3DDevice_SetRenderState_Simple(")
    d3d8 = (ROOT / "port/include/xdk/xdk_d3d8.h").read_text()
    table = _between(d3d8, "HALO_XDK_TABLE DWORD D3DSIMPLERENDERSTATEENCODE[D3DRS_SIMPLE_MAX] =", "};") + "};"
    table = table.replace("HALO_XDK_TABLE ", "static const ")
    enums = _enums(["D3DRS_PSCONSTANT0_0", "D3DRS_PSCONSTANT1_0", "D3DRS_PSFINALCOMBINERCONSTANT0",
        "D3DRS_PSFINALCOMBINERCONSTANT1", "D3DRS_FOGCOLOR", "D3DRS_FOGSTART", "D3DRS_FOGEND", "D3DRS_FOGDENSITY",
        "D3DRS_POINTSIZE", "D3DRS_BLENDCOLOR", "D3DRS_STENCILREF", "D3DRS_POLYGONOFFSETZSLOPESCALE",
        "D3DRS_POLYGONOFFSETZOFFSET", "D3DRS_SIMPLE_MAX", "D3DRS_MAX", "D3DRS_ALPHABLENDENABLE", "D3DRS_ZFUNC",
        "D3DTSS_BUMPENVMAT00", "D3DTSS_BUMPENVMAT01", "D3DTSS_BUMPENVMAT10", "D3DTSS_BUMPENVMAT11",
        "D3DTSS_BUMPENVLSCALE", "D3DTSS_BUMPENVLOFFSET", "D3DTSS_MAX", "D3DTSS_MINFILTER", "D3DRS_ALPHAREF",
        "D3DTSS_MIPMAPLODBIAS"])
    source = tmp_path / "retained_draws.c"
    source.write_text(r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned int DWORD;
typedef int BOOL;
#define TRUE 1
#define FALSE 0
#define D3DFASTCALL
#define D3DTSS_MAXSTAGES 4
""" + enums + "\n" + table + r"""
static DWORD D3D__RenderState[D3DRS_MAX];
static DWORD D3D__TextureState[D3DTSS_MAXSTAGES][D3DTSS_MAX];
""" + block + simple + r"""
int main(void)
{
    uint64_t kept;
    unsigned long index, uniform_moved;

    /* a clean draw: kept by the hash of the states as they are */
    render_state_set(D3DRS_ALPHABLENDENABLE, 0);
    kept = state_hash_now();
    assert(kept == state_hash_summed());
    /* the same states set again: found */
    render_state_set(D3DRS_ALPHABLENDENABLE, 0);
    texture_state_set(1, D3DTSS_MINFILTER, 0);
    assert(state_hash_now() == kept);
    /* a uniform's state (the combiners' constant, the fog's colour): found,
    its uniforms made again */
    render_state_set(D3DRS_PSCONSTANT0_0 + 3, 0x80402010);
    render_state_set(D3DRS_FOGCOLOR, 0x00ff00ff);
    texture_state_set(2, D3DTSS_BUMPENVMAT01, 0x3f800000);
    assert(state_hash_now() == kept);
    /* ...and the uniforms' states read again (uniform_moved), once one of
    them is set to another value; a pipeline's state, or the same value, not */
    uniform_moved = state_hash.uniform_moved;
    render_state_set(D3DRS_FOGCOLOR, 0x00ff00ff);
    render_state_set(D3DRS_ALPHABLENDENABLE, 0);
    assert(state_hash.uniform_moved == uniform_moved);
    texture_state_set(3, D3DTSS_MIPMAPLODBIAS, 0x3f000000);
    assert(state_hash.uniform_moved != uniform_moved);
    uniform_moved = state_hash.uniform_moved;
    render_state_set(D3DRS_ALPHAREF, 0x40);
    assert(state_hash.uniform_moved != uniform_moved);
    texture_state_set(3, D3DTSS_MIPMAPLODBIAS, 0);
    render_state_set(D3DRS_ALPHAREF, 0);
    kept = state_hash_now();
    /* a pipeline's state changed after it: not found (prepared again) */
    render_state_set(D3DRS_ALPHABLENDENABLE, 1);
    assert(state_hash_now() != kept);
    assert(state_hash_now() == state_hash_summed());
    render_state_set(D3DRS_ALPHABLENDENABLE, 0);
    assert(state_hash_now() == kept);
    texture_state_set(1, D3DTSS_MINFILTER, 2);
    assert(state_hash_now() != kept && state_hash_now() == state_hash_summed());
    texture_state_set(1, D3DTSS_MINFILTER, 0);
    /* a simple state, set as the game's inline setter sets it (the method,
    then the table) */
    for (index = 0; index < D3DRS_SIMPLE_MAX; index++)
    {
        D3DDevice_SetRenderState_Simple(D3DSIMPLERENDERSTATEENCODE[index], 7);
        D3D__RenderState[index] = 7;
        assert(state_hash_now() == state_hash_summed());
    }
    assert(state_hash_now() != kept);
    for (index = 0; index < D3DRS_SIMPLE_MAX; index++)
    {
        D3DDevice_SetRenderState_Simple(D3DSIMPLERENDERSTATEENCODE[index], 0);
        D3D__RenderState[index] = 0;
    }
    render_state_set(D3DRS_PSCONSTANT0_0 + 3, 0);
    assert(state_hash_now() == state_hash_summed());
    kept = state_hash_now();
    /* negative control: a state written past the setters is not followed
    (debug.retained_draws_verify says so)... */
    D3D__RenderState[D3DRS_ZFUNC] = 4;
    assert(state_hash_now() == kept);
    assert(state_hash_summed() != kept);
    /* ...until the tables are marked written whole (the uniforms' states
    read again too) */
    uniform_moved = state_hash.uniform_moved;
    state_hash_forget();
    assert(state_hash.uniform_moved != uniform_moved);
    assert(state_hash_now() == state_hash_summed() && state_hash_now() != kept);
    puts("ok");
    return 0;
}
""")
    binary = tmp_path / "retained_draws"
    subprocess.run([cc, "-std=c99", "-Wall", "-Werror", "-Wno-unused-function", str(source), "-o", str(binary)],
        check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
    assert result.stdout.strip() == "ok"
