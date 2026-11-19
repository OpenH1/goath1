/*
OPENH1_DLSS.C

display.temporal_aa "dlss"'s module (xgpu_vulkan_dlss.h): the game's calls
into NVIDIA's NGX (the DLSS SDK's libnvsdk_ngx.a / nvsdk_ngx_s.lib, linked
into this module alone), which loads the driver's NGX core and the DLSS
runtime (libnvidia-ngx-dlss.so.<version> / nvngx_dlss.dll) staged beside this
module. configure.py --dlss builds it (tools/vulkan_build.py's
dlss_module), from the SDK fetched at configure time; the repository holds
none of NVIDIA's files.

- Identified to NGX by a project ID and the custom engine type (an NVIDIA
  application ID is not needed for that).
- DLSS Super Resolution: a feature a window, made for its render size and
  output size, its quality mode the one whose range of render sizes holds
  the render scale's (render scale 1: DLAA); the motion at the render's
  resolution, not jittered; the depth reversed; display.hdr's linear light
  marked HDR, exposure 1 (no exposure texture, no auto exposure).
*/

#define OPENH1_DLSS_MODULE 1

#include <vulkan/vulkan_core.h>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_vk.h"

#include "../xgpu_vulkan_dlss.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#define EXPORT __declspec(dllexport)
#else
#include <dlfcn.h>
#define EXPORT __attribute__((visibility("default")))
#endif

/* the project's ID for NGX (a GUID of its own: NGX's custom engine type) */
#define PROJECT_ID "6b1d3f0e-58a2-4c7e-9f14-2d8e0a7c4b95"
#define ENGINE_VERSION "1.0"

static struct
{
	int ready;
	VkDevice device;
	NVSDK_NGX_Parameter *parameters;
	void (*log)(int warning, const char *line);
	/* the module's folder (where the DLSS runtime is), and NGX's data
	folder, as wide strings */
	wchar_t folder[1024], data[1024];
	const wchar_t *paths[1];
	NVSDK_NGX_FeatureCommonInfo common;
	/* the extensions' names, kept */
	char names[32][VK_MAX_EXTENSION_NAME_SIZE];
} module;

static void say(int warning, const char *format, ...)
{
	char line[512];
	va_list arguments;

	if (!module.log)
		return;
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	module.log(warning, line);
}

static void NVSDK_CONV ngx_log(const char *message, NVSDK_NGX_Logging_Level level, NVSDK_NGX_Feature source)
{
	size_t length = message ? strlen(message) : 0;
	char line[512];

	(void)level;
	(void)source;
	if (!length)
		return;
	/* (its own line end left off) */
	snprintf(line, sizeof(line), "%.*s", (int)(message[length - 1] == '\n' ? length - 1 : length), message);
	say(0, "NGX: %s", line);
}

static void wide(wchar_t *to, size_t capacity, const char *from)
{
	size_t length = mbstowcs(to, from, capacity - 1);

	if (length == (size_t)-1)
		length = 0;
	to[length] = 0;
}

/* the module's folder, and NGX's common information: that folder searched
for the DLSS runtime, and the log (HALO_DLSS_LOG: NGX's lines too) */
static void common_make(const char *data_folder)
{
	char folder[1024];

	folder[0] = 0;
#ifdef _WIN32
	{
		HMODULE self = NULL;
		wchar_t path[1024];
		DWORD length;

		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			(LPCWSTR)(void *)common_make, &self) && (length = GetModuleFileNameW(self, path, 1024)) > 0 && length < 1024)
		{
			while (length > 0 && path[length - 1] != L'\\' && path[length - 1] != L'/')
				length--;
			path[length] = 0;
			wcsncpy(module.folder, path, 1023);
		}
	}
#else
	{
		Dl_info info;

		if (dladdr((void *)common_make, &info) && info.dli_fname)
		{
			char *slash;

			snprintf(folder, sizeof(folder), "%s", info.dli_fname);
			slash = strrchr(folder, '/');
			if (slash)
				*slash = 0;
			else
				snprintf(folder, sizeof(folder), ".");
		}
		wide(module.folder, 1024, folder[0] ? folder : ".");
	}
