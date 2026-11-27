"""Route B of issue #1257: our weapons' and vehicles' textures baked from MCC's 3D preview meshes.

Reads MCC's cooked preview static meshes (UE4 4.2x, SM_*.uasset/.uexp in the pak: every H1 weapon's, and the
Warthog's, Ghost's, Banshee's and Scorpion's), lines each up with our classic model by a similarity transform
(principal axes, then trimmed ICP), and bakes our model's maps in their own layouts: for each texel, its 3D point,
the closest point on MCC's mesh, that point's UV and its section's textures, MCC's texel there.

- each base map: MCC's diffuse, padded past its islands, with mips, DXT1 (DXT5 where ours has alpha);
- each multipurpose map (the Xbox's channel order): its specular, self-illumination and detail masks from MCC's
  maps where one agrees with ours, fitted to our levels; the change-color mask always ours;
- beside each base map, <name>.normal.png (MCC's normal map turned into our tangent space, OpenGL style) and
  <name>.orm.png (AO, roughness, metalness) for the renderer's enhanced graphics.

    python3 tools/mcc_preview_bake.py --maps <folder of Xbox maps> --pak <MCC pak> --mod <mods/mcc_hd_test> \
        [--only AssaultRifle,warthog] [--max-size 2048]

Needs numpy, scipy and Pillow. Everything written goes to --mod: the player's own, from their own MCC install
(MCC-derived: never into the repo or anything shared).
"""

import argparse
import json
import struct
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
from scipy.spatial import cKDTree

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mcc_texture_remap import (DXT1, DXT3, DXT5, A8R8G8B8, Pak, XboxMap, bilinear, bitmap_tag_file,  # noqa: E402
                                decode_dxt, encode_dxt1, encode_dxt5, mcc_texture)

MESH_ROOT = "MCC/Content/UI/Features/Customization/Characters/H1/"


# ---------------------------------------------------------------- the cooked static mesh (UE4 4.2x, LOD0)

def _fstring(data: bytes, at: int):
    length, = struct.unpack_from("<i", data, at)
    at += 4
    if length < 0:
        return data[at:at - 2 * length].decode("utf-16-le").rstrip("\0"), at - 2 * length
    return data[at:at + length].rstrip(b"\0").decode("latin-1"), at + length


def package_tables(uasset: bytes):
    """the package's names and imports (class name, object name) from its summary"""
    at = 4
    legacy, = struct.unpack_from("<i", uasset, at)
    at += 8 + 8  # legacy UE3 version, UE4 version, licensee version
    count, = struct.unpack_from("<i", uasset, at)
    at += 4 + 20 * count  # custom versions: guid + version
    at += 4  # total header size
    _, at = _fstring(uasset, at)
    flags, name_count, name_offset = struct.unpack_from("<Iii", uasset, at)
    at += 12
    if not flags & 0x80000000:  # PKG_FilterEditorOnly unset: a localization id follows
        _, at = _fstring(uasset, at)
    _, _, _, _, import_count, import_offset = struct.unpack_from("<6i", uasset, at)
    names, at = [], name_offset
    for _ in range(name_count):
        name, at = _fstring(uasset, at)
        at += 4  # hashes
        names.append(name)
    imports = []
    for index in range(import_count):
        _, _, class_name, _, _, object_name, _ = struct.unpack_from("<7i", uasset, import_offset + 28 * index)
        imports.append((names[class_name], names[object_name]))
    return names, imports


