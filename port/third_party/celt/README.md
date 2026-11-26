# CELT 0.11.0

The library sources of Xiph's [CELT](https://downloads.xiph.org/releases/celt/) 0.11.0
(`celt-0.11.0.tar.gz`, SHA-256
`c94d4d34f5a2caa1574b1a94869202cacd959b55f643a8bafe0660008acad9c3`), unmodified, for the MCC import's
dialogue (`port/linux/src/mcc_import_voices.c`): MCC's FMOD banks are CELT 0.11.0 (FMOD's bitstream version
0x80000010), and 0.11.1 decodes them differently (its allocation and folding changed). BSD licence: `COPYING`.

Only `libcelt/`'s library files are kept, without the tests, tools, `testcelt.c`, `dump_modes.c`, `header.c`,
the fixed-point DSP headers and the static modes (FMOD's mode, 44.1 kHz with 512-sample frames, is a custom
one). `celt0110_names.h` is ours: it renames each of the library's external names with the prefix `celt0110_`,
so that they do not meet Opus's CELT (`port/third_party/opus`); the build includes it first in each file
(`celt_sources` and `celt_cflags` in `tools/build_common.py`), floating point, with `CUSTOM_MODES`,
`CUSTOM_MODES_ONLY`, `ENABLE_POSTFILTER`, `VAR_ARRAYS`, `HAVE_LRINTF` and `HAVE_LRINT`.
