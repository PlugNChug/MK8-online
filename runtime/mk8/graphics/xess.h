// Optional Intel XeSS Super Resolution bridge (host side, no guest changes).
//
// XeSS is Windows-only in the official SDK (libxess.dll): the Vulkan SR API exists,
// but Intel ships no Linux library. This module therefore compiles to a probe plus a
// one-line "unavailable" stub on Linux, and to a dynamic loader plus a Vulkan path on
// Windows. Nothing Intel is bundled: the DLL comes from tools/fetch_vendor_sdk.py
// ($XDG_DATA_HOME/mk8online/intel) or MK8_XESS_LIB.
//
// Every failure returns VK_NULL_HANDLE with one reasoned log line so the caller keeps
// the existing FSR/bilinear path. MK8_XESS=1 selects the bridge; unset means no call
// is made and no state is touched.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xess/mk8_xess_abi.h"

#define MK8_XESS_PATH_MAX 1024
#define MK8_XESS_REASON_MAX 512
#define MK8_XESS_MISSING_MAX 256

typedef enum Mk8XessLoadStatus {
    MK8_XESS_OK = 0,
    MK8_XESS_UNSUPPORTED_PLATFORM, // Linux build: the DLL cannot be loaded here
    MK8_XESS_NO_DLL,               // no libxess.dll in the searched directory
    MK8_XESS_DLL_LOAD_FAILED,      // file present but not a complete PE32+ x86-64 DLL, or LoadLibrary failed
    MK8_XESS_DLL_EXPORTS_MISSING,  // loaded, but not the XeSS Vulkan API this bridge calls
} Mk8XessLoadStatus;

// Facts read from the PE file without executing it. Used by the probe and by the
// tests that run on Linux (where libxess.dll cannot be loaded).
typedef struct Mk8XessPeInfo {
    bool valid;       // MZ + PE\0\0 + PE32+ + x86-64 + all sections inside the file
    bool exports_ok;  // all MK8_XESS_SYM_* names are present
    uint16_t machine;
    uint32_t sections;
    uint32_t exports;
    char missing[MK8_XESS_MISSING_MAX];
    char detail[MK8_XESS_REASON_MAX];
} Mk8XessPeInfo;

typedef struct Mk8XessProbe {
    Mk8XessLoadStatus status;
    bool platform_supported; // false on Linux
    bool dll_present;
    bool pe_valid;
    bool exports_ok;
    bool loaded;          // LoadLibraryA succeeded (Windows only)
    bool licence_present; // LICENSE.txt beside the DLL (the downloader stores it there)
    char directory[MK8_XESS_PATH_MAX];
    char dll[MK8_XESS_PATH_MAX];
    char reason[MK8_XESS_REASON_MAX];
} Mk8XessProbe;

#ifdef __cplusplus
extern "C" {
#endif

// MK8_XESS=1 (or any non-empty value other than "0") enables the bridge.
bool mk8_xess_enabled(void);

// Resolves MK8_XESS_LIB/MK8_XESS_DIR (or $XDG_DATA_HOME/mk8online/intel), validates
// the PE and, on Windows, loads it and checks its exports. On Linux it still
// validates the file, then reports MK8_XESS_UNSUPPORTED_PLATFORM. Never touches Vulkan.
bool mk8_xess_probe(Mk8XessProbe *out);

// Stable name for logs and tests: "ok", "unsupported-platform", "no-dll", ...
const char *mk8_xess_status_name(Mk8XessLoadStatus status);

// Prints exactly one line: "[xess] XeSS unavailable: <reason>\n".
void mk8_xess_log_unavailable(const Mk8XessProbe *probe);

// Parses a PE32+ x86-64 DLL and its export table. Portable; used by the probe and
// directly by tests to check the real libxess.dll on Linux.
bool mk8_xess_inspect_pe(const char *path, Mk8XessPeInfo *out);

// Loads the DLL and calls xessVKGetRequiredInstanceExtensions (no Vulkan object
// needed) and xessGetVersion. Returns the XeSS result code, or -1 with a reason.
// The returned extension names are owned by the DLL and stay valid while it is
// loaded (the bridge keeps it loaded for the process lifetime).
int mk8_xess_query(uint32_t *instance_extension_count, const char *const **instance_extensions,
                   uint32_t *min_vk_api_version, Mk8XessVersion *version, char *reason,
                   size_t reason_size);

#ifndef MK8_XESS_PROBE_ONLY
#include <vulkan/vulkan.h>

typedef struct TexBackend TexBackend;

// Records one XeSS SR execute into cb and returns the display-size output view, or
// VK_NULL_HANDLE (one-time log) so the caller keeps the existing path. Consumes the
// same colour/depth/jitter inputs as mk8_fsr4_upscale and mk8_dlss_upscale.
VkImageView mk8_xess_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw,
                             uint32_t rh, uint32_t ow, uint32_t oh, float frame_ms);
#endif

#ifdef __cplusplus
}
#endif
