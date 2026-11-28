"""The VR mode's effects (port/linux/vulkan/README.md, "VR"): the thickness
bake's distances to the edge of known shapes and each eye's transforms
(port/linux/vulkan/tests/effects_test.c, the real xgpu_vulkan_effects.c), and
the effects' and glows' SPIR-V (the emitter's, tools/spirv_corpus) valid for
Vulkan 1.1; and the thickness images' lifetimes: the stream's copy of one
looked up again once the texture cache has destroyed it, each baked on the
effects' workers (never on the render thread), and nothing baked from texels
the cache never read or made from texels since written. No GPU, display or game data is needed.
"""

import os
from pathlib import Path
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parent.parent


def compiler():
    return os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")


def test_thickness_and_views(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    binary = tmp_path / "effects_test"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-pthread",
                    str(ROOT / "port/linux/vulkan/tests/effects_test.c"), "-o", str(binary), "-lm"], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0 and run.stdout.strip() == "ok", run.stdout + run.stderr


def test_effect_shaders(tmp_path):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    build = tmp_path / "build"
    subprocess.run(["sh", str(ROOT / "tools/spirv_corpus/build.sh"), str(ROOT), str(build)], check=True,
                   env={**os.environ, "CC": cc})
    captures = tmp_path / "captures"
    captures.mkdir()
    corpus = tmp_path / "corpus"
    subprocess.run([str(build / "spirv_corpus"), str(captures), str(corpus)], check=True, capture_output=True)
    rows = [line.split("\t") for line in (corpus / "index.txt").read_text().splitlines()
            if line.startswith(("effect-", "glow-", "zsprite-", "screen-effect-"))]
    # soft and thick, one map and two, each fade, flat and foveated; the
    # glows' two shaders flat and foveated; a z-sprite's depth flat and in
    # both eyes, and as an effect soft and thick, each fade, flat and foveated
    assert len([row for row in rows if row[0].startswith("effect-")]) == 2 * 2 * 6 * 2
    assert len([row for row in rows if row[0].startswith("glow-")]) == 4
    # a flat screen's (display.soft_particles): soft, one map and two, each fade
    assert len([row for row in rows if row[0].startswith("screen-effect-")]) == 2 * 6
    assert len([row for row in rows if row[0].startswith("zsprite-")]) == 2 + 2 * 6 * 2
    files = sorted({row[1] for row in rows})
    assert all((corpus / f"{name}.spv").stat().st_size > 0 for name in files)
    validator = shutil.which("spirv-val")
    if not validator:
        pytest.skip("needs spirv-val (SPIRV-Tools)")
    for name in files:
        subprocess.run([validator, "--target-env", "vulkan1.1", str(corpus / f"{name}.spv")], check=True)


def _build_and_run(cc, tmp_path, name, text):
    source = tmp_path / f"{name}.c"
    source.write_text(text)
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    f"-I{ROOT / 'port/linux/src'}", str(source), "-o", str(binary)], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr


