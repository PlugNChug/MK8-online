// Windows x86-64 harness for the *runtime bridge*, not a reimplementation:
// it compiles runtime/mk8/graphics/dlss.cpp against a minimal vulkan/vk.h shim and calls
// mk8_dlss_upscale (Super Resolution) and mk8_dlss_fg_init/mk8_dlss_fg_evaluate (Frame
// Generation) with a real Vulkan device, synthetic colour/depth/motion vectors and a
// hidden Win32 swapchain for the FG pacing.
//
// It is the executable counterpart of tools/dlss_rtx_harness.cpp: that one validated the
// NGX entry-point ABI directly; this one exercises the code the game will call, including
// the Windows registry core lookup, the parameter vtable, core-side dispatch and the
// bridge's own logging and fail-closed paths.
//
// Build (mingw-w64, from the repository root; libvulkan-1.a generated from the Windows
// loader's exports):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DMK8_DLSS_TEST_API \
//       -Itools/dlss_bridge_include -Iruntime/mk8/graphics \
//       -I"$HOME/.cache/mk8-dlss-rtx/include" \
//       -o dlss_bridge_harness.exe tools/dlss_bridge_harness.cpp runtime/mk8/graphics/dlss.cpp \
//       -L"$HOME/.cache/mk8-dlss-rtx" -lvulkan-1 -luser32 -ladvapi32
//
// Run on the Windows machine with nvngx_dlss.dll / nvngx_dlssg.dll beside the exe:
//   dlss_bridge_harness.exe sr . out
//   dlss_bridge_harness.exe fg . out
//
// SR and FG are separate processes: the FG bridge refuses to run while MK8_DLSS (SR) is
// set, and NGX init is process-global. MK8_DLSS_TEST_API exposes the feature's output
// image to this harness; the runtime build does not define it.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR 1
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

// The bridge's own Vulkan surface. This shim must be included before dlss.h so the harness
// and the bridge agree on VkG/TexBackend; the real runtime uses runtime/nvn/vulkan/vk.h.
#include "vulkan/vk.h"
#include "dlss.h"
#include "mv_spirv.h"

VkG g_vk;

extern "C" {
float g_fsr_view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
float g_fsr_proj[16] = {1.30f, 0, 0, 0, 0, 1.732f, 0, 0, 0, 0, -1.0002f, -0.20002f, 0, 0, -1, 0};
int g_fsr_vp_valid = 1;
float g_fsr_jitter_x = 0.0f;
float g_fsr_jitter_y = 0.0f;
}

