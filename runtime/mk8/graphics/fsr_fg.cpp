// Native FSR 3 / FSR 4 frame generation (host side, no guest changes).
//
// MK8_FSR_FG=fsr3 replaces the window swapchain with the FFX frame-interpolation swapchain: the
// game renders into the provider's replacement buffers, records the frame-generation prepare
// dispatch (scene depth, motion vectors, jitter, camera), and presents through the provider, which
// inserts an interpolated image between rendered ones. MK8_FSR_FG=fsr4 keeps that swapchain (3.1.7)
// for pacing/replacement but routes the effect work to the native MLFI 4.0.2 provider. Default off;
// every failure falls back to the original swapchain and says why. No provider is bundled: the
// swapchain library comes from MK8_FSR_FG_LIB or $XDG_DATA_HOME/mk8online/fsr/; the MLFI effect
// library from MK8_FSR_MLFI_LIB (alias MK8_FSR4_FG_LIB), the same data directory, or
// local/fg-provider/.
//
// The swapchain provider's ABI (3.1.7) and, for fsr4, the effect provider's version (exactly
// 4.0.2) are checked before anything is created, and the version descriptor sent to the swapchain
// provider is parsed from that reported version rather than assumed.
//
// fsr4 topology: the 3.1.7 swapchain only learns that interpolation is enabled through
// ffxConfigureDescFrameGeneration on a 3.1.7 frame-generation context (its Configure forwards the
// config to the swapchain via fpSwapChainConfigureFrameGeneration). That context therefore exists
// in fsr4 as the swapchain's router and never dispatches effect work; the MLFI context receives
// PrepareV2 from the game's command buffer and FrameGeneration from the swapchain's callback.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <SDL3/SDL.h>

#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#else
#include <dlfcn.h>
#endif

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx/ffx_api.h"
#include "ffx/ffx_framegeneration.h"
#include "ffx/vk/ffx_api_framegeneration_vk.h"
#include "ffx/vk/ffx_api_vk.h"

#include "vulkan/vk.h"
#include "fsr_fg.h"

extern "C" {
// device.c: set when the extra present/acquire queue family was found and created.
extern uint32_t g_vk_fg_present_family;
// device.c: C-linkage wrappers for the queue lock (vk.h's declarations are C++-only).
void mk8_fsr_fg_queue_lock(void);
void mk8_fsr_fg_queue_unlock(void);
// exec.c (captured per main draw when MK8_FSR_FG is set).
extern float g_fsr_view[12];
extern float g_fsr_jitter_x;
extern float g_fsr_jitter_y;
extern int g_fsr_vp_valid;
}

#define FG_REQUIRED_VERSION "3.1.7"
#define FG4_REQUIRED_VERSION "4.0.2"
#define FG4_REQUIRED_ID 0xf600000001000002ull  // 0xF6000000 << 32 | 4.0.2

enum { FG_MODE_OFF = 0, FG_MODE_FSR3 = 1, FG_MODE_FSR4 = 2 };

// Mirrors tools/mlfi-provider/mlfi_provider_ext.h (MK8_MLFI_STATS_VERSION 1). An optional
// diagnostic export of the MLFI provider: only used for one log line per phase (measured counts at
// the provider's dispatch call sites), never for control flow.
typedef struct {
    uint32_t struct_size;
    uint32_t version;
    uint32_t graph_pipelines;
    uint32_t copy_pipelines;
    uint32_t compose_pipelines;
    uint32_t prepare_dispatches;
    uint32_t copy_dispatches;
    uint32_t model_dispatches;
    uint32_t compose_dispatches;
    uint64_t frames;
    uint32_t resets;
    uint32_t first_frame;
    uint32_t last_create_rc;
    uint32_t last_prepare_rc;
    uint32_t last_frame_rc;
    uint32_t display_width;
    uint32_t display_height;
    uint32_t render_width;
    uint32_t render_height;
    uint32_t backbuffer_format;
} Mk8MlfiStats;
typedef uint32_t (*PfnMk8MlfiGetStats)(void *, Mk8MlfiStats *);

typedef struct {
    bool tried;
    bool failed;
    bool proxied;      // the window swapchain is the FFX proxy (present/acquire must be routed)
    bool interpolating;
    int mode;          // FG_MODE_* (0 is off)
    void *library;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxConfigure configure;
    PfnFfxDispatch dispatch;
    // The effect that records PrepareV2 / FrameGeneration. fsr3: the swapchain library above.
    // fsr4: the MLFI library (separate handle and entry points).
    void *effect_library;
    PfnFfxCreateContext effect_create;
    PfnFfxDestroyContext effect_destroy;
    PfnFfxQuery effect_query;
    PfnFfxConfigure effect_configure;
    PfnFfxDispatch effect_dispatch;
    PfnMk8MlfiGetStats effect_stats;
    uint64_t effect_version_id;
    ffxContext sc_ctx;
    ffxContext fg_ctx;     // the effect doing the work
    ffxContext notify_ctx; // fsr4 only: a 3.1.7 effect context that routes configure to the swapchain
    VkSwapchainKHR proxy;
    uint32_t version;
    PFN_vkCreateSwapchainFFXAPI create_sc;
    PFN_vkDestroySwapchainFFXAPI destroy_sc;
    PFN_vkGetSwapchainImagesKHR get_images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    VkQueue present_q;
    VkQueue acquire_q;
    // fsr4: the MLFI provider has one uniform arena and one descriptor set per phase, so its
    // command buffers must not overlap. A VkEvent is set at the end of every recorded MLFI
    // command buffer and the next recording polls for it (timeline semaphores cannot be signalled
    // from inside a command buffer in Vulkan 1.2). This serialises only the provider's own work.
    VkEvent gate;
    bool gate_armed;
    uint64_t frame_id;
    uint32_t width, height;
    VkFormat format;
    bool prepared_any;
    bool logged_dispatch;
    bool logged_prepare;
} Fg;