def read_static_mesh(uasset: bytes, uexp: bytes):
    """LOD0 of a cooked static mesh: positions (n, 3, Unreal cm), UV channels (n, k, 2), triangles (m, 3), each
    triangle's section index, the sections (material slot, first index, triangle count, material name) and the
    vertices' tangent frames (tangent (n, 3), normal (n, 3), bitangent sign (n,)).

    The LOD's buffers are found by their headers rather than by walking every property: the position buffer is
    `stride 12, count n, bulk element 12, count n`; the sections come right before it (count, then 7 int32s each,
    then MaxDeviation); then the vertex buffer (strip flags, UV count, n, full-precision UVs, high-precision
    tangents, tangents, UVs), the color buffer and the index buffer (32-bit flag, bytes)."""
    names, imports = package_tables(uasset)
    position_at = None
    for at in range(0, len(uexp) - 16, 1):
        stride, count, element, count2 = struct.unpack_from("<4i", uexp, at)
        if stride == 12 and element == 12 and count == count2 and count > 2:
            position_at, vertex_count = at, count
            break
    if position_at is None:
        raise ValueError("no position buffer")
    # the sections: a count k and 7 int32s each, ending 4 bytes (MaxDeviation) before the positions
    sections = None
    for k in range(1, 64):
        start = position_at - 4 - 28 * k - 4
        if start < 0:
            break
        if struct.unpack_from("<i", uexp, start)[0] != k:
            continue
        rows = [struct.unpack_from("<7i", uexp, start + 4 + 28 * i) for i in range(k)]
        if rows[0][1] == 0 and all(0 <= r[3] <= r[4] < vertex_count and r[5] in (0, 1) and r[6] in (0, 1)
                                   for r in rows):
            sections = rows
            break
    if sections is None:
        raise ValueError("no section list before the position buffer")
    positions = np.frombuffer(uexp, "<f4", 3 * vertex_count, position_at + 16).reshape(-1, 3).astype(np.float64)
    at = position_at + 16 + 12 * vertex_count + 2  # + strip flags
    uv_count, count, full_uvs, high_tangents = struct.unpack_from("<4i", uexp, at)
    if count != vertex_count:
        raise ValueError("vertex buffer count differs from the positions'")
    at += 16
    element, count = struct.unpack_from("<ii", uexp, at)
    if element != 8:
        raise ValueError("high-precision tangents are not read")
    # two packed normals a vertex, tangent then normal (with the bitangent's sign in w): signed bytes over 127
    packed = np.frombuffer(uexp, np.int8, 8 * vertex_count, at + 8).reshape(-1, 2, 4).astype(np.float64) / 127
    tangents = (packed[:, 0, :3], packed[:, 1, :3], np.where(packed[:, 1, 3] < 0, -1.0, 1.0))
    at += 8 + element * count
    element, count = struct.unpack_from("<ii", uexp, at)
    uv_type = "<f4" if full_uvs else "<f2"
    uvs = np.frombuffer(uexp, uv_type, 2 * uv_count * vertex_count, at + 8).astype(np.float64)
    uvs = uvs.reshape(vertex_count, uv_count, 2)
    at += 8 + element * count
    at += 2  # color buffer: strip flags, stride, count, bulk
    stride, count = struct.unpack_from("<ii", uexp, at)
    at += 8
    element, count = struct.unpack_from("<ii", uexp, at)
    at += 8 + element * count
    wide, element, size = struct.unpack_from("<iii", uexp, at)
    indices = np.frombuffer(uexp, "<u4" if wide else "<u2", size // (4 if wide else 2), at + 12).astype(np.int64)
    triangles = indices.reshape(-1, 3)
    # the material slots, in order: each StaticMaterial's MaterialInterface (an import)
    index = {name: i for i, name in enumerate(names)}
    tag = struct.pack("<iiii", index["MaterialInterface"], 0, index["ObjectProperty"], 0)
    materials, at = [], 0
    while (at := uexp.find(tag, at)) >= 0:
        value, = struct.unpack_from("<i", uexp, at + 16 + 8 + 1)
        materials.append(imports[-value - 1][1] if value < 0 else f"export {value}")
        at += 1
    section_of = np.full(len(triangles), -1, np.int64)
    out_sections = []
    for number, (slot, first, tri_count, *_rest) in enumerate(sections):
        section_of[first // 3:first // 3 + tri_count] = number
        out_sections.append((slot, first, tri_count, materials[slot] if slot < len(materials) else "?"))
    return positions, uvs, triangles, section_of, out_sections, tangents


# ---------------------------------------------------------------- surfaces: sampling and closest points

def triangle_areas(points: np.ndarray, triangles: np.ndarray) -> np.ndarray:
    a, b, c = (points[triangles[:, i]] for i in range(3))
    return 0.5 * np.linalg.norm(np.cross(b - a, c - a), axis=1)


def sample_surface(points: np.ndarray, triangles: np.ndarray, count: int, rng):
    """`count` points spread over the surface by area: (points, triangle index, barycentrics)"""
    areas = triangle_areas(points, triangles)
    which = rng.choice(len(triangles), count, p=areas / areas.sum())
    r1, r2 = rng.random(count), rng.random(count)
    s = np.sqrt(r1)
    bary = np.stack([1 - s, s * (1 - r2), s * r2], 1)
    corners = points[triangles[which]]
    return np.einsum("ni,nij->nj", bary, corners), which, bary


def closest_on_triangles(p: np.ndarray, a: np.ndarray, b: np.ndarray, c: np.ndarray):
    """closest points on triangles (a, b, c) to points p, all (n, 3): (point, barycentrics (n, 3))
    (Ericson, Real-Time Collision Detection 5.1.5, vectorized)"""
    ab, ac, ap = b - a, c - a, p - a
    d1, d2 = (ab * ap).sum(1), (ac * ap).sum(1)
    bp = p - b
    d3, d4 = (ab * bp).sum(1), (ac * bp).sum(1)
    cp = p - c
    d5, d6 = (ab * cp).sum(1), (ac * cp).sum(1)
    va = d3 * d6 - d5 * d4
    vb = d5 * d2 - d1 * d6
    vc = d1 * d4 - d3 * d2
    n = len(p)
    bary = np.zeros((n, 3))
    done = np.zeros(n, bool)

    def put(mask, u, v, w):
        nonlocal done
        mask = mask & ~done
        bary[mask] = np.stack([u, v, w], 1)[mask]
        done |= mask

    one, zero = np.ones(n), np.zeros(n)
    with np.errstate(divide="ignore", invalid="ignore"):
        put((d1 <= 0) & (d2 <= 0), one, zero, zero)
        put((d3 >= 0) & (d4 <= d3), zero, one, zero)
        put((d6 >= 0) & (d5 <= d6), zero, zero, one)
        v = d1 / (d1 - d3)
        put((vc <= 0) & (d1 >= 0) & (d3 <= 0), 1 - v, v, zero)
        w = d2 / (d2 - d6)
        put((vb <= 0) & (d2 >= 0) & (d6 <= 0), 1 - w, zero, w)
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6))
        put((va <= 0) & ((d4 - d3) >= 0) & ((d5 - d6) >= 0), zero, 1 - w, w)
        denom = 1 / (va + vb + vc)
        v, w = vb * denom, vc * denom
        put(np.ones(n, bool), 1 - v - w, v, w)
    bary = np.nan_to_num(bary)
    point = bary[:, :1] * a + bary[:, 1:2] * b + bary[:, 2:] * c
    return point, bary


class Surface:
    """a mesh to find closest points on: candidate triangles from a k-d tree of dense samples, then exact"""

    def __init__(self, points, triangles, rng, samples=400000, candidates=12):
        self.points, self.triangles, self.candidates = points, triangles, candidates
        dense, self.sample_triangle, _ = sample_surface(points, triangles, samples, rng)
        # every triangle's centroid too, so tiny triangles are never missed
        centroids = points[triangles].mean(1)
        self.tree = cKDTree(np.concatenate([dense, centroids]))
        self.sample_triangle = np.concatenate([self.sample_triangle, np.arange(len(triangles))])

    def closest(self, query: np.ndarray, chunk=200000):
        """for each query point: distance, triangle, barycentrics"""
        n = len(query)
        best_d = np.full(n, np.inf)
        best_t = np.zeros(n, np.int64)
        best_b = np.zeros((n, 3))
        for start in range(0, n, chunk):
            q = query[start:start + chunk]
            _, near = self.tree.query(q, self.candidates)
            tris = self.sample_triangle[near]  # (m, k)
            for j in range(tris.shape[1]):
                t = self.triangles[tris[:, j]]
                point, bary = closest_on_triangles(q, self.points[t[:, 0]], self.points[t[:, 1]], self.points[t[:, 2]])
                d = np.linalg.norm(point - q, axis=1)
                sl = slice(start, start + len(q))
                better = d < best_d[sl]
                best_d[sl] = np.where(better, d, best_d[sl])
                best_t[sl] = np.where(better, tris[:, j], best_t[sl])
                best_b[sl] = np.where(better[:, None], bary, best_b[sl])
        return best_d, best_t, best_b