// ---------------------------------------------------------------------------------------
// Logging and small helpers
// ---------------------------------------------------------------------------------------
static void log_line(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("[harness] ", stdout);
    vfprintf(stdout, format, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
}

static void fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fputs("[harness] FAIL: ", stdout);
    vfprintf(stdout, format, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
    exit(1);
}

static std::string exe_directory(void) {
    char path[MAX_PATH * 2] = {0};
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (!length || length >= sizeof path) return std::string(".");
    std::string text(path);
    size_t slash = text.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : text.substr(0, slash);
}

static std::string join_path(const std::string &directory, const char *name) {
    if (directory.empty()) return std::string(name);
    char last = directory[directory.size() - 1];
    return directory + ((last == '\\' || last == '/') ? "" : "\\") + name;
}

static bool file_exists(const std::string &path) {
    DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void make_directory(const std::string &path) { CreateDirectoryA(path.c_str(), NULL); }

static uint8_t clamp_u8(float value) {
    if (value <= 0.0f) return 0;
    if (value >= 255.0f) return 255;
    return (uint8_t)(value + 0.5f);
}

// ---------------------------------------------------------------------------------------
// VMA subset: plain Vulkan allocations, enough for the bridge's images and its MV UBO.
// ---------------------------------------------------------------------------------------
struct VmaAllocation_T {
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;
};

static VkDeviceSize g_vma_device_total = 0;

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

VkResult vmaCreateImage(VmaAllocator, const VkImageCreateInfo *image_info,
                        const VmaAllocationCreateInfo *, VkImage *image, VmaAllocation *allocation,
                        VmaAllocationInfo *allocation_info_out) {
    VkResult result = vkCreateImage(g_vk.dev, image_info, NULL, image);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_vk.dev, *image, &requirements);
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (info.memoryTypeIndex == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VmaAllocation_T *allocation_data = new VmaAllocation_T;
    result = vkAllocateMemory(g_vk.dev, &info, NULL, &allocation_data->memory);
    if (result != VK_SUCCESS) {
        delete allocation_data;
        return result;
    }
    allocation_data->size = requirements.size;
    allocation_data->mapped = NULL;
    g_vma_device_total += requirements.size;
    result = vkBindImageMemory(g_vk.dev, *image, allocation_data->memory, 0);
    if (result != VK_SUCCESS) return result;
    if (allocation) *allocation = allocation_data;
    if (allocation_info_out) {
        memset(allocation_info_out, 0, sizeof *allocation_info_out);
        allocation_info_out->deviceMemory = allocation_data->memory;
        allocation_info_out->size = requirements.size;
    }
    return VK_SUCCESS;
}

VkResult vmaCreateBuffer(VmaAllocator, const VkBufferCreateInfo *buffer_info,
                         const VmaAllocationCreateInfo *allocation_create, VkBuffer *buffer,
                         VmaAllocation *allocation, VmaAllocationInfo *allocation_info_out) {
    VkResult result = vkCreateBuffer(g_vk.dev, buffer_info, NULL, buffer);
    if (result != VK_SUCCESS) return result;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_vk.dev, *buffer, &requirements);
    VkMemoryPropertyFlags wanted = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if (allocation_create && !(allocation_create->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT))
        wanted = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkMemoryAllocateInfo info = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = memory_type(requirements.memoryTypeBits, wanted);
    if (info.memoryTypeIndex == UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VmaAllocation_T *allocation_data = new VmaAllocation_T;
    result = vkAllocateMemory(g_vk.dev, &info, NULL, &allocation_data->memory);
    if (result != VK_SUCCESS) {
        delete allocation_data;
        return result;
    }
    allocation_data->size = requirements.size;
    allocation_data->mapped = NULL;
    g_vma_device_total += requirements.size;
    result = vkBindBufferMemory(g_vk.dev, *buffer, allocation_data->memory, 0);
    if (result != VK_SUCCESS) return result;
    if (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(g_vk.dev, allocation_data->memory, 0, VK_WHOLE_SIZE, 0, &allocation_data->mapped);
    log_line("vmaCreateBuffer: size=%llu host=%d mapped=%p", (unsigned long long)requirements.size,
             (wanted & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0, allocation_data->mapped);
    if (allocation) *allocation = allocation_data;
    if (allocation_info_out) {
        memset(allocation_info_out, 0, sizeof *allocation_info_out);
        allocation_info_out->deviceMemory = allocation_data->memory;
        allocation_info_out->size = requirements.size;
        allocation_info_out->pMappedData = allocation_data->mapped;
    }
    return VK_SUCCESS;
}

void vmaFreeMemory(VmaAllocator, VmaAllocation allocation) {
    if (!allocation) return;
    if (allocation->mapped) vkUnmapMemory(g_vk.dev, allocation->memory);
    if (allocation->memory) vkFreeMemory(g_vk.dev, allocation->memory, NULL);
    delete allocation;
}

// ---------------------------------------------------------------------------------------
// Bridge-support functions (normally provided by the runtime's Vulkan/FSR backend)
// ---------------------------------------------------------------------------------------
static VkSampler g_sampler_linear = VK_NULL_HANDLE;
static VkSampler g_sampler_nearest = VK_NULL_HANDLE;
static std::vector<VkDescriptorPool> g_descriptor_pools;

extern "C" VkImageView fsr_vk_image_view(TexBackend *backend, VkImageViewType type, VkFormat format,
                                         uint32_t level0, uint32_t level_count, uint32_t layer0,
                                         uint32_t layer_count, VkComponentMapping swizzle,
                                         VkImageAspectFlags aspect) {
    if (!backend || !backend->image) return VK_NULL_HANDLE;
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = backend->image;
    info.viewType = type;
    info.format = format;
    info.components = swizzle;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = level0;
    info.subresourceRange.levelCount = level_count;
    info.subresourceRange.baseArrayLayer = layer0;
    info.subresourceRange.layerCount = layer_count;
    VkImageView view = VK_NULL_HANDLE;
    return vkCreateImageView(g_vk.dev, &info, NULL, &view) == VK_SUCCESS ? view : VK_NULL_HANDLE;
}

extern "C" VkSampler fsr_vk_default_sampler(bool linear) {
    VkSampler *slot = linear ? &g_sampler_linear : &g_sampler_nearest;
    if (*slot) return *slot;
    VkSamplerCreateInfo info = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info.minFilter = info.magFilter;
    info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    info.maxLod = VK_LOD_CLAMP_NONE;
    if (vkCreateSampler(g_vk.dev, &info, NULL, slot) != VK_SUCCESS) return VK_NULL_HANDLE;
    return *slot;
}

extern "C" VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout layout) {
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
    };
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = (uint32_t)(sizeof sizes / sizeof sizes[0]);
    pool_info.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g_vk.dev, &pool_info, NULL, &pool) != VK_SUCCESS) {
        log_line("fsr_vk_alloc_set: vkCreateDescriptorPool failed");
        return VK_NULL_HANDLE;
    }
    g_descriptor_pools.push_back(pool);
    VkDescriptorSetAllocateInfo allocate = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    return vkAllocateDescriptorSets(g_vk.dev, &allocate, &set) == VK_SUCCESS ? set : VK_NULL_HANDLE;
}

extern "C" VkShaderModule fsr_vk_compile_glsl(const char *source, int stage, const char *name) {
    (void)source;
    (void)stage;
    if (!name || strcmp(name, "dlss_mv") != 0) return VK_NULL_HANDLE;
    VkShaderModuleCreateInfo info = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = sizeof kMk8DlssMvSpirv;
    info.pCode = kMk8DlssMvSpirv;
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult result = vkCreateShaderModule(g_vk.dev, &info, NULL, &module);
    log_line("fsr_vk_compile_glsl(%s, stage %d): result=%d module=%p", name, stage, result, (void *)module);
    return result == VK_SUCCESS ? module : VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------------------
// Vulkan device, hidden Win32 surface and swapchain
// ---------------------------------------------------------------------------------------
static VkInstance g_instance = VK_NULL_HANDLE;
static VkSurfaceKHR g_surface = VK_NULL_HANDLE;
static VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
static VkFormat g_swapchain_format = VK_FORMAT_UNDEFINED;
static VkExtent2D g_swapchain_extent = {1280, 720};
static HWND g_window = NULL;
static std::vector<std::string> g_device_extensions;

static bool has_extension(const char *name) {
    for (const std::string &extension : g_device_extensions)
        if (extension == name) return true;
    return false;
}

static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    (void)window;
    (void)message;
    (void)wparam;
    (void)lparam;
    return DefWindowProcA(window, message, wparam, lparam);
}

