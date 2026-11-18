// C bridge for the C++ FSR file: the runtime's Vulkan helpers have C linkage.
#include "vulkan/vk.h"

VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout layout) { return vk_alloc_set(layout); }
VkSampler fsr_vk_default_sampler(bool linear) { return vk_default_sampler(linear); }
VkImageView fsr_vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv,
                              uint32_t ly0, uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    return vk_image_view(b, type, fmt, lv0, nlv, ly0, nly, sw, aspect);
}
VkShaderModule fsr_vk_compile_glsl(const char *src, int stage, const char *name) {
    return vk_compile_glsl(src, stage, name);
}
