"""Mitsuba plugins written in Python for Halo's light sources and its detail-mapped albedo.

They are traced into Mitsuba's kernels like the built-in plugins (cuda_ad_rgb or llvm_ad_rgb).

Units (see README.md, "Units"): a light's `intensity` here is already pi * power * color, so a white
Lambertian surface facing it at distance 1 wu shows radiance power * color. The sky's `ambient` is a
radiance, so an unoccluded white floor shows radiance ambient. A sun's `irradiance` is pi * power * color.
"""

import math
import os

_BUMP_SIGN = tuple(float(c) for c in os.environ.get("RT_TRUTH_BUMP_SIGN", "1,1").split(","))

import drjit as dr
import mitsuba as mi


def _normalize(v):
    n = math.sqrt(sum(c * c for c in v))
    return [c / n for c in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def light_frame(direction, up=None):
    """(x, y, z) axes of a light: z along `direction`, x to the light's right, y toward `up` (world z when
    not given), as a camera at the light would see. The gel is laid on x (left to right) and y (bottom to
    top), like a slide in a projector."""
    z = _normalize(direction)
    up = up or [0.0, 0.0, 1.0]
    if abs(sum(a * b for a, b in zip(z, _normalize(up)))) > 0.999:
        up = [1.0, 0.0, 0.0]
    x = _normalize(_cross(z, up))
    y = _cross(x, z)
    return x, y, z


def _opq(v):
    """A constant as an opaque value (a kernel parameter, not a literal)."""
    return dr.opaque(mi.Float, float(v))


def _opq_list(values):
    a = mi.Float(values)
    dr.make_opaque(a)
    return a


def _vec(props, name, default=None):
    if not props.has_property(name):
        return default
    v = props[name]
    return [float(v[0]), float(v[1]), float(v[2])]


# Distance falloff modes (`distance_mode`):
#   inverse_square         1/d^2, the physical point light
#   inverse_square_cutoff  1/d^2 inside `radius`, 0 beyond
#   window_radius          1/d^2 * clamp(1 - (d/radius)^4, 0, 1)^2, a smooth end at `radius`
#   halo_radius            the tag's falloff curve, clamp(1 - d/radius, 0, 1)^exponent, with no 1/d^2
#   none                   no distance falloff
#   table                  the game's distance attenuation table at d / radius (no 1/d^2), 0 past radius
# Angular modes (`angular_mode`, spot lights only), between the inner and outer half-angles:
#   linear_angle  linear in the angle;  linear_cos  linear in cos;  smoothstep_cos  smoothstep in cos
# raised to `angular_exponent`.
DISTANCE_MODES = ("inverse_square", "inverse_square_cutoff", "window_radius", "halo_radius", "none", "table")
ANGULAR_MODES = ("linear_angle", "linear_cos", "smoothstep_cos", "none", "cos")
# Gels: "projector", a picture across the outer cone; "cube", a cube map strip (6 faces side by side in
# D3D order +x -x +y -y +z -z with D3D face UVs) looked up along (P - light) in the frame
# (forward, left, up), left = forward x up (rasterizer_xbox_environment.c:1244-1255: (-forward, -left,
# -up) dotted with light - P).


class HaloLight(mi.Emitter):
    """A point or spot light of a Halo light tag, with the spot's gel projected through its cone."""

    def __init__(self, props):
        mi.Emitter.__init__(self, props)
        self.position = _vec(props, "position")
        # Where the gel and cone are projected from (the game's drawn light); light leaves `position`.
        self.gel_origin = _vec(props, "gel_origin", self.position)
        self.intensity = _vec(props, "intensity")
        self.kind = props.get("kind", "point")
        self.distance_mode = props.get("distance_mode", "inverse_square")
        self.distance_exponent = props.get("distance_exponent", 1.0)
        self.radius = props.get("radius", 0.0)
        assert self.distance_mode in DISTANCE_MODES, self.distance_mode
        ex, ey, ez = light_frame(_vec(props, "direction", [0, 0, -1]), _vec(props, "up"))
        self.ex, self.ey, self.ez = mi.Vector3f(ex), mi.Vector3f(ey), mi.Vector3f(ez)
        self.half_in = math.radians(props.get("inner_half_deg", 0.0))
        self.half_out = math.radians(props.get("outer_half_deg", 90.0))
        self.angular_mode = props.get("angular_mode", "linear_angle")
        self.angular_exponent = props.get("angular_exponent", 1.0)
        assert self.angular_mode in ANGULAR_MODES, self.angular_mode
        if self.kind != "spot" and not props.has_property("cos_falloff") and self.angular_mode != "cos":
            self.angular_mode = "none"
        self.gel = props["gel"] if props.has_property("gel") else None
        self.gel_kind = props.get("gel_kind", "projector")
        self.gel_face_px = props.get("gel_face_px", 64)
        self.table = props["table"] if props.has_property("table") else None
        self.table_n = props.get("table_n", 2)
        self.cos_falloff = props.get("cos_falloff", math.cos(self.half_in))
        self.cos_cutoff = props.get("cos_cutoff", math.cos(self.half_out))
        fwd = _normalize(_vec(props, "direction", [0, 0, -1]))
        up = _normalize(_vec(props, "up") or [0.0, 0.0, 1.0])
        left = _normalize(_cross(fwd, up))
        # The cube's frame for (P - light): forward, left, up (up made perpendicular to forward).
        # rasterizer_xbox_environment.c:1244-1255 loads (-forward, -left, -up) and dots them with
        # (light - P); the flashlight's gel has its beam on the +x face (centre 0.99, -x face black).
        upp = _cross(left, fwd)
        self.cx, self.cy, self.cz = (mi.Vector3f(v) for v in (fwd, left, upp))
        self.m_flags = mi.EmitterFlags.DeltaPosition

    # Radiant intensity toward world direction w (unit, pointing away from the light).
    def radiant_intensity(self, w, active=True):
        f = mi.Spectrum(self.intensity)
        if self.kind != "spot" and self.gel is None and self.angular_mode == "none":
            return f
        lx, ly, lz = dr.dot(w, self.ex), dr.dot(w, self.ey), dr.dot(w, self.ez)
        cos_in, cos_out = self.cos_falloff, self.cos_cutoff
        if self.gel is not None and self.gel_kind == "cube":
            f = f * self.cube_gel(w, active)
        if self.angular_mode == "none":
            a = mi.Float(1.0)
        elif self.angular_mode == "cos":  # a Lambertian disk facing forward
            a = dr.maximum(lz, 0.0)
        elif self.angular_mode == "linear_angle":
            th = dr.acos(dr.clip(lz, -1.0, 1.0))
            a = dr.clip((self.half_out - th) / max(self.half_out - self.half_in, 1e-6), 0.0, 1.0)
        else:
            t = dr.clip((lz - cos_out) / max(cos_in - cos_out, 1e-6), 0.0, 1.0)
            a = t if self.angular_mode == "linear_cos" else t * t * (3.0 - 2.0 * t)
        if self.angular_exponent != 1.0:
            a = dr.power(a, self.angular_exponent)
        if self.angular_mode not in ("none", "cos"):
            a = dr.select(lz > cos_out, a, 0.0)
        f = f * a
        if self.gel is not None and self.gel_kind == "projector":
            t_out = math.tan(min(self.half_out, math.radians(89.0)))
            inv = 1.0 / (dr.maximum(lz, 1e-6) * t_out)
            si = dr.zeros(mi.SurfaceInteraction3f)
            # Image left to right along +x, top to bottom along -y.
            si.uv = mi.Point2f(0.5 + 0.5 * lx * inv, 0.5 - 0.5 * ly * inv)
            inside = (lz > 0) & (si.uv.x >= 0) & (si.uv.x <= 1) & (si.uv.y >= 0) & (si.uv.y <= 1)
            g = self.gel.eval(si, active & inside)
            f = f * dr.select(inside, g, 0.0)
        return f

    def cube_gel(self, w, active):
        x, y, z = dr.dot(w, self.cx), dr.dot(w, self.cy), dr.dot(w, self.cz)
        ax, ay, az = dr.abs(x), dr.abs(y), dr.abs(z)
        on_x = (ax >= ay) & (ax >= az)
        on_y = ~on_x & (ay >= az)
        on_z = ~on_x & ~on_y
        # D3D: +x (sc -z, tc -y), -x (+z, -y), +y (+x, +z), -y (+x, -z), +z (+x, -y), -z (-x, -y).
        face = dr.select(on_x, dr.select(x > 0, 0, 1), dr.select(on_y, dr.select(y > 0, 2, 3), dr.select(z > 0, 4, 5)))
        ma = dr.select(on_x, ax, dr.select(on_y, ay, az))
        sc = dr.select(on_x, dr.select(x > 0, -z, z), dr.select(on_y, x, dr.select(z > 0, x, -x)))
        tc = dr.select(on_x, -y, dr.select(on_y, dr.select(y > 0, z, -z), -y))
        u = 0.5 * (sc / ma + 1.0)
        v = 0.5 * (tc / ma + 1.0)
        # Keep the lookup half a texel inside its face so the strip's neighbours never bleed in.
        n = 6.0 * self.gel_face_px if hasattr(self, "gel_face_px") else 6.0 * 64
        si = dr.zeros(mi.SurfaceInteraction3f)
        eps = 0.5 / (n / 6.0)
        si.uv = mi.Point2f((mi.Float(face) + dr.clip(u, eps, 1 - eps)) / 6.0, dr.clip(v, eps, 1 - eps))
        return self.gel.eval(si, active)

    def distance_factor(self, dist2):
        m, r = self.distance_mode, max(self.radius, 1e-6)
        if m == "none":
            return mi.Float(1.0)
        if m == "table":
            t = dr.sqrt(dist2) / r
            n = self.table_n
            si = dr.zeros(mi.SurfaceInteraction3f)
            si.uv = mi.Point2f((dr.clip(t, 0.0, 1.0) * (n - 1) + 0.5) / n, 0.5)
            return dr.select(t < 1.0, self.table.eval_1(si), 0.0)
        if m == "halo_radius":
            return dr.power(dr.clip(1.0 - dr.sqrt(dist2) / r, 0.0, 1.0), self.distance_exponent)
        inv = dr.rcp(dist2)
        if m == "inverse_square_cutoff":
            return dr.select(dist2 < r * r, inv, 0.0)
        if m == "window_radius":
            q = dist2 / (r * r)
            return inv * dr.square(dr.clip(1.0 - q * q, 0.0, 1.0))
        return inv

    def _profile_dir(self, p_recv):
        g = mi.Point3f(self.gel_origin)
        return dr.normalize(p_recv - g)

    def sample_direction(self, it, sample, active=True):
        p = mi.Point3f(self.position)
        d = p - it.p
        dist2 = dr.squared_norm(d)
        dist = dr.sqrt(dist2)
        ds = dr.zeros(mi.DirectionSample3f)
        ds.p = p
        ds.n = mi.Normal3f(0, 0, 0)
        ds.uv = mi.Point2f(0, 0)
        ds.time = it.time
        ds.pdf = mi.Float(1.0)
        ds.delta = mi.Bool(True)
        ds.d = d / dist
        ds.dist = dist
        ds.emitter = mi.EmitterPtr(self)
        value = self.intensity_toward(it.p, -ds.d, active) * self.distance_factor(dist2)
        return ds, dr.select(active, value, 0.0)

    def intensity_toward(self, p_recv, w_from_emitter, active):
        """The angular profile (cone, gel) looked up from the gel origin; the emitter's own cosine
        (angular mode "cos") from the emitter."""
        if self.gel_origin == self.position:
            return self.radiant_intensity(w_from_emitter, active)
        f = self.radiant_intensity(self._profile_dir(p_recv), active)
        if self.angular_mode == "cos":
            lz_g = dr.dot(self._profile_dir(p_recv), self.ez)
            lz_e = dr.dot(w_from_emitter, self.ez)
            f = f * dr.select(lz_g > 0, dr.maximum(lz_e, 0.0) / lz_g, 0.0)
        return f

    def eval_direction(self, it, ds, active=True):
        d = mi.Point3f(self.position) - it.p
        dist2 = dr.squared_norm(d)
        return self.intensity_toward(it.p, -d / dr.sqrt(dist2), active) * self.distance_factor(dist2)

    def pdf_direction(self, it, ds, active=True):
        return mi.Float(0.0)

    def eval(self, si, active=True):
        return mi.Spectrum(0.0)

    def sample_wavelengths(self, si, sample, active=True):
        return mi.Spectrum(1.0), mi.Spectrum(0.0)

    def sample_ray(self, *args, **kwargs):
        raise NotImplementedError("halo_light: light tracing is not supported")

    def traverse(self, cb):
        pass

    def bbox(self):
        return mi.ScalarBoundingBox3f(mi.ScalarPoint3f(self.position))

    def to_string(self):
        return f"HaloLight[{self.kind} at {self.position}]"


class HaloSky(mi.Emitter):
    """The sky: a constant ambient radiance plus sun discs of their angular size (one environment emitter,
    since Mitsuba allows only one)."""

    def __init__(self, props):
        mi.Emitter.__init__(self, props)
        self.ambient = _vec(props, "ambient", [0, 0, 0])
        self.center = _vec(props, "center", [0, 0, 0])
        self.radius = props.get("scene_radius", 1000.0)
        self.suns = []
        i = 0
        while props.has_property(f"sun{i}_to_sun"):
            to_sun = _normalize(_vec(props, f"sun{i}_to_sun"))
            half = math.radians(max(props.get(f"sun{i}_angular_diameter_deg", 0.53), 1e-3) * 0.5)
            irr = _vec(props, f"sun{i}_irradiance")
            # Disc radiance L with irradiance E on a plane facing the sun: E = L * pi * sin^2(half).
            L = [c / (math.pi * math.sin(half) ** 2) for c in irr]
            x, y, z = light_frame(to_sun)
            self.suns.append(dict(z=mi.Vector3f(z), x=mi.Vector3f(x), y=mi.Vector3f(y),
                                  cos_half=math.cos(half), L=L))
            i += 1
        has_amb = max(self.ambient) > 0
        self.p_sun = (0.5 if has_amb else 1.0) if self.suns else 0.0
        self.m_flags = mi.EmitterFlags.Infinite

    def _eval_dir(self, w):
        v = mi.Spectrum(self.ambient)
        for s in self.suns:
            v += dr.select(dr.dot(w, s["z"]) >= s["cos_half"], mi.Spectrum(s["L"]), 0.0)
        return v

    def _pdf_dir(self, w):
        pdf = mi.Float((1.0 - self.p_sun) / (4.0 * math.pi))
        n = len(self.suns)
        for s in self.suns:
            cone = 1.0 / (2.0 * math.pi * (1.0 - s["cos_half"]))
            pdf += dr.select(dr.dot(w, s["z"]) >= s["cos_half"], self.p_sun / n * cone, 0.0)
        return pdf

    def eval(self, si, active=True):
        return dr.select(active, self._eval_dir(-si.wi), 0.0)

    def sample_direction(self, it, sample, active=True):
        u = mi.Point2f(sample)
        w = mi.warp.square_to_uniform_sphere(u)
        if self.suns:
            p = self.p_sun
            pick_sun = u.x < p
            ux = dr.select(pick_sun, u.x / p, (u.x - p) / max(1.0 - p, 1e-9))
            uu = mi.Point2f(ux, u.y)
            w = mi.warp.square_to_uniform_sphere(uu)
            n = len(self.suns)
            for k, s in enumerate(self.suns):
                # Sun k takes the slice [k/n, (k+1)/n) of the remapped x.
                lo, hi = k / n, (k + 1) / n
                sel = pick_sun & (ux >= lo) & (ux < hi)
                uk = mi.Point2f((ux - lo) * n, u.y)
                local = mi.warp.square_to_uniform_cone(uk, s["cos_half"])
                ws = s["x"] * local.x + s["y"] * local.y + s["z"] * local.z
                w = dr.select(sel, ws, w)
        pdf = self._pdf_dir(w)
        dist = 2.0 * (self.radius + dr.norm(it.p - mi.Point3f(self.center)))
        ds = dr.zeros(mi.DirectionSample3f)
        ds.p = it.p + w * dist
        ds.n = -w
        ds.uv = mi.Point2f(0, 0)
        ds.time = it.time
        ds.pdf = pdf
        ds.delta = mi.Bool(False)
        ds.d = w
        ds.dist = dist
        ds.emitter = mi.EmitterPtr(self)
        value = dr.select(active & (pdf > 0), self._eval_dir(w) / pdf, 0.0)
        return ds, value

    def pdf_direction(self, it, ds, active=True):
        return dr.select(active, self._pdf_dir(ds.d), 0.0)

    def eval_direction(self, it, ds, active=True):
        return dr.select(active, self._eval_dir(ds.d), 0.0)

    def sample_wavelengths(self, si, sample, active=True):
        return mi.Spectrum(1.0), mi.Spectrum(0.0)

    def sample_ray(self, *args, **kwargs):
        raise NotImplementedError("halo_sky: light tracing is not supported")

    def traverse(self, cb):
        pass

    def bbox(self):
        return mi.ScalarBoundingBox3f()

    def to_string(self):
        return f"HaloSky[ambient {self.ambient}, {len(self.suns)} sun(s)]"


# How a detail map combines with the color so far (`*_mode`), Halo's detail functions, in the game's
# gamma space:
#   double_biased_multiply  c * d * 2;  multiply  c * d;  biased_add  c + 2 * d - 1
ALBEDO_MODES = ("double_biased_multiply", "multiply", "biased_add", "none")


def _combine(c, d, mode):
    if mode == "double_biased_multiply":
        return c * d * 2.0
    if mode == "multiply":
        return c * d
    if mode == "biased_add":
        return c + 2.0 * d - 1.0
    return c


class HaloAlbedo(mi.Texture):
    """A shader's diffuse albedo as the game's texture pass makes it (rasterizer_xbox_environment.c's
    _rasterizer_environment_diffuse_texture_draw, rasterizer_xbox_models.c), in gamma space:
      detail = (1 - secondary.a) * secondary + secondary.a * detail   (with a secondary detail map)
      color  = clamp(base combined with detail, 0, 1), then with micro detail
    Maps stored as linear (value^gamma, the .lin maps) are taken back to gamma space first, and the
    result is made linear again: albedo = clamp(color, 0, 1)^gamma. Each map has its own UV scale."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        g = lambda k: props[k] if props.has_property(k) else None
        self.base, self.detail, self.secondary = g("base"), g("detail"), g("secondary")
        self.secondary_alpha, self.micro = g("secondary_alpha"), g("micro")
        self.base_scale = [_opq(props.get("base_scale_u", 1.0)), _opq(props.get("base_scale_v", 1.0))]
        self.detail_scale = [_opq(props.get("detail_scale_u", 1.0)), _opq(props.get("detail_scale_v", 1.0))]
        self.secondary_scale = _opq(props.get("secondary_scale", 1.0))
        self.micro_scale = _opq(props.get("micro_scale", 1.0))
        self.detail_mode = props.get("detail_mode", "double_biased_multiply")
        self.micro_mode = props.get("micro_mode", "double_biased_multiply")
        self.gamma = props.get("gamma", 1.0)
        self.mean_value = props.get("mean_value", 0.5)
        assert self.detail_mode in ALBEDO_MODES and self.micro_mode in ALBEDO_MODES

    def _at(self, tex, si, su, sv, active):
        si2 = mi.SurfaceInteraction3f(si)
        si2.uv = mi.Point2f(si.uv.x * su, si.uv.y * sv)
        v = tex.eval(si2, active)
        return v if self.gamma == 1.0 else dr.power(dr.maximum(v, 0.0), 1.0 / self.gamma)

    def _at1(self, tex, si, s, active):
        si2 = mi.SurfaceInteraction3f(si)
        si2.uv = si.uv * s
        return tex.eval_1(si2, active)

    def eval(self, si, active=True):
        bu, bv = self.base_scale
        c = self._at(self.base, si, bu, bv, active)
        d = None
        if self.detail is not None:
            d = self._at(self.detail, si, self.detail_scale[0], self.detail_scale[1], active)
        if self.secondary is not None:
            s = self._at(self.secondary, si, self.secondary_scale, self.secondary_scale, active)
            a = self._at1(self.secondary_alpha, si, self.secondary_scale, active) if self.secondary_alpha is not None else 1.0
            d = s if d is None else (1.0 - a) * s + a * d
        if d is not None:
            c = dr.clip(_combine(c, d, self.detail_mode), 0.0, 1.0)
        if self.micro is not None:
            m = self._at(self.micro, si, self.micro_scale, self.micro_scale, active)
            c = dr.clip(_combine(c, m, self.micro_mode), 0.0, 1.0)
        c = dr.clip(c, 0.0, 1.0)
        return c if self.gamma == 1.0 else dr.power(c, self.gamma)

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return self.mean_value

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloAlbedo[%s, %s]" % (self.detail_mode, self.micro_mode)


class HaloBump(mi.BSDF):
    """A bump-mapped surface as the game shades it: the normal n = normalize(T b.x + B b.y + N max(b.z,
    0.05)), b = the bump map's texel x 2 - 1, T and B the vertex's own tangent and binormal (interpolated,
    as the game's are), N the interpolated normal (xgpu_spirv.c:5811-5821, xgpu_msl.c:2917). The nested
    BSDF is evaluated in the frame of n, as Mitsuba's normalmap does."""

    def __init__(self, props):
        mi.BSDF.__init__(self, props)
        self.nested = props["bsdf"]
        self.bump = props["bump"]
        # The bump map's mip levels (box-filtered), chosen by the pixel's footprint as a GPU's trilinear
        # sampling does, so the truth's bump detail compares like for like with the real-time render.
        # All levels side by side in one texture (bump_mips), level k at x offset bump_mip_x<k>: two lookups
        # a sample, whatever the level count (a texture per level made the kernels huge).
        self.mips = props["bump_mips"] if props.has_property("bump_mips") else None
        self.n_levels = props.get("bump_levels", 0)
        pad = lambda v: v + [v[-1] if v else 0.0] * (16 - len(v))
        self.mip_x = _opq_list(pad([props.get("bump_mip_x%d" % k, 0.0) for k in range(self.n_levels)]))
        self.mip_w = _opq_list(pad([props.get("bump_mip_w%d" % k, 1.0) for k in range(self.n_levels)]))
        self.mip_h = _opq_list(pad([props.get("bump_mip_h%d" % k, 1.0) for k in range(self.n_levels)]))
        self.top = _opq(max(self.n_levels - 1, 0))
        # (constants as opaque values: every material's code is then the same and the kernel compiles one
        # function for them all; Python numbers would be literals, a function per material)
        self.size = [_opq(props.get("bump_width", 1.0)), _opq(props.get("bump_height", 1.0))]
        self.scale = [_opq(props.get("bump_scale_u", 1.0)), _opq(props.get("bump_scale_v", 1.0))]
        self.pixel_angle = props.get("pixel_angle", 0.0)
        self.pixel_angle_o = _opq(self.pixel_angle)
        self.m_flags = self.nested.flags()
        self.m_components = [self.nested.flags(i) for i in range(self.nested.component_count())]

    def _texel(self, si, active):
        if self.mips is None or self.pixel_angle <= 0:
            return self.bump.eval(si, active)
        # The footprint of a camera ray's pixel at the hit (a ray cone), in texels of level 0 along the
        # texture's more stretched axis (the GPU's isotropic choice).
        foot = si.t * self.pixel_angle_o / dr.maximum(dr.abs(mi.Frame3f.cos_theta(si.wi)), 0.05)
        tu = foot * self.scale[0] * self.size[0] / dr.maximum(dr.norm(si.dp_du), 1e-8)
        tv = foot * self.scale[1] * self.size[1] / dr.maximum(dr.norm(si.dp_dv), 1e-8)
        lod = dr.clip(dr.log2(dr.maximum(dr.maximum(tu, tv), 1.0)), 0.0, self.top)
        l0 = mi.UInt32(dr.floor(lod))
        l1 = mi.UInt32(dr.minimum(dr.floor(lod) + 1.0, self.top))
        t = lod - mi.Float(l0)
        xs, ws, hs = self.mip_x, self.mip_w, self.mip_h
        u = si.uv.x * self.scale[0]
        v = si.uv.y * self.scale[1]
        fu, fv = u - dr.floor(u), v - dr.floor(v)

        def at(level):
            si2 = mi.SurfaceInteraction3f(si)
            si2.uv = mi.Point2f(dr.gather(mi.Float, xs, level) + fu * dr.gather(mi.Float, ws, level),
                                fv * dr.gather(mi.Float, hs, level))
            return self.mips.eval(si2, active)
        return at(l0) * (1.0 - t) + at(l1) * t

    def _perturbed(self, si, active):
        T = si.shape.eval_attribute_3("vertex_halo_tangent", si, active)
        B = si.shape.eval_attribute_3("vertex_halo_binormal", si, active)
        b = self._texel(si, active) * 2.0 - 1.0
        if _BUMP_SIGN != (1.0, 1.0):  # (an experiment's sign flip, RT_TRUTH_BUMP_SIGN="sx,sy")
            b = mi.Color3f(b.x * _BUMP_SIGN[0], b.y * _BUMP_SIGN[1], b.z)
        N = si.sh_frame.n
        n = dr.normalize(T * b.x + B * b.y + N * dr.maximum(b.z, 0.05))
        n = dr.select(dr.isfinite(n.x) & (dr.squared_norm(T) > 0), n, N)
        psi = mi.SurfaceInteraction3f(si)
        psi.sh_frame = mi.Frame3f(n)
        psi.wi = psi.to_local(si.to_world(si.wi))
        return psi

    def sample(self, ctx, si, sample1, sample2, active=True):
        psi = self._perturbed(si, active)
        bs, w = self.nested.sample(ctx, psi, sample1, sample2, active)
        wo = psi.to_world(bs.wo)
        bs.wo = si.to_local(wo)
        ok = active & (dr.dot(si.n, wo) * mi.Frame3f.cos_theta(bs.wo) > 0)
        return bs, dr.select(ok, w, 0.0)

    def eval(self, ctx, si, wo, active=True):
        psi = self._perturbed(si, active)
        w = si.to_world(wo)
        ok = active & (dr.dot(si.n, w) * mi.Frame3f.cos_theta(wo) > 0)
        return dr.select(ok, self.nested.eval(ctx, psi, psi.to_local(w), ok), 0.0)

    def pdf(self, ctx, si, wo, active=True):
        psi = self._perturbed(si, active)
        w = si.to_world(wo)
        ok = active & (dr.dot(si.n, w) * mi.Frame3f.cos_theta(wo) > 0)
        return dr.select(ok, self.nested.pdf(ctx, psi, psi.to_local(w), ok), 0.0)

    def eval_pdf(self, ctx, si, wo, active=True):
        psi = self._perturbed(si, active)
        w = si.to_world(wo)
        ok = active & (dr.dot(si.n, w) * mi.Frame3f.cos_theta(wo) > 0)
        v, p = self.nested.eval_pdf(ctx, psi, psi.to_local(w), ok)
        return dr.select(ok, v, 0.0), dr.select(ok, p, 0.0)

    def eval_diffuse_reflectance(self, si, active=True):
        return self.nested.eval_diffuse_reflectance(si, active)

    def eval_null_transmission(self, si, active=True):
        return self.nested.eval_null_transmission(si, active)

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloBump[%s]" % self.nested


class HaloLightmapRadiance(mi.Texture):
    """A lightmapped surface's static radiance: its albedo (at its UV) x its lightmap page's texel (at its
    lightmap UV, the mesh attribute vertex_halo_uv2) decoded ^gamma."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        self.albedo = props["albedo"]
        self.lightmap = props["lightmap"] if props.has_property("lightmap") else None
        self.gamma = props.get("gamma", 2.2)
        # An environment shader's self-illumination as the lightmap pass adds it at the tick (rasterizer_
        # xbox_environment.c:862-898): map.r x primary + map.g x secondary + map.b x plasma, added to the
        # lightmap in gamma space before the texture pass multiplies by the albedo.
        self.si_map = props["si_map"] if props.has_property("si_map") else None
        self.si_scale = _opq(props.get("si_scale", 1.0))
        self.si_colors = [mi.Color3f(*[_opq(c) for c in _vec(props, k, [0, 0, 0])])
                          for k in ("si_primary", "si_secondary", "si_plasma")]

    def eval(self, si, active=True):
        if self.lightmap is None:  # an object's environment shader: its self-illumination alone
            lm = mi.Color3f(0.0)
        else:
            u3 = si.shape.eval_attribute_3("vertex_halo_uv2", si, active)
            si2 = mi.SurfaceInteraction3f(si)
            si2.uv = mi.Point2f(u3.x, u3.y)
            lm = dr.maximum(self.lightmap.eval(si2, active), 0.0)
        if self.si_map is not None:
            si3 = mi.SurfaceInteraction3f(si)
            si3.uv = si.uv * self.si_scale
            m = self.si_map.eval(si3, active)
            p, q, r = self.si_colors
            lm = dr.clip(lm + m.x * p + m.y * q + m.z * r, 0.0, 1.0)
        if self.gamma != 1.0:
            lm = dr.power(lm, self.gamma)
        return self.albedo.eval(si, active) * lm

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return 0.1

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloLightmapRadiance"


class HaloSelfIllum(mi.Texture):
    """A model's self-illumination as the model pass draws it at the tick: the drawn color (its function
    and external color applied, rasterizer_xbox_models.c:1975-2044) masked by the multipurpose map's green,
    added to the light before the albedo multiplies (gamma space), so its radiance is albedo x
    (mask x color)^2.2."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        self.albedo = props["albedo"]
        self.mask = props["mask"] if props.has_property("mask") else None
        self.color = mi.Color3f(*[_opq(c) for c in _vec(props, "color", [0, 0, 0])])

    def eval(self, si, active=True):
        m = mi.Float(1.0) if self.mask is None else self.mask.eval(si, active).y
        return self.albedo.eval(si, active) * dr.power(dr.clip(self.color * m, 0.0, 1.0), 2.2)

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return 0.1

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloSelfIllum"


class HaloChangeColor(mi.Texture):
    """A model's albedo with its change color as drawn: base x lerp(1, change color, mask), the mask the
    multipurpose map's blue as exported (the Xbox layout: red specular, green self-illumination, blue change
    color; the combiner reads it as T2.a, rasterizer_xbox_models.c stage 2, so the bitmap's channels are
    ordered differently in memory; the exported alpha is 1 everywhere), in gamma space, made linear (^2.2 of a .lin base is the
    base itself, so the change color is decoded ^2.2 and multiplied in)."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        self.albedo = props["albedo"]
        self.mask = props["mask"] if props.has_property("mask") else None
        self.change = mi.Color3f(*[_opq(c) for c in _vec(props, "change", [1, 1, 1])])

    def eval(self, si, active=True):
        a = mi.Float(1.0) if self.mask is None else self.mask.eval_1(si, active)  # the change-color mask
        mix = (1.0 - a) + a * self.change
        return self.albedo.eval(si, active) * dr.power(dr.clip(mix, 0.0, 1.0), 2.2)

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return 0.3

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloChangeColor"


class HaloModelLight(mi.Texture):
    """An object's static light as the game's model lighting draws it: albedo x (ambient + sum over the
    two distant lights of color x max(0, N . -direction))^2.2, the lighting in gamma space as the model
    pass adds it (object_lights.c lights_prepare_for_object_static, build_distant_lights), N the vertex
    normal."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        self.albedo = props["albedo"]
        self.ambient = mi.Color3f(*[_opq(c) for c in _vec(props, "ambient", [0, 0, 0])])
        self.lights = []
        for k in range(2):
            c = _vec(props, "color%d" % k)
            if c is not None:
                d = _vec(props, "direction%d" % k)
                self.lights.append((mi.Color3f(*[_opq(x) for x in c]), mi.Vector3f(*[_opq(-x) for x in d])))

    def eval(self, si, active=True):
        n = si.sh_frame.n
        light = mi.Color3f(self.ambient)
        for c, to_light in self.lights:
            light += c * dr.maximum(dr.dot(n, to_light), 0.0)
        return self.albedo.eval(si, active) * dr.power(dr.clip(light, 0.0, 1.0), 2.2)

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return 0.1

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return "HaloModelLight"


_registered = False


def register():
    global _registered
    if _registered:
        return
    mi.register_emitter("halo_light", lambda p: HaloLight(p))
    mi.register_emitter("halo_sky", lambda p: HaloSky(p))
    mi.register_texture("halo_albedo", lambda p: HaloAlbedo(p))
    mi.register_bsdf("halo_bump", lambda p: HaloBump(p))
    mi.register_texture("halo_lightmap_radiance", lambda p: HaloLightmapRadiance(p))
    mi.register_texture("halo_self_illum", lambda p: HaloSelfIllum(p))
    mi.register_texture("halo_change_color", lambda p: HaloChangeColor(p))
    mi.register_texture("halo_model_light", lambda p: HaloModelLight(p))
    _registered = True
