/*
 * mlfi_capture: run AMD's signed FidelityFX frame-generation DLL under Wine against the host
 * Vulkan device and record the host-side MLFI layout: pipeline creation order, dispatch order and
 * geometry, descriptor bindings, buffer uploads (InitializerBuffer / CsTensorSizes) and the
 * per-frame tensor tables.
 *
 * The DLL is not redistributed here. Point --dll at the user's own copy from the FSR SDK 2.4.0
 * archive. Raw shader/model bytes are written below --out (outside the repository); only derived
 * metadata belongs in docs/en/reports/MLFI-CAPTURE.md.
 *
 * How the interception works
 * --------------------------
 * The DLL imports a handful of Vulkan entry points statically and resolves the rest through the
 * vkDeviceProcAddr supplied in VkDeviceContext. Both paths are covered:
 *   - the DLL's import address table is patched after LoadLibraryA (IAT hook), and
 *   - VkDeviceContext.vkDeviceProcAddr is our wrapper, which returns wrappers for the entry
 *     points we record and the driver pointer otherwise.
 *
 * Build: see build.sh. It needs mingw-w64, the FidelityFX SDK headers and Vulkan headers.
 *
 * Exit codes: 0 captured, 1 usage/environment, 2 Vulkan, 3 FFX, 4 DLL/export.
 */

#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "ffx_api.h"
#include "ffx_framegeneration.h"
#include "vk/ffx_api_vk.h"

#define MAX_BUFFERS 4096
#define MAX_MAPPINGS 256
#define MAX_SNAPSHOTS 256
#define MAX_MODULES 1024
#define MAX_PIPELINES 1024
#define MAX_SETS 4096
#define MAX_SET_LAYOUTS 512
#define MAX_DESC_BINDINGS 8192
#define MAX_BOUND_SETS 16
#define MAX_IMAGES 256
#define MAX_HOOKS 64
#define MAX_TENSOR_TABLES 512

/* ------------------------------------------------------------------ logging */

static CRITICAL_SECTION g_lock;
static FILE *g_log;
static FILE *g_events;
static FILE *g_passes;
static char g_out[MAX_PATH];
static uint64_t g_seq;
static const char *g_stage = "init";
static int g_substitute = 1;

static void log_open(void) {
    InitializeCriticalSection(&g_lock);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\log.txt", g_out);
    g_log = fopen(path, "w");
    snprintf(path, sizeof(path), "%s\\events.tsv", g_out);
    g_events = fopen(path, "w");
    snprintf(path, sizeof(path), "%s\\passes.tsv", g_out);
    g_passes = fopen(path, "w");
    if (!g_log || !g_events || !g_passes) {
        fprintf(stderr, "cannot open output files under %s\n", g_out);
        exit(1);
    }
    fprintf(g_events, "seq\tstage\tkind\ta\tb\tc\td\tdetail\n");
    fprintf(g_log, "# mlfi_capture log\n");
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    EnterCriticalSection(&g_lock);
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_lock);
}

static void event(const char *kind, const char *a, const char *b, const char *c, const char *d, const char *detail) {
    EnterCriticalSection(&g_lock);
    fprintf(g_events, "%llu\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n",
            (unsigned long long)++g_seq, g_stage, kind, a ? a : "", b ? b : "", c ? c : "", d ? d : "", detail ? detail : "");
    fflush(g_events);
    LeaveCriticalSection(&g_lock);
}

/* ------------------------------------------------------------------ hashing */

