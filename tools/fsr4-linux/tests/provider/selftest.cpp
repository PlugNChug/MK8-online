// Headless self-test for the native FSR 4.1.2 FFX provider.
//
// Two checks, both against a real Vulkan device and the installed provider:
//   1. ffxQuery(GET_VERSIONS) without a context must list exactly "FSR 4.1.2";
//   2. ffxCreateContext through the deprecated 1.x Vulkan backend descriptor (the
//      path the MK8 runtime uses with MK8_FSR_LEGACY_ABI=1), a neutral 1920x1080
//      -> 2560x1440 dispatch, a readback, and ffxDestroyContext.
//
// The provider reads its SPIR-V and model bank from FSR4_ASSET_DIR, else from
// <provider-dir>/fsr4-assets. --assets sets FSR4_ASSET_DIR explicitly.
//
// Usage:
//   selftest <provider.so> [--query-only] [--assets <dir>]
//            [<render-w> <render-h> <output-w> <output-h>]
//
// SPDX-License-Identifier: GPL-3.0-only

#include <vulkan/vulkan.h>

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx_api.h"
#include "vk/ffx_api_vk.h"
#include "ffx_upscale.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define VK_CHECK(x)                                                                  \
    do {                                                                             \
        VkResult r_ = (x);                                                           \
        if (r_ != VK_SUCCESS) {                                                      \
            std::fprintf(stderr, "VK_FAIL %s = %d (%s:%d)\n", #x, (int)r_, __FILE__, \
                         __LINE__);                                                  \
            return 1;                                                                \
        }                                                                            \
    } while (0)

namespace {

using PfnFfxCreateContext = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*,
                                                const ffxAllocationCallbacks*);
using PfnFfxDestroyContext = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using PfnFfxConfigure = ffxReturnCode_t (*)(ffxContext*, const ffxConfigureDescHeader*);
using PfnFfxQuery = ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*);
using PfnFfxDispatch = ffxReturnCode_t (*)(ffxContext*, const ffxDispatchDescHeader*);

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return UINT32_MAX;
}

Img makeImage(VkDevice device, VkPhysicalDevice physical, VkFormat format, uint32_t w,
              uint32_t h, VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Img img;
    img.format = format;
    img.create.imageType = VK_IMAGE_TYPE_2D;
    img.create.format = format;
    img.create.extent = {w, h, 1};
    img.create.mipLevels = 1;
    img.create.arrayLayers = 1;
    img.create.samples = VK_SAMPLE_COUNT_1_BIT;
    img.create.tiling = VK_IMAGE_TILING_OPTIMAL;
    img.create.usage = usage;
    img.create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img.create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &img.create, nullptr, &img.image) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memoryType(physical, req.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(device, &ai, nullptr, &img.memory) != VK_SUCCESS ||
        vkBindImageMemory(device, img.image, img.memory, 0) != VK_SUCCESS)
        return {};
    (void)aspect;
    return img;
}

