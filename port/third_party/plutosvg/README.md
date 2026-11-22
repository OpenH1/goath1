# plutosvg

A small SVG renderer in C by Samuel Ugochukwu, on plutovg's canvas
(`port/third_party/plutovg`).

- Upstream: https://github.com/sammycage/plutosvg, release v0.0.8 (commit
  fd8a080b3d0bccc41f1ae708d3785e7f08398a58).
- Licence: MIT, `LICENSE`, the same text as plutovg's (the releases carry
  that one).
- What is here: upstream's `source/plutosvg.c` and `plutosvg.h` (not its
  FreeType hooks), with OpenH1's additions in `plutosvg.c` (each marked
  `(OpenH1)`, the first commit here being upstream's as released), which
  the menu vector rebuilds' art needs and upstream lacks:
  - layers (`render_layered`): an element with a `clip-path`, a `mask` or a
    `filter`, or a group (`g`, `use`, an inner `svg`) with an `opacity`
    below 1, drawn on a layer of the canvas's size, then filtered, clipped
    and masked, and drawn with its opacity. Regions in `userSpaceOnUse`
    units are kept; `objectBoundingBox` ones (no art uses them) are taken
    as unbounded;
  - clip paths: their shapes filled by their `clip-rule`;
  - masks: luminance or (`mask-type="alpha"`) alpha;
  - filters: `feGaussianBlur` (SVG's three box blurs), `feComponentTransfer`
    (`feFuncR/G/B/A`: identity, table, discrete, linear, gamma),
    `feColorMatrix` (matrix, saturate, luminanceToAlpha) and `feMerge`, from
    `SourceGraphic`, `SourceAlpha` or the previous result, worked in sRGB;
  - a `use` of a `symbol` sized by the use's `width` and `height`.
- What uses it: the menus' vector art (`port/linux/src/menu_vector.c`), in
  every build's guest (`tools/build_common.py`, `plutosvg_sources`).
