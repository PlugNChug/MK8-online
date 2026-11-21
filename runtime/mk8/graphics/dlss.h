// Optional NVIDIA DLSS Super Resolution / DLAA bridge (host side, no guest changes).
//
// The loader is deliberately independent of Vulkan: mk8_dlss_probe() only resolves paths,
// dlopens the NGX feature plugin and core, and checks their exported entry points. It runs
// (and is tested) on a machine without an NVIDIA GPU. The upscale path additionally needs
// the runtime's Vulkan device and is compiled out with -DMK8_DLSS_PROBE_ONLY.
//
// Nothing NVIDIA is bundled: the plugin and core come from the driver and from
// the optional launcher component. Every failure returns
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
    MK8_DLSS_NO_PLUGIN,              // no feature plugin (libnvidia-ngx-dlss*.so / nvngx_dlss*.dll)
    MK8_DLSS_PLUGIN_LOAD_FAILED,     // the file exists but dlopen/LoadLibrary failed (corrupt, wrong arch)
    MK8_DLSS_PLUGIN_EXPORTS_MISSING, // loaded, but it is not the expected NGX feature plugin
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

#ifdef MK8_DLSS_TEST_API
// Harness-only access to the feature-owned output image, so a native test can read it back.
// The runtime build does not define MK8_DLSS_TEST_API and keeps the view-only contract.
VkImage mk8_dlss_test_output_image(void);
VkFormat mk8_dlss_test_output_format(void);
#endif

#endif

#ifdef __cplusplus
}
#endif
