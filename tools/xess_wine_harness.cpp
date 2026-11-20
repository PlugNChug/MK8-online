// Standalone Windows/Wine XeSS SR dispatch harness.
//
// This is evidence tooling, not runtime code. It loads the official libxess.dll
// through the same mk8_xess_probe() used by the runtime, creates a Vulkan 1.3
// device with the extensions/features XeSS asks for, initialises SR for
// 1280x720 -> 2560x1440 (PERFORMANCE), records xessVKExecute twice (reset then
// a shifted frame), submits, waits and reads back the output.
//
// It writes frame0.rgba / frame1.rgba (tightly packed RGBA8) into argv[1] and
// prints machine-readable stats. A negative execute result is reported verbatim;
// no output is treated as success. It also prints xessGetIntelXeFXVersion, which is
// 0.0.0 when the Intel XMX library is not loaded (the DP4a-class path).
//
// Build (Linux cross-compile):
//   x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -DMK8_XESS_PROBE_ONLY \
//     -I <vulkan headers> -I runtime/mk8/graphics \
//     tools/xess_wine_harness.cpp runtime/mk8/graphics/xess.cpp -o xess_harness.exe
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "xess.h"

#define WIDTH 2560
#define HEIGHT 1440
#define IN_WIDTH 1280
#define IN_HEIGHT 720

typedef int (*CreateCtxFn)(VkInstance, VkPhysicalDevice, VkDevice, Mk8XessContextHandle *);
typedef int (*InitCtxFn)(Mk8XessContextHandle, const Mk8XessInitParams *);
typedef int (*ExecuteFn)(Mk8XessContextHandle, VkCommandBuffer, const Mk8XessExecuteParams *);
typedef int (*DestroyCtxFn)(Mk8XessContextHandle);
typedef int (*ReqDevFn)(VkInstance, VkPhysicalDevice, uint32_t *, const char *const **);
typedef int (*ReqFeatFn)(VkInstance, VkPhysicalDevice, void **);
typedef int (*GetVersionFn)(Mk8XessVersion *);
typedef int (*GetInputResFn)(Mk8XessContextHandle, const Mk8Xess2D *, int, Mk8Xess2D *);
typedef int (*IntelVerFn)(Mk8XessContextHandle, Mk8XessVersion *);

static PFN_vkCreateInstance pCreateInstance;
static PFN_vkGetInstanceProcAddr pGIPA;
static PFN_vkEnumeratePhysicalDevices pEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties pGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties pGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice pCreateDevice;
static PFN_vkDestroyDevice pDestroyDevice;
static PFN_vkDestroyInstance pDestroyInstance;
static PFN_vkCreateImage pCreateImage;
static PFN_vkDestroyImage pDestroyImage;
static PFN_vkGetImageMemoryRequirements pGetImageMemoryRequirements;
static PFN_vkCreateImageView pCreateImageView;
static PFN_vkDestroyImageView pDestroyImageView;
static PFN_vkAllocateMemory pAllocateMemory;
static PFN_vkFreeMemory pFreeMemory;
static PFN_vkBindImageMemory pBindImageMemory;
static PFN_vkCreateBuffer pCreateBuffer;
static PFN_vkDestroyBuffer pDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements pGetBufferMemoryRequirements;
static PFN_vkBindBufferMemory pBindBufferMemory;
static PFN_vkMapMemory pMapMemory;
static PFN_vkUnmapMemory pUnmapMemory;
static PFN_vkCreateCommandPool pCreateCommandPool;
static PFN_vkDestroyCommandPool pDestroyCommandPool;
static PFN_vkAllocateCommandBuffers pAllocateCommandBuffers;
static PFN_vkFreeCommandBuffers pFreeCommandBuffers;
static PFN_vkBeginCommandBuffer pBeginCommandBuffer;
static PFN_vkEndCommandBuffer pEndCommandBuffer;
static PFN_vkCmdPipelineBarrier pCmdPipelineBarrier;
static PFN_vkCmdCopyBufferToImage pCmdCopyBufferToImage;
static PFN_vkCmdCopyImageToBuffer pCmdCopyImageToBuffer;
static PFN_vkQueueSubmit pQueueSubmit;
static PFN_vkQueueWaitIdle pQueueWaitIdle;
static PFN_vkDeviceWaitIdle pDeviceWaitIdle;

