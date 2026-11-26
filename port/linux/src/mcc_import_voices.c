/*
MCC_IMPORT_VOICES.C

MCC's English dialogue imported as mod-style sound tag files (mcc_import_voices.h; docs/mcc-import.md, step 4).

The bank, halo1/sound/pc/sounds_dialogs.fsb, is FMOD's FSB5: a 0x3C-byte header (0x40 in version 0), a header of
each sample (8 bytes packed: its frames, data offset, channels and rate, then chunks: loop points and the like),
a table of names (only each tag's last part, not its permutations'), then the samples' data. Each sample is FMOD's
CELT: frames of 512 samples at 44.1 kHz, each an 8-byte header (0x17C30DF3 big-endian, then its size
little-endian) and a CELT 0.11.0 packet (port/third_party/celt). It is read with pread, one sample at a time.

Its list, lst/sounds_dialogs.lst.bin, is little-endian: a version (1) and a count, then for each tag its path's
length (with its terminating zero), the path (sound\dialog\...), the index of its first sample in the bank and how
many there are: one for each of our tag's permutations, in the tag's order (its pitch ranges' in turn).

Each tag of the list is taken from the first of the player's Xbox maps that has it (xbox_map_file.h), and written
as out_folder/tags/<its path>.sound: a Halo PC tag file (big-endian, version 4) as mod_sounds.c reads it, with the
Xbox tag's values, pitch ranges, permutations (names, gains, links), mouth and subtitle data, and only its samples
swapped: MCC's, decoded, in the Xbox tag's channels at 44.1 kHz, encoded as Ogg Vorbis (libvorbis's VBR at quality
MCC_IMPORT_VORBIS_QUALITY, about 110 kbps mono; port/third_party/libvorbis), each permutation one stream whose
sample_buffer_size is its 16-bit PCM's bytes, as Halo PC's tool writes one. A permutation too long for a sound
cache block is left whole: mod_sounds.c splits it into linked pieces as it loads (range_split). Each file is
written whole or not at all (a .new beside it, renamed over it). One tag's samples are held at a time.
*/

#include "mcc_import_voices.h"
#include "posix.h"
#include "xbox_map_file.h"

#include "../../third_party/celt/celt0110_names.h"
#include "../../third_party/celt/celt.h"
#include "../../third_party/libvorbis/include/vorbis/vorbisenc.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---------- constants */

#define FSB5_MAGIC 0x35425346UL /* "FSB5" */
#define FSB5_CODEC_CELT 0x0C
#define FSB5_MOST_SAMPLES 65536
#define FSB5_MOST_HEADERS_SIZE (16UL << 20)
/* FMOD's CELT: its frames' sync word, their samples, its rate, the largest packet a frame has (CELT's) */
#define FSB_CELT_SYNC 0x17C30DF3UL
#define FSB_CELT_FRAME 512
#define FSB_CELT_RATE 44100
#define FSB_CELT_MOST_PACKET 1275
/* the most a sample's data or frames may be (a sound tag file holds 64 MB: mods.h's MAXIMUM_MOD_TAG_FILE_SIZE) */
#define MOST_SAMPLE_BYTES (16UL << 20)
#define MOST_SAMPLE_FRAMES (16L << 20)

#define LIST_VERSION 1
#define LIST_MOST_SIZE (16UL << 20)
#define LIST_MOST_TAGS 16384
#define LIST_MOST_PATH 255

/* the Xbox map's tags (the game's tag_groups.h and sound_definitions.h, 32-bit and little-endian) */
#define TAG_INSTANCE_SIZE 0x20
#define GROUP_SOUND 0x736E6421UL /* "snd!" */
#define SOUND_SIZE 0xA4
#define PITCH_RANGE_SIZE 0x48
#define PERMUTATION_SIZE 0x7C
#define MOST_PITCH_RANGES 8
#define MOST_PERMUTATIONS 256
#define MOST_MOUTH_DATA 8192
#define MOST_SUBTITLE_DATA 512

/* a Halo PC tag file's (mods.h's) */
#define TAG_FILE_HEADER_SIZE 0x40
#define TAG_FILE_SOUND_VERSION 4
#define TAG_FILE_MOST_SIZE (64UL << 20)
#define SOUND_SAMPLE_RATE_44K 1
#define SOUND_COMPRESSION_NONE 0
#define SOUND_COMPRESSION_OGG_VORBIS 3
/* libvorbis's VBR quality (-0.1 to 1: 0.6 is oggenc's -q6) */
#define MCC_IMPORT_VORBIS_QUALITY 0.6f
#define MOST_TAG_PERMUTATIONS (MOST_PITCH_RANGES * MOST_PERMUTATIONS)