static Fg fg;

static ffxReturnCode_t frame_generation_callback(ffxDispatchDescFrameGeneration *params, void *context);

// ------------------------------------------------------------------ provider load and version

static void *load_library(const char *path) {
#ifdef _WIN32
    return (void *)LoadLibraryA(path);
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

static void *load_symbol(void *library, const char *name) {
#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)library, name);
#else
    return dlsym(library, name);
#endif
}

static const char *provider_path(void) {
    const char *path = getenv("MK8_FSR_FG_LIB");
    if (path && *path) return path;
    static char fallback[4096];
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg) {
        snprintf(fallback, sizeof fallback, "%s/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so", xdg);
        return fallback;
    }
    const char *home = getenv("HOME");
    if (home && *home) {
        snprintf(fallback, sizeof fallback, "%s/.local/share/mk8online/fsr/libamd_fidelityfx_framegeneration_vk.so", home);
        return fallback;
    }
    return "libamd_fidelityfx_framegeneration_vk.so";
}

static const char *mlfi_provider_path(void) {
    // MK8_FSR_MLFI_LIB is the documented name; MK8_FSR4_FG_LIB is the alias from the provider
    // document's integration section. An explicit path is authoritative (no silent fallback).
    const char *path = getenv("MK8_FSR_MLFI_LIB");
    if (!path || !*path) path = getenv("MK8_FSR4_FG_LIB");
    return path && *path ? path : NULL;
}

static void fail(const char *reason) {
    fg.failed = true;
    fprintf(stderr, "[fsr_fg] disabled: %s\n", reason);
}

// One clear line per process. `permanent` also stops later attempts (a missing provider/version is
// not something a resize can fix; a destroyed swapchain is retried after the surface comes back).
static int mode_from_env(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("MK8_FSR_FG");
        if (e && *e && !strcasecmp(e, "fsr3")) mode = FG_MODE_FSR3;
        else if (e && *e && !strcasecmp(e, "fsr4")) mode = FG_MODE_FSR4;
        else mode = FG_MODE_OFF;
        if (e && *e && mode == FG_MODE_OFF)
            fprintf(stderr, "[fsr_fg] ignored: MK8_FSR_FG=%s (only fsr3 and fsr4 are implemented)\n", e);
        // fsr3: the source-built provider (tools/gen_fg_blobs.py) is the validated one: the
        // generated blobs bind the constant/UAV/SRV buffers, declare the non-atomic storage images
        // with format Unknown, and the headless fixture, the 120-frame harness (validation clean)
        // and an in-game run exercise the interpolation path. fsr4: the MLFI 4.0.2 provider records
        // the captured MLFI graph (tools/mlfi-provider); it is validated headlessly and in-game but
        // its composition stage is ours, not AMD's. The exact 3.1.7 and 4.0.2 checks below still
        // refuse any other provider.
        if (mode == FG_MODE_FSR4)
            fprintf(stderr, "[fsr_fg] mode fsr4: 3.1.7 frame-interpolation swapchain + MLFI 4.0.2 effect\n");
    }
    return mode;
}

static bool enabled_by_env(void) { return mode_from_env() != FG_MODE_OFF; }

static bool load_provider(void) {
    if (fg.library) return true;  // (kept loaded across a surface recreation)
    if (fg.tried) return false;
    fg.tried = true;
    fg.mode = mode_from_env();
    const char *path = provider_path();
    fg.library = load_library(path);
    if (!fg.library) {
        static char reason[4200];
        snprintf(reason, sizeof reason, "provider not loaded: %s", path);
        fail(reason);
        return false;
    }
    fg.create = (PfnFfxCreateContext)load_symbol(fg.library, "ffxCreateContext");
    fg.destroy = (PfnFfxDestroyContext)load_symbol(fg.library, "ffxDestroyContext");
    fg.query = (PfnFfxQuery)load_symbol(fg.library, "ffxQuery");
    fg.configure = (PfnFfxConfigure)load_symbol(fg.library, "ffxConfigure");
    fg.dispatch = (PfnFfxDispatch)load_symbol(fg.library, "ffxDispatch");
    if (!fg.create || !fg.destroy || !fg.query || !fg.configure || !fg.dispatch) {
        fail("provider lacks the FFX exports (ffxCreateContext/ffxDestroyContext/ffxQuery/ffxConfigure/ffxDispatch)");
        return false;
    }
    fprintf(stderr, "[fsr_fg] provider loaded: %s\n", path);
    if (fg.mode == FG_MODE_FSR3) {
        // One library: the same entry points create the swapchain and the effect.
        fg.effect_library = fg.library;
        fg.effect_create = fg.create;
        fg.effect_destroy = fg.destroy;
        fg.effect_query = fg.query;
        fg.effect_configure = fg.configure;
        fg.effect_dispatch = fg.dispatch;
    }
    return true;
}

