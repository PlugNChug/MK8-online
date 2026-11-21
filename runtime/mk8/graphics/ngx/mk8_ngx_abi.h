// Minimal NGX (DLSS) Vulkan ABI used by runtime/mk8/graphics/dlss.cpp.
//
// This is NOT a copy of NVIDIA's headers. The DLSS SDK headers are published as
// SPDX-License-Identifier: LicenseRef-NvidiaProprietary (checked on v310.9.16), so this
// project does not commit them, for the same reason it does not commit the .so files.
// This file lists only the interface facts the bridge needs: entry-point names, numeric
// enum values, parameter names and field order. The downloaded SDK under
// $XDG_DATA_HOME/mk8online/nvidia/ngx/include/ is the normative reference;
// tests/test_dlss.py compares these layouts against it with static_asserts when present.
//
// Only x86-64 little-endian is targeted (the project's platforms); the layouts below are
// the ones both the Itanium and the MSVC C++ ABIs produce for these scalar members.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#define MK8_NGX_CONV __cdecl
#else
#define MK8_NGX_CONV
#endif

// NGX SDK API version (NVSDK_NGX_VERSION_API_MACRO in nvsdk_ngx_defs.h, v310.9.16).
#define MK8_NGX_API_VERSION 0x0000015

typedef struct Mk8NgxParameter Mk8NgxParameter;
typedef struct Mk8NgxHandle Mk8NgxHandle;

// NVSDK_NGX_Result
enum {
    MK8_NGX_RESULT_SUCCESS = 0x1,
    MK8_NGX_RESULT_FAIL = 0xBAD00000,
    MK8_NGX_RESULT_FAIL_NOT_INITIALIZED = 0xBAD00007,
    MK8_NGX_RESULT_FAIL_FEATURE_NOT_SUPPORTED = 0xBAD00001,
    MK8_NGX_RESULT_FAIL_PLATFORM_ERROR = 0xBAD00002,
    MK8_NGX_RESULT_FAIL_INVALID_PARAMETER = 0xBAD00005,
    MK8_NGX_RESULT_FAIL_MISSING_INPUT = 0xBAD0000A,
    MK8_NGX_RESULT_FAIL_UNABLE_TO_INITIALIZE_FEATURE = 0xBAD0000B,
    MK8_NGX_RESULT_FAIL_UNABLE_TO_WRITE_TO_APP_DATA_PATH = 0xBAD0000F,
};
#define MK8_NGX_FAILED(result) (((result) & 0xFFF00000) == MK8_NGX_RESULT_FAIL)
#define MK8_NGX_SUCCEEDED(result) (!MK8_NGX_FAILED(result))

// NVSDK_NGX_Feature / NVSDK_NGX_EngineType / NVSDK_NGX_Application_Identifier_Type
enum { MK8_NGX_FEATURE_SUPER_SAMPLING = 1, MK8_NGX_FEATURE_FRAME_GENERATION = 11 };
enum { MK8_NGX_ENGINE_TYPE_CUSTOM = 0 };
enum { MK8_NGX_IDENTIFIER_PROJECT_ID = 1 };

// NVSDK_NGX_PerfQuality_Value
enum {
    MK8_NGX_PERFQUALITY_MAX_PERF = 0,
    MK8_NGX_PERFQUALITY_BALANCED = 1,
    MK8_NGX_PERFQUALITY_MAX_QUALITY = 2,
    MK8_NGX_PERFQUALITY_ULTRA_PERFORMANCE = 3,
    MK8_NGX_PERFQUALITY_ULTRA_QUALITY = 4,
    MK8_NGX_PERFQUALITY_DLAA = 5,
};

// NVSDK_NGX_Feature_Support_Result (bitfield in NVSDK_NGX_FeatureRequirement)
enum {
    MK8_NGX_FEATURE_SUPPORTED = 0,
    MK8_NGX_FEATURE_CHECK_NOT_PRESENT = 1,
    MK8_NGX_FEATURE_DRIVER_VERSION_UNSUPPORTED = 2,
    MK8_NGX_FEATURE_ADAPTER_UNSUPPORTED = 4,
    MK8_NGX_FEATURE_OS_VERSION_BELOW_MINIMUM = 8,
    MK8_NGX_FEATURE_NOT_IMPLEMENTED = 16,
};

