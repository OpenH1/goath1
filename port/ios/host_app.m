/*
HOST_APP.M (iOS)

The iOS app (OpenH1.app, tools/ios_build.py): what the host does before the
game starts, in place of port/macos/host_app.m.

- The app's files are in its Documents folder, which the Files app shows
  (Info.plist's UIFileSharingEnabled and LSSupportsOpeningDocumentsInPlace):
  config.toml (SDL's base path for the game), the data root (maps,
  custom_maps, debug.txt) and the saves (saves). The host's log (with the
  game's lines) is logs/halo.log there, the previous start's
  logs/halo.previous.log. The Metal pipelines' archive is in
  Library/Caches/OpenH1 (host_metal.m). HALO_DATA_ROOT and HALO_SAVE_ROOT,
  and config.toml's paths.data and paths.saves, still choose other folders.
- Internet play's brokers (brokers.txt in the app) are copied beside
  config.toml at each start, as on macOS.
- The background: as the app goes there (SDL's WILL_ENTER_BACKGROUND), the
  presentation stops (iOS refuses GPU work from the background:
  host_metal_background) and the game waits in its next event poll
  (host_app_background_wait, port/android/host/host_sdl.c), turning UIKit's
  run loop, until the app is back in the foreground. iOS suspends the
  process meanwhile; a network game's other machines drop it after 15
  seconds, as on Android.
*/

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>

/* (host.h's host_page_size is Mach's too: not used here) */
#define host_page_size host_page_size_of_guest_memory
#include "host.h"
#undef host_page_size
#include "tomlc17.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static NSString *documents_folder;
static char settings_folder[1024];

const char *host_app_settings_folder(void)
{
	return settings_folder[0] ? settings_folder : NULL;
}

const char *host_app_documents_folder(void)
{
	return documents_folder ? documents_folder.fileSystemRepresentation : NULL;
}

/* port/macos/host_metal.m */
void host_metal_background(int away);
static NSString *config_path_setting(const char *name);
static double config_number_setting(const char *name, double fallback);
/* display.render_scale's default, 0: the device's (render_scale_for_device) */
#define RENDER_SCALE_DEFAULT 0.0
/* port/ios/data_import.m */
void host_app_game_data(const char *documents);

/* ---------- the background */

static atomic_int in_background;

static bool SDLCALL lifecycle_event(void *userdata, SDL_Event *event)
{
	(void)userdata;
	if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
	{
		host_logf(HOST_LOG_INFO, "the app goes to the background: the game waits");
		atomic_store(&in_background, 1);
		host_metal_background(1);
	}
	else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
	{
		host_logf(HOST_LOG_INFO, "the app is back in the foreground");
		atomic_store(&in_background, 0);
		host_metal_background(0);
	}
	return true;
}

/* (the main thread, the game's: host_sdl.c) */
void host_app_background_wait(void)
{
	static int watching;

	if (!watching)
	{
		watching = 1;
		SDL_AddEventWatch(lifecycle_event, NULL);
	}
	while (atomic_load(&in_background))
	{
		SDL_PumpEvents();
		SDL_Delay(50);
	}
}

/* ---------- the sound's route */

/* (port/android/host/host_audio_route.c, its thread: the sound's log)
what the audio session plays on: 1 headphones (wired, AirPods and other
Bluetooth headphones), 0 the speaker or another, -1 none */
int host_app_audio_headphones(void)
{
	@autoreleasepool
	{
		AVAudioSessionRouteDescription *route = [AVAudioSession sharedInstance].currentRoute;

		for (AVAudioSessionPortDescription *port in route.outputs)
		{
			NSString *type = port.portType;

			if ([type isEqualToString:AVAudioSessionPortHeadphones] ||
				[type isEqualToString:AVAudioSessionPortBluetoothA2DP] ||
				[type isEqualToString:AVAudioSessionPortBluetoothLE] ||
				[type isEqualToString:AVAudioSessionPortBluetoothHFP])
				return 1;
		}
		return route.outputs.count ? 0 : -1;
	}
}

