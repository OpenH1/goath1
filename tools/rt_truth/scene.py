"""Reads an rt2 scene folder (see rt2-scene-format.md) and builds the Mitsuba scene from it.

Everything here maps Halo's data onto physics; README.md, "Mapping", says how and why.
"""

import json
import math
import os
from pathlib import Path

import numpy as np

import mitsuba as mi
import drjit as dr

from . import halo_plugins

# Halo powers to radiance (README.md, "Units"). A white Lambertian surface (BRDF 1/pi) facing a light
# of power p at 1 wu shows radiance p * color: its intensity is pi * p * color (W/sr per wu^2 units).
# The sun's irradiance is pi * p * color; the ambient sky and glowing surfaces are radiances p * color.
POINT_INTENSITY_PER_POWER = math.pi
SUN_IRRADIANCE_PER_POWER = math.pi
AMBIENT_RADIANCE_PER_POWER = 1.0
EMISSION_RADIANCE_PER_POWER = 1.0


# ---------------------------------------------------------------- PLY

_PLY_TYPES = {"char": "i1", "int8": "i1", "uchar": "u1", "uint8": "u1", "short": "i2", "int16": "i2",
              "ushort": "u2", "uint16": "u2", "int": "i4", "int32": "i4", "uint": "u4", "uint32": "u4",
              "float": "f4", "float32": "f4", "double": "f8", "float64": "f8"}


def read_ply(path):
    """Binary little-endian PLY: vertex x y z nx ny nz u v [u2 v2], faces with a vertex list and an int
    `material`. Returns positions, normals, uvs (N x 2), triangles (M x 3) and per-triangle materials."""
    with open(path, "rb") as f:
        assert f.readline().strip() == b"ply", path
        elements = []
        while True:
            line = f.readline().decode("ascii").strip()
            if line == "end_header":
                break
            tok = line.split()
            if not tok or tok[0] in ("comment", "obj_info"):
                continue
            if tok[0] == "format":
                assert tok[1] == "binary_little_endian", f"{path}: {tok[1]} PLY is not supported"
            elif tok[0] == "element":
                elements.append((tok[1], int(tok[2]), []))
            elif tok[0] == "property":
                elements[-1][2].append(tok[1:])
        data = f.read()
    off = 0
    out = {}
    for name, count, props in elements:
        if all(p[0] != "list" for p in props):
            dt = np.dtype([(p[1], "<" + _PLY_TYPES[p[0]]) for p in props])
            arr = np.frombuffer(data, dt, count, off)
            off += dt.itemsize * count
            out[name] = arr
            continue
        # Faces: try the fixed layout of triangles first.
        fields = []
        for p in props:
            if p[0] == "list":
                fields.append(("_n_" + p[3], "<" + _PLY_TYPES[p[1]]))
                fields.append((p[3], "<" + _PLY_TYPES[p[2]], (3,)))
            else:
                fields.append((p[1], "<" + _PLY_TYPES[p[0]]))
        dt = np.dtype(fields)
        arr = np.frombuffer(data, dt, count, off) if off + dt.itemsize * count <= len(data) else None
        lst = [p for p in props if p[0] == "list"][0]
        if arr is not None and np.all(arr["_n_" + lst[3]] == 3):
            off += dt.itemsize * count
            out[name] = arr
            continue
        # Polygons: fan-triangulate.
        tris, mats = [], []
        for _ in range(count):
            rec = {}
            for p in props:
                if p[0] == "list":
                    ct = np.dtype("<" + _PLY_TYPES[p[1]])
                    n = int(np.frombuffer(data, ct, 1, off)[0]); off += ct.itemsize
                    it = np.dtype("<" + _PLY_TYPES[p[2]])
                    rec[p[3]] = np.frombuffer(data, it, n, off); off += it.itemsize * n
                else:
                    t = np.dtype("<" + _PLY_TYPES[p[0]])
                    rec[p[1]] = np.frombuffer(data, t, 1, off)[0]; off += t.itemsize
            idx = rec[lst[3]]
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
                mats.append(rec.get("material", 0))
        out[name] = {"vertex_indices": np.array(tris).reshape(-1, 3), "material": np.array(mats)}
    v = out["vertex"]
    f = out["face"]
    if hasattr(f, "dtype"):
        idx_name = [n for n in f.dtype.names if not n.startswith("_n_") and f.dtype[n].shape == (3,)][0]
        tris = np.asarray(f[idx_name], np.int64)
        mats = np.asarray(f["material"], np.int64) if "material" in f.dtype.names else np.zeros(len(tris), np.int64)
    else:
        tris, mats = f["vertex_indices"].astype(np.int64), f["material"].astype(np.int64)
    pos = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float32)
    nrm = np.stack([v["nx"], v["ny"], v["nz"]], 1).astype(np.float32) if "nx" in v.dtype.names else None
    uv = np.stack([v["u"], v["v"]], 1).astype(np.float32) if "u" in v.dtype.names else np.zeros((len(pos), 2), np.float32)
    return pos, nrm, uv, tris, mats