#endif
	wide(module.data, 1024, data_folder && *data_folder ? data_folder : ".");
	memset(&module.common, 0, sizeof(module.common));
	module.paths[0] = module.folder;
	module.common.PathListInfo.Path = module.paths;
	module.common.PathListInfo.Length = 1;
	module.common.LoggingInfo.LoggingCallback = ngx_log;
	module.common.LoggingInfo.MinimumLoggingLevel = getenv("HALO_DLSS_LOG") ? NVSDK_NGX_LOGGING_LEVEL_ON :
		NVSDK_NGX_LOGGING_LEVEL_OFF;
	module.common.LoggingInfo.DisableOtherLoggingSinks = getenv("HALO_DLSS_LOG") == NULL;
}

static void discovery(NVSDK_NGX_FeatureDiscoveryInfo *info)
{
	memset(info, 0, sizeof(*info));
	info->SDKVersion = NVSDK_NGX_Version_API;
	info->FeatureID = NVSDK_NGX_Feature_SuperSampling;
	info->Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
	info->Identifier.v.ProjectDesc.ProjectId = PROJECT_ID;
	info->Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
	info->Identifier.v.ProjectDesc.EngineVersion = ENGINE_VERSION;
	info->ApplicationDataPath = module.data;
	info->FeatureInfo = &module.common;
}

/* (the names kept from first, at most capacity) */
static uint32_t names_kept(const VkExtensionProperties *properties, uint32_t count, uint32_t first, const char **names,
	uint32_t capacity)
{
	uint32_t index, kept = 0;

	for (index = 0; index < count && kept < capacity && first + kept < 32; index++)
	{
		snprintf(module.names[first + kept], VK_MAX_EXTENSION_NAME_SIZE, "%s", properties[index].extensionName);
		names[kept] = module.names[first + kept];
		kept++;
	}
	return kept;
}

static uint32_t instance_extensions(const char **names, uint32_t capacity)
{
	NVSDK_NGX_FeatureDiscoveryInfo info;
	VkExtensionProperties *properties = NULL;
	uint32_t count = 0;

	if (!module.common.PathListInfo.Path)
		common_make(NULL);
	discovery(&info);
	if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &properties)))
		return 0;
	/* (the instance's names first in the list, the device's after) */
	return names_kept(properties, count, 0, names, capacity < 16 ? capacity : 16);
}

static uint32_t device_extensions(VkInstance instance, VkPhysicalDevice physical,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr, const char **names, uint32_t capacity)
{
	NVSDK_NGX_FeatureDiscoveryInfo info;
	VkExtensionProperties *properties = NULL;
	uint32_t count = 0;

	(void)get_instance_proc_addr;
	if (!module.common.PathListInfo.Path)
		common_make(NULL);
	discovery(&info);
	if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, physical, &info, &count,
		&properties)))
		return 0;
	return names_kept(properties, count, 16, names, capacity < 16 ? capacity : 16);
}

static int initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
	PFN_vkGetInstanceProcAddr get_instance_proc_addr, PFN_vkGetDeviceProcAddr get_device_proc_addr,
	const char *data_folder, void (*log)(int warning, const char *line))
{
	NVSDK_NGX_Result result;
	int available = 0, needs_driver = 0;

	module.log = log;
	common_make(data_folder);
	result = NVSDK_NGX_VULKAN_Init_with_ProjectID(PROJECT_ID, NVSDK_NGX_ENGINE_TYPE_CUSTOM, ENGINE_VERSION, module.data,
		instance, physical, device, get_instance_proc_addr, get_device_proc_addr, &module.common, NVSDK_NGX_Version_API);
	if (NVSDK_NGX_FAILED(result))
	{
		say(0, "NGX not begun (0x%08x): no NVIDIA driver's NGX here", (unsigned)result);
		return 0;
	}
	module.device = device;
	result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&module.parameters);
	if (NVSDK_NGX_FAILED(result) || !module.parameters)
	{
		say(1, "NGX's capabilities not read (0x%08x)", (unsigned)result);
		NVSDK_NGX_VULKAN_Shutdown1(device);
		module.parameters = NULL;
		return 0;
	}
	NVSDK_NGX_Parameter_GetI(module.parameters, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
	result = NVSDK_NGX_Parameter_GetI(module.parameters, NVSDK_NGX_Parameter_SuperSampling_Available, &available);
	if (NVSDK_NGX_FAILED(result) || !available)
	{
		int code = 0;

		NVSDK_NGX_Parameter_GetI(module.parameters, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &code);
		say(0, "DLSS Super Resolution not available (0x%08x%s)", (unsigned)code,
			needs_driver ? ", a newer driver needed" : "");
		NVSDK_NGX_VULKAN_DestroyParameters(module.parameters);
		NVSDK_NGX_VULKAN_Shutdown1(device);
		module.parameters = NULL;
		return 0;
	}
	module.ready = 1;
	return 1;
}

