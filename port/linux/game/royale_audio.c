/* royale_audio.c: Royale's sound: the Pelican pilot's radio, campaign music at the big moments and the announcer

Everything here is the map's own sounds (b30's, Silent Cartographer's):

- the drop: the level's own insertion radio, the Pelican pilot's
  (sound\dialog\b30\b30_030_pilot, then b30_032_pilot), over the level's
  opening music (levels\b30\music\b30_01: the Halo theme), which plays out
  as the first squads land;
- the final few squads (ENDGAME): the level's driving music
  (levels\b30\music\b30_05);
- the win (RESULTS): the level's extraction music
  (sound\sinomatixx_music\b30_extraction_music);
- the announcer (sound\dialog\multiplayer1\...): a squad wiped, the final
  two squads and "Game over". A campaign map has none of the announcer's
  lines, so they are borrowed from the stock bloodgulch.map in the maps
  folder (read once: its samples, as they are): each is played through one
  of the map's own dialog sounds Royale never plays (the level's late
  dialog, its scripts gone), its permutation swapped for the borrowed one
  for the game and given back after (royale_audio_read gives the sound
  cache its samples, as mod_sounds_read does a mod's).

Every cue is timed off the synced Royale state (the phase, its clock, the
squads alive), each machine playing its own at the same tick, so every
player hears the same thing. Nothing here changes a rule. */

#include "cseries.h"
#include "cache/cache_files.h"
#include "sound/game_sound.h"
#include "sound/sound_definitions.h"
#include "tag_files/tag_groups.h"
#include "royale_audio.h"
#include "royale.h"

void platform_log(char const *format, ...);
/* the port's zlib (world_sdf.c's) */
int z_uncompress(unsigned char *destination, unsigned long *destination_size, const unsigned char *source,
	unsigned long source_size);

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- the sounds */

enum
{
	_audio_radio_pilot_1 = 0,
	_audio_radio_pilot_2,
	_audio_announcer_wipe,
	_audio_announcer_final_two,
	_audio_announcer_game_over,
	NUMBER_OF_ROYALE_AUDIO_SOUNDS
};

enum
{
	_audio_music_drop = 0,
	_audio_music_endgame,
	_audio_music_win,
	NUMBER_OF_ROYALE_AUDIO_MUSIC
};

static char const *const royale_audio_sound_names[NUMBER_OF_ROYALE_AUDIO_SOUNDS] =
{
	"sound\\dialog\\b30\\b30_030_pilot",
	"sound\\dialog\\b30\\b30_032_pilot",
	/* (a squad of two wiped out at once: the closest real line) */
	"sound\\dialog\\multiplayer1\\double_kill",
	/* (two squads left: the last minute's call) */
	"sound\\dialog\\multiplayer1\\one_minute_to_win",
	"sound\\dialog\\multiplayer1\\game_over",
};

static char const *const royale_audio_music_names[NUMBER_OF_ROYALE_AUDIO_MUSIC] =
{
	"levels\\b30\\music\\b30_01",
	"levels\\b30\\music\\b30_05",
	"sound\\sinomatixx_music\\b30_extraction_music",
};

/* the drop's radio: the first line this long into INSERTION, the second
this long after the first ends */
#define ROYALE_AUDIO_RADIO_DELAY_SECONDS 2.0f
#define ROYALE_AUDIO_RADIO_GAP_SECONDS 0.6f
/* the drop's music plays out this long into PLAY */
#define ROYALE_AUDIO_DROP_MUSIC_PLAY_SECONDS 12.0f
/* squads wiped are announced once this few are left (before, too many) */
#define ROYALE_AUDIO_WIPE_ANNOUNCE_SQUADS 8
/* the announcer, the radio and the music's loudness */
#define ROYALE_AUDIO_VOICE_SCALE 1.0f
#define ROYALE_AUDIO_MUSIC_GAIN 1.0f

/* ---------- the module's state */

static struct
{
	boolean started;
	long sounds[NUMBER_OF_ROYALE_AUDIO_SOUNDS];
	long music[NUMBER_OF_ROYALE_AUDIO_MUSIC];
	boolean music_playing[NUMBER_OF_ROYALE_AUDIO_MUSIC];
	/* the last tick's state the cues are edges of */
	enum royale_phase phase;
	long phase_time;
	short squads_alive;
	/* the cues of this phase played (a flag each) */
	unsigned long cues;
	/* (INSERTION) the phase time the second radio line plays at */
	long radio_second_time;
	boolean final_two_announced;
} royale_audio;

