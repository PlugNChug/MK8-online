// Native FSR 3 frame generation (host side, no guest changes).
//
// MK8_FSR_FG=fsr3 replaces the window swapchain with the FFX frame-interpolation swapchain: the
// game renders into the provider's replacement buffers, records the frame-generation prepare
// dispatch (scene depth, motion vectors, jitter, camera), and presents through the provider, which
// inserts an interpolated image between rendered ones. Default off; every failure falls back to the
// original swapchain and says why. No provider is bundled: the library comes from MK8_FSR_FG_LIB or
// $XDG_DATA_HOME/mk8online/fsr/.
//
// The provider's swapchain ABI (3.1.7) is checked before anything is created, and the version
// descriptor sent to the provider is parsed from that reported version rather than assumed.
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

typedef struct {
    bool tried;
    bool failed;
    bool proxied;      // the window swapchain is the FFX proxy (present/acquire must be routed)
    bool interpolating;
    void *library;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxConfigure configure;
    PfnFfxDispatch dispatch;
    ffxContext sc_ctx;
    ffxContext fg_ctx;
    VkSwapchainKHR proxy;
    uint32_t version;
    PFN_vkCreateSwapchainFFXAPI create_sc;
    PFN_vkDestroySwapchainFFXAPI destroy_sc;
    PFN_vkGetSwapchainImagesKHR get_images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    VkQueue present_q;
    VkQueue acquire_q;
    uint64_t frame_id;
    uint32_t width, height;
    VkFormat format;
    bool prepared_any;
    bool logged_dispatch;
} Fg;

static Fg fg;

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

static void fail(const char *reason) {
    fg.failed = true;
    fprintf(stderr, "[fsr_fg] disabled: %s\n", reason);
}

// One clear line per process. `permanent` also stops later attempts (a missing provider/version is
// not something a resize can fix; a destroyed swapchain is retried after the surface comes back).
static bool enabled_by_env(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("MK8_FSR_FG");
        mode = (e && *e && !strcasecmp(e, "fsr3")) ? 1 : 0;
        if (e && *e && !mode) fprintf(stderr, "[fsr_fg] ignored: MK8_FSR_FG=%s (only fsr3 is implemented)\n", e);
        if (mode) {
            // The source-built provider (tools/gen_fg_blobs.py) creates contexts and reports 3.1.7,
            // but its generated SPIR-V declares storage-image formats that do not match the SDK's
            // resources (Rg32f vs R16G16_FLOAT, R32ui vs R8_UINT, ...); the GPU faults on the second
            // present. Enabling it needs an explicit acknowledgement until those blobs are fixed and
            // dispatch-validated. A provider built from AMD's signed SPIR-V also passes this check.
            const char *allow = getenv("MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER");
            if (!allow || !*allow || !strcmp(allow, "0")) {
                mode = 0;
                fprintf(stderr, "[fsr_fg] disabled: the source-built provider is not dispatch-validated "
                                "(storage-image formats in the generated shader blobs do not match the SDK resources; "
                                "the GPU faults on the second present). Set MK8_FSR_FG_ALLOW_UNVALIDATED_PROVIDER=1 only "
                                "to test a provider build.\n");
            }
        }
    }
    return mode != 0;
}