# ---------------------------------------------------------------- alignment: a similarity transform

def umeyama(src: np.ndarray, dst: np.ndarray, reflect: bool):
    """scale s, rotation R (a reflection if `reflect`), shift t minimizing |s R src + t - dst|"""
    mu_s, mu_d = src.mean(0), dst.mean(0)
    xs, xd = src - mu_s, dst - mu_d
    u, sigma, vt = np.linalg.svd(xd.T @ xs / len(src))
    d = np.ones(3)
    if (np.linalg.det(u) * np.linalg.det(vt) < 0) != reflect:
        d[2] = -1
    rot = u @ np.diag(d) @ vt
    scale = (sigma * d).sum() / (xs ** 2).sum(1).mean()
    return scale, rot, mu_d - scale * rot @ mu_s


def icp(ours: np.ndarray, tree: cKDTree, theirs: np.ndarray, scale, rot, shift, reflect, iterations=60, keep=0.9):
    for _ in range(iterations):
        moved = scale * ours @ rot.T + shift
        dist, near = tree.query(moved)
        cut = np.quantile(dist, keep)
        use = dist <= cut
        scale, rot, shift = umeyama(ours[use], theirs[near[use]], reflect)
    moved = scale * ours @ rot.T + shift
    dist, _ = tree.query(moved)
    return scale, rot, shift, dist


