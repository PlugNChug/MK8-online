// Headless Vulkan validation for the FSR upscaler provider.
//
// Creates a compute-capable Vulkan device, asks the provider for a 1920x1080 -> 2560x1440 upscale context (the
// size and flags the game bridge uses), dispatches one frame from a procedural input, and reads the output back.
// Context creation builds every upscaler pipeline from the generated shader blobs; a blob/layout mismatch fails
// here. The readback proves the dispatch actually wrote an image.
//
// Usage: test_context <lib> [render_w] [render_h] [upscale_w] [upscale_h] [flags]
//        [--provider NAME] [--override-id HEX] [--out PATH] [--debug] [--no-device-features]
//
// --provider selects a version through ffxQuery + ffxOverrideVersion exactly like runtime/mk8/graphics/fsr.cpp.
// --override-id forces an unadvertised provider id (diagnostic) and --out writes <PATH>.rgba (raw RGBA8) and
// <PATH>.ppm for inspection. --debug enables the provider message callback at verbose level.
//
// The generated storage images declare format Unknown (the SDK creates R8/R16 images), and two SPD shaders use
// VK_KHR_compute_shader_derivatives. A real device must enable those features; --no-device-features creates the
// device exactly like the unpatched game does, which is the diagnostic for a runtime that has not enabled them.
#include "ffx_api.h"
#include "vk/ffx_api_vk.h"
#include "ffx_upscale.h"
#include <vulkan/vulkan.h>
#include "platform.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <vector>

typedef ffxReturnCode_t (*PfnCreate)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxQueryDescHeader*);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext*, const ffxDispatchDescHeader*);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext*, const ffxConfigureDescHeader*);

static void ffx_debug_message(uint32_t type, const wchar_t* text)
{
    fwprintf(stdout, L"[ffx %u] %ls\n", type, text ? text : L"(null)");
    fflush(stdout);
}

static const char* rc_name(ffxReturnCode_t rc)
{
    switch (rc)
    {
    case FFX_API_RETURN_OK: return "OK";
    case FFX_API_RETURN_ERROR: return "ERROR";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
    case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
    case FFX_API_RETURN_ERROR_MEMORY: return "MEMORY";
    case FFX_API_RETURN_ERROR_PARAMETER: return "PARAMETER";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
    default: return "?";
    }
}

static uint32_t memory_type(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags wanted)
{
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted)
            return i;
    return 0;
}

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
};

