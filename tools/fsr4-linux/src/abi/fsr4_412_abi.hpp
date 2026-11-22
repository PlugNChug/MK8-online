// FSR 4.1.2 ABI helpers: tensor sizes, scratch layout, constants blocks.
//
// All values here were recovered from AMD's signed Vulkan DLL (see PHASE0.md)
// and are validated byte-for-byte by tests/abi/abi_test.cpp against reference
// traces.
//
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>
#include <cstddef>

namespace fsr4_412 {

struct Dims {
    uint32_t width = 0;
    uint32_t height = 0;
};

// GPU-side 104-byte MLSR_Optimized_Constants block.
struct OptimizedConstants {
    float inv_size[2];                 // 0
    float scale[2];                    // 8
    float inv_scale[2];                // 16
    float jitter[2];                   // 24
    float mv_scale[2];                 // 32
    float tex_size[2];                 // 40
    float max_render_size[2];          // 48
    float motion_vector_jitter_cancel[2]; // 56
    int32_t width;                     // 64
    int32_t height;                    // 68
    int32_t reset;                     // 72
    int32_t width_lr;                  // 76
    int32_t height_lr;                 // 80
    float pre_exposure;                // 84
    float previous_pre_exposure;       // 88
    int32_t rcas_enabled;              // 92
    float rcas_sharpness;              // 96
    float pad;                         // 100
};
static_assert(sizeof(OptimizedConstants) == 104, "FSR4 4.1.2 constants are 104 bytes");

// GPU-side 32-byte auto-exposure SPD constants.
struct SpdConstants {
    uint32_t mips;          // 0
    uint32_t num_workgroups; // 4
    uint32_t workgroup_offset[2]; // 8
    float inv_input_size[2]; // 16
    float pre_exposure;     // 24
    float pad;              // 28
};
static_assert(sizeof(SpdConstants) == 32, "SPD constants are 32 bytes");

// GPU-side 32-byte RCAS constants.
struct RcasConstants {
    uint32_t config[4];     // 0: [0] = exp2(2*sharpness-2), [1] = packed fp16 pair
    float pre_exposure;     // 16
    float pad[3];           // 20
};
static_assert(sizeof(RcasConstants) == 32, "RCAS constants are 32 bytes");

// Per-dispatch inputs (mirrors the fields of ffxDispatchDescUpscale we use).
struct DispatchInfo {
    uint32_t render_w = 0;
    uint32_t render_h = 0;
    uint32_t upscale_w = 0;
    uint32_t upscale_h = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float motion_scale_x = 0.0f;
    float motion_scale_y = 0.0f;
    float pre_exposure = 1.0f;
    bool reset = false;
    bool enable_sharpening = false;
    float sharpness = 0.0f;
};

// Per-context state that persists across dispatches.
struct ContextInfo {
    uint32_t max_render_w = 0;
    uint32_t max_render_h = 0;
    uint32_t max_upscale_w = 0;
    uint32_t max_upscale_h = 0;
    uint32_t flags = 0;
    bool first_dispatch = true;
    bool has_previous_exposure = false;
    float previous_exposure = 0.0f;
    bool has_previous_jitter = false;
    float previous_jitter[2] = {0.0f, 0.0f};
};

// FFX_UPSCALE_ENABLE_* bits used by the 4.1.2 ABI.
enum UpscaleFlags : uint32_t {
    kHighDynamicRange = 1u << 0,
    kDisplayResolutionMotionVectors = 1u << 1,
    kMotionVectorsJitterCancellation = 1u << 2,
    kDepthInverted = 1u << 3,
    kDepthInfinite = 1u << 4,
    kAutoExposure = 1u << 5,
};

// Resolution class: the shaders are selected per class, not per exact size.
// The class maximum is used for the tensor bucket strides and scratch size.
Dims classMaximum(uint32_t out_w, uint32_t out_h);

// 17-entry CsTensorSizes table (272 bytes) for the actual upscale size.
void writeTensorSizes(Dims upscale, uint8_t out[272]);

// Scratch buffer capacity for the class (20,880,256 / 83,232,256 / 332,352,256).
uint64_t scratchCapacity(Dims upscale);

// 104-byte MLSR_Optimized_Constants.
void writeOptimizedConstants(const ContextInfo& context, const DispatchInfo& dispatch,
                             uint8_t out[104]);

// 32-byte SPD constants (auto exposure pass). `groups` is the SPD dispatch grid.
void writeSpdConstants(Dims render, uint32_t groups_x, uint32_t groups_y,
                       float pre_exposure, uint8_t out[32]);

// 32-byte RCAS constants.
void writeRcasConstants(float sharpness, float pre_exposure, uint8_t out[32]);

// SPD dispatch grid: ceil(render / 64) per axis.
void spdGroups(Dims render, uint32_t& groups_x, uint32_t& groups_y);

// RCAS dispatch grid: ceil(upscale / 16) per axis.
void rcasGroups(Dims upscale, uint32_t& groups_x, uint32_t& groups_y);

struct DispatchGrid {
    uint32_t x = 1;
    uint32_t y = 1;
    uint32_t z = 1;
};

// Dispatch grid for canonical pass 0..26 (prepass, pass0_post..pass12_post,
// postpass) given the 272-byte tensor table. Validated against the traces.
DispatchGrid modelPassDispatch(uint32_t pass_index, Dims upscale, Dims render,
                               const uint8_t tensor[272]);

// fp32 -> IEEE fp16 (round-to-nearest-even); exposed for tests.
uint16_t floatToHalf(float value);

} // namespace fsr4_412
