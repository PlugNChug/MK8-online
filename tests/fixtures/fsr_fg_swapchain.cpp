// Native smoke test for the FFX frame-interpolation swapchain provider.
//
// It mirrors runtime/mk8/graphics/fsr_fg.cpp against a headless Vulkan surface: verify that the
// provider offers the swapchain ABI 3.1.7, create the swapchain context with the extra present and
// acquire queues, query the replacement functions, create a 1080p frame-generation context,
// configure it, record a prepare dispatch and present several frames (the provider's interpolation
// and composition run between presents), and destroy everything in the documented order.
//
// The device enables formatless storage images because the generated frame-generation SPIR-V
// declares its storage images with format Unknown. When the Khronos validation layer is available
// (or MK8_FSR_FG_VALIDATION=1 is set), the instance enables it and a debug messenger; any validation
// error fails the run. Any failure returns non-zero; no step is treated as optional.
//
// Usage: fsr_fg_swapchain [provider.so]
// Provider resolution otherwise matches the runtime: MK8_FSR_FG_LIB, XDG_DATA_HOME, HOME.
#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_framegeneration_vk.h"
#include "vk/ffx_api_vk.h"

#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kFrames = 6;
uint32_t g_validation_errors = 0;

VKAPI_ATTR VkBool32 VKAPI_CALL validation_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    const bool error = severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    fprintf(stderr, "validation %s: %s: %s\n", error ? "ERROR" : "warning",
            data->pMessageIdName ? data->pMessageIdName : "(no id)", data->pMessage);
    if (error) g_validation_errors++;
    return VK_FALSE;
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

const char* provider_path(const char* argument) {
    if (argument && *argument) return argument;
    const char* env = getenv("MK8_FSR_FG_LIB");
    if (env && *env) return env;
    static char path[4096];
    const char* xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so", xdg);
        return path;
    }
    const char* home = getenv("HOME");
    if (home && *home) {
        snprintf(path, sizeof path, "%s/.local/share/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so", home);
        return path;
    }
    return "libamd_fidelityfx_framegeneration_vk.so";
}

bool load_provider(Provider* p, const char* path) {
    p->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!p->library) {
        printf("dlopen: %s\n", dlerror());
        return false;
    }
    p->create = (PfnCreate)dlsym(p->library, "ffxCreateContext");
    p->destroy = (PfnDestroy)dlsym(p->library, "ffxDestroyContext");
    p->query = (PfnQuery)dlsym(p->library, "ffxQuery");
    p->configure = (PfnConfigure)dlsym(p->library, "ffxConfigure");
    p->dispatch = (PfnDispatch)dlsym(p->library, "ffxDispatch");
    if (!p->create || !p->destroy || !p->query || !p->configure || !p->dispatch) {
        printf("provider lacks FFX exports\n");
        return false;
    }
    return true;
}

std::mutex g_submit_mutex;
VkQueue g_game, g_present, g_acquire;

VkResult submit_game(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard<std::mutex> lock(g_submit_mutex);
    return vkQueueSubmit(g_game, count, submits, fence);
}
VkResult submit_present(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard<std::mutex> lock(g_submit_mutex);
    return vkQueueSubmit(g_present, count, submits, fence);
}
VkResult submit_acquire(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    std::lock_guard<std::mutex> lock(g_submit_mutex);
    return vkQueueSubmit(g_acquire, count, submits, fence);
}

ffxContext g_effect;
Provider g_provider;

ffxReturnCode_t frame_generation(ffxDispatchDescFrameGeneration* params, void*) {
    return g_provider.dispatch(&g_effect, &params->header);
}