#define MOST_PATH 1024

/* ---------- structures */

struct bank_sample
{
	unsigned long offset;
	unsigned long size;
	long frames;
	int channels;
};

struct bank
{
	int descriptor;
	long count;
	struct bank_sample *samples;
};

struct list_tag
{
	char *path;
	unsigned long first;
	unsigned long count;
	int done;
};

/* what the run did, for the report */
struct tally
{
	long written;
	long skipped;
	unsigned long long bytes;
	/* the first reason a tag was skipped, and its tag */
	const char *reason;
	char reason_tag[LIST_MOST_PATH + 1];
};

/* ---------- bytes */

static unsigned long word_le(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static unsigned long word_be(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] << 24 | (unsigned long)bytes[1] << 16 | (unsigned long)bytes[2] << 8 |
		(unsigned long)bytes[3];
}

static unsigned int half_le(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8;
}

static void put_be32(unsigned char *at, unsigned long value)
{
	at[0] = (unsigned char)(value >> 24);
	at[1] = (unsigned char)(value >> 16);
	at[2] = (unsigned char)(value >> 8);
	at[3] = (unsigned char)value;
}

static void put_be16(unsigned char *at, unsigned int value)
{
	at[0] = (unsigned char)(value >> 8);
	at[1] = (unsigned char)value;
}

/* `count` little-endian values of `size` bytes from `in` written big-endian at `out` */
static void swap_into(unsigned char *out, const unsigned char *in, int size, int count)
{
	int index, byte;

	for (index = 0; index < count; index++)
		for (byte = 0; byte < size; byte++)
			out[index * size + byte] = in[index * size + size - 1 - byte];
}

static int read_at(int descriptor, void *buffer, unsigned long size, unsigned long offset)
{
	unsigned char *at = buffer;

	while (size)
	{
		ssize_t got = pread(descriptor, at, size, (off_t)offset);

		if (got <= 0)
			return 0;
		at += got;
		size -= (unsigned long)got;
		offset += (unsigned long)got;
	}
	return 1;
}

/* ---------- the bank */

static void bank_close(struct bank *bank)
{
	if (bank->descriptor >= 0)
		close(bank->descriptor);
	free(bank->samples);
	bank->descriptor = -1;
	bank->samples = NULL;
}

/* The bank's samples' places, frames and channels (a sample of other than 1 or 2 channels has 0): 0 if it is
not an FSB5 bank of CELT or cannot be read. */
static int bank_open(const char *path, struct bank *bank)
{
	unsigned char header[0x40];
	unsigned char *headers = NULL;
	unsigned long version, headers_size, names_size, data_size, header_size, data_offset, at;
	long index;

	memset(bank, 0, sizeof(*bank));
	bank->descriptor = open(path, O_RDONLY | O_CLOEXEC);
	if (bank->descriptor < 0 || !read_at(bank->descriptor, header, sizeof(header), 0) ||
		word_le(header) != FSB5_MAGIC)
	{
		goto refused;
	}
	version = word_le(header + 4);
	bank->count = (long)word_le(header + 8);
	headers_size = word_le(header + 0xC);
	names_size = word_le(header + 0x10);
	data_size = word_le(header + 0x14);
	header_size = version == 0 ? 0x40 : 0x3C;
	if (version > 1 || word_le(header + 0x18) != FSB5_CODEC_CELT || bank->count < 1 ||
		bank->count > FSB5_MOST_SAMPLES || headers_size > FSB5_MOST_HEADERS_SIZE ||
		names_size > FSB5_MOST_HEADERS_SIZE || (unsigned long)bank->count * 8 > headers_size)
	{
		goto refused;
	}
	data_offset = header_size + headers_size + names_size;
	headers = malloc(headers_size);
	bank->samples = calloc((size_t)bank->count, sizeof(*bank->samples));
	if (!headers || !bank->samples || !read_at(bank->descriptor, headers, headers_size, header_size))
		goto refused;
	at = 0;
	for (index = 0; index < bank->count; index++)
	{
		struct bank_sample *sample = &bank->samples[index];
		unsigned long low, high;
		int more;

		if (at + 8 > headers_size)
			goto refused;
		low = word_le(headers + at);
		high = word_le(headers + at + 4);
		at += 8;
		/* (bit 0 more chunks, 1-4 rate, 5-6 channels, 7-33 offset in 32 bytes, 34-63 frames) */
		sample->frames = (long)(high >> 2);
		sample->offset = ((low >> 7) | (high & 3) << 25) << 5;
		sample->channels = ((low >> 5) & 3) == 0 ? 1 : ((low >> 5) & 3) == 1 ? 2 : 0;
		more = (int)(low & 1);
		while (more)
		{
			unsigned long chunk, size;

			if (at + 4 > headers_size)
				goto refused;
			chunk = word_le(headers + at);
			size = (chunk >> 1) & 0xFFFFFF;
			more = (int)(chunk & 1);
			if (size > headers_size - at - 4)
				goto refused;
			/* (a channel count of its own) */
			if ((chunk >> 25) == 1 && size >= 1)
				sample->channels = headers[at + 4] == 1 ? 1 : headers[at + 4] == 2 ? 2 : 0;
			at += 4 + size;
		}
		if (sample->offset > data_size || (index && sample->offset < bank->samples[index - 1].offset))
			goto refused;
	}
	/* (each sample's data runs to the next's, the last to the end) */
	for (index = 0; index < bank->count; index++)
	{
		struct bank_sample *sample = &bank->samples[index];
		unsigned long end = index + 1 < bank->count ? bank->samples[index + 1].offset : data_size;

		sample->size = end - sample->offset;
		sample->offset += data_offset;
	}
	free(headers);
	return 1;

refused:
	free(headers);
	bank_close(bank);
	return 0;
}

