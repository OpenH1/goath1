"""audio.hrtf_file (sofa_fit.c): a SOFA file of measured responses read (port/linux/src/hrtf_sofa.c) and fitted into
the HRTF's table (dsound_sdl.c's hrtf_fit), and the Xbox's table, as it is, with none or one that cannot be read.

The SOFA file is made here, small and of known figures (no measured dataset is committed): HDF5 as netCDF-4 writes a
SOFA file (a version 2 superblock and object headers, their checksums, the root group's links in its header, a
string attribute, Data.IR chunked through shuffle and deflate, its last chunk partly past the end), 44.1 kHz, a
response for each of the table's directions (SOFA's azimuth counterclockwise: those to the right are negative) and
the poles. Each response's spectrum is flat within each of the table's bands (zero phase, the response's peak its
first sample), at levels known for each ear and band, the time between the ears in Data.Delay; at 90 degrees round,
each ear flat at one level, the responses delayed impulses (the delay from their onsets, Data.Delay 0).
"""

import math
import os
import struct
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, HARNESS, ROOT, function, mutated, read  # noqa: E402
from harness.tests.test_mixer import generated  # noqa: E402

ZLIB = ROOT / "port/third_party/zlib"

RATE = 44100.0
SAMPLES = 512
CROSSOVERS = (500, 1000, 2000, 3000, 4500, 6700, 10000, 14000)  # dsound_sdl.c's hrtf_crossovers
BANDS = len(CROSSOVERS) + 1
ELEVATIONS = (-60, -40, -20, 0, 20, 40, 60)
AZIMUTHS = 13
OUTPUT_RATE = 48000.0
CHUNK = 16
UNDEFINED = 0xFFFFFFFFFFFFFFFF

# a fault in the fit, and what must catch it: SOFA's azimuth taken as to the right (a sound to the right heard from
# the left's measurements, its ears swapped)
FAULT = ("input.vectors[m][0] = -sin(azimuth) * cos(elevation);", "input.vectors[m][0] = sin(azimuth) * cos(elevation);")


# ---------- HDF5, as little as a SOFA file needs

def lookup3(data):
    """HDF5's checksum (Bob Jenkins' lookup3 hashlittle, initial value 0)"""
    mask = 0xFFFFFFFF

    def rot(x, k):
        return ((x << k) | (x >> (32 - k))) & mask

    a = b = c = (0xDEADBEEF + len(data)) & mask
    words = lambda block: struct.unpack("<3I", block)  # noqa: E731
    rest = data
    while len(rest) > 12:
        x, y, z = words(rest[:12])
        a, b, c = (a + x) & mask, (b + y) & mask, (c + z) & mask
        a = (a - c) & mask; a ^= rot(c, 4); c = (c + b) & mask  # noqa: E702
        b = (b - a) & mask; b ^= rot(a, 6); a = (a + c) & mask  # noqa: E702
        c = (c - b) & mask; c ^= rot(b, 8); b = (b + a) & mask  # noqa: E702
        a = (a - c) & mask; a ^= rot(c, 16); c = (c + b) & mask  # noqa: E702
        b = (b - a) & mask; b ^= rot(a, 19); a = (a + c) & mask  # noqa: E702
        c = (c - b) & mask; c ^= rot(b, 4); b = (b + a) & mask  # noqa: E702
        rest = rest[12:]
    if not rest:
        return c
    x, y, z = words(rest + bytes(12 - len(rest)))
    a, b, c = (a + x) & mask, (b + y) & mask, (c + z) & mask
    c ^= b; c = (c - rot(b, 14)) & mask  # noqa: E702
    a ^= c; a = (a - rot(c, 11)) & mask  # noqa: E702
    b ^= a; b = (b - rot(a, 25)) & mask  # noqa: E702
    c ^= b; c = (c - rot(b, 16)) & mask  # noqa: E702
    a ^= c; a = (a - rot(c, 4)) & mask  # noqa: E702
    b ^= a; b = (b - rot(a, 14)) & mask  # noqa: E702
    c ^= b; c = (c - rot(b, 24)) & mask  # noqa: E702
    return c


