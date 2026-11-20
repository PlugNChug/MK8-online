// Minimal XeSS Vulkan ABI used by the optional host bridge.
//
// This is NOT a copy of Intel's SDK headers. It declares only the names, numeric
// values and field order that cross the libxess.dll boundary, so the bridge can be
// built without vendoring the proprietary SDK. The downloaded SDK remains the
// normative reference; tests/test_xess.py compiles this header together with the
// official xess.h/xess_vk.h (when present) and static_asserts size/offset equality.
//
// Layout facts taken from XeSS SDK 3.0.2:
//   - every public struct uses #pragma pack(push, 8);
//   - xess_vk_image_view_info field order: view, image, range, format, width, height;
//   - xess_vk_execute_params_t starts with six image views, then jitter/exposure/
//     reset/input size, then six xess_coord_t bases (xess_coord_t == xess_2d_t);
//   - xess_vk_init_params_t starts with outputResolution, qualitySetting, initFlags,
//     node masks, then optional external heaps and pipeline cache.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MK8_XESS_ABI_PACK 8
#if defined(_MSC_VER)
#pragma pack(push, MK8_XESS_ABI_PACK)
#else
#pragma pack(push, MK8_XESS_ABI_PACK)
#endif

typedef struct Mk8XessContext* Mk8XessContextHandle;

typedef struct Mk8XessVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
    uint16_t reserved;
} Mk8XessVersion;

typedef struct Mk8Xess2D {
    uint32_t x;
    uint32_t y;
} Mk8Xess2D;

typedef Mk8Xess2D Mk8XessCoord;

typedef struct Mk8XessImageView {
    VkImageView imageView;
    VkImage image;
    VkImageSubresourceRange subresourceRange;
    VkFormat format;
    unsigned int width;
    unsigned int height;
} Mk8XessImageView;

typedef struct Mk8XessExecuteParams {
    Mk8XessImageView colorTexture;
    Mk8XessImageView velocityTexture;
    Mk8XessImageView depthTexture;
    Mk8XessImageView exposureScaleTexture;
    Mk8XessImageView responsivePixelMaskTexture;
    Mk8XessImageView outputTexture;

    float jitterOffsetX;
    float jitterOffsetY;
    float exposureScale;
    uint32_t resetHistory;
    uint32_t inputWidth;
    uint32_t inputHeight;

    Mk8XessCoord inputColorBase;
    Mk8XessCoord inputMotionVectorBase;
    Mk8XessCoord inputDepthBase;
    Mk8XessCoord inputResponsiveMaskBase;
    Mk8XessCoord reserved0;
    Mk8XessCoord outputColorBase;
} Mk8XessExecuteParams;

typedef struct Mk8XessInitParams {
    Mk8Xess2D outputResolution;
    int qualitySetting;
    uint32_t initFlags;
    uint32_t creationNodeMask;
    uint32_t visibleNodeMask;
    VkDeviceMemory tempBufferHeap;
    uint64_t bufferHeapOffset;
    VkDeviceMemory tempTextureHeap;
    uint64_t textureHeapOffset;
    VkPipelineCache pipelineCache;
} Mk8XessInitParams;

#pragma pack(pop)

// xess_result_t: negative values are errors, 1/2 are warnings.
enum {
    MK8_XESS_RESULT_WARNING_NONEXISTING_FOLDER = 1,
    MK8_XESS_RESULT_WARNING_OLD_DRIVER = 2,
    MK8_XESS_RESULT_SUCCESS = 0,
    MK8_XESS_RESULT_ERROR_UNSUPPORTED_DEVICE = -1,
    MK8_XESS_RESULT_ERROR_UNSUPPORTED_DRIVER = -2,
    MK8_XESS_RESULT_ERROR_UNINITIALIZED = -3,
    MK8_XESS_RESULT_ERROR_INVALID_ARGUMENT = -4,
    MK8_XESS_RESULT_ERROR_DEVICE_OUT_OF_MEMORY = -5,
    MK8_XESS_RESULT_ERROR_DEVICE = -6,
    MK8_XESS_RESULT_ERROR_NOT_IMPLEMENTED = -7,
    MK8_XESS_RESULT_ERROR_INVALID_CONTEXT = -8,
    MK8_XESS_RESULT_ERROR_OPERATION_IN_PROGRESS = -9,
    MK8_XESS_RESULT_ERROR_UNSUPPORTED = -10,
    MK8_XESS_RESULT_ERROR_CANT_LOAD_LIBRARY = -11,
    MK8_XESS_RESULT_ERROR_WRONG_CALL_ORDER = -12,
    MK8_XESS_RESULT_ERROR_UNKNOWN = -1000,
};

