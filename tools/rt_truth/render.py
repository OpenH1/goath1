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
from .integrator import render_pass

# A player's game on the machine: the render waits while one runs (the GPU is theirs then).
PLAYER_GAME = r"^/home/[^/]+/Applications/OpenH1[^/ ]*/bin/openh1"  # any install: OpenH1, -Beta, -RT


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
    """The rig's GPU lock, taken shared (as `goat-rig run --shared`): a truth render is no timing
    measurement, so it runs beside picture runs and only waits out an exclusive (timing) holder."""

    def __init__(self, path, log, exclusive=False):
        self.path, self.log, self.f, self.mode = path, log, None, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH

    def __enter__(self):
        if self.path and os.path.exists(os.path.dirname(self.path)):
            self.f = open(self.path, "a")
            try:
                fcntl.flock(self.f, self.mode | fcntl.LOCK_NB)
            except BlockingIOError:
                self.log("waiting for the GPU lock %s" % self.path)
                fcntl.flock(self.f, self.mode)
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


def fit_static(samples, keep, static_means, gamma, flip):
    """The static groups' scales: least squares (non-negative) of the game's static light at each lit BSP
    pixel, albedo x lightmap (the lightmap decoded by `gamma`), against each group's truth radiance.
    Returns (scales, R^2, target image, fitted image, mask)."""
    lm = samples["lightmap_flipped" if flip else "lightmap"]
    lm = np.maximum(lm, 0.0) ** gamma
    alb = samples["albedo"]
    m = samples["valid"] & keep & (alb @ display.LUMA > 0.02)
    y_img = (alb * lm) @ display.LUMA
    X_img = [g @ display.LUMA for g in static_means]
    y = y_img[m]
    X = np.stack([x[m] for x in X_img], 1)
    active = list(range(X.shape[1]))
    k = np.zeros(X.shape[1])
    while active:  # non-negative least squares by dropping negative terms
        sol, *_ = np.linalg.lstsq(X[:, active], y, rcond=None)
        if (sol >= 0).all():
            k[active] = sol
            break
        active = [a for a, v in zip(active, sol) if v > 0]
    fit = X @ k
    ss = ((y - y.mean()) ** 2).sum()
    r2 = 1 - ((y - fit) ** 2).sum() / ss if ss > 0 else float("nan")
    fit_img = sum(kk * x for kk, x in zip(k, X_img))
    return k, float(r2), y_img, fit_img, m


def best_fit(samples, keep, static_means, log):
    """The fit with the lightmap pages as stored (linear, the format says), v as stored; two other readings
    are logged beside it."""
    best = None
    # The pages as stored with v as stored are the reading (albedo x lightmap at each pixel reproduces the
    # original's look, checked on a10_interior); the others are logged for comparison only.
    for gamma, flip in ((1.0, False), (2.2, False), (1.0, True)):
        if True:
            r = fit_static(samples, keep, static_means, gamma, flip)
            log("  lightmap fit, pages ^%g, v %s: scales %s, R^2 %.3f" % (
                gamma, "flipped" if flip else "as stored", np.round(r[0], 5).tolist(), r[1]))
            if best is None:
                best = (r, gamma, flip)
    return best


