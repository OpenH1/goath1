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
*/

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>
#include <string.h>

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
int host_display_adapters(void (*each)(void *context, const char *name, unsigned vendor,
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
			each(context, name, description.VendorId, (unsigned long long)description.DedicatedVideoMemory,
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
