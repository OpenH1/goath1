# Importing MCC's high-res weapon and vehicle textures (plan)

> The survey below stands. Its remap plan does not: matching islands by painted detail failed (MCC
> repainted them), so the importer bakes from MCC's preview meshes instead. See `docs/mcc-import.md`.

Goal: one button, "Import HD textures from MCC", builds `mods/mcc_hd_textures/` from the
player's own MCC install. We ship no MCC data. Surveyed read-only on a current Steam
install (2026-11).

## Answers

1. **Where they live.** Not in Halo 1's own files. `halo1/maps/*.map`, `bitmaps.map` and the
   classic `hce_*` copies in `halo1/prebuild/paks/*.ipak` hold only the classic sizes
   (512 at most, 12.9 MB for all weapons and vehicles). The `wpn_*`/`vhc_*` textures in
   the ipaks are Anniversary art for the Anniversary models. The high-res set is in the
   **Unreal pak** `MCC/Content/Paks/MCC-WindowsNoEditor.pak` (UE4 pak v7, not encrypted,
   not compressed). It's under `MCC/Content/UI/Features/Customization/SharedTextures/H1/{Weapons,Vehicles}/`,
   as `H1S2_{WS,VS}_<OBJECT>[_<PART>]_<SKIN>[_<MAP>]` (`.uasset`/`.uexp`, with the top mips in
   `.ubulk`). These are the menu's 3D customization previews. Skins are folded into the
   name: the `DEFAULT` token is the stock look, and every other token (`BRUTAL`, `ZEALOT`,
   `DESERT`, `HOTROD` and so on) is an alternate skin. Each skin has a diffuse map, a
   `_MULTIPURPOSE` map, and PBR maps (`_NORMAL`, `_METALLIC`, `_ROUGHNESS`, `_AO`,
   `_EMISSIVE`). There are 881 textures in all (2.2 GB).
2. **One-to-one.** They aren't 4K: the largest are 2048², mostly `PF_DXT1`. That format maps
   straight onto our DXT1, so no re-encode is needed. Mips 2048 to 128 are in `.ubulk`, and
   the smaller ones are in `.uexp`. Their names don't match the tags, so the importer needs
   a fixed map from texture name to tag path. Every weapon and vehicle has a counterpart,
   found by whole-image correlation against every classic bitmap (best against the
   runner-up). Weapons: Sniper 0.88, Pistol 0.75, Shotgun 0.74, Needler 0.72, AR fp 0.68,
   Rocket Launcher 0.65, Plasma Rifle 0.56, Fuel Rod 0.57, Plasma Pistol 0.51,
   Flamethrower 0.44; the runner-up is always 0.29 or less. Vehicles: Warthog 0.64,
   Scorpion 0.66, Ghost 0.48, Banshee 0.40. That makes about 70 diffuse+multipurpose
   files.
   - **Correction:** an earlier note said the weapons don't match. That came from a bug
     that read the 64² mip from past the end of `.ubulk`.
   - **None of them drop in as they are.** Each is the classic atlas with many islands
     moved and resized. I checked each 32-pixel block of the classic texture for its best
     match within ±48 px of the same spot in the MCC one, both at 512²:

     | Texture | In place | Shifted | No match nearby |
     | --- | --- | --- | --- |
     | Warthog | 23% | 30% | 45% |
     | Ghost | 7% | 42% | 50% |
     | AR | 12% | 54% | 32% |
     | Pistol | 28% | 43% | 27% |

     A straight swap would put about half of each model's surfaces on the wrong part of
     the picture. The side-by-side Warthog picture shows the same-shaped pieces moved and
     resized.
   - The classic tags have 1 bitmap and 0 or 1 sequence, so the counts work.
   - There's no high-res set for the Pelican, Dropship, Longsword, lifepod or turrets.
3. **Size.** The default set is about 46 MB for vehicles and roughly 90 MB with the weapons, as DXT1 with mips. The largest file is a
   2048² DXT1 at 2.7 MB, well under every platform's per-bitmap limit (16 to 64 MB), so
   Android and iOS can take it as it is. If memory is tight, drop the top mip.
4. **Shaders.** The layouts match, so the classic `shader_model` settings (map scales, detail
   maps) still apply. The PBR and emissive maps have nowhere to go in a classic shader, so
   we leave them out, and that part of the preview look is lost. **Open:** the channel
   order of the `_MULTIPURPOSE` maps (Xbox or PC). Check each channel against the classic
   map, then set `channel_order` to match.