def render_scene(folder, out=None, spp_pass=8, target=0.01, max_spp=65536, max_minutes=60.0,
                 min_passes=4, gpu_lock=os.path.expanduser("~/goat-rig/locks/gpu.lock"),
                 log=print, flashlight_scale=None, flashlight_emitter=None, static_mode="lightmap",
                 object_static="halo", emitters_file=None):
    folder = Path(folder)
    out = Path(out) if out else folder / "truth"
    from . import VARIANT as variant
    sc = Scene(folder, flashlight_scale, flashlight_emitter=flashlight_emitter, static_mode=static_mode,
               object_static=object_static, emitters_file=emitters_file)
    t0 = time.time()
    groups = [g for g in sc.groups if sc.group_has_light(g)]
    statics = [g for g in groups if g in sc.static_groups]
    log("%s: %d triangles, groups %s" % (folder.name, sc.n_tris, groups))
    scenes = {g: mi.load_dict(sc.build(g)) for g in groups}
    samples = sc.lightmap_samples(scenes[groups[-1]])
    # The same light on white Lambertian surfaces (same normal maps and cutouts): its direct part is the
    # demodulated irradiance the real-time comparison uses.
    wsc = Scene(folder, flashlight_scale, white=True, flashlight_emitter=flashlight_emitter, static_mode=static_mode,
                object_static=object_static, emitters_file=emitters_file)
    whites = {g: mi.load_dict(wsc.build(g)) for g in groups}
    acc_w = {g: np.zeros((len(samples["valid"]), len(samples["valid"][0]), 3)) for g in groups}
    grey = [m["index"] for m in sc.material_summaries.values() if "placeholder" in m.get("albedo", "")]
    if grey:
        sc.placeholders.append("%d of %d materials without a base map: grey 0.5 albedo" % (
            len(grey), len(sc.material_summaries)))
    H, W = int(sc.camera["height"]), int(sc.camera["width"])
    acc = {g: {k: np.zeros((H, W, 3)) for k in ("full", "d2", "d1")} for g in groups}
    s1 = {g: np.zeros((H, W)) for g in groups}
    s2 = {g: np.zeros((H, W)) for g in groups}
    n = 0
    stats = {}
    # The world only: without the HUD's text (mask_rects) and the first-person weapon and hands.
    keep = display.rect_mask((H, W), sc.camera.get("mask_rects"))
    fp = sc.first_person_mask(scenes[[g for g in groups if g != "flashlight"][0]] if len(groups) > 1 or groups[0] != "flashlight" else mi.load_dict(sc.build("ambient")))
    keep &= ~fp
    use_fit = sc.static_mode == "fit" and bool(statics) and samples["valid"].any()
    log("fit and comparison on %.1f%% of the pixels (first-person %.1f%%), lightmapped BSP %.1f%%" % (
        100 * keep.mean(), 100 * fp.mean(), 100 * (samples["valid"] & keep).mean()))
    original = None
    orig_path = folder / "original.png"
    if orig_path.exists():
        original = (read_image(orig_path)[:, :, :3] * 255.0 + 0.5).astype(np.uint8)
        if original.shape[:2] != (H, W):
            from PIL import Image
            original = np.asarray(Image.fromarray(original).resize((W, H), Image.BILINEAR))
    log("scene built in %.1f s" % (time.time() - t0))
    scales = {g: 1.0 for g in groups}
    # Resume: the sums of earlier chunks of this render (same groups, size and samples a pass), so a long
    # render can run in pieces and give the GPU back between them.
    state_path = out / "_state.npz"
    if state_path.exists():
        st = np.load(state_path, allow_pickle=True)
        if list(st["groups"]) == groups and int(st["spp_pass"]) == spp_pass and st["s1_" + groups[0]].shape == (H, W):
            n = int(st["n"])
            for g in groups:
                for k in acc[g]:
                    acc[g][k] = st["acc_%s_%s" % (g, k)]
                acc_w[g] = st["accw_" + g]
                s1[g], s2[g] = st["s1_" + g], st["s2_" + g]
            log("resumed at pass %d (%d spp)" % (n, n * spp_pass))
    n0 = n
    t_render = time.time()
    with GpuLock(gpu_lock, log):
        while True:
            wait_for_no_player(log)
            for g in groups:
                p = {k: v.astype(np.float64) for k, v in render_pass(scenes[g], spp_pass, n, parts=True).items()}
                imgs = {"d1": p["emission"], "d2": p["emission"] + p["direct"]}
                imgs["full"] = imgs["d2"] + p["indirect"]
                for key, img in imgs.items():
                    acc[g][key] += img
                acc_w[g] += render_pass(whites[g], spp_pass, n, parts=True, max_depth=2)["direct"]
                y = imgs["full"] @ display.LUMA.astype(np.float64)
                s1[g] += y
                s2[g] += y * y
            n += 1
            el = (time.time() - t_render) / 60.0
            if n >= min_passes and (n == min_passes or n % 8 == 0 or n * spp_pass >= max_spp or el >= max_minutes):
                if use_fit:
                    (k, *_), _, _ = best_fit(samples, keep, [acc[g]["full"] / n for g in statics], lambda *a: None)
                    scales.update(zip(statics, k))
                # Noise of the whole picture: the groups are independent renders, so variances add.
                mean = sum(scales[g] * s1[g] / n for g in groups)
                var = sum(scales[g] ** 2 * np.maximum(s2[g] / n - (s1[g] / n) ** 2, 0) for g in groups)
                stats = noise_stats(mean * n, (var + mean * mean) * n, n, DISPLAY_FLOOR)
                done = stats["median_rel"] <= target and stats["p95_rel"] <= 3 * target
                log("pass %d (%d spp): median noise %.3f%%, 95th %.3f%%, %.1f min" % (
                    n, n * spp_pass, 100 * stats["median_rel"], 100 * stats["p95_rel"], el))
                if done or n * spp_pass >= max_spp or el >= max_minutes:
                    stats["converged"] = bool(done)
                    break
    spp = n * spp_pass
    out.mkdir(parents=True, exist_ok=True)
    save = {"groups": np.array(groups), "spp_pass": spp_pass, "n": n}
    for g in groups:
        for k in acc[g]:
            save["acc_%s_%s" % (g, k)] = acc[g][k].astype(np.float32)
        save["accw_" + g] = acc_w[g].astype(np.float32)
        save["s1_" + g], save["s2_" + g] = s1[g], s2[g]
    np.savez(state_path, **save)
    mean = {g: {k: v / n for k, v in acc[g].items()} for g in groups}
    out.mkdir(parents=True, exist_ok=True)
    fit_lines = []
    if use_fit:
        log("static fit against the lightmap:")
        (k, r2, y_img, fit_img, m), gamma, flip = best_fit(samples, keep, [mean[g]["full"] for g in statics], log)
        scales.update(zip(statics, k))
        fit_lines = ["static fit (least squares against albedo x lightmap, %d lit BSP pixels; pages ^%g, v %s): R^2 %.3f" % (
            int(m.sum()), gamma, "flipped" if flip else "as stored", r2)] + [
            "  scale %s %.6g" % (g, scales[g]) for g in statics]
        write_exr(out / "static_fit_residual.exr", np.where(m, y_img - fit_img, 0.0)[..., None].repeat(3, -1))
        residual_png(y_img, fit_img, m, out / "static_fit_residual.png")
    elif sc.static_mode == "emitters":
        # the bake against the light traced from the emitter file, at the lit BSP pixels (no fit: scale 1)
        m = samples["valid"] & keep & (samples["albedo"] @ display.LUMA > 0.02)
        y_img = (samples["albedo"] * np.maximum(samples["lightmap"], 0.0) ** 2.2) @ display.LUMA
        fit_img = mean["solved"]["full"] @ display.LUMA
        ss = ((y_img[m] - y_img[m].mean()) ** 2).sum()
        r2v = float(1 - ((y_img[m] - fit_img[m]) ** 2).sum() / ss) if ss > 0 else float("nan")
        fit_lines = ["static light: traced from %s; against the bake (albedo x lightmap^2.2) at %d lit BSP pixels: "
                     "R^2 %.3f, traced / bake mean light %.3f" % (Path(emitters_file).name, int(m.sum()), r2v,
                                                                   fit_img[m].mean() / max(y_img[m].mean(), 1e-12))]
        write_exr(out / "static_fit_residual.exr", np.where(m, y_img - fit_img, 0.0)[..., None].repeat(3, -1))
        residual_png(y_img, fit_img, m, out / "static_fit_residual.png")
        bake = display.sdr_pixels(np.where(m[..., None], samples["albedo"] * np.maximum(samples["lightmap"], 0.0) ** 2.2, 0.0))
        from PIL import Image
        Image.fromarray(bake).save(out / "bake_bsp.png")
    else:
        fit_lines = (["static light: Halo's lightmap (albedo x lightmap^2.2 emitted by the BSP), scale 1"]
                     if sc.static_mode == "lightmap" else
                     ["static fit: none (no lightmapped BSP pixels or no static light); static scales 1"])
    if "flashlight" in mean:
        for k in mean["flashlight"]:
            mean["flashlight"][k] = np.where(fp[..., None], 0.0, mean["flashlight"][k])
    w = {g: scales[g] for g in groups}
    full = sum(w[g] * mean[g]["full"] for g in groups)
    emission = sum(w[g] * mean[g]["d1"] for g in groups)
    direct = sum(w[g] * (mean[g]["d2"] - mean[g]["d1"]) for g in groups)
    indirect = sum(w[g] * (mean[g]["full"] - mean[g]["d2"]) for g in groups)
    zero = np.zeros((H, W, 3))
    static = sum((w[g] * mean[g]["full"] for g in statics), zero)
    write_exr(out / "truth.exr", full)
    parts = out / "truth_parts"
    write_exr(parts / "direct.exr", direct)
    write_exr(parts / "indirect.exr", indirect)
    write_exr(parts / "emission.exr", emission)
    write_exr(parts / "static.exr", static)
    for g in groups:  # each light group's whole light, to see what one source adds
        write_exr(parts / ("group_%s.exr" % g), w[g] * mean[g]["full"])
    write_exr(parts / "direct_flashlight.exr",
              mean["flashlight"]["d2"] - mean["flashlight"]["d1"] if "flashlight" in mean else zero)
    irr = {g: acc_w[g] / n for g in groups}
    if "flashlight" in irr:
        irr["flashlight"] = np.where(fp[..., None], 0.0, irr["flashlight"])
    write_exr(parts / "irradiance_direct.exr", sum(w[g] * irr[g] for g in groups))
    write_exr(parts / "irradiance_direct_static.exr", sum((w[g] * irr[g] for g in statics), zero))
    write_exr(parts / "irradiance_direct_flashlight.exr", irr.get("flashlight", zero))
    save_mask(out, keep)
    mood = display.sdr_pixels(full)
    from PIL import Image
    Image.fromarray(mood).save(out / "mood.png")
    ratio = None
    if original is not None:
        ratio = float(display.picture_light(mood)[keep].mean() / max(display.picture_light(original)[keep].mean(), 1e-12))
    compare(original, mood, out / "compare.png", sc.placeholders)

    minutes = (time.time() - t_render) / 60.0
    info = {
        "scene": folder.name, "variant": variant, "spp": spp, "passes": n, "spp_per_pass": spp_pass,
        "render_minutes": round(minutes, 2), "noise": stats, "noise_target_median": target,
        "scales": scales, "truth_over_original_mean_light": ratio,
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
        "groups %s" % " ".join(groups),
    ] + fit_lines + [
        "dynamic lights in the game's own units (color and attenuation decoded ^2.2), scale 1" + (
            "; the flashlight physical (1/d^2) with power x %g" % flashlight_scale if flashlight_scale is not None else ""),
        "truth / original mean light (world pixels): %s" % (("%.3f" % ratio) if ratio else "n/a"),
    ] + ["PLACEHOLDER: %s" % p for p in sc.placeholders] + ["assumption: %s" % x for x in sc.notes]
    (out / "samples.txt").write_text("\n".join(lines) + "\n")
    log("\n".join(lines))
    return info


