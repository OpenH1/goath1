# libogg

Xiph.Org's libogg 1.3.5 (`libogg-1.3.5.tar.gz`, SHA-256
`0eb4b4b9420a0f51db142ba3f9c64b333f826532dc0f48c6410ae51f4799b664`), BSD licensed (`COPYING`): `src/bitwise.c`,
`src/framing.c`, `src/crctable.h` and `include/ogg/ogg.h`, `os_types.h` unmodified. `include/ogg/config_types.h` is
ours, in place of the one configure writes. The MCC import writes its dialogue as Ogg Vorbis with it
(`port/linux/src/mcc_import_voices.c`, with `port/third_party/libvorbis`).
