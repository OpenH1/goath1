"""The truth's path tracer: Mitsuba's unidirectional path tracer (next event estimation with multiple
importance sampling, Russian roulette), written out so that shadow rays pass through cutouts.

Mitsuba's own `path` integrator lets a camera or bounce ray through an alpha-tested surface (its `mask`
BSDF) but stops every shadow ray there, so light behind a grate or foliage reaches a pixel only by
chance hits: from a small sun that is fireflies of 10^4 (b30). Here a shadow ray carries the surfaces'
null transmission (1 - opacity), and a path ray passes a cutout with that probability without counting
a bounce. Everything else follows Mitsuba's prb.py.

Each path's light is split by its length as it is traced: emission (1 vertex: what the camera sees of
emitters and the sky), direct (2: one bounce) and indirect (more), so the parts sum to the whole with
no extra renders. The loops are symbolic (dr.while_loop): one kernel per pass.
"""

import drjit as dr
import mitsuba as mi

MAX_PASSES = 16  # surfaces a ray may pass through between two vertices


def _mis(a, b):
    return dr.detach(dr.select(a > 0, a / (a + b), 0.0))


def _null_T(si, active):
    """The fraction of light a surface lets through (1 - opacity for a cutout, 1 for a null surface)."""
    t = si.bsdf().eval_null_transmission(si, active)
    return dr.select(active & si.is_valid(), t, 0.0)


def intersect_through(scene, sampler, ray, beta, active):
    """The first surface the ray stops at: it passes each cutout with the probability of its null
    transmission (its mean over the channels), the throughput weighted by transmission / probability."""
    si = scene.ray_intersect(ray, active)
    passing = mi.Bool(active)
    i = mi.UInt32(0)

    def cond(si, beta, passing, i, sampler):
        return passing & (i < MAX_PASSES)

    def body(si, beta, passing, i, sampler):
        t = _null_T(si, passing)
        p = dr.mean(t)
        passing = passing & si.is_valid() & (p > 0) & (sampler.next_1d(passing) < p)
        beta = dr.select(passing, beta * t / dr.maximum(p, 1e-8), beta)
        si2 = scene.ray_intersect(si.spawn_ray(ray.d), passing)
        si = dr.select(passing, si2, si)
        return si, beta, passing, i + 1, sampler

    si, beta, passing, i, sampler = dr.while_loop((si, beta, passing, i, sampler), cond, body)
    return si, beta


def shadow_T(scene, si, ds, active):
    """The transmission from si to the sampled emitter point, through cutouts."""
    ray = si.spawn_ray_to(ds.p)
    T = mi.Spectrum(1.0)
    act = mi.Bool(active)
    i = mi.UInt32(0)

    def cond(ray, T, act, i):
        return act & (i < MAX_PASSES)

    def body(ray, T, act, i):
        hit = scene.ray_intersect(ray, act)
        blocked = act & hit.is_valid()
        t = _null_T(hit, blocked)
        T = dr.select(blocked, T * t, T)
        act = blocked & (dr.max(t) > 0)
        ray = dr.select(act, hit.spawn_ray_to(ds.p), ray)
        return ray, T, act, i + 1

    ray, T, act, i = dr.while_loop((ray, T, act, i), cond, body)
    return dr.select(act, 0.0, T)


