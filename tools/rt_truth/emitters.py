"""Solves a level's static light sources from its lightmap (inverse rendering, the lightmap as the answer key).

`python -m rt_truth emitters <scene folder> --out <file>` writes the level's emitter file (rt2-scene-format.md,
"Emitters"): the BSP's glowing shaders with their power fitted, the sky's parts with theirs, the indoor ambient
floor, and only where those cannot explain a region, a few added surface-patch emitters. The truth renderer reads
the file with `render --static emitters --emitters <file>`; the real-time pass is meant to read the same file.

How (README.md, "Solving the emitters"):
1. Samples: the BSP's lightmap texels (texel centres inside each lightmapped triangle; a triangle that holds
   no centre gives one sample at its middle), each with its world point, normal and world area. The target
   is the texel as the game shows it, decoded ^2.2: the radiance a white surface shows there (irradiance / pi).
2. Transport: from each sample, paths with next-event estimation at every vertex through the BSP alone (the
   lightmap tool lit the BSP without objects), cutouts passed, diffuse bounces off the BSP's albedos, so the
   bake's bounce is in. Every light event is credited to its source's group, so one pass gives each group's
   light at unit power at every sample: the regressors X (N x groups x RGB).
3. Fit: nonnegative least squares weighted by world area, with the shaders' powers shrunk toward one shared
   scale x the tag's power (the tag's relative powers are the prior, so nearly collinear shaders do not trade
   light), the strength of that pull chosen by spatial two-fold cross-validation.
4. Added emitters: every opaque BSP surface cut into patches (2 wu cells x 6 facings) is a candidate emitter;
   a least squares of the tag sources (free) and the patches (each with an L1 cost) keeps the fewest patches
   that explain the light the tag sources cannot (at most `--max-added`); those get an RGB power each.
"""

import hashlib
import json
import math
import time
from pathlib import Path

import numpy as np

import mitsuba as mi
import drjit as dr

from . import display
from .scene import Scene, EMISSION_RADIANCE_PER_POWER, AMBIENT_RADIANCE_PER_POWER, SUN_IRRADIANCE_PER_POWER
from .integrator import intersect_through, shadow_T

FORMAT = "goat-h1 rt2 emitters 1"
GAMMA = 2.2  # the pages as the game shows them: value^2.2 is a white surface's radiance (README, "Static light")
TARGET_GAMMA = 2.2  # the target's decode of the page (set by --target-gamma; 1: the lightmap tool's own linear values)
LANES = 1 << 22
WITH_OBJECTS = True  # the transport's world holds the level's baked object types (set by --objects)
GLOW_PATTERN = False # a glowing shader glows where its self-illumination map does (set by --glow)
CUTOUTS_PASS = True  # alpha-tested BSP surfaces pass light through their holes (set by --cutouts)


# ---------------------------------------------------------------- the lightmap's samples

def bsp_file(sc):
    sc.load_geometry()
    for f in sc.geometry:
        if f.parent.name != "objects":
            return f
    raise RuntimeError("no BSP in %s" % sc.folder)


def emitting_materials(sc):
    """{material index: (power, color, two_sided)} of the shaders with radiosity power (their emission)."""
    out = {}
    for m in sc.materials:
        em = m.get("emission") or {}
        if float(em.get("power") or 0) > 0:
            out[int(m["index"])] = (float(em["power"]), [float(c) for c in (em.get("color") or [1, 1, 1])],
                                    bool(m.get("two_sided")) or (m.get("type") or "").startswith("transparent"))
    return out


def is_transparent(sc, mid):
    m = sc.materials[mid] if 0 <= mid < len(sc.materials) else {}
    return (m.get("type") or "").startswith("transparent") and not m.get("alpha_test")


def texel_samples(sc, max_samples=400000, seed=1):
    """The lightmap's samples: pos, ns (shading normal), ng, face, page, texel (x, y), target (N x 3,
    the texel ^2.2), weight (world area), as arrays."""
    f = bsp_file(sc)
    pos, nrm, uv, tris, mats, uv2, lmap, tb = sc.geometry[f]
    emit = emitting_materials(sc)
    pages = {}
    for k in np.unique(lmap[lmap >= 0]):
        img = sc.image("geometry/lightmaps/%d.png" % k)
        if img is not None:
            pages[int(k)] = img[:, :, :3].astype(np.float64)
    out = {k: [] for k in ("face", "bary", "tx", "ty", "w")}
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    area = 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)
    for fi in range(len(tris)):
        p = int(lmap[fi])
        if p not in pages or int(mats[fi]) in emit or is_transparent(sc, int(mats[fi])) or area[fi] <= 0:
            continue
        H, W = pages[p].shape[:2]
        q = uv2[tris[fi]].astype(np.float64) * [W, H]  # texel space, v as stored (README)
        lo = np.floor(q.min(0) - 0.5).astype(int)
        hi = np.ceil(q.max(0) - 0.5).astype(int)
        xs, ys = np.meshgrid(np.arange(max(lo[0], 0), min(hi[0], W - 1) + 1),
                             np.arange(max(lo[1], 0), min(hi[1], H - 1) + 1))
        P = np.stack([xs.ravel() + 0.5, ys.ravel() + 0.5], 1)
        d = (q[1, 0] - q[0, 0]) * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * (q[1, 1] - q[0, 1])
        bary = None
        if abs(d) > 1e-12 and len(P):
            l1 = ((P[:, 0] - q[0, 0]) * (q[2, 1] - q[0, 1]) - (q[2, 0] - q[0, 0]) * (P[:, 1] - q[0, 1])) / d
            l2 = ((q[1, 0] - q[0, 0]) * (P[:, 1] - q[0, 1]) - (P[:, 0] - q[0, 0]) * (q[1, 1] - q[0, 1])) / d
            l0 = 1 - l1 - l2
            ins = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
            if ins.any():
                bary = np.stack([l0[ins], l1[ins], l2[ins]], 1)
                txy = P[ins] - 0.5
        if bary is None:  # no texel centre inside: one sample at the middle, bilinear
            bary = np.array([[1 / 3, 1 / 3, 1 / 3]])
            txy = (q.mean(0) - 0.5)[None]
        n = len(bary)
        out["face"].append(np.full(n, fi))
        out["bary"].append(bary)
        out["tx"].append(txy[:, 0])
        out["ty"].append(txy[:, 1])
        out["w"].append(np.full(n, area[fi] / n))
    face = np.concatenate(out["face"])
    bary = np.concatenate(out["bary"])
    tx, ty, w = (np.concatenate(out[k]) for k in ("tx", "ty", "w"))
    total = len(face)
    rng = np.random.default_rng(seed)
    if total > max_samples:  # uniform subsample, weights scaled to keep the area
        keep = np.sort(rng.choice(total, max_samples, replace=False))
        face, bary, tx, ty, w = face[keep], bary[keep], tx[keep], ty[keep], w[keep] * total / max_samples
    T = tris[face]
    P = (bary[:, :, None] * pos[T]).sum(1)
    ng = np.cross(pos[T[:, 1]] - pos[T[:, 0]], pos[T[:, 2]] - pos[T[:, 0]])
    ng /= np.maximum(np.linalg.norm(ng, axis=1, keepdims=True), 1e-20)
    if nrm is not None:
        ns = (bary[:, :, None] * nrm[T]).sum(1)
        ns /= np.maximum(np.linalg.norm(ns, axis=1, keepdims=True), 1e-20)
        bad = ~np.isfinite(ns).all(1) | ((ns * ng).sum(1) < 0.1)
        ns[bad] = ng[bad]
    else:
        ns = ng.copy()
    target = np.zeros((len(face), 3))
    page = lmap[face]
    for k, img in pages.items():
        s = page == k
        if s.any():
            target[s] = _bilinear3(img, tx[s], ty[s])
    return {"pos": P, "ns": ns, "ng": ng, "face": face, "page": page, "tx": tx, "ty": ty, "w": w,
            "lm": target, "target": np.maximum(target, 0.0) ** TARGET_GAMMA, "texels_total": total}


def _bilinear3(img, x, y):
    H, W = img.shape[:2]
    x0 = np.clip(np.floor(x).astype(int), 0, W - 1)
    y0 = np.clip(np.floor(y).astype(int), 0, H - 1)
    x1, y1 = np.minimum(x0 + 1, W - 1), np.minimum(y0 + 1, H - 1)
    fx, fy = np.clip(x - x0, 0, 1)[:, None], np.clip(y - y0, 0, 1)[:, None]
    return ((1 - fx) * (1 - fy) * img[y0, x0] + fx * (1 - fy) * img[y0, x1] +
            (1 - fx) * fy * img[y1, x0] + fx * fy * img[y1, x1])


# ---------------------------------------------------------------- sources and their Mitsuba scenes

def patch_mesh(tri_pos, name, bsdf, emitter, sc):
    """A patch emitter: its triangles (M x 3 x 3 world positions, wound toward the side that glows)."""
    P = np.asarray(tri_pos, np.float32).reshape(-1, 3)
    t = np.arange(len(P)).reshape(-1, 3)
    return sc.make_mesh(name, P, None, np.zeros((len(P), 2), np.float32), t, bsdf, emitter)


