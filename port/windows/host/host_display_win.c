/*
HOST_DISPLAY_WIN.C

display.hdr (#1122): what Windows says of the display a window is on that
SDL does not (its headroom and SDR white are SDL's: host_vulkan.c), its
black and its full-frame peak in nits, as DXGI's IDXGIOutput6::GetDesc1
reports them (the Windows HDR Calibration app's values where it has run).
The output is the one whose monitor the window is on, found on the adapter
whose LUID is the Vulkan device's, else on any adapter (a laptop's display
on its integrated GPU while another draws). dxgi.dll is loaded as it is
asked, so that nothing links it.

#1283: Windows' graphics preference for the game, which can keep a card
out of Vulkan's list (host_gpu_preference_check).
*/

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <stdlib.h>
#include <string.h>

#include "host_win.h"
#include "host_log_queue.h"

/* (their own, so that dxguid.lib is not needed) */
static const GUID factory_iid = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } };
static const GUID output6_iid = { 0x068346e8, 0xaaec, 0x4b84, { 0xad, 0xd7, 0x13, 0x7f, 0x51, 0x3f, 0x77, 0xa1 } };

/* the output of an adapter that shows a monitor, its description read
into description; 0 if none does */
static int adapter_output(IDXGIAdapter1 *adapter, HMONITOR monitor, DXGI_OUTPUT_DESC1 *description)
{
	IDXGIOutput *output;
	UINT index;

	for (index = 0; IDXGIAdapter1_EnumOutputs(adapter, index, &output) == S_OK; index++)
	{
		DXGI_OUTPUT_DESC plain;
		IDXGIOutput6 *output6 = NULL;
		int found = 0;

		if (IDXGIOutput_GetDesc(output, &plain) == S_OK && plain.Monitor == monitor &&
			IDXGIOutput_QueryInterface(output, &output6_iid, (void **)&output6) == S_OK)
		{
			found = IDXGIOutput6_GetDesc1(output6, description) == S_OK;
			IDXGIOutput6_Release(output6);
		}
		IDXGIOutput_Release(output);
		if (found)
			return 1;
	}
	return 0;
}

static IDXGIFactory1 *factory_make(void)
{
	typedef HRESULT (WINAPI *create_factory_function)(REFIID, void **);
	static HMODULE dxgi;
	static create_factory_function create_factory;
	IDXGIFactory1 *factory = NULL;

	if (!dxgi)
	{
		dxgi = LoadLibraryW(L"dxgi.dll");
		if (dxgi)
			create_factory = (create_factory_function)(void *)GetProcAddress(dxgi, "CreateDXGIFactory1");
	}
	if (!create_factory || create_factory(&factory_iid, (void **)&factory) != S_OK)
		return NULL;
	return factory;
}

/* #1283: each adapter DXGI lists, told to each (its description in UTF-8,
vendor ID, dedicated video memory, LUID, and whether it is a software one,
like the Basic Render Driver); the count told */
int host_display_adapters(void (*each)(void *context, const char *name, unsigned vendor, unsigned device,
	unsigned long long dedicated_memory, const unsigned char luid[8], int software), void *context)
{
	IDXGIFactory1 *factory = factory_make();
	IDXGIAdapter1 *adapter;
	UINT index;
	int count = 0;

	if (!factory)
		return 0;
	for (index = 0; IDXGIFactory1_EnumAdapters1(factory, index, &adapter) == S_OK; index++)
	{
		DXGI_ADAPTER_DESC1 description;
		char name[128];

		if (IDXGIAdapter1_GetDesc1(adapter, &description) == S_OK)
		{
			if (!WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, (int)sizeof(name), NULL, NULL))
				strcpy(name, "an adapter");
			each(context, name, description.VendorId, description.DeviceId, (unsigned long long)description.DedicatedVideoMemory,
				(const unsigned char *)&description.AdapterLuid, (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0);
			count++;
		}
		IDXGIAdapter1_Release(adapter);
	}
	IDXGIFactory1_Release(factory);
	return count;
}

