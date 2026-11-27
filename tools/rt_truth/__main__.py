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
    r.add_argument("--static", default="lightmap", choices=("lightmap", "fit"),
                   help="static light from Halo's lightmap (default) or fitted emitters (sky, sun, radiosity)")
    r.add_argument("--object-static", default="halo", choices=("physical", "halo"),
                   help="objects' static light: the game's model lighting (halo, the default: the owner's "
                   "baseline) or only the light the lightmapped walls re-emit (physical)")
    r.add_argument("--centers", action="store_true", help="irradiance at pixel centres (as a rasterizer shades), "
                   "not averaged over each pixel")
    r.add_argument("--dynamic-direct-only", action="store_true", help="only the other dynamic lights' direct part")
    r.add_argument("--light", type=int, help="with --dynamic-direct-only: that light alone (its index among them)")
    r.add_argument("--dynamic-scale", type=float, help="the other dynamic lights in the physical model with this k")
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
        if a.dynamic_direct_only:
            return render_flashlight_direct(a.scene, a.out, a.flashlight_scale, centers=a.centers, group="dynamic",
                                            light_index=a.light, light_scale=a.dynamic_scale)
        if a.flashlight_direct_only:
            return render_flashlight_direct(a.scene, a.out, a.flashlight_scale, centers=a.centers)
        render_scene(a.scene, a.out, a.spp_pass, a.target, a.max_spp, a.max_minutes, flashlight_scale=a.flashlight_scale,
                     static_mode=a.static, object_static=a.object_static)
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
