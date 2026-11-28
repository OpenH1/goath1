#!/usr/bin/env python3
"""The names the release publishes each build under, as players see them in
their downloads: OpenH1-<machine>[-debug] (.dmg for the Mac, .apk for
Android, .zip for the rest). The workflow's artifacts and dist/ keep their own names
(tools/ci_build.py dist_name: halo-<platform>-<config>), which this maps.

    python3 tools/release_names.py <artifact>   # its published name, without the extension
    python3 tools/release_names.py --check-symbols <folder>   # warns of a build without its symbols
    python3 tools/release_names.py --check-release <folder>   # fails when a file every release has is missing

The self-updaters ask for these names (port/linux/src/updater.c's
UPDATE_NAME, and the Android app's Updater.java, with -debug for a debug
build).

Beside each build the release has its symbols, OpenH1-<machine>[-debug]-symbols.zip
(the artifact halo-<platform>-<config>-symbols, tools/ci_build.py's): the
guest image with its source lines, halo_guest.debug.elf, of exactly that
build (and Windows's PDBs), which tools/symbolize_crash.py reads a crash
report with. No player and no self-updater downloads them. The Mac's and
iOS's are named for their system (OpenH1-macOS-symbols.zip), as their
builds' names (OpenH1.dmg) are not. --check-symbols warns (a GitHub
::warning:: line) of each build in a release's folder whose symbols are
missing or hold no image, and never fails: symbols missing do not stop a
release.

--check-release fails (exit 1, a ::error:: line for each) when the release's
folder lacks a file every release publishes (EXPECTED below: each platform's
builds, the iOS app, the Windows replay tool and the Mac's appcast), so that
a build whose artifact never arrived (a job the release did not wait for, an
upload that failed: build-2554 went out without its iOS app) stops the
release instead of publishing without it. The GitHub workflow's release job
and tools/release_local.py both run it before the sums are signed. Symbols
stay --check-symbols's (warnings only), and the DLSS add-on is optional.
"""

import sys
import zipfile
from pathlib import Path

# the release builds' names; a debug build's is the same with -debug
NAMES = {
    "windows": "OpenH1-Windows",
    "linux-x86_64": "OpenH1-Linux",
    "linux-x86_64-vr": "OpenH1-Linux-VR",
    "linux-arm64": "OpenH1-Linux-ARM64",
    "steam-frame-vr": "OpenH1-Steam-Frame",
    "macos": "OpenH1",
    "android": "OpenH1-Android",
}

# the symbols' names where the build's would not say the machine
SYMBOLS_NAMES = {
    "macos": "OpenH1-macOS",
    "ios": "OpenH1-iOS",
}

# every file a release publishes, by platform (the builds' names above with
# their extension; the iOS app keeps its artifact's name, halo-ios-debug.zip,
# as the releases always had it): --check-release
EXPECTED = {
    "linux-x86_64": ["OpenH1-Linux.zip", "OpenH1-Linux-debug.zip", "OpenH1-Linux-VR.zip"],
    "linux-arm64": ["OpenH1-Linux-ARM64.zip", "OpenH1-Linux-ARM64-debug.zip", "OpenH1-Steam-Frame.zip"],
    "windows": ["OpenH1-Windows.zip", "OpenH1-Windows-debug.zip", "halo-windows-replay.zip"],
    "android": ["OpenH1-Android.apk", "OpenH1-Android-debug.apk"],
    "macos": ["OpenH1.dmg", "OpenH1-debug.dmg", "appcast.xml"],
    "ios": ["halo-ios-debug.zip"],
}

# the file in each symbols zip that tools/symbolize_crash.py reads
GUEST_DEBUG_IMAGE = "halo_guest.debug.elf"

# the players' files of the builds (the rest are .zip)
EXTENSIONS = {"macos": ".dmg", "android": ".apk"}


def symbols_name(platform: str, config: str) -> str:
    """the published name of a build's symbols, without the extension"""
    return SYMBOLS_NAMES.get(platform, NAMES.get(platform)) + ("-debug" if config == "debug" else "") + "-symbols"


def published(artifact: str) -> str:
    """halo-<platform>-<config>[-symbols] -> OpenH1-<machine>[-debug][-symbols]; an artifact of no machine
    here keeps its name"""
    name, symbols = (artifact[:-len("-symbols")], True) if artifact.endswith("-symbols") else (artifact, False)
    for config in ("release", "debug"):
        if name.startswith("halo-") and name.endswith(f"-{config}"):
            platform = name[len("halo-"):-len(f"-{config}")]
            if symbols and (platform in NAMES or platform in SYMBOLS_NAMES):
                return symbols_name(platform, config)
            if platform in NAMES:
                return NAMES[platform] + ("-debug" if config == "debug" else "")
    return artifact


def missing_symbols(folder: Path) -> list:
    """what is wrong with the symbols of the builds in a release's folder:
    a build's file (OpenH1-Linux.zip...) without its symbols zip, or one
    whose zip has no guest image in it"""
    files = {path.name for path in folder.iterdir() if path.is_file()}
    problems = []
    for platform in NAMES:
        for config in ("release", "debug"):
            build = NAMES[platform] + ("-debug" if config == "debug" else "") + EXTENSIONS.get(platform, ".zip")
            if build not in files:
                continue
            symbols = symbols_name(platform, config) + ".zip"
            if symbols not in files:
                problems.append(f"{build} has no {symbols}: its crash reports get no source lines")
                continue
            try:
                with zipfile.ZipFile(folder / symbols) as archive:
                    sizes = {Path(info.filename).name: info.file_size for info in archive.infolist()}
            except (OSError, zipfile.BadZipFile) as error:
                problems.append(f"{symbols} cannot be read: {error}")
                continue
            if not sizes.get(GUEST_DEBUG_IMAGE):
                problems.append(f"{symbols} has no {GUEST_DEBUG_IMAGE}")
    return problems


def missing_files(folder: Path) -> list:
    """the files of EXPECTED a release's folder lacks (or has empty), as
    <platform>: <file>"""
    return [f"{platform}: {name}" for platform, names in EXPECTED.items() for name in names
            if not (folder / name).is_file() or (folder / name).stat().st_size == 0]


def main() -> int:
    arguments = sys.argv[1:]
    if len(arguments) == 2 and arguments[0] == "--check-symbols":
        problems = missing_symbols(Path(arguments[1]))
        for problem in problems:
            print(f"::warning::{problem}")
        if not problems:
            print("every build has its symbols")
        return 0
    if len(arguments) == 2 and arguments[0] == "--check-release":
        missing = missing_files(Path(arguments[1]))
        for name in missing:
            print(f"::error::the release has no {name}")
        if missing:
            print(f"{len(missing)} expected files missing: nothing is published", file=sys.stderr)
            return 1
        print(f"every expected file is there ({sum(len(names) for names in EXPECTED.values())})")
        return 0
    if len(arguments) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    print(published(arguments[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