static void create_hidden_window(void) {
    HINSTANCE instance = GetModuleHandleA(NULL);
    WNDCLASSA window_class = {0};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = "Mk8DlssBridgeHarness";
    if (!RegisterClassA(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        fail("RegisterClassA failed: %lu", (unsigned long)GetLastError());
    g_window = CreateWindowExA(0, window_class.lpszClassName, "mk8 dlss bridge harness", WS_POPUP, 0, 0, 1280,
                               720, NULL, NULL, instance, NULL);
    if (!g_window) fail("CreateWindowExA failed: %lu", (unsigned long)GetLastError());
}

static void create_instance(void) {
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "mk8-dlss-bridge-harness";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "mk8-online";
    application.engineVersion = VK_MAKE_VERSION(4, 0, 1);
    application.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = 3;
    info.ppEnabledExtensionNames = extensions;
    if (vkCreateInstance(&info, NULL, &g_instance) != VK_SUCCESS) fail("vkCreateInstance failed");
    VkWin32SurfaceCreateInfoKHR surface_info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surface_info.hinstance = GetModuleHandleA(NULL);
    surface_info.hwnd = g_window;
    if (vkCreateWin32SurfaceKHR(g_instance, &surface_info, NULL, &g_surface) != VK_SUCCESS)
        fail("vkCreateWin32SurfaceKHR failed");
}

static void create_device(void) {
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(g_instance, &device_count, NULL) != VK_SUCCESS || !device_count)
        fail("no Vulkan physical devices");
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(g_instance, &device_count, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(device, &props);
        log_line("physical device: %s", props.deviceName);
        if (props.vendorID == 0x10DE && !physical) physical = device;
    }
    if (!physical) physical = devices[0];
    g_vk.phys = physical;
    vkGetPhysicalDeviceProperties(physical, &g_vk.props);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, g_surface, &present);
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
                (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT) &&
            present) {
            family = i;
            break;
        }
    }
    if (family == UINT32_MAX) fail("no graphics+compute+present queue family");
    g_vk.qfam = family;

    uint32_t extension_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, NULL);
    std::vector<VkExtensionProperties> extensions(extension_count);
    vkEnumerateDeviceExtensionProperties(physical, NULL, &extension_count, extensions.data());
    for (const VkExtensionProperties &extension : extensions) g_device_extensions.push_back(extension.extensionName);

    const char *wanted[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        "VK_NV_low_latency2",
        "VK_NVX_binary_import",
        "VK_NVX_image_view_handle",
        "VK_KHR_buffer_device_address",
        "VK_KHR_push_descriptor",
    };
    std::vector<const char *> enabled;
    for (const char *name : wanted) {
        if (has_extension(name))
            enabled.push_back(name);
        else
            log_line("device extension not available: %s", name);
    }
    if (!has_extension("VK_NVX_binary_import") || !has_extension("VK_NVX_image_view_handle") ||
        !has_extension("VK_KHR_buffer_device_address") || !has_extension("VK_KHR_push_descriptor"))
        fail("the device lacks an extension NGX requires (see the list above)");

    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(physical, &supported);
    VkPhysicalDeviceVulkan12Features available12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features available13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &available12;
    available12.pNext = &available13;
    vkGetPhysicalDeviceFeatures2(physical, &features2);

    VkPhysicalDeviceVulkan12Features enabled12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.bufferDeviceAddress = available12.bufferDeviceAddress;
    enabled12.descriptorIndexing = available12.descriptorIndexing;
    enabled12.shaderSampledImageArrayNonUniformIndexing = available12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.shaderStorageImageArrayNonUniformIndexing =
        available12.shaderStorageImageArrayNonUniformIndexing;
    enabled12.descriptorBindingSampledImageUpdateAfterBind =
        available12.descriptorBindingSampledImageUpdateAfterBind;
    enabled12.descriptorBindingStorageImageUpdateAfterBind =
        available12.descriptorBindingStorageImageUpdateAfterBind;
    enabled12.descriptorBindingPartiallyBound = available12.descriptorBindingPartiallyBound;
    enabled12.runtimeDescriptorArray = available12.runtimeDescriptorArray;
    enabled12.timelineSemaphore = available12.timelineSemaphore;
    enabled12.scalarBlockLayout = available12.scalarBlockLayout;
    VkPhysicalDeviceVulkan13Features enabled13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.synchronization2 = available13.synchronization2;
    enabled13.dynamicRendering = available13.dynamicRendering;
    enabled13.maintenance4 = available13.maintenance4;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = &enabled12;
    enabled12.pNext = &enabled13;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = (uint32_t)enabled.size();
    info.ppEnabledExtensionNames = enabled.data();
    info.pEnabledFeatures = &supported;
    if (vkCreateDevice(physical, &info, NULL, &g_vk.dev) != VK_SUCCESS) fail("vkCreateDevice failed");
    vkGetDeviceQueue(g_vk.dev, family, 0, &g_vk.queue);
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    if (vkCreateCommandPool(g_vk.dev, &pool_info, NULL, &g_vk.pool) != VK_SUCCESS)
        fail("vkCreateCommandPool failed");

    g_vk.inst = g_instance;
    g_vk.surface = g_surface;
    log_line("device: %s, queue family %u, %u extensions", g_vk.props.deviceName, family,
             (unsigned)enabled.size());
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_surface, &caps) != VK_SUCCESS)
        fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_surface, &format_count, NULL);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_surface, &format_count, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                    VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    for (const VkSurfaceFormatKHR &format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = format;
            break;
        }
    }
    g_swapchain_format = chosen.format;
    uint32_t width = caps.currentExtent.width == UINT32_MAX ? 1280 : caps.currentExtent.width;
    uint32_t height = caps.currentExtent.height == UINT32_MAX ? 720 : caps.currentExtent.height;
    if (width > 1280) width = 1280;
    if (height > 720) height = 720;
    g_swapchain_extent = {width, height};
    VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = g_surface;
    info.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = g_swapchain_extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    info.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_vk.dev, &info, NULL, &g_swapchain) != VK_SUCCESS)
        fail("vkCreateSwapchainKHR failed");
    g_vk.swapchain = g_swapchain;
    g_vk.sc_format = g_swapchain_format;
    g_vk.sc_extent = g_swapchain_extent;
    log_line("swapchain: %ux%u, format %d (hidden window, FIFO)", width, height, (int)g_swapchain_format);
}

