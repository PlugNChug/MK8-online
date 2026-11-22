#pragma once

#include <vulkan/vulkan.h>

// Captured render targets are written again in GENERAL after a vendor pass.
class ScopedSampledImage {
public:
    ScopedSampledImage(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect)
        : cb(cb), image(image), aspect(aspect) {
        transition(VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    }

    ScopedSampledImage(const ScopedSampledImage&) = delete;
    ScopedSampledImage& operator=(const ScopedSampledImage&) = delete;

    ~ScopedSampledImage() {
        transition(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                   VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }

private:
    VkCommandBuffer cb;
    VkImage image;
    VkImageAspectFlags aspect;

    void transition(VkImageLayout before, VkImageLayout after, VkAccessFlags src, VkAccessFlags dst) {
        if (!image) return;
        VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcAccessMask = src;
        barrier.dstAccessMask = dst;
        barrier.oldLayout = before;
        barrier.newLayout = after;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {aspect, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
};