// fsr4: the MLFI effect provider is a second library. fsr3 aliases the swapchain library above.
// Loading is explicit: an env path that fails is an error, not a reason to try another file.
static bool load_effect_provider(void) {
    if (fg.mode != FG_MODE_FSR4) return fg.effect_create != NULL;
    if (fg.effect_library) return true;
    char candidates[4][4096];
    int count = 0;
    const char *explicit_path = mlfi_provider_path();
    if (explicit_path) {
        snprintf(candidates[count++], sizeof candidates[0], "%s", explicit_path);
    } else {
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME");
        if (xdg && *xdg)
            snprintf(candidates[count++], sizeof candidates[0], "%s/mk8online/fsr/libmk8_mlfi_framegeneration_vk.so", xdg);
        if (home && *home)
            snprintf(candidates[count++], sizeof candidates[0], "%s/.local/share/mk8online/fsr/libmk8_mlfi_framegeneration_vk.so", home);
        snprintf(candidates[count++], sizeof candidates[0], "local/fg-provider/libmk8_mlfi_framegeneration_vk.so");
    }
    for (int i = 0; i < count; i++) {
        fg.effect_library = load_library(candidates[i]);
        if (!fg.effect_library) continue;
        fg.effect_create = (PfnFfxCreateContext)load_symbol(fg.effect_library, "ffxCreateContext");
        fg.effect_destroy = (PfnFfxDestroyContext)load_symbol(fg.effect_library, "ffxDestroyContext");
        fg.effect_query = (PfnFfxQuery)load_symbol(fg.effect_library, "ffxQuery");
        fg.effect_configure = (PfnFfxConfigure)load_symbol(fg.effect_library, "ffxConfigure");
        fg.effect_dispatch = (PfnFfxDispatch)load_symbol(fg.effect_library, "ffxDispatch");
        if (!fg.effect_create || !fg.effect_destroy || !fg.effect_query || !fg.effect_configure || !fg.effect_dispatch) {
            fg.effect_library = NULL;
            continue;
        }
        fg.effect_stats = (PfnMk8MlfiGetStats)load_symbol(fg.effect_library, "mk8MlfiGetStats");
        fprintf(stderr, "[fsr_fg] MLFI effect provider loaded: %s (stats export %s)\n", candidates[i],
                fg.effect_stats ? "present" : "absent");
        return true;
    }
    static char reason[4200];
    if (explicit_path) snprintf(reason, sizeof reason, "MLFI provider not loaded: %s", explicit_path);
    else snprintf(reason, sizeof reason, "MLFI provider not loaded (MK8_FSR_MLFI_LIB, $XDG_DATA_HOME/mk8online/fsr/, "
                                         "$HOME/.local/share/mk8online/fsr/, local/fg-provider/)");
    fail(reason);
    return false;
}

// The swapchain provider's version list is the only place the ABI variant is stated. Refuse to
// create anything unless this build's expected 3.1.7 is exactly what the provider reports.
static bool check_swapchain_version(void) {
    VkDeviceContext device = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr};
    uint64_t count = 0;
    ffxQueryDescGetVersions versions = {0};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    versions.device = &device;
    versions.outputCount = &count;
    ffxReturnCode_t rc = fg.query(NULL, &versions.header);
    if (rc != FFX_API_RETURN_OK) {
        static char reason[160];
        snprintf(reason, sizeof reason, "swapchain version query returned %d", (int)rc);
        fail(reason);
        return false;
    }
    if (count == 0 || count > 16) {
        fail("swapchain provider reported no versions");
        return false;
    }
    uint64_t ids[16] = {0};
    const char *names[16] = {0};
    uint64_t capacity = count;
    versions.outputCount = &capacity;
    versions.versionIds = ids;
    versions.versionNames = names;
    rc = fg.query(NULL, &versions.header);
    if (rc != FFX_API_RETURN_OK) {
        fail("swapchain version list query failed");
        return false;
    }
    const char *found = NULL;
    for (uint64_t i = 0; i < capacity && i < count; i++) {
        fprintf(stderr, "[fsr_fg] swapchain provider offers %s\n", names[i] ? names[i] : "(unnamed)");
        if (names[i] && strcmp(names[i], FG_REQUIRED_VERSION) == 0) found = names[i];
    }
    if (!found) {
        static char reason[160];
        snprintf(reason, sizeof reason, "swapchain provider does not offer %s", FG_REQUIRED_VERSION);
        fail(reason);
        return false;
    }
    unsigned major = 0, minor = 0, patch = 0;
    if (sscanf(found, "%u.%u.%u", &major, &minor, &patch) != 3) {
        fail("swapchain provider version name is not major.minor.patch");
        return false;
    }
    fg.version = (major << 22) | (minor << 12) | patch;
    fprintf(stderr, "[fsr_fg] swapchain provider %s (version 0x%x)\n", found, fg.version);
    return true;
}

// fsr4: the MLFI library must answer the frame-generation effect with exactly 4.0.2. Both the name
// and the packed id are required; the id is what the creation override carries. A different
// version (a future provider) is refused here rather than created and driven with an unverified
// descriptor layout.
static bool check_effect_version(void) {
    if (fg.mode != FG_MODE_FSR4) return true;
    VkDeviceContext device = {g_vk.dev, g_vk.phys, vkGetDeviceProcAddr};
    uint64_t count = 0;
    ffxQueryDescGetVersions versions = {0};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    versions.device = &device;
    versions.outputCount = &count;
    ffxReturnCode_t rc = fg.effect_query(NULL, &versions.header);
    if (rc != FFX_API_RETURN_OK) {
        static char reason[160];
        snprintf(reason, sizeof reason, "MLFI version query returned %d", (int)rc);
        fail(reason);
        return false;
    }
    if (count == 0 || count > 16) {
        fail("MLFI provider reported no frame-generation versions");
        return false;
    }
    uint64_t ids[16] = {0};
    const char *names[16] = {0};
    uint64_t capacity = count;
    versions.outputCount = &capacity;
    versions.versionIds = ids;
    versions.versionNames = names;
    rc = fg.effect_query(NULL, &versions.header);
    if (rc != FFX_API_RETURN_OK) {
        fail("MLFI version list query failed");
        return false;
    }
    uint64_t found_id = 0;
    for (uint64_t i = 0; i < capacity && i < count; i++) {
        fprintf(stderr, "[fsr_fg] MLFI effect provider offers %s (id 0x%016llx)\n",
                names[i] ? names[i] : "(unnamed)", (unsigned long long)ids[i]);
        if (names[i] && strcmp(names[i], FG4_REQUIRED_VERSION) == 0 && ids[i] == FG4_REQUIRED_ID) found_id = ids[i];
    }
    if (!found_id) {
        static char reason[200];
        snprintf(reason, sizeof reason, "MLFI provider does not offer %s (id 0x%016llx)", FG4_REQUIRED_VERSION,
                 (unsigned long long)FG4_REQUIRED_ID);
        fail(reason);
        return false;
    }
    fg.effect_version_id = found_id;
    fprintf(stderr, "[fsr_fg] MLFI effect provider %s (id 0x%016llx)\n", FG4_REQUIRED_VERSION,
            (unsigned long long)fg.effect_version_id);
    return true;
}

