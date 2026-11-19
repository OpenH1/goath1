"""The MCC subtitles' reader (port/linux/src/mcc_subtitles.c) against a pack
made here in MCC's layout (no MCC text), and damaged or missing packs."""
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "port/linux/src"
ZLIB = ROOT / "port/third_party/zlib"

DRIVER = r'''
#include "mcc_subtitles.h"
#include <stdio.h>
#include <string.h>
int steam_workshop_mcc_file(const char *relative, char *path, unsigned long size) {
 (void)relative; (void)path; (void)size; return 0;
}
static void utf8(const unsigned short *text) {
 for (; *text; text++) {
  unsigned c = *text;
  if (c == 0x0A) printf("\\n");
  else if (c < 0x80) putchar((int)c);
  else if (c < 0x800) { putchar(0xC0 | c >> 6); putchar(0x80 | (c & 63)); }
  else { putchar(0xE0 | c >> 12); putchar(0x80 | (c >> 6 & 63)); putchar(0x80 | (c & 63)); }
 }
}
int main(int argc, char **argv) {
 char reason[MCC_SUBTITLES_REASON_SIZE];
 struct mcc_subtitles *lines;
 int i;
 if (argc < 3) return 2;
 lines = mcc_subtitles_read_file(argv[1], argv[2], reason);
 if (!lines) { printf("ERR %s\n", reason); return 0; }
 printf("%d\n", mcc_subtitles_count(lines));
 for (i = 3; i < argc; i++) {
  const unsigned short *text = mcc_subtitles_find(lines, argv[i]);
  if (text) utf8(text); else printf("(none)");
  printf("\n");
 }
 mcc_subtitles_free(lines);
 return 0;
}
'''

ENGLISH = "\r\n".join([
    'test_string\t\t\t\t"test"',
    'UI_ACCEPT\t\t\t\t"Accept"',
    '"CTRL_Mouse btn 3"\t\t\t\t"Mouse Button 3"',
    'sound\\\\dialog\\\\t10\\\\t10_010_alpha\t\t\t\t"First line, said plainly."',
    'sound\\\\dialog\\\\t10\\\\t10_020_beta\t\t\t\t"A \\"quoted\\" word,\\nthen a second line."',
    'SOUND\\\\DIALOG\\\\T10\\\\T10_030_GAMMA\t\t\t\t"Upper case key — and a dash."',
    'sound\\\\dialog\\\\t10\\\\t10_040_blank\t\t\t\t""',
    'sound\\\\dialog\\\\t10\\\\t10_050_open\t\t\t\t"no closing quote',
    'sound\\\\dialog\\\\x10\\\\x10_01_delta\t"One tab only."',
    'sound\\\\dialog\\\\t10\\\\t10_010_alpha\t\t\t\t"A second line of the same key."',
]) + "\r\n"
FRENCH = "\r\n".join([
    'sound\\\\dialog\\\\t10\\\\t10_010_alpha\t\t\t\t"Première réplique."',
]) + "\r\n"


def string_list(text):
    return b"\xff\xfe" + text.encode("utf-16-le")


def pak(entries, chunk=256):
    """MCC's layout: the entry table and the entries' bytes, inflated, cut into
    chunks of `chunk` bytes, each its size and a zlib stream after a table of
    their offsets (padded, as MCC's is)"""
    table_size = 4 + sum(12 + len(name) + 12 for name, _, _ in entries)
    body, table, at = b"", struct.pack("<I", len(entries)), table_size
    for name, kind, data in entries:
        table += struct.pack("<III", at, len(data), len(name)) + name.encode() + struct.pack("<I", kind) + bytes(8)
        body += data
        at += len(data)
    stream = table + body
    chunks = [stream[i:i + chunk] for i in range(0, len(stream), chunk)]
    header = 4 + 4 * len(chunks) + 64
    offsets, blobs, offset = [], b"", header
    for part in chunks:
        blob = struct.pack("<I", len(part)) + zlib.compress(part)
        offsets.append(offset)
        blobs += blob
        offset += len(blob)
    head = struct.pack("<I", len(chunks)) + b"".join(struct.pack("<I", o) for o in offsets)
    return head + bytes(header - len(head)) + blobs


def good_pak():
    return pak([("TexturesInfo", 5, b"\x01" * 300), ("strings_English", 14, string_list(ENGLISH)),
                ("strings_french", 14, string_list(FRENCH))])