/* The bank's sample `index` decoded into `pcm` (frames * channels samples, the tag's `channels`, from the
sample's own: a mono one fills both, a stereo one is averaged for mono), by `mode`; its data read into `data`
(grown as needed). 0 if it cannot be read or decoded. */
static int sample_decode(struct bank *bank, long index, CELTMode *mode, int channels, short *pcm,
	unsigned char **data, unsigned long *data_capacity)
{
	const struct bank_sample *sample = &bank->samples[index];
	short frame[FSB_CELT_FRAME * 2];
	CELTDecoder *decoder;
	unsigned long at = 0;
	long done = 0;
	int error = 0;

	if (!sample->channels || sample->size > MOST_SAMPLE_BYTES)
		return 0;
	if (sample->size > *data_capacity)
	{
		unsigned char *grown = realloc(*data, sample->size);

		if (!grown)
			return 0;
		*data = grown;
		*data_capacity = sample->size;
	}
	if (!read_at(bank->descriptor, *data, sample->size, sample->offset))
		return 0;
	decoder = celt_decoder_create_custom(mode, sample->channels, &error);
	if (!decoder || error != CELT_OK)
	{
		if (decoder)
			celt_decoder_destroy(decoder);
		return 0;
	}
	while (done < sample->frames)
	{
		unsigned long size;
		long count, frame_index;

		if (sample->size - at < 8 || word_be(*data + at) != FSB_CELT_SYNC)
			break;
		size = word_le(*data + at + 4);
		if (size < 1 || size > FSB_CELT_MOST_PACKET || size > sample->size - at - 8 ||
			celt_decode(decoder, *data + at + 8, (int)size, frame, FSB_CELT_FRAME) != CELT_OK)
		{
			break;
		}
		at += 8 + size;
		count = sample->frames - done < FSB_CELT_FRAME ? sample->frames - done : FSB_CELT_FRAME;
		for (frame_index = 0; frame_index < count; frame_index++)
		{
			short *out = pcm + (done + frame_index) * channels;

			if (sample->channels == channels)
				memcpy(out, frame + frame_index * channels, (size_t)channels * sizeof(short));
			else if (channels == 1)
				out[0] = (short)((frame[2 * frame_index] + frame[2 * frame_index + 1]) / 2);
			else
				out[0] = out[1] = frame[frame_index];
		}
		done += count;
	}
	celt_decoder_destroy(decoder);
	return done == sample->frames;
}

/* ---------- the Ogg Vorbis encoder */

struct ogg_out
{
	unsigned char *data;
	unsigned long size;
	unsigned long capacity;
	int failed;
};

static void ogg_append(struct ogg_out *out, const unsigned char *bytes, long count)
{
	if (out->failed || count <= 0)
		return;
	if (out->size + (unsigned long)count > out->capacity)
	{
		unsigned long capacity = (out->size + (unsigned long)count) * 2;
		unsigned char *grown = realloc(out->data, capacity);

		if (!grown)
		{
			out->failed = 1;
			return;
		}
		out->data = grown;
		out->capacity = capacity;
	}
	memcpy(out->data + out->size, bytes, (size_t)count);
	out->size += (unsigned long)count;
}

