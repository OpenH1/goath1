#!/usr/bin/env python3
"""Royale's built-in map configs: port/assets/royale/<map>.toml into port/linux/game/royale_maps.inc, the texts the
game parses (port/linux/game/royale_config.c). Run it after changing a config:

    python3 tools/royale_maps.py           # writes the .inc
    python3 tools/royale_maps.py --check   # fails if the .inc is out of date
"""

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CONFIGS = ROOT / "port/assets/royale"
OUTPUT = ROOT / "port/linux/game/royale_maps.inc"


def c_string(text):
    lines = []
    for line in text.splitlines():
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        lines.append(f'\t\t"{escaped}\\n"')
    return "\n".join(lines) if lines else '\t\t""'


def generated():
    out = ["/* Royale's built-in map configs, port/assets/royale: made by tools/royale_maps.py, not by hand */", "",
           "static struct", "{", "\tchar const *name;", "\tchar const *text;", "} const royale_maps[] =", "{"]
    for path in sorted(CONFIGS.glob("*.toml")) if CONFIGS.is_dir() else []:
        out.append(f'\t{{ "{path.stem}",')
        out.append(c_string(path.read_text(encoding="utf-8")) + " },")
    out.append("\t{ NULL, NULL },")
    out.append("};")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = generated()
    if args.check:
        if not OUTPUT.exists() or OUTPUT.read_text(encoding="utf-8") != text:
            print(f"{OUTPUT.relative_to(ROOT)} is out of date: run tools/royale_maps.py", file=sys.stderr)
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