enum
{
	_audio_cue_radio_1 = 0,
	_audio_cue_radio_2,
	_audio_cue_drop_music_out,
};

/* ---------- private code */

static long royale_audio_find(
	long group,
	char const *name)
{
	return tag_loaded(group, name);
}

static void royale_audio_play(
	short sound)
{
	long definition_index = royale_audio.sounds[sound];

	if (definition_index == NONE)
		return;
	unspatialized_impulse_sound_new(definition_index, ROYALE_AUDIO_VOICE_SCALE);
	platform_log("royale audio: %s", royale_audio_sound_names[sound]);
}

static void royale_audio_music_start(
	short music)
{
	short other;

	/* (one piece at a time) */
	for (other = 0; other < NUMBER_OF_ROYALE_AUDIO_MUSIC; other++)
	{
		if (other != music && royale_audio.music_playing[other])
		{
			scripted_looping_sound_stop(royale_audio.music[other]);
			royale_audio.music_playing[other] = FALSE;
		}
	}
	if (royale_audio.music[music] == NONE || royale_audio.music_playing[music])
		return;
	scripted_looping_sound_start(royale_audio.music[music], NONE, ROYALE_AUDIO_MUSIC_GAIN);
	royale_audio.music_playing[music] = TRUE;
	platform_log("royale audio: music %s", royale_audio_music_names[music]);
}

static void royale_audio_music_stop(
	short music)
{
	if (!royale_audio.music_playing[music])
		return;
	/* (its stop: the piece's own ending plays out) */
	scripted_looping_sound_stop(royale_audio.music[music]);
	royale_audio.music_playing[music] = FALSE;
}

/* a sound's length in ticks (its longest permutation's) */
static long royale_audio_length_ticks(
	short sound)
{
	long definition_index = royale_audio.sounds[sound];

	if (definition_index == NONE)
		return 0;
	return royale_seconds_to_ticks((real)sound_definition_get(definition_index)->longest_permutation_length / 1000.0f);
}

/* the cue is due this tick: its time reached (since the last tick, in this
phase) and not yet played */
static boolean royale_audio_cue_due(
	short cue,
	long cue_time,
	long phase_time)
{
	if (TEST_FLAG(royale_audio.cues, cue) || phase_time < cue_time)
		return FALSE;
	SET_FLAG(royale_audio.cues, cue, TRUE);
	return TRUE;
}

static void royale_audio_phase_entered(
	enum royale_phase phase)
{
	switch (phase)
	{
	case ROYALE_PHASE_INSERTION:
		royale_audio_music_start(_audio_music_drop);
		break;
	case ROYALE_PHASE_ENDGAME:
		royale_audio_music_start(_audio_music_endgame);
		break;
	case ROYALE_PHASE_RESULTS:
		royale_audio_music_start(_audio_music_win);
		royale_audio_play(_audio_announcer_game_over);
		break;
	default:
		break;
	}
}

static void royale_audio_stop_all(
	void)
{
	short music;

	for (music = 0; music < NUMBER_OF_ROYALE_AUDIO_MUSIC; music++)
		royale_audio_music_stop(music);
}


/* ---------- the announcer, borrowed

The stock multiplayer maps' announcer lines, for a map without them: read
from bloodgulch.map (an Xbox map: its header, then the rest deflated; its
tags at the Xbox's tag address), each line's first permutation's samples
(Xbox ADPCM, as this build plays) copied once and kept for the run. */

#define ROYALE_AUDIO_XBOX_TAG_BASE 0x803A6000UL
#define ROYALE_AUDIO_XBOX_HEADER_SIZE 0x800
#define ROYALE_AUDIO_MAXIMUM_MAP_SIZE (96UL * 1024 * 1024)
#define ROYALE_AUDIO_SOUND_COMPRESSION_XBOX_ADPCM 1

enum
{
	_audio_borrowed_wipe = 0,
	_audio_borrowed_final_two,
	_audio_borrowed_game_over,
	NUMBER_OF_ROYALE_AUDIO_BORROWED
};

/* the map's own dialog each borrowed line plays through (the level's
last scenes': Royale has no scripts to play them) */
static char const *const royale_audio_borrower_names[NUMBER_OF_ROYALE_AUDIO_BORROWED] =
{
	"sound\\dialog\\b30\\b30_520_sarge2",
	"sound\\dialog\\b30\\b30_530_sarge2",
	"sound\\dialog\\b30\\b30_520_fitzgerald",
};

