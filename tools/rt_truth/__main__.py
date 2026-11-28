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
    r.add_argument("--static", default="lightmap", choices=("lightmap", "fit", "emitters"),
                   help="static light from Halo's lightmap (default), fitted emitters (sky, sun, radiosity), or "
                   "traced from a solved emitter file (--emitters)")
    r.add_argument("--emitters", help="with --static emitters: the level's emitter file (python -m rt_truth emitters)")
    r.add_argument("--object-static", default="halo", choices=("physical", "halo"),
                   help="objects' static light: the game's model lighting (halo, the default: the owner's "
                   "baseline) or only the light the lightmapped walls re-emit (physical)")
    r.add_argument("--centers", action="store_true", help="irradiance at pixel centres (as a rasterizer shades), "
                   "not averaged over each pixel")
    r.add_argument("--dynamic-direct-only", action="store_true", help="only the other dynamic lights' direct part")
    r.add_argument("--light", type=int, help="with --dynamic-direct-only: that light alone (its index among them)")
    r.add_argument("--dynamic-scale", type=float, help="the other dynamic lights in the physical model with this k")
    r.add_argument("--flashlight-direct-only", action="store_true", help="only truth_parts/direct_flashlight.exr")
    e = sub.add_parser("emitters", help="solve a level's static light sources from its lightmap: its emitter file")
    e.add_argument("scene", help="a scene folder exported on the level (any camera: the whole BSP is used)")
    e.add_argument("--out", required=True, help="the emitter file to write")
    e.add_argument("--report", help="a folder for the residual pages and samples")
    e.add_argument("--spp", type=int, default=64, help="paths a sample")
    e.add_argument("--samples", type=int, default=300000, help="lightmap samples at most")
    e.add_argument("--max-added", type=int, default=24, help="added emitters (surface patches) at most")
    e.add_argument("--objects", default="on", choices=("on", "off"),
                   help="the level's scenery, machines, controls and fixtures in the transport (shading the bake)")
    e.add_argument("--cutouts", default="pass", choices=("pass", "solid"),
                   help="alpha-tested BSP surfaces in the transport: light through their holes, or solid")
    e.add_argument("--glow", default="uniform", choices=("map", "uniform"),
                   help="a glowing shader's emission: where its self-illumination map glows, or its whole surface")
    e.add_argument("--target-gamma", type=float, default=2.2,
                   help="the lightmap page's decode for the fit: 2.2 as the game shows it, 1 the tool's linear values")
    e.add_argument("--mode", default="rooms", choices=("rooms", "fit"),
                   help="rooms: the tag's lamps, brightness matched per room-sized cell (the chosen form); "
                   "fit: the free least-squares fit with added patches")
    e.add_argument("--cell", type=float, default=12.0, help="rooms: the cell, wu")
    e.add_argument("--patch-size", type=float, default=2.0, help="the candidate patches' cell, wu")
    e.add_argument("--weights", default="area", choices=("area", "display"),
                   help="squared errors by world area (linear light), or as the shown page sees them")
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
        if a.static == "emitters" and not a.emitters:
            ap.error("--static emitters needs --emitters FILE")
        render_scene(a.scene, a.out, a.spp_pass, a.target, a.max_spp, a.max_minutes, flashlight_scale=a.flashlight_scale,
                     static_mode=a.static, object_static=a.object_static, emitters_file=a.emitters)
    elif a.cmd == "emitters":
        from . import emitters as _E
        from .emitters import solve_level
        _E.WITH_OBJECTS = a.objects == "on"
        _E.CUTOUTS_PASS = a.cutouts == "pass"
        _E.GLOW_PATTERN = a.glow == "map"
        _E.TARGET_GAMMA = a.target_gamma
        from .render import GpuLock, wait_for_no_player
        import os
        wait_for_no_player(print)
        with GpuLock(os.path.expanduser("~/goat-rig/locks/gpu.lock"), print):  # shared, as a truth render: no timing run
            if a.mode == "rooms":
                return _E.solve_rooms(a.scene, a.out, spp=max(a.spp, 128), max_samples=a.samples, cell=a.cell)
            solve_level(a.scene, a.out, spp=a.spp, max_samples=a.samples, max_added=a.max_added,
                        report=a.report, weights=a.weights, patch_size=a.patch_size)
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