def align(ours_pts, theirs_pts):
    """the best similarity transform taking our samples onto theirs: principal axes (every sign, with and without
    a mirror), each refined by trimmed ICP; the lowest median distance wins"""
    tree = cKDTree(theirs_pts)
    mu_o, mu_t = ours_pts.mean(0), theirs_pts.mean(0)
    _, _, vo = np.linalg.svd(ours_pts - mu_o, full_matrices=False)
    _, _, vt = np.linalg.svd(theirs_pts - mu_t, full_matrices=False)
    scale0 = np.sqrt(((theirs_pts - mu_t) ** 2).sum(1).mean() / ((ours_pts - mu_o) ** 2).sum(1).mean())
    best = None
    subset = ours_pts[:: max(1, len(ours_pts) // 6000)]
    for signs in np.array(np.meshgrid([1, -1], [1, -1], [1, -1])).T.reshape(-1, 3):
        rot = vt.T @ np.diag(signs) @ vo
        reflect = np.linalg.det(rot) < 0
        shift = mu_t - scale0 * rot @ mu_o
        s, r, t, dist = icp(subset, tree, theirs_pts, scale0, rot, shift, reflect, iterations=25)
        score = np.median(dist)
        if best is None or score < best[0]:
            best = (score, s, r, t, reflect)
    _, s, r, t, reflect = best
    s, r, t, _ = icp(ours_pts, tree, theirs_pts, s, r, t, reflect, iterations=40)
    return s, r, t, reflect


# ---------------------------------------------------------------- our texels' 3D points

def raster_bary(uv: np.ndarray, triangles: np.ndarray, width: int, height: int):
    """for each texel of a width x height texture: the triangle drawn there (-1 for none) and its barycentrics
    (texel centers inside, UVs wrapped)"""
    tri_of = np.full((height, width), -1, np.int64)
    bary = np.zeros((height, width, 3))
    size = np.array([width, height], np.float64)
    pix = uv[triangles] * size  # (m, 3, 2)
    for index, tri in enumerate(pix):
        shift = np.floor(tri.mean(0) / size) * size
        tri = tri - shift
        x0, y0 = np.floor(tri.min(0)).astype(int)
        x1, y1 = np.ceil(tri.max(0)).astype(int)
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        (ax, ay), (bx, by), (cx, cy) = tri
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-12:
            continue
        w0 = ((bx - xs) * (cy - ys) - (by - ys) * (cx - xs)) / area
        w1 = ((cx - xs) * (ay - ys) - (cy - ys) * (ax - xs)) / area
        w2 = 1 - w0 - w1
        # a half-texel margin so edges are covered (clamped back onto the triangle)
        eps = 0.75 / max(1e-9, np.sqrt(abs(area)))
        inside = (w0 >= -eps) & (w1 >= -eps) & (w2 >= -eps)
        iy, ix = np.nonzero(inside)
        w = np.clip(np.stack([w0[iy, ix], w1[iy, ix], w2[iy, ix]], 1), 0, None)
        w /= w.sum(1, keepdims=True)
        gy, gx = (iy + y0) % height, (ix + x0) % width
        strict = (w0[iy, ix] >= 0) & (w1[iy, ix] >= 0) & (w2[iy, ix] >= 0)
        # a strictly inside texel always wins over a margin one already there
        free = (tri_of[gy, gx] < 0) | strict
        tri_of[gy[free], gx[free]] = index
        bary[gy[free], gx[free]] = w[free]
    return tri_of, bary


# ---------------------------------------------------------------- the meshes and their textures

# each MCC preview mesh and our models it stands for (the first-person model first)
TARGETS = [
    ("SM_H1_AssaultRifle", ["weapons\\assault rifle\\fp\\fp", "weapons\\assault rifle\\assault rifle"]),
    ("SM_H1_Flamethrower", ["weapons\\flamethrower\\fp\\fp", "weapons\\flamethrower\\flamethrower"]),
    ("SM_H1_FuelRodGun", ["weapons\\fuel rod gun\\fuel rod gun"]),
    ("SM_H1_Needler", ["weapons\\needler\\fp\\fp", "weapons\\needler\\needler"]),
    ("SM_H1_Pistol", ["weapons\\pistol\\fp\\fp", "weapons\\pistol\\pistol"]),
    ("SM_H1_PlasmaPistol", ["weapons\\plasma pistol\\fp\\fp", "weapons\\plasma pistol\\plasma pistol"]),
    ("SM_H1_PlasmaRifle", ["weapons\\plasma rifle\\fp\\fp", "weapons\\plasma rifle\\plasma rifle"]),
    ("SM_H1_RocketLauncher", ["weapons\\rocket launcher\\fp\\fp", "weapons\\rocket launcher\\rocket launcher"]),
    ("SM_H1_Shotgun", ["weapons\\shotgun\\fp\\fp", "weapons\\shotgun\\shotgun"]),
    ("SM_H1_SniperRifle", ["weapons\\sniper rifle\\fp\\fp", "weapons\\sniper rifle\\sniper rifle"]),
    ("SM_warthog", ["vehicles\\warthog\\warthog"]),
    # (the Ghost's and Banshee's preview meshes carry the editor's placeholder material: their skins' material
    # instances, named here, go with their UVs)
    ("SM_Ghost", ["vehicles\\ghost\\ghost"], "MI_H1S2_VS_GHOST_DEFAULT"),
    ("SM_Banshee", ["vehicles\\banshee\\banshee"], "MI_H1S2_VS_BANSHEE_DEFAULT"),
    ("SM_Scorpion", ["vehicles\\scorpion\\scorpion", "vehicles\\scorpion\\scorpion_mp\\scorpion_mp"]),
]
# a texture's role by the end of its name (any case); a texture of none of them is the diffuse
ROLE_SUFFIXES = [("_MULTIPURPOSE", "multipurpose"), ("_METALLIC", "metallic"), ("_ROUGHNESS", "roughness"),
                 ("_NORMAL", "normal"), ("_NM", "normal"), ("_EMISSIVE", "emissive"), ("_AO", "ao"),
                 ("_OPACITY", None)]
TEXTURE_FOLDER = "MCC/Content/UI/Features/Customization/SharedTextures/H1/"


def texture_role(name: str):
    upper = name.upper()
    for suffix, role in ROLE_SUFFIXES:
        if upper.endswith(suffix):
            return role
    return "diffuse"


class Materials:
    """each material instance's textures by role: the H1S2 textures its package imports (its texture parameters'
    values), and where it imports none of a role, the texture named as its diffuse's with that role's suffix"""

    def __init__(self, pak: Pak):
        self.pak = pak
        self.names = {}
        for path in pak.entries:
            if path.startswith(TEXTURE_FOLDER) and path.endswith(".uasset"):
                name = path.rsplit("/", 1)[-1][:-len(".uasset")]
                self.names[name.upper()] = name
        self.found = {}

    def roles(self, material: str) -> dict:
        if material in self.found:
            return self.found[material]
        roles = {}
        try:
            asset = pak_asset(self.pak, MESH_ROOT, material)
            _, imports = package_tables(self.pak.read(asset + ".uasset"))
        except LookupError:
            imports = []
        for kind, name in imports:
            if kind == "Texture2D" and name.upper().startswith("H1S2_") and name.upper() in self.names:
                role = texture_role(name)
                if role and role not in roles:
                    roles[role] = name
        diffuse = roles.get("diffuse")
        if diffuse:
            for suffix, role in ROLE_SUFFIXES:
                if role and role not in roles and (diffuse + suffix).upper() in self.names:
                    roles[role] = self.names[(diffuse + suffix).upper()]
        self.found[material] = roles
        return roles


def pak_asset(pak: Pak, root: str, name: str) -> str:
    """the one .uasset named `name` under `root`, without its extension"""
    found = [path for path in pak.entries if path.startswith(root) and path.endswith("/" + name + ".uasset")]
    if len(found) != 1:
        raise LookupError(f"{name}: {len(found)} assets of that name")
    return found[0][:-len(".uasset")]


class Maps:
    """the Xbox maps of a folder, read as they are needed (one at a time), and which one has each model"""

    def __init__(self, folder: Path):
        self.paths = sorted(path for path in Path(folder).glob("*.map") if path.name != "ui.map")
        self.cached = (None, None)

    def get(self, path):
        if self.cached[0] != path:
            self.cached = (path, XboxMap(path))
        return self.cached[1]

    def find_model(self, name: str):
        """(the map's file name, the map, the model's tag address) of the first map that has the model"""
        for path in self.paths:
            game_map = self.get(path)
            for group, found, address in game_map.instances:
                if group == "mode" and found.lower() == name.lower() and address:
                    return path.name, game_map, address
        return None, None, None


def bake_size(width: int, height: int, max_size: int):
    factor = 1
    while factor < 4 and max(width, height) * factor * 2 <= max_size:
        factor *= 2
    return width * factor, height * factor


def dilate(image: np.ndarray, have: np.ndarray, steps: int):
    """`image`'s texels where `have` is false taken from their neighbors where it is true, `steps` texels out (the
    padding past an island's edge that filtering and mips read)"""
    image, have = image.astype(np.float64).copy(), have.copy()
    for _ in range(steps):
        grow = np.zeros(image.shape, np.float64)
        count = np.zeros(have.shape)
        for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            shifted = np.roll(have, (dy, dx), (0, 1))
            grow += np.roll(image, (dy, dx), (0, 1)) * shifted[..., None]
            count += shifted
        new = ~have & (count > 0)
        if not new.any():
            break
        image[new] = grow[new] / count[new][:, None]
        have |= new
    return image, have


def masked_blur(values: np.ndarray, mask: np.ndarray, radius: int) -> np.ndarray:
    """values (h, w) blurred by a box `radius` texels across, three times (about a Gaussian), over `mask` only (an
    island's texels never take its neighbors' across the empty space between them)"""
    from scipy.ndimage import uniform_filter
    weight = mask.astype(np.float64)
    total = np.where(mask, values, 0.0).astype(np.float64)
    for _ in range(3):
        total = uniform_filter(total, 2 * radius + 1, mode="wrap")
        weight = uniform_filter(weight, 2 * radius + 1, mode="wrap")
    return total / np.maximum(weight, 1e-6)


def levels(rgb: np.ndarray):
    """mean luminance, mean saturation and the luminance's standard deviation of (n, 3) colors of 0-255"""
    luminance = rgb @ np.array([0.299, 0.587, 0.114])
    high, low = rgb.max(1), rgb.min(1)
    saturation = np.where(high > 1, (high - low) / np.maximum(high, 1), 0)
    return luminance.mean(), saturation.mean(), luminance.std()


def unit(v: np.ndarray) -> np.ndarray:
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), 1e-9)


