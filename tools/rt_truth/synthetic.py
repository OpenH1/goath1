"""The synthetic proof scene and its checks.

A box room; a spot light with a gel shining at the corner where the floor meets the far wall; a cube
between them whose shadow falls across that corner; a glowing panel. The check renders the spot's direct
light alone and holds it, pixel by pixel, to the analytic answer computed here independently in numpy
(albedo * power * angular * gel * cos / d^2, its shadow by a ray-box test): the shadow bends at the corner,
the gel projects, and the falloff is inverse-square.
"""

import json
import math
from pathlib import Path

import numpy as np

from .scene import write_ply

ROOM = np.array([4.0, 4.0, 2.5])
LIGHT = np.array([1.5, 1.5, 2.2])
AIM = np.array([4.0, 2.5, 0.0])
CUBE_C, CUBE_H = np.array([3.4, 2.3, 0.6]), 0.12
POWER = 3.0
CONE_DEG = [40.0, 60.0]  # full apex angles: half-angles 20 and 30
ALBEDO = 0.6  # base 0.6, detail 0.5, double biased multiply


def _quad(c, e1, e2, normal, mat, verts, tris, mats):
    """A rectangle c + s*e1 + t*e2, s,t in [0,1], facing `normal`; UVs in world units."""
    e1, e2, normal = np.asarray(e1, float), np.asarray(e2, float), np.asarray(normal, float)
    base = len(verts)
    for s, t in ((0, 0), (1, 0), (1, 1), (0, 1)):
        p = c + s * e1 + t * e2
        verts.append((*p, *normal, s * np.linalg.norm(e1), t * np.linalg.norm(e2)))
    a, b = (0, 1, 2), (0, 2, 3)
    if np.dot(np.cross(e1, e2), normal) < 0:
        a, b = (0, 2, 1), (0, 3, 2)
    tris += [[base + i for i in a], [base + i for i in b]]
    mats += [mat, mat]


def _png16(path, rgb):
    import cv2
    path.parent.mkdir(parents=True, exist_ok=True)
    img = (np.clip(rgb, 0, 1) * 65535 + 0.5).astype(np.uint16)
    cv2.imwrite(str(path), img[:, :, ::-1])