// ------------------------------------------------------------------ queues (serialized with the game)

// The FFX swapchain owns a present thread and submits its own command buffers. Every submission
// goes through the game's queue lock so the render thread and that thread never enter the driver
// at the same time. The proxy present itself is called without the lock (device.c routes it that
// way) because it may wait for that thread.
static VkResult submit_game(uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    mk8_fsr_fg_queue_lock();
    VkResult r = vkQueueSubmit(g_vk.queue, count, submits, fence);
    mk8_fsr_fg_queue_unlock();
    return r;
}

static VkResult submit_present(uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    mk8_fsr_fg_queue_lock();
    VkResult r = vkQueueSubmit(fg.present_q, count, submits, fence);
    mk8_fsr_fg_queue_unlock();
    return r;
}

static VkResult submit_acquire(uint32_t count, const VkSubmitInfo *submits, VkFence fence) {
    mk8_fsr_fg_queue_lock();
    VkResult r = vkQueueSubmit(fg.acquire_q, count, submits, fence);
    mk8_fsr_fg_queue_unlock();
    return r;
}

static bool surface_format(VkFormat format, uint32_t *out) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM: *out = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM; return true;
    case VK_FORMAT_B8G8R8A8_SRGB: *out = FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB; return true;
    case VK_FORMAT_R8G8B8A8_UNORM: *out = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM; return true;
    case VK_FORMAT_R8G8B8A8_SRGB: *out = FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB; return true;
    default: return false;
    }
}

// ------------------------------------------------------------------ provider contexts

