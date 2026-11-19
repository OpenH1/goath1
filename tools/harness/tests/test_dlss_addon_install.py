"""The NVIDIA DLSS add-on's install and removal (port/linux/src/dlss_addon.c, the whole file, with updater.c's zip
unpacking and the real SHA-256): an archive packed as tools/dlss_addon.py packs it is checked against its pin and put
in place; a tampered one, or one that is not the add-on's, is refused with nothing installed and nothing left half
installed; an older add-on is replaced; a removal leaves nothing; and what a crash left is deleted at the next start.
Each with negative controls.

Compiled for this computer (dlss_addon.c), as test_update_fallback.py's is, on Linux and macOS."""

import hashlib
import os
import subprocess
import zipfile
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from harness import CHECK_FAILED, HARNESS, ROOT, function, mutated, read  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_release_signature import crypto_includes  # noqa: E402

sys.path.insert(0, str(ROOT / "tools"))
import dlss_addon as packer  # noqa: E402
from test_update_fallback import zlib_objects  # noqa: E402

ZLIB = ROOT / "port/third_party/zlib"
RUNTIME = "libnvidia-ngx-dlss.so.310.9.16"
ZIP_FUNCTIONS = ["updater_local_separators", "updater_make_folders", "updater_name_inside", "zip_word", "zip_long",
                 "zip_extract_entry", "zip_extract", "updater_zip_unpack"]

# a fault in dlss_addon.c, and the case that must catch it
NEGATIVE_CONTROLS = {
    # the archive not compared with the pin: a tampered archive installed
    "hash-unchecked": (("\tif (had != size || strcmp(hex, sha256))", "\tif (0)"), "tampered"),
    # a refused install's work left behind
    "partial-kept": (("done:\n\tdlss_delete_tree(partial, 0);", "done:\n\t;"), "not-addon"),
    # the archive's files not checked: an archive without the runtime installed
    "files-unchecked": (("|| !has_module || !has_runtime || !has_license)", "|| !has_module)"), "not-addon"),
    # a removal that leaves the add-on's folder
    "remove-kept": (("\t\tif (dlss_delete_tree(removed, 0))\n\t\t\treturn 1;", "\t\treturn 1;"), "remove"),
    # the start's clean-up ignoring the removal's mark
    "mark-ignored": (("\tif (dlss_exists(mark))\n\t\tdlss_delete_tree(path, 0);", "\t(void)mark;"), "clean"),
}


def generated(fault=None):
    source = read("port/linux/src/dlss_addon.c")
    if fault:
        source = mutated(source, *NEGATIVE_CONTROLS[fault][0])
    updater = read("port/linux/src/updater.c")
    zip_code = '#include "zlib_prefixed.h"\n' + "\n\n".join(function(updater, name) for name in ZIP_FUNCTIONS)
    return dict(crypto_includes()) | {
        "src/under_test.inc": source,
        "zip_functions.inc": zip_code + "\n",
        "update.h": read("port/linux/src/update.h"),
        "src/dlss_addon.h": read("port/linux/src/dlss_addon.h"),
        "src/dlss_addon_folder.h": read("port/linux/src/dlss_addon_folder.h"),
        "src/dlss_addon_pin.h": read("port/linux/src/dlss_addon_pin.h"),
        "game/menu_table.h": read("port/linux/game/menu_table.h"),
        "SDL3/SDL.h": "", "src/platform.h": "",
    }


_built = {}


def build(tmp_path_factory, fault=None):
    if fault in _built:
        return _built[fault]
    work = tmp_path_factory.mktemp(f"dlss_addon-{fault or 'as-is'}")
    for name, text in generated(fault).items():
        (work / name).parent.mkdir(parents=True, exist_ok=True)
        (work / name).write_text(text, encoding="latin-1")
    executable = work / "dlss_addon"
    result = subprocess.run([os.environ.get("CC", "clang"), "-std=gnu99", "-O1", "-Wall", "-Werror",
                             "-Wno-unused-function", "-DDLSS_ADDON_TEST=1", "-I", str(HARNESS / "include"),
                             "-I", str(work / "src"), "-I", str(work), "-I", str(HARNESS / "tests"), "-I", str(ZLIB),
                             str(HARNESS / "tests/dlss_addon.c"), *map(str, zlib_objects(tmp_path_factory)),
                             "-o", str(executable), "-lpthread"], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"dlss_addon.c does not compile:\n{result.stderr}")
    _built[fault] = executable
    return executable


def archive(folder, files, stored=False):
    """an archive as tools/dlss_addon.py packs it (stored: its files not deflated, so that a changed byte keeps
    its size), in the download's place (dlss.partial/download.zip): its path, size and SHA-256"""
    path = folder / "dlss.partial" / "download.zip"
    path.parent.mkdir(parents=True, exist_ok=True)
    if stored:
        with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as opened:
            for name in sorted(files):
                opened.writestr(zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0)), files[name])
    else:
        packer.write_archive(path, files)
    data = path.read_bytes()
    return path, len(data), hashlib.sha256(data).hexdigest()


def addon_files():
    return {"libopenh1_dlss.so": b"module\n" * 500, RUNTIME: b"runtime\n" * 5000,
            "NVIDIA-DLSS-LICENSE.txt": b"license\n" * 50, "OPENH1-DLSS-LICENSE.txt": b"MIT\n"}


def arguments(case, folder):
    """the case's archive: the add-on's, a byte of it changed after its pin was taken, or one without the runtime"""
    files = addon_files()
    if case == "not-addon":
        del files[RUNTIME]
    path, size, digest = archive(folder, files, stored=case == "tampered")
    # (a copy for the second install of "replace", the first's download deleted with its work)
    (folder / "source.zip").write_bytes(path.read_bytes())
    if case == "tampered":
        # (a whole, readable archive of the same size whose runtime is another's: only the pin's SHA-256 tells)
        files[RUNTIME] = files[RUNTIME][:-1] + b"!"
        path, _, _ = archive(folder, files, stored=True)
    return [str(path), str(size), digest]


CASES = {"install": "install", "replace": "replace", "remove": "remove", "tampered": "refused",
         "not-addon": "refused", "clean": "clean"}


def run(executable, case, folder):
    result = subprocess.run([str(executable), CASES[case], str(folder), *arguments(case, folder)],
                            capture_output=True, text=True, errors="replace", timeout=60)
    return result.returncode, (result.stdout + result.stderr).strip()


@pytest.mark.parametrize("case", CASES)
def test_case(case, tmp_path, tmp_path_factory):
    status, output = run(build(tmp_path_factory), case, tmp_path)
    assert status == 0, output


@pytest.mark.parametrize("control", NEGATIVE_CONTROLS)
def test_negative_control(control, tmp_path, tmp_path_factory):
    case = NEGATIVE_CONTROLS[control][1]
    status, output = run(build(tmp_path_factory, control), case, tmp_path)
    assert status == CHECK_FAILED, f"dlss_addon.c with a fault ({control}) passed '{case}'\n{output}"
