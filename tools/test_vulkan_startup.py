"""Vulkan startup handles Android device enumeration (the swapchain's rotation, surface
loss and present modes: port/linux/vulkan/tests/passes_test.c, tools/test_vulkan_passes.py)."""

from pathlib import Path
import shutil
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]


def test_physical_device_enumeration(tmp_path):
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        pytest.skip("needs a C compiler")
    text = (ROOT / "port/linux/vulkan/xgpu_vulkan.c").read_text()
    function = text[text.index("static int physical_choose("):
                    text.index("static int device_make(")]
    source = tmp_path / "devices.c"
    source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "xgpu_vulkan_gpu.c"
#define VK_API_VERSION_1_1 11
#define VK_API_VERSION_1_3 13
#define VK_API_VERSION_MAJOR(version) ((version) / 10)
#define VK_API_VERSION_MINOR(version) ((version) % 10)
#define VK_API_VERSION_PATCH(version) 0
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 2
#define VK_MEMORY_HEAP_DEVICE_LOCAL_BIT 1
#define VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU 2
#define VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU 1
#define VK_QUEUE_GRAPHICS_BIT 1
#define VK_TRUE 1
#define HOST_LOG_INFO 0
#define HOST_LOG_ERROR 1
typedef unsigned VkPhysicalDevice;
typedef unsigned VkBool32;
typedef struct {
    unsigned apiVersion, deviceType;
    char deviceName[32];
    struct { float timestampPeriod; } limits;
} VkPhysicalDeviceProperties;
typedef struct { unsigned sType; void *pNext; unsigned char deviceLUID[8]; unsigned deviceLUIDValid; }
    VkPhysicalDeviceIDProperties;