# ---------------------------------------------------------------- the bake

class Mesh:
    """an MCC preview mesh, with its surface to find closest points on and its sections' textures"""

    def __init__(self, pak: Pak, materials: Materials, name: str, rng, fallback: str = ""):
        asset = pak_asset(pak, MESH_ROOT, name)
        (self.positions, self.uvs, self.triangles, self.section_of, self.sections,
         self.tangents) = read_static_mesh(pak.read(asset + ".uasset"), pak.read(asset + ".uexp"))
        self.roles = [materials.roles(material) or (materials.roles(fallback) if fallback else {})
                      for (_, _, _, material) in self.sections]
        self.surface = Surface(self.positions, self.triangles, rng)
        self.samples, _, _ = sample_surface(self.positions, self.triangles, 60000, rng)


def lookup(mesh: Mesh, pieces, width: int, height: int, want_frames: bool):
    """for each texel of a width x height texture of our pieces (uv, triangles, positions, normals, fit): 0 not
    drawn, 1 found, 2 nothing close; the closest MCC triangle and its barycentrics; and with want_frames, our
    tangent frame there and MCC's, both in our model's space"""
    best = np.full((height, width), np.inf)
    state = np.zeros((height, width), np.int8)
    tri_at = np.full((height, width), -1, np.int64)
    bary_at = np.zeros((height, width, 3), np.float32)
    ours_frame = theirs_frame = None
    if want_frames:
        ours_frame = np.zeros((height, width, 3, 3), np.float32)
        theirs_frame = np.zeros((height, width, 3, 3), np.float32)
    for uv, tris, xyz, normals, (s, r, t, near) in pieces:
        tri_of, bary = raster_bary(uv, tris, width, height)
        mask = tri_of >= 0
        ours_tri = tris[tri_of[mask]]
        weights = bary[mask]
        points = np.einsum("ni,nij->nj", weights, xyz[ours_tri])
        d, tri, b = mesh.surface.closest(s * points @ r.T + t)
        iy, ix = np.nonzero(mask)
        better = d < best[iy, ix]
        y, x = iy[better], ix[better]
        best[y, x] = d[better]
        state[y, x] = np.where(d[better] <= near, 1, 2)
        tri_at[y, x] = tri[better]
        bary_at[y, x] = b[better]
        if want_frames:
            # ours: the vertex normal, the tangent along +u and the bitangent along +v of the triangle's UVs
            n = unit(np.einsum("ni,nij->nj", weights, normals[ours_tri]))
            p, q = xyz[ours_tri], uv[ours_tri]
            e1, e2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
            du1, dv1 = q[:, 1, 0] - q[:, 0, 0], q[:, 1, 1] - q[:, 0, 1]
            du2, dv2 = q[:, 2, 0] - q[:, 0, 0], q[:, 2, 1] - q[:, 0, 1]
            det = du1 * dv2 - du2 * dv1
            det = np.where(np.abs(det) < 1e-12, 1e-12, det)[:, None]
            tangent = (e1 * dv2[:, None] - e2 * dv1[:, None]) / det
            bitangent = (e2 * du1[:, None] - e1 * du2[:, None]) / det
            tangent = unit(tangent - n * (n * tangent).sum(1, keepdims=True))
            bitangent = unit(bitangent - n * (n * bitangent).sum(1, keepdims=True) -
                             tangent * (tangent * bitangent).sum(1, keepdims=True))
            ours_frame[y, x] = np.stack([tangent, bitangent, n], 1)[better]
            # MCC's: its vertices' tangent and normal, the bitangent cross(normal, tangent) times their sign (as
            # Unreal's shaders make it), turned into our model's space
            vt = mesh.triangles[tri]
            tx = np.einsum("ni,nij->nj", b, mesh.tangents[0][vt])
            tz = np.einsum("ni,nij->nj", b, mesh.tangents[1][vt])
            sign = mesh.tangents[2][vt[np.arange(len(vt)), b.argmax(1)]]
            tx, tz = unit(tx), unit(tz)
            ty = np.cross(tz, tx) * sign[:, None]
            frame = np.stack([tx, ty, tz], 1) @ r  # (rows: directions in MCC's space) -> ours: R^T v, as rows v R
            theirs_frame[y, x] = frame[better]
    return {"state": state, "tri": tri_at, "bary": bary_at, "ours": ours_frame, "theirs": theirs_frame}


class Textures:
    def __init__(self, pak: Pak):
        self.pak, self.loaded = pak, {}

    def get(self, name: str):
        if name not in self.loaded:
            try:
                self.loaded[name] = mcc_texture(self.pak, name)
            except (ValueError, LookupError, KeyError) as error:
                print(f"      ({name}: not read: {error})")
                self.loaded[name] = None
        return self.loaded[name]


def sample_role(mesh: Mesh, textures: Textures, look: dict, role: str):
    """each found texel's MCC texel of a role's texture (RGBA), NaN where its section has none"""
    state, tri = look["state"], look["tri"]
    out = np.full(state.shape + (4,), np.nan, np.float32)
    found = state == 1
    tris = tri[found]
    sections = mesh.section_of[tris]
    uv = np.einsum("ni,nij->nj", look["bary"][found], mesh.uvs[mesh.triangles[tris], 0])
    values = np.full((len(tris), 4), np.nan, np.float32)
    for number, roles in enumerate(mesh.roles):
        name = roles.get(role)
        sel = sections == number
        if not name or not sel.any():
            continue
        image = textures.get(name)
        if image is None:
            continue
        values[sel] = bilinear(image, uv[sel, 0] * image.shape[1], uv[sel, 1] * image.shape[0], wrap=True)
    out[found] = values
    return out


def classic_enlarged(classic: np.ndarray, width: int, height: int) -> np.ndarray:
    ys, xs = np.mgrid[0:height, 0:width]
    h, w = classic.shape[:2]
    return bilinear(classic.astype(np.float64), (xs + 0.5) * w / width, (ys + 0.5) * h / height, wrap=True)


