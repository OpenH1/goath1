"""The NVIDIA DLSS add-on (docs/temporal-aa.md, "The add-on"): the archive a player's game downloads from inside the
game, apart from the game's own package, which holds none of NVIDIA's files.

    python3 tools/dlss_addon.py pack --platform linux-x86_64 --module build/linux_x86_64/libopenh1_dlss.so \\
        --runtime build/linux_x86_64/libnvidia-ngx-dlss.so.310.9.16 \\
        --runtime build/linux_x86_64/libnvidia-ngx-dlssd.so.310.9.16 \\
        --license build/linux_x86_64/NVIDIA-DLSS-LICENSE.txt --out build/dlss-addon
    python3 tools/dlss_addon.py pin --tag build-1400 build/dlss-addon/openh1-dlss-*.json
    python3 tools/dlss_addon.py check-package dist/halo-linux-x86_64-release

pack writes, into --out, for one platform (linux-x86_64, windows-x64):

- openh1-dlss-<platform>-<version>.zip: the module (libopenh1_dlss.so, openh1_dlss.dll: port/linux/vulkan/dlss/
  openh1_dlss.c, MIT licensed, its license OPENH1-DLSS-LICENSE.txt), NVIDIA's DLSS runtimes (Super Resolution's,
  the first --runtime, which the game requires; Ray Reconstruction's, libnvidia-ngx-dlssd / nvngx_dlssd, optional)
  and NVIDIA's license (NVIDIA-DLSS-LICENSE.txt), each stored at the archive's top, its date fixed, in name order: the same inputs make
  the same bytes;
- openh1-dlss-<platform>-<version>.json: its manifest (the version, platform, file, size and SHA-256, and each
  file's);
- openh1-dlss-license-<version>.txt: NVIDIA's license alone, which the game shows before it downloads the archive.

The version is the DLSS SDK's and the module's source's (its SHA-256's first 8 digits): 310.9.16-1a2b3c4d.

pin writes port/linux/src/dlss_addon_pin.h from the manifests, which the game is built with: each platform's URL
(the release's: https://github.com/OpenH1/goath1/releases/download/<tag>/<file>), size and SHA-256, and the
license's. The file committed pins none (DLSS_ADDON_VERSION ""): such a game offers no download. A release's
workflow packs the add-on, uploads its files to the release, pins them, then builds the game.

check-package fails if a folder (a release's staged package) holds any of NVIDIA's files or the module.
"""

import argparse
import hashlib
import json
import re
import stat
import sys
import zipfile
from pathlib import Path
from typing import Dict, Iterable, List, Optional

ROOT = Path(__file__).resolve().parent.parent
MODULE_SOURCES = [ROOT / "port/linux/vulkan/dlss/openh1_dlss.c", ROOT / "port/linux/vulkan/xgpu_vulkan_dlss.h"]
PIN = ROOT / "port/linux/src/dlss_addon_pin.h"
REPOSITORY = "OpenH1/goath1"
URL = "https://github.com/{repository}/releases/download/{tag}/{file}"
PLATFORMS = {
    "linux-x86_64": {"module": "libopenh1_dlss.so", "runtime": "libnvidia-ngx-dlss.so.",
                     "rr_runtime": "libnvidia-ngx-dlssd.so.", "macro": "LINUX_X86_64"},
    "windows-x64": {"module": "openh1_dlss.dll", "runtime": "nvngx_dlss.dll", "rr_runtime": "nvngx_dlssd.dll",
                    "macro": "WINDOWS_X64"},
}
NVIDIA_LICENSE = "NVIDIA-DLSS-LICENSE.txt"
MODULE_LICENSE = "OPENH1-DLSS-LICENSE.txt"
# the files no package of the game may hold: NVIDIA's (the DLSS runtime, NGX, the SDK's license) and the module
# that links NGX (the add-on's alone)
NVIDIA_NAMES = re.compile(r"(^|/)(libopenh1_dlss\.so|openh1_dlss\.(dll|pdb)|libnvidia-ngx[^/]*|nvngx[^/]*\.dll|"
                          r"nvsdk_ngx[^/]*|NVIDIA-DLSS-LICENSE\.txt|openh1-dlss-[^/]*)$", re.IGNORECASE)
# (the module's license: the MIT license's text, the module's own copyright)
MODULE_LICENSE_TEXT = """\
OpenH1's DLSS module (libopenh1_dlss.so, openh1_dlss.dll), built from
port/linux/vulkan/dlss/openh1_dlss.c of https://github.com/OpenH1/goath1

MIT License

Copyright (c) 2026 the GOAT H1 project

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

The module links NVIDIA's NGX library, and the add-on carries NVIDIA's DLSS
runtime: those are NVIDIA's, under NVIDIA-DLSS-LICENSE.txt, not this license.
"""


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sdk_version() -> str:
    sys.path.insert(0, str(ROOT))
    from tools.vulkan_build import DLSS_SDK_TAG  # noqa: E402
    return DLSS_SDK_TAG.lstrip("v")


