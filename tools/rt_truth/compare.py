"""A real-time render against the truth, on demodulated irradiance (the light arriving at each surface,
before its albedo, with the bump-mapped normal), in the game's light units on both sides.

compare(scene_folder, realtime_exr, out_dir=None, part="direct_flashlight") -> dict:
  tile16_bias  over 16x16-pixel tiles, t = mean_rt / mean_truth - 1 of each kept tile's luminance:
               {"mean": signed mean of t, "max_abs": max |t|, "p95_abs": 95th percentile of |t|,
                "global": the ratio of the kept pixels' sums less 1, "tiles": tiles kept}.
               A tile is kept when at least half its pixels are kept and its truth mean is at least
               DARK_TILE (1%) of the 99th-percentile tile mean (the dark floor: tiles the light barely
               reaches carry no meaningful ratio).
  rel_mse      the mean over kept pixels of (rt - truth)^2 / (truth^2 + eps), luminance, with
               eps = (REL_EPS x the lit kept pixels' mean truth)^2, REL_EPS = 0.01 (lit: over 1% of the
               99.9th percentile).
  signed_png   per-pixel signed relative error, mid-grey 0, lighter = the real-time render brighter,
               darker = dimmer, full scale at +-50%; left-out pixels hatched (reads in black and white).
  mask_png     white = kept.
  kept_pixels, notes.
Kept pixels: outside camera.json's mask_rects (the HUD's text), not the first-person weapon and hands,
inside the real-time side's mask.png when it gives one, and finite on both sides.
"""

import json
from pathlib import Path

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722])
TILE = 16
DARK_TILE = 0.01
REL_EPS = 0.01


def read_exr(path):
    import mitsuba as mi
    a = np.asarray(mi.Bitmap(str(path)), np.float64)
    return a[..., :3] if a.ndim == 3 else np.repeat(a[..., None], 3, -1)


def world_mask(folder, H, W):
    """Kept pixels by the scene alone: the truth's saved mask, else computed (mask_rects, first person)."""
    folder = Path(folder)
    saved = folder / "truth" / "mask.png"
    if saved.exists():
        from PIL import Image
        m = np.asarray(Image.open(saved).convert("L")) > 127
        if m.shape == (H, W):
            return m, "truth/mask.png"
    import mitsuba as mi
    from .scene import Scene
    from . import display
    sc = Scene(folder)
    keep = display.rect_mask((H, W), sc.camera.get("mask_rects"))
    keep &= ~sc.first_person_mask(mi.load_dict(sc.build("ambient")))
    return keep, "mask_rects and first-person objects"


def _hatch(shape):
    return ((np.add.outer(np.arange(shape[0]), np.arange(shape[1])) // 4) % 2).astype(bool)


def compare(scene_folder, realtime_exr, out_dir=None, part="direct_flashlight", quantity="irradiance"):
    """quantity "radiance" compares the finished light (albedo included) instead, for real-time pictures
    that are not demodulated."""
    from PIL import Image
    folder = Path(scene_folder)
    notes = []
    tpath = folder / "truth" / "truth_parts" / ("irradiance_%s.exr" % part)
    if quantity == "radiance":
        tpath = folder / "truth" / "truth_parts" / ("%s.exr" % part)
    elif not tpath.exists():
        tpath = folder / "truth" / "truth_parts" / ("%s.exr" % part)
        notes.append("no irradiance render of %s: compared on radiance (albedo not removed)" % part)
    truth = read_exr(tpath)
    rt = read_exr(realtime_exr)
    H, W = truth.shape[:2]
    if rt.shape[:2] != (H, W):
        raise ValueError("real-time %s is %s, the truth %s" % (realtime_exr, rt.shape[:2], (H, W)))
    keep, why = world_mask(folder, H, W)
    rt_mask = Path(realtime_exr).parent / "mask.png"
    if rt_mask.exists():
        m = np.asarray(Image.open(rt_mask).convert("L")) > 127
        if m.shape == (H, W):
            keep &= m
            why += " and the real-time mask.png"
    yt, yr = truth @ LUMA, rt @ LUMA
    keep &= np.isfinite(yt) & np.isfinite(yr)
    notes.append("kept: " + why)
    # Tiles.
    th, tw = H // TILE, W // TILE
    crop = (slice(0, th * TILE), slice(0, tw * TILE))
    k = keep[crop].reshape(th, TILE, tw, TILE).astype(float)
    st = (np.where(keep, yt, 0)[crop]).reshape(th, TILE, tw, TILE).sum((1, 3))
    sr = (np.where(keep, yr, 0)[crop]).reshape(th, TILE, tw, TILE).sum((1, 3))
    kn = k.sum((1, 3))
    mt = st / np.maximum(kn, 1)
    half = kn >= TILE * TILE / 2
    floor = DARK_TILE * (np.percentile(mt[half], 99) if half.any() else 0)
    tiles = half & (mt > max(floor, 1e-12))
    t = sr[tiles] / st[tiles] - 1
    bias = {"mean": float(t.mean()) if t.size else None, "max_abs": float(np.abs(t).max()) if t.size else None,
            "p95_abs": float(np.percentile(np.abs(t), 95)) if t.size else None,
            "global": float(yr[keep].sum() / max(yt[keep].sum(), 1e-30) - 1), "tiles": int(tiles.sum()),
            "dark_floor": float(floor)}
    # (eps from the lit pixels' mean: a light that reaches a few percent of the picture would otherwise get
    # an eps near zero and a relMSE of its dark pixels' noise)
    lit = keep & (yt > 0.01 * (np.percentile(yt[keep], 99.9) if keep.any() else 0))
    eps = (REL_EPS * yt[lit].mean()) ** 2 if lit.any() else 1e-12
    rel_mse = float(np.mean((yr[keep] - yt[keep]) ** 2 / (yt[keep] ** 2 + eps))) if keep.any() else None
    r = {"part": part, "quantity": quantity, "truth": str(tpath), "realtime": str(realtime_exr), "tile16_bias": bias,
         "rel_mse": rel_mse, "rel_eps": eps, "kept_pixels": int(keep.sum()), "notes": notes}
    if out_dir:
        out = Path(out_dir)
        out.mkdir(parents=True, exist_ok=True)
        rel = np.where(keep, (yr - yt) / np.maximum(yt, np.sqrt(eps)), 0.0)
        v = np.clip(0.5 + rel, 0, 1)  # +-50% full scale
        img = np.where(keep, v * 255, np.where(_hatch(keep.shape), 90, 160)).astype(np.uint8)
        Image.fromarray(img).save(out / "signed_error.png")
        Image.fromarray((keep * 255).astype(np.uint8)).save(out / "mask.png")
        tile_img = np.full((th, tw), 128, np.uint8)
        tile_img[tiles] = np.clip(128 + 255 * t, 0, 255).astype(np.uint8)  # +-50% full scale
        Image.fromarray(np.kron(tile_img, np.ones((TILE, TILE), np.uint8))).save(out / "tile_bias.png")
        r.update(signed_png=str(out / "signed_error.png"), mask_png=str(out / "mask.png"),
                 tile_png=str(out / "tile_bias.png"))
        (out / "compare.json").write_text(json.dumps(r, indent=1))
    return r