def test_stream_looks_the_thickness_up_again_once_destroyed(tmp_path):
    # The stream keeps the thickness it looked up for texture 0's stage and
    # the watched pages' serial. The texture cache lets an idle texture go
    # (its thickness destroyed) without either changing, and the same stage
    # draws again later: the image kept would be a destroyed one.
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    function = text[text.index("static int effect_textures(struct xgpu_vulkan_texture textures[XGPU_VULKAN_TEXTURES])"):
                    text.index("/* a draw of the current pipeline, made and drawn")]
    _build_and_run(cc, tmp_path, "effect_textures", r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "xgpu_stream.h"
#define XGPU_SPIRV_EFFECT_DEPTH_BINDING 2
#define XGPU_SPIRV_EFFECT_THICKNESS_BINDING 3
#define XGPU_VULKAN_TEXTURES 6
enum { XGPU_VULKAN_IMAGE_2D, XGPU_VULKAN_IMAGE_2D_ARRAY };
enum { XGPU_VULKAN_FILTER_POINT, XGPU_VULKAN_FILTER_LINEAR };
enum { XGPU_VULKAN_MIP_NONE };
enum { XGPU_VULKAN_ADDRESS_CLAMP };
typedef uint64_t VkSampler;
struct xgpu_vulkan_image { int destroyed; };
struct xgpu_vulkan_texture { struct xgpu_vulkan_image *image; VkSampler sampler; uint8_t kind; };
struct xgpu_vulkan_sampler_description { uint8_t minify, magnify, mip, address[3], anisotropy; };
struct host_render_texture_stage
{
    struct xgpu_stream_texture_stage stage;
    struct xgpu_stream_texture_level levels[XGPU_STREAM_MIP_LEVELS];
};
struct stream_target { int depth; struct xgpu_vulkan_image *image, *multisampled; };
#define XGPU_MULTIVIEW_NONE 0
static struct
{
    uint32_t depth;
    int samples;
    const struct xgpu_stream_pipeline_define *definition;
    struct host_render_texture_stage stages[4];
    struct xgpu_vulkan_texture textures[XGPU_VULKAN_TEXTURES];
    uint32_t textures_serial;
    struct xgpu_vulkan_image *thickness;
    int thickness_known;
    struct xgpu_stream_texture_stage thickness_stage;
    uint32_t thickness_serial, thickness_epoch;
} stream;
static struct stream_target depth_target = { 1, NULL, NULL };
static struct xgpu_vulkan_image images[2], depth_image, depth_copy;
static int copies, copy_fails;
static struct xgpu_vulkan_image *cached = &images[0];
static unsigned lookups;
static uint32_t epoch;
static struct stream_target *target_get(uint32_t number) { return number ? &depth_target : NULL; }
static void target_resolve(struct stream_target *target) { (void)target; }
static struct xgpu_vulkan_image *target_depth_sampled(struct stream_target *target)
{
    (void)target;
    copies++;
    return copy_fails ? NULL : &depth_copy;
}
static VkSampler xgpu_vulkan_sampler(const struct xgpu_vulkan_sampler_description *description)
{ return 1 + description->minify; }
static struct xgpu_vulkan_image *xgpu_vulkan_textures_thickness(const struct xgpu_stream_texture_stage *stage)
{
    (void)stage;
    lookups++;
    return cached;
}
static uint32_t xgpu_vulkan_textures_thickness_epoch(void) { return epoch; }
""" + function + r"""
int main(void)
{
    struct xgpu_vulkan_texture textures[XGPU_VULKAN_TEXTURES];

    depth_target.image = &depth_image;
    stream.depth = 1;
    stream.textures_serial = 7;
    stream.stages[0].stage.source = 1;
    stream.stages[0].stage.resource = 0x1000;
    /* the depth sampled is its copy, never the attachment drawn against
    (#1247): no pass of its own; where no copy can be made, the attachment
    in a pass that writes none of it */
    assert(effect_textures(textures) == 0 && copies == 1);
    assert(lookups == 1 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[0]);
    assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].image == &depth_copy);
    copy_fails = 1;
    assert(effect_textures(textures) == 1 && textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].image == &depth_image);
    copy_fails = 0;
    /* the same stage, serial and epoch: kept */
    effect_textures(textures);
    assert(lookups == 1 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[0]);
    /* the texture let go of (its thickness destroyed), the same stage drawn
    again, the serial as it was: looked up again, the new one drawn */
    images[0].destroyed = 1;
    epoch++;
    cached = &images[1];
    effect_textures(textures);
    assert(lookups == 2 && textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image == &images[1]);
    assert(!textures[XGPU_SPIRV_EFFECT_THICKNESS_BINDING].image->destroyed);
    effect_textures(textures);
    assert(lookups == 2);
    /* the eyes' depth is their layers; a flat screen's (display.soft_particles,
    a pipeline of no views) its one 2D image, as its shader samples it */
    assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D_ARRAY);
    {
        static struct xgpu_stream_pipeline_define flat;

        flat.key.views = XGPU_MULTIVIEW_NONE;
        stream.definition = &flat;
        assert(effect_textures(textures) == 0);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].image == &depth_copy);
        flat.key.views = 1;
        effect_textures(textures);
        assert(textures[XGPU_SPIRV_EFFECT_DEPTH_BINDING].kind == XGPU_VULKAN_IMAGE_2D_ARRAY);
    }
    return 0;
}
""")


LOOKUP_TEST = r"""
#include <assert.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "xgpu_stream.h"
#include "xgpu_vulkan_effects.c"
typedef uint32_t D3DCOLOR;
typedef uint32_t DWORD;
enum { XGPU_SOURCE_NONE, XGPU_SOURCE_TEXTURE };
enum { HOST_LOG_WARN };
enum { XGPU_VULKAN_IMAGE_2D };
enum { XGPU_VULKAN_FORMAT_RGBA8 };
enum { XGPU_VULKAN_USAGE_SAMPLED };
struct xgpu_texture_description { unsigned long width, height, depth; int cube_map, compressed; };
struct xgpu_vulkan_image_description { int type, format, usage; uint32_t width, height, depth_or_layers, levels; };
struct xgpu_vulkan_image { int destroyed; uint8_t texels[16 * 16 * 4]; };
struct entry
{
    struct xgpu_texture_description description;
    uint32_t address, size, generation;
    struct xgpu_vulkan_image *thickness;
    uint32_t thickness_generation, thickness_failed;
    struct xgpu_vulkan_thickness_job *thickness_job;
};
static struct
{
    uint32_t thickness_epoch;
    struct { uint32_t thickness; double thickness_seconds, thickness_landing_seconds; } statistics;
} cache;
static struct entry the_entry;
static struct xgpu_vulkan_image images[8];
static unsigned made;
static uint32_t guest[16 * 16];
static pthread_t main_thread;
static int decodes, decodes_on_main;
static double seconds(void) { return 0.0; }
static void host_logf(int level, const char *format, ...) { (void)level; (void)format; }
static const void *host_pointer(uint32_t address) { return address ? (const void *)guest : NULL; }
static unsigned long xgpu_texture_level_offset(const struct xgpu_texture_description *description, unsigned long level)
{ (void)description; (void)level; return 0; }
static unsigned long xgpu_texture_level_size(const struct xgpu_texture_description *description, unsigned long level)
{ (void)level; return description->width * description->height * 4; }
static void xgpu_texture_decode_dxt(const struct xgpu_texture_description *description, unsigned long level,
    const unsigned char *source, DWORD *destination)
{ (void)description; (void)level; (void)source; (void)destination; abort(); }
static int xgpu_texture_decode_level(const struct xgpu_texture_description *description, unsigned long level,
    const unsigned char *source, const D3DCOLOR *palette, DWORD *destination)
{
    (void)level; (void)palette;
    __atomic_add_fetch(&decodes, 1, __ATOMIC_RELAXED);
    if (pthread_equal(pthread_self(), main_thread))
        decodes_on_main++;
    memcpy(destination, source, description->width * description->height * 4);
    return 1;
}
static void xgpu_vulkan_image_destroy(struct xgpu_vulkan_image *image) { image->destroyed = 1; }
static struct xgpu_vulkan_image *xgpu_vulkan_image_create(const struct xgpu_vulkan_image_description *description)
{ (void)description; return &images[made++]; }
static void xgpu_vulkan_image_upload(struct xgpu_vulkan_image *image, int a, int b, int c, int d, int e, uint32_t width,
    uint32_t height, int f, const void *texels, uint32_t pitch, int g)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)pitch; memcpy(image->texels, texels, width * height * 4); }