/* (port/android/host/host_sdl.c: host_sdl_audio_stream_timing) the latency
the audio session reports for its output now (microseconds): the route's
(AirPods' and other Bluetooth headphones' radio included) and the session's
I/O buffer, as macOS's figure counts the device's */
int host_app_audio_output_latency(void)
{
	@autoreleasepool
	{
		AVAudioSession *session = [AVAudioSession sharedInstance];
		NSTimeInterval seconds = session.outputLatency + session.IOBufferDuration;

		return seconds > 0.0 && seconds < 10.0 ? (int)(seconds * 1.0e6) : -1;
	}
}

/* ---------- the render scale and the device's temperature */

/* the render scale a device draws at by default. Stretched by the display
(no MetalFX temporal scaler): a phone whose screen has three pixels to the
point draws two of them (2/3 each way, under half the pixels), sharp at a
phone's distance and inside the 120 Hz frame where all of them were not (an
iPhone 15 Pro Max: 8 to 10 ms of GPU a frame at 3x, so 60 Hz); a 2x phone
0.8, as an Android phone does; an iPad all of its own. Rebuilt by MetalFX's
temporal scaler (temporal): its 3D view alone at the scale, from frames
jittered across its pixels, the HUD and menus at the screen's: a 3x phone
half each way (Apple's "performance" ratio, still 1.5 pixels to the point,
a quarter of the screen's pixels: 1398x645 of an iPhone 15 Pro Max's
2796x1290), a 2x phone and an iPad 2/3 (Apple's "quality" ratio, 1.33 to
the point: an 11-inch iPad Pro's 2420x1668 drawn at 1613x1112), each under
the stretched default's pixels for the GPU's time that MetalFX takes */
static float render_scale_for_device(BOOL temporal)
{
	__block float scale = temporal ? 2.0f / 3.0f : 1.0f;

	void (^choose)(void) = ^{
		if ([UIDevice currentDevice].userInterfaceIdiom == UIUserInterfaceIdiomPhone)
			scale = [UIScreen mainScreen].nativeScale >= 2.5 ? (temporal ? 0.5f : 2.0f / 3.0f) :
				(temporal ? 2.0f / 3.0f : 0.8f);
	};
	if ([NSThread isMainThread])
		choose();
	else
		dispatch_sync(dispatch_get_main_queue(), choose);
	return scale;
}

/* port/macos/xgpu_metal_temporal.m: 1 MetalFX's temporal scaler, 2 its
spatial one alone, 0 neither */
int xgpu_metal_metalfx_supported(void);

/* whether display.temporal_aa is "metalfx" (HALO_TEMPORAL_AA, else
config.toml's; commented out, "metalfx" where the GPU has MetalFX's
temporal scaler, as port_config.c's config_default_of says, else off) and
the GPU has MetalFX: it upscales the game's render scale to the screen's
pixels (#1228) */
static BOOL render_scale_upscaled(void)
{
	const char *value = getenv("HALO_TEMPORAL_AA");
	NSString *setting;
	BOOL upscaled;

	if (value && *value)
		upscaled = !strcmp(value, "metalfx");
	else if ((setting = config_path_setting("display.temporal_aa")))
		upscaled = [setting isEqualToString:@"metalfx"];
	else
		upscaled = xgpu_metal_metalfx_supported() == 1;
	return upscaled && xgpu_metal_metalfx_supported();
}

