#!/usr/bin/env python3
"""A release's files from its builds' artifacts, and its notes: the packaging
the GitHub workflow's release job does (.github/workflows/build.yml,
"Package" and "Publish") and tools/release_local.py does from our own
machines, the one code both use so that the two cannot drift.

    python3 tools/release_package.py package <artifacts> <release> [--dlss <folder>]
    python3 tools/release_package.py notes --subject S --commit-url U --build-url U [--carried F] > notes.md

package reads <artifacts>/<name>/ folders, each one build's artifact as
tools/ci_build.py leaves it in dist/ (halo-<platform>-<config>[-symbols],
halo-windows-replay...), and writes each under the name players see
(tools/release_names.py: OpenH1-Windows.zip, OpenH1-Linux.zip, OpenH1.dmg...):
the Linux builds with their executables' modes (tools/linux_package.py zip);
the Steam Frame's package, a zip already; the macOS build's disk image as it
is; the Android app's single APK as it is; each build's symbols zipped beside
it (a symbols zip that fails warns, and the release goes on); anything else
zipped as it is (the iOS app, halo-ios-debug.zip; the Windows replay tool).
No package may hold NVIDIA's files (tools/dlss_addon.py check-package); the
DLSS add-on's own files, openh1-dlss-*, are copied from --dlss when given.

notes prints the release's notes: the subject, the commit and build links,
the network versions a dedicated server's self-updater reads
(tools/ci_build.py notes), what to download on each system, and --carried's
lines (a platform carried over from an earlier release).
"""

import argparse
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

import release_names  # noqa: E402

DOWNLOADS = """The release builds are for playing; the debug builds stop at the first failed assertion and log it.

On Windows (64-bit Windows 10 1903 or later, with a Vulkan 1.3 graphics driver), download OpenH1-Windows.zip.

On Linux and the Steam Deck, download OpenH1-Linux.zip (OpenH1-Linux-ARM64.zip on 64-bit ARM, OpenH1-Linux-VR.zip for VR): it draws with Vulkan 1.3.

On the Steam Frame, in desktop mode, download OpenH1-Steam-Frame.zip, unzip it and open install.sh in it (port/linux/frame/README.md).

On macOS (Apple silicon, macOS 14.4 or later), download OpenH1.dmg, open it and drag GOAT H1 to Applications: the app is signed and notarized.

On Android (64-bit, Android 9 or later, with Vulkan 1.3), download OpenH1-Android.apk on the phone and install it. The app is signed with OpenH1's own key now: an install signed before it must be uninstalled once first (back up its maps and save folders).

The -symbols zips are not for playing: each has one build's debugging information (OpenH1-macOS-symbols.zip is OpenH1.dmg's, OpenH1-Linux-debug-symbols.zip OpenH1-Linux-debug.zip's), which tools/symbolize_crash.py gives a crash report's frames their source lines with.
"""


def zip_folder(folder: Path, output: Path) -> None:
    """the folder's files, relative to it (as `cd folder && zip -qr output .`)"""
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED) as archive:
        for path in sorted(folder.rglob("*")):
            if path.is_file():
                archive.write(path, path.relative_to(folder))


def package(artifacts: Path, release: Path, dlss: Path = None) -> int:
    release.mkdir(parents=True, exist_ok=True)
    for build in sorted(path for path in artifacts.iterdir() if path.is_dir()):
        name = build.name
        published = release_names.published(name)
        if name.startswith("halo-") and name.endswith("-symbols"):
            output = release / f"{published}.zip"
            try:
                zip_folder(build, output)
            except OSError as error:
                print(f"::warning::{name} could not be zipped ({error}): the release has no {output.name}")
                output.unlink(missing_ok=True)
        elif name.startswith("halo-steam-frame-"):
            shutil.copy2(build / f"{name}.zip", release / f"{published}.zip")
        elif name.startswith("halo-linux-"):
            subprocess.run([sys.executable, str(TOOLS / "linux_package.py"), "zip", str(build),
                            str(release / f"{published}.zip")], check=True)
        elif name.startswith("halo-macos-"):
            shutil.copy2(build / "OpenH1.dmg", release / f"{published}.dmg")
        elif name.startswith("halo-android-"):
            apks = sorted(build.glob("*.apk"))
            if len(apks) != 1:
                print(f"::error::{name} has no single APK")
                return 1
            shutil.copy2(apks[0], release / f"{published}.apk")
        else:
            zip_folder(build, release / f"{published}.zip")
    # (no package holds NVIDIA's files: then the DLSS add-on's, apart)
    subprocess.run([sys.executable, str(TOOLS / "dlss_addon.py"), "check-package", str(release)], check=True)
    addon = sorted(dlss.glob("openh1-dlss-*")) if dlss and dlss.is_dir() else []
    for path in addon:
        shutil.copy2(path, release / path.name)
    if not addon:
        print("::warning::no DLSS add-on pinned: this release offers none")
    for path in sorted(release.iterdir()):
        print(f"{path.stat().st_size:>12}  {path.name}")
    return 0


def notes(subject: str, commit_url: str, build_url: str, carried: Path = None) -> str:
    versions = subprocess.run([sys.executable, str(TOOLS / "ci_build.py"), "notes"], check=True,
                              capture_output=True, text=True).stdout
    text = f"{subject}\n\nCommit: {commit_url}\nBuild: {build_url}\n{versions}\n{DOWNLOADS}"
    if carried and carried.is_file():
        text += f"\n{carried.read_text().strip()}\n"
    return text


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("package")
    command.add_argument("artifacts", type=Path)
    command.add_argument("release", type=Path)
    command.add_argument("--dlss", type=Path, help="the DLSS add-on's pinned files (openh1-dlss-*)")
    command = commands.add_parser("notes")
    command.add_argument("--subject", required=True)
    command.add_argument("--commit-url", required=True)
    command.add_argument("--build-url", required=True)
    command.add_argument("--carried", type=Path, help="lines on platforms carried over from an earlier release")
    args = parser.parse_args()
    if args.command == "package":
        return package(args.artifacts, args.release, args.dlss)
    sys.stdout.write(notes(args.subject, args.commit_url, args.build_url, args.carried))
    return 0


if __name__ == "__main__":
    sys.exit(main())