// xess_quality_settings_t.
enum {
    MK8_XESS_QUALITY_ULTRA_PERFORMANCE = 100,
    MK8_XESS_QUALITY_PERFORMANCE = 101,
    MK8_XESS_QUALITY_BALANCED = 102,
    MK8_XESS_QUALITY_QUALITY = 103,
    MK8_XESS_QUALITY_ULTRA_QUALITY = 104,
    MK8_XESS_QUALITY_ULTRA_QUALITY_PLUS = 105,
    MK8_XESS_QUALITY_AA = 106,
};

// xess_init_flags_t.
enum {
    MK8_XESS_INIT_FLAG_NONE = 0,
    MK8_XESS_INIT_FLAG_HIGH_RES_MV = 1 << 0,
    MK8_XESS_INIT_FLAG_INVERTED_DEPTH = 1 << 1,
    MK8_XESS_INIT_FLAG_EXPOSURE_SCALE_TEXTURE = 1 << 2,
    MK8_XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK = 1 << 3,
    MK8_XESS_INIT_FLAG_USE_NDC_VELOCITY = 1 << 4,
    MK8_XESS_INIT_FLAG_EXTERNAL_DESCRIPTOR_HEAP = 1 << 5,
    MK8_XESS_INIT_FLAG_LDR_INPUT_COLOR = 1 << 6,
    MK8_XESS_INIT_FLAG_JITTERED_MV = 1 << 7,
    MK8_XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE = 1 << 8,
};

typedef int (*Mk8XessVKCreateContextFn)(VkInstance instance, VkPhysicalDevice physicalDevice,
                                       VkDevice device, Mk8XessContextHandle* out);
typedef int (*Mk8XessVKInitFn)(Mk8XessContextHandle context, const Mk8XessInitParams* params);
typedef int (*Mk8XessVKExecuteFn)(Mk8XessContextHandle context, VkCommandBuffer commandBuffer,
                                 const Mk8XessExecuteParams* params);
typedef int (*Mk8XessDestroyContextFn)(Mk8XessContextHandle context);
typedef int (*Mk8XessVKGetRequiredInstanceExtensionsFn)(uint32_t* count, const char* const** names,
                                                       uint32_t* minVkApiVersion);
typedef int (*Mk8XessVKGetRequiredDeviceExtensionsFn)(VkInstance instance, VkPhysicalDevice physicalDevice,
                                                     uint32_t* count, const char* const** names);
typedef int (*Mk8XessVKGetRequiredDeviceFeaturesFn)(VkInstance instance, VkPhysicalDevice physicalDevice,
                                                   void** features);
typedef int (*Mk8XessGetVersionFn)(Mk8XessVersion* version);

// Symbol names looked up with GetProcAddress. Kept as macros so the probe and the
// tests cannot drift from the loader.
#define MK8_XESS_SYM_CREATE "xessVKCreateContext"
#define MK8_XESS_SYM_INIT "xessVKInit"
#define MK8_XESS_SYM_EXECUTE "xessVKExecute"
#define MK8_XESS_SYM_DESTROY "xessDestroyContext"
#define MK8_XESS_SYM_INSTANCE_EXTENSIONS "xessVKGetRequiredInstanceExtensions"
#define MK8_XESS_SYM_DEVICE_EXTENSIONS "xessVKGetRequiredDeviceExtensions"
#define MK8_XESS_SYM_DEVICE_FEATURES "xessVKGetRequiredDeviceFeatures"
#define MK8_XESS_SYM_GET_VERSION "xessGetVersion"

#ifdef __cplusplus
}
#endif