class HDF5:
    def __init__(self):
        self.data = bytearray(48)  # (the superblock's, written last)

    def put(self, blob, align=8):
        self.data += bytes(-len(self.data) % align)
        address = len(self.data)
        self.data += blob
        return address

    def header(self, messages):
        body = b"".join(struct.pack("<BHB", kind, len(m), 0) + m for kind, m in messages)
        head = b"OHDR" + bytes([2, 0x02]) + struct.pack("<I", len(body)) + body
        return self.put(head + struct.pack("<I", lookup3(head)))

    def finish(self, root):
        superblock = b"\x89HDF\r\n\x1a\n" + bytes([2, 8, 8, 0]) + struct.pack("<4Q", 0, UNDEFINED, len(self.data), root)
        self.data[:48] = superblock + struct.pack("<I", lookup3(superblock))
        return bytes(self.data)


def dataspace(dims):
    return bytes([2, len(dims), 0, 1]) + b"".join(struct.pack("<Q", d) for d in dims)


DOUBLE = bytes([0x11, 0x20, 63, 0]) + struct.pack("<IHHBBBBI", 8, 0, 64, 52, 11, 0, 52, 1023)
SCALAR = bytes([2, 0, 0, 0])


def string_attribute(name, text):
    name = name.encode() + b"\0"
    text = text.encode()
    kind = bytes([0x13, 0, 0, 0]) + struct.pack("<I", len(text))
    return (0x0C, bytes([3, 0]) + struct.pack("<HHH", len(name), len(kind), len(SCALAR)) + b"\0" + name + kind + SCALAR +
            text)


def contiguous(file, dims, values, attributes=()):
    raw = struct.pack(f"<{len(values)}d", *values)
    address = file.put(raw)
    return file.header([(0x01, dataspace(dims)), (0x03, DOUBLE), (0x05, bytes([3, 0x09])),
                        (0x08, bytes([3, 1]) + struct.pack("<QQ", address, len(raw))), *attributes])


def chunked(file, dims, values):
    """dims[0] in chunks of CHUNK, through shuffle and deflate"""
    import zlib

    row = len(values) // dims[0]
    keys = []
    for start in range(0, dims[0], CHUNK):
        chunk = values[start * row:(start + CHUNK) * row]
        chunk = chunk + [0.0] * (CHUNK * row - len(chunk))
        raw = struct.pack(f"<{len(chunk)}d", *chunk)
        shuffled = bytes(raw[e * 8 + b] for b in range(8) for e in range(len(chunk)))
        packed = zlib.compress(shuffled, 6)
        keys.append((len(packed), start, file.put(packed)))
    node = b"TREE" + bytes([1, 0]) + struct.pack("<HQQ", len(keys), UNDEFINED, UNDEFINED)
    for size, start, address in keys:
        node += struct.pack("<II4QQ", size, 0, start, 0, 0, 0, address)
    node += struct.pack("<II4Q", 0, 0, len(keys) * CHUNK, 0, 0, 0)
    node += bytes(3136 - len(node))  # (a node's whole size, K 32)
    tree = file.put(node)
    chunk_dims = struct.pack("<4I", CHUNK, dims[1], dims[2], 8)
    pipeline = bytes([2, 2]) + struct.pack("<HHHI", 2, 1, 1, 8) + struct.pack("<HHHI", 1, 0, 1, 6)
    return file.header([(0x01, dataspace(dims)), (0x03, DOUBLE), (0x05, bytes([3, 0x0B])), (0x0B, pipeline),
                        (0x08, bytes([3, 2, 4]) + struct.pack("<Q", tree) + chunk_dims)])


def link(name, address):
    name = name.encode()
    return (0x06, bytes([1, 0, len(name)]) + name + struct.pack("<Q", address))


# ---------- the responses

def band_of(bin_):
    hertz = bin_ * RATE / SAMPLES
    return sum(hertz >= c for c in CROSSOVERS)


def band_responses():
    """each band's zero-phase response: its bins at 1, the others at 0"""
    responses = []
    for band in range(BANDS):
        gains = [1.0 if band_of(min(k, SAMPLES - k)) == band else 0.0 for k in range(SAMPLES)]
        responses.append([sum(g * math.cos(2 * math.pi * k * n / SAMPLES) for k, g in enumerate(gains) if g) / SAMPLES
                          for n in range(SAMPLES)])
    return responses


def directions():
    """the table's directions (azimuth to the right, elevation), the rows from below, then the poles"""
    return [(c * 15, e) for e in ELEVATIONS for c in range(AZIMUTHS)] + [(0, -90), (0, 90)]