static struct
{
	boolean tried;
	struct
	{
		byte *samples; /* NULL: not read */
		long size;
		short encoding;
		short sample_rate;
		long length_milliseconds;
	} lines[NUMBER_OF_ROYALE_AUDIO_BORROWED];
	/* the borrowing, while a game has it */
	struct
	{
		long tag_index; /* NONE: none */
		struct sound_definition *definition;
		struct sound_definition original;
		struct sound_pitch_range range;
		struct sound_permutation permutation;
	} borrowed[NUMBER_OF_ROYALE_AUDIO_BORROWED];
} royale_audio_announcer;

static unsigned long royale_audio_read_u32(
	byte const *data)
{
	return (unsigned long)data[0] | ((unsigned long)data[1] << 8) | ((unsigned long)data[2] << 16) |
		((unsigned long)data[3] << 24);
}

static short royale_audio_read_s16(
	byte const *data)
{
	return (short)((unsigned short)data[0] | ((unsigned short)data[1] << 8));
}

/* the whole map file, inflated, or NULL */
static byte *royale_audio_map_read(
	char const *name,
	unsigned long *size)
{
	char path[256];
	FILE *file;
	byte *packed = NULL;
	byte *map = NULL;
	long length;

	snprintf(path, sizeof(path), "%s%s.map", cache_files_map_directory(), name);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > ROYALE_AUDIO_XBOX_HEADER_SIZE &&
		(unsigned long)length <= ROYALE_AUDIO_MAXIMUM_MAP_SIZE && fseek(file, 0, SEEK_SET) == 0 &&
		(packed = malloc((size_t)length)) != NULL && fread(packed, 1, (size_t)length, file) == (size_t)length)
	{
		unsigned long full = royale_audio_read_u32(packed + 8);

		/* ('head', version 5: an Xbox map) */
		if (royale_audio_read_u32(packed) == 0x68656164UL && royale_audio_read_u32(packed + 4) == 5 &&
			full > ROYALE_AUDIO_XBOX_HEADER_SIZE && full <= ROYALE_AUDIO_MAXIMUM_MAP_SIZE)
		{
			if (full == (unsigned long)length)
			{
				map = packed;
				packed = NULL;
				*size = full;
			}
			else if ((map = malloc(full)) != NULL)
			{
				unsigned long inflated = full - ROYALE_AUDIO_XBOX_HEADER_SIZE;

				csmemcpy(map, packed, ROYALE_AUDIO_XBOX_HEADER_SIZE);
				if (z_uncompress(map + ROYALE_AUDIO_XBOX_HEADER_SIZE, &inflated, packed + ROYALE_AUDIO_XBOX_HEADER_SIZE,
					(unsigned long)length - ROYALE_AUDIO_XBOX_HEADER_SIZE) != 0 ||
					inflated != full - ROYALE_AUDIO_XBOX_HEADER_SIZE)
				{
					free(map);
					map = NULL;
				}
				else
				{
					*size = full;
				}
			}
		}
	}
	if (packed)
		free(packed);
	fclose(file);
	return map;
}