def static_shapes(sc, emitters, render=False, weights=None, floor=None, with_objects=None):
    """The BSP's meshes (one per material) and the area emitters. `emitters` maps a source key
    ("shader:<index>" or "rect:<i>") to its radiance RGB (or None to leave it dark) and, for rects, the
    rect. render=False: diffuse surfaces of the BSP's albedos only (the lightmap tool's world);
    render=True: the truth's full materials, and the ambient floor `floor` (RGB) as each other lightmapped
    surface's emission (albedo x floor). Returns (shapes, {mesh name: source key}, {mesh name: (BSP file,
    its faces)})."""
    f = bsp_file(sc)
    pos, nrm, uv, tris, mats, uv2, lmap, tb = sc.geometry[f]
    emit = emitting_materials(sc)
    shapes, owner, faces = {}, {}, {}
    null = mi.load_dict({"type": "null"})
    weights = weights or {}
    if with_objects is None:
        with_objects = WITH_OBJECTS
    for mid in np.unique(mats):
        mid = int(mid)
        bsdf, _, summ = sc.material(mid)
        if not render:
            if is_transparent(sc, mid):
                bsdf = null
            else:
                bsdf = mi.load_dict({"type": "diffuse", "reflectance": sc.albedos[mid]})
                if CUTOUTS_PASS and mid in getattr(sc, "cutouts", {}):
                    bsdf = mi.load_dict({"type": "mask", "opacity": sc.cutouts[mid], "bsdf": bsdf})
        # "shader:<i>" a shader's glow; "glow:<i>" a shader without radiosity tried as an emitter
        key = "shader:%d" % mid if "shader:%d" % mid in emitters else "glow:%d" % mid
        L = emitters.get(key)
        transparent = is_transparent(sc, mid)
        if transparent and L is None:
            continue  # passes light and gives none
        sel = tris[mats == mid]
        used, inv = np.unique(sel.ravel(), return_inverse=True)
        n = nrm[used] if nrm is not None else None
        if n is not None and (not np.all(np.isfinite(n)) or np.any(np.linalg.norm(n, axis=1) < 0.5)):
            n = None
        sides = [False]
        if mid in emit and L is not None and emit[mid][2]:
            sides.append(True)  # a light card: both faces glow
        for back in sides:
            em = None
            if L is not None:
                pat = glow_pattern(sc, mid, L) if GLOW_PATTERN and key.startswith("shader:") else None
                em = mi.load_dict({"type": "area", "radiance": pat or {"type": "rgb", "value": [float(x) for x in L]},
                                   "sampling_weight": float(weights.get(key, 1.0)) / len(sides)})
            elif render and floor is not None and max(floor) > 0 and not transparent and not back:
                # (halo_model_light with no lights: albedo x ambient^2.2, so the ambient is floor^(1/2.2))
                em = mi.load_dict({"type": "area", "radiance": {
                    "type": "halo_model_light", "albedo": sc.albedos[mid],
                    "ambient": [float(max(x, 0.0)) ** (1 / 2.2) for x in floor]}})
            t = inv.reshape(-1, 3)
            name = "bsp_m%d%s" % (mid, "_back" if back else "")
            shapes[name] = sc.make_mesh(name, pos[used], -n if (back and n is not None) else n, uv[used],
                                        t[:, [0, 2, 1]] if back else t,
                                        null if back else bsdf, em, tb[used] if tb is not None else None)
            faces[name] = (f, np.nonzero(mats == mid)[0])
            if em is not None and L is not None:
                owner[name] = key
    if not render and with_objects:
        # the level's own objects (scenery, machines, controls, light fixtures: Halo's baked object types)
        # as the bake saw them: they shade and reflect (a10's floor is black under its cryo tubes)
        types = sc.object_types()
        for fpath, (opos, onrm, ouv, otris, omats, *_r) in sc.geometry.items():
            if fpath.parent.name != "objects" or types.get(fpath.resolve()) not in sc.BAKED_OBJECT_TYPES:
                continue
            for mid in np.unique(omats):
                mid = int(mid)
                sc.material(mid)
                if is_transparent(sc, mid):
                    continue
                bsdf = mi.load_dict({"type": "diffuse", "reflectance": sc.albedos[mid]})
                if mid in getattr(sc, "cutouts", {}):
                    bsdf = mi.load_dict({"type": "mask", "opacity": sc.cutouts[mid], "bsdf": bsdf})
                sel = otris[omats == mid]
                used, inv = np.unique(sel.ravel(), return_inverse=True)
                name = "obj_%s_m%d" % (fpath.stem, mid)
                shapes[name] = sc.make_mesh(name, opos[used], None, ouv[used], inv.reshape(-1, 3), bsdf, None)
    for key, L in emitters.items():
        if not key.startswith("patch:") or L is None:
            continue
        rad, tri_pos = L
        em = mi.load_dict({"type": "area", "radiance": {"type": "rgb", "value": [float(x) for x in rad]},
                           "sampling_weight": float(weights.get(key, 1.0))})
        name = "patch_%s" % key[6:]
        shapes[name] = patch_mesh(tri_pos, name, null, em, sc)
        owner[name] = key
    return shapes, owner, faces


def glow_pattern(sc, mid, L):
    """Where a glowing shader glows: its self-illumination map as the game draws it (map.r x primary +
    map.g x secondary + map.b x plasma, the "on" colors, at the map's scale), times the radiance L: a lamp's
    housing does not glow, its tube does. None when the shader has no such map. The pattern's mean over the
    map is about the share of the surface that glows, so the fitted scale stays the tag power's."""
    m = sc.materials[mid] if 0 <= mid < len(sc.materials) else {}
    si = m.get("self_illumination") if isinstance(m.get("self_illumination"), dict) else {}
    img = sc.image(si["map"]) if si.get("map") else None
    if img is None:
        return None
    on = [np.array(si.get(k) or [0, 0, 0], float) for k in ("primary_on", "secondary_on", "plasma_on")]
    w = img[:, :, 0:1] * on[0] + img[:, :, 1:2] * on[1] + img[:, :, 2:3] * on[2]
    lum = (w @ display.LUMA).clip(0, None)
    if lum.max() <= 0:
        return None
    pat = np.ascontiguousarray((lum[:, :, None] * np.array(L, float)[None, None, :]).astype(np.float32))
    sc_ = float(si.get("map_scale") or 1.0)
    return {"type": "bitmap", "bitmap": mi.Bitmap(pat), "raw": True, "filter_type": "bilinear", "wrap_mode": "repeat",
            "to_uv": mi.ScalarTransform4f().scale([sc_, sc_, 1.0])}


def sky_parts(sc):
    """The sky's parts as separate sources: {"sky:ambient": (halo_sky dict at unit scale), "sky:sun<i>": ...}."""
    sky = (sc.lights or {}).get("sky") or {}
    sc.load_geometry()
    lo, hi = sc.bounds
    center, radius = (lo + hi) / 2, float(np.linalg.norm(hi - lo) / 2)
    out = {}
    a = sky.get("ambient") or {}
    amb = [x * float(a.get("power", 0.0)) * AMBIENT_RADIANCE_PER_POWER for x in (a.get("color") or [0, 0, 0])]
    if max(amb) > 0:
        out["sky:ambient"] = {"type": "halo_sky", "ambient": amb, "center": center.tolist(), "scene_radius": radius}
    for i, s in enumerate(sky.get("sun") or []):
        dirn = np.array(s["direction"], float)
        if str(s.get("direction_is", "to_sun")).lower() in ("from_sun", "travel", "light_travel"):
            dirn = -dirn
        irr = [c * float(s.get("power", 1.0)) * SUN_IRRADIANCE_PER_POWER for c in s["color"]]
        if max(irr) <= 0:
            continue
        out["sky:sun%d" % i] = {"type": "halo_sky", "ambient": [0.0, 0.0, 0.0], "center": center.tolist(),
                                "scene_radius": radius, "sun0_to_sun": dirn.tolist(), "sun0_irradiance": irr,
                                "sun0_angular_diameter_deg": float(s.get("angular_diameter_deg") or 0.53)}
    return out


def indoor_ambient(sc):
    sky = (sc.lights or {}).get("sky") or {}
    a = sky.get("ambient_indoor") or {}
    c = [x * float(a.get("power", 0.0)) for x in (a.get("color") or [0, 0, 0])]
    return c if max(c) > 0 else None


# ---------------------------------------------------------------- transport

def emitter_ids(scene, owner):
    """{emitter registry id: source key}: `owner` maps a shape's id to its source key, or is one key that
    the scene's every emitter (its sky) is credited to."""
    out = {}
    if isinstance(owner, str):
        for em in scene.emitters():
            out[int(dr.reinterpret_array(mi.UInt32, mi.EmitterPtr(em))[0])] = owner
        return out
    for s in scene.shapes():
        if s.emitter() is not None and s.id() in owner:
            out[int(dr.reinterpret_array(mi.UInt32, mi.EmitterPtr(s.emitter()))[0])] = owner[s.id()]
    return out