static void *sym(HMODULE module, const char *name) { return (void *)GetProcAddress(module, name); }

static int load_vulkan(void) {
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    if (!module) return 0;
    pCreateInstance = (PFN_vkCreateInstance)sym(module, "vkCreateInstance");
    pGIPA = (PFN_vkGetInstanceProcAddr)sym(module, "vkGetInstanceProcAddr");
    return pCreateInstance && pGIPA;
}

static int load_instance(VkInstance instance) {
#define LOAD(fn) p##fn = (PFN_vk##fn)pGIPA(instance, "vk" #fn); if (!p##fn) return 0
    LOAD(EnumeratePhysicalDevices);
    LOAD(GetPhysicalDeviceProperties);
    LOAD(GetPhysicalDeviceQueueFamilyProperties);
    LOAD(GetPhysicalDeviceMemoryProperties);
    LOAD(CreateDevice);
    LOAD(DestroyDevice);
    LOAD(DestroyInstance);
    LOAD(CreateImage);
    LOAD(DestroyImage);
    LOAD(GetImageMemoryRequirements);
    LOAD(CreateImageView);
    LOAD(DestroyImageView);
    LOAD(AllocateMemory);
    LOAD(FreeMemory);
    LOAD(BindImageMemory);
    LOAD(CreateBuffer);
    LOAD(DestroyBuffer);
    LOAD(GetBufferMemoryRequirements);
    LOAD(BindBufferMemory);
    LOAD(MapMemory);
    LOAD(UnmapMemory);
    LOAD(CreateCommandPool);
    LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers);
    LOAD(FreeCommandBuffers);
    LOAD(BeginCommandBuffer);
    LOAD(EndCommandBuffer);
    LOAD(CmdPipelineBarrier);
    LOAD(CmdCopyBufferToImage);
    LOAD(CmdCopyImageToBuffer);
    LOAD(QueueSubmit);
    LOAD(QueueWaitIdle);
    LOAD(DeviceWaitIdle);
#undef LOAD
    return 1;
}

