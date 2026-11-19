/*
MCC_SUBTITLES.H

The campaign's dialogue lines, read from the player's own MCC install
(display.subtitles, #1217): nothing of MCC's is in the repository or the
release. MCC keeps them in halo1/prebuild/paks/initial_textures.s3dpak, a
Saber pack:

- the file: a u32 count of chunks, then that many u32 offsets of them (the
  table padded after them); each chunk a u32 size inflated, then a zlib
  stream;
- the chunks inflated, one after another: a u32 count of entries, then each
  entry's u32 offset (in the inflated stream), u32 size, u32 length of its
  name, the name, a u32 type and 8 zero bytes;
- the entries strings_<language> (type 14; English's name is
  strings_English): UTF-16LE text with a byte order mark, a line each, the
  key, tabs, then the text in quotes (\" and \n its escapes). The dialogue's
  keys are the sounds' tag paths, their backslashes doubled
  (sound\\dialog\\a10\\a10_010_cryotech); no timing, no speaker.

Only the chunks that hold the entry table and the language's list are
inflated. Every read is bounds-checked: a pack not of this layout (another
MCC update's, a damaged file) is refused with its reason, never guessed at.
*/

#ifndef __HALO_LINUX_MCC_SUBTITLES_H
#define __HALO_LINUX_MCC_SUBTITLES_H

#include <stddef.h>

enum
{
	MCC_SUBTITLES_LANGUAGES = 11,
	/* the largest pack read, entry, line */
	MCC_SUBTITLES_MAXIMUM_PAK = 256 * 1024 * 1024,
	MCC_SUBTITLES_MAXIMUM_LIST = 16 * 1024 * 1024,
	MCC_SUBTITLES_MAXIMUM_TEXT = 1024,
	MCC_SUBTITLES_REASON_SIZE = 128,
};

/* MCC's languages, as display.subtitles_language and the entries' names have
them (strings_<name>, case-blind) */
extern const char *const mcc_subtitles_languages[MCC_SUBTITLES_LANGUAGES];

struct mcc_subtitles;

/* the language's dialogue lines from a pack's bytes; NULL and the reason
(always NUL terminated) when it cannot be read */
struct mcc_subtitles *mcc_subtitles_read(const unsigned char *data, size_t size, const char *language,
	char reason[MCC_SUBTITLES_REASON_SIZE]);
/* ... from the pack at a host path */
struct mcc_subtitles *mcc_subtitles_read_file(const char *path, const char *language,
	char reason[MCC_SUBTITLES_REASON_SIZE]);
/* the pack in the MCC Steam install (steam_workshop_mcc_file): 1 and its host
path, or 0 */
int mcc_subtitles_pak_path(char *path, unsigned long size);
void mcc_subtitles_free(struct mcc_subtitles *subtitles);

int mcc_subtitles_count(const struct mcc_subtitles *subtitles);
/* a sound's line (UTF-16, NUL terminated; a line break as 0x0A) by its tag
path (sound\dialog\..., case-blind, '/' or '\' between its parts); NULL
for none */
const unsigned short *mcc_subtitles_find(const struct mcc_subtitles *subtitles, const char *tag_path);

#endif
