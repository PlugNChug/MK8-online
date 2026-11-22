// Native Windows x86-64 harness for the NGX Vulkan ABI in
// runtime/mk8/graphics/ngx/mk8_ngx_abi.h, run against NVIDIA's DLSS plugins on a real
// RTX GPU. It is the "execute it somewhere with an NVIDIA card" counterpart to
// tests/test_dlss.py (which only exercises the loader on a machine without one).
//
// What it does, in order:
//   1. finds the driver-installed NGX core through
//      HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore\FullPath (that is how the
//      driver exposes _nvngx.dll on current Windows drivers: it is not copied to
//      System32),
//   2. loads nvngx_dlss.dll / nvngx_dlssg.dll from this executable's directory,
//   3. creates a Vulkan 1.3 instance and a compute device on the first NVIDIA GPU,
//      enabling the extensions NGX reports from NVSDK_NGX_VULKAN_RequiredExtensions
//      (called before instance creation, which is the only point where it is useful),
//   4. calls NVSDK_NGX_VULKAN_Init_with_ProjectID and prints SuperSampling.Available
//      and FrameGeneration.Available plus their *NeedsUpdatedDriver / MinDriverVersion
//      companions,
//   5. creates a SuperSampling feature 1280x720 -> 2560x1440 (PERFORMANCE), evaluates
//      several frames with synthetic colour/depth/motion vectors/jitter, checks that
//      the output changed from a magenta sentinel, measures it and writes a PNG,
//   6. attempts the same for FrameGeneration: availability, feature creation and, if
//      creation succeeds, headless evaluation with app-owned output images. DLSS-G
//      normally presents the interpolated frame itself through the app's swapchain;
//      there is no swapchain here, so an evaluation failure is reported as such and
//      not worked around,
//   7. shuts NGX down and prints a machine-readable summary.
//
// It reuses our ABI header verbatim: the structs, entry-point signatures and
// parameter strings passed to NVIDIA's binaries are the ones runtime/mk8/graphics/dlss.cpp
// uses. A wrong layout or signature therefore shows up as a failure here, not later in
// the game. Frame Generation needs a handful of definitions that our bridge does not
// have yet (feature id 11 and the DLSSG.* parameters); they are declared below, taken
// from the DLSS SDK v310.9.16 headers, and are deliberately local to this harness.
//
// Build (cross, from the repository root):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -Wall -Wextra \
//       -I"$HOME/.cache/mk8-dlss-rtx/include" \
//       -o dlss_rtx_harness.exe tools/dlss_rtx_harness.cpp
//
// Run (on the Windows machine, DLLs beside the exe):
//   dlss_rtx_harness.exe [dlss-dir] [output-dir]
//
// Nothing NVIDIA is committed or linked: the harness LoadLibrary()s the core and the
// plugins at run time and never ships them. Exit codes: 0 SR evaluated and non-empty,
// 1 SR unavailable/failed, 2 Vulkan setup failed, 3 bad arguments.

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "../runtime/mk8/graphics/ngx/mk8_ngx_abi.h"

// ---------------------------------------------------------------------------------------
// Facts from nvsdk_ngx_defs_dlssg.h / nvsdk_ngx_params_dlssg.h v310.9.16 that the SR
// bridge does not need. Kept here so mk8_ngx_abi.h stays the SR-only contract.
// ---------------------------------------------------------------------------------------
enum { HG_NGX_FEATURE_FRAME_GENERATION = 11 };

#define HG_FG_AVAILABLE "FrameGeneration.Available"
#define HG_FG_INIT_RESULT "FrameGeneration.FeatureInitResult"
#define HG_FG_MIN_DRIVER_MAJOR "FrameGeneration.MinDriverVersionMajor"
#define HG_FG_MIN_DRIVER_MINOR "FrameGeneration.MinDriverVersionMinor"
#define HG_FG_NEEDS_UPDATED_DRIVER "FrameGeneration.NeedsUpdatedDriver"
#define HG_FI_AVAILABLE "FrameInterpolation.Available"

#define HG_FG_BACKBUFFER_FORMAT "DLSSG.BackbufferFormat"
#define HG_FG_BACKBUFFER "DLSSG.Backbuffer"
#define HG_FG_MVECS "DLSSG.MVecs"
#define HG_FG_DEPTH "DLSSG.Depth"
#define HG_FG_OUTPUT_INTERPOLATED "DLSSG.OutputInterpolated"
#define HG_FG_CAMERA_VIEW_TO_CLIP "DLSSG.CameraViewToClip"
#define HG_FG_CLIP_TO_CAMERA_VIEW "DLSSG.ClipToCameraView"
#define HG_FG_CLIP_TO_PREV_CLIP "DLSSG.ClipToPrevClip"
#define HG_FG_PREV_CLIP_TO_CLIP "DLSSG.PrevClipToClip"
#define HG_FG_JITTER_X "DLSSG.JitterOffsetX"
#define HG_FG_JITTER_Y "DLSSG.JitterOffsetY"
#define HG_FG_MVEC_SCALE_X "DLSSG.MvecScaleX"
#define HG_FG_MVEC_SCALE_Y "DLSSG.MvecScaleY"
#define HG_FG_CAMERA_NEAR "DLSSG.CameraNear"
#define HG_FG_CAMERA_FAR "DLSSG.CameraFar"
#define HG_FG_CAMERA_FOV "DLSSG.CameraFOV"
#define HG_FG_CAMERA_ASPECT "DLSSG.CameraAspectRatio"
#define HG_FG_COLOR_BUFFERS_HDR "DLSSG.ColorBuffersHDR"
#define HG_FG_DEPTH_INVERTED "DLSSG.DepthInverted"
#define HG_FG_CAMERA_MOTION_INCLUDED "DLSSG.CameraMotionIncluded"
#define HG_FG_RESET "DLSSG.Reset"
#define HG_FG_NOT_RENDERING "DLSSG.NotRenderingGameFrames"
#define HG_FG_ORTHO "DLSSG.OrthoProjection"
#define HG_FG_MVEC_INVALID "DLSSG.MvecInvalidValue"
#define HG_FG_MVEC_DILATED "DLSSG.MvecDilated"
#define HG_FG_MENU_DETECTION "DLSSG.MenuDetectionEnabled"
#define HG_FG_MULTI_FRAME_COUNT "DLSSG.MultiFrameCount"
#define HG_FG_MULTI_FRAME_INDEX "DLSSG.MultiFrameIndex"

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------
static std::mutex g_log_mutex;

static void log_line(const char *format, ...) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    va_list args;
    va_start(args, format);
    fputs("[mk8-dlss] ", stdout);
    vfprintf(stdout, format, args);
    fputc('\n', stdout);
    va_end(args);
    fflush(stdout);
}

static void log_ngx(const char *message, int level, int feature) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stdout, "[ngx:%d:%d] %s\n", level, feature, message ? message : "(null)");
    fflush(stdout);
}

static const char *result_name(int result) {
    switch ((unsigned)result) {
    case MK8_NGX_RESULT_SUCCESS: return "success";
    case MK8_NGX_RESULT_FAIL: return "fail";
    case MK8_NGX_RESULT_FAIL_FEATURE_NOT_SUPPORTED: return "feature-not-supported";
    case MK8_NGX_RESULT_FAIL_INVALID_PARAMETER: return "invalid-parameter";
    case MK8_NGX_RESULT_FAIL_NOT_INITIALIZED: return "not-initialized";
    case MK8_NGX_RESULT_FAIL_UNABLE_TO_INITIALIZE_FEATURE: return "unable-to-initialize-feature";
    case MK8_NGX_RESULT_FAIL_UNABLE_TO_WRITE_TO_APP_DATA_PATH: return "unable-to-write-app-data-path";
    default: return "other";
    }
}

static void log_result(const char *what, int rc) {
    log_line("%s: 0x%08x (%s)", what, (unsigned)rc, result_name(rc));
}

// ---------------------------------------------------------------------------------------
// Tiny filesystem helpers (no <filesystem>, keeps the static link boring)
// ---------------------------------------------------------------------------------------
static std::string exe_directory(void) {
    char path[MAX_PATH * 2] = {0};
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof path);
    if (!length || length >= sizeof path) return std::string(".");
    std::string text(path);
    size_t slash = text.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : text.substr(0, slash);
}