void imageBarrier(VkCommandBuffer cb, VkImage image, VkImageLayout old_layout,
                  VkImageLayout new_layout, VkAccessFlags src, VkAccessFlags dst,
                  VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage,
                  VkImageAspectFlags aspect) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool hasExtension(const std::vector<VkExtensionProperties>& all, const char* name) {
    for (const auto& e : all)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

void messageCallback(uint32_t type, const wchar_t* message) {
    std::printf("[ffx] type=%u %ls\n", type, message ? message : L"(null)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <provider.so> [--query-only] [--assets <dir>] "
                     "[<render-w> <render-h> <output-w> <output-h>]\n",
                     argv[0]);
        return 2;
    }
    const std::string provider_path = argv[1];
    bool query_only = false;
    uint32_t rw = 1920, rh = 1080, ow = 2560, oh = 1440;
    std::vector<uint32_t> sizes;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--query-only") {
            query_only = true;
        } else if (arg == "--assets") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--assets needs a directory\n");
                return 2;
            }
            setenv("FSR4_ASSET_DIR", argv[++i], 1);
        } else if (!arg.empty() && arg[0] == '-') {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            return 2;
        } else {
            sizes.push_back(static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 0)));
        }
    }
    if (!sizes.empty() && sizes.size() != 4) {
        std::fprintf(stderr, "give exactly four sizes (render w/h, output w/h)\n");
        return 2;
    }
    if (sizes.size() == 4) {
        rw = sizes[0];
        rh = sizes[1];
        ow = sizes[2];
        oh = sizes[3];
    }
    if (!rw || !rh || !ow || !oh) {
        std::fprintf(stderr, "sizes must be non-zero\n");
        return 2;
    }

    void* library = dlopen(provider_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "dlopen %s failed: %s\n", provider_path.c_str(), dlerror());
        return 1;
    }
    auto create = reinterpret_cast<PfnFfxCreateContext>(dlsym(library, "ffxCreateContext"));
    auto destroy = reinterpret_cast<PfnFfxDestroyContext>(dlsym(library, "ffxDestroyContext"));
    auto configure = reinterpret_cast<PfnFfxConfigure>(dlsym(library, "ffxConfigure"));
    auto query = reinterpret_cast<PfnFfxQuery>(dlsym(library, "ffxQuery"));
    auto dispatch = reinterpret_cast<PfnFfxDispatch>(dlsym(library, "ffxDispatch"));
    if (!create || !destroy || !configure || !query || !dispatch) {
        std::fprintf(stderr, "%s is missing one of the five FFX entry points\n",
                     provider_path.c_str());
        return 1;
    }
    std::printf("provider: %s\n", provider_path.c_str());
    if (const char* assets = std::getenv("FSR4_ASSET_DIR"))
        std::printf("assets: %s\n", assets);
    else
        std::printf("assets: <provider-dir>/fsr4-assets (default)\n");

    // 1. Version enumeration without a context. The runtime calls this before
    // creating the context to resolve MK8_FSR_VERSION.
    uint64_t count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.outputCount = &count;
    ffxReturnCode_t rc = query(nullptr, &versions.header);
    std::printf("ffxQuery(GET_VERSIONS) -> 0x%x (count=%llu)\n", rc,
                (unsigned long long)count);
    if (rc != FFX_API_RETURN_OK) return 1;
    if (count != 1) {
        std::fprintf(stderr, "expected exactly one provider version, got %llu\n",
                     (unsigned long long)count);
        return 1;
    }
    uint64_t version_id = 0;
    const char* version_name = nullptr;
    uint64_t capacity = 1;
    versions.outputCount = &capacity;
    versions.versionIds = &version_id;
    versions.versionNames = &version_name;
    rc = query(nullptr, &versions.header);
    if (rc != FFX_API_RETURN_OK || !version_name) {
        std::fprintf(stderr, "ffxQuery(GET_VERSIONS) names failed: 0x%x\n", rc);
        return 1;
    }
    std::printf("version[0]: name=%s id=0x%llx\n", version_name,
                (unsigned long long)version_id);
    if (std::strcmp(version_name, "4.1.2") != 0) {
        std::fprintf(stderr, "provider does not list version 4.1.2\n");
        return 1;
    }
    if (version_id != FFX_UPSCALER_VERSION) {
        std::fprintf(stderr, "version id 0x%llx != FFX_UPSCALER_VERSION 0x%x\n",
                     (unsigned long long)version_id, FFX_UPSCALER_VERSION);
        return 1;
    }
    if (query_only) {
        std::printf("RESULT: OK\n");
        return 0;
    }

    // 2. A real device, created like the game's FSR path: the provider's FP8
    // cooperative-matrix shaders need both extensions.
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr4-selftest";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t count_devices = 0;
    vkEnumeratePhysicalDevices(instance, &count_devices, nullptr);
    std::vector<VkPhysicalDevice> devices(count_devices);
    vkEnumeratePhysicalDevices(instance, &count_devices, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = d;
            break;
        }
    }
    if (!physical && !devices.empty()) physical = devices[0];
    if (!physical) {
        std::fprintf(stderr, "no Vulkan physical device\n");
        return 1;
    }

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> extensions(ext_count);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &ext_count, extensions.data());
    if (!hasExtension(extensions, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) ||
        !hasExtension(extensions, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) {
        std::fprintf(stderr,
                     "device lacks %s or %s; the FSR 4.1.2 provider cannot run here\n",
                     VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                     VK_EXT_SHADER_FLOAT8_EXTENSION_NAME);
        return 1;
    }

    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    core.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &cm;
    cm.pNext = &f8;
    vkGetPhysicalDeviceFeatures2(physical, &core);
    if (!cm.cooperativeMatrix || !f8.shaderFloat8) {
        std::fprintf(stderr,
                     "device exposes the extensions but not the features "
                     "(cooperativeMatrix=%u shaderFloat8=%u)\n",
                     cm.cooperativeMatrix, f8.shaderFloat8);
        return 1;
    }
    const char* enabled[] = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                             VK_EXT_SHADER_FLOAT8_EXTENSION_NAME};
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 2;
    dci.ppEnabledExtensionNames = enabled;
    dci.pNext = &core;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    const VkImageUsageFlags input_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Img color_img = makeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, rw, rh,
                              input_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    Img depth_img = makeImage(device, physical, VK_FORMAT_D32_SFLOAT, rw, rh, input_usage,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
    Img motion_img = makeImage(device, physical, VK_FORMAT_R16G16_SFLOAT, rw, rh,
                               input_usage, VK_IMAGE_ASPECT_COLOR_BIT);
    Img output_img = makeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    if (!color_img.image || !depth_img.image || !motion_img.image || !output_img.image) {
        std::fprintf(stderr, "failed to create test images\n");
        return 1;
    }

    // Deterministic neutral inputs: a smooth colour gradient, depth at the far
    // plane and zero motion vectors. The check is structural (the chain runs and
    // writes a varied image), not an image-quality claim.
    const VkDeviceSize cb_size = static_cast<VkDeviceSize>(rw) * rh * 4;
    std::vector<uint8_t> color(static_cast<size_t>(cb_size));
    for (uint32_t y = 0; y < rh; ++y) {
        for (uint32_t x = 0; x < rw; ++x) {
            uint8_t* p = color.data() + (static_cast<size_t>(y) * rw + x) * 4;
            p[0] = static_cast<uint8_t>(x * 255u / (rw ? rw - 1 : 1));
            p[1] = static_cast<uint8_t>(y * 255u / (rh ? rh - 1 : 1));
            p[2] = static_cast<uint8_t>(128);
            p[3] = 255;
        }
    }
    std::vector<uint8_t> depth(static_cast<size_t>(cb_size), 0);
    std::vector<uint8_t> motion(static_cast<size_t>(cb_size), 0);
    for (uint32_t i = 0; i < rw * rh; ++i) {
        const uint32_t one = 0x3f800000u;  // 1.0f
        std::memcpy(depth.data() + static_cast<size_t>(i) * 4, &one, 4);
    }

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = cb_size * 3;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_mem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &upload));
    VkMemoryRequirements ureq{};
    vkGetBufferMemoryRequirements(device, upload, &ureq);
    VkMemoryAllocateInfo uai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    uai.allocationSize = ureq.size;
    uint32_t mt = memoryType(physical, ureq.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) {
        std::fprintf(stderr, "no host-visible memory type\n");
        return 1;
    }
    uai.memoryTypeIndex = mt;
    VK_CHECK(vkAllocateMemory(device, &uai, nullptr, &upload_mem));
    VK_CHECK(vkBindBufferMemory(device, upload, upload_mem, 0));
    void* ptr = nullptr;
    VK_CHECK(vkMapMemory(device, upload_mem, 0, bci.size, 0, &ptr));
    std::memcpy(ptr, color.data(), color.size());
    std::memcpy(static_cast<uint8_t*>(ptr) + cb_size, depth.data(), depth.size());
    std::memcpy(static_cast<uint8_t*>(ptr) + cb_size * 2, motion.data(), motion.size());
    vkUnmapMemory(device, upload_mem);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(device, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(device, &cbai, &cb));

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    imageBarrier(cb, color_img.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    imageBarrier(cb, depth_img.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_IMAGE_ASPECT_DEPTH_BIT);
    imageBarrier(cb, motion_img.image, VK_IMAGE_LAYOUT_UNDEFINED,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {rw, rh, 1};
    vkCmdCopyBufferToImage(cb, upload, color_img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.bufferOffset = cb_size;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyBufferToImage(cb, upload, depth_img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.bufferOffset = cb_size * 2;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cb, upload, motion_img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    imageBarrier(cb, color_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    imageBarrier(cb, depth_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    imageBarrier(cb, motion_img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    imageBarrier(cb, output_img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    VK_CHECK(vkCreateFence(device, &fci, nullptr, &fence));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));
    vkResetFences(device, 1, &fence);

    // Deprecated 1.x backend descriptor: ffxCreateBackendVKDesc_DEPRECATED has
    // the same three VkDeviceContext fields first, which is why the runtime's
    // MK8_FSR_LEGACY_ABI path reinterprets the current struct.
    ffxCreateBackendVKDesc_DEPRECATED backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = physical;
    backend.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscaleVersion version_desc{};
    version_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version_desc.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.maxRenderSize = {rw, rh};
    upscale.maxUpscaleSize = {ow, oh};
    upscale.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    upscale.fpMessage = messageCallback;
    upscale.header.pNext = &version_desc.header;
    version_desc.header.pNext = &backend.header;

    ffxContext context = nullptr;
    rc = create(&context, &upscale.header, nullptr);
    std::printf(
        "ffxCreateContext(UPSCALE %ux%u -> %ux%u legacy ABI flags=0x%x) -> 0x%x (OK)\n",
        rw, rh, ow, oh, upscale.flags, rc);
    if (rc != FFX_API_RETURN_OK || !context) {
        if (rc == FFX_API_RETURN_ERROR_RUNTIME_ERROR)
            std::fprintf(stderr,
                         "context creation failed; check FSR4_ASSET_DIR contains "
                         "shaders/ and models/FP8_standard.bin\n");
        return 1;
    }

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    rc = query(&context, &provider.header);
    std::printf("ffxQuery(provider) -> 0x%x id=0x%llx name=%s\n", rc,
                (unsigned long long)provider.versionId,
                provider.versionName ? provider.versionName : "(null)");
    if (rc != FFX_API_RETURN_OK || !provider.versionName ||
        std::strcmp(provider.versionName, "FSR 4.1.2") != 0)
        return 1;

    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = cb;
    desc.color = ffxApiGetResourceVK(
        color_img.image,
        ffxApiGetImageResourceDescriptionVK(color_img.image, color_img.create, 0),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth = ffxApiGetResourceVK(
        depth_img.image,
        ffxApiGetImageResourceDescriptionVK(depth_img.image, depth_img.create, 0),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors = ffxApiGetResourceVK(
        motion_img.image,
        ffxApiGetImageResourceDescriptionVK(motion_img.image, motion_img.create, 0),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.output = ffxApiGetResourceVK(
        output_img.image,
        ffxApiGetImageResourceDescriptionVK(output_img.image, output_img.create, 0),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.renderSize = {rw, rh};
    desc.upscaleSize = {ow, oh};
    desc.motionVectorScale = {static_cast<float>(rw), static_cast<float>(rh)};
    desc.jitterOffset = {0.0f, 0.0f};
    desc.enableSharpening = false;
    desc.sharpness = 0.0f;
    desc.frameTimeDelta = 16.7f;
    desc.preExposure = 1.0f;
    desc.reset = true;
    desc.cameraNear = 0.1f;
    desc.cameraFar = 1000.0f;
    desc.cameraFovAngleVertical = 1.0472f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = 0;
    rc = dispatch(&context, &desc.header);
    std::printf("ffxDispatch(UPSCALE) -> 0x%x (OK)\n", rc);
    if (rc != FFX_API_RETURN_OK) return 1;

    imageBarrier(cb, output_img.image, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
                 VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    const VkDeviceSize out_size = static_cast<VkDeviceSize>(ow) * oh * 4;
    VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sbci.size = out_size;
    sbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    VK_CHECK(vkCreateBuffer(device, &sbci, nullptr, &staging));
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(device, staging, &sreq);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = memoryType(physical, sreq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(device, &sai, nullptr, &staging_mem));
    VK_CHECK(vkBindBufferMemory(device, staging, staging_mem, 0));
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {ow, oh, 1};
    vkCmdCopyImageToBuffer(cb, output_img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging, 1, &copy);
    VK_CHECK(vkEndCommandBuffer(cb));
    VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
    VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    void* out_ptr = nullptr;
    VK_CHECK(vkMapMemory(device, staging_mem, 0, out_size, 0, &out_ptr));
    const uint8_t* px = static_cast<const uint8_t*>(out_ptr);
    size_t nonzero = 0;
    bool seen[256] = {};
    uint8_t lo = 255, hi = 0;
    for (VkDeviceSize i = 0; i < out_size; ++i) {
        if (px[i]) ++nonzero;
        seen[px[i]] = true;
        lo = std::min(lo, px[i]);
        hi = std::max(hi, px[i]);
    }
    size_t unique = 0;
    for (bool s : seen) unique += s ? 1 : 0;
    std::printf("output: %llu bytes, nonzero=%zu, unique=%zu, min=%u, max=%u\n",
                (unsigned long long)out_size, nonzero, unique, lo, hi);
    vkUnmapMemory(device, staging_mem);

    const bool varied = nonzero >= out_size / 2 && unique >= 16;
    if (!varied) {
        std::fprintf(stderr, "output is degenerate (all-zero or uniform)\n");
        return 1;
    }

    rc = destroy(&context, nullptr);
    std::printf("ffxDestroyContext -> 0x%x (OK)\n", rc);
    if (rc != FFX_API_RETURN_OK) return 1;

    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_mem, nullptr);
    vkDestroyBuffer(device, upload, nullptr);
    vkFreeMemory(device, upload_mem, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyImage(device, output_img.image, nullptr);
    vkFreeMemory(device, output_img.memory, nullptr);
    vkDestroyImage(device, motion_img.image, nullptr);
    vkFreeMemory(device, motion_img.memory, nullptr);
    vkDestroyImage(device, depth_img.image, nullptr);
    vkFreeMemory(device, depth_img.memory, nullptr);
    vkDestroyImage(device, color_img.image, nullptr);
    vkFreeMemory(device, color_img.memory, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    std::printf("RESULT: OK\n");
    return 0;
}