/* #1269: the LUID of the adapter whose output shows the window's monitor
(physical_choose prefers the Vulkan device of that LUID among its equals);
0 if none is found */
int host_display_adapter_luid(void *window, unsigned char luid[8])
{
	IDXGIFactory1 *factory;
	IDXGIAdapter1 *adapter;
	HMONITOR monitor;
	UINT index;
	int found = 0;

	if (!window || !(monitor = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST)) || !(factory = factory_make()))
		return 0;
	for (index = 0; !found && IDXGIFactory1_EnumAdapters1(factory, index, &adapter) == S_OK; index++)
	{
		IDXGIOutput *output;
		UINT output_index;

		for (output_index = 0; !found && IDXGIAdapter1_EnumOutputs(adapter, output_index, &output) == S_OK;
			output_index++)
		{
			DXGI_OUTPUT_DESC description;
			DXGI_ADAPTER_DESC1 adapter_description;

			if (IDXGIOutput_GetDesc(output, &description) == S_OK && description.Monitor == monitor &&
				IDXGIAdapter1_GetDesc1(adapter, &adapter_description) == S_OK)
			{
				memcpy(luid, &adapter_description.AdapterLuid, 8);
				found = 1;
			}
			IDXGIOutput_Release(output);
		}
		IDXGIAdapter1_Release(adapter);
	}
	IDXGIFactory1_Release(factory);
	return found;
}

int host_display_luminance(const unsigned char luid[8], void *window, float *black_nits, float *peak_nits,
	float *full_frame_nits, float primaries[8])
{
	DXGI_OUTPUT_DESC1 description;
	IDXGIFactory1 *factory;
	IDXGIAdapter1 *adapter;
	HMONITOR monitor;
	UINT index;
	int pass, found = 0;

	*black_nits = *peak_nits = *full_frame_nits = 0.0f;
	if (!window)
		return 0;
	monitor = MonitorFromWindow((HWND)window, MONITOR_DEFAULTTONEAREST);
	if (!monitor)
		return 0;
	/* (a factory of its own each time: one's outputs keep the descriptions
	they had when it was made) */
	if (!(factory = factory_make()))
		return 0;
	/* (the Vulkan device's adapter first; then any) */
	for (pass = luid ? 0 : 1; pass < 2 && !found; pass++)
	{
		for (index = 0; !found && IDXGIFactory1_EnumAdapters1(factory, index, &adapter) == S_OK; index++)
		{
			DXGI_ADAPTER_DESC1 adapter_description;
			int ours = luid && IDXGIAdapter1_GetDesc1(adapter, &adapter_description) == S_OK &&
				!memcmp(&adapter_description.AdapterLuid, luid, 8);

			if (pass == 1 || ours)
				found = adapter_output(adapter, monitor, &description);
			IDXGIAdapter1_Release(adapter);
		}
	}
	IDXGIFactory1_Release(factory);
	if (!found)
		return 0;
	*black_nits = description.MinLuminance > 0.0f ? description.MinLuminance : 0.0f;
	*peak_nits = description.MaxLuminance > 0.0f ? description.MaxLuminance : 0.0f;
	*full_frame_nits = description.MaxFullFrameLuminance > 0.0f ? description.MaxFullFrameLuminance : 0.0f;
	if (primaries)
	{
		primaries[0] = description.RedPrimary[0];
		primaries[1] = description.RedPrimary[1];
		primaries[2] = description.GreenPrimary[0];
		primaries[3] = description.GreenPrimary[1];
		primaries[4] = description.BluePrimary[0];
		primaries[5] = description.BluePrimary[1];
		primaries[6] = description.WhitePoint[0];
		primaries[7] = description.WhitePoint[1];
	}
	return 1;
}

/* ---------- Windows' graphics preference (#1283)

Windows keeps a GPU preference for each program, by its executable's full
path: HKCU\Software\Microsoft\DirectX\UserGpuPreferences, the value named
by the path, "GpuPreference=2;" for high performance, as Settings > System
> Display > Graphics writes it (no administrator's rights). Its "power
saving" GPU, a Ryzen's integrated Radeon, is what the vendors' layers and
drivers can then give a Vulkan program alone, which the game's layer
filters (xgpu_vulkan.c's load_functions) do not reach: #1283's RTX 5070 was
not listed with AMD's layer off.

So, where DXGI lists an NVIDIA or AMD card that Vulkan does not, and the
player has no value of their own for halo.exe, the game sets high
performance and starts itself again, once: the copy has
HOST_GPU_RESTARTED_VARIABLE, and says whether the card is then listed. The
value is the player's from then on (Settings shows it), and never
overwritten: a later start with the card still missing finds it and does
not start again. The path is this process's, the one that makes the Vulkan
instance (a copy the launcher started is the same halo.exe, host_main_win.c).
The copy is this one's command line and environment, its loader's variables
the player's (host_vulkan.c); this process hides its window, waits for the
copy and ends with its exit code, as the launcher does. */

#define GPU_PREFERENCES_KEY L"Software\\Microsoft\\DirectX\\UserGpuPreferences"