// NVSDK_NGX_DLSS_Feature_Flags
#define MK8_NGX_DLSS_FLAG_NONE 0
#define MK8_NGX_DLSS_FLAG_IS_HDR (1 << 0)
#define MK8_NGX_DLSS_FLAG_MV_LOW_RES (1 << 1)
#define MK8_NGX_DLSS_FLAG_MV_JITTERED (1 << 2)
#define MK8_NGX_DLSS_FLAG_DEPTH_INVERTED (1 << 3)
#define MK8_NGX_DLSS_FLAG_ALPHA_UPSCALING (1 << 7)

// Parameter names (NVSDK_NGX_Parameter_* in nvsdk_ngx_defs.h).
#define MK8_NGX_PARAM_WIDTH "Width"
#define MK8_NGX_PARAM_HEIGHT "Height"
#define MK8_NGX_PARAM_OUT_WIDTH "OutWidth"
#define MK8_NGX_PARAM_OUT_HEIGHT "OutHeight"
#define MK8_NGX_PARAM_PERF_QUALITY "PerfQualityValue"
#define MK8_NGX_PARAM_CREATION_NODE_MASK "CreationNodeMask"
#define MK8_NGX_PARAM_VISIBILITY_NODE_MASK "VisibilityNodeMask"
#define MK8_NGX_PARAM_DLSS_CREATE_FLAGS "DLSS.Feature.Create.Flags"
#define MK8_NGX_PARAM_DLSS_ENABLE_OUTPUT_SUBRECTS "DLSS.Enable.Output.Subrects"
#define MK8_NGX_PARAM_SUPER_SAMPLING_AVAILABLE "SuperSampling.Available"
#define MK8_NGX_PARAM_SUPER_SAMPLING_FEATURE_INIT_RESULT "SuperSampling.FeatureInitResult"
#define MK8_NGX_PARAM_COLOR "Color"
#define MK8_NGX_PARAM_OUTPUT "Output"
#define MK8_NGX_PARAM_DEPTH "Depth"
#define MK8_NGX_PARAM_MOTION_VECTORS "MotionVectors"
#define MK8_NGX_PARAM_JITTER_X "Jitter.Offset.X"
#define MK8_NGX_PARAM_JITTER_Y "Jitter.Offset.Y"
#define MK8_NGX_PARAM_MV_SCALE_X "MV.Scale.X"
#define MK8_NGX_PARAM_MV_SCALE_Y "MV.Scale.Y"
#define MK8_NGX_PARAM_RESET "Reset"
#define MK8_NGX_PARAM_SHARPNESS "Sharpness"
#define MK8_NGX_PARAM_PRE_EXPOSURE "DLSS.Pre.Exposure"
#define MK8_NGX_PARAM_FRAME_TIME_DELTA_MS "FrameTimeDeltaInMsec"
#define MK8_NGX_PARAM_RENDER_SUBRECT_WIDTH "DLSS.Render.Subrect.Dimensions.Width"
#define MK8_NGX_PARAM_RENDER_SUBRECT_HEIGHT "DLSS.Render.Subrect.Dimensions.Height"