// One frame in flight for the MLFI provider (fsr4 only). See the Fg.gate comment.
static bool gate_create(void) {
    if (fg.mode != FG_MODE_FSR4 || fg.gate) return true;
    VkEventCreateInfo info = {VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
    VkResult r = vkCreateEvent(g_vk.dev, &info, NULL, &fg.gate);
    if (r != VK_SUCCESS) {
        fail("MLFI frame-in-flight event creation failed");
        return false;
    }
    fg.gate_armed = false;
    return true;
}

static void gate_destroy(void) {
    if (!fg.gate) return;
    vkDeviceWaitIdle(g_vk.dev);  // the event may still be referenced by a submitted command buffer
    vkDestroyEvent(g_vk.dev, fg.gate, NULL);
    fg.gate = VK_NULL_HANDLE;
    fg.gate_armed = false;
}

// Polls the event set by the previously recorded command buffer. The GPU work is the provider's
// interpolation/prepare (a few milliseconds), so polling at 0.1 ms is cheaper than a queue stall.
static bool gate_wait(void) {
    if (fg.mode != FG_MODE_FSR4 || !fg.gate || !fg.gate_armed) return true;
    uint64_t deadline = SDL_GetTicksNS() + 5000000000ull;  // 5 s: a real stall, not pacing
    for (;;) {
        VkResult r = vkGetEventStatus(g_vk.dev, fg.gate);
        if (r == VK_EVENT_SET) {
            vkResetEvent(g_vk.dev, fg.gate);
            fg.gate_armed = false;
            return true;
        }
        if (r != VK_EVENT_RESET) {
            fail("MLFI frame-in-flight event status failed");
            return false;
        }
        if (SDL_GetTicksNS() > deadline) {
            // The previous command buffer was dropped without setting the event (or the GPU
            // stalled): stop generating rather than overwrite the provider's single arena under a
            // pending command.
            ffxConfigureDescFrameGeneration off = {0};
            off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            off.swapChain = (void *)fg.proxy;
            off.frameGenerationEnabled = false;
            off.frameID = fg.frame_id;
            if (fg.notify_ctx) fg.configure(&fg.notify_ctx, &off.header);
            fg.interpolating = false;
            fail("MLFI frame-in-flight wait timed out; presenting real frames only");
            return false;
        }
        SDL_DelayNS(100000);
    }
}

static void gate_signal(VkCommandBuffer cb) {
    if (fg.mode != FG_MODE_FSR4 || !fg.gate || !cb) return;
    vkCmdSetEvent(cb, fg.gate, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    fg.gate_armed = true;
}

// Enables/disables interpolation for the proxy swapchain with the frame callback that dispatches
// the effect. fsr3: the 3.1.7 effect context itself forwards this configure to the swapchain
// (fpSwapChainConfigureFrameGeneration). fsr4: that router is fg.notify_ctx (3.1.7) while the MLFI
// context receives the same configure for its own frame-ID/reset bookkeeping; it never forwards.
static ffxReturnCode_t set_interpolation(bool enabled, uint64_t frame_id) {
    ffxConfigureDescFrameGeneration config = {0};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = (void *)fg.proxy;
    if (enabled) {
        config.frameGenerationCallback = frame_generation_callback;
        config.frameGenerationCallbackUserContext = &fg.fg_ctx;
    }
    config.frameGenerationEnabled = enabled;
    config.allowAsyncWorkloads = false;  // the present/acquire queues are already the async path here
    config.frameID = frame_id;
    ffxReturnCode_t rc;
    if (fg.notify_ctx) {
        rc = fg.configure(&fg.notify_ctx, &config.header);
        if (rc != FFX_API_RETURN_OK) return rc;
        return fg.effect_configure(&fg.fg_ctx, &config.header);
    }
    return fg.effect_configure(&fg.fg_ctx, &config.header);
}

static void destroy_effect(void) {
    if (!fg.fg_ctx && !fg.notify_ctx) return;
    if (fg.proxied && fg.fg_ctx)
        set_interpolation(false, fg.frame_id);  // waits for pending interpolation/presents
    if (fg.fg_ctx && fg.effect_destroy) fg.effect_destroy(&fg.fg_ctx, NULL);
    if (fg.notify_ctx && fg.destroy) fg.destroy(&fg.notify_ctx, NULL);
    fg.fg_ctx = NULL;
    fg.notify_ctx = NULL;
    fg.interpolating = false;
    fg.prepared_any = false;
    fg.logged_dispatch = false;
    fg.logged_prepare = false;
}

static bool create_effect(uint32_t width, uint32_t height, uint32_t display_format) {
    ffxCreateContextDescFrameGenerationVersion version = {0};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateBackendVKDesc backend = {0};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext.vkDevice = g_vk.dev;
    backend.deviceContext.vkPhysicalDevice = g_vk.phys;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxCreateContextDescFrameGeneration effect = {0};
    effect.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    effect.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    effect.displaySize = {width, height};
    effect.maxRenderSize = {width, height};
    effect.backBufferFormat = display_format;
    effect.flags = 0;  // no async compute, no HDR: the window image is an SDR UNORM swapchain
    ffxOverrideVersion override = {0};
    override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override.versionId = fg.effect_version_id;
    if (fg.mode == FG_MODE_FSR4) {
        // The 3.1.7 effect context is the swapchain's configure router only (it never dispatches):
        // it takes the unmodified descriptor. The MLFI context is selected explicitly by the
        // version override, which the 3.1.7 swapchain library would refuse and the MLFI provider
        // requires to equal 4.0.2.
        ffxReturnCode_t router_rc = fg.create(&fg.notify_ctx, &effect.header, NULL);
        if (router_rc != FFX_API_RETURN_OK || !fg.notify_ctx) {
            static char reason[160];
            snprintf(reason, sizeof reason, "3.1.7 swapchain router context creation failed (ffx %d)", (int)router_rc);
            fg.notify_ctx = NULL;
            fail(reason);
            return false;
        }
        backend.header.pNext = &override.header;
    }
    ffxReturnCode_t rc = fg.effect_create(&fg.fg_ctx, &effect.header, NULL);
    if (rc != FFX_API_RETURN_OK || !fg.fg_ctx) {
        static char reason[200];
        if (fg.mode == FG_MODE_FSR4)
            snprintf(reason, sizeof reason, "MLFI 4.0.2 context creation failed (ffx %d; this provider implements "
                                            "1280x720 with an 8-bit UNORM swapchain)", (int)rc);
        else
            snprintf(reason, sizeof reason, "frame-generation context creation failed (ffx %d)", (int)rc);
        fg.fg_ctx = NULL;
        if (fg.notify_ctx && fg.destroy) fg.destroy(&fg.notify_ctx, NULL);
        fg.notify_ctx = NULL;
        fail(reason);
        return false;
    }
    ffxQueryGetProviderVersion provider = {0};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fg.effect_query(&fg.fg_ctx, &provider.header) == FFX_API_RETURN_OK)
        fprintf(stderr, "[fsr_fg] frame-generation effect provider %s%s\n", provider.versionName ? provider.versionName : "(unknown)",
                fg.mode == FG_MODE_FSR4 ? " (MLFI)" : "");
    if (fg.mode == FG_MODE_FSR4)
        fprintf(stderr, "[fsr_fg] MLFI effect context: %ux%u format %d, override 0x%016llx\n", width, height,
                (int)display_format, (unsigned long long)fg.effect_version_id);
    return true;
}

static void destroy_contexts(void) {
    destroy_effect();
    if (fg.dispatch && fg.sc_ctx) {
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait = {0};
        wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        fg.dispatch(&fg.sc_ctx, &wait.header);
    }
    if (fg.destroy && fg.sc_ctx) fg.destroy(&fg.sc_ctx, NULL);
    fg.sc_ctx = NULL;
    if (fg.destroy_sc && fg.proxy) fg.destroy_sc(g_vk.dev, fg.proxy, NULL, (void *)fg.proxy);
    fg.proxy = VK_NULL_HANDLE;
    fg.proxied = false;
    gate_destroy();
}

static bool create_contexts(const VkSwapchainCreateInfoKHR *ci) {
    if (g_vk_fg_present_family == UINT32_MAX) {
        fail("no separate present/acquire queue family");
        return false;
    }
    uint32_t display_format = 0;
    if (!surface_format(ci->imageFormat, &display_format)) {
        static char reason[128];
        snprintf(reason, sizeof reason, "unsupported swapchain format %d", (int)ci->imageFormat);
        fail(reason);
        return false;
    }
    vkGetDeviceQueue(g_vk.dev, g_vk_fg_present_family, 0, &fg.present_q);
    vkGetDeviceQueue(g_vk.dev, g_vk_fg_present_family, 1, &fg.acquire_q);
    if (!fg.present_q || !fg.acquire_q) {
        fail("the present/acquire queues are missing from the created device");
        return false;
    }

    ffxCreateContextDescFrameGenerationSwapChainVersionVK version = {0};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    version.version = fg.version;
    // The provider paces presents itself (interpolated + real). Its command pool is sized for a
    // vsync-throttled swapchain; the game's default MAILBOX would let the presenter run ahead and
    // exhaust it. FIFO makes the real swapchain the throttle, as the FSR3 samples do.
    VkSwapchainCreateInfoKHR paced = *ci;
    if (paced.presentMode != VK_PRESENT_MODE_FIFO_KHR) {
        static int said;
        if (!said++) fprintf(stderr, "[fsr_fg] present mode %d replaced by FIFO so the frame-interpolation swapchain paces itself\n",
                             (int)paced.presentMode);
        paced.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    }
    ffxCreateContextDescFrameGenerationSwapChainVK swapchain = {0};
    swapchain.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    swapchain.header.pNext = &version.header;
    swapchain.physicalDevice = g_vk.phys;
    swapchain.device = g_vk.dev;
    VkSwapchainKHR proxy = VK_NULL_HANDLE;
    swapchain.swapchain = &proxy;
    swapchain.createInfo = paced;
    // The provider creates and owns the real window swapchain; the game's old handle (if any) is
    // not the provider's and is never passed as oldSwapchain.
    swapchain.createInfo.oldSwapchain = VK_NULL_HANDLE;
    swapchain.gameQueue = {g_vk.queue, g_vk.qfam, submit_game};
    swapchain.presentQueue = {fg.present_q, g_vk_fg_present_family, submit_present};
    swapchain.imageAcquireQueue = {fg.acquire_q, g_vk_fg_present_family, submit_acquire};
    ffxReturnCode_t rc = fg.create(&fg.sc_ctx, &swapchain.header, NULL);
    if (rc != FFX_API_RETURN_OK || !fg.sc_ctx || !proxy) {
        static char reason[128];
        snprintf(reason, sizeof reason, "swapchain context creation failed (ffx %d)", (int)rc);
        fg.sc_ctx = NULL;
        fg.proxy = VK_NULL_HANDLE;
        fail(reason);
        return false;
    }
    fg.proxy = proxy;

    ffxQueryDescSwapchainReplacementFunctionsVK functions = {0};
    functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
    rc = fg.query(&fg.sc_ctx, &functions.header);
    if (rc != FFX_API_RETURN_OK || !functions.pOutCreateSwapchainFFXAPI || !functions.pOutDestroySwapchainFFXAPI ||
        !functions.pOutGetSwapchainImagesKHR || !functions.pOutAcquireNextImageKHR || !functions.pOutQueuePresentKHR) {
        fail("the provider did not return the swapchain replacement functions");
        destroy_contexts();
        return false;
    }
    fg.create_sc = functions.pOutCreateSwapchainFFXAPI;
    fg.destroy_sc = functions.pOutDestroySwapchainFFXAPI;
    fg.get_images = functions.pOutGetSwapchainImagesKHR;
    fg.acquire = functions.pOutAcquireNextImageKHR;
    fg.present = functions.pOutQueuePresentKHR;

    if (!gate_create()) {
        destroy_contexts();
        return false;
    }
    if (!create_effect(ci->imageExtent.width, ci->imageExtent.height, display_format)) {
        destroy_contexts();
        return false;
    }

    fg.width = ci->imageExtent.width;
    fg.height = ci->imageExtent.height;
    fg.format = ci->imageFormat;
    fg.frame_id = 0;
    fg.proxied = true;
    fg.interpolating = false;
    fprintf(stderr, "[fsr_fg] active: FFX frame-interpolation swapchain %ux%u format %d (present family %u)\n",
            fg.width, fg.height, (int)fg.format, g_vk_fg_present_family);
    return true;
}

// Called by device.c instead of vkCreateSwapchainKHR when MK8_FSR_FG=fsr3.
extern "C" bool mk8_fsr_fg_create_swapchain(const VkSwapchainCreateInfoKHR *ci, VkSwapchainKHR *swapchain) {
    if (!enabled_by_env() || fg.failed) return false;
    if (!load_provider()) return false;
    if (!load_effect_provider()) return false;
    if (!fg.proxied && (!check_swapchain_version() || !check_effect_version())) return false;
    if (fg.proxied) {
        bool changed = ci->imageExtent.width != fg.width || ci->imageExtent.height != fg.height ||
                       ci->imageFormat != fg.format;
        // fsr4: the MLFI provider caches views of the swapchain's interpolation outputs and of the
        // replacement buffers. They are recreated by the 3.1.7 swapchain on a resize, so recreate
        // the effect on every proxy recreation rather than risk a stale view. fsr3 keeps its
        // existing behaviour (the effect's display/render sizes are fixed at creation).
        bool recreate_effect = changed || fg.mode == FG_MODE_FSR4;
        if (recreate_effect) destroy_effect();
        VkSwapchainCreateInfoKHR recreate = *ci;
        recreate.oldSwapchain = VK_NULL_HANDLE;
        recreate.presentMode = VK_PRESENT_MODE_FIFO_KHR;  // keep the provider's pacing throttle
        VkSwapchainKHR same = fg.proxy;
        VkResult r = fg.create_sc(g_vk.dev, &recreate, NULL, &same, (void *)fg.proxy);
        if (r != VK_SUCCESS) {
            static char reason[128];
            snprintf(reason, sizeof reason, "swapchain recreation failed (%d)", (int)r);
            destroy_contexts();
            *swapchain = VK_NULL_HANDLE;
            fail(reason);
            return false;
        }
        uint32_t display_format = 0;
        if (recreate_effect) {
            if (!surface_format(ci->imageFormat, &display_format) ||
                !create_effect(ci->imageExtent.width, ci->imageExtent.height, display_format)) {
                destroy_contexts();
                *swapchain = VK_NULL_HANDLE;
                if (!fg.failed) fail("frame-generation context recreation failed");
                return false;
            }
        }
        fg.width = ci->imageExtent.width;
        fg.height = ci->imageExtent.height;
        fg.format = ci->imageFormat;
        fprintf(stderr, "[fsr_fg] swapchain recreated %ux%u\n", fg.width, fg.height);
        return true;
    }
    if (!create_contexts(ci)) return false;
    *swapchain = fg.proxy;
    return true;
}

extern "C" bool mk8_fsr_fg_active(void) { return fg.proxied; }

extern "C" VkResult mk8_fsr_fg_get_images(VkDevice device, VkSwapchainKHR swapchain,
                                          uint32_t *count, VkImage *images) {
    if (!fg.proxied || swapchain != fg.proxy) return vkGetSwapchainImagesKHR(device, swapchain, count, images);
    uint32_t available = 0;
    VkResult r = fg.get_images(device, swapchain, &available, NULL);
    if (r != VK_SUCCESS) return r;
    if (available > *count) available = *count;
    r = fg.get_images(device, swapchain, &available, images);
    if (r == VK_SUCCESS || r == VK_INCOMPLETE) *count = available;
    return r;
}

extern "C" VkResult mk8_fsr_fg_acquire(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                       VkSemaphore semaphore, VkFence fence, uint32_t *index) {
    if (!fg.proxied || swapchain != fg.proxy) return vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, index);
    return fg.acquire(device, swapchain, timeout, semaphore, fence, index);
}