def made(work):
    """the SOFA file, its figures expected (sofa_fit.c's) and a broken copy"""
    per_band = band_responses()
    ir, positions, delays, expected = [], [], [], []
    for d, (azimuth, elevation) in enumerate(directions()):
        alike = azimuth in (0, 180) or abs(elevation) == 90
        levels = [[round(8 * math.sin(1.3 * d + 0.7 * b + 2.1 * ear) - 3 * ear, 1) for b in range(BANDS)]
                  for ear in range(2)]
        if alike:
            levels[1] = levels[0]
        if azimuth == 90:
            # (impulses, the delay in their onsets)
            levels = [[-12.0 + d % 7] * BANDS, [2.0] * BANDS]
            onsets = (20 + d % 7, 3)
            for ear in range(2):
                ir += [10 ** (levels[ear][0] / 20) if n == onsets[ear] else 0.0 for n in range(SAMPLES)]
            delays += [0.0, 0.0]
            frames = (onsets[0] - onsets[1]) * OUTPUT_RATE / RATE
        else:
            for ear in range(2):
                gains = [10 ** (level / 20) for level in levels[ear]]
                ir += [sum(g * r[n] for g, r in zip(gains, per_band)) for n in range(SAMPLES)]
            delay = 0.0 if alike else 1.5 + 25 * math.sin(math.radians(azimuth)) * math.cos(math.radians(elevation))
            delays += [delay, 0.0 if alike else 1.0]
            frames = 0.0 if alike else (delay - 1.0) * OUTPUT_RATE / RATE
        positions += [(360 - azimuth) % 360, elevation, 1.5]
        expected.append(struct.pack(f"<{2 * BANDS + 1}f", *levels[0], *levels[1], frames))

    count = len(directions())
    file = HDF5()
    objects = [("Data.IR", chunked(file, (count, 2, SAMPLES), ir)),
               ("Data.SamplingRate", contiguous(file, (1,), [RATE])),
               ("Data.Delay", contiguous(file, (count, 2), delays)),
               ("SourcePosition", contiguous(file, (count, 3), positions,
                                             [string_attribute("Type", "spherical")]))]
    root = file.header([(0x02, bytes([0, 0]) + struct.pack("<QQ", UNDEFINED, UNDEFINED)), (0x0A, bytes([0, 0])),
                        *[link(name, address) for name, address in objects],
                        string_attribute("SOFAConventions", "SimpleFreeFieldHRIR")])
    data = file.finish(root)
    (work / "head.sofa").write_bytes(data)
    (work / "expected.bin").write_bytes(b"".join(expected))
    (work / "broken.sofa").write_bytes(data[:len(data) // 2])
    return work / "head.sofa", work / "expected.bin", work / "broken.sofa"


# ---------- the test

def zlib_objects(work):
    """the port's zlib's inflate, compiled"""
    objects = []
    for source in ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c"):
        objects.append(work / f"{source}.o")
        subprocess.run([os.environ.get("CC", "clang"), "-c", "-O1", "-w", "-DZ_PREFIX", "-I", str(ZLIB), str(ZLIB / source), "-o",
                        str(objects[-1])], check=True)
    return objects


def built(work, objects, fault=None):
    text = read("port/linux/src/dsound_sdl.c")
    (_, mixer), = generated(text, fault=fault)
    (work / "under_test.inc").write_text(mixer, encoding="latin-1")
    (work / "hrtf_load.inc").write_text(function(text, "hrtf_load") + "\n", encoding="latin-1")
    executable = work / ("sofa_fit" + (".exe" if os.name == "nt" else ""))
    command = [os.environ.get("CC", "clang"), "-std=gnu99", "-O0" if fault else "-O2", "-ffp-contract=off", "-Wall", "-Werror",
               "-Wno-unused-function", "-Wno-unused-variable", "-I", str(HARNESS / "include"), "-I", str(work), "-I",
               str(ROOT / "port/linux/src"), "-I", str(ZLIB), str(HARNESS / "tests/sofa_fit.c"), *map(str, objects),
               "-o", str(executable), *([] if os.name == "nt" else ["-lm"])]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    return executable


def fitted(tmp_path_factory, fault=None):
    work = tmp_path_factory.mktemp("sofa_fit-fault" if fault else "sofa_fit")
    files = made(work)
    executable = built(work, zlib_objects(work), fault)
    result = subprocess.run([str(executable), "sofa", *map(str, files)], capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


def test_sofa_fitted(tmp_path_factory):
    status, output = fitted(tmp_path_factory)
    assert status == 0, output


def test_negative_control(tmp_path_factory):
    status, output = fitted(tmp_path_factory, FAULT)
    assert status == CHECK_FAILED, f"the fit with SOFA's azimuth to the right passed: the test cannot see it\n{output}"
