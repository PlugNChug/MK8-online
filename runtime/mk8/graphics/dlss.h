// Optional NVIDIA DLSS Super Resolution / DLAA bridge (host side, no guest changes).
//
// The loader is deliberately independent of Vulkan: mk8_dlss_probe() only resolves paths,
// dlopens the NGX feature plugin and core, and checks their exported entry points. It runs
// (and is tested) on a machine without an NVIDIA GPU. The upscale path additionally needs
// the runtime's Vulkan device and is compiled out with -DMK8_DLSS_PROBE_ONLY.
//
// Nothing NVIDIA is bundled: the plugin and core come from the driver and from
// tools/fetch_vendor_sdk.py ($XDG_DATA_HOME/mk8online/nvidia). Every failure returns
// VK_NULL_HANDLE with one reasoned log line, so the caller keeps the existing path.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MK8_DLSS_PATH_MAX 1024
#define MK8_DLSS_REASON_MAX 512

// NVSDK_NGX_Feature_Support_Result bits (the same numeric values for every feature).
#define MK8_DLSS_SUPPORT_CHECK_NOT_PRESENT 1u
#define MK8_DLSS_SUPPORT_DRIVER_VERSION_UNSUPPORTED 2u
#define MK8_DLSS_SUPPORT_ADAPTER_UNSUPPORTED 4u
#define MK8_DLSS_SUPPORT_OS_VERSION_BELOW_MINIMUM 8u
#define MK8_DLSS_SUPPORT_NOT_IMPLEMENTED 16u

// Why the loader cannot be used. MK8_DLSS_OK is the only usable state.
typedef enum Mk8DlssLoadStatus {
    MK8_DLSS_OK = 0,
    MK8_DLSS_NO_PLUGIN,              // no libnvidia-ngx-dlss.so* (nvngx_dlss.dll) in the directory
    MK8_DLSS_PLUGIN_LOAD_FAILED,     // the file exists but dlopen/LoadLibrary failed (corrupt, wrong arch)
    MK8_DLSS_PLUGIN_EXPORTS_MISSING, // loaded, but it is not a DLSS feature plugin
    MK8_DLSS_NO_CORE,                // no driver-provided NGX core (libnvidia-ngx.so.1 / _nvngx.dll)
    MK8_DLSS_CORE_LOAD_FAILED,       // the core exists but did not load
    MK8_DLSS_CORE_EXPORTS_MISSING,   // an NGX core without the Vulkan API this bridge calls
} Mk8DlssLoadStatus;

typedef struct Mk8DlssProbe {
    Mk8DlssLoadStatus status;
    bool plugin_loaded;
    bool plugin_exports_ok;
    bool core_loaded;
    bool core_exports_ok;
    bool licence_present; // LICENSE.txt beside the plugin (the downloader stores it there)
    char directory[MK8_DLSS_PATH_MAX]; // where the plugin was searched for
    char plugin[MK8_DLSS_PATH_MAX];    // resolved plugin path ("" when none was found)
    char core[MK8_DLSS_PATH_MAX];      // resolved core path ("" when none was found)
    char reason[MK8_DLSS_REASON_MAX];  // human-readable, one sentence
} Mk8DlssProbe;

#ifdef __cplusplus
extern "C" {
#endif

// Resolves MK8_DLSS_DIR/MK8_DLSS_LIB/MK8_DLSS_CORE (or the default download directory),
// dlopens the plugin and core and checks their exports. Opens and closes everything, so it
// is safe to call repeatedly with different environments. Never touches Vulkan objects.
// Returns true only when `out->status == MK8_DLSS_OK`.
bool mk8_dlss_probe(Mk8DlssProbe *out);

// Stable name for logs and tests: "ok", "no-plugin", "plugin-load-failed", ...
const char *mk8_dlss_status_name(Mk8DlssLoadStatus status);

// Prints exactly one line: "[dlss] DLSS unavailable: <reason>\n".
void mk8_dlss_log_unavailable(const Mk8DlssProbe *probe);

// DLSS Frame Generation (MK8_DLSS_FG=1). The probe and gate are independent of Vulkan so
// they run on a machine without an NVIDIA GPU; the feature path is compiled out with
// -DMK8_DLSS_PROBE_ONLY.
bool mk8_dlss_fg_enabled(void);
bool mk8_dlss_fg_probe(Mk8DlssProbe *out);
const char *mk8_dlss_fg_status_name(Mk8DlssLoadStatus status);
// Prints exactly one line: "[dlss_fg] DLSS Frame Generation unavailable: <reason>\n".
void mk8_dlss_fg_log_unavailable(const Mk8DlssProbe *probe);
// Decodes NVSDK_NGX_Feature_Support_Result bits (driver too old, GPU unsupported, ...).
// Returns "supported" for 0 and fills `buffer` otherwise. Never returns NULL.
const char *mk8_dlss_requirement_text(unsigned bits, char *buffer, size_t size);

#ifndef MK8_DLSS_PROBE_ONLY
#include <vulkan/vulkan.h>

typedef struct TexBackend TexBackend;

// Records one DLSS Super Resolution / DLAA evaluation into cb and returns the display-size
// output view, or VK_NULL_HANDLE (one-time log) so the caller keeps the existing path.
// Mirrors mk8_fsr4_upscale's flow and consumes the same colour/depth/jitter inputs.
VkImageView mk8_dlss_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw,
                             uint32_t rh, uint32_t ow, uint32_t oh, float frame_ms);

