/*
XBOX_MAP_FILE.H

An Xbox map read from its file, off the game's thread (xbox_map_file.c): its
2 KB header, then one zlib stream of the rest, inflated as it is read and
never held whole (as map_unpack.c reads it to copy it). Its tags are at its
end (the header says where), and what they point at (a bitmap's pages, a
sound's samples) earlier. The HD effects (hd_effects.c) and the MCC import
(mcc_import.c) read the player's own maps with it.
*/

#ifndef XBOX_MAP_FILE_H
#define XBOX_MAP_FILE_H

/* the address the tags are at in the Xbox's memory (a pointer in them less
this is its offset in the tags read) */
#define XBOX_MAP_TAGS_BASE 0x803A6000UL
/* the largest tags read */
#define XBOX_MAP_MOST_TAGS_SIZE (64UL << 20)

/* a range of the map (its offset as inflated, the header's 2 KB included)
copied out into destination */
struct xbox_map_range
{
	unsigned long offset;
	unsigned long size;
	unsigned char *destination;
};

/* Inflates the map at `path` (a host path) far enough to copy each of the
ranges; 0 if it cannot be read or ends first. */
int xbox_map_ranges_read(const char *path, struct xbox_map_range *ranges, int count);

/* The map's tags (malloc'd, *size bytes; the caller frees them), after its
header is checked (an Xbox map's, version 5); NULL if it is not one or
cannot be read. */
unsigned char *xbox_map_tags_read(const char *path, unsigned long *size);

/* the tags' bytes at the Xbox address `address` (`size` of them), or NULL
outside them */
const unsigned char *xbox_map_tags_at(const unsigned char *tags, unsigned long tags_size, unsigned long address,
	unsigned long size);

#endif