static uint64_t fnv1a(const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void hex64(uint64_t v, char *out, size_t n) { snprintf(out, n, "%016llx", (unsigned long long)v); }

/* ------------------------------------------------------------------ raw dump */

static void dump_raw(const char *tag, const void *data, size_t size) {
    if (!data || !size || size > (256u << 20)) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\raw\\%06llu_%s.bin", g_out, (unsigned long long)g_seq, tag);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

static void dump_named(const char *name, const void *data, size_t size) {
    if (!data || !size || size > (64u << 20)) return;
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\raw\\%s.bin", g_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, size, f);
    fclose(f);
}

/* ------------------------------------------------------------------ Vulkan tables */

static PFN_vkGetInstanceProcAddr real_GIPA;
static PFN_vkGetDeviceProcAddr real_GDPA;

static PFN_vkCreateInstance vkCreateInstance;
static PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceFeatures vkGetPhysicalDeviceFeatures;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties;
static PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice vkCreateDevice;
static PFN_vkGetDeviceQueue vkGetDeviceQueue;
static PFN_vkDeviceWaitIdle vkDeviceWaitIdle;
static PFN_vkCreateCommandPool vkCreateCommandPool;
static PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer vkBeginCommandBuffer;
static PFN_vkEndCommandBuffer vkEndCommandBuffer;
static PFN_vkQueueSubmit vkQueueSubmit;
static PFN_vkQueueWaitIdle vkQueueWaitIdle;
static PFN_vkCreateImage vkCreateImage;
static PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements;
static PFN_vkBindImageMemory vkBindImageMemory;
static PFN_vkCreateImageView vkCreateImageView;
static PFN_vkDestroyImage vkDestroyImage;
static PFN_vkDestroyImageView vkDestroyImageView;
static PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier;
static PFN_vkCreateBuffer vkCreateBuffer;
static PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements;
static PFN_vkBindBufferMemory vkBindBufferMemory;
static PFN_vkDestroyBuffer vkDestroyBuffer;
static PFN_vkAllocateMemory vkAllocateMemory;
static PFN_vkFreeMemory vkFreeMemory;
static PFN_vkMapMemory vkMapMemory;
static PFN_vkUnmapMemory vkUnmapMemory;
static PFN_vkCreateShaderModule vkCreateShaderModule;
static PFN_vkDestroyShaderModule vkDestroyShaderModule;
static PFN_vkCreateDescriptorSetLayout vkCreateDescriptorSetLayout;
static PFN_vkAllocateDescriptorSets vkAllocateDescriptorSets;
static PFN_vkUpdateDescriptorSets vkUpdateDescriptorSets;
static PFN_vkCreatePipelineLayout vkCreatePipelineLayout;
static PFN_vkCreateComputePipelines vkCreateComputePipelines;
static PFN_vkCmdBindPipeline vkCmdBindPipeline;
static PFN_vkCmdBindDescriptorSets vkCmdBindDescriptorSets;
static PFN_vkCmdDispatch vkCmdDispatch;
static PFN_vkCmdDispatchIndirect vkCmdDispatchIndirect;
static PFN_vkCmdUpdateBuffer vkCmdUpdateBuffer;
static PFN_vkCmdCopyBuffer vkCmdCopyBuffer;
static PFN_vkCmdFillBuffer vkCmdFillBuffer;
static PFN_vkCmdPushConstants vkCmdPushConstants;
static PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage;
static PFN_vkCmdCopyImageToBuffer vkCmdCopyImageToBuffer;

static VkInstance g_instance;
static VkPhysicalDevice g_physical;
static VkDevice g_device;
static uint32_t g_queue_family;

/* ------------------------------------------------------------------ tracked objects */

typedef struct {
    VkBuffer buf;
    VkDeviceSize size;
    VkDeviceMemory mem;
    VkDeviceSize mem_offset;
    uint8_t *shadow;          /* mirror of host-written bytes, when affordable */
    uint8_t *snapshot;        /* copy of a CPU mapping, taken on unmap */
    int shadow_valid;
} Buf;

typedef struct {
    VkDeviceMemory mem;
    VkDeviceSize offset, size;
    void *ptr;
    int active;
} Mapping;

typedef struct {
    VkDeviceMemory mem;
    VkDeviceSize offset, size;
    uint8_t *data;
} MemSnap;

typedef struct {
    VkShaderModule module;
    uint32_t size;
    uint64_t hash;
    char entry[64];
} Module;

typedef struct {
    VkPipeline pipe;
    char entry[64];
    uint32_t module_size;
    uint64_t module_hash;
    int substituted;
} Pipeline;

typedef struct {
    VkDescriptorSetLayout layout;
    uint32_t count;
    struct { uint32_t binding; VkDescriptorType type; } items[32];
} DescLayout;

typedef struct {
    VkDescriptorSet set;
    VkDescriptorSetLayout layout;
} SetInfo;

typedef struct {
    VkDescriptorSet set;
    uint32_t binding;
    VkDescriptorType type;
    VkBuffer buffer;
    VkDeviceSize offset, range;
    VkImageView image;
} DescBinding;

typedef struct {
    VkDescriptorSet set;
    uint32_t first;
    uint32_t count;
    uint32_t dyn_count;
    VkDeviceSize dyn[8];
} BoundSet;

typedef struct {
    VkImage image;
    uint32_t w, h;
    VkFormat format;
    char name[32];
} ImageInfo;

typedef struct {
    char stage[16];
    char entry[64];
    VkPipeline pipe;
    int substituted;
    uint32_t gx, gy, gz;
    VkDeviceSize weight_offset, weight_range;
    VkDeviceSize tensor_offset, constants_offset;
    int tensor_id;
} PassRec;

static Buf g_bufs[MAX_BUFFERS]; static int g_buf_count;
static Mapping g_maps[MAX_MAPPINGS]; static int g_map_count;
static MemSnap g_snaps[MAX_SNAPSHOTS]; static int g_snap_count;
static Module g_mods[MAX_MODULES]; static int g_mod_count;
static Pipeline g_pipes[MAX_PIPELINES]; static int g_pipe_count;
static DescLayout g_layouts[MAX_SET_LAYOUTS]; static int g_layout_count;
static SetInfo g_sets[MAX_SETS]; static int g_set_count;
static DescBinding g_desc[MAX_DESC_BINDINGS]; static int g_desc_count;
static BoundSet g_bound[MAX_BOUND_SETS]; static int g_bound_count;
static ImageInfo g_images[MAX_IMAGES]; static int g_image_count;
static PassRec g_pass_records[MAX_PIPELINES]; static int g_pass_count;

static Buf *find_buf(VkBuffer b) {
    for (int i = 0; i < g_buf_count; i++) if (g_bufs[i].buf == b) return &g_bufs[i];
    return NULL;
}
static Module *find_mod(VkShaderModule m) {
    for (int i = 0; i < g_mod_count; i++) if (g_mods[i].module == m) return &g_mods[i];
    return NULL;
}
static Pipeline *find_pipe(VkPipeline p) {
    for (int i = 0; i < g_pipe_count; i++) if (g_pipes[i].pipe == p) return &g_pipes[i];
    return NULL;
}
static DescLayout *find_layout(VkDescriptorSetLayout l) {
    for (int i = 0; i < g_layout_count; i++) if (g_layouts[i].layout == l) return &g_layouts[i];
    return NULL;
}
static SetInfo *find_set(VkDescriptorSet s) {
    for (int i = 0; i < g_set_count; i++) if (g_sets[i].set == s) return &g_sets[i];
    return NULL;
}

static uint8_t *buffer_bytes(Buf *b, VkDeviceSize offset, VkDeviceSize len, VkDeviceSize *available) {
    *available = 0;
    if (!b || offset >= b->size) return NULL;
    if (offset + len > b->size) len = b->size - offset;
    /* live CPU mapping first */
    for (int i = g_map_count - 1; i >= 0; i--) {
        Mapping *m = &g_maps[i];
        if (m->active && m->mem == b->mem && b->mem_offset + offset >= m->offset &&
            b->mem_offset + offset + len <= m->offset + m->size) {
            *available = len;
            return (uint8_t *)m->ptr + (b->mem_offset + offset - m->offset);
        }
    }
    for (int i = g_snap_count - 1; i >= 0; i--) {
        MemSnap *s = &g_snaps[i];
        if (s->mem == b->mem && b->mem_offset + offset >= s->offset &&
            b->mem_offset + offset + len <= s->offset + s->size) {
            *available = len;
            return s->data + (b->mem_offset + offset - s->offset);
        }
    }
    if (b->shadow_valid && b->shadow && offset + len <= b->size) {
        *available = len;
        return b->shadow + offset;
    }
    return NULL;
}

static void shadow_write(Buf *b, VkDeviceSize offset, const void *data, VkDeviceSize len) {
    if (!b || !data || !len || offset >= b->size) return;
    if (offset + len > b->size) len = b->size - offset;
    /* Do not mirror very large working buffers; they are not needed to recover the layout. */
    if (b->size > (48u << 20)) return;
    if (!b->shadow) {
        b->shadow = (uint8_t *)calloc(1, (size_t)b->size);
        if (!b->shadow) return;
    }
    memcpy(b->shadow + offset, data, (size_t)len);
    b->shadow_valid = 1;
}

/* ------------------------------------------------------------------ wrappers: memory and buffers */

static VkResult VKAPI_CALL wCreateBuffer(VkDevice dev, const VkBufferCreateInfo *ci, const VkAllocationCallbacks *a, VkBuffer *out) {
    VkResult r = vkCreateBuffer(dev, ci, a, out);
    if (r == VK_SUCCESS && g_buf_count < MAX_BUFFERS) {
        Buf *b = &g_bufs[g_buf_count++];
        memset(b, 0, sizeof(*b));
        b->buf = *out; b->size = ci->size;
        char s[64]; snprintf(s, sizeof(s), "%llu", (unsigned long long)ci->size);
        event("createbuf", s, "", "", "", "");
        log_msg("[buffer] %p size=%llu usage=0x%x", (void *)*out, (unsigned long long)ci->size, ci->usage);
    }
    return r;
}

static VkResult VKAPI_CALL wBindBufferMemory(VkDevice dev, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    VkResult r = vkBindBufferMemory(dev, buf, mem, off);
    if (r == VK_SUCCESS) {
        Buf *b = find_buf(buf);
        if (b) { b->mem = mem; b->mem_offset = off; }
        event("bindbuf", "", "", "", "", "");
        log_msg("[buffer] bind %p mem=%p off=%llu", (void *)buf, (void *)mem, (unsigned long long)off);
    }
    return r;
}

static VkResult VKAPI_CALL wAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    VkResult r = vkAllocateMemory(dev, ai, a, out);
    if (r == VK_SUCCESS) {
        char s[64]; snprintf(s, sizeof(s), "%llu", (unsigned long long)ai->allocationSize);
        event("allocmem", s, "", "", "", "");
        log_msg("[memory] %p size=%llu type=%u", (void *)*out, (unsigned long long)ai->allocationSize, ai->memoryTypeIndex);
    }
    return r;
}

static VkResult VKAPI_CALL wMapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags flags, void **ptr) {
    VkResult r = vkMapMemory(dev, mem, off, size, flags, ptr);
    if (r == VK_SUCCESS) {
        if (g_map_count < MAX_MAPPINGS) {
            Mapping *m = &g_maps[g_map_count++];
            m->mem = mem; m->offset = off; m->size = size; m->ptr = *ptr; m->active = 1;
        }
        char s[64]; snprintf(s, sizeof(s), "%llu", (unsigned long long)size);
        event("mapmem", s, "", "", "", "");
        log_msg("[memory] map %p off=%llu size=%llu -> %p", (void *)mem, (unsigned long long)off, (unsigned long long)size, *ptr);
    }
    return r;
}

static void VKAPI_CALL wUnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    /* Snapshot before the mapping goes away: the model upload is copied from a mapped staging
     * buffer that the DLL unmaps before recording the copy. */
    for (int i = g_map_count - 1; i >= 0; i--) {
        Mapping *m = &g_maps[i];
        if (m->active && m->mem == mem) {
            if (g_snap_count < MAX_SNAPSHOTS) {
                MemSnap *s = &g_snaps[g_snap_count++];
                s->mem = mem; s->offset = m->offset; s->size = m->size;
                s->data = (uint8_t *)malloc((size_t)m->size);
                if (s->data) {
                    memcpy(s->data, m->ptr, (size_t)m->size);
                    uint64_t h = fnv1a(s->data, (size_t)m->size);
                    log_msg("[memory] snapshot %p off=%llu size=%llu hash=%016llx", (void *)mem,
                            (unsigned long long)m->offset, (unsigned long long)m->size, (unsigned long long)h);
                    char tag[96]; snprintf(tag, sizeof(tag), "memsnap_%p_%llu_%llu", (void*)mem,
                            (unsigned long long)m->offset, (unsigned long long)m->size);
                    dump_named(tag, s->data, (size_t)m->size);
                    char a[64], b[64], c[32];
                    snprintf(a, sizeof(a), "%p", (void*)mem);
                    snprintf(b, sizeof(b), "%llu", (unsigned long long)m->size);
                    hex64(h, c, sizeof(c));
                    event("memsnap", a, b, c, "", "");
                }
            }
            m->active = 0;
        }
    }
    vkUnmapMemory(dev, mem);
}

/* ------------------------------------------------------------------ wrappers: shader modules and pipelines */

static const uint32_t g_dummy_spv[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x0000000a, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004,
    0x6e69616d, 0x00000000, 0x00040047, 0x00000009, 0x0000000b, 0x00000019, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00040017,
    0x00000007, 0x00000006, 0x00000003, 0x0004002b, 0x00000006, 0x00000008, 0x00000001, 0x0006002c,
    0x00000007, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x000100fd, 0x00010038
};