static struct entry *xbox_entry_current(const struct xgpu_stream_texture_stage *stage, const D3DCOLOR **palette)
{
    (void)stage;
    *palette = NULL;
    return &the_entry;
}
""" + "%s" + r"""
static void disc(void)
{
    int x, y;

    for (y = 0; y < 16; y++)
        for (x = 0; x < 16; x++)
            guest[y * 16 + x] = (x - 8) * (x - 8) + (y - 8) * (y - 8) < 36 ? 0xffffffffu : 0;
}

int main(void)
{
    struct xgpu_stream_texture_stage stage;
    uint8_t expected[16 * 16 * 4];
    struct xgpu_vulkan_image *first;
    uint32_t epoch;

    main_thread = pthread_self();
    memset(&stage, 0, sizeof(stage));
    stage.source = XGPU_SOURCE_TEXTURE;
    stage.resource = 0x1000;
    disc();
    assert(xgpu_vulkan_thickness_bake(guest, 16, 16, expected));
    the_entry.description.width = the_entry.description.height = 16;
    the_entry.description.depth = 1;
    the_entry.address = 0x2000;
    the_entry.size = sizeof(guest);
    /* never read: nothing asked for */
    assert(!xgpu_vulkan_textures_thickness(&stage) && !the_entry.thickness_job);
    /* read: asked of the workers, none drawn meanwhile, and asked once
    however often it is looked up */
    the_entry.generation = 3;
    epoch = xgpu_vulkan_textures_thickness_epoch();
    assert(!xgpu_vulkan_textures_thickness(&stage) && the_entry.thickness_job);
    assert(!xgpu_vulkan_textures_thickness(&stage));
    assert(xgpu_vulkan_thickness_wait(10.0));
    assert(decodes == 1 && !decodes_on_main);
    /* back between frames: made (the same bytes as a bake here), the epoch
    moved on so that the streams look again, then kept */
    thickness_collect(0);
    assert(!the_entry.thickness_job && the_entry.thickness == &images[0]);
    assert(!memcmp(images[0].texels, expected, sizeof(expected)));
    assert(xgpu_vulkan_textures_thickness_epoch() != epoch);
    first = xgpu_vulkan_textures_thickness(&stage);
    assert(first == &images[0] && decodes == 1 && made == 1);
    /* read again with another palette, at the same generation: the old
    thickness destroyed, the epoch moved on, asked for again */
    epoch = xgpu_vulkan_textures_thickness_epoch();
    thickness_forget(&the_entry);
    assert(first->destroyed && !the_entry.thickness && xgpu_vulkan_textures_thickness_epoch() != epoch);
    assert(!xgpu_vulkan_textures_thickness(&stage) && the_entry.thickness_job);
    /* let go of while on its way: it comes back to no one */
    thickness_forget(&the_entry);
    assert(xgpu_vulkan_thickness_wait(10.0));
    thickness_collect(0);
    assert(made == 1 && !the_entry.thickness && decodes == 2);
    /* the texels written while it was on its way: what comes back is the
    old texels', not made; asked for again at the next look */
    assert(!xgpu_vulkan_textures_thickness(&stage) && the_entry.thickness_job);
    the_entry.generation = 4;
    assert(xgpu_vulkan_thickness_wait(10.0));
    thickness_collect(0);
    assert(made == 1 && !the_entry.thickness && !the_entry.thickness_job);
    assert(!xgpu_vulkan_textures_thickness(&stage) && the_entry.thickness_job);
    assert(xgpu_vulkan_textures_thickness_settle(10.0) == 1);
    assert(the_entry.thickness == &images[1] && the_entry.thickness_generation == 4 && decodes == 4);
    /* too large: never asked for, and not asked again at this generation */
    thickness_forget(&the_entry);
    the_entry.description.width = XGPU_VULKAN_THICKNESS_LARGEST + 1;
    assert(!xgpu_vulkan_textures_thickness(&stage) && !the_entry.thickness_job);
    assert(the_entry.thickness_failed == 4);
    return 0;
}
"""


def _lookup_source(text):
    forget = text[text.index("/* the entry's thickness destroyed, its bake on the way let go of"):
                  text.index("static uint32_t bucket_index(")]
    lookup = text[text.index("/* what a bake is made from: the first level's texels"):
                  text.index("uint32_t xgpu_vulkan_textures_thickness_epoch(void)")]
    epoch = text[text.index("uint32_t xgpu_vulkan_textures_thickness_epoch(void)"):
                 text.index("/* ---------- images */")]
    return LOOKUP_TEST % (forget + lookup + epoch)


def _lookup_run(cc, tmp_path, name, text):
    source = tmp_path / f"{name}.c"
    source.write_text(text)
    binary = tmp_path / name
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-pthread",
                    f"-I{ROOT / 'port/linux/src'}", f"-I{ROOT / 'port/linux/vulkan'}", str(source), "-o", str(binary),
                    "-lm"], check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


def test_thickness_is_baked_apart_only_from_texels_read_and_forgotten_with_its_palette(tmp_path):
    # The texture cache's lookup asks the effects' workers for a bitmap's
    # thickness and draws none until it comes back (no frame waits for a
    # bake); it lands between frames, the same bytes as a bake on the
    # render thread, moving the epoch on. Nothing is baked from texels the
    # cache never read; a thickness forgotten (another palette, the texture
    # let go of) or out of date (its texels written) while on its way is
    # never made.
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_textures.c").read_text()
    reuse = text[text.index("static struct entry *xbox_entry_find("):
                 text.index("entry = calloc(1, sizeof(*entry));")]
    assert "thickness_forget(oldest);" in reuse
    sweep = text[text.index("void xgpu_vulkan_textures_frame_end(void)"):]
    assert "thickness_collect(THICKNESS_LANDED_BYTES);" in sweep and "thickness_forget(entry);" in sweep
    run = _lookup_run(cc, tmp_path, "thickness_lookup", _lookup_source(text))
    assert run.returncode == 0, run.stdout + run.stderr


@pytest.mark.parametrize("name, original, broken", [
    # (the lookup waiting for the bake, as the render thread did before)
    ("waited", "\tthickness_request(entry, palette);\n\treturn NULL;\n",
     "\tthickness_request(entry, palette);\n\txgpu_vulkan_thickness_wait(10.0);\n\tthickness_collect(0);\n"
     "\treturn entry->thickness;\n"),
    # (a bake let go of still made when it comes back)
    ("unforgotten", "\t\tentry->thickness_job->owner = NULL;\n", ""),
    # (a bake of texels since written made all the same)
    ("stale", "\tif (job->generation != entry->generation)\n\t\treturn;\n", ""),
    # (the streams that found none never told to look again)
    ("unannounced", "\t/* (the streams that found none look again) */\n\tcache.thickness_epoch++;\n", ""),
])
def test_thickness_lookup_negative_controls(tmp_path, name, original, broken):
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_textures.c").read_text()
    assert text.count(original) == 1, name
    run = _lookup_run(cc, tmp_path, name, _lookup_source(text.replace(original, broken)))
    assert run.returncode != 0, f"{name}: the lookup's test passed with the code broken"


DEPTH_SAMPLED_TEST = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct xgpu_vulkan_image_description { uint32_t width, height, depth_or_layers; };
struct xgpu_vulkan_image { struct xgpu_vulkan_image_description description; int destroyed; };
struct stream_target
{
    struct xgpu_vulkan_image *image;
    uint32_t width, height, layers;
    int depth;
    struct xgpu_vulkan_image *depth_copy, *depth_copied_from;
    uint32_t depth_writes, depth_copied;
};
static struct { int preparing; uint32_t depth_copies; } stream;
static struct xgpu_vulkan_image made[8];
static int made_count, copies;
static const struct xgpu_vulkan_image_description *xgpu_vulkan_image_description(const struct xgpu_vulkan_image *image)
{ return &image->description; }
static void xgpu_vulkan_image_destroy(struct xgpu_vulkan_image *image) { if (image) image->destroyed = 1; }
static struct xgpu_vulkan_image *target_image_make(const struct stream_target *target, int samples)
{
    struct xgpu_vulkan_image *image = &made[made_count++];

    (void)samples;
    image->description.width = target->width;
    image->description.height = target->height;
    image->description.depth_or_layers = target->layers > 1 ? target->layers : 1;
    return image;
}
static int xgpu_vulkan_image_copy_depth(struct xgpu_vulkan_image *source, struct xgpu_vulkan_image *destination)
{
    (void)source; (void)destination;
    copies++;
    return 1;
}
""" + "%s" + r"""
int main(void)
{
    struct xgpu_vulkan_image attachment = { { 64, 32, 1 }, 0 }, other = { { 64, 32, 1 }, 0 };
    struct stream_target target;
    struct xgpu_vulkan_image *first;

    memset(&target, 0, sizeof(target));
    target.image = &attachment;
    target.width = 64;
    target.height = 32;
    target.layers = 1;
    target.depth = 1;
    /* made and copied once; sampled again with no write between: the same
    copy, not copied again */
    first = target_depth_sampled(&target);
    assert(first && first != &attachment && copies == 1);
    assert(target_depth_sampled(&target) == first && copies == 1);
    /* the depth written (a draw, a clear): copied again before it is sampled */
    target.depth_writes++;
    assert(target_depth_sampled(&target) == first && copies == 2);
    assert(target_depth_sampled(&target) == first && copies == 2);
    /* another image given the target (borrowed): copied from it */
    target.image = &other;
    assert(target_depth_sampled(&target) == first && copies == 3);
    /* the target's size changed: a new copy of the new size */
    target.width = 128;
    assert(target_depth_sampled(&target) != first && first->destroyed && copies == 4);
    /* preparation copies nothing */
    target.depth_writes++;
    stream.preparing = 1;
    assert(target_depth_sampled(&target) && copies == 4);
    /* not a depth target: none */
    target.depth = 0;
    assert(!target_depth_sampled(&target));
    return 0;
}
"""


def _depth_sampled_source(text):
    start = text.index("static struct xgpu_vulkan_image *target_depth_sampled(struct stream_target *target)\n{")
    end = text.index("\n}\n", start) + 3
    return DEPTH_SAMPLED_TEST % text[start:end]


@pytest.mark.parametrize("name, original, broken", [
    (None, None, None),
    # (the copy made once, never again after the depth is written: stale)
    ("stale", " || target->depth_copied != target->depth_writes", ""),
])
def test_depth_sampled_is_a_copy_made_again_when_written(tmp_path, name, original, broken):
    # The draws that sample the depth target they are drawn against (soft
    # effects, glows, the shadows' passes) sample a copy of it, never the
    # attachment itself (#1247), copied again only once the depth has been
    # written since; the negative control must fail.
    cc = compiler()
    if not cc:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan_stream.c").read_text()
    if name:
        assert text.count(original) == 1
        text = text.replace(original, broken)
    source = tmp_path / "depth_sampled.c"
    source.write_text(_depth_sampled_source(text))
    binary = tmp_path / "depth_sampled"
    subprocess.run([cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", str(source),
                    "-o", str(binary)], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert (run.returncode == 0) == (name is None), (name, run.stdout + run.stderr)