static bool load_provider(void) {
    if (fg.library) return true;  // (kept loaded across a surface recreation)
    if (fg.tried) return false;
    fg.tried = true;
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
    return true;
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

static void destroy_effect(void) {
    if (!fg.fg_ctx) return;
    if (fg.proxied && fg.configure) {
        ffxConfigureDescFrameGeneration off = {0};
        off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        off.swapChain = (void *)fg.proxy;
        off.frameGenerationEnabled = false;
        off.frameID = fg.frame_id;
        fg.configure(&fg.fg_ctx, &off.header);  // waits for pending interpolation/presents
    }
    if (fg.destroy) fg.destroy(&fg.fg_ctx, NULL);
    fg.fg_ctx = NULL;
    fg.interpolating = false;
    fg.prepared_any = false;
    fg.logged_dispatch = false;
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
    ffxReturnCode_t rc = fg.create(&fg.fg_ctx, &effect.header, NULL);
    if (rc != FFX_API_RETURN_OK || !fg.fg_ctx) {
        static char reason[128];
        snprintf(reason, sizeof reason, "frame-generation context creation failed (ffx %d)", (int)rc);
        fg.fg_ctx = NULL;
        fail(reason);
        return false;
    }
    ffxQueryGetProviderVersion provider = {0};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fg.query(&fg.fg_ctx, &provider.header) == FFX_API_RETURN_OK)
        fprintf(stderr, "[fsr_fg] frame-generation effect provider %s\n", provider.versionName ? provider.versionName : "(unknown)");
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
    if (!fg.proxied && !check_swapchain_version()) return false;
    if (fg.proxied) {
        bool changed = ci->imageExtent.width != fg.width || ci->imageExtent.height != fg.height ||
                       ci->imageFormat != fg.format;
        if (changed) destroy_effect();  // the effect's display/render sizes are fixed at creation
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
        if (changed && (!surface_format(ci->imageFormat, &display_format) ||
                        !create_effect(ci->imageExtent.width, ci->imageExtent.height, display_format))) {
            destroy_contexts();
            *swapchain = VK_NULL_HANDLE;
            if (!fg.failed) fail("frame-generation context recreation failed");
            return false;
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
    ffxReturnCode_t rc = fg.dispatch(effect, &params->header);
    if (rc == FFX_API_RETURN_OK && !fg.logged_dispatch) {
        fg.logged_dispatch = true;
        fprintf(stderr, "[fsr_fg] first interpolated frame recorded (frameID %llu, %ux%u)\n",
                (unsigned long long)params->frameID,
                params->presentColor.description.width, params->presentColor.description.height);
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
    if (!fg.configure || !fg.fg_ctx || !fg.proxied) return;
    ffxConfigureDescFrameGeneration off = {0};
    off.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    off.swapChain = (void *)fg.proxy;
    off.frameGenerationEnabled = false;
    off.frameID = fg.frame_id;
    if (fg.configure(&fg.fg_ctx, &off.header) == FFX_API_RETURN_OK) fg.interpolating = false;
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
    ffxConfigureDescFrameGeneration config = {0};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = (void *)fg.proxy;
    config.frameGenerationCallback = frame_generation_callback;
    config.frameGenerationCallbackUserContext = &fg.fg_ctx;
    config.frameGenerationEnabled = true;
    config.allowAsyncWorkloads = false;  // the present/acquire queues are already the async path here
    config.frameID = frame_id;
    ffxReturnCode_t rc = fg.configure(&fg.fg_ctx, &config.header);
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
        ffxApiGetImageResourceDescriptionVK(depth.image, depth_ci, VK_IMAGE_USAGE_SAMPLED_BIT),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = ffxApiGetResourceVK(motion.image,
        ffxApiGetImageResourceDescriptionVK(motion.image, motion_ci, VK_IMAGE_USAGE_SAMPLED_BIT),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    camera_vectors(prepare.cameraPosition, prepare.cameraUp, prepare.cameraRight, prepare.cameraForward);
    static bool said_camera;
    if (!said_camera) {
        said_camera = true;
        fprintf(stderr, "[fsr_fg] camera %s: position %.1f %.1f %.1f forward %.1f %.1f %.1f\n",
                g_fsr_vp_valid ? "from the captured view matrix" : "default (no captured view matrix)",
                prepare.cameraPosition[0], prepare.cameraPosition[1], prepare.cameraPosition[2],
                prepare.cameraForward[0], prepare.cameraForward[1], prepare.cameraForward[2]);
    }
    rc = fg.dispatch(&fg.fg_ctx, &prepare.header);
    if (rc != FFX_API_RETURN_OK) {
        static char reason[128];
        snprintf(reason, sizeof reason, "frame-generation prepare failed (ffx %d); presenting real frames only", (int)rc);
        disable_interpolation();
        fail(reason);
    } else {
        fg.interpolating = true;
        fg.prepared_any = true;
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
