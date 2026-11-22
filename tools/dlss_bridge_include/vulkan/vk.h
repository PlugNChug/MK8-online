// Minimal Vulkan backend surface for the standalone bridge harness.
//
// runtime/mk8/graphics/dlss.cpp includes "vulkan/vk.h" and uses only these members and
// helpers. The harness compiles the real bridge source against this shim so it can call
// the public bridge API on a real device without pulling in the runtime's NVN layer.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct VmaAllocation_T *VmaAllocation;
typedef struct VmaAllocator_T *VmaAllocator;

typedef enum VmaMemoryUsage {
    VMA_MEMORY_USAGE_UNKNOWN = 0,
    VMA_MEMORY_USAGE_GPU_ONLY = 1,
    VMA_MEMORY_USAGE_CPU_ONLY = 2,
    VMA_MEMORY_USAGE_CPU_TO_GPU = 3,
    VMA_MEMORY_USAGE_GPU_TO_CPU = 4,
    VMA_MEMORY_USAGE_AUTO = 5,
    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE = 6,
    VMA_MEMORY_USAGE_AUTO_PREFER_HOST = 7,
} VmaMemoryUsage;

enum {
    VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT = 0x00000001,
    VMA_ALLOCATION_CREATE_MAPPED_BIT = 0x00000004,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT = 0x00000400,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 0x00000800,
};

typedef struct VmaAllocationCreateInfo {
    VkFlags flags;
    VmaMemoryUsage usage;
    VkMemoryPropertyFlags requiredFlags;
    VkMemoryPropertyFlags preferredFlags;
    uint32_t memoryTypeBits;
} VmaAllocationCreateInfo;

typedef struct VmaAllocationInfo {
    uint32_t memoryType;
    VkDeviceMemory deviceMemory;
    VkDeviceSize offset;
    VkDeviceSize size;
    void *pMappedData;
} VmaAllocationInfo;

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VmaAllocator vma;
    VkPhysicalDeviceProperties props;
    VkPipelineCache pcache;
    VkSemaphore timeline;
    VkCommandPool pool;
    bool custom_border, divisor, host_import, geometry;
    bool bda;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat sc_format;
    VkExtent2D sc_extent;
    uint32_t sc_count;
    VkImage sc_images[8];
    VkImageView sc_views[8];
} VkG;

extern VkG g_vk;

typedef struct { VkBuffer buf; uint64_t off; void *ptr; } VkSpan;

typedef struct TexBackend {
    VkImage image;
    VmaAllocation mem;
    VkFormat format;
    VkImageViewType base_type;
    uint32_t width, height, depth, layers, levels, samples;
    VkImageAspectFlags aspect;
    bool gpu_owned, uploaded, dynamic;
} TexBackend;

VkResult vmaCreateImage(VmaAllocator allocator, const VkImageCreateInfo *image_info,
                        const VmaAllocationCreateInfo *allocation_info, VkImage *image,
                        VmaAllocation *allocation, VmaAllocationInfo *allocation_info_out);
VkResult vmaCreateBuffer(VmaAllocator allocator, const VkBufferCreateInfo *buffer_info,
                         const VmaAllocationCreateInfo *allocation_info, VkBuffer *buffer,
                         VmaAllocation *allocation, VmaAllocationInfo *allocation_info_out);
void vmaFreeMemory(VmaAllocator allocator, VmaAllocation allocation);
