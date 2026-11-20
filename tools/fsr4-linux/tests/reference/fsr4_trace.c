// Reference tracer for AMD FSR Upscaling 4.1.2 (FidelityFX SDK v2.4.0, Vulkan).
//
// Runs the signed amd_fidelityfx_upscaler_vk.dll under Wine on the host Vulkan
// driver, intercepts the backend's device-level entry points through the
// VkDeviceContext::vkDeviceProcAddr we hand it, and records everything the
// native port needs:
//   - shader module hashes (map to the 4.1.2 SPIR-V extracted from the DLL)
//   - pipeline -> shader mapping and the per-dispatch pipeline/group counts
//   - descriptor set layouts (bindings + types)
//   - created images (format/extent/usage) and buffers (size/usage)
//   - the CsTensorSizes UBO bytes and the MLSR_Optimized_Constants bytes
//   - deterministic input images and the upscaled output as raw files
//
// Build/run: see build.sh and README.md in this directory.
//
// This file is a Windows PE (mingw) only harness; it is not part of the shipped
// library.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <ffx_api.h>
#include <vk/ffx_api_vk.h>
#include <ffx_upscale.h>

// ---------------------------------------------------------------------------
// Small SHA-256 (public-domain style implementation, used only for blob IDs)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t buf[64];
    size_t used;
} sha256_ctx;

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void sha256_block(sha256_ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 |
               (uint32_t)p[i * 4 + 2] << 8 | (uint32_t)p[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_init(sha256_ctx* c) {
    static const uint32_t init[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    memcpy(c->h, init, sizeof(init));
    c->total = 0;
    c->used = 0;
}

static void sha256_update(sha256_ctx* c, const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    c->total += n;
    while (n) {
        size_t take = 64 - c->used;
        if (take > n) take = n;
        memcpy(c->buf + c->used, p, take);
        c->used += take;
        p += take;
        n -= take;
        if (c->used == 64) {
            sha256_block(c, c->buf);
            c->used = 0;
        }
    }
}

static void sha256_hex(sha256_ctx* c, char out[65]) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t zero = 0;
    while (c->used != 56) sha256_update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha256_update(c, len, 8);
    for (int i = 0; i < 8; ++i) sprintf(out + i * 8, "%08x", c->h[i]);
    out[64] = 0;
}

// ---------------------------------------------------------------------------
// Trace state
// ---------------------------------------------------------------------------
#define MAX_SHADERS 512
#define MAX_PIPES 512
#define MAX_IMAGES 512
#define MAX_BUFFERS 512
#define MAX_MEMORIES 256
#define MAX_MAPS 64

typedef struct {
    VkShaderModule mod;
    uint32_t size;
    char sha[65];
} ShaderRec;

typedef struct {
    VkPipeline pipe;
    VkShaderModule mod;
    char entry[96];
    uint64_t store_index;
} PipeRec;

typedef struct {
    VkImage image;
    VkFormat format;
    VkExtent3D extent;
    VkImageUsageFlags usage;
} ImageRec;

typedef struct {
    VkBuffer buffer;
    VkDeviceSize size;
    VkBufferUsageFlags usage;
    VkDeviceMemory memory;
    VkDeviceSize memory_offset;
} BufferRec;

typedef struct {
    VkDeviceMemory memory;
    VkDeviceSize size;
} MemoryRec;

typedef struct {
    VkDeviceMemory memory;
    void* ptr;
    VkDeviceSize size;
    int owned;
} MapRec;

static ShaderRec g_shaders[MAX_SHADERS];
static int g_shader_count;
static PipeRec g_pipes[MAX_PIPES];
static int g_pipe_count;
static ImageRec g_images[MAX_IMAGES];
static int g_image_count;
static BufferRec g_buffers[MAX_BUFFERS];
static int g_buffer_count;
static MemoryRec g_memories[MAX_MEMORIES];
static int g_memory_count;
static MapRec g_maps[MAX_MAPS];
static int g_map_count;

static FILE* g_log;
static uint64_t g_dispatch_index;
static uint32_t g_out_w = 1920, g_out_h = 1080;
static uint32_t g_rw = 960, g_rh = 540;
static uint32_t g_create_flags;
static uint32_t g_dispatch_flags;
static uint32_t g_ctx_max_rw, g_ctx_max_rh, g_ctx_max_uw, g_ctx_max_uh;
static int g_sharpen;
static float g_sharpness = 0.9f;
static int g_found_tensor;
static int g_found_constants;
static int g_found_spd;
static int g_found_rcas;
static int g_desc_log_budget = 4000;
static int g_dispatch_log_budget = 200;
static int g_verbose;

#define TLOG(...)                            \
    do {                                     \
        if (g_log) {                         \
            fprintf(g_log, __VA_ARGS__);     \
            fprintf(g_log, "\n");            \
            fflush(g_log);                   \
        }                                    \
    } while (0)

static ShaderRec* findShader(VkShaderModule mod) {
    for (int i = 0; i < g_shader_count; ++i)
        if (g_shaders[i].mod == mod) return &g_shaders[i];
    return NULL;
}

static PipeRec* findPipe(VkPipeline pipe) {
    for (int i = 0; i < g_pipe_count; ++i)
        if (g_pipes[i].pipe == pipe) return &g_pipes[i];
    return NULL;
}

static BufferRec* findBuffer(VkBuffer buffer) {
    for (int i = 0; i < g_buffer_count; ++i)
        if (g_buffers[i].buffer == buffer) return &g_buffers[i];
    return NULL;
}

static MemoryRec* findMemory(VkDeviceMemory memory) {
    for (int i = 0; i < g_memory_count; ++i)
        if (g_memories[i].memory == memory) return &g_memories[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// Real entry points + hooks
// ---------------------------------------------------------------------------
static PFN_vkGetDeviceProcAddr g_gdpa;
static VkDevice g_device;

static void* realDevFn(VkDevice dev, const char* name) {
    return (void*)g_gdpa(dev, name);
}

static PFN_vkCreateShaderModule real_create_shader_module;
static PFN_vkDestroyShaderModule real_destroy_shader_module;
static PFN_vkDestroyPipeline real_destroy_pipeline;
static PFN_vkCreateComputePipelines real_create_compute_pipelines;
static PFN_vkCreateBuffer real_create_buffer;
static PFN_vkCreateImage real_create_image;
static PFN_vkCreateImageView real_create_image_view;
static PFN_vkAllocateMemory real_allocate_memory;
static PFN_vkBindBufferMemory real_bind_buffer_memory;
static PFN_vkBindImageMemory real_bind_image_memory;
static PFN_vkMapMemory real_map_memory;
static PFN_vkUnmapMemory real_unmap_memory;
static PFN_vkCreateDescriptorSetLayout real_create_descriptor_set_layout;
static PFN_vkCreatePipelineLayout real_create_pipeline_layout;
static PFN_vkCreateSampler real_create_sampler;
static PFN_vkUpdateDescriptorSets real_update_descriptor_sets;
static PFN_vkCmdBindPipeline real_cmd_bind_pipeline;
static PFN_vkCmdBindDescriptorSets real_cmd_bind_descriptor_sets;
static PFN_vkCmdDispatch real_cmd_dispatch;
static PFN_vkCmdDispatchIndirect real_cmd_dispatch_indirect;
static PFN_vkCmdDispatchBase real_cmd_dispatch_base;
static PFN_vkCmdUpdateBuffer real_cmd_update_buffer;
static PFN_vkCmdClearColorImage real_cmd_clear_color_image;
static PFN_vkCmdFillBuffer real_cmd_fill_buffer;
static PFN_vkCmdCopyBuffer real_cmd_copy_buffer;
static PFN_vkCmdCopyBufferToImage real_cmd_copy_buffer_to_image;
static PFN_vkCmdCopyImage real_cmd_copy_image;
static PFN_vkCmdPipelineBarrier real_cmd_pipeline_barrier;
static PFN_vkCmdPipelineBarrier2 real_cmd_pipeline_barrier2;
static PFN_vkEndCommandBuffer real_end_command_buffer;
static PFN_vkQueueSubmit real_queue_submit;

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateShaderModule(
    VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkShaderModule* out) {
    if (!real_create_shader_module) real_create_shader_module =
        (PFN_vkCreateShaderModule)realDevFn(dev, "vkCreateShaderModule");
    VkResult r = real_create_shader_module(dev, ci, ac, out);
    if (r == VK_SUCCESS && out && g_shader_count < MAX_SHADERS) {
        ShaderRec* s = &g_shaders[g_shader_count++];
        s->mod = *out;
        s->size = (uint32_t)ci->codeSize;
        sha256_ctx c;
        sha256_init(&c);
        sha256_update(&c, ci->pCode, ci->codeSize);
        sha256_hex(&c, s->sha);
        TLOG("SHADER handle=%p size=%u sha256=%s", (void*)*out, s->size, s->sha);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hkDestroyShaderModule(
    VkDevice dev, VkShaderModule mod, const VkAllocationCallbacks* ac) {
    if (!real_destroy_shader_module) real_destroy_shader_module =
        (PFN_vkDestroyShaderModule)realDevFn(dev, "vkDestroyShaderModule");
    for (int i = 0; i < g_shader_count; ++i) {
        if (g_shaders[i].mod == mod) { g_shaders[i] = g_shaders[--g_shader_count]; break; }
    }
    real_destroy_shader_module(dev, mod, ac);
}

static VKAPI_ATTR void VKAPI_CALL hkDestroyPipeline(
    VkDevice dev, VkPipeline pipe, const VkAllocationCallbacks* ac) {
    if (!real_destroy_pipeline) real_destroy_pipeline =
        (PFN_vkDestroyPipeline)realDevFn(dev, "vkDestroyPipeline");
    for (int i = 0; i < g_pipe_count; ++i) {
        if (g_pipes[i].pipe == pipe) { g_pipes[i] = g_pipes[--g_pipe_count]; break; }
    }
    real_destroy_pipeline(dev, pipe, ac);
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateComputePipelines(
    VkDevice dev, VkPipelineCache cache, uint32_t count,
    const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* ac,
    VkPipeline* out) {
    if (!real_create_compute_pipelines) real_create_compute_pipelines =
        (PFN_vkCreateComputePipelines)realDevFn(dev, "vkCreateComputePipelines");
    VkResult r = real_create_compute_pipelines(dev, cache, count, infos, ac, out);
    if (r == VK_SUCCESS && out) {
        for (uint32_t i = 0; i < count; ++i) {
            if (g_pipe_count >= MAX_PIPES) break;
            PipeRec* p = &g_pipes[g_pipe_count++];
            p->pipe = out[i];
            p->mod = infos[i].stage.module;
            p->store_index = g_pipe_count - 1;
            const char* name = infos[i].stage.pName ? infos[i].stage.pName : "";
            strncpy(p->entry, name, sizeof(p->entry) - 1);
            ShaderRec* s = findShader(p->mod);
            TLOG("PIPELINE handle=%p shader=%p sha256=%s entry=%s", (void*)p->pipe,
                 (void*)p->mod, s ? s->sha : "?", p->entry);
        }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateBuffer(
    VkDevice dev, const VkBufferCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkBuffer* out) {
    if (!real_create_buffer) real_create_buffer =
        (PFN_vkCreateBuffer)realDevFn(dev, "vkCreateBuffer");
    VkResult r = real_create_buffer(dev, ci, ac, out);
    if (r == VK_SUCCESS && out && g_buffer_count < MAX_BUFFERS) {
        BufferRec* b = &g_buffers[g_buffer_count++];
        memset(b, 0, sizeof(*b));
        b->buffer = *out;
        b->size = ci->size;
        b->usage = ci->usage;
        TLOG("BUFFER handle=%p size=%llu usage=0x%x", (void*)*out,
             (unsigned long long)ci->size, (unsigned)ci->usage);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateImage(
    VkDevice dev, const VkImageCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkImage* out) {
    if (!real_create_image) real_create_image =
        (PFN_vkCreateImage)realDevFn(dev, "vkCreateImage");
    VkResult r = real_create_image(dev, ci, ac, out);
    if (r == VK_SUCCESS && out && g_image_count < MAX_IMAGES) {
        ImageRec* im = &g_images[g_image_count++];
        im->image = *out;
        im->format = ci->format;
        im->extent = ci->extent;
        im->usage = ci->usage;
        TLOG("IMAGE handle=%p format=%d extent=%ux%ux%u mips=%u layers=%u usage=0x%x",
             (void*)*out, (int)ci->format, ci->extent.width, ci->extent.height,
             ci->extent.depth, ci->mipLevels, ci->arrayLayers, (unsigned)ci->usage);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateImageView(
    VkDevice dev, const VkImageViewCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkImageView* out) {
    if (!real_create_image_view) real_create_image_view =
        (PFN_vkCreateImageView)realDevFn(dev, "vkCreateImageView");
    VkResult r = real_create_image_view(dev, ci, ac, out);
    if (r == VK_SUCCESS) TLOG("IMAGEVIEW handle=%p image=%p format=%d", (void*)*out,
                              (void*)ci->image, (int)ci->format);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkAllocateMemory(
    VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* ac,
    VkDeviceMemory* out) {
    if (!real_allocate_memory) real_allocate_memory =
        (PFN_vkAllocateMemory)realDevFn(dev, "vkAllocateMemory");
    VkResult r = real_allocate_memory(dev, ai, ac, out);
    if (r == VK_SUCCESS && out && g_memory_count < MAX_MEMORIES) {
        MemoryRec* m = &g_memories[g_memory_count++];
        m->memory = *out;
        m->size = ai->allocationSize;
        TLOG("MEMORY handle=%p size=%llu type=%u", (void*)*out,
             (unsigned long long)ai->allocationSize, ai->memoryTypeIndex);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkBindBufferMemory(
    VkDevice dev, VkBuffer buf, VkDeviceMemory mem, VkDeviceSize off) {
    if (!real_bind_buffer_memory) real_bind_buffer_memory =
        (PFN_vkBindBufferMemory)realDevFn(dev, "vkBindBufferMemory");
    BufferRec* b = findBuffer(buf);
    if (b) { b->memory = mem; b->memory_offset = off; }
    TLOG("BINDBUFFER buffer=%p memory=%p offset=%llu", (void*)buf, (void*)mem,
         (unsigned long long)off);
    return real_bind_buffer_memory(dev, buf, mem, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL hkBindImageMemory(
    VkDevice dev, VkImage img, VkDeviceMemory mem, VkDeviceSize off) {
    if (!real_bind_image_memory) real_bind_image_memory =
        (PFN_vkBindImageMemory)realDevFn(dev, "vkBindImageMemory");
    TLOG("BINDIMAGE image=%p memory=%p offset=%llu", (void*)img, (void*)mem,
         (unsigned long long)off);
    return real_bind_image_memory(dev, img, mem, off);
}

static VKAPI_ATTR VkResult VKAPI_CALL hkMapMemory(
    VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size,
    VkMemoryMapFlags flags, void** data) {
    if (!real_map_memory) real_map_memory =
        (PFN_vkMapMemory)realDevFn(dev, "vkMapMemory");
    VkResult r = real_map_memory(dev, mem, off, size, flags, data);
    if (r == VK_SUCCESS && data && *data && g_map_count < MAX_MAPS) {
        MemoryRec* m = findMemory(mem);
        VkDeviceSize actual = size == VK_WHOLE_SIZE ? (m ? m->size - off : 0) : size;
        MapRec* mr = &g_maps[g_map_count++];
        mr->memory = mem;
        mr->ptr = *data;
        mr->size = actual;
        TLOG("MAP memory=%p offset=%llu size=%llu ptr=%p", (void*)mem,
             (unsigned long long)off, (unsigned long long)actual, *data);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hkUnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    if (!real_unmap_memory) real_unmap_memory =
        (PFN_vkUnmapMemory)realDevFn(dev, "vkUnmapMemory");
    TLOG("UNMAP memory=%p", (void*)mem);
    // Snapshot small mappings before they disappear; drop the rest so a later
    // scan never touches a dangling pointer.
    for (int i = 0; i < g_map_count; ++i) {
        if (g_maps[i].memory != mem || g_maps[i].owned) continue;
        if (g_maps[i].size > 0 && g_maps[i].size <= 1024 * 1024) {
            void* copy = malloc((size_t)g_maps[i].size);
            if (copy) {
                memcpy(copy, g_maps[i].ptr, (size_t)g_maps[i].size);
                g_maps[i].ptr = copy;
                g_maps[i].owned = 1;
                continue;
            }
        }
        g_maps[i] = g_maps[--g_map_count];
        --i;
    }
    real_unmap_memory(dev, mem);
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateDescriptorSetLayout(
    VkDevice dev, const VkDescriptorSetLayoutCreateInfo* ci,
    const VkAllocationCallbacks* ac, VkDescriptorSetLayout* out) {
    if (!real_create_descriptor_set_layout) real_create_descriptor_set_layout =
        (PFN_vkCreateDescriptorSetLayout)realDevFn(dev, "vkCreateDescriptorSetLayout");
    VkResult r = real_create_descriptor_set_layout(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        TLOG("DSLAYOUT handle=%p flags=0x%x bindings=%u", (void*)*out,
             (unsigned)ci->flags, ci->bindingCount);
        for (uint32_t i = 0; i < ci->bindingCount; ++i) {
            const VkDescriptorSetLayoutBinding* b = &ci->pBindings[i];
            TLOG("  BINDING set_layout=%p binding=%u type=%d count=%u stage=0x%x",
                 (void*)*out, b->binding, (int)b->descriptorType, b->descriptorCount,
                 (unsigned)b->stageFlags);
        }
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreatePipelineLayout(
    VkDevice dev, const VkPipelineLayoutCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkPipelineLayout* out) {
    if (!real_create_pipeline_layout) real_create_pipeline_layout =
        (PFN_vkCreatePipelineLayout)realDevFn(dev, "vkCreatePipelineLayout");
    VkResult r = real_create_pipeline_layout(dev, ci, ac, out);
    if (r == VK_SUCCESS) {
        TLOG("PLAYOUT handle=%p sets=%u push_ranges=%u", (void*)*out, ci->setLayoutCount,
             ci->pushConstantRangeCount);
        for (uint32_t i = 0; i < ci->setLayoutCount; ++i)
            TLOG("  SET layout=%p", (void*)ci->pSetLayouts[i]);
        for (uint32_t i = 0; i < ci->pushConstantRangeCount; ++i)
            TLOG("  PUSH stage=0x%x offset=%u size=%u",
                 (unsigned)ci->pPushConstantRanges[i].stageFlags,
                 ci->pPushConstantRanges[i].offset, ci->pPushConstantRanges[i].size);
    }
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL hkCreateSampler(
    VkDevice dev, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks* ac,
    VkSampler* out) {
    if (!real_create_sampler) real_create_sampler =
        (PFN_vkCreateSampler)realDevFn(dev, "vkCreateSampler");
    VkResult r = real_create_sampler(dev, ci, ac, out);
    if (r == VK_SUCCESS)
        TLOG("SAMPLER handle=%p filter=%d/%d mip=%d/%d addr=%d/%d/%d", (void*)*out,
             (int)ci->magFilter, (int)ci->minFilter, (int)ci->mipmapMode,
             (int)ci->maxLod, (int)ci->addressModeU, (int)ci->addressModeV,
             (int)ci->addressModeW);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL hkUpdateDescriptorSets(
    VkDevice dev, uint32_t write_count, const VkWriteDescriptorSet* writes,
    uint32_t copy_count, const VkCopyDescriptorSet* copies) {
    if (!real_update_descriptor_sets) real_update_descriptor_sets =
        (PFN_vkUpdateDescriptorSets)realDevFn(dev, "vkUpdateDescriptorSets");
    if (g_desc_log_budget > 0) {
        for (uint32_t i = 0; i < write_count && g_desc_log_budget > 0; ++i) {
            const VkWriteDescriptorSet* w = &writes[i];
            const char* kind = "?";
            unsigned long long a = 0, b = 0;
            if (w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                kind = w->descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ? "ubo" : "ssbo";
                if (w->pBufferInfo && w->descriptorCount) {
                    a = (unsigned long long)(uintptr_t)w->pBufferInfo[0].buffer;
                    b = (unsigned long long)w->pBufferInfo[0].range;
                }
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ||
                       w->descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
                kind = w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE ? "tex" : "uav";
                if (w->pImageInfo && w->descriptorCount)
                    a = (unsigned long long)(uintptr_t)w->pImageInfo[0].imageView;
            } else if (w->descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                kind = "sampler";
                if (w->pImageInfo && w->descriptorCount)
                    a = (unsigned long long)(uintptr_t)w->pImageInfo[0].sampler;
            }
            TLOG("DESCWRITE set=%p binding=%u elem=%u count=%u type=%d kind=%s obj=%llx range=%llu",
                 (void*)w->dstSet, w->dstBinding, w->dstArrayElement, w->descriptorCount,
                 (int)w->descriptorType, kind, a, b);
            --g_desc_log_budget;
        }
    }
    real_update_descriptor_sets(dev, write_count, writes, copy_count, copies);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdBindPipeline(
    VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipeline pipe) {
    if (!real_cmd_bind_pipeline) real_cmd_bind_pipeline =
        (PFN_vkCmdBindPipeline)g_gdpa(g_device, "vkCmdBindPipeline");
    PipeRec* p = findPipe(pipe);
    TLOG("BINDPIPE cb=%p pipeline=%p shader=%p entry=%s", (void*)cb, (void*)pipe,
         p ? (void*)p->mod : (void*)0, p ? p->entry : "?");
    real_cmd_bind_pipeline(cb, bind, pipe);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdBindDescriptorSets(
    VkCommandBuffer cb, VkPipelineBindPoint bind, VkPipelineLayout layout, uint32_t first,
    uint32_t count, const VkDescriptorSet* sets, uint32_t dyn_count, const uint32_t* dyn) {
    if (!real_cmd_bind_descriptor_sets) real_cmd_bind_descriptor_sets =
        (PFN_vkCmdBindDescriptorSets)g_gdpa(g_device, "vkCmdBindDescriptorSets");
    TLOG("BINDSETS cb=%p layout=%p first=%u count=%u dyn=%u", (void*)cb, (void*)layout,
         first, count, dyn_count);
    real_cmd_bind_descriptor_sets(cb, bind, layout, first, count, sets, dyn_count, dyn);
}

// ---------------------------------------------------------------------------
// Constant buffer discovery
// ---------------------------------------------------------------------------
static uint32_t round8(uint32_t v) { return (v + 7u) / 8u * 8u; }

static void dumpBytes(const char* path, const void* data, size_t n) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fwrite(data, 1, n, f);
    fclose(f);
}

static int scanConstants(void) {
    const uint32_t hw = round8(g_out_w) / 2u, hh = round8(g_out_h) / 2u;
    const uint32_t qw = round8(g_out_w) / 4u, qh = round8(g_out_h) / 4u;
    for (int mi = 0; mi < g_map_count; ++mi) {
        const MapRec* m = &g_maps[mi];
        if (!m->ptr || m->size < 272) continue;
        const uint8_t* base = (const uint8_t*)m->ptr;
        for (uint64_t off = 0; off + 272 <= m->size; off += 16) {
            const uint32_t* u = (const uint32_t*)(base + off);
            if (!g_found_tensor && u[0] == hw && u[1] == hh && u[2] == 0 && u[3] == 0 &&
                u[4] == round8(g_out_w) && u[5] == round8(g_out_h) && u[8] == hw &&
                u[9] == hh && u[12] == hw && u[13] == hh && u[16] == qw &&
                u[17] == qh) {
                char path[64];
                sprintf(path, "tensor.bin");
                dumpBytes(path, u, 272);
                g_found_tensor = 1;
                TLOG("TENSOR found mem=%p off=0x%llx", (void*)m->memory,
                     (unsigned long long)off);
                for (int e = 0; e < 17; ++e)
                    TLOG("  T[%02d] = %u x %u (z=%u w=%u)", e, u[e * 4], u[e * 4 + 1],
                         u[e * 4 + 2], u[e * 4 + 3]);
            }
        }
        // Auto-exposure SPD constants (32 bytes).
        for (uint64_t off = 0; off + 32 <= m->size; off += 4) {
            if (g_found_spd) break;
            const uint8_t* p = base + off;
            uint32_t mips, nwg, wgx, wgy;
            float ix, iy, pre;
            memcpy(&mips, p + 0, 4);
            memcpy(&nwg, p + 4, 4);
            memcpy(&wgx, p + 8, 4);
            memcpy(&wgy, p + 12, 4);
            memcpy(&ix, p + 16, 4);
            memcpy(&iy, p + 20, 4);
            memcpy(&pre, p + 24, 4);
            const uint32_t nwg_expected = ((g_rw + 63) / 64) * ((g_rh + 63) / 64);
            if (mips >= 1 && mips <= 20 && nwg == nwg_expected && wgx == 0 && wgy == 0 &&
                ix > 0.0f && iy > 0.0f && ix < 1.0f && iy < 1.0f && pre > 0.0f &&
                pre < 100.0f) {
                dumpBytes("spd.bin", p, 32);
                g_found_spd = 1;
                TLOG("SPD_BLOCK found off=0x%llx mips=%u nwg=%u inv=(%.8f,%.8f) pre=%.4f",
                     (unsigned long long)off, mips, nwg, ix, iy, pre);
            }
        }
        // RCAS constants (32 bytes): [config uvec4][preExposure][pad].
        for (uint64_t off = 0; off + 32 <= m->size; off += 4) {
            if (g_found_rcas) break;
            const uint8_t* p = base + off;
            uint32_t c0, c1, c2, c3;
            float pre, p5, p6, p7;
            memcpy(&c0, p + 0, 4);
            memcpy(&c1, p + 4, 4);
            memcpy(&c2, p + 8, 4);
            memcpy(&c3, p + 12, 4);
            memcpy(&pre, p + 16, 4);
            memcpy(&p5, p + 20, 4);
            memcpy(&p6, p + 24, 4);
            memcpy(&p7, p + 28, 4);
            const float f0 = *(const float*)&c0;
            if (f0 > 0.0f && f0 < 1.0f && c2 == 0 && c3 == 0 && pre > 0.0f &&
                pre < 100.0f && p5 == 0.0f && p6 == 0.0f && p7 == 0.0f) {
                dumpBytes("rcas.bin", p, 32);
                g_found_rcas = 1;
                TLOG("RCAS_BLOCK found off=0x%llx f0=0x%08x f1=0x%08x pre=%.4f",
                     (unsigned long long)off, c0, c1, pre);
            }
        }
        for (uint64_t off = 0; off + 104 <= m->size; off += 4) {
            const uint8_t* p = base + off;
            int w, h, reset, wl, hl;
            float pre;
            memcpy(&w, p + 64, 4);
            memcpy(&h, p + 68, 4);
            memcpy(&reset, p + 72, 4);
            memcpy(&wl, p + 76, 4);
            memcpy(&hl, p + 80, 4);
            memcpy(&pre, p + 84, 4);
            if (!g_found_constants && w == (int)g_out_w && h == (int)g_out_h &&
                wl == (int)g_rw && hl == (int)g_rh && reset >= 0 && reset <= 1 &&
                pre > 0.0f && pre < 100.0f) {
                dumpBytes("constants.bin", p, 104);
                if (off + 144 <= m->size) {
                    dumpBytes("rcas.bin", p + 112, 32);
                    const uint32_t* r = (const uint32_t*)(p + 112);
                    TLOG("RCAS_BLOCK f0=0x%08x f1=0x%08x f2=0x%08x f3=0x%08x f4=0x%08x",
                         r[0], r[1], r[2], r[3], r[4]);
                }
                g_found_constants = 1;
                TLOG("CONSTANTS found mem=%p off=0x%llx reset=%d pre=%.6f wl=%d hl=%d",
                     (void*)m->memory, (unsigned long long)off, reset, pre, wl, hl);
                const float* f = (const float*)p;
                TLOG("  C: inv_size=(%.6f,%.6f) scale=(%.6f,%.6f) inv_scale=(%.6f,%.6f)",
                     f[0], f[1], f[2], f[3], f[4], f[5]);
                TLOG("  C: jitter=(%.6f,%.6f) mv_scale=(%.6f,%.6f) tex=(%.6f,%.6f)",
                     f[6], f[7], f[8], f[9], f[10], f[11]);
                TLOG("  C: max_render=(%.6f,%.6f) jc=(%.6f,%.6f) w=%d h=%d reset=%d wl=%d hl=%d pre=%.6f prev=%.6f rcas=%d sharp=%.6f",
                     f[12], f[13], f[14], f[15], w, h, reset, wl, hl, f[21], f[22],
                     *(const int*)(p + 92), *(const float*)(p + 96));
            }
        }
    }
    return g_found_tensor && g_found_constants;
}

static int g_dumped_maps;

static void dumpLiveMaps(void) {
    for (int i = 0; i < g_map_count; ++i) {
        if (!g_maps[i].ptr || g_maps[i].size == 0) continue;
        char path[64];
        sprintf(path, "map_%02d.bin", i);
        dumpBytes(path, g_maps[i].ptr, (size_t)g_maps[i].size);
        TLOG("MAPDUMP %s mem=%p size=%llu owned=%d", path, (void*)g_maps[i].memory,
             (unsigned long long)g_maps[i].size, g_maps[i].owned);
    }
}

static VKAPI_ATTR void VKAPI_CALL hkCmdDispatch(
    VkCommandBuffer cb, uint32_t gx, uint32_t gy, uint32_t gz) {
    if (!real_cmd_dispatch) real_cmd_dispatch =
        (PFN_vkCmdDispatch)g_gdpa(g_device, "vkCmdDispatch");
    if (!g_dumped_maps && getenv("FSR4_TRACE_DUMP_MAPS")) {
        dumpLiveMaps();
        g_dumped_maps = 1;
    }
    if (getenv("FSR4_TRACE_DUMP_MAPS_ALL")) dumpLiveMaps();
    scanConstants();
    if (g_dispatch_log_budget > 0) {
        TLOG("DISPATCH n=%llu cb=%p groups=%u,%u,%u", (unsigned long long)g_dispatch_index,
             (void*)cb, gx, gy, gz);
        --g_dispatch_log_budget;
    }
    ++g_dispatch_index;
    real_cmd_dispatch(cb, gx, gy, gz);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdDispatchBase(
    VkCommandBuffer cb, uint32_t bx, uint32_t by, uint32_t bz, uint32_t gx, uint32_t gy,
    uint32_t gz) {
    if (!real_cmd_dispatch_base) real_cmd_dispatch_base =
        (PFN_vkCmdDispatchBase)g_gdpa(g_device, "vkCmdDispatchBase");
    TLOG("DISPATCHBASE n=%llu groups=%u,%u,%u base=%u,%u,%u",
         (unsigned long long)g_dispatch_index, gx, gy, gz, bx, by, bz);
    ++g_dispatch_index;
    real_cmd_dispatch_base(cb, bx, by, bz, gx, gy, gz);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdDispatchIndirect(
    VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off) {
    if (!real_cmd_dispatch_indirect) real_cmd_dispatch_indirect =
        (PFN_vkCmdDispatchIndirect)g_gdpa(g_device, "vkCmdDispatchIndirect");
    TLOG("DISPATCHINDIRECT n=%llu buffer=%p offset=%llu",
         (unsigned long long)g_dispatch_index, (void*)buf, (unsigned long long)off);
    ++g_dispatch_index;
    real_cmd_dispatch_indirect(cb, buf, off);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdUpdateBuffer(
    VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size,
    const void* data) {
    if (!real_cmd_update_buffer) real_cmd_update_buffer =
        (PFN_vkCmdUpdateBuffer)g_gdpa(g_device, "vkCmdUpdateBuffer");
    TLOG("UPDATEBUFFER cb=%p buffer=%p offset=%llu size=%llu", (void*)cb, (void*)buf,
         (unsigned long long)off, (unsigned long long)size);
    if (size == 272) {
        dumpBytes("tensor.bin", data, 272);
        g_found_tensor = 1;
        const uint32_t* u = (const uint32_t*)data;
        for (int e = 0; e < 17; ++e)
            TLOG("  TU[%02d] = %u x %u", e, u[e * 4], u[e * 4 + 1]);
    }
    if (size == 104) {
        dumpBytes("constants.bin", data, 104);
        g_found_constants = 1;
    }
    real_cmd_update_buffer(cb, buf, off, size, data);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdClearColorImage(
    VkCommandBuffer cb, VkImage image, VkImageLayout layout, const VkClearColorValue* color,
    uint32_t range_count, const VkImageSubresourceRange* ranges) {
    if (!real_cmd_clear_color_image) real_cmd_clear_color_image =
        (PFN_vkCmdClearColorImage)g_gdpa(g_device, "vkCmdClearColorImage");
    TLOG("CLEARCOLOR cb=%p image=%p layout=%d rgba=%.6f,%.6f,%.6f,%.6f", (void*)cb,
         (void*)image, (int)layout, color->float32[0], color->float32[1],
         color->float32[2], color->float32[3]);
    real_cmd_clear_color_image(cb, image, layout, color, range_count, ranges);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdFillBuffer(
    VkCommandBuffer cb, VkBuffer buf, VkDeviceSize off, VkDeviceSize size, uint32_t data) {
    if (!real_cmd_fill_buffer) real_cmd_fill_buffer =
        (PFN_vkCmdFillBuffer)g_gdpa(g_device, "vkCmdFillBuffer");
    TLOG("FILLBUFFER cb=%p buffer=%p offset=%llu size=%llu data=0x%x", (void*)cb,
         (void*)buf, (unsigned long long)off, (unsigned long long)size, data);
    real_cmd_fill_buffer(cb, buf, off, size, data);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdCopyBuffer(
    VkCommandBuffer cb, VkBuffer src, VkBuffer dst, uint32_t count,
    const VkBufferCopy* regions) {
    if (!real_cmd_copy_buffer) real_cmd_copy_buffer =
        (PFN_vkCmdCopyBuffer)g_gdpa(g_device, "vkCmdCopyBuffer");
    TLOG("COPYBUFFER cb=%p src=%p dst=%p regions=%u", (void*)cb, (void*)src, (void*)dst,
         count);
    real_cmd_copy_buffer(cb, src, dst, count, regions);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdCopyBufferToImage(
    VkCommandBuffer cb, VkBuffer src, VkImage dst, VkImageLayout layout, uint32_t count,
    const VkBufferImageCopy* regions) {
    if (!real_cmd_copy_buffer_to_image) real_cmd_copy_buffer_to_image =
        (PFN_vkCmdCopyBufferToImage)g_gdpa(g_device, "vkCmdCopyBufferToImage");
    TLOG("COPYBUFFERTOIMAGE cb=%p src=%p image=%p layout=%d regions=%u", (void*)cb,
         (void*)src, (void*)dst, (int)layout, count);
    real_cmd_copy_buffer_to_image(cb, src, dst, layout, count, regions);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdCopyImage(
    VkCommandBuffer cb, VkImage src, VkImageLayout src_layout, VkImage dst,
    VkImageLayout dst_layout, uint32_t count, const VkImageCopy* regions) {
    if (!real_cmd_copy_image) real_cmd_copy_image =
        (PFN_vkCmdCopyImage)g_gdpa(g_device, "vkCmdCopyImage");
    TLOG("COPYIMAGE cb=%p src=%p dst=%p regions=%u", (void*)cb, (void*)src, (void*)dst,
         count);
    real_cmd_copy_image(cb, src, src_layout, dst, dst_layout, count, regions);
}

static VKAPI_ATTR void VKAPI_CALL hkEndCommandBuffer(VkCommandBuffer cb) {
    if (!real_end_command_buffer) real_end_command_buffer =
        (PFN_vkEndCommandBuffer)g_gdpa(g_device, "vkEndCommandBuffer");
    scanConstants();
    real_end_command_buffer(cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL hkQueueSubmit(
    VkQueue q, uint32_t count, const VkSubmitInfo* si, VkFence fence) {
    if (!real_queue_submit) real_queue_submit =
        (PFN_vkQueueSubmit)g_gdpa(g_device, "vkQueueSubmit");
    scanConstants();
    return real_queue_submit(q, count, si, fence);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdPipelineBarrier(
    VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
    VkDependencyFlags f, uint32_t mc, const VkMemoryBarrier* mb, uint32_t bc,
    const VkBufferMemoryBarrier* bb, uint32_t ic, const VkImageMemoryBarrier* ib) {
    if (!real_cmd_pipeline_barrier) real_cmd_pipeline_barrier =
        (PFN_vkCmdPipelineBarrier)g_gdpa(g_device, "vkCmdPipelineBarrier");
    TLOG("BARRIER cb=%p src=0x%x dst=0x%x mem=%u buf=%u img=%u", (void*)cb,
         (unsigned)src, (unsigned)dst, mc, bc, ic);
    real_cmd_pipeline_barrier(cb, src, dst, f, mc, mb, bc, bb, ic, ib);
}

static VKAPI_ATTR void VKAPI_CALL hkCmdPipelineBarrier2(
    VkCommandBuffer cb, const VkDependencyInfo* di) {
    if (!real_cmd_pipeline_barrier2) real_cmd_pipeline_barrier2 =
        (PFN_vkCmdPipelineBarrier2)g_gdpa(g_device, "vkCmdPipelineBarrier2");
    TLOG("BARRIER2 cb=%p mem=%u buf=%u img=%u", (void*)cb, di->memoryBarrierCount,
         di->bufferMemoryBarrierCount, di->imageMemoryBarrierCount);
    real_cmd_pipeline_barrier2(cb, di);
}

static PFN_vkVoidFunction hookFor(const char* name) {
#define HOOK(fn) if (strcmp(name, "vk" #fn) == 0) return (PFN_vkVoidFunction)hk##fn
    HOOK(CreateShaderModule);
    HOOK(DestroyShaderModule);
    HOOK(DestroyPipeline);
    HOOK(CreateComputePipelines);
    HOOK(CreateBuffer);
    HOOK(CreateImage);
    HOOK(CreateImageView);
    HOOK(AllocateMemory);
    HOOK(BindBufferMemory);
    HOOK(BindImageMemory);
    HOOK(MapMemory);
    HOOK(UnmapMemory);
    HOOK(CreateDescriptorSetLayout);
    HOOK(CreatePipelineLayout);
    HOOK(CreateSampler);
    HOOK(UpdateDescriptorSets);
    HOOK(CmdBindPipeline);
    HOOK(CmdBindDescriptorSets);
    HOOK(CmdDispatch);
    HOOK(CmdDispatchBase);
    HOOK(CmdDispatchIndirect);
    HOOK(CmdUpdateBuffer);
    HOOK(CmdClearColorImage);
    HOOK(CmdCopyBuffer);
    HOOK(CmdCopyBufferToImage);
    HOOK(CmdCopyImage);
    HOOK(CmdFillBuffer);
    HOOK(CmdPipelineBarrier);
    HOOK(CmdPipelineBarrier2);
    HOOK(EndCommandBuffer);
    HOOK(QueueSubmit);
#undef HOOK
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL hookedGetDeviceProcAddr(
    VkDevice dev, const char* name) {
    PFN_vkVoidFunction h = hookFor(name);
    if (h) return h;
    return g_gdpa(dev, name);
}

// ---------------------------------------------------------------------------
// Vulkan bootstrap (same shape as the reference harness)
// ---------------------------------------------------------------------------
#define VK_GLOBAL(name) static PFN_##name name;
VK_GLOBAL(vkCreateInstance) VK_GLOBAL(vkEnumeratePhysicalDevices) VK_GLOBAL(vkGetPhysicalDeviceProperties)
VK_GLOBAL(vkCreateDevice) VK_GLOBAL(vkGetDeviceProcAddr) VK_GLOBAL(vkDestroyDevice) VK_GLOBAL(vkDestroyInstance)
VK_GLOBAL(vkGetDeviceQueue) VK_GLOBAL(vkCreateImage) VK_GLOBAL(vkGetImageMemoryRequirements)
VK_GLOBAL(vkAllocateMemory) VK_GLOBAL(vkBindImageMemory) VK_GLOBAL(vkCreateImageView)
VK_GLOBAL(vkCreateCommandPool) VK_GLOBAL(vkAllocateCommandBuffers) VK_GLOBAL(vkBeginCommandBuffer)
VK_GLOBAL(vkEndCommandBuffer) VK_GLOBAL(vkCreateFence) VK_GLOBAL(vkQueueSubmit) VK_GLOBAL(vkWaitForFences)
VK_GLOBAL(vkResetFences) VK_GLOBAL(vkCmdPipelineBarrier) VK_GLOBAL(vkCreateBuffer)
VK_GLOBAL(vkGetBufferMemoryRequirements) VK_GLOBAL(vkBindBufferMemory) VK_GLOBAL(vkMapMemory)
VK_GLOBAL(vkUnmapMemory) VK_GLOBAL(vkCmdCopyImageToBuffer) VK_GLOBAL(vkCmdCopyBufferToImage)
VK_GLOBAL(vkDestroyImage) VK_GLOBAL(vkDestroyImageView) VK_GLOBAL(vkFreeMemory)
VK_GLOBAL(vkDestroyBuffer) VK_GLOBAL(vkDestroyCommandPool) VK_GLOBAL(vkDestroyFence)
VK_GLOBAL(vkDeviceWaitIdle)

#define VK_CHECK(x)                                                       \
    do {                                                                  \
        VkResult r_ = (x);                                                \
        if (r_ != VK_SUCCESS) {                                           \
            printf("VK_FAIL %s = %d\n", #x, (int)r_);                     \
            return 1;                                                     \
        }                                                                 \
    } while (0)

static void ffxMessageCb(uint32_t type, const wchar_t* message) {
    printf("[ffx] type=%u %ls\n", type, message ? message : L"(null)");
}

typedef ffxReturnCode_t (*PFN_ffxCreateContext)(ffxContext*, ffxCreateContextDescHeader*,
                                                const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PFN_ffxDestroyContext)(ffxContext*, const ffxAllocationCallbacks*);
typedef ffxReturnCode_t (*PFN_ffxDispatch)(ffxContext*, const ffxDispatchDescHeader*);
typedef ffxReturnCode_t (*PFN_ffxQuery)(ffxContext*, const ffxQueryDescHeader*);

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    if (argc == 5) {
        g_rw = (uint32_t)atoi(argv[1]);
        g_rh = (uint32_t)atoi(argv[2]);
        g_out_w = (uint32_t)atoi(argv[3]);
        g_out_h = (uint32_t)atoi(argv[4]);
    }
    g_verbose = getenv("FSR4_TRACE_VERBOSE") != NULL;
    if (const char* v = getenv("FSR4_CREATE_FLAGS")) g_create_flags = (uint32_t)strtoul(v, NULL, 0);
    if (const char* v = getenv("FSR4_DISPATCH_FLAGS")) g_dispatch_flags = (uint32_t)strtoul(v, NULL, 0);
    if (const char* v = getenv("FSR4_SHARPEN")) g_sharpen = atoi(v) != 0;
    if (const char* v = getenv("FSR4_SHARPNESS")) g_sharpness = (float)atof(v);
    if (const char* v = getenv("FSR4_CTX_MAX_R")) sscanf(v, "%ux%u", &g_ctx_max_rw, &g_ctx_max_rh);
    if (const char* v = getenv("FSR4_CTX_MAX_U")) sscanf(v, "%ux%u", &g_ctx_max_uw, &g_ctx_max_uh);

    g_ctx_max_rw = g_ctx_max_rw ? g_ctx_max_rw : g_rw;
    g_ctx_max_rh = g_ctx_max_rh ? g_ctx_max_rh : g_rh;
    g_ctx_max_uw = g_ctx_max_uw ? g_ctx_max_uw : g_out_w;
    g_ctx_max_uh = g_ctx_max_uh ? g_ctx_max_uh : g_out_h;
    g_log = fopen("trace.txt", "w");
    TLOG("FSR4_TRACE render=%ux%u output=%ux%u ctx_render=%ux%u ctx_output=%ux%u create_flags=0x%x dispatch_flags=0x%x sharpen=%d sharpness=%.3f",
         g_rw, g_rh, g_out_w, g_out_h, g_ctx_max_rw, g_ctx_max_rh, g_ctx_max_uw,
         g_ctx_max_uh, g_create_flags, g_dispatch_flags, g_sharpen, g_sharpness);

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) { printf("no vulkan-1.dll\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa =
        (PFN_vkGetInstanceProcAddr)GetProcAddress(vk, "vkGetInstanceProcAddr");
    if (!gipa) { printf("no vkGetInstanceProcAddr\n"); return 1; }

    VkInstance inst;
    VkApplicationInfo appInfo = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "fsr4-trace";
    appInfo.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appInfo;
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance) { printf("missing vkCreateInstance\n"); return 1; }
    VK_CHECK(vkCreateInstance(&ici, NULL, &inst));
#define LOAD_G(name) name = (PFN_##name)gipa(inst, #name); if (!name) { printf("missing " #name "\n"); return 1; }
    LOAD_G(vkEnumeratePhysicalDevices) LOAD_G(vkGetPhysicalDeviceProperties)
    LOAD_G(vkCreateDevice) LOAD_G(vkGetDeviceProcAddr) LOAD_G(vkDestroyDevice)
    LOAD_G(vkDestroyInstance)
#undef LOAD_G

    uint32_t devCount = 0;
    vkEnumeratePhysicalDevices(inst, &devCount, NULL);
    if (!devCount) { printf("no physical devices\n"); return 1; }
    VkPhysicalDevice phys[8];
    vkEnumeratePhysicalDevices(inst, &devCount, phys);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys[0], &props);
    printf("GPU: %s (vendor 0x%x device 0x%x) api %u.%u.%u\n", props.deviceName,
           props.vendorID, props.deviceID, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    TLOG("GPU %s vendor=0x%x device=0x%x api=%u.%u.%u", props.deviceName, props.vendorID,
         props.deviceID, VK_VERSION_MAJOR(props.apiVersion),
         VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    g_gdpa = vkGetDeviceProcAddr;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = 0;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev;
    VK_CHECK(vkCreateDevice(phys[0], &dci, NULL, &dev));
    g_device = dev;
#define LOAD_D(name) name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); if (!name) { printf("missing dev " #name "\n"); return 1; }
    LOAD_D(vkGetDeviceQueue) LOAD_D(vkCreateImage) LOAD_D(vkGetImageMemoryRequirements)
    LOAD_D(vkAllocateMemory) LOAD_D(vkBindImageMemory) LOAD_D(vkCreateImageView)
    LOAD_D(vkCreateCommandPool) LOAD_D(vkAllocateCommandBuffers)
    LOAD_D(vkBeginCommandBuffer) LOAD_D(vkEndCommandBuffer) LOAD_D(vkCreateFence)
    LOAD_D(vkQueueSubmit) LOAD_D(vkWaitForFences) LOAD_D(vkResetFences)
    LOAD_D(vkCmdPipelineBarrier) LOAD_D(vkCreateBuffer)
    LOAD_D(vkGetBufferMemoryRequirements) LOAD_D(vkBindBufferMemory) LOAD_D(vkMapMemory)
    LOAD_D(vkUnmapMemory) LOAD_D(vkCmdCopyImageToBuffer) LOAD_D(vkCmdCopyBufferToImage)
    LOAD_D(vkDestroyImage) LOAD_D(vkDestroyImageView) LOAD_D(vkFreeMemory)
    LOAD_D(vkDestroyBuffer) LOAD_D(vkDestroyCommandPool) LOAD_D(vkDestroyFence)
    LOAD_D(vkDeviceWaitIdle)
#undef LOAD_D

    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    const uint32_t RW = g_rw, RH = g_rh, OW = g_out_w, OH = g_out_h;

    VkPhysicalDeviceMemoryProperties mp;
    PFN_vkGetPhysicalDeviceMemoryProperties gmp =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst,
                                                      "vkGetPhysicalDeviceMemoryProperties");
    if (!gmp) { printf("missing memory props\n"); return 1; }
    gmp(phys[0], &mp);

    // --- images ---
    VkImage images[4];
    VkDeviceMemory mems[4];
    VkImageView views[4];
    VkImageCreateInfo icis[4];
    VkFormat fmts[4] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D32_SFLOAT,
                        VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM};
    uint32_t ws[4] = {RW, RW, RW, OW}, hs[4] = {RH, RH, RH, OH};
    VkImageUsageFlags usages[4] = {
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT};
    for (int i = 0; i < 4; i++) {
        VkImageCreateInfo c = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        c.imageType = VK_IMAGE_TYPE_2D;
        c.format = fmts[i];
        c.extent = (VkExtent3D){ws[i], hs[i], 1};
        c.mipLevels = 1;
        c.arrayLayers = 1;
        c.samples = VK_SAMPLE_COUNT_1_BIT;
        c.tiling = VK_IMAGE_TILING_OPTIMAL;
        c.usage = usages[i];
        c.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        icis[i] = c;
        VK_CHECK(vkCreateImage(dev, &c, NULL, &images[i]));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(dev, images[i], &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = 0;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((mr.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                ai.memoryTypeIndex = t;
                break;
            }
        }
        VK_CHECK(vkAllocateMemory(dev, &ai, NULL, &mems[i]));
        VK_CHECK(vkBindImageMemory(dev, images[i], mems[i], 0));
        VkImageViewCreateInfo v = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image = images[i];
        v.viewType = VK_IMAGE_VIEW_TYPE_2D;
        v.format = fmts[i];
        v.subresourceRange.aspectMask =
            (i == 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        v.subresourceRange.levelCount = 1;
        v.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(dev, &v, NULL, &views[i]));
    }

    // staging buffer for output readback
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    VkDeviceSize stagingSize = (VkDeviceSize)OW * OH * 4;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = stagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VK_CHECK(vkCreateBuffer(dev, &bci, NULL, &staging));
    VkMemoryRequirements bmr;
    vkGetBufferMemoryRequirements(dev, staging, &bmr);
    VkMemoryAllocateInfo bmai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    bmai.allocationSize = bmr.size;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
        if ((bmr.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            bmai.memoryTypeIndex = t;
            break;
        }
    }
    VK_CHECK(vkAllocateMemory(dev, &bmai, NULL, &stagingMem));
    VK_CHECK(vkBindBufferMemory(dev, staging, stagingMem, 0));

    // --- deterministic inputs ---
    const VkDeviceSize colorBytes = (VkDeviceSize)RW * RH * 4;
    const VkDeviceSize depthBytes = (VkDeviceSize)RW * RH * 4;
    const VkDeviceSize mvBytes = (VkDeviceSize)RW * RH * 4;
    unsigned char* colorCpu = (unsigned char*)malloc((size_t)colorBytes);
    float* depthCpu = (float*)malloc((size_t)depthBytes);
    uint16_t* mvCpu = (uint16_t*)malloc((size_t)mvBytes);
    if (!colorCpu || !depthCpu || !mvCpu) return 1;
    for (uint32_t y = 0; y < RH; y++) {
        for (uint32_t x = 0; x < RW; x++) {
            unsigned char* p = colorCpu + ((VkDeviceSize)y * RW + x) * 4;
            p[0] = (unsigned char)(x * 255 / RW);
            p[1] = (unsigned char)(y * 255 / RH);
            p[2] = 200;
            p[3] = 255;
        }
    }
    for (VkDeviceSize i = 0; i < (VkDeviceSize)RW * RH; ++i) depthCpu[i] = 1.0f;
    memset(mvCpu, 0, (size_t)mvBytes);
    dumpBytes("in_color.rgba8", colorCpu, (size_t)colorBytes);
    dumpBytes("in_depth.f32", depthCpu, (size_t)depthBytes);
    dumpBytes("in_mv.rg16f", mvCpu, (size_t)mvBytes);
    {
        VkBuffer inbuf;
        VkDeviceMemory inmem;
        VkBufferCreateInfo ibci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ibci.size = colorBytes + depthBytes + mvBytes;
        ibci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_CHECK(vkCreateBuffer(dev, &ibci, NULL, &inbuf));
        VkMemoryRequirements imr;
        vkGetBufferMemoryRequirements(dev, inbuf, &imr);
        VkMemoryAllocateInfo imai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        imai.allocationSize = imr.size;
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            if ((imr.memoryTypeBits & (1u << t)) &&
                (mp.memoryTypes[t].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
                imai.memoryTypeIndex = t;
                break;
            }
        }
        VK_CHECK(vkAllocateMemory(dev, &imai, NULL, &inmem));
        VK_CHECK(vkBindBufferMemory(dev, inbuf, inmem, 0));
        void* mapped = NULL;
        VK_CHECK(vkMapMemory(dev, inmem, 0, ibci.size, 0, &mapped));
        memcpy(mapped, colorCpu, (size_t)colorBytes);
        memcpy((unsigned char*)mapped + colorBytes, depthCpu, (size_t)depthBytes);
        memcpy((unsigned char*)mapped + colorBytes + depthBytes, mvCpu, (size_t)mvBytes);
        vkUnmapMemory(dev, inmem);

        VkCommandPool fpool;
        VkCommandPoolCreateInfo fpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        fpci.queueFamilyIndex = 0;
        VK_CHECK(vkCreateCommandPool(dev, &fpci, NULL, &fpool));
        VkCommandBuffer fcb;
        VkCommandBufferAllocateInfo fcai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        fcai.commandPool = fpool;
        fcai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        fcai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &fcai, &fcb));
        VkCommandBufferBeginInfo fbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(fcb, &fbbi));
        VkImageMemoryBarrier pre[3] = {
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
             images[0], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
             images[1], {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
             images[2], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
        vkCmdPipelineBarrier(fcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 3,
                             pre);
        VkBufferImageCopy cr = {0};
        cr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        cr.imageSubresource.layerCount = 1;
        cr.imageExtent = (VkExtent3D){RW, RH, 1};
        vkCmdCopyBufferToImage(fcb, inbuf, images[0], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &cr);
        VkBufferImageCopy dr = cr;
        dr.bufferOffset = colorBytes;
        dr.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(fcb, inbuf, images[1], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &dr);
        VkBufferImageCopy mr = cr;
        mr.bufferOffset = colorBytes + depthBytes;
        vkCmdCopyBufferToImage(fcb, inbuf, images[2], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &mr);
        VkImageMemoryBarrier post[4] = {
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, images[0],
             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, images[1],
             {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
             VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0, images[2],
             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_SHADER_READ_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, 0,
             images[3], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}}};
        vkCmdPipelineBarrier(fcb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 4,
                             post);
        VK_CHECK(vkEndCommandBuffer(fcb));
        VkFence ff;
        VkFenceCreateInfo ffc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VK_CHECK(vkCreateFence(dev, &ffc, NULL, &ff));
        VkSubmitInfo fsi = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        fsi.commandBufferCount = 1;
        fsi.pCommandBuffers = &fcb;
        VK_CHECK(vkQueueSubmit(queue, 1, &fsi, ff));
        VK_CHECK(vkWaitForFences(dev, 1, &ff, VK_TRUE, UINT64_MAX));
        vkDestroyFence(dev, ff, NULL);
        vkDestroyCommandPool(dev, fpool, NULL);
        vkDestroyBuffer(dev, inbuf, NULL);
        vkFreeMemory(dev, inmem, NULL);
    }

    // --- load the FSR SDK loader + upscaler ---
    HMODULE loader = LoadLibraryA("amd_fidelityfx_loader_vk.dll");
    if (!loader) { printf("LoadLibrary loader failed %lu\n", GetLastError()); return 1; }
    PFN_ffxCreateContext pCreate = (PFN_ffxCreateContext)GetProcAddress(loader, "ffxCreateContext");
    PFN_ffxDestroyContext pDestroy = (PFN_ffxDestroyContext)GetProcAddress(loader, "ffxDestroyContext");
    PFN_ffxDispatch pDispatch = (PFN_ffxDispatch)GetProcAddress(loader, "ffxDispatch");
    PFN_ffxQuery pQuery = (PFN_ffxQuery)GetProcAddress(loader, "ffxQuery");
    if (!pCreate || !pDestroy || !pDispatch || !pQuery) { printf("missing ffx exports\n"); return 1; }
    printf("loader + exports OK\n");

    VkDeviceContext devCtx = {0};
    devCtx.vkDevice = dev;
    devCtx.vkPhysicalDevice = phys[0];
    devCtx.vkDeviceProcAddr = hookedGetDeviceProcAddr;

    ffxCreateBackendVKDesc backend = {0};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = devCtx;

    ffxCreateContextDescUpscaleVersion ver = {0};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;

    ffxCreateContextDescUpscale upscale = {0};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.maxRenderSize.width = g_ctx_max_rw;
    upscale.maxRenderSize.height = g_ctx_max_rh;
    upscale.maxUpscaleSize.width = g_ctx_max_uw;
    upscale.maxUpscaleSize.height = g_ctx_max_uh;
    upscale.flags = g_create_flags;
    upscale.fpMessage = ffxMessageCb;
    upscale.header.pNext = &ver.header;
    ver.header.pNext = &backend.header;

    ffxContext ctx = NULL;
    ffxReturnCode_t rc = pCreate(&ctx, &upscale.header, NULL);
    printf("ffxCreateContext -> 0x%x\n", rc);
    TLOG("CREATE rc=0x%x", rc);
    if (rc != FFX_API_RETURN_OK || !ctx) return 1;

    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = 0;
    VK_CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &bbi));

    ffxDispatchDescUpscale disp = {0};
    disp.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    disp.commandList = cb;
    disp.color = ffxApiGetResourceVK(
        images[0], ffxApiGetImageResourceDescriptionVK(images[0], icis[0],
                                                      VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    disp.depth = ffxApiGetResourceVK(
        images[1], ffxApiGetImageResourceDescriptionVK(images[1], icis[1],
                                                      VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    disp.motionVectors = ffxApiGetResourceVK(
        images[2], ffxApiGetImageResourceDescriptionVK(images[2], icis[2],
                                                      VK_IMAGE_USAGE_SAMPLED_BIT),
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    disp.output = ffxApiGetResourceVK(
        images[3],
        ffxApiGetImageResourceDescriptionVK(images[3], icis[3], VK_IMAGE_USAGE_STORAGE_BIT),
        VK_IMAGE_LAYOUT_GENERAL);
    disp.renderSize.width = RW;
    disp.renderSize.height = RH;
    disp.upscaleSize.width = OW;
    disp.upscaleSize.height = OH;
    disp.motionVectorScale.x = (float)RW;
    disp.motionVectorScale.y = (float)RH;
    disp.frameTimeDelta = 16.7f;
    disp.preExposure = 1.0f;
    disp.cameraNear = 0.1f;
    disp.cameraFar = 1000.0f;
    disp.cameraFovAngleVertical = 1.0472f;
    disp.viewSpaceToMetersFactor = 1.0f;
    disp.enableSharpening = g_sharpen;
    disp.sharpness = g_sharpness;
    disp.flags = g_dispatch_flags;

    rc = pDispatch(&ctx, &disp.header);
    printf("ffxDispatch -> 0x%x\n", rc);
    TLOG("DISPATCH_RC rc=0x%x", rc);
    if (rc != FFX_API_RETURN_OK) return 1;

    VkImageMemoryBarrier imb = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    imb.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.image = images[3];
    imb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imb.subresourceRange.levelCount = 1;
    imb.subresourceRange.layerCount = 1;
    imb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = (VkExtent3D){OW, OH, 1};
    vkCmdCopyImageToBuffer(cb, images[3], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1,
                           &region);
    VK_CHECK(vkEndCommandBuffer(cb));

    VkFence fence;
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VK_CHECK(vkQueueSubmit(queue, 1, &si, fence));
    VK_CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX));

    void* mapped = NULL;
    VK_CHECK(vkMapMemory(dev, stagingMem, 0, stagingSize, 0, &mapped));
    unsigned long long sum = 0;
    int nonzero = 0;
    const unsigned char* px = (const unsigned char*)mapped;
    for (VkDeviceSize i = 0; i < stagingSize; i++) {
        sum += px[i];
        if (px[i]) nonzero++;
    }
    dumpBytes("out_color.rgba8", mapped, (size_t)stagingSize);
    vkUnmapMemory(dev, stagingMem);
    printf("output readback: %d/%lld bytes non-zero, sum=%llu\n", nonzero,
           (long long)stagingSize, sum);
    TLOG("OUTPUT nonzero=%d bytes=%lld sum=%llu", nonzero, (long long)stagingSize, sum);
    TLOG("SUMMARY dispatches=%llu shaders=%d pipelines=%d images=%d buffers=%d tensor=%d constants=%d",
         (unsigned long long)g_dispatch_index, g_shader_count, g_pipe_count, g_image_count,
         g_buffer_count, g_found_tensor, g_found_constants);

    pDestroy(&ctx, NULL);
    printf("context destroyed OK\n");
    if (g_log) fclose(g_log);
    return 0;
}
