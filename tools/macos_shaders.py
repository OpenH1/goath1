#!/usr/bin/env python3
"""The macOS app's Metal shaders and pipelines, made when it is built
(tools/macos_build.py runs this; port/macos/README.md, "Shaders built ahead
of time"):

    python tools/macos_shaders.py --tool build/macos/xgpu_metal_prebuild \
        --manifest port/assets/render/manifest.txt --work build/macos/shaders \
        --library build/macos/shaders.metallib

1. The game's vertex shaders are read from its code (tools/render_manifest.py
   vertex), and the tool (port/macos/tools/xgpu_metal_prebuild.c) writes each
   shader the manifest's draws, each derived under every setting (xgpu_manifest_derive), and those vertex
   shaders need as <name>.metal, and the pipelines as a Metal pipelines script whose functions are the
   library's.
2. Each source is compiled to AIR (xcrun metal, in parallel, as the backend
   would compile it: MSL 3.1 for macOS 14.4, or with --platform ios for iOS
   17.4, safe maths) and linked into one library, shaders.metallib, which the
   app looks each shader up in.
The pipelines are not built here: a GPU's binaries are the OS's and its
driver's (a binary archive serves only the macOS version it was made for,
and one for each version and GPU family would be hundreds of MB). The app
makes them from this library while a map loads (port/macos/README.md).
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
from pathlib import Path

# as xgpu_metal_shader_create compiles a shader (MTLMathModeSafe), for each
# platform: its SDK and its deployment target (the macOS app's; the iOS
# app's, tools/ios_build.py)
METAL_FLAGS = ["-std=metal3.1", "-fmetal-math-mode=safe"]
# the functions the backend looks up by a name of its own rather than its
# source's hash, which the library must have or the app compiles them as a map
# is prepared: display.temporal_aa's passes (xgpu_metal_temporal.m's
# function_names, written by the tool as xgpu_metal_temporal.metal)
NAMED_FUNCTIONS = ["xgpu_temporal_vertex", "xgpu_temporal_resolve", "xgpu_temporal_view", "xgpu_temporal_reactive",
                   "xgpu_temporal_stretch"]
PLATFORMS = {
    "macos": ("macosx", "-mmacosx-version-min=14.4"),
    "ios": ("iphoneos", "-mios-version-min=17.4"),
    "ios_simulator": ("iphonesimulator", "-mios-simulator-version-min=17.4"),
}


def run(command, **arguments):
    result = subprocess.run(command, capture_output=True, text=True, **arguments)
    if result.returncode != 0:
        sys.stderr.write(result.stdout + result.stderr)
        raise SystemExit(f"failed: {' '.join(map(str, command[:4]))} ...")
    return result.stdout


def compile_chunk(folder, names, platform):
    sdk, minimum = PLATFORMS[platform]
    run(["xcrun", "-sdk", sdk, "metal", *METAL_FLAGS, minimum, "-c", *names], cwd=folder)


def library_lacks(library, platform):
    """the NAMED_FUNCTIONS a library lacks (its symbols: metal-nm)"""
    symbols = run(["xcrun", "-sdk", PLATFORMS[platform][0], "metal-nm", str(library)])
    defined = {line.split()[-1] for line in symbols.splitlines() if " T " in line}
    return [name for name in NAMED_FUNCTIONS if name not in defined]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--platform", choices=sorted(PLATFORMS), default="macos")
    parser.add_argument("--tool")
    parser.add_argument("--manifest")
    parser.add_argument("--work")
    parser.add_argument("--library")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--check", help="only check that this library has the NAMED_FUNCTIONS")
    arguments = parser.parse_args()
    if arguments.check:
        lacks = library_lacks(arguments.check, arguments.platform)
        raise SystemExit(f"{arguments.check} lacks {', '.join(lacks)}" if lacks else 0)
    if not all((arguments.tool, arguments.manifest, arguments.work, arguments.library)):
        parser.error("--tool, --manifest, --work and --library (or --check)")

    work = Path(arguments.work)
    sources = work / "sources"
    if work.exists():
        shutil.rmtree(work)
    sources.mkdir(parents=True)
    library = Path(arguments.library).resolve()
    script = work / "pipelines.mtlp-json"
    vertex = work / "vertex_shaders.txt"
    print(run([sys.executable, str(Path(__file__).resolve().parent / "render_manifest.py"), "vertex",
               str(vertex)]).strip())
    print(run([arguments.tool, arguments.manifest, str(sources), str(script), str(library), str(vertex)]).strip())

    names = sorted(path.name for path in sources.glob("*.metal"))
    chunks = [names[index::arguments.jobs] for index in range(arguments.jobs)]
    with concurrent.futures.ThreadPoolExecutor(arguments.jobs) as pool:
        list(pool.map(lambda chunk: compile_chunk(sources, chunk, arguments.platform) if chunk else None, chunks))
    airs = sorted(str(path) for path in sources.glob("*.air"))
    library.parent.mkdir(parents=True, exist_ok=True)
    run(["xcrun", "-sdk", PLATFORMS[arguments.platform][0], "metallib", *airs, "-o", str(library)])
    lacks = library_lacks(library, arguments.platform)
    if lacks:
        raise SystemExit(f"{library} lacks {', '.join(lacks)}: the app would compile them as a map is prepared")
    print(f"{library}: {len(airs)} sources, {library.stat().st_size // 1024} KB")



if __name__ == "__main__":
    main()