static VkResult VKAPI_CALL wCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *out) {
    VkResult r = vkCreateShaderModule(dev, ci, a, out);
    if (r == VK_SUCCESS && g_mod_count < MAX_MODULES) {
        Module *m = &g_mods[g_mod_count++];
        memset(m, 0, sizeof(*m));
        m->module = *out;
        m->size = (uint32_t)ci->codeSize;
        m->hash = fnv1a(ci->pCode, ci->codeSize);
        const uint32_t *w = ci->pCode;
        uint32_t words = (uint32_t)(ci->codeSize / 4);
        for (uint32_t at = 5; at + 1 <= words; ) {
            uint32_t word = w[at], cnt = word >> 16, op = word & 0xffff;
            if (!cnt) break;
            if (op == 15 && cnt >= 4) {
                const char *nm = (const char *)(w + at + 3);
                strncpy(m->entry, nm, sizeof(m->entry) - 1);
                break;
            }
            at += cnt;
        }
        char h[32], s[32];
        hex64(m->hash, h, sizeof(h));
        snprintf(s, sizeof(s), "%u", m->size);
        event("module", m->entry, s, h, "", "");
        log_msg("[module] %p size=%u hash=%s entry=%s", (void *)*out, m->size, h, m->entry);
        char tag[160];
        snprintf(tag, sizeof(tag), "module_%s_%u", m->entry[0] ? m->entry : "unknown", m->size);
        dump_raw(tag, ci->pCode, ci->codeSize);
    }
    return r;
}

static int is_known_driver_crash(const char *entry, uint32_t module_size) {
    if (!g_substitute) return 0;
    static const char *bad[] = {
        "mlfi_4k_pass151", "mlfi_4k_pass152", "mlfi_4k_pass190",
        "mlfi_8k_pass151", "mlfi_8k_pass152", "mlfi_8k_pass190"
    };
    if (!entry) return 0;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        if (!strcmp(entry, bad[i])) return 1;
    /* RADV also segfaults compiling the 5,004-byte splat/preprocess 'main' module. */
    if (!strcmp(entry, "main") && module_size == 5004) return 1;
    return 0;
}

static VkPipeline make_substitute_pipeline(VkDevice dev, VkPipelineLayout layout) {
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof(g_dummy_spv);
    smci.pCode = g_dummy_spv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &smci, NULL, &module) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(dev, module, NULL);
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    return pipe;
}

static VkResult VKAPI_CALL wCreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t count,
        const VkComputePipelineCreateInfo *infos, const VkAllocationCallbacks *a, VkPipeline *pipes) {
    for (uint32_t i = 0; i < count; i++) {
        const char *entry = infos[i].stage.pName ? infos[i].stage.pName : "";
        Module *m = find_mod(infos[i].stage.module);
        uint32_t msize = m ? m->size : 0;
        uint64_t mhash = m ? m->hash : 0;
        int subst = 0;
        VkResult r;
        const VkSpecializationInfo *spec = infos[i].stage.pSpecializationInfo;
        if (spec) {
            log_msg("[spec] entry=%s mapEntries=%u dataSize=%zu", entry, spec->mapEntryCount, spec->dataSize);
            for (uint32_t k = 0; k < spec->mapEntryCount; k++)
                log_msg("[spec]   id=%u offset=%u size=%zu", spec->pMapEntries[k].constantID, spec->pMapEntries[k].offset, spec->pMapEntries[k].size);
            if (spec->dataSize && spec->pData) {
                char h[32]; hex64(fnv1a(spec->pData, spec->dataSize), h, sizeof(h));
                log_msg("[spec]   data hash=%s", h);
                char tag[96]; snprintf(tag, sizeof(tag), "spec_%06llu_%s", (unsigned long long)g_seq, entry);
                dump_raw(tag, spec->pData, spec->dataSize);
            }
        }
        if (is_known_driver_crash(entry, msize)) {
            subst = 1;
            pipes[i] = make_substitute_pipeline(dev, infos[i].layout);
            r = pipes[i] ? VK_SUCCESS : VK_ERROR_UNKNOWN;
            log_msg("[pipeline] SUBSTITUTE entry=%s module_size=%u module_hash=%016llx (known RADV compile crash)", entry, msize, (unsigned long long)mhash);
        } else {
            r = vkCreateComputePipelines(dev, cache, 1, &infos[i], a, &pipes[i]);
            log_msg("[pipeline] entry=%s module_size=%u module_hash=%016llx result=%d pipe=%p", entry, msize, (unsigned long long)mhash, r, (void *)pipes[i]);
        }
        if (r != VK_SUCCESS) return r;
        if (g_pipe_count < MAX_PIPELINES) {
            Pipeline *p = &g_pipes[g_pipe_count++];
            memset(p, 0, sizeof(*p));
            p->pipe = pipes[i];
            strncpy(p->entry, entry, sizeof(p->entry) - 1);
            p->module_size = msize;
            p->module_hash = mhash;
            p->substituted = subst;
            char h[32], s[32], idx[32];
            hex64(mhash, h, sizeof(h));
            snprintf(s, sizeof(s), "%u", msize);
            snprintf(idx, sizeof(idx), "%d", g_pipe_count - 1);
            event("pipeline", idx, entry, s, h, subst ? "substituted" : "");
        }
    }
    return VK_SUCCESS;
}


static VkResult VKAPI_CALL wCreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
    VkResult r = vkCreatePipelineLayout(dev, ci, a, out);
    if (r == VK_SUCCESS) {
        log_msg("[pipeline_layout] %p sets=%u push_ranges=%u", (void*)*out, ci->setLayoutCount, ci->pushConstantRangeCount);
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; i++)
            log_msg("[pipeline_layout]   push stage=0x%x off=%u size=%u", ci->pPushConstantRanges[i].stageFlags,
                    ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size);
    }
    return r;
}

/* ------------------------------------------------------------------ wrappers: descriptors */

static VkResult VKAPI_CALL wCreateDescriptorSetLayout(VkDevice dev, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
    VkResult r = vkCreateDescriptorSetLayout(dev, ci, a, out);
    if (r == VK_SUCCESS && g_layout_count < MAX_SET_LAYOUTS) {
        DescLayout *l = &g_layouts[g_layout_count++];
        memset(l, 0, sizeof(*l));
        l->layout = *out;
        l->count = ci->bindingCount < 32 ? ci->bindingCount : 32;
        for (uint32_t i = 0; i < l->count; i++) {
            l->items[i].binding = ci->pBindings[i].binding;
            l->items[i].type = ci->pBindings[i].descriptorType;
        }
        log_msg("[layout] %p bindings=%u", (void *)*out, ci->bindingCount);
        for (uint32_t i = 0; i < ci->bindingCount; i++)
            log_msg("[layout]   binding=%u type=%d count=%u", ci->pBindings[i].binding, ci->pBindings[i].descriptorType, ci->pBindings[i].descriptorCount);
    }
    return r;
}

static VkResult VKAPI_CALL wAllocateDescriptorSets(VkDevice dev, const VkDescriptorSetAllocateInfo *ai, VkDescriptorSet *sets) {
    VkResult r = vkAllocateDescriptorSets(dev, ai, sets);
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < ai->descriptorSetCount && g_set_count < MAX_SETS; i++) {
            g_sets[g_set_count].set = sets[i];
            g_sets[g_set_count].layout = ai->pSetLayouts[i];
            g_set_count++;
        }
    }
    return r;
}

static void set_desc_binding(VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset, VkDeviceSize range, VkImageView image) {
    for (int i = 0; i < g_desc_count; i++) {
        if (g_desc[i].set == set && g_desc[i].binding == binding) {
            g_desc[i].type = type; g_desc[i].buffer = buffer; g_desc[i].offset = offset; g_desc[i].range = range; g_desc[i].image = image;
            return;
        }
    }
    if (g_desc_count < MAX_DESC_BINDINGS) {
        DescBinding *d = &g_desc[g_desc_count++];
        d->set = set; d->binding = binding; d->type = type; d->buffer = buffer; d->offset = offset; d->range = range; d->image = image;
    }
}

static void VKAPI_CALL wUpdateDescriptorSets(VkDevice dev, uint32_t writes, const VkWriteDescriptorSet *w, uint32_t copies, const VkCopyDescriptorSet *c) {
    for (uint32_t i = 0; i < writes; i++) {
        const VkWriteDescriptorSet *x = &w[i];
        if ((x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
             x->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) && x->pBufferInfo) {
            set_desc_binding(x->dstSet, x->dstBinding, x->descriptorType, x->pBufferInfo[0].buffer, x->pBufferInfo[0].offset, x->pBufferInfo[0].range, VK_NULL_HANDLE);
            log_msg("[desc] set=%p binding=%u type=%d buffer=%p off=%llu range=%llu", (void *)x->dstSet, x->dstBinding, x->descriptorType,
                    (void *)x->pBufferInfo[0].buffer, (unsigned long long)x->pBufferInfo[0].offset, (unsigned long long)x->pBufferInfo[0].range);
            char a[32], b[32], c[64], d[64];
            snprintf(a, sizeof(a), "%u", x->dstBinding);
            snprintf(b, sizeof(b), "%d", x->descriptorType);
            snprintf(c, sizeof(c), "%llu", (unsigned long long)x->pBufferInfo[0].offset);
            snprintf(d, sizeof(d), "%llu", (unsigned long long)x->pBufferInfo[0].range);
            event("descwrite", a, b, c, d, "");
        } else if ((x->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || x->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) && x->pImageInfo) {
            set_desc_binding(x->dstSet, x->dstBinding, x->descriptorType, VK_NULL_HANDLE, 0, 0, x->pImageInfo[0].imageView);
            log_msg("[desc] set=%p binding=%u type=%d imageView=%p", (void *)x->dstSet, x->dstBinding, x->descriptorType, (void *)x->pImageInfo[0].imageView);
        }
    }
    vkUpdateDescriptorSets(dev, writes, w, copies, c);
}

