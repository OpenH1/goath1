#!/usr/bin/env python3
"""A held frame's stability: how much the ray-traced passes' noise moves a still picture (shimmer), whether the
picture drifts while held (swelling), and how far its first frame is from its converged one.

    python3 tools/rt2_stability.py <recording> --frame N [--replay build/linux_x86_64/halo_replay]
        [--host omarchy|5900x|user@host] [--frames 120] [--out build/rt2_stability] [--reset]

The replay service (docs/replay_protocol.md, "hold") plays frame N F times as though the world stood still (the
identity reprojection, the frame number advancing: the passes' noise turns as in play). Its pictures (the
presented SDR picture, its sRGB decoded to linear light) give, over the HUD's pixels masked out (the ID buffer and
the draws' kinds, where the recording has marks):

- the temporal standard deviation of each pixel's luminance over the last 20 plays: p50 and p99, absolute
  (linear, 0 to 1) and relative (to the pixel's mean, where it is not black);
- swelling: the largest change of a 32x32 tile's mean luminance between plays 30 and 120;
- frame 1 against 120: the largest and the median tile-mean change;
- a heat map (heat.png) of the per-pixel deviation, black none, white at the p99.9.

With --host the replay runs there over ssh (the recording and the replay binary are paths on that machine), holding
goat-rig's GPU lock shared; on 5900x it refuses while the owner's own game runs. No numpy needed.
"""

import argparse
import base64
import json
import math
import shlex
import struct
import subprocess
import sys
import zlib
from pathlib import Path

try:
    import numpy as np
except ImportError:
    np = None

ROOT = Path(__file__).resolve().parent.parent
TILE = 32
LAST = 20
MASKED_KINDS = {"hud", "text", "menu_widget", "cinematic_letterbox", "debug"}
# (the machine owner's own game, which goat-rig never runs beside: the remote shell's $HOME)
OWNER_GAME = "^$HOME/Applications/OpenH1/bin/openh1"


def ssh_target(host):
    """goat-rig's: <name>.fabric, unless ~/.config/goat-rig/hosts gives a "<name> <user@host>" line"""
    hosts = Path.home() / ".config" / "goat-rig" / "hosts"
    if hosts.is_file():
        for line in hosts.read_text().splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] == host:
                return parts[1]
    return host if "@" in host or "." in host else f"{host}.fabric"


def srgb_linear(byte):
    c = byte / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


LUT_R = [0.2126 * srgb_linear(i) for i in range(256)]
LUT_G = [0.7152 * srgb_linear(i) for i in range(256)]
LUT_B = [0.0722 * srgb_linear(i) for i in range(256)]


def png_luminance(data):
    """an 8-bit RGB or RGBA PNG's pixels' linear luminance, row-major; (width, height, list, or a numpy array
    when numpy is there)"""
    if np is not None:
        return png_luminance_numpy(data)
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, idat, width = 8, [], 0
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour not in (2, 6):
                raise ValueError("only 8-bit RGB(A) PNGs")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        at += 12 + length
    raw = zlib.decompress(b"".join(idat))
    stride = width * channels + 1
    out = []
    previous = bytearray(stride - 1)
    for y in range(height):
        line = bytearray(raw[y * stride + 1:(y + 1) * stride])
        kind = raw[y * stride]
        if kind:
            # (the filters, rarely used by the service: its compressed PNGs use Sub)
            for i in range(len(line)):
                a = line[i - channels] if i >= channels else 0
                b = previous[i]
                c = previous[i - channels] if i >= channels else 0
                if kind == 1:
                    line[i] = (line[i] + a) & 255
                elif kind == 2:
                    line[i] = (line[i] + b) & 255
                elif kind == 3:
                    line[i] = (line[i] + ((a + b) >> 1)) & 255
                elif kind == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        previous = line
        out.extend(LUT_R[r] + LUT_G[g] + LUT_B[b] for r, g, b in
                   zip(line[0::channels], line[1::channels], line[2::channels]))
    return width, height, out