def gel_image(n=256):
    i = (np.arange(n) * 4 // n)
    cells = (i[:, None] + i[None, :]) % 2
    g = np.where(cells == 0, 1.0, 0.25)
    return np.repeat(g[:, :, None], 3, 2)


def write_scene(folder):
    folder = Path(folder)
    tex = folder.parent / "textures"
    _png16(tex / "synthetic_wall.png", np.full((8, 8, 3), 0.6))
    _png16(tex / "synthetic_detail.png", np.full((8, 8, 3), 0.5))
    _png16(tex / "synthetic_gel.png", gel_image())
    v, t, m = [], [], []
    X, Y, Z = ROOM
    o = np.zeros(3)
    _quad(o, [X, 0, 0], [0, Y, 0], [0, 0, 1], 0, v, t, m)                      # floor
    _quad(np.array([0, 0, Z]), [X, 0, 0], [0, Y, 0], [0, 0, -1], 0, v, t, m)  # ceiling
    _quad(o, [0, Y, 0], [0, 0, Z], [1, 0, 0], 0, v, t, m)                      # wall x=0
    _quad(np.array([X, 0, 0]), [0, Y, 0], [0, 0, Z], [-1, 0, 0], 0, v, t, m)  # wall x=4, the corner's
    _quad(o, [X, 0, 0], [0, 0, Z], [0, 1, 0], 0, v, t, m)                      # wall y=0
    _quad(np.array([0, Y, 0]), [X, 0, 0], [0, 0, Z], [0, -1, 0], 0, v, t, m)  # wall y=4
    h = CUBE_H
    for ax in range(3):
        for sgn in (-1, 1):
            nrm = np.zeros(3); nrm[ax] = sgn
            a, b = [(ax + 1) % 3, (ax + 2) % 3]
            e1 = np.zeros(3); e1[a] = 2 * h
            e2 = np.zeros(3); e2[b] = 2 * h
            c = CUBE_C - h + (nrm > 0) * 2 * h * (np.arange(3) == ax)
            _quad(c, e1, e2, nrm, 1, v, t, m)
    _quad(np.array([0.005, 1.5, 1.0]), [0, 1.0, 0], [0, 0, 0.6], [1, 0, 0], 2, v, t, m)  # glowing panel
    va = np.array(v, np.float32)
    (folder / "geometry").mkdir(parents=True, exist_ok=True)
    write_ply(folder / "geometry" / "bsp.ply", va[:, :3], va[:, 3:6], va[:, 6:8], np.array(t), np.array(m))
    fwd = np.array([3.7, 2.6, 0.15]) - np.array([0.5, 0.5, 1.8])
    cam = {"map": "synthetic", "label": "box", "tick": 0, "position": [0.5, 0.5, 1.8],
           "forward": (fwd / np.linalg.norm(fwd)).tolist(), "up": [0, 0, 1], "fov_h_deg": 75.0,
           "width": 960, "height": 540, "exposure_note": "synthetic: none"}
    wall = {"base_map": "textures/synthetic_wall.png", "detail_map": "textures/synthetic_detail.png",
            "detail_scale": 4.0, "albedo_note": "double biased multiply (base * detail * 2)",
            "specular": {"mask": None, "tint": [0, 0, 0], "roughness": 0.5}, "alpha_test": False,
            "two_sided": False, "emission": {"power": 0.0, "color": [1, 1, 1], "source": "none"}}
    mats = [dict(index=0, tag="synthetic\\shaders\\wall", type="environment", **wall),
            dict(index=1, tag="synthetic\\shaders\\cube", type="environment", **wall),
            {"index": 2, "tag": "synthetic\\shaders\\panel", "type": "environment",
             "base_map": "textures/synthetic_wall.png",
             "emission": {"power": 2.0, "color": [1.0, 0.8, 0.6], "source": "synthetic"}}]
    d = AIM - LIGHT
    lights = {"sky": {"tag": "synthetic", "sun": [], "ambient": {"color": [0, 0, 0], "power": 0.0}},
              "lights": [{"tag": "synthetic\\spot", "kind": "spot", "position": LIGHT.tolist(),
                          "direction": (d / np.linalg.norm(d)).tolist(), "color": [1, 1, 1], "power": POWER,
                          "radius": 0.0, "falloff": "inverse square; linear in angle from inner to outer",
                          "cone_deg": CONE_DEG, "gel": "textures/synthetic_gel.png", "source": "flashlight"}]}
    (folder / "camera.json").write_text(json.dumps(cam, indent=1))
    (folder / "materials.json").write_text(json.dumps(mats, indent=1))
    (folder / "lights.json").write_text(json.dumps(lights, indent=1))
    print("wrote", folder)


# ---------------------------------------------------------------- the check

def _bilinear(img, u, v):
    """Mitsuba's bilinear lookup (texel centers at (i + 0.5) / n), clamped at the edges."""
    h, w = img.shape[:2]
    x = np.clip(u * w - 0.5, 0, w - 1)
    y = np.clip(v * h - 0.5, 0, h - 1)
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
    fx, fy = x - x0, y - y0
    return ((img[y0, x0] * (1 - fx) + img[y0, x1] * fx) * (1 - fy) +
            (img[y1, x0] * (1 - fx) + img[y1, x1] * fx) * fy)


def predict(p, n, gel):
    """The spot's direct radiance at points p (normals n), and its parts, in numpy."""
    from .halo_plugins import light_frame
    ex, ey, ez = (np.array(a) for a in light_frame((AIM - LIGHT).tolist()))
    w = p - LIGHT
    d = np.linalg.norm(w, axis=-1)
    w = w / d[..., None]
    lx, ly, lz = w @ ex, w @ ey, w @ ez
    hin, hout = math.radians(CONE_DEG[0] / 2), math.radians(CONE_DEG[1] / 2)
    th = np.arccos(np.clip(lz, -1, 1))
    ang = np.clip((hout - th) / (hout - hin), 0, 1) * (lz > math.cos(hout))
    t = math.tan(hout)
    u = 0.5 + 0.5 * lx / (np.maximum(lz, 1e-6) * t)
    v = 0.5 - 0.5 * ly / (np.maximum(lz, 1e-6) * t)
    g = _bilinear(gel[:, :, 0], u, v) * ((u >= 0) & (u <= 1) & (v >= 0) & (v <= 1))
    cos_i = np.clip(np.sum(n * -w, -1), 0, None)
    # The shadow: the segment from p to the light against the cube (slabs).
    lo, hi = CUBE_C - CUBE_H, CUBE_C + CUBE_H
    seg = LIGHT - p
    with np.errstate(divide="ignore", invalid="ignore"):
        t0 = (lo - p) / seg
        t1 = (hi - p) / seg
    tmin = np.nanmax(np.minimum(t0, t1), -1)
    tmax = np.nanmin(np.maximum(t0, t1), -1)
    blocked = (tmax >= np.maximum(tmin, 1e-4)) & (tmin <= 1.0)
    vis = ~blocked
    unshadowed = ALBEDO * POWER * ang * g * cos_i / (d * d)
    return unshadowed * vis, dict(d=d, ang=ang, gel=g, cos=cos_i, vis=vis, unshadowed=unshadowed)


def check(folder):
    import mitsuba as mi
    import drjit as dr
    from .scene import Scene
    folder = Path(folder)
    sc = Scene(folder)
    scene = mi.load_dict(sc.build("flashlight"))
    W, H = sc.camera["width"], sc.camera["height"]
    from .integrator import render_pass
    img = sum(render_pass(scene, 64, i, parts=True)["direct"] for i in range(4)).astype(np.float64).mean(-1) / 4
    sensor = scene.sensors()[0]
    gel = sc.image("textures/synthetic_gel.png")

    def hits(dx, dy):
        xs, ys = np.meshgrid((np.arange(W) + 0.5 + dx) / W, (np.arange(H) + 0.5 + dy) / H)
        ray, _ = sensor.sample_ray(0.0, 0.5, mi.Point2f(xs.ravel(), ys.ravel()), mi.Point2f(0.5, 0.5))
        si = scene.ray_intersect(ray)
        p = np.stack([np.asarray(si.p.x), np.asarray(si.p.y), np.asarray(si.p.z)], -1).reshape(H, W, 3)
        nn = np.stack([np.asarray(si.n.x), np.asarray(si.n.y), np.asarray(si.n.z)], -1).reshape(H, W, 3)
        return p, nn

    p, nrm = hits(0, 0)
    pred, parts = predict(p, nrm, gel)
    # Keep the pixels whose prediction is smooth across the pixel (no shadow, gel or cone edge inside it).
    smooth = np.ones((H, W), bool)
    for dx, dy in ((-0.45, -0.45), (0.45, -0.45), (-0.45, 0.45), (0.45, 0.45)):
        pc, nc = hits(dx, dy)
        q, qp = predict(pc, nc, gel)
        smooth &= (qp["vis"] == parts["vis"]) & (np.abs(qp["unshadowed"] - parts["unshadowed"]) <=
                                                 0.02 * np.maximum(parts["unshadowed"], 1e-9))
        smooth &= np.abs(np.sum(nc * nrm, -1) - 1) < 1e-4
    floor = np.abs(p[..., 2]) < 1e-3
    wall = np.abs(p[..., 0] - ROOM[0]) < 1e-3
    smooth &= np.any(np.abs(p - CUBE_C) > CUBE_H + 1e-3, -1)  # not on the cube itself
    peak = parts["unshadowed"].max()
    lit = smooth & parts["vis"] & (pred > 0.01 * peak)
    ratio = img[lit] / pred[lit]
    err = np.abs(ratio - 1)
    # The falloff: radiance over everything but distance, against distance, on a log-log line.
    y = img[lit] / (ALBEDO * POWER * parts["ang"][lit] * parts["gel"][lit] * parts["cos"][lit])
    slope, icpt = np.polyfit(np.log(parts["d"][lit]), np.log(y), 1)
    # The gel: measured over the gel-less prediction, against the gel, in the cone's full-strength core.
    core = lit & (parts["ang"] >= 1.0)
    nogel = ALBEDO * POWER * parts["cos"] / parts["d"] ** 2
    gel_meas = img[core] / nogel[core]
    gel_corr = np.corrcoef(gel_meas, parts["gel"][core])[0, 1]
    bright = parts["gel"][core] > 0.99
    dark = parts["gel"][core] < 0.26
    gel_contrast = np.median(gel_meas[dark]) / np.median(gel_meas[bright])
    # The shadow: where the cube blocks the light (cone and gel lit), the render must be dark.
    sh = smooth & ~parts["vis"] & (parts["unshadowed"] > 0.05 * peak)
    sh_ok = img[sh] < 0.01 * parts["unshadowed"][sh]
    lit_ok = err < 0.02
    sh_floor, sh_wall = int((sh & floor).sum()), int((sh & wall).sum())
    agree = (sh_ok.sum() + lit_ok.sum()) / max(len(sh_ok) + len(lit_ok), 1)
    res = {
        "pixels_lit_checked": int(lit.sum()), "median_rel_error": float(np.median(err)),
        "p99_rel_error": float(np.percentile(err, 99)),
        "falloff_exponent": float(slope), "falloff_constant_rel": float(np.exp(icpt) - 1),
        "gel_correlation": float(gel_corr), "gel_dark_over_bright": float(gel_contrast),
        "shadow_pixels_floor": sh_floor, "shadow_pixels_wall": sh_wall,
        "shadow_dark_fraction": float(sh_ok.mean()) if len(sh_ok) else 0.0,
        "lit_shadow_agreement": float(agree),
    }
    ok = (res["median_rel_error"] < 0.005 and abs(slope + 2) < 0.01 and gel_corr > 0.999 and
          abs(gel_contrast - 0.25) < 0.01 and sh_floor >= 50 and sh_wall >= 50 and
          res["shadow_dark_fraction"] > 0.995 and agree > 0.99)
    res["pass"] = bool(ok)
    out = folder / "check.json"
    out.write_text(json.dumps(res, indent=1))
    from PIL import Image
    vis = np.clip(img / (np.percentile(img, 99.5) + 1e-9), 0, 1) ** (1 / 2.2)
    dbg = np.stack([vis] * 3, -1)
    dbg[sh & floor] = [0.2, 0.4, 1.0]
    dbg[sh & wall] = [1.0, 0.3, 0.2]
    Image.fromarray((dbg * 255).astype(np.uint8)).save(folder / "check.png")
    print(json.dumps(res, indent=1))
    return ok