static Image make_image(VkDevice device, VkPhysicalDevice gpu, VkFormat format, uint32_t w, uint32_t h,
                        VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    Image img;
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
    if (vkCreateImage(device, &img.create, nullptr, &img.image) != VK_SUCCESS) { printf("vkCreateImage failed\n"); return {}; }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags src_stage,
                    VkPipelineStageFlags dst_stage, VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

int main(int argc, char** argv)
{
    const char* lib = "build/libamd_fidelityfx_upscaler_vk.so";
    const char* provider_name = nullptr;
    const char* out_path = nullptr;
    uint64_t forced_id = 0;
    uint32_t rw = 1920, rh = 1080, ow = 2560, oh = 1440;
    uint32_t flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;  // what runtime/mk8/graphics/fsr.cpp passes
    bool device_features = true;
    bool debug_messages = false;
    int positional = 0;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--no-device-features") == 0) { device_features = false; continue; }
        if (strcmp(argv[i], "--debug") == 0) { debug_messages = true; continue; }
        if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) { provider_name = argv[++i]; continue; }
        if (strcmp(argv[i], "--override-id") == 0 && i + 1 < argc) { forced_id = strtoull(argv[++i], nullptr, 0); continue; }
        if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) { out_path = argv[++i]; continue; }
        switch (positional++)
        {
        case 0: lib = argv[i]; break;
        case 1: rw = (uint32_t)atoi(argv[i]); break;
        case 2: rh = (uint32_t)atoi(argv[i]); break;
        case 3: ow = (uint32_t)atoi(argv[i]); break;
        case 4: oh = (uint32_t)atoi(argv[i]); break;
        case 5: flags = (uint32_t)strtoul(argv[i], nullptr, 0); break;
        default: break;
        }
    }

    void* h = mk8_lib_open(lib);
    if (!h) { printf("load: %s\n", mk8_lib_error()); return 1; }
    PfnCreate create = (PfnCreate)mk8_lib_symbol(h, "ffxCreateContext");
    PfnDestroy destroy = (PfnDestroy)mk8_lib_symbol(h, "ffxDestroyContext");
    PfnQuery query = (PfnQuery)mk8_lib_symbol(h, "ffxQuery");
    PfnDispatch dispatch = (PfnDispatch)mk8_lib_symbol(h, "ffxDispatch");
    if (!create || !destroy || !query || !dispatch) { printf("missing exports\n"); return 1; }
    if (debug_messages)
    {
        PfnConfigure configure = (PfnConfigure)mk8_lib_symbol(h, "ffxConfigure");
        if (configure)
        {
            ffxConfigureDescGlobalDebug gd{};
            gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
            gd.effectId = FFX_API_EFFECT_ID_UPSCALE;
            gd.fpMessage = ffx_debug_message;
            gd.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
            ffxReturnCode_t crc = configure(nullptr, &gd.header);
            printf("ffxConfigure(GLOBALDEBUG) rc=%d (%s)\n", (int)crc, rc_name(crc));
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;  // the game creates a Vulkan 1.3 device
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) { printf("vkCreateInstance failed\n"); return 1; }

    uint32_t gpu_count = 0;
    vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    if (!gpu_count) { printf("no Vulkan devices\n"); return 1; }
    std::vector<VkPhysicalDevice> gpus(gpu_count);
    vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.data());
    VkPhysicalDevice gpu = gpus[0];
    for (VkPhysicalDevice candidate : gpus)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(candidate, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { gpu = candidate; break; }
    }
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("device: %s (vendor=0x%04x device=0x%04x api %u.%u.%u)\n", props.deviceName, props.vendorID,
           props.deviceID, VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &family_count, families.data());
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i)
        if (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = i; break; }
    if (family == UINT32_MAX) { printf("no compute queue\n"); return 1; }

    VkPhysicalDeviceFeatures available{};
    vkGetPhysicalDeviceFeatures(gpu, &available);
    bool format_write = device_features && available.shaderStorageImageWriteWithoutFormat;
    bool format_read = device_features && available.shaderStorageImageReadWithoutFormat;
    printf("formatless storage images: write=%d read=%d (%s)\n", (int)format_write, (int)format_read,
           device_features ? "requested" : "not requested (game-like device)");

    uint32_t ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    std::vector<VkExtensionProperties> exts(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, exts.data());
    bool have_derivatives = false;
    for (const auto& e : exts)
        if (strcmp(e.extensionName, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0) have_derivatives = true;
    bool derivatives = device_features && have_derivatives;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR deriv_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
    if (derivatives)
    {
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &deriv_features};
        vkGetPhysicalDeviceFeatures2(gpu, &q);
        derivatives = deriv_features.computeDerivativeGroupLinear;
    }
    printf("compute shader derivatives: %d\n", (int)derivatives);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceFeatures features{};
    features.shaderStorageImageWriteWithoutFormat = format_write;
    features.shaderStorageImageReadWithoutFormat = format_read;
    const char* enabled_exts[1] = {VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME};
    deriv_features.pNext = nullptr;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features;
    if (derivatives)
    {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = enabled_exts;
        dci.pNext = &deriv_features;
    }
    VkDevice device = VK_NULL_HANDLE;
    if (vkCreateDevice(gpu, &dci, nullptr, &device) != VK_SUCCESS) { printf("vkCreateDevice failed\n"); return 1; }
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(device, family, 0, &queue);

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = device;
    backend.deviceContext.vkPhysicalDevice = gpu;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    ffxCreateContextDescUpscale upscale{};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.flags = flags;
    upscale.maxRenderSize = {rw, rh};
    upscale.maxUpscaleSize = {ow, oh};

    // Mirror runtime/mk8/graphics/fsr.cpp: the version descriptor carries the API version this call site was
    // built against, and a named provider version is selected with the 64-bit ffxOverrideVersion id from
    // ffxQuery. AMD's signed Windows DLL lists 4.1.2, 3.1.6 and 2.3.4 on RDNA 4, but only 3.1.6 and 2.3.4 on
    // other GPUs; the Linux provider only 3.1.6.
    ffxCreateContextDescUpscaleVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion override_version{};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    upscale.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    if (provider_name || forced_id)
    {
        if (forced_id)
        {
            // A version the provider did not advertise. Used to capture the exact rejection on hardware the
            // provider filters out (for example FSR 4.1.2 on a non-AMD GPU).
            override_version.versionId = forced_id;
            printf("forcing unadvertised version id 0x%llx\n", (unsigned long long)forced_id);
        }
        else
        {
            uint64_t count = 0;
            ffxQueryDescGetVersions versions{};
            versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            versions.device = &backend.deviceContext;
            versions.outputCount = &count;
            ffxReturnCode_t vrc = query(nullptr, &versions.header);
            printf("version query rc=%d count=%llu\n", (int)vrc, (unsigned long long)count);
            if (vrc == FFX_API_RETURN_OK && count > 0 && count <= 16)
            {
                uint64_t ids[16] = {0};
                const char* names[16] = {nullptr};
                uint64_t cap = count;
                versions.outputCount = &cap;
                versions.versionIds = ids;
                versions.versionNames = names;
                vrc = query(nullptr, &versions.header);
                for (uint64_t i = 0; i < cap; ++i)
                {
                    printf("version[%llu] id=0x%016llx name=%s\n", (unsigned long long)i,
                           (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
                    if (names[i] && strcmp(names[i], provider_name) == 0) override_version.versionId = ids[i];
                }
            }
            if (!override_version.versionId)
            {
                printf("provider does not offer version %s\n", provider_name);
                mk8_lib_close(h);
                return 2;
            }
        }
        printf("selecting version %s (0x%llx)\n", provider_name ? provider_name : "forced",
               (unsigned long long)override_version.versionId);
        version.header.pNext = &override_version.header;
        override_version.header.pNext = &backend.header;
    }

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = create(&ctx, &upscale.header, nullptr);
    printf("ffxCreateContext(UPSCALE %ux%u -> %ux%u flags=0x%x) rc=%d (%s) ctx=%p\n",
           rw, rh, ow, oh, upscale.flags, (int)rc, rc_name(rc), (void*)ctx);
    if (rc != FFX_API_RETURN_OK || !ctx) return 2;

    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    rc = query(&ctx, &provider.header);
    printf("ffxQuery(provider) rc=%d id=0x%llx name=%s\n", (int)rc,
           (unsigned long long)provider.versionId, provider.versionName ? provider.versionName : "(null)");
    bool provider_ok = rc == FFX_API_RETURN_OK && provider.versionName &&
                       (provider_name ? strcmp(provider.versionName, provider_name) == 0 : true);

    // Inputs at render size, output at display size.
    Image color = make_image(device, gpu, VK_FORMAT_R8G8B8A8_UNORM, rw, rh,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    Image depth = make_image(device, gpu, VK_FORMAT_D32_SFLOAT, rw, rh,
                             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    Image motion = make_image(device, gpu, VK_FORMAT_R16G16_SFLOAT, rw, rh,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    Image output = make_image(device, gpu, VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Upload a gradient so a successful upscale has varied, non-zero pixels.
    std::vector<uint8_t> pixels((size_t)rw * rh * 4);
    for (uint32_t y = 0; y < rh; ++y)
        for (uint32_t x = 0; x < rw; ++x)
        {
            uint8_t* p = &pixels[((size_t)y * rw + x) * 4];
            p[0] = (uint8_t)(x * 255u / (rw - 1));
            p[1] = (uint8_t)(y * 255u / (rh - 1));
            p[2] = 128;
            p[3] = 255;
        }
    std::vector<float> depths((size_t)rw * rh, 0.5f);
    std::vector<uint16_t> mvs((size_t)rw * rh * 2, 0);

    const VkDeviceSize color_size = (VkDeviceSize)rw * rh * 4;
    const VkDeviceSize depth_size = (VkDeviceSize)rw * rh * 4;
    const VkDeviceSize mv_size = (VkDeviceSize)rw * rh * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = color_size + depth_size + mv_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer upload = VK_NULL_HANDLE;
    VkDeviceMemory upload_mem = VK_NULL_HANDLE;
    vkCreateBuffer(device, &bci, nullptr, &upload);
    VkMemoryRequirements ureq{};
    vkGetBufferMemoryRequirements(device, upload, &ureq);
    VkMemoryAllocateInfo uai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    uai.allocationSize = ureq.size;
    uai.memoryTypeIndex = memory_type(gpu, ureq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &uai, nullptr, &upload_mem);
    vkBindBufferMemory(device, upload, upload_mem, 0);
    void* ptr = nullptr;
    vkMapMemory(device, upload_mem, 0, bci.size, 0, &ptr);
    memcpy(ptr, pixels.data(), pixels.size());
    memcpy((uint8_t*)ptr + color_size, depths.data(), depths.size() * sizeof(float));
    memcpy((uint8_t*)ptr + color_size + depth_size, mvs.data(), mvs.size() * sizeof(uint16_t));
    vkUnmapMemory(device, upload_mem);

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(device, &pci, nullptr, &pool);
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device, &cbai, &cb);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &begin);
    barrier(cb, color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(cb, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(cb, motion.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {rw, rh, 1};
    vkCmdCopyBufferToImage(cb, upload, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.bufferOffset = color_size;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyBufferToImage(cb, upload, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.bufferOffset = color_size + depth_size;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cb, upload, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(cb, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(cb, motion.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    barrier(cb, output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
            VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    vkCreateFence(device, &fci, nullptr, &fence);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    vkQueueSubmit(queue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
    vkResetFences(device, 1, &fence);

    vkBeginCommandBuffer(cb, &begin);
    ffxDispatchDescUpscale desc{};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = cb;
    desc.color = ffxApiGetResourceVK(color.image,
        ffxApiGetImageResourceDescriptionVK(color.image, color.create, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.depth = ffxApiGetResourceVK(depth.image,
        ffxApiGetImageResourceDescriptionVK(depth.image, depth.create, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.motionVectors = ffxApiGetResourceVK(motion.image,
        ffxApiGetImageResourceDescriptionVK(motion.image, motion.create, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_COMPUTE_READ);
    desc.output = ffxApiGetResourceVK(output.image,
        ffxApiGetImageResourceDescriptionVK(output.image, output.create, FFX_API_RESOURCE_USAGE_UAV),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.renderSize = {rw, rh};
    desc.upscaleSize = {ow, oh};
    desc.motionVectorScale = {(float)rw, (float)rh};
    desc.jitterOffset = {0.0f, 0.0f};
    desc.enableSharpening = true;
    desc.sharpness = 0.2f;
    desc.frameTimeDelta = 16.7f;
    desc.preExposure = 1.0f;
    desc.reset = true;
    desc.cameraNear = 0.1f;
    desc.cameraFar = 1000.0f;
    desc.cameraFovAngleVertical = 1.0472f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    rc = dispatch(&ctx, &desc.header);
    printf("ffxDispatch(UPSCALE) rc=%d (%s)\n", (int)rc, rc_name(rc));
    if (rc != FFX_API_RETURN_OK)
    {
        destroy(&ctx, nullptr);
        printf("RESULT: FAIL\n");
        return 3;
    }
    barrier(cb, output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

    const VkDeviceSize out_size = (VkDeviceSize)ow * oh * 4;
    VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sbci.size = out_size;
    sbci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    vkCreateBuffer(device, &sbci, nullptr, &staging);
    VkMemoryRequirements sreq{};
    vkGetBufferMemoryRequirements(device, staging, &sreq);
    VkMemoryAllocateInfo sai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = memory_type(gpu, sreq.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device, &sai, nullptr, &staging_mem);
    vkBindBufferMemory(device, staging, staging_mem, 0);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {ow, oh, 1};
    vkCmdCopyImageToBuffer(cb, output.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    vkEndCommandBuffer(cb);
    vkQueueSubmit(queue, 1, &submit, fence);
    vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);

    void* out_ptr = nullptr;
    vkMapMemory(device, staging_mem, 0, out_size, 0, &out_ptr);
    size_t nonzero = 0;
    unsigned long long sum = 0;
    uint8_t first = ((const uint8_t*)out_ptr)[0];
    bool varied = false;
    for (VkDeviceSize i = 0; i < out_size; ++i)
    {
        uint8_t v = ((const uint8_t*)out_ptr)[i];
        sum += v;
        if (v) ++nonzero;
        if (v != first) varied = true;
    }
    printf("output: %zu/%llu non-zero, sum=%llu, varied=%d\n", nonzero, (unsigned long long)out_size, sum, (int)varied);

    // Write the upscaled image for visual inspection: raw RGBA8 plus a P6 PPM the caller can convert to PNG.
    if (out_path)
    {
        char path[1024];
        snprintf(path, sizeof(path), "%s.rgba", out_path);
        if (FILE* f = fopen(path, "wb"))
        {
            fwrite(out_ptr, 1, (size_t)out_size, f);
            fclose(f);
            printf("wrote %s (%llu bytes)\n", path, (unsigned long long)out_size);
        }
        else printf("cannot write %s\n", path);
        snprintf(path, sizeof(path), "%s.ppm", out_path);
        if (FILE* f = fopen(path, "wb"))
        {
            fprintf(f, "P6\n%u %u\n255\n", ow, oh);
            const uint8_t* p = (const uint8_t*)out_ptr;
            for (uint32_t y = 0; y < oh; ++y)
                for (uint32_t x = 0; x < ow; ++x)
                {
                    const uint8_t* px = p + ((size_t)y * ow + x) * 4;
                    fwrite(px, 1, 3, f);
                }
            fclose(f);
            printf("wrote %s\n", path);
        }
        else printf("cannot write %s\n", path);
    }
    vkUnmapMemory(device, staging_mem);

    ffxReturnCode_t drc = destroy(&ctx, nullptr);
    printf("ffxDestroyContext rc=%d (%s)\n", (int)drc, rc_name(drc));

    vkDeviceWaitIdle(device);
    for (Image* img : {&color, &depth, &motion, &output})
    {
        if (img->view) vkDestroyImageView(device, img->view, nullptr);
        if (img->image) vkDestroyImage(device, img->image, nullptr);
        if (img->memory) vkFreeMemory(device, img->memory, nullptr);
    }
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, staging_mem, nullptr);
    vkDestroyBuffer(device, upload, nullptr);
    vkFreeMemory(device, upload_mem, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    mk8_lib_close(h);

    bool ok = provider_ok && drc == FFX_API_RETURN_OK && nonzero > out_size / 2 && varied;
    printf("RESULT: %s\n", ok ? "OK" : "FAIL");
    return ok ? 0 : 4;
}