def png_rgb_numpy(data):
    """an 8-bit RGB(A) PNG's pixels as a height x width x 3 uint8 array (numpy)"""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    at, idat = 8, []
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if depth != 8 or colour not in (2, 6):
                raise ValueError("only 8-bit RGB(A) PNGs")
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat.append(body)
        at += 12 + length
    raw = np.frombuffer(zlib.decompress(b"".join(idat)), np.uint8).reshape(height, width * channels + 1)
    kinds, rows = raw[:, 0], raw[:, 1:].astype(np.int32).reshape(height, width, channels)
    out = np.empty_like(rows)
    previous = np.zeros((width, channels), np.int32)
    for y in range(height):
        line, kind = rows[y], kinds[y]
        if kind == 0:
            line = line
        elif kind == 1:
            line = np.cumsum(line, axis=0) & 255
        elif kind == 2:
            line = (line + previous) & 255
        else:
            # (Average and Paeth, which the service does not write: byte by byte)
            line = line.copy()
            for x in range(width):
                a = line[x - 1] if x else np.zeros(channels, np.int32)
                b, c = previous[x], (previous[x - 1] if x else np.zeros(channels, np.int32))
                if kind == 3:
                    line[x] = (line[x] + ((a + b) >> 1)) & 255
                else:
                    p = a + b - c
                    pa, pb, pc = np.abs(p - a), np.abs(p - b), np.abs(p - c)
                    line[x] = (line[x] + np.where((pa <= pb) & (pa <= pc), a, np.where(pb <= pc, b, c))) & 255
        out[y] = line
        previous = line
    return out[:, :, :3].astype(np.uint8)


def png_luminance_numpy(data):
    rgb = png_rgb_numpy(data)
    lut = np.array([srgb_linear(i) for i in range(256)])
    lum = 0.2126 * lut[rgb[..., 0]] + 0.7152 * lut[rgb[..., 1]] + 0.0722 * lut[rgb[..., 2]]
    return rgb.shape[1], rgb.shape[0], lum.reshape(-1)


def png_write(path, width, height, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * width * 3:(y + 1) * width * 3]) for y in range(height))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xffffffff)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def percentile(values, p):
    if not len(values):
        return float("nan")
    ordered = np.sort(np.asarray(values)) if np is not None else sorted(values)
    return float(ordered[min(len(ordered) - 1, int(p / 100.0 * len(ordered)))])


class Service:
    def __init__(self, recording, replay, host):
        command = [replay, recording, "--serve"]
        if host:
            target = ssh_target(host)
            remote = " ".join(shlex.quote(part) for part in command)
            if "5900x" in target:
                remote = (f"if pgrep -fa \"{OWNER_GAME}\" >/dev/null; then echo 'the owner is playing' >&2; "
                          f"exit 3; fi; {remote}")
            # (goat-rig's GPU lock, shared: a run for pictures, not timings)
            remote = f"mkdir -p ~/goat-rig/locks; flock -s ~/goat-rig/locks/gpu.lock sh -c {shlex.quote(remote)}"
            command = ["ssh", "-C", target, remote]
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                        bufsize=1)

    def ask(self, request):
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise SystemExit(f"rt2_stability: the replay stopped (exit {self.process.wait()})")
        reply = json.loads(line)
        if not reply.get("ok"):
            raise SystemExit(f"rt2_stability: {request['cmd']}: {reply.get('error')}")
        return reply

    def close(self):
        try:
            self.ask({"cmd": "quit"})
        except (SystemExit, BrokenPipeError, json.JSONDecodeError):
            pass
        self.process.wait()


def hud_mask(service, frame, width, height):
    """1 where the pixel is measured: not the HUD's (needs the recording's marks), else every pixel"""
    info = service.ask({"cmd": "info"})
    if not info.get("marks") or "ids" not in info.get("capabilities", []):
        return [1] * (width * height), "none (the recording has no marks or the backend no ID buffer)"
    draws = service.ask({"cmd": "draws", "n": frame})["draws"]
    masked = {draw["i"] + 1 for draw in draws if draw.get("kind") in MASKED_KINDS}
    reply = service.ask({"cmd": "frame", "n": frame, "ids": True, "inline": True, "compress": True})
    ids = (zlib.decompress(base64.b64decode(reply["ids_zlib_base64"])) if "ids_zlib_base64" in reply else
           base64.b64decode(reply["ids_base64"]))
    top = struct.unpack(f"<{len(ids) // 4}I", ids)
    if len(top) != width * height:
        return [1] * (width * height), "none (the ID buffer's size is not the picture's)"
    mask = [0 if value in masked else 1 for value in top]
    return mask, f"{len(mask) - sum(mask)} pixels of {len(masked)} HUD draws"