static void ogg_pages(struct ogg_out *out, ogg_stream_state *stream, int flush)
{
	ogg_page page;

	while (flush ? ogg_stream_flush(stream, &page) : ogg_stream_pageout(stream, &page))
	{
		ogg_append(out, page.header, page.header_len);
		ogg_append(out, page.body, page.body_len);
	}
}

/* `frames` of 16-bit PCM in `channels` at 44.1 kHz as one Ogg Vorbis stream (malloc'd, *size bytes); NULL if it
cannot be encoded */
static unsigned char *vorbis_encode(const short *pcm, long frames, int channels, long serial, unsigned long *size)
{
	vorbis_info info;
	vorbis_comment comment;
	vorbis_dsp_state dsp;
	vorbis_block block;
	ogg_stream_state stream;
	ogg_packet header, header_comment, header_code, packet;
	struct ogg_out out;
	long done = 0;
	int started = 0, ended = 0;

	memset(&out, 0, sizeof(out));
	vorbis_info_init(&info);
	if (vorbis_encode_init_vbr(&info, channels, FSB_CELT_RATE, MCC_IMPORT_VORBIS_QUALITY) != 0)
	{
		vorbis_info_clear(&info);
		return NULL;
	}
	vorbis_comment_init(&comment);
	vorbis_analysis_init(&dsp, &info);
	vorbis_block_init(&dsp, &block);
	ogg_stream_init(&stream, (int)serial);
	started = 1;
	vorbis_analysis_headerout(&dsp, &comment, &header, &header_comment, &header_code);
	ogg_stream_packetin(&stream, &header);
	ogg_stream_packetin(&stream, &header_comment);
	ogg_stream_packetin(&stream, &header_code);
	/* (the headers on pages of their own, as the specification asks) */
	ogg_pages(&out, &stream, 1);
	while (!ended && !out.failed)
	{
		long count = frames - done < 1024 ? frames - done : 1024, frame;

		if (count > 0)
		{
			float **buffer = vorbis_analysis_buffer(&dsp, (int)count);
			int channel;

			for (frame = 0; frame < count; frame++)
				for (channel = 0; channel < channels; channel++)
					buffer[channel][frame] = pcm[(done + frame) * channels + channel] / 32768.f;
			vorbis_analysis_wrote(&dsp, (int)count);
			done += count;
		}
		else
		{
			/* (the end of the stream) */
			vorbis_analysis_wrote(&dsp, 0);
		}
		while (vorbis_analysis_blockout(&dsp, &block) == 1)
		{
			vorbis_analysis(&block, NULL);
			vorbis_bitrate_addblock(&block);
			while (vorbis_bitrate_flushpacket(&dsp, &packet))
			{
				ogg_stream_packetin(&stream, &packet);
				ogg_pages(&out, &stream, 0);
			}
		}
		if (count <= 0)
			ended = 1;
	}
	ogg_pages(&out, &stream, 1);
	if (started)
	{
		ogg_stream_clear(&stream);
		vorbis_block_clear(&block);
		vorbis_dsp_clear(&dsp);
		vorbis_comment_clear(&comment);
	}
	vorbis_info_clear(&info);
	if (out.failed || !out.size)
	{
		free(out.data);
		return NULL;
	}
	*size = out.size;
	return out.data;
}

/* ---------- the list */

static void list_free(struct list_tag *tags, long count)
{
	long index;

	if (!tags)
		return;
	for (index = 0; index < count; index++)
		free(tags[index].path);
	free(tags);
}

/* (by path, without case) */
static int list_tag_compare(const void *a, const void *b)
{
	return strcasecmp(((const struct list_tag *)a)->path, ((const struct list_tag *)b)->path);
}

/* The list's tags, sorted by path (*count of them), each a run within the bank's `samples`: NULL if it is not
one or cannot be read. */
static struct list_tag *list_read(const char *path, long samples, long *count)
{
	struct list_tag *tags = NULL;
	unsigned char *list = NULL;
	unsigned long size = 0, at = 8;
	long index = 0;
	FILE *file = fopen(path, "rb");