@pytest.fixture(scope="module")
def reader(tmp_path_factory):
    compiler = os.environ.get("CC") or shutil.which("clang") or shutil.which("cc")
    assert compiler, "C compiler required"
    work = tmp_path_factory.mktemp("mcc-subtitles")
    (work / "driver.c").write_text(DRIVER)
    windows = sys.platform == "win32"
    flags = ["--target=i686-pc-windows-msvc", "-fuse-ld=lld", "-D_CRT_SECURE_NO_WARNINGS"] if windows else \
        ["-D_GNU_SOURCE"]
    # (where the compiler has them: the reads checked byte by byte)
    if os.environ.get("MCC_SUBTITLES_SANITIZE"):
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]
    exe = work / ("reader.exe" if windows else "reader")
    sources = [work / "driver.c", SRC / "mcc_subtitles.c", *sorted(ZLIB.glob("*.c"))]
    result = subprocess.run([compiler, *flags, "-std=gnu11", "-Wall", "-DZ_PREFIX", "-I", str(SRC), "-I",
                             str(ZLIB), *map(str, sources), "-o", str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    yield exe, work
    # (this module's own tool and packs)
    shutil.rmtree(work)


def read(reader, data, language, *keys, name="pak"):
    exe, work = reader
    path = work / name
    if data is not None:
        path.write_bytes(data)
    result = subprocess.run([str(exe), str(path), language, *keys], capture_output=True, timeout=30)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return result.stdout.decode().splitlines()


def test_reads_dialogue_lines(reader):
    lines = read(reader, good_pak(), "english", "sound\\dialog\\t10\\t10_010_alpha",
                 "SOUND/Dialog/t10/t10_020_beta", "sound\\dialog\\t10\\t10_030_gamma",
                 "sound\\dialog\\x10\\x10_01_delta", "sound\\dialog\\t10\\t10_040_blank",
                 "sound\\dialog\\t10\\t10_050_open", "ui_accept", "sound\\dialog\\t10\\missing")
    # (alpha, beta, gamma, delta: not the UI's keys, the blank line, the line
    # cut short; a key twice, its first)
    assert lines[0] == "4"
    assert lines[1:] == ["First line, said plainly.", 'A "quoted" word,\\nthen a second line.',
                         "Upper case key — and a dash.", "One tab only.", "(none)", "(none)", "(none)",
                         "(none)"]


def test_other_language_and_chunking(reader):
    # (chunks of 64: the table and the lists across many of them)
    lines = read(reader, pak([("strings_English", 14, string_list(ENGLISH)),
                              ("strings_french", 14, string_list(FRENCH))], chunk=64),
                 "french", "sound\\dialog\\t10\\t10_010_alpha")
    assert lines == ["1", "Première réplique."]


def test_refuses_what_is_not_the_layout(reader):
    data = good_pak()
    assert read(reader, data, "korean") == ["ERR the pack has no strings_korean list"]
    assert read(reader, None, "english", name="no such pak") == ["ERR the pack cannot be opened"]
    # (cut short anywhere, a byte of a stream changed, a count or an offset
    # not a pack's, a list that is not UTF-16: refused, never read past)
    for cut in range(0, len(data), 97):
        assert read(reader, data[:cut], "english")[0].startswith("ERR"), cut
    damaged = bytearray(data)
    damaged[struct.unpack_from("<I", data, 4)[0] + 8] ^= 0xFF
    assert read(reader, bytes(damaged), "english")[0].startswith("ERR")
    assert read(reader, struct.pack("<I", 0x7FFFFFFF) + data[4:], "english") == \
        ["ERR the pack's chunk table is not a pack's"]
    swapped = bytearray(data)
    swapped[4:8], swapped[8:12] = data[8:12], data[4:8]
    assert read(reader, bytes(swapped), "english") == ["ERR the pack's chunk offsets are out of order"]
    assert read(reader, pak([("strings_English", 14, ENGLISH.encode("utf-16-le"))]), "english") == \
        ["ERR the language's list is not UTF-16 text"]
    assert read(reader, pak([("strings_English", 3, string_list(ENGLISH))]), "english") == \
        ["ERR the language's list is not a string list"]
    assert read(reader, pak([("strings_English", 14, string_list("UI_ACCEPT\t\"Accept\"\r\n"))]), "english") == \
        ["ERR the language's list has no dialogue lines"]
