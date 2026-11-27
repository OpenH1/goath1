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
        lmap = np.asarray(f["lightmap"], np.int64) if "lightmap" in f.dtype.names else np.full(len(tris), -1)
    else:
        tris, mats = f["vertex_indices"].astype(np.int64), f["material"].astype(np.int64)
        lmap = np.full(len(tris), -1)
    pos = np.stack([v["x"], v["y"], v["z"]], 1).astype(np.float32)
    nrm = np.stack([v["nx"], v["ny"], v["nz"]], 1).astype(np.float32) if "nx" in v.dtype.names else None
    uv = np.stack([v["u"], v["v"]], 1).astype(np.float32) if "u" in v.dtype.names else np.zeros((len(pos), 2), np.float32)
    uv2 = np.stack([v["u2"], v["v2"]], 1).astype(np.float32) if "u2" in v.dtype.names else np.zeros((len(pos), 2), np.float32)
    return pos, nrm, uv, tris, mats, uv2, lmap


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

    def __init__(self, folder, flashlight_scale=None, white=False):
        halo_plugins.register()
        self.flashlight_scale = flashlight_scale
        # white: every surface a white Lambertian (albedo 1, no specular) with the same normal maps and
        # cutouts, so a render's direct light is the demodulated irradiance (as the radiance a white
        # surface shows: irradiance / pi, the game's light units).
        self.white = white
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
        if m.get("detail_function"):
            f = m["detail_function"]
            # "double biased add" is the models' base + 2 x detail - 1 (rasterizer_xbox_models.c)
            return {"double_biased_add": "biased_add"}.get(f, f), "detail_function"
        note = (m.get("albedo_note") or "").lower().replace("-", " ").replace("_", " ")
        if "double" in note and "multiply" in note:
            return "double_biased_multiply", note
        if "biased add" in note or "add" in note:
            return "biased_add", note
        if "multiply" in note:
            return "multiply", note
        return "double_biased_multiply", "default (note says nothing usable: %r)" % note

    # Halo's specular has no roughness (a cube map reflection); the truth gives it this one.
    DEFAULT_SPECULAR_ROUGHNESS = 0.3

    def material_plugins(self, idx):
        """(bsdf, area emitter dict or None, summary) for material `idx` (an emitter is made per mesh)."""
        m = self.materials[idx] if 0 <= idx < len(self.materials) else {"index": idx}
        summary = {"index": idx, "tag": m.get("tag"), "type": m.get("type")}
        mtype = (m.get("type") or "").lower()
        base = self.image(m["base_map"]) if m.get("base_map") else None
        # The .lin maps hold value^2.2 of the game's gamma-space texels.
        gamma = 2.2 if str(m.get("base_map") or "").endswith(".lin.png") else 1.0
        if self.white:
            albedo = mi.load_dict({"type": "rgb", "value": [1.0, 1.0, 1.0]})
            summary["albedo"] = "white (irradiance render)"
        elif base is None:
            albedo = mi.load_dict({"type": "rgb", "value": [0.5, 0.5, 0.5]})
            summary["albedo"] = "placeholder 0.5 grey (no base map%s)" % (
                ": " + m["base_map"] + " not found" if m.get("base_map") else "")
        else:
            mode, why = self.albedo_mode(m)
            props = {"type": "halo_albedo", "base": self.bitmap(base[:, :, :3]), "gamma": gamma,
                     "detail_mode": mode, "micro_mode": m.get("micro_detail_function") or "double_biased_multiply",
                     "base_scale_u": float(m.get("map_u_scale") or m.get("base_scale") or 1.0),
                     "base_scale_v": float(m.get("map_v_scale") or m.get("base_scale") or 1.0)}
            parts = ["base"]

            def rescale(img):
                if m.get("rescale_detail_maps") and img is not None:
                    return base.shape[1] / img.shape[1], base.shape[0] / img.shape[0]
                return 1.0, 1.0

            det = self.image(m["detail_map"]) if m.get("detail_map") else None
            if det is not None:
                ru, rv = rescale(det)
                ds = float(m.get("detail_scale") or 1.0)
                props.update(detail=self.bitmap(det[:, :, :3]), detail_scale_u=ds * ru,
                             detail_scale_v=float(m.get("detail_v_scale") or 1.0) * ds * rv)
                parts.append("detail x%g %s" % (ds, mode))
            sec = self.image(m["secondary_detail_map"]) if m.get("secondary_detail_map") else None
            if sec is not None:
                props.update(secondary=self.bitmap(sec[:, :, :3]), secondary_alpha=self.bitmap(sec[:, :, 3:4]),
                             secondary_scale=float(m.get("secondary_detail_scale") or 1.0) * rescale(sec)[0])
                parts.append("secondary detail")
            mic = self.image(m["micro_detail_map"]) if m.get("micro_detail_map") else None
            if mic is not None:
                props.update(micro=self.bitmap(mic[:, :, :3]),
                             micro_scale=float(m.get("micro_detail_scale") or 1.0) * rescale(mic)[0])
                parts.append("micro x%g %s" % (props["micro_scale"], props["micro_mode"]))
            props["mean_value"] = float(np.clip(base[:, :, :3].mean(), 0, 1))
            albedo = mi.load_dict(props)
            summary["albedo"] = ", ".join(parts) + (" (gamma %g)" % gamma)
            if mtype == "model" and m.get("detail_mask"):
                self.note_once("model detail masks are not applied (the detail covers the whole map)")

        spec = m.get("specular") or {}
        tint = spec.get("tint") or [0, 0, 0]
        bright = float(spec.get("brightness", spec.get("perpendicular_brightness", 1.0)) or 0.0)
        tint = [t * bright for t in tint]
        if max(tint) > 0 and not mtype.startswith("transparent") and not self.white:
            mask = None
            msk = spec.get("mask")
            if isinstance(msk, str) and msk.endswith(".png"):
                mask = self.spec_mask(msk, base, summary)
            elif base is not None and msk:
                mask = base[:, :, 3]  # "base map alpha x ...": the base map's alpha (the detail alphas left out)
            rough = spec.get("roughness")
            rough = self.DEFAULT_SPECULAR_ROUGHNESS if rough is None else float(rough)
            spec_tex = self.bitmap(mask[:, :, None] * np.array(tint, np.float32)[None, None, :]) if mask is not None \
                else mi.load_dict({"type": "rgb", "value": tint})
            bsdf = {"type": "roughplastic", "distribution": "ggx", "alpha": max(rough * rough, 1e-3),
                    "diffuse_reflectance": albedo, "specular_reflectance": spec_tex, "nonlinear": False}
            summary["specular"] = "GGX alpha %.3g, tint x brightness %s, mask %s" % (
                max(rough * rough, 1e-3), [round(t, 3) for t in tint], "base alpha" if mask is not None else "1")
        else:
            bsdf = {"type": "diffuse", "reflectance": albedo}

        bump = self.image(m["bump_map"]) if m.get("bump_map") and not mtype.startswith("transparent") else None
        if bump is not None:
            # Halo's bump maps are stored as tangent-space normal maps (RGB around 0.5, 0.5, 1), tiled
            # bump_scale times over the base UVs; Mitsuba's frame (dp/du, dp/dv, n) is taken as theirs.
            bs = float(m.get("bump_scale") or 1.0)
            nm = mi.load_dict({"type": "bitmap", "bitmap": mi.Bitmap(np.ascontiguousarray(bump[:, :, :3], np.float32)),
                               "raw": True, "filter_type": "bilinear", "wrap_mode": "repeat",
                               "to_uv": mi.ScalarTransform4f().scale([bs, bs, 1.0])})
            bsdf = {"type": "normalmap", "normalmap": nm, "bsdf": bsdf}
            summary["bump"] = "normal map x%g" % bs
            self.note_once("bump maps as tangent-space normal maps in Mitsuba's (dp/du, dp/dv) frame (green sign unverified)")
        transparent = mtype.startswith("transparent")
        if transparent and not m.get("alpha_test"):
            # Glass, decals, light cards and effects carry no surface to bounce light off in the truth;
            # their radiosity emission stays.
            bsdf = {"type": "null"}
            summary["surface"] = "transparent: left out (light passes), but for its glow"
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
            # Radiosity units: radiance power x color, linear (the fit scales them to the lightmap).
            L = [c * power * EMISSION_RADIANCE_PER_POWER for c in color]
            emitter = {"type": "area", "radiance": {"type": "rgb", "value": L}}
            summary["emission"] = "area emitter, radiance %s (radiosity units)" % [round(x, 4) for x in L]
        return bsdf_obj, emitter, summary

    def note_once(self, text):
        if text not in self.notes:
            self.notes.append(text)

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
    # The static groups are what Halo's lightmaps hold, in the lightmap tool's unknown units, each fitted to
    # the lightmap with its own scale; the dynamic groups are in the game's own (linearized) units.
    #   ambient     the sky's ambient dome
    #   sun         the sky's lights (suns and broad sky lights)
    #   emitters    the BSP's glowing surfaces (shader radiosity), and those of scenery and devices
    #   flashlight  the lights whose source is "flashlight"
    #   dynamic     every other light of lights.json, and the glowing surfaces of other objects
    GROUPS = ("ambient", "sun", "emitters", "flashlight", "dynamic")
    STATIC_GROUPS = ("ambient", "sun", "emitters")

    def lights_of(self, group):
        ls = (self.lights or {}).get("lights") or []
        if group == "flashlight":
            return [L for L in ls if L.get("source") == "flashlight"]
        if group == "dynamic":
            return [L for L in ls if L.get("source") != "flashlight"]
        return []

    # Objects whose glow the lightmap tool bakes (Halo's object types: scenery 6, machine 7, control 8,
    # light fixture 9): their emitters are static light; other objects' (units, weapons, items) dynamic.
    BAKED_OBJECT_TYPES = (6, 7, 8, 9)

    def object_types(self):
        if not hasattr(self, "_obj_types"):
            p = self.folder / "geometry" / "objects.json"
            self._obj_types = {}
            if p.exists():
                for o in json.loads(p.read_text()):
                    self._obj_types[(self.folder / "geometry" / o["file"]).resolve()] = o.get("type")
        return self._obj_types

    def emitter_group(self, fpath):
        if fpath.parent.name != "objects":
            return "emitters"
        return "emitters" if self.object_types().get(fpath.resolve()) in self.BAKED_OBJECT_TYPES else "dynamic"

    def _has_emitters(self, group):
        self.load_geometry()
        for fpath, (pos, nrm, uv, tris, mats, *_) in self.geometry.items():
            if self.emitter_group(fpath) != group:
                continue
            for mid in np.unique(mats):
                if self.material(int(mid))[1] is not None:
                    return True
        return False

    def group_has_light(self, group):
        if group in ("flashlight", "dynamic") and self.lights_of(group):
            return True
        if group in ("dynamic", "emitters"):
            return self._has_emitters(group)
        sky = self.sky_dict(np.zeros(3), 1.0)
        if group == "ambient":
            return max(sky["ambient"]) > 0
        if group == "sun":
            return "sun0_to_sun" in sky
        return False

    def load_geometry(self):
        if getattr(self, "geometry", None) is None:
            self.geometry = {}
            for fpath in self.geometry_files():
                g = list(read_ply(fpath))
                if len(g[3]):
                    pos, nrm, tris = g[0], g[1], g[3]
                    if nrm is not None:
                        # Halo's triangles wind against their normals (the BSP's all do); Mitsuba takes the
                        # winding as the geometric normal (an emitter's side), so it is made to agree.
                        cr = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
                        flip = (cr * nrm[tris].sum(1)).sum(1) < 0
                        if flip.any():
                            tris = tris.copy()
                            tris[flip] = tris[flip][:, [0, 2, 1]]
                            g[3] = tris
                            self.flipped = getattr(self, "flipped", 0) + int(flip.sum())
                    self.geometry[fpath] = tuple(g)
            if not self.geometry:
                raise RuntimeError("%s: no geometry" % self.folder)
            lo = np.min([g[0].min(0) for g in self.geometry.values()], 0)
            hi = np.max([g[0].max(0) for g in self.geometry.values()], 0)
            self.bounds = (lo, hi)
            self.n_tris = sum(len(g[3]) for g in self.geometry.values())
            if getattr(self, "flipped", 0):
                self.note_once("%d of %d triangles wound against their normals: rewound" % (self.flipped, self.n_tris))

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
        self.mesh_faces = {}
        fp_files = self.first_person_files() if group == "flashlight" else set()
        for fpath, (pos, nrm, uv, tris, mats, *_) in self.geometry.items():
            is_object = fpath.parent.name == "objects"
            if fpath.resolve() in fp_files:
                # The flashlight's lamp sits inside the first-person weapon, which would shadow all of its
                # light: the weapon and hands are left out of the flashlight's render (they are behind the
                # lamp, unlit by it; their pixels are zeroed in its parts).
                continue
            emit = group is None or group == self.emitter_group(fpath)
            for mid in np.unique(mats):
                sel = tris[mats == mid]
                used, inv = np.unique(sel.ravel(), return_inverse=True)
                bsdf, emitter, summ = self.material(int(mid))
                summaries[int(mid)] = summ
                if summ.get("surface", "").startswith("transparent") and not (emitter and emit):
                    # A surface that passes light is left out: in Mitsuba it would still block every shadow
                    # ray, so light behind it would arrive only by chance hits (fireflies from the sun).
                    continue
                n = nrm[used] if nrm is not None else None
                if n is not None and (not np.all(np.isfinite(n)) or np.any(np.linalg.norm(n, axis=1) < 0.5)):
                    n = None  # unusable normals: Mitsuba falls back to flat face normals
                name = "%s_m%d" % (fpath.stem if not is_object else "obj_" + fpath.stem, mid)
                self.mesh_faces[name] = (fpath, np.nonzero(mats == mid)[0])
                shapes[name] = self.make_mesh(name, pos[used], n, uv[used], inv.reshape(-1, 3), bsdf,
                                              mi.load_dict(emitter) if (emitter and emit) else None)
        self.meshes = shapes
        lo, hi = self.bounds
        center = (lo + hi) / 2
        radius = float(np.linalg.norm(hi - lo) / 2)
        d = {"type": "scene", "sensor": self.sensor()}
        d.update(shapes)
        if group in (None, "ambient", "sun"):
            d["sky"] = self.sky_dict(center, radius, ambient=group != "sun", suns=group != "ambient")
        lights = (self.lights or {}).get("lights") or [] if group is None else self.lights_of(group)
        for i, ld in enumerate(self.light_dicts(lights)):
            d["light_%d" % i] = ld
        self.material_summaries = summaries
        return d

    def first_person_files(self):
        """The object files of the first-person weapon and hands (objects.json's `first_person`)."""
        p = self.folder / "geometry" / "objects.json"
        if not p.exists():
            return set()
        return {(self.folder / "geometry" / o["file"]).resolve() for o in json.loads(p.read_text())
                if o.get("first_person")}

    def first_person_mask(self, scene):
        """True where a pixel's center sees the first-person weapon or hands: the nearest hit of the whole
        scene against that of those objects alone."""
        self.load_geometry()
        fp = self.first_person_files()
        H, W = int(self.camera["height"]), int(self.camera["width"])
        if not fp:
            return np.zeros((H, W), bool)
        shapes = {}
        grey = mi.load_dict({"type": "diffuse"})
        for fpath, (pos, nrm, uv, tris, mats, *_) in self.geometry.items():
            if fpath.resolve() in fp:
                shapes["fp_" + fpath.stem] = self.make_mesh("fp_" + fpath.stem, pos, None, uv, tris, grey, None)
        fp_scene = mi.load_dict({"type": "scene", **shapes})
        sensor = scene.sensors()[0]
        xs, ys = np.meshgrid((np.arange(W) + 0.5) / W, (np.arange(H) + 0.5) / H)
        ray, _ = sensor.sample_ray(0.0, 0.5, mi.Point2f(xs.ravel(), ys.ravel()), mi.Point2f(0.5, 0.5))
        t_all = np.asarray(scene.ray_intersect(ray).t)
        t_fp = np.asarray(fp_scene.ray_intersect(ray).t)
        return (np.isfinite(t_fp) & (t_fp <= t_all * (1 + 1e-4) + 1e-6)).reshape(H, W)

    def pixel_rays(self, scene):
        H, W = int(self.camera["height"]), int(self.camera["width"])
        xs, ys = np.meshgrid((np.arange(W) + 0.5) / W, (np.arange(H) + 0.5) / H)
        ray, _ = scene.sensors()[0].sample_ray(0.0, 0.5, mi.Point2f(xs.ravel(), ys.ravel()), mi.Point2f(0.5, 0.5))
        return ray

    def lightmap_samples(self, scene):
        """What Halo's lightmap says at each pixel's center: the BSP surface seen there, its diffuse albedo
        (linear), its lightmap page and lightmap UV, and the page's texel (bilinear) read with v as stored
        and with v flipped, the texel's values as stored. Built on the last build()'s meshes."""
        H, W = int(self.camera["height"]), int(self.camera["width"])
        ray = self.pixel_rays(scene)
        si = scene.ray_intersect(ray)
        valid = np.asarray(si.is_valid())
        ptr = np.asarray(dr.reinterpret_array(mi.UInt32, si.shape))
        prim = np.asarray(si.prim_index).astype(np.int64)
        p = np.stack([np.asarray(si.p.x), np.asarray(si.p.y), np.asarray(si.p.z)], -1)
        try:
            alb = si.bsdf().eval_diffuse_reflectance(si)
            albedo = np.stack([np.asarray(alb.x), np.asarray(alb.y), np.asarray(alb.z)], -1)
        except Exception as e:  # pragma: no cover
            self.note_once("albedo at pixels unavailable (%s): albedo 0.5" % e)
            albedo = np.full((H * W, 3), 0.5)
        n = H * W
        page = np.full(n, -1)
        uv2 = np.zeros((n, 2))
        by_ptr = {}
        for name, mesh in self.meshes.items():
            key = int(np.asarray(dr.reinterpret_array(mi.UInt32, mi.ShapePtr(mesh)))[0])
            by_ptr[key] = name
        for key, name in by_ptr.items():
            fpath, faces = self.mesh_faces[name]
            if fpath.parent.name == "objects":
                continue
            sel = valid & (ptr == key)
            if not sel.any():
                continue
            pos, nrm, uv, tris, mats, uv2v, lmap = self.geometry[fpath]
            f = faces[prim[sel]]
            a, b, c = pos[tris[f, 0]], pos[tris[f, 1]], pos[tris[f, 2]]
            v0, v1, v2 = b - a, c - a, p[sel] - a
            d00, d01, d11 = (v0 * v0).sum(1), (v0 * v1).sum(1), (v1 * v1).sum(1)
            d20, d21 = (v2 * v0).sum(1), (v2 * v1).sum(1)
            den = np.where(np.abs(d00 * d11 - d01 * d01) > 0, d00 * d11 - d01 * d01, 1)
            bv = (d11 * d20 - d01 * d21) / den
            bw = (d00 * d21 - d01 * d20) / den
            bu = 1 - bv - bw
            uv2[sel] = (bu[:, None] * uv2v[tris[f, 0]] + bv[:, None] * uv2v[tris[f, 1]] +
                        bw[:, None] * uv2v[tris[f, 2]])
            page[sel] = lmap[f]
        lm = np.zeros((n, 3))
        lm_flip = np.zeros((n, 3))
        for k in np.unique(page[page >= 0]):
            img = self.image("geometry/lightmaps/%d.png" % k)
            if img is None:
                continue
            sel = page == k
            from .synthetic import _bilinear
            for out, vv in ((lm, uv2[sel, 1]), (lm_flip, 1 - uv2[sel, 1])):
                out[sel] = np.stack([_bilinear(img[:, :, ch], uv2[sel, 0], vv) for ch in range(3)], -1)
        has = (page >= 0) & np.isin(page, [k for k in np.unique(page[page >= 0])
                                           if self.resolve("geometry/lightmaps/%d.png" % k)])
        return {"valid": has.reshape(H, W), "albedo": albedo.reshape(H, W, 3),
                "lightmap": lm.reshape(H, W, 3), "lightmap_flipped": lm_flip.reshape(H, W, 3)}

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

    def sky_dict(self, center, radius, ambient=True, suns=True):
        sky = (self.lights or {}).get("sky") or {}
        if self.lights is None:
            amb = [1.0, 1.0, 1.0]
        else:
            a = sky.get("ambient") or {}
            amb = [x * float(a.get("power", 0.0)) * AMBIENT_RADIANCE_PER_POWER for x in (a.get("color") or [0, 0, 0])]
            if (sky.get("ambient_indoor") or {}).get("power"):
                self.note_once("sky ambient_indoor (power %g) ignored: no physical counterpart" % sky["ambient_indoor"]["power"])
        if not ambient:
            amb = [0.0, 0.0, 0.0]
        d = {"type": "halo_sky", "ambient": amb, "center": center.tolist(), "scene_radius": radius}
        for i, s in enumerate((sky.get("sun") or []) if suns else []):
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
        if "distance_attenuation" in t:
            ang = "none" if (light.get("cone_cosines") or [-1])[0] <= -1 else "linear_cos"
            return "table", 1.0, ang, 1.0, "the game's distance attenuation table at d / radius"
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
            color = list(L.get("color", [1, 1, 1]))
            game_units = dm == "table"
            if game_units:
                # The game adds color x attenuation x gel x N.L in gamma space: color and table decoded ^2.2.
                color = [max(c, 0.0) ** 2.2 for c in color]
            if L.get("source") == "flashlight" and self.flashlight_scale is not None:
                # The flashlight: physical inverse-square falloff times a window reaching 0 at the tag's
                # radius (as the real-time renderer's; a point here, its disk only matters very near), its
                # power calibrated by one scale k
                # (rt2-calibration.json: the original's brightness at the beam's center on a wall ~3 m off).
                dm = "window_radius"
                power *= self.flashlight_scale
                why = "physical 1/d^2 x (1 - (d/r)^4)^2 window at the tag's radius, power x k = %g (calibration)" % self.flashlight_scale
            d = {"type": "halo_light", "kind": L.get("kind", "point"),
                 "position": list(L["position"]), "direction": list(L.get("direction") or [0, 0, -1]),
                 "intensity": [c * power * POINT_INTENSITY_PER_POWER for c in color],
                 "distance_mode": dm, "distance_exponent": de, "radius": float(L.get("radius") or 0.0),
                 "angular_mode": am, "angular_exponent": ae}
            if L.get("up"):
                d["up"] = list(L["up"])
            if game_units:
                tab = (self.lights.get("distance_attenuation") or {}).get("value_as_stored")
                tab = np.array(tab if tab else [1.0, 0.0], np.float32) ** 2.2
                d["table"] = self.bitmap(tab.reshape(1, -1, 1), wrap="clamp")
                d["table_n"] = len(tab)
                cc = L.get("cone_cosines")
                if cc and cc[0] > -1:
                    d["cos_falloff"], d["cos_cutoff"] = float(cc[0]), float(cc[1])
            if d["kind"] == "spot" and not game_units:
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
            if game_units and L.get("gel"):
                g = self.image(L["gel"])
                if g is None:
                    self.notes.append("gel %s not found: no gel on %s" % (L["gel"], L.get("tag")))
                elif g.shape[1] == 6 * g.shape[0]:
                    d["gel"] = self.bitmap(g[:, :, :3], wrap="clamp")
                    d["gel_kind"] = "cube"
                    d["gel_face_px"] = int(g.shape[0])
                    # On the structure the game shapes the light by the gel alone, no cone.
                    d["angular_mode"] = "none"
                    self.note_once("cube gels: D3D face order and UVs, looked up along P - light in the (forward, left, up) frame "
                                   "(rasterizer_xbox_environment.c:1244-1255; the beam on the gel's +x face); the cone is left off where a gel shapes the light")
                else:
                    d["gel"] = self.bitmap(g[:, :, :3], wrap="clamp")
            if L.get("source") == "flashlight" and self.flashlight_scale is not None:
                # rt2-calibration.json's model: a 1 cm disk at the lamp facing forward (cos at the emitter;
                # a point here: the disk changes 1/d^2 by under 1e-5 past 0.1 wu), the gel its angular
                # profile, no cone.
                d["angular_mode"] = "cos"
            out.append(d)
            self.light_summaries[str(L.get("tag")) + str(L["position"])] = {
                "tag": L.get("tag"), "source": L.get("source"), "kind": d["kind"], "distance": dm,
                "angular": d["angular_mode"], "falloff_from": why, "gel": L.get("gel"),
                "color_linear": [round(c, 4) for c in color]}
        return out