	*count = 0;
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0)
	{
		long length = ftell(file);

		if (length >= 8 && (unsigned long)length <= LIST_MOST_SIZE && fseek(file, 0, SEEK_SET) == 0 &&
			(list = malloc((size_t)length)) != NULL && fread(list, 1, (size_t)length, file) == (size_t)length)
		{
			size = (unsigned long)length;
		}
	}
	fclose(file);
	if (!size || word_le(list) != LIST_VERSION || word_le(list + 4) < 1 || word_le(list + 4) > LIST_MOST_TAGS)
		goto refused;
	*count = (long)word_le(list + 4);
	if (!(tags = calloc((size_t)*count, sizeof(*tags))))
		goto refused;
	for (index = 0; index < *count; index++)
	{
		unsigned long length;

		if (size - at < 4)
			goto refused;
		length = word_le(list + at);
		at += 4;
		if (length < 2 || length > LIST_MOST_PATH + 1 || size - at < length + 8 || list[at + length - 1] ||
			memchr(list + at, 0, length - 1))
		{
			goto refused;
		}
		if (!(tags[index].path = malloc(length)))
			goto refused;
		memcpy(tags[index].path, list + at, length);
		at += length;
		tags[index].first = word_le(list + at);
		tags[index].count = word_le(list + at + 4);
		at += 8;
		if (tags[index].count < 1 || tags[index].count > MOST_PITCH_RANGES * MOST_PERMUTATIONS ||
			tags[index].first > (unsigned long)samples || tags[index].count > (unsigned long)samples - tags[index].first)
		{
			goto refused;
		}
	}
	if (at != size)
		goto refused;
	free(list);
	qsort(tags, (size_t)*count, sizeof(*tags), list_tag_compare);
	return tags;

refused:
	free(list);
	list_free(tags, *count);
	*count = 0;
	return NULL;
}

static struct list_tag *list_find(struct list_tag *tags, long count, const char *path)
{
	struct list_tag key;

	key.path = (char *)path;
	return bsearch(&key, tags, (size_t)count, sizeof(*tags), list_tag_compare);
}

/* ---------- the tag file */

/* whether the tag path is relative and stays within the folder (no drive, no parent, no empty part) */
static int path_safe(const char *path)
{
	const char *at;

	for (at = path; *at; at++)
	{
		int separator = *at == '\\' || *at == '/';

		if (*at == ':' || (separator && (at == path || !at[1] || at[1] == '\\' || at[1] == '/')) ||
			(*at == '.' && at[1] == '.' && (at == path || at[-1] == '\\' || at[-1] == '/')))
		{
			return 0;
		}
	}
	return at != path;
}

/* out_folder/tags/<tag path, its separators made />.sound, and each folder on the way made */
static int file_path(const char *out_folder, const char *tag, char *path, size_t size)
{
	size_t start, index;

	if (snprintf(path, size, "%s/tags/", out_folder) >= (int)size)
		return 0;
	start = strlen(path);
	if (start + strlen(tag) + sizeof(".sound") + 4 > size)
		return 0;
	for (index = 0; index < start; index++)
		if (path[index] == '/' && index)
		{
			path[index] = 0;
			posix_make_directory(path);
			path[index] = '/';
		}
	for (index = 0; tag[index]; index++)
	{
		if (tag[index] == '\\' || tag[index] == '/')
		{
			path[start + index] = 0;
			posix_make_directory(path);
			path[start + index] = '/';
		}
		else
			path[start + index] = tag[index];
	}
	strcpy(path + start + index, ".sound");
	return 1;
}

/* a file written whole or not at all (a .new beside it, renamed over it) */
static int file_write(const char *path, const void *data, unsigned long size)
{
	char temporary[MOST_PATH + 8];
	FILE *file;
	int written;

	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	if (!file)
		return 0;
	written = fwrite(data, 1, size, file) == size;
	written = fclose(file) == 0 && written;
	/* (Windows renames over nothing) */
	if (written)
	{
		remove(path);
		written = rename(temporary, path) == 0;
	}
	if (!written)
		remove(temporary);
	return written;
}