// DLSS Frame Generation (nvsdk_ngx_defs_dlssg.h / nvsdk_ngx_params_dlssg.h).
#define MK8_NGX_PARAM_FRAME_GENERATION_AVAILABLE "FrameGeneration.Available"
#define MK8_NGX_PARAM_FRAME_GENERATION_FEATURE_INIT_RESULT "FrameGeneration.FeatureInitResult"
#define MK8_NGX_PARAM_FRAME_GENERATION_NEEDS_UPDATED_DRIVER "FrameGeneration.NeedsUpdatedDriver"
#define MK8_NGX_PARAM_FRAME_GENERATION_MIN_DRIVER_MAJOR "FrameGeneration.MinDriverVersionMajor"
#define MK8_NGX_PARAM_FRAME_GENERATION_MIN_DRIVER_MINOR "FrameGeneration.MinDriverVersionMinor"
#define MK8_NGX_DLSSG_BACKBUFFER_FORMAT "DLSSG.BackbufferFormat"
#define MK8_NGX_DLSSG_BACKBUFFER "DLSSG.Backbuffer"
#define MK8_NGX_DLSSG_MVECS "DLSSG.MVecs"
#define MK8_NGX_DLSSG_DEPTH "DLSSG.Depth"
#define MK8_NGX_DLSSG_HUDLESS "DLSSG.HUDLess"
#define MK8_NGX_DLSSG_UI "DLSSG.UI"
#define MK8_NGX_DLSSG_UI_ALPHA "DLSSG.UIAlpha"
#define MK8_NGX_DLSSG_BIDIRECTIONAL_DISTORTION_FIELD "DLSSG.BidirectionalDistortionField"
#define MK8_NGX_DLSSG_OUTPUT_INTERPOLATED "DLSSG.OutputInterpolated"
#define MK8_NGX_DLSSG_OUTPUT_REAL "DLSSG.OutputReal"
#define MK8_NGX_DLSSG_OUTPUT_DISABLE_INTERPOLATION "DLSSG.OutputDisableInterpolation"
#define MK8_NGX_DLSSG_MULTI_FRAME_COUNT "DLSSG.MultiFrameCount"
#define MK8_NGX_DLSSG_MULTI_FRAME_INDEX "DLSSG.MultiFrameIndex"
#define MK8_NGX_DLSSG_BACKBUFFER_FRAME_ID "DLSSG.BackbufferFrameID"
#define MK8_NGX_DLSSG_CAMERA_VIEW_TO_CLIP "DLSSG.CameraViewToClip"
#define MK8_NGX_DLSSG_CLIP_TO_CAMERA_VIEW "DLSSG.ClipToCameraView"
#define MK8_NGX_DLSSG_CLIP_TO_LENS_CLIP "DLSSG.ClipToLensClip"
#define MK8_NGX_DLSSG_CLIP_TO_PREV_CLIP "DLSSG.ClipToPrevClip"
#define MK8_NGX_DLSSG_PREV_CLIP_TO_CLIP "DLSSG.PrevClipToClip"
#define MK8_NGX_DLSSG_JITTER_OFFSET_X "DLSSG.JitterOffsetX"
#define MK8_NGX_DLSSG_JITTER_OFFSET_Y "DLSSG.JitterOffsetY"
#define MK8_NGX_DLSSG_MVEC_SCALE_X "DLSSG.MvecScaleX"
#define MK8_NGX_DLSSG_MVEC_SCALE_Y "DLSSG.MvecScaleY"
#define MK8_NGX_DLSSG_CAMERA_PINHOLE_OFFSET_X "DLSSG.CameraPinholeOffsetX"
#define MK8_NGX_DLSSG_CAMERA_PINHOLE_OFFSET_Y "DLSSG.CameraPinholeOffsetY"
#define MK8_NGX_DLSSG_CAMERA_POS_X "DLSSG.CameraPosX"
#define MK8_NGX_DLSSG_CAMERA_POS_Y "DLSSG.CameraPosY"
#define MK8_NGX_DLSSG_CAMERA_POS_Z "DLSSG.CameraPosZ"
#define MK8_NGX_DLSSG_CAMERA_UP_X "DLSSG.CameraUpX"
#define MK8_NGX_DLSSG_CAMERA_UP_Y "DLSSG.CameraUpY"
#define MK8_NGX_DLSSG_CAMERA_UP_Z "DLSSG.CameraUpZ"
#define MK8_NGX_DLSSG_CAMERA_RIGHT_X "DLSSG.CameraRightX"
#define MK8_NGX_DLSSG_CAMERA_RIGHT_Y "DLSSG.CameraRightY"
#define MK8_NGX_DLSSG_CAMERA_RIGHT_Z "DLSSG.CameraRightZ"
#define MK8_NGX_DLSSG_CAMERA_FWD_X "DLSSG.CameraFwdX"
#define MK8_NGX_DLSSG_CAMERA_FWD_Y "DLSSG.CameraFwdY"
#define MK8_NGX_DLSSG_CAMERA_FWD_Z "DLSSG.CameraFwdZ"
#define MK8_NGX_DLSSG_CAMERA_NEAR "DLSSG.CameraNear"
#define MK8_NGX_DLSSG_CAMERA_FAR "DLSSG.CameraFar"
#define MK8_NGX_DLSSG_CAMERA_FOV "DLSSG.CameraFOV"
#define MK8_NGX_DLSSG_CAMERA_ASPECT_RATIO "DLSSG.CameraAspectRatio"
#define MK8_NGX_DLSSG_COLOR_BUFFERS_HDR "DLSSG.ColorBuffersHDR"
#define MK8_NGX_DLSSG_DEPTH_INVERTED "DLSSG.DepthInverted"
#define MK8_NGX_DLSSG_CAMERA_MOTION_INCLUDED "DLSSG.CameraMotionIncluded"
#define MK8_NGX_DLSSG_RESET "DLSSG.Reset"
#define MK8_NGX_DLSSG_AUTOMODE_OVERRIDE_RESET "DLSSG.AutomodeOverrideReset"
#define MK8_NGX_DLSSG_NOT_RENDERING_GAME_FRAMES "DLSSG.NotRenderingGameFrames"
#define MK8_NGX_DLSSG_ORTHO_PROJECTION "DLSSG.OrthoProjection"
#define MK8_NGX_DLSSG_MVEC_INVALID_VALUE "DLSSG.MvecInvalidValue"
#define MK8_NGX_DLSSG_MVEC_DILATED "DLSSG.MvecDilated"
#define MK8_NGX_DLSSG_MENU_DETECTION_ENABLED "DLSSG.MenuDetectionEnabled"
#define MK8_NGX_DLSSG_INTERNAL_WIDTH "DLSSG.InternalWidth"
#define MK8_NGX_DLSSG_INTERNAL_HEIGHT "DLSSG.InternalHeight"
#define MK8_NGX_DLSSG_DYNAMIC_RESOLUTION "DLSSG.DynamicResolution"

