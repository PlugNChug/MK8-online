// End-to-end test for the native FFX provider through the loader.
//
// Creates a Vulkan device, loads libamd_fidelityfx_loader_vk.so, creates an FSR
// 4.1.2 context, dispatches one frame from the captured reference inputs and
// writes out_provider.rgba8 for comparison.
//
// Usage:
//   provider_test <trace-dir> <loader.so> <rw> <rh> <ow> <oh> <flags> <sharpen>
//                 <sharpness> [dispatch_flags]
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

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
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

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    const std::streamsize size = in.tellg();
    in.seekg(0);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (size) in.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

void writeFile(const std::string& path, const void* data, size_t size) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
}

struct Img {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

uint32_t memoryType(VkPhysicalDevice physical, uint32_t bits,
                    VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    return 0;
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
    vkAllocateMemory(device, &ai, nullptr, &img.memory);
    vkBindImageMemory(device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(device, &vi, nullptr, &img.view);
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

void messageCallback(uint32_t type, const wchar_t* message) {
    std::printf("[ffx] type=%u %ls\n", type, message ? message : L"(null)");
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 10 && argc != 11) {
        std::fprintf(stderr,
                     "usage: %s <trace-dir> <loader.so> <rw> <rh> <ow> <oh> <flags> "
                     "<sharpen> <sharpness> [dispatch_flags]\n",
                     argv[0]);
        return 2;
    }
    const std::string trace_dir = argv[1];
    const std::string loader_path = argv[2];
    const uint32_t rw = static_cast<uint32_t>(std::atoi(argv[3]));
    const uint32_t rh = static_cast<uint32_t>(std::atoi(argv[4]));
    const uint32_t ow = static_cast<uint32_t>(std::atoi(argv[5]));
    const uint32_t oh = static_cast<uint32_t>(std::atoi(argv[6]));
    const uint32_t flags = static_cast<uint32_t>(std::strtoul(argv[7], nullptr, 0));
    const bool sharpen = std::atoi(argv[8]) != 0;
    const float sharpness = std::strtof(argv[9], nullptr);
    const uint32_t dispatch_flags =
        argc == 11 ? static_cast<uint32_t>(std::strtoul(argv[10], nullptr, 0)) : 0;

    const std::vector<uint8_t> color = readFile(trace_dir + "/in_color.rgba8");
    const std::vector<uint8_t> depth = readFile(trace_dir + "/in_depth.f32");
    const std::vector<uint8_t> motion = readFile(trace_dir + "/in_mv.rg16f");

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr4-provider-test";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            physical = d;
            break;
        }
    }
    if (!physical) physical = devices[0];

    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceCooperativeMatrix2FeaturesNV cm2{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
    core.pNext = &f11;
    f11.pNext = &f12;
    f12.pNext = &f13;
    f13.pNext = &cm;
    cm.pNext = &f8;
    f8.pNext = &cm2;
    vkGetPhysicalDeviceFeatures2(physical, &core);

    std::vector<const char*> extensions = {VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                                           VK_EXT_SHADER_FLOAT8_EXTENSION_NAME};

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    dci.pNext = &core;
    VkDevice device = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, 0, 0, &queue);

