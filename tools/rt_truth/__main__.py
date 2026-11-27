"""python -m rt_truth render <scene folder> [...] | synthetic <folder> | check <synthetic scene folder>"""

import argparse
import sys


def main():
    ap = argparse.ArgumentParser(prog="rt_truth")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("render", help="render a scene folder's truth/")
    r.add_argument("scene")
    r.add_argument("--out", help="the output folder (default <scene>/truth)")
    r.add_argument("--target", type=float, default=0.01, help="median relative noise to stop at (0.01: 1%%)")
    r.add_argument("--spp-pass", type=int, default=16)
    r.add_argument("--max-spp", type=int, default=65536)
    r.add_argument("--max-minutes", type=float, default=60.0)
    s = sub.add_parser("synthetic", help="write the synthetic proof scene")
    s.add_argument("folder")
    c = sub.add_parser("check", help="check the synthetic scene's shadow, gel and falloff with numbers")
    c.add_argument("folder")
    a = ap.parse_args()
    if a.cmd == "render":
        from .render import render_scene
        render_scene(a.scene, a.out, a.spp_pass, a.target, a.max_spp, a.max_minutes)
    elif a.cmd == "synthetic":
        from .synthetic import write_scene
        write_scene(a.folder)
    else:
        from .synthetic import check
        sys.exit(0 if check(a.folder) else 1)


main()
