#include <assert.h>
#include <stdlib.h>
#include "nvn/vulkan/vk.h"

#undef FATAL
#define FATAL(...) abort()
#undef VK_CHECK
#define VK_CHECK(x) assert((x) == VK_SUCCESS)
VkG g_vk;
static VkImageCreateInfo image;
static VkImageViewCreateInfo view;
static VkImageSubresourceRange cleared;
static unsigned allocations, views, clears, barriers;
static bool depth_clear, d16_supported = true;
static VkImageAspectFlags transitioned;

VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ci, const VmaAllocationCreateInfo *ai,
                        VkImage *out, VmaAllocation *mem, VmaAllocationInfo *info) {
    image = *ci;
    *out = (VkImage)(uintptr_t)++allocations;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d, const VkImageViewCreateInfo *ci,
                                                 const VkAllocationCallbacks *alloc, VkImageView *out) {
    view = *ci;
    *out = (VkImageView)(uintptr_t)++views;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice d, VkFormat fmt, VkFormatProperties2 *p) {
    VkFormatProperties3 *features = p->pNext;
    features->optimalTilingFeatures = fmt == VK_FORMAT_D16_UNORM && !d16_supported ? 0 :
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT |
        VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice d, VkFormat fmt, VkImageType type, VkImageTiling tiling, VkImageUsageFlags usage,
    VkImageCreateFlags flags, VkImageFormatProperties *p) {
    *p = (VkImageFormatProperties){.maxExtent={1,1,1},.maxMipLevels=1,.maxArrayLayers=6,.sampleCounts=VK_SAMPLE_COUNT_1_BIT};
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice device, const VkSamplerCreateInfo *ci,
                                               const VkAllocationCallbacks *alloc, VkSampler *out) {
    assert(ci->compareEnable && ci->compareOp == VK_COMPARE_OP_LESS_OR_EQUAL);
    *out = (VkSampler)(uintptr_t)42;
    return VK_SUCCESS;
}
VkCommandBuffer vk_upload_cmd(void) { return (VkCommandBuffer)(uintptr_t)1; }
void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) { transitioned = aspect; }
void vk_full_barrier(VkCommandBuffer cb) { barriers++; }
VKAPI_ATTR void VKAPI_CALL vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout,
    const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *ranges) {
    assert(v->depth == 0 && n == 1 && layout == VK_IMAGE_LAYOUT_GENERAL);
    depth_clear = true; cleared = ranges[0]; clears++;
}
VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cb, VkImage img, VkImageLayout layout,
    const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *ranges) {
    assert(v->uint32[0] == 0 && v->uint32[1] == 0 && v->uint32[2] == 0 && v->uint32[3] == 0 && n == 1);
    depth_clear = false; cleared = ranges[0]; clears++;
}

#include "dummy_factory.inc"

int main(void) {
    const int types[] = {TEXTYPE_1D,TEXTYPE_2D,TEXTYPE_1D_ARRAY,TEXTYPE_2D_ARRAY,TEXTYPE_CUBE,TEXTYPE_CUBE_ARRAY};
    const VkImageViewType expected[] = {VK_IMAGE_VIEW_TYPE_1D,VK_IMAGE_VIEW_TYPE_2D,VK_IMAGE_VIEW_TYPE_1D_ARRAY,
                                        VK_IMAGE_VIEW_TYPE_2D_ARRAY,VK_IMAGE_VIEW_TYPE_CUBE,VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
    for (unsigned i=0; i<sizeof(types)/sizeof(types[0]); i++) {
        unsigned layers = i >= 4 ? 6 : 1;
        d16_supported = i != 1;  /* Cover selection of the second supported format. */
        for (unsigned depth=0; depth<2; depth++) {
            VkImageView handle = depth ? vk_shadow_dummy_view(types[i]) : vk_dummy_view(types[i]);
            VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            assert(handle && vk_is_color_dummy_view(handle) == !depth);
            assert(image.format == (depth ? (d16_supported ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT) : VK_FORMAT_R8G8B8A8_UNORM));
            assert(image.format == view.format && view.viewType == expected[i]);
            assert(image.arrayLayers == layers && view.subresourceRange.layerCount == layers && cleared.layerCount == layers);
            assert(!!(image.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) == (layers == 6));
            assert(image.samples == VK_SAMPLE_COUNT_1_BIT && image.mipLevels == 1);
            assert(transitioned == aspect && cleared.aspectMask == aspect && view.subresourceRange.aspectMask == aspect);
            assert(depth_clear == depth && clears == views && views == allocations);
            assert((depth ? vk_shadow_dummy_view(types[i]) : vk_dummy_view(types[i])) == handle && views == 2*i+depth+1);
        }
    }
    assert(!vk_is_color_dummy_view(VK_NULL_HANDLE) && barriers == clears);
    assert(vk_shadow_default_sampler() == (VkSampler)(uintptr_t)42);
    return 0;
}