/* the quality mode whose range of render sizes holds the render size (the
optimal size nearest it); by the ratio where NGX gives no ranges */
static NVSDK_NGX_PerfQuality_Value mode_choose(const struct openh1_dlss_create *create)
{
	static const NVSDK_NGX_PerfQuality_Value modes[] = {
		NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced,
		NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_UltraPerformance };
	float ratio = (float)create->render[0] / (float)create->output[0];
	NVSDK_NGX_PerfQuality_Value chosen = ratio >= 0.99f ? NVSDK_NGX_PerfQuality_Value_DLAA :
		ratio >= 0.62f ? NVSDK_NGX_PerfQuality_Value_MaxQuality : ratio >= 0.54f ? NVSDK_NGX_PerfQuality_Value_Balanced :
		ratio >= 0.42f ? NVSDK_NGX_PerfQuality_Value_MaxPerf : NVSDK_NGX_PerfQuality_Value_UltraPerformance;
	void *callback = NULL;
	unsigned int best = 0xffffffffu;
	size_t index;

	NVSDK_NGX_Parameter_GetVoidPointer(module.parameters, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback, &callback);
	if (!callback)
		return chosen;
	for (index = 0; index < sizeof(modes) / sizeof(modes[0]); index++)
	{
		unsigned int optimal[2] = { 0, 0 }, low[2], high[2], distance;

		NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_Width, create->output[0]);
		NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_Height, create->output[1]);
		NVSDK_NGX_Parameter_SetI(module.parameters, NVSDK_NGX_Parameter_PerfQualityValue, (int)modes[index]);
		NVSDK_NGX_Parameter_SetI(module.parameters, NVSDK_NGX_Parameter_RTXValue, 0);
		if (NVSDK_NGX_FAILED(((PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback)callback)(module.parameters)))
			continue;
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_OutWidth, &optimal[0]);
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_OutHeight, &optimal[1]);
		if (!optimal[0] || !optimal[1])
			continue;
		low[0] = high[0] = optimal[0];
		low[1] = high[1] = optimal[1];
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width, &high[0]);
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height, &high[1]);
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width, &low[0]);
		NVSDK_NGX_Parameter_GetUI(module.parameters, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height, &low[1]);
		if (create->render[0] < low[0] || create->render[0] > high[0] || create->render[1] < low[1] ||
			create->render[1] > high[1])
			continue;
		distance = optimal[0] > create->render[0] ? optimal[0] - create->render[0] : create->render[0] - optimal[0];
		if (distance < best)
		{
			best = distance;
			chosen = modes[index];
		}
	}
	return chosen;
}

static void *feature_create(VkCommandBuffer commands, const struct openh1_dlss_create *create)
{
	static const char *const presets[] = {
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
		NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality };
	NVSDK_NGX_Handle *handle = NULL;
	NVSDK_NGX_PerfQuality_Value mode;
	NVSDK_NGX_Result result;
	int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
	size_t index;

	if (!module.ready || !create->render[0] || !create->render[1] || !create->output[0] || !create->output[1])
		return NULL;
	if (create->hdr)
		flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
	mode = mode_choose(create);
	for (index = 0; index < sizeof(presets) / sizeof(presets[0]); index++)
		NVSDK_NGX_Parameter_SetUI(module.parameters, presets[index], (unsigned int)create->preset);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_CreationNodeMask, 1);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_VisibilityNodeMask, 1);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_Width, create->render[0]);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_Height, create->render[1]);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_OutWidth, create->output[0]);
	NVSDK_NGX_Parameter_SetUI(module.parameters, NVSDK_NGX_Parameter_OutHeight, create->output[1]);
	NVSDK_NGX_Parameter_SetI(module.parameters, NVSDK_NGX_Parameter_PerfQualityValue, (int)mode);
	NVSDK_NGX_Parameter_SetI(module.parameters, NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, flags);
	NVSDK_NGX_Parameter_SetI(module.parameters, NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects, 0);
	result = NVSDK_NGX_VULKAN_CreateFeature1(module.device, commands, NVSDK_NGX_Feature_SuperSampling,
		module.parameters, &handle);
	if (NVSDK_NGX_FAILED(result) || !handle)
	{
		say(1, "a DLSS feature not made, %ux%u to %ux%u, mode %d (0x%08x)", create->render[0], create->render[1],
			create->output[0], create->output[1], (int)mode, (unsigned)result);
		return NULL;
	}
	say(0, "a DLSS feature made, %ux%u to %ux%u, mode %d%s", create->render[0], create->render[1], create->output[0],
		create->output[1], (int)mode, create->hdr ? ", HDR" : "");
	return handle;
}

