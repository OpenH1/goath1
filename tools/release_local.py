#!/usr/bin/env python3
"""A release made on our own build machines, with GitHub only hosting the
files: the same builds, tests, packaging, signing and files as the GitHub
workflow's release (.github/workflows/build.yml), through the same scripts
(tools/ci_build.py for each build, tools/ci_plan.py's test lists,
tools/release_package.py, tools/release_names.py --check-release,
tools/macos_appcast.py, tools/release_sign.py). docs/releasing.md, "From our
own machines", has the steps and how to change a machine, and where this
tool stands: parked, its first dry run stopped at the Linux build.

    python3 tools/release_local.py build   --number N [--ref REF] [--only P,P] [--no-dlss]
    python3 tools/release_local.py sign    --number N       # on the signing Mac, at its keyboard
    python3 tools/release_local.py status  --number N
    python3 tools/release_local.py publish --number N [--dry-run]

build (run from any checkout that reaches the machines over ssh, and has gh
signed in) builds commit REF (default origin/master; it must be on GitHub,
which the machines fetch it from) as build-N:

- on the Linux machine (omarchy): Linux x86-64 (debug, release, VR) in the
  CI image's Ubuntu 22.04 file system (~/goat-rig/ci/rootfs, the
  self-hosted runners' own, so the builds start on the same distributions
  as GitHub's), its build-dependent tests and the scorecard on the debug
  build, then every test that needs no build (tools/ci_plan.py's two lists),
  the network numbers and the generated menus' check (the merge checks);
  Windows (debug, release, and the replay tool) cross-compiled against the
  MSVC toolchain in ~/goat-rig/xwin (configure.py --windows-cross, through
  ci_build.py's CI_CONFIGURE_ARGS); and, unless --no-dlss, the DLSS add-on
  for both, pinned to build-N before the games are built;
- on the Mac (the release Mac): macOS (debug, release), iOS (debug), Android
  (debug, release) and Linux ARM64 (debug, release, the Steam Frame's VR
  package) in a container of .github/ci-image built for arm64 (OrbStack),
  one after another; each tree but macOS's (which sign notarizes from) is
  removed once its files are sent, as that Mac's disk is small.

Each platform's dist/ folders go to the Linux machine's
~/goat-release/build-N/artifacts (the layout the workflow's artifacts have),
are packaged into ~/goat-release/build-N/release, and its result is kept in
status/<platform>. A platform whose build failed is carried over from the
latest release, as the workflow carries a Mac build GitHub had no runner
for (notes say so); a test that failed stops the release. No step may run
past STEP_TIMEOUT. Nothing is signed or published.

sign runs on the Mac that holds the signing keys, by its owner, once its
login keychain is unlocked (`security unlock-keychain` at its keyboard, or
over ssh when asked for the password): the Android APKs signed again with
the release keystore (~/.openh1-android/openh1-release.jks, its password the
keychain's "OpenH1 Android keystore" item: apksigner reads it from the
environment, never a file or a log), the macOS release app signed with the
keychain's Developer ID and notarized with the App Store Connect API key
(~/.appstoreconnect/private_keys/AuthKey_<id>.p8, its issuer ID from
OPENH1_ASC_ISSUER_ID or ~/.config/openh1/asc-issuer-id), its disk image
signed for Sparkle (the keychain's openh1 key), appcast.xml, every expected
file checked (tools/release_names.py --check-release), SHA256SUMS, and its
signature by the release key (~/.config/openh1/release-signing-key, read by
tools/release_sign.py alone), checked against port/assets/release-key.pub.
Only SHA256SUMS (a few KB), the two APKs and the disk image travel to that
Mac and back; the keys never leave it.

publish uploads the signed release folder with one `gh release create
--latest` (and keeps the last five build-* releases, as the workflow does),
from the Linux machine, with the caller's gh token passed on stdin, after
--check-release and the signature's check pass. --dry-run prints what it
would upload.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REPO = "OpenH1/goath1"
GITHUB = f"https://github.com/{REPO}.git"


def machine(name: str, variable: str) -> str:
    """a machine's ssh target: the variable's, else goat-rig's hosts file's "<name> <user@host>" line
    (~/.config/goat-rig/hosts, kept on each computer and never committed), else <name>.fabric"""
    if os.environ.get(variable):
        return os.environ[variable]
    hosts = Path.home() / ".config/goat-rig/hosts"
    for line in hosts.read_text().splitlines() if hosts.is_file() else []:
        if len(line.split()) == 2 and line.split()[0] == name:
            return line.split()[1]
    return f"{name}.fabric"


# the machines: the Linux one (omarchy) and the Mac that builds the Apple platforms, Android and Linux ARM64,
# and holds the signing keys ("release-mac" in the hosts file)
LINUX = machine("omarchy", "OPENH1_RELEASE_LINUX")
MAC = machine("release-mac", "OPENH1_RELEASE_MAC")
# each machine's folder of a release, and its mirror of the repository
WORK = "goat-release"
LINUX_MIRROR = "~/goat-rig/repo.git"
MAC_MIRROR = "~/goat-build/repo.git"
# no build, test or transfer step runs longer
STEP_TIMEOUT = 45 * 60
PLATFORMS = ["linux-x86_64", "windows", "macos", "ios", "android", "linux-arm64"]
# what each platform's build leaves in dist/ (the workflow's artifacts' names)
ARTIFACTS = {
    "linux-x86_64": ["halo-linux-x86_64-debug", "halo-linux-x86_64-release", "halo-linux-x86_64-vr-release"],
    "linux-arm64": ["halo-linux-arm64-debug", "halo-linux-arm64-release", "halo-steam-frame-vr-release"],
    "windows": ["halo-windows-debug", "halo-windows-release", "halo-windows-replay"],
    "android": ["halo-android-debug", "halo-android-release"],
    "macos": ["halo-macos-debug", "halo-macos-release"],
    "ios": ["halo-ios-debug"],
}
# the published files of a platform carried over from an earlier release
CARRY_PATTERNS = {
    "linux-x86_64": ["OpenH1-Linux.zip", "OpenH1-Linux-debug.zip", "OpenH1-Linux-VR.zip",
                     "OpenH1-Linux-symbols.zip", "OpenH1-Linux-debug-symbols.zip", "OpenH1-Linux-VR-symbols.zip"],
    "linux-arm64": ["OpenH1-Linux-ARM64*.zip", "OpenH1-Steam-Frame*.zip"],
    "windows": ["OpenH1-Windows*.zip", "halo-windows-replay.zip"],
    "android": ["OpenH1-Android*"],
    "macos": ["*.dmg", "OpenH1-macOS*-symbols.zip", "appcast.xml"],
    "ios": ["halo-ios-debug.zip", "OpenH1-iOS*-symbols.zip"],
}

# the build machines' common settings (tools/ci_build.py reads them)
LINUX_CACHE_ENV = ("CCACHE_MAXSIZE=30G CCACHE_NOHASHDIR=true "
                   "CCACHE_SLOPPINESS=include_file_mtime,include_file_ctime,time_macros")
MAC_PATH = 'export PATH="/opt/homebrew/bin:$HOME/.orbstack/bin:$PATH"'
JAVA_HOME = "/Applications/Android Studio.app/Contents/jbr/Contents/Home"


def say(message: str) -> None:
    print(f"[release {time.strftime('%H:%M:%S')}] {message}", flush=True)


def remote(host: str, script: str, timeout: int = STEP_TIMEOUT, log: Path = None, check: bool = True,
) -> subprocess.CompletedProcess:
    """a bash script run on a machine (the script on stdin: no quoting through the remote's login shell, the
    5900X's fish among them)"""
    command = ["ssh", "-o", "BatchMode=yes", "-o", "ServerAliveInterval=30", host, "bash", "-s"]
    started = time.monotonic()
    try:
        result = subprocess.run(command, input="set -euo pipefail\n" + script, text=True,
                                capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        result = subprocess.CompletedProcess(command, 124, error.stdout or "", (error.stderr or "") +
                                             f"\ntimed out after {timeout} s")
    if log:
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(f"$ {host}\n{script}\n--- {time.monotonic() - started:.0f} s, exit {result.returncode}\n"
                       f"{result.stdout}\n{result.stderr}")
    if check and result.returncode:
        tail = "\n".join((result.stdout + result.stderr).splitlines()[-25:])
        raise RuntimeError(f"{host}: exit {result.returncode}{f' (log {log})' if log else ''}\n{tail}")
    return result


def checkout(mirror: str, folder: str, sha: str, seed: str = "") -> str:
    """a script: <folder> made a checkout of sha (fetched into the machine's mirror from GitHub, then cloned
    sharing its objects), its downloads (build/*/third_party) copied from <seed> when given"""
    return f"""
git -C {mirror} fetch -q {GITHUB} {sha} || git -C {mirror} fetch -q {GITHUB}
if [ ! -d {folder}/.git ]; then
  rm -rf {folder}; git clone -q --shared --no-checkout {mirror} {folder}
fi
git -C {folder} checkout -q --detach --force {sha}
git -C {folder} clean -fdxq -e build -e .cache
for third in {seed}/build/*/third_party; do
  [ -d "$third" ] || continue
  target={folder}/build/$(basename "$(dirname "$third")")/third_party
  [ -d "$target" ] || {{ mkdir -p "$(dirname "$target")"; cp -a --reflink=auto "$third" "$target"; }}
done
"""


class Release:
    def __init__(self, number: int, sha: str = ""):
        self.number = number
        self.sha = sha
        self.dir = f"~/{WORK}/build-{number}"
        self.logs = ROOT / "build" / "release-local" / f"build-{number}"

    # --- the Linux machine (omarchy)
    def sandbox(self, tree: str, commands: str) -> str:
        """a script: commands run in the CI image's file system (the self-hosted runners' sandbox,
        tools/ci-runner/host.sh, with the network: configure fetches), tree at /runner (a folder the image has: its root is read-only), the rig's compile cache
        and the guests' LLVM kept"""
        inner = f"set -euo pipefail\nexport {LINUX_CACHE_ENV}\n{commands}"
        return f"""
mkdir -p ~/goat-rig/llvm-cache {self.dir}/home
bwrap --ro-bind ~/goat-rig/ci/rootfs / --dev /dev --tmpfs /dev/shm --proc /proc --ro-bind /sys /sys \\
  --tmpfs /run --tmpfs /tmp --tmpfs /var/tmp --ro-bind /etc/resolv.conf /etc/resolv.conf \\
  --bind {tree} /runner --bind ~/goat-rig/ccache /goat-rig/ccache --bind ~/goat-rig/llvm-cache /cache/llvm \\
  --bind {self.dir}/home /home/build \\
  --unshare-user --uid 0 --gid 0 --unshare-pid --unshare-ipc --die-with-parent --new-session \\
  --clearenv --setenv HOME /home/build --setenv TMPDIR /tmp --setenv LANG C.UTF-8 \\
  --setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \\
  --setenv CI_BUILD_NUMBER {self.number} --setenv CI_COMPILER_LAUNCHER ccache \\
  --setenv CCACHE_DIR /goat-rig/ccache --setenv CCACHE_BASEDIR /runner --setenv CI_GUEST_LLVM_CACHE /cache/llvm \\
  --chdir /runner nice -n 10 bash -c {shlex.quote(inner)}
"""

    def to_artifacts(self, tree: str, names) -> str:
        """a script, on the Linux machine: dist/<name> folders (and their -symbols) of tree into artifacts/"""
        return "".join(f"""
for d in {tree}/dist/{name} {tree}/dist/{name}-symbols; do
  [ -d "$d" ] && {{ rm -rf {self.dir}/artifacts/$(basename "$d"); cp -a "$d" {self.dir}/artifacts/; }}
done""" for name in names) + "\n"

    def dlss(self) -> None:
        """the DLSS add-on of both platforms, pinned to build-N (the workflow's dlss-addon-* and dlss-pin
        jobs): pin/dlss_addon_pin.h and dlss/openh1-dlss-*; a platform that fails is left out, as there"""
        linux, windows = f"{self.dir}/src-linux", f"{self.dir}/src-windows"
        script = f"rm -rf {self.dir}/dlss {self.dir}/pin; mkdir -p {self.dir}/dlss {self.dir}/pin\n"
        script += "(" + self.sandbox(linux, """
python3 configure.py --release --dlss --guest-llvm-cache "$CI_GUEST_LLVM_CACHE"
ninja dlss_addon
python3 tools/dlss_addon.py verify build/dlss-addon/openh1-dlss-linux-x86_64-*.json
""") + f") && cp {linux}/build/dlss-addon/openh1-dlss-* {self.dir}/dlss/ || echo 'DLSS: the Linux add-on failed'\n"
        script += f"""(cd {windows} && export PATH="$HOME/goat-rig/bin:$PATH" &&
  python3 configure.py --release --dlss --guest-llvm-cache ~/goat-rig/llvm-cache {windows_cross()} &&
  nice -n 10 ninja dlss_addon &&
  python3 tools/dlss_addon.py verify build/dlss-addon/openh1-dlss-windows-x64-*.json &&
  cp build/dlss-addon/openh1-dlss-* {self.dir}/dlss/) || echo 'DLSS: the Windows add-on failed'
shopt -s nullglob
manifests=({self.dir}/dlss/openh1-dlss-linux-x86_64-*.json {self.dir}/dlss/openh1-dlss-windows-x64-*.json)
[ "${{#manifests[@]}}" -gt 0 ] || {{ echo "no add-on to pin"; exit 1; }}
python3 {linux}/tools/dlss_addon.py pin --tag build-{self.number} --out {self.dir}/pin/dlss_addon_pin.h "${{manifests[@]}}"
"""
        remote(LINUX, script, log=self.logs / "dlss.log")

    def pin(self, tree: str) -> str:
        """a script: the add-on's pin (when made) in place of the committed one, as the workflow's builds"""
        return f"[ -f {self.dir}/pin/dlss_addon_pin.h ] && cp {self.dir}/pin/dlss_addon_pin.h {tree}/port/linux/src/ || true\n"

    def linux_x86_64(self) -> None:
        tree = f"{self.dir}/src-linux"
        # (the workflow's linux-x86_64 job: debug; the build's tests and the scorecard; release; VR release)
        remote(LINUX, self.pin(tree) + f"""
[ -d {self.dir}/../scorecard-baseline ] && rm -rf {tree}/.scorecard-baseline && cp -a {self.dir}/../scorecard-baseline {tree}/.scorecard-baseline || true
""" + self.sandbox(tree, """
python3 tools/ci_build.py linux_x86_64 debug
BUILD_TESTS="$(python3 tools/ci_plan.py --all-build-tests)"
serial='' light='' parallel=''
for test in $BUILD_TESTS; do
  case "$test" in
    tools/test_profile.py|tools/test_host_units.py::*) serial="$serial $test" ;;
    tools/test_light_storage.py) light=yes ;;
    *) parallel="$parallel $test" ;;
  esac
done
python3 -c "import sys; sys.path.insert(0, '.'); from pathlib import Path; from tools.linux_guest_build import fetch_openxr_headers; fetch_openxr_headers(Path('build/linux_x86_64/third_party/openxr'))"
status=0
[ -z "$serial" ] || python3 -m pytest -q -rs $serial || status=1
[ -z "$light" ] || (PATH="$(python3 tools/guest_toolchain.py --cache "$CI_GUEST_LLVM_CACHE"):$PATH" python3 tools/test_light_storage.py) || status=1
[ -z "$parallel" ] || python3 -m pytest -q -rs -n 24 --dist loadfile $parallel || status=1
python3 tools/scorecard/ci.py || status=1
echo "$status" > /runner/build-tests.status
python3 tools/ci_build.py linux_x86_64 release
python3 tools/ci_build.py linux_x86_64 release --vr
""") + self.to_artifacts(tree, ARTIFACTS["linux-x86_64"]) + f"""
[ "$(cat {tree}/build-tests.status)" = 0 ] || {{ echo "the build's tests or the scorecard failed"; exit 3; }}
""", log=self.logs / "linux-x86_64.log")

    def tests(self) -> None:
        tree = f"{self.dir}/src-linux"
        # (the workflow's tests job, every shard's files at once, and its merge checks: the network numbers,
        # the generated menus, on the machine itself, with the PC version's inputs kept in ~/menu-inputs-check)
        remote(LINUX, self.sandbox(tree, """
PATH="$(python3 tools/guest_toolchain.py --cache "$CI_GUEST_LLVM_CACHE"):$PATH"
python3 -m pytest -q -rs -n 24 --dist loadfile $(python3 tools/ci_plan.py --all-tests)
""") + f"""
cd {tree}
python3 tools/netver.py check --final
MENU_INPUTS=~/menu-inputs-check tools/menus_regen.sh --fetch > /dev/null
~/menu-inputs-check/venv/bin/python -m pip install -q pytest
~/menu-inputs-check/venv/bin/python -m pytest -q -rs tools/test_netver.py tools/test_merge_friction.py
MENU_INPUTS=~/menu-inputs-check tools/menus_regen.sh --check
""", log=self.logs / "tests.log")

    def windows(self) -> None:
        tree = f"{self.dir}/src-windows"
        # (the workflow's windows job, cross-compiled: debug, the replay tool, the light storage's test, release;
        # its start-up check needs Windows and is the Windows VM's, not run here)
        remote(LINUX, self.pin(tree) + f"""
cd {tree}
export PATH="$HOME/goat-rig/bin:$PATH" CI_BUILD_NUMBER={self.number} CI_COMPILER_LAUNCHER=$HOME/goat-rig/bin/rig-ccache
export CI_GUEST_LLVM_CACHE=$HOME/goat-rig/llvm-cache CI_CONFIGURE_ARGS={shlex.quote(windows_cross())}
nice -n 10 python3 tools/ci_build.py windows debug
rm -rf dist/halo-windows-replay && mkdir -p dist/halo-windows-replay
cp build/windows/halo_replay.exe build/windows/halo_replay.pdb build/windows/SDL3.dll dist/halo-windows-replay/
PATH="$(python3 tools/guest_toolchain.py --cache "$CI_GUEST_LLVM_CACHE"):$PATH" python3 tools/test_light_storage.py
nice -n 10 python3 tools/ci_build.py windows release
""" + self.to_artifacts(tree, ARTIFACTS["windows"]), log=self.logs / "windows.log")

    # --- the release Mac
    def mac_build(self, platform: str) -> None:
        tree = f"{self.dir}/src-{platform}"
        env = f"""{MAC_PATH}
cd {tree}
export CI_BUILD_NUMBER={self.number} CI_COMPILER_LAUNCHER=ccache CI_GUEST_LLVM_CACHE=$HOME/{WORK}/llvm-cache
export CI_GUEST_ASM_CACHE=$HOME/{WORK}/guest-asm-{platform} ANDROID_HOME= ANDROID_SDK_ROOT= ANDROID_NDK_HOME=
"""
        if platform == "macos":
            public = sparkle_public_key()
            steps = f"""
[ -x ~/{WORK}/venv/bin/python ] || python3 -m venv ~/{WORK}/venv
~/{WORK}/venv/bin/python -m pip install -q dmgbuild==1.6.7
xcrun -sdk macosx metal --version > /dev/null
~/{WORK}/venv/bin/python tools/ci_build.py macos debug
SPARKLE_PUBLIC_KEY={shlex.quote(public)} ~/{WORK}/venv/bin/python tools/ci_build.py macos release
"""
        elif platform == "ios":
            steps = "xcrun -sdk iphoneos metal --version > /dev/null\npython3 tools/ci_build.py ios debug\n"
        elif platform == "android":
            # (signed with this Mac's debug key here; `sign` signs them again with the release keystore)
            steps = f"""
export ANDROID_HOME=$HOME/Library/Android/sdk JAVA_HOME={shlex.quote(JAVA_HOME)}
export PATH="$JAVA_HOME/bin:$PATH"
python3 tools/ci_build.py android debug
python3 tools/ci_build.py android release
python3 tools/test_touch_layout.py
python3 tools/test_mcc_metadata_importer.py
"""
        else:  # linux-arm64: the CI image, built for arm64 from the tree's own Dockerfile, in OrbStack
            steps = f"""
docker build -q -t openh1-ci-linux:ubuntu-22.04-arm64 --build-arg ARCH=arm64 .github/ci-image > /dev/null
mkdir -p ~/{WORK}/ccache-arm64 ~/{WORK}/llvm-cache-arm64
docker run --rm -v "$PWD:/work" -v "$HOME/{WORK}/ccache-arm64:/ccache" -v "$HOME/{WORK}/llvm-cache-arm64:/llvm" \\
  -w /work -e CI_BUILD_NUMBER={self.number} -e CI_COMPILER_LAUNCHER=ccache -e CCACHE_DIR=/ccache \\
  -e CCACHE_BASEDIR=/work -e CCACHE_MAXSIZE=5G -e CI_GUEST_LLVM_CACHE=/llvm -e CI_GUEST_ASM_CACHE=/work/.cache/guest-asm \\
  openh1-ci-linux:ubuntu-22.04-arm64 bash -c '
    set -euo pipefail
    python3 tools/ci_build.py linux_arm64 debug
    python3 tools/ci_build.py linux_arm64 release
    python3 tools/ci_build.py linux_arm64 release --vr'
"""
        names = ARTIFACTS[platform]
        send = " ".join(f"dist/{name} dist/{name}-symbols" for name in names)
        remote(MAC, checkout(MAC_MIRROR, tree, self.sha) + env + steps + f"""
for d in {send}; do [ -d "$d" ] && rsync -a "$d" {LINUX}:{self.dir}/artifacts/; done
""" + ("" if platform == "macos" else f"cd ~ && rm -rf {tree}\n"), log=self.logs / f"{platform}.log")

    # --- the steps
    def build(self, platforms, dlss: bool) -> dict:
        self.logs.mkdir(parents=True, exist_ok=True)
        say(f"build-{self.number}: {self.sha} on {LINUX} and {MAC}; logs in {self.logs}")
        remote(LINUX, f"rm -rf {self.dir}/release {self.dir}/status; mkdir -p {self.dir}/artifacts {self.dir}/status\n"
               + checkout(LINUX_MIRROR, f"{self.dir}/src-linux", self.sha, "~/goat-rig/builds/master")
               + checkout(LINUX_MIRROR, f"{self.dir}/src-windows", self.sha, "~/goat-rig/builds/master.win")
               + f"echo {self.sha} > {self.dir}/sha\n", log=self.logs / "checkout.log")
        if dlss and ({"linux-x86_64", "windows"} & set(platforms)):
            try:
                say("DLSS add-on: building and pinning")
                self.dlss()
            except RuntimeError as error:
                say(f"DLSS add-on: none ({str(error).splitlines()[0]}): the release offers none")
        results = {}

        def attempt(platform, step):
            say(f"{platform}: started")
            started = time.monotonic()
            try:
                step()
                results[platform] = "ok"
            except RuntimeError as error:
                code = str(error).split("exit ", 1)[-1].split()[0]
                results[platform] = "test-failed" if code == "3" or platform == "tests" else "build-failed"
                print(error, file=sys.stderr)
            say(f"{platform}: {results[platform]} ({time.monotonic() - started:.0f} s)")

        def linux_side():
            if "linux-x86_64" in platforms:
                attempt("linux-x86_64", self.linux_x86_64)
                attempt("tests", self.tests)
            if "windows" in platforms:
                attempt("windows", self.windows)

        def mac_side():
            for platform in ("macos", "ios", "android", "linux-arm64"):
                if platform in platforms:
                    attempt(platform, lambda p=platform: self.mac_build(p))

        with ThreadPoolExecutor(2) as pool:
            for future in [pool.submit(linux_side), pool.submit(mac_side)]:
                future.result()
        status = "".join(f"echo {value} > {self.dir}/status/{key}\n" for key, value in results.items())
        remote(LINUX, status)
        self.assemble(results)
        return results

    def assemble(self, results: dict) -> None:
        """the release folder from the artifacts (tools/release_package.py), the failed builds' platforms carried
        over from the latest release, the notes"""
        latest = gh(["release", "view", "--repo", REPO, "--json", "tagName", "--jq", ".tagName"]).strip()
        carried = [p for p, r in results.items() if r == "build-failed" and p in CARRY_PATTERNS]
        script = f"""
cd {self.dir}/src-linux
rm -rf {self.dir}/release
dlss=(); ls {self.dir}/dlss/openh1-dlss-* > /dev/null 2>&1 && dlss=(--dlss {self.dir}/dlss)
python3 tools/release_package.py package {self.dir}/artifacts {self.dir}/release "${{dlss[@]}}"
: > {self.dir}/carried.txt
"""
        for platform in carried:
            patterns = " ".join(f"--pattern {shlex.quote(p)}" for p in CARRY_PATTERNS[platform])
            script += f"""
rm -rf {self.dir}/carried && mkdir -p {self.dir}/carried
curl -fsSL https://api.github.com/repos/{REPO}/releases/tags/{latest} > {self.dir}/carried/release.json
python3 - {self.dir}/carried {patterns} <<'PY'
import fnmatch, json, sys, urllib.request
folder, patterns = sys.argv[1], sys.argv[2::2]
for asset in json.load(open(folder + "/release.json"))["assets"]:
    if any(fnmatch.fnmatch(asset["name"], p) for p in patterns):
        urllib.request.urlretrieve(asset["browser_download_url"], folder + "/" + asset["name"])
        print("carried", asset["name"])
PY
rm {self.dir}/carried/release.json
"""
            if platform == "macos":
                script += (f"[ -f {self.dir}/carried/appcast.xml ] && sed -i 's#/releases/download/{latest}/#"
                           f"/releases/download/build-{self.number}/#g' {self.dir}/carried/appcast.xml\n")
            script += (f"cp {self.dir}/carried/* {self.dir}/release/\n"
                       f"echo \"The {platform} build is {latest}'s: its build on our machines failed.\" >> "
                       f"{self.dir}/carried.txt\n")
        script += f"python3 tools/release_names.py --check-symbols {self.dir}/release\n"
        remote(LINUX, script, log=self.logs / "assemble.log")
        # (the notes, and the appcast's changes since the last release: as the workflow's, through the caller's gh)
        subject = git(["log", "-1", "--format=%s", self.sha]).strip()
        changes = gh(["api", f"repos/{REPO}/compare/{latest}...{self.sha}", "--jq",
                      '.commits[] | .commit.message | split("\\n") | if (.[0] | startswith("Merge pull request")) '
                      'then (map(select(length > 0)) | .[1] // empty) elif (.[0] | startswith("Merge ")) then empty '
                      'else .[0] end'], check=False)
        lines = list(dict.fromkeys(line for line in changes.splitlines() if line))
        build_url = f"local release from our own machines (tools/release_local.py), {time.strftime('%Y-%m-%d')}"
        remote(LINUX, f"""
cat > {self.dir}/notes.txt <<'NOTES_EOF'
{chr(10).join(lines)}
NOTES_EOF
cd {self.dir}/src-linux
python3 tools/release_package.py notes --subject {shlex.quote(subject)} \\
  --commit-url https://github.com/{REPO}/commit/{self.sha} --build-url {shlex.quote(build_url)} \\
  --carried {self.dir}/carried.txt > {self.dir}/notes.md
""")

    def status(self) -> int:
        result = remote(LINUX, f"""
cd {self.dir}
echo "commit: $(cat sha 2>/dev/null)"
for f in status/*; do [ -f "$f" ] && echo "$(basename "$f"): $(cat "$f")"; done
[ -s carried.txt ] && cat carried.txt
ls release 2>/dev/null | wc -l | sed 's/^ */files: /'
python3 src-linux/tools/release_names.py --check-release release || true
[ -f release/SHA256SUMS.sig ] && python3 src-linux/tools/release_sign.py verify release/SHA256SUMS && echo signature ok || echo "not signed yet"
""", check=False)
        print(result.stdout + result.stderr)
        return 0

    # --- sign: on the Mac with the keys, at its keyboard
    def sign(self) -> int:
        home = Path.home()
        tree = home / WORK / f"build-{self.number}" / "src-macos"
        work = home / WORK / f"build-{self.number}" / "signing"
        work.mkdir(parents=True, exist_ok=True)
        os.chmod(work, 0o700)
        problems = []
        if subprocess.run(["security", "show-keychain-info"], capture_output=True).returncode:
            problems.append("the login keychain is locked: run `security unlock-keychain` (it asks for this Mac's "
                            "login password), then sign again")
        issuer = os.environ.get("OPENH1_ASC_ISSUER_ID", "").strip()
        issuer_file = home / ".config/openh1/asc-issuer-id"
        if not issuer and issuer_file.is_file():
            issuer = issuer_file.read_text().strip()
        if not issuer:
            problems.append(f"no App Store Connect issuer ID: put it in {issuer_file} (App Store Connect, Users and "
                            "Access, Integrations, the Issuer ID above the keys) or OPENH1_ASC_ISSUER_ID")
        keys = sorted((home / ".appstoreconnect/private_keys").glob("AuthKey_*.p8"))
        jks = home / ".openh1-android/openh1-release.jks"
        release_key = home / ".config/openh1/release-signing-key"
        for path, what in ((keys[0] if keys else None, "the App Store Connect API key (AuthKey_<id>.p8)"),
                           (jks, "the Android release keystore"), (release_key, "the release key's seed")):
            if not path or not path.is_file():
                problems.append(f"no {what}")
        if not (tree / "build/macos/OpenH1.app").is_dir():
            problems.append(f"no macOS release app in {tree}: run build first")
        if problems:
            for problem in problems:
                print(f"sign: {problem}", file=sys.stderr)
            return 1
        tests = remote(LINUX, f"cat {self.dir}/status/linux-x86_64 {self.dir}/status/tests", check=False).stdout
        if any(word in tests for word in ("test-failed", "build-failed")) or tests.count("ok") < 2:
            print(f"sign: the tests did not pass ({' '.join(tests.split())}): nothing is signed", file=sys.stderr)
            return 1
        env = dict(os.environ, PATH=f"/opt/homebrew/bin:{os.environ.get('PATH', '')}")

        # the APKs signed again with the release keystore (its password straight from the keychain into
        # apksigner's environment)
        apksigner = sorted((home / "Library/Android/sdk/build-tools").glob("*/apksigner"))[-1]
        password = subprocess.run(["security", "find-generic-password", "-s", "OpenH1 Android keystore", "-w"],
                                  capture_output=True, text=True).stdout.rstrip("\n")
        if not password:
            print("sign: the keychain has no \"OpenH1 Android keystore\" password", file=sys.stderr)
            return 1
        sign_env = dict(env, JAVA_HOME=JAVA_HOME, OPENH1_KEYSTORE_PASSWORD=password)
        for apk in ("OpenH1-Android.apk", "OpenH1-Android-debug.apk"):
            run(["scp", "-q", f"{LINUX}:{self.dir}/release/{apk}", str(work / apk)])
            run([str(apksigner), "sign", "--ks", str(jks), "--ks-key-alias", "openh1",
                 "--ks-pass", "env:OPENH1_KEYSTORE_PASSWORD", "--key-pass", "env:OPENH1_KEYSTORE_PASSWORD",
                 str(work / apk)], env=sign_env)
            run([str(apksigner), "verify", str(work / apk)], env=sign_env)
            run(["scp", "-q", str(work / apk), f"{LINUX}:{self.dir}/release/{apk}"])
        del password, sign_env

        # the macOS release: Developer ID, notarized, its disk image signed for Sparkle
        identity = ""
        for line in subprocess.run(["security", "find-identity", "-v", "-p", "codesigning"], capture_output=True,
                                   text=True).stdout.splitlines():
            if "Developer ID Application" in line:
                identity = line.split()[1]
                break
        if not identity:
            print("sign: the keychain has no Developer ID Application identity", file=sys.stderr)
            return 1
        dmg = tree / "dist/halo-macos-release/OpenH1.dmg"
        run([sys.executable, "tools/macos_app.py", "notarize", "--app", "build/macos/OpenH1.app",
             "--output", str(dmg), "--identity", identity, "--key", str(keys[0]),
             "--key-id", keys[0].stem.split("_", 1)[1], "--issuer", issuer], cwd=tree, env=env)
        sign_update = tree / "build/macos/third_party/Sparkle/bin/sign_update"
        signature = subprocess.run([str(sign_update), "--account", "openh1", str(dmg)], capture_output=True,
                                   text=True, check=True).stdout
        (work / "OpenH1.dmg.sparkle").write_text(signature)
        run(["scp", "-q", str(dmg), f"{LINUX}:{self.dir}/release/OpenH1.dmg"])
        run(["scp", "-q", str(work / "OpenH1.dmg.sparkle"), f"{LINUX}:{self.dir}/OpenH1.dmg.sparkle"])

        # the appcast, every file checked, the sums
        tag = f"build-{self.number}"
        remote(LINUX, f"""
cd {self.dir}/src-linux
python3 tools/macos_appcast.py --build {self.number} --url https://github.com/{REPO}/releases/download/{tag}/OpenH1.dmg \\
  --signature {self.dir}/OpenH1.dmg.sparkle --notes {self.dir}/notes.txt \\
  --release-page https://github.com/{REPO}/releases/tag/{tag} --output {self.dir}/release/appcast.xml
python3 tools/release_names.py --check-release {self.dir}/release
rm -f {self.dir}/release/SHA256SUMS {self.dir}/release/SHA256SUMS.sig
python3 tools/release_sign.py sums {self.dir}/release
""")
        # the sums signed here by the release key, and checked there against port/assets/release-key.pub
        run(["scp", "-q", f"{LINUX}:{self.dir}/release/SHA256SUMS", str(work / "SHA256SUMS")])
        openssl = next((p for p in ("/opt/homebrew/bin/openssl", "/usr/local/bin/openssl") if Path(p).exists()), "")
        run([sys.executable, "tools/release_sign.py", "sign", "--key-file", str(release_key),
             str(work / "SHA256SUMS")], cwd=tree, env=dict(env, OPENSSL=openssl) if openssl else env)
        run(["scp", "-q", str(work / "SHA256SUMS.sig"), f"{LINUX}:{self.dir}/release/SHA256SUMS.sig"])
        remote(LINUX, f"cd {self.dir}/src-linux && python3 tools/release_sign.py verify {self.dir}/release/SHA256SUMS")
        for path in work.iterdir():
            path.unlink()
        work.rmdir()
        say(f"{tag} signed: publish it with `python3 tools/release_local.py publish --number {self.number}`")
        return 0

    # --- publish: one upload
    def publish(self, dry_run: bool) -> int:
        sha = remote(LINUX, f"cat {self.dir}/sha").stdout.strip()
        tag = f"build-{self.number}"
        script = f"""
cd {self.dir}/release
python3 ../src-linux/tools/release_names.py --check-release .
(cd ../src-linux && python3 tools/release_sign.py verify {self.dir}/release/SHA256SUMS)
files=(*.zip *.dmg *.apk SHA256SUMS SHA256SUMS.sig appcast.xml $(ls openh1-dlss-*.json openh1-dlss-*.txt 2> /dev/null))
"""
        create = (f'gh release create {tag} "${{files[@]}}" --repo {REPO} --target {sha} --latest '
                  f'--title "Build {self.number} ({sha[:7]})" --notes-file ../notes.md')
        if dry_run:
            print(remote(LINUX, script + f'echo would run: {shlex.quote(create)}; printf "  %s\\n" "${{files[@]}}"\n')
                  .stdout)
            return 0
        token = gh(["auth", "token"]).strip()
        # (the token on stdin's first line, never in a command line or a file)
        command = ["ssh", "-o", "BatchMode=yes", LINUX, "bash", "-s"]
        body = token + "\n" + "read -r GH_TOKEN; export GH_TOKEN\nset -euo pipefail\n"
        result = subprocess.run(command, input=body + script + create + f"""
gh release list --repo {REPO} --limit 100 --json tagName,createdAt \\
  --jq '[.[] | select(.tagName | startswith("build-"))] | sort_by(.createdAt) | reverse | .[5:] | .[].tagName' |
while read -r old; do gh release delete "$old" --repo {REPO} --cleanup-tag --yes; done
""", text=True, timeout=STEP_TIMEOUT)
        return result.returncode


def run(command, **kwargs) -> None:
    subprocess.run([str(part) for part in command], check=True, **kwargs)


def git(arguments) -> str:
    return subprocess.run(["git", *arguments], cwd=ROOT, capture_output=True, text=True, check=True).stdout


def gh(arguments, check: bool = True) -> str:
    return subprocess.run(["gh", *arguments], capture_output=True, text=True, check=check).stdout


def sparkle_public_key() -> str:
    """the public half of Sparkle's key, the repository variable SPARKLE_PUBLIC_KEY (not secret)"""
    return gh(["variable", "get", "SPARKLE_PUBLIC_KEY", "--repo", REPO]).strip()


def windows_cross() -> str:
    """configure.py's options of the Windows build cross-compiled on the Linux machine, as goat-rig build
    --windows makes them (tools/rig/goat-rig, windows_cross_options: xwin's MSVC toolchain and SDK)"""
    root = "$HOME/goat-rig/xwin/sysroot"
    return (f'--windows-cross --windows-vctools-dir "$(ls -d {root}/VC/Tools/MSVC/*/ | sort | tail -n 1)" '
            f'--windows-sdk-dir "{root}/Windows Kits/10"')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0],
                                     formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("build", "sign", "status", "publish"):
        command = commands.add_parser(name)
        command.add_argument("--number", type=int, required=True, help="the release's number, build-<N>")
        if name == "build":
            command.add_argument("--ref", default="origin/master", help="the commit (on GitHub)")
            command.add_argument("--only", default=",".join(PLATFORMS), help="platforms, by commas")
            command.add_argument("--no-dlss", action="store_true", help="no DLSS add-on")
        if name == "publish":
            command.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    release = Release(args.number)
    if args.command == "build":
        git(["fetch", "-q", GITHUB, "+refs/heads/master:refs/remotes/origin/master"])
        release.sha = git(["rev-parse", f"{args.ref}^{{commit}}"]).strip()
        platforms = [p for p in args.only.split(",") if p]
        unknown = set(platforms) - set(PLATFORMS)
        if unknown:
            parser.error(f"unknown platforms: {', '.join(sorted(unknown))} (of {', '.join(PLATFORMS)})")
        results = release.build(platforms, not args.no_dlss)
        print(json.dumps(results, indent=2))
        release.status()
        return 0 if all(value == "ok" for value in results.values()) else 1
    if args.command == "status":
        return release.status()
    if args.command == "sign":
        return release.sign()
    return release.publish(args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