/* the announcer's lines from a stock multiplayer map (once a run) */
static void royale_audio_announcer_read(
	void)
{
	static short const lines[NUMBER_OF_ROYALE_AUDIO_BORROWED] =
		{ _audio_announcer_wipe, _audio_announcer_final_two, _audio_announcer_game_over };
	unsigned long size = 0;
	byte *map;
	unsigned long tags_offset, index_offset, count, tag;
	short found = 0;

	if (royale_audio_announcer.tried)
		return;
	royale_audio_announcer.tried = TRUE;
	map = royale_audio_map_read("bloodgulch", &size);
	if (!map)
	{
		platform_log("royale audio: no announcer (bloodgulch.map not read)");
		return;
	}
#define ROYALE_AUDIO_AT(address) ((address) - ROYALE_AUDIO_XBOX_TAG_BASE + tags_offset)
#define ROYALE_AUDIO_FITS(offset, bytes) ((offset) < size && (bytes) <= size - (offset))
	tags_offset = royale_audio_read_u32(map + 0x10);
	if (!ROYALE_AUDIO_FITS(tags_offset, 16))
		goto done;
	index_offset = ROYALE_AUDIO_AT(royale_audio_read_u32(map + tags_offset));
	count = royale_audio_read_u32(map + tags_offset + 12);
	if (count > 65535 || !ROYALE_AUDIO_FITS(index_offset, count * 32))
		goto done;
	for (tag = 0; tag < count; tag++)
	{
		byte const *entry = map + index_offset + tag * 32;
		unsigned long name_offset = ROYALE_AUDIO_AT(royale_audio_read_u32(entry + 16));
		unsigned long data_offset = ROYALE_AUDIO_AT(royale_audio_read_u32(entry + 20));
		short line;

		if (royale_audio_read_u32(entry) != SOUND_DEFINITION_TAG || !ROYALE_AUDIO_FITS(name_offset, 1))
			continue;
		for (line = 0; line < NUMBER_OF_ROYALE_AUDIO_BORROWED; line++)
		{
			char const *want = royale_audio_sound_names[lines[line]];
			size_t want_length = strlen(want) + 1;
			unsigned long ranges_offset, permutations_offset, samples_offset, samples_size;

			if (royale_audio_announcer.lines[line].samples || !ROYALE_AUDIO_FITS(name_offset, want_length) ||
				memcmp(map + name_offset, want, want_length) != 0)
			{
				continue;
			}
			/* the definition (its encoding, rate, length, first pitch range),
			the range's first permutation and its samples */
			if (!ROYALE_AUDIO_FITS(data_offset, sizeof(struct sound_definition)) ||
				royale_audio_read_s16(map + data_offset + offsetof(struct sound_definition, compression)) !=
					ROYALE_AUDIO_SOUND_COMPRESSION_XBOX_ADPCM ||
				royale_audio_read_u32(map + data_offset + offsetof(struct sound_definition, pitch_ranges)) < 1)
			{
				continue;
			}
			ranges_offset = ROYALE_AUDIO_AT(royale_audio_read_u32(map + data_offset +
				offsetof(struct sound_definition, pitch_ranges) + 4));
			if (!ROYALE_AUDIO_FITS(ranges_offset, sizeof(struct sound_pitch_range)) ||
				royale_audio_read_u32(map + ranges_offset + offsetof(struct sound_pitch_range, permutations)) < 1)
			{
				continue;
			}
			permutations_offset = ROYALE_AUDIO_AT(royale_audio_read_u32(map + ranges_offset +
				offsetof(struct sound_pitch_range, permutations) + 4));
			if (!ROYALE_AUDIO_FITS(permutations_offset, sizeof(struct sound_permutation)))
				continue;
			samples_size = royale_audio_read_u32(map + permutations_offset + offsetof(struct sound_permutation, samples));
			samples_offset = royale_audio_read_u32(map + permutations_offset + offsetof(struct sound_permutation, samples) +
				offsetof(struct tag_data, file_offset));
			if (samples_size == 0 || samples_size > 4UL * 1024 * 1024 || !ROYALE_AUDIO_FITS(samples_offset, samples_size))
				continue;
			royale_audio_announcer.lines[line].samples = malloc(samples_size);
			if (!royale_audio_announcer.lines[line].samples)
				continue;
			csmemcpy(royale_audio_announcer.lines[line].samples, map + samples_offset, samples_size);
			royale_audio_announcer.lines[line].size = (long)samples_size;
			royale_audio_announcer.lines[line].encoding =
				royale_audio_read_s16(map + data_offset + offsetof(struct sound_definition, encoding));
			royale_audio_announcer.lines[line].sample_rate =
				royale_audio_read_s16(map + data_offset + offsetof(struct sound_definition, sample_rate));
			royale_audio_announcer.lines[line].length_milliseconds =
				(long)royale_audio_read_u32(map + data_offset + offsetof(struct sound_definition, longest_permutation_length));
			found++;
		}
	}
done:
#undef ROYALE_AUDIO_AT
#undef ROYALE_AUDIO_FITS
	free(map);
	platform_log("royale audio: %d of %d announcer lines read from bloodgulch.map", found, NUMBER_OF_ROYALE_AUDIO_BORROWED);
}

/* gives the borrowed sounds back their own (before the map's tags go) */
static void royale_audio_announcer_give_back(
	void)
{
	short line;

	for (line = 0; line < NUMBER_OF_ROYALE_AUDIO_BORROWED; line++)
	{
		long tag_index = royale_audio_announcer.borrowed[line].tag_index;

		if (!royale_audio_announcer.borrowed[line].definition)
			continue;
		royale_audio_announcer.borrowed[line].tag_index = NONE;
		/* (only into the same map's tags still loaded) */
		if (tag_loaded(SOUND_DEFINITION_TAG, royale_audio_borrower_names[line]) == tag_index &&
			sound_definition_get(tag_index) == royale_audio_announcer.borrowed[line].definition)
		{
			*royale_audio_announcer.borrowed[line].definition = royale_audio_announcer.borrowed[line].original;
		}
		royale_audio_announcer.borrowed[line].definition = NULL;
	}
}