typedef struct { unsigned sType; void *pNext; } VkPhysicalDeviceProperties2;
typedef struct {
    unsigned memoryHeapCount;
    struct { unsigned long long size; unsigned flags; } memoryHeaps[2];
} VkPhysicalDeviceMemoryProperties;
typedef struct { unsigned queueFlags, timestampValidBits; } VkQueueFamilyProperties;
struct xgpu_vulkan_options {
    VkPhysicalDevice (*choose_physical)(unsigned);
    const char *gpu;
    int display_luid_known;
    unsigned char display_luid[8];
};
static struct {
    unsigned instance, surface, physical, queue_family, timestamp_bits, gpu_index, gpu_count;
    VkPhysicalDeviceMemoryProperties memory_properties;
    VkPhysicalDeviceProperties properties;
    struct { int gpu_timestamps; } capabilities;
} vk;
static unsigned available = 1, count_calls, fetch_calls, warnings;
static int count_failure, fetch_failure, unsupported;
static int check(int result, const char *name) { assert(name); return result >= 0; }
static void host_logf(int priority, const char *format, ...)
{ assert(format); if (priority == HOST_LOG_ERROR) warnings++; }
static int vkEnumeratePhysicalDevices(unsigned instance, unsigned *count, VkPhysicalDevice *devices)
{
    unsigned index;
    assert(instance == 42);
    if (!devices) {
        count_calls++;
        *count = available;
        return count_failure ? -1 : 0;
    }
    fetch_calls++;
    /* The phone's driver leaves an oversized count unchanged. Android's
       loader then reads unwritten handles and crashes before returning. */
    assert(count_calls && *count > 0 && *count <= available && *count <= 32);
    if (fetch_failure) return -1;
    for (index = 0; index < *count; index++) devices[index] = index + 1;
    return *count < available ? 5 : 0; /* VK_INCOMPLETE */
}
static void vkGetPhysicalDeviceProperties(VkPhysicalDevice device, VkPhysicalDeviceProperties *properties)
{
    assert(device > 0 && device <= available);
    properties->apiVersion = unsupported ? 12 : 13;
    properties->deviceType = device == 2 || device == 3 ? 2 : 1;
    properties->limits.timestampPeriod = 1.0f;
    snprintf(properties->deviceName, sizeof(properties->deviceName), "GPU %u", device);
}
static void vkGetPhysicalDeviceProperties2(VkPhysicalDevice device, VkPhysicalDeviceProperties2 *properties)
{
    VkPhysicalDeviceIDProperties *id = properties->pNext;
    assert(device && properties->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 && id &&
        id->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES);
    memset(id->deviceLUID, (int)device, 8);
    id->deviceLUIDValid = 1;
}
static void vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice device, unsigned *count,
    VkQueueFamilyProperties *families)
{
    assert(device && *count >= 1);
    *count = 1;
    families[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
    families[0].timestampValidBits = 64;
}
static void vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice device, unsigned family,
    unsigned surface, VkBool32 *present)
{ assert(device && !family && surface); *present = VK_TRUE; }
/* (device 3 the most memory, of a discrete GPU's type too) */
static void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice device, VkPhysicalDeviceMemoryProperties *properties)
{
    assert(device);
    properties->memoryHeapCount = 2;
    properties->memoryHeaps[0].size = device == 3 ? 4096 : 1024;
    properties->memoryHeaps[0].flags = VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
    properties->memoryHeaps[1].size = 1 << 20;
    properties->memoryHeaps[1].flags = 0;
}
static VkPhysicalDevice runtime_device(unsigned instance) { assert(instance == 42); return 1; }
''' + function + r'''
static void reset(unsigned devices)
{
    memset(&vk, 0, sizeof(vk));
    vk.instance = 42;
    available = devices;
    count_calls = fetch_calls = warnings = 0;
    count_failure = fetch_failure = unsupported = 0;
}
int main(void)
{
    struct xgpu_vulkan_options options;
    memset(&options, 0, sizeof(options));
    reset(1);
    assert(physical_choose(&options) && vk.physical == 1);
    assert(count_calls == 1 && fetch_calls == 1 && !warnings);
    assert(vk.capabilities.gpu_timestamps && vk.timestamp_bits == 64);
    reset(2);
    vk.surface = 1;
    assert(physical_choose(&options) && vk.physical == 2);
    /* (#1269: the integrated GPU listed first, the discrete second) */
    assert(vk.gpu_index == 1 && vk.gpu_count == 2);
    reset(3);
    assert(physical_choose(&options) && vk.physical == 3);
    reset(3);
    options.display_luid_known = 1;
    memset(options.display_luid, 2, 8);
    assert(physical_choose(&options) && vk.physical == 2);
    memset(options.display_luid, 1, 8);
    assert(physical_choose(&options) && vk.physical == 3);
    options.display_luid_known = 0;
    reset(3);
    options.gpu = "gpu 1";
    assert(physical_choose(&options) && vk.physical == 1);
    options.gpu = "0";
    assert(physical_choose(&options) && vk.physical == 1);
    options.gpu = "nothing";
    assert(physical_choose(&options) && vk.physical == 3);
    options.gpu = NULL;
    reset(40);
    assert(physical_choose(&options) && vk.physical == 3 && vk.gpu_count == 32);
    reset(0);
    assert(!physical_choose(&options) && count_calls == 1 && !fetch_calls && warnings == 1);
    reset(1);
    count_failure = 1;
    assert(!physical_choose(&options) && !fetch_calls);
    reset(1);
    fetch_failure = 1;
    assert(!physical_choose(&options) && fetch_calls == 1 && !vk.physical);
    reset(1);
    unsupported = 1;
    assert(!physical_choose(&options) && warnings == 1);
    reset(1);
    /* (VR: the runtime's device, whatever display.gpu says) */
    options.choose_physical = runtime_device;
    options.gpu = "2";
    assert(physical_choose(&options) && vk.physical == 1 && !count_calls && !fetch_calls);
    return 0;
}
''')
    binary = tmp_path / "devices"
    subprocess.run([compiler, "-std=gnu89", "-Wall", "-Wextra", "-Werror", f"-I{ROOT / 'port/linux/vulkan'}",
                    str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