// device.c calls this without holding vk_queue_lock: the proxy may wait for its present thread,
// whose submissions take that same lock.
extern "C" VkResult mk8_fsr_fg_present(VkQueue queue, const VkPresentInfoKHR *info) {
    if (!fg.proxied || !info || info->swapchainCount == 0 || info->pSwapchains[0] != fg.proxy)
        return vkQueuePresentKHR(queue, info);
    return fg.present(queue, info);
}

// ------------------------------------------------------------------ per-frame dispatch

static ffxReturnCode_t frame_generation_callback(ffxDispatchDescFrameGeneration *params, void *context) {
    ffxContext *effect = (ffxContext *)context;
    if (!gate_wait()) return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    ffxReturnCode_t rc = fg.effect_dispatch(effect, &params->header);
    if (rc == FFX_API_RETURN_OK) gate_signal((VkCommandBuffer)params->commandList);
    if (rc == FFX_API_RETURN_OK && !fg.logged_dispatch) {
        fg.logged_dispatch = true;
        fprintf(stderr, "[fsr_fg] first interpolated frame recorded (frameID %llu, %ux%u)\n",
                (unsigned long long)params->frameID,
                params->presentColor.description.width, params->presentColor.description.height);
        if (fg.mode == FG_MODE_FSR4 && fg.effect_stats) {
            Mk8MlfiStats stats = {0};
            stats.struct_size = sizeof stats;
            if (fg.effect_stats(*effect, &stats) == FFX_API_RETURN_OK && stats.version == 1)
                fprintf(stderr, "[fsr_fg] MLFI frame generation: %u model + %u compose dispatches, frames %llu, resets %u\n",
                        stats.model_dispatches, stats.compose_dispatches, (unsigned long long)stats.frames, stats.resets);
        }
    }
    return rc;
}