/* display.render_scale (HALO_RENDER_SCALE): the game draws at this fraction
of the screen's pixels each way, and the display scales the picture up
(port/android/host/host_sdl.c gives the game the window's pixels times it;
host_metal.m makes the layer's drawables that size); 0, the device's
(render_scale_for_device: lower where MetalFX's temporal scaler rebuilds
it). Where
MetalFX upscales it (render_scale_upscaled) the drawables are the screen's
size (1 here) and the game draws its 3D view at the render scale
(HALO_UPSCALE_RENDER_SCALE: d3d8_device.c's render_scale_configured), which
MetalFX builds the screen's pixels from. Read at the start (host_app_start),
before the game reads its environment. */
float host_app_render_scale(void)
{
	static float scale;

	if (!scale)
	{
		const char *value = getenv("HALO_RENDER_SCALE");

		scale = value ? (float)atof(value) : (float)config_number_setting("display.render_scale", RENDER_SCALE_DEFAULT);
		if (scale == 0.0f)
			scale = render_scale_for_device(render_scale_upscaled() && xgpu_metal_metalfx_supported() == 1);
		if (!(scale >= 0.25f && scale <= 1.0f))
			scale = 1.0f;
		if (scale < 1.0f && render_scale_upscaled())
		{
			char text[16];

			snprintf(text, sizeof(text), "%g", scale);
			setenv("HALO_UPSCALE_RENDER_SCALE", text, 1);
			host_logf(HOST_LOG_INFO, "render scale: %.2f, upscaled by MetalFX's %s scaler", scale,
				xgpu_metal_metalfx_supported() == 1 ? "temporal" : "spatial");
			scale = 1.0f;
		}
		else
		{
			host_logf(HOST_LOG_INFO, "render scale: %.2f", scale);
		}
	}
	return scale;
}

static const char *thermal_state_name(NSProcessInfoThermalState state)
{
	return state == NSProcessInfoThermalStateNominal ? "nominal" : state == NSProcessInfoThermalStateFair ? "fair" :
		state == NSProcessInfoThermalStateSerious ? "serious" : state == NSProcessInfoThermalStateCritical ?
		"critical" : "unknown";
}

/* the device's thermal state in the log, at the start and as it changes:
a hot phone slows its processor and GPU, which a frame's times show */
static void thermal_watch(void)
{
	host_logf(HOST_LOG_INFO, "thermal state: %s", thermal_state_name(NSProcessInfo.processInfo.thermalState));
	[[NSNotificationCenter defaultCenter] addObserverForName:NSProcessInfoThermalStateDidChangeNotification object:nil
		queue:nil usingBlock:^(NSNotification *note) {
			(void)note;
			host_logf(HOST_LOG_INFO, "thermal state: %s", thermal_state_name(NSProcessInfo.processInfo.thermalState));
		}];
}

/* ---------- the log */

static void log_open(NSString *logs)
{
	NSString *log = [logs stringByAppendingPathComponent:@"halo.log"];
	NSString *previous = [logs stringByAppendingPathComponent:@"halo.previous.log"];

	rename(log.fileSystemRepresentation, previous.fileSystemRepresentation);
	if (!freopen(log.fileSystemRepresentation, "w", stderr))
		return;
	dup2(STDERR_FILENO, STDOUT_FILENO);
	setvbuf(stderr, NULL, _IOLBF, 0);
	setvbuf(stdout, NULL, _IOLBF, 0);
}

/* ---------- config.toml's settings the app reads */

static NSString *config_path_setting(const char *name)
{
	NSString *path = [documents_folder stringByAppendingPathComponent:@"config.toml"];
	toml_result_t result = toml_parse_file_ex(path.fileSystemRepresentation);
	NSString *value = nil;

	if (!result.ok)
		return nil;
	{
		toml_datum_t datum = toml_seek(result.toptab, name);

		if (datum.type == TOML_STRING && datum.u.s[0])
			value = [NSString stringWithUTF8String:datum.u.s];
	}
	toml_free(result);
	return value;
}

/* a number of config.toml's (an integer or a real), or fallback */
static double config_number_setting(const char *name, double fallback)
{
	NSString *path = [documents_folder stringByAppendingPathComponent:@"config.toml"];
	toml_result_t result = toml_parse_file_ex(path.fileSystemRepresentation);
	double value = fallback;

	if (!result.ok)
		return fallback;
	{
		toml_datum_t datum = toml_seek(result.toptab, name);

		if (datum.type == TOML_FP64)
			value = datum.u.fp64;
		else if (datum.type == TOML_INT64)
			value = (double)datum.u.int64;
	}
	toml_free(result);
	return value;
}

