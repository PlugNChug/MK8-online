// Headless Vulkan harness for the native FidelityFX frame-generation swapchain.
//
// This is the standalone version of the runtime's MK8_FSR_FG path: it creates a real swapchain on
// VK_EXT_headless_surface (or VK_KHR_display), replaces it through ffxCreateContext with the FFX
// frame-generation swapchain, records a real prepare dispatch every frame, presents through
// pOutQueuePresentKHR, and verifies that interpolation output is actually produced. Every step is
// mandatory: any Vulkan/FFX failure, validation error or missing generated frame exits non-zero.
//
// The API sequence, the per-frame order and the provider quirks are documented in docs/FG-HARNESS.md.
//
// Build:
//   g++ -std=c++17 -O2 -I runtime/mk8/graphics/ffx tools/fg-harness/fg_harness.cpp
//       -o fg-harness -lvulkan -ldl -pthread
//
// Run:
//   ./fg-harness --frames 120 --width 1280 --height 720
//   MK8_FG_HARNESS_LIB=/path/libamd_fidelityfx_framegeneration_vk.so ./fg-harness
//
// Output ends with one machine-readable line:
//   FG-HARNESS ok rendered=120 presented=240 last_present_count=240 generated=120 real=120 ...

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_framegeneration_vk.h"
#include "vk/ffx_api_vk.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <vector>

#include "shaders.h"

namespace {

// ---------------------------------------------------------------------------------------------
// diagnostics
// ---------------------------------------------------------------------------------------------

[[noreturn]] void die(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fputs("FG-HARNESS fail: ", stderr);
    std::vfprintf(stderr, format, arguments);
    std::fputc('\n', stderr);
    va_end(arguments);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(1);
}

void note(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::fputs("[fg-harness] ", stdout);
    std::vfprintf(stdout, format, arguments);
    std::fputc('\n', stdout);
    va_end(arguments);
}

void check_vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) die("%s returned VkResult %d", what, static_cast<int>(result));
}

const char* ffx_name(ffxReturnCode_t code) {
    switch (code) {
    case FFX_API_RETURN_OK: return "FFX_API_RETURN_OK";
    case FFX_API_RETURN_ERROR: return "FFX_API_RETURN_ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "FFX_API_RETURN_ERROR_RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "FFX_API_RETURN_NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "FFX_API_RETURN_ERROR_MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER: return "FFX_API_RETURN_ERROR_PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    default: return "FFX_API_RETURN_?";
    }
}

void check_ffx(ffxReturnCode_t code, const char* what) {
    if (code != FFX_API_RETURN_OK) die("%s returned %s (%d)", what, ffx_name(code), static_cast<int>(code));
}

// ---------------------------------------------------------------------------------------------
// options and provider
// ---------------------------------------------------------------------------------------------

struct Options {
    uint32_t frames = 120;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t shift = 8;
    bool validation = true;
    bool validation_flag_given = false;
    std::string surface = "auto";  // auto | headless | display
    std::string provider;
};

void usage(const char* program) {
    std::printf(
        "usage: %s [--frames N] [--width W] [--height H] [--shift PX] [--provider PATH]\n"
        "          [--validation|--no-validation] [--surface auto|headless|display]\n"
        "\n"
        "Provider resolution: --provider, then MK8_FG_HARNESS_LIB, then MK8_FSR_FG_LIB,\n"
        "then $XDG_DATA_HOME/mk8online/fsr/, then $HOME/.local/share/mk8online/fsr/.\n",
        program);
}

uint32_t parse_uint(const char* value, const char* option, uint32_t minimum, uint32_t maximum) {
    char* end = nullptr;
    errno = 0;
    unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsed < minimum || parsed > maximum)
        die("%s expects an integer in [%u, %u], got '%s'", option, minimum, maximum, value);
    return static_cast<uint32_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const char* argument = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) die("%s needs a value", argument);
            return argv[++i];
        };
        if (!std::strcmp(argument, "--help") || !std::strcmp(argument, "-h")) {
            usage(argv[0]);
            std::exit(0);
        } else if (!std::strcmp(argument, "--frames")) {
            options.frames = parse_uint(next(), "--frames", 2, 100000);
        } else if (!std::strcmp(argument, "--width")) {
            options.width = parse_uint(next(), "--width", 64, 16384);
        } else if (!std::strcmp(argument, "--height")) {
            options.height = parse_uint(next(), "--height", 64, 16384);
        } else if (!std::strcmp(argument, "--shift")) {
            options.shift = parse_uint(next(), "--shift", 0, 256);
        } else if (!std::strcmp(argument, "--provider")) {
            options.provider = next();
        } else if (!std::strcmp(argument, "--surface")) {
            options.surface = next();
            if (options.surface != "auto" && options.surface != "headless" && options.surface != "display")
                die("--surface expects auto, headless or display");
        } else if (!std::strcmp(argument, "--validation")) {
            options.validation = true;
            options.validation_flag_given = true;
        } else if (!std::strcmp(argument, "--no-validation")) {
            options.validation = false;
            options.validation_flag_given = true;
        } else {
            die("unknown option '%s' (try --help)", argument);
        }
    }
    return options;
}

std::string default_provider_path() {
    if (const char* value = std::getenv("MK8_FG_HARNESS_LIB"); value && *value) return value;
    if (const char* value = std::getenv("MK8_FSR_FG_LIB"); value && *value) return value;
    if (const char* value = std::getenv("XDG_DATA_HOME"); value && *value)
        return std::string(value) + "/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so";
    if (const char* value = std::getenv("HOME"); value && *value)
        return std::string(value) + "/.local/share/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so";
    return "libamd_fidelityfx_framegeneration_vk.so";
}

typedef ffxReturnCode_t (*PfnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxQueryDescHeader*);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext*, const ffxConfigureDescHeader*);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext*, const ffxDispatchDescHeader*);

struct Provider {
    void* library = nullptr;
    PfnCreate create = nullptr;
    PfnDestroy destroy = nullptr;
    PfnQuery query = nullptr;
    PfnConfigure configure = nullptr;
    PfnDispatch dispatch = nullptr;
};

Provider load_provider(const std::string& path) {
    Provider provider;
    provider.library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!provider.library) die("dlopen(%s): %s", path.c_str(), dlerror());
    provider.create = reinterpret_cast<PfnCreate>(dlsym(provider.library, "ffxCreateContext"));
    provider.destroy = reinterpret_cast<PfnDestroy>(dlsym(provider.library, "ffxDestroyContext"));
    provider.query = reinterpret_cast<PfnQuery>(dlsym(provider.library, "ffxQuery"));
    provider.configure = reinterpret_cast<PfnConfigure>(dlsym(provider.library, "ffxConfigure"));
    provider.dispatch = reinterpret_cast<PfnDispatch>(dlsym(provider.library, "ffxDispatch"));
    if (!provider.create || !provider.destroy || !provider.query || !provider.configure || !provider.dispatch)
        die("%s lacks the FFX exports", path.c_str());
    return provider;
}

// ---------------------------------------------------------------------------------------------
// validation layer
// ---------------------------------------------------------------------------------------------

std::atomic<uint64_t> g_validation_errors{0};
std::atomic<uint64_t> g_validation_warnings{0};
std::mutex g_validation_message_mutex;
char g_first_validation_error[1024] = {};

VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const char* text = (data && data->pMessage) ? data->pMessage : "(no message)";
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        {
            std::lock_guard<std::mutex> lock(g_validation_message_mutex);
            if (!g_first_validation_error[0])
                std::snprintf(g_first_validation_error, sizeof g_first_validation_error, "%s", text);
        }
        g_validation_errors.fetch_add(1, std::memory_order_relaxed);
        std::fprintf(stderr, "[validation error] %s\n", text);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        uint64_t count = g_validation_warnings.fetch_add(1, std::memory_order_relaxed);
        if (count < 8) std::fprintf(stderr, "[validation warning] %s\n", text);
    }
    return VK_FALSE;
}

// ---------------------------------------------------------------------------------------------
// small Vulkan helpers
// ---------------------------------------------------------------------------------------------

bool has_extension(const std::vector<VkExtensionProperties>& values, const char* name) {
    for (const VkExtensionProperties& value : values)
        if (!std::strcmp(value.extensionName, name)) return true;
    return false;
}

bool has_layer(const std::vector<VkLayerProperties>& values, const char* name) {
    for (const VkLayerProperties& value : values)
        if (!std::strcmp(value.layerName, name)) return true;
    return false;
}

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    die("no memory type for bits=0x%x properties=0x%x", bits, static_cast<unsigned>(properties));
}

