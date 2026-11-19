"""The NVIDIA DLSS add-on's packing (tools/dlss_addon.py): the archive and its manifest, the same bytes from the same
files, a tampered archive refused (the negative control), the pin the game is built with, and the game's packages
free of NVIDIA's files. Needs no build: the files are stand-ins.

    python -m pytest -q tools/test_dlss_addon.py
"""

import json
import sys
import zipfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import dlss_addon  # noqa: E402
import linux_package  # noqa: E402

RUNTIME = "libnvidia-ngx-dlss.so.310.9.16"


@pytest.fixture
def build(tmp_path):
    folder = tmp_path / "build"
    folder.mkdir()
    for name in ("libopenh1_dlss.so", RUNTIME, "NVIDIA-DLSS-LICENSE.txt"):
        (folder / name).write_bytes(f"stand-in {name}\n".encode() * 100)
    return folder


def packed(build, out, version="310.9.16-test"):
    return dlss_addon.pack("linux-x86_64", build / "libopenh1_dlss.so", build / RUNTIME,
                           build / "NVIDIA-DLSS-LICENSE.txt", out, version)


def test_archive_and_manifest(build, tmp_path):
    manifest_path = packed(build, tmp_path / "out")
    manifest = json.loads(manifest_path.read_text())
    archive = manifest_path.with_name(manifest["file"])
    assert archive.name == "openh1-dlss-linux-x86_64-310.9.16-test.zip"
    assert manifest["size"] == archive.stat().st_size
    assert manifest["sha256"] == dlss_addon.sha256(archive.read_bytes())
    with zipfile.ZipFile(archive) as opened:
        assert sorted(opened.namelist()) == sorted(["libopenh1_dlss.so", RUNTIME, "NVIDIA-DLSS-LICENSE.txt",
                                                    "OPENH1-DLSS-LICENSE.txt"])
        assert b"MIT License" in opened.read("OPENH1-DLSS-LICENSE.txt")
        assert (opened.getinfo("libopenh1_dlss.so").external_attr >> 16) & 0o777 == 0o755
    assert (tmp_path / "out" / manifest["license"]["file"]).read_bytes() == \
        (build / "NVIDIA-DLSS-LICENSE.txt").read_bytes()
    assert dlss_addon.verify(manifest_path) == []


def test_same_files_same_bytes(build, tmp_path):
    first = packed(build, tmp_path / "a")
    second = packed(build, tmp_path / "b")
    assert json.loads(first.read_text())["sha256"] == json.loads(second.read_text())["sha256"]


def test_tampered_archive_refused(build, tmp_path):
    """the negative control: one byte of the archive changed, and verify says so"""
    manifest_path = packed(build, tmp_path / "out")
    archive = manifest_path.with_name(json.loads(manifest_path.read_text())["file"])
    data = bytearray(archive.read_bytes())
    data[40] ^= 0xFF
    archive.write_bytes(bytes(data))
    assert any("SHA-256" in problem for problem in dlss_addon.verify(manifest_path))


def test_wrong_module_refused(build, tmp_path):
    with pytest.raises(ValueError):
        dlss_addon.pack("windows-x64", build / "libopenh1_dlss.so", build / RUNTIME,
                        build / "NVIDIA-DLSS-LICENSE.txt", tmp_path / "out", "x")


def test_pin(build, tmp_path):
    manifest = json.loads(packed(build, tmp_path / "out").read_text())
    text = dlss_addon.pin_text([manifest], "build-1400")
    assert '#define DLSS_ADDON_VERSION "310.9.16-test"' in text
    assert ('#define DLSS_ADDON_URL_LINUX_X86_64 "https://github.com/OpenH1/goath1/releases/download/build-1400/'
            'openh1-dlss-linux-x86_64-310.9.16-test.zip"') in text
    assert f'#define DLSS_ADDON_SHA256_LINUX_X86_64 "{manifest["sha256"]}"' in text
    assert f'#define DLSS_ADDON_SIZE_LINUX_X86_64 {manifest["size"]}ULL' in text
    # (a platform not packed: none pinned)
    assert '#define DLSS_ADDON_SHA256_WINDOWS_X64 ""' in text


def test_committed_pin_pins_nothing():
    """the repository's pin is the empty one (a release's workflow writes its own)"""
    assert dlss_addon.PIN.read_text() == dlss_addon.pin_text([], "none")


def test_package_without_nvidia_files(build, tmp_path):
    """the Linux release's tree from a --dlss build's folder: none of NVIDIA's files in it"""
    for name in ("halo", "libSDL3.so.0", "openh1.assets"):
        (build / name).write_bytes(name.encode())
    tree = tmp_path / "tree"
    linux_package.stage_tree(tree, build / "halo", build / "libSDL3.so.0", linux_package.notices("linux"))
    assert dlss_addon.check_package(tree) == []


def test_package_check_finds_nvidia_files(tmp_path):
    """the negative control: a package with the runtime, the module or the add-on's zip in it is refused"""
    tree = tmp_path / "tree"
    (tree / "lib/openh1").mkdir(parents=True)
    (tree / "bin").mkdir()
    (tree / "lib/openh1" / RUNTIME).write_bytes(b"x")
    (tree / "bin/openh1_dlss.dll").write_bytes(b"x")
    with zipfile.ZipFile(tree / "inner.zip", "w") as archive:
        archive.writestr("nvngx_dlss.dll", b"x")
    found = dlss_addon.check_package(tree)
    assert f"lib/openh1/{RUNTIME}" in found and "bin/openh1_dlss.dll" in found and "inner.zip:nvngx_dlss.dll" in found