/* ------------------------------------------------------------------ wrappers: command recording */

static VkPipeline g_bound_pipeline;

static void VKAPI_CALL wCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bind, VkPipeline pipe) {
    if (bind == VK_PIPELINE_BIND_POINT_COMPUTE) g_bound_pipeline = pipe;
    Pipeline *p = find_pipe(pipe);
    event("bind", p ? p->entry : "?", "", "", "", "");
    vkCmdBindPipeline(cmd, bind, pipe);
}

static void VKAPI_CALL wCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bind, VkPipelineLayout layout,
        uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynCount, const uint32_t *dyn) {
    if (bind == VK_PIPELINE_BIND_POINT_COMPUTE) {
        /* replace the bound range */
        BoundSet next[MAX_BOUND_SETS]; int n = 0;
        for (int i = 0; i < g_bound_count; i++) {
            if (g_bound[i].first + g_bound[i].count <= first || g_bound[i].first >= first + count) next[n++] = g_bound[i];
        }
        for (uint32_t i = 0; i < count && n < MAX_BOUND_SETS; i++) {
            BoundSet *b = &next[n++];
            memset(b, 0, sizeof(*b));
            b->set = sets[i]; b->first = first + i; b->count = 1;
            b->dyn_count = dynCount < 8 ? dynCount : 8;
            for (uint32_t k = 0; k < b->dyn_count; k++) b->dyn[k] = dyn ? dyn[k] : 0;
        }
        memcpy(g_bound, next, sizeof(BoundSet) * (size_t)n);
        g_bound_count = n;
        log_msg("[bindset] first=%u count=%u dyn=%u", first, count, dynCount);
    }
    vkCmdBindDescriptorSets(cmd, bind, layout, first, count, sets, dynCount, dyn);
}

/* Locate the buffer bound to a descriptor binding at record time. */
static int bound_descriptor(uint32_t binding, VkBuffer *buffer, VkDeviceSize *offset, VkDeviceSize *range) {
    for (int i = 0; i < g_bound_count; i++) {
        for (int d = 0; d < g_desc_count; d++) {
            if (g_desc[d].set == g_bound[i].set && g_desc[d].binding == binding && g_desc[d].buffer) {
                VkDeviceSize dyn = 0;
                SetInfo *si = find_set(g_bound[i].set);
                DescLayout *sl = si ? find_layout(si->layout) : NULL;
                if (sl && (g_desc[d].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || g_desc[d].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC)) {
                    uint32_t index = 0;
                    for (uint32_t k = 0; k < sl->count; k++) {
                        if (sl->items[k].type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC || sl->items[k].type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC) {
                            if (sl->items[k].binding == binding) { if (index < g_bound[i].dyn_count) dyn = g_bound[i].dyn[index]; break; }
                            index++;
                        }
                    }
                }
                *buffer = g_desc[d].buffer;
                *offset = g_desc[d].offset + dyn;
                *range = g_desc[d].range;
                return 1;
            }
        }
    }
    return 0;
}

static FILE *g_tensor_index;
static int g_tensor_count;

static int dump_tensor_table(VkBuffer buffer, VkDeviceSize offset, const char *entry) {
    Buf *b = find_buf(buffer);
    VkDeviceSize avail = 0;
    uint8_t *data = buffer_bytes(b, offset, 1472, &avail);
    if (!data || avail < 1472) return -1;
    uint64_t hash = fnv1a(data, 1472);
    int id = -1;
    for (int i = 0; i < g_tensor_count; i++) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\tensor_%03d.bin", g_out, i);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        uint8_t tmp[1472];
        if (fread(tmp, 1, 1472, f) == 1472 && fnv1a(tmp, 1472) == hash) { id = i; fclose(f); break; }
        fclose(f);
    }
    if (id < 0) {
        id = g_tensor_count++;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\tensor_%03d.bin", g_out, id);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(data, 1, 1472, f); fclose(f); }
        fprintf(g_tensor_index, "%d\t%s\t%016llx\n", id, entry, (unsigned long long)hash);
        fflush(g_tensor_index);
    }
    char idbuf[32]; snprintf(idbuf, sizeof(idbuf), "%d", id);
    event("tensor", entry, idbuf, "", "", "");
    return id;
}

static void VKAPI_CALL wCmdDispatch(VkCommandBuffer cmd, uint32_t x, uint32_t y, uint32_t z) {
    Pipeline *p = find_pipe(g_bound_pipeline);
    const char *entry = p ? p->entry : "?";
    VkDeviceSize weight_off = 0, weight_range = 0, tensor_off = 0, constants_off = 0;
    static const uint32_t want_bindings[] = { 0, 128, 256, 257 };
    for (unsigned wi = 0; wi < sizeof(want_bindings) / sizeof(want_bindings[0]); wi++) {
        VkBuffer bb = VK_NULL_HANDLE; VkDeviceSize boff = 0, brange = 0;
        if (!bound_descriptor(want_bindings[wi], &bb, &boff, &brange) || !bb) continue;
        char bno[16], ofs[64], rng[64], bufh[64];
        snprintf(bno, sizeof(bno), "%u", want_bindings[wi]);
        snprintf(ofs, sizeof(ofs), "%llu", (unsigned long long)boff);
        snprintf(rng, sizeof(rng), "%llu", (unsigned long long)brange);
        snprintf(bufh, sizeof(bufh), "%p", (void*)bb);
        event("passdesc", entry, bno, ofs, rng, bufh);
        if (want_bindings[wi] == 0) { weight_off = boff; weight_range = brange; }
        if (want_bindings[wi] == 256) constants_off = boff;
        if (want_bindings[wi] == 257) tensor_off = boff;
        log_msg("[passdesc] %s binding=%u buffer=%p off=%llu range=%llu", entry, want_bindings[wi], (void*)bb,
                (unsigned long long)boff, (unsigned long long)brange);
        if (want_bindings[wi] == 256 && brange && brange <= 512) {
            Buf *bbuf = find_buf(bb);
            VkDeviceSize avail = 0;
            uint8_t *data = buffer_bytes(bbuf, boff, brange, &avail);
            if (data && avail >= brange) {
                char h[32]; hex64(fnv1a(data, (size_t)brange), h, sizeof(h));
                event("constants", entry, ofs, rng, h, "");
                log_msg("[constants] %s off=%llu size=%llu hash=%s first=%02x%02x%02x%02x", entry,
                        (unsigned long long)boff, (unsigned long long)brange, h, data[0], data[1], data[2], data[3]);
                char tag[96]; snprintf(tag, sizeof(tag), "const_%06llu_%s", (unsigned long long)g_seq, entry);
                dump_raw(tag, data, (size_t)brange);
            }
        }
    }
    VkBuffer tb = VK_NULL_HANDLE; VkDeviceSize toff = 0, trange = 0;
    int tensor_id = -1;
    if (bound_descriptor(257, &tb, &toff, &trange) && tb) tensor_id = dump_tensor_table(tb, toff, entry);
    if (g_pass_count < MAX_PIPELINES) {
        PassRec *rec = &g_pass_records[g_pass_count++];
        memset(rec, 0, sizeof(*rec));
        strncpy(rec->stage, g_stage, sizeof(rec->stage) - 1);
        strncpy(rec->entry, entry, sizeof(rec->entry) - 1);
        rec->pipe = g_bound_pipeline;
        rec->substituted = p && p->substituted;
        rec->gx = x; rec->gy = y; rec->gz = z;
        rec->weight_offset = weight_off; rec->weight_range = weight_range;
        rec->tensor_offset = tensor_off; rec->constants_offset = constants_off;
        rec->tensor_id = tensor_id;
    }
    char a[32], b[32], c[32], d[32];
    snprintf(a, sizeof(a), "%u", x); snprintf(b, sizeof(b), "%u", y); snprintf(c, sizeof(c), "%u", z);
    snprintf(d, sizeof(d), "%p", (void *)g_bound_pipeline);
    event("dispatch", entry, a, b, c, d);
    log_msg("[dispatch] %s groups=%ux%ux%u pipeline=%p%s", entry, x, y, z, (void *)g_bound_pipeline, p && p->substituted ? " (substituted)" : "");
    vkCmdDispatch(cmd, x, y, z);
}

static void VKAPI_CALL wCmdDispatchIndirect(VkCommandBuffer cmd, VkBuffer buf, VkDeviceSize off) {
    Pipeline *p = find_pipe(g_bound_pipeline);
    char a[32]; snprintf(a, sizeof(a), "%llu", (unsigned long long)off);
    event("dispatch_indirect", p ? p->entry : "?", a, "", "", "");
    log_msg("[dispatch] %s indirect buffer=%p off=%llu", p ? p->entry : "?", (void *)buf, (unsigned long long)off);
    vkCmdDispatchIndirect(cmd, buf, off);
}

