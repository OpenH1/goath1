/*
SOFA_FIT.C (test)

audio.hrtf_file (test_sofa_fit.py): a SOFA file read (port/linux/src/
hrtf_sofa.c) and fitted into the HRTF's table (dsound_sdl.c's hrtf_fit,
through its hrtf_load; the mixer as under_test.inc, as test_mixer.py takes
it). test_sofa_fit.py writes a small SOFA file of its own (HDF5 as netCDF-4
writes it: a version 2 superblock and object headers, Data.IR chunked
through shuffle and deflate) with a response for each of the table's
directions whose band levels and delays it knows, and those (expected:
for each direction, the rows from below and round from ahead to the right,
then the poles below and above, the left ear's nine levels, the right's and
the delay in output frames, as floats).
"sofa <file> <expected> <broken file>":
	- an empty audio.hrtf_file, a missing file and a broken one leave the
	  mixer on the Xbox's table, as it is (the same table, byte for byte);
	- the file read: every direction of the table, looked up as the mixer
	  does (hrtf_lookup), at the levels and delay expected, the levels all
	  moved alike so that ahead is as loud overall as the Xbox's.
*/

#include "harness.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>

typedef int BOOL;
typedef unsigned long DWORD;
typedef long LONG;
typedef unsigned long ULONG;
typedef void *LPVOID;
typedef void (*LPFNXMEDIAOBJECTCALLBACK)(void *, void *, DWORD);
typedef struct { void *lpVtbl; } IDirectSoundStream;
typedef struct { void *pvBuffer; DWORD dwMaxSize; } XMEDIAPACKET;
typedef struct
{
	LONG lRoom, lRoomHF;
	float flRoomRolloffFactor, flDecayTime, flDecayHFRatio;
	LONG lReflections;
	float flReflectionsDelay;
	LONG lReverb;
	float flReverbDelay, flDiffusion, flDensity, flHFReference;
} DSI3DL2LISTENER;
#define DSBVOLUME_MIN (-10000)
#define DS3DMODE_NORMAL 0x00000000
#define DS3DMODE_HEADRELATIVE 0x00000001
#define DS3DMODE_DISABLE 0x00000002

typedef unsigned long long Uint64;
#define platform_log(...) ((void)0)
#define SDL_GetTicksNS() 0ull

static int logged;

static const char *config_string(const char *name)
{
	(void)name;
	return "";
}

static void voice_audio_mix(float *output, unsigned long frames)
{
	(void)output;
	(void)frames;
}

#include "under_test.inc"
#include "hrtf_sofa.c"

/* (what hrtf_load logs, counted) */
#undef platform_log
#define platform_log(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr), ++logged)
#include "hrtf_load.inc"

#define DIRECTIONS (HRTF_ELEVATIONS * HRTF_AZIMUTHS + 2)
#define FIGURES (2 * HRTF_BANDS + 1)

static BOOL on_xbox_table(void)
{
	return hrtf_rows == hrtf_table && hrtf_ends == hrtf_poles &&
		!memcmp(hrtf_rows, hrtf_table, sizeof(hrtf_table)) && !memcmp(hrtf_ends, hrtf_poles, sizeof(hrtf_poles));
}

static double level(const float *figures)
{
	double power = 0.0;
	int band;

	for (band = 0; band < 2 * HRTF_BANDS; band++)
		power += pow(10.0, figures[band] / 10.0);
	return 10.0 * log10(power / (2.0 * HRTF_BANDS));
}

int main(int argc, char **argv)
{
	char const *case_name = argc > 1 ? argv[1] : "";

	CASE("sofa")
	{
		static float expected[DIRECTIONS][FIGURES], xbox_front[FIGURES];
		FILE *file;
		double offset;
		int d, band, logged_before;

		CHECK(argc == 5, "sofa <file> <expected> <broken file>");
		file = fopen(argv[3], "rb");
		CHECK(file && fread(expected, sizeof(expected), 1, file) == 1, "cannot read %s", argv[3]);
		fclose(file);
		hrtf_initialize();

		/* (none, a missing file and a broken one: the Xbox's) */
		hrtf_load("");
		CHECK(on_xbox_table() && logged == 0, "an empty audio.hrtf_file changed the table or logged");
		hrtf_load("no such file.sofa");
		CHECK(on_xbox_table() && logged == 1, "a missing file: not the Xbox's table, or not logged once");
		hrtf_load(argv[4]);
		CHECK(on_xbox_table() && logged == 2, "a broken file: not the Xbox's table, or not logged once");

		logged_before = logged;
		hrtf_load(argv[2]);
		CHECK(!on_xbox_table(), "the SOFA file was not fitted");
		CHECK(logged == logged_before + 1, "the fit logged %d lines", logged - logged_before);

		/* (as loud ahead as the Xbox's) */
		memcpy(xbox_front, hrtf_table[HRTF_ELEVATIONS / 2][0].left, sizeof(float) * HRTF_BANDS);
		memcpy(xbox_front + HRTF_BANDS, hrtf_table[HRTF_ELEVATIONS / 2][0].right, sizeof(float) * HRTF_BANDS);
		offset = level(xbox_front) - level(expected[(HRTF_ELEVATIONS / 2) * HRTF_AZIMUTHS]);
		for (d = 0; d < DIRECTIONS; d++)
		{
			struct hrtf_direction got;
			float azimuth = d < DIRECTIONS - 2 ? (float)(d % HRTF_AZIMUTHS * 15) : 0.0f;
			float elevation = d < DIRECTIONS - 2 ? hrtf_elevations[d / HRTF_AZIMUTHS + 1] : (d == DIRECTIONS - 2 ? -90.0f : 90.0f);

			hrtf_lookup(azimuth, elevation, &got);
			for (band = 0; band < HRTF_BANDS; band++)
			{
				CHECK(fabs(got.left[band] - (expected[d][band] + offset)) < 0.01 &&
					fabs(got.right[band] - (expected[d][HRTF_BANDS + band] + offset)) < 0.01,
					"%.0f degrees round, %.0f up, band %d: %.2f and %.2f dB, not %.2f and %.2f", azimuth, elevation, band,
					got.left[band], got.right[band], expected[d][band] + offset, expected[d][HRTF_BANDS + band] + offset);
			}
			CHECK(fabs(got.delay - expected[d][2 * HRTF_BANDS]) < 0.01, "%.0f degrees round, %.0f up: a delay of %.2f "
				"frames, not %.2f", azimuth, elevation, got.delay, expected[d][2 * HRTF_BANDS]);
		}
		printf("sofa: %d directions as expected (levels moved %.2f dB to the Xbox's ahead)\n", DIRECTIONS, offset);
		return 0;
	}
	fprintf(stderr, "unknown case %s\n", case_name);
	return 2;
}