def tile_means(lum, mask, width, height):
    means = {}
    for ty in range(height // TILE):
        for tx in range(width // TILE):
            total = count = 0
            for y in range(ty * TILE, (ty + 1) * TILE):
                row = y * width
                for x in range(tx * TILE, (tx + 1) * TILE):
                    if mask[row + x]:
                        total += lum[row + x]
                        count += 1
            if count >= TILE * TILE // 2:
                means[(tx, ty)] = total / count
    return means


def tile_change(a, b):
    """the largest and the median relative change of the tiles' means (tiles darker than 0.002 left out), and
    the largest absolute one"""
    rel, absolute = [], []
    for key, value in a.items():
        other = b.get(key)
        if other is None:
            continue
        absolute.append(abs(value - other))
        if max(value, other) > 0.002:
            rel.append(abs(value - other) / max(other, 1e-6))
    return (max(rel) if rel else 0.0, percentile(rel, 50), max(absolute) if absolute else 0.0)


def shots_for(frames):
    """the plays a hold of so many keeps: 1, 30, 120 and the last LAST"""
    return sorted({1, 30, 120, *range(frames - LAST + 1, frames + 1)})


def hold(service, frame, frames=120, reset=False):
    """the replay's hold of frame (protocol 3): (reply, width, height, mask, what the mask is)"""
    info = service.ask({"cmd": "info"})
    if "hold" not in info.get("capabilities", []):
        raise SystemExit("rt2_stability: this replay has no hold (protocol 3)")
    reply = service.ask({"cmd": "hold", "n": frame, "frames": frames, "shots": shots_for(frames), "inline": True,
                         "compress": True, "reset": reset})
    width, height = reply["width"], reply["height"]
    mask, mask_said = hud_mask(service, frame, width, height)
    return info, reply, width, height, mask, mask_said


def measure(reply, width, height, mask, frames, out, perturb=None):
    """the hold's pictures measured (the module's doc); hold_<k>.png and heat.png written into out. perturb(k, lum)
    (a negative control) changes a play's luminance before it is measured. Returns the result's measures."""
    last = list(range(frames - LAST + 1, frames + 1))
    shots = shots_for(frames)
    pictures = {}
    for shot in reply["shots"]:
        data = base64.b64decode(shot["png_base64"])
        if shot["k"] in (1, 30, 120, frames):
            (out / f"hold_{shot['k']}.png").write_bytes(data)
        w, h, lum = png_luminance(data)
        if (w, h) != (width, height):
            raise SystemExit(f"rt2_stability: play {shot['k']} is {w}x{h}, not {width}x{height}")
        pictures[shot["k"]] = perturb(shot["k"], lum) if perturb else lum
    missing = [k for k in shots if k not in pictures]
    if missing:
        raise SystemExit(f"rt2_stability: plays without pictures: {missing}")
    count = width * height
    if np is not None:
        measured_mask = np.asarray(mask, bool)
        stack = np.stack([np.asarray(pictures[k], np.float64) for k in last])
        mean = stack.mean(0)
        deviation = np.sqrt(np.maximum((stack * stack).mean(0) - mean * mean, 0.0))
        floor_mask = measured_mask & (np.arange(count) >= count * 2 // 3)
        bright = mean > 0.002
        measured = deviation[measured_mask]
        relative = (deviation / np.maximum(mean, 1e-9))[measured_mask & bright]
        floor = deviation[floor_mask]
        floor_relative = (deviation / np.maximum(mean, 1e-9))[floor_mask & bright]
        tiles = {k: tile_means_numpy(np.asarray(pictures[k], np.float64), measured_mask, width, height)
                 for k in (1, 30, 120)}
    else:
        total = [0.0] * count
        squares = [0.0] * count
        for k in last:
            lum = pictures[k]
            total = [t + v for t, v in zip(total, lum)]
            squares = [s + v * v for s, v in zip(squares, lum)]
        n = float(len(last))
        deviation = [math.sqrt(max(s / n - (t / n) ** 2, 0.0)) for t, s in zip(total, squares)]
        mean = [t / n for t in total]
        measured = [d for d, m in zip(deviation, mask) if m]
        relative = [d / mu for d, mu, m in zip(deviation, mean, mask) if m and mu > 0.002]
        # (the lower third of the view: the floor, at the Flood corner)
        floor = [d for i, (d, m) in enumerate(zip(deviation, mask)) if m and i >= count * 2 // 3]
        floor_relative = [d / mean[i] for i, (d, m) in enumerate(zip(deviation, mask))
                          if m and i >= count * 2 // 3 and mean[i] > 0.002]
        tiles = {k: tile_means(pictures[k], mask, width, height) for k in (1, 30, 120)}
    swelling = tile_change(tiles[30], tiles[120])
    first = tile_change(tiles[1], tiles[120])

    # the heat map: black none, through red and yellow, white at the p99.9
    top = max(percentile(measured, 99.9), 1e-6)
    if np is not None:
        v = np.where(np.asarray(mask, bool), np.minimum(np.asarray(deviation) / top, 1.0) * 3.0, 0.0)
        rgb = np.stack([np.clip(v, 0, 1), np.clip(v - 1, 0, 1), np.clip(v - 2, 0, 1)], -1) * 255
        rgb = rgb.astype(np.uint8)
        rgb[~np.asarray(mask, bool)] = (0x30, 0x30, 0x60)
        rgb = bytearray(rgb.tobytes())
    else:
        rgb = bytearray(count * 3)
        for i, d in enumerate(deviation):
            v = min(d / top, 1.0) * 3.0 if mask[i] else 0.0
            rgb[3 * i] = int(min(v, 1.0) * 255)
            rgb[3 * i + 1] = int(min(max(v - 1.0, 0.0), 1.0) * 255)
            rgb[3 * i + 2] = int(min(max(v - 2.0, 0.0), 1.0) * 255)
            if not mask[i]:
                rgb[3 * i:3 * i + 3] = b"\x30\x30\x60"
    png_write(out / "heat.png", width, height, rgb)
    return {
        "temporal_std": {"plays": [last[0], last[-1]], "p50": percentile(measured, 50), "p99": percentile(measured, 99),
                         "relative_p50": percentile(relative, 50), "relative_p99": percentile(relative, 99),
                         "floor_p50": percentile(floor, 50), "floor_p99": percentile(floor, 99),
                         "floor_relative_p99": percentile(floor_relative, 99), "heat_white": top},
        "swelling_30_120": {"max_relative": swelling[0], "median_relative": swelling[1], "max_absolute": swelling[2]},
        "first_vs_120": {"max_relative": first[0], "median_relative": first[1], "max_absolute": first[2]},
    }


def tile_means_numpy(lum, mask, width, height):
    """tile_means, with numpy"""
    tw, th = width // TILE, height // TILE
    lum = lum.reshape(height, width)[:th * TILE, :tw * TILE].reshape(th, TILE, tw, TILE)
    keep = mask.reshape(height, width)[:th * TILE, :tw * TILE].reshape(th, TILE, tw, TILE)
    total = (lum * keep).sum((1, 3))
    count = keep.sum((1, 3))
    return {(tx, ty): total[ty, tx] / count[ty, tx] for ty in range(th) for tx in range(tw)
            if count[ty, tx] >= TILE * TILE // 2}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("recording", help="the recording (HALO_RENDER_RECORD); on --host, its path there")
    parser.add_argument("--frame", type=int, required=True, help="the frame held")
    parser.add_argument("--replay", default=str(ROOT / "build" / "linux_x86_64" / "halo_replay"),
                        help="the replay binary (on --host, its path there)")
    parser.add_argument("--host", help="run the replay there over ssh: omarchy, 5900x or user@host")
    parser.add_argument("--frames", type=int, default=120, help="the plays of the hold (at least 120)")
    parser.add_argument("--reset", action="store_true", help="the hold's first play begins its histories again")
    parser.add_argument("--out", type=Path, default=ROOT / "build" / "rt2_stability", help="the results' folder")
    args = parser.parse_args()
    if args.frames < 120:
        parser.error("--frames: at least 120 (the swelling check reads play 120)")
    args.out.mkdir(parents=True, exist_ok=True)

    service = Service(args.recording, args.replay, args.host)
    try:
        info, reply, width, height, mask, mask_said = hold(service, args.frame, args.frames, args.reset)
    finally:
        service.close()
    measures = measure(reply, width, height, mask, args.frames, args.out)
    result = {
        "recording": args.recording, "frame": args.frame, "frames": args.frames, "reset": args.reset,
        "backend": info.get("backend"), "build": info.get("build"), "size": [width, height], "hud_mask": mask_said,
        "hold_ms": reply.get("ms"), **measures,
    }
    (args.out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    t = result["temporal_std"]
    swelling = result["swelling_30_120"]
    first = result["first_vs_120"]
    print(f"frame {args.frame} held {args.frames} plays ({info.get('backend')} {info.get('build')}, "
          f"{width}x{height}; HUD masked: {mask_said})")
    print(f"temporal std, plays {t['plays'][0]}-{t['plays'][1]} (linear luminance 0-1): p50 {t['p50']:.5f}  "
          f"p99 {t['p99']:.5f}; relative p50 {t['relative_p50']:.2%}  p99 {t['relative_p99']:.2%}")
    print(f"  the floor (lower third): p50 {t['floor_p50']:.5f}  p99 {t['floor_p99']:.5f}  "
          f"relative p99 {t['floor_relative_p99']:.2%}")
    print(f"swelling, tile means 30 vs 120: max {swelling['max_relative']:.2%} (abs {swelling['max_absolute']:.5f}), "
          f"median {swelling['median_relative']:.2%}")
    print(f"frame 1 vs 120 tile means: max {first['max_relative']:.2%} (abs {first['max_absolute']:.5f}), "
          f"median {first['median_relative']:.2%}")
    print(f"heat map: {args.out / 'heat.png'} (white = std {t['heat_white']:.5f})")


if __name__ == "__main__":
    sys.exit(main())
