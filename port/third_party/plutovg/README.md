# plutovg

A 2D vector graphics library in C by Samuel Ugochukwu: paths, strokes,
gradients and compositing on 32-bit premultiplied ARGB surfaces.

- Upstream: https://github.com/sammycage/plutovg, release v1.3.3 (commit
  bbd91f0d06a71491691b36330f29dffa4af87ccf).
- Licence: MIT, `LICENSE` (SHA-256
  `a55d1e78eaf75779c336b4ba061e74aae56a21195d234474fb24a398935d6ce1`). Its
  scan converter and stroker (`src/plutovg-ft-*`) are FreeType's, under the
  FreeType License, `src/FTL.TXT`; its image and font loaders are stb's
  (public domain or MIT, in their headers).
- What is here: upstream's `include/` and `src/`, with one change (marked
  `(OpenH1)`): `src/plutovg-ft-raster.c` notes a full cell table with a flag
  instead of FreeType's `longjmp` (the game's guests have no `setjmp`), and
  splits the band the same way.
- What uses it: plutosvg (`port/third_party/plutosvg`), for the menus' vector
  art (`port/linux/src/menu_vector.c`), in every build's guest
  (`tools/build_common.py`, `plutosvg_sources`). The releases carry the
  licences as `plutovg-LICENSE.txt` and `plutovg-FTL.txt`
  (`tools/linux_package.py`, `tools/frame_package.py`).