def save_mask(out, keep):
    """truth/mask.png: white where a comparison counts (not the HUD's mask_rects, not the first-person
    weapon and hands); compare.py reads it."""
    from PIL import Image
    out.mkdir(parents=True, exist_ok=True)
    Image.fromarray((keep * 255).astype(np.uint8)).save(out / "mask.png")


def residual_png(y_img, fit_img, m, path):
    """The fit's residual: grey where the lightmap and the fit agree, white where the lightmap is brighter
    (light the truth lacks: a missing emitter), black where it is darker; outside the fit, mid-blue-free
    flat grey with a hatch so it reads in black and white too."""
    from PIL import Image
    r = np.where(m, y_img - fit_img, 0.0)
    scale = np.percentile(np.abs(r[m]), 98) + 1e-9 if m.any() else 1.0
    v = np.clip(0.5 + 0.5 * r / scale, 0, 1)
    img = (v * 255).astype(np.uint8)
    hatch = ((np.add.outer(np.arange(m.shape[0]), np.arange(m.shape[1])) // 4) % 2).astype(bool)
    img = np.where(m, img, np.where(hatch, 100, 150)).astype(np.uint8)
    Image.fromarray(img).save(path)


def _font(size):
    from PIL import ImageFont
    for f in ("/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
              "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default(size=size)


def compare(original, mood, path, placeholders):
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
    label = "truth (path traced)"
    if any("lights.json" in p for p in placeholders):
        label += "  PLACEHOLDER LIGHTS"
    grey = [p for p in placeholders if "without a base map" in p]
    if grey:
        label += "  (%s)" % grey[0].split(":")[0]
    d.text((W + 12, bar * 0.18), label, fill=(255, 255, 255), font=f)
    canvas.save(path)


def render_flashlight_direct(folder, out=None, flashlight_scale=None, flashlight_emitter=None, centers=False, spp_pass=8, target=0.002, max_spp=4096,
                             max_minutes=30.0, gpu_lock=os.path.expanduser("~/goat-rig/locks/gpu.lock"), log=print,
                             group="flashlight", light_index=None, light_scale=None):
    """Only a group's one-bounce light (truth_parts/direct_<name>.exr and its irradiance), converged:
    the flashlight, or the other dynamic lights (all, or the one at light_index), for comparing the
    real-time lights before the whole truth. light_scale: those lights in the physical model (rt2-
    calibration.json's: power x k, 1/d^2 x (1-(d/r)^4)^2) with k = light_scale."""
    folder = Path(folder)
    out = Path(out) if out else folder / "truth"
    name = group if light_index is None else "%s_%d" % (group, light_index)

    def make(**kw):
        s_ = Scene(folder, flashlight_scale, flashlight_emitter=flashlight_emitter, **kw)
        s_.light_filter = light_index
        s_.dynamic_scale = light_scale
        return s_
    sc = make()
    if not sc.lights_of(group):
        raise SystemExit("%s: no %s light in lights.json" % (folder, group))
    scene = mi.load_dict(sc.build(group))
    white = mi.load_dict(make(white=True).build(group))
    flat = mi.load_dict(make(white=True, bump=False).build(group))
    acc_f = 0.0
    H, W = int(sc.camera["height"]), int(sc.camera["width"])
    acc_w = np.zeros((H, W, 3))
    full_scene = mi.load_dict(sc.build("ambient"))
    fp = sc.first_person_mask(full_scene)
    del full_scene
    s1, s2, acc, n = np.zeros((H, W)), np.zeros((H, W)), np.zeros((H, W, 3)), 0
    acc_i = {}
    t0 = time.time()
    stats = {}
    with GpuLock(gpu_lock, log):
        while True:
            wait_for_no_player(log)
            pp = render_pass(scene, spp_pass, n, parts=True)
            img = pp["direct"].astype(np.float64)
            acc += img
            for k in ("indirect", "indirect1", "indirect_demod", "indirect1_demod"):
                acc_i[k] = acc_i.get(k, 0.0) + pp[k]
            acc_w += render_pass(white, spp_pass, n, parts=True, centers=centers)["direct"]
            acc_f = acc_f + render_pass(flat, spp_pass, n, parts=True, max_depth=2, centers=centers)["direct"]
            y = img @ display.LUMA.astype(np.float64)
            s1 += y
            s2 += y * y
            n += 1
            el = (time.time() - t0) / 60.0
            if n >= 4 and (n % 8 == 0 or n * spp_pass >= max_spp):
                stats = noise_stats(s1, s2, n, DISPLAY_FLOOR)
                done = stats["median_rel"] <= target and stats["p95_rel"] <= 3 * target
                log("pass %d (%d spp): median noise %.3f%%, 95th %.3f%%, %.1f min" % (
                    n, n * spp_pass, 100 * stats["median_rel"], 100 * stats["p95_rel"], el))
                if done or n * spp_pass >= max_spp or el >= max_minutes:
                    stats["converged"] = bool(done)
                    break
    write_exr(out / "truth_parts" / ("direct_%s.exr" % name), np.where(fp[..., None], 0.0, acc / n))
    write_exr(out / "truth_parts" / ("irradiance_direct_%s.exr" % name), np.where(fp[..., None], 0.0, acc_w / n))
    # The same with the geometric (interpolated vertex) normal, no normal maps.
    write_exr(out / "truth_parts" / ("irradiance_direct_%s_geometric.exr" % name), np.where(fp[..., None], 0.0, acc_f / n))
    # The light after one or more bounces (all bounces, Russian roulette from the fifth), and after exactly
    # one; the irradiance_ ones divided by the first hit's diffuse albedo (the light arriving there).
    for k, fname in (("indirect", "indirect_%s"), ("indirect1", "indirect1_%s"),
                     ("indirect_demod", "irradiance_indirect_%s"), ("indirect1_demod", "irradiance_indirect1_%s")):
        write_exr(out / "truth_parts" / ((fname % name) + ".exr"), np.where(fp[..., None], 0.0, acc_i[k] / n))
    save_mask(out, display.rect_mask((H, W), sc.camera.get("mask_rects")) & ~fp)
    lines = ["direct_" + name + ": spp %d, %.1f min, noise median %.3f%% 95th %.3f%% (floor %g), converged %s" % (
        n * spp_pass, (time.time() - t0) / 60, 100 * stats["median_rel"], 100 * stats["p95_rel"], DISPLAY_FLOOR,
        stats.get("converged")),
        "flashlight: %s" % json.dumps(list(sc.light_summaries.values()))] + ["assumption: %s" % x for x in sc.notes]
    (out / ("direct_%s.txt" % name)).write_text("\n".join(lines) + "\n")
    log("\n".join(lines))
