// Headless validation for the MK8-online MLFI provider.
//
// Creates a Vulkan device with the feature set the captured MLFI modules need, creates the
// provider context at 1280x720, feeds two different frames through
// ffxDispatch(PrepareV2) + ffxDispatch(FrameGeneration), submits the command buffers and reads
// outputs[0] back.  It asserts the FFX return codes, the provider's dispatch counters, that the
// output is fully non-zero and that it changes when the input changes.
//
// The program prints one `MLFI-PROVIDER frame` line per frame and a final
// `MLFI-PROVIDER RESULT: OK|FAIL` line.  The Khronos validation layer is enabled; the same three
// documented validator disagreements as docs/MLFI-REPLAY.md 7 are counted separately, and any
// other error or warning fails the run.
//
// Build: tools/mlfi-provider/build.sh.  Run: `mlfi_provider_test` with
// MK8_MLFI_MODULES / MK8_MLFI_MODEL pointing at the prepared extraction.

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx/ffx_api.h"
#include "ffx/ffx_api_types.h"
#include "ffx/ffx_framegeneration.h"
#include "ffx/vk/ffx_api_vk.h"
// The SDK header relies on C++ typedef elision for this tag.
typedef struct VkQueueInfoFFXAPI VkQueueInfoFFXAPI;
#include "ffx/vk/ffx_api_framegeneration_vk.h"

#include "mlfi_provider_ext.h"

typedef struct FfxApiResource FfxApiResource;
typedef struct ffxCreateContextDescFrameGeneration ffxCreateContextDescFrameGeneration;
typedef struct ffxCreateBackendVKDesc ffxCreateBackendVKDesc;
typedef struct ffxCreateContextDescFrameGenerationVersion ffxCreateContextDescFrameGenerationVersion;
typedef struct ffxOverrideVersion ffxOverrideVersion;
typedef struct ffxConfigureDescFrameGeneration ffxConfigureDescFrameGeneration;
typedef struct ffxQueryDescGetVersions ffxQueryDescGetVersions;
typedef struct ffxQueryGetProviderVersion ffxQueryGetProviderVersion;
typedef struct ffxDispatchDescFrameGenerationPrepareV2 ffxDispatchDescFrameGenerationPrepareV2;

#define TEST_WIDTH 1280u
#define TEST_HEIGHT 720u
#define TEST_PIXELS (TEST_WIDTH * TEST_HEIGHT)
#define TEST_FRAMES 2

static int g_validation_errors;
static int g_validation_warnings;
static int g_known_cm_fconvert;
static int g_known_int_fetch;
static int g_known_cm2_spirv;
static int g_verbose;

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                     void* user)
{
    (void)types;
    (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
    {
        if (strstr(data->pMessage, "SPV_NV_cooperative_matrix2 was declared") ||
            strstr(data->pMessage, "CooperativeMatrixConversionsNV was declared") ||
            strstr(data->pMessage, "CooperativeMatrixPerElementOperationsNV was declared") ||
            strstr(data->pMessage, "CooperativeMatrixReductionsNV was declared"))
        {
            g_known_cm2_spirv++;
            return VK_FALSE;
        }
        if (strstr(data->pMessage, "VUID-vkCmdDispatch-magFilter-04553"))
        {
            g_known_int_fetch++;
            return VK_FALSE;
        }
        if (strstr(data->pMessage, "Expected Use of Matrix type and Result Type to be identical"))
        {
            g_known_cm_fconvert++;
            return VK_FALSE;
        }
        g_validation_errors++;
        fprintf(stderr, "[validation] ERROR: %s\n", data->pMessage);
        return VK_FALSE;
    }
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
    {
        g_validation_warnings++;
        if (g_verbose || g_validation_warnings <= 5)
            fprintf(stderr, "[validation] warning: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

static int has_instance_layer(const char* name)
{
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS)
        return 0;
    VkLayerProperties* props = (VkLayerProperties*)calloc(count ? count : 1, sizeof(*props));
    vkEnumerateInstanceLayerProperties(&count, props);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(props[i].layerName, name))
            found = 1;
    free(props);
    return found;
}

static int has_device_extension(VkPhysicalDevice phys, const char* name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(phys, NULL, &count, NULL) != VK_SUCCESS)
        return 0;
    VkExtensionProperties* props = (VkExtensionProperties*)calloc(count ? count : 1, sizeof(*props));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &count, props);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(props[i].extensionName, name))
            found = 1;
    free(props);
    return found;
}