def version() -> str:
    """the DLSS SDK's version and the module's source's: 310.9.16-1a2b3c4d (its lines' ends taken as LF, so a
    Windows checkout's CRLF gives every platform's add-on the same version, as dlss_addon.py pin requires)"""
    source = hashlib.sha256()
    for path in MODULE_SOURCES:
        source.update(path.read_bytes().replace(b"\r\n", b"\n"))
    return f"{sdk_version()}-{source.hexdigest()[:8]}"


def archive_name(platform: str, addon_version: str) -> str:
    return f"openh1-dlss-{platform}-{addon_version}.zip"


def license_name(addon_version: str) -> str:
    return f"openh1-dlss-license-{addon_version}.txt"


def write_archive(output: Path, files: Dict[str, bytes]) -> None:
    """files (name: bytes) into the zip output: stored at its top, in name order, dated 1980-01-01, the libraries
    0755 and the rest 0644 (the same files, the same bytes)"""
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(files):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            executable = name.endswith((".so", ".dll")) or ".so." in name
            info.external_attr = (stat.S_IFREG | (0o755 if executable else 0o644)) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, files[name], compresslevel=9)


def pack(platform: str, module: Path, runtime: Path, license_path: Path, out: Path,
         addon_version: str = "", rr_runtime: Optional[Path] = None) -> Path:
    """the add-on's archive, manifest and license in out (with Ray Reconstruction's runtime too, if given); the
    manifest's path"""
    names = PLATFORMS[platform]
    if module.name != names["module"]:
        raise ValueError(f"{platform}'s module is {names['module']}, not {module.name}")
    if not runtime.name.startswith(names["runtime"]) or runtime.name.startswith(names["rr_runtime"]):
        raise ValueError(f"{platform}'s DLSS runtime is {names['runtime']}..., not {runtime.name}")
    if rr_runtime and not rr_runtime.name.startswith(names["rr_runtime"]):
        raise ValueError(f"{platform}'s Ray Reconstruction runtime is {names['rr_runtime']}..., not {rr_runtime.name}")
    addon_version = addon_version or version()
    license_text = license_path.read_bytes()
    files = {module.name: module.read_bytes(), runtime.name: runtime.read_bytes(), NVIDIA_LICENSE: license_text,
             MODULE_LICENSE: MODULE_LICENSE_TEXT.encode()}
    if rr_runtime:
        files[rr_runtime.name] = rr_runtime.read_bytes()
    out.mkdir(parents=True, exist_ok=True)
    archive = out / archive_name(platform, addon_version)
    temporary = archive.with_name(archive.name + ".tmp")
    write_archive(temporary, files)
    temporary.replace(archive)
    data = archive.read_bytes()
    (out / license_name(addon_version)).write_bytes(license_text)
    manifest = {
        "name": "openh1-dlss", "version": addon_version, "platform": platform, "file": archive.name,
        "size": len(data), "sha256": sha256(data), "module": module.name, "runtime": runtime.name,
        "rr_runtime": rr_runtime.name if rr_runtime else "",
        "license": {"file": license_name(addon_version), "size": len(license_text), "sha256": sha256(license_text)},
        "files": [{"name": name, "size": len(files[name]), "sha256": sha256(files[name])} for name in sorted(files)],
    }
    path = out / f"openh1-dlss-{platform}-{addon_version}.json"
    path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"{archive} ({len(data)} bytes, SHA-256 {manifest['sha256']})", flush=True)
    return path


def verify(manifest_path: Path) -> List[str]:
    """what is wrong with the archive beside its manifest (its size, its SHA-256, its files): none, an empty list"""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    archive = manifest_path.with_name(manifest["file"])
    problems = []
    data = archive.read_bytes() if archive.is_file() else b""
    if len(data) != manifest["size"]:
        problems.append(f"{archive.name} is {len(data)} bytes, not {manifest['size']}")
    if sha256(data) != manifest["sha256"]:
        problems.append(f"{archive.name}'s SHA-256 is {sha256(data)}, not {manifest['sha256']}")
        return problems
    with zipfile.ZipFile(archive) as opened:
        listed = {entry["name"]: entry for entry in manifest["files"]}
        if sorted(opened.namelist()) != sorted(listed):
            problems.append(f"{archive.name} holds {sorted(opened.namelist())}, not {sorted(listed)}")
        for name in opened.namelist():
            if name in listed and sha256(opened.read(name)) != listed[name]["sha256"]:
                problems.append(f"{name}'s SHA-256 is not the manifest's")
    return problems


def c_string(text: str) -> str:
    return json.dumps(text)


