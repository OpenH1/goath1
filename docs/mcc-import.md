# Import from MCC (design)

Issue #1257. A player who owns Halo MCC presses **IMPORT FROM MCC** in Settings, and the game builds, from their own
install, sharper weapon and vehicle textures (with normal and material maps for Enhanced graphics) and MCC's
44.1 kHz English dialogue. We ship none of MCC's data: the repo holds only numbers. The surveys are
`docs/mcc-texture-import-plan.md` and `docs/mcc-sound-import-plan.md`; the Python prototypes are
`tools/mcc_preview_bake.py` (with `tools/mcc_texture_remap.py`'s readers).

## What ships

`port/assets/mcc_import/mcc_import.json` (embedded by `tools/embed_assets.py`, as `effects.json` is), numbers and
names only, made by `tools/mcc_preview_bake.py --fits-out`:

- each MCC preview mesh (`SM_H1_AssaultRifle`, `SM_warthog`, ...), its vertex and triangle counts (a check that
  MCC's mesh is still the one the fit was made for) and the material it falls back to;
- each of our models it stands for: the model tag, the first stock map that has it, and the similarity transform
  that lines it up with MCC's mesh (scale, 3x3 rotation or mirror, shift) and the "close" distance. This replaces
  the prototype's principal-axes and ICP search, so the game does no fitting.

## The import (C, on the player's machine, no Python)

`port/linux/src/mcc_import.c` runs it on a thread of its own, started from Settings, as `hd_effects.c` makes its
effects: progress (0 to 1, with the step's name), cancel (checked between texels' rows and between tags), and a
report line at the end.

1. **Find MCC:** `paths.mcc` if set (the folder picker's choice), else `steam_workshop_mcc_file` (Steam's
   libraries: Windows registry, Linux native, Flatpak, Snap, every `libraryfolders.vdf`).
2. **Pak:** `ue4_pak.c` reads `MCC/Content/Paks/MCC-WindowsNoEditor.pak` (UE4 pak v7, not encrypted or
   compressed): the footer, then the index only. Each asset is read with `pread` at its offset, so the 11 GB file is
   never loaded; the largest read is one 2048² texture's top mip (2.7 MB). Cooked packages' names and imports,
   `Texture2D` (DXT1, DXT5, BC5) and `StaticMesh` LOD0 (positions, UVs, tangent frames, sections, indices) as the
   prototype reads them.
3. **Bake** (`mcc_import_bake.c`, the prototype's math step for step): our model's parts from the Xbox map
   (`xbox_map_file.c`); each texel of the bitmap at up to 4x (at most 2048) rasterized to its 3D point, moved by the
   shipped fit, and the closest point on MCC's mesh found (a uniform grid of its triangles, searched only within
   the "close" distance, on all cores but one); MCC's texel there for each role.
   - Diffuse: MCC's detail with our colors and levels (each channel's mean and contrast matched to ours), padded
     8 texels past each island, DXT1 (DXT5 where ours has alpha), mips to 1x1.
   - Multipurpose (Xbox channel order): our masks kept, MCC's matching map's fine detail added; change color ours.
   - `.normal.png` (MCC's normal map turned into our tangent space) and `.orm.png` (occlusion, roughness,
     metalness, with the classic look as the baseline) for Enhanced graphics' material maps.
   - **Layouts** (`"layouts"` in the JSON): bitmaps whose UV layout MCC's own textures already have, so no mesh
     and no fit. The Spartan (`characters\cyborg\cyborg`) is one: MCC's `SK_H1_Spartan` is our model, so its
     `Characters/H1/Spartans/Textures/T_cyborg_01_*` (2048²) map texel for texel onto `characters\cyborg\bitmaps\cyborg`
     at 4x. The diffuse's channels are matched to ours over our model's islands; our texels are kept on the visor
     (the color's alpha hole, which MCC paints white); our multipurpose map (with the change color) is kept; the
     DirectX normal map's green is flipped; the `.orm.png` takes the occlusion, the color's alpha as roughness and
     the metalness on the same classic baseline. MCC has no first-person arms, so the hands stay ours.
4. **Voices** (`mcc_import_voices.c`): `halo1/sound/pc/sounds_dialogs.fsb` (FSB5, CELT 0.11 at 44.1 kHz) and its
   `lst/sounds_dialogs.lst.bin` (tag path to samples), decoded with the vendored BSD CELT 0.11 decoder
   (`port/third_party/celt`), encoded as Ogg Vorbis at q6 (`port/third_party/libvorbis`; about 110 kbps mono) in `.sound` tags that keep each Xbox tag's header, pitch ranges,
   permutation names, gains and mouth data. English dialogue only (1,903 tags). Effects and music are not imported
   (the effects are byte-identical, the music no better).
5. **Output:** the data folder's `mcc_import/tags/...` (`.bitmap`, `.normal.png`, `.orm.png`, `.sound`), written
   whole or not at all (`.new` then renamed), and `mcc_import/import.note`: the importer's version, the pak's and
   the dialogue bank's sizes and times, each source map's size and CRC, and each bitmap's stock CRC.

## Drawing and playing it

Not a mod: not in the Mods list, the mods' counts, the host's mod check or reports. `mods.c` plays it as the
built-in HD effects are played, under every mod: a mod's file of the same tag wins, then the import's, then the
map's. A bitmap is drawn from the import only where the map's bitmap is the stock one it was baked from (its
pixels' CRC), and its material maps only with it. `display.mcc_import` (Settings) turns all of it on or off live;
off reads nothing. Settings also has **RE-IMPORT** (offered when the note no longer matches: MCC updated, or the
maps changed) and **REMOVE** (deletes `mcc_import`). CLEAR CACHES leaves it (it is the player's import, not a cache).

## Later: phones and Mac

They can't read MCC (no Windows install, and 11 GB is too much to copy). A desktop's `mcc_import` folder is
self-contained, so a later "export" zips it (about 90 MB of textures and 120 MB of dialogue) and the phone or Mac
imports the zip into its own data folder; nothing in the folder depends on the machine that made it. The Mac can
also point `paths.mcc` at a CrossOver or Whisky bottle.

## Milestones

(a) find MCC, read the pak, bake the AR in game; (b) every weapon and vehicle, with material maps; (c) voices;
(d) Settings: on/off, import, re-import, remove.