def trace(scene, sampler, ray, rr_depth=5, max_depth=1024):
    """(emission, direct, indirect): the light of paths of 1, 2 and more vertices, in one pass."""
    ctx = mi.BSDFContext()
    Le_, Ld_, Li_ = mi.Spectrum(0.0), mi.Spectrum(0.0), mi.Spectrum(0.0)
    # (demodulated parts: the light leaving the first hit divided by its diffuse albedo, so the light
    # arriving there; and the one-bounce indirect light, paths of exactly 3 vertices)
    I1_, Dd_, Id_, I1d_ = mi.Spectrum(0.0), mi.Spectrum(0.0), mi.Spectrum(0.0), mi.Spectrum(0.0)
    inv_a0 = mi.Spectrum(0.0)
    beta = mi.Spectrum(1.0)
    active = mi.Bool(True)
    prev_si = dr.zeros(mi.SurfaceInteraction3f)
    prev_pdf = mi.Float(1.0)
    prev_delta = mi.Bool(True)
    depth = mi.UInt32(0)

    def add(depth_of_path, value, acc, inv_a0, mask):
        Le_, Ld_, Li_, I1_, Dd_, Id_, I1d_ = acc
        Le_ = dr.select(mask & (depth_of_path == 1), Le_ + value, Le_)
        Ld_ = dr.select(mask & (depth_of_path == 2), Ld_ + value, Ld_)
        Li_ = dr.select(mask & (depth_of_path > 2), Li_ + value, Li_)
        I1_ = dr.select(mask & (depth_of_path == 3), I1_ + value, I1_)
        Dd_ = dr.select(mask & (depth_of_path == 2), Dd_ + value * inv_a0, Dd_)
        Id_ = dr.select(mask & (depth_of_path > 2), Id_ + value * inv_a0, Id_)
        I1d_ = dr.select(mask & (depth_of_path == 3), I1d_ + value * inv_a0, I1d_)
        return Le_, Ld_, Li_, I1_, Dd_, Id_, I1d_

    def cond(sampler, ray, beta, active, prev_si, prev_pdf, prev_delta, depth, acc, inv_a0):
        return active

    def body(sampler, ray, beta, active, prev_si, prev_pdf, prev_delta, depth, acc, inv_a0):
        si, beta = intersect_through(scene, sampler, ray, beta, active)
        # Light from an emitter hit here (or the sky when missed): a path of depth + 1 vertices.
        ds = mi.DirectionSample3f(scene, si=si, ref=prev_si)
        em_pdf = scene.pdf_emitter_direction(prev_si, ds, active & ~prev_delta)
        Le = beta * _mis(prev_pdf, em_pdf) * ds.emitter.eval(si, active)
        acc = add(depth + 1, Le, acc, inv_a0, active)
        active &= si.is_valid() & (depth + 1 < max_depth)
        bsdf = si.bsdf(ray)
        a0 = bsdf.eval_diffuse_reflectance(si, active & (depth == 0))
        inv_a0 = dr.select(active & (depth == 0), dr.select(a0 > 1e-3, dr.rcp(a0), 0.0), inv_a0)
        # Next event: a light sample, its shadow ray through cutouts: a path of depth + 2 vertices.
        active_em = active & mi.has_flag(bsdf.flags(), mi.BSDFFlags.Smooth)
        ds_em, em_w = scene.sample_emitter_direction(si, sampler.next_2d(active), False, active_em)
        active_em &= ds_em.pdf != 0
        T = shadow_T(scene, si, ds_em, active_em)
        wo = si.to_local(ds_em.d)
        bsdf_val, bsdf_pdf = bsdf.eval_pdf(ctx, si, wo, active_em)
        mis_em = dr.select(ds_em.delta, 1.0, _mis(ds_em.pdf, bsdf_pdf))
        acc = add(depth + 2, beta * mis_em * bsdf_val * em_w * T, acc, inv_a0, active_em)
        # The next direction from the BSDF.
        bs, bw = bsdf.sample(ctx, si, sampler.next_1d(active), sampler.next_2d(active), active)
        beta = dr.select(active, beta * bw, beta)
        prev_si = dr.select(active, si, prev_si)
        prev_pdf = dr.select(active, bs.pdf, prev_pdf)
        prev_delta = dr.select(active, mi.has_flag(bs.sampled_type, mi.BSDFFlags.Delta), prev_delta)
        ray = dr.select(active, si.spawn_ray(si.to_world(bs.wo)), ray)
        active &= (bs.pdf > 0) & (dr.max(beta) > 0)
        depth = dr.select(active, depth + 1, depth)
        q = dr.minimum(dr.max(beta), 0.95)
        rr = active & (depth >= rr_depth)
        survive = sampler.next_1d(rr) < q
        beta = dr.select(rr, dr.select(survive, beta / dr.maximum(q, 1e-8), 0.0), beta)
        active &= ~rr | survive
        return sampler, ray, beta, active, prev_si, prev_pdf, prev_delta, depth, acc, inv_a0

    state = dr.while_loop((sampler, ray, beta, active, prev_si, prev_pdf, prev_delta, depth,
                           (Le_, Ld_, Li_, I1_, Dd_, Id_, I1d_), inv_a0), cond, body)
    return state[-2]


# emission, direct, indirect sum to the light; indirect1 is the one-bounce share of indirect; *_demod are
# direct, indirect and indirect1 divided by the first hit's diffuse albedo (the light arriving there)
PARTS = ("emission", "direct", "indirect", "indirect1", "direct_demod", "indirect_demod", "indirect1_demod")
MAX_LANES = 1 << 22  # samples a launch: evaluated (wavefront) loops keep each lane's state in memory


def render_pass(scene, spp, seed, parts=False, max_depth=1024, centers=False):
    """One pass of `spp` samples per pixel (uniform over the pixel: a box filter): the full light as
    an H x W x 3 array, or with parts=True a dict of emission, direct and indirect (which sum to it).
    Launched in chunks of at most MAX_LANES samples."""
    import numpy as np
    sensor = scene.sensors()[0]
    W, H = (int(v) for v in sensor.film().size())
    step = max(1, MAX_LANES // (W * H))
    out = {k: np.zeros((H, W, 3)) for k in PARTS}
    done = 0
    while done < spp:
        k = min(step, spp - done)
        part = _render_chunk(scene, sensor, W, H, k, seed * 4096 + done, max_depth, centers)
        for name in out:
            out[name] += part[name] * k
        done += k
    for name in out:
        out[name] /= spp
    if parts:
        return out
    return out["emission"] + out["direct"] + out["indirect"]


def _render_chunk(scene, sensor, W, H, spp, seed, max_depth, centers):
    import numpy as np
    n = W * H * spp
    sampler = mi.load_dict({"type": "independent"})
    # An opaque seed: one compiled kernel serves every pass (a literal would recompile each one).
    sampler.seed(dr.opaque(mi.UInt32, seed), n)
    idx = dr.arange(mi.UInt32, n)
    pix = idx // spp
    # centers: each sample at its pixel's centre (as a rasterizer shades it) rather than over its area
    jx, jy = sampler.next_1d(), sampler.next_1d()
    x = mi.Float(pix % W) + (0.5 if centers else jx)
    y = mi.Float(pix // W) + (0.5 if centers else jy)
    ray, w = sensor.sample_ray(0.0, 0.5, mi.Point2f(x / W, y / H), mi.Point2f(0.5, 0.5))
    parts = trace(scene, sampler, ray, max_depth=max_depth)
    out = {}
    for name, L in zip(PARTS, parts):
        L = L * w
        chans = []
        for c in (L.x, L.y, L.z):
            acc = dr.zeros(mi.Float, W * H)
            dr.scatter_reduce(dr.ReduceOp.Add, acc, dr.select(dr.isfinite(c), c, 0.0), pix)
            chans.append(acc)
        dr.eval(chans)
        out[name] = np.stack([np.asarray(c) for c in chans], -1).reshape(H, W, 3) / spp
    return out