def fit_to(values: np.ndarray, target: np.ndarray):
    """values scaled and shifted to best match target (least squares), and their correlation"""
    if len(values) < 100 or values.std() < 1e-3 or target.std() < 1e-3:
        return None, 0.0
    corr = float(np.corrcoef(values, target)[0, 1])
    slope = corr * target.std() / values.std()
    return (lambda v: (v - values.mean()) * slope + target.mean()), corr


def write_bitmap(mod: Path, game_map, tag: int, image: np.ndarray, alpha_wanted: bool):
    info = game_map.bitmap(tag)
    fmt = DXT5 if alpha_wanted and image[:, :, 3].min() < 250 else DXT1
    name = game_map.instances[tag][1]
    target = mod / "tags" / (name.replace("\\", "/") + ".bitmap")
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(bitmap_tag_file(game_map, info, np.clip(image, 0, 255), fmt))
    return "DXT5" if fmt == DXT5 else "DXT1"


def has_alpha(info: dict) -> bool:
    return info["format"] in (DXT3, DXT5, A8R8G8B8)


def bake_diffuse(mesh, textures, game_map, tag, pieces, args, extras):
    info = game_map.bitmap(tag)
    name = game_map.instances[tag][1]
    if info["count"] != 1 or info["type"] != 0:
        print(f"    {name}: {info['count']} bitmaps of type {info['type']}, left out")
        return
    width, height = bake_size(info["width"], info["height"], args.max_size)
    look = lookup(mesh, pieces, width, height, want_frames=extras is not None)
    diffuse = sample_role(mesh, textures, look, "diffuse")
    baked = ~np.isnan(diffuse[:, :, 0])
    state = look["state"]
    drawn = max((state > 0).sum(), 1)
    coverage = baked.sum() / drawn
    line = (f"    {name}: {info['width']}x{info['height']} -> {width}x{height}, baked {coverage:.1%} of "
            f"{drawn} texels drawn (nothing close {(state == 2).sum() / drawn:.1%}, no MCC diffuse there "
            f"{((state == 1) & ~baked).sum() / drawn:.1%})")
    if coverage < args.coverage:
        print(line + " -- too little, left out")
        return
    out = classic_enlarged(game_map.bitmap_rgba(tag), width, height)
    # MCC's paint is lighter and greyer than ours by design: each channel's mean and contrast over the baked texels
    # set to ours there, so MCC brings its detail and our colors and levels stay
    mcc, ours = diffuse[baked, :3].astype(np.float64), out[baked, :3]
    matched = (mcc - mcc.mean(0)) * (ours.std(0) / np.maximum(mcc.std(0), 1e-3)) + ours.mean(0)
    adjusted = np.zeros_like(diffuse[:, :, :3], dtype=np.float64)
    adjusted[baked] = matched
    stats = [levels(ours), levels(mcc), levels(np.clip(matched, 0, 255))]
    line += ("; luminance, saturation, contrast: ours " + ", ".join(f"{v:.2f}" for v in stats[0]) +
             "; MCC as baked " + ", ".join(f"{v:.2f}" for v in stats[1]) +
             "; matched " + ", ".join(f"{v:.2f}" for v in stats[2]))
    padded, have = dilate(adjusted, baked, 8)
    take = have & (state != 2) & ~((state == 1) & ~baked)
    out[take, :3] = padded[take]
    fmt = write_bitmap(args.mod, game_map, tag, out, has_alpha(info))
    # (the DXT encoding's own effect on the levels: the top mip decoded again)
    encoded = (encode_dxt5 if fmt == "DXT5" else encode_dxt1)(np.clip(out, 0, 255))
    decoded = decode_dxt(encoded, width, height, 16 if fmt == "DXT5" else 8)
    shift = levels(decoded[baked, :3].astype(np.float64))[0] - levels(np.clip(out[baked, :3], 0, 255))[0]
    print(line + f", {fmt} (its encoding moves the mean luminance {shift:+.2f})")
    if extras is not None:
        write_extras(mesh, textures, look, extras / name.replace("\\", "/"), width, height,
                     game_map.bitmap_rgba(tag))


def write_extras(mesh, textures, look, stem: Path, width, height, classic=None):
    """MCC's normal map and its AO, roughness and metalness in our layout, as the level 2 renderer reads them beside
    the base map: <stem>.normal.png (tangent space, red along +u, green toward the picture's top, OpenGL style) and
    <stem>.orm.png (red AO, green roughness, blue metalness)"""
    stem.parent.mkdir(parents=True, exist_ok=True)
    found = look["state"] == 1
    normal = sample_role(mesh, textures, look, "normal")
    ok = found & ~np.isnan(normal[:, :, 0])
    nx, ny = normal[ok, 0] / 127.5 - 1, normal[ok, 1] / 127.5 - 1
    nz = np.sqrt(np.clip(1 - nx * nx - ny * ny, 0, 1))
    # MCC's tangent space (Unreal's frame from its own vertex tangents) -> our model's space (the fit's rotation,
    # the mirror with it) -> our tangent space (+u, +v, the normal); the picture's top is -v
    model = np.einsum("ni,nij->nj", np.stack([nx, ny, nz], 1), look["theirs"][ok])
    ours = unit(np.einsum("nij,nj->ni", look["ours"][ok], model)) * np.array([1, -1, 1])
    image = np.zeros((height, width, 3))
    image[ok] = (ours * 0.5 + 0.5) * 255
    image, have = dilate(image, ok, 8)
    image[~have] = (128, 128, 255)
    if ok.sum() > 100:
        Image.fromarray(np.clip(np.round(image), 0, 255).astype(np.uint8)).save(str(stem) + ".normal.png")
    # the renderer draws a surface without these maps as the classic graphics do: no occlusion, sharp reflection
    # (roughness 0), no metal darkening. That is the baseline here: MCC's maps only add to it where they stand out
    # (the occlusion's brightest tenth unoccluded, roughness above its median, metal above half), so they add
    # variation and never make the whole surface duller or darker than the classic one
    channels = []
    notes = []
    for role in ("ao", "roughness", "metallic"):
        values = sample_role(mesh, textures, look, role)[:, :, 0]
        have_role = found & ~np.isnan(values)
        raw = values[have_role].astype(np.float64)
        if role == "ao":
            mapped = np.clip(raw / max(np.percentile(raw, 90), 1) * 255, 0, 255) if len(raw) else raw
            default = 255
        elif role == "roughness":
            mapped = np.clip((raw - np.median(raw)) * 0.5, 0, 255) if len(raw) else raw
            default = 0
        else:
            mapped = np.clip((raw - 128) * 2 * 0.5, 0, 255) if len(raw) else raw
            default = 0
        layer = np.zeros((height, width, 1))
        layer[have_role, 0] = mapped
        layer, have = dilate(layer, have_role, 8)
        layer[~have] = default
        channels.append(layer[:, :, 0])
        if len(raw):
            notes.append(f"{role} mean {raw.mean():.0f} -> {mapped.mean():.0f}")
    orm = np.stack(channels, 2)
    Image.fromarray(np.clip(np.round(orm), 0, 255).astype(np.uint8)).save(str(stem) + ".orm.png")
    flat = float(np.mean(ours[:, 2])) if len(ours) else 0.0
    source = float(np.mean(nz)) if len(nz) else 0.0
    print(f"      normal map on {ok.sum() / max(found.sum(), 1):.0%} of the found texels (mean z {flat:.3f}, MCC's own "
          f"{source:.3f}); "
          f"orm: " + ", ".join(notes))