int host_gpu_restarted(void)
{
	static int known, restarted;

	if (!known)
	{
		known = 1;
		restarted = GetEnvironmentVariableW(HOST_GPU_RESTARTED_VARIABLE, NULL, 0) > 0;
	}
	return restarted;
}

void host_gpu_preference_check(const char *missing, void *window)
{
	wchar_t path[MAX_PATH], value[256];
	wchar_t *command;
	DWORD length, size, type, code = 1;
	HKEY key;
	LONG status;
	int has_value = 0;
	STARTUPINFOW startup;
	PROCESS_INFORMATION process;

	/* (not for anything this process starts later: the self-updater's) */
	SetEnvironmentVariableW(HOST_GPU_RESTARTED_VARIABLE, NULL);
	if (host_gpu_restarted())
	{
		if (missing[0])
			host_logf(HOST_LOG_WARN, "vulkan: the last start set Windows' graphics preference for the game to high "
				"performance and started it again, and %s is still not in Vulkan's list: Windows' preference "
				"did not hide it (#1283)", missing);
		else
			host_logf(HOST_LOG_INFO, "vulkan: every card Windows lists is in Vulkan's list now: Windows' graphics "
				"preference for the game (power saving, or Windows' choice) had hidden one; the last start set it to "
				"high performance (#1283)");
		return;
	}
	if (!missing[0])
		return;
	length = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (!length || length >= MAX_PATH)
		return;
	/* (the long path, as Settings' file picker gives it) */
	{
		wchar_t long_path[MAX_PATH];
		DWORD long_length = GetLongPathNameW(path, long_path, MAX_PATH);

		if (long_length && long_length < MAX_PATH)
			wcscpy(path, long_path);
	}
	if (RegOpenKeyExW(HKEY_CURRENT_USER, GPU_PREFERENCES_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
	{
		size = sizeof(value) - sizeof(wchar_t);
		status = RegQueryValueExW(key, path, NULL, &type, (BYTE *)value, &size);
		/* (any value of the player's, of any type or length, is theirs) */
		has_value = status == ERROR_SUCCESS || status == ERROR_MORE_DATA;
		if (status == ERROR_SUCCESS && type == REG_SZ)
			value[size / sizeof(wchar_t)] = 0;
		else
			wcscpy(value, L"(not text)");
		RegCloseKey(key);
	}
	if (has_value)
	{
		char *text = host_utf8_from_wide(value);

		host_logf(HOST_LOG_WARN, "vulkan: Windows' graphics preference for the game is the player's own (%s): left "
			"as it is. If %s should draw, Settings > System > Display > Graphics sets the game to high "
			"performance (#1283)", text ? text : "?", missing);
		free(text);
		return;
	}
	status = RegCreateKeyExW(HKEY_CURRENT_USER, GPU_PREFERENCES_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
	if (status == ERROR_SUCCESS)
	{
		static const wchar_t high[] = L"GpuPreference=2;";

		status = RegSetValueExW(key, path, 0, REG_SZ, (const BYTE *)high, sizeof(high));
		RegCloseKey(key);
	}
	if (status != ERROR_SUCCESS)
	{
		host_logf(HOST_LOG_WARN, "vulkan: cannot set Windows' graphics preference for the game (error %ld)",
			(long)status);
		return;
	}
	host_logf(HOST_LOG_WARN, "vulkan: %s is not in Vulkan's list: Windows' graphics preference for the game set to "
		"high performance (GpuPreference=2, as Settings > System > Display > Graphics would), and the game started "
		"again to use it (#1283)", missing);
	/* (this process's lines in halo.log before the copy's) */
	host_log_queue_flush(1000);
	memset(&startup, 0, sizeof(startup));
	startup.cb = sizeof(startup);
	command = _wcsdup(GetCommandLineW());
	SetEnvironmentVariableW(HOST_GPU_RESTARTED_VARIABLE, L"1");
	if (!command || !CreateProcessW(path, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process))
	{
		SetEnvironmentVariableW(HOST_GPU_RESTARTED_VARIABLE, NULL);
		host_logf(HOST_LOG_WARN, "vulkan: cannot start the game again (error %lu): it goes on with the GPU it has; "
			"the next start uses the preference", (unsigned long)GetLastError());
		free(command);
		return;
	}
	SetEnvironmentVariableW(HOST_GPU_RESTARTED_VARIABLE, NULL);
	free(command);
	if (window)
		ShowWindow((HWND)window, SW_HIDE);
	CloseHandle(process.hThread);
	WaitForSingleObject(process.hProcess, INFINITE);
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	/* (nothing of this process's to keep: its drivers' DLLs are not
	unloaded, which a process with a device can hang in) */
	TerminateProcess(GetCurrentProcess(), code);
}
