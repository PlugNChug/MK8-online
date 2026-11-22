// Native Linux FSR Upscaling 4.1.2 Vulkan provider (FFX API 2.4.0).
//
// Records AMD's 4.1.2 pass chain into the caller's command buffer using the
// SPIR-V extracted from the user's signed DLL and the ABI recovered in Phase 0.
// No Wine, no readbacks, no queue submissions.
//
// SPDX-License-Identifier: GPL-3.0-only

#include <vulkan/vulkan.h>

#if !defined(_WIN32)
// The SDK headers declare FFX_API_ENTRY as __declspec(dllexport); accept it on
// ELF platforms and export through visibility attributes instead.
#define __declspec(x)
#endif

#include "ffx_api.h"
#include "vk/ffx_api_vk.h"
#include "ffx_upscale.h"
#include "dispatch_slots.hpp"

#include "abi/fsr4_412_abi.hpp"
#include "abi/fsr4_412_selector_table.hpp"
#include "provider/asset_index.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kModelPassCount = 27;
constexpr uint32_t kPrepassIndex = 0;
constexpr uint32_t kPostpassIndex = 26;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    VkDeviceSize allocation_size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    VkDeviceSize allocation_size = 0;
};

struct CachedView {
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkImageView view = VK_NULL_HANDLE;
};

struct PassPipeline {
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
};

struct DispatchSlot {
    Buffer tensor_ubo, constants_ubo, spd_ubo, rcas_ubo;
    VkDescriptorSet model_set = VK_NULL_HANDLE;
    VkDescriptorSet spd_set = VK_NULL_HANDLE;
    VkDescriptorSet rcas_set = VK_NULL_HANDLE;
    std::vector<CachedView> views;
};

struct ProviderContext {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;

    uint32_t max_render_w = 0, max_render_h = 0;
    uint32_t max_upscale_w = 0, max_upscale_h = 0;
    uint32_t flags = 0;
    uint32_t bucket = 0;
    uint32_t key = 0;
    std::string asset_root;

    bool first_dispatch = true;
    bool initialized = false;
    bool has_previous_exposure = false;
    float previous_exposure = 0.0f;
    bool has_previous_jitter = false;
    float previous_jitter[2] = {0.0f, 0.0f};

    Image history, reprojected, recurrent, exposure, dummy_exposure;
    Image rcas_input, exposure_atomic, exposure_mip;
    Image neutral_depth, neutral_motion;
    VkSampler sampler = VK_NULL_HANDLE;
    Buffer weights, scratch;
    fsr4_412::SubmissionValues submissions{};
    std::array<DispatchSlot, 3> slots;
    VkSemaphore timeline = VK_NULL_HANDLE;

    VkDescriptorSetLayout model_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout spd_layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout rcas_layout = VK_NULL_HANDLE;
    VkPipelineLayout model_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout spd_pipeline_layout = VK_NULL_HANDLE;
    VkPipelineLayout rcas_pipeline_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::vector<PassPipeline> model_passes;
    PassPipeline spd_pass;
    PassPipeline rcas_pass;
    bool has_spd = false;
    bool has_rcas = false;

    // Descriptor infos must stay alive until vkUpdateDescriptorSets.
    VkDescriptorBufferInfo weight_info{}, scratch_info{}, tensor_info{}, constants_info{};
    VkDescriptorBufferInfo spd_constants_info{}, rcas_constants_info{};
    VkDescriptorImageInfo sampler_info{};
    VkDescriptorImageInfo m0{}, m4{}, m6{}, m9{}, m17{};
    VkDescriptorImageInfo m129{}, m131{}, m134{}, m137{};
    VkDescriptorImageInfo s0{}, s128{}, s129{}, s130{};
    VkDescriptorImageInfo r11{}, r17{}, r18{};
    VkDescriptorImageInfo dispatch1{}, dispatch2{}, dispatch3{}, dispatch130{};
};

VkFormat ffxFormatToVk(uint32_t format) {
    switch (format) {
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
        case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
        case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
        case FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM:
            return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case FFX_API_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}

std::string moduleDir() {
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(&moduleDir), &info) && info.dli_fname) {
        const std::string path = info.dli_fname;
        const auto slash = path.find_last_of('/');
        if (slash != std::string::npos) return path.substr(0, slash);
    }
    return ".";
}

uint32_t findMemoryType(const ProviderContext& c, uint32_t type_bits,
                        VkMemoryPropertyFlags required) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(c.physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & required) == required)
            return i;
    }
    return 0;
}

Buffer createBuffer(const ProviderContext& c, VkDeviceSize size, VkBufferUsageFlags usage,
                    VkMemoryPropertyFlags properties) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(c.device, &ci, nullptr, &b.buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, b.buffer, &req);
    b.allocation_size = req.size;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(c, req.memoryTypeBits, properties);
    if (vkAllocateMemory(c.device, &ai, nullptr, &b.memory) != VK_SUCCESS) return {};
    vkBindBufferMemory(c.device, b.buffer, b.memory, 0);
    return b;
}

Image createImage(const ProviderContext& c, VkFormat format, uint32_t width, uint32_t height,
                  VkImageUsageFlags usage, VkImageAspectFlags aspect) {
    Image img;
    img.format = format;
    img.width = width;
    img.height = height;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(c.device, &ci, nullptr, &img.image) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, img.image, &req);
    img.allocation_size = req.size;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(c, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(c.device, &ai, nullptr, &img.memory) != VK_SUCCESS) return {};
    vkBindImageMemory(c.device, img.image, img.memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    if (vkCreateImageView(c.device, &vi, nullptr, &img.view) != VK_SUCCESS) return {};
    return img;
}