static void VKAPI_CALL wCmdUpdateBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, const void *data) {
    Buf *b = find_buf(dst);
    shadow_write(b, off, data, size);
    uint64_t hash = fnv1a(data, (size_t)size);
    char a[64], s[64], h[32];
    snprintf(a, sizeof(a), "%llu", (unsigned long long)off);
    snprintf(s, sizeof(s), "%llu", (unsigned long long)size);
    hex64(hash, h, sizeof(h));
    event("updatebuf", a, s, h, "", "");
    log_msg("[update] dst=%p off=%llu size=%llu hash=%s", (void *)dst, (unsigned long long)off, (unsigned long long)size, h);
    if (size <= (64u << 10)) {
        char tag[64]; snprintf(tag, sizeof(tag), "update_%llu_%llu", (unsigned long long)off, (unsigned long long)size);
        dump_raw(tag, data, (size_t)size);
    }
    vkCmdUpdateBuffer(cmd, dst, off, size, data);
}

static void VKAPI_CALL wCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t regions, const VkBufferCopy *rc) {
    for (uint32_t i = 0; i < regions; i++) {
        Buf *sb = find_buf(src);
        VkDeviceSize avail = 0;
        uint8_t *data = buffer_bytes(sb, rc[i].srcOffset, rc[i].size, &avail);
        char a[64], b[64], c[64];
        snprintf(a, sizeof(a), "%llu", (unsigned long long)rc[i].size);
        snprintf(b, sizeof(b), "%llu", (unsigned long long)rc[i].srcOffset);
        snprintf(c, sizeof(c), "%llu", (unsigned long long)rc[i].dstOffset);
        if (data && avail >= rc[i].size) {
            uint64_t hash = fnv1a(data, (size_t)rc[i].size);
            char h[32]; hex64(hash, h, sizeof(h));
            event("copybuf", a, b, c, h, "");
            log_msg("[copy] src=%p dst=%p size=%llu srcOff=%llu dstOff=%llu hash=%s", (void *)src, (void *)dst,
                    (unsigned long long)rc[i].size, (unsigned long long)rc[i].srcOffset, (unsigned long long)rc[i].dstOffset, h);
            if (rc[i].size <= (64u << 20)) {
                char tag[64]; snprintf(tag, sizeof(tag), "copy_%llu_%llu", (unsigned long long)rc[i].dstOffset, (unsigned long long)rc[i].size);
                dump_raw(tag, data, (size_t)rc[i].size);
            }
            Buf *db = find_buf(dst);
            shadow_write(db, rc[i].dstOffset, data, rc[i].size);
        } else {
            event("copybuf", a, b, c, "", "source-unavailable");
            log_msg("[copy] src=%p dst=%p size=%llu srcOff=%llu dstOff=%llu source bytes unavailable", (void *)src, (void *)dst,
                    (unsigned long long)rc[i].size, (unsigned long long)rc[i].srcOffset, (unsigned long long)rc[i].dstOffset);
        }
    }
    vkCmdCopyBuffer(cmd, src, dst, regions, rc);
}

static void VKAPI_CALL wCmdFillBuffer(VkCommandBuffer cmd, VkBuffer dst, VkDeviceSize off, VkDeviceSize size, uint32_t value) {
    Buf *b = find_buf(dst);
    if (b && b->shadow && off + size <= b->size) {
        uint32_t *p = (uint32_t *)(b->shadow + off);
        for (VkDeviceSize i = 0; i + 4 <= size; i += 4) p[i / 4] = value;
        b->shadow_valid = 1;
    }
    event("fillbuf", "", "", "", "", "");
    vkCmdFillBuffer(cmd, dst, off, size, value);
}

static void VKAPI_CALL wCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout layout, VkShaderStageFlags stages, uint32_t off, uint32_t size, const void *data) {
    char a[32], b[32]; snprintf(a, sizeof(a), "%u", off); snprintf(b, sizeof(b), "%u", size);
    event("pushconst", a, b, "", "", "");
    vkCmdPushConstants(cmd, layout, stages, off, size, data);
}

/* ------------------------------------------------------------------ wrappers: misc */

static VkResult VKAPI_CALL wQueueSubmit(VkQueue q, uint32_t count, const VkSubmitInfo *si, VkFence fence) {
    char a[32]; snprintf(a, sizeof(a), "%u", count);
    event("submit", a, "", "", "", "");
    log_msg("[submit] count=%u", count);
    return vkQueueSubmit(q, count, si, fence);
}

static VkResult VKAPI_CALL wBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *bi) {
    g_bound_pipeline = VK_NULL_HANDLE;
    g_bound_count = 0;
    event("cmdbuf_begin", "", "", "", "", "");
    return vkBeginCommandBuffer(cmd, bi);
}

static VkResult VKAPI_CALL wEndCommandBuffer(VkCommandBuffer cmd) {
    event("cmdbuf_end", "", "", "", "", "");
    return vkEndCommandBuffer(cmd);
}

/* ------------------------------------------------------------------ vkDeviceProcAddr wrappers */

static PFN_vkVoidFunction VKAPI_CALL wGetDeviceProcAddr(VkDevice dev, const char *name);

static PFN_vkVoidFunction dynamic_wrapper(const char *name, PFN_vkVoidFunction p) {
    if (!strcmp(name, "vkCreateComputePipelines")) { vkCreateComputePipelines = (PFN_vkCreateComputePipelines)p; return (PFN_vkVoidFunction)wCreateComputePipelines; }
    if (!strcmp(name, "vkCmdDispatch")) { vkCmdDispatch = (PFN_vkCmdDispatch)p; return (PFN_vkVoidFunction)wCmdDispatch; }
    if (!strcmp(name, "vkCmdDispatchIndirect")) { vkCmdDispatchIndirect = (PFN_vkCmdDispatchIndirect)p; return (PFN_vkVoidFunction)wCmdDispatchIndirect; }
    if (!strcmp(name, "vkCmdUpdateBuffer")) { vkCmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)p; return (PFN_vkVoidFunction)wCmdUpdateBuffer; }
    if (!strcmp(name, "vkCmdCopyBuffer")) { vkCmdCopyBuffer = (PFN_vkCmdCopyBuffer)p; return (PFN_vkVoidFunction)wCmdCopyBuffer; }
    if (!strcmp(name, "vkCmdFillBuffer")) { vkCmdFillBuffer = (PFN_vkCmdFillBuffer)p; return (PFN_vkVoidFunction)wCmdFillBuffer; }
    if (!strcmp(name, "vkCmdPushConstants")) { vkCmdPushConstants = (PFN_vkCmdPushConstants)p; return (PFN_vkVoidFunction)wCmdPushConstants; }
    if (!strcmp(name, "vkCmdCopyBufferToImage")) { vkCmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)p; return p; }
    if (!strcmp(name, "vkCmdCopyImageToBuffer")) { vkCmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)p; return p; }
    if (!strcmp(name, "vkCreateShaderModule")) { vkCreateShaderModule = (PFN_vkCreateShaderModule)p; return (PFN_vkVoidFunction)wCreateShaderModule; }
    if (!strcmp(name, "vkCmdBindPipeline")) { vkCmdBindPipeline = (PFN_vkCmdBindPipeline)p; return (PFN_vkVoidFunction)wCmdBindPipeline; }
    if (!strcmp(name, "vkCmdBindDescriptorSets")) { vkCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)p; return (PFN_vkVoidFunction)wCmdBindDescriptorSets; }
    if (!strcmp(name, "vkUpdateDescriptorSets")) { vkUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)p; return (PFN_vkVoidFunction)wUpdateDescriptorSets; }
    if (!strcmp(name, "vkCreateBuffer")) { vkCreateBuffer = (PFN_vkCreateBuffer)p; return (PFN_vkVoidFunction)wCreateBuffer; }
    if (!strcmp(name, "vkBindBufferMemory")) { vkBindBufferMemory = (PFN_vkBindBufferMemory)p; return (PFN_vkVoidFunction)wBindBufferMemory; }
    if (!strcmp(name, "vkAllocateMemory")) { vkAllocateMemory = (PFN_vkAllocateMemory)p; return (PFN_vkVoidFunction)wAllocateMemory; }
    if (!strcmp(name, "vkMapMemory")) { vkMapMemory = (PFN_vkMapMemory)p; return (PFN_vkVoidFunction)wMapMemory; }
    if (!strcmp(name, "vkUnmapMemory")) { vkUnmapMemory = (PFN_vkUnmapMemory)p; return (PFN_vkVoidFunction)wUnmapMemory; }
    return p;
}

