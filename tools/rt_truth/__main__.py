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
    r.add_argument("--spp-pass", type=int, default=8)
    r.add_argument("--max-spp", type=int, default=65536)
    r.add_argument("--max-minutes", type=float, default=60.0)
    r.add_argument("--flashlight-scale", type=float, help="the flashlight's power scale k: physical 1/d^2 falloff")
    r.add_argument("--flashlight-emitter", help="x,y,z: where the flashlight's light leaves (the lamp), the gel "
                   "still projected from lights.json's position (until lights.json carries emitter_position)")
    r.add_argument("--flashlight-direct-only", action="store_true", help="only truth_parts/direct_flashlight.exr")
    k = sub.add_parser("compare", help="a real-time irradiance EXR against the truth's part")
    k.add_argument("scene")
    k.add_argument("exr")
    k.add_argument("--part", default="direct_flashlight")
    k.add_argument("--out")
    k.add_argument("--quantity", default="irradiance", choices=("irradiance", "radiance"))
    k.add_argument("--json", action="store_true")
    s = sub.add_parser("synthetic", help="write the synthetic proof scene")
    s.add_argument("folder")
    c = sub.add_parser("check", help="check the synthetic scene's shadow, gel and falloff with numbers")
    c.add_argument("folder")
    a = ap.parse_args()
    if a.cmd == "render":
        from .render import render_scene, render_flashlight_direct
        if a.flashlight_direct_only:
            em = [float(c) for c in a.flashlight_emitter.split(",")] if a.flashlight_emitter else None
            return render_flashlight_direct(a.scene, a.out, a.flashlight_scale, em)
        render_scene(a.scene, a.out, a.spp_pass, a.target, a.max_spp, a.max_minutes, flashlight_scale=a.flashlight_scale,
                     flashlight_emitter=[float(c) for c in a.flashlight_emitter.split(",")] if a.flashlight_emitter else None)
    elif a.cmd == "compare":
        import json
        from .compare import compare
        r = compare(a.scene, a.exr, a.out, a.part, a.quantity)
        print(json.dumps(r, indent=None if a.json else 1))
    elif a.cmd == "synthetic":
        from .synthetic import write_scene
        write_scene(a.folder)
    else:
        from .synthetic import check
        sys.exit(0 if check(a.folder) else 1)


main()
