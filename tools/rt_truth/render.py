"""Renders a scene folder's truth/ (rt2-scene-format.md, "truth/"): path traced until converged."""

import fcntl
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

import mitsuba as mi
import drjit as dr

from . import display
from .scene import Scene, read_image

# A player's game on the machine: the render waits while one runs (the GPU is theirs then).
PLAYER_GAME = r"^/home/[^/]+/Applications/OpenH1/bin/openh1"


def player_playing():
    return subprocess.run(["pgrep", "-f", PLAYER_GAME], capture_output=True).returncode == 0


def wait_for_no_player(log):
    if not player_playing():
        return
    log("a player's game is running: waiting until it closes")
    while player_playing():
        time.sleep(30)
    log("the game closed: rendering again")


class GpuLock:
    """The rig's GPU lock (flock), held for the render so timing runs never share the GPU with it."""

    def __init__(self, path, log):
        self.path, self.log, self.f = path, log, None

    def __enter__(self):
        if self.path and os.path.exists(os.path.dirname(self.path)):
            self.f = open(self.path, "a")
            try:
                fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                self.log("waiting for the GPU lock %s" % self.path)
                fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *a):
        if self.f:
            fcntl.flock(self.f, fcntl.LOCK_UN)
            self.f.close()


def write_exr(path, img):
    path.parent.mkdir(parents=True, exist_ok=True)
    mi.Bitmap(np.ascontiguousarray(img, np.float32)).write(str(path))


# Noise is judged relative to a pixel's light, or, where the picture shows it darker, relative to this
# much light on the display (linear, 1 = paper white; sRGB code 25 of 255): darker noise is not seen.
DISPLAY_FLOOR = 0.01


def noise_stats(s1, s2, n, floor):
    """Per-pixel standard error of the mean luminance over n independent passes, relative to the pixel's
    luminance, or to `floor` (the same units) where the pixel is darker than that."""
    mean = s1 / n
    var = np.maximum(s2 / n - mean * mean, 0.0) * n / max(n - 1, 1)
    se = np.sqrt(var / n)
    floor = max(floor, 1e-12)
    rel = se / np.maximum(mean, floor)
    # The image mean's error: from the passes' image means.
    return {"median_rel": float(np.median(rel)), "p95_rel": float(np.percentile(rel, 95)),
            "p99_rel": float(np.percentile(rel, 99)), "rel_floor": floor}


