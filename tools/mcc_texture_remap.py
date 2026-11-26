"""MCC's high-res weapon and vehicle textures remapped onto our models' layouts (issue #1257, route A).

MCC's menu previews have 2048x2048 versions of the weapons' and vehicles' textures
(MCC/Content/Paks/MCC-WindowsNoEditor.pak, UI/Features/Customization/SharedTextures/H1/...), each the classic
atlas with its islands moved and resized. This tool works out, once and offline, where each UV island of our own
model went (a recipe: island bounds and transforms, numbers only, no MCC pixels), and applies a recipe to a player's
own MCC install to build a bitmap mod in the classic layout at the high resolution.

    python3 tools/mcc_texture_remap.py list --pak <MCC pak> [--filter H1S2_WS_ASSAULT]
    python3 tools/mcc_texture_remap.py recipe --map bloodgulch.map --bitmap "vehicles\\warthog\\bitmaps\\warthog" \\
        --pak <MCC pak> --texture H1S2_VS_WARTHOG_DEFAULT --out warthog.json [--debug-dir <folder>]
    python3 tools/mcc_texture_remap.py apply --map bloodgulch.map --pak <MCC pak> --mod <mods/mcc_hd_test> \\
        warthog.json assault_rifle.json

Needs numpy and Pillow. The map is an Xbox map (compressed after its header); the MCC pak is read, never written.
"""

import argparse
import json
import struct
import sys
import time
import zlib
from pathlib import Path

import numpy as np

# ---------------------------------------------------------------- the Unreal pak (UE4 pak version 7)

PAK_MAGIC = 0x5A6F12E1
TEXTURE_ROOT = "MCC/Content/UI/Features/Customization/SharedTextures/H1/"


class Pak:
    """an Unreal pak's index (version 7, not encrypted): file name -> (offset of its data, size)"""

    def __init__(self, path: Path):
        self.path = Path(path)
        self.file = open(self.path, "rb")
        self.file.seek(-44, 2)
        footer = self.file.read(44)
        magic, version, index_offset, index_size = struct.unpack_from("<IIQQ", footer, 0)
        if magic != PAK_MAGIC or version != 7:
            raise ValueError(f"{path}: not a version 7 Unreal pak (magic {magic:#x}, version {version})")
        self.file.seek(index_offset)
        index = self.file.read(index_size)
        mount, at = self._string(index, 0)
        count, = struct.unpack_from("<i", index, at)
        at += 4
        self.entries = {}
        for _ in range(count):
            name, at = self._string(index, at)
            offset, size, _, compression = struct.unpack_from("<QQQI", index, at)
            at += 28 + 20
            if compression:
                blocks, = struct.unpack_from("<i", index, at)
                at += 4 + 16 * blocks
            at += 1 + 4  # encrypted, compression block size
            self.entries[mount.replace("../../../", "") + name] = (offset, size, compression)

    @staticmethod
    def _string(data: bytes, at: int):
        length, = struct.unpack_from("<i", data, at)
        at += 4
        if length < 0:
            text = data[at:at - 2 * length].decode("utf-16-le")
            return text.rstrip("\0"), at - 2 * length
        return data[at:at + length].rstrip(b"\0").decode("utf-8"), at + length

    def read(self, name: str) -> bytes:
        offset, size, compression = self.entries[name]
        if compression:
            raise ValueError(f"{name}: compressed entries are not read")
        # (each entry's data follows a copy of its index record: 53 bytes uncompressed)
        self.file.seek(offset + 53)
        return self.file.read(size)

    def find_texture(self, name: str) -> str:
        """the .uasset path of texture `name` (H1S2_VS_WARTHOG_DEFAULT) under the customization textures"""
        matches = [path for path in self.entries if path.startswith(TEXTURE_ROOT) and
                   path.rsplit("/", 1)[-1] == name + ".uasset"]
        if len(matches) != 1:
            raise LookupError(f"{name}: {len(matches)} textures of that name in the pak")
        return matches[0]


# the pixel formats a Texture2D's platform data can have here, and their bytes per 4x4 block
UE_FORMATS = {"PF_DXT1": 8, "PF_DXT5": 16, "PF_BC5": 16}


def read_texture(pak: Pak, name: str):
    """texture `name`'s top mip as (format, width, height, its blocks' bytes): the .uexp's platform data names its
    format and size, and the big mips are in the .ubulk, the largest first"""
    asset = pak.find_texture(name)
    uexp = pak.read(asset[:-len(".uasset")] + ".uexp")
    uasset = pak.read(asset)
    format_name = next((key for key in UE_FORMATS if key.encode() in uasset), None)
    if not format_name:
        raise ValueError(f"{name}: not a DXT1/DXT5 texture")
    block = UE_FORMATS[format_name]
    bulk_name = asset[:-len(".uasset")] + ".ubulk"
    bulk = pak.read(bulk_name) if bulk_name in pak.entries else b""
    # the platform data: SizeX, SizeY, NumSlices, then the format's name as an FString; find it by that name
    marker = struct.pack("<i", len(format_name) + 1) + format_name.encode() + b"\0"
    at = uexp.find(marker)
    if at < 12:
        raise ValueError(f"{name}: no platform data")
    width, height, _ = struct.unpack_from("<iii", uexp, at - 12)
    top = (width // 4) * (height // 4) * block
    if len(bulk) >= top:
        data = bulk[:top]
    else:
        raise ValueError(f"{name}: its top mip ({top} bytes) is not in the .ubulk ({len(bulk)} bytes)")
    return format_name, width, height, data


# ---------------------------------------------------------------- DXT decoding and encoding

def _565(colors: np.ndarray) -> np.ndarray:
    r = (colors >> 11) & 31
    g = (colors >> 5) & 63
    b = colors & 31
    return np.stack([(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)], -1).astype(np.float32)


def decode_dxt(data: bytes, width: int, height: int, block_size: int, explicit_alpha: bool = False) -> np.ndarray:
    """DXT1 or DXT5 (or with explicit_alpha DXT3) blocks as an RGBA float array (height, width, 4) of 0-255"""
    bw, bh = width // 4, height // 4
    blocks = np.frombuffer(data, np.uint8)[:bw * bh * block_size].reshape(bh * bw, block_size)
    color = blocks[:, block_size - 8:]
    c0 = color[:, 0].astype(np.uint32) | (color[:, 1].astype(np.uint32) << 8)
    c1 = color[:, 2].astype(np.uint32) | (color[:, 3].astype(np.uint32) << 8)
    bits = (color[:, 4].astype(np.uint32) | (color[:, 5].astype(np.uint32) << 8) |
            (color[:, 6].astype(np.uint32) << 16) | (color[:, 7].astype(np.uint32) << 24))
    p0, p1 = _565(c0), _565(c1)
    four = (c0 > c1) | (block_size == 16)
    palette = np.empty((len(blocks), 4, 4), np.float32)
    palette[:, 0, :3], palette[:, 1, :3] = p0, p1
    palette[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) / 3, (p0 + p1) / 2)
    palette[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) / 3, 0)
    palette[:, :, 3] = 255
    palette[:, 3, 3] = np.where(four, 255, 0)
    indices = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    pixels = np.take_along_axis(palette, indices[:, :, None].astype(np.int64).repeat(4, 2), 1)
    if block_size == 16 and explicit_alpha:
        nibbles = np.stack([blocks[:, :8] & 15, blocks[:, :8] >> 4], 2).reshape(-1, 16)
        pixels[:, :, 3] = nibbles.astype(np.float32) * 17
    elif block_size == 16:
        alpha = blocks[:, :8]
        a0, a1 = alpha[:, 0].astype(np.float32), alpha[:, 1].astype(np.float32)
        abits = np.zeros(len(blocks), np.uint64)
        for byte in range(6):
            abits |= alpha[:, 2 + byte].astype(np.uint64) << np.uint64(8 * byte)
        aidx = ((abits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)).astype(np.int64)
        steps = np.arange(8, dtype=np.float32)
        eight = (a0 > a1)[:, None]
        apal = np.where(eight, np.where(steps == 0, a0[:, None], np.where(steps == 1, a1[:, None],
                                         ((8 - steps) * a0[:, None] + (steps - 1) * a1[:, None]) / 7)),
                        np.where(steps == 0, a0[:, None], np.where(steps == 1, a1[:, None],
                                 np.where(steps == 6, 0, np.where(steps == 7, 255,
                                          ((6 - steps) * a0[:, None] + (steps - 1) * a1[:, None]) / 5)))))
        pixels[:, :, 3] = np.take_along_axis(apal, aidx, 1)
    return pixels.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(height, width, 4)