static const char* vk_result_name(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    default: return "VkResult";
    }
}

static uint64_t fnv1a64(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

typedef struct
{
    VkBuffer buffer;
    VkDeviceMemory memory;
    void* map;
    VkDeviceSize size;
} HostBuffer;

static HostBuffer create_host_buffer(VkDevice dev, VkPhysicalDevice phys, VkDeviceSize size,
                                     VkBufferUsageFlags usage)
{
    HostBuffer out;
    memset(&out, 0, sizeof(out));
    out.size = size;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &ci, NULL, &out.buffer) != VK_SUCCESS)
        return out;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, out.buffer, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
        return out;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &ai, NULL, &out.memory) != VK_SUCCESS)
        return out;
    if (vkBindBufferMemory(dev, out.buffer, out.memory, 0) != VK_SUCCESS)
        return out;
    if (vkMapMemory(dev, out.memory, 0, VK_WHOLE_SIZE, 0, &out.map) != VK_SUCCESS)
        out.map = NULL;
    return out;
}

typedef struct
{
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFormat format;
} TestImage;

static TestImage create_image(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h,
                              VkFormat format, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    TestImage out;
    memset(&out, 0, sizeof(out));
    out.format = format;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(dev, &ci, NULL, &out.image) != VK_SUCCESS)
        return out;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, out.image, &req);
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(phys, &props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            type = i;
            break;
        }
    if (type == UINT32_MAX)
        return out;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    if (vkAllocateMemory(dev, &ai, NULL, &out.memory) != VK_SUCCESS)
        return out;
    if (vkBindImageMemory(dev, out.image, out.memory, 0) != VK_SUCCESS)
        return out;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &vi, NULL, &out.view);
    return out;
}

static void fill_pattern(uint8_t* pixels, uint32_t w, uint32_t h, int phase)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t* p = pixels + ((size_t)y * w + x) * 4;
            int r = 30 + (int)(190.0 * x / (w - 1));
            int g = 30 + (int)(170.0 * y / (h - 1));
            int b = 50 + (int)(100.0 * (0.5 + 0.5 * sin((x + phase * 23.0) * 0.045)));
            int bar = (int)((phase * 61) % w);
            int dx = (int)x - bar;
            if (dx > -1 && dx < 72)
            {
                r = 240;
                g = 80;
                b = 50;
            }
            if (y > h * 4 / 5)
            {
                r = (r + 40) / 2;
                g = (g + 40) / 2;
                b = 220;
            }
            p[0] = (uint8_t)(r > 255 ? 255 : r);
            p[1] = (uint8_t)(g > 255 ? 255 : g);
            p[2] = (uint8_t)(b > 255 ? 255 : b);
            p[3] = 255;
        }
}

static void fill_depth(float* pixels, uint32_t w, uint32_t h)
{
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            pixels[(size_t)y * w + x] = 0.5f;
}

static uint16_t float_to_half(float value)
{
    union { float f; uint32_t u; } bits = {value};
    uint32_t sign = (bits.u >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((bits.u >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits.u & 0x7fffffu;
    if (((bits.u >> 23) & 0xff) == 0xff)
        return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0));
    if (exponent <= 0)
    {
        if (exponent < -10)
            return (uint16_t)sign;
        mantissa |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exponent);
        uint32_t half_mantissa = mantissa >> shift;
        if ((mantissa >> (shift - 1)) & 1)
            half_mantissa++;
        return (uint16_t)(sign | half_mantissa);
    }
    if (exponent >= 31)
        return (uint16_t)(sign | 0x7c00u);
    uint32_t half = sign | ((uint32_t)exponent << 10) | (mantissa >> 13);
    if (mantissa & 0x1000u)
        half++;
    return (uint16_t)half;
}

static void fill_motion(uint16_t* pixels, uint32_t w, uint32_t h, float vx, float vy)
{
    uint16_t hx = float_to_half(vx), hy = float_to_half(vy);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            pixels[((size_t)y * w + x) * 2 + 0] = hx;
            pixels[((size_t)y * w + x) * 2 + 1] = hy;
        }
}