// ---------------------------------------------------------------------------------------
// Images, uploads and readback
// ---------------------------------------------------------------------------------------
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkDeviceSize size = 0;
};

static Image create_image(VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect) {
    Image out;
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_vk.dev, &info, NULL, &out.image) != VK_SUCCESS) fail("vkCreateImage failed");
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(g_vk.dev, out.image, &requirements);
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_vk.dev, &allocation, NULL, &out.memory) != VK_SUCCESS)
        fail("vkAllocateMemory(image) failed");
    if (vkBindImageMemory(g_vk.dev, out.image, out.memory, 0) != VK_SUCCESS)
        fail("vkBindImageMemory failed");
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g_vk.dev, &view, NULL, &out.view) != VK_SUCCESS)
        fail("vkCreateImageView failed");
    out.format = format;
    out.width = width;
    out.height = height;
    out.aspect = aspect;
    out.size = requirements.size;
    return out;
}

static void destroy_image(Image *image) {
    if (image->view) vkDestroyImageView(g_vk.dev, image->view, NULL);
    if (image->image) vkDestroyImage(g_vk.dev, image->image, NULL);
    if (image->memory) vkFreeMemory(g_vk.dev, image->memory, NULL);
    memset(image, 0, sizeof *image);
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier info = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    info.srcAccessMask = src_access;
    info.dstAccessMask = dst_access;
    info.oldLayout = old_layout;
    info.newLayout = new_layout;
    info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    info.image = image;
    info.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &info);
}

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkDeviceSize size = 0;
};

static Buffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    Buffer out;
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_vk.dev, &info, NULL, &out.buffer) != VK_SUCCESS) fail("vkCreateBuffer failed");
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(g_vk.dev, out.buffer, &requirements);
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, properties);
    if (vkAllocateMemory(g_vk.dev, &allocation, NULL, &out.memory) != VK_SUCCESS)
        fail("vkAllocateMemory(buffer) failed");
    if (vkBindBufferMemory(g_vk.dev, out.buffer, out.memory, 0) != VK_SUCCESS)
        fail("vkBindBufferMemory failed");
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        vkMapMemory(g_vk.dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.mapped);
    out.size = size;
    return out;
}

static void destroy_buffer(Buffer *buffer) {
    if (buffer->mapped) vkUnmapMemory(g_vk.dev, buffer->memory);
    if (buffer->buffer) vkDestroyBuffer(g_vk.dev, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(g_vk.dev, buffer->memory, NULL);
    memset(buffer, 0, sizeof *buffer);
}

static VkCommandBuffer begin_commands(VkFence *fence) {
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g_vk.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(g_vk.dev, &allocate, &cb) != VK_SUCCESS) fail("vkAllocateCommandBuffers failed");
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cb, &begin) != VK_SUCCESS) fail("vkBeginCommandBuffer failed");
    if (fence) {
        VkFenceCreateInfo info = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkCreateFence(g_vk.dev, &info, NULL, fence) != VK_SUCCESS) fail("vkCreateFence failed");
    }
    return cb;
}

static void submit_and_wait(VkCommandBuffer cb, VkFence fence) {
    if (vkEndCommandBuffer(cb) != VK_SUCCESS) fail("vkEndCommandBuffer failed");
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (vkQueueSubmit(g_vk.queue, 1, &submit, fence) != VK_SUCCESS) fail("vkQueueSubmit failed");
    if (fence) {
        if (vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS)
            fail("vkWaitForFences failed");
        vkDestroyFence(g_vk.dev, fence, NULL);
    }
    vkFreeCommandBuffers(g_vk.dev, g_vk.pool, 1, &cb);
}