void check_ffx_resource(const FfxApiResource& resource, const char* what) {
    if (!resource.resource) die("%s is empty", what);
}

void image_barrier(VkCommandBuffer command, VkImage image, VkImageLayout old_layout, VkImageLayout new_layout,
                   VkAccessFlags source_access, VkAccessFlags destination_access, VkPipelineStageFlags source_stage,
                   VkPipelineStageFlags destination_stage) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = source_access;
    barrier.dstAccessMask = destination_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, source_stage, destination_stage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// Mapping used by the provider for FfxApiResourceState. The present callback receives resources
// in these states and must transition from exactly these layouts.
VkImageLayout layout_from_state(uint32_t state) {
    switch (state) {
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return VK_IMAGE_LAYOUT_GENERAL;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_READ: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case FFX_API_RESOURCE_STATE_PRESENT: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    default: return VK_IMAGE_LAYOUT_GENERAL;
    }
}

VkAccessFlags access_from_state(uint32_t state) {
    switch (state) {
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_PRESENT: return VK_ACCESS_NONE;
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return VK_ACCESS_SHADER_READ_BIT;
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case FFX_API_RESOURCE_STATE_COPY_SRC: return VK_ACCESS_TRANSFER_READ_BIT;
    case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_ACCESS_TRANSFER_WRITE_BIT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    default: return VK_ACCESS_SHADER_READ_BIT;
    }
}

VkPipelineStageFlags stage_from_state(uint32_t state) {
    switch (state) {
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ: return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
    case FFX_API_RESOURCE_STATE_COPY_DEST: return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case FFX_API_RESOURCE_STATE_PRESENT: return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET: return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    default: return VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    }
}

// ---------------------------------------------------------------------------------------------
// harness state shared with provider callbacks
// ---------------------------------------------------------------------------------------------

struct Capture {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
    VkImage source = VK_NULL_HANDLE;
    uint64_t frame = 0;
    bool generated = false;
    uint64_t checksum = 0;
};

struct Harness {
    Provider provider;
    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const char* surface_name = "unknown";
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue game_queue = VK_NULL_HANDLE;
    VkQueue present_queue = VK_NULL_HANDLE;
    VkQueue acquire_queue = VK_NULL_HANDLE;
    uint32_t game_family = UINT32_MAX;
    uint32_t present_family = UINT32_MAX;
    bool present_family_has_graphics = false;

    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence frame_fence = VK_NULL_HANDLE;
    bool fence_submitted = false;
    VkSemaphore acquired_semaphore = VK_NULL_HANDLE;
    VkSemaphore rendered_semaphore = VK_NULL_HANDLE;

    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;  // becomes the FFX proxy
    uint32_t swapchain_image_count = 0;
    std::vector<VkImage> swapchain_images;
    std::vector<VkImageView> swapchain_views;

    ffxContext swapchain_context = nullptr;
    ffxContext effect = nullptr;
    PFN_vkCreateSwapchainFFXAPI create_swapchain = nullptr;
    PFN_vkDestroySwapchainFFXAPI destroy_swapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR get_swapchain_images = nullptr;
    PFN_vkAcquireNextImageKHR acquire_next_image = nullptr;
    PFN_vkQueuePresentKHR queue_present = nullptr;
    PFN_getLastPresentCountFFXAPI get_last_present_count = nullptr;

    VkImage depth_image = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    VkImage motion_image = VK_NULL_HANDLE;
    VkDeviceMemory motion_memory = VK_NULL_HANDLE;
    VkImageCreateInfo depth_info{};
    VkImageCreateInfo motion_info{};

    VkShaderModule vertex_shader = VK_NULL_HANDLE;
    VkShaderModule fragment_shader = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;

    Capture capture_previous_real;
    Capture capture_generated;
    Capture capture_current_real;
    uint32_t capture_frame = 0;
    std::atomic<uint32_t> capture_mask{0};  // bit 0 previous real, bit 1 generated, bit 2 current real

    std::atomic<uint32_t> present_callback_generated{0};
    std::atomic<uint32_t> present_callback_real{0};
    std::atomic<uint32_t> present_callback_errors{0};
    std::atomic<uint32_t> frame_generation_calls{0};
    std::atomic<uint32_t> frame_generation_failures{0};

    std::mutex submit_mutex;
    std::mutex capture_mutex;
    std::string last_callback_error;
    std::mutex callback_error_mutex;

    uint32_t shift_pixels = 8;
};

Harness* g_harness = nullptr;

void callback_error(const char* what) {
    if (!g_harness) return;
    std::lock_guard<std::mutex> lock(g_harness->callback_error_mutex);
    if (g_harness->last_callback_error.empty()) g_harness->last_callback_error = what;
}

// The provider submits on the game/present/acquire queues from its own threads. All submissions are
// serialized here so the driver is never entered concurrently on one VkQueue.
VkResult submit_locked(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    if (!g_harness) return VK_ERROR_DEVICE_LOST;
    std::lock_guard<std::mutex> lock(g_harness->submit_mutex);
    return vkQueueSubmit(queue, count, submits, fence);
}

VkResult submit_game(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    return submit_locked(g_harness ? g_harness->game_queue : VK_NULL_HANDLE, count, submits, fence);
}
VkResult submit_present(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    return submit_locked(g_harness ? g_harness->present_queue : VK_NULL_HANDLE, count, submits, fence);
}
VkResult submit_acquire(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    return submit_locked(g_harness ? g_harness->acquire_queue : VK_NULL_HANDLE, count, submits, fence);
}

// ---------------------------------------------------------------------------------------------
// present callback: compose the frame into the FFX-provided output image
// ---------------------------------------------------------------------------------------------