static bool file_exists(const std::string &path) {
    DWORD attributes = GetFileAttributesA(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static std::string absolute_path(const std::string &path) {
    char buffer[MAX_PATH * 2] = {0};
    DWORD length = GetFullPathNameA(path.c_str(), (DWORD)sizeof buffer, buffer, NULL);
    return (length && length < sizeof buffer) ? std::string(buffer) : path;
}

static std::string join_path(const std::string &directory, const char *name) {
    if (directory.empty()) return std::string(name);
    char last = directory[directory.size() - 1];
    return directory + ((last == '\\' || last == '/') ? "" : "\\") + name;
}

static bool make_directory(const std::string &path) {
    if (CreateDirectoryA(path.c_str(), NULL)) return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static unsigned long long file_size(const std::string &path) {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
}

// The driver registers its NGX directory instead of copying _nvngx.dll into System32
// (checked on this machine: System32 has no nvngx*.dll, the DriverStore copy is the
// 32.0.16.2101 "NGX Runtime Library"). The plugin resolves the core by that path, so the
// harness has to load the same file or it would initialise a second, unrelated core.
static std::string registry_ngx_directory(void) {
    HKEY key = NULL;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS)
        return std::string();
    char buffer[1024] = {0};
    DWORD size = (DWORD)sizeof buffer;
    DWORD type = 0;
    LONG rc = RegQueryValueExA(key, "FullPath", NULL, &type, (LPBYTE)buffer, &size);
    RegCloseKey(key);
    if (rc != ERROR_SUCCESS || type != REG_SZ || !buffer[0]) return std::string();
    return std::string(buffer);
}

// ---------------------------------------------------------------------------------------
// Vulkan entry points, loaded through the loader (never linked: mingw has no
// libvulkan-1.a, and loading by name is what a real app does anyway).
// ---------------------------------------------------------------------------------------
#define VK_GLOBAL_FUNCS(X)                                                                          \
    X(vkCreateInstance)                                                                             \
    X(vkEnumerateInstanceExtensionProperties)                                                       \
    X(vkEnumerateInstanceVersion)

#define VK_INSTANCE_FUNCS(X)                                                                        \
    X(vkEnumeratePhysicalDevices)                                                                   \
    X(vkGetPhysicalDeviceProperties)                                                                \
    X(vkGetPhysicalDeviceProperties2)                                                               \
    X(vkGetPhysicalDeviceFeatures)                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                 \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                     \
    X(vkGetPhysicalDeviceMemoryProperties)                                                          \
    X(vkGetPhysicalDeviceFormatProperties)                                                          \
    X(vkEnumerateDeviceExtensionProperties)                                                         \
    X(vkCreateDevice)                                                                               \
    X(vkDestroyInstance)

#define VK_DEVICE_FUNCS(X)                                                                          \
    X(vkGetDeviceQueue)                                                                             \
    X(vkCreateImage)                                                                                \
    X(vkGetImageMemoryRequirements)                                                                 \
    X(vkAllocateMemory)                                                                             \
    X(vkBindImageMemory)                                                                            \
    X(vkCreateImageView)                                                                            \
    X(vkCreateBuffer)                                                                               \
    X(vkGetBufferMemoryRequirements)                                                                \
    X(vkBindBufferMemory)                                                                           \
    X(vkMapMemory)                                                                                  \
    X(vkUnmapMemory)                                                                                \
    X(vkCreateCommandPool)                                                                          \
    X(vkAllocateCommandBuffers)                                                                     \
    X(vkResetCommandBuffer)                                                                         \
    X(vkBeginCommandBuffer)                                                                         \
    X(vkEndCommandBuffer)                                                                           \
    X(vkCmdPipelineBarrier)                                                                         \
    X(vkCmdCopyBufferToImage)                                                                       \
    X(vkCmdCopyImageToBuffer)                                                                       \
    X(vkCmdClearColorImage)                                                                         \
    X(vkQueueSubmit)                                                                                \
    X(vkQueueWaitIdle)                                                                              \
    X(vkCreateFence)                                                                                \
    X(vkWaitForFences)                                                                              \
    X(vkResetFences)                                                                                \
    X(vkDestroyFence)                                                                               \
    X(vkDestroyCommandPool)                                                                         \
    X(vkDestroyImageView)                                                                           \
    X(vkDestroyImage)                                                                               \
    X(vkDestroyBuffer)                                                                              \
    X(vkFreeMemory)                                                                                 \
    X(vkDestroyDevice)                                                                              \
    X(vkDeviceWaitIdle)

#define HG_DECLARE_VK(name) static PFN_##name vkfn_##name = NULL;
VK_GLOBAL_FUNCS(HG_DECLARE_VK)
VK_INSTANCE_FUNCS(HG_DECLARE_VK)
VK_DEVICE_FUNCS(HG_DECLARE_VK)

static PFN_vkGetInstanceProcAddr g_gipa = NULL;
static PFN_vkGetDeviceProcAddr g_gdpa = NULL;

static bool load_global_functions(void) {
    HMODULE loader = LoadLibraryA("vulkan-1.dll");
    if (!loader) {
        log_line("vulkan-1.dll did not load: Win32 error %lu", (unsigned long)GetLastError());
        return false;
    }
    g_gipa = (PFN_vkGetInstanceProcAddr)GetProcAddress(loader, "vkGetInstanceProcAddr");
    if (!g_gipa) {
        log_line("vulkan-1.dll has no vkGetInstanceProcAddr");
        return false;
    }
    bool ok = true;
#define HG_LOAD_GLOBAL(name)                                                                        \
    vkfn_##name = (PFN_##name)g_gipa(NULL, #name);                                                         \
    if (!name) {                                                                                    \
        log_line("missing global Vulkan entry point %s", #name);                                    \
        ok = false;                                                                                 \
    }
    VK_GLOBAL_FUNCS(HG_LOAD_GLOBAL)
#undef HG_LOAD_GLOBAL
    if (!vkfn_vkEnumerateInstanceVersion) {
        log_line("vkEnumerateInstanceVersion missing; assuming Vulkan 1.0 loader");
        ok = true;
    }
    return ok;
}

static bool load_instance_functions(VkInstance instance) {
    bool ok = true;
#define HG_LOAD_INSTANCE(name)                                                                      \
    vkfn_##name = (PFN_##name)g_gipa(instance, #name);                                                     \
    if (!name) {                                                                                    \
        log_line("missing instance entry point %s", #name);                                         \
        ok = false;                                                                                 \
    }
    VK_INSTANCE_FUNCS(HG_LOAD_INSTANCE)
#undef HG_LOAD_INSTANCE
    return ok;
}

static bool load_device_functions(VkDevice device) {
    bool ok = true;
#define HG_LOAD_DEVICE(name)                                                                        \
    vkfn_##name = (PFN_##name)g_gdpa(device, #name);                                                       \
    if (!name) {                                                                                    \
        log_line("missing device entry point %s", #name);                                           \
        ok = false;                                                                                 \
    }
    VK_DEVICE_FUNCS(HG_LOAD_DEVICE)
#undef HG_LOAD_DEVICE
    return ok;
}

// ---------------------------------------------------------------------------------------
// Vulkan objects
// ---------------------------------------------------------------------------------------
static VkInstance g_instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_phys = VK_NULL_HANDLE;
static VkDevice g_device = VK_NULL_HANDLE;
static VkQueue g_queue = VK_NULL_HANDLE;
static uint32_t g_queue_family = 0;
static VkCommandPool g_pool = VK_NULL_HANDLE;
static VkCommandBuffer g_cb = VK_NULL_HANDLE;
static VkFence g_fence = VK_NULL_HANDLE;
static int g_pointer_setter = 0;   // NVSDK_NGX_Parameter::Set(void*) vtable slot (verified at run time)
static int g_float_slot = -1;      // NVSDK_NGX_Parameter::Set(float) vtable slot (probed at run time)

static std::vector<std::string> g_instance_extensions;
static std::vector<std::string> g_device_extensions;

static bool has_string(const std::vector<std::string> &list, const char *name) {
    for (const std::string &entry : list)
        if (entry == name) return true;
    return false;
}

static void add_unique(std::vector<std::string> &list, const char *name) {
    if (!name || !*name || has_string(list, name)) return;
    list.push_back(name);
}

static bool enumerate_instance_extensions(void) {
    uint32_t count = 0;
    if (vkfn_vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> props(count);
    if (count && vkfn_vkEnumerateInstanceExtensionProperties(NULL, &count, props.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties &ext : props) g_instance_extensions.push_back(ext.extensionName);
    return true;
}

static bool enumerate_device_extensions(VkPhysicalDevice phys) {
    uint32_t count = 0;
    if (vkfn_vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> props(count);
    if (count && vkfn_vkEnumerateDeviceExtensionProperties(phys, NULL, &count, props.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties &ext : props) g_device_extensions.push_back(ext.extensionName);
    return true;
}

static bool queue_family_compute(VkPhysicalDevice phys, uint32_t *out_family) {
    uint32_t count = 0;
    vkfn_vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, NULL);
    if (!count) return false;
    std::vector<VkQueueFamilyProperties> families(count);
    vkfn_vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
    for (uint32_t i = 0; i < count; i++) {
        if (!(families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) continue;
        *out_family = i;
        return true;
    }
    return false;
}

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags wanted) {
    VkPhysicalDeviceMemoryProperties memory;
    vkfn_vkGetPhysicalDeviceMemoryProperties(g_phys, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((memory.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
    }
    return UINT32_MAX;
}

static void image_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                          VkImageLayout old_layout, VkImageLayout new_layout, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    vkfn_vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

struct HgImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

struct HgBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;
    VkDeviceSize size = 0;
};

static bool create_image(HgImage *out, VkFormat format, uint32_t width, uint32_t height,
                         VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    memset(out, 0, sizeof *out);
    VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = (VkExtent3D){width, height, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult rc = vkfn_vkCreateImage(g_device, &info, NULL, &out->image);
    if (rc != VK_SUCCESS) {
        log_line("vkCreateImage %ux%u failed: %d", width, height, rc);
        return false;
    }
    VkMemoryRequirements requirements;
    vkfn_vkGetImageMemoryRequirements(g_device, out->image, &requirements);
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX) {
        log_line("no device-local memory type for a %ux%u image", width, height);
        return false;
    }
    if (vkfn_vkAllocateMemory(g_device, &allocation, NULL, &out->memory) != VK_SUCCESS) {
        log_line("vkAllocateMemory (%llu bytes) failed", (unsigned long long)requirements.size);
        return false;
    }
    if (vkfn_vkBindImageMemory(g_device, out->image, out->memory, 0) != VK_SUCCESS) return false;

    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = out->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange.aspectMask = aspect;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkfn_vkCreateImageView(g_device, &view, NULL, &out->view) != VK_SUCCESS) return false;
    out->format = format;
    out->width = width;
    out->height = height;
    out->aspect = aspect;
    out->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static void destroy_image(HgImage *image) {
    if (image->view) vkfn_vkDestroyImageView(g_device, image->view, NULL);
    if (image->image) vkfn_vkDestroyImage(g_device, image->image, NULL);
    if (image->memory) vkfn_vkFreeMemory(g_device, image->memory, NULL);
    memset(image, 0, sizeof *image);
}

static bool create_buffer(HgBuffer *out, VkDeviceSize size, VkBufferUsageFlags usage,
                          VkMemoryPropertyFlags properties) {
    memset(out, 0, sizeof *out);
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkfn_vkCreateBuffer(g_device, &info, NULL, &out->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements;
    vkfn_vkGetBufferMemoryRequirements(g_device, out->buffer, &requirements);
    VkMemoryAllocateInfo allocation = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, properties);
    if (allocation.memoryTypeIndex == UINT32_MAX) {
        log_line("no memory type for a %llu-byte buffer", (unsigned long long)size);
        return false;
    }
    if (vkfn_vkAllocateMemory(g_device, &allocation, NULL, &out->memory) != VK_SUCCESS) return false;
    if (vkfn_vkBindBufferMemory(g_device, out->buffer, out->memory, 0) != VK_SUCCESS) return false;
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkfn_vkMapMemory(g_device, out->memory, 0, VK_WHOLE_SIZE, 0, &out->mapped) != VK_SUCCESS)
            return false;
    }
    out->size = size;
    return true;
}

static void destroy_buffer(HgBuffer *buffer) {
    if (buffer->mapped) vkfn_vkUnmapMemory(g_device, buffer->memory);
    if (buffer->buffer) vkfn_vkDestroyBuffer(g_device, buffer->buffer, NULL);
    if (buffer->memory) vkfn_vkFreeMemory(g_device, buffer->memory, NULL);
    memset(buffer, 0, sizeof *buffer);
}

static bool begin_commands(void) {
    vkfn_vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkfn_vkBeginCommandBuffer(g_cb, &begin) == VK_SUCCESS;
}

static bool submit_and_wait(void) {
    if (vkfn_vkEndCommandBuffer(g_cb) != VK_SUCCESS) return false;
    vkfn_vkResetFences(g_device, 1, &g_fence);
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &g_cb;
    if (vkfn_vkQueueSubmit(g_queue, 1, &submit, g_fence) != VK_SUCCESS) return false;
    return vkfn_vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, 60ull * 1000ull * 1000ull * 1000ull) == VK_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Synthetic frames. Content is a panning, patterned "scene": high-frequency detail for
// the network to reconstruct, a depth ramp and constant camera motion vectors that match
// the pan. It is not meant to look like a game; it is meant to prove that a real DLSS
// dispatch ran and wrote structured pixels.
// ---------------------------------------------------------------------------------------
static float halton(int index, int base) {
    float result = 0.0f;
    float fraction = 1.0f;
    int value = index;
    while (value > 0) {
        fraction /= (float)base;
        result += fraction * (float)(value % base);
        value /= base;
    }
    return result;
}

static uint8_t clamp_u8(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

static void fill_frame(uint32_t width, uint32_t height, int frame, int pan_pixels_per_frame,
                       uint8_t *rgba, float *depth, float *motion) {
    const int shift = frame * pan_pixels_per_frame;
    for (uint32_t y = 0; y < height; y++) {
        const float fy = (float)y / (float)(height - 1);
        for (uint32_t x = 0; x < width; x++) {
            // Panning world coordinate, wrapped so the pattern never runs out.
            int wx = (int)x - shift;
            wx %= 256;
            if (wx < 0) wx += 256;

            // Sky/ground gradient plus a moving checker band and thin stripes, i.e. the
            // mix of smooth and high-frequency content DLSS is supposed to handle.
            float r = 32.0f + 180.0f * fy;
            float g = 48.0f + 120.0f * (1.0f - fy);
            float b = 90.0f + 90.0f * fy;
            const bool checker = (((wx / 8) + (int)(y / 8)) & 1) != 0;
            if (fy > 0.55f && checker) {
                r += 40.0f;
                g += 70.0f;
                b += 30.0f;
            }
            if ((wx % 32) < 2) {
                r = 240.0f;
                g = 230.0f;
                b = 200.0f;
            }
            if (y > height * 3 / 4) {
                // A near "road" band: strong horizontal edge at the bottom.
                r = 40.0f + (float)(wx % 16) * 6.0f;
                g = 44.0f;
                b = 52.0f;
            }
            size_t pixel = ((size_t)y * width + x) * 4;
            rgba[pixel + 0] = clamp_u8(r);
            rgba[pixel + 1] = clamp_u8(g);
            rgba[pixel + 2] = clamp_u8(b);
            rgba[pixel + 3] = 255;
            if (depth) depth[(size_t)y * width + x] = 0.15f + 0.75f * fy;
            if (motion) {
                // NGX motion vectors are render-pixel deltas from the current pixel to
                // where that point was in the previous frame: the scene moved right, so
                // the vector points left.
                motion[((size_t)y * width + x) * 2 + 0] = -(float)pan_pixels_per_frame;
                motion[((size_t)y * width + x) * 2 + 1] = 0.0f;
            }
        }
    }
}

// ---------------------------------------------------------------------------------------
// PNG (RGBA8, stored deflate blocks; no zlib dependency in the static mingw build)
// ---------------------------------------------------------------------------------------
static uint32_t g_crc_table[256];
static bool g_crc_ready = false;

static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_table[n] = c;
    }
    g_crc_ready = true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t length) {
    if (!g_crc_ready) crc_init();
    for (size_t i = 0; i < length; i++) crc = g_crc_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

static uint32_t adler32_update(uint32_t adler, const uint8_t *data, size_t length) {
    uint32_t a = adler & 0xFFFF, b = (adler >> 16) & 0xFFFF;
    for (size_t i = 0; i < length; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static void put_u32_be(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back((uint8_t)(value >> 24));
    out.push_back((uint8_t)(value >> 16));
    out.push_back((uint8_t)(value >> 8));
    out.push_back((uint8_t)value);
}

static void png_chunk(FILE *file, const char type[4], const std::vector<uint8_t> &data) {
    std::vector<uint8_t> header;
    put_u32_be(header, (uint32_t)data.size());
    fwrite(header.data(), 1, header.size(), file);
    fwrite(type, 1, 4, file);
    if (!data.empty()) fwrite(data.data(), 1, data.size(), file);
    uint32_t crc = 0xFFFFFFFFu;
    crc = crc32_update(crc, (const uint8_t *)type, 4);
    if (!data.empty()) crc = crc32_update(crc, data.data(), data.size());
    crc ^= 0xFFFFFFFFu;
    put_u32_be(header, crc);
    fwrite(header.data() + 4, 1, 4, file);
}

static bool write_png_rgba8(const std::string &path, const uint8_t *rgba, uint32_t width,
                            uint32_t height) {
    if (!rgba || !width || !height) return false;
    const size_t row_bytes = (size_t)width * 4;
    std::vector<uint8_t> filtered;
    filtered.reserve((row_bytes + 1) * height);
    for (uint32_t y = 0; y < height; y++) {
        filtered.push_back(0);  // filter type: none
        filtered.insert(filtered.end(), rgba + (size_t)y * row_bytes,
                        rgba + (size_t)y * row_bytes + row_bytes);
    }

    std::vector<uint8_t> idat;
    idat.reserve(filtered.size() / 65535 * 5 + 64 + filtered.size());
    idat.push_back(0x78);  // zlib: deflate, 32K window
    idat.push_back(0x01);  // no preset dictionary, fastest
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
    put_u32_be(idat, adler32_update(1, filtered.data(), filtered.size()));

    FILE *file = fopen(path.c_str(), "wb");
    if (!file) {
        log_line("cannot open %s for writing", path.c_str());
        return false;
    }
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    fwrite(signature, 1, sizeof signature, file);

    std::vector<uint8_t> ihdr;
    put_u32_be(ihdr, width);
    put_u32_be(ihdr, height);
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // RGBA
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    png_chunk(file, "IHDR", ihdr);
    png_chunk(file, "IDAT", idat);
    png_chunk(file, "IEND", std::vector<uint8_t>());
    fclose(file);
    return true;
}

// ---------------------------------------------------------------------------------------
// Output statistics
// ---------------------------------------------------------------------------------------
struct HgStats {
    uint64_t bytes = 0;
    uint64_t nonzero_bytes = 0;
    uint64_t checksum = 1469598103934665603ull;
    uint32_t min_channel[4] = {255, 255, 255, 255};
    uint32_t max_channel[4] = {0, 0, 0, 0};
    double mean_channel[4] = {0, 0, 0, 0};
    uint32_t sentinel_pixels = 0;
    uint32_t distinct_pixels = 0;
    double horizontal_gradient = 0.0;
};

static HgStats compute_stats(const uint8_t *rgba, uint32_t width, uint32_t height,
                             uint32_t sentinel) {
    HgStats stats;
    const size_t pixels = (size_t)width * height;
    stats.bytes = pixels * 4;
    double sum[4] = {0, 0, 0, 0};
    std::vector<uint32_t> values(pixels);
    for (size_t i = 0; i < pixels; i++) {
        uint32_t pixel = 0;
        for (int c = 0; c < 4; c++) {
            uint8_t value = rgba[i * 4 + c];
            stats.min_channel[c] = std::min<uint32_t>(stats.min_channel[c], value);
            stats.max_channel[c] = std::max<uint32_t>(stats.max_channel[c], value);
            sum[c] += value;
            if (value) stats.nonzero_bytes++;
            pixel |= (uint32_t)value << (c * 8);
        }
        values[i] = pixel;
        if (pixel == sentinel) stats.sentinel_pixels++;
        stats.checksum ^= pixel;
        stats.checksum *= 1099511628211ull;
    }
    for (int c = 0; c < 4; c++) stats.mean_channel[c] = sum[c] / (double)pixels;
    std::sort(values.begin(), values.end());
    stats.distinct_pixels = (uint32_t)(std::unique(values.begin(), values.end()) - values.begin());

    // Mean absolute R/G/B difference between horizontally adjacent pixels: zero for a
    // flat/clear-colour frame, clearly positive for a reconstructed image.
    double gradient = 0.0;
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t *row = rgba + (size_t)y * width * 4;
        for (uint32_t x = 1; x < width; x++) {
            for (int c = 0; c < 3; c++)
                gradient += fabs((double)row[x * 4 + c] - (double)row[(x - 1) * 4 + c]);
        }
    }
    stats.horizontal_gradient = gradient / (double)((size_t)(width - 1) * height * 3);
    return stats;
}

static void print_stats(const char *label, const HgStats &s, uint32_t sentinel) {
    log_line("%s: bytes=%llu nonzero_bytes=%llu sentinel_pixels=%u/%u distinct_pixels=%u",
             label, (unsigned long long)s.bytes, (unsigned long long)s.nonzero_bytes,
             s.sentinel_pixels, (unsigned)(s.bytes / 4), s.distinct_pixels);
    log_line("%s: checksum=0x%016llx min/max R=%u/%u G=%u/%u B=%u/%u A=%u/%u mean=%.2f/%.2f/%.2f/%.2f "
             "horiz_gradient=%.3f",
             label, (unsigned long long)s.checksum, s.min_channel[0], s.max_channel[0],
             s.min_channel[1], s.max_channel[1], s.min_channel[2], s.max_channel[2],
             s.min_channel[3], s.max_channel[3], s.mean_channel[0], s.mean_channel[1],
             s.mean_channel[2], s.mean_channel[3], s.horizontal_gradient);
    (void)sentinel;
}

// ---------------------------------------------------------------------------------------
// NGX entry points (resolved from the core and the feature plugins)
// ---------------------------------------------------------------------------------------
// The Windows driver core exports NVSDK_NGX_VULKAN_Init_ProjectID with nine arguments:
// the SDK's public Init_with_ProjectID minus the two vkGet*ProcAddr pointers (the core
// resolves those through the loader). Verified from _nvngx.dll 32.0.16.2101: it reads
// args 1..9 at rsp+0x330..0x370 and never touches args 10/11. Calling it with the
// 11-argument SDK shape passes the engine-version string where the feature-info pointer
// belongs and faults inside the path-list walk.
typedef int(MK8_NGX_CONV *HgNgxInitProjectId9Fn)(const char *project_id, int engine_type,
                                                 const char *engine_version,
                                                 const wchar_t *application_data_path,
                                                 VkInstance instance, VkPhysicalDevice physical_device,
                                                 VkDevice device, int sdk_version,
                                                 const Mk8NgxFeatureCommonInfo *feature_info);

struct HgNgx {
    HMODULE core = NULL;
    HMODULE plugin = NULL;
    HMODULE plugin_fg = NULL;

    Mk8NgxInitWithProjectIdFn init = NULL;
    HgNgxInitProjectId9Fn init9 = NULL;
    Mk8NgxGetCapabilityParametersFn get_capability = NULL;
    Mk8NgxParameterSetUiFn set_ui = NULL;
    Mk8NgxParameterSetIFn set_i = NULL;
    Mk8NgxParameterSetFFn set_f = NULL;
    Mk8NgxParameterSetVoidPointerFn set_pointer = NULL;
    Mk8NgxParameterGetUiFn get_ui = NULL;
    Mk8NgxParameterGetUllFn get_ull = NULL;
    Mk8NgxShutdown1Fn shutdown1 = NULL;
    Mk8NgxRequiredExtensionsFn required_extensions = NULL;
    Mk8NgxGetFeatureRequirementsFn get_requirements = NULL;

    Mk8NgxCreateFeature1Fn create_feature1 = NULL;
    Mk8NgxCreateFeatureFn create_feature = NULL;
    Mk8NgxEvaluateFeatureFn evaluate = NULL;
    Mk8NgxReleaseFeatureFn release = NULL;

    bool initialised = false;
    Mk8NgxParameter *params = NULL;
};

template <typename T>
static T resolve_symbol(HMODULE module, const char *name) {
    return module ? (T)GetProcAddress(module, name) : NULL;
}

// The Windows NGX runtime does not export the SDK's C parameter helpers
// (NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetVoidPointer/GetUI). It hands back an
// NVSDK_NGX_Parameter C++ object whose virtual functions are the only interface.
// nvsdk_ngx_params.h declares 17 virtuals (8 Set overloads, 8 Get, Reset), but MSVC
// orders the overloads differently from the declaration. The driver's vtable 0x1800b7120
// was disassembled and the slots were confirmed with EvaluateFeature on 621.01:
//   0 Set(void*)      1,2 Set(ID3D11/12Resource*) (stubs)   3 Set(uint)   4 Set(int)
//   5,6 Set(float/double)                                  7 Set(ull)
//   8 Get(resource)   11 Get(uint)   15 Get(ull, converts)  16 Reset
// Slot 6 is the one the snippet's Get(float) sees. Windows x64 has one calling
// convention and the vptr is at offset 0, so a mingw build can index the table.
typedef void (*HgParamSetUI)(void *self, const char *name, unsigned value);
typedef void (*HgParamSetI)(void *self, const char *name, int value);
typedef void (*HgParamSetF)(void *self, const char *name, float value);
typedef void (*HgParamSetPtr)(void *self, const char *name, void *value);
typedef int (*HgParamGetUI)(const void *self, const char *name, unsigned *out);
typedef int (*HgParamGetULL)(const void *self, const char *name, unsigned long long *out);
typedef int (*HgParamGetPtr)(const void *self, const char *name, void **out);

static void *vtable_entry(void *object, int index) {
    if (!object) return NULL;
    void **vtable = *(void ***)object;
    return vtable ? vtable[index] : NULL;
}

static void vtable_set_ui(Mk8NgxParameter *params, const char *name, unsigned value) {
    HgParamSetUI fn = (HgParamSetUI)vtable_entry(params, 3);
    if (fn) fn(params, name, value);
}
static void vtable_set_i(Mk8NgxParameter *params, const char *name, int value) {
    HgParamSetI fn = (HgParamSetI)vtable_entry(params, 4);
    if (fn) fn(params, name, value);
}
static void vtable_set_f(Mk8NgxParameter *params, const char *name, float value) {
    // Slot 1 is an empty stub in the Vulkan core; slot 6 stores the value as type 4 and
    // slot 5 as type 5. Which one matches the snippet's Get(float) is probed.
    int index = g_float_slot >= 0 ? g_float_slot : 6;
    HgParamSetF fn = (HgParamSetF)vtable_entry(params, index);
    if (fn) fn(params, name, value);
}
static void vtable_set_pointer(Mk8NgxParameter *params, const char *name, void *value) {
    HgParamSetPtr fn = (HgParamSetPtr)vtable_entry(params, 7);
    if (fn) fn(params, name, value);
}
static int vtable_get_ui(Mk8NgxParameter *params, const char *name, unsigned *out) {
    HgParamGetUI fn = (HgParamGetUI)vtable_entry(params, 11);
    return fn ? fn(params, name, out) : (int)MK8_NGX_RESULT_FAIL;
}
static int vtable_get_ull(Mk8NgxParameter *params, const char *name, unsigned long long *out) {
    HgParamGetULL fn = (HgParamGetULL)vtable_entry(params, 15);
    return fn ? fn(params, name, out) : (int)MK8_NGX_RESULT_FAIL;
}

// MSVC does not lay overloaded virtuals out in declaration order the way the SDK
// header reads, and the Vulkan core's parameter object has no C setters. The Set(void*)
// slot is therefore discovered at run time (see run_super_resolution): try each of the
// eight Set slots until EvaluateFeature accepts the four image resources.
static void vtable_set_pointer_index(Mk8NgxParameter *params, int index, const char *name, void *value) {
    HgParamSetPtr fn = (HgParamSetPtr)vtable_entry(params, index);
    if (fn) fn(params, name, value);
}

static std::string load_error_text(void) {
    DWORD code = GetLastError();
    char text[512] = {0};
    DWORD written = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL,
                                   code, 0, text, (DWORD)sizeof text, NULL);
    if (!written) return "Win32 error " + std::to_string((unsigned long)code);
    while (written && (text[written - 1] == '\r' || text[written - 1] == '\n')) text[--written] = 0;
    return std::string(text);
}

// Adds a directory to the loader's search path so the plugin's own LoadLibraryA("_nvngx.dll")
// finds the very same driver file we loaded, instead of silently loading a second copy.
static bool add_dll_directory(const std::string &directory) {
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, directory.c_str(), -1, wide, 1024)) return false;
    return AddDllDirectory(wide) != NULL;
}

static bool ngx_load(HgNgx *ngx, const std::string &dlss_dir) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    add_dll_directory(dlss_dir);

    std::string driver_dir = registry_ngx_directory();
    std::string core_path;
    if (!driver_dir.empty()) {
        add_dll_directory(driver_dir);
        if (file_exists(join_path(driver_dir, "_nvngx.dll")))
            core_path = join_path(driver_dir, "_nvngx.dll");
        else if (file_exists(join_path(driver_dir, "nvngx.dll")))
            core_path = join_path(driver_dir, "nvngx.dll");
    }
    if (core_path.empty()) {
        const char *fallbacks[] = {"_nvngx.dll", "nvngx.dll"};
        for (const char *fallback : fallbacks) {
            std::string candidate = join_path(dlss_dir, fallback);
            if (file_exists(candidate)) {
                core_path = candidate;
                break;
            }
        }
    }
    if (core_path.empty()) {
        log_line("no NGX core: HKLM NGXCore\\FullPath missing and no _nvngx.dll/nvngx.dll beside the exe");
        return false;
    }
    log_line("NGX core: %s (%llu bytes)", core_path.c_str(), file_size(core_path));

    // Loading by name (with the driver directory in the search path) makes the plugin's
    // internal lookup of "_nvngx.dll" resolve to this module.
    const char *core_name = (core_path.find("_nvngx.dll") != std::string::npos) ? "_nvngx.dll" : "nvngx.dll";
    ngx->core = LoadLibraryA(core_name);
    if (!ngx->core) {
        log_line("LoadLibraryA(%s) failed: %s (trying the full path)", core_name, load_error_text().c_str());
        ngx->core = LoadLibraryA(core_path.c_str());
    }
    if (!ngx->core) {
        log_line("NGX core did not load: %s", load_error_text().c_str());
        return false;
    }
    char loaded_path[MAX_PATH] = {0};
    GetModuleFileNameA(ngx->core, loaded_path, MAX_PATH);
    log_line("NGX core loaded from: %s", loaded_path);

    std::string plugin_path = join_path(dlss_dir, "nvngx_dlss.dll");
    if (!file_exists(plugin_path)) {
        log_line("missing DLSS plugin: %s", plugin_path.c_str());
        return false;
    }
    ngx->plugin = LoadLibraryA(plugin_path.c_str());
    if (!ngx->plugin) {
        log_line("nvngx_dlss.dll did not load: %s", load_error_text().c_str());
        return false;
    }
    log_line("DLSS plugin: %s (%llu bytes)", plugin_path.c_str(), file_size(plugin_path));

    std::string fg_path = join_path(dlss_dir, "nvngx_dlssg.dll");
    if (file_exists(fg_path)) {
        ngx->plugin_fg = LoadLibraryA(fg_path.c_str());
        if (!ngx->plugin_fg)
            log_line("nvngx_dlssg.dll present but did not load: %s", load_error_text().c_str());
        else
            log_line("DLSS-G plugin: %s (%llu bytes)", fg_path.c_str(), file_size(fg_path));
    } else {
        log_line("no nvngx_dlssg.dll beside the exe; Frame Generation will report unavailable");
    }

    ngx->init = resolve_symbol<Mk8NgxInitWithProjectIdFn>(ngx->core, "NVSDK_NGX_VULKAN_Init_with_ProjectID");
    ngx->init9 = resolve_symbol<HgNgxInitProjectId9Fn>(ngx->core, "NVSDK_NGX_VULKAN_Init_ProjectID");
    if (ngx->init9)
        log_line("core exposes NVSDK_NGX_VULKAN_Init_ProjectID (9-argument driver form); "
                 "the SDK spelling Init_with_ProjectID is %s",
                 ngx->init ? "also present" : "absent");
    ngx->get_capability = resolve_symbol<Mk8NgxGetCapabilityParametersFn>(ngx->core, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    ngx->set_ui = resolve_symbol<Mk8NgxParameterSetUiFn>(ngx->core, "NVSDK_NGX_Parameter_SetUI");
    ngx->set_i = resolve_symbol<Mk8NgxParameterSetIFn>(ngx->core, "NVSDK_NGX_Parameter_SetI");
    ngx->set_f = resolve_symbol<Mk8NgxParameterSetFFn>(ngx->core, "NVSDK_NGX_Parameter_SetF");
    ngx->set_pointer = resolve_symbol<Mk8NgxParameterSetVoidPointerFn>(ngx->core, "NVSDK_NGX_Parameter_SetVoidPointer");
    ngx->get_ui = resolve_symbol<Mk8NgxParameterGetUiFn>(ngx->core, "NVSDK_NGX_Parameter_GetUI");
    ngx->get_ull = resolve_symbol<Mk8NgxParameterGetUllFn>(ngx->core, "NVSDK_NGX_Parameter_GetULL");
    ngx->shutdown1 = resolve_symbol<Mk8NgxShutdown1Fn>(ngx->core, "NVSDK_NGX_VULKAN_Shutdown1");
    ngx->required_extensions = resolve_symbol<Mk8NgxRequiredExtensionsFn>(ngx->core, "NVSDK_NGX_VULKAN_RequiredExtensions");
    // The core owns the dispatch: its CreateFeature/EvaluateFeature route to the plugin
    // that Init loaded. The plugin's own copies keep separate state and return
    // FAIL_NotInitialized when called directly (verified on 621.01 + 310.9.16).
    ngx->create_feature1 = resolve_symbol<Mk8NgxCreateFeature1Fn>(ngx->core, "NVSDK_NGX_VULKAN_CreateFeature1");
    ngx->create_feature = resolve_symbol<Mk8NgxCreateFeatureFn>(ngx->core, "NVSDK_NGX_VULKAN_CreateFeature");
    ngx->evaluate = resolve_symbol<Mk8NgxEvaluateFeatureFn>(ngx->core, "NVSDK_NGX_VULKAN_EvaluateFeature");
    ngx->release = resolve_symbol<Mk8NgxReleaseFeatureFn>(ngx->core, "NVSDK_NGX_VULKAN_ReleaseFeature");
    if (!ngx->create_feature1 || !ngx->evaluate || !ngx->release) {
        log_line("core is missing a feature entry point; falling back to the plugin's exports");
        if (!ngx->create_feature1) ngx->create_feature1 = resolve_symbol<Mk8NgxCreateFeature1Fn>(ngx->plugin, "NVSDK_NGX_VULKAN_CreateFeature1");
        if (!ngx->create_feature) ngx->create_feature = resolve_symbol<Mk8NgxCreateFeatureFn>(ngx->plugin, "NVSDK_NGX_VULKAN_CreateFeature");
        if (!ngx->evaluate) ngx->evaluate = resolve_symbol<Mk8NgxEvaluateFeatureFn>(ngx->plugin, "NVSDK_NGX_VULKAN_EvaluateFeature");
        if (!ngx->release) ngx->release = resolve_symbol<Mk8NgxReleaseFeatureFn>(ngx->plugin, "NVSDK_NGX_VULKAN_ReleaseFeature");
    }
    ngx->get_requirements = resolve_symbol<Mk8NgxGetFeatureRequirementsFn>(ngx->plugin, "NVSDK_NGX_VULKAN_GetFeatureRequirements");

    if (!ngx->set_ui || !ngx->set_i || !ngx->set_f || !ngx->set_pointer || !ngx->get_ui) {
        log_line("NGX runtime exports no NVSDK_NGX_Parameter_* C helpers; using the "
                 "NVSDK_NGX_Parameter vtable (as nvsdk_ngx_params.h defines it)");
        if (!ngx->set_ui) ngx->set_ui = vtable_set_ui;
        if (!ngx->set_i) ngx->set_i = vtable_set_i;
        if (!ngx->set_f) ngx->set_f = vtable_set_f;
        if (!ngx->set_pointer) ngx->set_pointer = vtable_set_pointer;
        if (!ngx->get_ui) ngx->get_ui = vtable_get_ui;
        if (!ngx->get_ull) ngx->get_ull = vtable_get_ull;
    }

    bool ok = (ngx->init || ngx->init9) && ngx->get_capability && ngx->set_ui && ngx->set_i && ngx->set_f &&
              ngx->set_pointer && ngx->get_ui && ngx->evaluate && ngx->release &&
              (ngx->create_feature1 || ngx->create_feature);
    if (!ok) {
        log_line("NGX exports missing on this core/plugin pair (init=%d capability=%d evaluate=%d)",
                 ngx->init != NULL, ngx->get_capability != NULL, ngx->evaluate != NULL);
        return false;
    }
    log_line("NGX exports resolved (core + nvngx_dlss.dll)");
    return true;
}

static bool ngx_available(HgNgx *ngx, const char *name, unsigned *out) {
    unsigned value = 0;
    int rc = ngx->get_ui(ngx->params, name, &value);
    if (MK8_NGX_FAILED(rc) && ngx->get_ull) {
        unsigned long long wide = 0;
        if (MK8_NGX_SUCCEEDED(ngx->get_ull(ngx->params, name, &wide))) {
            value = (unsigned)wide;
            rc = MK8_NGX_RESULT_SUCCESS;
        }
    }
    if (MK8_NGX_FAILED(rc)) {
        log_line("parameter %s could not be read: 0x%08x", name, (unsigned)rc);
        return false;
    }
    *out = value;
    return true;
}

// ---------------------------------------------------------------------------------------
// Vulkan setup
// ---------------------------------------------------------------------------------------
static bool collect_ngx_extensions(Mk8NgxRequiredExtensionsFn required,
                                   std::vector<std::string> *instance_exts,
                                   std::vector<std::string> *device_exts) {
    unsigned instance_count = 0, device_count = 0;
    const char **instance_names = NULL;
    const char **device_names = NULL;
    int rc = required(&instance_count, &instance_names, &device_count, &device_names);
    if (MK8_NGX_FAILED(rc)) {
        log_line("NVSDK_NGX_VULKAN_RequiredExtensions failed: 0x%08x (%s)", (unsigned)rc, result_name(rc));
        return false;
    }
    log_line("NGX required extensions: %u instance, %u device", instance_count, device_count);
    for (unsigned i = 0; i < instance_count; i++) {
        const char *name = instance_names[i];
        if (!name) continue;
        bool available = has_string(g_instance_extensions, name);
        log_line("  instance: %s%s", name, available ? "" : " (NOT AVAILABLE FROM THE LOADER)");
        add_unique(*instance_exts, name);
    }
    for (unsigned i = 0; i < device_count; i++) {
        const char *name = device_names[i];
        if (!name) continue;
        log_line("  device: %s", name);
        add_unique(*device_exts, name);
    }
    return true;
}

static void print_driver_properties(VkPhysicalDevice phys) {
    VkPhysicalDeviceProperties props;
    vkfn_vkGetPhysicalDeviceProperties(phys, &props);
    log_line("GPU: %s (type %d, api %u.%u.%u)", props.deviceName, (int)props.deviceType,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion));
    log_line("Vulkan driverVersion=0x%08x vendorID=0x%04x deviceID=0x%04x",
             props.driverVersion, props.vendorID, props.deviceID);

    VkPhysicalDeviceProperties2 props2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceDriverProperties driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    props2.pNext = &driver;
    vkfn_vkGetPhysicalDeviceProperties2(phys, &props2);
    log_line("Vulkan driver: %s %s (conformance %u.%u.%u)", driver.driverName, driver.driverInfo,
             driver.conformanceVersion.major, driver.conformanceVersion.minor,
             driver.conformanceVersion.subminor);
}

static bool vulkan_setup(const std::vector<std::string> &ngx_instance_exts,
                         const std::vector<std::string> &ngx_device_exts) {
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkfn_vkEnumerateInstanceVersion(&loader_version);
    log_line("Vulkan loader: %u.%u.%u", VK_API_VERSION_MAJOR(loader_version),
             VK_API_VERSION_MINOR(loader_version), VK_API_VERSION_PATCH(loader_version));

    std::vector<std::string> instance_exts;
    for (const std::string &name : ngx_instance_exts) {
        if (!has_string(g_instance_extensions, name.c_str())) {
            log_line("skipping unsupported instance extension %s", name.c_str());
            continue;
        }
        add_unique(instance_exts, name.c_str());
    }
    // Dependencies the loader asks for when the *_win32 extensions are enabled.
    if (has_string(instance_exts, "VK_KHR_external_memory_win32") ||
        has_string(instance_exts, "VK_KHR_external_semaphore_win32"))
        add_unique(instance_exts, "VK_KHR_get_physical_device_properties2");
    if (has_string(instance_exts, "VK_KHR_external_memory_win32"))
        add_unique(instance_exts, "VK_KHR_external_memory_capabilities");
    if (has_string(instance_exts, "VK_KHR_external_semaphore_win32"))
        add_unique(instance_exts, "VK_KHR_external_semaphore_capabilities");

    std::vector<const char *> instance_names;
    for (const std::string &name : instance_exts) instance_names.push_back(name.c_str());

    VkApplicationInfo application = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    application.pApplicationName = "mk8-dlss-rtx-harness";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "mk8-online";
    application.engineVersion = VK_MAKE_VERSION(4, 0, 1);
    application.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo create = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create.pApplicationInfo = &application;
    create.enabledExtensionCount = (uint32_t)instance_names.size();
    create.ppEnabledExtensionNames = instance_names.empty() ? NULL : instance_names.data();
    VkResult rc = vkfn_vkCreateInstance(&create, NULL, &g_instance);
    if (rc != VK_SUCCESS) {
        log_line("vkCreateInstance failed: %d", rc);
        return false;
    }
    if (!load_instance_functions(g_instance)) return false;
    log_line("Vulkan instance created (api 1.3, %u NGX instance extensions)", (unsigned)instance_names.size());

    uint32_t device_count = 0;
    if (vkfn_vkEnumeratePhysicalDevices(g_instance, &device_count, NULL) != VK_SUCCESS || !device_count) {
        log_line("no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(device_count);
    vkfn_vkEnumeratePhysicalDevices(g_instance, &device_count, devices.data());
    for (uint32_t i = 0; i < device_count; i++) {
        VkPhysicalDeviceProperties props;
        vkfn_vkGetPhysicalDeviceProperties(devices[i], &props);
        log_line("physical device %u: %s", i, props.deviceName);
        if (!g_phys && props.vendorID == 0x10DE) g_phys = devices[i];
    }
    if (!g_phys) g_phys = devices[0];

    print_driver_properties(g_phys);
    enumerate_device_extensions(g_phys);

    // Feature requirements for SR and (if the plugin is here) FG, before feature creation.
    // Cheap and it is exactly the diagnostic the bridge logs on a failing machine.
    if (g_phys) {
        VkPhysicalDeviceProperties props;
        vkfn_vkGetPhysicalDeviceProperties(g_phys, &props);
        (void)props;
    }

    if (!queue_family_compute(g_phys, &g_queue_family)) {
        log_line("no compute queue family");
        return false;
    }
    log_line("compute queue family: %u", g_queue_family);

    std::vector<std::string> device_exts;
    for (const std::string &name : ngx_device_exts) {
        if (!has_string(g_device_extensions, name.c_str())) {
            log_line("skipping unsupported device extension %s", name.c_str());
            continue;
        }
        add_unique(device_exts, name.c_str());
    }
    if (has_string(device_exts, "VK_KHR_external_memory_win32"))
        add_unique(device_exts, "VK_KHR_external_memory");
    if (has_string(device_exts, "VK_KHR_external_semaphore_win32"))
        add_unique(device_exts, "VK_KHR_external_semaphore");
    if (has_string(device_exts, "VK_KHR_external_fence_win32"))
        add_unique(device_exts, "VK_KHR_external_fence");

    std::vector<const char *> device_names;
    for (const std::string &name : device_exts) {
        if (!has_string(g_device_extensions, name.c_str())) {
            log_line("skipping unsupported device extension %s", name.c_str());
            continue;
        }
        device_names.push_back(name.c_str());
    }
    log_line("enabling %u device extensions", (unsigned)device_names.size());

    VkPhysicalDeviceFeatures supported;
    vkfn_vkGetPhysicalDeviceFeatures(g_phys, &supported);

    VkPhysicalDeviceVulkan12Features available12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features available13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features2.pNext = &available12;
    available12.pNext = &available13;
    vkfn_vkGetPhysicalDeviceFeatures2(g_phys, &features2);

    VkPhysicalDeviceVulkan12Features enabled12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.bufferDeviceAddress = available12.bufferDeviceAddress;
    enabled12.descriptorIndexing = available12.descriptorIndexing;
    enabled12.shaderSampledImageArrayNonUniformIndexing = available12.shaderSampledImageArrayNonUniformIndexing;
    enabled12.shaderStorageImageArrayNonUniformIndexing = available12.shaderStorageImageArrayNonUniformIndexing;
    enabled12.descriptorBindingSampledImageUpdateAfterBind = available12.descriptorBindingSampledImageUpdateAfterBind;
    enabled12.descriptorBindingStorageImageUpdateAfterBind = available12.descriptorBindingStorageImageUpdateAfterBind;
    enabled12.descriptorBindingPartiallyBound = available12.descriptorBindingPartiallyBound;
    enabled12.descriptorBindingVariableDescriptorCount = available12.descriptorBindingVariableDescriptorCount;
    enabled12.runtimeDescriptorArray = available12.runtimeDescriptorArray;
    enabled12.timelineSemaphore = available12.timelineSemaphore;
    enabled12.scalarBlockLayout = available12.scalarBlockLayout;
    enabled12.uniformBufferStandardLayout = available12.uniformBufferStandardLayout;
    enabled12.hostQueryReset = available12.hostQueryReset;

    VkPhysicalDeviceVulkan13Features enabled13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.synchronization2 = available13.synchronization2;
    enabled13.dynamicRendering = available13.dynamicRendering;
    enabled13.maintenance4 = available13.maintenance4;

    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = g_queue_family;
    queue.queueCount = 1;
    float priority = 1.0f;
    queue.pQueuePriorities = &priority;

    VkDeviceCreateInfo create_device = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_device.pNext = &enabled12;
    enabled12.pNext = &enabled13;
    create_device.queueCreateInfoCount = 1;
    create_device.pQueueCreateInfos = &queue;
    create_device.enabledExtensionCount = (uint32_t)device_names.size();
    create_device.ppEnabledExtensionNames = device_names.empty() ? NULL : device_names.data();
    create_device.pEnabledFeatures = &supported;

    rc = vkfn_vkCreateDevice(g_phys, &create_device, NULL, &g_device);
    if (rc != VK_SUCCESS) {
        log_line("vkCreateDevice failed: %d", rc);
        return false;
    }
    g_gdpa = (PFN_vkGetDeviceProcAddr)g_gipa(g_instance, "vkGetDeviceProcAddr");
    if (!g_gdpa) {
        log_line("loader did not provide vkGetDeviceProcAddr");
        return false;
    }
    if (!load_device_functions(g_device)) return false;
    vkfn_vkGetDeviceQueue(g_device, g_queue_family, 0, &g_queue);

    VkCommandPoolCreateInfo pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = g_queue_family;
    if (vkfn_vkCreateCommandPool(g_device, &pool, NULL, &g_pool) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo allocate = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = g_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkfn_vkAllocateCommandBuffers(g_device, &allocate, &g_cb) != VK_SUCCESS) return false;
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkfn_vkCreateFence(g_device, &fence, NULL, &g_fence) != VK_SUCCESS) return false;

    log_line("Vulkan device created: %u device extensions, compute queue %u", (unsigned)device_names.size(),
             g_queue_family);
    log_line("VK_NV_low_latency2 present: %s", has_string(g_device_extensions, "VK_NV_low_latency2") ? "yes" : "no");
    return true;
}

// ---------------------------------------------------------------------------------------
// SR driver: create, evaluate N frames, read back
// ---------------------------------------------------------------------------------------
struct HgSizes {
    uint32_t rw = 1280;
    uint32_t rh = 720;
    uint32_t ow = 2560;
    uint32_t oh = 1440;
};

static bool run_super_resolution(HgNgx *ngx, const HgSizes &sizes, const std::string &output_dir) {
    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depth_format = VK_FORMAT_R32_SFLOAT;
    const VkFormat motion_format = VK_FORMAT_R32G32_SFLOAT;
    const VkFormat output_format = VK_FORMAT_R8G8B8A8_UNORM;
    const uint32_t sentinel = 0xFFFF00FFu;  // A=FF B=00 G=00 R=FF, magenta

    HgImage color, depth, motion, output;
    HgBuffer color_upload, depth_upload, motion_upload, readback;
    bool ok = false;
    Mk8NgxHandle *handle = NULL;
    std::string output_png;
    Mk8NgxParameter *params = NULL;
    uint8_t *cpu_color = NULL;
    float *cpu_depth = NULL;
    float *cpu_motion = NULL;
    uint8_t *last_output = NULL;
    HgStats last_stats;
    bool have_output = false;
    int frames = 10;
    unsigned init_result = 0;
    int rc = 0;


    if (!create_image(&color, color_format, sizes.rw, sizes.rh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&depth, depth_format, sizes.rw, sizes.rh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&motion, motion_format, sizes.rw, sizes.rh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&output, output_format, sizes.ow, sizes.oh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        goto done;

    if (!create_buffer(&color_upload, (VkDeviceSize)sizes.rw * sizes.rh * 4,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !create_buffer(&depth_upload, (VkDeviceSize)sizes.rw * sizes.rh * 4,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !create_buffer(&motion_upload, (VkDeviceSize)sizes.rw * sizes.rh * 8,
                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !create_buffer(&readback, (VkDeviceSize)sizes.ow * sizes.oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        goto done;

    params = ngx->params;
    ngx->set_ui(params, MK8_NGX_PARAM_CREATION_NODE_MASK, 1);
    ngx->set_ui(params, MK8_NGX_PARAM_VISIBILITY_NODE_MASK, 1);
    ngx->set_ui(params, MK8_NGX_PARAM_WIDTH, sizes.rw);
    ngx->set_ui(params, MK8_NGX_PARAM_HEIGHT, sizes.rh);
    ngx->set_ui(params, MK8_NGX_PARAM_OUT_WIDTH, sizes.ow);
    ngx->set_ui(params, MK8_NGX_PARAM_OUT_HEIGHT, sizes.oh);
    ngx->set_i(params, MK8_NGX_PARAM_PERF_QUALITY, MK8_NGX_PERFQUALITY_MAX_PERF);
    ngx->set_i(params, MK8_NGX_PARAM_DLSS_CREATE_FLAGS, MK8_NGX_DLSS_FLAG_NONE);
    ngx->set_i(params, MK8_NGX_PARAM_DLSS_ENABLE_OUTPUT_SUBRECTS, 0);

    vkfn_vkDeviceWaitIdle(g_device);
    if (!begin_commands()) {
        log_line("vkBeginCommandBuffer failed for CreateFeature1");
        goto done;
    }
    log_line("creating SuperSampling feature %ux%u -> %ux%u (PERFORMANCE)", sizes.rw, sizes.rh,
             sizes.ow, sizes.oh);
    rc = ngx->create_feature1(g_device, g_cb, MK8_NGX_FEATURE_SUPER_SAMPLING, params, &handle);
    log_result("NVSDK_NGX_VULKAN_CreateFeature1(SuperSampling)", rc);
    if (MK8_NGX_FAILED(rc) || !handle) goto done;
    if (!submit_and_wait()) {
        log_line("submitting the CreateFeature1 command buffer failed");
        goto done;
    }
    if (ngx->get_ui(params, MK8_NGX_PARAM_SUPER_SAMPLING_FEATURE_INIT_RESULT, &init_result))
        log_line("SuperSampling.FeatureInitResult = 0x%08x (%s)", init_result, result_name((int)init_result));

    cpu_color = (uint8_t *)malloc((size_t)sizes.rw * sizes.rh * 4);
    cpu_depth = (float *)malloc((size_t)sizes.rw * sizes.rh * 4);
    cpu_motion = (float *)malloc((size_t)sizes.rw * sizes.rh * 8);
    if (!cpu_color || !cpu_depth || !cpu_motion) {
        log_line("out of host memory for the synthetic frames");
        free(cpu_color);
        free(cpu_depth);
        free(cpu_motion);
        goto done;
    }

    frames = 10;
    last_output = (uint8_t *)malloc((size_t)sizes.ow * sizes.oh * 4);
    for (int frame = 0; frame < frames; frame++) {
        fill_frame(sizes.rw, sizes.rh, frame, 2, cpu_color, cpu_depth, cpu_motion);
        memcpy(color_upload.mapped, cpu_color, (size_t)sizes.rw * sizes.rh * 4);
        memcpy(depth_upload.mapped, cpu_depth, (size_t)sizes.rw * sizes.rh * 4);
        memcpy(motion_upload.mapped, cpu_motion, (size_t)sizes.rw * sizes.rh * 8);

        if (!begin_commands()) {
            log_line("vkBeginCommandBuffer failed on frame %d", frame);
            break;
        }

        // Upload the three inputs.
        HgImage *inputs[3] = {&color, &depth, &motion};
        HgBuffer *uploads[3] = {&color_upload, &depth_upload, &motion_upload};
        for (int i = 0; i < 3; i++) {
            HgImage *image = inputs[i];
            if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED)
                image_barrier(g_cb, image->image, image->aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            else
                image_barrier(g_cb, image->image, image->aspect, image->layout,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region = {};
            region.imageSubresource.aspectMask = image->aspect;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = (VkExtent3D){image->width, image->height, 1};
            vkfn_vkCmdCopyBufferToImage(g_cb, uploads[i]->buffer, image->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            // Frame 3 deliberately uses the layout our bridge assumes for inputs
            // (SHADER_READ_ONLY_OPTIMAL) to see whether NGX accepts it.
            VkImageLayout target = (frame == 3) ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                                : VK_IMAGE_LAYOUT_GENERAL;
            image_barrier(g_cb, image->image, image->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            image->layout = target;
        }

        // Clear the output to the sentinel so "NGX wrote nothing" is distinguishable from
        // "NGX wrote a uniform image".
        if (output.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            image_barrier(g_cb, output.image, output.aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        else
            image_barrier(g_cb, output.image, output.aspect, output.layout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear = {};
        clear.float32[0] = 1.0f;
        clear.float32[1] = 0.0f;
        clear.float32[2] = 1.0f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range = {output.aspect, 0, 1, 0, 1};
        vkfn_vkCmdClearColorImage(g_cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        image_barrier(g_cb, output.image, output.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        output.layout = VK_IMAGE_LAYOUT_GENERAL;

        Mk8NgxResourceVk color_resource = {};
        color_resource.Resource.ImageViewInfo.ImageView = color.view;
        color_resource.Resource.ImageViewInfo.Image = color.image;
        color_resource.Resource.ImageViewInfo.SubresourceRange = (VkImageSubresourceRange){color.aspect, 0, 1, 0, 1};
        color_resource.Resource.ImageViewInfo.Format = color_format;
        color_resource.Resource.ImageViewInfo.Width = sizes.rw;
        color_resource.Resource.ImageViewInfo.Height = sizes.rh;
        color_resource.Type = 0;
        color_resource.ReadWrite = false;

        Mk8NgxResourceVk depth_resource = color_resource;
        depth_resource.Resource.ImageViewInfo.ImageView = depth.view;
        depth_resource.Resource.ImageViewInfo.Image = depth.image;
        depth_resource.Resource.ImageViewInfo.Format = depth_format;

        Mk8NgxResourceVk motion_resource = color_resource;
        motion_resource.Resource.ImageViewInfo.ImageView = motion.view;
        motion_resource.Resource.ImageViewInfo.Image = motion.image;
        motion_resource.Resource.ImageViewInfo.Format = motion_format;

        Mk8NgxResourceVk output_resource = color_resource;
        output_resource.Resource.ImageViewInfo.ImageView = output.view;
        output_resource.Resource.ImageViewInfo.Image = output.image;
        output_resource.Resource.ImageViewInfo.Format = output_format;
        output_resource.Resource.ImageViewInfo.Width = sizes.ow;
        output_resource.Resource.ImageViewInfo.Height = sizes.oh;
        output_resource.ReadWrite = true;

        const float jitter_x = halton(frame + 1, 2) - 0.5f;
        const float jitter_y = halton(frame + 1, 3) - 0.5f;
        const int pointer_slot = g_pointer_setter;
        const bool float_probe = g_float_slot < 0;
        const int float_slot = float_probe ? ((frame % 2 == 0) ? 6 : 5) : g_float_slot;
        g_float_slot = float_slot;
        vtable_set_pointer_index(params, pointer_slot, MK8_NGX_PARAM_COLOR, &color_resource);
        vtable_set_pointer_index(params, pointer_slot, MK8_NGX_PARAM_OUTPUT, &output_resource);
        vtable_set_pointer_index(params, pointer_slot, MK8_NGX_PARAM_DEPTH, &depth_resource);
        vtable_set_pointer_index(params, pointer_slot, MK8_NGX_PARAM_MOTION_VECTORS, &motion_resource);
        ngx->set_f(params, MK8_NGX_PARAM_JITTER_X, jitter_x);
        ngx->set_f(params, MK8_NGX_PARAM_JITTER_Y, jitter_y);
        ngx->set_f(params, MK8_NGX_PARAM_MV_SCALE_X, 1.0f);
        ngx->set_f(params, MK8_NGX_PARAM_MV_SCALE_Y, 1.0f);
        ngx->set_f(params, MK8_NGX_PARAM_SHARPNESS, 0.0f);
        ngx->set_f(params, MK8_NGX_PARAM_PRE_EXPOSURE, 1.0f);
        ngx->set_f(params, MK8_NGX_PARAM_FRAME_TIME_DELTA_MS, 16.6f);
        ngx->set_ui(params, MK8_NGX_PARAM_RENDER_SUBRECT_WIDTH, sizes.rw);
        ngx->set_ui(params, MK8_NGX_PARAM_RENDER_SUBRECT_HEIGHT, sizes.rh);
        ngx->set_ui(params, MK8_NGX_PARAM_RESET, (frame == 0 || float_probe) ? 1 : 0);

        rc = ngx->evaluate(g_cb, handle, params, NULL);
        log_line("frame %d: EvaluateFeature 0x%08x (%s), pointer slot %d, float slot %d, jitter %.3f/%.3f, "
                 "input layout %s",
                 frame, (unsigned)rc, result_name(rc), pointer_slot, float_slot, jitter_x, jitter_y,
                 frame == 3 ? "SHADER_READ_ONLY_OPTIMAL" : "GENERAL");
        if (MK8_NGX_SUCCEEDED(rc) && float_probe)
            log_line("Set(float) is vtable slot %d (discovered by EvaluateFeature)", float_slot);
        if (MK8_NGX_FAILED(rc) && float_probe) g_float_slot = -1;

        image_barrier(g_cb, output.image, output.aspect, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = output.aspect;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = (VkExtent3D){sizes.ow, sizes.oh, 1};
        vkfn_vkCmdCopyImageToBuffer(g_cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                               &region);
        image_barrier(g_cb, output.image, output.aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        output.layout = VK_IMAGE_LAYOUT_GENERAL;

        if (!submit_and_wait()) {
            log_line("frame %d: submit/wait failed", frame);
            break;
        }
        if (MK8_NGX_FAILED(rc)) continue;

        const uint8_t *pixels = (const uint8_t *)readback.mapped;
        HgStats stats = compute_stats(pixels, sizes.ow, sizes.oh, sentinel);
        print_stats("sr frame", stats, sentinel);
        memcpy(last_output, pixels, (size_t)sizes.ow * sizes.oh * 4);
        last_stats = stats;
        have_output = true;
    }

    if (have_output) {
        output_png = join_path(output_dir, "sr_2560x1440.png");
        if (write_png_rgba8(output_png, last_output, sizes.ow, sizes.oh))
            log_line("wrote %s", output_png.c_str());
        write_png_rgba8(join_path(output_dir, "sr_input_1280x720.png"), cpu_color, sizes.rw, sizes.rh);

        // A uniform image (all sentinel, or one distinct pixel value) is not a result.
        const bool structured = last_stats.sentinel_pixels < (sizes.ow * sizes.oh) / 2 &&
                                last_stats.distinct_pixels > 64;
        log_line("SR verdict: %s (distinct_pixels=%u sentinel_pixels=%u horiz_gradient=%.3f)",
                 structured ? "STRUCTURED OUTPUT" : "NOT A REAL FRAME", last_stats.distinct_pixels,
                 last_stats.sentinel_pixels, last_stats.horizontal_gradient);
        ok = structured;
    }

    free(last_output);
    free(cpu_color);
    free(cpu_depth);
    free(cpu_motion);

done:
    if (handle && ngx->release) {
        int rc = ngx->release(handle);
        log_result("NVSDK_NGX_VULKAN_ReleaseFeature(SuperSampling)", rc);
    }
    destroy_buffer(&readback);
    destroy_buffer(&motion_upload);
    destroy_buffer(&depth_upload);
    destroy_buffer(&color_upload);
    destroy_image(&output);
    destroy_image(&motion);
    destroy_image(&depth);
    destroy_image(&color);
    return ok;
}

// ---------------------------------------------------------------------------------------
// Frame Generation: availability, create, headless evaluate
// ---------------------------------------------------------------------------------------
static bool run_frame_generation(HgNgx *ngx, const HgSizes &sizes, const std::string &output_dir) {
    unsigned available = 0;
    if (!ngx_available(ngx, HG_FG_AVAILABLE, &available)) return false;
    log_line("FrameGeneration.Available = %u", available);

    unsigned interpolation = 0;
    if (ngx_available(ngx, HG_FI_AVAILABLE, &interpolation))
        log_line("FrameInterpolation.Available = %u", interpolation);

    unsigned needs_driver = 0;
    if (ngx_available(ngx, HG_FG_NEEDS_UPDATED_DRIVER, &needs_driver))
        log_line("FrameGeneration.NeedsUpdatedDriver = %u", needs_driver);
    unsigned minimum_major = 0, minimum_minor = 0;
    if (ngx_available(ngx, HG_FG_MIN_DRIVER_MAJOR, &minimum_major) &&
        ngx_available(ngx, HG_FG_MIN_DRIVER_MINOR, &minimum_minor))
        log_line("FrameGeneration.MinDriverVersion = %u.%u", minimum_major, minimum_minor);

    if (!ngx->plugin_fg || available != 1) {
        log_line("Frame Generation not attempted: plugin=%s available=%u", ngx->plugin_fg ? "loaded" : "missing",
                 available);
        return false;
    }

    const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    const VkFormat depth_format = VK_FORMAT_R32_SFLOAT;
    const VkFormat motion_format = VK_FORMAT_R32G32_SFLOAT;
    const uint32_t sentinel = 0xFFFF00FFu;

    HgImage backbuffer, depth, motion, interpolated;
    HgBuffer depth_upload, motion_upload, readback;
    Mk8NgxHandle *handle = NULL;
    bool ok = false;
    uint8_t *last_output = NULL;
    std::string fg_png_path;
    Mk8NgxParameter *params = NULL;
    uint8_t *cpu_color = NULL;
    float *cpu_depth = NULL;
    float *cpu_motion = NULL;
    uint8_t *cpu_backbuffer = NULL;
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float camera_view_to_clip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float clip_to_camera_view[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    int frames = 3;
    unsigned init_result = 0;
    int rc = 0;

    if (!create_image(&backbuffer, color_format, sizes.ow, sizes.oh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&depth, depth_format, sizes.rw, sizes.rh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&motion, motion_format, sizes.rw, sizes.rh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) ||
        !create_image(&interpolated, color_format, sizes.ow, sizes.oh,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        goto done;

    if (!create_buffer(&depth_upload, (VkDeviceSize)sizes.rw * sizes.rh * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !create_buffer(&motion_upload, (VkDeviceSize)sizes.rw * sizes.rh * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !create_buffer(&readback, (VkDeviceSize)sizes.ow * sizes.oh * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        goto done;

    params = ngx->params;
    ngx->set_ui(params, MK8_NGX_PARAM_CREATION_NODE_MASK, 1);
    ngx->set_ui(params, MK8_NGX_PARAM_VISIBILITY_NODE_MASK, 1);
    ngx->set_ui(params, MK8_NGX_PARAM_WIDTH, sizes.ow);
    ngx->set_ui(params, MK8_NGX_PARAM_HEIGHT, sizes.oh);
    ngx->set_ui(params, HG_FG_BACKBUFFER_FORMAT, (unsigned)color_format);

    vkfn_vkDeviceWaitIdle(g_device);
    if (!begin_commands()) goto done;
    log_line("creating FrameGeneration feature %ux%u, backbuffer format %d", sizes.ow, sizes.oh, (int)color_format);
    // The v310.9.16 SDK helper uses NVSDK_NGX_VULKAN_CreateFeature (no VkDevice) for FG.
    rc = ngx->create_feature ? ngx->create_feature(g_cb, HG_NGX_FEATURE_FRAME_GENERATION, params, &handle)
                             : MK8_NGX_RESULT_FAIL;
    log_result("NVSDK_NGX_VULKAN_CreateFeature(FrameGeneration)", rc);
    if (MK8_NGX_FAILED(rc) || !handle) {
        if (ngx->create_feature1) {
            log_line("retrying Frame Generation creation with CreateFeature1(VkDevice)");
            handle = NULL;
            rc = ngx->create_feature1(g_device, g_cb, HG_NGX_FEATURE_FRAME_GENERATION, params, &handle);
            log_result("NVSDK_NGX_VULKAN_CreateFeature1(FrameGeneration)", rc);
        }
        if (MK8_NGX_FAILED(rc) || !handle) {
            log_line("Frame Generation feature creation failed; evaluating it is impossible without a handle");
            goto done;
        }
    }
    if (!submit_and_wait()) goto done;
    if (ngx->get_ui(params, HG_FG_INIT_RESULT, &init_result))
        log_line("FrameGeneration.FeatureInitResult = 0x%08x (%s)", init_result, result_name((int)init_result));

    cpu_color = (uint8_t *)malloc((size_t)sizes.rw * sizes.rh * 4);
    cpu_depth = (float *)malloc((size_t)sizes.rw * sizes.rh * 4);
    cpu_motion = (float *)malloc((size_t)sizes.rw * sizes.rh * 8);
    cpu_backbuffer = (uint8_t *)malloc((size_t)sizes.ow * sizes.oh * 4);
    last_output = (uint8_t *)malloc((size_t)sizes.ow * sizes.oh * 4);
    if (!cpu_color || !cpu_depth || !cpu_motion || !cpu_backbuffer || !last_output) {
        free(cpu_color);
        free(cpu_depth);
        free(cpu_motion);
        free(cpu_backbuffer);
        goto done;
    }

    frames = 3;
    for (int frame = 0; frame < frames; frame++) {
        fill_frame(sizes.rw, sizes.rh, frame, 2, cpu_color, cpu_depth, cpu_motion);
        // A display-resolution "backbuffer" with the same pan, as a game would present it.
        for (uint32_t y = 0; y < sizes.oh; y++) {
            for (uint32_t x = 0; x < sizes.ow; x++) {
                uint32_t sx = x / 2, sy = y / 2;
                size_t src = ((size_t)sy * sizes.rw + sx) * 4;
                size_t dst = ((size_t)y * sizes.ow + x) * 4;
                memcpy(cpu_backbuffer + dst, cpu_color + src, 4);
            }
        }
        memcpy(depth_upload.mapped, cpu_depth, (size_t)sizes.rw * sizes.rh * 4);
        memcpy(motion_upload.mapped, cpu_motion, (size_t)sizes.rw * sizes.rh * 8);

        if (!begin_commands()) break;
        {
            HgImage *inputs[2] = {&depth, &motion};
            HgBuffer *uploads[2] = {&depth_upload, &motion_upload};
            for (int i = 0; i < 2; i++) {
                HgImage *image = inputs[i];
                if (image->layout == VK_IMAGE_LAYOUT_UNDEFINED)
                    image_barrier(g_cb, image->image, image->aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
                else
                    image_barrier(g_cb, image->image, image->aspect, image->layout,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT);
                VkBufferImageCopy region = {};
                region.imageSubresource.aspectMask = image->aspect;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = (VkExtent3D){image->width, image->height, 1};
                vkfn_vkCmdCopyBufferToImage(g_cb, uploads[i]->buffer, image->image,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                image_barrier(g_cb, image->image, image->aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                image->layout = VK_IMAGE_LAYOUT_GENERAL;
            }
        }

        // Backbuffer: upload through a staging path (device-local image, host buffer).
        HgBuffer backbuffer_upload;
        if (!create_buffer(&backbuffer_upload, (VkDeviceSize)sizes.ow * sizes.oh * 4,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            break;
        memcpy(backbuffer_upload.mapped, cpu_backbuffer, (size_t)sizes.ow * sizes.oh * 4);
        if (backbuffer.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            image_barrier(g_cb, backbuffer.image, backbuffer.aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        else
            image_barrier(g_cb, backbuffer.image, backbuffer.aspect, backbuffer.layout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy backbuffer_region = {};
        backbuffer_region.imageSubresource.aspectMask = backbuffer.aspect;
        backbuffer_region.imageSubresource.layerCount = 1;
        backbuffer_region.imageExtent = (VkExtent3D){sizes.ow, sizes.oh, 1};
        vkfn_vkCmdCopyBufferToImage(g_cb, backbuffer_upload.buffer, backbuffer.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &backbuffer_region);
        image_barrier(g_cb, backbuffer.image, backbuffer.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        backbuffer.layout = VK_IMAGE_LAYOUT_GENERAL;

        if (interpolated.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            image_barrier(g_cb, interpolated.image, interpolated.aspect, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        else
            image_barrier(g_cb, interpolated.image, interpolated.aspect, interpolated.layout,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear = {};
        clear.float32[0] = 1.0f;
        clear.float32[1] = 0.0f;
        clear.float32[2] = 1.0f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range = {interpolated.aspect, 0, 1, 0, 1};
        vkfn_vkCmdClearColorImage(g_cb, interpolated.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        image_barrier(g_cb, interpolated.image, interpolated.aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        interpolated.layout = VK_IMAGE_LAYOUT_GENERAL;

        Mk8NgxResourceVk backbuffer_resource = {};
        backbuffer_resource.Resource.ImageViewInfo.ImageView = backbuffer.view;
        backbuffer_resource.Resource.ImageViewInfo.Image = backbuffer.image;
        backbuffer_resource.Resource.ImageViewInfo.SubresourceRange = (VkImageSubresourceRange){backbuffer.aspect, 0, 1, 0, 1};
        backbuffer_resource.Resource.ImageViewInfo.Format = color_format;
        backbuffer_resource.Resource.ImageViewInfo.Width = sizes.ow;
        backbuffer_resource.Resource.ImageViewInfo.Height = sizes.oh;
        backbuffer_resource.Type = 0;
        backbuffer_resource.ReadWrite = false;

        Mk8NgxResourceVk depth_resource = backbuffer_resource;
        depth_resource.Resource.ImageViewInfo.ImageView = depth.view;
        depth_resource.Resource.ImageViewInfo.Image = depth.image;
        depth_resource.Resource.ImageViewInfo.Format = depth_format;
        depth_resource.Resource.ImageViewInfo.Width = sizes.rw;
        depth_resource.Resource.ImageViewInfo.Height = sizes.rh;

        Mk8NgxResourceVk motion_resource = depth_resource;
        motion_resource.Resource.ImageViewInfo.ImageView = motion.view;
        motion_resource.Resource.ImageViewInfo.Image = motion.image;
        motion_resource.Resource.ImageViewInfo.Format = motion_format;

        Mk8NgxResourceVk output_resource = backbuffer_resource;
        output_resource.Resource.ImageViewInfo.ImageView = interpolated.view;
        output_resource.Resource.ImageViewInfo.Image = interpolated.image;
        output_resource.ReadWrite = true;

        const int fg_pointer_slot = getenv("MK8_HARNESS_FG_SETTER")
                                         ? atoi(getenv("MK8_HARNESS_FG_SETTER"))
                                         : (g_pointer_setter >= 0 ? g_pointer_setter : 0);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_BACKBUFFER, &backbuffer_resource);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_MVECS, &motion_resource);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_DEPTH, &depth_resource);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_OUTPUT_INTERPOLATED, &output_resource);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_CAMERA_VIEW_TO_CLIP, camera_view_to_clip);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_CLIP_TO_CAMERA_VIEW, clip_to_camera_view);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_CLIP_TO_PREV_CLIP, identity);
        vtable_set_pointer_index(params, fg_pointer_slot, HG_FG_PREV_CLIP_TO_CLIP, identity);
        ngx->set_f(params, HG_FG_JITTER_X, 0.0f);
        ngx->set_f(params, HG_FG_JITTER_Y, 0.0f);
        ngx->set_f(params, HG_FG_MVEC_SCALE_X, 1.0f);
        ngx->set_f(params, HG_FG_MVEC_SCALE_Y, 1.0f);
        ngx->set_f(params, HG_FG_CAMERA_NEAR, 0.1f);
        ngx->set_f(params, HG_FG_CAMERA_FAR, 1000.0f);
        ngx->set_f(params, HG_FG_CAMERA_FOV, 1.0471976f);
        ngx->set_f(params, HG_FG_CAMERA_ASPECT, (float)sizes.ow / (float)sizes.oh);
        ngx->set_ui(params, HG_FG_COLOR_BUFFERS_HDR, 0);
        ngx->set_ui(params, HG_FG_DEPTH_INVERTED, 0);
        ngx->set_ui(params, HG_FG_CAMERA_MOTION_INCLUDED, 1);
        ngx->set_ui(params, HG_FG_NOT_RENDERING, 0);
        ngx->set_ui(params, HG_FG_ORTHO, 0);
        ngx->set_f(params, HG_FG_MVEC_INVALID, 1.0e9f);
        ngx->set_ui(params, HG_FG_MVEC_DILATED, 0);
        ngx->set_ui(params, HG_FG_MENU_DETECTION, 0);
        ngx->set_ui(params, HG_FG_MULTI_FRAME_COUNT, 1);
        ngx->set_ui(params, HG_FG_MULTI_FRAME_INDEX, 1);
        ngx->set_ui(params, HG_FG_RESET, frame == 0 ? 1 : 0);

        {
            void *probe8 = NULL, *probe15 = NULL;
            HgParamGetPtr get8 = (HgParamGetPtr)vtable_entry(params, 8);
            HgParamGetPtr get15 = (HgParamGetPtr)vtable_entry(params, 15);
            int rc8 = get8 ? get8(params, HG_FG_CLIP_TO_PREV_CLIP, &probe8) : -1;
            int rc15 = get15 ? get15(params, HG_FG_CLIP_TO_PREV_CLIP, &probe15) : -1;
            log_line("fg probe: Get(slot8)=0x%08x ptr=%p, Get(slot15)=0x%08x ptr=%p, expect=%p",
                     (unsigned)rc8, probe8, (unsigned)rc15, probe15, (void *)identity);
        }
        rc = ngx->evaluate(g_cb, handle, params, NULL);
        log_line("fg frame %d: EvaluateFeature 0x%08x (%s)", frame, (unsigned)rc, result_name(rc));

        image_barrier(g_cb, interpolated.image, interpolated.aspect, VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region = {};
        region.imageSubresource.aspectMask = interpolated.aspect;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = (VkExtent3D){sizes.ow, sizes.oh, 1};
        vkfn_vkCmdCopyImageToBuffer(g_cb, interpolated.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                               &region);
        image_barrier(g_cb, interpolated.image, interpolated.aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        interpolated.layout = VK_IMAGE_LAYOUT_GENERAL;

        bool submitted = submit_and_wait();
        destroy_buffer(&backbuffer_upload);
        if (!submitted) {
            log_line("fg frame %d: submit/wait failed", frame);
            break;
        }
        if (MK8_NGX_FAILED(rc)) break;

        HgStats stats = compute_stats((const uint8_t *)readback.mapped, sizes.ow, sizes.oh, sentinel);
        print_stats("fg frame", stats, sentinel);
        // A copy of the backbuffer would be byte-identical to it; a generated frame is not.
        double difference = 0.0;
        const uint8_t *fg_pixels = (const uint8_t *)readback.mapped;
        for (size_t i = 0; i < (size_t)sizes.ow * sizes.oh; i++)
            for (int c = 0; c < 3; c++)
                difference += fabs((double)fg_pixels[i * 4 + c] - (double)cpu_backbuffer[i * 4 + c]);
        difference /= (double)((size_t)sizes.ow * sizes.oh * 3);
        log_line("fg frame %d: mean |interpolated - backbuffer| = %.3f", frame, difference);
        memcpy(last_output, readback.mapped, (size_t)sizes.ow * sizes.oh * 4);
        ok = stats.sentinel_pixels < (sizes.ow * sizes.oh) / 2 && stats.distinct_pixels > 64;
        if (frame == frames - 1)
            write_png_rgba8(join_path(output_dir, "fg_backbuffer_2560x1440.png"), cpu_backbuffer, sizes.ow,
                            sizes.oh);
    }

    if (last_output) {
        fg_png_path = join_path(output_dir, "fg_2560x1440.png");
        if (write_png_rgba8(fg_png_path, last_output, sizes.ow, sizes.oh))
            log_line("wrote %s", fg_png_path.c_str());
    }
    log_line("FG verdict: %s", ok ? "STRUCTURED INTERPOLATED FRAME" : "NO INTERPOLATED FRAME");
    free(cpu_color);
    free(cpu_depth);
    free(cpu_motion);
    free(cpu_backbuffer);

done:
    if (handle && ngx->release) {
        int rc = ngx->release(handle);
        log_result("NVSDK_NGX_VULKAN_ReleaseFeature(FrameGeneration)", rc);
    }
    destroy_buffer(&readback);
    destroy_buffer(&motion_upload);
    destroy_buffer(&depth_upload);
    destroy_image(&interpolated);
    destroy_image(&motion);
    destroy_image(&depth);
    destroy_image(&backbuffer);
    free(last_output);
    return ok;
}

// ---------------------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------------------
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    log_line("mk8 DLSS/NGX Windows harness (x86-64), built against runtime/mk8/graphics/ngx/mk8_ngx_abi.h");
    log_line("ABI: sizeof(Resource_VK)=%u FeatureCommonInfo=%u FeatureDiscoveryInfo=%u DlssCreateParams=%u",
             (unsigned)sizeof(Mk8NgxResourceVk), (unsigned)sizeof(Mk8NgxFeatureCommonInfo),
             (unsigned)sizeof(Mk8NgxFeatureDiscoveryInfo), (unsigned)sizeof(Mk8NgxDlssCreateParams));

    std::string dlss_dir = absolute_path(argc > 1 ? argv[1] : exe_directory());
    std::string output_dir = absolute_path(argc > 2 ? argv[2] : dlss_dir);
    if (output_dir.empty()) output_dir = ".";
    make_directory(output_dir);
    log_line("DLL directory: %s", dlss_dir.c_str());
    log_line("output directory: %s", output_dir.c_str());

    HgNgx ngx;
    if (!ngx_load(&ngx, dlss_dir)) {
        log_line("RESULT: FAIL (loader)");
        return 1;
    }
    if (!load_global_functions()) {
        log_line("RESULT: FAIL (Vulkan loader)");
        return 2;
    }
    if (!enumerate_instance_extensions()) {
        log_line("RESULT: FAIL (instance extension enumeration)");
        return 2;
    }

    // RequiredExtensions is only meaningful before instance/device creation; the bridge
    // cannot use it (the game device already exists), which is why this harness exists.
    std::vector<std::string> ngx_instance_exts, ngx_device_exts;
    collect_ngx_extensions(ngx.required_extensions, &ngx_instance_exts, &ngx_device_exts);

    if (!vulkan_setup(ngx_instance_exts, ngx_device_exts)) {
        log_line("RESULT: FAIL (Vulkan setup)");
        return 2;
    }

    // Feature discovery (what the bridge does before Init) for both features.
    const char *project_id = "6f1d2c3a-8b4e-4d7a-9c15-3e0a5b7c9d21";
    wchar_t data_path[MAX_PATH] = {0};
    {
        char narrow[MAX_PATH] = {0};
        snprintf(narrow, sizeof narrow, "%s\\ngx-data", output_dir.c_str());
        make_directory(narrow);
        MultiByteToWideChar(CP_UTF8, 0, narrow, -1, data_path, MAX_PATH);
    }
    wchar_t wide_dll_dir[MAX_PATH] = {0};
    MultiByteToWideChar(CP_UTF8, 0, dlss_dir.c_str(), -1, wide_dll_dir, MAX_PATH);
    const wchar_t *search_paths[1] = {wide_dll_dir};

    Mk8NgxFeatureCommonInfo common = {};
    common.PathListInfo.Path = search_paths;
    common.PathListInfo.Length = 1;
    common.LoggingInfo.LoggingCallback = getenv("MK8_HARNESS_NO_NGX_LOG") ? NULL : log_ngx;
    common.LoggingInfo.MinimumLoggingLevel = 2;  // verbose
    common.LoggingInfo.DisableOtherLoggingSinks = false;

    const int features[2] = {MK8_NGX_FEATURE_SUPER_SAMPLING, HG_NGX_FEATURE_FRAME_GENERATION};
    const char *feature_names[2] = {"SuperSampling", "FrameGeneration"};
    for (int i = 0; i < 2; i++) {
        Mk8NgxFeatureDiscoveryInfo discovery = {};
        discovery.SDKVersion = MK8_NGX_API_VERSION;
        discovery.FeatureID = features[i];
        discovery.Identifier.IdentifierType = MK8_NGX_IDENTIFIER_PROJECT_ID;
        discovery.Identifier.v.ProjectDesc.ProjectId = project_id;
        discovery.Identifier.v.ProjectDesc.EngineType = MK8_NGX_ENGINE_TYPE_CUSTOM;
        discovery.Identifier.v.ProjectDesc.EngineVersion = "mk8-online";
        discovery.ApplicationDataPath = data_path;
        discovery.FeatureInfo = &common;
        Mk8NgxFeatureRequirement requirement = {};
        int rc = ngx.get_requirements ? ngx.get_requirements(g_instance, g_phys, &discovery, &requirement) : MK8_NGX_RESULT_FAIL;
        log_line("%s GetFeatureRequirements: 0x%08x supported=0x%x minArch=0x%x minOS=%s", feature_names[i],
                 (unsigned)rc, (unsigned)requirement.FeatureSupported, requirement.MinHWArchitecture,
                 requirement.MinOSVersion);
    }

    int rc = 0;
    if (ngx.init9) {
        log_line("NVSDK_NGX_VULKAN_Init_ProjectID: project=%s", project_id);
        rc = ngx.init9(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1", data_path, g_instance,
                       g_phys, g_device, MK8_NGX_API_VERSION, &common);
        log_result("NVSDK_NGX_VULKAN_Init_ProjectID", rc);
    } else {
        log_line("NVSDK_NGX_VULKAN_Init_with_ProjectID: project=%s", project_id);
        rc = ngx.init(project_id, MK8_NGX_ENGINE_TYPE_CUSTOM, "mk8-online 4.0.1", data_path, g_instance,
                      g_phys, g_device, g_gipa, g_gdpa, &common, MK8_NGX_API_VERSION);
        log_result("NVSDK_NGX_VULKAN_Init_with_ProjectID", rc);
    }
    if (MK8_NGX_FAILED(rc)) {
        log_line("RESULT: FAIL (NGX Init)");
        return 1;
    }
    ngx.initialised = true;

    rc = ngx.get_capability(&ngx.params);
    log_result("NVSDK_NGX_VULKAN_GetCapabilityParameters", rc);
    if (MK8_NGX_FAILED(rc) || !ngx.params) {
        log_line("RESULT: FAIL (capability parameters)");
        return 1;
    }

    unsigned sr_available = 0;
    if (!ngx_available(&ngx, MK8_NGX_PARAM_SUPER_SAMPLING_AVAILABLE, &sr_available)) {
        log_line("RESULT: FAIL (SuperSampling.Available unreadable)");
        return 1;
    }
    log_line("SuperSampling.Available = %u", sr_available);

    HgSizes sizes;
    bool sr_ok = false;
    if (sr_available == 1) {
        sr_ok = run_super_resolution(&ngx, sizes, output_dir);
    } else {
        log_line("SuperSampling.Available = %u: Super Resolution cannot be created on this GPU/driver", sr_available);
    }

    bool fg_ok = run_frame_generation(&ngx, sizes, output_dir);

    if (ngx.shutdown1) {
        int shutdown_rc = ngx.shutdown1(g_device);
        log_result("NVSDK_NGX_VULKAN_Shutdown1", shutdown_rc);
    }
    vkfn_vkDeviceWaitIdle(g_device);
    if (g_fence) vkfn_vkDestroyFence(g_device, g_fence, NULL);
    if (g_pool) vkfn_vkDestroyCommandPool(g_device, g_pool, NULL);
    if (g_device) vkfn_vkDestroyDevice(g_device, NULL);
    if (g_instance) vkfn_vkDestroyInstance(g_instance, NULL);

    log_line("RESULT: SR=%s FG=%s", sr_ok ? "PASS" : "FAIL", fg_ok ? "PASS" : "NO-FRAME");
    return sr_ok ? 0 : 1;
}
