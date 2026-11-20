#include "fsr.cpp"
#include "dispatch_slots.hpp"

#include <cassert>
#include <map>

VkG g_vk{};
float g_fsr_view[12], g_fsr_proj[16];
int g_fsr_vp_valid;
TexBackend* g_fsr_captured;

static std::map<VkImage, VkImageLayout> layouts;
static std::map<VkImage, int> depth;
static bool fail_dispatch = true;
static bool observed_reset;

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(
    VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
    uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
    uint32_t count, const VkImageMemoryBarrier* barriers) {
    for (uint32_t i = 0; i < count; ++i) {
        const auto& barrier = barriers[i];
        assert(layouts.at(barrier.image) == barrier.oldLayout);
        layouts[barrier.image] = barrier.newLayout;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(
    VkCommandBuffer, VkImage src, VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
    uint32_t, const VkImageBlit*, VkFilter) {
    assert(layouts.at(src) == src_layout && layouts.at(dst) == dst_layout);
    depth[dst] = depth.at(src);
}

static ffxReturnCode_t dispatch_fixture(ffxContext*, const ffxDispatchDescHeader* header,
                                         VkSemaphore timeline, uint64_t submission) {
    const auto& desc = *reinterpret_cast<const ffxDispatchDescUpscale*>(header);
    assert(layouts.at(reinterpret_cast<VkImage>(desc.color.resource)) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(layouts.at(reinterpret_cast<VkImage>(desc.depth.resource)) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    assert(desc.jitterOffset.x == 0 && desc.jitterOffset.y == 0);
    assert(timeline == g_vk.timeline && submission == 4);
    observed_reset = desc.reset;
    return fail_dispatch ? FFX_API_RETURN_ERROR_RUNTIME_ERROR : FFX_API_RETURN_OK;
}

extern "C" VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout) { abort(); }
extern "C" VkSampler fsr_vk_default_sampler(bool) { abort(); }
extern "C" VkImageView fsr_vk_image_view(TexBackend*, VkImageViewType, VkFormat, uint32_t, uint32_t,
                                         uint32_t, uint32_t, VkComponentMapping, VkImageAspectFlags) {
    return reinterpret_cast<VkImageView>(1);
}
extern "C" VkShaderModule fsr_vk_compile_glsl(const char*, int, const char*) { abort(); }
extern "C" VkSpan fsr_vk_uniform(uint64_t) { abort(); }
extern "C" uint64_t fsr_vk_pending_seq() { return 4; }
extern "C" uint64_t fsr_vk_completed_seq() { return 2; }
VkResult vmaCreateImage(VmaAllocator, const VkImageCreateInfo*, const VmaAllocationCreateInfo*,
                         VkImage*, VmaAllocation*, VmaAllocationInfo*) { abort(); }
void vmaFreeMemory(VmaAllocator, VmaAllocation) { abort(); }
VkImageView mk8_dlss_upscale(VkCommandBuffer, TexBackend*, TexBackend*, uint32_t, uint32_t,
                              uint32_t, uint32_t, float) { abort(); }

int main() {
    fsr4_412::SubmissionValues slots{};
    for (uint64_t i = 0; i < slots.size(); ++i) {
        assert(fsr4_412::availableSlot(slots, 4) == static_cast<int>(i));
        slots[i] = i + 5;
    }
    assert(fsr4_412::availableSlot(slots, 4) < 0);
    assert(fsr4_412::availableSlot(slots, 5) == 0);
    assert(!fsr4_412::allSlotsComplete(slots, 6));
    assert(fsr4_412::allSlotsComplete(slots, 7));

    TexBackend capture{};
    capture.image = reinterpret_cast<VkImage>(1);
    capture.format = VK_FORMAT_R32_SFLOAT;
    capture.width = 256;
    capture.height = 144;
    capture.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    layouts[capture.image] = VK_IMAGE_LAYOUT_GENERAL;
    fsr.resampled_depth = reinterpret_cast<VkImage>(2);
    fsr.resampled_depth_view = reinterpret_cast<VkImageView>(2);
    fsr.resampled_format = capture.format;
    fsr.resampled_w = 512;
    fsr.resampled_h = 288;
    layouts[fsr.resampled_depth] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const auto cb = reinterpret_cast<VkCommandBuffer>(1);
    for (int value : {12, 27}) {
        depth[capture.image] = value;
        assert(ensure_resampled_depth(cb, &capture, 512, 288));
        assert(depth.at(fsr.resampled_depth) == value);
        assert(layouts.at(capture.image) == VK_IMAGE_LAYOUT_GENERAL);
        assert(layouts.at(fsr.resampled_depth) == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    fsr.last_fg_use = 3;
    assert(!ensure_resampled_depth(cb, &capture, 768, 432));

    TexBackend color = capture;
    color.image = reinterpret_cast<VkImage>(3);
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    layouts[color.image] = VK_IMAGE_LAYOUT_GENERAL;
    g_vk.timeline = reinterpret_cast<VkSemaphore>(1);
    fsr.tried = fsr.available = fsr.primed = true;
    fsr.context = reinterpret_cast<ffxContext>(1);
    fsr.dispatch_v1 = dispatch_fixture;
    fsr.rw = capture.width;
    fsr.rh = capture.height;
    fsr.ow = 512;
    fsr.oh = 288;
    fsr.output = reinterpret_cast<VkImage>(4);
    fsr.output_view = reinterpret_cast<VkImageView>(4);
    fsr.motion = reinterpret_cast<VkImage>(5);
    setenv("MK8_FSR4", "1", 1);
    setenv("MK8_FSR_VERSION", "4.1.2", 1);
    for (bool fail : {true, false}) {
        fail_dispatch = fail;
        const auto output = mk8_fsr4_upscale(cb, &color, &capture, fsr.rw, fsr.rh, fsr.ow, fsr.oh, 16.7f);
        assert(output == (fail ? VK_NULL_HANDLE : fsr.output_view));
        assert(fsr.reset_history == fail);
        assert(observed_reset);
        assert(layouts.at(color.image) == VK_IMAGE_LAYOUT_GENERAL);
        assert(layouts.at(capture.image) == VK_IMAGE_LAYOUT_GENERAL);
    }
}