def encode_dxt1(image: np.ndarray) -> bytes:
    """an RGB image (height, width, 3+) of 0-255, both sides multiples of 4, as DXT1 blocks (four colors: each
    block's endpoints along its colors' main axis, refined once by least squares)"""
    height, width = image.shape[:2]
    pixels = image[:, :, :3].astype(np.float32).reshape(height // 4, 4, width // 4, 4, 3)
    pixels = pixels.transpose(0, 2, 1, 3, 4).reshape(-1, 16, 3)
    mean = pixels.mean(1, keepdims=True)
    centered = pixels - mean
    covariance = np.einsum("bki,bkj->bij", centered, centered)
    axis = np.ones((len(pixels), 3), np.float32)
    for _ in range(6):
        axis = np.einsum("bij,bj->bi", covariance, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum("bki,bi->bk", centered, axis)
    lo, hi = t.min(1), t.max(1)
    e0 = mean[:, 0] + hi[:, None] * axis
    e1 = mean[:, 0] + lo[:, None] * axis
    weights = np.array([0, 1, 1 / 3, 2 / 3], np.float32)  # palette i: e0*(1-w) + e1*w

    def quantize(color):
        color = np.clip(color, 0, 255)
        r = np.round(color[:, 0] * 31 / 255).astype(np.uint32)
        g = np.round(color[:, 1] * 63 / 255).astype(np.uint32)
        b = np.round(color[:, 2] * 31 / 255).astype(np.uint32)
        return (r << 11) | (g << 5) | b

    def fit(e0, e1):
        q0, q1 = quantize(e0), quantize(e1)
        swap = q0 < q1
        q0, q1 = np.where(swap, q1, q0), np.where(swap, q0, q1)
        p0, p1 = _565(q0), _565(q1)
        palette = p0[:, None] * (1 - weights[None, :, None]) + p1[:, None] * weights[None, :, None]
        distance = ((pixels[:, :, None] - palette[:, None]) ** 2).sum(-1)
        index = distance.argmin(2)
        # a block of one color (q0 == q1) would be three-color DXT1: all index 0 then
        index = np.where((q0 == q1)[:, None], 0, index)
        return q0, q1, index, np.take_along_axis(distance, index[:, :, None], 2).sum((1, 2))

    q0, q1, index, error = fit(e0, e1)
    # refine: least squares endpoints for the chosen weights
    w = weights[index]
    a, b = 1 - w, w
    aa, ab, bb = (a * a).sum(1), (a * b).sum(1), (b * b).sum(1)
    ax = np.einsum("bk,bki->bi", a, pixels)
    bx = np.einsum("bk,bki->bi", b, pixels)
    det = aa * bb - ab * ab
    ok = np.abs(det) > 1e-6
    det = np.where(ok, det, 1)
    r0 = (bb[:, None] * ax - ab[:, None] * bx) / det[:, None]
    r1 = (aa[:, None] * bx - ab[:, None] * ax) / det[:, None]
    r0, r1 = np.where(ok[:, None], r0, e0), np.where(ok[:, None], r1, e1)
    q0b, q1b, indexb, errorb = fit(r0, r1)
    better = errorb < error
    q0, q1, index = np.where(better, q0b, q0), np.where(better, q1b, q1), np.where(better[:, None], indexb, index)
    bits = np.zeros(len(pixels), np.uint32)
    for k in range(16):
        bits |= index[:, k].astype(np.uint32) << np.uint32(2 * k)
    out = np.empty((len(pixels), 4), np.uint16)
    out = np.stack([q0.astype(np.uint16), q1.astype(np.uint16), (bits & 0xFFFF).astype(np.uint16),
                    (bits >> 16).astype(np.uint16)], 1)
    return out.astype("<u2").tobytes()


def encode_dxt5(image: np.ndarray) -> bytes:
    """an RGBA image (height, width, 4) of 0-255, both sides multiples of 4, as DXT5 blocks: each block's alpha
    between its smallest and largest (eight steps), its color as DXT1's"""
    height, width = image.shape[:2]
    alpha = image[:, :, 3].astype(np.float32).reshape(height // 4, 4, width // 4, 4)
    alpha = np.clip(np.round(alpha.transpose(0, 2, 1, 3).reshape(-1, 16)), 0, 255)
    a0, a1 = alpha.max(1), alpha.min(1)
    steps = np.arange(8, dtype=np.float32)
    # a0 > a1: index 0 is a0, 1 is a1, 2-7 between; a0 == a1: every index 0
    palette = np.where(steps == 0, a0[:, None], np.where(steps == 1, a1[:, None],
                       ((8 - steps) * a0[:, None] + (steps - 1) * a1[:, None]) / 7))
    index = np.abs(alpha[:, :, None] - palette[:, None, :]).argmin(2).astype(np.uint64)
    index = np.where((a0 == a1)[:, None], 0, index).astype(np.uint64)
    bits = np.zeros(len(alpha), np.uint64)
    for k in range(16):
        bits |= index[:, k] << np.uint64(3 * k)
    alpha_blocks = np.zeros((len(alpha), 8), np.uint8)
    alpha_blocks[:, 0], alpha_blocks[:, 1] = a0.astype(np.uint8), a1.astype(np.uint8)
    for byte in range(6):
        alpha_blocks[:, 2 + byte] = ((bits >> np.uint64(8 * byte)) & np.uint64(255)).astype(np.uint8)
    color = np.frombuffer(encode_dxt1(image), np.uint8).reshape(-1, 8)
    return np.concatenate([alpha_blocks, color], 1).tobytes()


def mip_chain(image: np.ndarray):
    """the image and each mip below it to 1x1, box filtered"""
    mips = [image]
    while mips[-1].shape[0] > 1 or mips[-1].shape[1] > 1:
        current = mips[-1]
        h, w = current.shape[:2]
        if h > 1:
            current = (current[0::2] + current[1::2]) / 2
        if w > 1:
            current = (current[:, 0::2] + current[:, 1::2]) / 2
        mips.append(current)
    return mips


def pad4(image: np.ndarray) -> np.ndarray:
    h, w = image.shape[:2]
    return np.pad(image, ((0, max(0, 4 - h)), (0, max(0, 4 - w)), (0, 0)), mode="edge")


# ---------------------------------------------------------------- an Xbox map's tags

TAG_BASE = 0x803A6000
FORMAT_BITS = {0: 8, 1: 8, 2: 8, 3: 16, 6: 16, 8: 16, 9: 16, 10: 32, 11: 32, 14: 4, 15: 8, 16: 8}
DXT1, DXT3, DXT5, A8R8G8B8, X8R8G8B8 = 14, 15, 16, 11, 10


class XboxMap:
    """an Xbox map (compressed after its 0x800-byte header) with its tags as the game has them in memory"""

    def __init__(self, path: Path):
        data = Path(path).read_bytes()
        self.data = data[:0x800] + zlib.decompress(data[0x800:])
        offset, size = struct.unpack_from("<ii", self.data, 0x10)
        self.tags = self.data[offset:offset + size]
        instances, _, _, count = struct.unpack_from("<IiIi", self.tags, 0)
        self.instances = []
        for index in range(count):
            group, _, _, _, name, definition, _, _ = struct.unpack_from("<4sIIiIIII", self.tags,
                                                                         instances - TAG_BASE + index * 0x20)
            self.instances.append((group[::-1].decode("latin-1"), self.string(name), definition))

    def string(self, address: int) -> str:
        start = address - TAG_BASE
        return self.tags[start:self.tags.index(b"\0", start)].decode("latin-1")

    def at(self, address: int, fmt: str, offset: int = 0):
        return struct.unpack_from("<" + fmt, self.tags, address - TAG_BASE + offset)

    def block(self, address: int, offset: int, size: int):
        count, elements = self.at(address, "iI", offset)
        return [elements + size * index for index in range(count)]

    def find(self, group: str, name: str) -> int:
        name = name.replace("/", "\\").lower()
        for index, (found_group, found_name, _) in enumerate(self.instances):
            if found_group == group and found_name.lower() == name:
                return index
        raise LookupError(f"{group} {name}")

    def reference(self, address: int, offset: int):
        """the tag index a tag reference at `offset` names, or None"""
        index, = self.at(address, "i", offset + 0xC)
        return None if index == -1 else index & 0xFFFF

    def bitmap(self, tag: int):
        """the bitmap tag's root and its first bitmap's (width, height, format, flags, mipmaps, pixels)"""
        address = self.instances[tag][2]
        bitmaps = self.block(address, 0x60, 0x30)
        width, height, depth, kind, fmt, flags = self.at(bitmaps[0], "hhhhhH", 4)
        mipmaps, = self.at(bitmaps[0], "h", 0x14)
        offset, size = self.at(bitmaps[0], "ii", 0x18)
        return {"address": address, "count": len(bitmaps), "width": width, "height": height, "depth": depth,
                "type": kind, "format": fmt, "flags": flags, "mipmaps": mipmaps,
                "pixels": self.data[offset:offset + size]}

    def bitmap_rgba(self, tag: int) -> np.ndarray:
        """the bitmap tag's first bitmap's top mip as RGBA floats (DXT, or 32-bit swizzled)"""
        info = self.bitmap(tag)
        width, height, fmt = info["width"], info["height"], info["format"]
        if fmt in (DXT1, DXT3, DXT5):
            return decode_dxt(info["pixels"], width, height, 8 if fmt == DXT1 else 16, explicit_alpha=fmt == DXT3)
        if fmt in (A8R8G8B8, X8R8G8B8):
            raw = np.frombuffer(info["pixels"], np.uint8)[:width * height * 4].reshape(-1, 4)
            if info["flags"] & 8:  # swizzled
                raw = raw[swizzle_order(width, height)]
            bgra = raw.reshape(height, width, 4).astype(np.float32)
            rgba = bgra[:, :, [2, 1, 0, 3]]
            if fmt == X8R8G8B8:
                rgba[:, :, 3] = 255
            return rgba
        raise ValueError(f"bitmap format {fmt} is not read")

    def model_parts(self, address: int, base_only: bool = False):
        """a model's highest-detail parts: a list of (its shader_model's base map's tag index or None, texcoords
        (n, 2) as drawn (scaled by the model's base map scales), triangles (m, 3), vertex positions (n, 3), vertex
        normals (n, 3), its shader_model's multipurpose map's tag index or None). base_only: each region's first
        permutation only (the whole object, not its damaged or destroyed forms)"""
        u_scale, v_scale = self.at(address, "ff", 0x30)
        u_scale, v_scale = u_scale or 1.0, v_scale or 1.0
        bases, multipurposes = [], []
        for shader in self.block(address, 0xDC, 0x20):
            tag = self.reference(shader, 0)
            base = multipurpose = None
            if tag is not None and self.instances[tag][0] == "soso":
                base = self.reference(self.instances[tag][2], 0xA4)
                multipurpose = self.reference(self.instances[tag][2], 0xBC)
            bases.append(base)
            multipurposes.append(multipurpose)
        geometries = self.block(address, 0xD0, 0x30)
        # the geometries the highest detail of each permutation draws
        wanted = set()
        for region in self.block(address, 0xC4, 0x4C):
            permutations = self.block(region, 0x40, 0x58)
            for permutation in permutations[:1] if base_only else permutations:
                levels = [level for level in self.at(permutation, "5h", 0x40) if level != -1]
                if levels:
                    wanted.add(levels[-1])
        parts = []
        for geometry_index in sorted(wanted):
            for part in self.block(geometries[geometry_index], 0x24, 0x68):
                shader_index, = self.at(part, "h", 4)
                uv, triangles, positions, normals = self.part_geometry(part)
                if uv is None:
                    continue
                ok = 0 <= shader_index < len(bases)
                parts.append((bases[shader_index] if ok else None, uv * np.array([u_scale, v_scale], np.float32),
                              triangles, positions, normals, multipurposes[shader_index] if ok else None))
        return parts

    def models_using(self, bitmap_tag: int, with_positions: bool = False):
        """every model's highest-detail parts whose shader_model's base map is the bitmap: a list of (model name,
        texcoord arrays (n, 2) as drawn (scaled by the model's base map scales), triangle index arrays (m, 3)),
        and with_positions, each one's vertex positions (n, 3) after them"""
        found = []
        for group, name, address in self.instances:
            if group != "mode" or not address:
                continue
            parts = [part for part in self.model_parts(address) if part[0] == bitmap_tag]
            if not parts:
                continue
            bases = np.cumsum([0] + [len(part[1]) for part in parts])[:-1]
            model = (name, np.concatenate([part[1] for part in parts]),
                     np.concatenate([part[2] + base for part, base in zip(parts, bases)]))
            found.append(model + (np.concatenate([part[3] for part in parts]),) if with_positions else model)
        return found

    def part_geometry(self, part: int):
        """a part's texcoords, its triangles (from its strip, degenerate ones left out), its vertex positions and its
        vertex normals"""
        vertex_type, _, vertex_count = self.at(part, "hHi", 0x44 + 0x10)
        triangle_type, _, index_count = self.at(part, "hHi", 0x44)
        vertex_hardware, = self.at(part, "I", 0x44 + 0x10 + 0x10)
        index_hardware, = self.at(part, "I", 0x44 + 0xC)
        if not vertex_hardware or not index_hardware or vertex_count <= 0:
            return None, None, None, None
        vertices = self.at(vertex_hardware, "I", 4)[0] | 0x80000000
        indices = self.at(index_hardware, "I", 4)[0] | 0x80000000
        if vertex_type == 5:  # model compressed: 32 bytes, texcoord int16 x2 at 24
            raw = np.frombuffer(self.tags, np.uint8, vertex_count * 32, vertices - TAG_BASE).reshape(-1, 32)
            tc = raw[:, 24:28].copy().view("<i2").astype(np.float32)
            uv = (tc * 2 + 1) / 65535
            xyz = raw[:, 0:12].copy().view("<f4").astype(np.float32)
            # the normal packed 11:11:10, signed
            packed = raw[:, 12:16].copy().view("<u4")[:, 0].astype(np.int64)
            x, y, z = packed & 0x7FF, (packed >> 11) & 0x7FF, (packed >> 22) & 0x3FF
            x, y, z = np.where(x >= 0x400, x - 0x800, x), np.where(y >= 0x400, y - 0x800, y), np.where(z >= 0x200, z - 0x400, z)
            normals = np.stack([x / 1023.0, y / 1023.0, z / 511.0], 1).astype(np.float32)
        elif vertex_type == 4:  # model uncompressed: 68 bytes, texcoord float x2 at 48
            raw = np.frombuffer(self.tags, np.uint8, vertex_count * 68, vertices - TAG_BASE).reshape(-1, 68)
            uv = raw[:, 48:56].copy().view("<f4").astype(np.float32)
            xyz = raw[:, 0:12].copy().view("<f4").astype(np.float32)
            normals = raw[:, 12:24].copy().view("<f4").astype(np.float32)
        else:
            raise ValueError(f"vertex type {vertex_type} is not read")
        if triangle_type == 0:
            count = 3 * index_count
            idx = np.frombuffer(self.tags, "<u2", count, indices - TAG_BASE).astype(np.int64)
            tris = idx.reshape(-1, 3)
        else:
            count = index_count + 2
            idx = np.frombuffer(self.tags, "<u2", count, indices - TAG_BASE).astype(np.int64)
            tris = np.stack([idx[:-2], idx[1:-1], idx[2:]], 1)
        keep = ((tris[:, 0] != tris[:, 1]) & (tris[:, 1] != tris[:, 2]) & (tris[:, 0] != tris[:, 2]) &
                (tris.max(1) < vertex_count))
        normals = normals / np.maximum(np.linalg.norm(normals, axis=1, keepdims=True), 1e-6)
        return uv, tris[keep], xyz, normals


def swizzle_order(width: int, height: int) -> np.ndarray:
    """for each linear texel (row major), its index in the Xbox's swizzled (Morton) order"""
    xs, ys = np.meshgrid(np.arange(width), np.arange(height))
    index = np.zeros_like(xs)
    bit = 0
    x_bits, y_bits = width.bit_length() - 1, height.bit_length() - 1
    for level in range(max(x_bits, y_bits)):
        if level < x_bits:
            index |= ((xs >> level) & 1) << bit
            bit += 1
        if level < y_bits:
            index |= ((ys >> level) & 1) << bit
            bit += 1
    return index.reshape(-1)


# ---------------------------------------------------------------- UV islands

def rasterize(triangles: np.ndarray, labels: np.ndarray, width: int, height: int) -> np.ndarray:
    """a label map (height, width), -1 where no triangle is: each triangle (pixel coordinates (n, 3, 2), any
    offset: wrapped) marks the texels whose centers it covers, and the texels its corners and edges' midpoints are
    in (so a thin triangle still marks its texels)"""
    out = np.full((height, width), -1, np.int32)
    for tri, label in zip(triangles, labels):
        x0, y0 = np.floor(tri.min(0)).astype(int)
        x1, y1 = np.ceil(tri.max(0)).astype(int)
        xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        (ax, ay), (bx, by), (cx, cy) = tri
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if abs(area) > 1e-9:
            w0 = ((bx - xs) * (cy - ys) - (by - ys) * (cx - xs)) / area
            w1 = ((cx - xs) * (ay - ys) - (cy - ys) * (ax - xs)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= -1e-4) & (w1 >= -1e-4) & (w2 >= -1e-4)
            iy, ix = np.nonzero(inside)
            out[(iy + y0) % height, (ix + x0) % width] = label
        points = np.concatenate([tri, (tri + np.roll(tri, 1, 0)) / 2])
        px, py = np.floor(points).astype(int).T
        out[py % height, px % width] = label
    return out


def islands_find(uv: np.ndarray, triangles: np.ndarray, width: int, height: int):
    """the UV islands of the triangles: groups of triangles joined by shared texcoords, and groups whose texels
    overlap (a mirrored part drawn on the same texels) joined too. Each island: its triangles' pixel coordinates
    (moved by whole textures to near the first group's), its bounds and its texel count, the largest first."""
    points = (uv[triangles] * np.array([width, height], np.float32)).astype(np.float64)  # (n, 3, 2)
    parent = list(range(len(triangles)))

    def root(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def join(a, b):
        a, b = root(a), root(b)
        if a != b:
            parent[max(a, b)] = min(a, b)

    key = {}
    for index, tri in enumerate(points):
        for corner in tri:
            k = (round(float(corner[0]) * 16), round(float(corner[1]) * 16))
            if k in key:
                join(index, key[k])
            else:
                key[k] = index
    groups = {}
    for index in range(len(triangles)):
        groups.setdefault(root(index), []).append(index)
    moved = []
    for group in groups.values():
        tris = points[group]
        shift = np.floor(tris.reshape(-1, 2).mean(0) / [width, height]) * [width, height]
        moved.append(tris - shift)
    masks = [rasterize(t, np.zeros(len(t), np.int32), width, height) >= 0 for t in moved]
    sizes = [int(m.sum()) for m in masks]
    owner = list(range(len(moved)))

    def owner_root(i):
        while owner[i] != i:
            i = owner[i]
        return i

    for a in range(len(moved)):
        for b in range(a + 1, len(moved)):
            overlap = int(np.logical_and(masks[a], masks[b]).sum())
            if overlap and overlap >= 0.2 * min(sizes[a], sizes[b]):
                ra, rb = owner_root(a), owner_root(b)
                if ra != rb:
                    owner[max(ra, rb)] = min(ra, rb)
    merged = {}
    for index in range(len(moved)):
        merged.setdefault(owner_root(index), []).append(index)
    islands = []
    for members in merged.values():
        anchor = moved[members[0]].reshape(-1, 2).mean(0)
        parts = []
        for m in members:
            t = moved[m]
            shift = np.round((t.reshape(-1, 2).mean(0) - anchor) / [width, height]) * [width, height]
            parts.append(t - shift)
        tris = np.concatenate(parts)
        mask = np.zeros((height, width), bool)
        for m in members:
            mask |= masks[m]
        lo, hi = tris.reshape(-1, 2).min(0), tris.reshape(-1, 2).max(0)
        islands.append({"triangles": tris, "bounds": [float(lo[0]), float(lo[1]), float(hi[0]), float(hi[1])],
                        "texels": int(mask.sum())})
    islands.sort(key=lambda island: (-island["texels"], island["bounds"]))
    return islands


def island_mask(island):
    """the island's texels about its bounds (whole texels, unwrapped): (x0, y0, mask)"""
    tris = island["triangles"]
    x0, y0 = np.floor(tris.reshape(-1, 2).min(0)).astype(int) - 1
    x1, y1 = np.ceil(tris.reshape(-1, 2).max(0)).astype(int) + 1
    w, h = x1 - x0 + 1, y1 - y0 + 1
    local = np.full((h, w), False)
    # (rasterized unwrapped: a local frame bigger than the island never wraps it)
    big = rasterize(tris - [x0, y0], np.zeros(len(tris), np.int32), w + 4, h + 4) >= 0
    local[:, :] = big[:h, :w]
    return int(x0), int(y0), local


def classic_islands(game_map: "XboxMap", tag: str):
    bitmap_tag = game_map.find("bitm", tag)
    info = game_map.bitmap(bitmap_tag)
    width, height = info["width"], info["height"]
    models = game_map.models_using(bitmap_tag)
    if not models:
        raise LookupError(f"{tag}: no model draws it")
    uv = np.concatenate([m[1] for m in models])
    bases = np.cumsum([0] + [len(m[1]) for m in models])[:-1]
    triangles = np.concatenate([m[2] + base for m, base in zip(models, bases)])
    return bitmap_tag, info, [m[0] for m in models], islands_find(uv, triangles, width, height)


# ---------------------------------------------------------------- an island found in the MCC texture

ORIENTATIONS = [np.array(m, np.float64) for m in (
    ((1, 0), (0, 1)), ((0, -1), (1, 0)), ((-1, 0), (0, -1)), ((0, 1), (-1, 0)),     # turned 0, 90, 180, 270
    ((-1, 0), (0, 1)), ((0, 1), (1, 0)), ((1, 0), (0, -1)), ((0, -1), (-1, 0)))]    # and mirrored
SCALES = np.exp(np.linspace(np.log(0.55), np.log(1.8), 12))
HIGH_PASS = 5  # classic texels: the detail matched is what a box filter this wide leaves out


def luminance(rgba: np.ndarray) -> np.ndarray:
    return rgba[:, :, 0] * 0.299 + rgba[:, :, 1] * 0.587 + rgba[:, :, 2] * 0.114


def box_down(image: np.ndarray, factor: int) -> np.ndarray:
    h, w = image.shape[:2]
    shape = (h // factor, factor, w // factor, factor) + image.shape[2:]
    return image[:h - h % factor, :w - w % factor].reshape(shape).mean((1, 3))


def box_blur(image: np.ndarray, size: int) -> np.ndarray:
    """a box filter `size` texels wide, so that a texel's value is the mean of the classic texel it is in the middle
    of (clamped at the edges)"""
    out = image
    for axis in (0, 1):
        padded = np.pad(out, [(size, size) if a == axis else (0, 0) for a in range(out.ndim)], mode="edge")
        csum = np.cumsum(padded, axis, dtype=np.float64)
        csum = np.concatenate([np.zeros_like(np.take(csum, [0], axis)), csum], axis)
        n = out.shape[axis]
        lo = np.arange(n) + size - size // 2
        out = (np.take(csum, lo + size, axis) - np.take(csum, lo, axis)) / size
    return out


def bilinear(image: np.ndarray, x: np.ndarray, y: np.ndarray, wrap: bool = False) -> np.ndarray:
    """the image at continuous pixel coordinates (texel i's center at i + 0.5)"""
    h, w = image.shape[:2]
    x, y = x - 0.5, y - 0.5
    x0, y0 = np.floor(x).astype(np.int64), np.floor(y).astype(np.int64)
    fx, fy = x - x0, y - y0
    if wrap:
        xa, xb, ya, yb = x0 % w, (x0 + 1) % w, y0 % h, (y0 + 1) % h
    else:
        xa, xb = np.clip(x0, 0, w - 1), np.clip(x0 + 1, 0, w - 1)
        ya, yb = np.clip(y0, 0, h - 1), np.clip(y0 + 1, 0, h - 1)
    if image.ndim == 3:
        fx, fy = fx[..., None], fy[..., None]
    return ((image[ya, xa] * (1 - fx) + image[ya, xb] * fx) * (1 - fy) +
            (image[yb, xa] * (1 - fx) + image[yb, xb] * fx) * fy)


def masked_ncc(image_fft, image2_fft, shape, fft_shape, template, mask):
    """the masked normalized cross-correlation of the template at every place it fits wholly in the image"""
    h, w = shape
    th, tw = template.shape
    n = mask.sum()
    if n < 4:
        return None
    t = template * mask
    st, st2 = t.sum(), (t * template).sum()
    tvar = st2 - st * st / n
    if tvar <= 1.0 * n:  # (detail of a level of 255 or less: nothing to match)
        return None
    kf = np.conj(np.fft.rfft2(t, fft_shape))
    mf = np.conj(np.fft.rfft2(mask.astype(np.float64), fft_shape))
    cross = np.fft.irfft2(image_fft * kf, fft_shape)[:h - th + 1, :w - tw + 1]
    si = np.fft.irfft2(image_fft * mf, fft_shape)[:h - th + 1, :w - tw + 1]
    si2 = np.fft.irfft2(image2_fft * mf, fft_shape)[:h - th + 1, :w - tw + 1]
    ivar = np.maximum(si2 - si * si / n, 0)
    score = (cross - st * si / n) / np.sqrt(np.maximum(ivar * tvar, 1e-9))
    score[ivar < 0.25 * n] = -1
    return score


class Matcher:
    """an island of the classic texture found in the MCC one: first everywhere at a low mip, over scales and the 8
    orientations, then refined (offset and a scale on each axis) at full size"""

    def __init__(self, classic: np.ndarray, mcc: np.ndarray):
        # (both matched by their detail, a high pass HIGH_PASS texels wide at each level: the shading a redraw
        # changes, and the broad gradients any place has, left out)
        self.classic = classic                         # classic luminance (h, w)
        self.classic_detail = classic - box_blur(classic, HIGH_PASS)
        self.ratio = mcc.shape[1] / classic.shape[1]   # MCC texels per classic texel, nominally
        self.mcc = mcc
        ratio = int(round(self.ratio))
        base = box_down(mcc, ratio)                    # level k: 2^k classic texels per texel
        self.levels = [level - box_blur(level, HIGH_PASS) for level in (base, box_down(base, 2), box_down(base, 4))]
        self.ffts = {}
        self.mcc_detail = box_blur(mcc, ratio) - box_blur(mcc, HIGH_PASS * ratio)

    def level_fft(self, level, fft_shape):
        key = (level, fft_shape)
        if key not in self.ffts:
            image = self.levels[level]
            self.ffts[key] = (np.fft.rfft2(image, fft_shape), np.fft.rfft2(image * image, fft_shape))
        return self.ffts[key]

    def template(self, x0, y0, mask, level, scale, orientation):
        """the island's texels as they would be in the MCC texture at `level`, turned and scaled: (template,
        mask, where the island's local origin is in the template)"""
        h, w = mask.shape
        factor = scale / 2 ** level
        corners = np.array([[0, 0], [w, 0], [0, h], [w, h]], np.float64)
        mapped = corners @ orientation.T * factor
        lo = mapped.min(0)
        size = np.maximum(np.ceil(mapped.max(0) - lo).astype(int), 1)
        ty, tx = np.mgrid[0:size[1], 0:size[0]] + 0.5
        inverse = orientation.T / factor
        local = np.stack([tx + lo[0], ty + lo[1]], -1) @ inverse.T
        values = bilinear(self.classic, local[..., 0] + x0, local[..., 1] + y0, wrap=True)
        values = values - box_blur(values, HIGH_PASS)
        mask_values = bilinear(mask.astype(np.float64), local[..., 0], local[..., 1])
        inside = (local[..., 0] >= 0) & (local[..., 0] < w) & (local[..., 1] >= 0) & (local[..., 1] < h)
        return values, (mask_values > 0.5) & inside, -lo

    def coarse(self, x0, y0, mask):
        """the best place at each scale and orientation, the best first: (score, scale, orientation index, where
        the island's local origin is in the MCC texture at full size)"""
        h, w = mask.shape
        size = max(h, w)
        level = 0 if size < 48 else 1 if size < 128 else 2
        image = self.levels[level]
        results = []
        for oi, orientation in enumerate(ORIENTATIONS):
            for scale in SCALES:
                template, tmask, origin_in_template = self.template(x0, y0, mask, level, scale, orientation)
                th, tw = template.shape
                if th > image.shape[0] or tw > image.shape[1]:
                    continue
                fft_shape = tuple(int(2 ** np.ceil(np.log2(a + b))) for a, b in zip(image.shape, template.shape))
                ifft, ifft2 = self.level_fft(level, fft_shape)
                score = masked_ncc(ifft, ifft2, image.shape, fft_shape, template, tmask)
                if score is None:
                    return [], level
                at = np.unravel_index(np.argmax(score), score.shape)
                origin = (np.array([at[1], at[0]], np.float64) + origin_in_template) * 2 ** level * self.ratio
                results.append((float(score[at]), float(scale), oi, origin))
        results.sort(key=lambda r: -r[0])
        return results, level

    def refined_score(self, params, oi, points, values):
        """the NCC of the classic texels (local points, values) with the MCC texture at params (tx, ty, log sx,
        log sy)"""
        tx, ty, lsx, lsy = params
        matrix = ORIENTATIONS[oi] @ np.diag([np.exp(lsx), np.exp(lsy)]) * self.ratio
        mapped = points @ matrix.T + [tx, ty]
        h, w = self.mcc.shape
        if mapped[:, 0].min() < 0 or mapped[:, 1].min() < 0 or mapped[:, 0].max() > w or mapped[:, 1].max() > h:
            return -1.0
        sampled = bilinear(self.mcc_detail, mapped[:, 0], mapped[:, 1])
        a, b = values - values.mean(), sampled - sampled.mean()
        den = np.sqrt((a * a).sum() * (b * b).sum())
        return float((a * b).sum() / den) if den > 1e-9 else -1.0

    def refine(self, x0, y0, mask, scale, oi, origin):
        iy, ix = np.nonzero(mask)
        points = np.stack([ix + 0.5, iy + 0.5], 1).astype(np.float64)
        values = bilinear(self.classic_detail, points[:, 0] + x0, points[:, 1] + y0, wrap=True)
        start = np.array([origin[0], origin[1], np.log(scale), np.log(scale)])
        steps = np.array([1.5 * self.ratio, 1.5 * self.ratio, 0.04, 0.04])

        def cost(p):
            return -self.refined_score(p, oi, points, values)

        best = nelder_mead(cost, start, steps, 150)
        best = nelder_mead(cost, best, steps / 4, 100)
        return -cost(best), best


def nelder_mead(function, start, steps, iterations):
    simplex = [np.array(start, np.float64)]
    for k in range(len(start)):
        point = np.array(start, np.float64)
        point[k] += steps[k]
        simplex.append(point)
    values = [function(p) for p in simplex]
    for _ in range(iterations):
        order = np.argsort(values)
        simplex = [simplex[i] for i in order]
        values = [values[i] for i in order]
        centroid = np.mean(simplex[:-1], 0)
        reflected = centroid + (centroid - simplex[-1])
        fr = function(reflected)
        if fr < values[0]:
            expanded = centroid + 2 * (centroid - simplex[-1])
            fe = function(expanded)
            simplex[-1], values[-1] = (expanded, fe) if fe < fr else (reflected, fr)
        elif fr < values[-2]:
            simplex[-1], values[-1] = reflected, fr
        else:
            contracted = centroid + 0.5 * (simplex[-1] - centroid)
            fc = function(contracted)
            if fc < values[-1]:
                simplex[-1], values[-1] = contracted, fc
            else:
                for i in range(1, len(simplex)):
                    simplex[i] = simplex[0] + 0.5 * (simplex[i] - simplex[0])
                    values[i] = function(simplex[i])
    return simplex[int(np.argmin(values))]


# the least refined score an island is placed at, and how far its coarse score must be ahead of the best place
# elsewhere (unless the refined score is high on its own)
ACCEPT_SCORE = 0.5
ACCEPT_MARGIN = 0.05
ACCEPT_ALONE = 0.85

_MATCHER = None


def _match_island(job):
    """(island index, x0, y0, mask) -> (index, the island's placement)"""
    index, x0, y0, mask = job
    matcher = _MATCHER
    results, level = matcher.coarse(x0, y0, mask)
    if not results:
        return index, {"placed": False, "why": "flat: no detail to match"}
    best_score, _, _, best_origin = results[0]
    away = max(8.0, 0.5 * max(mask.shape)) * matcher.ratio
    runner = max((r[0] for r in results[1:] if np.linalg.norm(r[3] - best_origin) > away), default=-1.0)
    # the three best places, and the island left where it is (as many are), which wins a near tie
    starts = [(scale, oi, origin) for _, scale, oi, origin in results[:3]]
    starts.append((1.0, 0, np.array([x0, y0], np.float64) * matcher.ratio))
    candidates = []
    for k, (scale, oi, origin) in enumerate(starts):
        refined, params = matcher.refine(x0, y0, mask, scale, oi, origin)
        candidates.append((refined + (0.03 if k == len(starts) - 1 else 0), refined, oi, params))
    candidates.sort(key=lambda c: -c[0])
    candidates = [c[1:] for c in candidates]
    refined, oi, params = candidates[0]
    tx, ty, lsx, lsy = params
    matrix = ORIENTATIONS[oi] @ np.diag([np.exp(lsx), np.exp(lsy)]) * matcher.ratio
    # MCC = matrix @ (p - (x0, y0)) + t, for classic texel coordinates p (the island's, unwrapped)
    offset = np.array([tx, ty]) - matrix @ np.array([x0, y0], np.float64)
    # (an island found where it is in the classic texture, at its size, needs no lead over elsewhere)
    in_place = (oi == 0 and abs(lsx) < 0.03 and abs(lsy) < 0.03 and
                np.abs(np.array([tx, ty]) - np.array([x0, y0]) * matcher.ratio).max() < 2 * matcher.ratio)
    placed = refined >= ACCEPT_SCORE and (best_score - runner >= ACCEPT_MARGIN or refined >= ACCEPT_ALONE or
                                          in_place)
    return index, {"placed": bool(placed), "score": round(float(refined), 4), "coarse": round(best_score, 4),
                   "runner_up": round(float(runner), 4), "orientation": oi, "in_place": bool(in_place),
                   "matrix": [round(float(v), 6) for v in matrix.reshape(-1)],
                   "offset": [round(float(v), 4) for v in offset],
                   **({} if placed else {"why": "no clear match"})}


def make_recipe(game_map: "XboxMap", tag: str, mcc_rgba: np.ndarray, texture: str, jobs: int = 0):
    import multiprocessing
    global _MATCHER
    bitmap_tag, info, models, islands = classic_islands(game_map, tag)
    classic = luminance(game_map.bitmap_rgba(bitmap_tag)).astype(np.float64)
    _MATCHER = Matcher(classic, luminance(mcc_rgba).astype(np.float64))
    work = [(index,) + island_mask(island) for index, island in enumerate(islands)]
    with multiprocessing.get_context("fork").Pool(jobs or None) as pool:
        placements = dict(pool.map(_match_island, work, chunksize=1))
    _MATCHER = None
    return {"format": 1, "tag": tag, "texture": texture, "models": models,
            "classic_size": [info["width"], info["height"]],
            "mcc_size": [int(mcc_rgba.shape[1]), int(mcc_rgba.shape[0])],
            "islands": [{"island": index, "bounds": [round(v, 3) for v in island["bounds"]],
                         "texels": island["texels"], **placements[index]} for index, island in enumerate(islands)]}


# ---------------------------------------------------------------- a recipe applied: the bitmap in the classic layout

DILATE = 8  # texels at the output size each island's mapping reaches past its edges (seams, mips)


def apply_recipe(game_map: "XboxMap", recipe: dict, mcc_rgba: np.ndarray):
    """the classic texture at the high resolution: each placed island's texels taken from the MCC texture by its
    transform, reaching DILATE texels past its edges; the rest the classic texture, enlarged"""
    bitmap_tag, info, _, islands = classic_islands(game_map, recipe["tag"])
    width, height = info["width"], info["height"]
    if [width, height] != recipe["classic_size"] or len(islands) != len(recipe["islands"]):
        raise ValueError(f"{recipe['tag']}: the map's islands are not the recipe's")
    for island, entry in zip(islands, recipe["islands"]):
        if np.abs(np.array(island["bounds"]) - entry["bounds"]).max() > 0.01:
            raise ValueError(f"{recipe['tag']}: island {entry['island']} is not where the recipe has it")
    factor = int(round(mcc_rgba.shape[1] / width))
    factor = 1 << max(0, factor.bit_length() - 1)
    out_w, out_h = width * factor, height * factor
    tris = np.concatenate([island["triangles"] * factor for island in islands])
    labels = np.concatenate([np.full(len(island["triangles"]), index, np.int32)
                             for index, island in enumerate(islands)])
    label = rasterize(tris, labels, out_w, out_h)
    for _ in range(DILATE):
        empty = label < 0
        if not empty.any():
            break
        grown = label.copy()
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            shifted = np.roll(label, (dy, dx), (0, 1))
            take = empty & (grown < 0) & (shifted >= 0)
            grown[take] = shifted[take]
        label = grown
    classic = game_map.bitmap_rgba(bitmap_tag).astype(np.float64)
    ys, xs = np.mgrid[0:out_h, 0:out_w]
    px, py = (xs + 0.5) / factor, (ys + 0.5) / factor
    out = bilinear(classic, px, py, wrap=True)
    mcc = mcc_rgba.astype(np.float64)
    for index, (island, entry) in enumerate(zip(islands, recipe["islands"])):
        if not entry["placed"]:
            continue
        sel = label == index
        if not sel.any():
            continue
        x, y = px[sel], py[sel]
        cx, cy = (entry["bounds"][0] + entry["bounds"][2]) / 2, (entry["bounds"][1] + entry["bounds"][3]) / 2
        x = x + np.round((cx - x) / width) * width
        y = y + np.round((cy - y) / height) * height
        matrix = np.array(entry["matrix"]).reshape(2, 2)
        mx = matrix[0, 0] * x + matrix[0, 1] * y + entry["offset"][0]
        my = matrix[1, 0] * x + matrix[1, 1] * y + entry["offset"][1]
        out[sel] = bilinear(mcc, mx, my)
    out[:, :, 3] = bilinear(classic[:, :, 3], px, py, wrap=True)  # the classic alpha, enlarged
    return info, out


def bitmap_tag_file(game_map: "XboxMap", info: dict, image: np.ndarray, fmt: int = DXT1) -> bytes:
    """a Halo PC bitmap tag file (big-endian, version 7) of the map's bitmap with one DXT1 (or DXT5) bitmap of
    `image` and all its mips to 1x1, its sequences and sprites the map's"""
    address = info["address"]
    height, width = image.shape[:2]
    mips = mip_chain(image)
    encode = encode_dxt5 if fmt == DXT5 else encode_dxt1
    pixels = b"".join(encode(pad4(mip)) for mip in mips)
    root = bytearray(0x6C)
    for offset in (0x00, 0x04, 0x06, 0x14, 0x16, 0x18, 0x1A, 0x4C, 0x4E, 0x50):
        struct.pack_into(">h", root, offset, *game_map.at(address, "h", offset))
    for offset in (0x08, 0x0C, 0x10, 0x44, 0x48):
        struct.pack_into(">f", root, offset, *game_map.at(address, "f", offset))
    # the group's format: 0 compressed with color-key transparency (DXT1), 2 with interpolated alpha (DXT5)
    struct.pack_into(">h", root, 0x02, 2 if fmt == DXT5 else 0)
    struct.pack_into(">I", root, 0x30, len(pixels))
    sequences = game_map.block(address, 0x54, 0x40)
    struct.pack_into(">I", root, 0x54, len(sequences))
    struct.pack_into(">I", root, 0x60, 1)
    tail = b""
    sprite_blocks = []
    for sequence in sequences:
        element = bytearray(0x40)
        element[:32] = game_map.tags[sequence - TAG_BASE:][:32]
        struct.pack_into(">hh", element, 0x20, *game_map.at(sequence, "hh", 0x20))
        sprites = game_map.block(sequence, 0x34, 0x20)
        struct.pack_into(">I", element, 0x34, len(sprites))
        tail += bytes(element)
        sprite_blocks.append(sprites)
    for sprites in sprite_blocks:
        for sprite in sprites:
            element = bytearray(0x20)
            struct.pack_into(">h", element, 0, *game_map.at(sprite, "h", 0))
            struct.pack_into(">6f", element, 8, *game_map.at(sprite, "6f", 8))
            tail += bytes(element)
    bitmap = bytearray(0x30)
    bitmap[0:4] = b"bitm"
    struct.pack_into(">hhhhhH", bitmap, 4, width, height, 1, 0, fmt, 3)  # 2D, power of two, compressed
    struct.pack_into(">h", bitmap, 0x14, len(mips) - 1)
    struct.pack_into(">ii", bitmap, 0x18, 0, len(pixels))
    header = bytearray(0x40)
    header[0x24:0x28] = b"bitm"
    struct.pack_into(">I", header, 0x2C, 0x40)
    struct.pack_into(">H", header, 0x38, 7)
    header[0x3C:0x40] = b"blam"
    return bytes(header) + bytes(root) + pixels + tail + bytes(bitmap)


# ---------------------------------------------------------------- the command line

def mcc_texture(pak: Pak, name: str) -> np.ndarray:
    """an MCC texture's top mip as RGBA floats of 0-255 (BC5's two channels in red and green, blue 0)"""
    format_name, width, height, data = read_texture(pak, name)
    if format_name == "PF_BC5":
        # each block two alpha-style blocks, red then green: decoded as DXT5's alpha, one at a time
        blocks = np.frombuffer(data, np.uint8).reshape(-1, 2, 8)
        out = np.zeros((height, width, 4), np.float32)
        for channel in range(2):
            fake = np.zeros((len(blocks), 16), np.uint8)
            fake[:, :8] = blocks[:, channel]
            out[:, :, channel] = decode_dxt(fake.tobytes(), width, height, 16)[:, :, 3]
        out[:, :, 3] = 255
        return out
    return decode_dxt(data, width, height, UE_FORMATS[format_name])


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    listing = commands.add_parser("list", help="the pak's weapon and vehicle textures")
    listing.add_argument("--pak", required=True, type=Path)
    listing.add_argument("--filter", default="")
    making = commands.add_parser("recipe", help="a recipe for one bitmap tag")
    making.add_argument("--map", required=True, type=Path)
    making.add_argument("--bitmap", required=True, help="the bitmap tag's name")
    making.add_argument("--pak", required=True, type=Path)
    making.add_argument("--texture", required=True, help="the MCC texture's name (H1S2_VS_WARTHOG_DEFAULT)")
    making.add_argument("--out", required=True, type=Path)
    making.add_argument("--jobs", type=int, default=0)
    applying = commands.add_parser("apply", help="recipes applied: a bitmap mod")
    applying.add_argument("--map", required=True, type=Path)
    applying.add_argument("--pak", required=True, type=Path)
    applying.add_argument("--mod", required=True, type=Path, help="the mod's folder (written)")
    applying.add_argument("recipes", nargs="+", type=Path)
    args = parser.parse_args(argv)

    pak = Pak(args.pak)
    if args.command == "list":
        for name in sorted(pak.entries):
            if name.startswith(TEXTURE_ROOT) and name.endswith(".uasset") and args.filter in name:
                print(name[len(TEXTURE_ROOT):-len(".uasset")])
        return 0
    game_map = XboxMap(args.map)
    if args.command == "recipe":
        start = time.time()
        recipe = make_recipe(game_map, args.bitmap, mcc_texture(pak, args.texture), args.texture, args.jobs)
        args.out.write_text(json.dumps(recipe, indent=1) + "\n")
        placed = sum(island["placed"] for island in recipe["islands"])
        texels = sum(island["texels"] for island in recipe["islands"])
        placed_texels = sum(island["texels"] for island in recipe["islands"] if island["placed"])
        print(f"{args.bitmap}: {placed} of {len(recipe['islands'])} islands placed "
              f"({100 * placed_texels / max(texels, 1):.0f}% of their texels), {time.time() - start:.1f} s")
        return 0
    start = time.time()
    tags = args.mod / "tags"
    for recipe_path in args.recipes:
        recipe = json.loads(recipe_path.read_text())
        info, image = apply_recipe(game_map, recipe, mcc_texture(pak, recipe["texture"]))
        target = tags / (recipe["tag"].replace("\\", "/") + ".bitmap")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(bitmap_tag_file(game_map, info, np.clip(image, 0, 255)))
        print(f"{recipe['tag']}: {image.shape[1]}x{image.shape[0]} -> {target}")
    (args.mod / "mod.toml").write_text('name = "MCC HD textures (test)"\nid = "mcc_hd_test"\nversion = "0.1"\n'
                                      'description = "Weapon and vehicle textures from your own MCC install"\n')
    print(f"applied in {time.time() - start:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