static void camera_vectors(float position[3], float up[3], float right[3], float forward[3]) {
    // g_fsr_view is the row-major 3x4 world->view matrix captured in the main draw. The camera axes
    // in world space are the rows of its rotation; the position is -R^T t.
    position[0] = position[1] = position[2] = 0.0f;
    up[0] = 0.0f; up[1] = 1.0f; up[2] = 0.0f;
    right[0] = 1.0f; right[1] = 0.0f; right[2] = 0.0f;
    forward[0] = 0.0f; forward[1] = 0.0f; forward[2] = -1.0f;
    if (!g_fsr_vp_valid) return;
    const float *m = g_fsr_view;
    for (int row = 0; row < 3; row++) {
        right[row] = m[row * 4 + 0];
        up[row] = m[row * 4 + 1];
        forward[row] = -m[row * 4 + 2];
    }
    for (int row = 0; row < 3; row++)
        position[row] = -(m[0 * 4 + row] * m[0 * 4 + 3] + m[1 * 4 + row] * m[1 * 4 + 3] + m[2 * 4 + row] * m[2 * 4 + 3]);
}

static void describe_image(VkImage image, VkFormat format, uint32_t w, uint32_t h, VkImageCreateInfo *ci) {
    memset(ci, 0, sizeof *ci);
    ci->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci->imageType = VK_IMAGE_TYPE_2D;
    ci->format = format;
    ci->extent = {w, h, 1};
    ci->mipLevels = 1;
    ci->arrayLayers = 1;
    ci->samples = VK_SAMPLE_COUNT_1_BIT;
    ci->tiling = VK_IMAGE_TILING_OPTIMAL;
    ci->usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci->sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    (void)image;
}

// Stop the provider from calling the frame-generation callback; the proxy keeps presenting the
// real frame. Used when a per-frame dispatch fails after the swapchain is already in place.
static void disable_interpolation(void) {
    if (!fg.effect_configure || !fg.fg_ctx || !fg.proxied) return;
    if (set_interpolation(false, fg.frame_id) == FFX_API_RETURN_OK) fg.interpolating = false;
}