static PFN_vkVoidFunction VKAPI_CALL wGetDeviceProcAddr(VkDevice dev, const char *name) {
    PFN_vkVoidFunction p = real_GDPA(dev, name);
    if (!p) return NULL;
    return dynamic_wrapper(name, p);
}

/* ------------------------------------------------------------------ IAT hook */

typedef struct { const char *name; void *fn; } Hook;

static int hook_iat(HMODULE mod, const char *target_dll, const Hook *hooks, int n) {
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)((char *)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (IMAGE_IMPORT_DESCRIPTOR *imp = (IMAGE_IMPORT_DESCRIPTOR *)((char *)mod + dir.VirtualAddress); imp->Name; imp++) {
        const char *dll = (char *)mod + imp->Name;
        if (_stricmp(dll, target_dll)) continue;
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA64 *orig = (IMAGE_THUNK_DATA64 *)((char *)mod + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA64 *iat = (IMAGE_THUNK_DATA64 *)((char *)mod + imp->FirstThunk);
        for (; orig->u1.Function; orig++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL64(orig->u1.Ordinal)) continue;
            const char *name = (const char *)((IMAGE_IMPORT_BY_NAME *)((char *)mod + orig->u1.AddressOfData))->Name;
            for (int i = 0; i < n; i++) {
                if (!strcmp(name, hooks[i].name)) {
                    DWORD old = 0;
                    if (VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) {
                        iat->u1.Function = (ULONG_PTR)hooks[i].fn;
                        VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
                        patched++;
                        log_msg("[hook] %s -> %p", name, hooks[i].fn);
                    }
                }
            }
        }
    }
    return patched;
}

/* ------------------------------------------------------------------ Vulkan init */

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
}

typedef struct {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    uint32_t w, h;
    VkFormat format;
} TestImage;

static TestImage make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, const char *name) {
    TestImage t; memset(&t, 0, sizeof(t));
    t.w = w; t.h = h; t.format = fmt;
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent.width = w; ci.extent.height = h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g_device, &ci, NULL, &t.image) != VK_SUCCESS) { log_msg("[error] createImage %s failed", name); return t; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_device, t.image, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size; ai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g_device, &ai, NULL, &t.mem) != VK_SUCCESS) { log_msg("[error] image memory %s failed", name); return t; }
    vkBindImageMemory(g_device, t.image, t.mem, 0);
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vi.image = t.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
    vi.subresourceRange.aspectMask = fmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    vkCreateImageView(g_device, &vi, NULL, &t.view);
    if (g_image_count < MAX_IMAGES) {
        ImageInfo *ii = &g_images[g_image_count++];
        ii->image = t.image; ii->w = w; ii->h = h; ii->format = fmt;
        strncpy(ii->name, name, sizeof(ii->name) - 1);
    }
    log_msg("[image] %s %ux%u fmt=%d image=%p view=%p", name, w, h, fmt, (void *)t.image, (void *)t.view);
    return t;
}

static struct FfxApiResource resource2d(TestImage *t, uint32_t format, uint32_t usage, uint32_t state) {
    struct FfxApiResource r; memset(&r, 0, sizeof(r));
    r.resource = (void *)t->image; r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = format;
    r.description.width = t->w; r.description.height = t->h;
    r.description.depth = 1; r.description.mipCount = 1; r.description.usage = usage;
    return r;
}

static void barrier_general(VkCommandBuffer cmd, TestImage *t, int first) {
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange.aspectMask = t->format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

static int create_instance(void) {
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.apiVersion = VK_API_VERSION_1_4;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, NULL, &g_instance) != VK_SUCCESS) { log_msg("[error] vkCreateInstance failed"); return 0; }
    return 1;
}