// NVSDK_NGX_PathListInfo
typedef struct Mk8NgxPathListInfo {
    const wchar_t *const *Path;
    unsigned Length;
} Mk8NgxPathListInfo;

// NVSDK_NGX_LoggingInfo. The callback signature is
// void(const char *, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature); both enums are int.
typedef struct Mk8NgxLoggingInfo {
    void(MK8_NGX_CONV *LoggingCallback)(const char *, int, int);
    int MinimumLoggingLevel;
    bool DisableOtherLoggingSinks;
} Mk8NgxLoggingInfo;

// NVSDK_NGX_FeatureCommonInfo
typedef struct Mk8NgxFeatureCommonInfo {
    Mk8NgxPathListInfo PathListInfo;
    void *InternalData;
    Mk8NgxLoggingInfo LoggingInfo;
} Mk8NgxFeatureCommonInfo;

// NVSDK_NGX_ProjectIdDescription / NVSDK_NGX_Application_Identifier
typedef struct Mk8NgxProjectIdDescription {
    const char *ProjectId;
    int EngineType;
    const char *EngineVersion;
} Mk8NgxProjectIdDescription;

typedef struct Mk8NgxApplicationIdentifier {
    int IdentifierType;
    union {
        Mk8NgxProjectIdDescription ProjectDesc;
        unsigned long long ApplicationId;
    } v;
} Mk8NgxApplicationIdentifier;

// NVSDK_NGX_FeatureDiscoveryInfo
typedef struct Mk8NgxFeatureDiscoveryInfo {
    int SDKVersion;
    int FeatureID;
    Mk8NgxApplicationIdentifier Identifier;
    const wchar_t *ApplicationDataPath;
    const Mk8NgxFeatureCommonInfo *FeatureInfo;
} Mk8NgxFeatureDiscoveryInfo;

// NVSDK_NGX_FeatureRequirement
typedef struct Mk8NgxFeatureRequirement {
    int FeatureSupported;
    unsigned MinHWArchitecture;
    char MinOSVersion[255];
} Mk8NgxFeatureRequirement;

// NVSDK_NGX_Coordinates / NVSDK_NGX_Dimensions / NVSDK_NGX_PrecisionInfo
typedef struct Mk8NgxCoordinates {
    unsigned X;
    unsigned Y;
} Mk8NgxCoordinates;

typedef struct Mk8NgxDimensions {
    unsigned Width;
    unsigned Height;
} Mk8NgxDimensions;

typedef struct Mk8NgxPrecisionInfo {
    unsigned IsLowPrecision;
    float Bias;
    float Scale;
} Mk8NgxPrecisionInfo;

// NVSDK_NGX_ImageViewInfo_VK / NVSDK_NGX_BufferInfo_VK / NVSDK_NGX_Resource_VK
typedef struct Mk8NgxImageViewInfoVk {
    VkImageView ImageView;
    VkImage Image;
    VkImageSubresourceRange SubresourceRange;
    VkFormat Format;
    unsigned Width;
    unsigned Height;
} Mk8NgxImageViewInfoVk;