static NVSDK_NGX_Resource_VK resource(const struct openh1_dlss_image *image, int written)
{
	NVSDK_NGX_Resource_VK made;

	memset(&made, 0, sizeof(made));
	made.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
	made.Resource.ImageViewInfo.ImageView = image->view;
	made.Resource.ImageViewInfo.Image = image->image;
	made.Resource.ImageViewInfo.SubresourceRange.aspectMask = image->aspect;
	made.Resource.ImageViewInfo.SubresourceRange.levelCount = 1;
	made.Resource.ImageViewInfo.SubresourceRange.layerCount = 1;
	made.Resource.ImageViewInfo.Format = image->format;
	made.Resource.ImageViewInfo.Width = image->width;
	made.Resource.ImageViewInfo.Height = image->height;
	made.ReadWrite = written != 0;
	return made;
}

static int feature_evaluate(VkCommandBuffer commands, void *feature, const struct openh1_dlss_frame *frame)
{
	NVSDK_NGX_Parameter *parameters = module.parameters;
	NVSDK_NGX_Resource_VK color, depth, motion, output;
	NVSDK_NGX_Result result;

	if (!module.ready || !feature)
		return 0;
	color = resource(&frame->color, 0);
	depth = resource(&frame->depth, 0);
	motion = resource(&frame->motion, 0);
	output = resource(&frame->output, 1);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_Color, &color);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_Output, &output);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_Depth, &depth);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_MotionVectors, &motion);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_TransparencyMask, NULL);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_ExposureTexture, NULL);
	NVSDK_NGX_Parameter_SetVoidPointer(parameters, NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask, NULL);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_X, frame->jitter[0]);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Jitter_Offset_Y, frame->jitter[1]);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_Sharpness, 0.0f);
	NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_Reset, frame->reset ? 1 : 0);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_X, frame->motion_scale[0]);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_MV_Scale_Y, frame->motion_scale[1]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_X, frame->corner[0]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_Color_Subrect_Base_Y, frame->corner[1]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, frame->corner[0]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, frame->corner[1]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, frame->corner[0]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, frame->corner[1]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_X, 0);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Output_Subrect_Base_Y, 0);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, frame->render[0]);
	NVSDK_NGX_Parameter_SetUI(parameters, NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, frame->render[1]);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Pre_Exposure, 1.0f);
	NVSDK_NGX_Parameter_SetF(parameters, NVSDK_NGX_Parameter_DLSS_Exposure_Scale, 1.0f);
	NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_X_Axis, 0);
	NVSDK_NGX_Parameter_SetI(parameters, NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis, 0);
	result = NVSDK_NGX_VULKAN_EvaluateFeature_C(commands, (NVSDK_NGX_Handle *)feature, parameters, NULL);
	if (NVSDK_NGX_FAILED(result))
	{
		say(1, "a DLSS frame not recorded (0x%08x)", (unsigned)result);
		return 0;
	}
	return 1;
}

static void feature_release(void *feature)
{
	if (feature)
		NVSDK_NGX_VULKAN_ReleaseFeature((NVSDK_NGX_Handle *)feature);
}

static void shutdown_device(VkDevice device)
{
	if (module.parameters)
		NVSDK_NGX_VULKAN_DestroyParameters(module.parameters);
	if (module.ready || module.device)
		NVSDK_NGX_VULKAN_Shutdown1(device);
	module.parameters = NULL;
	module.ready = 0;
	module.device = VK_NULL_HANDLE;
}

EXPORT const struct openh1_dlss *openh1_dlss(void)
{
	static const struct openh1_dlss calls = {
		OPENH1_DLSS_ABI, instance_extensions, device_extensions, initialize, feature_create, feature_evaluate,
		feature_release, shutdown_device };

	return &calls;
}
