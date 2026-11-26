# Importing MCC's classic Halo 1 sounds (survey)

Goal: find out whether a player's own MCC install holds Halo 1's classic sounds at higher quality than
our Xbox data, so an importer could swap them in. We'd ship nothing of MCC's. Surveyed read-only on a
current Steam install (2026-11). Our side: the Xbox maps, with 2,879 unique sound tags (823 effects,
1,903 dialogue, 153 music).

## Where the sounds are

The MCC maps don't hold the samples. Every permutation in `halo1/maps/*.map` is marked external, and
there's no `sounds.map` beside them (`maps/custom_edition/sounds.map` is the Custom Edition one). MCC
plays everything from FMOD banks in `halo1/sound/pc/` (FSB5, with name lists in `lst/*.lst.bin` that
map each tag path to a run of samples). Classic and Anniversary are told apart by the path:

| Bank | Format | Holds | Classic? |
| --- | --- | --- | --- |
| `sounds_adpcm.fsb` (88 MB) | IMA ADPCM, 22.05 kHz mono / 44.1 kHz stereo | 747 tags under `sound\old_sfx\` | Classic effects |
| `sounds_debug.fsb` (95 MB) | CELT, 44.1 kHz | the same effects under their own `sound\sfx\` names | Anniversary |
| `sounds_music.fsb` (957 MB) | CELT, 44.1 kHz, 2, 4 or 6 channels | `sound\old_music\music\` (classic) plus Anniversary music and quad ambience | Mixed, by path |
| `sounds_stream.fsb` (159 MB) | CELT, 44.1 kHz | `sound\old_music\sinomatixx\` (classic) plus Anniversary ambience | Mixed, by path |
| `sounds_dialogs*.fsb` (310 to 356 MB each) | CELT, 44.1 kHz, mostly mono | all 1,903 dialogue tags; English, French, German, Italian, Spanish | Classic (dialogue was never re-recorded) |

The `prebuild/sounds/pc/*.fsb` files (FSB4: cinematics, menus, loading music) and the `.s3dpak` files are
Anniversary only. Nothing is Ogg or Opus.

## Quality compared with ours

- **Effects: no gain.** For 721 of the 723 classic effect tags found, every permutation in
  `sounds_adpcm.fsb` is byte-for-byte the same as our Xbox ADPCM (3,013 permutations, durations 100%
  equal). The other 2 differ only slightly in size. So every weapon shot, explosion, vehicle loop,
  footstep and ambient loop on the sample list is the same file as ours. There's nothing to import.
- **Dialogue: really better (real full band), but a different master.** The permutation counts
  match on all 1,903 tags, and the lengths match on 6,093 permutations (89% within 1%, median +0.3%).
  I decoded 10 lines (Cortana, Keyes, marines, a grunt, the multiplayer announcer) and compared each
  with ours after resampling MCC's to 22.05 kHz:
  - **Energy above 11 kHz:** MCC's is -26 to -46 dB below the whole line (median about -34 dB), with
    content up to 15 to 19 kHz. That band moves with the speech: its loudness tracks the 4 to 10 kHz
    band at 0.75 to 0.97, so it's real sibilance and breath. An upsample of our 22 kHz files would
    have nothing there (below -80 dB). Ours can't hold anything above 11 kHz.
  - **Same line, not the same file:** the spectrograms match at 0.68 to 0.81. The waveforms match
    well on 1 line of 10 (r 0.98) and on 2 more after stretch correction (0.86). The others drift
    against ours by 0.03% to 1.3%, enough that their waveform correlation falls to about 0 to 0.56. So
    MCC's are the same takes from a separate 44.1 kHz master, not made from our data. Lip-sync data
    made for ours may slip by a frame or two on long lines.
  - **Noise floor:** both are clean, and MCC's is the same or 1 to 5 dB lower (the 5th-percentile frame
    against the loudest frame, full band and 4 to 10 kHz).
- **Music: the same recording, no clear gain.** 4 cues (`haloperc`, `battle2_full`, `spooky1`,
  `nightmare`), both 44.1 kHz stereo:
  - **Same recording:** r 0.94 to 0.9997 after a 0.01% to 0.09% stretch correction, and the
    spectrograms match at 0.94 to 0.99.
  - **Energy above 16 kHz:** MCC's is lower, -49 to -86 dB against our -42 to -58 dB. MCC's encoder cuts
    off around 16 kHz, and our extra top end is mostly ADPCM hiss.
  - **Noise floor:** within 1 dB on 3 cues; MCC's is 8 dB lower in the 4 to 10 kHz band on `spooky1`.
    The difference between the two versions is 10 to 32 dB below the music.
  - At best MCC's is slightly cleaner and duller. It isn't worth an importer on its own. The 37
    `sinomatixx` cutscene tracks weren't checked; they're probably the same case.

## Coverage

- Effects: 723 of 823 have a classic copy (63 are missing, such as the Warthog engine set and some
  `sinomatixx_foley`), but none is better quality: 0%.
- Dialogue: 1,903 of 1,903 (100%) are better: the same takes with real sound up to about 18 kHz.
- Music: 153 of 153 (100%) are there, plus 37 cutscene tracks, but with no clear gain.
- Languages: all four other languages cover all 1,903 tags, but 28% of their tags have a different
  permutation count from English. Our data is English, so they would be a language swap with their own
  permutation lists, not a quality swap.

## How it would plug in

- It needs conversion. A mod `.sound` file (see `docs/mods.md`) can carry 44 kHz PCM or Ogg. The importer
  would copy each Xbox tag's header, pitch ranges, permutation names, gains and mouth data, decode CELT,
  and write the samples as 16-bit PCM or Ogg. Music has to be cut back into our pieces at the Xbox
  piece lengths, which match to within about 0.05%.
- The engine keeps the gain. `mod_sounds.c` plays mod samples as 16-bit PCM at their own rate (up to
  the mixer's 48 kHz) and decodes them from the file as they load. Long permutations are split into
  linked pieces. Only a permutation that doesn't fit falls back to Xbox ADPCM. So 44.1 kHz mono
  dialogue and stereo music would play as written, with no engine change.
- Size: English dialogue is 330 MB as CELT (ours is 126 MB). As Ogg at about 96 kbps mono it's
  roughly 120 MB per language. Classic music is about 1.7 hours (ours is 263 MB ADPCM), roughly 140 MB as
  Ogg at about 190 kbps. Dialogue alone, the part worth importing, comes to about 120 MB for English.
- Finding the install: the same Steam library search as the texture plan, then
  `halo1/sound/pc/` and its `lst/` folder.