// Called from frame_end() before the submission, after the last render pass. The prepare dispatch
// is recorded into the game's command buffer; the provider's present callback dispatches the
// interpolation later from the same thread.
extern "C" void mk8_fsr_fg_prepare(VkCommandBuffer cb, uint32_t width, uint32_t height) {
    if (!fg.proxied || fg.failed || !fg.fg_ctx || !cb || !width || !height) return;
    if (width != fg.width || height != fg.height) {
        static bool said;
        if (!said) said = true, fprintf(stderr, "[fsr_fg] skipping prepare: %ux%u does not match the swapchain %ux%u\n",
                                         width, height, fg.width, fg.height);
        return;
    }
    Mk8FsrFgInput depth = {0}, motion = {0};
    if (!mk8_fsr_fg_inputs(cb, width, height, &depth, &motion) || !depth.image || !motion.image) {
        static bool said;
        if (!said) said = true, fprintf(stderr, "[fsr_fg] skipping prepare: depth/motion inputs are unavailable\n");
        return;
    }

    uint64_t frame_id = fg.frame_id++;
    if (!gate_wait()) return;
    ffxReturnCode_t rc = set_interpolation(true, frame_id);
    if (rc != FFX_API_RETURN_OK) {
        static char reason[128];
        snprintf(reason, sizeof reason, "ffxConfigure failed (ffx %d); presenting real frames only", (int)rc);
        disable_interpolation();
        fail(reason);
        return;
    }

    VkImageCreateInfo depth_ci, motion_ci;
    describe_image(depth.image, depth.format, width, height, &depth_ci);
    describe_image(motion.image, motion.format, width, height, &motion_ci);
    ffxDispatchDescFrameGenerationPrepareV2 prepare = {0};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prepare.frameID = frame_id;
    prepare.commandList = cb;
    prepare.renderSize = {width, height};
    prepare.jitterOffset = {g_fsr_jitter_x, g_fsr_jitter_y};
    prepare.motionVectorScale.x = motion.real ? 1.0f : (float)width;
    prepare.motionVectorScale.y = motion.real ? 1.0f : (float)height;
    static uint64_t last_prepare_ns;
    uint64_t prepare_ns = SDL_GetTicksNS();
    float frame_ms = last_prepare_ns ? (float)((prepare_ns - last_prepare_ns) / 1e6) : 16.7f;
    last_prepare_ns = prepare_ns;
    if (!(frame_ms > 1.0f && frame_ms < 100.0f)) frame_ms = 16.7f;  // (first frame / a pause -> nominal)
    prepare.frameTimeDelta = frame_ms;
    prepare.reset = !fg.prepared_any;
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.cameraFovAngleVertical = 1.0472f;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = ffxApiGetResourceVK(depth.image,
        ffxApiGetImageResourceDescriptionVK(depth.image, depth_ci, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceVK(motion.image,
        ffxApiGetImageResourceDescriptionVK(motion.image, motion_ci, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    camera_vectors(prepare.cameraPosition, prepare.cameraUp, prepare.cameraRight, prepare.cameraForward);
    static int camera_state = -1;
    static float last_camera_position[3];
    static uint64_t last_camera_log;
    bool camera_changed = (int)g_fsr_vp_valid != camera_state;
    if (g_fsr_vp_valid && !camera_changed) {
        float dx = prepare.cameraPosition[0] - last_camera_position[0];
        float dy = prepare.cameraPosition[1] - last_camera_position[1];
        float dz = prepare.cameraPosition[2] - last_camera_position[2];
        // The scan overwrites the view every matching draw; log a moving camera at most every 120
        // frames so a race position proves the gameplay matrix, not just the first matching block.
        if (dx * dx + dy * dy + dz * dz > 1.0f && fg.frame_id - last_camera_log >= 120) camera_changed = true;
    }
    if (camera_changed) {
        camera_state = (int)g_fsr_vp_valid;
        memcpy(last_camera_position, prepare.cameraPosition, sizeof last_camera_position);
        last_camera_log = fg.frame_id;
        fprintf(stderr, "[fsr_fg] camera %s: position %.1f %.1f %.1f forward %.1f %.1f %.1f\n",
                g_fsr_vp_valid ? "from the captured view matrix" : "default (no captured view matrix)",
                prepare.cameraPosition[0], prepare.cameraPosition[1], prepare.cameraPosition[2],
                prepare.cameraForward[0], prepare.cameraForward[1], prepare.cameraForward[2]);
    }
    rc = fg.effect_dispatch(&fg.fg_ctx, &prepare.header);
    bool prepared = false;
    if (rc != FFX_API_RETURN_OK) {
        static char reason[128];
        snprintf(reason, sizeof reason, "frame-generation prepare failed (ffx %d); presenting real frames only", (int)rc);
        disable_interpolation();
        fail(reason);
    } else {
        fg.interpolating = true;
        fg.prepared_any = true;
        prepared = true;
        if (fg.mode == FG_MODE_FSR4 && !fg.logged_prepare) {
            fg.logged_prepare = true;
            if (fg.effect_stats) {
                Mk8MlfiStats stats = {0};
                stats.struct_size = sizeof stats;
                // The diagnostic export takes the context pointer itself, not a pointer to the
                // ffxContext handle that ffxDispatch/ffxConfigure take.
                if (fg.effect_stats(fg.fg_ctx, &stats) == FFX_API_RETURN_OK && stats.version == 1)
                    fprintf(stderr, "[fsr_fg] MLFI prepare frame %llu: %u preprocess + %u copy dispatches (resets %u)\n",
                            (unsigned long long)prepare.frameID, stats.prepare_dispatches, stats.copy_dispatches,
                            stats.resets);
            }
        } else if (fg.mode == FG_MODE_FSR4 && fg.effect_stats && frame_id % 600 == 0) {
            // One periodic line during a long run: the provider's own counters, so a mode that
            // stopped generating (or a dropped interpolation) is visible in the log.
            Mk8MlfiStats stats = {0};
            stats.struct_size = sizeof stats;
            if (fg.effect_stats(fg.fg_ctx, &stats) == FFX_API_RETURN_OK && stats.version == 1)
                fprintf(stderr, "[fsr_fg] MLFI progress: %llu generated frames, %u model + %u compose dispatches\n",
                        (unsigned long long)stats.frames, stats.model_dispatches, stats.compose_dispatches);
        }
    }

    // The captured depth is written as a colour attachment by the next frame's depth copy; give it
    // back to GENERAL once the prepare has consumed it.
    if (depth.real) {
        VkImageMemoryBarrier back = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        back.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        back.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        back.image = depth.image;
        back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, NULL, 0, NULL, 1, &back);
    }
    if (prepared) gate_signal(cb);
}

// Called by surface_lost() instead of vkDestroySwapchainKHR. Without a proxy it is the plain call.
extern "C" void mk8_fsr_fg_destroy_swapchain(VkDevice device, VkSwapchainKHR swapchain,
                                             const VkAllocationCallbacks *allocator) {
    if (!fg.proxied || swapchain != fg.proxy) {
        vkDestroySwapchainKHR(device, swapchain, allocator);
        return;
    }
    fprintf(stderr, "[fsr_fg] destroying the frame-interpolation swapchain\n");
    destroy_contexts();
    fg.tried = false;  // the library stays loaded; a new surface may create fresh contexts
    fg.failed = false;
}