/* One tag written as its tag file, from the Xbox sound at `root` in `tags` and the list's run of samples: NULL
if it was, else why not. */
static const char *tag_write(const unsigned char *tags, unsigned long tags_size, const unsigned char *root,
	const struct list_tag *entry, struct bank *bank, CELTMode *mode, const char *out_folder,
	unsigned char **data, unsigned long *data_capacity, unsigned long *written_size)
{
	const unsigned char *ranges, *range_permutations[MOST_PITCH_RANGES];
	const unsigned char *promotion = NULL;
	unsigned long range_count, promotion_length, permutation_total = 0, size, cursor, range_index, index;
	unsigned long sample = entry->first;
	unsigned char **encoded = NULL;
	unsigned long *encoded_sizes = NULL;
	unsigned char *file = NULL;
	char path[MOST_PATH];
	int channels = half_le(root + 0x6C) == 0 ? 1 : 2;
	const char *reason = NULL;

	/* the Xbox tag's pitch ranges, permutations and their data, and the file's size */
	range_count = word_le(root + 0x98);
	if (half_le(root + 0x6C) > 1 || range_count < 1 || range_count > MOST_PITCH_RANGES ||
		!(ranges = xbox_map_tags_at(tags, tags_size, word_le(root + 0x9C), range_count * PITCH_RANGE_SIZE)))
	{
		return "its Xbox tag's pitch ranges cannot be read";
	}
	promotion_length = word_le(root + 0x78);
	if (promotion_length && (promotion_length > LIST_MOST_PATH ||
		!(promotion = xbox_map_tags_at(tags, tags_size, word_le(root + 0x74), promotion_length + 1)) ||
		promotion[promotion_length] || memchr(promotion, 0, promotion_length)))
	{
		promotion_length = 0;
	}
	size = TAG_FILE_HEADER_SIZE + SOUND_SIZE + (promotion_length ? promotion_length + 1 : 0) +
		range_count * PITCH_RANGE_SIZE;
	for (range_index = 0; range_index < range_count; range_index++)
	{
		const unsigned char *range = ranges + range_index * PITCH_RANGE_SIZE;
		unsigned long count = word_le(range + 0x3C), index;

		if (count < 1 || count > MOST_PERMUTATIONS ||
			!(range_permutations[range_index] = xbox_map_tags_at(tags, tags_size, word_le(range + 0x40),
				count * PERMUTATION_SIZE)))
		{
			return "its Xbox tag's permutations cannot be read";
		}
		for (index = 0; index < count; index++)
		{
			const unsigned char *permutation = range_permutations[range_index] + index * PERMUTATION_SIZE;
			unsigned long mouth = word_le(permutation + 0x54), subtitle = word_le(permutation + 0x68);
			long frames;

			if (permutation_total + index >= entry->count)
				return "MCC has fewer permutations";
			frames = bank->samples[sample + permutation_total + index].frames;
			if (mouth > MOST_MOUTH_DATA || subtitle > MOST_SUBTITLE_DATA ||
				(mouth && !xbox_map_tags_at(tags, tags_size, word_le(permutation + 0x60), mouth)) ||
				(subtitle && !xbox_map_tags_at(tags, tags_size, word_le(permutation + 0x74), subtitle)))
			{
				return "its Xbox tag's mouth or subtitle data cannot be read";
			}
			if (frames < 1 || frames > MOST_SAMPLE_FRAMES)
				return "an MCC sample is empty or too long";
			size += PERMUTATION_SIZE + mouth + subtitle;
		}
		permutation_total += count;
	}
	if (permutation_total != entry->count)
		return "MCC has more permutations";
	if (!path_safe(entry->path) || !file_path(out_folder, entry->path, path, sizeof(path)))
		return "its path cannot be written";
	/* each permutation's samples decoded and encoded, in the file's order */
	if (!(encoded = calloc(permutation_total, sizeof(*encoded))) ||
		!(encoded_sizes = calloc(permutation_total, sizeof(*encoded_sizes))))
	{
		reason = "out of memory";
		goto finished;
	}
	for (index = 0; index < permutation_total && !reason; index++)
	{
		unsigned long frames = (unsigned long)bank->samples[sample + index].frames;
		short *pcm = malloc(frames * (unsigned long)channels * sizeof(short));

		if (!pcm)
			reason = "out of memory";
		else if (!sample_decode(bank, (long)(sample + index), mode, channels, pcm, data, data_capacity))
			reason = "an MCC sample cannot be decoded";
		else if (!(encoded[index] = vorbis_encode(pcm, (long)frames, channels, (long)(sample + index),
			&encoded_sizes[index])))
		{
			reason = "an MCC sample cannot be encoded";
		}
		free(pcm);
		if (!reason)
		{
			size += encoded_sizes[index];
			if (size > TAG_FILE_MOST_SIZE)
				reason = "it is longer than a sound tag file holds";
		}
	}
	if (reason)
		goto finished;
	if (!(file = calloc(1, size)))
	{
		reason = "out of memory";
		goto finished;
	}

	/* the header, and the sound's values, the Xbox tag's (little-endian) written big-endian: 16-bit PCM at
	44.1 kHz, its promotion sound by name, its pointers and counters none */
	memcpy(file + 0x24, "snd!", 4);
	put_be32(file + 0x2C, TAG_FILE_HEADER_SIZE);
	put_be16(file + 0x38, TAG_FILE_SOUND_VERSION);
	put_be16(file + 0x3A, 0xFF);
	memcpy(file + 0x3C, "blam", 4);
	cursor = TAG_FILE_HEADER_SIZE;
	swap_into(file + cursor, root, 4, 1);
	swap_into(file + cursor + 4, root + 4, 2, 2);
	put_be16(file + cursor + 6, SOUND_SAMPLE_RATE_44K);
	swap_into(file + cursor + 0x08, root + 0x08, 4, 10);
	swap_into(file + cursor + 0x3C, root + 0x3C, 4, 3);
	swap_into(file + cursor + 0x54, root + 0x54, 4, 3);
	swap_into(file + cursor + 0x6C, root + 0x6C, 2, 1);
	put_be16(file + cursor + 0x6E, SOUND_COMPRESSION_OGG_VORBIS);
	memcpy(file + cursor + 0x70, "snd!", 4);
	put_be32(file + cursor + 0x78, promotion_length);
	put_be32(file + cursor + 0x7C, 0xFFFFFFFFUL);
	swap_into(file + cursor + 0x80, root + 0x80, 2, 2);
	swap_into(file + cursor + 0x84, root + 0x84, 4, 1);
	put_be32(file + cursor + 0x98, range_count);
	cursor += SOUND_SIZE;
	if (promotion_length)
	{
		memcpy(file + cursor, promotion, promotion_length + 1);
		cursor += promotion_length + 1;
	}
	/* the pitch ranges */
	for (range_index = 0; range_index < range_count; range_index++)
	{
		const unsigned char *range = ranges + range_index * PITCH_RANGE_SIZE;

		memcpy(file + cursor, range, 32);
		file[cursor + 31] = 0;
		swap_into(file + cursor + 0x20, range + 0x20, 4, 3);
		swap_into(file + cursor + 0x2C, range + 0x2C, 2, 1);
		swap_into(file + cursor + 0x30, range + 0x30, 4, 1);
		put_be32(file + cursor + 0x3C, word_le(range + 0x3C));
		cursor += PITCH_RANGE_SIZE;
	}
	/* each range's permutations, then each one's samples (MCC's, as Ogg Vorbis), mouth and subtitle data */
	for (range_index = 0, index = 0; range_index < range_count; range_index++)
	{
		unsigned long count = word_le(ranges + range_index * PITCH_RANGE_SIZE + 0x3C), within;
		unsigned long data_at = cursor + count * PERMUTATION_SIZE;

		for (within = 0; within < count; within++, index++)
		{
			const unsigned char *permutation = range_permutations[range_index] + within * PERMUTATION_SIZE;
			unsigned char *out = file + cursor + within * PERMUTATION_SIZE;
			unsigned long mouth = word_le(permutation + 0x54), subtitle = word_le(permutation + 0x68);
			unsigned long frames = (unsigned long)bank->samples[sample + index].frames;

			memcpy(out, permutation, 32);
			out[31] = 0;
			swap_into(out + 0x20, permutation + 0x20, 4, 2);
			put_be16(out + 0x28, SOUND_COMPRESSION_OGG_VORBIS);
			swap_into(out + 0x2A, permutation + 0x2A, 2, 1);
			/* (sample_buffer_size: its 16-bit PCM's bytes) */
			put_be32(out + 0x38, frames * (unsigned long)channels * 2);
			put_be32(out + 0x40, encoded_sizes[index]);
			put_be32(out + 0x54, mouth);
			put_be32(out + 0x68, subtitle);
			memcpy(file + data_at, encoded[index], encoded_sizes[index]);
			data_at += encoded_sizes[index];
			if (mouth)
				memcpy(file + data_at, xbox_map_tags_at(tags, tags_size, word_le(permutation + 0x60), mouth), mouth);
			data_at += mouth;
			if (subtitle)
				memcpy(file + data_at, xbox_map_tags_at(tags, tags_size, word_le(permutation + 0x74), subtitle),
					subtitle);
			data_at += subtitle;
		}
		cursor = data_at;
	}
	if (cursor != size)
		reason = "its file came out the wrong size";
	if (!reason && !file_write(path, file, size))
		reason = "its file cannot be written";
	if (!reason)
		*written_size = size;

finished:
	if (encoded)
		for (index = 0; index < permutation_total; index++)
			free(encoded[index]);
	free(encoded);
	free(encoded_sizes);
	free(file);
	return reason;
}