typedef struct Mk8NgxBufferInfoVk {
    VkBuffer Buffer;
    unsigned SizeInBytes;
} Mk8NgxBufferInfoVk;

typedef struct Mk8NgxResourceVk {
    union {
        Mk8NgxImageViewInfoVk ImageViewInfo;
        Mk8NgxBufferInfoVk BufferInfo;
    } Resource;
    int Type;  // NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 0, _VK_BUFFER = 1
    bool ReadWrite;
} Mk8NgxResourceVk;

// NVSDK_NGX_Feature_Create_Params / NVSDK_NGX_DLSS_Create_Params (layout check only;
// the bridge sets the individual parameters, as the SDK helper does).
typedef struct Mk8NgxDlssCreateParams {
    struct {
        unsigned InWidth;
        unsigned InHeight;
        unsigned InTargetWidth;
        unsigned InTargetHeight;
        int InPerfQualityValue;
    } Feature;
    int InFeatureCreateFlags;
    bool InEnableOutputSubrects;
} Mk8NgxDlssCreateParams;

// NVSDK_NGX_DLSSG_Create_Params (nvsdk_ngx_params_dlssg.h).
typedef struct Mk8NgxDlssgCreateParams {
    unsigned Width;
    unsigned Height;
    unsigned NativeBackbufferFormat;
    unsigned RenderWidth;
    unsigned RenderHeight;
    bool DynamicResolutionScaling;
} Mk8NgxDlssgCreateParams;

// NVSDK_NGX_DLSSG_Opt_Eval_Params. The official struct has C++ default member initializers;
// they do not affect the layout, and the bridge fills every member it passes.
typedef struct Mk8NgxDlssgOptEvalParams {
    unsigned multiFrameCount;
    unsigned multiFrameIndex;
    float cameraViewToClip[4][4];
    float clipToCameraView[4][4];
    float clipToLensClip[4][4];
    float clipToPrevClip[4][4];
    float prevClipToClip[4][4];
    float jitterOffset[2];
    float mvecScale[2];
    float cameraPinholeOffset[2];
    float cameraPos[3];
    float cameraUp[3];
    float cameraRight[3];
    float cameraFwd[3];
    float cameraNear;
    float cameraFar;
    float cameraFOV;
    float cameraAspectRatio;
    bool colorBuffersHDR;
    bool depthInverted;
    bool cameraMotionIncluded;
    bool reset;
    bool automodeOverrideReset;
    bool notRenderingGameFrames;
    bool orthoProjection;
    float motionVectorsInvalidValue;
    bool motionVectorsDilated;
    bool menuDetectionEnabled;
    Mk8NgxCoordinates mvecsSubrectBase;
    Mk8NgxDimensions mvecsSubrectSize;
    Mk8NgxCoordinates depthSubrectBase;
    Mk8NgxDimensions depthSubrectSize;
    Mk8NgxCoordinates hudLessSubrectBase;
    Mk8NgxDimensions hudLessSubrectSize;
    Mk8NgxCoordinates uiSubrectBase;
    Mk8NgxDimensions uiSubrectSize;
    Mk8NgxCoordinates uiAlphaSubrectBase;
    Mk8NgxDimensions uiAlphaSubrectSize;
    Mk8NgxCoordinates bidirectionalDistFieldSubrectBase;
    Mk8NgxDimensions bidirectionalDistFieldSubrectSize;
    Mk8NgxPrecisionInfo bidirectionalDistFieldPrecisionInfo;
    float minRelativeLinearDepthObjectSeparation;
    Mk8NgxCoordinates backbufferSubrectBase;
    Mk8NgxDimensions backbufferSubrectSize;
    Mk8NgxCoordinates outputInterpSubrectBase;
    Mk8NgxDimensions outputInterpSubrectSize;
    Mk8NgxCoordinates outputRealSubrectBase;
    Mk8NgxDimensions outputRealSubrectSize;
} Mk8NgxDlssgOptEvalParams;

// Entry points. The feature plugin exports CreateFeature*/EvaluateFeature/ReleaseFeature/
// GetFeatureRequirements; the driver-provided NGX core exports Init/parameters/Shutdown.
typedef int(MK8_NGX_CONV *Mk8NgxInitWithProjectIdFn)(
    const char *project_id, int engine_type, const char *engine_version,
    const wchar_t *application_data_path, VkInstance instance, VkPhysicalDevice physical_device,
    VkDevice device, PFN_vkGetInstanceProcAddr get_instance_proc_addr,
    PFN_vkGetDeviceProcAddr get_device_proc_addr, const Mk8NgxFeatureCommonInfo *feature_info,
    int sdk_version);

