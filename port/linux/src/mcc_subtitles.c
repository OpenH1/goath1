/*
MCC_SUBTITLES.C

The campaign's dialogue lines from the MCC install's pack (mcc_subtitles.h).
*/

#include "mcc_subtitles.h"
#include "steam_workshop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define PAK_FILE "halo1/prebuild/paks/initial_textures.s3dpak"
#define DIALOG_PREFIX "sound\\dialog\\"
/* the entries' type of a string list */
#define STRING_LIST_TYPE 14

enum
{
	MAXIMUM_CHUNKS = 1 << 16,
	MAXIMUM_CHUNK = 16 * 1024 * 1024,
	MAXIMUM_ENTRIES = 1 << 14,
	MAXIMUM_NAME = 255,
	MAXIMUM_KEY = 255,
	MAXIMUM_LINES = 1 << 16,
};

const char *const mcc_subtitles_languages[MCC_SUBTITLES_LANGUAGES] =
{
	"english", "french", "german", "italian", "spanish", "latam_spanish", "portuguese", "japanese", "korean",
	"chinese", "chinese_simplified",
};

struct line
{
	char *key;
	unsigned short *text;
};

struct mcc_subtitles
{
	int count;
	/* sorted by key */
	struct line *lines;
};

/* the pack as read: its chunks, and the one last inflated */
struct pak
{
	const unsigned char *data;
	size_t size;
	unsigned long count;
	/* each chunk's offset in the inflated stream, and the stream's end */
	unsigned long long *starts;
	unsigned char *chunk;
	long inflated;
};

/* ---------- the pack */