void destroyImage(const ProviderContext& c, Image& img) {
    if (img.view) vkDestroyImageView(c.device, img.view, nullptr);
    if (img.image) vkDestroyImage(c.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(c.device, img.memory, nullptr);
    img = {};
}

void destroyBuffer(const ProviderContext& c, Buffer& b) {
    if (b.buffer) vkDestroyBuffer(c.device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(c.device, b.memory, nullptr);
    b = {};
}

VkImageView cachedView(ProviderContext& c, DispatchSlot& slot, VkImage image, VkFormat format,
                       VkImageAspectFlags aspect) {
    if (!image) return VK_NULL_HANDLE;
    for (const CachedView& v : slot.views)
        if (v.image == image && v.format == format && v.aspect == aspect) return v.view;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(c.device, &vi, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    slot.views.push_back({image, format, aspect, view});
    return view;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return false;
    const std::streamsize size = in.tellg();
    in.seekg(0);
    out.resize(static_cast<size_t>(size));
    return size == 0 || in.read(reinterpret_cast<char*>(out.data()), size).good();
}

bool loadShader(const ProviderContext& c, uint32_t blob_id, std::vector<uint8_t>& out) {
    const char* file = fsr4_412::assetFile(blob_id);
    if (!file) return false;
    return readFile(c.asset_root + "/shaders/" + file, out);
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

void computeBarrier(VkCommandBuffer cb) {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

VkDescriptorSetLayout createLayout(const ProviderContext& c,
                                   const std::vector<VkDescriptorSetLayoutBinding>& binds) {
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(binds.size());
    ci.pBindings = binds.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(c.device, &ci, nullptr, &layout);
    return layout;
}

VkDescriptorSetLayoutBinding binding(uint32_t b, VkDescriptorType t) {
    VkDescriptorSetLayoutBinding lb{};
    lb.binding = b;
    lb.descriptorType = t;
    lb.descriptorCount = 1;
    lb.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    return lb;
}

VkPipelineLayout createPipelineLayout(const ProviderContext& c, VkDescriptorSetLayout layout) {
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 1;
    ci.pSetLayouts = &layout;
    VkPipelineLayout out = VK_NULL_HANDLE;
    vkCreatePipelineLayout(c.device, &ci, nullptr, &out);
    return out;
}

uint32_t computeKey(uint32_t context_flags, uint32_t dispatch_flags) {
    const uint32_t depth_inverted =
        (context_flags & FFX_UPSCALE_ENABLE_DEPTH_INVERTED) ? 1u : 0u;
    const uint32_t display_mv =
        (context_flags & FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS) ? 1u : 0u;
    const uint32_t auto_exposure =
        (context_flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) ? 1u : 0u;
    uint32_t colour = 0;
    if (dispatch_flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB)
        colour = 2;
    else if (dispatch_flags & FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ)
        colour = 3;
    else if (context_flags & FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE)
        colour = 1;
    const uint32_t jitter =
        (context_flags & FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) ? 1u : 0u;
    return depth_inverted | ((display_mv ? 0u : 1u) << 1) | (auto_exposure << 2) |
           (colour << 3) | (jitter << 5);
}

uint64_t contextMemoryUsage(const ProviderContext& c) {
    uint64_t total = 0;
    const Image* images[] = {&c.history,     &c.reprojected,   &c.recurrent,
                             &c.exposure,    &c.dummy_exposure, &c.rcas_input,
                             &c.exposure_atomic, &c.exposure_mip, &c.neutral_depth,
                             &c.neutral_motion};
    for (const Image* img : images)
        if (img->image) total += img->allocation_size;
    total += c.weights.allocation_size + c.scratch.allocation_size;
    for (const auto& slot : c.slots)
        total += slot.tensor_ubo.allocation_size + slot.constants_ubo.allocation_size +
                 slot.spd_ubo.allocation_size + slot.rcas_ubo.allocation_size;
    return total;
}

float halton(int32_t index, int32_t base) {
    float f = 1.0f;
    float result = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

uint32_t bucketFor(uint32_t width, uint32_t height) {
    for (uint32_t k = 0; k < 3; ++k) {
        const uint32_t tile_w = 480u << k;
        const uint32_t tile_h = 270u << k;
        if ((width + tile_w - 1) / tile_w <= 4 && (height + tile_h - 1) / tile_h <= 4)
            return k;
    }
    return 2;
}

const char* passEntry(uint32_t pass) {
    static const char* names[kModelPassCount] = {
        "fsr4_model_v07_fp8_no_scale_prepass",   "fsr4_model_v07_fp8_no_scale_pass0_post",
        "fsr4_model_v07_fp8_no_scale_pass1",     "fsr4_model_v07_fp8_no_scale_pass1_post",
        "fsr4_model_v07_fp8_no_scale_pass2",     "fsr4_model_v07_fp8_no_scale_pass2_post",
        "fsr4_model_v07_fp8_no_scale_pass3",     "fsr4_model_v07_fp8_no_scale_pass3_post",
        "fsr4_model_v07_fp8_no_scale_pass4",     "fsr4_model_v07_fp8_no_scale_pass4_post",
        "fsr4_model_v07_fp8_no_scale_pass5",     "fsr4_model_v07_fp8_no_scale_pass5_post",
        "fsr4_model_v07_fp8_no_scale_pass6",     "fsr4_model_v07_fp8_no_scale_pass6_post",
        "fsr4_model_v07_fp8_no_scale_pass7",     "fsr4_model_v07_fp8_no_scale_pass7_post",
        "fsr4_model_v07_fp8_no_scale_pass8",     "fsr4_model_v07_fp8_no_scale_pass8_post",
        "fsr4_model_v07_fp8_no_scale_pass9",     "fsr4_model_v07_fp8_no_scale_pass9_post",
        "fsr4_model_v07_fp8_no_scale_pass10",    "fsr4_model_v07_fp8_no_scale_pass10_post",
        "fsr4_model_v07_fp8_no_scale_pass11",    "fsr4_model_v07_fp8_no_scale_pass11_post",
        "fsr4_model_v07_fp8_no_scale_pass12",    "fsr4_model_v07_fp8_no_scale_pass12_post",
        "fsr4_model_v07_fp8_no_scale_postpass",
    };
    return pass < kModelPassCount ? names[pass] : "";
}

void destroyPipelines(ProviderContext& c) {
    for (PassPipeline& p : c.model_passes) {
        if (p.pipeline) vkDestroyPipeline(c.device, p.pipeline, nullptr);
        if (p.module) vkDestroyShaderModule(c.device, p.module, nullptr);
    }
    c.model_passes.clear();
    if (c.spd_pass.pipeline) vkDestroyPipeline(c.device, c.spd_pass.pipeline, nullptr);
    if (c.spd_pass.module) vkDestroyShaderModule(c.device, c.spd_pass.module, nullptr);
    c.spd_pass = {};
    if (c.rcas_pass.pipeline) vkDestroyPipeline(c.device, c.rcas_pass.pipeline, nullptr);
    if (c.rcas_pass.module) vkDestroyShaderModule(c.device, c.rcas_pass.module, nullptr);
    c.rcas_pass = {};
}

bool createPipeline(ProviderContext& c, uint32_t blob_id, const char* entry,
                    VkPipelineLayout layout, PassPipeline& out) {
    std::vector<uint8_t> bytes;
    if (!loadShader(c, blob_id, bytes) || bytes.size() % 4 != 0) {
        std::fprintf(stderr, "fsr4: missing shader blob %u\n", blob_id);
        return false;
    }
    VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smci.codeSize = bytes.size();
    smci.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    if (vkCreateShaderModule(c.device, &smci, nullptr, &out.module) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = out.module;
    cpci.stage.pName = entry;
    cpci.layout = layout;
    if (vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &out.pipeline) !=
        VK_SUCCESS) {
        std::fprintf(stderr, "fsr4: pipeline for blob %u (%s) failed\n", blob_id, entry);
        return false;
    }
    return true;
}

bool ensurePipelines(ProviderContext& c, uint32_t key) {
    if (!c.model_passes.empty() && c.key == key) return true;
    destroyPipelines(c);
    c.key = key;
    c.model_passes.resize(kModelPassCount);
    for (uint32_t i = 0; i < kModelPassCount; ++i) {
        const uint32_t blob = fsr4_412::selectPassBlob(i, c.bucket, key);
        if (!createPipeline(c, blob, passEntry(i), c.model_pipeline_layout,
                            c.model_passes[i]))
            return false;
    }
    if (c.has_spd &&
        !createPipeline(c, fsr4_412::kSpdBlobId, "main", c.spd_pipeline_layout, c.spd_pass))
        return false;
    if (c.has_rcas) {
        const uint32_t rcas_blob = fsr4_412::selectRcasBlob(key);
        if (!createPipeline(c, rcas_blob, "fsr_rcas_pass", c.rcas_pipeline_layout,
                            c.rcas_pass))
            return false;
    }
    return true;
}

bool createContextResources(ProviderContext& c) {
    const VkImageUsageFlags sampled_storage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageAspectFlags color = VK_IMAGE_ASPECT_COLOR_BIT;
    const uint32_t ow = c.max_upscale_w;
    const uint32_t oh = c.max_upscale_h;

    c.history = createImage(c, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, sampled_storage, color);
    c.reprojected = createImage(c, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, sampled_storage, color);
    c.recurrent = createImage(c, VK_FORMAT_R8G8B8A8_UNORM, ow, oh, sampled_storage, color);
    c.exposure = createImage(c, VK_FORMAT_R32_SFLOAT, 2, 1, sampled_storage, color);
    c.dummy_exposure = createImage(
        c, VK_FORMAT_R32G32_SFLOAT, 1, 1,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, color);
    c.has_spd = (c.flags & FFX_UPSCALE_ENABLE_AUTO_EXPOSURE) != 0;
    c.has_rcas = true;
    if (c.has_spd) {
        c.exposure_atomic = createImage(c, VK_FORMAT_R32_UINT, 1, 1, sampled_storage, color);
        c.exposure_mip = createImage(c, VK_FORMAT_R32_SFLOAT, (c.max_render_w + 15) / 16,
                                     (c.max_render_h + 15) / 16, sampled_storage, color);
    }
    c.rcas_input = createImage(c, VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, sampled_storage, color);
    c.neutral_depth = createImage(c, VK_FORMAT_R32_SFLOAT, c.max_render_w, c.max_render_h,
                                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                  color);
    c.neutral_motion = createImage(c, VK_FORMAT_R16G16_SFLOAT, c.max_render_w, c.max_render_h,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   color);
    if (!c.history.image || !c.reprojected.image || !c.recurrent.image || !c.exposure.image ||
        !c.dummy_exposure.image || !c.rcas_input.image || !c.neutral_depth.image ||
        !c.neutral_motion.image)
        return false;

    std::vector<uint8_t> weights;
    if (!readFile(c.asset_root + "/models/FP8_standard.bin", weights) || weights.empty())
        return false;
    c.weights = createBuffer(c, weights.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* ptr = nullptr;
    if (!c.weights.buffer ||
        vkMapMemory(c.device, c.weights.memory, 0, c.weights.size, 0, &ptr) != VK_SUCCESS)
        return false;
    std::memcpy(ptr, weights.data(), weights.size());
    vkUnmapMemory(c.device, c.weights.memory);

    const uint64_t scratch_size =
        fsr4_412::scratchCapacity({c.max_upscale_w, c.max_upscale_h});
    c.scratch = createBuffer(c, scratch_size,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!c.scratch.buffer) return false;

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 1000.0f;
    if (vkCreateSampler(c.device, &sci, nullptr, &c.sampler) != VK_SUCCESS) return false;

    std::vector<VkDescriptorSetLayoutBinding> model_binds = {
        binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
        binding(21, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        binding(129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        binding(130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        binding(131, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        binding(134, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        binding(137, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
        binding(141, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER),
        binding(256, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        binding(257, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
        binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
    };
    c.model_layout = createLayout(c, model_binds);
    if (c.has_spd) {
        std::vector<VkDescriptorSetLayoutBinding> binds = {
            binding(0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(128, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(256, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
        };
        c.spd_layout = createLayout(c, binds);
    }
    if (c.has_rcas) {
        std::vector<VkDescriptorSetLayoutBinding> binds = {
            binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER),
            binding(11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE),
            binding(17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE),
            binding(320, VK_DESCRIPTOR_TYPE_SAMPLER),
        };
        c.rcas_layout = createLayout(c, binds);
    }
    c.model_pipeline_layout = createPipelineLayout(c, c.model_layout);
    if (c.has_spd) c.spd_pipeline_layout = createPipelineLayout(c, c.spd_layout);
    if (c.has_rcas) c.rcas_pipeline_layout = createPipelineLayout(c, c.rcas_layout);

    const uint32_t set_count = c.slots.size() * (1 + (c.has_spd ? 1u : 0u) + (c.has_rcas ? 1u : 0u));
    VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8 * set_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * set_count},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 7 * set_count},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * set_count},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 1 * set_count},
    };
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = set_count;
    dpci.poolSizeCount = 5;
    dpci.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(c.device, &dpci, nullptr, &c.descriptor_pool) != VK_SUCCESS)
        return false;
    auto alloc = [&](VkDescriptorSetLayout layout, VkDescriptorSet& set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = c.descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        return vkAllocateDescriptorSets(c.device, &ai, &set) == VK_SUCCESS;
    };
    for (auto& slot : c.slots) {
        slot.tensor_ubo = createBuffer(c, 272, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        slot.constants_ubo = createBuffer(c, 104, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (c.has_spd)
            slot.spd_ubo = createBuffer(c, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (c.has_rcas)
            slot.rcas_ubo = createBuffer(c, 32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!slot.tensor_ubo.buffer || !slot.constants_ubo.buffer ||
            (c.has_spd && !slot.spd_ubo.buffer) || (c.has_rcas && !slot.rcas_ubo.buffer)) return false;

        if (!alloc(c.model_layout, slot.model_set)) return false;
        if (c.has_spd && !alloc(c.spd_layout, slot.spd_set)) return false;
        if (c.has_rcas && !alloc(c.rcas_layout, slot.rcas_set)) return false;

        auto image = [](VkImageView view) {
            VkDescriptorImageInfo info{};
            info.imageView = view;
            info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            return info;
        };
        c.weight_info = {c.weights.buffer, 0, VK_WHOLE_SIZE};
        c.scratch_info = {c.scratch.buffer, 0, VK_WHOLE_SIZE};
        c.tensor_info = {slot.tensor_ubo.buffer, 0, VK_WHOLE_SIZE};
        c.constants_info = {slot.constants_ubo.buffer, 0, VK_WHOLE_SIZE};
        c.sampler_info.sampler = c.sampler;
        c.m0 = image(c.history.view);
        c.m4 = image(c.recurrent.view);
        c.m6 = image(c.dummy_exposure.view);
        c.m9 = image(c.reprojected.view);
        c.m17 = image(c.exposure.view);
        c.m129 = image(c.history.view);
        c.m131 = image(c.reprojected.view);
        c.m134 = image(c.recurrent.view);
        c.m137 = image(c.exposure.view);
        c.s128 = image(c.exposure_atomic.view);
        c.s129 = image(c.exposure_mip.view);
        c.s130 = image(c.exposure.view);
        c.spd_constants_info = {slot.spd_ubo.buffer, 0, VK_WHOLE_SIZE};
        c.rcas_constants_info = {slot.rcas_ubo.buffer, 0, VK_WHOLE_SIZE};
        c.r17 = image(c.exposure.view);
        c.r18 = image(c.rcas_input.view);

        std::vector<VkWriteDescriptorSet> writes;
        auto write_image = [&](VkDescriptorSet set, uint32_t b, VkDescriptorImageInfo* info,
                               VkDescriptorType type) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = info;
            writes.push_back(w);
        };
        auto write_buffer = [&](VkDescriptorSet set, uint32_t b, VkDescriptorBufferInfo* info,
                                VkDescriptorType type) {
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = set;
            w.dstBinding = b;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pBufferInfo = info;
            writes.push_back(w);
        };
        write_image(slot.model_set, 0, &c.m0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 1, &c.dispatch1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 2, &c.dispatch2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 3, &c.dispatch3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 4, &c.m4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 6, &c.m6, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 9, &c.m9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_image(slot.model_set, 17, &c.m17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
        write_buffer(slot.model_set, 21, &c.weight_info, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_image(slot.model_set, 129, &c.m129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(slot.model_set, 130, &c.dispatch130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(slot.model_set, 131, &c.m131, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(slot.model_set, 134, &c.m134, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_image(slot.model_set, 137, &c.m137, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write_buffer(slot.model_set, 141, &c.scratch_info, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        write_buffer(slot.model_set, 256, &c.tensor_info, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        write_buffer(slot.model_set, 257, &c.constants_info, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        write_image(slot.model_set, 320, &c.sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);

        if (c.has_spd) {
            write_image(slot.spd_set, 0, &c.s0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(slot.spd_set, 128, &c.s128, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(slot.spd_set, 129, &c.s129, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(slot.spd_set, 130, &c.s130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_buffer(slot.spd_set, 256, &c.spd_constants_info,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            write_image(slot.spd_set, 320, &c.sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);
        }
        if (c.has_rcas) {
            write_buffer(slot.rcas_set, 0, &c.rcas_constants_info,
                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
            write_image(slot.rcas_set, 11, &c.r11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            write_image(slot.rcas_set, 17, &c.r17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(slot.rcas_set, 18, &c.r18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            write_image(slot.rcas_set, 320, &c.sampler_info, VK_DESCRIPTOR_TYPE_SAMPLER);
        }
        vkUpdateDescriptorSets(c.device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                               nullptr);
    }
    return true;
}

void destroyContext(ProviderContext* c) {
    if (!c) return;
    if (c->device && !c->timeline) vkDeviceWaitIdle(c->device);
    for (auto& slot : c->slots) {
        for (const CachedView& view : slot.views)
            vkDestroyImageView(c->device, view.view, nullptr);
        destroyBuffer(*c, slot.tensor_ubo);
        destroyBuffer(*c, slot.constants_ubo);
        destroyBuffer(*c, slot.spd_ubo);
        destroyBuffer(*c, slot.rcas_ubo);
    }
    destroyPipelines(*c);
    if (c->sampler) vkDestroySampler(c->device, c->sampler, nullptr);
    destroyImage(*c, c->history);
    destroyImage(*c, c->reprojected);
    destroyImage(*c, c->recurrent);
    destroyImage(*c, c->exposure);
    destroyImage(*c, c->dummy_exposure);
    destroyImage(*c, c->rcas_input);
    destroyImage(*c, c->exposure_atomic);
    destroyImage(*c, c->exposure_mip);
    destroyImage(*c, c->neutral_depth);
    destroyImage(*c, c->neutral_motion);
    destroyBuffer(*c, c->weights);
    destroyBuffer(*c, c->scratch);
    if (c->descriptor_pool) vkDestroyDescriptorPool(c->device, c->descriptor_pool, nullptr);
    if (c->model_pipeline_layout)
        vkDestroyPipelineLayout(c->device, c->model_pipeline_layout, nullptr);
    if (c->spd_pipeline_layout)
        vkDestroyPipelineLayout(c->device, c->spd_pipeline_layout, nullptr);
    if (c->rcas_pipeline_layout)
        vkDestroyPipelineLayout(c->device, c->rcas_pipeline_layout, nullptr);
    if (c->model_layout) vkDestroyDescriptorSetLayout(c->device, c->model_layout, nullptr);
    if (c->spd_layout) vkDestroyDescriptorSetLayout(c->device, c->spd_layout, nullptr);
    if (c->rcas_layout) vkDestroyDescriptorSetLayout(c->device, c->rcas_layout, nullptr);
    delete c;
}

void uploadBuffer(const ProviderContext& c, Buffer& b, const void* data, size_t size) {
    if (!b.buffer) return;
    void* ptr = nullptr;
    if (vkMapMemory(c.device, b.memory, 0, b.size, 0, &ptr) != VK_SUCCESS) return;
    std::memcpy(ptr, data, size);
    vkUnmapMemory(c.device, b.memory);
}

} // namespace

// ---------------------------------------------------------------------------
// FFX entry points
// ---------------------------------------------------------------------------
#define FSR4_EXPORT extern "C" __attribute__((visibility("default")))

FSR4_EXPORT ffxReturnCode_t ffxCreateContext(ffxContext* context,
                                             ffxCreateContextDescHeader* desc,
                                             const ffxAllocationCallbacks* mem_cb) {
    (void)mem_cb;
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    *context = nullptr;

    const ffxCreateContextDescUpscale* upscale = nullptr;
    const ffxCreateBackendVKDesc* backend = nullptr;
    VkDeviceContext legacy_device{};
    bool have_legacy = false;
    for (ffxCreateContextDescHeader* h = desc; h; h = h->pNext) {
        if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)
            upscale = reinterpret_cast<const ffxCreateContextDescUpscale*>(h);
        else if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK)
            backend = reinterpret_cast<const ffxCreateBackendVKDesc*>(h);
        else if (h->type == FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED) {
            const auto* old = reinterpret_cast<const ffxCreateBackendVKDesc_DEPRECATED*>(h);
            legacy_device.vkDevice = old->vkDevice;
            legacy_device.vkPhysicalDevice = old->vkPhysicalDevice;
            legacy_device.vkDeviceProcAddr = old->vkDeviceProcAddr;
            have_legacy = true;
        }
    }
    if (!upscale) return FFX_API_RETURN_ERROR_PARAMETER;
    VkDeviceContext device_context{};
    if (backend)
        device_context = backend->deviceContext;
    else if (have_legacy)
        device_context = legacy_device;
    else
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!device_context.vkDevice || !device_context.vkPhysicalDevice ||
        !device_context.vkDeviceProcAddr)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!upscale->maxRenderSize.width || !upscale->maxRenderSize.height ||
        !upscale->maxUpscaleSize.width || !upscale->maxUpscaleSize.height)
        return FFX_API_RETURN_ERROR_PARAMETER;

    auto* c = new ProviderContext();
    c->device = device_context.vkDevice;
    c->physical = device_context.vkPhysicalDevice;
    c->gdpa = device_context.vkDeviceProcAddr;
    c->max_render_w = upscale->maxRenderSize.width;
    c->max_render_h = upscale->maxRenderSize.height;
    c->max_upscale_w = upscale->maxUpscaleSize.width;
    c->max_upscale_h = upscale->maxUpscaleSize.height;
    c->flags = upscale->flags;
    c->bucket = bucketFor(c->max_upscale_w, c->max_upscale_h);
    if (const char* root = std::getenv("FSR4_ASSET_DIR")) {
        c->asset_root = root;
    } else {
        // Default: assets staged next to the effect library.
        c->asset_root = moduleDir() + "/fsr4-assets";
    }

    if (!createContextResources(*c) || !ensurePipelines(*c, computeKey(c->flags, 0))) {
        destroyContext(c);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    *context = c;
    return FFX_API_RETURN_OK;
}

FSR4_EXPORT ffxReturnCode_t ffxDestroyContext(ffxContext* context,
                                              const ffxAllocationCallbacks* mem_cb) {
    (void)mem_cb;
    if (!context || !*context) return FFX_API_RETURN_ERROR_PARAMETER;
    auto* c = reinterpret_cast<ProviderContext*>(*context);
    uint64_t completed = 0;
    if (c->timeline &&
        (vkGetSemaphoreCounterValue(c->device, c->timeline, &completed) != VK_SUCCESS ||
         !fsr4_412::allSlotsComplete(c->submissions, completed)))
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    destroyContext(c);
    *context = nullptr;
    return FFX_API_RETURN_OK;
}

FSR4_EXPORT ffxReturnCode_t ffxConfigure(ffxContext* context,
                                         const ffxConfigureDescHeader* desc) {
    (void)context;
    (void)desc;
    return FFX_API_RETURN_OK;
}

FSR4_EXPORT ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc) {
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    ProviderContext* c = (context && *context) ? reinterpret_cast<ProviderContext*>(*context)
                                               : nullptr;

    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION) {
        auto* out = reinterpret_cast<ffxQueryGetProviderVersion*>(desc);
        out->versionId = FFX_UPSCALER_VERSION;
        out->versionName = "FSR 4.1.2";
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS) {
        auto* out = reinterpret_cast<ffxQueryDescGetVersions*>(desc);
        if (out->outputCount) {
            const uint64_t capacity = *out->outputCount;
            if (capacity >= 1) {
                if (out->versionIds) out->versionIds[0] = FFX_UPSCALER_VERSION;
                // The runtime selects MK8_FSR_VERSION by this list name, and
                // AMD's signed provider lists plain "4.1.2"/"3.1.6". Keep the
                // display name ("FSR 4.1.2") on GET_PROVIDER_VERSION.
                if (out->versionNames) out->versionNames[0] = "4.1.2";
            }
            *out->outputCount = 1;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GETUPSCALERATIOFROMQUALITYMODE) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode*>(desc);
        if (!out->pOutUpscaleRatio) return FFX_API_RETURN_ERROR_PARAMETER;
        switch (out->qualityMode) {
            case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: *out->pOutUpscaleRatio = 1.0f; break;
            case FFX_UPSCALE_QUALITY_MODE_QUALITY: *out->pOutUpscaleRatio = 1.5f; break;
            case FFX_UPSCALE_QUALITY_MODE_BALANCED: *out->pOutUpscaleRatio = 1.7f; break;
            case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: *out->pOutUpscaleRatio = 2.0f; break;
            case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE:
                *out->pOutUpscaleRatio = 3.0f;
                break;
            default: return FFX_API_RETURN_ERROR_PARAMETER;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE) {
        auto* out =
            reinterpret_cast<ffxQueryDescUpscaleGetRenderResolutionFromQualityMode*>(desc);
        if (!out->pOutRenderWidth || !out->pOutRenderHeight)
            return FFX_API_RETURN_ERROR_PARAMETER;
        float ratio = 0.0f;
        switch (out->qualityMode) {
            case FFX_UPSCALE_QUALITY_MODE_NATIVEAA: ratio = 1.0f; break;
            case FFX_UPSCALE_QUALITY_MODE_QUALITY: ratio = 1.5f; break;
            case FFX_UPSCALE_QUALITY_MODE_BALANCED: ratio = 1.7f; break;
            case FFX_UPSCALE_QUALITY_MODE_PERFORMANCE: ratio = 2.0f; break;
            case FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE: ratio = 3.0f; break;
            default: return FFX_API_RETURN_ERROR_PARAMETER;
        }
        *out->pOutRenderWidth = static_cast<uint32_t>(out->displayWidth / ratio);
        *out->pOutRenderHeight = static_cast<uint32_t>(out->displayHeight / ratio);
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetJitterPhaseCount*>(desc);
        if (!out->pOutPhaseCount || !out->renderWidth)
            return FFX_API_RETURN_ERROR_PARAMETER;
        const float ratio = static_cast<float>(out->displayWidth) /
                            static_cast<float>(out->renderWidth);
        *out->pOutPhaseCount = static_cast<int32_t>(8.0f * ratio * ratio);
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTEROFFSET) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetJitterOffset*>(desc);
        if (!out->pOutX || !out->pOutY || out->phaseCount <= 0)
            return FFX_API_RETURN_ERROR_PARAMETER;
        *out->pOutX = halton((out->index % out->phaseCount) + 1, 2) - 0.5f;
        *out->pOutY = halton((out->index % out->phaseCount) + 1, 3) - 0.5f;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsage*>(desc);
        if (!c || !out->gpuMemoryUsageUpscaler) return FFX_API_RETURN_ERROR_PARAMETER;
        out->gpuMemoryUsageUpscaler->totalUsageInBytes = contextMemoryUsage(*c);
        out->gpuMemoryUsageUpscaler->aliasableUsageInBytes = 0;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE_V2) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetGPUMemoryUsageV2*>(desc);
        if (!c || !out->gpuMemoryUsageUpscaler) return FFX_API_RETURN_ERROR_PARAMETER;
        out->gpuMemoryUsageUpscaler->totalUsageInBytes = contextMemoryUsage(*c);
        out->gpuMemoryUsageUpscaler->aliasableUsageInBytes = 0;
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_UPSCALE_GET_RESOURCE_REQUIREMENTS) {
        auto* out = reinterpret_cast<ffxQueryDescUpscaleGetResourceRequirements*>(desc);
        out->required_resources = FFX_API_QUERY_RESOURCE_INPUT_COLOR |
                                  FFX_API_QUERY_RESOURCE_INPUT_DEPTH |
                                  FFX_API_QUERY_RESOURCE_INPUT_MV;
        out->optional_resources = FFX_API_QUERY_RESOURCE_INPUT_EXPOSURE |
                                  FFX_API_QUERY_RESOURCE_INPUT_REACTIVEMASK |
                                  FFX_API_QUERY_RESOURCE_INPUT_TRANSPARENCYCOMPOSITION;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_NO_PROVIDER;
}

static ffxReturnCode_t dispatch(ffxContext* context, const ffxDispatchDescHeader* desc,
                                VkSemaphore timeline, uint64_t submission) {
    if (!context || !*context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type != FFX_API_DISPATCH_DESC_TYPE_UPSCALE)
        return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
    auto* c = reinterpret_cast<ProviderContext*>(*context);
    const auto* d = reinterpret_cast<const ffxDispatchDescUpscale*>(desc);
    if (!d->commandList || !d->color.resource || !d->output.resource)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (!d->renderSize.width || !d->renderSize.height)
        return FFX_API_RETURN_ERROR_PARAMETER;

    const uint32_t render_w = d->renderSize.width;
    const uint32_t render_h = d->renderSize.height;
    const uint32_t out_w = d->upscaleSize.width ? d->upscaleSize.width : c->max_upscale_w;
    const uint32_t out_h = d->upscaleSize.height ? d->upscaleSize.height : c->max_upscale_h;
    if (render_w > c->max_render_w || render_h > c->max_render_h || out_w > c->max_upscale_w ||
        out_h > c->max_upscale_h)
        return FFX_API_RETURN_ERROR_PARAMETER;

    const uint32_t key = computeKey(c->flags, d->flags);
    uint64_t completed = 0;
    if ((c->timeline && c->timeline != timeline) || (timeline &&
        (vkGetSemaphoreCounterValue(c->device, timeline, &completed) != VK_SUCCESS ||
         submission <= completed)))
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    const int index = fsr4_412::availableSlot(c->submissions, completed);
    if (index < 0 || (key != c->key && !fsr4_412::allSlotsComplete(c->submissions, completed)))
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    if (!ensurePipelines(*c, key)) {
        destroyPipelines(*c);
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    }
    auto& slot = c->slots[index];
    for (const CachedView& view : slot.views)
        vkDestroyImageView(c->device, view.view, nullptr);
    slot.views.clear();

    auto* cb = reinterpret_cast<VkCommandBuffer>(d->commandList);
    const VkImage color_image = reinterpret_cast<VkImage>(d->color.resource);
    const VkImage depth_image = reinterpret_cast<VkImage>(d->depth.resource);
    const VkImage motion_image = reinterpret_cast<VkImage>(d->motionVectors.resource);
    const VkImage output_image = reinterpret_cast<VkImage>(d->output.resource);
    const VkFormat color_format = ffxFormatToVk(d->color.description.format);
    const VkFormat depth_format = ffxFormatToVk(d->depth.description.format);
    // The native port also accepts depth already resolved to a colour image (R32_SFLOAT):
    // it has no DEPTH aspect, so the view and the barriers must use COLOR for it.
    const bool depth_is_color = depth_format == VK_FORMAT_R32_SFLOAT ||
                                depth_format == VK_FORMAT_R16_SFLOAT ||
                                depth_format == VK_FORMAT_R16_UNORM;
    const VkImageAspectFlags depth_aspect =
        depth_is_color ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    const VkFormat motion_format = ffxFormatToVk(d->motionVectors.description.format);
    const VkFormat output_format = ffxFormatToVk(d->output.description.format);

    const VkImageView color_view = cachedView(
        *c, slot, color_image,
        color_format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : color_format,
        VK_IMAGE_ASPECT_COLOR_BIT);
    const VkImageView depth_view =
        depth_image ? cachedView(*c, slot, depth_image,
                                 depth_format == VK_FORMAT_UNDEFINED ? VK_FORMAT_D32_SFLOAT
                                                                    : depth_format,
                                 depth_aspect)
                    : c->neutral_depth.view;
    const VkImageView motion_view =
        motion_image ? cachedView(*c, slot, motion_image,
                                  motion_format == VK_FORMAT_UNDEFINED
                                      ? VK_FORMAT_R16G16_SFLOAT
                                      : motion_format,
                                  VK_IMAGE_ASPECT_COLOR_BIT)
                     : c->neutral_motion.view;
    const VkImageView output_view = cachedView(
        *c, slot, output_image,
        output_format == VK_FORMAT_UNDEFINED ? VK_FORMAT_R8G8B8A8_UNORM : output_format,
        VK_IMAGE_ASPECT_COLOR_BIT);
    if (!color_view || !output_view || !depth_view || !motion_view) return FFX_API_RETURN_ERROR_PARAMETER;

    fsr4_412::DispatchInfo di;
    di.render_w = render_w;
    di.render_h = render_h;
    di.upscale_w = out_w;
    di.upscale_h = out_h;
    di.jitter_x = d->jitterOffset.x;
    di.jitter_y = d->jitterOffset.y;
    di.motion_scale_x = d->motionVectorScale.x;
    di.motion_scale_y = d->motionVectorScale.y;
    di.pre_exposure = d->preExposure;
    di.reset = d->reset;
    di.enable_sharpening = d->enableSharpening;
    di.sharpness = d->sharpness;

    fsr4_412::ContextInfo ci;
    ci.max_render_w = c->max_render_w;
    ci.max_render_h = c->max_render_h;
    ci.max_upscale_w = c->max_upscale_w;
    ci.max_upscale_h = c->max_upscale_h;
    ci.flags = c->flags;
    ci.first_dispatch = c->first_dispatch;
    ci.has_previous_exposure = c->has_previous_exposure;
    ci.previous_exposure = c->previous_exposure;
    ci.has_previous_jitter = c->has_previous_jitter;
    ci.previous_jitter[0] = c->previous_jitter[0];
    ci.previous_jitter[1] = c->previous_jitter[1];

    uint8_t tensor[272];
    uint8_t constants[104];
    fsr4_412::writeTensorSizes({out_w, out_h}, tensor);
    fsr4_412::writeOptimizedConstants(ci, di, constants);
    uint8_t spd[32] = {};
    uint32_t spd_gx = 0, spd_gy = 0;
    if (c->has_spd) {
        fsr4_412::spdGroups({render_w, render_h}, spd_gx, spd_gy);
        fsr4_412::writeSpdConstants({render_w, render_h}, spd_gx, spd_gy, d->preExposure, spd);
    }
    uint8_t rcas[32] = {};
    if (c->has_rcas) fsr4_412::writeRcasConstants(d->sharpness, d->preExposure, rcas);

    uploadBuffer(*c, slot.tensor_ubo, tensor, sizeof(tensor));
    uploadBuffer(*c, slot.constants_ubo, constants, sizeof(constants));
    if (c->has_spd) uploadBuffer(*c, slot.spd_ubo, spd, sizeof(spd));
    if (c->has_rcas) uploadBuffer(*c, slot.rcas_ubo, rcas, sizeof(rcas));

    auto image = [](VkImageView view) {
        VkDescriptorImageInfo info{};
        info.imageView = view;
        info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        return info;
    };
    c->dispatch1 = image(motion_view);
    c->dispatch2 = image(depth_view);
    c->dispatch3 = image(color_view);
    c->dispatch130 = image(d->enableSharpening ? c->rcas_input.view : output_view);
    if (c->has_spd) c->s0 = image(color_view);
    if (c->has_rcas) {
        c->r11 = image(output_view);
        c->r18 = image(c->rcas_input.view);
    }
    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](VkDescriptorSet set, uint32_t b, VkDescriptorImageInfo* info,
                     VkDescriptorType type) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = b;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = info;
        writes.push_back(w);
    };
    write(slot.model_set, 1, &c->dispatch1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    write(slot.model_set, 2, &c->dispatch2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    write(slot.model_set, 3, &c->dispatch3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    write(slot.model_set, 130, &c->dispatch130, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
    if (c->has_spd) write(slot.spd_set, 0, &c->s0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    if (c->has_rcas) {
        write(slot.rcas_set, 11, &c->r11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
        write(slot.rcas_set, 18, &c->r18, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    }
    vkUpdateDescriptorSets(c->device, static_cast<uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);

    const VkImageAspectFlags color_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const bool reset = d->reset || c->first_dispatch;

    if (!c->initialized) {
        auto to_general = [&](Image& img) {
            imageBarrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
                         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
        };
        to_general(c->history);
        to_general(c->reprojected);
        to_general(c->recurrent);
        to_general(c->exposure);
        to_general(c->dummy_exposure);
        to_general(c->rcas_input);
        to_general(c->neutral_depth);
        to_general(c->neutral_motion);
        if (c->has_spd) {
            to_general(c->exposure_atomic);
            to_general(c->exposure_mip);
        }
        c->initialized = true;
    }

    imageBarrier(cb, color_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
    if (depth_image)
        imageBarrier(cb, depth_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, depth_aspect);
    if (motion_image)
        imageBarrier(cb, motion_image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, color_aspect);
    imageBarrier(cb, output_image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0,
                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 color_aspect);

    if (reset) {
        VkClearColorValue zero{};
        VkClearColorValue one{};
        one.float32[0] = 1.0f;
        one.float32[1] = 1.0f;
        const VkImageSubresourceRange full{color_aspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, c->history.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &full);
        vkCmdClearColorImage(cb, c->reprojected.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &full);
        vkCmdClearColorImage(cb, c->recurrent.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &full);
        vkCmdClearColorImage(cb, c->exposure.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &full);
        vkCmdClearColorImage(cb, c->dummy_exposure.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1,
                             &full);
        if (c->first_dispatch) {
            vkCmdClearColorImage(cb, c->neutral_depth.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1,
                                 &full);
            VkClearColorValue zero_motion{};
            vkCmdClearColorImage(cb, c->neutral_motion.image, VK_IMAGE_LAYOUT_GENERAL,
                                 &zero_motion, 1, &full);
        }
        vkCmdFillBuffer(cb, c->scratch.buffer, 0, VK_WHOLE_SIZE, 0);
        computeBarrier(cb);
    }

    if (c->has_spd) {
        VkClearColorValue zero_u{};
        const VkImageSubresourceRange full{color_aspect, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, c->exposure_atomic.image, VK_IMAGE_LAYOUT_GENERAL, &zero_u,
                             1, &full);
        computeBarrier(cb);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->spd_pipeline_layout, 0,
                                1, &slot.spd_set, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->spd_pass.pipeline);
        vkCmdDispatch(cb, spd_gx, spd_gy, 1);
        computeBarrier(cb);
    }

    for (uint32_t i = 0; i < kModelPassCount; ++i) {
        const fsr4_412::DispatchGrid grid = fsr4_412::modelPassDispatch(
            i, {out_w, out_h}, {render_w, render_h}, tensor);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->model_pipeline_layout,
                                0, 1, &slot.model_set, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->model_passes[i].pipeline);
        vkCmdDispatch(cb, grid.x, grid.y, 1);
        computeBarrier(cb);
    }

    if (c->has_rcas && d->enableSharpening) {
        uint32_t gx = 0, gy = 0;
        fsr4_412::rcasGroups({out_w, out_h}, gx, gy);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->rcas_pipeline_layout, 0,
                                1, &slot.rcas_set, 0, nullptr);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->rcas_pass.pipeline);
        vkCmdDispatch(cb, gx, gy, 1);
        computeBarrier(cb);
    }

    imageBarrier(cb, color_image, VK_IMAGE_LAYOUT_GENERAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                 VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, color_aspect);
    if (depth_image)
        imageBarrier(cb, depth_image, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, depth_aspect);
    if (motion_image)
        imageBarrier(cb, motion_image, VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, color_aspect);

    if (timeline) {
        c->timeline = timeline;
        c->submissions[index] = submission;
    }
    c->first_dispatch = false;
    c->has_previous_exposure = true;
    c->previous_exposure = d->preExposure;
    c->has_previous_jitter = true;
    c->previous_jitter[0] = d->jitterOffset.x;
    c->previous_jitter[1] = d->jitterOffset.y;
    return FFX_API_RETURN_OK;
}


FSR4_EXPORT ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc) {
    // Standalone callers serialize submissions and keep input resources alive until completion.
    return dispatch(context, desc, VK_NULL_HANDLE, 0);
}

FSR4_EXPORT ffxReturnCode_t mk8Fsr4DispatchV1(ffxContext* context, const ffxDispatchDescHeader* desc,
                                             VkSemaphore timeline, uint64_t submission) {
    if (!timeline || !submission) return FFX_API_RETURN_ERROR_PARAMETER;
    return dispatch(context, desc, timeline, submission);
}