def transport(scene, owner, keys, S, spp=64, max_depth=8, seed=0, log=print):
    """Each source's light at every sample (N x K x 3): white Lambertian receivers at the samples, paths
    through the scene with next-event estimation at each vertex, each event credited to its source."""
    K = len(keys)
    kidx = {k: i for i, k in enumerate(keys)}
    ids = emitter_ids(scene, owner)
    lut_np = np.full(max(list(ids) + [0]) + 1, K, np.uint32)  # K: credited to no group
    for eid, key in ids.items():
        if key in kidx:
            lut_np[eid] = kidx[key]
    lut = mi.UInt32(lut_np)
    N = len(S["pos"])
    acc = [np.zeros(N * (K + 1), np.float32) for _ in range(3)]
    per = max(1, min(LANES // spp, (1 << 24) // (K + 1)))  # lanes, and the credit buffers' size
    t0 = time.time()
    from .render import player_playing
    for start in range(0, N, per):
        if player_playing():  # the machine's owner started a game: the GPU is theirs, stop at once
            raise SystemExit("a player's game started: stopped")
        idx = np.arange(start, min(N, start + per))
        n = len(idx) * spp
        sampler = mi.load_dict({"type": "independent"})
        sampler.seed(dr.opaque(mi.UInt32, seed * 1000003 + start), n)
        lane = dr.arange(mi.UInt32, n)
        P = mi.Point3f(*[dr.gather(mi.Float, mi.Float(S["pos"][idx, c].astype(np.float32)), lane // spp) for c in range(3)])
        Ng = mi.Normal3f(*[dr.gather(mi.Float, mi.Float(S["ng"][idx, c].astype(np.float32)), lane // spp) for c in range(3)])
        Ns = mi.Normal3f(*[dr.gather(mi.Float, mi.Float(S["ns"][idx, c].astype(np.float32)), lane // spp) for c in range(3)])
        it = dr.zeros(mi.SurfaceInteraction3f, n)
        it.p, it.n = P, Ng
        it.sh_frame = mi.Frame3f(Ns)
        it.wi = mi.Vector3f(0, 0, 1)
        it.t = mi.Float(0.0)
        bufs = [dr.zeros(mi.Float, len(idx) * (K + 1)) for _ in range(3)]
        local = (lane // spp) * (K + 1)

        def credit(value, ds, mask):
            g = dr.gather(mi.UInt32, lut, dr.reinterpret_array(mi.UInt32, ds.emitter), mask)
            g = dr.select(mask, g, K)
            for c, buf in enumerate(bufs):
                v = value[c]
                dr.scatter_reduce(dr.ReduceOp.Add, buf, dr.select(mask & dr.isfinite(v), v, 0.0), local + g, mask)

        # vertex 0: the white receiver at the sample
        active = mi.Bool(True)
        ds, w = scene.sample_emitter_direction(it, sampler.next_2d(), False, active)
        cos_s = dr.dot(Ns, ds.d)
        ok = active & (ds.pdf != 0) & (cos_s > 0) & (dr.dot(Ng, ds.d) > 0)
        T = shadow_T(scene, it, ds, ok)
        credit(w * T * cos_s / math.pi, ds, ok)
        d = it.sh_frame.to_world(mi.warp.square_to_cosine_hemisphere(sampler.next_2d()))
        active &= dr.dot(d, Ng) > 0
        ray = it.spawn_ray(d)
        beta = mi.Spectrum(1.0)
        ctx = mi.BSDFContext()
        for depth in range(1, max_depth):
            si, beta = intersect_through(scene, sampler, ray, beta, active)
            active &= si.is_valid()
            bsdf = si.bsdf(ray)
            ds, w = scene.sample_emitter_direction(si, sampler.next_2d(active), False, active)
            okd = active & (ds.pdf != 0)
            T = shadow_T(scene, si, ds, okd)
            f = bsdf.eval(ctx, si, si.to_local(ds.d), okd)
            credit(beta * f * w * T, ds, okd)
            bs, bw = bsdf.sample(ctx, si, sampler.next_1d(active), sampler.next_2d(active), active)
            beta = dr.select(active, beta * bw, beta)
            active &= (bs.pdf > 0) & (dr.max(beta) > 0)
            ray = dr.select(active, si.spawn_ray(si.to_world(bs.wo)), ray)
            if depth >= 3:
                q = dr.minimum(dr.max(beta), 0.95)
                survive = sampler.next_1d(active) < q
                beta = dr.select(active, beta / dr.maximum(q, 1e-6), beta)
                active &= survive
            dr.eval(bufs, beta, ray, active)
        for c in range(3):
            acc[c][start * (K + 1):(start + len(idx)) * (K + 1)] += np.asarray(bufs[c]) / spp
    X = np.stack([a.reshape(N, K + 1)[:, :K] for a in acc], -1)
    log("  transport: %d samples x %d spp, %d sources, %.1f s" % (N, spp, K, time.time() - t0))
    return X


def regressors(sc, S, sources, spp, log, seed=0, sky_spp=None):
    """X for the sources: {"shader:<i>": radiance, "rect:<i>": (radiance, rect), "sky:...": dict,
    "floor": rgb, "patch:<i>": (radiance, triangles)}; one scene for the area emitters, one per sky part (Mitsuba takes one sky a scene).
    Returns (keys, X N x K x 3)."""
    area = {k: v for k, v in sources.items() if k.startswith(("shader:", "patch:"))}
    keys, Xs = [], []
    if area:
        shapes, owner, _ = static_shapes(sc, area, weights={k: 1.0 / _mesh_count(sc, k) for k in area})
        scene = mi.load_dict({"type": "scene", **shapes})
        ks = sorted(area)
        Xs.append(transport(scene, owner, ks, S, spp, seed=seed, log=log))
        keys += ks
    for k, skyd in sources.items():
        if not k.startswith("sky:"):
            continue
        shapes, _, _ = static_shapes(sc, {})
        scene = mi.load_dict({"type": "scene", **shapes, "sky": skyd})
        Xs.append(transport(scene, k, [k], S, sky_spp or spp, seed=seed, log=log))
        keys.append(k)
    # A source's light at a white surface can be no more than its own radiance (it filling the whole
    # hemisphere) or, for the sky, its whole radiance and irradiance / pi: a texel beside a lamp sampled by
    # area otherwise gets a rare 1/d^2 spike far past that (an estimator of no use for a fit).
    if Xs:
        Xc = np.concatenate(Xs, 1)
        for j, k in enumerate(keys):
            b = bound(sources[k])
            lum = Xc[:, j] @ display.LUMA
            over = lum > b
            Xc[over, j] *= (b / lum[over])[:, None]
        Xs = [Xc]
    if "floor" in sources:
        c = np.array(sources["floor"], float)
        Xs.append(np.broadcast_to(c, (len(S["pos"]), 1, 3)).copy())
        keys.append("floor")
    return keys, np.concatenate(Xs, 1) if Xs else np.zeros((len(S["pos"]), 0, 3))



def bound(src):
    """The most light (luminance) a source at unit scale can give a white surface."""
    if isinstance(src, dict):  # a sky part
        b = np.array(src.get("ambient", [0, 0, 0]), float)
        if "sun0_irradiance" in src:
            b = b + np.array(src["sun0_irradiance"], float) / math.pi
        return float(b @ display.LUMA)
    if isinstance(src, tuple):  # a patch: (radiance, triangles)
        return float(np.array(src[0], float) @ display.LUMA)
    return float(np.array(src, float) @ display.LUMA)


def _mesh_count(sc, key):
    if key.startswith("shader:"):
        mid = int(key[7:])
        e = emitting_materials(sc).get(mid)
        return 2 if (e and e[2]) else 1
    return 1


# ---------------------------------------------------------------- the fit

def nnls_quadratic(G, b, iters=3000, tol=1e-10):
    """min 1/2 c'Gc - b'c, c >= 0, by cyclic coordinate descent (G small, positive semidefinite)."""
    K = len(b)
    c = np.zeros(K)
    d = np.maximum(np.diag(G), 1e-30)
    for _ in range(iters):
        delta = 0.0
        for j in range(K):
            g = G[j] @ c - b[j]
            nv = max(0.0, c[j] - g / d[j])
            delta = max(delta, abs(nv - c[j]) * math.sqrt(d[j]))
            c[j] = nv
        if delta < tol * (1 + math.sqrt(abs(b @ c))):
            break
    return c


def design(X, y, w, keys):
    """Columns: one per source (its light at unit scale, RGB stacked as rows); a rect has three (R, G, B)."""
    cols, names = [], []
    N = X.shape[0]
    for j, k in enumerate(keys):
        if k.startswith("patch:"):
            for c in range(3):
                col = np.zeros((N, 3))
                col[:, c] = X[:, j, c]
                cols.append(col.reshape(-1))
                names.append((k, c))
        else:
            cols.append(X[:, j, :].reshape(-1))
            names.append((k, None))
    A = np.stack(cols, 1) if cols else np.zeros((N * 3, 0))
    return A, names


def solve(X, y, w, keys, alpha, fit_rows=None):
    """Fitted coefficients (by design column) with the shaders' scales pulled toward one shared scale k:
    penalty alpha x mean column energy x sum_j |X_j|^2 / mean|X|^2 (c_j - k)^2 over the shader columns."""
    A, names = design(X, y, w, keys)
    W = np.repeat(w, 3)
    yy = y.reshape(-1)
    if fit_rows is not None:
        r = np.repeat(fit_rows, 3)
        A, W, yy = A[r], W[r], yy[r]
    Aw = A * W[:, None]
    G = A.T @ Aw
    b = Aw.T @ yy
    shader = [i for i, (k, c) in enumerate(names) if k.startswith("shader:")]
    rects = [i for i, (k, c) in enumerate(names) if k.startswith("patch:")]
    M = len(names)
    Gx = np.zeros((M + 1, M + 1))
    Gx[:M, :M] = G
    bx = np.zeros(M + 1)
    bx[:M] = b
    scale = np.trace(G) / max(M, 1)
    if shader and alpha > 0:
        for i in shader:
            lam = alpha * G[i, i]
            Gx[i, i] += lam
            Gx[i, M] -= lam
            Gx[M, i] -= lam
            Gx[M, M] += lam
    else:
        Gx[M, M] = 1.0
    for i in rects:  # a light ridge on the added emitters, toward none
        Gx[i, i] += 1e-4 * G[i, i] + 1e-12 * scale
    c = nnls_quadratic(Gx, bx)
    return c[:M], float(c[M]), names


def predict(X, keys, coef, names):
    A, _ = design(X, None, None, keys)
    return (A @ coef).reshape(-1, 3)


def r2(y, f, w):
    yl, fl = y @ display.LUMA, f @ display.LUMA
    mu = (w * yl).sum() / w.sum()
    ss = (w * (yl - mu) ** 2).sum()
    return float(1 - (w * (yl - fl) ** 2).sum() / ss) if ss > 0 else float("nan")


def folds(S, cell=8.0):
    """Two spatial folds: 8 wu cells in a checkerboard (neighbouring texels share their light)."""
    q = np.floor(S["pos"] / cell).astype(np.int64)
    return ((q[:, 0] + q[:, 1] + q[:, 2]) % 2) == 0


def choose_alpha(X, y, w, keys, S, log, grid=(0.0, 0.01, 0.1, 1.0, 10.0, 100.0)):
    fa = folds(S)
    best = None
    for a in grid:
        err = 0.0
        for fit_rows in (fa, ~fa):
            coef, k, names = solve(X, y, w, keys, a, fit_rows)
            f = predict(X, keys, coef, names)
            t = ~fit_rows
            err += float((w[t, None] * (y[t] - f[t]) ** 2).sum())
        log("  pull toward the shared scale %g: held-out error %.6g" % (a, err))
        if best is None or err < best[1] * (1 - 1e-6):
            best = (a, err)
    return best[0]


# ---------------------------------------------------------------- added emitters

# ---------------------------------------------------------------- the whole solve

def scenario_key(sc):
    """What the file was solved from: the export's scenario, BSP and a hash of its BSP and lightmap pages."""
    e = json.loads((sc.folder / "export.json").read_text()) if (sc.folder / "export.json").exists() else {}
    h = hashlib.sha256()
    f = bsp_file(sc)
    h.update(f.read_bytes())
    for p in sorted((sc.folder / "geometry" / "lightmaps").glob("*.png"), key=lambda q: int(q.stem)):
        h.update(p.read_bytes())
    return e.get("scenario"), int(e.get("bsp_index", 0)), h.hexdigest()[:16]


def near_emitters(sc, P, radius):
    """Samples within `radius` wu of a glowing (radiosity) surface: distance to the faces' points, sampled
    every 0.25 wu over each face."""
    f = bsp_file(sc)
    pos, _, _, tris, mats = sc.geometry[f][:5]
    emit = emitting_materials(sc)
    pts = []
    for fi in np.nonzero(np.isin(mats, list(emit)))[0]:
        a, b, c = pos[tris[fi]]
        n = int(min(12, max(1, np.ceil(max(np.linalg.norm(b - a), np.linalg.norm(c - a)) / 0.25))))
        for i in range(n + 1):
            for j in range(n + 1 - i):
                pts.append(a + (b - a) * i / n + (c - a) * j / n)
    if not pts:
        return np.zeros(len(P), bool)
    pts = np.array(pts)
    # a grid of `radius` cells: each sample checks the points in its cell and the 26 around it
    cell = {}
    for k, q in zip(map(tuple, np.floor(pts / radius).astype(np.int64)), pts):
        cell.setdefault(k, []).append(q)
    cell = {k: np.array(v) for k, v in cell.items()}
    near = np.zeros(len(P), bool)
    ci = np.floor(P / radius).astype(np.int64)
    offs = [(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)]
    for i in range(len(P)):
        x, y, z = ci[i]
        for a, b, c in offs:
            q = cell.get((x + a, y + b, z + c))
            if q is not None and (((q - P[i]) ** 2).sum(1) < radius * radius).any():
                near[i] = True
                break
    return near


def patch_dictionary(sc, size=2.0, min_area=0.05):
    """Every opaque BSP surface cut into patches: faces grouped by a `size` wu cell of their middle and the
    axis their normal is nearest (6 facings). Each patch: its faces' triangles lifted 0.01 wu off the
    surface, wound to glow toward the face's normal. Returns [{"faces", "triangles", "center", "normal",
    "area"}]."""
    f = bsp_file(sc)
    pos, _, _, tris, mats = sc.geometry[f][:5]
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    cr = np.cross(b - a, c - a)
    area = 0.5 * np.linalg.norm(cr, axis=1)
    ng = cr / np.maximum(2 * area, 1e-20)[:, None]
    mid = (a + b + c) / 3
    ok = (area > 0) & ~np.array([is_transparent(sc, int(m)) for m in mats])
    ax = np.argmax(np.abs(ng), 1) * 2 + (np.take_along_axis(ng, np.argmax(np.abs(ng), 1)[:, None], 1)[:, 0] < 0)
    cell = np.floor(mid / size).astype(np.int64)
    key = np.concatenate([cell, ax[:, None]], 1)
    keys, inv = np.unique(key[ok], axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    idx = np.nonzero(ok)[0]
    out = []
    for g in range(len(keys)):
        fs = idx[inv == g]
        A = area[fs].sum()
        if A < min_area:
            continue
        T = np.stack([a[fs], b[fs], c[fs]], 1) + 0.01 * ng[fs][:, None, :]
        n = (ng[fs] * area[fs, None]).sum(0)
        out.append({"faces": fs, "triangles": T, "center": (mid[fs] * area[fs, None]).sum(0) / A,
                    "normal": n / max(np.linalg.norm(n), 1e-20), "area": float(A)})
    return out


def lasso_nnls(G, b, pen, L=None, iters=1500, c0=None):
    """min 1/2 c'Gc - b'c + pen'c, c >= 0 (FISTA)."""
    if L is None:
        v = np.random.default_rng(0).standard_normal(len(b))
        for _ in range(50):
            v = G @ v
            v /= np.linalg.norm(v) + 1e-30
        L = float(v @ G @ v) * 1.01 + 1e-30
    c = np.zeros(len(b)) if c0 is None else c0.copy()
    z, t = c.copy(), 1.0
    for _ in range(iters):
        cn = np.maximum(0.0, z - (G @ z - b + pen) / L)
        tn = (1 + math.sqrt(1 + 4 * t * t)) / 2
        z = cn + (t - 1) / tn * (cn - c)
        c, t = cn, tn
    return c, L


def select_patches(Xb, Xp, y, w, max_added, log, screen=400):
    """The patches the bake needs: luminance least squares of the tag sources (free, nonnegative) and the
    patches (nonnegative, each with an L1 cost scaled by its column's norm, so a patch is used only where it
    explains light the others cannot), the cost the smallest that keeps at most `max_added` patches."""
    lum = display.LUMA
    A = np.concatenate([Xb @ lum, Xp @ lum], 1).astype(np.float64)
    yl = y @ lum
    G = A.T @ (A * w[:, None])
    b = A.T @ (w * yl)
    nb = Xb.shape[1]
    # screening: the tag sources' fit first, then only the `screen` patches whose light best matches what it
    # leaves (the gradient at zero, by column norm) go into the L1 solve
    cb, _ = lasso_nnls(G[:nb, :nb], b[:nb], np.zeros(nb), iters=3000)
    gain = (b[nb:] - G[nb:, :nb] @ cb) / np.sqrt(np.maximum(np.diag(G)[nb:], 1e-30))
    keep = np.argsort(-gain)[:screen]
    idx = np.concatenate([np.arange(nb), nb + keep])
    G, b, A = G[np.ix_(idx, idx)], b[idx], A[:, idx]
    norm = np.sqrt(np.maximum(np.diag(G), 1e-30))
    lam_hi = float(np.max(b[nb:] / norm[nb:])) if Xp.shape[1] else 0.0
    lo, hi = 0.0, lam_hi
    best = None
    L = None
    for _ in range(14):
        lam = (lo + hi) / 2 if lo > 0 else hi / 8 if best is None else (lo + hi) / 2
        pen = np.concatenate([np.zeros(nb), lam * norm[nb:]])
        c, L = lasso_nnls(G, b, pen, L)
        n_on = int((c[nb:] > 1e-9 * (1 + c.max())).sum())
        fl_ = A @ c
        log("    L1 cost %.4g: %d patches, R^2 %.4f" % (lam, n_on, 1 - (w * (yl - fl_) ** 2).sum() / (w * (yl - (w * yl).sum() / w.sum()) ** 2).sum()))
        if n_on <= max_added:
            best = (lam, c, n_on)
            hi = lam
        else:
            lo = lam
    c0, _ = lasso_nnls(G, b, np.zeros(len(b)), L, iters=4000)
    f0 = A @ c0
    log("    no L1 cost (every patch free): %d patches, R^2 %.4f" % (int((c0[nb:] > 0).sum()),
        1 - (w * (yl - f0) ** 2).sum() / (w * (yl - (w * yl).sum() / w.sum()) ** 2).sum()))
    lam, c, n_on = best
    on = np.nonzero(c[nb:] > 1e-9 * (1 + c.max()))[0]
    energy = c[nb:][on] * (A[:, nb:][:, on] * w[:, None]).sum(0)
    order = on[np.argsort(-energy)]
    log("  patches: %d of %d kept (%d screened; L1 cost %.4g of %.4g)" % (len(order), Xp.shape[1], len(keep), lam, lam_hi))
    return [int(keep[o]) for o in order]


def solve_level(folder, out, spp=64, max_samples=300000, max_added=24, report=None, log=print, weights="area",
                dict_samples=20000, dict_spp=256, patch_size=2.0):
    t0 = time.time()
    sc = Scene(folder, static_mode="fit")
    sc.load_geometry()
    for mid in range(len(sc.materials)):
        sc.material(mid)
    S = texel_samples(sc, max_samples)
    y = S["target"]
    # The bake clips at 1 (next to lamps): a clipped texel says only "at least this", so it is left out.
    clipped = S["lm"].max(1) >= 0.985
    # A texel within 0.5 wu of a glowing surface holds the lamp's near field, which a texel cannot resolve (its
    # footprint averages a 1/d^2 peak; the bake clips there): 30-50x off the traced light at a10's halogens,
    # where the floors 2-6 wu off agree. Left out too.
    near = near_emitters(sc, S["pos"], 0.5)
    log("  left out: %.1f%% of the samples clipped in the bake, %.1f%% more within 0.5 wu of a glowing surface" % (
        100 * clipped.mean(), 100 * (near & ~clipped).mean()))
    S["w"] = np.where(clipped | near, 0.0, S["w"])
    # weights "display": what the player sees is the page as shown (^1/2.2): an error dy shows as
    # 0.45 y^-0.55 dy, so squared errors are weighted by y^-1.1 (floored at 0.005, the eye's dark limit).
    S["w_area"] = S["w"].copy()
    if weights == "display":
        S["w"] = S["w"] * (np.maximum(S["target"] @ display.LUMA, 0.0) + 0.005) ** -1.1
    w, wa = S["w"], S["w_area"]
    log("%s: %d lightmap samples (of %d texels), %.1f wu^2" % (Path(folder).name, len(w), S["texels_total"], wa.sum()))
    emit = emitting_materials(sc)
    sources = {}
    for mid, (p, col, two) in sorted(emit.items()):
        sources["shader:%d" % mid] = [c * p * EMISSION_RADIANCE_PER_POWER for c in col]
    sources.update(sky_parts(sc))
    floor = indoor_ambient(sc)
    sources["floor"] = floor if floor else [1.0, 1.0, 1.0]
    keys, X = regressors(sc, S, sources, spp, log)
    base = list(keys)
    alpha = choose_alpha(X, y, w, keys, S, log)
    coef, kshared, names = solve(X, y, w, keys, alpha)
    f = predict(X, keys, coef, names)
    shown = lambda q: np.maximum(q, 0) ** (1 / TARGET_GAMMA)
    r2_tags, r2_tags_disp = r2(y, f, wa), r2(shown(y), shown(f), wa)
    log("tag sources alone: R^2 %.4f, as shown %.4f (pull %g, shared scale %.4g)" % (r2_tags, r2_tags_disp, alpha, kshared))
    # (3) added emitters: the BSP's surface patches, chosen on a subset of the samples at more paths each
    patches = patch_dictionary(sc, patch_size) if max_added > 0 else []
    rng = np.random.default_rng(7)
    sub = np.sort(rng.choice(len(w), min(dict_samples, len(w)), replace=False, p=wa / wa.sum()))
    Ssub = {k: (v[sub] if isinstance(v, np.ndarray) and len(v) == len(w) else v) for k, v in S.items()}
    psrc = {"patch:%d" % i: ([1.0, 1.0, 1.0], P["triangles"]) for i, P in enumerate(patches)}
    order = []
    if psrc:
        pk, PX = regressors(sc, Ssub, psrc, dict_spp, log, seed=3)
        order = [int(pk[j][6:]) for j in select_patches(X[sub], PX, y[sub], np.ones(len(sub)), max_added, log)]
    chosen = ["patch:%d" % i for i in order]
    if chosen:
        ck, CX = regressors(sc, S, {k: psrc[k] for k in chosen}, spp * 2, log, seed=5)
        Xall, allkeys = np.concatenate([X, CX], 1), base + ck
    else:
        Xall, allkeys = X, base
    coef, kshared, names = solve(Xall, y, w, allkeys, alpha)
    f = predict(Xall, allkeys, coef, names)
    final_r2, r2_disp = r2(y, f, wa), r2(shown(y), shown(f), wa)
    log("with %d added emitters: R^2 %.4f, as shown %.4f" % (len(chosen), final_r2, r2_disp))
    scen, bsp, h = scenario_key(sc)
    doc = {"format": FORMAT, "map": (sc.camera.get("map") or Path(folder).name.split("_")[0]), "scenario": scen,
           "bsp_index": bsp, "solved_from": {"bsp_and_lightmaps_sha256_16": h, "scene": Path(folder).name},
           "units": "radiance in the game's linear light units: a white Lambertian surface lit by a source of radiance L "
                    "over its whole hemisphere shows L; the lightmap's texel v as the game shows it is v^2.2 of these units. "
                    "Positions in Halo world units (1 wu = 10 ft), z up",
           "fit": {"r2": round(final_r2, 4), "r2_display_gamma": round(r2_disp, 4),
                   "r2_tag_sources_alone": round(r2_tags, 4), "r2_display_gamma_tag_sources_alone": round(r2_tags_disp, 4),
                   "samples": int(len(w)), "samples_fitted": int((wa > 0).sum()), "texels": int(S["texels_total"]),
                   "area_wu2": round(float(wa.sum()), 2), "spp": spp,
                   "target": "lightmap texel ^%g (README, Static light)" % TARGET_GAMMA,
                   "weights": (weights + ": " + ("world area x (texel ^2.2 luminance + 0.005)^-1.1" if weights == "display"
                               else "world area")) + "; R^2 by area (r2: linear light, r2_display_gamma: as shown); "
                               "left out: texels clipped in the bake and within 0.5 wu of a glowing surface",
                   "shared_scale": round(kshared, 6), "pull_toward_shared_scale": alpha,
                   "added_emitters_from": "%d surface patches (%g wu cells x 6 facings), L1-selected (README)" % (len(patches), patch_size),
                   "transport": "BSP only (no objects), diffuse bounces off the BSP albedos, cutouts passed, "
                                "next-event estimation at every vertex, up to 8 vertices with Russian roulette"},
           "shader_emitters": [], "sky": {}, "ambient_floor": None, "added_emitters": []}
    cof = {}
    for (k, c), v in zip(names, coef):
        if c is None:
            cof[k] = float(v)
        else:
            cof.setdefault(k, [0.0, 0.0, 0.0])[c] = float(v)
    for mid, (p, col, two) in sorted(emit.items()):
        k = "shader:%d" % mid
        sv = cof.get(k, 0.0)
        m = sc.materials[mid]
        doc["shader_emitters"].append({
            "shader": m["tag"], "material_index": mid, "type": m.get("type"),
            "radiance": [round(c * p * EMISSION_RADIANCE_PER_POWER * sv, 6) for c in col],
            "two_sided": two, "tag_power": p, "tag_color": col, "scale": round(sv, 6),
            "scale_over_shared": round(sv / kshared, 4) if kshared > 0 else None,
            "from": "shader radiosity power (0x04) x color (0x08) x scale; the scale fitted (README, Solving the emitters)"})
    sky = (sc.lights or {}).get("sky") or {}
    if "sky:ambient" in cof:
        a = sky.get("ambient") or {}
        doc["sky"]["ambient"] = {"radiance": [round(x * float(a.get("power", 0)) * cof["sky:ambient"], 6) for x in a["color"]],
                                 "tag_power": a.get("power"), "tag_color": a.get("color"), "scale": round(cof["sky:ambient"], 6)}
    suns = []
    for i, sn in enumerate(sky.get("sun") or []):
        k = "sky:sun%d" % i
        if k in cof:
            suns.append({"index": i, "direction_to_sun": sn["direction"], "angular_diameter_deg": sn.get("angular_diameter_deg"),
                         "irradiance": [round(c * float(sn.get("power", 1)) * math.pi * cof[k], 6) for c in sn["color"]],
                         "tag_power": sn.get("power"), "tag_color": sn.get("color"), "scale": round(cof[k], 6)})
    doc["sky"]["suns"] = suns
    fl = np.array(sources["floor"]) * cof.get("floor", 0.0)
    doc["ambient_floor"] = {"radiance": [round(float(x), 6) for x in fl], "scale": round(cof.get("floor", 0.0), 6),
                            "tag": "sky indoor ambient radiosity color (0x38) x power (0x44)" if floor else "none in the tag: white",
                            "meaning": "a constant radiance every lightmapped surface receives (it shows albedo x this more); no bounce"}
    for k in chosen:
        P = patches[int(k[6:])]
        rad = cof.get(k, [0.0, 0.0, 0.0])
        if max(rad) <= 0:
            continue
        doc["added_emitters"].append({
            "id": len(doc["added_emitters"]), "shape": "triangles", "one_sided": True,
            "center": [round(float(x), 4) for x in P["center"]], "normal": [round(float(x), 5) for x in P["normal"]],
            "area_wu2": round(P["area"], 5), "radiance": [round(x, 6) for x in rad],
            "power": round(float(np.array(rad) @ display.LUMA) * P["area"] * math.pi, 6),
            "triangles": [[round(float(x), 4) for x in t.ravel()] for t in P["triangles"]],
            "why": "light the bake holds that the tag's sources cannot give: a surface patch (%d BSP triangles) chosen "
                   "by the L1-costed fit, glowing toward its normal, lifted 0.01 wu off the surface" % len(P["faces"])})
    doc["seconds"] = round(time.time() - t0, 1)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n")
    log("wrote %s: R^2 %.4f (as shown %.4f; tag sources alone %.4f), %d added emitters, %.0f s" % (
        out, final_r2, r2_disp, r2_tags, len(doc["added_emitters"]), time.time() - t0))
    if report:
        write_report(sc, S, y, f, Path(report), doc)
    return doc


def write_report(sc, S, y, f, folder, doc):
    """Residual pages (grey: the fit matches; white: the bake brighter; black: darker; blue: no sample) and the
    pages of the fit beside the bake (both as the game shows them, ^1/2.2)."""
    from PIL import Image
    folder.mkdir(parents=True, exist_ok=True)
    yl, fl = y @ display.LUMA, f @ display.LUMA
    for k in np.unique(S["page"]):
        img = sc.image("geometry/lightmaps/%d.png" % k)
        H, W = img.shape[:2]
        res = np.zeros((H, W, 3), np.uint8)
        res[:] = (0, 0, 80)
        fit = np.zeros((H, W, 3))
        s = S["page"] == k
        x = np.clip(np.round(S["tx"][s]).astype(int), 0, W - 1)
        yy = np.clip(np.round(S["ty"][s]).astype(int), 0, H - 1)
        scale = max(np.percentile(yl, 99), 1e-6)
        g = np.clip(128 + 127 * (yl[s] - fl[s]) / scale, 0, 255).astype(np.uint8)
        res[yy, x] = np.stack([g, g, g], 1)
        fit[yy, x] = np.maximum(f[s], 0) ** (1 / TARGET_GAMMA)
        both = np.concatenate([img[:, :, :3], np.clip(fit, 0, 1)], 1)
        Image.fromarray(res).save(folder / ("residual_page_%d.png" % k))
        Image.fromarray((both * 255 + 0.5).astype(np.uint8)).save(folder / ("bake_vs_fit_page_%d.png" % k))
    np.savez_compressed(folder / "samples.npz", pos=S["pos"].astype(np.float32), target=y.astype(np.float32),
                        fit=f.astype(np.float32), w=S["w"].astype(np.float32), page=S["page"])
    (folder / "emitters.json").write_text(json.dumps(doc, indent=1) + "\n")


# ---------------------------------------------------------------- reading the file (the truth's "solved" group)

def solved_scene_dict(sc, path):
    """The truth's static light from an emitter file: the BSP with its full materials, the shaders' and added
    emitters at their solved radiance, the ambient floor, the sky's parts at their solved scale (one sky),
    and the objects (their materials, lit by that light: no static light of their own)."""
    doc = json.loads(Path(path).read_text())
    assert doc.get("format") == FORMAT, "%s: not an emitter file (%s)" % (path, doc.get("format"))
    sc.load_geometry()
    for mid in range(len(sc.materials)):
        sc.material(mid)
    by_tag = {m["tag"]: int(m["index"]) for m in sc.materials}
    src = {}
    for e in doc["shader_emitters"]:
        if e["shader"] in by_tag:
            src["shader:%d" % by_tag[e["shader"]]] = e["radiance"]
    for r in doc["added_emitters"]:
        src["patch:%d" % r["id"]] = (r["radiance"], r["triangles"])
    for r in doc.get("lamps") or []:  # a lamp group: the shader's surfaces in its region, glowing
        src["patch:lamp%d" % r["id"]] = (r["radiance"], r["triangles"])
    floor = (doc.get("ambient_floor") or {}).get("radiance")
    shapes, owner, faces = static_shapes(sc, src, render=True, floor=floor)
    for fpath, (pos, nrm, uv, tris, mats, *_rest) in sc.geometry.items():
        if fpath.parent.name != "objects":
            continue
        tb = sc.geometry[fpath][7]
        for mid in np.unique(mats):
            bsdf, _, summ = sc.material(int(mid))
            if summ.get("surface", "").startswith("transparent"):
                continue
            sel = tris[mats == mid]
            used, inv = np.unique(sel.ravel(), return_inverse=True)
            n = nrm[used] if nrm is not None else None
            if n is not None and (not np.all(np.isfinite(n)) or np.any(np.linalg.norm(n, axis=1) < 0.5)):
                n = None
            name = "obj_%s_m%d" % (fpath.stem, mid)
            faces[name] = (fpath, np.nonzero(mats == mid)[0])
            shapes[name] = sc.make_mesh(name, pos[used], n, uv[used], inv.reshape(-1, 3), bsdf, None,
                                        tb[used] if tb is not None else None)
    sc.meshes, sc.mesh_faces = shapes, faces
    d = {"type": "scene", "sensor": sc.sensor()}
    d.update(shapes)
    sky = doc.get("sky") or {}
    lo, hi = sc.bounds
    skyd = {"type": "halo_sky", "ambient": [0.0, 0.0, 0.0], "center": ((lo + hi) / 2).tolist(),
            "scene_radius": float(np.linalg.norm(hi - lo) / 2)}
    if sky.get("ambient"):
        skyd["ambient"] = [float(x) for x in sky["ambient"]["radiance"]]
    for i, s in enumerate(sky.get("suns") or []):
        skyd["sun%d_to_sun" % i] = [float(x) for x in s["direction_to_sun"]]
        skyd["sun%d_irradiance" % i] = [float(x) for x in s["irradiance"]]
        skyd["sun%d_angular_diameter_deg" % i] = float(s.get("angular_diameter_deg") or 0.53)
    if max(skyd["ambient"]) > 0 or "sun0_to_sun" in skyd:
        d["sky"] = skyd
    sc.note_once("static light: the emitter file %s (R^2 %s against the lightmap; %d shader emitters, %d added)" % (
        Path(path).name, doc["fit"].get("r2"), len(doc["shader_emitters"]), len(doc["added_emitters"]) + len(doc.get("lamps") or [])))
    return d


# ---------------------------------------------------------------- rooms: the tag's own lamps, brightness per room

def lamp_groups(sc, cell):
    """Each glowing shader's surfaces split by `cell` wu cells of their middles (a room-sized cluster): its
    triangles lifted 0.01 wu toward their normals (and, for a light card, a copy facing back)."""
    f = bsp_file(sc)
    pos, _, _, tris, mats = sc.geometry[f][:5]
    a, b, c = pos[tris[:, 0]], pos[tris[:, 1]], pos[tris[:, 2]]
    cr = np.cross(b - a, c - a)
    ar = 0.5 * np.linalg.norm(cr, axis=1)
    ng = cr / np.maximum(2 * ar, 1e-20)[:, None]
    mid = (a + b + c) / 3
    out = []
    for m, (p, col, two) in sorted(emitting_materials(sc).items()):
        fs = np.nonzero((mats == m) & (ar > 0))[0]
        cells = np.floor(mid[fs] / cell).astype(np.int64)
        for key in np.unique(cells, axis=0):
            g = fs[(cells == key).all(1)]
            T = np.stack([a[g], b[g], c[g]], 1)
            tri = list(T + 0.01 * ng[g][:, None, :])
            if two:
                tri += list((T - 0.01 * ng[g][:, None, :])[:, [0, 2, 1]])
            out.append({"material": m, "shader": sc.materials[m]["tag"], "cell": key.tolist(), "power": p,
                        "color": col, "two_sided": two, "triangles": np.array(tri), "area": float(ar[g].sum())})
    if FIXTURES:
        out += fixture_groups(sc, cell, out)
    return out


FIXTURES = True  # the level's light fixtures' glowing parts are lamp candidates too (set by --fixtures off)
# (scenery and light fixtures, placed once and never moving: a machine or control (a door, a lift) moves and has
# its own lamps at run time, xgpu_vulkan_lt_object_lamps)
FIXTURE_TYPES = (6, 9)


def fixture_glow(sc, mid, otype):
    """A baked object's glowing part: (color, two_sided) or None. A model shader's self-illumination (its upper
    colour, the "on" one: a fixture's lamp, light_ceiling's housing glow) on any baked object; a light fixture's
    (type 9) transparent parts too (its light card or cone, both faces). The tags give them no radiosity power
    (model shaders have none), yet the bake has their light: d20's and a50's rooms are lit by light_ceiling
    fixtures alone, and without them the fit held the nearest glow strips at its 20x limit."""
    m = sc.materials[mid] if 0 <= mid < len(sc.materials) else {}
    si = m.get("self_illumination") if isinstance(m.get("self_illumination"), dict) else {}
    up = [float(c) for c in (si.get("color_upper") or [0, 0, 0])]
    if max(up) > 0 and not (m.get("type") or "").startswith("transparent"):
        return up, False
    if otype == 9 and (m.get("type") or "").startswith("transparent"):
        return [1.0, 1.0, 1.0], True
    return None


CONES = False  # a fixture's light cone or card a source beside its glowing lamp (set by --cones on; study)
GLOW_KEPT = 0.99  # the share of a fixture group's glow its kept triangles hold (fixture_groups' thinning)
# (barycentric points a triangle's self-illumination mask is read at: its middle, and halfway to each corner and
# to each edge's middle)
_MASK_POINTS = np.array([[1 / 3, 1 / 3, 1 / 3], [2 / 3, 1 / 6, 1 / 6], [1 / 6, 2 / 3, 1 / 6], [1 / 6, 1 / 6, 2 / 3],
                         [1 / 2, 1 / 2, 0], [0, 1 / 2, 1 / 2], [1 / 2, 0, 1 / 2]])


def self_illum_mask(sc, mid, UV):
    """Each triangle's glow over its area, 0 to 1: a model shader's self-illumination as the model pass draws
    it, (mask x color)^2.2 with the mask the multipurpose map's green (HaloSelfIllum), read at _MASK_POINTS of
    each triangle (UV: T x 3 x 2, the map's u and v scales applied) and averaged as mask^2.2. None when the shader
    has no multipurpose map (it glows all over)."""
    m = sc.materials[mid] if 0 <= mid < len(sc.materials) else {}
    img = sc.image(m["multipurpose_map"]) if m.get("multipurpose_map") else None
    if img is None or not len(UV):
        return None
    tex = sc.bitmap(img[:, :, 1:2])
    su, sv = float(m.get("map_u_scale") or 1.0), float(m.get("map_v_scale") or 1.0)
    P = np.einsum("pc,tcx->tpx", _MASK_POINTS, UV).reshape(-1, 2) * np.array([su, sv])
    si = dr.zeros(mi.SurfaceInteraction3f, len(P))
    si.uv = mi.Point2f(P[:, 0].astype(np.float32), P[:, 1].astype(np.float32))
    g = np.clip(np.array(tex.eval_1(si)), 0.0, 1.0).reshape(len(UV), len(_MASK_POINTS))
    return (g ** GAMMA).mean(1)


def thin_glow(T, ar, glow):
    """The triangles that hold GLOW_KEPT of a group's glow (area x glow over its area), the strongest first: a
    fixture's housing, where its mask is black, gives no light and is not a light source to sample."""
    w = ar * glow
    total = float(w.sum())
    if total <= 0:
        return np.zeros(0, np.int64)
    order = np.argsort(-w, kind="stable")
    n = int(np.searchsorted(np.cumsum(w[order]), GLOW_KEPT * total) + 1)
    return np.sort(order[:min(n, len(order))])


def fixture_groups(sc, cell, bsp_groups):
    """The static baked objects' glowing parts (fixture_glow; FIXTURE_TYPES) as lamp groups, by material and `cell` wu cell like the
    BSP's: world triangles lifted 0.01 wu toward their normals. Their prior power is the BSP lamps' median tag
    power (none: 1), so the fit's per-group scale starts at the level's; it fits them as it fits the BSP's."""
    types = sc.object_types()
    powers = sorted(g["power"] for g in bsp_groups)
    p0 = powers[len(powers) // 2] if powers else 1.0
    found = {}
    for fpath, (opos, onrm, ouv, otris, omats, *_r) in sc.geometry.items():
        otype = types.get(fpath.resolve())
        if fpath.parent.name != "objects" or otype not in FIXTURE_TYPES:
            continue
        # (an object whose lamp glows (a model part with its self-illumination mask lit) is lit by that lamp: its
        # light cone or card is the lamp's light seen in the air (an additive shader, no radiosity power), not a
        # second source, so it is a source only on a fixture without such a lamp)
        parts = {}
        for mid in np.unique(omats):
            glow = fixture_glow(sc, int(mid), otype)
            if glow is not None:
                parts[int(mid)] = glow
        lamp = False
        for mid, glow in parts.items():
            if not glow[1]:
                sel = otris[omats == mid]
                mask = self_illum_mask(sc, mid, ouv[sel])
                lamp = lamp or mask is None or bool((mask > 0).any())
        for mid, glow in parts.items():
            if glow[1] and lamp and not CONES:
                continue
            sel = otris[omats == mid]
            T, UV = opos[sel], ouv[sel]
            cr = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0])
            ar = 0.5 * np.linalg.norm(cr, axis=1)
            ok = ar > 1e-8
            T, UV, cr, ar = T[ok], UV[ok], cr[ok], ar[ok]
            ng = cr / (2 * ar)[:, None]
            # (a model shader glows where its mask does: only the triangles that hold its glow, each glowing
            # evenly at the group's radiance as the pass samples it; a light card or cone glows all over)
            mask = None if glow[1] else self_illum_mask(sc, int(mid), UV)
            keep = thin_glow(T, ar, mask) if mask is not None else np.arange(len(T))
            cells = np.floor(T.mean(1) / cell).astype(np.int64)
            for i in keep:
                k = (int(mid), tuple(cells[i]))
                f = found.setdefault(k, {"tri": [], "area": 0.0, "glow": glow})
                f["tri"].append(T[i] + 0.01 * ng[i])
                if glow[1]:
                    f["tri"].append((T[i] - 0.01 * ng[i])[[0, 2, 1]])
                f["area"] += float(ar[i])
    out = []
    for (mid, key), f in sorted(found.items()):
        out.append({"material": mid, "shader": sc.materials[mid]["tag"], "cell": list(key), "power": p0,
                    "color": f["glow"][0], "two_sided": f["glow"][1], "triangles": np.array(f["tri"]),
                    "area": f["area"], "fixture": True})
    return out


def solve_rooms(folder, out, spp=128, max_samples=300000, cell=12.0, report=None, log=print, outside=0.05):
    """The level's light from its own lamps (the shaders with radiosity power, at the tag's color and relative
    power), with one brightness scale per room-sized cluster of lamps (`cell` wu cells), set so that in each
    cell the traced mean light equals the bake's (linear light, the lightmap ^2.2, area-weighted; texels
    clipped in the bake or within 0.5 wu of a lamp left out). A lamp's scale moves by the bake-over-traced
    ratio of the cells its light lands in, weighted by how much lands in each (a fixed point, damped)."""
    t0 = time.time()
    sc = Scene(folder, static_mode="fit")
    sc.load_geometry()
    for mid in range(len(sc.materials)):
        sc.material(mid)
    S = texel_samples(sc, max_samples)
    y = S["target"] @ display.LUMA
    clipped = S["lm"].max(1) >= 0.985
    near = near_emitters(sc, S["pos"], 0.5)
    w = np.where(clipped | near, 0.0, S["w"])
    lamps = lamp_groups(sc, cell)
    # (a fixture is a light of this BSP only inside its lit space: the scenario's objects stand in every BSP's,
    # and one placed where this BSP has no lightmapped surface in its cell or the cells beside it lights none of
    # the bake, so the bake cannot set it and the fit pushed it to its limit (d20 bsp0's other halls))
    have = {tuple(c) for c in np.unique(np.floor(S["pos"][S["w"] > 0] / cell).astype(np.int64), axis=0).tolist()}
    near = [(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1)]
    inside = [not L.get("fixture") or any((L["cell"][0] + a, L["cell"][1] + b, L["cell"][2] + c) in have
                                          for a, b, c in near) for L in lamps]
    if not all(inside):
        log("rooms: %d fixture groups (%d triangles) outside the BSP's lightmapped cells left out" % (
            inside.count(False), sum(len(L["triangles"]) for L, ok in zip(lamps, inside) if not ok)))
        lamps = [L for L, ok in zip(lamps, inside) if ok]
    src = {"patch:%d" % i: ([c * L["power"] * EMISSION_RADIANCE_PER_POWER for c in L["color"]], L["triangles"])
           for i, L in enumerate(lamps)}
    sky = sky_parts(sc)  # the sky's dome and lights at the tag's power, to tell the sky's light from the lamps'
    src.update(sky)
    keys, X = regressors(sc, S, src, spp, log, sky_spp=max(128, spp // 8))  # the sky's light is smooth
    li = [j for j, k in enumerate(keys) if k.startswith("patch:")]
    si = [j for j, k in enumerate(keys) if k.startswith("sky:")]
    order = [int(keys[j][6:]) for j in li]
    Xl = X[:, li] @ display.LUMA  # N x K, each lamp group at the tag's power
    Xs = X[:, si] @ display.LUMA  # N x parts, each sky part at the tag's power
    tc = np.floor(S["pos"] / cell).astype(np.int64)
    _, inv = np.unique(tc, axis=0, return_inverse=True)
    inv = inv.reshape(-1)
    nc = inv.max() + 1
    # (1) Cells the lamps cannot light (at the level's scale, under `outside` of the bake's light there) are lit
    # by something else: the sky through an opening, a ship's hull outside. They are the sky's, left out.
    w_all = w.copy()
    Y0 = np.bincount(inv, w * y, nc)
    dark = np.zeros(nc, bool)
    for _ in range(3):
        k0 = float((w * y).sum() / max((w * Xl.sum(1)).sum(), 1e-30))
        F0 = np.bincount(inv, w * Xl.sum(1) * k0, nc)
        new = (Y0 > 0) & (F0 < outside * Y0) & ~dark
        if not new.any():
            break
        dark |= new
        w = np.where(dark[inv], 0.0, w)
    # (2) The sky's scales (one a part: dome, each sky light) from those cells, where only the sky lights the bake;
    # the sky's light is then taken off the bake everywhere, and the lamps match what is left. A cell where the
    # sky gives all but `outside` of the bake's light is the sky's too.
    Sc = np.stack([np.bincount(inv, w_all * Xs[:, j], nc) for j in range(len(si))], 1) if si else np.zeros((nc, 0))
    ks = np.zeros(len(si))
    # only where the sky is the light: those cells hold 1% of the bake at least, and the sky at the tag's power
    # gives a tenth of their light at least (a crack's leak does not set the sky's scale for the whole BSP)
    fit_c = dark & (Sc.sum(1) >= 0.1 * Y0) if si else dark
    if si and fit_c.any() and Y0[dark].sum() >= 0.01 * Y0.sum():
        D = Sc[fit_c]
        ks = np.minimum(nnls_quadratic(D.T @ D, D.T @ Y0[fit_c]), 4.0)
    sky_c = Sc @ ks
    Yc = np.where(dark, 0.0, np.maximum(Y0 - sky_c, 0.0))
    dark |= (Y0 > 0) & (Yc < outside * Y0)
    Yc = np.where(dark, 0.0, Yc)
    w = np.where(dark[inv], 0.0, w_all)
    bake_all = max(float(Y0.sum()), 1e-30)
    out_share = float(Y0[dark].sum()) / bake_all
    sky_share = float(np.minimum(sky_c, Y0).sum()) / bake_all
    if dark.any() or ks.any():
        log("rooms: the sky (its tags x %s) gives %.1f%% of the bake's light; %.1f%% is in cells left to it" % (
            ", ".join("%s %.3g" % (keys[j][4:], k) for j, k in zip(si, ks)), 100 * sky_share, 100 * out_share))
    k0 = float(Yc.sum() / max((w * Xl.sum(1)).sum(), 1e-30))
    land = np.stack([np.bincount(inv, w * Xl[:, j], nc) for j in range(len(order))], 1)  # cells x lamps
    # (and a fixture whose light reaches the bake's texels at under 1% of the fixtures' typical reach for its
    # area: a lamp behind a wall of this BSP's halls, which the bake cannot set either)
    fx = np.array([bool(lamps[i].get("fixture")) for i in order], bool)
    if fx.any():
        reach = land.sum(0) / np.maximum([lamps[i]["area"] for i in order], 1e-30)
        cut = fx & (reach < 0.01 * float(np.median(reach[fx])))
        if cut.any():
            log("rooms: %d fixture groups whose light does not reach the bake left out" % int(cut.sum()))
            order = [i for i, c in zip(order, cut) if not c]
            Xl, land, fx = Xl[:, ~cut], land[:, ~cut], fx[~cut]
    share = land / np.maximum(land.sum(0, keepdims=True), 1e-30)
    level = np.full(len(order), k0)
    s = level.copy()
    for it in range(30):
        Fc = land @ s
        ratio = np.where((Fc > 0) & (Yc > 0), Yc / np.maximum(Fc, 1e-30), 1.0)
        s = np.clip(s * np.exp(0.5 * (share * np.log(ratio)[:, None]).sum(0)), level / 20, level * 20)
    Fc = land @ s
    if report:
        # (the cells' bake less the sky, each group's light landing in them, the scales: for a look at a fit)
        Path(report).mkdir(parents=True, exist_ok=True)
        keys_c, first = np.unique(tc, axis=0, return_index=True)
        np.savez_compressed(Path(report) / "rooms_cells.npz", cells=keys_c, bake=Yc, bake_all=Y0, land=land, scale=s,
                            level=level, fixture=fx, area=np.bincount(inv, w, nc),
                            shaders=np.array([lamps[i]["shader"] for i in order]),
                            group_cells=np.array([lamps[i]["cell"] for i in order]))
    lit = (Yc > 0) & (Fc > 0)
    wc = np.bincount(inv, w, nc)
    lr = np.log(np.maximum(Fc[lit], 1e-30) / Yc[lit])
    spread = float(np.exp(np.sqrt((wc[lit] * lr ** 2).sum() / wc[lit].sum())))
    total = float((w * (Xl @ s)).sum() / max(float(Yc.sum()), 1e-30))  # against the bake less the sky
    log("rooms: %d lamp groups in %g wu cells; level scale %.4g; traced / bake over the level %.3f; per cell "
        "(area-weighted) within x%.2f typically" % (len(order), cell, k0, total, spread))
    # Groups held at the 20x limit: the bake's light there has no source among the BSP's lamps (d20's rooms, lit
    # in the bake by what the tags do not give), and the cell's mean was met by the few texels beside a strip,
    # its near-field peak, while the rest of the room stays dark: the game shows those rooms black with hot spots.
    pw = np.array([lamps[i]["power"] * float(np.array(lamps[i]["color"]) @ display.LUMA) * lamps[i]["area"]
                   for i in order]) * s
    at_limit = s >= level * 20 * 0.999
    limit_share = float(pw[at_limit].sum() / max(pw.sum(), 1e-30))
    if limit_share > 0.05:
        log("rooms: %d groups held at the 20x limit, %.0f%% of the lamps' power: their cells' light is not the "
            "lamps' (near-field peaks meet the cell's mean); not a file to ship" % (int(at_limit.sum()), 100 * limit_share))
    scen, bsp, h = scenario_key(sc)
    doc = {"format": FORMAT, "map": (sc.camera.get("map") or Path(folder).name.split("_")[0]), "scenario": scen,
           "bsp_index": bsp, "solved_from": {"bsp_and_lightmaps_sha256_16": h, "scene": Path(folder).name},
           "mode": "rooms",
           "units": "radiance in the game's linear light units: a white Lambertian surface lit by a source of radiance L "
                    "over its whole hemisphere shows L; the lightmap's texel v as the game shows it is v^2.2 of these units. "
                    "Positions in Halo world units (1 wu = 10 ft), z up",
           "fit": {"how": "the tag's lamps at the tag's color x relative power; one scale per lamp group (a shader's "
                          "surfaces in one %g wu cell), set so each cell's traced mean light equals the bake's" % cell,
                   "level_scale": round(k0, 6), "traced_over_bake_level": round(total, 4),
                   "cell_brightness_spread": round(spread, 3), "cell_wu": cell, "spp": spp,
                   "groups_at_limit": int(at_limit.sum()), "power_share_at_limit": round(limit_share, 4),
                   "sky": {"bake_share": round(sky_share, 4), "cells_left_to_it_bake_share": round(out_share, 4),
                           "scales": {keys[j][4:]: round(float(k), 6) for j, k in zip(si, ks)},
                           "how": "the sky's tag parts traced too; their scales fitted where the lamps at the level "
                                  "scale give under %g of the bake's light, their light taken off the bake before the "
                                  "lamps are matched; a cell the sky gives all but %g of is left to it. Not in this "
                                  "file: the sky is traced from its own tags apart" % (outside, outside)},
                   "samples": int(len(w)), "samples_used": int((w > 0).sum()),
                   "transport": "BSP and the level's scenery/machines, diffuse bounces off their albedos, every bounce"},
           "shader_emitters": [], "sky": {"suns": []}, "ambient_floor": {"radiance": [0.0, 0.0, 0.0]},
           "added_emitters": [], "lamps": []}
    for j, i in enumerate(order):
        L = lamps[i]
        lo = (np.array(L["cell"]) * cell).tolist()
        doc["lamps"].append({
            "id": j, "shader": L["shader"], "region": {"min": lo, "max": [x + cell for x in lo]},
            "radiance": [round(c * L["power"] * EMISSION_RADIANCE_PER_POWER * float(s[j]), 6) for c in L["color"]],
            "scale": round(float(s[j]), 6), "scale_over_level": round(float(s[j] / level[j]), 4), "tag_power": L["power"],
            "tag_color": L["color"], "two_sided": L["two_sided"], "area_wu2": round(L["area"], 4),
            **({"fixture": True, "from": "a baked object's glowing part (fixture_glow): the BSP lamps' median tag "
                "power as its prior"} if L.get("fixture") else {}),
            "triangles": [[round(float(x), 4) for x in t.ravel()] for t in L["triangles"]]})
    doc["seconds"] = round(time.time() - t0, 1)
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1) + "\n")
    log("wrote %s: %d lamp groups, %.0f s" % (out, len(doc["lamps"]), time.time() - t0))
    return doc