    const VkImageUsageFlags in_usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    Img color_img = makeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, rw, rh, in_usage,
                              VK_IMAGE_ASPECT_COLOR_BIT);
    Img depth_img = makeImage(device, physical, VK_FORMAT_D32_SFLOAT, rw, rh, in_usage,
                              VK_IMAGE_ASPECT_DEPTH_BIT);
    Img motion_img = makeImage(device, physical, VK_FORMAT_R16G16_SFLOAT, rw, rh, in_usage,
                               VK_IMAGE_ASPECT_COLOR_BIT);
    Img output_img = makeImage(device, physical, VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT);

    // Upload inputs.
    const VkDeviceSize cb_size = static_cast<VkDeviceSize>(rw) * rh * 4;
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
    uai.memoryTypeIndex = memoryType(physical, ureq.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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

    // Upload + transition.
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

    // Load the loader and create the context.
    void* loader = dlopen(loader_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        std::fprintf(stderr, "dlopen %s failed: %s\n", loader_path.c_str(), dlerror());
        return 1;
    }
    auto create = reinterpret_cast<PfnFfxCreateContext>(dlsym(loader, "ffxCreateContext"));
    auto destroy = reinterpret_cast<PfnFfxDestroyContext>(dlsym(loader, "ffxDestroyContext"));
    auto dispatch = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*,
                                                         const ffxDispatchDescHeader*)>(
        dlsym(loader, "ffxDispatch"));
    auto query = reinterpret_cast<ffxReturnCode_t (*)(ffxContext*, ffxQueryDescHeader*)>(
        dlsym(loader, "ffxQuery"));
    if (!create || !destroy || !dispatch || !query) {
        std::fprintf(stderr, "loader is missing FFX exports\n");
        return 1;
    }

    VkDeviceContext device_context{};
    device_context.vkDevice = device;
    device_context.vkPhysicalDevice = physical;
    device_context.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = device_context;

    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.maxRenderSize = {rw, rh};
    upscale.maxUpscaleSize = {ow, oh};
    upscale.flags = flags;
    upscale.fpMessage = messageCallback;
    upscale.header.pNext = &version.header;
    version.header.pNext = &backend.header;

    ffxContext context = nullptr;
    ffxReturnCode_t rc = create(&context, &upscale.header, nullptr);
    std::printf("ffxCreateContext -> 0x%x\n", rc);
    if (rc != FFX_API_RETURN_OK || !context) return 1;

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    rc = query(&context, &provider.header);
    std::printf("provider query -> 0x%x id=0x%llx name=%s\n", rc,
                (unsigned long long)provider.versionId,
                provider.versionName ? provider.versionName : "(null)");

    // Query parity checks.
    {
        int failures = 0;
        auto check = [&](bool ok, const char* what) {
            if (!ok) {
                std::fprintf(stderr, "query mismatch: %s\n", what);
                ++failures;
            }
        };
        const float expected_ratios[5] = {1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
        for (uint32_t mode = 0; mode < 5; ++mode) {
            float ratio = 0.0f;
            ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode q{};
            q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE;
            q.qualityMode = mode;
            q.pOutUpscaleRatio = &ratio;
            check(query(&context, &q.header) == FFX_API_RETURN_OK, "ratio rc");
            check(ratio == expected_ratios[mode], "ratio value");
        }
        {
            uint32_t rw = 0, rh = 0;
            ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
            q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
            q.displayWidth = 1920;
            q.displayHeight = 1080;
            q.qualityMode = FFX_UPSCALE_QUALITY_MODE_QUALITY;
            q.pOutRenderWidth = &rw;
            q.pOutRenderHeight = &rh;
            check(query(&context, &q.header) == FFX_API_RETURN_OK, "render res rc");
            check(rw == 1280 && rh == 720, "render res 1.5x");
        }
        {
            int32_t phases = 0;
            ffxQueryDescUpscaleGetJitterPhaseCount q{};
            q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
            q.renderWidth = 1280;
            q.displayWidth = 1920;
            q.pOutPhaseCount = &phases;
            check(query(&context, &q.header) == FFX_API_RETURN_OK, "phase rc");
            check(phases == 18, "phase count 1.5x");
            float x = 0.0f, y = 0.0f;
            ffxQueryDescUpscaleGetJitterOffset j{};
            j.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET;
            j.index = 1;
            j.phaseCount = phases;
            j.pOutX = &x;
            j.pOutY = &y;
            check(query(&context, &j.header) == FFX_API_RETURN_OK, "jitter rc");
            check(x == -0.25f && std::abs(y - 0.1666667f) < 1e-5f, "jitter offset");
        }
        FfxApiEffectMemoryUsage usage{};
        ffxQueryDescUpscaleGetGPUMemoryUsageV2 mem{};
        mem.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2;
        mem.gpuMemoryUsageUpscaler = &usage;
        check(query(&context, &mem.header) == FFX_API_RETURN_OK, "memory rc");
        check(usage.totalUsageInBytes > 0, "memory total");
        ffxQueryDescUpscaleGetResourceRequirements req{};
        req.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS;
        check(query(&context, &req.header) == FFX_API_RETURN_OK, "resources rc");
        check((req.required_resources & FFX_API_QUERY_RESOURCE_INPUT_COLOR) != 0,
              "color required");
        std::printf("queries: ratio/resolution/phase/jitter ok, gpu memory %llu bytes, "
                    "required resources 0x%llx\n",
                    (unsigned long long)usage.totalUsageInBytes,
                    (unsigned long long)req.required_resources);
        if (failures) return 1;
    }

    // Dispatch.
    VK_CHECK(vkBeginCommandBuffer(cb, &begin));
    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = cb;
    desc.color = ffxApiGetResourceVK(
        color_img.image,
        ffxApiGetImageResourceDescriptionVK(color_img.image, color_img.create,
                                            VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    desc.depth = ffxApiGetResourceVK(
        depth_img.image,
        ffxApiGetImageResourceDescriptionVK(depth_img.image, depth_img.create,
                                            VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    desc.motionVectors = ffxApiGetResourceVK(
        motion_img.image,
        ffxApiGetImageResourceDescriptionVK(motion_img.image, motion_img.create,
                                            VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    desc.output = ffxApiGetResourceVK(
        output_img.image,
        ffxApiGetImageResourceDescriptionVK(output_img.image, output_img.create,
                                            VK_IMAGE_USAGE_STORAGE_BIT),
        VK_IMAGE_LAYOUT_GENERAL);
    desc.renderSize = {rw, rh};
    desc.upscaleSize = {ow, oh};
    desc.motionVectorScale = {static_cast<float>(rw), static_cast<float>(rh)};
    desc.jitterOffset = {0.0f, 0.0f};
    desc.enableSharpening = sharpen;
    desc.sharpness = sharpness;
    desc.frameTimeDelta = 16.7f;
    desc.preExposure = 1.0f;
    desc.reset = false;
    desc.cameraNear = 0.1f;
    desc.cameraFar = 1000.0f;
    desc.cameraFovAngleVertical = 1.0472f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = dispatch_flags;
    rc = dispatch(&context, &desc.header);
    std::printf("ffxDispatch -> 0x%x\n", rc);
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
    writeFile("out_provider.rgba8", out_ptr, static_cast<size_t>(out_size));
    unsigned long long sum = 0;
    size_t nonzero = 0;
    const uint8_t* px = static_cast<const uint8_t*>(out_ptr);
    for (VkDeviceSize i = 0; i < out_size; ++i) {
        sum += px[i];
        if (px[i]) ++nonzero;
    }
    std::printf("output: %zu/%llu non-zero, sum=%llu\n", nonzero,
                (unsigned long long)out_size, sum);
    vkUnmapMemory(device, staging_mem);

    // Optional GPU benchmark: time the full FSR4 chain with timestamp queries.
    if (const char* frames_env = std::getenv("FSR4_BENCH_FRAMES")) {
        const int frames = std::max(1, std::atoi(frames_env));
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical, &props);
        const double ns_per_tick = props.limits.timestampPeriod;

        VkQueryPoolCreateInfo qpci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpci.queryCount = static_cast<uint32_t>(frames * 2);
        VkQueryPool query_pool = VK_NULL_HANDLE;
        VK_CHECK(vkCreateQueryPool(device, &qpci, nullptr, &query_pool));

        VK_CHECK(vkBeginCommandBuffer(cb, &begin));
        vkCmdResetQueryPool(cb, query_pool, 0, static_cast<uint32_t>(frames * 2));
        double cpu_total_ms = 0.0;
        for (int i = 0; i < frames; ++i) {
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool,
                                static_cast<uint32_t>(i * 2));
            const auto t0 = std::chrono::steady_clock::now();
            const ffxReturnCode_t brc = dispatch(&context, &desc.header);
            const auto t1 = std::chrono::steady_clock::now();
            if (brc != FFX_API_RETURN_OK) {
                std::fprintf(stderr, "bench dispatch failed (0x%x)\n", brc);
                return 1;
            }
            cpu_total_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, query_pool,
                                static_cast<uint32_t>(i * 2 + 1));
        }
        VK_CHECK(vkEndCommandBuffer(cb));
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, fence));
        VK_CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

        std::vector<uint64_t> timestamps(static_cast<size_t>(frames) * 2);
        VK_CHECK(vkGetQueryPoolResults(device, query_pool, 0,
                                       static_cast<uint32_t>(timestamps.size()),
                                       timestamps.size() * sizeof(uint64_t), timestamps.data(),
                                       sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        double total_ms = 0.0;
        double min_ms = 1e9, max_ms = 0.0;
        for (int i = 0; i < frames; ++i) {
            const double ms = double(timestamps[i * 2 + 1] - timestamps[i * 2]) *
                              ns_per_tick / 1e6;
            total_ms += ms;
            min_ms = std::min(min_ms, ms);
            max_ms = std::max(max_ms, ms);
        }
        std::printf("bench: %d frames, GPU %.3f ms avg / %.3f min / %.3f max, "
                    "CPU record %.3f ms/frame (%.3f ms total)\n",
                    frames, total_ms / frames, min_ms, max_ms, cpu_total_ms / frames,
                    cpu_total_ms);
        vkDestroyQueryPool(device, query_pool, nullptr);
    }

    destroy(&context, nullptr);
    std::printf("context destroyed\n");
    return 0;
}
