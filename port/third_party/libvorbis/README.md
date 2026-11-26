# libvorbis

Xiph.Org's libvorbis 1.3.7 (`libvorbis-1.3.7.tar.gz`, SHA-256
`0e982409a9c3fc82ee06e08205b1355e5c6aa4c36bca58146ef399621b0ce5ab`), BSD licensed (`COPYING`): `lib/` (its encoder and
the codec it needs, without the `barkmel.c`, `psytune.c` and `tone.c` tools and without `vorbisfile.c`), `lib/books`,
`lib/modes` and `include/vorbis/codec.h`, `vorbisenc.h`, unmodified. The MCC import encodes MCC's dialogue with it
(`port/linux/src/mcc_import_voices.c`); the game plays Ogg Vorbis with stb_vorbis.