ffxReturnCode_t present_callback(ffxCallbackDescFrameGenerationPresent* parameters, void*) {
    Harness& harness = *g_harness;
    if (!parameters || !parameters->commandList || !parameters->currentBackBuffer.resource ||
        !parameters->outputSwapChainBuffer.resource) {
        harness.present_callback_errors.fetch_add(1);
        callback_error("present callback received an empty command list or resource");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(parameters->commandList);
    VkImage source = reinterpret_cast<VkImage>(parameters->currentBackBuffer.resource);
    VkImage destination = reinterpret_cast<VkImage>(parameters->outputSwapChainBuffer.resource);
    const FfxApiResourceDescription& source_description = parameters->currentBackBuffer.description;
    const FfxApiResourceDescription& destination_description = parameters->outputSwapChainBuffer.description;

    if (source_description.width != destination_description.width ||
        source_description.height != destination_description.height) {
        harness.present_callback_errors.fetch_add(1);
        callback_error("present callback source and destination sizes differ");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }

    uint32_t source_state = parameters->currentBackBuffer.state;
    uint32_t destination_state = parameters->outputSwapChainBuffer.state;
    VkAccessFlags source_access = access_from_state(source_state);
    VkAccessFlags destination_access = access_from_state(destination_state);
    VkImageLayout source_layout = layout_from_state(source_state);
    VkImageLayout destination_layout = layout_from_state(destination_state);
    VkPipelineStageFlags source_stage = stage_from_state(source_state);
    VkPipelineStageFlags destination_stage = stage_from_state(destination_state);

    VkImageMemoryBarrier before[2]{};
    before[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    before[0].srcAccessMask = source_access;
    before[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    before[0].oldLayout = source_layout;
    before[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    before[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    before[0].image = source;
    before[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    before[1] = before[0];
    before[1].srcAccessMask = destination_access;
    before[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    before[1].oldLayout = destination_layout;
    before[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    before[1].image = destination;
    vkCmdPipelineBarrier(command, source_stage | destination_stage,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 2, before);

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {std::min(source_description.width, destination_description.width),
                   std::min(source_description.height, destination_description.height), 1};
    vkCmdCopyImage(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    // Readback: capture the previous real frame, the first generated frame at capture_frame, and the
    // current real frame, so the comparison can prove the generated image is neither neighbour.
    Capture* capture = nullptr;
    {
        std::lock_guard<std::mutex> lock(harness.capture_mutex);
        if (parameters->frameID + 1 == harness.capture_frame && !parameters->isGeneratedFrame &&
            !(harness.capture_mask.load() & 1)) {
            harness.capture_mask.fetch_or(1);
            capture = &harness.capture_previous_real;
        } else if (parameters->frameID == harness.capture_frame && parameters->isGeneratedFrame &&
                   !(harness.capture_mask.load() & 2)) {
            harness.capture_mask.fetch_or(2);
            capture = &harness.capture_generated;
        } else if (parameters->frameID == harness.capture_frame && !parameters->isGeneratedFrame &&
                   !(harness.capture_mask.load() & 4)) {
            harness.capture_mask.fetch_or(4);
            capture = &harness.capture_current_real;
        }
        if (capture) {
            capture->source = source;
            capture->frame = parameters->frameID;
            capture->generated = parameters->isGeneratedFrame;
        }
    }
    if (capture) {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = copy.extent;
        vkCmdCopyImageToBuffer(command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, capture->buffer, 1, &region);

        VkBufferMemoryBarrier host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        host_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        host_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        host_barrier.buffer = capture->buffer;
        host_barrier.offset = 0;
        host_barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                             1, &host_barrier, 0, nullptr);
    }

    VkImageMemoryBarrier after[2]{};
    after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    after[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    after[0].dstAccessMask = source_access;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[0].newLayout = source_layout;
    after[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    after[0].image = source;
    after[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    after[1] = after[0];
    after[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    after[1].dstAccessMask = destination_access;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[1].newLayout = destination_layout;
    after[1].image = destination;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         source_stage | destination_stage, 0, 0, nullptr, 0, nullptr, 2, after);

    if (parameters->isGeneratedFrame)
        harness.present_callback_generated.fetch_add(1, std::memory_order_relaxed);
    else
        harness.present_callback_real.fetch_add(1, std::memory_order_relaxed);
    return FFX_API_RETURN_OK;
}

ffxReturnCode_t frame_generation_callback(ffxDispatchDescFrameGeneration* parameters, void*) {
    Harness& harness = *g_harness;
    harness.frame_generation_calls.fetch_add(1, std::memory_order_relaxed);
    ffxReturnCode_t result = harness.provider.dispatch(&harness.effect, &parameters->header);
    if (result != FFX_API_RETURN_OK) {
        harness.frame_generation_failures.fetch_add(1, std::memory_order_relaxed);
        callback_error("ffxDispatch(FRAMEGENERATION) failed");
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// instance, surface, device
// ---------------------------------------------------------------------------------------------

struct SurfaceSelection {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    const char* name = "unknown";
    uint32_t width = 0;
    uint32_t height = 0;
    bool headless = false;
};

SurfaceSelection create_headless_surface(VkInstance instance, uint32_t width, uint32_t height) {
    SurfaceSelection selection;
    auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(
        vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT"));
    if (!create) return selection;
    VkHeadlessSurfaceCreateInfoEXT info{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult result = create(instance, &info, nullptr, &surface);
    if (result != VK_SUCCESS) {
        std::fprintf(stderr, "[fg-harness] vkCreateHeadlessSurfaceEXT returned %d\n", static_cast<int>(result));
        return selection;
    }
    selection.surface = surface;
    selection.name = "VK_EXT_headless_surface";
    selection.width = width;
    selection.height = height;
    selection.headless = true;
    return selection;
}

SurfaceSelection create_display_surface(VkInstance instance, uint32_t width, uint32_t height) {
    SurfaceSelection selection;
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) return selection;
    std::vector<VkPhysicalDevice> devices(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    auto get_displays = reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPropertiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPropertiesKHR"));
    auto get_planes = reinterpret_cast<PFN_vkGetPhysicalDeviceDisplayPlanePropertiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDisplayPlanePropertiesKHR"));
    auto get_plane_displays = reinterpret_cast<PFN_vkGetDisplayPlaneSupportedDisplaysKHR>(
        vkGetInstanceProcAddr(instance, "vkGetDisplayPlaneSupportedDisplaysKHR"));
    auto get_capabilities = reinterpret_cast<PFN_vkGetDisplayPlaneCapabilitiesKHR>(
        vkGetInstanceProcAddr(instance, "vkGetDisplayPlaneCapabilitiesKHR"));
    auto create = reinterpret_cast<PFN_vkCreateDisplayPlaneSurfaceKHR>(
        vkGetInstanceProcAddr(instance, "vkCreateDisplayPlaneSurfaceKHR"));
    if (!get_displays || !get_planes || !get_plane_displays || !get_capabilities || !create) return selection;

    for (VkPhysicalDevice device : devices) {
        uint32_t display_count = 0;
        if (get_displays(device, &display_count, nullptr) != VK_SUCCESS || display_count == 0) continue;
        std::vector<VkDisplayPropertiesKHR> displays(display_count);
        if (get_displays(device, &display_count, displays.data()) != VK_SUCCESS) continue;
        uint32_t plane_count = 0;
        if (get_planes(device, &plane_count, nullptr) != VK_SUCCESS || plane_count == 0) continue;
        std::vector<VkDisplayPlanePropertiesKHR> planes(plane_count);
        if (get_planes(device, &plane_count, planes.data()) != VK_SUCCESS) continue;
        for (uint32_t plane = 0; plane < plane_count; ++plane) {
            uint32_t supported = 0;
            if (get_plane_displays(device, plane, &supported, nullptr) != VK_SUCCESS || supported == 0) continue;
            std::vector<VkDisplayKHR> supported_displays(supported);
            if (get_plane_displays(device, plane, &supported, supported_displays.data()) != VK_SUCCESS) continue;
            for (VkDisplayKHR display : supported_displays) {
                uint32_t mode_count = 0;
                auto get_modes = reinterpret_cast<PFN_vkGetDisplayModePropertiesKHR>(
                    vkGetInstanceProcAddr(instance, "vkGetDisplayModePropertiesKHR"));
                if (!get_modes || get_modes(device, display, &mode_count, nullptr) != VK_SUCCESS || mode_count == 0)
                    continue;
                std::vector<VkDisplayModePropertiesKHR> modes(mode_count);
                if (get_modes(device, display, &mode_count, modes.data()) != VK_SUCCESS) continue;
                for (const VkDisplayModePropertiesKHR& mode : modes) {
                    VkDisplayPlaneCapabilitiesKHR capabilities{};
                    if (get_capabilities(device, mode.displayMode, plane, &capabilities) != VK_SUCCESS) continue;
                    if (!(capabilities.supportedAlpha & VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR)) continue;
                    VkDisplaySurfaceCreateInfoKHR create_info{VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR};
                    create_info.displayMode = mode.displayMode;
                    create_info.planeIndex = plane;
                    create_info.planeStackIndex = planes[plane].currentStackIndex;
                    create_info.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
                    create_info.globalAlpha = 1.0f;
                    create_info.alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR;
                    create_info.imageExtent = {width, height};
                    VkSurfaceKHR surface = VK_NULL_HANDLE;
                    if (create(instance, &create_info, nullptr, &surface) == VK_SUCCESS) {
                        selection.surface = surface;
                        selection.name = "VK_KHR_display";
                        selection.width = width;
                        selection.height = height;
                        return selection;
                    }
                }
            }
        }
    }
    return selection;
}

void destroy_instance_messenger(VkInstance instance, VkDebugUtilsMessengerEXT messenger) {
    if (messenger) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance, messenger, nullptr);
    }
}

struct QueueChoice {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    uint32_t game = UINT32_MAX;
    uint32_t present = UINT32_MAX;
    bool present_has_graphics = false;
    VkPhysicalDeviceProperties properties{};
};

QueueChoice choose_device(VkInstance instance, VkSurfaceKHR surface) {
    uint32_t count = 0;
    check_vk(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices(count)");
    if (!count) die("no Vulkan physical device");
    std::vector<VkPhysicalDevice> devices(count);
    check_vk(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");

    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) {
            note("skipping %s: Vulkan %u.%u < 1.3", properties.deviceName,
                 VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion));
            continue;
        }
        uint32_t extension_count = 0;
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr) != VK_SUCCESS) continue;
        std::vector<VkExtensionProperties> extensions(extension_count);
        if (vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, extensions.data()) != VK_SUCCESS)
            continue;
        if (!has_extension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        uint32_t format_count = 0;
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, nullptr) != VK_SUCCESS ||
            format_count == 0)
            continue;
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        if (vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &format_count, formats.data()) != VK_SUCCESS)
            continue;
        bool format_ok = false;
        VkSurfaceFormatKHR chosen{};
        for (const VkSurfaceFormatKHR& candidate : formats) {
            if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM || candidate.format == VK_FORMAT_R8G8B8A8_UNORM) {
                chosen = candidate;
                format_ok = true;
                break;
            }
        }
        if (!format_ok && formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
            chosen.format = VK_FORMAT_B8G8R8A8_UNORM;
            chosen.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
            format_ok = true;
        }
        if (!format_ok) continue;

        uint32_t present_mode_count = 0;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, nullptr) != VK_SUCCESS ||
            present_mode_count == 0)
            continue;
        std::vector<VkPresentModeKHR> modes(present_mode_count);
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &present_mode_count, modes.data()) !=
            VK_SUCCESS)
            continue;
        if (std::find(modes.begin(), modes.end(), VK_PRESENT_MODE_FIFO_KHR) == modes.end()) continue;

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());

        uint32_t game = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present);
            if (present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                game = i;
                break;
            }
        }
        if (game == UINT32_MAX) continue;

        uint32_t present = UINT32_MAX;
        for (uint32_t i = 0; i < family_count; ++i) {
            if (i == game) continue;
            VkBool32 supports = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &supports);
            if (!supports || families[i].queueCount < 2) continue;
            if (!(families[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) continue;
            present = i;
            break;
        }
        if (present == UINT32_MAX) {
            // Fall back to the game family with three queues. The provider supports a single family;
            // log it so a run that uses this path is not mistaken for the validated two-family layout.
            if (families[game].queueCount < 3) {
                note("skipping %s: no second present family with two queues", properties.deviceName);
                continue;
            }
            present = game;
        }

        QueueChoice choice;
        choice.physical = device;
        choice.game = game;
        choice.present = present;
        choice.present_has_graphics = (families[present].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        choice.properties = properties;
        return choice;
    }
    die("no Vulkan 1.3 device supports graphics + compute + present with the requested surface");
}

// ---------------------------------------------------------------------------------------------
// resource creation
// ---------------------------------------------------------------------------------------------

void allocate_image(Harness& harness, const VkImageCreateInfo& info, VkImage* image, VkDeviceMemory* memory) {
    check_vk(vkCreateImage(harness.device, &info, nullptr, image), "vkCreateImage");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(harness.device, *image, &requirements);
    uint32_t type = find_memory_type(harness.physical, requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    check_vk(vkAllocateMemory(harness.device, &allocation, nullptr, memory), "vkAllocateMemory(image)");
    check_vk(vkBindImageMemory(harness.device, *image, *memory, 0), "vkBindImageMemory");
}

void upload_image(Harness& harness, VkImage image, VkFormat format, uint32_t width, uint32_t height,
                  const void* pixels, VkDeviceSize bytes) {
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    check_vk(vkCreateBuffer(harness.device, &buffer_info, nullptr, &staging), "vkCreateBuffer(staging)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(harness.device, staging, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(harness.physical, requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk(vkAllocateMemory(harness.device, &allocation, nullptr, &staging_memory), "vkAllocateMemory(staging)");
    check_vk(vkBindBufferMemory(harness.device, staging, staging_memory, 0), "vkBindBufferMemory(staging)");
    void* mapped = nullptr;
    check_vk(vkMapMemory(harness.device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory(staging)");
    std::memcpy(mapped, pixels, static_cast<size_t>(bytes));
    vkUnmapMemory(harness.device, staging_memory);

    VkCommandBuffer command = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = harness.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(harness.device, &command_info, &command), "vkAllocateCommandBuffers(upload)");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check_vk(vkBeginCommandBuffer(command, &begin), "vkBeginCommandBuffer(upload)");
    VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_transfer.srcAccessMask = 0;
    to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_transfer.image = image;
    to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_transfer);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(command, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkImageMemoryBarrier to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_read);
    check_vk(vkEndCommandBuffer(command), "vkEndCommandBuffer(upload)");

    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    check_vk(vkCreateFence(harness.device, &fence_info, nullptr, &fence), "vkCreateFence(upload)");
    check_vk(submit_locked(harness.game_queue, 1, &submit, fence), "vkQueueSubmit(upload)");
    check_vk(vkWaitForFences(harness.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(upload)");
    vkDestroyFence(harness.device, fence, nullptr);
    vkFreeCommandBuffers(harness.device, harness.command_pool, 1, &command);
    vkDestroyBuffer(harness.device, staging, nullptr);
    vkFreeMemory(harness.device, staging_memory, nullptr);
    (void)format;
}

uint16_t float_to_half(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return static_cast<uint16_t>(sign);
    if (exponent >= 31) return static_cast<uint16_t>(sign | 0x7c00u);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13));
}

void create_depth_and_motion(Harness& harness) {
    harness.depth_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    harness.depth_info.imageType = VK_IMAGE_TYPE_2D;
    harness.depth_info.format = VK_FORMAT_R32_SFLOAT;
    harness.depth_info.extent = {harness.extent.width, harness.extent.height, 1};
    harness.depth_info.mipLevels = 1;
    harness.depth_info.arrayLayers = 1;
    harness.depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    harness.depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    harness.depth_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    harness.depth_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    allocate_image(harness, harness.depth_info, &harness.depth_image, &harness.depth_memory);

    const uint32_t width = harness.extent.width;
    const uint32_t height = harness.extent.height;
    std::vector<float> depth(static_cast<size_t>(width) * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            float value = 0.2f + 0.75f * (static_cast<float>(y) / static_cast<float>(height - 1));
            if (x > width / 2 - 100 && x < width / 2 + 100 && y > height / 2 - 100 && y < height / 2 + 100)
                value = 0.05f;
            depth[static_cast<size_t>(y) * width + x] = value;
        }
    }
    upload_image(harness, harness.depth_image, harness.depth_info.format, width, height, depth.data(),
                 depth.size() * sizeof(float));

    harness.motion_info = harness.depth_info;
    harness.motion_info.format = VK_FORMAT_R16G16_SFLOAT;
    allocate_image(harness, harness.motion_info, &harness.motion_image, &harness.motion_memory);
    std::vector<uint16_t> motion(static_cast<size_t>(width) * height * 2);
    const uint16_t shift = float_to_half(static_cast<float>(harness.shift_pixels) / static_cast<float>(width));
    const uint16_t zero = float_to_half(0.0f);
    for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
        motion[i * 2 + 0] = shift;
        motion[i * 2 + 1] = zero;
    }
    upload_image(harness, harness.motion_image, harness.motion_info.format, width, height, motion.data(),
                 motion.size() * sizeof(uint16_t));
}

VkShaderModule create_shader_module(Harness& harness, const uint32_t* words, uint32_t bytes) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = bytes;
    info.pCode = words;
    VkShaderModule module = VK_NULL_HANDLE;
    check_vk(vkCreateShaderModule(harness.device, &info, nullptr, &module), "vkCreateShaderModule");
    return module;
}

