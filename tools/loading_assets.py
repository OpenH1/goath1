#!/usr/bin/env python3
"""Convert the supplied loading artwork to the embedded RGBA images: at
640x480, and at its own size for the sharp frame to resample.

Run manually with Pillow installed; regular builds use the committed payload.
"""
import zlib
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DIRECTORY = ROOT / "port/assets/loading"


def main():
    with Image.open(DIRECTORY / "LOADING.jpg") as source:
        picture = source.convert("RGBA").resize((640, 480), Image.Resampling.LANCZOS)
        (DIRECTORY / "loading.rgba.zlib").write_bytes(zlib.compress(picture.tobytes(), 9))
        whole = source.convert("RGBA").tobytes()
        (DIRECTORY / "loading-source.rgba.zlib").write_bytes(zlib.compress(whole, 9))


if __name__ == "__main__":
    main()