5. **Finding the install.** Reuse `port/linux/src/steam_workshop.c`'s Steam library search
   (Windows, Linux native/Flatpak/Snap, `libraryfolders.vdf`), then
   `steamapps/common/Halo The Master Chief Collection/MCC/Content/Paks/`.
   - Mac: no native MCC, so check CrossOver/Whisky bottles and offer a folder picker.
   - Android: let the player pick the `.pak` (11 GB), or a small file a desktop
     import has already exported. Copying 11 GB to a phone is unrealistic, so the
     export-from-desktop route is better.

## Remapping to our models' layouts

Every texture needs a remap, including the Assault Rifle. In the Warthog picture the
islands keep their shapes and are only moved and resized. So the cheap route should work
for most islands, and the bake route covers what it can't.

- **Route A: an island recipe we derive offline.** Rasterize our model's UV triangles (the
  Xbox `mod2` we already load) into island masks. For each island, fit a scale, a shift
  and one of 8 orientations into the MCC texture by normalized cross-correlation. We ship
  a recipe of only numbers per texture, no pixels. At import, the game decodes DXT1
  (bcdec), warps each island into a 2048² canvas in our layout, pads the edges and
  re-encodes DXT1 (needs stb_dxt).
  - Work: about 2 to 3 days for the fitting tool and recipes, plus 1 day for the importer's
    warp.
  - Import time: a few seconds per texture on desktop, 1 to 2 minutes for all 70, longer
    on phones.
  - Risk: an island that was re-cut rather than moved fails the fit, and is flagged for
    Route B.
- **Route B: a bake from the preview mesh.** Parse the cooked Unreal meshes in the pak
  (`UI/Features/Customization/Characters/H1/Weapons/SM_H1_<Weapon>.uexp`, a static mesh
  of about 230 KB for the AR; the vehicles are `SK_*` skeletal meshes). That means the
  versioned LOD serialization (positions, UV buffer in half or full floats, indices).
  Then line the preview mesh up with our model, and for each of our triangles find the
  matching preview triangle, giving one affine map per triangle. That's still a
  numbers-only recipe we derive offline, and the import is the same warp as Route A.
  - Work: about 1 week (mesh parser 3 to 4 days, alignment and matching 2 to 3 days).
  - Import time: as Route A. Baking at import instead (rasterizing per texel) would be
    10 to 30 s per texture on desktop and isn't needed.
- **Recommendation:** Route A first for all 70 textures. Use Route B only for the islands
  Route A flags. Both keep the import a plain warp, so no Unreal parsing ships in the game.

## Proposed flow

1. **Detect:** Steam libraries, then the pak. Check the pak's footer and version, and refuse
   anything unknown.
2. **Read:** parse the pak index. For each entry in the fixed name map, read the `.uexp`
   (size, format, small mips) and the `.ubulk` (big mips).
3. **Convert:** decode DXT1, warp the islands by the shipped recipe, re-encode DXT1 with
   mips, plus the multipurpose channel fix if needed.
4. **Write the mod:** HEK `.bitmap` files that take the classic tag's bitmap, sequence and
   flags and swap in the new pixels. They go under `mods/mcc_hd_textures/tags/` with a
   `mod.toml`, and each file runs through the `mod_bitmaps.c` checks before it's written.
5. **Menu:** a button on the MCC settings or packages screen, with progress, "N textures
   imported", and "Remove".

## Risks

- **The layouts differ:** every texture needs the island remap, and recipe quality decides
  how it looks. Check a few weapons and vehicles in game before shipping.
- MCC patches can rename or move the assets. The name map is versioned, and missing
  entries are skipped and reported.
- A redraw doesn't line up texel for texel. Correlation shows the same layout, but check a
  few vehicles in game before shipping.
- Files go only into the player's own mods folder, and nothing is ever uploaded or shared.

## Steps

1. Name map: Unreal texture name to classic tag path, for about 70 weapon and vehicle
   files, plus the multipurpose channel check.
2. Island recipe tool (offline, Route A), with Route B for the islands it flags.
3. UE4 pak and Texture2D reader (index, `.uexp` header, mips in `.ubulk`), as a C library
   with no UI.
4. Bitmap tag writer: an HEK `.bitmap` built from the classic tag plus the new pixels,
   round-trip checked by `mod_bitmaps.c`.
5. Importer core plus a console command (`mcc_import_textures`), then a check in game on
   omarchy.
6. Desktop menu button, and the Mac folder picker.
7. Android: import a set exported on the desktop through `MccImportActivity`.