/* the announcer's lines, for a map without them: through the borrowed
sounds; each line's sound index in royale_audio.sounds */
static void royale_audio_announcer_borrow(
	void)
{
	static short const lines[NUMBER_OF_ROYALE_AUDIO_BORROWED] =
		{ _audio_announcer_wipe, _audio_announcer_final_two, _audio_announcer_game_over };
	short line;

	royale_audio_announcer_read();
	for (line = 0; line < NUMBER_OF_ROYALE_AUDIO_BORROWED; line++)
	{
		struct sound_definition *definition;
		struct sound_pitch_range *range;
		struct sound_permutation *permutation;
		long tag_index;

		royale_audio_announcer.borrowed[line].tag_index = NONE;
		royale_audio_announcer.borrowed[line].definition = NULL;
		if (royale_audio.sounds[lines[line]] != NONE || !royale_audio_announcer.lines[line].samples)
			continue;
		tag_index = tag_loaded(SOUND_DEFINITION_TAG, royale_audio_borrower_names[line]);
		if (tag_index == NONE)
			continue;
		definition = sound_definition_get(tag_index);
		if (definition->pitch_ranges.count < 1 || definition->compression != ROYALE_AUDIO_SOUND_COMPRESSION_XBOX_ADPCM)
			continue;
		royale_audio_announcer.borrowed[line].tag_index = tag_index;
		royale_audio_announcer.borrowed[line].definition = definition;
		royale_audio_announcer.borrowed[line].original = *definition;
		/* the range: the definition's first, with the one borrowed permutation */
		range = &royale_audio_announcer.borrowed[line].range;
		*range = *TAG_BLOCK_GET_ELEMENT(&definition->pitch_ranges, 0, struct sound_pitch_range);
		range->actual_permutation_count = 1;
		range->played_permutation_mask = 0;
		range->previous_permutation_index = NONE;
		range->forced_permutation_index = NONE;
		permutation = &royale_audio_announcer.borrowed[line].permutation;
		*permutation = *TAG_BLOCK_GET_ELEMENT(&range->permutations, 0, struct sound_permutation);
		permutation->skip_fraction = 0.0f;
		permutation->gain = 1.0f;
		permutation->compression = ROYALE_AUDIO_SOUND_COMPRESSION_XBOX_ADPCM;
		permutation->next_permutation_index = NONE;
		permutation->sample_buffer_size = 0;
		permutation->samples.size = royale_audio_announcer.lines[line].size;
		permutation->samples.file_offset = 0;
		permutation->samples.address = royale_audio_announcer.lines[line].samples;
		csmemset(&permutation->mouth_data, 0, sizeof(permutation->mouth_data));
		csmemset(&permutation->subtitle_data, 0, sizeof(permutation->subtitle_data));
		permutation->unknown0 = (long)NONE;
		permutation->unknown1 = 0;
		permutation->unknown2 = (unsigned long)tag_index;
		permutation->unknown3 = (unsigned long)tag_index;
		range->permutations.count = 1;
		range->permutations.address = permutation;
		definition->pitch_ranges.count = 1;
		definition->pitch_ranges.address = range;
		definition->encoding = royale_audio_announcer.lines[line].encoding;
		definition->sample_rate = royale_audio_announcer.lines[line].sample_rate;
		definition->longest_permutation_length = royale_audio_announcer.lines[line].length_milliseconds;
		definition->promotion_sound.index = NONE;
		definition->promotion_count = 0;
		royale_audio.sounds[lines[line]] = tag_index;
	}
}

/* ---------- public code */

void royale_audio_start(
	struct royale_map_config const *config)
{
	short index;
	short missing = 0;

