/*
MCC_IMPORT_VOICES.H

MCC's English dialogue imported as mod-style sound tag files (docs/mcc-import.md, step 4): the player's own
halo1/sound/pc/sounds_dialogs.fsb (FSB5, CELT 0.11) and lst/sounds_dialogs.lst.bin, decoded and written as Ogg Vorbis (from 16-bit
PCM, libvorbis at q6) .sound files that keep each of our Xbox tags' header, pitch ranges, permutation names, gains and mouth data.
Runs on the import's thread (mcc_import.c); reads only, except under out_folder.
*/

#ifndef MCC_IMPORT_VOICES_H
#define MCC_IMPORT_VOICES_H

#include <stddef.h>

/* the import's progress and cancel (mcc_import.c's): progress is called now and then with 0 to 1, and returns
nonzero to cancel */
typedef int (*mcc_import_progress_proc)(void *context, float fraction);

/* Imports the dialogue: mcc_root is the install's folder (host path, the one holding halo1/), map_paths the
player's Xbox maps (host paths, the stock campaign and multiplayer maps; each sound tag is taken from the first
that has it), out_folder the import's folder (its tags/ is written). Returns how many .sound files were written,
or -1 if canceled or the bank cannot be read; report (report_size bytes) says what happened in a line. */
long mcc_import_voices(const char *mcc_root, const char *const *map_paths, int map_count, const char *out_folder,
	mcc_import_progress_proc progress, void *context, char *report, size_t report_size);

/* the files the voices are made from (relative to mcc_root): their sizes and times go in the import's note */
#define MCC_IMPORT_VOICES_BANK "halo1/sound/pc/sounds_dialogs.fsb"
#define MCC_IMPORT_VOICES_LIST "halo1/sound/pc/lst/sounds_dialogs.lst.bin"

#endif