// Windows driver cores export this nine-argument form instead of the SDK helper above: the
// two vkGet*ProcAddr pointers are absent and the SDK version precedes FeatureCommonInfo.
// Calling the eleven-argument shape on such a core passes the engine-version string where the
// path list belongs and faults inside _nvngx.dll (verified on 32.0.16.2101).
typedef int(MK8_NGX_CONV *Mk8NgxInitProjectIdFn)(
    const char *project_id, int engine_type, const char *engine_version,
    const wchar_t *application_data_path, VkInstance instance, VkPhysicalDevice physical_device,
    VkDevice device, int sdk_version, const Mk8NgxFeatureCommonInfo *feature_info);

// The Windows runtime exports no NVSDK_NGX_Parameter_SetUI/SetI/SetF/SetVoidPointer/GetUI.
// Parameters are the NVSDK_NGX_Parameter C++ object; MSVC's overload order is not the
// header's declaration order. These are the slots the bridge uses (verified on 621.01):
// 0 Set(void*), 3 Set(uint), 4 Set(int), 6 Set(float), 7 Set(ull),
// 11 Get(uint), 15 Get(ull). Linux keeps the exported C helpers.
enum {
    MK8_NGX_WIN_VTABLE_SET_POINTER = 0,
    MK8_NGX_WIN_VTABLE_SET_UI = 3,
    MK8_NGX_WIN_VTABLE_SET_I = 4,
    MK8_NGX_WIN_VTABLE_SET_F = 6,
    MK8_NGX_WIN_VTABLE_SET_ULL = 7,
    MK8_NGX_WIN_VTABLE_GET_UI = 11,
    MK8_NGX_WIN_VTABLE_GET_ULL = 15,
};
typedef int(MK8_NGX_CONV *Mk8NgxGetCapabilityParametersFn)(Mk8NgxParameter **out_parameters);
typedef void(MK8_NGX_CONV *Mk8NgxParameterSetUiFn)(Mk8NgxParameter *, const char *, unsigned);
typedef void(MK8_NGX_CONV *Mk8NgxParameterSetIFn)(Mk8NgxParameter *, const char *, int);
typedef void(MK8_NGX_CONV *Mk8NgxParameterSetFFn)(Mk8NgxParameter *, const char *, float);
typedef void(MK8_NGX_CONV *Mk8NgxParameterSetVoidPointerFn)(Mk8NgxParameter *, const char *, void *);
typedef void(MK8_NGX_CONV *Mk8NgxParameterSetUllFn)(Mk8NgxParameter *, const char *, unsigned long long);
typedef int(MK8_NGX_CONV *Mk8NgxParameterGetUiFn)(Mk8NgxParameter *, const char *, unsigned *);
typedef int(MK8_NGX_CONV *Mk8NgxParameterGetUllFn)(Mk8NgxParameter *, const char *, unsigned long long *);
typedef int(MK8_NGX_CONV *Mk8NgxCreateFeature1Fn)(VkDevice, VkCommandBuffer, int feature, Mk8NgxParameter *,
                                                 Mk8NgxHandle **);
typedef int(MK8_NGX_CONV *Mk8NgxCreateFeatureFn)(VkCommandBuffer, int feature, Mk8NgxParameter *, Mk8NgxHandle **);
typedef int(MK8_NGX_CONV *Mk8NgxEvaluateFeatureFn)(VkCommandBuffer, const Mk8NgxHandle *, Mk8NgxParameter *, void *);
typedef int(MK8_NGX_CONV *Mk8NgxReleaseFeatureFn)(Mk8NgxHandle *);
typedef int(MK8_NGX_CONV *Mk8NgxShutdown1Fn)(VkDevice);
typedef int(MK8_NGX_CONV *Mk8NgxGetFeatureRequirementsFn)(VkInstance, VkPhysicalDevice,
                                                          const Mk8NgxFeatureDiscoveryInfo *,
                                                          Mk8NgxFeatureRequirement *);
typedef int(MK8_NGX_CONV *Mk8NgxRequiredExtensionsFn)(unsigned *instance_count, const char ***instance_names,
                                                     unsigned *device_count, const char ***device_names);