	(void)config;
	royale_audio_stop_all();
	royale_audio_announcer_give_back();
	csmemset(&royale_audio, 0, sizeof(royale_audio));
	for (index = 0; index < NUMBER_OF_ROYALE_AUDIO_SOUNDS; index++)
	{
		royale_audio.sounds[index] = royale_audio_find(SOUND_DEFINITION_TAG, royale_audio_sound_names[index]);
		missing += royale_audio.sounds[index] == NONE;
	}
	for (index = 0; index < NUMBER_OF_ROYALE_AUDIO_MUSIC; index++)
	{
		royale_audio.music[index] = royale_audio_find(LOOPING_SOUND_DEFINITION_TAG, royale_audio_music_names[index]);
		missing += royale_audio.music[index] == NONE;
	}
	royale_audio_announcer_borrow();
	royale_audio.phase = ROYALE_PHASE_NONE;
	royale_audio.squads_alive = NONE;
	royale_audio.started = TRUE;
	platform_log("royale audio: radio %s, music %s/%s/%s, announcer %s (%d of the sounds missing)",
		royale_audio.sounds[_audio_radio_pilot_1] != NONE ? "yes" : "no",
		royale_audio.music[_audio_music_drop] != NONE ? "yes" : "no",
		royale_audio.music[_audio_music_endgame] != NONE ? "yes" : "no",
		royale_audio.music[_audio_music_win] != NONE ? "yes" : "no",
		royale_audio.sounds[_audio_announcer_game_over] != NONE ? "yes" : "no", missing);
}

void royale_audio_reset(
	void)
{
	royale_audio_announcer_give_back();
	/* (the map's sounds are let go with its tags; only forget them) */
	csmemset(&royale_audio, 0, sizeof(royale_audio));
	royale_audio.phase = ROYALE_PHASE_NONE;
}

void royale_audio_tick(
	void)
{
	enum royale_phase phase;
	long phase_time;
	short squads_alive;

	if (!royale_audio.started || !royale_active())
		return;
	phase = royale_phase();
	phase_time = royale_phase_time();
	squads_alive = royale_squads_alive();
	if (phase != royale_audio.phase)
	{
		royale_audio.cues = 0;
		royale_audio.radio_second_time = NONE;
		royale_audio_phase_entered(phase);
	}

	/* the drop: the pilot's radio, then the music plays out once squads land */
	if (phase == ROYALE_PHASE_INSERTION)
	{
		long first = royale_seconds_to_ticks(ROYALE_AUDIO_RADIO_DELAY_SECONDS);

		if (royale_audio_cue_due(_audio_cue_radio_1, first, phase_time))
		{
			royale_audio_play(_audio_radio_pilot_1);
			royale_audio.radio_second_time = first + royale_audio_length_ticks(_audio_radio_pilot_1) +
				royale_seconds_to_ticks(ROYALE_AUDIO_RADIO_GAP_SECONDS);
		}
		if (royale_audio.radio_second_time != NONE &&
			royale_audio_cue_due(_audio_cue_radio_2, royale_audio.radio_second_time, phase_time))
		{
			royale_audio_play(_audio_radio_pilot_2);
		}
	}
	if (phase == ROYALE_PHASE_PLAY &&
		royale_audio_cue_due(_audio_cue_drop_music_out, royale_seconds_to_ticks(ROYALE_AUDIO_DROP_MUSIC_PLAY_SECONDS),
			phase_time))
	{
		royale_audio_music_stop(_audio_music_drop);
	}

	/* the announcer: a squad wiped out (late in the match), the final two */
	if ((phase == ROYALE_PHASE_PLAY || phase == ROYALE_PHASE_ENDGAME) && royale_audio.squads_alive != NONE &&
		squads_alive < royale_audio.squads_alive)
	{
		if (squads_alive == 2 && !royale_audio.final_two_announced)
		{
			royale_audio.final_two_announced = TRUE;
			royale_audio_play(_audio_announcer_final_two);
		}
		else if (squads_alive > 1 && squads_alive <= ROYALE_AUDIO_WIPE_ANNOUNCE_SQUADS)
		{
			royale_audio_play(_audio_announcer_wipe);
		}
	}
	royale_audio.phase = phase;
	royale_audio.phase_time = phase_time;
	royale_audio.squads_alive = squads_alive;
}

/* (xbox_sound_cache.c, as it loads a permutation's samples) a borrowed
announcer line's, from memory: TRUE if the permutation is one */
boolean royale_audio_read(
	struct sound_permutation const *permutation,
	void *buffer)
{
	short line;

	for (line = 0; line < NUMBER_OF_ROYALE_AUDIO_BORROWED; line++)
	{
		if (permutation == &royale_audio_announcer.borrowed[line].permutation)
		{
			if (!permutation->samples.address)
				return FALSE;
			csmemcpy(buffer, permutation->samples.address, (size_t)permutation->samples.size);
			return TRUE;
		}
	}
	return FALSE;
}
