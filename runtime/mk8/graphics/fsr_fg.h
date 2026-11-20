// Inputs shared between the FSR upscaler bridge and the native frame-generation bridge.
//
// fsr.cpp owns the captured scene depth and the camera motion-vector compute; fsr_fg.cpp asks for
// them once per presented frame. Both are optional: the helpers substitute a cleared neutral depth
// image and zero motion vectors, and callers must log that state so a prototype is not mistaken for
// a qualified image.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    bool restore_general;
    bool real;  // depth: the game's captured scene depth; motion: camera-derived vectors
} Mk8FsrFgInput;

#ifdef __cplusplus
extern "C" {
#endif

// Records the depth/motion barriers and, when available, the camera motion-vector dispatch into cb.
// The caller transitions a capture with restore_general around the provider dispatch.
bool mk8_fsr_fg_inputs(VkCommandBuffer cb, uint32_t width, uint32_t height,
                       Mk8FsrFgInput *depth, Mk8FsrFgInput *motion);

#ifdef __cplusplus
}
#endif