// Upload a tightly packed CPU image into a GENERAL-layout image. The staging buffer is
// created once by the caller and stays alive until after the frame's submission.
static void upload(VkCommandBuffer cb, Image *image, Buffer *staging, const void *data, size_t bytes) {
    memcpy(staging->mapped, data, bytes);
    if (image->layout != VK_IMAGE_LAYOUT_GENERAL)
        barrier(cb, image->image, image->aspect, image->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    else
        barrier(cb, image->image, image->aspect, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = image->aspect;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {image->width, image->height, 1};
    vkCmdCopyBufferToImage(cb, staging->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, image->image, image->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    image->layout = VK_IMAGE_LAYOUT_GENERAL;
}

// Copy a GENERAL-layout image into a host buffer and restore GENERAL.
static void readback(VkCommandBuffer cb, Image *image, Buffer *destination) {
    barrier(cb, image->image, image->aspect, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = image->aspect;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {image->width, image->height, 1};
    vkCmdCopyImageToBuffer(cb, image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination->buffer, 1, &region);
    barrier(cb, image->image, image->aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

// ---------------------------------------------------------------------------------------
// Statistics and PNG (RGBA8; BGRA is swizzled on the way in)
// ---------------------------------------------------------------------------------------
struct Stats {
    uint64_t bytes = 0;
    uint64_t nonzero = 0;
    uint64_t checksum = 1469598103934665603ull;
    uint32_t min_channel[4] = {255, 255, 255, 255};
    uint32_t max_channel[4] = {0, 0, 0, 0};
    double mean_channel[4] = {0, 0, 0, 0};
    uint32_t sentinel_pixels = 0;
    uint32_t distinct = 0;
    double gradient = 0.0;
};

static void swizzle_bgra_to_rgba(const uint8_t *source, uint8_t *destination, size_t pixels) {
    for (size_t i = 0; i < pixels; i++) {
        destination[i * 4 + 0] = source[i * 4 + 2];
        destination[i * 4 + 1] = source[i * 4 + 1];
        destination[i * 4 + 2] = source[i * 4 + 0];
        destination[i * 4 + 3] = source[i * 4 + 3];
    }
}

static Stats compute_stats(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t sentinel) {
    Stats stats;
    size_t pixels = (size_t)width * height;
    stats.bytes = pixels * 4;
    double sums[4] = {0, 0, 0, 0};
    std::vector<uint32_t> values(pixels);
    for (size_t i = 0; i < pixels; i++) {
        uint32_t pixel = 0;
        for (int c = 0; c < 4; c++) {
            uint8_t value = rgba[i * 4 + c];
            stats.min_channel[c] = std::min<uint32_t>(stats.min_channel[c], value);
            stats.max_channel[c] = std::max<uint32_t>(stats.max_channel[c], value);
            sums[c] += value;
            if (value) stats.nonzero++;
            pixel |= (uint32_t)value << (c * 8);
        }
        values[i] = pixel;
        if (pixel == sentinel) stats.sentinel_pixels++;
        stats.checksum ^= pixel;
        stats.checksum *= 1099511628211ull;
    }
    for (int c = 0; c < 4; c++) stats.mean_channel[c] = sums[c] / (double)pixels;
    std::sort(values.begin(), values.end());
    stats.distinct = (uint32_t)(std::unique(values.begin(), values.end()) - values.begin());
    double gradient = 0.0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = rgba + (size_t)y * width * 4;
        for (uint32_t x = 1; x < width; x++)
            for (int c = 0; c < 3; c++)
                gradient += fabs((double)row[x * 4 + c] - (double)row[(x - 1) * 4 + c]);
    }
    stats.gradient = gradient / (double)((size_t)(width - 1) * height * 3);
    return stats;
}

static void print_stats(const char *label, const Stats &stats, uint32_t total_pixels) {
    log_line("%s: bytes=%llu nonzero=%llu sentinel=%u/%u distinct=%u checksum=0x%016llx", label,
             (unsigned long long)stats.bytes, (unsigned long long)stats.nonzero, stats.sentinel_pixels,
             total_pixels, stats.distinct, (unsigned long long)stats.checksum);
    log_line("%s: min/max R=%u/%u G=%u/%u B=%u/%u A=%u/%u mean=%.2f/%.2f/%.2f/%.2f gradient=%.3f", label,
             stats.min_channel[0], stats.max_channel[0], stats.min_channel[1], stats.max_channel[1],
             stats.min_channel[2], stats.max_channel[2], stats.min_channel[3], stats.max_channel[3],
             stats.mean_channel[0], stats.mean_channel[1], stats.mean_channel[2], stats.mean_channel[3],
             stats.gradient);
}

static uint32_t g_crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[n] = c;
    }
}
static uint32_t crc_update(uint32_t crc, const uint8_t *data, size_t length) {
    for (size_t i = 0; i < length; i++) crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}
static uint32_t adler_update(uint32_t adler, const uint8_t *data, size_t length) {
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}
static void put_be32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back((uint8_t)(value >> 24));
    out.push_back((uint8_t)(value >> 16));
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}
static void png_chunk(FILE *file, const char type[4], const std::vector<uint8_t> &data) {
    std::vector<uint8_t> header;
    put_be32(header, (uint32_t)data.size());
    fwrite(header.data(), 1, header.size(), file);
    fwrite(type, 1, 4, file);
    if (!data.empty()) fwrite(data.data(), 1, data.size(), file);
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc_update(crc, (const uint8_t *)type, 4);
    if (!data.empty()) crc = crc_update(crc, data.data(), data.size());
    put_be32(header, crc ^ 0xFFFFFFFFu);
    fwrite(header.data() + 4, 1, 4, file);
}
static bool write_png(const std::string &path, const uint8_t *rgba, uint32_t width, uint32_t height) {
    if (!rgba || !width || !height) return false;
    size_t row_bytes = (size_t)width * 4;
    std::vector<uint8_t> filtered;
    filtered.reserve((row_bytes + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        filtered.push_back(0);
        filtered.insert(filtered.end(), rgba + (size_t)y * row_bytes, rgba + (size_t)(y + 1) * row_bytes);
    }
    std::vector<uint8_t> idat;
    idat.reserve(filtered.size() + filtered.size() / 65535 * 5 + 64);
    idat.push_back(0x78);
    idat.push_back(0x01);
    size_t offset = 0;
    while (offset < filtered.size()) {
        size_t block = std::min<size_t>(65535, filtered.size() - offset);
        bool last = offset + block == filtered.size();
        idat.push_back(last ? 1 : 0);
        idat.push_back((uint8_t)(block & 0xFF));
        idat.push_back((uint8_t)(block >> 8));
        uint16_t inverse = (uint16_t)~(uint16_t)block;
        idat.push_back((uint8_t)(inverse & 0xFF));
        idat.push_back((uint8_t)(inverse >> 8));
        idat.insert(idat.end(), filtered.begin() + (ptrdiff_t)offset,
                    filtered.begin() + (ptrdiff_t)(offset + block));
        offset += block;
    }
    put_be32(idat, adler_update(1, filtered.data(), filtered.size()));
    FILE *file = fopen(path.c_str(), "wb");
    if (!file) return false;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(signature, 1, sizeof signature, file);
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.insert(ihdr.end(), 3, 0);
    png_chunk(file, "IHDR", ihdr);
    png_chunk(file, "IDAT", idat);
    png_chunk(file, "IEND", std::vector<uint8_t>());
    fclose(file);
    return true;
}

// ---------------------------------------------------------------------------------------
// Synthetic frames
// ---------------------------------------------------------------------------------------
static float halton(int index, int base) {
    float result = 0.0f, fraction = 1.0f;
    int value = index;
    while (value > 0) {
        fraction /= (float)base;
        result += fraction * (float)(value % base);
        value /= base;
    }
    return result;
}

static void fill_color(uint32_t width, uint32_t height, int frame, uint8_t *rgba) {
    int shift = frame * 3;
    for (uint32_t y = 0; y < height; y++) {
        float fy = (float)y / (float)(height - 1);
        for (uint32_t x = 0; x < width; x++) {
            int wx = (int)x - shift;
            wx %= 256;
            if (wx < 0) wx += 256;
            float r = 24.0f + 190.0f * fy;
            float g = 40.0f + 140.0f * (1.0f - fy);
            float b = 80.0f + 100.0f * fy;
            if (fy > 0.5f && (((wx / 8) + (int)(y / 8)) & 1)) {
                r += 35.0f;
                g += 60.0f;
                b += 25.0f;
            }
            if ((wx % 32) < 2) {
                r = 240.0f;
                g = 235.0f;
                b = 210.0f;
            }
            if (fy > 0.78f) {
                r = 38.0f + (float)(wx % 16) * 5.0f;
                g = 44.0f;
                b = 54.0f;
            }
            size_t p = ((size_t)y * width + x) * 4;
            rgba[p + 0] = clamp_u8(r);
            rgba[p + 1] = clamp_u8(g);
            rgba[p + 2] = clamp_u8(b);
            rgba[p + 3] = 255;
        }
    }
}

static void fill_depth(uint32_t width, uint32_t height, float *depth) {
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            depth[(size_t)y * width + x] = 0.15f + 0.8f * ((float)y / (float)(height - 1));
}

static void fill_motion(uint32_t width, uint32_t height, int frame, float *motion) {
    float dx = (float)(frame * 3);
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            motion[((size_t)y * width + x) * 2 + 0] = -dx;
            motion[((size_t)y * width + x) * 2 + 1] = 0.0f;
        }
}

// ---------------------------------------------------------------------------------------
// Super Resolution through mk8_dlss_upscale
// ---------------------------------------------------------------------------------------
static const uint32_t kSentinel = 0xFFFF00FFu;  // A=FF B=00 G=00 R=FF

static void run_sr(const std::string &output_dir) {
    const uint32_t rw = 1280, rh = 720, ow = 2560, oh = 1440;
    const VkDeviceSize color_bytes = (VkDeviceSize)rw * rh * 4;
    const VkDeviceSize depth_bytes = (VkDeviceSize)rw * rh * 4;
    Image color = create_image(VK_FORMAT_R8G8B8A8_UNORM, rw, rh,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = create_image(VK_FORMAT_R32_SFLOAT, rw, rh,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    TexBackend color_backend = {};
    color_backend.image = color.image;
    color_backend.format = color.format;
    color_backend.width = rw;
    color_backend.height = rh;
    color_backend.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    TexBackend depth_backend = {};
    depth_backend.image = depth.image;
    depth_backend.format = depth.format;
    depth_backend.width = rw;
    depth_backend.height = rh;
    depth_backend.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    uint8_t *cpu_color = (uint8_t *)malloc((size_t)color_bytes);
    float *cpu_depth = (float *)malloc((size_t)depth_bytes);
    if (!cpu_color || !cpu_depth) fail("out of host memory");
    Buffer readback_buffer = create_buffer((VkDeviceSize)ow * oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer color_staging = create_buffer(color_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer depth_staging = create_buffer(depth_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::vector<uint8_t> rgba((size_t)ow * oh * 4);
    uint8_t *last_output = (uint8_t *)malloc((size_t)ow * oh * 4);
    if (!last_output) fail("out of host memory");
    Stats last_stats;
    bool have_output = false;
    VkImage output_image = VK_NULL_HANDLE;
    const int frames = 6;

    for (int frame = 0; frame < frames; frame++) {
        fill_color(rw, rh, frame, cpu_color);
        fill_depth(rw, rh, cpu_depth);
        g_fsr_jitter_x = halton(frame + 1, 2) - 0.5f;
        g_fsr_jitter_y = halton(frame + 1, 3) - 0.5f;
        g_fsr_view[3] = -(float)frame * 0.01f;

        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cb = begin_commands(&fence);
        upload(cb, &color, &color_staging, cpu_color, (size_t)color_bytes);
        upload(cb, &depth, &depth_staging, cpu_depth, (size_t)depth_bytes);
        if (output_image) {
            // Prove the feature wrote the frame: clear its image to magenta first.
            barrier(cb, output_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearColorValue clear = {};
            clear.float32[0] = 1.0f;
            clear.float32[1] = 0.0f;
            clear.float32[2] = 1.0f;
            clear.float32[3] = 1.0f;
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, output_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            barrier(cb, output_image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT |
                        VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        VkImageView view = mk8_dlss_upscale(cb, &color_backend, &depth_backend, rw, rh, ow, oh, 16.6f);
        if (view == VK_NULL_HANDLE) {
            submit_and_wait(cb, fence);
            fail("mk8_dlss_upscale returned VK_NULL_HANDLE on frame %d (see the bridge log above)", frame);
        }
        output_image = mk8_dlss_test_output_image();
        if (!output_image) {
            submit_and_wait(cb, fence);
            fail("mk8_dlss_test_output_image returned null after a successful upscale");
        }
        Image output = {};
        output.image = output_image;
        output.format = mk8_dlss_test_output_format();
        output.width = ow;
        output.height = oh;
        output.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        readback(cb, &output, &readback_buffer);
        submit_and_wait(cb, fence);

        const uint8_t *pixels = (const uint8_t *)readback_buffer.mapped;
        Stats stats = compute_stats(pixels, ow, oh, kSentinel);
        print_stats("sr", stats, ow * oh);
        if (frame > 0 && stats.sentinel_pixels != 0)
            log_line("sr frame %d: WARNING: %u pixels still equal the magenta sentinel", frame,
                     stats.sentinel_pixels);
        memcpy(last_output, pixels, (size_t)ow * oh * 4);
        last_stats = stats;
        have_output = true;
    }

    if (have_output) {
        std::string path = join_path(output_dir, "bridge_sr_2560x1440.png");
        if (write_png(path, last_output, ow, oh)) log_line("wrote %s", path.c_str());
        write_png(join_path(output_dir, "bridge_sr_input_1280x720.png"), cpu_color, rw, rh);
        bool ok = last_stats.sentinel_pixels == 0 && last_stats.distinct > 64 &&
                  last_stats.nonzero > 0;
        log_line("SR verdict: %s (distinct=%u sentinel=%u gradient=%.3f)",
                 ok ? "REAL EVALUATED FRAME" : "NOT A REAL FRAME", last_stats.distinct,
                 last_stats.sentinel_pixels, last_stats.gradient);
        if (!ok) exit(2);
    }
    free(cpu_color);
    free(cpu_depth);
    free(last_output);
    destroy_buffer(&depth_staging);
    destroy_buffer(&color_staging);
    destroy_buffer(&readback_buffer);
    destroy_image(&depth);
    destroy_image(&color);
}

// ---------------------------------------------------------------------------------------
// Frame Generation through mk8_dlss_fg_init / mk8_dlss_fg_evaluate
// ---------------------------------------------------------------------------------------
static void run_fg(const std::string &output_dir) {
    const uint32_t display_w = 1280, display_h = 720;
    const uint32_t render_w = 960, render_h = 540;
    VkFormat format = g_swapchain_format;
    if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_R8G8B8A8_UNORM)
        fail("unsupported swapchain format %d for the FG harness", (int)format);
    bool bgra = format == VK_FORMAT_B8G8R8A8_UNORM;

    Image backbuffer = create_image(format, display_w, display_h,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = create_image(VK_FORMAT_R32_SFLOAT, render_w, render_h,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    Image motion = create_image(VK_FORMAT_R32G32_SFLOAT, render_w, render_h,
                                VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT);
    Buffer back_readback = create_buffer((VkDeviceSize)display_w * display_h * 4,
                                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer interp_readback = create_buffer((VkDeviceSize)display_w * display_h * 4,
                                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer back_staging = create_buffer((VkDeviceSize)display_w * display_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer depth_staging = create_buffer((VkDeviceSize)render_w * render_h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    Buffer motion_staging = create_buffer((VkDeviceSize)render_w * render_h * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    uint8_t *cpu_color = (uint8_t *)malloc((size_t)display_w * display_h * 4);
    float *cpu_depth = (float *)malloc((size_t)render_w * render_h * 4);
    float *cpu_motion = (float *)malloc((size_t)render_w * render_h * 8);
    std::vector<uint8_t> back_rgba((size_t)display_w * display_h * 4);
    std::vector<uint8_t> interp_rgba((size_t)display_w * display_h * 4);
    uint8_t *last_interp = (uint8_t *)malloc((size_t)display_w * display_h * 4);
    if (!cpu_color || !cpu_depth || !cpu_motion || !last_interp) fail("out of host memory");

    Mk8DlssFgInit init = {};
    init.instance = g_instance;
    init.physical_device = g_vk.phys;
    init.device = g_vk.dev;
    init.present_queue = g_vk.queue;
    init.command_buffer = VK_NULL_HANDLE;
    init.swapchain = g_swapchain;
    init.format = format;
    init.width = display_w;
    init.height = display_h;
    init.render_width = render_w;
    init.render_height = render_h;
    init.color_hdr = false;
    init.depth_inverted = false;
    log_line("calling mk8_dlss_fg_init (%ux%u, render %ux%u, swapchain format %d)", display_w, display_h,
             render_w, render_h, (int)format);
    if (!mk8_dlss_fg_init(&init)) fail("mk8_dlss_fg_init returned false (see the bridge log above)");
    log_line("mk8_dlss_fg_ready = %d", (int)mk8_dlss_fg_ready());

    float projection[16] = {1.30f, 0, 0, 0, 0, 1.732f, 0, 0, 0, 0, -1.0002f, -0.20002f,
                            0, 0, -1, 0};
    float view[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    Stats last_stats;
    bool have_frame = false;
    const int frames = 3;
    for (int frame = 0; frame < frames; frame++) {
        fill_color(display_w, display_h, frame, cpu_color);
        fill_depth(render_w, render_h, cpu_depth);
        fill_motion(render_w, render_h, frame, cpu_motion);
        view[3] = -(float)frame * 0.02f;

        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer cb = begin_commands(&fence);
        upload(cb, &backbuffer, &back_staging, cpu_color, (size_t)display_w * display_h * 4);
        upload(cb, &depth, &depth_staging, cpu_depth, (size_t)render_w * render_h * 4);
        upload(cb, &motion, &motion_staging, cpu_motion, (size_t)render_w * render_h * 8);

        Mk8DlssFgInputs inputs = {};
        inputs.command_buffer = cb;
        inputs.backbuffer = backbuffer.image;
        inputs.backbuffer_view = backbuffer.view;
        inputs.backbuffer_format = backbuffer.format;
        inputs.backbuffer_layout = VK_IMAGE_LAYOUT_GENERAL;
        inputs.backbuffer_width = display_w;
        inputs.backbuffer_height = display_h;
        inputs.depth = depth.image;
        inputs.depth_view = depth.view;
        inputs.depth_format = depth.format;
        inputs.depth_layout = VK_IMAGE_LAYOUT_GENERAL;
        inputs.depth_width = render_w;
        inputs.depth_height = render_h;
        inputs.depth_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        inputs.motion = motion.image;
        inputs.motion_view = motion.view;
        inputs.motion_format = motion.format;
        inputs.motion_layout = VK_IMAGE_LAYOUT_GENERAL;
        inputs.motion_width = render_w;
        inputs.motion_height = render_h;
        inputs.jitter_x = halton(frame + 1, 2) - 0.5f;
        inputs.jitter_y = halton(frame + 1, 3) - 0.5f;
        memcpy(inputs.view, view, sizeof inputs.view);
        memcpy(inputs.projection, projection, sizeof inputs.projection);
        inputs.frame_id = (uint64_t)frame + 1;
        inputs.reset = frame == 0;
        inputs.not_rendering = false;

        Mk8DlssFgFrame result = mk8_dlss_fg_evaluate(&inputs);
        if (!result.valid || !result.interpolated) {
            submit_and_wait(cb, fence);
            fail("mk8_dlss_fg_evaluate returned no interpolated frame on frame %d (see the bridge log above)",
                 frame);
        }
        // Read the backbuffer and the interpolated image. Both are in GENERAL.
        Image interp = {};
        interp.image = result.interpolated_image;
        interp.format = result.format;
        interp.width = result.width;
        interp.height = result.height;
        interp.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        readback(cb, &backbuffer, &back_readback);
        readback(cb, &interp, &interp_readback);
        submit_and_wait(cb, fence);

        const uint8_t *back_pixels = (const uint8_t *)back_readback.mapped;
        const uint8_t *interp_pixels = (const uint8_t *)interp_readback.mapped;
        size_t pixels = (size_t)display_w * display_h;
        if (bgra) {
            swizzle_bgra_to_rgba(back_pixels, back_rgba.data(), pixels);
            swizzle_bgra_to_rgba(interp_pixels, interp_rgba.data(), pixels);
        } else {
            memcpy(back_rgba.data(), back_pixels, pixels * 4);
            memcpy(interp_rgba.data(), interp_pixels, pixels * 4);
        }
        Stats stats = compute_stats(interp_rgba.data(), display_w, display_h, kSentinel);
        print_stats("fg", stats, display_w * display_h);
        double difference = 0.0;
        for (size_t i = 0; i < pixels; i++)
            for (int c = 0; c < 3; c++)
                difference += fabs((double)interp_rgba[i * 4 + c] - (double)back_rgba[i * 4 + c]);
        difference /= (double)(pixels * 3);
        log_line("fg frame %d: mean |interpolated - backbuffer| = %.3f", frame, difference);
        memcpy(last_interp, interp_rgba.data(), pixels * 4);
        last_stats = stats;
        have_frame = true;
    }

    if (have_frame) {
        std::string path = join_path(output_dir, "bridge_fg_1280x720.png");
        if (write_png(path, last_interp, display_w, display_h)) log_line("wrote %s", path.c_str());
        std::string back_path = join_path(output_dir, "bridge_fg_backbuffer_1280x720.png");
        write_png(back_path, back_rgba.data(), display_w, display_h);
        bool ok = last_stats.sentinel_pixels == 0 && last_stats.distinct > 64 && last_stats.nonzero > 0;
        log_line("FG verdict: %s (distinct=%u sentinel=%u gradient=%.3f)",
                 ok ? "REAL INTERPOLATED FRAME" : "NOT A REAL FRAME", last_stats.distinct,
                 last_stats.sentinel_pixels, last_stats.gradient);
        if (!ok) exit(2);
    }
    mk8_dlss_fg_shutdown();
    free(cpu_color);
    free(cpu_depth);
    free(cpu_motion);
    free(last_interp);
    destroy_buffer(&motion_staging);
    destroy_buffer(&depth_staging);
    destroy_buffer(&back_staging);
    destroy_buffer(&interp_readback);
    destroy_buffer(&back_readback);
    destroy_image(&motion);
    destroy_image(&depth);
    destroy_image(&backbuffer);
}

// ---------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    crc_init();
    if (argc < 2) {
        fprintf(stderr, "usage: %s sr|fg [dll-dir] [output-dir]\n", argv[0]);
        return 1;
    }
    bool fg = strcmp(argv[1], "fg") == 0;
    bool sr = strcmp(argv[1], "sr") == 0;
    if (!fg && !sr) {
        fprintf(stderr, "usage: %s sr|fg [dll-dir] [output-dir]\n", argv[0]);
        return 1;
    }
    std::string dll_dir = argc > 2 ? argv[2] : exe_directory();
    std::string output_dir = argc > 3 ? argv[3] : join_path(dll_dir, "out");
    make_directory(output_dir);
    log_line("mode=%s dll-dir=%s output-dir=%s", sr ? "sr" : "fg", dll_dir.c_str(), output_dir.c_str());
    log_line("bridge: %s", sr ? "mk8_dlss_upscale" : "mk8_dlss_fg_init/mk8_dlss_fg_evaluate");

    // The bridge uses getenv(), which reads the CRT's copy: SetEnvironmentVariableA alone
    // would update only the Win32 block and the bridge would return before logging.
    _putenv_s("MK8_DLSS_DIR", dll_dir.c_str());
    _putenv_s("XDG_DATA_HOME", join_path(output_dir, "xdg").c_str());
    _putenv_s("MK8_FSR_FG", "");
    if (sr) {
        _putenv_s("MK8_DLSS", "1");
        _putenv_s("MK8_DLSS_FG", "");
    } else {
        _putenv_s("MK8_DLSS_FG", "1");
        _putenv_s("MK8_DLSS", "");
    }

    create_hidden_window();
    create_instance();
    create_device();
    if (fg) {
        create_swapchain();
    } else {
        // No swapchain is needed for SR; keep the surface alive because the instance owns it.
        g_swapchain_format = VK_FORMAT_R8G8B8A8_UNORM;
    }

    if (sr)
        run_sr(output_dir);
    else
        run_fg(output_dir);

    log_line("harness complete");
    return 0;
}