def write_ply(path, pos, nrm, uv, tris, mats, uv2=None):
    """Writes the scene format's PLY layout (for the synthetic test scene)."""
    uv2 = np.zeros_like(uv) if uv2 is None else uv2
    hdr = ("ply\nformat binary_little_endian 1.0\nelement vertex %d\n" % len(pos) +
           "".join("property float %s\n" % n for n in ("x", "y", "z", "nx", "ny", "nz", "u", "v", "u2", "v2")) +
           "element face %d\nproperty list uchar int vertex_indices\nproperty int material\nend_header\n" % len(tris))
    vert = np.concatenate([pos, nrm, uv, uv2], 1).astype("<f4")
    fdt = np.dtype([("n", "u1"), ("i", "<i4", (3,)), ("m", "<i4")])
    fa = np.zeros(len(tris), fdt)
    fa["n"] = 3; fa["i"] = tris; fa["m"] = mats
    with open(path, "wb") as f:
        f.write(hdr.encode("ascii")); f.write(vert.tobytes()); f.write(fa.tobytes())


# ---------------------------------------------------------------- images

def read_image(path):
    """A PNG as float RGBA in [0, 1], values as stored (the format's textures are already linear)."""
    import cv2
    img = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if img is None:
        raise FileNotFoundError(path)
    scale = 65535.0 if img.dtype == np.uint16 else 255.0 if img.dtype == np.uint8 else 1.0
    img = img.astype(np.float32) / scale
    if img.ndim == 2:
        img = np.stack([img, img, img, np.ones_like(img)], 2)
    elif img.shape[2] == 3:
        img = np.concatenate([img[:, :, ::-1], np.ones_like(img[:, :, :1])], 2)
    else:
        img = img[:, :, [2, 1, 0, 3]]
    return np.ascontiguousarray(img)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def luminance(img):
    return img[..., 0] * 0.2126 + img[..., 1] * 0.7152 + img[..., 2] * 0.0722


# ---------------------------------------------------------------- scene