/* ---------- the import */

static void tally_skip(struct tally *tally, const char *reason, const char *tag)
{
	tally->skipped++;
	if (!tally->reason)
	{
		tally->reason = reason;
		snprintf(tally->reason_tag, sizeof(tally->reason_tag), "%s", tag);
	}
}

long mcc_import_voices(const char *mcc_root, const char *const *map_paths, int map_count, const char *out_folder,
	mcc_import_progress_proc progress, void *context, char *report, size_t report_size)
{
	char path[MOST_PATH];
	struct bank bank;
	struct list_tag *list = NULL;
	struct tally tally;
	CELTMode *mode = NULL;
	unsigned char *data = NULL;
	unsigned long data_capacity = 0;
	long list_count = 0, handled = 0, missing = 0, index;
	int map, canceled = 0, error = 0;

	memset(&tally, 0, sizeof(tally));
	if (report && report_size)
		report[0] = 0;
	bank.descriptor = -1;
	bank.samples = NULL;
	snprintf(path, sizeof(path), "%s/%s", mcc_root, MCC_IMPORT_VOICES_BANK);
	if (!bank_open(path, &bank))
	{
		if (report && report_size)
			snprintf(report, report_size, "voices: MCC's dialogue bank (%s) cannot be read", MCC_IMPORT_VOICES_BANK);
		return -1;
	}
	snprintf(path, sizeof(path), "%s/%s", mcc_root, MCC_IMPORT_VOICES_LIST);
	list = list_read(path, bank.count, &list_count);
	mode = celt_mode_create(FSB_CELT_RATE, FSB_CELT_FRAME, &error);
	if (!list || !mode || error != CELT_OK)
	{
		if (report && report_size)
			snprintf(report, report_size, "voices: %s", !list ?
				"MCC's dialogue list (" MCC_IMPORT_VOICES_LIST ") cannot be read" : "the CELT decoder cannot start");
		if (mode)
			celt_mode_destroy(mode);
		list_free(list, list_count);
		bank_close(&bank);
		return -1;
	}
	if (progress && progress(context, 0.f))
		canceled = 1;

	/* each map's sound tags that the list has and no map before it had */
	for (map = 0; map < map_count && !canceled && handled < list_count; map++)
	{
		unsigned long tags_size = 0, instances, count, instance_index;
		unsigned char *tags = xbox_map_tags_read(map_paths[map], &tags_size);

		if (!tags)
			continue;
		instances = word_le(tags);
		count = word_le(tags + 0xC);
		for (instance_index = 0; instance_index < count && !canceled; instance_index++)
		{
			const unsigned char *instance = xbox_map_tags_at(tags, tags_size,
				instances + instance_index * TAG_INSTANCE_SIZE, TAG_INSTANCE_SIZE);
			const unsigned char *name, *root;
			struct list_tag *entry;
			unsigned long name_address, written_size = 0;
			const char *reason;
			char tag[LIST_MOST_PATH + 1];
			size_t length;

			if (!instance)
				break;
			if (word_le(instance) != GROUP_SOUND)
				continue;
			name_address = word_le(instance + 0x10);
			if (!(name = xbox_map_tags_at(tags, tags_size, name_address, 1)))
				continue;
			length = strnlen((const char *)name, tags_size - (name_address - XBOX_MAP_TAGS_BASE));
			if (length > LIST_MOST_PATH || length == tags_size - (name_address - XBOX_MAP_TAGS_BASE))
				continue;
			memcpy(tag, name, length + 1);
			if (!(entry = list_find(list, list_count, tag)) || entry->done)
				continue;
			entry->done = 1;
			handled++;
			if (!(root = xbox_map_tags_at(tags, tags_size, word_le(instance + 0x14), SOUND_SIZE)))
				reason = "its Xbox tag cannot be read";
			else
				reason = tag_write(tags, tags_size, root, entry, &bank, mode, out_folder, &data, &data_capacity,
					&written_size);
			if (reason)
				tally_skip(&tally, reason, entry->path);
			else
			{
				tally.written++;
				tally.bytes += written_size;
			}
			if (progress && progress(context, (float)handled / (float)list_count))
				canceled = 1;
		}
		free(tags);
	}
	for (index = 0; index < list_count; index++)
		if (!list[index].done)
			missing++;

	if (report && report_size)
	{
		if (canceled)
			snprintf(report, report_size, "voices: canceled after %ld of %ld tags written", tally.written, list_count);
		else
		{
			int at = snprintf(report, report_size,
				"voices: %ld of %ld tags written (%.0f MB), %ld missing from the maps, %ld skipped", tally.written,
				list_count, (double)tally.bytes / (1024.0 * 1024.0), missing, tally.skipped);

			if (tally.reason && at > 0 && (size_t)at < report_size)
				snprintf(report + at, report_size - (size_t)at, " (%s: %s)", tally.reason_tag, tally.reason);
		}
	}
	free(data);
	celt_mode_destroy(mode);
	list_free(list, list_count);
	bank_close(&bank);
	return canceled ? -1 : tally.written;
}