static void image_barrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                          VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

int main(int argc, char** argv)
{
    float motion_px = 0.0f;
    int d32_depth = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--verbose"))
            g_verbose = 1;
        else if (!strcmp(argv[i], "--d32-depth"))
            d32_depth = 1;
        else if (!strcmp(argv[i], "--motion") && i + 1 < argc)
            motion_px = (float)atof(argv[++i]);
        else
        {
            printf("usage: %s [--verbose] [--motion PX] [--d32-depth]\n", argv[0]);
            return 1;
        }
    }

    // ---------------------------------------------------------------- instance and validation
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mlfi-provider-test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char* layers[1] = {"VK_LAYER_KHRONOS_validation"};
    const char* extensions[1] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    int validation = has_instance_layer(layers[0]);
    if (validation)
    {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = layers;
        ici.enabledExtensionCount = 1;
        ici.ppEnabledExtensionNames = extensions;
    }
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (vkCreateInstance)\n");
        return 1;
    }
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (validation)
    {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
        if (create)
        {
            VkDebugUtilsMessengerCreateInfoEXT mci = {
                VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                                  VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
            mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mci.pfnUserCallback = debug_callback;
            create(instance, &mci, NULL, &messenger);
        }
    }
    if (!validation)
        fprintf(stderr, "[mlfi-provider-test] validation layer not available\n");

    // ---------------------------------------------------------------- device
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, NULL);
    if (!device_count)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (no Vulkan device)\n");
        return 1;
    }
    VkPhysicalDevice* devices = (VkPhysicalDevice*)calloc(device_count, sizeof(*devices));
    vkEnumeratePhysicalDevices(instance, &device_count, devices);
    VkPhysicalDevice phys = devices[0];
    for (uint32_t i = 0; i < device_count; ++i)
    {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            phys = devices[i];
            break;
        }
    }
    free(devices);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s (api %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, NULL);
    VkQueueFamilyProperties* families =
        (VkQueueFamilyProperties*)calloc(family_count ? family_count : 1, sizeof(*families));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &family_count, families);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
            (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT))
        {
            family = i;
            break;
        }
    free(families);
    if (family == UINT32_MAX)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (no graphics+compute queue family)\n");
        return 1;
    }

    VkPhysicalDeviceFeatures core;
    vkGetPhysicalDeviceFeatures(phys, &core);
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    VkPhysicalDeviceVariablePointersFeatures varptr = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES};
    VkPhysicalDevice16BitStorageFeatures st16 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES};
    void** tail = &f12.pNext;
    *tail = &cm;
    tail = &cm.pNext;
    *tail = &cm2;
    tail = &cm2.pNext;
    *tail = &f8;
    tail = &f8.pNext;
    *tail = &img64;
    tail = &img64.pNext;
    *tail = &varptr;
    tail = &varptr.pNext;
    *tail = &st16;
    tail = &st16.pNext;
    *tail = &f13;
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(phys, &query);
    if (!core.shaderInt64 || !f12.shaderFloat16 || !f12.shaderInt8 || !cm.cooperativeMatrix ||
        !cm2.cooperativeMatrixConversions || !cm2.cooperativeMatrixPerElementOperations ||
        !cm2.cooperativeMatrixReductions || !f8.shaderFloat8CooperativeMatrix ||
        !img64.shaderImageInt64Atomics || !f12.vulkanMemoryModel || !f13.subgroupSizeControl)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (device lacks the MLFI feature set)\n");
        return 1;
    }

    f13.subgroupSizeControl = VK_TRUE;
    f13.computeFullSubgroups = VK_FALSE;
    f12.vulkanMemoryModelAvailabilityVisibilityChains =
        f12.vulkanMemoryModelAvailabilityVisibilityChains ? VK_TRUE : VK_FALSE;
    f12.scalarBlockLayout = f12.scalarBlockLayout ? VK_TRUE : VK_FALSE;
    cm2.cooperativeMatrixProperties2 = VK_FALSE;
    varptr.variablePointers = varptr.variablePointersStorageBuffer;
    st16.storageBuffer16BitAccess = st16.storageBuffer16BitAccess ? VK_TRUE : VK_FALSE;
    st16.uniformAndStorageBuffer16BitAccess =
        st16.uniformAndStorageBuffer16BitAccess ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceFeatures enable = {0};
    enable.shaderInt64 = VK_TRUE;
    enable.shaderStorageImageReadWithoutFormat = VK_TRUE;
    enable.shaderStorageImageWriteWithoutFormat = VK_TRUE;

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    const char* device_extensions[10];
    uint32_t ext_count = 0;
    device_extensions[ext_count++] = VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME;
    if (has_device_extension(phys, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME;
    device_extensions[ext_count++] = VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME;
    device_extensions[ext_count++] = VK_EXT_SHADER_FLOAT8_EXTENSION_NAME;
    if (has_device_extension(phys, VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME;
    if (has_device_extension(phys, VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME;
    if (has_device_extension(phys, VK_KHR_8BIT_STORAGE_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_KHR_8BIT_STORAGE_EXTENSION_NAME;
    if (has_device_extension(phys, VK_KHR_16BIT_STORAGE_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_KHR_16BIT_STORAGE_EXTENSION_NAME;
    if (has_device_extension(phys, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME;
    if (has_device_extension(phys, VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME))
        device_extensions[ext_count++] = VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME;

    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &enable;
    dci.enabledExtensionCount = ext_count;
    dci.ppEnabledExtensionNames = device_extensions;
    dci.pNext = &f12;
    VkDevice dev = VK_NULL_HANDLE;
    VkResult vr = vkCreateDevice(phys, &dci, NULL, &dev);
    if (vr != VK_SUCCESS)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (vkCreateDevice %s)\n", vk_result_name(vr));
        return 1;
    }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, family, 0, &queue);

    // ---------------------------------------------------------------- provider queries
    int failures = 0;
    {
        ffxQueryDescGetVersions versions = {0};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        uint64_t count = 0;
        versions.outputCount = &count;
        ffxReturnCode_t rc = ffxQuery(NULL, &versions.header);
        if (rc != FFX_API_RETURN_OK || count != 1)
        {
            printf("version count query rc=%d count=%llu\n", (int)rc, (unsigned long long)count);
            failures++;
        }
        else
        {
            uint64_t id = 0;
            const char* name = NULL;
            count = 1;
            versions.versionIds = &id;
            versions.versionNames = &name;
            rc = ffxQuery(NULL, &versions.header);
            printf("version query rc=%d id=0x%016llx name=%s\n", (int)rc, (unsigned long long)id,
                   name ? name : "(null)");
            if (rc != FFX_API_RETURN_OK || id != 0xf600000001000002ull || !name || strcmp(name, "4.0.2"))
                failures++;
        }
        ffxQueryDescGetVersions swap = {0};
        swap.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        swap.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        uint64_t swap_count = 0;
        swap.outputCount = &swap_count;
        ffxReturnCode_t swap_rc = ffxQuery(NULL, &swap.header);
        printf("swapchain version query rc=%d count=%llu (expected NO_PROVIDER/0)\n", (int)swap_rc,
               (unsigned long long)swap_count);
        if (swap_rc != FFX_API_RETURN_NO_PROVIDER || swap_count != 0)
            failures++;
    }

    ffxCreateBackendVKDesc backend = {0};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = dev;
    backend.deviceContext.vkPhysicalDevice = phys;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescFrameGenerationVersion api_version = {0};
    api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    api_version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration create = {0};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.header.pNext = &api_version.header;
    api_version.header.pNext = &backend.header;
    create.displaySize.width = TEST_WIDTH;
    create.displaySize.height = TEST_HEIGHT;
    create.maxRenderSize.width = TEST_WIDTH;
    create.maxRenderSize.height = TEST_HEIGHT;
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;

    // Wrong override and wrong size must be refused before any pipeline is built.
    {
        ffxOverrideVersion override = {0};
        override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override.versionId = 0xf600000000c01006ull;  // FSR 3.1.6 frame interpolation
        ffxContext refused = NULL;
        backend.header.pNext = &override.header;
        ffxReturnCode_t rc = ffxCreateContext(&refused, &create.header, NULL);
        printf("create with foreign override rc=%d (expected NO_PROVIDER)\n", (int)rc);
        if (rc != FFX_API_RETURN_NO_PROVIDER || refused)
            failures++;
        backend.header.pNext = NULL;
        ffxCreateContextDescFrameGeneration wrong_size = create;
        wrong_size.displaySize.width = 1920;
        wrong_size.displaySize.height = 1080;
        wrong_size.maxRenderSize.width = 1920;
        wrong_size.maxRenderSize.height = 1080;
        ffxContext refused_size = NULL;
        rc = ffxCreateContext(&refused_size, &wrong_size.header, NULL);
        printf("create at 1920x1080 rc=%d (expected PARAMETER)\n", (int)rc);
        if (rc != FFX_API_RETURN_ERROR_PARAMETER || refused_size)
            failures++;
    }

    ffxContext context = NULL;
    ffxReturnCode_t rc = ffxCreateContext(&context, &create.header, NULL);
    printf("ffxCreateContext rc=%d context=%p\n", (int)rc, (void*)context);
    if (rc != FFX_API_RETURN_OK || !context)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (context creation rc=%d)\n", (int)rc);
        return 1;
    }
    {
        ffxQueryGetProviderVersion provider = {0};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        rc = ffxQuery(&context, &provider.header);
        printf("provider version rc=%d id=0x%016llx name=%s\n", (int)rc,
               (unsigned long long)provider.versionId, provider.versionName ? provider.versionName : "(null)");
        if (rc != FFX_API_RETURN_OK || provider.versionId != 0xf600000001000002ull)
            failures++;
    }
    {
        ffxConfigureDescFrameGeneration config = {0};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.frameGenerationEnabled = true;
        config.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        rc = ffxConfigure(&context, &config.header);
        printf("ffxConfigure rc=%d\n", (int)rc);
        if (rc != FFX_API_RETURN_OK)
            failures++;
    }

    Mk8MlfiStats stats;
    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    uint32_t stats_rc = mk8MlfiGetStats(context, &stats);
    printf("stats rc=%u pipelines=%u/%u/%u display=%ux%u render=%ux%u\n", stats_rc,
           stats.graph_pipelines, stats.copy_pipelines, stats.compose_pipelines, stats.display_width,
           stats.display_height, stats.render_width, stats.render_height);
    if (stats_rc != FFX_API_RETURN_OK || stats.graph_pipelines != 195)
        failures++;

    // ---------------------------------------------------------------- test resources
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(dev, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.commandBufferCount = 1;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);

    TestImage present = create_image(dev, phys, TEST_WIDTH, TEST_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM,
                                     VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VkFormat depth_format = d32_depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
    VkImageAspectFlags depth_aspect = d32_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    TestImage depth = create_image(dev, phys, TEST_WIDTH, TEST_HEIGHT, depth_format,
                                   VK_IMAGE_USAGE_SAMPLED_BIT, depth_aspect);
    TestImage motion = create_image(dev, phys, TEST_WIDTH, TEST_HEIGHT, VK_FORMAT_R16G16_SFLOAT,
                                    VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    TestImage output = create_image(dev, phys, TEST_WIDTH, TEST_HEIGHT, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
    if (!present.image || !depth.image || !motion.image || !output.image)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (test image creation)\n");
        return 1;
    }
    const VkDeviceSize color_bytes = (VkDeviceSize)TEST_WIDTH * TEST_HEIGHT * 4;
    HostBuffer upload = create_host_buffer(dev, phys, color_bytes * 2 + (VkDeviceSize)TEST_WIDTH * TEST_HEIGHT * 4,
                                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    HostBuffer readback = create_host_buffer(dev, phys, color_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!upload.buffer || !upload.map || !readback.buffer || !readback.map)
    {
        printf("MLFI-PROVIDER RESULT: FAIL (host buffer creation)\n");
        return 1;
    }

    uint8_t* pattern = (uint8_t*)malloc((size_t)color_bytes);
    uint8_t* output_host = (uint8_t*)malloc((size_t)color_bytes);
    uint64_t hashes[TEST_FRAMES] = {0};
    uint64_t nonzero[TEST_FRAMES] = {0};
    uint64_t total = (uint64_t)TEST_WIDTH * TEST_HEIGHT;
    int changed = 0;

    for (int frame = 0; frame < TEST_FRAMES; ++frame)
    {
        fill_pattern(pattern, TEST_WIDTH, TEST_HEIGHT, frame * 3 + 1);
        size_t color_off = 0;
        size_t depth_off = (size_t)color_bytes;
        size_t motion_off = (size_t)color_bytes + (size_t)TEST_WIDTH * TEST_HEIGHT * 4;
        memcpy((uint8_t*)upload.map + color_off, pattern, (size_t)color_bytes);
        float* depth_host = (float*)((uint8_t*)upload.map + depth_off);
        fill_depth(depth_host, TEST_WIDTH, TEST_HEIGHT);
        uint16_t* motion_host = (uint16_t*)((uint8_t*)upload.map + motion_off);
        fill_motion(motion_host, TEST_WIDTH, TEST_HEIGHT, motion_px / (float)TEST_WIDTH, 0.0f);

        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &bi);
        VkImageLayout input_old = frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED
                                             : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkPipelineStageFlags input_src = frame == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                    : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkAccessFlags input_access = frame == 0 ? 0 : VK_ACCESS_SHADER_READ_BIT;
        image_barrier(cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT, input_old,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, input_src,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, input_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        image_barrier(cmd, depth.image, depth_aspect, input_old,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, input_src,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, input_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        image_barrier(cmd, motion.image, VK_IMAGE_ASPECT_COLOR_BIT, input_old,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, input_src,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, input_access, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy = {0};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent.width = TEST_WIDTH;
        copy.imageExtent.height = TEST_HEIGHT;
        copy.imageExtent.depth = 1;
        copy.bufferOffset = color_off;
        vkCmdCopyBufferToImage(cmd, upload.buffer, present.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.bufferOffset = depth_off;
        copy.imageSubresource.aspectMask = depth_aspect;
        vkCmdCopyBufferToImage(cmd, upload.buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.bufferOffset = motion_off;
        vkCmdCopyBufferToImage(cmd, upload.buffer, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        image_barrier(cmd, present.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        image_barrier(cmd, depth.image, depth_aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        image_barrier(cmd, motion.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        image_barrier(cmd, output.image, VK_IMAGE_ASPECT_COLOR_BIT,
                      frame == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                      VK_IMAGE_LAYOUT_GENERAL,
                      frame == 0 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                      VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                      VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        // Sentinel fill: any pixel the provider does not compose stays magenta and is counted
        // below, so "the pass wrote the whole output" is measured rather than assumed.
        VkClearColorValue sentinel;
        sentinel.float32[0] = 0.0f;
        sentinel.float32[1] = 1.0f;
        sentinel.float32[2] = 0.0f;
        sentinel.float32[3] = 1.0f;
        VkImageSubresourceRange output_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, &sentinel, 1, &output_range);
        VkMemoryBarrier clear_done = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        clear_done.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clear_done.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 1, &clear_done, 0, NULL, 0, NULL);

        ffxDispatchDescFrameGenerationPrepareV2 prepare = {0};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID = (uint64_t)frame;
        prepare.commandList = cmd;
        prepare.renderSize.width = TEST_WIDTH;
        prepare.renderSize.height = TEST_HEIGHT;
        prepare.jitterOffset.x = 0.0f;
        prepare.jitterOffset.y = 0.0f;
        prepare.motionVectorScale.x = 1.0f;
        prepare.motionVectorScale.y = 1.0f;
        prepare.frameTimeDelta = 16.6f;
        prepare.reset = frame == 0;
        prepare.cameraNear = 0.1f;
        prepare.cameraFar = 1000.0f;
        prepare.cameraFovAngleVertical = 1.0f;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth.resource = (void*)depth.image;
        prepare.depth.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        prepare.depth.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
        prepare.depth.description.width = TEST_WIDTH;
        prepare.depth.description.height = TEST_HEIGHT;
        prepare.depth.description.mipCount = 1;
        prepare.depth.description.usage =
            d32_depth ? FFX_API_RESOURCE_USAGE_DEPTHTARGET : FFX_API_RESOURCE_USAGE_READ_ONLY;
        prepare.depth.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        prepare.motionVectors.resource = (void*)motion.image;
        prepare.motionVectors.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        prepare.motionVectors.description.format = FFX_API_SURFACE_FORMAT_R16G16_FLOAT;
        prepare.motionVectors.description.width = TEST_WIDTH;
        prepare.motionVectors.description.height = TEST_HEIGHT;
        prepare.motionVectors.description.mipCount = 1;
        prepare.motionVectors.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        prepare.cameraPosition[0] = 0.0f;
        prepare.cameraPosition[1] = 0.0f;
        prepare.cameraPosition[2] = 5.0f;
        prepare.cameraUp[1] = 1.0f;
        prepare.cameraRight[0] = 1.0f;
        prepare.cameraForward[2] = -1.0f;
        rc = ffxDispatch(&context, &prepare.header);
        if (rc != FFX_API_RETURN_OK)
        {
            printf("PrepareV2 frame %d rc=%d\n", frame, (int)rc);
            failures++;
            break;
        }

        ffxDispatchDescFrameGeneration generation = {0};
        generation.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        generation.commandList = cmd;
        generation.presentColor.resource = (void*)present.image;
        generation.presentColor.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        generation.presentColor.description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        generation.presentColor.description.width = TEST_WIDTH;
        generation.presentColor.description.height = TEST_HEIGHT;
        generation.presentColor.description.mipCount = 1;
        generation.presentColor.state = FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ;
        generation.outputs[0].resource = (void*)output.image;
        generation.outputs[0].description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
        generation.outputs[0].description.format = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
        generation.outputs[0].description.width = TEST_WIDTH;
        generation.outputs[0].description.height = TEST_HEIGHT;
        generation.outputs[0].description.mipCount = 1;
        generation.outputs[0].state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
        generation.numGeneratedFrames = 1;
        generation.reset = frame == 0;
        generation.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        generation.minMaxLuminance[1] = 1000.0f;
        generation.frameID = (uint64_t)frame;
        rc = ffxDispatch(&context, &generation.header);
        if (rc != FFX_API_RETURN_OK)
        {
            printf("FrameGeneration frame %d rc=%d\n", frame, (int)rc);
            failures++;
            break;
        }

        VkBufferImageCopy read = {0};
        read.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        read.imageSubresource.layerCount = 1;
        read.imageExtent.width = TEST_WIDTH;
        read.imageExtent.height = TEST_HEIGHT;
        read.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, readback.buffer, 1, &read);
        VkMemoryBarrier to_host = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        to_host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                             &to_host, 0, NULL, 0, NULL);
        vkEndCommandBuffer(cmd);

        VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        vr = vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
        if (vr == VK_SUCCESS)
            vr = vkQueueWaitIdle(queue);
        if (vr != VK_SUCCESS)
        {
            printf("frame %d submit/wait failed: %s\n", frame, vk_result_name(vr));
            failures++;
            break;
        }
        memcpy(output_host, readback.map, (size_t)color_bytes);
        hashes[frame] = fnv1a64(output_host, (size_t)color_bytes);
        uint64_t unwritten = 0;
        for (uint64_t i = 0; i < total; ++i)
        {
            if (output_host[i * 4] || output_host[i * 4 + 1] || output_host[i * 4 + 2])
                nonzero[frame]++;
            if (output_host[i * 4] == 0 && output_host[i * 4 + 1] == 255 && output_host[i * 4 + 2] == 0)
                unwritten++;
        }
        if (unwritten)
        {
            printf("frame %d left %llu/%llu pixels at the sentinel colour\n", frame,
                   (unsigned long long)unwritten, (unsigned long long)total);
            failures++;
        }
        if (frame && hashes[frame] != hashes[frame - 1])
            changed = 1;
        {
            const char* dump_dir = getenv("MK8_MLFI_DUMP_DIR");
            if (dump_dir && *dump_dir)
            {
                char path[4096];
                snprintf(path, sizeof(path), "%s/frame%d.rgba", dump_dir, frame);
                FILE* f = fopen(path, "wb");
                if (f)
                {
                    fwrite(output_host, 1, (size_t)color_bytes, f);
                    fclose(f);
                }
            }
        }
        printf("MLFI-PROVIDER frame %d hash=%016llx nonzero=%llu/%llu motion_px=%.2f\n", frame,
               (unsigned long long)hashes[frame], (unsigned long long)nonzero[frame],
               (unsigned long long)total, motion_px);
        fflush(stdout);
        vkResetCommandBuffer(cmd, 0);
    }

    memset(&stats, 0, sizeof(stats));
    stats.struct_size = sizeof(stats);
    stats_rc = mk8MlfiGetStats(context, &stats);
    printf("MLFI-PROVIDER stats rc=%u prepare_dispatches=%u copy_dispatches=%u model_dispatches=%u "
           "compose_dispatches=%u frames=%llu resets=%u last_prepare_rc=%u last_frame_rc=%u\n",
           stats_rc, stats.prepare_dispatches, stats.copy_dispatches, stats.model_dispatches,
           stats.compose_dispatches,
           (unsigned long long)stats.frames, stats.resets, stats.last_prepare_rc, stats.last_frame_rc);
    if (stats_rc != FFX_API_RETURN_OK || stats.prepare_dispatches != 5 || stats.copy_dispatches != 2 ||
        stats.model_dispatches != 191 || stats.compose_dispatches != 1 || stats.frames != TEST_FRAMES ||
        stats.last_prepare_rc != FFX_API_RETURN_OK || stats.last_frame_rc != FFX_API_RETURN_OK)
        failures++;
    if (!changed)
    {
        printf("output did not change between two different inputs\n");
        failures++;
    }
    // The compose pass writes every pixel of outputs[0]; the sentinel check above proves it and
    // the prediction is non-zero everywhere for both the reset frame and the following frame.
    for (int frame = 0; frame < TEST_FRAMES; ++frame)
        if (nonzero[frame] != total)
        {
            printf("frame %d output has %llu/%llu non-zero pixels\n", frame,
                   (unsigned long long)nonzero[frame], (unsigned long long)total);
            failures++;
        }

    ffxDestroyContext(&context, NULL);
    VkResult final_wait = vkDeviceWaitIdle(dev);
    if (final_wait != VK_SUCCESS)
    {
        printf("vkDeviceWaitIdle after the frames: %s\n", vk_result_name(final_wait));
        failures++;
    }
    if (g_validation_errors || g_validation_warnings)
        failures++;

    printf("MLFI-PROVIDER summary pipelines=%u dispatches=%u compose=1 validation_errors=%d "
           "validation_warnings=%d known_cm_fconvert=%d known_int_fetch_linear=%d known_cm2_spirv=%d "
           "hashes=%016llx/%016llx changed=%s device=%s\n",
           stats.graph_pipelines, stats.prepare_dispatches + stats.copy_dispatches +
               stats.model_dispatches + stats.compose_dispatches,
           g_validation_errors, g_validation_warnings, g_known_cm_fconvert, g_known_int_fetch,
           g_known_cm2_spirv, (unsigned long long)hashes[0], (unsigned long long)hashes[1],
           changed ? "yes" : "no", final_wait == VK_SUCCESS ? "alive" : vk_result_name(final_wait));
    free(pattern);
    free(output_host);
    vkDestroyBuffer(dev, upload.buffer, NULL);
    vkFreeMemory(dev, upload.memory, NULL);
    vkUnmapMemory(dev, readback.memory);
    vkDestroyBuffer(dev, readback.buffer, NULL);
    vkFreeMemory(dev, readback.memory, NULL);
    vkDestroyImageView(dev, present.view, NULL);
    vkDestroyImageView(dev, depth.view, NULL);
    vkDestroyImageView(dev, motion.view, NULL);
    vkDestroyImageView(dev, output.view, NULL);
    vkDestroyImage(dev, present.image, NULL);
    vkDestroyImage(dev, depth.image, NULL);
    vkDestroyImage(dev, motion.image, NULL);
    vkDestroyImage(dev, output.image, NULL);
    vkFreeMemory(dev, present.memory, NULL);
    vkFreeMemory(dev, depth.memory, NULL);
    vkFreeMemory(dev, motion.memory, NULL);
    vkFreeMemory(dev, output.memory, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    if (messenger)
    {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy =
            (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy)
            destroy(instance, messenger, NULL);
    }
    vkDestroyInstance(instance, NULL);
    if (failures)
    {
        printf("MLFI-PROVIDER RESULT: FAIL failures=%d\n", failures);
        return 1;
    }
    printf("MLFI-PROVIDER RESULT: OK\n");
    return 0;
}