class Scene:
    """One scene folder, loaded; `notes` collects every placeholder and every choice made from free text."""

    def __init__(self, folder):
        halo_plugins.register()
        self.folder = Path(folder)
        self.notes = []
        self.placeholders = []
        self.camera = json.loads((self.folder / "camera.json").read_text())
        mpath = self.folder / "materials.json"
        self.materials = json.loads(mpath.read_text()) if mpath.exists() else []
        lpath = self.folder / "lights.json"
        if lpath.exists():
            self.lights = json.loads(lpath.read_text())
        else:
            # Placeholder until the lights helper writes lights.json: a white sky of radiance 1 and a
            # flashlight at the camera along its view (cone 30/60 degrees, power 1, inverse-square, no gel).
            c = self.camera
            self.lights = {"sky": {"tag": "placeholder", "sun": [], "ambient": {"color": [1, 1, 1], "power": 1.0}},
                           "lights": [{"tag": "placeholder flashlight", "kind": "spot", "source": "flashlight",
                                       "position": c["position"], "direction": c["forward"], "up": c["up"],
                                       "color": [1, 1, 1], "power": 1.0, "cone_deg": [30.0, 60.0],
                                       "falloff": "inverse square"}]}
            self.placeholders.append("lights.json missing: placeholder sky (ambient 1, no sun) and a placeholder "
                                     "flashlight at the camera (power 1, cone 30/60, no gel)")
        self._tex_cache = {}

    # A texture path from the JSON: relative to the scene folder, else to the scenes folder above it.
    def resolve(self, rel):
        for base in (self.folder, self.folder.parent):
            p = base / rel
            if p.exists():
                return p
        return None

    def image(self, rel):
        p = self.resolve(rel)
        if p is None:
            return None
        key = str(p)
        if key not in self._tex_cache:
            self._tex_cache[key] = read_image(p)
        return self._tex_cache[key]

    @staticmethod
    def bitmap(arr, wrap="repeat"):
        return mi.load_dict({"type": "bitmap", "bitmap": mi.Bitmap(np.ascontiguousarray(arr, np.float32)),
                             "raw": True, "filter_type": "bilinear", "wrap_mode": wrap})

    # ---- materials

    @staticmethod
    def albedo_mode(m):
        if m.get("albedo_mode"):
            return m["albedo_mode"], "albedo_mode"
        note = (m.get("albedo_note") or "").lower().replace("-", " ").replace("_", " ")
        if not m.get("detail_map"):
            return "none", "no detail map"
        if "double" in note and "multiply" in note:
            return "double_biased_multiply", note
        if "biased add" in note or "add" in note:
            return "biased_add", note
        if "multiply" in note:
            return "multiply", note
        return "double_biased_multiply", "default (note says nothing usable: %r)" % note

    def material_plugins(self, idx):
        """(bsdf, area emitter dict or None, summary) for material `idx` (an emitter is made per mesh)."""
        m = self.materials[idx] if 0 <= idx < len(self.materials) else {"index": idx}
        summary = {"index": idx, "tag": m.get("tag")}
        mtype = (m.get("type") or "").lower()
        base = self.image(m["base_map"]) if m.get("base_map") else None
        if base is None:
            base_tex = mi.load_dict({"type": "rgb", "value": [0.5, 0.5, 0.5]})
            summary["albedo"] = "placeholder 0.5 grey (no base map%s)" % (
                ": " + m["base_map"] + " not found" if m.get("base_map") else "")
            mean = 0.5
        else:
            base_tex = self.bitmap(base[:, :, :3])
            mean = float(base[:, :, :3].mean())
        mode, why = self.albedo_mode(m)
        detail = self.image(m["detail_map"]) if m.get("detail_map") else None
        if detail is not None and mode != "none":
            props = {"type": "halo_albedo", "base": base_tex, "detail": self.bitmap(detail[:, :, :3]),
                     "detail_scale": float(m.get("detail_scale") or 1.0), "mode": mode,
                     "mean_value": float(np.clip(mean * (2 * detail[:, :, :3].mean() if mode == "double_biased_multiply" else 1), 0, 1))}
            albedo = mi.load_dict(props)
            summary.setdefault("albedo", "base %s detail x%g (%s, from %s)" % (mode, props["detail_scale"], mode, why))
        else:
            albedo = base_tex
            summary.setdefault("albedo", "base only")

        spec = m.get("specular") or {}
        tint = spec.get("tint") or [0, 0, 0]
        if max(tint) > 0:
            mask = self.spec_mask(spec.get("mask"), base, summary)
            rough = float(spec.get("roughness", 0.5))
            spec_tex = self.bitmap(mask[:, :, None] * np.array(tint, np.float32)[None, None, :]) if mask is not None \
                else mi.load_dict({"type": "rgb", "value": tint})
            # Halo's roughness taken as perceptual roughness: microfacet alpha = roughness^2.
            bsdf = {"type": "roughplastic", "distribution": "ggx", "alpha": max(rough * rough, 1e-3),
                    "diffuse_reflectance": albedo, "specular_reflectance": spec_tex, "nonlinear": False}
            summary["specular"] = "GGX alpha %.3g, tint %s, mask %s" % (max(rough * rough, 1e-3), tint, spec.get("mask"))
        else:
            bsdf = {"type": "diffuse", "reflectance": albedo}

        transparent = mtype.startswith("transparent")
        if transparent and not m.get("alpha_test"):
            # Glass, decals and effects carry no surface to bounce light off in the truth; their glow stays.
            bsdf = {"type": "null"}
            summary["surface"] = "transparent: null (light passes)"
        if m.get("two_sided") and bsdf["type"] != "null":
            bsdf = {"type": "twosided", "bsdf": bsdf}
        if m.get("alpha_test") and base is not None and bsdf["type"] != "null":
            cut = (base[:, :, 3] >= 0.5).astype(np.float32)[:, :, None]  # Halo's alpha test at 50%
            bsdf = {"type": "mask", "opacity": self.bitmap(cut), "bsdf": bsdf}
            summary["cutout"] = "alpha test at 0.5 of the base map's alpha"
        bsdf_obj = mi.load_dict(bsdf)

        em = m.get("emission") or {}
        emitter = None
        power = float(em.get("power") or 0.0)
        if power > 0:
            color = em.get("color") or [1, 1, 1]
            L = [c * power * EMISSION_RADIANCE_PER_POWER for c in color]
            if em.get("map") and self.image(em["map"]) is not None:
                rad = self.bitmap(self.image(em["map"])[:, :, :3] * np.array(L, np.float32))
            else:
                rad = {"type": "rgb", "value": L}  # (an emitter's RGB may pass 1)
            emitter = {"type": "area", "radiance": rad}
            summary["emission"] = "area emitter, radiance %s" % [round(x, 4) for x in L]
        return bsdf_obj, emitter, summary

    def spec_mask(self, mask, base, summary):
        if not mask:
            return None
        if mask in ("base_alpha", "base.alpha", "base_map.alpha"):
            return None if base is None else base[:, :, 3]
        path, _, chan = mask.partition(":")
        img = self.image(path)
        if img is None:
            summary["specular_mask"] = "%s not found: mask 1" % mask
            return None
        return img[:, :, 3] if chan == "alpha" else luminance(img[:, :, :3])

    # ---- geometry

    def geometry_files(self):
        g = self.folder / "geometry"
        files = [g / "bsp.ply"] if (g / "bsp.ply").exists() else []
        if (g / "objects").is_dir():
            files += sorted((g / "objects").glob("*.ply"))
        return files

    # Light groups. The parts add up: truth = sum of the groups' renders, since light adds linearly.
    #   static      what Halo's lightmaps hold: the sky (ambient and sun) and the BSP's glowing surfaces
    #   flashlight  the lights whose source is "flashlight"
    #   dynamic     every other light of lights.json, and the glowing surfaces of objects
    GROUPS = ("static", "flashlight", "dynamic")

    def lights_of(self, group):
        ls = (self.lights or {}).get("lights") or []
        if group == "flashlight":
            return [L for L in ls if L.get("source") == "flashlight"]
        if group == "dynamic":
            return [L for L in ls if L.get("source") != "flashlight"]
        return []

    def group_has_light(self, group):
        if self.lights_of(group):
            return True
        self.load_geometry()
        want_bsp = group == "static"
        if group == "flashlight":
            return False
        for fpath, (pos, nrm, uv, tris, mats) in self.geometry.items():
            if (fpath.parent.name != "objects") != want_bsp:
                continue
            for mid in np.unique(mats):
                if self.material(int(mid))[1] is not None:
                    return True
        if group == "static":
            sky = self.sky_dict(np.zeros(3), 1.0)
            return max(sky["ambient"]) > 0 or "sun0_to_sun" in sky
        return False

    def load_geometry(self):
        if getattr(self, "geometry", None) is None:
            self.geometry = {}
            for fpath in self.geometry_files():
                g = read_ply(fpath)
                if len(g[3]):
                    self.geometry[fpath] = g
            if not self.geometry:
                raise RuntimeError("%s: no geometry" % self.folder)
            lo = np.min([g[0].min(0) for g in self.geometry.values()], 0)
            hi = np.max([g[0].max(0) for g in self.geometry.values()], 0)
            self.bounds = (lo, hi)
            self.n_tris = sum(len(g[3]) for g in self.geometry.values())

    def material(self, mid):
        if not hasattr(self, "_mat_cache"):
            self._mat_cache = {}
        if mid not in self._mat_cache:
            self._mat_cache[mid] = self.material_plugins(mid)
        return self._mat_cache[mid]

    def build(self, group=None):
        """The Mitsuba scene dict (without integrator) of one light group (None: every light)."""
        halo_plugins.register()
        self.load_geometry()
        shapes = {}
        summaries = {}
        for fpath, (pos, nrm, uv, tris, mats) in self.geometry.items():
            is_object = fpath.parent.name == "objects"
            emit = group is None or group == ("dynamic" if is_object else "static")
            for mid in np.unique(mats):
                sel = tris[mats == mid]
                used, inv = np.unique(sel.ravel(), return_inverse=True)
                bsdf, emitter, summ = self.material(int(mid))
                summaries[int(mid)] = summ
                n = nrm[used] if nrm is not None else None
                if n is not None and (not np.all(np.isfinite(n)) or np.any(np.linalg.norm(n, axis=1) < 0.5)):
                    n = None  # unusable normals: Mitsuba falls back to flat face normals
                name = "%s_m%d" % (fpath.stem if not is_object else "obj_" + fpath.stem, mid)
                shapes[name] = self.make_mesh(name, pos[used], n, uv[used], inv.reshape(-1, 3), bsdf,
                                              mi.load_dict(emitter) if (emitter and emit) else None)
        lo, hi = self.bounds
        center = (lo + hi) / 2
        radius = float(np.linalg.norm(hi - lo) / 2)
        d = {"type": "scene", "sensor": self.sensor()}
        d.update(shapes)
        if group in (None, "static"):
            d["sky"] = self.sky_dict(center, radius)
        lights = (self.lights or {}).get("lights") or [] if group is None else self.lights_of(group)
        for i, ld in enumerate(self.light_dicts(lights)):
            d["light_%d" % i] = ld
        self.material_summaries = summaries
        return d

    @staticmethod
    def make_mesh(name, pos, nrm, uv, tris, bsdf, emitter):
        props = mi.Properties()
        props["bsdf"] = bsdf
        if emitter is not None:
            props["emitter"] = emitter
        mesh = mi.Mesh(name, len(pos), len(tris), props, has_vertex_normals=nrm is not None,
                       has_vertex_texcoords=True)
        p = mi.traverse(mesh)
        p["vertex_positions"] = mi.Float(np.ascontiguousarray(pos, np.float32).ravel())
        p["faces"] = mi.UInt32(np.ascontiguousarray(tris, np.uint32).ravel())
        if nrm is not None:
            p["vertex_normals"] = mi.Float(np.ascontiguousarray(nrm, np.float32).ravel())
        p["vertex_texcoords"] = mi.Float(np.ascontiguousarray(uv, np.float32).ravel())
        p.update()
        return mesh

    def sensor(self):
        c = self.camera
        pos = np.array(c["position"], float)
        fwd = np.array(c["forward"], float)
        return {"type": "perspective", "fov": float(c["fov_h_deg"]), "fov_axis": "x",
                "near_clip": 0.002, "far_clip": 1e5,
                "to_world": mi.ScalarTransform4f().look_at(origin=pos.tolist(), target=(pos + fwd).tolist(),
                                                           up=list(c["up"])),
                "film": {"type": "hdrfilm", "width": int(c["width"]), "height": int(c["height"]),
                         "rfilter": {"type": "box"}, "pixel_format": "rgb"},
                "sampler": {"type": "independent"}}

    def sky_dict(self, center, radius):
        sky = (self.lights or {}).get("sky") or {}
        if self.lights is None:
            amb = [1.0, 1.0, 1.0]
        else:
            a = sky.get("ambient") or {}
            amb = [x * float(a.get("power", 0.0)) * AMBIENT_RADIANCE_PER_POWER for x in (a.get("color") or [0, 0, 0])]
        d = {"type": "halo_sky", "ambient": amb, "center": center.tolist(), "scene_radius": radius}
        for i, s in enumerate(sky.get("sun") or []):
            # The format's sun direction points from the scene toward the sun unless it says otherwise.
            dirn = np.array(s["direction"], float)
            if str(s.get("direction_is", "to_sun")).lower() in ("from_sun", "travel", "light_travel"):
                dirn = -dirn
            d["sun%d_to_sun" % i] = dirn.tolist()
            d["sun%d_irradiance" % i] = [c * float(s.get("power", 1.0)) * SUN_IRRADIANCE_PER_POWER for c in s["color"]]
            d["sun%d_angular_diameter_deg" % i] = float(s.get("angular_diameter_deg") or 0.53)
        return d

    @staticmethod
    def falloff_modes(light):
        """(distance_mode, distance_exponent, angular_mode, angular_exponent, why) from the light's falloff.

        `falloff` may be a dict {"distance": mode, "distance_exponent": e, "angular": mode,
        "angular_exponent": e} or free text from the tag; free text is matched by keywords and the choice
        is logged."""
        f = light.get("falloff")
        if isinstance(f, dict):
            return (f.get("distance", "inverse_square"), float(f.get("distance_exponent", 1.0)),
                    f.get("angular", "linear_angle"), float(f.get("angular_exponent", 1.0)), "structured")
        t = (f or "").lower()
        dist = "inverse_square"
        if "none" in t or "constant" in t:
            dist = "none"
        elif "window" in t:
            dist = "window_radius"
        elif "cutoff" in t:
            dist = "inverse_square_cutoff"
        elif "radius" in t and "inverse" not in t:
            dist = "halo_radius"
        ang = "smoothstep_cos" if "smooth" in t else "linear_cos" if "cos" in t else "linear_angle"
        return dist, 1.0, ang, 1.0, "text: %r" % (f,)

    def light_dicts(self, lights):
        out = []
        if not hasattr(self, "light_summaries"):
            self.light_summaries = {}
        for L in lights:
            dm, de, am, ae, why = self.falloff_modes(L)
            power = float(L.get("power", 1.0))
            d = {"type": "halo_light", "kind": L.get("kind", "point"),
                 "position": list(L["position"]), "direction": list(L.get("direction") or [0, 0, -1]),
                 "intensity": [c * power * POINT_INTENSITY_PER_POWER for c in L.get("color", [1, 1, 1])],
                 "distance_mode": dm, "distance_exponent": de, "radius": float(L.get("radius") or 0.0),
                 "angular_mode": am, "angular_exponent": ae}
            if L.get("up"):
                d["up"] = list(L["up"])
            if d["kind"] == "spot":
                cone = L.get("cone_deg") or [0.0, 90.0]
                # cone_deg holds full apex angles unless the light says "cone_is": "half".
                k = 1.0 if L.get("cone_is") == "half" else 0.5
                d["inner_half_deg"], d["outer_half_deg"] = cone[0] * k, cone[1] * k
                if L.get("gel"):
                    g = self.image(L["gel"])
                    if g is None:
                        self.notes.append("gel %s not found: no gel on %s" % (L["gel"], L.get("tag")))
                    else:
                        d["gel"] = self.bitmap(g[:, :, :3], wrap="clamp")
            out.append(d)
            self.light_summaries[str(L.get("tag")) + str(L["position"])] = {
                "tag": L.get("tag"), "source": L.get("source"), "kind": d["kind"], "distance": dm,
                "angular": am, "falloff_from": why}
        return out