static unsigned long le32(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static void refuse(char *reason, const char *text)
{
	snprintf(reason, MCC_SUBTITLES_REASON_SIZE, "%s", text);
}

static unsigned long chunk_offset(const struct pak *pak, unsigned long index)
{
	return le32(pak->data + 4 + 4 * index);
}

static int pak_open(struct pak *pak, const unsigned char *data, size_t size, char *reason)
{
	unsigned long index;

	memset(pak, 0, sizeof(*pak));
	pak->data = data;
	pak->size = size;
	pak->inflated = -1;
	if (size < 8 || size > MCC_SUBTITLES_MAXIMUM_PAK)
	{
		refuse(reason, "the pack's size is not a pack's");
		return 0;
	}
	pak->count = le32(data);
	if (pak->count < 1 || pak->count > MAXIMUM_CHUNKS || 4 + 4 * (unsigned long long)pak->count > size)
	{
		refuse(reason, "the pack's chunk table is not a pack's");
		return 0;
	}
	pak->starts = malloc(sizeof(*pak->starts) * (pak->count + 1));
	pak->chunk = malloc(MAXIMUM_CHUNK);
	if (!pak->starts || !pak->chunk)
	{
		refuse(reason, "out of memory");
		return 0;
	}
	pak->starts[0] = 0;
	for (index = 0; index < pak->count; index++)
	{
		unsigned long offset = chunk_offset(pak, index);
		unsigned long end = index + 1 < pak->count ? chunk_offset(pak, index + 1) : (unsigned long)size;
		unsigned long inflated;

		/* (each after the table and the one before it, its size and some
		stream inside the file) */
		if (offset < 4 + 4 * pak->count || end > size || offset + 4 >= end)
		{
			refuse(reason, "the pack's chunk offsets are out of order");
			return 0;
		}
		inflated = le32(data + offset);
		if (!inflated || inflated > MAXIMUM_CHUNK)
		{
			refuse(reason, "a chunk's size is not a chunk's");
			return 0;
		}
		pak->starts[index + 1] = pak->starts[index] + inflated;
	}
	return 1;
}

static void pak_close(struct pak *pak)
{
	free(pak->starts);
	free(pak->chunk);
}

static int pak_inflate(struct pak *pak, unsigned long index)
{
	unsigned long offset = chunk_offset(pak, index);
	unsigned long end = index + 1 < pak->count ? chunk_offset(pak, index + 1) : (unsigned long)pak->size;
	unsigned long inflated = (unsigned long)(pak->starts[index + 1] - pak->starts[index]);
	z_stream stream;
	int result;

	if (pak->inflated == (long)index)
		return 1;
	pak->inflated = -1;
	memset(&stream, 0, sizeof(stream));
	if (inflateInit(&stream) != Z_OK)
		return 0;
	stream.next_in = (Bytef *)(pak->data + offset + 4);
	stream.avail_in = (uInt)(end - offset - 4);
	stream.next_out = pak->chunk;
	stream.avail_out = (uInt)inflated;
	result = inflate(&stream, Z_FINISH);
	/* (the whole of its size, the stream ending there) */
	if (result != Z_STREAM_END || stream.total_out != inflated)
	{
		inflateEnd(&stream);
		return 0;
	}
	inflateEnd(&stream);
	pak->inflated = (long)index;
	return 1;
}

/* the inflated stream's bytes at an offset: 0 past its end or for a chunk
that does not inflate */
static int pak_read(struct pak *pak, unsigned long long at, void *out, unsigned long length)
{
	unsigned char *bytes = out;
	unsigned long low = 0, high = pak->count;

	if (at + length > pak->starts[pak->count])
		return 0;
	/* (the chunk that holds at) */
	while (high - low > 1)
	{
		unsigned long middle = (low + high) / 2;

		if (pak->starts[middle] <= at)
			low = middle;
		else
			high = middle;
	}
	while (length)
	{
		unsigned long long within = at - pak->starts[low];
		unsigned long long room = pak->starts[low + 1] - at;
		unsigned long take = room < length ? (unsigned long)room : length;

		if (!pak_inflate(pak, low))
			return 0;
		memcpy(bytes, pak->chunk + within, take);
		bytes += take;
		at += take;
		length -= take;
		low++;
	}
	return 1;
}

/* the language's list's offset and size, from the entry table */
static int pak_find_list(struct pak *pak, const char *language, unsigned long long *offset, unsigned long *size,
	char *reason)
{
	unsigned char word[12];
	char name[MAXIMUM_NAME + 1], wanted[64];
	unsigned long long at = 4;
	unsigned long count, index;

	if (snprintf(wanted, sizeof(wanted), "strings_%s", language) >= (int)sizeof(wanted))
	{
		refuse(reason, "no such language");
		return 0;
	}
	if (!pak_read(pak, 0, word, 4))
	{
		refuse(reason, "the pack's first chunk does not inflate");
		return 0;
	}
	count = le32(word);
	if (!count || count > MAXIMUM_ENTRIES)
	{
		refuse(reason, "the pack's entry table is not a pack's");
		return 0;
	}
	for (index = 0; index < count; index++)
	{
		unsigned long length, type, character;

		if (!pak_read(pak, at, word, 12))
		{
			refuse(reason, "the pack's entry table is cut short");
			return 0;
		}
		length = le32(word + 8);
		if (!length || length > MAXIMUM_NAME || !pak_read(pak, at + 12, name, length))
		{
			refuse(reason, "an entry's name is not a name");
			return 0;
		}
		name[length] = 0;
		for (character = 0; character < length; character++)
		{
			if ((unsigned char)name[character] < 0x20 || (unsigned char)name[character] > 0x7E)
			{
				refuse(reason, "an entry's name is not a name");
				return 0;
			}
		}
		*offset = le32(word);
		*size = le32(word + 4);
		if (!pak_read(pak, at + 12 + length, word, 4))
		{
			refuse(reason, "the pack's entry table is cut short");
			return 0;
		}
		type = le32(word);
		at += 12 + length + 12;
		if (strlen(name) != strlen(wanted) || strncmp(name, "strings_", 8))
			continue;
		for (character = 8; name[character] && (name[character] | 0x20) == (wanted[character] | 0x20); character++)
			;
		if (name[character])
			continue;
		if (type != STRING_LIST_TYPE || *size < 2 || *size > MCC_SUBTITLES_MAXIMUM_LIST || (*size & 1) ||
			*offset + *size > pak->starts[pak->count])
		{
			refuse(reason, "the language's list is not a string list");
			return 0;
		}
		return 1;
	}
	snprintf(reason, MCC_SUBTITLES_REASON_SIZE, "the pack has no %s list", wanted);
	return 0;
}

/* ---------- the list */

static int space(unsigned short character)
{
	return character == ' ' || character == '\t';
}

/* a key to its tag path: ASCII, its doubled backslashes one, '/' a backslash,
lower case; 0 for a key that is none's */
static int key_path(const unsigned short *key, unsigned long length, char *path)
{
	unsigned long index, out = 0;

	for (index = 0; index < length; index++)
	{
		unsigned short character = key[index];

		if (character <= 0x20 || character > 0x7E || out >= MAXIMUM_KEY)
			return 0;
		if (character == '/')
			character = '\\';
		if (character == '\\' && out && path[out - 1] == '\\')
			continue;
		path[out++] = (char)(character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character);
	}
	path[out] = 0;
	return out > 0;
}

static int line_compare(const void *a, const void *b)
{
	return strcmp(((const struct line *)a)->key, ((const struct line *)b)->key);
}

/* a line of the list (without its line break): its key and text, unescaped;
0 for a line that is not a dialogue line's */
static int list_line(const unsigned short *text, unsigned long length, struct line *line)
{
	char key[MAXIMUM_KEY + 1];
	unsigned short words[MCC_SUBTITLES_MAXIMUM_TEXT];
	unsigned long index = 0, start, count = 0;

	while (index < length && !space(text[index]))
		index++;
	if (!index || !key_path(text, index, key) || strncmp(key, DIALOG_PREFIX, strlen(DIALOG_PREFIX)))
		return 0;
	while (index < length && space(text[index]))
		index++;
	if (index >= length || text[index] != '"')
		return 0;
	start = ++index;
	for (; index < length && text[index] != '"'; index++)
	{
		unsigned short character = text[index];

		if (character == '\\' && index + 1 < length)
		{
			unsigned short next = text[++index];

			character = next == 'n' ? 0x0A : next == 't' ? ' ' : next == '"' || next == '\\' ? next : 0;
			if (!character)
			{
				/* (another escape: as it is written) */
				if (count < MCC_SUBTITLES_MAXIMUM_TEXT - 1)
					words[count++] = '\\';
				character = next;
			}
		}
		if (count < MCC_SUBTITLES_MAXIMUM_TEXT - 1)
			words[count++] = character;
	}
	/* (its closing quote; a line of no words, MCC's blank ones, none) */
	if (index >= length || index == start)
		return 0;
	while (count && space(words[count - 1]))
		count--;
	if (!count)
		return 0;
	line->key = malloc(strlen(key) + 1);
	line->text = malloc(sizeof(*line->text) * (count + 1));
	if (!line->key || !line->text)
	{
		free(line->key);
		free(line->text);
		return 0;
	}
	strcpy(line->key, key);
	memcpy(line->text, words, sizeof(*words) * count);
	line->text[count] = 0;
	return 1;
}

static struct mcc_subtitles *list_read(const unsigned char *bytes, unsigned long size, char *reason)
{
	struct mcc_subtitles *subtitles;
	unsigned short *text;
	unsigned long length = size / 2, index, start;
	int kept = 0, other;

	if (bytes[0] != 0xFF || bytes[1] != 0xFE)
	{
		refuse(reason, "the language's list is not UTF-16 text");
		return NULL;
	}
	text = malloc(sizeof(*text) * length);
	subtitles = calloc(1, sizeof(*subtitles));
	if (!text || !subtitles)
	{
		free(text);
		free(subtitles);
		refuse(reason, "out of memory");
		return NULL;
	}
	for (index = 0; index < length; index++)
		text[index] = (unsigned short)(bytes[2 * index] | bytes[2 * index + 1] << 8);
	for (start = 1; start < length; start = index + 1)
	{
		struct line line;
		unsigned long end;

		for (index = start; index < length && text[index] != 0x0A; index++)
			;
		end = index;
		if (end > start && text[end - 1] == 0x0D)
			end--;
		if (!list_line(text + start, end - start, &line))
			continue;
		if (subtitles->count % 256 == 0)
		{
			struct line *more;

			if (subtitles->count >= MAXIMUM_LINES ||
				!(more = realloc(subtitles->lines, sizeof(*more) * (subtitles->count + 256))))
			{
				free(line.key);
				free(line.text);
				break;
			}
			subtitles->lines = more;
		}
		subtitles->lines[subtitles->count++] = line;
	}
	free(text);
	if (!subtitles->count)
	{
		mcc_subtitles_free(subtitles);
		refuse(reason, "the language's list has no dialogue lines");
		return NULL;
	}
	qsort(subtitles->lines, subtitles->count, sizeof(*subtitles->lines), line_compare);
	/* (a key twice: its first kept) */
	for (other = 0; other < subtitles->count; other++)
	{
		if (kept && !strcmp(subtitles->lines[kept - 1].key, subtitles->lines[other].key))
		{
			free(subtitles->lines[other].key);
			free(subtitles->lines[other].text);
			continue;
		}
		subtitles->lines[kept++] = subtitles->lines[other];
	}
	subtitles->count = kept;
	return subtitles;
}

/* ---------- public code */

struct mcc_subtitles *mcc_subtitles_read(const unsigned char *data, size_t size, const char *language,
	char reason[MCC_SUBTITLES_REASON_SIZE])
{
	struct mcc_subtitles *subtitles = NULL;
	struct pak pak;
	unsigned long long offset;
	unsigned long list_size;
	unsigned char *list;

	reason[0] = 0;
	if (pak_open(&pak, data, size, reason) && pak_find_list(&pak, language, &offset, &list_size, reason))
	{
		list = malloc(list_size);
		if (!list)
			refuse(reason, "out of memory");
		else if (!pak_read(&pak, offset, list, list_size))
			refuse(reason, "the language's list does not inflate");
		else
			subtitles = list_read(list, list_size, reason);
		free(list);
	}
	pak_close(&pak);
	return subtitles;
}

struct mcc_subtitles *mcc_subtitles_read_file(const char *path, const char *language,
	char reason[MCC_SUBTITLES_REASON_SIZE])
{
	struct mcc_subtitles *subtitles;
	unsigned char *data;
	FILE *file = fopen(path, "rb");
	long size;

	if (!file)
	{
		refuse(reason, "the pack cannot be opened");
		return NULL;
	}
	if (fseek(file, 0, SEEK_END) || (size = ftell(file)) < 8 || size > MCC_SUBTITLES_MAXIMUM_PAK ||
		fseek(file, 0, SEEK_SET))
	{
		fclose(file);
		refuse(reason, "the pack's size is not a pack's");
		return NULL;
	}
	data = malloc((size_t)size);
	if (!data || fread(data, 1, (size_t)size, file) != (size_t)size)
	{
		fclose(file);
		free(data);
		refuse(reason, "the pack cannot be read");
		return NULL;
	}
	fclose(file);
	subtitles = mcc_subtitles_read(data, (size_t)size, language, reason);
	free(data);
	return subtitles;
}

int mcc_subtitles_pak_path(char *path, unsigned long size)
{
	return steam_workshop_mcc_file(PAK_FILE, path, size);
}

void mcc_subtitles_free(struct mcc_subtitles *subtitles)
{
	int index;

	if (!subtitles)
		return;
	for (index = 0; index < subtitles->count; index++)
	{
		free(subtitles->lines[index].key);
		free(subtitles->lines[index].text);
	}
	free(subtitles->lines);
	free(subtitles);
}

int mcc_subtitles_count(const struct mcc_subtitles *subtitles)
{
	return subtitles ? subtitles->count : 0;
}

const unsigned short *mcc_subtitles_find(const struct mcc_subtitles *subtitles, const char *tag_path)
{
	unsigned short wide[MAXIMUM_KEY + 1];
	char key[MAXIMUM_KEY + 1];
	struct line wanted, *found;
	unsigned long length;

	if (!subtitles || !tag_path || (length = (unsigned long)strlen(tag_path)) > MAXIMUM_KEY)
		return NULL;
	for (unsigned long index = 0; index < length; index++)
		wide[index] = (unsigned char)tag_path[index];
	if (!key_path(wide, length, key))
		return NULL;
	wanted.key = key;
	found = bsearch(&wanted, subtitles->lines, subtitles->count, sizeof(*subtitles->lines), line_compare);
	return found ? found->text : NULL;
}