def render_scene(folder, out=None, spp_pass=16, target=0.01, max_spp=65536, max_minutes=60.0,
                 min_passes=4, gpu_lock=os.path.expanduser("~/goat-rig/locks/gpu.lock"),
                 log=print):
    folder = Path(folder)
    out = Path(out) if out else folder / "truth"
    from . import VARIANT as variant
    sc = Scene(folder)
    t0 = time.time()
    groups = [g for g in Scene.GROUPS if sc.group_has_light(g)] or ["static"]
    log("%s: %d triangles, groups %s" % (folder.name, sc.n_tris, groups))
    scenes = {g: mi.load_dict(sc.build(g)) for g in groups}
    grey = [m["index"] for m in sc.material_summaries.values() if "placeholder" in m.get("albedo", "")]
    if grey:
        sc.placeholders.append("%d of %d materials without a base map: grey 0.5 albedo" % (
            len(grey), len(sc.material_summaries)))
    integ = {d: mi.load_dict({"type": "path", "max_depth": d, "rr_depth": 5}) for d in (1, 2, -1)}
    H, W = int(sc.camera["height"]), int(sc.camera["width"])
    acc = {g: {k: np.zeros((H, W, 3)) for k in ("full", "d2", "d1")} for g in groups}
    s1 = np.zeros((H, W))
    s2 = np.zeros((H, W))
    n = 0
    stats = {}
    orig_path = folder / "original.png"
    original, keep = None, None
    if orig_path.exists():
        original = (read_image(orig_path)[:, :, :3] * 255.0 + 0.5).astype(np.uint8)
        if original.shape[:2] != (H, W):
            from PIL import Image
            original = np.asarray(Image.fromarray(original).resize((W, H), Image.BILINEAR))
        keep = display.rect_mask((H, W), sc.camera.get("mask_rects"))
    log("scene built in %.1f s" % (time.time() - t0))
    t_render = time.time()
    with GpuLock(gpu_lock, log):
        while True:
            wait_for_no_player(log)
            total = np.zeros((H, W, 3))
            for g in groups:
                imgs = {}
                for key, depth in (("full", -1), ("d2", 2), ("d1", 1)):
                    img = mi.render(scenes[g], integrator=integ[depth], spp=spp_pass, seed=n)
                    imgs[key] = np.asarray(img, np.float64)
                for k in imgs:
                    acc[g][k] += imgs[k]
                total += imgs["full"]
            y = total @ display.LUMA.astype(np.float64)
            s1 += y
            s2 += y * y
            n += 1
            el = (time.time() - t_render) / 60.0
            if n >= min_passes and (n == min_passes or n % 8 == 0 or n * spp_pass >= max_spp):
                # The floor follows the exposure the picture will have (or 5% of the mean light without an
                # original).
                if original is not None:
                    mean_now = sum(acc[g]["full"] for g in groups) / n
                    k_now, _ = display.match_exposure(mean_now, original, keep, iters=40)
                    floor = DISPLAY_FLOOR / k_now
                else:
                    floor = 0.05 * (s1 / n).mean()
                stats = noise_stats(s1, s2, n, floor)
                done = stats["median_rel"] <= target and stats["p95_rel"] <= 3 * target
                if True:
                    log("pass %d (%d spp): median noise %.3f%%, 95th %.3f%%, %.1f min" % (
                        n, n * spp_pass, 100 * stats["median_rel"], 100 * stats["p95_rel"], el))
                if done or n * spp_pass >= max_spp or el >= max_minutes:
                    stats["converged"] = bool(done)
                    break
    spp = n * spp_pass
    mean = {g: {k: v / n for k, v in acc[g].items()} for g in groups}
    full = sum(mean[g]["full"] for g in groups)
    emission = sum(mean[g]["d1"] for g in groups)
    direct = sum(mean[g]["d2"] - mean[g]["d1"] for g in groups)
    indirect = sum(mean[g]["full"] - mean[g]["d2"] for g in groups)

    # The one global exposure: the truth's picture as bright on average as the original's.
    if original is not None:
        k, target_light = display.match_exposure(full, original, keep)
    else:
        k, target_light = 1.0, None
    out.mkdir(parents=True, exist_ok=True)
    write_exr(out / "truth.exr", k * full)
    parts = out / "truth_parts"
    write_exr(parts / "direct.exr", k * direct)
    write_exr(parts / "indirect.exr", k * indirect)
    write_exr(parts / "emission.exr", k * emission)
    zero = np.zeros((H, W, 3))
    write_exr(parts / "static.exr", k * mean["static"]["full"] if "static" in mean else zero)
    write_exr(parts / "direct_flashlight.exr",
              k * (mean["flashlight"]["d2"] - mean["flashlight"]["d1"]) if "flashlight" in mean else zero)
    mood = display.sdr_pixels(k * full)
    from PIL import Image
    Image.fromarray(mood).save(out / "mood.png")
    compare(original, mood, out / "compare.png", k, sc.placeholders)

    minutes = (time.time() - t_render) / 60.0
    info = {
        "scene": folder.name, "variant": variant, "spp": spp, "passes": n, "spp_per_pass": spp_pass,
        "render_minutes": round(minutes, 2), "noise": stats, "noise_target_median": target,
        "exposure": k, "original_mean_light": target_light,
        "groups": groups, "placeholders": sc.placeholders, "notes": sc.notes,
        "materials": sc.material_summaries, "lights": getattr(sc, "light_summaries", {}),
    }
    (out / "info.json").write_text(json.dumps(info, indent=1, default=str))
    lines = [
        "spp %d (%d passes of %d), %s" % (spp, n, spp_pass, variant),
        "time %.1f min" % minutes,
        "noise (standard error of the mean luminance, per pixel, relative; floor: light shown at %g of paper white):" % DISPLAY_FLOOR,
        "  median %.3f%%  95th %.3f%%  99th %.3f%%  converged %s (target median %.2f%%, 95th %.2f%%)" % (
            100 * stats.get("median_rel", float("nan")), 100 * stats.get("p95_rel", float("nan")),
            100 * stats.get("p99_rel", float("nan")), stats.get("converged"), 100 * target, 300 * target),
        "exposure %.6g (applied to truth.exr and its parts; 1 without original.png)" % k,
        "groups %s" % " ".join(groups),
    ] + ["PLACEHOLDER: %s" % p for p in sc.placeholders] + ["note: %s" % x for x in sc.notes]
    (out / "samples.txt").write_text("\n".join(lines) + "\n")
    log("\n".join(lines))
    return info


def _font(size):
    from PIL import ImageFont
    for f in ("/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default(size=size)


def compare(original, mood, path, k, placeholders):
    """The original on the left and the truth on the right, labeled."""
    from PIL import Image, ImageDraw
    H, W = mood.shape[:2]
    bar = max(36, H // 22)
    canvas = Image.new("RGB", (2 * W, H + bar), (16, 16, 16))
    if original is not None:
        canvas.paste(Image.fromarray(original), (0, bar))
    canvas.paste(Image.fromarray(mood), (W, bar))
    d = ImageDraw.Draw(canvas)
    f = _font(int(bar * 0.6))
    d.text((12, bar * 0.18), "original (game)" if original is not None else "no original.png", fill=(255, 255, 255), font=f)
    label = "truth (path traced, exposure %.3g)" % k
    if placeholders:
        label += "  PLACEHOLDER LIGHTS/MATERIALS"
    d.text((W + 12, bar * 0.18), label, fill=(255, 255, 255), font=f)
    canvas.save(path)