def bake_multipurpose(mesh, textures, game_map, tag, pieces, args):
    """our multipurpose map (the Xbox's channels: red the specular or reflection mask, green self-illumination,
    blue the change-color mask, alpha the detail mask) with its masks from MCC's maps where they agree with ours:
    MCC's multipurpose channel that best matches each of ours (else for the specular mask, MCC's metallic,
    roughness and AO), fitted to our mask's levels; the change-color mask always ours"""
    info = game_map.bitmap(tag)
    name = game_map.instances[tag][1]
    if info["count"] != 1 or info["type"] != 0:
        print(f"    {name}: {info['count']} bitmaps of type {info['type']}, left out")
        return
    width, height = bake_size(info["width"], info["height"], args.max_size)
    look = lookup(mesh, pieces, width, height, want_frames=False)
    classic = classic_enlarged(game_map.bitmap_rgba(tag), width, height)
    found = look["state"] == 1
    multipurpose = sample_role(mesh, textures, look, "multipurpose")
    metallic = sample_role(mesh, textures, look, "metallic")[:, :, 0]
    roughness = sample_role(mesh, textures, look, "roughness")[:, :, 0]
    ao = sample_role(mesh, textures, look, "ao")[:, :, 0]
    emissive = sample_role(mesh, textures, look, "emissive")[:, :, :3].mean(2)
    out = classic.copy()
    notes = []
    has_mp = found & ~np.isnan(multipurpose[:, :, 0])
    if has_mp.sum() > 100:
        # which of MCC's multipurpose channels each of ours is (the open channel-order question)
        rows = []
        for ours in range(4):
            rows.append([fit_to(multipurpose[has_mp, theirs], classic[has_mp, ours])[1] for theirs in range(3)])
        notes.append("MCC multipurpose RGB vs ours (spec, self-illum, change color, detail): " +
                     "; ".join(",".join(f"{c:+.2f}" for c in row) for row in rows))
    for channel, label in ((0, "spec"), (1, "self-illum"), (3, "detail")):
        target = classic[:, :, channel]
        if target[found].max() < 16 and channel != 0:
            notes.append(f"{label} empty in ours, kept")
            continue
        candidates = []
        if has_mp.sum() > 100:
            for theirs in range(3):
                candidates.append((f"MCC mp {'RGB'[theirs]}", multipurpose[:, :, theirs]))
        if channel == 0:
            rough = np.where(np.isnan(roughness), np.nan, 1 - roughness / 255)
            metal = np.where(np.isnan(metallic), 0.0, metallic / 255)
            occlusion = np.where(np.isnan(ao), 1.0, ao / 255)
            candidates.append(("(1-rough)(0.5+0.5 metal) AO", rough * (0.5 + 0.5 * metal) * occlusion))
        if channel == 1:
            candidates.append(("MCC emissive", emissive))
        best = None
        for label2, values in candidates:
            ok = found & ~np.isnan(values)
            mapping, corr = fit_to(values[ok], target[ok])
            # (the MCC map must cover most of the texels it would replace)
            if (mapping is not None and corr > 0.3 and ok.sum() >= 0.5 * found.sum() and
                    (best is None or corr > best[1])):
                best = (label2, corr, mapping, values)
        if best is None:
            notes.append(f"{label} kept ours (no MCC map agrees)")
            continue
        label2, corr, mapping, values = best
        ok = found & ~np.isnan(values)
        # our mask kept as it is, with MCC's fine detail (its fitted map less its own blur, about a classic
        # texel and a half across) added on top: the level and the broad shapes stay ours
        fitted = np.zeros((height, width))
        fitted[ok] = mapping(values[ok])
        radius = max(2, int(round(1.5 * width / info["width"])))
        detail = np.where(ok, fitted - masked_blur(fitted, ok, radius), 0.0)
        before = target[ok].mean()
        replaced = fitted[ok].mean()
        layer = np.clip(target + detail, 0, 255)[:, :, None]
        padded, have = dilate(layer, ok, 8)
        take = have & (look["state"] != 2)
        out[take, channel] = padded[take, 0]
        notes.append(f"{label}: ours with {label2}'s detail (r {corr:.2f}; mean ours {before:.1f}, "
                     f"the first bake's {replaced:.1f}, now {out[ok, channel].mean():.1f})")
    line = (f"    {name} (multipurpose): {info['width']}x{info['height']} -> {width}x{height}, found "
            f"{found.sum() / max((look['state'] > 0).sum(), 1):.1%}; " + "; ".join(notes))
    if not any("detail (r" in note for note in notes):
        print(line + " -- nothing from MCC, left out")
        return
    print(line + ", " + write_bitmap(args.mod, game_map, tag, out, has_alpha(info)))


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--maps", required=True, type=Path, help="a folder of Xbox maps")
    parser.add_argument("--pak", required=True, type=Path)
    parser.add_argument("--mod", required=True, type=Path, help="the mod's folder (written)")
    parser.add_argument("--no-extras", action="store_true", help="leave out the .normal.png and .orm.png maps")
    parser.add_argument("--only", default="", help="the meshes to do, by part of their names (all by default)")
    parser.add_argument("--max-size", type=int, default=2048)
    parser.add_argument("--near", type=float, default=0.01, help="'close', as a fraction of our model's size")
    parser.add_argument("--fit", type=float, default=0.8, help="the share of our surface that must be close")
    parser.add_argument("--coverage", type=float, default=0.5, help="the share of a bitmap's texels to bake")
    parser.add_argument("--fits-out", type=Path, help="write each model's fit (numbers only: the game's "
                        "port/assets/mcc_import/mcc_import.json) here")
    parser.add_argument("--fits-in", type=Path, help="use these fits (a --fits-out file) instead of fitting")
    args = parser.parse_args(argv)
    rng = np.random.default_rng(1257)
    pak = Pak(args.pak)
    maps = Maps(args.maps)
    materials = Materials(pak)
    textures = Textures(pak)
    only = [word.lower() for word in args.only.split(",") if word]
    fits_in = {}
    if args.fits_in:
        for entry in json.loads(args.fits_in.read_text())["meshes"]:
            for model in entry["models"]:
                fits_in[(entry["mesh"], model["model"])] = model
    fits_out = []
    for mesh_name, model_names, *fallback in TARGETS:
        if only and not any(word in mesh_name.lower() for word in only):
            continue
        start = time.time()
        mesh = Mesh(pak, materials, mesh_name, rng, *fallback)
        print(f"{mesh_name}: {len(mesh.positions)} vertices, {len(mesh.triangles)} triangles; textures by section: " +
              "; ".join(f"{material}: {','.join(sorted(roles)) or '-'}"
                        for (_, _, _, material), roles in zip(mesh.sections, mesh.roles)))
        # the bitmaps to bake: (map, tag) -> pieces of our fitted models that draw it
        diffuse, multipurpose = {}, {}
        fits_entry = []
        fits_out.append({"mesh": mesh_name, "fallback": fallback[0] if fallback else "",
                         "vertices": int(len(mesh.positions)), "triangles": int(len(mesh.triangles)),
                         "models": fits_entry})
        for name in model_names:
            map_name, game_map, address = maps.find_model(name)
            if game_map is None:
                print(f"  {name}: in none of the maps")
                continue
            parts = game_map.model_parts(address, base_only=True)
            bases = np.cumsum([0] + [len(p[1]) for p in parts])[:-1]
            xyz = np.concatenate([p[3] for p in parts]).astype(np.float64)
            tris = np.concatenate([p[2] + b for p, b in zip(parts, bases)])
            corners = xyz[tris].reshape(-1, 3)
            extent = np.linalg.norm(corners.max(0) - corners.min(0))
            ours, _, _ = sample_surface(xyz, tris, 60000, rng)
            if (mesh_name, name) in fits_in:
                shipped = fits_in[(mesh_name, name)]
                s, r, t = shipped["scale"], np.array(shipped["rotation"]).reshape(3, 3), np.array(shipped["shift"])
                reflect = np.linalg.det(r) < 0
            else:
                s, r, t, reflect = align(ours, mesh.samples)
            dist, _, _ = mesh.surface.closest(s * ours @ r.T + t)
            close = (dist <= args.near * s * extent).mean()
            print(f"  {name} ({map_name}): scale {s:.1f}{', mirrored' if reflect else ''}; distance to MCC's "
                  f"surface median {np.median(dist) / (s * extent):.4f} of the model's size, {close:.1%} within "
                  f"{args.near:.0%}" + ("" if close >= args.fit else " -- does not fit, skipped"))
            if close < args.fit:
                continue
            fit = (s, r, t, args.near * s * extent)
            fits_entry.append({"model": name, "map": map_name[:-len(".map")], "scale": float(s),
                               "rotation": [float(v) for v in np.asarray(r).reshape(-1)],
                               "shift": [float(v) for v in t], "near": float(fit[3])})
            for base, uv, part_tris, part_xyz, normals, mp in parts:
                piece = (uv.astype(np.float64), part_tris, part_xyz.astype(np.float64), normals.astype(np.float64),
                         fit)
                key = game_map.instances[base][1] if base is not None else None
                if key:
                    diffuse.setdefault(key, (game_map, base, []))[2].append(piece)
                key = game_map.instances[mp][1] if mp is not None else None
                if key:
                    multipurpose.setdefault(key, (game_map, mp, []))[2].append(piece)
        # (each bitmap baked from the first map that has it: maps are read one at a time, so read it again)
        for kind, table in (("diffuse", diffuse), ("multipurpose", multipurpose)):
            for name, (game_map, tag, pieces) in sorted(table.items()):
                if game_map.instances[tag][1] != name:
                    continue
                if kind == "diffuse":
                    extras = None if args.no_extras else args.mod / "tags"
                    bake_diffuse(mesh, textures, game_map, tag, pieces, args, extras)
                else:
                    bake_multipurpose(mesh, textures, game_map, tag, pieces, args)
        print(f"  ({time.time() - start:.0f} s)")
        textures.loaded.clear()
    if args.fits_out:
        # (the texture-only layouts, which have no fit, kept from the file as they are)
        kept = json.loads(args.fits_out.read_text()).get("layouts") if args.fits_out.is_file() else None
        fits = {"version": 1, "meshes": fits_out, **({"layouts": kept} if kept else {})}
        args.fits_out.write_text(json.dumps(fits, indent=1) + "\n")
    args.mod.mkdir(parents=True, exist_ok=True)
    (args.mod / "mod.toml").write_text(
        'name = "MCC HD textures (test)"\nid = "mcc_hd_test"\nversion = "0.1"\n'
        'description = "Weapon and vehicle textures from your own MCC install"\n'
        '# the multipurpose maps are the Xbox maps\' own, in the Xbox\'s channel order\n'
        'channel_order = "xbox"\n')
    count = len(list((args.mod / "tags").rglob("*.bitmap"))) if (args.mod / "tags").exists() else 0
    print(f"{count} bitmaps in {args.mod}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
