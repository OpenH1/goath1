/*
SOUND_BUFFER_FRAMES.C (test)

audio.buffer_frames' choice (port/linux/src/dsound_sdl.c's
buffer_frames_choose and timing_check, as test_sound_buffer_frames.py
takes them) on a fake Windows device: SDL 3.4's WASAPI driver in shared
mode (SDL_wasapi.c, WASAPI_PrepDevice) rounds the request to the engine's
fundamental period and holds it between the driver's smallest and largest
period (IAudioClient3::GetSharedModeEnginePeriod), the period the device
then asks for at a time.
	- automatic (0), a 48 kHz endpoint that allows 336 frames (7 ms) to its
	  default 480: it starts at the 336, and a dropout after the game has
	  settled grows it to the 480;
	- automatic, a driver with the engine's 10 ms alone: 480;
	- a number (512) kept, dropouts or not.
*/
#include "harness.h"

typedef int BOOL;
typedef unsigned long long Uint64;
typedef struct fake_stream SDL_AudioStream;
struct fake_stream { int frames; };

/* the fake device: its periods (frames at 48 kHz) */
static unsigned int fundamental_period, min_period, max_period;
static struct fake_stream device;
static int opens, granted;
static Uint64 now_ms;
static long setting;

static long config_integer(const char *name) { (void)name; return setting; }
static Uint64 SDL_GetTicks(void) { return now_ms; }
static const char *SDL_GetError(void) { return ""; }
static void SDL_DestroyAudioStream(SDL_AudioStream *stream) { (void)stream; }
static void silent_clock_start(void) { CHECK(FALSE, "the device failed to open"); }
static void platform_log(const char *format, ...) { (void)format; }
#define pthread_mutex_lock(lock) ((void)(lock))
#define pthread_mutex_unlock(lock) ((void)(lock))
static int timing_lock;
static struct { unsigned long dropouts; double silence_ms; } timing;
static SDL_AudioStream *audio_stream;
static SDL_AudioStream *audio_open(void);

#include "under_test.inc"

/* audio_open at buffer_frames, as SDL's WASAPI driver opens it */
static SDL_AudioStream *audio_open(void)
{
	double steps = (double)buffer_frames / fundamental_period;
	unsigned int period = fundamental_period * (unsigned int)(steps + 0.5);

	if (period < min_period)
		period = min_period;
	if (period > max_period)
		period = max_period;
	granted = (int)period;
	opens++;
	audio_opened_at = SDL_GetTicks();
	timing.dropouts = 0;
	timing.silence_ms = 0.0;
	return &device;
}

/* the device run for seconds at a frame each 16 ms, with a
dropout at each of the times given (ms from the start; 0 ends them) */
static void play(int seconds, const Uint64 *dropouts)
{
	Uint64 start = now_ms;

	while (now_ms - start < (Uint64)seconds * 1000)
	{
		now_ms += 16;
		for (const Uint64 *at = dropouts; *at; at++)
			if (now_ms - start >= *at && now_ms - start < *at + 16)
				timing.dropouts++, timing.silence_ms += 2.0;
		timing_check();
	}
}

static void open_device(unsigned int fundamental, unsigned int smallest, unsigned int largest)
{
	fundamental_period = fundamental;
	min_period = smallest;
	max_period = largest;
	now_ms = 1000;
	buffer_frames_choose();
	audio_stream = audio_open();
}

int main(int argc, char **argv)
{
	CHECK(argc == 2, "case name");
	const char *case_name = argv[1];
	static const Uint64 none[] = { 0 };
	static const Uint64 settled[] = { 6000, 0 };

	CASE("smallest-period")
	{
		open_device(48, 336, 480);
		CHECK(granted == 336, "automatic took %d frames, not the device's smallest period, 336", granted);
		play(3, none);
		CHECK(opens == 1 && granted == 336, "opened again without a dropout (%d frames)", granted);
		play(10, settled);
		CHECK(opens == 2 && granted == 480, "a dropout did not grow it to the default (%d frames, %d opens)", granted,
			opens);
		return 0;
	}
	CASE("default-only")
	{
		open_device(480, 480, 480);
		CHECK(granted == 480, "a driver of 10 ms alone took %d frames", granted);
		return 0;
	}
	CASE("number-kept")
	{
		setting = 512;
		open_device(48, 336, 480);
		CHECK(buffer_frames == 512 && granted == 480, "audio.buffer_frames 512 asked %ld, took %d", buffer_frames,
			granted);
		play(10, settled);
		CHECK(opens == 1 && buffer_frames == 512, "a set size grew (%ld frames, %d opens)", buffer_frames, opens);
		return 0;
	}
	CHECK(FALSE, "unknown case %s", case_name);
	return 1;
}