/* ---------- start */

void host_app_start(void)
{
	@autoreleasepool
	{
		NSBundle *bundle = [NSBundle mainBundle];
		NSFileManager *files = [NSFileManager defaultManager];
		NSURL *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
		NSURL *caches = [[files URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask].firstObject
			URLByAppendingPathComponent:@"OpenH1"];
		NSString *logs;
		NSString *brokers;
		NSString *saves;

		/* (SDL's view controller turns with the device: landscape only, as
		Info.plist says, whichever way up; the home indicator hidden until
		the screen's edge is touched) */
		SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
		SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "1");
		documents_folder = documents.path;
		logs = [documents_folder stringByAppendingPathComponent:@"logs"];
		[files createDirectoryAtPath:logs withIntermediateDirectories:YES attributes:nil error:NULL];
		[files createDirectoryAtURL:caches withIntermediateDirectories:YES attributes:nil error:NULL];
		log_open(logs);
		snprintf(settings_folder, sizeof(settings_folder), "%s/", documents_folder.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "%s %s (%s) on %s %s, %s", [[bundle objectForInfoDictionaryKey:@"CFBundleName"] UTF8String],
			[[bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] UTF8String],
			bundle.bundleIdentifier.UTF8String, UIDevice.currentDevice.systemName.UTF8String,
			UIDevice.currentDevice.systemVersion.UTF8String, bundle.bundlePath.fileSystemRepresentation);
		host_logf(HOST_LOG_INFO, "settings, game data and saves: %s", settings_folder);
		thermal_watch();

		/* internet play's brokers (network.brokers_file), the app's list
		beside config.toml at each start */
		brokers = [bundle pathForResource:@"brokers" ofType:@"txt"];
		if (brokers)
		{
			NSString *copy = [documents_folder stringByAppendingPathComponent:@"brokers.txt"];

			[files removeItemAtPath:copy error:NULL];
			[files copyItemAtPath:brokers toPath:copy error:NULL];
		}
		/* ...and the stock maps' preparations' records (port/linux/src/
		prepared_shipped.h), the same way */
		{
			NSString *records = [bundle pathForResource:@"prepared" ofType:@"records"];

			if (records)
			{
				NSString *copy = [documents_folder stringByAppendingPathComponent:@"prepared.records"];

				[files removeItemAtPath:copy error:NULL];
				[files copyItemAtPath:records toPath:copy error:NULL];
			}
		}
		/* ...and the textures', fonts' and menus' bytes (port/linux/src/
		embedded_assets.h), the same way */
		{
			NSString *assets = [bundle pathForResource:@"openh1" ofType:@"assets"];

			if (assets)
			{
				NSString *copy = [documents_folder stringByAppendingPathComponent:@"openh1.assets"];

				[files removeItemAtPath:copy error:NULL];
				[files copyItemAtPath:assets toPath:copy error:NULL];
			}
		}

		saves = [documents_folder stringByAppendingPathComponent:@"saves"];
		if (!getenv("HALO_SAVE_ROOT") && !config_path_setting("paths.saves"))
			setenv("HALO_SAVE_ROOT", saves.fileSystemRepresentation, 1);
		if (!getenv("HALO_DATA_ROOT") && !config_path_setting("paths.data"))
		{
			setenv("HALO_DATA_ROOT", documents_folder.fileSystemRepresentation, 1);
			/* (the first start: the disc image or the maps, asked for) */
			host_app_game_data(documents_folder.fileSystemRepresentation);
		}
		/* (the render scale chosen before the game starts: its environment's
		HALO_UPSCALE_RENDER_SCALE) */
		(void)host_app_render_scale();
		/* (HOME is the app's container: the Metal archive's folder,
		Library/Caches/OpenH1) */
	}
}