bool allocate_image(VkDevice device, VkPhysicalDevice physical, const VkImageCreateInfo& ci, VkImage* image,
                    VkDeviceMemory* memory_handle) {
    if (vkCreateImage(device, &ci, nullptr, image) != VK_SUCCESS) return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, *image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
        if ((requirements.memoryTypeBits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            type = i;
            break;
        }
    }
    if (type == UINT32_MAX) return false;
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = type;
    if (vkAllocateMemory(device, &allocation, nullptr, memory_handle) != VK_SUCCESS) return false;
    return vkBindImageMemory(device, *image, *memory_handle, 0) == VK_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const char* path = provider_path(argc > 1 ? argv[1] : nullptr);
    Provider provider;
    if (!load_provider(&provider, path)) return 1;
    g_provider = provider;
    printf("provider: %s\n", path);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    bool validation = false;
    bool debug_utils = false;
    {
        uint32_t layer_count = 0;
        vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
        std::vector<VkLayerProperties> layers(layer_count);
        vkEnumerateInstanceLayerProperties(&layer_count, layers.data());
        for (const VkLayerProperties& layer : layers)
            if (strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
        uint32_t extension_count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr);
        std::vector<VkExtensionProperties> available(extension_count);
        vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, available.data());
        for (const VkExtensionProperties& extension : available)
            if (strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0) debug_utils = true;
    }
    if (!validation || !debug_utils) {
        if (getenv("MK8_FSR_FG_VALIDATION")) {
            printf("FAIL: validation requested but the layer or VK_EXT_debug_utils is unavailable\n");
            return 1;
        }
        validation = false;  // run without the layer rather than requiring it everywhere
    }
    VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = validation_message;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME,
                                VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = validation ? 3u : 2u;
    instance_info.ppEnabledExtensionNames = extensions;
    instance_info.enabledLayerCount = validation ? 1u : 0u;
    instance_info.ppEnabledLayerNames = layers;
    instance_info.pNext = validation ? &debug_info : nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&instance_info, nullptr, &instance) != VK_SUCCESS) {
        printf("vkCreateInstance failed\n");
        return 1;
    }
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation) {
        auto create_messenger =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (!create_messenger || create_messenger(instance, &debug_info, nullptr, &messenger) != VK_SUCCESS) {
            printf("FAIL: could not install the validation messenger\n");
            return 1;
        }
        printf("validation: VK_LAYER_KHRONOS_validation enabled\n");
    } else {
        printf("validation: layer not enabled (not installed)\n");
    }
    auto create_headless = (PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(instance, "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT headless{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!create_headless || create_headless(instance, &headless, nullptr, &surface) != VK_SUCCESS) {
        printf("headless surface unavailable\n");
        return 1;
    }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(gpu, 0, surface, &present);
        if (present && properties.apiVersion >= VK_API_VERSION_1_3) {
            physical = gpu;
            printf("device: %s\n", properties.deviceName);
            break;
        }
    }
    if (!physical) {
        printf("no Vulkan 1.3 device with present support\n");
        return 1;
    }

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    uint32_t game_family = UINT32_MAX, present_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (game_family == UINT32_MAX && present && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
            game_family = i;
    }
    for (uint32_t i = 0; i < family_count; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface, &present);
        if (present && i != game_family && families[i].queueCount >= 2 &&
            (families[i].queueFlags & (VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))) {
            present_family = i;
            break;
        }
    }
    if (game_family == UINT32_MAX || present_family == UINT32_MAX) {
        printf("no game/present queue pair for the frame-interpolation swapchain\n");
        return 1;
    }
    printf("queues: game family %u, present/acquire family %u\n", game_family, present_family);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queues[2] = {{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO},
                                         {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}};
    queues[0].queueFamilyIndex = game_family;
    queues[0].queueCount = 1;
    queues[0].pQueuePriorities = &priority;
    queues[1].queueFamilyIndex = present_family;
    queues[1].queueCount = 2;
    queues[1].pQueuePriorities = &priority;
    std::vector<const char*> device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t property_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &property_count, nullptr);
    std::vector<VkExtensionProperties> device_properties(property_count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &property_count, device_properties.data());
    bool derivatives_supported = false;
    for (const VkExtensionProperties& property : device_properties)
        if (strcmp(property.extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0)
            derivatives_supported = true;
    if (derivatives_supported) device_extensions.push_back(VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);

    // The generated frame-generation shaders need formatless storage images (their OpTypeImage
    // format is Unknown), timeline semaphores (the swapchain creates them), dynamic rendering and
    // ComputeDerivativeGroupLinearKHR (the inpainting-pyramid passes).
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR available_derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    VkPhysicalDeviceVulkan13Features available13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features available12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    available13.pNext = &available_derivatives;
    available12.pNext = &available13;
    VkPhysicalDeviceFeatures2 available_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    available_features.pNext = &available12;
    vkGetPhysicalDeviceFeatures2(physical, &available_features);
    if (!available_features.features.shaderStorageImageWriteWithoutFormat) {
        printf("FAIL: the device cannot write storage images without a format\n");
        return 1;
    }
    if (!available12.timelineSemaphore || !available13.dynamicRendering || !derivatives_supported ||
        !available_derivatives.computeDerivativeGroupLinear) {
        printf("FAIL: missing a device feature the frame-generation provider requires\n");
        return 1;
    }
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR enabled_derivatives{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    enabled_derivatives.computeDerivativeGroupLinear = VK_TRUE;
    VkPhysicalDeviceVulkan13Features enabled13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    enabled13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features enabled12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    enabled12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 enabled_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    enabled_features.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    enabled_features.features.shaderStorageImageReadWithoutFormat =
        available_features.features.shaderStorageImageReadWithoutFormat;
    enabled_features.pNext = &enabled12;
    enabled12.pNext = &enabled13;
    enabled13.pNext = &enabled_derivatives;
    printf("features: formatless storage image write=1 read=%d derivatives=%d\n",
           (int)enabled_features.features.shaderStorageImageReadWithoutFormat,
           (int)enabled_derivatives.computeDerivativeGroupLinear);
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 2;
    device_info.pQueueCreateInfos = queues;
    device_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    device_info.ppEnabledExtensionNames = device_extensions.data();
    device_info.pNext = &enabled_features;
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(physical, &device_info, nullptr, &device) != VK_SUCCESS) {
        printf("vkCreateDevice failed\n");
        return 1;
    }
    vkGetDeviceQueue(device, game_family, 0, &g_game);
    vkGetDeviceQueue(device, present_family, 0, &g_present);
    vkGetDeviceQueue(device, present_family, 1, &g_acquire);

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = device;
    backend.deviceContext.vkPhysicalDevice = physical;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    uint64_t version_count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    versions.device = &backend.deviceContext;
    versions.outputCount = &version_count;
    if (provider.query(nullptr, &versions.header) != FFX_API_RETURN_OK || version_count == 0) {
        printf("swapchain version query failed\n");
        return 1;
    }
    std::vector<uint64_t> ids(version_count);
    std::vector<const char*> names(version_count);
    uint64_t capacity = version_count;
    versions.outputCount = &capacity;
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (provider.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        printf("swapchain version list failed\n");
        return 1;
    }
    uint32_t version = 0;
    bool found = false;
    for (uint64_t i = 0; i < capacity; i++) {
        printf("swapchain provider offers %s\n", names[i] ? names[i] : "(unnamed)");
        unsigned major = 0, minor = 0, patch = 0;
        if (names[i] && strcmp(names[i], "3.1.7") == 0 && sscanf(names[i], "%u.%u.%u", &major, &minor, &patch) == 3) {
            version = (major << 22) | (minor << 12) | patch;
            found = true;
        }
    }
    if (!found) {
        printf("FAIL: swapchain provider does not offer 3.1.7\n");
        return 1;
    }

    VkSurfaceCapabilitiesKHR surface_caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &surface_caps);
    VkSwapchainCreateInfoKHR create_info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create_info.surface = surface;
    create_info.minImageCount = surface_caps.minImageCount > 3 ? surface_caps.minImageCount : 3;
    create_info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    create_info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create_info.imageExtent = {1920, 1080};
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    create_info.preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    create_info.clipped = VK_TRUE;

    ffxCreateContextDescFrameGenerationSwapChainVersionVK swapchain_version{};
    swapchain_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    swapchain_version.version = version;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    ffxCreateContextDescFrameGenerationSwapChainVK swapchain_desc{};
    swapchain_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    swapchain_desc.header.pNext = &swapchain_version.header;
    swapchain_desc.physicalDevice = physical;
    swapchain_desc.device = device;
    swapchain_desc.swapchain = &swapchain;
    swapchain_desc.createInfo = create_info;
    swapchain_desc.gameQueue = {g_game, game_family, submit_game};
    swapchain_desc.presentQueue = {g_present, present_family, submit_present};
    swapchain_desc.imageAcquireQueue = {g_acquire, present_family, submit_acquire};
    ffxContext swapchain_context = nullptr;
    ffxReturnCode_t rc = provider.create(&swapchain_context, &swapchain_desc.header, nullptr);
    printf("swapchain context: rc=%d (%s)\n", (int)rc, rc == FFX_API_RETURN_OK ? "OK" : "FAILED");
    if (rc != FFX_API_RETURN_OK || !swapchain_context || !swapchain) return 1;

    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    rc = provider.query(&swapchain_context, &functions.header);
    if (rc != FFX_API_RETURN_OK || !functions.pOutGetSwapchainImagesKHR || !functions.pOutAcquireNextImageKHR ||
        !functions.pOutQueuePresentKHR || !functions.pOutDestroySwapchainFFXAPI) {
        printf("replacement functions failed\n");
        return 1;
    }
    uint32_t image_count = 0;
    if (functions.pOutGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr) != VK_SUCCESS || image_count == 0) {
        printf("swapchain image count failed\n");
        return 1;
    }
    std::vector<VkImage> images(image_count);
    if (functions.pOutGetSwapchainImagesKHR(device, swapchain, &image_count, images.data()) != VK_SUCCESS) {
        printf("swapchain images failed\n");
        return 1;
    }
    printf("replacement images: %u\n", image_count);

    ffxCreateContextDescFrameGenerationVersion effect_version{};
    effect_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    effect_version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration effect{};
    effect.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    effect.header.pNext = &effect_version.header;
    effect_version.header.pNext = &backend.header;
    effect.displaySize = {create_info.imageExtent.width, create_info.imageExtent.height};
    effect.maxRenderSize = {create_info.imageExtent.width, create_info.imageExtent.height};
    effect.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    rc = provider.create(&g_effect, &effect.header, nullptr);
    printf("frame-generation context: rc=%d (%s)\n", (int)rc, rc == FFX_API_RETURN_OK ? "OK" : "FAILED");
    if (rc != FFX_API_RETURN_OK || !g_effect) return 1;

    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = swapchain;
    config.frameGenerationCallback = frame_generation;
    config.frameGenerationEnabled = true;
    config.frameID = 0;
    rc = provider.configure(&g_effect, &config.header);
    printf("configure: rc=%d (%s)\n", (int)rc, rc == FFX_API_RETURN_OK ? "OK" : "FAILED");
    if (rc != FFX_API_RETURN_OK) return 1;

    // Depth 1.0 and zero motion vectors: enough to run the prepare pipeline end to end.
    VkImageCreateInfo depth_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    depth_info.imageType = VK_IMAGE_TYPE_2D;
    depth_info.format = VK_FORMAT_R32_SFLOAT;
    depth_info.extent = {1920, 1080, 1};
    depth_info.mipLevels = 1;
    depth_info.arrayLayers = 1;
    depth_info.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    depth_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory = VK_NULL_HANDLE;
    bool depth_ok = allocate_image(device, physical, depth_info, &depth, &depth_memory);
    VkImageCreateInfo motion_info = depth_info;
    motion_info.format = VK_FORMAT_R16G16_SFLOAT;
    VkImage motion = VK_NULL_HANDLE;
    VkDeviceMemory motion_memory = VK_NULL_HANDLE;
    bool motion_ok = allocate_image(device, physical, motion_info, &motion, &motion_memory);
    if (!depth_ok || !motion_ok) {
        printf("depth/motion allocation failed\n");
        return 1;
    }

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = game_family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &pool_info, nullptr, &pool);
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = kFrames;
    VkCommandBuffer commands[kFrames]{};
    vkAllocateCommandBuffers(device, &command_info, commands);
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphore acquired[kFrames]{}, rendered[kFrames]{};
    VkFence fences[kFrames]{};
    for (uint32_t frame = 0; frame < kFrames; frame++) {
        vkCreateSemaphore(device, &semaphore_info, nullptr, &acquired[frame]);
        vkCreateSemaphore(device, &semaphore_info, nullptr, &rendered[frame]);
        vkCreateFence(device, &fence_info, nullptr, &fences[frame]);
    }

    auto barrier = [](VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                      VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
                      VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = src;
        b.dstAccessMask = dst;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    };

    ffxDispatchDescFrameGenerationPrepareV2 prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.renderSize = {1920, 1080};
    prepare.jitterOffset = {0.0f, 0.0f};
    prepare.motionVectorScale = {1920.0f, 1080.0f};
    prepare.frameTimeDelta = 16.7f;
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.0472f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceVK(depth, ffxApiGetImageResourceDescriptionVK(depth, depth_info, 0),
                                        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceVK(motion, ffxApiGetImageResourceDescriptionVK(motion, motion_info, 0),
                                                FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.cameraPosition[2] = 5.0f;
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = -1.0f;

    VkImageView views[kFrames]{};
    for (uint32_t frame = 0; frame < kFrames; frame++) {
        uint32_t index = 0;
        VkResult acquire_result =
            functions.pOutAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired[frame], VK_NULL_HANDLE, &index);
        if (acquire_result != VK_SUCCESS) {
            printf("acquire %u failed: %d\n", frame, (int)acquire_result);
            return 1;
        }
        VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_info.image = images[index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = create_info.imageFormat;
        view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &view_info, nullptr, &views[frame]) != VK_SUCCESS) {
            printf("image view %u failed\n", frame);
            return 1;
        }

        VkCommandBuffer command = commands[frame];
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) return 1;

        // The depth and motion images are cleared once and stay in SHADER_READ_ONLY for every
        // prepare dispatch. The backbuffer is acquired fresh each frame, so its layout starts at
        // UNDEFINED.
        if (frame == 0) {
            VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkClearColorValue depth_clear{{1.0f, 0.0f, 0.0f, 0.0f}};
            barrier(command, depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(command, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &depth_clear, 1, &range);
            barrier(command, depth, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            VkClearColorValue motion_clear{{0, 0, 0, 0}};
            barrier(command, motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdClearColorImage(command, motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &motion_clear, 1, &range);
            barrier(command, motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }

        barrier(command, images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = views[frame];
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color.float32[0] = 0.1f + 0.01f * (float)frame;
        attachment.clearValue.color.float32[1] = 0.2f;
        attachment.clearValue.color.float32[2] = 0.3f;
        attachment.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {1920, 1080}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &attachment;
        vkCmdBeginRendering(command, &rendering);
        vkCmdEndRendering(command);
        barrier(command, images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        prepare.frameID = frame;
        prepare.commandList = command;
        prepare.reset = frame == 0;
        rc = provider.dispatch(&g_effect, &prepare.header);
        if (rc != FFX_API_RETURN_OK) {
            printf("prepare dispatch %u: rc=%d (FAILED)\n", frame, (int)rc);
            return 1;
        }
        if (vkEndCommandBuffer(command) != VK_SUCCESS) return 1;

        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &acquired[frame];
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &rendered[frame];
        if (vkQueueSubmit(g_game, 1, &submit, fences[frame]) != VK_SUCCESS) {
            printf("submit %u failed\n", frame);
            return 1;
        }
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &rendered[frame];
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain;
        present.pImageIndices = &index;
        VkResult present_result = functions.pOutQueuePresentKHR(g_game, &present);
        printf("frame %u: image %u prepare OK present %d\n", frame, index, (int)present_result);
        if (present_result != VK_SUCCESS && present_result != VK_SUBOPTIMAL_KHR) return 1;
    }
    VkResult fences_result = vkWaitForFences(device, kFrames, fences, VK_TRUE, UINT64_MAX);
    if (fences_result != VK_SUCCESS) {
        printf("frame fences failed: %d\n", (int)fences_result);
        return 1;
    }
    printf("presented %u frames\n", kFrames);

    config.frameGenerationEnabled = false;
    config.frameGenerationCallback = nullptr;
    provider.configure(&g_effect, &config.header);
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    provider.dispatch(&swapchain_context, &wait.header);
    if (provider.destroy(&g_effect, nullptr) != FFX_API_RETURN_OK) {
        printf("destroy frame-generation context failed\n");
        return 1;
    }
    g_effect = nullptr;
    if (provider.destroy(&swapchain_context, nullptr) != FFX_API_RETURN_OK) {
        printf("destroy swapchain context failed\n");
        return 1;
    }
    functions.pOutDestroySwapchainFFXAPI(device, swapchain, nullptr, (void*)swapchain);
    VkResult idle = vkDeviceWaitIdle(device);
    printf("device idle after present: %d\n", (int)idle);
    if (idle != VK_SUCCESS) return 1;

    printf("validation errors: %u\n", g_validation_errors);
    for (uint32_t frame = 0; frame < kFrames; frame++) {
        if (views[frame]) vkDestroyImageView(device, views[frame], nullptr);
        vkDestroySemaphore(device, acquired[frame], nullptr);
        vkDestroySemaphore(device, rendered[frame], nullptr);
        vkDestroyFence(device, fences[frame], nullptr);
    }
    vkDestroyImage(device, depth, nullptr);
    vkDestroyImage(device, motion, nullptr);
    vkFreeMemory(device, depth_memory, nullptr);
    vkFreeMemory(device, motion_memory, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    if (messenger) {
        auto destroy_messenger =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_messenger) destroy_messenger(instance, messenger, nullptr);
    }
    vkDestroySurfaceKHR(instance, surface, nullptr);
    vkDestroyInstance(instance, nullptr);
    dlclose(provider.library);
    if (g_validation_errors != 0) {
        printf("RESULT: FAILED (validation errors)\n");
        return 1;
    }
    printf("RESULT: OK\n");
    return 0;
}