static uint32_t memory_type(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties properties;
    pGetPhysicalDeviceMemoryProperties(physical, &properties);
    for (uint32_t i = 0; i < properties.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        if ((properties.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

static int make_image(VkPhysicalDevice physical, VkDevice device, VkFormat format, uint32_t width,
                      uint32_t height, VkImageUsageFlags usage, VkImage *image, VkDeviceMemory *memory) {
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
    if (pCreateImage(device, &info, NULL, image) != VK_SUCCESS) return 0;
    VkMemoryRequirements requirements;
    pGetImageMemoryRequirements(device, *image, &requirements);
    uint32_t type = memory_type(physical, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return 0;
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (pAllocateMemory(device, &allocate, NULL, memory) != VK_SUCCESS) return 0;
    return pBindImageMemory(device, *image, *memory, 0) == VK_SUCCESS;
}

static int make_buffer(VkPhysicalDevice physical, VkDevice device, VkDeviceSize size, VkBufferUsageFlags usage,
                       VkBuffer *buffer, VkDeviceMemory *memory, void **mapped) {
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (pCreateBuffer(device, &info, NULL, buffer) != VK_SUCCESS) return 0;
    VkMemoryRequirements requirements;
    pGetBufferMemoryRequirements(device, *buffer, &requirements);
    uint32_t type = memory_type(physical, requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) return 0;
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (pAllocateMemory(device, &allocate, NULL, memory) != VK_SUCCESS) return 0;
    if (pBindBufferMemory(device, *buffer, *memory, 0) != VK_SUCCESS) return 0;
    if (pMapMemory(device, *memory, 0, VK_WHOLE_SIZE, 0, mapped) != VK_SUCCESS) return 0;
    return 1;
}

static VkImageView make_view(VkDevice device, VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (pCreateImageView(device, &info, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

static void barrier(VkCommandBuffer command, VkImage image, VkImageAspectFlags aspect, VkImageLayout old_layout,
                    VkImageLayout new_layout, VkAccessFlags src_access, VkAccessFlags dst_access,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier barrier_info = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier_info.srcAccessMask = src_access;
    barrier_info.dstAccessMask = dst_access;
    barrier_info.oldLayout = old_layout;
    barrier_info.newLayout = new_layout;
    barrier_info.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_info.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_info.image = image;
    barrier_info.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    pCmdPipelineBarrier(command, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier_info);
}

static uint16_t half_from_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t exponent = (int32_t)((bits >> 23) & 0xffu) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffffu;
    if (exponent <= 0) return (uint16_t)sign;
    if (exponent >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | (mantissa >> 13));
}

static void fill_color(uint8_t *pixels, int frame) {
    for (uint32_t y = 0; y < IN_HEIGHT; y++) {
        for (uint32_t x = 0; x < IN_WIDTH; x++) {
            uint32_t sx = (x + (uint32_t)frame * 64u) % IN_WIDTH;
            uint8_t *p = pixels + ((size_t)y * IN_WIDTH + x) * 4;
            p[0] = (uint8_t)(sx * 255u / (IN_WIDTH - 1));
            p[1] = (uint8_t)(y * 255u / (IN_HEIGHT - 1));
            p[2] = (uint8_t)(((sx / 32u) + (y / 32u)) & 1u ? 230 : 20);
            p[3] = 255;
        }
    }
}

static void fill_depth(float *depth) {
    for (uint32_t y = 0; y < IN_HEIGHT; y++)
        for (uint32_t x = 0; x < IN_WIDTH; x++)
            depth[(size_t)y * IN_WIDTH + x] = 0.1f + 0.9f * (float)y / (float)(IN_HEIGHT - 1);
}

static void fill_velocity(uint16_t *velocity, int frame) {
    uint16_t dx = half_from_float(frame ? 64.0f : 0.0f);
    for (size_t i = 0; i < (size_t)IN_WIDTH * IN_HEIGHT; i++) {
        velocity[i * 2 + 0] = dx;
        velocity[i * 2 + 1] = 0;
    }
}

typedef struct Stats {
    uint64_t nonzero;
    uint64_t checksum;
    uint32_t unique;
    uint8_t min_value;
    uint8_t max_value;
} Stats;

static Stats stats_of(const uint8_t *pixels, size_t size) {
    Stats stats = {0, 1469598103934665603ull, 0, 255, 0};
    int seen[256] = {0};
    int any = 0;
    for (size_t i = 0; i < size; i++) {
        uint8_t value = pixels[i];
        if (value) stats.nonzero++;
        if (!seen[value]) {
            seen[value] = 1;
            stats.unique++;
        }
        stats.checksum ^= value;
        stats.checksum *= 1099511628211ull;
        if (!any || value < stats.min_value) stats.min_value = value;
        if (!any || value > stats.max_value) stats.max_value = value;
        any = 1;
    }
    return stats;
}

static int run_frame(VkDevice device, VkQueue queue, VkCommandPool pool, VkPhysicalDevice physical,
                     VkCommandBuffer command, int frame, VkImage color, VkImage depth, VkImage velocity,
                     VkImage output, VkImageView color_view, VkImageView depth_view, VkImageView velocity_view,
                     VkImageView output_view, VkBuffer color_stage, void *color_map, VkBuffer depth_stage,
                     void *depth_map, VkBuffer velocity_stage, void *velocity_map, VkBuffer readback,
                     void *readback_map, Mk8XessContextHandle context, ExecuteFn execute,
                     const char *outdir) {
    VkBufferImageCopy color_copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {IN_WIDTH, IN_HEIGHT, 1}};
    VkBufferImageCopy velocity_copy = color_copy;
    VkBufferImageCopy depth_copy = color_copy;
    depth_copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = pBeginCommandBuffer(command, &begin);
    if (result != VK_SUCCESS) {
        printf("begin_rc=%d\n", (int)result);
        return 0;
    }
    if (frame == 0) {
        barrier(command, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(command, velocity, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(command, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        pCmdCopyBufferToImage(command, color_stage, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &color_copy);
        pCmdCopyBufferToImage(command, velocity_stage, velocity, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                              &velocity_copy);
        pCmdCopyBufferToImage(command, depth_stage, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &depth_copy);
        barrier(command, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, velocity, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    } else {
        barrier(command, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        barrier(command, velocity, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
        pCmdCopyBufferToImage(command, color_stage, color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &color_copy);
        pCmdCopyBufferToImage(command, velocity_stage, velocity, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                              &velocity_copy);
        barrier(command, color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, velocity, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(command, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }

    Mk8XessExecuteParams params = {0};
    params.colorTexture.imageView = color_view;
    params.colorTexture.image = color;
    params.colorTexture.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.colorTexture.format = VK_FORMAT_R8G8B8A8_UNORM;
    params.colorTexture.width = IN_WIDTH;
    params.colorTexture.height = IN_HEIGHT;
    params.velocityTexture.imageView = velocity_view;
    params.velocityTexture.image = velocity;
    params.velocityTexture.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.velocityTexture.format = VK_FORMAT_R16G16_SFLOAT;
    params.velocityTexture.width = IN_WIDTH;
    params.velocityTexture.height = IN_HEIGHT;
    params.depthTexture.imageView = depth_view;
    params.depthTexture.image = depth;
    params.depthTexture.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    params.depthTexture.format = VK_FORMAT_D32_SFLOAT;
    params.depthTexture.width = IN_WIDTH;
    params.depthTexture.height = IN_HEIGHT;
    params.outputTexture.imageView = output_view;
    params.outputTexture.image = output;
    params.outputTexture.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    params.outputTexture.format = VK_FORMAT_R8G8B8A8_UNORM;
    params.outputTexture.width = WIDTH;
    params.outputTexture.height = HEIGHT;
    params.jitterOffsetX = frame ? -0.25f : 0.25f;
    params.jitterOffsetY = frame ? 0.25f : -0.25f;
    params.exposureScale = 1.0f;
    params.resetHistory = frame == 0 ? 1u : 0u;
    params.inputWidth = IN_WIDTH;
    params.inputHeight = IN_HEIGHT;

    int execute_rc = execute(context, command, &params);
    printf("frame%d_execute_rc=%d\n", frame, execute_rc);
    if (execute_rc < 0) return 0;

    barrier(command, output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy read_copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {WIDTH, HEIGHT, 1}};
    pCmdCopyImageToBuffer(command, output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &read_copy);
    result = pEndCommandBuffer(command);
    if (result != VK_SUCCESS) {
        printf("end_rc=%d\n", (int)result);
        return 0;
    }
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    result = pQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    printf("frame%d_submit_rc=%d\n", frame, (int)result);
    if (result != VK_SUCCESS) return 0;
    result = pQueueWaitIdle(queue);
    printf("frame%d_wait_rc=%d\n", frame, (int)result);
    if (result != VK_SUCCESS) return 0;

    const uint8_t *pixels = (const uint8_t *)readback_map;
    size_t bytes = (size_t)WIDTH * HEIGHT * 4;
    Stats stats = stats_of(pixels, bytes);
    printf("frame%d_nonzero=%llu\n", frame, (unsigned long long)stats.nonzero);
    printf("frame%d_unique=%u\n", frame, stats.unique);
    printf("frame%d_min=%u\n", frame, stats.min_value);
    printf("frame%d_max=%u\n", frame, stats.max_value);
    printf("frame%d_checksum=%llu\n", frame, (unsigned long long)stats.checksum);
    char path[1024];
    snprintf(path, sizeof path, "%s/frame%d.rgba", outdir, frame);
    FILE *file = fopen(path, "wb");
    if (!file) {
        printf("write_failed=%s\n", path);
        return 0;
    }
    size_t written = fwrite(pixels, 1, bytes, file);
    fclose(file);
    printf("frame%d_bytes=%llu\n", frame, (unsigned long long)written);
    printf("frame%d_path=%s\n", frame, path);
    return 1;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *outdir = argc > 1 ? argv[1] : ".";
    Mk8XessProbe probe;
    if (!mk8_xess_probe(&probe)) {
        printf("probe_failed=%s\n", probe.reason);
        return 2;
    }
    HMODULE xess = LoadLibraryA(probe.dll);
    if (!xess) {
        printf("xess_load_failed=1\n");
        return 2;
    }
    CreateCtxFn xcreate = (CreateCtxFn)sym(xess, "xessVKCreateContext");
    InitCtxFn xinit = (InitCtxFn)sym(xess, "xessVKInit");
    ExecuteFn xexecute = (ExecuteFn)sym(xess, "xessVKExecute");
    DestroyCtxFn xdestroy = (DestroyCtxFn)sym(xess, "xessDestroyContext");
    ReqDevFn xdev = (ReqDevFn)sym(xess, "xessVKGetRequiredDeviceExtensions");
    ReqFeatFn xfeat = (ReqFeatFn)sym(xess, "xessVKGetRequiredDeviceFeatures");
    GetVersionFn xversion = (GetVersionFn)sym(xess, "xessGetVersion");
    GetInputResFn xinput = (GetInputResFn)sym(xess, "xessGetInputResolution");
    IntelVerFn xintel = (IntelVerFn)sym(xess, "xessGetIntelXeFXVersion");
    if (!xcreate || !xinit || !xexecute || !xdestroy || !xdev || !xfeat) {
        printf("missing_xess_entry=1\n");
        return 2;
    }
    if (!load_vulkan()) {
        printf("no_vulkan=1\n");
        return 2;
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "mk8-xess-dispatch";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo instance_info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult result = pCreateInstance(&instance_info, NULL, &instance);
    printf("vkCreateInstance=%d\n", (int)result);
    if (result != VK_SUCCESS) return 3;
    if (!load_instance(instance)) {
        printf("instance_functions_missing=1\n");
        return 3;
    }
    uint32_t device_count = 0;
    pEnumeratePhysicalDevices(instance, &device_count, NULL);
    VkPhysicalDevice physical_devices[8];
    if (device_count > 8) device_count = 8;
    pEnumeratePhysicalDevices(instance, &device_count, physical_devices);
    if (!device_count) {
        printf("no_physical_device=1\n");
        return 4;
    }
    VkPhysicalDevice physical = physical_devices[0];
    VkPhysicalDeviceProperties properties;
    pGetPhysicalDeviceProperties(physical, &properties);
    printf("device=%s\n", properties.deviceName);
    uint32_t queue_count = 0;
    pGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, NULL);
    VkQueueFamilyProperties queues[16];
    if (queue_count > 16) queue_count = 16;
    pGetPhysicalDeviceQueueFamilyProperties(physical, &queue_count, queues);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < queue_count; i++)
        if (queues[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            family = i;
            break;
        }
    if (family == UINT32_MAX) {
        printf("no_queue=1\n");
        return 5;
    }
    uint32_t extension_count = 0;
    const char *const *extensions = NULL;
    int xess_rc = xdev(instance, physical, &extension_count, &extensions);
    printf("required_extensions_rc=%d\ncount=%u\n", xess_rc, extension_count);
    for (uint32_t i = 0; i < extension_count && i < 16; i++) printf("ext=%s\n", extensions[i]);
    void *features = NULL;
    xess_rc = xfeat(instance, physical, &features);
    printf("required_features_rc=%d\n", xess_rc);
    VkDeviceQueueCreateInfo queue_info = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    float priority = 1.0f;
    queue_info.queueFamilyIndex = family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkDeviceCreateInfo device_info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = extension_count;
    device_info.ppEnabledExtensionNames = extensions;
    device_info.pNext = features;
    VkDevice device = VK_NULL_HANDLE;
    result = pCreateDevice(physical, &device_info, NULL, &device);
    printf("vkCreateDevice=%d\n", (int)result);
    if (result != VK_SUCCESS) return 6;
    VkQueue queue = VK_NULL_HANDLE;
    PFN_vkGetDeviceQueue get_queue = (PFN_vkGetDeviceQueue)pGIPA(instance, "vkGetDeviceQueue");
    if (!get_queue) {
        printf("no_get_device_queue=1\n");
        return 6;
    }
    get_queue(device, family, 0, &queue);

    Mk8XessContextHandle context = NULL;
    xess_rc = xcreate(instance, physical, device, &context);
    printf("xessVKCreateContext=%d\n", xess_rc);
    if (xess_rc < 0 || !context) return 7;
    Mk8Xess2D output_resolution = {WIDTH, HEIGHT};
    Mk8Xess2D input_resolution = {IN_WIDTH, IN_HEIGHT};
    if (xinput) {
        int ic = xinput(context, &output_resolution, MK8_XESS_QUALITY_PERFORMANCE, &input_resolution);
        printf("xessGetInputResolution=%d %ux%u\n", ic, input_resolution.x, input_resolution.y);
    }
    Mk8XessInitParams init = {0};
    init.outputResolution = output_resolution;
    init.qualitySetting = MK8_XESS_QUALITY_PERFORMANCE;
    init.initFlags = MK8_XESS_INIT_FLAG_LDR_INPUT_COLOR;
    init.creationNodeMask = 1;
    init.visibleNodeMask = 1;
    xess_rc = xinit(context, &init);
    printf("xessVKInit=%d\n", xess_rc);
    if (xess_rc < 0) return 7;
    if (xversion) {
        Mk8XessVersion version = {0, 0, 0, 0};
        if (xversion(&version) >= 0)
            printf("xess_version=%u.%u.%u\n", version.major, version.minor, version.patch);
    }
    // XMX is Intel-only: the SDK returns 0.0.0 when the Intel XeFX library is not
    // loaded, which is the non-XMX (DP4a-class) path on NVIDIA/AMD.
    if (xintel) {
        Mk8XessVersion xeFX = {0, 0, 0, 0};
        int intel_rc = xintel(context, &xeFX);
        printf("xess_intel_xefx_rc=%d version=%u.%u.%u\n", intel_rc, xeFX.major, xeFX.minor, xeFX.patch);
    }

    VkImage color = VK_NULL_HANDLE, depth = VK_NULL_HANDLE, velocity = VK_NULL_HANDLE, output = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE, depth_mem = VK_NULL_HANDLE, velocity_mem = VK_NULL_HANDLE,
                   output_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE, depth_view = VK_NULL_HANDLE, velocity_view = VK_NULL_HANDLE,
                output_view = VK_NULL_HANDLE;
    VkBuffer color_stage = VK_NULL_HANDLE, depth_stage = VK_NULL_HANDLE, velocity_stage = VK_NULL_HANDLE,
             readback = VK_NULL_HANDLE;
    VkDeviceMemory color_stage_mem = VK_NULL_HANDLE, depth_stage_mem = VK_NULL_HANDLE,
                   velocity_stage_mem = VK_NULL_HANDLE, readback_mem = VK_NULL_HANDLE;
    void *color_map = NULL, *depth_map = NULL, *velocity_map = NULL, *readback_map = NULL;
    if (!make_image(physical, device, VK_FORMAT_R8G8B8A8_UNORM, IN_WIDTH, IN_HEIGHT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &color, &color_mem) ||
        !make_image(physical, device, VK_FORMAT_D32_SFLOAT, IN_WIDTH, IN_HEIGHT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &depth, &depth_mem) ||
        !make_image(physical, device, VK_FORMAT_R16G16_SFLOAT, IN_WIDTH, IN_HEIGHT,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &velocity, &velocity_mem) ||
        !make_image(physical, device, VK_FORMAT_R8G8B8A8_UNORM, WIDTH, HEIGHT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &output, &output_mem)) {
        printf("image_create_failed=1\n");
        return 8;
    }
    color_view = make_view(device, color, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    depth_view = make_view(device, depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    velocity_view = make_view(device, velocity, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    output_view = make_view(device, output, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!color_view || !depth_view || !velocity_view || !output_view) {
        printf("view_create_failed=1\n");
        return 8;
    }
    if (!make_buffer(physical, device, (VkDeviceSize)IN_WIDTH * IN_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &color_stage, &color_stage_mem, &color_map) ||
        !make_buffer(physical, device, (VkDeviceSize)IN_WIDTH * IN_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &depth_stage, &depth_stage_mem, &depth_map) ||
        !make_buffer(physical, device, (VkDeviceSize)IN_WIDTH * IN_HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &velocity_stage, &velocity_stage_mem, &velocity_map) ||
        !make_buffer(physical, device, (VkDeviceSize)WIDTH * HEIGHT * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &readback, &readback_mem, &readback_map)) {
        printf("buffer_create_failed=1\n");
        return 8;
    }
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (pCreateCommandPool(device, &pool_info, NULL, &pool) != VK_SUCCESS) {
        printf("pool_create_failed=1\n");
        return 8;
    }
    VkCommandBufferAllocateInfo allocate_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = pool;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (pAllocateCommandBuffers(device, &allocate_info, &command) != VK_SUCCESS) {
        printf("command_alloc_failed=1\n");
        return 8;
    }

    int ok = 1;
    for (int frame = 0; frame < 2 && ok; frame++) {
        fill_color((uint8_t *)color_map, frame);
        fill_depth((float *)depth_map);
        fill_velocity((uint16_t *)velocity_map, frame);
        ok = run_frame(device, queue, pool, physical, command, frame, color, depth, velocity, output, color_view,
                       depth_view, velocity_view, output_view, color_stage, color_map, depth_stage, depth_map,
                       velocity_stage, velocity_map, readback, readback_map, context, xexecute, outdir);
    }
    if (ok) printf("dispatch_ok=1\n");
    if (xdestroy) xdestroy(context);
    pDeviceWaitIdle(device);
    if (command) pFreeCommandBuffers(device, pool, 1, &command);
    if (pool) pDestroyCommandPool(device, pool, NULL);
    if (output_view) pDestroyImageView(device, output_view, NULL);
    if (color_view) pDestroyImageView(device, color_view, NULL);
    if (depth_view) pDestroyImageView(device, depth_view, NULL);
    if (velocity_view) pDestroyImageView(device, velocity_view, NULL);
    if (output) pDestroyImage(device, output, NULL);
    if (color) pDestroyImage(device, color, NULL);
    if (depth) pDestroyImage(device, depth, NULL);
    if (velocity) pDestroyImage(device, velocity, NULL);
    if (readback) pDestroyBuffer(device, readback, NULL);
    if (color_stage) pDestroyBuffer(device, color_stage, NULL);
    if (depth_stage) pDestroyBuffer(device, depth_stage, NULL);
    if (velocity_stage) pDestroyBuffer(device, velocity_stage, NULL);
    if (output_mem) pFreeMemory(device, output_mem, NULL);
    if (color_mem) pFreeMemory(device, color_mem, NULL);
    if (depth_mem) pFreeMemory(device, depth_mem, NULL);
    if (velocity_mem) pFreeMemory(device, velocity_mem, NULL);
    if (readback_mem) pFreeMemory(device, readback_mem, NULL);
    if (color_stage_mem) pFreeMemory(device, color_stage_mem, NULL);
    if (depth_stage_mem) pFreeMemory(device, depth_stage_mem, NULL);
    if (velocity_stage_mem) pFreeMemory(device, velocity_stage_mem, NULL);
    pDestroyDevice(device, NULL);
    pDestroyInstance(instance, NULL);
    return ok ? 0 : 9;
}
