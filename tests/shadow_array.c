#include <assert.h>
#include <string.h>
#include "nvn/vulkan/vk.h"

VkG g_vk;
static bool rendering = true, format_ok = true;
static int generation = 1, images, views, frees, reads, writes, barriers;
static uint64_t sequence = 10;
static VkImageCreateInfo image;
static VkImageViewCreateInfo view;
static VkImageSubresourceLayers read_layers;
static VkExtent3D read_extent;
static uint32_t scratch[1024], copied[8][4][256];
static VkImageView retired_view;
uint64_t vk_pending_seq(void) { return sequence; }
VkCommandBuffer vk_cmd(void) { assert(!rendering); return (VkCommandBuffer)(uintptr_t)9; }
void vk_end_pass(void) { rendering = false; }
void vk_image_init_layout(VkCommandBuffer cb,VkImage dst,VkImageAspectFlags aspect) {
    assert(cb == vk_cmd() && aspect == VK_IMAGE_ASPECT_DEPTH_BIT);
}
void vk_full_barrier(VkCommandBuffer cb) { assert(cb == vk_cmd()); barriers++; }
VkSpan vk_transient(uint64_t size,uint64_t alignment) {
    assert(!rendering && size <= sizeof scratch && alignment >= 4);
    return (VkSpan){(VkBuffer)(uintptr_t)3,16,NULL};
}
void vk_defer_view(VkImageView v) { assert(!retired_view); retired_view = v; }
void vk_defer_image(VkImage img,VmaAllocation a) { assert(retired_view); retired_view = NULL; frees++; }
VkResult vmaCreateImage(VmaAllocator v,const VkImageCreateInfo *ci,const VmaAllocationCreateInfo *ai,
                        VkImage *out,VmaAllocation *mem,VmaAllocationInfo *info) {
    image = *ci; *out = (VkImage)(uintptr_t)(100 + ++images);
    return VK_SUCCESS;
}
void vmaDestroyImage(VmaAllocator v,VkImage i,VmaAllocation m) { assert(0); }
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice d,const VkImageViewCreateInfo *ci,
                                                 const VkAllocationCallbacks *a,VkImageView *out) {
    view = *ci; *out = (VkImageView)(uintptr_t)(100 + ++views); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice d,VkFormat f,VkFormatProperties2 *p) {
    assert(f == VK_FORMAT_D32_SFLOAT);
    ((VkFormatProperties3*)p->pNext)->optimalTilingFeatures = format_ok ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT : 0;
}
VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceImageFormatProperties(VkPhysicalDevice d,VkFormat f,VkImageType t,
    VkImageTiling tiling,VkImageUsageFlags usage,VkImageCreateFlags flags,VkImageFormatProperties *p) {
    *p = (VkImageFormatProperties){.maxExtent={128,128,1},.maxMipLevels=8,.maxArrayLayers=16,.sampleCounts=1};
    return VK_SUCCESS;
}
static uint32_t bits(uint32_t mip,uint32_t layer,uint32_t index) {
    return 0x3f000000 + generation * 10000 + mip * 1000 + layer * 100 + index;
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cb,VkImage src,VkImageLayout layout,
                                                  VkBuffer out,uint32_t count,const VkBufferImageCopy *r) {
    assert(cb == vk_cmd() && layout == VK_IMAGE_LAYOUT_GENERAL && count == 1 && barriers > 0);
    read_layers = r->imageSubresource; read_extent = r->imageExtent; reads++;
    assert(read_layers.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && r->bufferOffset == 16 && read_extent.depth == 1);
    unsigned n = read_extent.width * read_extent.height;
    for (unsigned layer=0;layer<read_layers.layerCount;++layer)
        for (unsigned i=0;i<n;++i)
            scratch[layer*n+i] = bits(read_layers.mipLevel,read_layers.baseArrayLayer+layer,i);
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cb,VkBuffer src,VkImage dst,VkImageLayout layout,
                                                  uint32_t count,const VkBufferImageCopy *r) {
    assert(cb == vk_cmd() && layout == VK_IMAGE_LAYOUT_GENERAL && count == 1 && writes+1 == reads);
    assert(r->imageSubresource.aspectMask == VK_IMAGE_ASPECT_DEPTH_BIT && r->imageSubresource.baseArrayLayer == 0);
    assert(r->imageSubresource.layerCount == read_layers.layerCount && !memcmp(&read_extent,&r->imageExtent,sizeof read_extent));
    unsigned n = read_extent.width * read_extent.height; writes++;
    for (unsigned layer=0;layer<read_layers.layerCount;++layer)
        memcpy(copied[r->imageSubresource.mipLevel][layer],scratch+layer*n,n*sizeof *scratch);
}
static uint32_t level_w(const TexBackend *b,uint32_t m) { return b->width >> m ? b->width >> m : 1; }
static uint32_t level_h(const TexBackend *b,uint32_t m) { return b->height >> m ? b->height >> m : 1; }
#include "../runtime/mk8/graphics/shadow_array.inc"

static bool failed;
#define shadow_array_copy(b,fmt,sw,range) (failed=false,shadow_array_copy(b,fmt,sw,range,&failed))

int main(void) {
    TexBackend b = {.format=VK_FORMAT_R32_SFLOAT,.width=16,.height=8,.samples=1,.depth=1,.layers=5,.levels=4,
                    .image=(VkImage)(uintptr_t)1};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT,1,2,2,2};
    VkComponentMapping identity = {0};
    VkImageView d = shadow_array_copy(&b,b.format,identity,range);
    assert(d && images == 1 && image.format == VK_FORMAT_D32_SFLOAT && image.extent.width == 8 && image.extent.height == 4);
    assert(image.arrayLayers == 2 && image.mipLevels == 2 && view.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    assert(view.subresourceRange.baseArrayLayer == 0 && view.subresourceRange.baseMipLevel == 0 && writes == 2);
    assert(copied[0][0][31] == bits(1,2,31) && copied[0][1][0] == bits(1,3,0) && copied[1][1][7] == bits(2,3,7));
    rendering = true;
    assert(shadow_array_copy(&b,b.format,identity,range) == d && rendering && reads == 2);
    generation++; mk8_shadow_array_written(b.image);
    assert(shadow_array_copy(&b,b.format,identity,range) == d && !rendering && reads == 4);
    assert(copied[1][1][7] == bits(2,3,7));
    sequence++; rendering = true;
    assert(shadow_array_copy(&b,b.format,identity,range) == d && !rendering && reads == 6);
    forget_shadow_arrays(b.image); assert(frees == 1);
    format_ok = false; assert(!shadow_array_copy(&b,b.format,identity,range) && failed && reads == 6);
    format_ok = true; assert(!shadow_array_copy(&b,b.format,identity,range) && failed);
    sequence++;
    format_ok = true; b.samples = 4;
    assert(!shadow_array_copy(&b,b.format,identity,range) && !failed); b.samples = 1;
    assert(!shadow_array_copy(&b,VK_FORMAT_D32_SFLOAT,identity,range));
    assert(!shadow_array_copy(&b,b.format,(VkComponentMapping){.r=VK_COMPONENT_SWIZZLE_ONE},range) && failed);
    range.baseArrayLayer = 4; assert(!shadow_array_copy(&b,b.format,identity,range));
    range.baseArrayLayer = 1;
    for (unsigned i=2;i<12;++i) {
        b.image = (VkImage)(uintptr_t)i;
        assert(shadow_array_copy(&b,b.format,identity,range) != d);
    }
    assert(frees >= 3 && images == 11);
    return 0;
}