// ---------------------------------------------------------------------------------------
// DLSS Frame Generation present-time API.
//
// NGX DLSS-G does not present and does not replace the swapchain: the application keeps its
// normal swapchain, evaluates the feature into one display-size interpolated image and
// presents that image after the real (backbuffer) frame. This module owns the feature and the
// interpolated image; the WSI images, semaphores and queue submission stay with the caller.
// The real frame is the caller's backbuffer and is not modified (OutputReal is not set).
//
// Everything is gated on MK8_DLSS_FG=1. No function below is called by the runtime today;
// with the variable unset the module has no effect on the existing FSR FG/upscaler paths.
// ---------------------------------------------------------------------------------------

typedef struct Mk8DlssFgInit {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue present_queue;
    // An open command buffer on the present queue. It is used once to create the feature and
    // prime the output images and must be submitted before the first evaluate.
    VkCommandBuffer command_buffer;
    VkSwapchainKHR swapchain;
    VkFormat format;      // swapchain/backbuffer format (also the two output images' format)
    uint32_t width, height;              // display size (what DLSS-G outputs)
    uint32_t render_width, render_height; // game render size (for internal-resolution hints)
    bool color_hdr;       // tagged color buffers are HDR
    bool depth_inverted;  // depth is reverse-Z (value closer to the camera is larger)
} Mk8DlssFgInit;

typedef struct Mk8DlssFgInputs {
    VkCommandBuffer command_buffer;
    // The final display-size image of the current rendered frame (after upscaling/UI).
    VkImage backbuffer;
    VkImageView backbuffer_view;
    VkFormat backbuffer_format;
    VkImageLayout backbuffer_layout; // current layout; restored after EvaluateFeature
    uint32_t backbuffer_width, backbuffer_height;
    VkImage depth;
    VkImageView depth_view;
    VkFormat depth_format;
    VkImageLayout depth_layout;
    uint32_t depth_width, depth_height;
    VkImageAspectFlags depth_aspect;
    VkImage motion;
    VkImageView motion_view;
    VkFormat motion_format;
    VkImageLayout motion_layout;
    uint32_t motion_width, motion_height;
    // Jitter in render pixels (the same values the FSR bridge uses); converted to clip space.
    float jitter_x, jitter_y;
    // Row-major view (3x4) and projection (4x4) matrices from the main draw (toolchain/fsr.py).
    float view[12];
    float projection[16];
    // Optional camera parameters; pass 0 to derive FOV/aspect from the projection and use
    // the bridge defaults (0.1 / 1000) for near/far.
    float camera_near, camera_far, camera_fov, camera_aspect;
    uint64_t frame_id; // increments once per rendered frame, even while DLSS-G is paused
    bool reset;        // cut, mode change, or invalid history
    bool not_rendering; // menu/pause: DLSS-G should not interpolate
} Mk8DlssFgInputs;

typedef struct Mk8DlssFgFrame {
    bool valid;        // a feature exists and EvaluateFeature succeeded
    bool interpolated; // an intermediate frame is available
    VkImage real_image; // borrowed: the caller's backbuffer (not modified by NGX)
    VkImageView real_view;
    VkImage interpolated_image; // owned by this module until the next evaluate/shutdown
    VkImageView interpolated_view;
    VkFormat format;
    uint32_t width, height;
    uint64_t frame_id;
} Mk8DlssFgFrame;

enum {
    MK8_DLSS_FG_MARKER_SIMULATION_START = 0,
    MK8_DLSS_FG_MARKER_SIMULATION_END = 1,
    MK8_DLSS_FG_MARKER_RENDERSUBMIT_START = 2,
    MK8_DLSS_FG_MARKER_RENDERSUBMIT_END = 3,
    MK8_DLSS_FG_MARKER_PRESENT_START = 4,
    MK8_DLSS_FG_MARKER_PRESENT_END = 5,
    MK8_DLSS_FG_MARKER_INPUT_SAMPLE = 6,
    MK8_DLSS_FG_MARKER_TRIGGER_FLASH = 7,
};

// Creates NGX + the FrameGeneration feature for this swapchain. Requires VK_NV_low_latency2
// to be enabled on the device (it is not today; docs/DLSS-IMPL.md has the device.c hand-off).
// Returns false with one log line on any failure; the existing path must then run unchanged.
bool mk8_dlss_fg_init(const Mk8DlssFgInit *init);
void mk8_dlss_fg_shutdown(void);
bool mk8_dlss_fg_ready(void);
// Records one EvaluateFeature into inputs->command_buffer. On failure returns a zeroed frame
// (valid=false) and leaves the caller's existing path untouched; no fake image is returned.
// The real frame is the caller's backbuffer; only the interpolated image is owned here.
Mk8DlssFgFrame mk8_dlss_fg_evaluate(const Mk8DlssFgInputs *inputs);
// Records a copy of the interpolated result into an acquired swapchain image, transitioning
// it from `destination_layout` to PRESENT_SRC_KHR and leaving the internal image in GENERAL.
bool mk8_dlss_fg_copy_interpolated(VkCommandBuffer cb, VkImage destination, VkImageLayout destination_layout);
// Presents the real and interpolated swapchain images in that order, with VK_NV_low_latency2
// PRESENT markers when the device has them. The caller owns the two acquired images and the
// two semaphores (two binary semaphores, or timeline semaphores with distinct values).
VkResult mk8_dlss_fg_present(VkQueue queue, VkSwapchainKHR swapchain, uint32_t real_index,
                             uint32_t interpolated_index, VkSemaphore real_ready, VkSemaphore interpolated_ready);
// vkLatencySleepNV wrapper for the simulation loop; false when low latency is unavailable.
bool mk8_dlss_fg_latency_sleep(VkSemaphore signal, uint64_t value);
// vkSetLatencyMarkerNV wrapper; markers use MK8_DLSS_FG_MARKER_*.
void mk8_dlss_fg_latency_marker(int marker, uint64_t present_id);
#endif

#ifdef __cplusplus
}
#endif