void create_pipeline(Harness& harness) {
    harness.vertex_shader = create_shader_module(harness, pattern_vert_spirv, pattern_vert_spirv_size);
    harness.fragment_shader = create_shader_module(harness, pattern_frag_spirv, pattern_frag_spirv_size);

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    check_vk(vkCreatePipelineLayout(harness.device, &layout_info, nullptr, &harness.pipeline_layout),
             "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = harness.vertex_shader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = harness.fragment_shader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_FALSE;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &harness.format;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = harness.pipeline_layout;
    check_vk(vkCreateGraphicsPipelines(harness.device, VK_NULL_HANDLE, 1, &info, nullptr, &harness.pipeline),
             "vkCreateGraphicsPipelines");
}

Capture create_capture(Harness& harness) {
    Capture capture;
    capture.size = static_cast<VkDeviceSize>(harness.extent.width) * harness.extent.height * 4;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = capture.size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check_vk(vkCreateBuffer(harness.device, &info, nullptr, &capture.buffer), "vkCreateBuffer(capture)");
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(harness.device, capture.buffer, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(harness.physical, requirements.memoryTypeBits,
                                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check_vk(vkAllocateMemory(harness.device, &allocation, nullptr, &capture.memory),
             "vkAllocateMemory(capture)");
    check_vk(vkBindBufferMemory(harness.device, capture.buffer, capture.memory, 0), "vkBindBufferMemory(capture)");
    check_vk(vkMapMemory(harness.device, capture.memory, 0, VK_WHOLE_SIZE, 0, &capture.mapped), "vkMapMemory(capture)");
    std::memset(capture.mapped, 0, static_cast<size_t>(capture.size));
    return capture;
}

float halton(uint32_t index, uint32_t base) {
    // The first sample is at 0.5, not 0, so a zero-initialised frame still gets a jitter.
    float fraction = 1.0f;
    float result = 0.0f;
    uint32_t i = index + 1;
    while (i > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(i % base);
        i /= base;
    }
    return result;
}

// Fraction of pixels that differ by more than 8/255 in any channel.
double differing_fraction(const Capture& left, const Capture& right, uint32_t pixels) {
    const unsigned char* a = static_cast<const unsigned char*>(left.mapped);
    const unsigned char* b = static_cast<const unsigned char*>(right.mapped);
    size_t changed = 0;
    for (uint32_t i = 0; i < pixels; ++i) {
        const unsigned char* pa = a + static_cast<size_t>(i) * 4;
        const unsigned char* pb = b + static_cast<size_t>(i) * 4;
        int delta = 0;
        for (int channel = 0; channel < 3; ++channel)
            delta = std::max(delta, std::abs(static_cast<int>(pa[channel]) - static_cast<int>(pb[channel])));
        if (delta > 8) ++changed;
    }
    return static_cast<double>(changed) / static_cast<double>(pixels);
}

void verify_capture(Harness& harness) {
    const uint32_t pixels = harness.extent.width * harness.extent.height;
    for (Capture* capture : {&harness.capture_previous_real, &harness.capture_generated,
                             &harness.capture_current_real}) {
        const unsigned char* bytes = static_cast<const unsigned char*>(capture->mapped);
        uint64_t checksum = 0;
        for (size_t i = 0; i < static_cast<size_t>(capture->size); ++i)
            checksum = checksum * 1099511628211ull + bytes[i];
        capture->checksum = checksum;
        note("readback source: frame=%llu generated=%d image=%p checksum=0x%llx",
             static_cast<unsigned long long>(capture->frame), capture->generated ? 1 : 0,
             reinterpret_cast<void*>(capture->source), static_cast<unsigned long long>(checksum));
    }

    const unsigned char* generated = static_cast<const unsigned char*>(harness.capture_generated.mapped);
    unsigned int minimum = 255, maximum = 0;
    for (uint32_t i = 0; i < pixels; ++i) {
        unsigned int value = generated[static_cast<size_t>(i) * 4 + 1];
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (maximum < 8 || maximum - minimum < 8)
        die("generated frame is uniform or black (channel range %u..%u)", minimum, maximum);

    double from_previous = differing_fraction(harness.capture_generated, harness.capture_previous_real, pixels);
    double from_current = differing_fraction(harness.capture_generated, harness.capture_current_real, pixels);
    note("readback: generated-vs-previous=%.4f generated-vs-current=%.4f (threshold 0.005)", from_previous,
         from_current);
    if (from_previous < 0.005)
        die("generated frame is identical to the previous rendered frame (diff %.4f); interpolation did not run",
            from_previous);
    if (from_current < 0.005)
        die("generated frame is identical to the current rendered frame (diff %.4f); interpolation did not run",
            from_current);
}

}  // namespace

int main(int argc, char** argv) {
    Options options = parse_options(argc, argv);
    const std::string provider_path = options.provider.empty() ? default_provider_path() : options.provider;
    Harness harness;
    harness.shift_pixels = options.shift;
    g_harness = &harness;

    harness.provider = load_provider(provider_path);
    note("provider=%s", provider_path.c_str());

    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> layers(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
    const bool validation_available = has_layer(layers, "VK_LAYER_KHRONOS_validation");

    uint32_t extension_count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> instance_extensions(extension_count);
    vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, instance_extensions.data());
    const bool debug_utils_available = has_extension(instance_extensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const bool headless_available = has_extension(instance_extensions, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    const bool display_available = has_extension(instance_extensions, VK_KHR_DISPLAY_EXTENSION_NAME);

    const bool use_validation = options.validation && validation_available;
    if (options.validation && !validation_available)
        note("validation=unavailable (VK_LAYER_KHRONOS_validation not installed)");
    if (use_validation && !debug_utils_available)
        die("validation layer was enabled but VK_EXT_debug_utils is unavailable; errors cannot be verified");

    bool want_headless = options.surface == "auto" || options.surface == "headless";
    bool want_display = options.surface == "display" || (options.surface == "auto" && !want_headless);
    if (options.surface == "auto") want_display = true;
    if (want_headless && !headless_available) {
        if (options.surface == "headless") die("VK_EXT_headless_surface is not available");
        want_headless = false;
    }
    if (want_display && !display_available) {
        if (options.surface == "display") die("VK_KHR_display is not available");
        want_display = false;
    }

    std::vector<const char*> surface_extensions;
    if (want_headless) surface_extensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
    else if (want_display) surface_extensions.push_back(VK_KHR_DISPLAY_EXTENSION_NAME);
    else die("no usable presentation extension (need VK_EXT_headless_surface or VK_KHR_display)");

    // create_instance() needs the surface extension in its list; add it before the call.
    {
        std::vector<const char*> extensions;
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        if (debug_utils_available) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        for (const char* extension : surface_extensions) extensions.push_back(extension);

        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.pApplicationName = "mk8-fg-harness";
        application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        application.pEngineName = "none";
        application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        application.apiVersion = VK_API_VERSION_1_3;

        std::vector<const char*> enabled_layers;
        if (use_validation) enabled_layers.push_back("VK_LAYER_KHRONOS_validation");

        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        info.enabledLayerCount = static_cast<uint32_t>(enabled_layers.size());
        info.ppEnabledLayerNames = enabled_layers.data();
        check_vk(vkCreateInstance(&info, nullptr, &harness.instance), "vkCreateInstance");
    }

    if (use_validation && debug_utils_available) {
        auto create_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(harness.instance, "vkCreateDebugUtilsMessengerEXT"));
        if (create_messenger) {
            VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            info.pfnUserCallback = debug_message;
            check_vk(create_messenger(harness.instance, &info, nullptr, &harness.messenger),
                     "vkCreateDebugUtilsMessengerEXT");
        }
    }

    SurfaceSelection surface;
    if (want_headless) surface = create_headless_surface(harness.instance, options.width, options.height);
    if (!surface.surface && want_display)
        surface = create_display_surface(harness.instance, options.width, options.height);
    if (!surface.surface) {
        if (want_headless)
            die("VK_EXT_headless_surface is advertised but vkCreateHeadlessSurfaceEXT failed; "
                "VK_KHR_display fallback also failed (run under Xvfb and add an xcb surface path)");
        die("could not create a Vulkan presentation surface");
    }
    harness.surface = surface.surface;
    harness.surface_name = surface.name;
    note("surface=%s advertised=%s extent-request=%ux%u", surface.name,
         surface.headless ? "VK_EXT_headless_surface" : "VK_KHR_display", options.width, options.height);

    QueueChoice queue = choose_device(harness.instance, harness.surface);
    harness.physical = queue.physical;
    harness.game_family = queue.game;
    harness.present_family = queue.present;
    harness.present_family_has_graphics = queue.present_has_graphics;
    note("device=%s family-game=%u family-present=%u present-has-graphics=%s", queue.properties.deviceName,
         queue.game, queue.present, queue.present_has_graphics ? "yes" : "no");
    if (queue.game == queue.present)
        note("warning: using one queue family for game and present (validated layout uses two)");

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(harness.physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(harness.physical, &family_count, families.data());
    note("queues: game flags=0x%x count=%u, present flags=0x%x count=%u",
         static_cast<unsigned>(families[queue.game].queueFlags), families[queue.game].queueCount,
         static_cast<unsigned>(families[queue.present].queueFlags), families[queue.present].queueCount);

    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 available_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features13.pNext = &derivatives;
    features12.pNext = &features13;
    available_features.pNext = &features12;
    vkGetPhysicalDeviceFeatures2(harness.physical, &available_features);
    if (!features12.timelineSemaphore)
        die("device does not support timeline semaphores (required by the FFX swapchain)");
    if (!features13.dynamicRendering) die("device does not support dynamic rendering");
    if (!derivatives.computeDerivativeGroupLinear)
        die("device does not support VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR"
            "::computeDerivativeGroupLinear (required by the FFX FG shaders)");
    // The generated FG shader blobs declare their storage images with format Unknown
    // (tools/gen_fg_blobs.py), so the format comes from each image view; the device must allow it.
    if (!available_features.features.shaderStorageImageWriteWithoutFormat)
        die("device does not support shaderStorageImageWriteWithoutFormat (required by the FFX FG blobs)");

    VkPhysicalDeviceFeatures2 enabled_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled_features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.features.shaderStorageImageReadWithoutFormat =
        available_features.features.shaderStorageImageReadWithoutFormat;
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.timelineSemaphore = VK_TRUE;
    enabled12.shaderInt8 = features12.shaderInt8;
    enabled12.samplerMirrorClampToEdge = features12.samplerMirrorClampToEdge;
    VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR enabled_derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    enabled_derivatives.computeDerivativeGroupLinear = VK_TRUE;
    enabled_derivatives.computeDerivativeGroupQuads = derivatives.computeDerivativeGroupQuads;
    enabled13.pNext = &enabled_derivatives;
    enabled12.pNext = &enabled13;

    float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    uint32_t queue_counts[2] = {1, 2};
    if (queue.game == queue.present) queue_counts[0] = 3;
    for (int i = 0; i < 2; ++i) {
        if (i == 1 && queue.game == queue.present) break;
        VkDeviceQueueCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        info.queueFamilyIndex = i == 0 ? queue.game : queue.present;
        info.queueCount = queue_counts[i];
        info.pQueuePriorities = &priority;
        queue_infos.push_back(info);
    }
    uint32_t device_extension_count = 0;
    vkEnumerateDeviceExtensionProperties(harness.physical, nullptr, &device_extension_count, nullptr);
    std::vector<VkExtensionProperties> device_extensions_available(device_extension_count);
    vkEnumerateDeviceExtensionProperties(harness.physical, nullptr, &device_extension_count,
                                         device_extensions_available.data());
    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (has_extension(device_extensions_available, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME))
        device_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    else if (!derivatives.computeDerivativeGroupLinear)
        die("VK_KHR_compute_shader_derivatives is missing and the compute derivative feature is unavailable");

    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    enabled_features.pNext = &enabled12;
    device_info.pNext = &enabled_features;
    device_info.queueCreateInfoCount = static_cast<uint32_t>(queue_infos.size());
    device_info.pQueueCreateInfos = queue_infos.data();
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    check_vk(vkCreateDevice(harness.physical, &device_info, nullptr, &harness.device), "vkCreateDevice");

    vkGetDeviceQueue(harness.device, queue.game, 0, &harness.game_queue);
    if (queue.game == queue.present) {
        vkGetDeviceQueue(harness.device, queue.present, 1, &harness.present_queue);
        vkGetDeviceQueue(harness.device, queue.present, 2, &harness.acquire_queue);
    } else {
        vkGetDeviceQueue(harness.device, queue.present, 0, &harness.present_queue);
        vkGetDeviceQueue(harness.device, queue.present, 1, &harness.acquire_queue);
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue.game;
    check_vk(vkCreateCommandPool(harness.device, &pool_info, nullptr, &harness.command_pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = harness.command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    check_vk(vkAllocateCommandBuffers(harness.device, &command_info, &harness.command), "vkAllocateCommandBuffers");

    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    check_vk(vkCreateFence(harness.device, &fence_info, nullptr, &harness.frame_fence), "vkCreateFence");
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    check_vk(vkCreateSemaphore(harness.device, &semaphore_info, nullptr, &harness.acquired_semaphore),
             "vkCreateSemaphore(acquire)");
    check_vk(vkCreateSemaphore(harness.device, &semaphore_info, nullptr, &harness.rendered_semaphore),
             "vkCreateSemaphore(render)");

    VkSurfaceCapabilitiesKHR capabilities{};
    check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(harness.physical, harness.surface, &capabilities),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = capabilities.currentExtent;
    if (extent.width == UINT32_MAX || extent.height == UINT32_MAX) {
        extent.width = std::clamp(options.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height =
            std::clamp(options.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    harness.extent = extent;
    harness.transform = capabilities.currentTransform;
    note("surface extent=%ux%u transform=0x%x min-images=%u", extent.width, extent.height,
         static_cast<unsigned>(harness.transform), capabilities.minImageCount);

    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(harness.physical, harness.surface, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(harness.physical, harness.surface, &format_count, formats.data());
    VkSurfaceFormatKHR chosen_format = formats[0];
    for (const VkSurfaceFormatKHR& candidate : formats) {
        if (candidate.format == VK_FORMAT_B8G8R8A8_UNORM) {
            chosen_format = candidate;
            break;
        }
    }
    harness.format = chosen_format.format;
    harness.color_space = chosen_format.colorSpace;
    note("swapchain format=%d colorspace=%d", static_cast<int>(harness.format),
         static_cast<int>(harness.color_space));

    // A normal swapchain first. The provider retires it (oldSwapchain) and returns the proxy handle
    // in the same variable.
    VkSwapchainCreateInfoKHR swapchain_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    swapchain_info.surface = harness.surface;
    swapchain_info.minImageCount = std::max(3u, capabilities.minImageCount);
    if (capabilities.maxImageCount && swapchain_info.minImageCount > capabilities.maxImageCount)
        swapchain_info.minImageCount = capabilities.maxImageCount;
    swapchain_info.imageFormat = harness.format;
    swapchain_info.imageColorSpace = harness.color_space;
    swapchain_info.imageExtent = harness.extent;
    swapchain_info.imageArrayLayers = 1;
    swapchain_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    swapchain_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swapchain_info.preTransform = harness.transform;
    swapchain_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swapchain_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swapchain_info.clipped = VK_TRUE;
    swapchain_info.oldSwapchain = VK_NULL_HANDLE;
    check_vk(vkCreateSwapchainKHR(harness.device, &swapchain_info, nullptr, &harness.swapchain),
             "vkCreateSwapchainKHR(normal)");

    // --- provider swapchain ABI -----------------------------------------------------------------
    VkDeviceContext device_context{harness.device, harness.physical, vkGetDeviceProcAddr};
    uint64_t version_count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    versions.device = &device_context;
    versions.outputCount = &version_count;
    check_ffx(harness.provider.query(nullptr, &versions.header), "ffxQuery(swapchain versions)");
    if (version_count == 0 || version_count > 16) die("swapchain provider returned %llu versions",
                                                      static_cast<unsigned long long>(version_count));
    std::vector<uint64_t> version_ids(version_count);
    std::vector<const char*> version_names(version_count);
    uint64_t capacity = version_count;
    versions.outputCount = &capacity;
    versions.versionIds = version_ids.data();
    versions.versionNames = version_names.data();
    check_ffx(harness.provider.query(nullptr, &versions.header), "ffxQuery(swapchain version list)");
    uint32_t swapchain_version = 0;
    bool found_317 = false;
    for (uint64_t i = 0; i < capacity && i < version_count; ++i) {
        note("swapchain provider offers %s", version_names[i] ? version_names[i] : "(unnamed)");
        if (version_names[i] && !std::strcmp(version_names[i], "3.1.7")) {
            unsigned major = 0, minor = 0, patch = 0;
            if (std::sscanf(version_names[i], "%u.%u.%u", &major, &minor, &patch) != 3)
                die("provider version name '%s' is not major.minor.patch", version_names[i]);
            swapchain_version = (major << 22) | (minor << 12) | patch;
            found_317 = true;
        }
    }
    if (!found_317) die("swapchain provider does not offer the required 3.1.7 ABI");
    note("swapchain ABI=3.1.7 (0x%x)", swapchain_version);

    ffxCreateContextDescFrameGenerationSwapChainVersionVK swapchain_version_desc{};
    swapchain_version_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    swapchain_version_desc.version = swapchain_version;

    ffxCreateContextDescFrameGenerationSwapChainVK swapchain_desc{};
    swapchain_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    swapchain_desc.header.pNext = &swapchain_version_desc.header;
    swapchain_desc.physicalDevice = harness.physical;
    swapchain_desc.device = harness.device;
    swapchain_desc.swapchain = &harness.swapchain;  // normal handle in, proxy handle out
    swapchain_desc.createInfo = swapchain_info;
    swapchain_desc.createInfo.oldSwapchain = harness.swapchain;
    swapchain_desc.gameQueue = {harness.game_queue, queue.game, submit_game};
    swapchain_desc.presentQueue = {harness.present_queue, queue.present, submit_present};
    swapchain_desc.imageAcquireQueue = {harness.acquire_queue, queue.present, submit_acquire};
    check_ffx(harness.provider.create(&harness.swapchain_context, &swapchain_desc.header, nullptr),
              "ffxCreateContext(FRAMEGENERATIONSWAPCHAIN_VK)");
    if (!harness.swapchain_context || harness.swapchain == VK_NULL_HANDLE)
        die("provider returned an empty swapchain context or swapchain handle");
    note("swapchain context=ok proxy-handle=%p", reinterpret_cast<void*>(harness.swapchain));

    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    check_ffx(harness.provider.query(&harness.swapchain_context, &functions.header),
              "ffxQuery(swapchain replacement functions)");
    if (!functions.pOutGetSwapchainImagesKHR || !functions.pOutAcquireNextImageKHR ||
        !functions.pOutQueuePresentKHR || !functions.pOutDestroySwapchainFFXAPI ||
        !functions.pOutGetLastPresentCountFFXAPI)
        die("provider did not fill every swapchain replacement function");
    harness.get_swapchain_images = functions.pOutGetSwapchainImagesKHR;
    harness.acquire_next_image = functions.pOutAcquireNextImageKHR;
    harness.queue_present = functions.pOutQueuePresentKHR;
    harness.destroy_swapchain = functions.pOutDestroySwapchainFFXAPI;
    harness.get_last_present_count = functions.pOutGetLastPresentCountFFXAPI;

    check_vk(harness.get_swapchain_images(harness.device, harness.swapchain, &harness.swapchain_image_count, nullptr),
             "pOutGetSwapchainImagesKHR(count)");
    if (!harness.swapchain_image_count) die("replacement swapchain has no images");
    harness.swapchain_images.resize(harness.swapchain_image_count);
    check_vk(harness.get_swapchain_images(harness.device, harness.swapchain, &harness.swapchain_image_count,
                                          harness.swapchain_images.data()),
             "pOutGetSwapchainImagesKHR(images)");
    note("replacement images=%u composition=%s", harness.swapchain_image_count,
         harness.present_family_has_graphics ? "present-queue" : "game-queue");

    for (VkImage image : harness.swapchain_images) {
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = harness.format;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        check_vk(vkCreateImageView(harness.device, &view_info, nullptr, &view), "vkCreateImageView");
        harness.swapchain_views.push_back(view);
    }

    // --- frame-generation effect context ---------------------------------------------------------
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = device_context;

    ffxCreateContextDescFrameGenerationVersion effect_version{};
    effect_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    effect_version.version = FFX_FRAMEGENERATION_VERSION;
    effect_version.header.pNext = &backend.header;

    ffxCreateContextDescFrameGeneration effect{};
    effect.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    effect.header.pNext = &effect_version.header;
    effect.flags = FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    effect.displaySize = {harness.extent.width, harness.extent.height};
    effect.maxRenderSize = {harness.extent.width, harness.extent.height};
    effect.backBufferFormat = ffxApiGetSurfaceFormatVK(harness.format);
    check_ffx(harness.provider.create(&harness.effect, &effect.header, nullptr),
              "ffxCreateContext(FRAMEGENERATION)");
    if (!harness.effect) die("provider returned an empty frame-generation context");
    ffxQueryGetProviderVersion effect_provider{};
    effect_provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (harness.provider.query(&harness.effect, &effect_provider.header) == FFX_API_RETURN_OK)
        note("frame-generation effect provider=%s",
             effect_provider.versionName ? effect_provider.versionName : "(unknown)");

    create_depth_and_motion(harness);
    create_pipeline(harness);
    // Capture a previous real frame, the generated frame and the current real frame at one
    // frameID. Frame 0 is a reset frame (the interpolation shader copies the backbuffer), so use
    // the middle of the run unless the run is only a couple of frames long.
    harness.capture_frame = std::max(1u, options.frames / 2);
    if (harness.capture_frame >= options.frames) harness.capture_frame = options.frames - 1;
    harness.capture_previous_real = create_capture(harness);
    harness.capture_generated = create_capture(harness);
    harness.capture_current_real = create_capture(harness);
    note("readback capture frame=%u buffers=%llu bytes each", harness.capture_frame,
         static_cast<unsigned long long>(harness.capture_generated.size));

    // --- frame loop ------------------------------------------------------------------------------
    const float phase_step = static_cast<float>(harness.shift_pixels);
    note("inputs: color=moving-bars(step=%gpx) depth=gradient+square(static,R32F) "
         "motion=constant-shift(%gpx,R16G16F,uv-scale) jitter=halton(2,3)",
         phase_step, phase_step);

    uint32_t rendered = 0;
    uint32_t present_results_suboptimal = 0;
    for (uint32_t frame = 0; frame < options.frames; ++frame) {
        if (harness.fence_submitted) {
            check_vk(vkWaitForFences(harness.device, 1, &harness.frame_fence, VK_TRUE, UINT64_MAX),
                     "vkWaitForFences(frame)");
            check_vk(vkResetFences(harness.device, 1, &harness.frame_fence), "vkResetFences(frame)");
        }

        uint32_t image_index = 0;
        VkResult acquire = harness.acquire_next_image(harness.device, harness.swapchain, UINT64_MAX,
                                                      harness.acquired_semaphore, VK_NULL_HANDLE, &image_index);
        check_vk(acquire, "pOutAcquireNextImageKHR");
        if (image_index >= harness.swapchain_images.size())
            die("acquire returned image index %u (count %u)", image_index,
                static_cast<unsigned>(harness.swapchain_images.size()));

        check_vk(vkResetCommandBuffer(harness.command, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check_vk(vkBeginCommandBuffer(harness.command, &begin), "vkBeginCommandBuffer");

        VkImage color = harness.swapchain_images[image_index];
        image_barrier(harness.command, color, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = harness.swapchain_views[image_index];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color.float32[0] = 0.02f;
        attachment.clearValue.color.float32[1] = 0.02f;
        attachment.clearValue.color.float32[2] = 0.04f;
        attachment.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, harness.extent};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(harness.command, &rendering);
        vkCmdBindPipeline(harness.command, VK_PIPELINE_BIND_POINT_GRAPHICS, harness.pipeline);
        VkViewport viewport{0.0f, 0.0f, static_cast<float>(harness.extent.width),
                            static_cast<float>(harness.extent.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, harness.extent};
        vkCmdSetViewport(harness.command, 0, 1, &viewport);
        vkCmdSetScissor(harness.command, 0, 1, &scissor);
        float push[4] = {static_cast<float>(harness.extent.width), static_cast<float>(harness.extent.height),
                         phase_step * static_cast<float>(frame), 0.0f};
        vkCmdPushConstants(harness.command, harness.pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, push);
        vkCmdDraw(harness.command, 3, 1, 0, 0);
        vkCmdEndRendering(harness.command);

        image_barrier(harness.command, color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        // Configure the effect before the prepare is recorded. The provider copies this into its
        // next-present config; frameID must match the prepare and the callback storage slot.
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = reinterpret_cast<void*>(harness.swapchain);
        config.presentCallback = present_callback;
        config.presentCallbackUserContext = &harness;
        config.frameGenerationCallback = frame_generation_callback;
        config.frameGenerationCallbackUserContext = &harness;
        config.frameGenerationEnabled = true;
        config.allowAsyncWorkloads = false;  // interpolation on the game queue; present queue owns composition
        config.frameID = frame;
        config.flags = 0;
        check_ffx(harness.provider.configure(&harness.effect, &config.header), "ffxConfigure(FRAMEGENERATION)");

        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID = frame;
        prepare.commandList = harness.command;
        prepare.renderSize = {harness.extent.width, harness.extent.height};
        prepare.jitterOffset = {0.25f * (halton(frame, 2) - 0.5f), 0.25f * (halton(frame, 3) - 0.5f)};
        prepare.motionVectorScale = {static_cast<float>(harness.extent.width),
                                     static_cast<float>(harness.extent.height)};
        prepare.frameTimeDelta = 1000.0f / 60.0f;
        prepare.reset = (frame == 0);
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 1000.0f;
        prepare.cameraFovAngleVertical = 1.0471975512f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = ffxApiGetResourceVK(harness.depth_image,
                                            ffxApiGetImageResourceDescriptionVK(
                                                harness.depth_image, harness.depth_info,
                                                FFX_API_RESOURCE_USAGE_READ_ONLY),
                                            FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        prepare.motionVectors = ffxApiGetResourceVK(harness.motion_image,
                                                    ffxApiGetImageResourceDescriptionVK(
                                                        harness.motion_image, harness.motion_info,
                                                        FFX_API_RESOURCE_USAGE_READ_ONLY),
                                                    FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
        prepare.cameraPosition[0] = 0.0f;
        prepare.cameraPosition[1] = 0.0f;
        prepare.cameraPosition[2] = 5.0f;
        prepare.cameraUp[1] = 1.0f;
        prepare.cameraRight[0] = 1.0f;
        prepare.cameraForward[2] = -1.0f;
        check_ffx_resource(prepare.depth, "prepare.depth");
        check_ffx_resource(prepare.motionVectors, "prepare.motionVectors");
        check_ffx(harness.provider.dispatch(&harness.effect, &prepare.header),
                  "ffxDispatch(FRAMEGENERATION_PREPARE_V2)");

        check_vk(vkEndCommandBuffer(harness.command), "vkEndCommandBuffer");

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &harness.acquired_semaphore;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &harness.command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &harness.rendered_semaphore;
        {
            std::lock_guard<std::mutex> lock(harness.submit_mutex);
            check_vk(vkQueueSubmit(harness.game_queue, 1, &submit, harness.frame_fence), "vkQueueSubmit(game)");
        }
        harness.fence_submitted = true;

        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &harness.rendered_semaphore;
        present.swapchainCount = 1;
        present.pSwapchains = &harness.swapchain;
        present.pImageIndices = &image_index;
        VkResult present_result = harness.queue_present(harness.game_queue, &present);
        if (present_result == VK_SUBOPTIMAL_KHR) {
            ++present_results_suboptimal;
        } else if (present_result != VK_SUCCESS) {
            die("pOutQueuePresentKHR returned %d at frame %u", static_cast<int>(present_result), frame);
        }

        {
            std::lock_guard<std::mutex> lock(harness.callback_error_mutex);
            if (!harness.last_callback_error.empty())
                die("frame-generation callback failed at frame %u: %s", frame,
                    harness.last_callback_error.c_str());
        }
        if (g_validation_errors.load(std::memory_order_relaxed) != 0) {
            die("Vulkan validation error at frame %u: %s", frame,
                g_first_validation_error[0] ? g_first_validation_error : "(see log)");
        }

        ++rendered;
        if (frame == 0 || (frame + 1) % 30 == 0 || frame + 1 == options.frames) {
            note("frame %u/%u presented-by-provider=%llu generated-callbacks=%u real-callbacks=%u",
                 frame + 1, options.frames,
                 static_cast<unsigned long long>(harness.get_last_present_count(harness.swapchain)),
                 harness.present_callback_generated.load(), harness.present_callback_real.load());
        }
    }

    // --- drain and verify -----------------------------------------------------------------------
    ffxConfigureDescFrameGeneration disable{};
    disable.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    disable.swapChain = reinterpret_cast<void*>(harness.swapchain);
    disable.frameGenerationEnabled = false;
    disable.frameID = options.frames;
    check_ffx(harness.provider.configure(&harness.effect, &disable.header), "ffxConfigure(disable)");
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    check_ffx(harness.provider.dispatch(&harness.swapchain_context, &wait.header),
              "ffxDispatch(WAIT_FOR_PRESENTS)");
    check_vk(vkDeviceWaitIdle(harness.device), "vkDeviceWaitIdle");

    if (harness.frame_generation_failures.load() != 0)
        die("%u ffxDispatch(FRAMEGENERATION) call(s) failed", harness.frame_generation_failures.load());
    if (harness.present_callback_errors.load() != 0)
        die("%u present callback(s) failed", harness.present_callback_errors.load());
    {
        std::lock_guard<std::mutex> lock(harness.callback_error_mutex);
        if (!harness.last_callback_error.empty())
            die("callback error: %s", harness.last_callback_error.c_str());
    }
    if (g_validation_errors.load() != 0)
        die("Vulkan validation reported %llu error(s); first: %s",
            static_cast<unsigned long long>(g_validation_errors.load()),
            g_first_validation_error[0] ? g_first_validation_error : "(see log)");

    if (harness.capture_mask.load() != 7) {
        note("readback=skipped (captured mask 0x%x)", harness.capture_mask.load());
        die("readback capture incomplete (mask 0x%x); the generated/real present callbacks were not all observed",
            harness.capture_mask.load());
    }
    verify_capture(harness);

    const char* validation = use_validation ? (debug_utils_available ? "clean" : "no-debug-utils") : "off";
    if (use_validation && !debug_utils_available)
        die("validation layer was enabled but VK_EXT_debug_utils is unavailable; errors cannot be verified");

    const uint64_t last_present_count = harness.get_last_present_count(harness.swapchain);
    const uint32_t generated = harness.present_callback_generated.load();
    const uint32_t real = harness.present_callback_real.load();
    const uint32_t presented = generated + real;
    const uint32_t generated_dispatch = harness.frame_generation_calls.load();
    if (presented < rendered)
        die("present callback ran %u times for %u rendered frames", presented, rendered);
    if (last_present_count < rendered)
        die("provider present count %llu is below the %u rendered frames",
            static_cast<unsigned long long>(last_present_count), rendered);
    if (generated == 0) die("no generated frame was composed");
    if (present_results_suboptimal)
        note("note: pOutQueuePresentKHR returned VK_SUBOPTIMAL_KHR %u time(s)",
             present_results_suboptimal);

    // --- shutdown --------------------------------------------------------------------------------
    check_ffx(harness.provider.destroy(&harness.effect, nullptr), "ffxDestroyContext(FRAMEGENERATION)");
    harness.effect = nullptr;
    check_ffx(harness.provider.destroy(&harness.swapchain_context, nullptr),
              "ffxDestroyContext(FGSWAPCHAIN_VK)");
    harness.swapchain_context = nullptr;
    VkSwapchainKHR proxy = harness.swapchain;
    harness.swapchain = VK_NULL_HANDLE;
    harness.destroy_swapchain(harness.device, proxy, nullptr, reinterpret_cast<void*>(proxy));

    // Device objects: the provider is gone, so plain destruction is correct.
    for (VkImageView view : harness.swapchain_views) vkDestroyImageView(harness.device, view, nullptr);
    vkDestroyPipeline(harness.device, harness.pipeline, nullptr);
    vkDestroyPipelineLayout(harness.device, harness.pipeline_layout, nullptr);
    vkDestroyShaderModule(harness.device, harness.vertex_shader, nullptr);
    vkDestroyShaderModule(harness.device, harness.fragment_shader, nullptr);
    for (Capture* capture : {&harness.capture_previous_real, &harness.capture_generated,
                             &harness.capture_current_real}) {
        vkUnmapMemory(harness.device, capture->memory);
        capture->mapped = nullptr;
        vkDestroyBuffer(harness.device, capture->buffer, nullptr);
        vkFreeMemory(harness.device, capture->memory, nullptr);
    }
    vkDestroyImage(harness.device, harness.depth_image, nullptr);
    vkFreeMemory(harness.device, harness.depth_memory, nullptr);
    vkDestroyImage(harness.device, harness.motion_image, nullptr);
    vkFreeMemory(harness.device, harness.motion_memory, nullptr);
    vkDestroySemaphore(harness.device, harness.acquired_semaphore, nullptr);
    vkDestroySemaphore(harness.device, harness.rendered_semaphore, nullptr);
    vkDestroyFence(harness.device, harness.frame_fence, nullptr);
    vkDestroyCommandPool(harness.device, harness.command_pool, nullptr);
    vkDestroyDevice(harness.device, nullptr);
    destroy_instance_messenger(harness.instance, harness.messenger);
    vkDestroySurfaceKHR(harness.instance, harness.surface, nullptr);
    vkDestroyInstance(harness.instance, nullptr);

    if (g_validation_errors.load() != 0)
        die("Vulkan validation reported %llu error(s) during shutdown; first: %s",
            static_cast<unsigned long long>(g_validation_errors.load()),
            g_first_validation_error[0] ? g_first_validation_error : "(see log)");

    std::printf("FG-HARNESS ok rendered=%u presented=%u last_present_count=%llu generated=%u real=%u "
                "readback=ok validation=%s warnings=%llu surface=%s extent=%ux%u format=%d "
                "generated_dispatch=%u\n",
                rendered, presented, static_cast<unsigned long long>(last_present_count), generated, real,
                validation, static_cast<unsigned long long>(g_validation_warnings.load()),
                surface.headless ? "headless" : "display", harness.extent.width,
                harness.extent.height, static_cast<int>(harness.format), generated_dispatch);
    std::fflush(stdout);

    dlclose(harness.provider.library);
    return 0;
}