def pin_text(manifests: Iterable[Dict], tag: str, repository: str = REPOSITORY) -> str:
    """dlss_addon_pin.h's text: each manifest's platform's URL, size and SHA-256 (a platform without one: none)"""
    by_platform = {manifest["platform"]: manifest for manifest in manifests}
    versions = {manifest["version"] for manifest in by_platform.values()}
    licenses = {manifest["license"]["sha256"] for manifest in by_platform.values()}
    if len(versions) > 1 or len(licenses) > 1:
        raise ValueError(f"the manifests are of different versions or licenses: {sorted(versions)}")
    addon_version = versions.pop() if versions else ""
    first = next(iter(by_platform.values()), None)
    lines = [
        "/*",
        "DLSS_ADDON_PIN.H",
        "",
        "Written by tools/dlss_addon.py pin: the NVIDIA DLSS add-on this build",
        "downloads (port/linux/src/dlss_addon.c), each platform's archive's URL,",
        "size and SHA-256, and NVIDIA's license's. DLSS_ADDON_VERSION \"\": none",
        "pinned, and no download offered.",
        "*/",
        "",
        "#ifndef DLSS_ADDON_PIN_H",
        "#define DLSS_ADDON_PIN_H",
        "",
        f"#define DLSS_ADDON_VERSION {c_string(addon_version)}",
        f"#define DLSS_ADDON_LICENSE_URL {c_string(URL.format(repository=repository, tag=tag, file=first['license']['file']) if first else '')}",
        f"#define DLSS_ADDON_LICENSE_SIZE {first['license']['size'] if first else 0}ULL",
        f"#define DLSS_ADDON_LICENSE_SHA256 {c_string(first['license']['sha256'] if first else '')}",
    ]
    for platform, names in PLATFORMS.items():
        manifest = by_platform.get(platform)
        macro = names["macro"]
        url = URL.format(repository=repository, tag=tag, file=manifest["file"]) if manifest else ""
        lines += [
            f"#define DLSS_ADDON_URL_{macro} {c_string(url)}",
            f"#define DLSS_ADDON_SIZE_{macro} {manifest['size'] if manifest else 0}ULL",
            f"#define DLSS_ADDON_SHA256_{macro} {c_string(manifest['sha256'] if manifest else '')}",
        ]
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def check_package(folder: Path) -> List[str]:
    """the files in folder (a package's) that are NVIDIA's or the add-on's: none, an empty list"""
    found = []
    for path in sorted(folder.rglob("*")):
        name = path.relative_to(folder).as_posix()
        if NVIDIA_NAMES.search(name):
            found.append(name)
        elif path.is_file() and path.suffix.lower() == ".zip":
            with zipfile.ZipFile(path) as archive:
                found += [f"{name}:{inner}" for inner in archive.namelist() if NVIDIA_NAMES.search(inner)]
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    pack_command = commands.add_parser("pack", help="the add-on's archive, manifest and license for a platform")
    pack_command.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    pack_command.add_argument("--module", type=Path, required=True)
    pack_command.add_argument("--runtime", type=Path, action="append", required=True,
                              help="Super Resolution's runtime, then (optional) Ray Reconstruction's")
    pack_command.add_argument("--license", type=Path, required=True)
    pack_command.add_argument("--out", type=Path, required=True)
    pack_command.add_argument("--stamp", type=Path, help="a file written once it is done (ninja's output)")
    verify_command = commands.add_parser("verify", help="an archive checked against its manifest")
    verify_command.add_argument("manifests", type=Path, nargs="+")
    pin_command = commands.add_parser("pin", help=f"write {PIN.relative_to(ROOT)} from manifests")
    pin_command.add_argument("--tag", required=True, help="the release the files are uploaded to (build-<n>)")
    pin_command.add_argument("--repository", default=REPOSITORY)
    pin_command.add_argument("--out", type=Path, default=PIN)
    pin_command.add_argument("manifests", type=Path, nargs="*")
    check_command = commands.add_parser("check-package", help="fail if a package holds NVIDIA's files")
    check_command.add_argument("folders", type=Path, nargs="+")
    commands.add_parser("version", help="the add-on's version")
    args = parser.parse_args()
    if args.command == "pack":
        if len(args.runtime) > 2:
            parser.error("--runtime: Super Resolution's and Ray Reconstruction's, at most")
        manifest = pack(args.platform, args.module, args.runtime[0], args.license, args.out,
                        rr_runtime=args.runtime[1] if len(args.runtime) > 1 else None)
        if args.stamp:
            args.stamp.write_text(f"{manifest.name}\n", encoding="utf-8")
    elif args.command == "verify":
        problems = [problem for path in args.manifests for problem in verify(path)]
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1 if problems else 0
    elif args.command == "pin":
        manifests = [json.loads(path.read_text(encoding="utf-8")) for path in args.manifests]
        args.out.write_text(pin_text(manifests, args.tag, args.repository), encoding="utf-8")
        print(f"{args.out}: {', '.join(manifest['platform'] for manifest in manifests) or 'nothing'} pinned")
    elif args.command == "check-package":
        found = [f"{folder}/{name}" for folder in args.folders for name in check_package(folder)]
        for name in found:
            print(f"a package holds NVIDIA's or the DLSS add-on's file: {name}", file=sys.stderr)
        return 1 if found else 0
    elif args.command == "version":
        print(version())
    return 0


if __name__ == "__main__":
    sys.exit(main())