static int init_device(int width, int height, const char *preferred_device) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, NULL);
    if (!n) { log_msg("[error] no Vulkan physical device"); return 0; }
    VkPhysicalDevice *pds = (VkPhysicalDevice *)calloc(n, sizeof(*pds));
    vkEnumeratePhysicalDevices(g_instance, &n, pds);
    g_physical = pds[0];
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.vendorID == 0x1002 && p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { g_physical = pds[i]; break; }
    }
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(g_physical, &props);
    log_msg("[device] %s vendor=%04x device=%04x api=%u.%u.%u", props.deviceName, props.vendorID, props.deviceID,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t ec = 0; vkEnumerateDeviceExtensionProperties(g_physical, NULL, &ec, NULL);
    VkExtensionProperties *exts = (VkExtensionProperties *)calloc(ec, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(g_physical, NULL, &ec, exts);
    const char *want[] = {
        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME,
        VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME,
        VK_EXT_SHADER_IMAGE_ATOMIC_INT64_EXTENSION_NAME, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
        VK_KHR_VARIABLE_POINTERS_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
        VK_KHR_16BIT_STORAGE_EXTENSION_NAME, VK_KHR_MAINTENANCE_5_EXTENSION_NAME
    };
    const char *used[16]; uint32_t usedn = 0;
    for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        for (uint32_t j = 0; j < ec; j++) if (!strcmp(exts[j].extensionName, want[i])) { used[usedn++] = want[i]; break; }
    }
    free(exts);

    VkPhysicalDeviceFeatures core; vkGetPhysicalDeviceFeatures(g_physical, &core);

    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT };
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT i64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };
    void **tail = &f11.pNext;
    *tail = &f12; tail = &f12.pNext;
    *tail = &f13; tail = &f13.pNext;
    *tail = &cm; tail = &cm.pNext;
    *tail = &cm2; tail = &cm2.pNext;
    *tail = &f8; tail = &f8.pNext;
    *tail = &i64;
    VkPhysicalDeviceFeatures2 q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    q.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(g_physical, &q);

    f11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess;
    f11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    f11.variablePointers = f11.variablePointers;
    f11.variablePointersStorageBuffer = f11.variablePointersStorageBuffer;
    f12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    f12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess;
    f12.shaderFloat16 = f12.shaderFloat16;
    f12.shaderInt8 = f12.shaderInt8;
    f12.timelineSemaphore = f12.timelineSemaphore;
    f12.vulkanMemoryModel = f12.vulkanMemoryModel;
    f12.vulkanMemoryModelDeviceScope = f12.vulkanMemoryModelDeviceScope;
    f12.bufferDeviceAddress = f12.bufferDeviceAddress;
    f13.subgroupSizeControl = f13.subgroupSizeControl;
    f13.computeFullSubgroups = f13.computeFullSubgroups;
    f13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    f13.maintenance4 = f13.maintenance4;

    log_msg("[features] f16=%d i8=%d vmm=%d varptr=%d cm=%d cmconv=%d cmper=%d cmred=%d f8=%d f8cm=%d i64=%d",
            f12.shaderFloat16, f12.shaderInt8, f12.vulkanMemoryModel, f11.variablePointers,
            cm.cooperativeMatrix, cm2.cooperativeMatrixConversions, cm2.cooperativeMatrixPerElementOperations,
            cm2.cooperativeMatrixReductions, f8.shaderFloat8, f8.shaderFloat8CooperativeMatrix, i64.shaderImageInt64Atomics);

    uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &qn, NULL);
    VkQueueFamilyProperties *qf = (VkQueueFamilyProperties *)calloc(qn, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(g_physical, &qn, qf);
    g_queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { g_queue_family = i; break; }
    free(qf);
    if (g_queue_family == UINT32_MAX) { log_msg("[error] no compute queue family"); return 0; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qi.queueFamilyIndex = g_queue_family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qi;
    dci.pNext = &f11; dci.pEnabledFeatures = &core;
    dci.enabledExtensionCount = usedn; dci.ppEnabledExtensionNames = used;
    VkResult r = vkCreateDevice(g_physical, &dci, NULL, &g_device);
    log_msg("[device] vkCreateDevice=%d extensions=%u", r, usedn);
    if (r != VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < usedn; i++) log_msg("[device] extension %s", used[i]);
    (void)preferred_device;
    return 1;
}

/* ------------------------------------------------------------------ FFX message callback */

static void ffx_message(uint32_t type, const wchar_t *text) {
    char utf8[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n <= 0) utf8[0] = 0;
    log_msg("[ffx %u] %s", type, utf8);
}

/* ------------------------------------------------------------------ main */

static void usage(void) {
    fprintf(stderr,
        "usage: mlfi_capture.exe --dll amd_fidelityfx_framegeneration_vk.dll --out DIR [options]\n"
        "  --width N --height N     display/render size (default 1280x720; <=4K class)\n"
        "  --frames N               frames to record (default 2)\n"
        "  --no-substitute          do not replace the modules that crash RADV (reproduces the crash)\n"
        "  --readback               read the generated output image back and hash it\n"
        "  --version-id HEX         force a provider version id that was not advertised (diagnostic)\n"
        "  --version-name NAME      advertised provider to select (default 4.0.2)\n");
}

int main(int argc, char **argv) {
    char dll_path[MAX_PATH] = "amd_fidelityfx_framegeneration_vk.dll";
    char out[MAX_PATH] = "";
    char want_name[32] = "4.0.2";
    uint64_t forced_version = 0;
    int width = 1280, height = 720, frames = 2, substitute = 1, readback = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dll") && i + 1 < argc) strncpy(dll_path, argv[++i], sizeof(dll_path) - 1);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) strncpy(out, argv[++i], sizeof(out) - 1);
        else if (!strcmp(argv[i], "--width") && i + 1 < argc) width = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--height") && i + 1 < argc) height = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--version-id") && i + 1 < argc) forced_version = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--version-name") && i + 1 < argc) { strncpy(want_name, argv[++i], sizeof(want_name) - 1); want_name[sizeof(want_name) - 1] = 0; }
        else if (!strcmp(argv[i], "--no-substitute")) substitute = 0;
        else if (!strcmp(argv[i], "--readback")) readback = 1;
        else if (!strcmp(argv[i], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(); return 1; }
    }
    if (!out[0]) {
        const char *home = getenv("USERPROFILE");
        if (!home) home = getenv("HOME");
        if (!home) { fprintf(stderr, "no --out and no HOME\n"); return 1; }
        snprintf(out, sizeof(out), "%s\\mlfi-capture-run", home);
    }
    strncpy(g_out, out, sizeof(g_out) - 1);
    CreateDirectoryA(g_out, NULL);
    char raw[MAX_PATH]; snprintf(raw, sizeof(raw), "%s\\raw", g_out); CreateDirectoryA(raw, NULL);

    log_open();
    log_msg("# mlfi_capture: size=%dx%d frames=%d substitute=%d", width, height, frames, substitute);
    g_substitute = substitute;

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { log_msg("[error] no vulkan-1.dll err=%lu", GetLastError()); return 1; }
    real_GIPA = (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    real_GDPA = (PFN_vkGetDeviceProcAddr)GetProcAddress(vk, "vkGetDeviceProcAddr");
    log_msg("[init] vk=%p gipa=%p gdpa=%p", (void*)vk, (void*)real_GIPA, (void*)real_GDPA);
    if (!real_GIPA || !real_GDPA) { log_msg("[error] no Vulkan loader entry points"); return 1; }

#define GIPA(name) name = (PFN_##name)real_GIPA(g_instance, #name)
#define GDPA(name) name = (PFN_##name)real_GDPA(g_device, #name)
    vkCreateInstance = (PFN_vkCreateInstance)real_GIPA(NULL, "vkCreateInstance");
    log_msg("[init] createInstance=%p", (void*)vkCreateInstance);
    if (!create_instance()) return 2;
    log_msg("[init] instance=%p", (void*)g_instance);

    GIPA(vkEnumeratePhysicalDevices);
    GIPA(vkGetPhysicalDeviceProperties);
    GIPA(vkGetPhysicalDeviceFeatures);
    GIPA(vkGetPhysicalDeviceFeatures2);
    GIPA(vkGetPhysicalDeviceMemoryProperties);
    GIPA(vkGetPhysicalDeviceQueueFamilyProperties);
    GIPA(vkEnumerateDeviceExtensionProperties);
    GIPA(vkCreateDevice);

    if (!init_device(width, height, NULL)) return 2;

    GDPA(vkGetDeviceQueue);
    GDPA(vkDeviceWaitIdle);
    GDPA(vkCreateCommandPool);
    GDPA(vkAllocateCommandBuffers);
    GDPA(vkBeginCommandBuffer);
    GDPA(vkEndCommandBuffer);
    GDPA(vkQueueSubmit);
    GDPA(vkQueueWaitIdle);
    GDPA(vkCreateImage);
    GDPA(vkGetImageMemoryRequirements);
    GDPA(vkBindImageMemory);
    GDPA(vkCreateImageView);
    GDPA(vkCmdPipelineBarrier);
    GDPA(vkCreateBuffer);
    GDPA(vkGetBufferMemoryRequirements);
    GDPA(vkBindBufferMemory);
    GDPA(vkAllocateMemory);
    GDPA(vkMapMemory);
    GDPA(vkUnmapMemory);
    GDPA(vkCreateShaderModule);
    GDPA(vkCreateDescriptorSetLayout);
    GDPA(vkAllocateDescriptorSets);
    GDPA(vkUpdateDescriptorSets);
    GDPA(vkCreatePipelineLayout);
    GDPA(vkCreateComputePipelines);
    GDPA(vkCmdBindPipeline);
    GDPA(vkCmdBindDescriptorSets);
    GDPA(vkCmdDispatch);
    GDPA(vkCmdDispatchIndirect);
    GDPA(vkCmdUpdateBuffer);
    GDPA(vkCmdCopyBuffer);
    GDPA(vkCmdFillBuffer);
    GDPA(vkCmdPushConstants);
    GDPA(vkDestroyShaderModule);
    GDPA(vkDestroyBuffer);
    GDPA(vkFreeMemory);
    GDPA(vkCmdCopyImageToBuffer);

    HMODULE dll = LoadLibraryA(dll_path);
    if (!dll) { log_msg("[error] LoadLibraryA(%s) failed err=%lu", dll_path, GetLastError()); return 4; }
    PfnFfxQuery ffx_query = (PfnFfxQuery)GetProcAddress(dll, "ffxQuery");
    PfnFfxCreateContext ffx_create = (PfnFfxCreateContext)GetProcAddress(dll, "ffxCreateContext");
    PfnFfxConfigure ffx_configure = (PfnFfxConfigure)GetProcAddress(dll, "ffxConfigure");
    PfnFfxDispatch ffx_dispatch = (PfnFfxDispatch)GetProcAddress(dll, "ffxDispatch");
    PfnFfxDestroyContext ffx_destroy = (PfnFfxDestroyContext)GetProcAddress(dll, "ffxDestroyContext");
    if (!ffx_query || !ffx_create || !ffx_configure || !ffx_dispatch || !ffx_destroy) {
        log_msg("[error] missing FFX exports");
        return 4;
    }
    log_msg("[dll] %s loaded at %p", dll_path, (void *)dll);

    Hook hooks[] = {
        { "vkCreateShaderModule", (void *)wCreateShaderModule },
        { "vkCreateBuffer", (void *)wCreateBuffer },
        { "vkBindBufferMemory", (void *)wBindBufferMemory },
        { "vkAllocateMemory", (void *)wAllocateMemory },
        { "vkMapMemory", (void *)wMapMemory },
        { "vkUnmapMemory", (void *)wUnmapMemory },
        { "vkUpdateDescriptorSets", (void *)wUpdateDescriptorSets },
        { "vkCreateDescriptorSetLayout", (void *)wCreateDescriptorSetLayout },
        { "vkCreatePipelineLayout", (void *)wCreatePipelineLayout },
        { "vkAllocateDescriptorSets", (void *)wAllocateDescriptorSets },
        { "vkCmdBindPipeline", (void *)wCmdBindPipeline },
        { "vkCmdBindDescriptorSets", (void *)wCmdBindDescriptorSets },
        { "vkBeginCommandBuffer", (void *)wBeginCommandBuffer },
        { "vkEndCommandBuffer", (void *)wEndCommandBuffer },
        { "vkQueueSubmit", (void *)wQueueSubmit },
        { "vkGetDeviceProcAddr", (void *)wGetDeviceProcAddr },
    };
    int patched = hook_iat(dll, "vulkan-1.dll", hooks, (int)(sizeof(hooks) / sizeof(hooks[0])));
    log_msg("[hook] %d IAT entries patched", patched);

    struct ffxConfigureDescGlobalDebug gd = { 0 };
    gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    gd.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION;
    gd.fpMessage = ffx_message;
    gd.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
    ffx_configure(NULL, (ffxConfigureDescHeader *)&gd);

    VkDeviceContext vkdc = { g_device, g_physical, wGetDeviceProcAddr };

    struct ffxQueryDescGetVersions vq = { 0 };
    vq.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    vq.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    vq.device = &vkdc;
    uint64_t count = 0; vq.outputCount = &count;
    uint32_t qr = ffx_query(NULL, (ffxQueryDescHeader *)&vq);
    log_msg("[ffx] version query count rc=%u count=%llu", qr, (unsigned long long)count);
    uint64_t ml_id = 0;
    if (count) {
        uint64_t *ids = (uint64_t *)calloc(count, sizeof(uint64_t));
        const char **names = (const char **)calloc(count, sizeof(char *));
        vq.outputCount = &count; vq.versionIds = ids; vq.versionNames = names;
        qr = ffx_query(NULL, (ffxQueryDescHeader *)&vq);
        for (uint64_t i = 0; i < count; i++) {
            log_msg("[ffx] version[%llu] id=%016llx name=%s", (unsigned long long)i, (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
            if (!strcmp(names[i] ? names[i] : "", want_name)) ml_id = ids[i];
        }
        free(ids); free(names);
    }
    if (!ml_id && forced_version) {
        ml_id = forced_version;
        log_msg("[ffx] forcing unadvertised version id=%016llx", (unsigned long long)ml_id);
    }
    if (!ml_id) { log_msg("[error] no %s provider version reported", want_name); return 3; }
    log_msg("[ffx] selecting provider %s id=%016llx", want_name, (unsigned long long)ml_id);

    struct ffxCreateContextDescFrameGeneration cd = { 0 };
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.flags = 0;
    cd.displaySize.width = (uint32_t)width; cd.displaySize.height = (uint32_t)height;
    cd.maxRenderSize.width = (uint32_t)width; cd.maxRenderSize.height = (uint32_t)height;
    cd.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    struct ffxCreateBackendVKDesc bd = { 0 };
    bd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    bd.deviceContext = vkdc;
    struct ffxOverrideVersion ov = { 0 };
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = ml_id;
    struct ffxCreateContextDescFrameGenerationVersion fv = { 0 };
    fv.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    fv.version = FFX_FRAMEGENERATION_VERSION;
    cd.header.pNext = (ffxApiHeader *)&bd;
    bd.header.pNext = (ffxApiHeader *)&ov;
    ov.header.pNext = (ffxApiHeader *)&fv;

    ffxContext ctx = NULL;
    g_stage = "create";
    uint32_t cr = ffx_create(&ctx, (ffxCreateContextDescHeader *)&cd, NULL);
    log_msg("[ffx] ffxCreateContext=%u ctx=%p", cr, ctx);
    if (cr != 0 || !ctx) { log_msg("[error] context creation failed"); return 3; }

    TestImage present = make_image((uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "present");
    TestImage output = make_image((uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "output");
    TestImage depth = make_image((uint32_t)width, (uint32_t)height, VK_FORMAT_D32_SFLOAT,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, "depth");
    TestImage mv = make_image((uint32_t)width, (uint32_t)height, VK_FORMAT_R16G16_SFLOAT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, "motion_vectors");
    if (!present.image || !output.image || !depth.image || !mv.image) return 2;

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.queueFamilyIndex = g_queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    if (vkCreateCommandPool(g_device, &pci, NULL, &pool) != VK_SUCCESS) return 2;
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g_device, &cbi, &cmd) != VK_SUCCESS) return 2;
    VkQueue queue; vkGetDeviceQueue(g_device, g_queue_family, 0, &queue);

    char tensor_path[MAX_PATH]; snprintf(tensor_path, sizeof(tensor_path), "%s\\tensors.tsv", g_out);
    g_tensor_index = fopen(tensor_path, "w");
    if (g_tensor_index) fprintf(g_tensor_index, "id\tentry\tfnv1a64\n");

    int failures = 0;
    for (int frame = 0; frame < frames; frame++) {
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        barrier_general(cmd, &present, frame == 0);
        barrier_general(cmd, &output, frame == 0);
        barrier_general(cmd, &depth, frame == 0);
        barrier_general(cmd, &mv, frame == 0);

        struct ffxConfigureDescFrameGeneration cfg = { 0 };
        cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.frameGenerationEnabled = true;
        cfg.allowAsyncWorkloads = false;
        cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
        cfg.onlyPresentGenerated = false;
        cfg.frameID = (uint64_t)frame;
        g_stage = "configure";
        uint32_t rc = ffx_configure(&ctx, (ffxConfigureDescHeader *)&cfg);
        log_msg("[frame %d] configure=%u", frame, rc);
        if (rc) failures++;

        struct ffxDispatchDescFrameGenerationPrepareV2 prep = { 0 };
        prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prep.frameID = (uint64_t)frame;
        prep.flags = 0;
        prep.commandList = cmd;
        prep.renderSize.width = (uint32_t)width; prep.renderSize.height = (uint32_t)height;
        prep.jitterOffset.x = 0; prep.jitterOffset.y = 0;
        prep.motionVectorScale.x = 1.0f; prep.motionVectorScale.y = 1.0f;
        prep.frameTimeDelta = 16.6f;
        prep.reset = frame == 0;
        prep.cameraNear = 0.1f; prep.cameraFar = 1000.0f;
        prep.cameraFovAngleVertical = 1.0f; prep.viewSpaceToMetersFactor = 1.0f;
        prep.depth = resource2d(&depth, FFX_API_SURFACE_FORMAT_R32_FLOAT, FFX_API_RESOURCE_USAGE_DEPTHTARGET, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.motionVectors = resource2d(&mv, FFX_API_SURFACE_FORMAT_R16G16_FLOAT, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        prep.cameraPosition[0] = 0; prep.cameraPosition[1] = 0; prep.cameraPosition[2] = 5;
        prep.cameraUp[1] = 1; prep.cameraRight[0] = 1; prep.cameraForward[2] = -1;
        g_stage = "prepare";
        rc = ffx_dispatch(&ctx, (ffxDispatchDescHeader *)&prep);
        log_msg("[frame %d] prepare=%u", frame, rc);
        if (rc) failures++;

        struct ffxDispatchDescFrameGeneration fg = { 0 };
        fg.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        fg.commandList = cmd;
        fg.presentColor = resource2d(&present, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_READ_ONLY, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        fg.outputs[0] = resource2d(&output, FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        fg.numGeneratedFrames = 1;
        fg.reset = frame == 0;
        fg.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
        fg.minMaxLuminance[1] = 1000.0f;
        fg.frameID = (uint64_t)frame;
        g_stage = "framegen";
        rc = ffx_dispatch(&ctx, (ffxDispatchDescHeader *)&fg);
        log_msg("[frame %d] framegen=%u", frame, rc);
        if (rc) failures++;

        g_stage = "submit";
        vkEndCommandBuffer(cmd);
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        VkResult vr = vkQueueSubmit(queue, 1, &si, NULL);
        if (vr == VK_SUCCESS) vr = vkQueueWaitIdle(queue);
        log_msg("[frame %d] submit=%d", frame, vr);
        if (vr != VK_SUCCESS) failures++;

        if (readback) {
            /* Read the output image through a host-visible buffer and hash it. */
            VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
            bci.size = (VkDeviceSize)width * height * 4;
            bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VkBuffer rb; vkCreateBuffer(g_device, &bci, NULL, &rb);
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_device, rb, &mr);
            VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            mai.allocationSize = mr.size;
            mai.memoryTypeIndex = find_memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkDeviceMemory rmem; vkAllocateMemory(g_device, &mai, NULL, &rmem);
            vkBindBufferMemory(g_device, rb, rmem, 0);
            VkCommandBufferBeginInfo rbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &rbi);
            VkBufferImageCopy region; memset(&region, 0, sizeof(region));
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = width; region.imageExtent.height = height; region.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(cmd, output.image, VK_IMAGE_LAYOUT_GENERAL, rb, 1, &region);
            vkEndCommandBuffer(cmd);
            VkSubmitInfo rsi = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            rsi.commandBufferCount = 1; rsi.pCommandBuffers = &cmd;
            vkQueueSubmit(queue, 1, &rsi, NULL);
            vkQueueWaitIdle(queue);
            void *map = NULL;
            if (vkMapMemory(g_device, rmem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
                size_t bytes = (size_t)width * height * 4;
                uint64_t h = fnv1a(map, bytes);
                size_t nonzero = 0;
                const uint8_t *p = (const uint8_t *)map;
                for (size_t i = 0; i < bytes; i += 4) if (p[i] | p[i + 1] | p[i + 2] | p[i + 3]) nonzero++;
                log_msg("[frame %d] output hash=%016llx nonzero_pixels=%zu/%u", frame, (unsigned long long)h, nonzero, (unsigned)width * height);
                dump_raw("output_readback", map, bytes);
                vkUnmapMemory(g_device, rmem);
            }
            vkFreeMemory(g_device, rmem, NULL);
            vkDestroyBuffer(g_device, rb, NULL);
        }
    }

    g_stage = "destroy";
    uint32_t dr = ffx_destroy(&ctx, NULL);
    log_msg("[ffx] ffxDestroyContext=%u", dr);
    if (g_tensor_index) fclose(g_tensor_index);
    fprintf(g_passes, "index\tstage\tentry\tpipeline\tsubstituted\tdispatch_x\tdispatch_y\tdispatch_z\tweight_offset\tweight_range\ttensor_offset\tconstants_offset\ttensor_table\n");
    for (int i = 0; i < g_pass_count; i++) {
        PassRec *r = &g_pass_records[i];
        fprintf(g_passes, "%d\t%s\t%s\t%p\t%d\t%u\t%u\t%u\t%llu\t%llu\t%llu\t%llu\t%d\n", i, r->stage, r->entry,
                (void*)r->pipe, r->substituted, r->gx, r->gy, r->gz,
                (unsigned long long)r->weight_offset, (unsigned long long)r->weight_range,
                (unsigned long long)r->tensor_offset, (unsigned long long)r->constants_offset, r->tensor_id);
    }
    fflush(g_passes);
    log_msg("# done failures=%d dispatches=%d", failures, g_pass_count);
    fflush(g_log);
    fflush(g_events);
    return failures ? 3 : 0;
}
