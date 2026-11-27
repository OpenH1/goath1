"""Mitsuba plugins written in Python for Halo's light sources and its detail-mapped albedo.

They are traced into Mitsuba's kernels like the built-in plugins (cuda_ad_rgb or llvm_ad_rgb).

Units (see README.md, "Units"): a light's `intensity` here is already pi * power * color, so a white
Lambertian surface facing it at distance 1 wu shows radiance power * color. The sky's `ambient` is a
radiance, so an unoccluded white floor shows radiance ambient. A sun's `irradiance` is pi * power * color.
"""

import math

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
# Angular modes (`angular_mode`, spot lights only), between the inner and outer half-angles:
#   linear_angle  linear in the angle;  linear_cos  linear in cos;  smoothstep_cos  smoothstep in cos
# raised to `angular_exponent`.
DISTANCE_MODES = ("inverse_square", "inverse_square_cutoff", "window_radius", "halo_radius", "none")
ANGULAR_MODES = ("linear_angle", "linear_cos", "smoothstep_cos")


class HaloLight(mi.Emitter):
    """A point or spot light of a Halo light tag, with the spot's gel projected through its cone."""

    def __init__(self, props):
        mi.Emitter.__init__(self, props)
        self.position = _vec(props, "position")
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
        self.gel = props["gel"] if props.has_property("gel") else None
        self.m_flags = mi.EmitterFlags.DeltaPosition

    # Radiant intensity toward world direction w (unit, pointing away from the light).
    def radiant_intensity(self, w, active=True):
        f = mi.Spectrum(self.intensity)
        if self.kind != "spot":
            return f
        lx, ly, lz = dr.dot(w, self.ex), dr.dot(w, self.ey), dr.dot(w, self.ez)
        cos_in, cos_out = math.cos(self.half_in), math.cos(self.half_out)
        if self.angular_mode == "linear_angle":
            th = dr.acos(dr.clip(lz, -1.0, 1.0))
            a = dr.clip((self.half_out - th) / max(self.half_out - self.half_in, 1e-6), 0.0, 1.0)
        else:
            t = dr.clip((lz - cos_out) / max(cos_in - cos_out, 1e-6), 0.0, 1.0)
            a = t if self.angular_mode == "linear_cos" else t * t * (3.0 - 2.0 * t)
        if self.angular_exponent != 1.0:
            a = dr.power(a, self.angular_exponent)
        a = dr.select(lz > cos_out, a, 0.0)
        f = f * a
        if self.gel is not None:
            t_out = math.tan(min(self.half_out, math.radians(89.0)))
            inv = 1.0 / (dr.maximum(lz, 1e-6) * t_out)
            si = dr.zeros(mi.SurfaceInteraction3f)
            # Image left to right along +x, top to bottom along -y.
            si.uv = mi.Point2f(0.5 + 0.5 * lx * inv, 0.5 - 0.5 * ly * inv)
            inside = (lz > 0) & (si.uv.x >= 0) & (si.uv.x <= 1) & (si.uv.y >= 0) & (si.uv.y <= 1)
            g = self.gel.eval(si, active & inside)
            f = f * dr.select(inside, g, 0.0)
        return f

    def distance_factor(self, dist2):
        m, r = self.distance_mode, max(self.radius, 1e-6)
        if m == "none":
            return mi.Float(1.0)
        if m == "halo_radius":
            return dr.power(dr.clip(1.0 - dr.sqrt(dist2) / r, 0.0, 1.0), self.distance_exponent)
        inv = dr.rcp(dist2)
        if m == "inverse_square_cutoff":
            return dr.select(dist2 < r * r, inv, 0.0)
        if m == "window_radius":
            q = dist2 / (r * r)
            return inv * dr.square(dr.clip(1.0 - q * q, 0.0, 1.0))
        return inv

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
        value = self.radiant_intensity(-ds.d, active) * self.distance_factor(dist2)
        return ds, dr.select(active, value, 0.0)

    def eval_direction(self, it, ds, active=True):
        d = mi.Point3f(self.position) - it.p
        dist2 = dr.squared_norm(d)
        return self.radiant_intensity(-d / dr.sqrt(dist2), active) * self.distance_factor(dist2)

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


# How the detail map combines with the base map (`mode`), Halo's detail map functions:
#   double_biased_multiply  base * detail * 2 (Halo's default for environment shaders)
#   multiply                base * detail
#   biased_add              base + detail - 0.5
#   none                    base alone
ALBEDO_MODES = ("double_biased_multiply", "multiply", "biased_add", "none")


class HaloAlbedo(mi.Texture):
    """A shader's diffuse albedo: its base map combined with its detail map, which tiles `detail_scale`
    times as often, then clamped to [0, 1] (an albedo above 1 would create energy)."""

    def __init__(self, props):
        mi.Texture.__init__(self, props)
        self.base = props["base"]
        self.detail = props["detail"] if props.has_property("detail") else None
        self.scale = props.get("detail_scale", 1.0)
        self.mode = props.get("mode", "double_biased_multiply")
        self.mean_value = props.get("mean_value", 0.5)
        assert self.mode in ALBEDO_MODES, self.mode

    def eval(self, si, active=True):
        b = self.base.eval(si, active)
        if self.detail is None or self.mode == "none":
            return dr.clip(b, 0.0, 1.0)
        si2 = mi.SurfaceInteraction3f(si)
        si2.uv = si.uv * self.scale
        d = self.detail.eval(si2, active)
        if self.mode == "double_biased_multiply":
            c = b * d * 2.0
        elif self.mode == "multiply":
            c = b * d
        else:
            c = b + d - 0.5
        return dr.clip(c, 0.0, 1.0)

    def eval_1(self, si, active=True):
        return mi.luminance(self.eval(si, active))

    def mean(self):
        return self.mean_value

    def is_spatially_varying(self):
        return True

    def traverse(self, cb):
        pass

    def to_string(self):
        return f"HaloAlbedo[{self.mode}]"


_registered = False


def register():
    global _registered
    if _registered:
        return
    mi.register_emitter("halo_light", lambda p: HaloLight(p))
    mi.register_emitter("halo_sky", lambda p: HaloSky(p))
    mi.register_texture("halo_albedo", lambda p: HaloAlbedo(p))
    _registered = True
