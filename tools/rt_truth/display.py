"""The game's display transform, so mood.png and the game's screenshots compare pixel for pixel.

The game's frame holds values in sRGB's encoding, 1.0 paper white (docs/hdr.md, "The frame"). An SDR
picture of it (the screenshot key's PNG, and the window's 8 bits) is xgpu_hdr_sdr_pixel
(port/linux/src/xgpu_hdr.h:887): a pixel with a channel past 1 goes through xgpu_hdr_tone_map at
Faithful and a peak of 1 (xgpu_hdr.h:592, xgpu_hdr_tone_map_full), then each channel is rounded to 8 bits.
The sRGB curves are xgpu_hdr_linear and xgpu_hdr_encoded (xgpu_hdr.h:410, 417). This is the same maths,
vectorized. The truth's linear light L (exposed) becomes the frame's value encoded(L), extended past 1.
"""

import numpy as np

LUMA = np.array([0.2126, 0.7152, 0.0722], np.float32)
FAITHFUL_CLIPPED = 2.0  # XGPU_HDR_FAITHFUL_CLIPPED


def linear(v):
    """xgpu_hdr_linear: sRGB's curve inverted, extended past 1."""
    v = np.maximum(v, 0.0)
    return np.where(v <= 0.04045, v / 12.92, np.power((v + 0.055) / 1.055, 2.4))


def encoded(l):
    """xgpu_hdr_encoded: sRGB's curve, extended past 1."""
    l = np.maximum(l, 0.0)
    return np.where(l <= 0.0031308, l * 12.92, 1.055 * np.power(l, 1.0 / 2.4) - 0.055)


def _smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _shoulder(x, knee, peak):
    """xgpu_hdr_shoulder."""
    rng = peak - knee
    if rng <= 0.0:
        return np.where(x <= knee, x, peak)
    t = (x - knee) / rng
    return np.where(x <= knee, x, knee + rng * t / (1.0 + t))


def tone_map_faithful(v, peak=1.0):
    """xgpu_hdr_tone_map_full(rgb, XGPU_HDR_MODE_FAITHFUL, peak, 0) on frame values v (H x W x 3)."""
    value = np.maximum(v, 0.0)
    sdr = linear(np.minimum(value, 1.0))
    light = linear(value)
    sdr_lum = sdr @ LUMA
    lum = light @ LUMA
    value_most = value.max(-1)
    clipped = _smoothstep(1.0, FAITHFUL_CLIPPED, value_most)[..., None]
    most = light.max(-1)
    hue = np.where(most > 0, _shoulder(most, 1.0, peak) / np.maximum(most, 1e-30), 0.0)[..., None]
    bright = np.where(sdr_lum > 0, _shoulder(lum / np.maximum(sdr_lum, 1e-30), 1.0, peak), 0.0)[..., None]
    out = encoded(light * hue * (1.0 - clipped) + sdr * bright * clipped)
    keep = (lum <= sdr_lum)[..., None]  # Faithful leaves a pixel with nothing past white as it is
    return np.where(keep, value, out)


def sdr_pixels(light_rgb):
    """The truth's linear light (H x W x 3) as the game's SDR picture, uint8 RGB (xgpu_hdr_sdr_pixel)."""
    v = encoded(light_rgb.astype(np.float64))
    past = (v > 1.0).any(-1, keepdims=True)
    v = np.where(past, tone_map_faithful(v, 1.0), v)
    return np.where(v <= 0, 0, np.where(v >= 1, 255, (v * 255.0 + 0.5).astype(np.int64))).astype(np.uint8)


def picture_light(rgb8):
    """The light a picture's pixels show (its sRGB values made linear), luminance per pixel."""
    return linear(rgb8.astype(np.float64) / 255.0) @ LUMA.astype(np.float64)


def rect_mask(shape, rects):
    """True where a pixel counts: outside every rect ([x0, y0, x1, y1] in pixels, or {x, y, w, h})."""
    keep = np.ones(shape, bool)
    for r in rects or []:
        if isinstance(r, dict):
            x0, y0, x1, y1 = r["x"], r["y"], r["x"] + r["w"], r["y"] + r["h"]
        else:
            x0, y0, x1, y1 = r
        keep[int(y0):int(y1), int(x0):int(x1)] = False
    return keep


def match_exposure(truth, original_rgb8, keep=None, iters=60):
    """The one global exposure k for which the truth's SDR picture shows, on average, the light the
    original shows (mean luminance of the pictures' linear light, outside camera.json's mask_rects: the
    HUD's text). Bisection on log k (the mean rises
    with k)."""
    keep = np.ones(truth.shape[:2], bool) if keep is None else keep
    target = picture_light(original_rgb8)[keep].mean()
    lum = truth @ LUMA
    if not np.isfinite(target) or target <= 0 or lum.mean() <= 0:
        return 1.0, target
    # Subsample for speed; the mean of a large regular subsample is the image's to well under 0.1%.
    sub, ksub = truth[::2, ::2], keep[::2, ::2]
    lo, hi = np.log(1e-6), np.log(1e6)
    for _ in range(iters):
        mid = 0.5 * (lo + hi)
        m = picture_light(sdr_pixels(sub * np.exp(mid)))[ksub].mean()
        if m < target:
            lo = mid
        else:
            hi = mid
    return float(np.exp(0.5 * (lo + hi))), float(target)
