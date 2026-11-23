// MK8-online MLFI provider: an FFX-compatible shell for AMD's FSR 4 ML frame interpolation.
//
// This library exports the five FFX entry points and runs the pass graph recovered from the signed
// provider (docs/en/reports/MLFI-CAPTURE.md, docs/en/reports/MLFI-REPLAY.md) inside the application's Vulkan command
// buffer.  It does not contain any AMD byte: the SPIR-V modules and the model bank are read at
// context creation from the user's own extraction (`tools/extract_mlfi.py`) after
// `tools/mlfi-replay/prepare.py` has legalised the four modules RADV cannot compile unchanged.
//
// The effect is the MLFI provider 4.0.2, id 0xF600000001000002 (same construction as the signed
// DLL: 0xF6000000 << 32 | version 4.0.2).  Selection follows the frame-generation descriptor type
// 0x00020001 and, when present, ffxOverrideVersion with that id.
//
// What is ours, not AMD's:
//   * the compose pass (compose.vert/frag) that writes `rw_prediction` into outputs[0], because
//     the vendor's final 8,980-byte `main` was never dispatched in the capture;
//   * the three input-copy shaders that move the application's present colour, depth and motion
//     vectors into the provider's images;
//   * the derivation of the per-frame camera constants from ffxDispatchDescFrameGenerationPrepareV2.
//
// This provider is validated headlessly at 1280x720 (see tools/mlfi-provider/mlfi_provider_test.c
// and docs/en/reports/MLFI-PROVIDER.md).  It does not implement the swapchain provider, HDR/sRGB outputs, any
// resolution other than 1280x720, or more than one frame in flight.

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include <vulkan/vulkan.h>

#include "ffx/ffx_api.h"
#include "ffx/ffx_api_types.h"
#include "ffx/ffx_framegeneration.h"
#include "ffx/vk/ffx_api_vk.h"

#include "mlfi_shaders.h"
#include "mlfi_graph.h"
#include "mlfi_provider_ext.h"

// The SDK headers are written for C++ (where `struct X` introduces the type name); in C the
// struct tags have to be named explicitly.  These typedefs cover the descriptors the provider
// touches and do not change the ABI.
typedef struct FfxApiResource FfxApiResource;
typedef struct ffxCreateContextDescFrameGeneration ffxCreateContextDescFrameGeneration;
typedef struct ffxCreateBackendVKDesc ffxCreateBackendVKDesc;
typedef struct ffxCreateContextDescFrameGenerationVersion ffxCreateContextDescFrameGenerationVersion;
typedef struct ffxOverrideVersion ffxOverrideVersion;
typedef struct ffxConfigureDescFrameGeneration ffxConfigureDescFrameGeneration;
typedef struct ffxConfigureDescGlobalDebug ffxConfigureDescGlobalDebug;
typedef struct ffxConfigureDescGlobalDebug1 ffxConfigureDescGlobalDebug1;
typedef struct ffxQueryDescGetVersions ffxQueryDescGetVersions;
typedef struct ffxQueryGetProviderVersion ffxQueryGetProviderVersion;
typedef struct ffxDispatchDescFrameGenerationPrepareV2 ffxDispatchDescFrameGenerationPrepareV2;

// ------------------------------------------------------------------ identity and limits ----------

#define MLFI_PROVIDER_VERSION_ID 0xf600000001000002ull  // 0xF6000000 << 32 | version 4.0.2
#define MLFI_PROVIDER_VERSION_NAME "4.0.2"
#define MLFI_PROVIDER_VERSION_NUMBER ((4u << 22) | (0u << 12) | 2u)
_Static_assert((MLFI_PROVIDER_VERSION_ID & 0xffffffffull) == MLFI_PROVIDER_VERSION_NUMBER,
               "the provider id's low 32 bits must be the packed 4.0.2 version");

#define MLFI_SUPPORTED_WIDTH 1280u
#define MLFI_SUPPORTED_HEIGHT 720u

#define MLFI_MODEL_SIZE 10657152ull        // model_blob[72 : 72 + 10,657,152]
#define MLFI_MODEL_BANK_OFFSET 72ull
#define MLFI_MODEL_FNV1A 0xb88e755565aa4249ull
#define MLFI_SCRATCH_SIZE 58397248ull
#define MLFI_ARENA_SIZE 3276800ull
#define MLFI_TENSOR_TABLE_SIZE 1472u

#define MLFI_MAX_BINDINGS 32
#define MLFI_MAX_MODULES 256
#define MLFI_MAX_PIPES 256
#define MLFI_MAX_IMAGES 64
#define MLFI_MAX_APPS 32
#define MLFI_PREPARE_ROWS 5

#define MLFI_API_VERSION VK_API_VERSION_1_3

// The five preprocess constant block sizes, in the capture's dispatch order.
static const uint32_t g_prepare_sizes[MLFI_PREPARE_ROWS] = {80, 64, 144, 64, 64};

// ------------------------------------------------------------------ logging -----------------------

typedef struct MlfiContext MlfiContext;

// ------------------------------------------------------------------ SPIR-V front end ---------------

typedef struct
{
    uint32_t set, binding;
    uint32_t var_id;
    VkDescriptorType type;
    char name[64];
    uint32_t image_format;
} MlfiBinding;

typedef struct
{
    char entry[96];
    char path[4096];
    char ubo_name[64];
    uint8_t* code;
    size_t size;
    uint32_t version;
    uint32_t local[3];
    MlfiBinding binds[MLFI_MAX_BINDINGS];
    uint32_t nbind;
} MlfiModule;

enum
{
    K_NONE = 0,
    K_SAMPLER,
    K_IMAGE,
    K_STRUCT,
    K_POINTER,
    K_BOOL,
    K_VOID,
    K_FLOAT,
    K_INT,
    K_VECTOR
};

typedef struct
{
    uint8_t kind[65536];
    uint8_t block[65536];
    uint16_t pointer_pointee[65536];
    uint8_t pointer_storage[65536];
    uint16_t image_sampled_type[65536];
    uint8_t image_sampled[65536];
    uint32_t image_format[65536];
    uint32_t image_dim[65536];
    uint32_t binding[65536];
    uint32_t set[65536];
    uint32_t var_type[65536];
    uint32_t var_storage[65536];
    uint8_t scalar_kind[65536];
    uint8_t scalar_bits[65536];
    uint8_t in_iface[65536];
    char* names[65536];
} MlfiSpirvScan;

static MlfiSpirvScan* g_scan;

static uint32_t rd32(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

// Same offset basis as tools/mlfi-capture/mlfi_capture.c and tools/mlfi-replay/mlfi_replay.c
// (1469598103934665603, not the canonical FNV-1a 64 basis 14695981039346656037), so the model
// hash can be compared with the capture log and with the replay.
static uint64_t fnv1a64(const void* data, size_t size)
{
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static void* mlfi_host_alloc(MlfiContext* ctx, size_t size);
static void mlfi_host_free(MlfiContext* ctx, void* pointer);

static uint8_t* mlfi_read_file(MlfiContext* ctx, const char* path, size_t* size)
{
    FILE* f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0)
    {
        fclose(f);
        return NULL;
    }
    uint8_t* data = (uint8_t*)mlfi_host_alloc(ctx, (size_t)n ? (size_t)n : 1);
    if (!data)
    {
        fclose(f);
        return NULL;
    }
    if ((size_t)n && fread(data, 1, (size_t)n, f) != (size_t)n)
    {
        mlfi_host_free(ctx, data);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *size = (size_t)n;
    return data;
}

static char* mlfi_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    char* p = (char*)malloc(n);
    if (p)
        memcpy(p, s, n);
    return p;
}

// Parse decorations and the type graph of one module.  Returns 0 on success.  This is the same
// front end as tools/mlfi-replay/mlfi_replay.c: the captured descriptor sets contain exactly the
// variables listed in OpEntryPoint for SPIR-V 1.4+ modules.
static int mlfi_spv_scan(const uint8_t* code, size_t size, MlfiModule* m)
{
    if (size < 20 || (size % 4) || rd32(code) != 0x07230203)
        return -1;
    m->version = rd32(code + 4);
    uint32_t words = (uint32_t)(size / 4);
    MlfiSpirvScan* s = g_scan;
    memset(s->kind, 0, sizeof(s->kind));
    memset(s->block, 0, sizeof(s->block));
    memset(s->image_sampled, 0, sizeof(s->image_sampled));
    memset(s->scalar_kind, 0, sizeof(s->scalar_kind));
    memset(s->in_iface, 0, sizeof(s->in_iface));
    for (uint32_t i = 0; i < 65536; ++i)
    {
        s->binding[i] = 0xffffffffu;
        s->set[i] = 0xffffffffu;
        s->var_type[i] = 0;
        s->var_storage[i] = 0xffffffffu;
        if (s->names[i])
        {
            free(s->names[i]);
            s->names[i] = NULL;
        }
    }
    m->entry[0] = 0;
    m->ubo_name[0] = 0;
    m->local[0] = m->local[1] = m->local[2] = 1;

    uint32_t at = 5;
    while (at + 1 <= words)
    {
        uint32_t word = rd32(code + at * 4);
        uint32_t count = word >> 16, op = word & 0xffff;
        if (!count || at + count > words)
            return -1;
        switch (op)
        {
        case 5: /* OpName */
        {
            uint32_t target = rd32(code + (at + 1) * 4);
            if (count >= 3 && target < 65536)
                s->names[target] = mlfi_strdup((const char*)(code + (at + 2) * 4));
            break;
        }
        case 15: /* OpEntryPoint */
            if (count >= 4 && !m->entry[0])
            {
                uint32_t model = rd32(code + (at + 1) * 4);
                if (model != 5)
                    return -2; /* not GLCompute */
                const char* name = (const char*)(code + (at + 3) * 4);
                snprintf(m->entry, sizeof(m->entry), "%s", name);
                uint32_t name_words = (uint32_t)((strlen(name) + 1 + 3) / 4);
                for (uint32_t k = at + 3 + name_words; k < at + count; ++k)
                    s->in_iface[rd32(code + k * 4) & 0xffff] = 1;
            }
            break;
        case 16: /* OpExecutionMode */
            if (count >= 6 && rd32(code + (at + 2) * 4) == 17)
            {
                m->local[0] = rd32(code + (at + 3) * 4);
                m->local[1] = rd32(code + (at + 4) * 4);
                m->local[2] = rd32(code + (at + 5) * 4);
            }
            break;
        case 71: /* OpDecorate */
        {
            uint32_t target = rd32(code + (at + 1) * 4) & 0xffff;
            uint32_t dec = rd32(code + (at + 2) * 4);
            uint32_t value = count >= 4 ? rd32(code + (at + 3) * 4) : 0;
            if (dec == 33)
                s->binding[target] = value;
            else if (dec == 34)
                s->set[target] = value;
            else if (dec == 2)
                s->block[target] = 2;
            else if (dec == 3)
                s->block[target] = 3;
            break;
        }
        case 21: /* OpTypeInt */
        {
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_INT;
            s->scalar_kind[id] = 0;
            s->scalar_bits[id] = (uint8_t)rd32(code + (at + 2) * 4);
            break;
        }
        case 22: /* OpTypeFloat */
        {
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_FLOAT;
            s->scalar_kind[id] = 1;
            s->scalar_bits[id] = (uint8_t)rd32(code + (at + 2) * 4);
            break;
        }
        case 23: /* OpTypeVector */
            s->kind[rd32(code + (at + 1) * 4) & 0xffff] = K_VECTOR;
            break;
        case 25: /* OpTypeImage */
        {
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_IMAGE;
            s->image_sampled_type[id] = (uint16_t)(rd32(code + (at + 2) * 4) & 0xffff);
            s->image_dim[id] = rd32(code + (at + 3) * 4);
            s->image_sampled[id] = (uint8_t)rd32(code + (at + 7) * 4);
            s->image_format[id] = count >= 9 ? rd32(code + (at + 8) * 4) : 0;
            break;
        }
        case 26: /* OpTypeSampler */
            s->kind[rd32(code + (at + 1) * 4) & 0xffff] = K_SAMPLER;
            break;
        case 30: /* OpTypeStruct */
            s->kind[rd32(code + (at + 1) * 4) & 0xffff] = K_STRUCT;
            break;
        case 32: /* OpTypePointer */
        {
            uint32_t id = rd32(code + (at + 1) * 4) & 0xffff;
            s->kind[id] = K_POINTER;
            s->pointer_storage[id] = (uint8_t)rd32(code + (at + 2) * 4);
            s->pointer_pointee[id] = (uint16_t)(rd32(code + (at + 3) * 4) & 0xffff);
            break;
        }
        case 59: /* OpVariable */
        {
            uint32_t type = rd32(code + (at + 1) * 4) & 0xffff;
            uint32_t id = rd32(code + (at + 2) * 4) & 0xffff;
            s->var_type[id] = type;
            s->var_storage[id] = rd32(code + (at + 3) * 4);
            break;
        }
        default:
            break;
        }
        at += count;
    }

    m->nbind = 0;
    for (uint32_t id = 1; id < 65536; ++id)
    {
        if (!s->var_type[id] || s->binding[id] == 0xffffffffu)
            continue;
        if (m->version >= 0x00010400u && !s->in_iface[id])
            continue;
        uint32_t storage = s->var_storage[id];
        if (storage != 0 && storage != 2 && storage != 12)
            continue;
        uint32_t ptr = s->var_type[id];
        if (s->kind[ptr] != K_POINTER)
            continue;
        uint32_t pointee = s->pointer_pointee[ptr];
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
        if (s->kind[pointee] == K_SAMPLER)
            type = VK_DESCRIPTOR_TYPE_SAMPLER;
        else if (s->kind[pointee] == K_IMAGE)
            type = s->image_sampled[pointee] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        else if (s->kind[pointee] == K_STRUCT)
            type = (storage == 2 && s->block[pointee] == 2) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                           : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if (type == VK_DESCRIPTOR_TYPE_MAX_ENUM)
            continue;
        if (m->nbind >= MLFI_MAX_BINDINGS)
            return -4;
        MlfiBinding* b = &m->binds[m->nbind++];
        memset(b, 0, sizeof(*b));
        b->var_id = id;
        b->set = s->set[id] == 0xffffffffu ? 0 : s->set[id];
        b->binding = s->binding[id];
        b->type = type;
        b->image_format = s->image_format[pointee];
        if (s->names[id])
            snprintf(b->name, sizeof(b->name), "%s", s->names[id]);
        if (type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER && !m->ubo_name[0] && b->name[0])
        {
            size_t n = strlen(b->name);
            if (n >= sizeof(m->ubo_name))
                n = sizeof(m->ubo_name) - 1;
            memcpy(m->ubo_name, b->name, n);
            m->ubo_name[n] = 0;
        }
    }
    for (uint32_t i = 0; i + 1 < m->nbind; ++i)
        for (uint32_t j = i + 1; j < m->nbind; ++j)
            if (m->binds[j].binding < m->binds[i].binding)
            {
                MlfiBinding t = m->binds[i];
                m->binds[i] = m->binds[j];
                m->binds[j] = t;
            }
    return m->entry[0] ? 0 : -3;
}

// ------------------------------------------------------------------ Vulkan entry points ----------

typedef struct
{
#define MLFI_VK_FN(name) PFN_vk##name name;
    MLFI_VK_FN(CreateBuffer)
    MLFI_VK_FN(DestroyBuffer)
    MLFI_VK_FN(GetBufferMemoryRequirements)
    MLFI_VK_FN(AllocateMemory)
    MLFI_VK_FN(FreeMemory)
    MLFI_VK_FN(BindBufferMemory)
    MLFI_VK_FN(MapMemory)
    MLFI_VK_FN(UnmapMemory)
    MLFI_VK_FN(CreateImage)
    MLFI_VK_FN(DestroyImage)
    MLFI_VK_FN(GetImageMemoryRequirements)
    MLFI_VK_FN(BindImageMemory)
    MLFI_VK_FN(CreateImageView)
    MLFI_VK_FN(DestroyImageView)
    MLFI_VK_FN(CreateSampler)
    MLFI_VK_FN(DestroySampler)
    MLFI_VK_FN(CreateShaderModule)
    MLFI_VK_FN(DestroyShaderModule)
    MLFI_VK_FN(CreateDescriptorSetLayout)
    MLFI_VK_FN(DestroyDescriptorSetLayout)
    MLFI_VK_FN(CreatePipelineLayout)
    MLFI_VK_FN(DestroyPipelineLayout)
    MLFI_VK_FN(CreateComputePipelines)
    MLFI_VK_FN(CreateGraphicsPipelines)
    MLFI_VK_FN(DestroyPipeline)
    MLFI_VK_FN(CreateDescriptorPool)
    MLFI_VK_FN(DestroyDescriptorPool)
    MLFI_VK_FN(AllocateDescriptorSets)
    MLFI_VK_FN(UpdateDescriptorSets)
    MLFI_VK_FN(CreateRenderPass)
    MLFI_VK_FN(DestroyRenderPass)
    MLFI_VK_FN(CreateFramebuffer)
    MLFI_VK_FN(DestroyFramebuffer)
    MLFI_VK_FN(CmdPipelineBarrier)
    MLFI_VK_FN(CmdClearColorImage)
    MLFI_VK_FN(CmdCopyBuffer)
    MLFI_VK_FN(CmdFillBuffer)
    MLFI_VK_FN(CmdCopyImage)
    MLFI_VK_FN(CmdBindPipeline)
    MLFI_VK_FN(CmdBindDescriptorSets)
    MLFI_VK_FN(CmdDispatch)
    MLFI_VK_FN(CmdPushConstants)
    MLFI_VK_FN(CmdBeginRenderPass)
    MLFI_VK_FN(CmdEndRenderPass)
    MLFI_VK_FN(CmdSetViewport)
    MLFI_VK_FN(CmdSetScissor)
    MLFI_VK_FN(CmdDraw)
#undef MLFI_VK_FN
} MlfiVk;

typedef struct
{
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
    VkFormat format;
    uint32_t w, h;
    VkImageAspectFlags aspect;
    int is_depth;
    const char* name;
} MlfiImage;

typedef struct
{
    MlfiModule* module;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipe_layout;
    VkPipeline pipeline;
    VkDescriptorSet set;
} MlfiPipe;

typedef struct
{
    const char* name;
    VkImageView sampled_view;
    VkImageView storage_view;
} MlfiResMap;

typedef struct
{
    VkImage image;
    VkFormat format;
    VkImageView view;
} MlfiAppView;

typedef struct
{
    VkFormat format;
    VkRenderPass pass;
    VkPipeline pipeline;
} MlfiComposePass;

typedef struct
{
    VkImageView view;
    uint32_t w, h;
    VkFramebuffer framebuffer;
} MlfiComposeTarget;

struct MlfiContext
{
    uint32_t magic;
    ffxAllocationCallbacks mem;
    ffxApiMessage message;
    uint32_t debug_level;

    VkDevice dev;
    VkPhysicalDevice phys;
    PFN_vkGetDeviceProcAddr gdpa;
    MlfiVk vk;

    uint32_t display_w, display_h;
    uint32_t render_w, render_h;
    uint32_t backbuffer_format;
    uint32_t create_flags;

    // ffxConfigureDescFrameGeneration, stored for the swapchain hand-off.  The callback is not
    // invoked by this milestone (the application calls ffxDispatch itself).
    void* swapchain;
    FfxApiPresentCallbackFunc present_callback;
    void* present_callback_user;
    FfxApiFrameGenerationDispatchFunc frame_callback;
    void* frame_callback_user;
    int frame_gen_enabled;
    int allow_async_workloads;
    int only_present_generated;
    uint32_t configure_flags;
    struct FfxApiResource hudless_color;
    struct FfxApiRect2D generation_rect;
    uint64_t last_configure_frame_id;
    int have_configure_frame_id;
    int reset_pending;

    char modules_dir[4096];
    char model_path[4096];

    MlfiModule modules[MLFI_MAX_MODULES];
    int module_count;

    MlfiPipe pipes[MLFI_MAX_PIPES];
    int pipe_count;
    int bind_counts[512];

    MlfiPipe prepare_dispatch[MLFI_PREPARE_ROWS];
    MlfiModule* prepare_modules[MLFI_PREPARE_ROWS];

    VkBuffer model, scratch, arena, staging_model;
    VkDeviceMemory model_mem, scratch_mem, arena_mem, staging_model_mem;
    void* arena_map;
    void* staging_map;

    MlfiImage images[MLFI_MAX_IMAGES];
    int image_count;
    MlfiImage* img_rgb0;
    MlfiImage* img_rgb1;
    MlfiImage* img_depth;
    MlfiImage* img_mv;
    MlfiImage* img_prediction;

    VkImageView splat_storage_view[2];
    VkImageView splat_sampled_view[2];

    MlfiResMap res[MLFI_MAX_APPS];
    int res_count;

    VkSampler sampler_linear, sampler_nearest;

    VkDescriptorPool descriptor_pool;

    // Three input-copy pipelines (colour RGBA8, motion vectors RG16F, depth R32F) and the compose
    // pipeline.  These are ours, not AMD's (see the shader headers).
    VkDescriptorSetLayout copy_set_layout[3];
    VkPipelineLayout copy_pipe_layout[3];
    VkPipeline copy_pipeline[3];
    VkDescriptorSet copy_set[3];

    VkDescriptorSetLayout compose_set_layout;
    VkPipelineLayout compose_pipe_layout;
    VkDescriptorSet compose_set;
    VkImageView prediction_sampled_view;

    MlfiComposePass compose_passes[8];
    int compose_pass_count;
    MlfiComposeTarget compose_targets[8];
    int compose_target_count;

    MlfiAppView app_views[MLFI_MAX_APPS];
    int app_view_count;

    int images_initialized;
    int model_uploaded;
    int first_dispatch;
    int prepared;
    int prepared_reset;
    uint64_t prepared_frame_id;

    int have_prev_vp, have_prev2_vp;
    float prev_vp[16], prev2_vp[16];

    uint64_t frames;
    uint32_t resets;
    uint32_t last_prepare_dispatches;
    uint32_t last_copy_dispatches;
    uint32_t last_model_dispatches;
    uint32_t last_compose_dispatches;
    ffxReturnCode_t last_create_rc;
    ffxReturnCode_t last_prepare_rc;
    ffxReturnCode_t last_frame_rc;
    int warned_missing_camera;
    int warned_reset_after_prepare;
};

static void* mlfi_host_alloc(MlfiContext* ctx, size_t size)
{
    if (ctx && ctx->mem.alloc)
        return ctx->mem.alloc(ctx->mem.pUserData, size);
    return malloc(size);
}

static void mlfi_host_free(MlfiContext* ctx, void* pointer)
{
    if (!pointer)
        return;
    if (ctx && ctx->mem.dealloc)
        ctx->mem.dealloc(ctx->mem.pUserData, pointer);
    else
        free(pointer);
}


static const char* mlfi_vk_result_name(VkResult r)
{
    switch (r)
    {
    case VK_SUCCESS: return "VK_SUCCESS";
    case VK_NOT_READY: return "VK_NOT_READY";
    case VK_TIMEOUT: return "VK_TIMEOUT";
    case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
    case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
    case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
    case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
    case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
    case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
    default: return "VkResult";
    }
}

static void mlfi_log(MlfiContext* ctx, const char* level, const char* fmt, va_list ap)
{
    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    fprintf(stderr, "[mlfi-provider] %s%s\n", level, buffer);
    if (ctx && ctx->message)
    {
        wchar_t wide[2048];
        size_t n = mbstowcs(wide, buffer, sizeof(wide) / sizeof(wide[0]) - 1);
        if (n != (size_t)-1)
        {
            wide[n] = 0;
            ctx->message(strcmp(level, "error: ") == 0 ? FFX_API_MESSAGE_TYPE_ERROR
                                                       : FFX_API_MESSAGE_TYPE_WARNING,
                         wide);
        }
    }
}

static void mlfi_info(MlfiContext* ctx, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mlfi_log(ctx, "", fmt, ap);
    va_end(ap);
}

static void mlfi_error(MlfiContext* ctx, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mlfi_log(ctx, "error: ", fmt, ap);
    va_end(ap);
}

static void mlfi_verbose(MlfiContext* ctx, const char* fmt, ...)
{
    if (!ctx || ctx->debug_level < FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS)
        return;
    va_list ap;
    va_start(ap, fmt);
    mlfi_log(ctx, "debug: ", fmt, ap);
    va_end(ap);
}

// ------------------------------------------------------------------ device setup -----------------

static int mlfi_load_vk(MlfiContext* ctx)
{
    int missing = 0;
#define MLFI_LOAD(name)                                                                     \
    do                                                                                      \
    {                                                                                       \
        ctx->vk.name = (PFN_vk##name)ctx->gdpa(ctx->dev, "vk" #name);                        \
        if (!ctx->vk.name)                                                                  \
        {                                                                                   \
            mlfi_error(ctx, "device entry point vk%s is missing", #name);                    \
            missing++;                                                                      \
        }                                                                                   \
    } while (0)
    MLFI_LOAD(CreateBuffer);
    MLFI_LOAD(DestroyBuffer);
    MLFI_LOAD(GetBufferMemoryRequirements);
    MLFI_LOAD(AllocateMemory);
    MLFI_LOAD(FreeMemory);
    MLFI_LOAD(BindBufferMemory);
    MLFI_LOAD(MapMemory);
    MLFI_LOAD(UnmapMemory);
    MLFI_LOAD(CreateImage);
    MLFI_LOAD(DestroyImage);
    MLFI_LOAD(GetImageMemoryRequirements);
    MLFI_LOAD(BindImageMemory);
    MLFI_LOAD(CreateImageView);
    MLFI_LOAD(DestroyImageView);
    MLFI_LOAD(CreateSampler);
    MLFI_LOAD(DestroySampler);
    MLFI_LOAD(CreateShaderModule);
    MLFI_LOAD(DestroyShaderModule);
    MLFI_LOAD(CreateDescriptorSetLayout);
    MLFI_LOAD(DestroyDescriptorSetLayout);
    MLFI_LOAD(CreatePipelineLayout);
    MLFI_LOAD(DestroyPipelineLayout);
    MLFI_LOAD(CreateComputePipelines);
    MLFI_LOAD(CreateGraphicsPipelines);
    MLFI_LOAD(DestroyPipeline);
    MLFI_LOAD(CreateDescriptorPool);
    MLFI_LOAD(DestroyDescriptorPool);
    MLFI_LOAD(AllocateDescriptorSets);
    MLFI_LOAD(UpdateDescriptorSets);
    MLFI_LOAD(CreateRenderPass);
    MLFI_LOAD(DestroyRenderPass);
    MLFI_LOAD(CreateFramebuffer);
    MLFI_LOAD(DestroyFramebuffer);
    MLFI_LOAD(CmdPipelineBarrier);
    MLFI_LOAD(CmdClearColorImage);
    MLFI_LOAD(CmdCopyBuffer);
    MLFI_LOAD(CmdFillBuffer);
    MLFI_LOAD(CmdCopyImage);
    MLFI_LOAD(CmdBindPipeline);
    MLFI_LOAD(CmdBindDescriptorSets);
    MLFI_LOAD(CmdDispatch);
    MLFI_LOAD(CmdPushConstants);
    MLFI_LOAD(CmdBeginRenderPass);
    MLFI_LOAD(CmdEndRenderPass);
    MLFI_LOAD(CmdSetViewport);
    MLFI_LOAD(CmdSetScissor);
    MLFI_LOAD(CmdDraw);
#undef MLFI_LOAD
    return missing ? -1 : 0;
}

static uint32_t mlfi_find_memory(MlfiContext* ctx, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(ctx->phys, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int mlfi_create_buffer(MlfiContext* ctx, const char* name, VkDeviceSize size,
                              VkBufferUsageFlags usage, VkMemoryPropertyFlags flags,
                              VkBuffer* buffer, VkDeviceMemory* memory, void** map)
{
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult r = ctx->vk.CreateBuffer(ctx->dev, &ci, NULL, buffer);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateBuffer(%s, %llu bytes) failed: %s", name,
                   (unsigned long long)size, mlfi_vk_result_name(r));
        return -1;
    }
    VkMemoryRequirements req;
    ctx->vk.GetBufferMemoryRequirements(ctx->dev, *buffer, &req);
    uint32_t type = mlfi_find_memory(ctx, req.memoryTypeBits, flags);
    if (type == UINT32_MAX)
    {
        mlfi_error(ctx, "no memory type for %s with flags 0x%x", name, flags);
        return -1;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = ctx->vk.AllocateMemory(ctx->dev, &ai, NULL, memory);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkAllocateMemory(%s) failed: %s", name, mlfi_vk_result_name(r));
        return -1;
    }
    r = ctx->vk.BindBufferMemory(ctx->dev, *buffer, *memory, 0);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkBindBufferMemory(%s) failed: %s", name, mlfi_vk_result_name(r));
        return -1;
    }
    if (map)
    {
        r = ctx->vk.MapMemory(ctx->dev, *memory, 0, VK_WHOLE_SIZE, 0, map);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "vkMapMemory(%s) failed: %s", name, mlfi_vk_result_name(r));
            return -1;
        }
        memset(*map, 0, (size_t)size);
    }
    mlfi_verbose(ctx, "buffer %-12s %9llu bytes", name, (unsigned long long)size);
    return 0;
}

static MlfiImage* mlfi_create_image(MlfiContext* ctx, const char* name, uint32_t w, uint32_t h,
                                    VkFormat format, VkImageUsageFlags usage,
                                    VkImageCreateFlags flags, int is_depth)
{
    if (ctx->image_count >= MLFI_MAX_IMAGES)
    {
        mlfi_error(ctx, "too many provider images");
        return NULL;
    }
    MlfiImage* img = &ctx->images[ctx->image_count++];
    memset(img, 0, sizeof(*img));
    img->name = name;
    img->format = format;
    img->w = w;
    img->h = h;
    img->is_depth = is_depth;
    img->aspect = is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.flags = flags;
    VkResult r = ctx->vk.CreateImage(ctx->dev, &ci, NULL, &img->image);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateImage(%s %ux%u fmt %d) failed: %s", name, w, h, (int)format,
                   mlfi_vk_result_name(r));
        return NULL;
    }
    VkMemoryRequirements req;
    ctx->vk.GetImageMemoryRequirements(ctx->dev, img->image, &req);
    uint32_t type = mlfi_find_memory(ctx, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX)
    {
        mlfi_error(ctx, "no device-local memory for image %s", name);
        return NULL;
    }
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    r = ctx->vk.AllocateMemory(ctx->dev, &ai, NULL, &img->memory);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkAllocateMemory(%s) failed: %s", name, mlfi_vk_result_name(r));
        return NULL;
    }
    r = ctx->vk.BindImageMemory(ctx->dev, img->image, img->memory, 0);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkBindImageMemory(%s) failed: %s", name, mlfi_vk_result_name(r));
        return NULL;
    }
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = img->aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    r = ctx->vk.CreateImageView(ctx->dev, &vi, NULL, &img->view);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateImageView(%s) failed: %s", name, mlfi_vk_result_name(r));
        return NULL;
    }
    mlfi_verbose(ctx, "image  %-14s %4ux%-4u fmt %-2d mem %llu bytes", name, w, h, (int)format,
                 (unsigned long long)req.size);
    return img;
}

static VkImageView mlfi_create_view_usage(MlfiContext* ctx, MlfiImage* img, VkFormat format,
                                          VkImageUsageFlags usage)
{
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkImageViewUsageCreateInfo vu = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    vu.usage = usage;
    vi.pNext = &vu;
    vi.image = img->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = img->aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    VkResult r = ctx->vk.CreateImageView(ctx->dev, &vi, NULL, &view);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateImageView(%s, fmt %d) failed: %s", img->name, (int)format,
                   mlfi_vk_result_name(r));
        return VK_NULL_HANDLE;
    }
    return view;
}

// ------------------------------------------------------------------ modules and pipelines ---------

static MlfiModule* mlfi_module_by_entry(MlfiContext* ctx, const char* entry)
{
    for (int i = 0; i < ctx->module_count; ++i)
        if (!strcmp(ctx->modules[i].entry, entry))
            return &ctx->modules[i];
    return NULL;
}

static MlfiModule* mlfi_module_by_ubo(MlfiContext* ctx, const char* ubo)
{
    for (int i = 0; i < ctx->module_count; ++i)
        if (!strcmp(ctx->modules[i].ubo_name, ubo))
            return &ctx->modules[i];
    return NULL;
}

static int mlfi_load_modules(MlfiContext* ctx)
{
    DIR* dir = opendir(ctx->modules_dir);
    if (!dir)
    {
        mlfi_error(ctx, "cannot open module directory %s: %s", ctx->modules_dir, strerror(errno));
        return -1;
    }
    struct dirent* entry;
    int model_modules = 0, mains = 0;
    while ((entry = readdir(dir)))
    {
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcmp(name + len - 4, ".spv"))
            continue;
        if (!strncmp(name, "mlfi_8k_", 8))
            continue;  // this milestone implements the captured 4K (1280x720) set only
        if (ctx->module_count >= MLFI_MAX_MODULES)
        {
            mlfi_error(ctx, "more than %d modules in %s", MLFI_MAX_MODULES, ctx->modules_dir);
            closedir(dir);
            return -1;
        }
        MlfiModule* m = &ctx->modules[ctx->module_count];
        memset(m, 0, sizeof(*m));
        size_t dl = strlen(ctx->modules_dir);
        if (dl + len + 2 > sizeof(m->path))
        {
            mlfi_error(ctx, "module path too long: %s/%s", ctx->modules_dir, name);
            closedir(dir);
            return -1;
        }
        memcpy(m->path, ctx->modules_dir, dl);
        m->path[dl] = '/';
        memcpy(m->path + dl + 1, name, len + 1);
        m->code = mlfi_read_file(ctx, m->path, &m->size);
        if (!m->code)
        {
            mlfi_error(ctx, "cannot read %s: %s", m->path, strerror(errno));
            closedir(dir);
            return -1;
        }
        if (mlfi_spv_scan(m->code, m->size, m) != 0)
        {
            mlfi_error(ctx, "%s: not a GLCompute SPIR-V module (run tools/mlfi-replay/prepare.py)",
                       m->path);
            closedir(dir);
            return -1;
        }
        if (!strcmp(m->entry, "main"))
            mains++;
        else if (!strncmp(m->entry, "mlfi_4k_pass", 12))
            model_modules++;
        ctx->module_count++;
    }
    closedir(dir);
    if (model_modules != 191)
    {
        mlfi_error(ctx, "%s has %d mlfi_4k modules, expected 191", ctx->modules_dir, model_modules);
        return -1;
    }
    if (mains != 4)
    {
        mlfi_error(ctx, "%s has %d preprocess 'main' modules, expected 4", ctx->modules_dir, mains);
        return -1;
    }
    mlfi_info(ctx, "modules: %d (191 model + 4 preprocess)", ctx->module_count);
    return 0;
}

static MlfiPipe* mlfi_create_pipe(MlfiContext* ctx, MlfiModule* m)
{
    if (ctx->pipe_count >= MLFI_MAX_PIPES)
    {
        mlfi_error(ctx, "too many pipelines");
        return NULL;
    }
    MlfiPipe* p = &ctx->pipes[ctx->pipe_count++];
    memset(p, 0, sizeof(*p));
    p->module = m;

    VkDescriptorSetLayoutBinding bindings[MLFI_MAX_BINDINGS];
    for (uint32_t i = 0; i < m->nbind; ++i)
    {
        bindings[i].binding = m->binds[i].binding;
        bindings[i].descriptorType = m->binds[i].type;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[i].pImmutableSamplers = NULL;
    }
    VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = m->nbind;
    sl.pBindings = bindings;
    VkResult r = ctx->vk.CreateDescriptorSetLayout(ctx->dev, &sl, NULL, &p->set_layout);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "%s: vkCreateDescriptorSetLayout failed: %s", m->entry,
                   mlfi_vk_result_name(r));
        return NULL;
    }
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p->set_layout;
    r = ctx->vk.CreatePipelineLayout(ctx->dev, &pl, NULL, &p->pipe_layout);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "%s: vkCreatePipelineLayout failed: %s", m->entry, mlfi_vk_result_name(r));
        return NULL;
    }
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = m->size;
    sm.pCode = (const uint32_t*)m->code;
    VkShaderModule module = VK_NULL_HANDLE;
    r = ctx->vk.CreateShaderModule(ctx->dev, &sm, NULL, &module);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "%s: vkCreateShaderModule failed: %s", m->entry, mlfi_vk_result_name(r));
        return NULL;
    }
    // Every MLFI pipeline requests wave32.  The capture did not record the pipeline create-info
    // pNext; the 78 cooperative-matrix modules use LocalSize 32x1x1, which is only valid at wave32
    // (docs/en/reports/MLFI-REPLAY.md 4).
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup = {
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    subgroup.requiredSubgroupSize = 32;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = m->entry;
    ci.stage.pNext = &subgroup;
    ci.layout = p->pipe_layout;
    r = ctx->vk.CreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipeline);
    ctx->vk.DestroyShaderModule(ctx->dev, module, NULL);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "%s: vkCreateComputePipelines failed: %s", m->entry,
                   mlfi_vk_result_name(r));
        return NULL;
    }
    mlfi_verbose(ctx, "pipeline %-22s local %ux%ux%u bindings %u", m->entry, m->local[0], m->local[1],
                 m->local[2], m->nbind);
    return p;
}

// ------------------------------------------------------------------ descriptors -------------------

static void mlfi_res_image(MlfiContext* ctx, const char* name, VkImageView sampled, VkImageView storage)
{
    if (ctx->res_count >= MLFI_MAX_APPS)
    {
        mlfi_error(ctx, "too many resource-map entries");
        return;
    }
    ctx->res[ctx->res_count].name = name;
    ctx->res[ctx->res_count].sampled_view = sampled;
    ctx->res[ctx->res_count].storage_view = storage;
    ctx->res_count++;
}

static const MlfiResMap* mlfi_res_find(MlfiContext* ctx, const char* name)
{
    for (int i = 0; i < ctx->res_count; ++i)
        if (!strcmp(ctx->res[i].name, name))
            return &ctx->res[i];
    return NULL;
}

// Writes one descriptor set from the module's own bindings, the captured arena offsets and the
// provider's images.  `prepare_index` is -1 for model passes.
static int mlfi_write_pass_descriptors(MlfiContext* ctx, MlfiPipe* p, const MlfiRow* row,
                                       int prepare_index)
{
    VkDescriptorBufferInfo binfo[MLFI_MAX_BINDINGS];
    VkDescriptorImageInfo iinfo[MLFI_MAX_BINDINGS];
    VkWriteDescriptorSet writes[MLFI_MAX_BINDINGS];
    uint32_t nw = 0;
    memset(binfo, 0, sizeof(binfo));
    memset(iinfo, 0, sizeof(iinfo));

    for (uint32_t i = 0; i < p->module->nbind; ++i)
    {
        MlfiBinding* b = &p->module->binds[i];
        VkWriteDescriptorSet* w = &writes[nw++];
        memset(w, 0, sizeof(*w));
        w->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w->dstSet = p->set;
        w->dstBinding = b->binding;
        w->descriptorCount = 1;
        w->descriptorType = b->type;

        if (b->type == VK_DESCRIPTOR_TYPE_SAMPLER)
        {
            iinfo[i].sampler = strstr(b->name, "nearest") ? ctx->sampler_nearest : ctx->sampler_linear;
            w->pImageInfo = &iinfo[i];
            continue;
        }
        if (b->type == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
        {
            if (!strcmp(b->name, "rw_splatted"))
            {
                iinfo[i].imageView = ctx->splat_storage_view[prepare_index == 3 ? 1 : 0];
                iinfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                w->pImageInfo = &iinfo[i];
                continue;
            }
            const MlfiResMap* r = mlfi_res_find(ctx, b->name);
            if (!r)
            {
                mlfi_error(ctx, "%s: binding %u (%s) has no provider resource", p->module->entry,
                           b->binding, b->name);
                return -1;
            }
            iinfo[i].imageView =
                b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? r->storage_view : r->sampled_view;
            if (!iinfo[i].imageView)
            {
                mlfi_error(ctx, "%s: binding %u (%s) has no %s view", p->module->entry, b->binding,
                           b->name, b->type == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? "storage" : "sampled");
                return -1;
            }
            iinfo[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            w->pImageInfo = &iinfo[i];
            continue;
        }

        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0, range = VK_WHOLE_SIZE;
        if (!strcmp(b->name, "InitializerBuffer"))
        {
            buffer = ctx->model;
        }
        else if (!strcmp(b->name, "ScratchBuffer"))
        {
            buffer = ctx->scratch;
        }
        else if (!strcmp(b->name, "CsTensorSizes"))
        {
            if (!row->tensor_offset)
            {
                mlfi_error(ctx, "%s: binds CsTensorSizes but the capture has no tensor offset",
                           p->module->entry);
                return -1;
            }
            buffer = ctx->arena;
            offset = row->tensor_offset;
            range = MLFI_TENSOR_TABLE_SIZE;
        }
        else if (b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        {
            if (prepare_index >= 0)
            {
                buffer = ctx->arena;
                offset = row->constants_offset;
                range = g_prepare_sizes[prepare_index];
            }
            else
            {
                if (!row->constants_offset)
                {
                    mlfi_error(ctx, "%s: binds %s but the capture has no constants offset",
                               p->module->entry, b->name);
                    return -1;
                }
                buffer = ctx->arena;
                offset = row->constants_offset;
                range = 80;
            }
        }
        else
        {
            mlfi_error(ctx, "%s: binding %u (%s) is not a known buffer", p->module->entry,
                       b->binding, b->name);
            return -1;
        }
        binfo[i].buffer = buffer;
        binfo[i].offset = offset;
        binfo[i].range = range;
        w->pBufferInfo = &binfo[i];
    }
    ctx->vk.UpdateDescriptorSets(ctx->dev, nw, writes, 0, NULL);
    return 0;
}

// ------------------------------------------------------------------ provider resources ------------

static int mlfi_create_resources(MlfiContext* ctx)
{
    const uint32_t w = ctx->render_w, h = ctx->render_h;
    const uint32_t sw = w / 3, sh = h / 3;  // 426x240 at 720p, the captured constant
    const uint32_t fw = w / 8, fh = h / 8;  // 160x90, tensor entries 70/71

    if (mlfi_create_buffer(ctx, "model", MLFI_MODEL_SIZE,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->model, &ctx->model_mem, NULL) != 0)
        return -1;
    if (mlfi_create_buffer(ctx, "scratch", MLFI_SCRATCH_SIZE,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &ctx->scratch, &ctx->scratch_mem, NULL) != 0)
        return -1;
    if (mlfi_create_buffer(ctx, "arena", MLFI_ARENA_SIZE,
                           VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &ctx->arena, &ctx->arena_mem, &ctx->arena_map) != 0)
        return -1;
    if (mlfi_create_buffer(ctx, "model-stage", MLFI_MODEL_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                           &ctx->staging_model, &ctx->staging_model_mem, &ctx->staging_map) != 0)
        return -1;

    MlfiImage* rgb0 = mlfi_create_image(ctx, "rgb0", w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0, 0);
    MlfiImage* rgb1 = mlfi_create_image(ctx, "rgb1", w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                            VK_IMAGE_USAGE_STORAGE_BIT,
                                        0, 0);
    MlfiImage* depth = mlfi_create_image(ctx, "depth", w, h, VK_FORMAT_R32_SFLOAT,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* mv = mlfi_create_image(ctx, "mv", w, h, VK_FORMAT_R16G16_SFLOAT,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* distortion = mlfi_create_image(ctx, "distortion", w, h, VK_FORMAT_R16G16_SFLOAT,
                                              VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    MlfiImage* dilated = mlfi_create_image(ctx, "dilated_mv", sw, sh, VK_FORMAT_R16G16_SFLOAT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* dsdepth = mlfi_create_image(ctx, "downscaled_depth", sw, sh, VK_FORMAT_R32_SFLOAT,
                                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* fwdmv = mlfi_create_image(ctx, "fwd_mv", sw, sh, VK_FORMAT_R16G16_SFLOAT,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* splat0 = mlfi_create_image(ctx, "splat0to05", sw, sh, VK_FORMAT_R32G32_UINT,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, 0);
    MlfiImage* splat1 = mlfi_create_image(ctx, "splat1to05", sw, sh, VK_FORMAT_R32G32_UINT,
                                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                          VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, 0);
    MlfiImage* flow0 = mlfi_create_image(ctx, "flow0to05", fw, fh, VK_FORMAT_R16G16_SFLOAT,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* flow1 = mlfi_create_image(ctx, "flow1to05", fw, fh, VK_FORMAT_R16G16_SFLOAT,
                                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, 0, 0);
    MlfiImage* hudless = mlfi_create_image(ctx, "hudless_ui", w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                           VK_IMAGE_USAGE_SAMPLED_BIT, 0, 0);
    MlfiImage* prediction = mlfi_create_image(ctx, "prediction", w, h, VK_FORMAT_R32G32B32A32_SFLOAT,
                                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                              0, 0);
    if (!rgb0 || !rgb1 || !depth || !mv || !distortion || !dilated || !dsdepth || !fwdmv ||
        !splat0 || !splat1 || !flow0 || !flow1 || !hudless || !prediction)
        return -1;
    ctx->img_rgb0 = rgb0;
    ctx->img_rgb1 = rgb1;
    ctx->img_depth = depth;
    ctx->img_mv = mv;
    ctx->img_prediction = prediction;

    ctx->splat_storage_view[0] = mlfi_create_view_usage(ctx, splat0, VK_FORMAT_R64_UINT,
                                                        VK_IMAGE_USAGE_STORAGE_BIT);
    ctx->splat_storage_view[1] = mlfi_create_view_usage(ctx, splat1, VK_FORMAT_R64_UINT,
                                                        VK_IMAGE_USAGE_STORAGE_BIT);
    ctx->splat_sampled_view[0] = splat0->view;
    ctx->splat_sampled_view[1] = splat1->view;
    if (!ctx->splat_storage_view[0] || !ctx->splat_storage_view[1])
        return -1;

    mlfi_res_image(ctx, "r_input_rgb0", rgb0->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_input_rgb1", rgb1->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_input_mv", mv->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_input_depth", depth->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_distortion_field", distortion->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "rw_output_dilated_mv", dilated->view, dilated->view);
    mlfi_res_image(ctx, "rw_output_downscaled_depth", dsdepth->view, dsdepth->view);
    mlfi_res_image(ctx, "r_input_flow", dilated->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_depth", dsdepth->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "rw_splatted", ctx->splat_sampled_view[0], ctx->splat_storage_view[0]);
    mlfi_res_image(ctx, "r_input_mv0", dilated->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_input_depth0", dsdepth->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_packed_splatted1to0", VK_NULL_HANDLE, ctx->splat_storage_view[1]);
    mlfi_res_image(ctx, "rw_output_mv_0_to_1", fwdmv->view, fwdmv->view);
    mlfi_res_image(ctx, "rw_output_flow_0to05", flow0->view, flow0->view);
    mlfi_res_image(ctx, "rw_output_flow_1to05", flow1->view, flow1->view);
    mlfi_res_image(ctx, "r_input_packed_splatted0to05", ctx->splat_sampled_view[0], VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_input_packed_splatted1to05", ctx->splat_sampled_view[1], VK_NULL_HANDLE);
    mlfi_res_image(ctx, "r_hudless_ui", hudless->view, VK_NULL_HANDLE);
    mlfi_res_image(ctx, "rw_prediction", VK_NULL_HANDLE, prediction->view);
    mlfi_res_image(ctx, "rw_target", VK_NULL_HANDLE, prediction->view);

    VkSamplerCreateInfo smci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smci.magFilter = VK_FILTER_LINEAR;
    smci.minFilter = VK_FILTER_LINEAR;
    smci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    smci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkResult r = ctx->vk.CreateSampler(ctx->dev, &smci, NULL, &ctx->sampler_linear);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateSampler(linear) failed: %s", mlfi_vk_result_name(r));
        return -1;
    }
    smci.magFilter = smci.minFilter = VK_FILTER_NEAREST;
    r = ctx->vk.CreateSampler(ctx->dev, &smci, NULL, &ctx->sampler_nearest);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateSampler(nearest) failed: %s", mlfi_vk_result_name(r));
        return -1;
    }

    ctx->prediction_sampled_view = prediction->view;
    return 0;
}

// ------------------------------------------------------------------ arena and per-frame blocks ---

static void mlfi_put_u32(uint8_t* block, uint32_t index, uint32_t value)
{
    memcpy(block + index * 4, &value, sizeof(value));
}

static void mlfi_put_f32(uint8_t* block, uint32_t index, float value)
{
    memcpy(block + index * 4, &value, sizeof(value));
}

static void mlfi_mat4_mul(const float a[16], const float b[16], float out[16])
{
    // Column-major a * b.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
        {
            double sum = 0.0;
            for (int k = 0; k < 4; ++k)
                sum += (double)a[k * 4 + r] * (double)b[c * 4 + k];
            out[c * 4 + r] = (float)sum;
        }
}

static int mlfi_mat4_inv(const float m[16], float out[16])
{
    // Column-major Gauss-Jordan inverse in double precision.
    double a[4][8];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
        {
            a[r][c] = m[c * 4 + r];
            a[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    for (int col = 0; col < 4; ++col)
    {
        int pivot = col;
        for (int r = col + 1; r < 4; ++r)
            if (fabs(a[r][col]) > fabs(a[pivot][col]))
                pivot = r;
        if (fabs(a[pivot][col]) < 1e-12)
            return -1;
        if (pivot != col)
            for (int k = 0; k < 8; ++k)
            {
                double t = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = t;
            }
        double d = a[col][col];
        for (int k = 0; k < 8; ++k)
            a[col][k] /= d;
        for (int r = 0; r < 4; ++r)
        {
            if (r == col)
                continue;
            double f = a[r][col];
            if (f == 0.0)
                continue;
            for (int k = 0; k < 8; ++k)
                a[r][k] -= f * a[col][k];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[c * 4 + r] = (float)a[r][c + 4];
    return 0;
}

// Builds the view-projection matrix used by the vendor's forward-MV constants, in the convention
// the capture proves: Vulkan depth range [0,1], right-handed, row-vector look-at, column-major
// storage.  With the capture's synthetic camera (position 0,0,5; up +Y; right +X; forward -Z;
// near 0.1, far 1000, fov 1.0, 1280x720) this reproduces the captured 144-byte block to ~2e-6
// relative (the vendor's initial "previous" matrix carries a 2 ppm scale we do not reproduce).
static int mlfi_build_view_projection(const float position[3], const float up[3],
                                      const float right[3], const float forward[3],
                                      float fov_y, float near_plane, float far_plane,
                                      float aspect, float out[16])
{
    float r[3] = {right[0], right[1], right[2]};
    float u[3] = {up[0], up[1], up[2]};
    float f[3] = {forward[0], forward[1], forward[2]};
    float nr = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    float nu = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    float nf = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (nr < 1e-6f || nu < 1e-6f || nf < 1e-6f || !(fov_y > 1e-4f) || !(far_plane > near_plane) ||
        !(aspect > 0.0f))
        return -1;
    for (int i = 0; i < 3; ++i)
    {
        r[i] /= nr;
        u[i] /= nu;
        f[i] /= nf;
    }
    float tan_half = tanf(fov_y * 0.5f);
    if (!(tan_half > 1e-6f))
        return -1;

    float p[16] = {0};
    p[0] = 1.0f / (tan_half * aspect);
    p[5] = 1.0f / tan_half;
    // Column-major: index = column * 4 + row.  Vulkan clip space is z in [0,1] and the view is
    // right-handed, so P[2][3] = far*near/(near-far) and P[3][2] = -1.
    p[10] = far_plane / (near_plane - far_plane);
    p[11] = -1.0f;
    p[14] = far_plane * near_plane / (near_plane - far_plane);

    float v[16] = {0};
    v[0] = r[0]; v[1] = u[0]; v[2] = -f[0];
    v[4] = r[1]; v[5] = u[1]; v[6] = -f[1];
    v[8] = r[2]; v[9] = u[2]; v[10] = -f[2];
    v[12] = -(r[0] * position[0] + r[1] * position[1] + r[2] * position[2]);
    v[13] = -(u[0] * position[0] + u[1] * position[1] + u[2] * position[2]);
    v[14] = (f[0] * position[0] + f[1] * position[1] + f[2] * position[2]);
    v[15] = 1.0f;

    mlfi_mat4_mul(p, v, out);
    return 0;
}

static int mlfi_camera_vp(MlfiContext* ctx, const struct ffxDispatchDescFrameGenerationPrepareV2* desc,
                          float out[16])
{
    const float* pos = desc->cameraPosition;
    const float* up = desc->cameraUp;
    const float* right = desc->cameraRight;
    const float* forward = desc->cameraForward;
    double magnitude = 0.0;
    for (int i = 0; i < 3; ++i)
        magnitude += fabs((double)pos[i]) + fabs((double)up[i]) + fabs((double)right[i]) +
                     fabs((double)forward[i]);
    if (magnitude < 1e-6)
    {
        if (!ctx->warned_missing_camera)
        {
            ctx->warned_missing_camera = 1;
            mlfi_info(ctx, "PrepareV2 has no camera basis; forward-MV constants use identity");
        }
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        memcpy(out, identity, sizeof(identity));
        return 0;
    }
    float aspect = (float)ctx->render_w / (float)ctx->render_h;
    return mlfi_build_view_projection(pos, up, right, forward, desc->cameraFovAngleVertical,
                                      desc->cameraNear, desc->cameraFar, aspect, out);
}

static void mlfi_write_prepare_constants(MlfiContext* ctx, const struct ffxDispatchDescFrameGenerationPrepareV2* desc)
{
    uint8_t* arena = (uint8_t*)ctx->arena_map;
    const uint32_t w = ctx->render_w, h = ctx->render_h;
    const uint32_t sw = w / 3, sh = h / 3;
    const float gen_min[2] = {0.0f, 0.0f};
    const float gen_max[2] = {1.0f, 1.0f};

    // Row 0: CsDilate (80 B), layout pinned by the module's member decorations.
    {
        uint8_t* b = arena + g_mlfi_rows[0].constants_offset;
        memset(b, 0, 80);
        mlfi_put_f32(b, 0, -desc->jitterOffset.x);
        mlfi_put_f32(b, 1, -desc->jitterOffset.y);
        mlfi_put_u32(b, 2, w);
        mlfi_put_u32(b, 3, h);
        mlfi_put_u32(b, 4, ctx->display_w);
        mlfi_put_u32(b, 5, ctx->display_h);
        mlfi_put_u32(b, 6, sw);
        mlfi_put_u32(b, 7, sh);
        mlfi_put_f32(b, 8, desc->motionVectorScale.x);
        mlfi_put_f32(b, 9, desc->motionVectorScale.y);
        mlfi_put_f32(b, 10, gen_min[0]);
        mlfi_put_f32(b, 11, gen_min[1]);
        mlfi_put_f32(b, 12, gen_max[0]);
        mlfi_put_f32(b, 13, gen_max[1]);
        mlfi_put_u32(b, 14, 0);  // depthInverted: only flag 0 is accepted at creation
        mlfi_put_u32(b, 15, 0);
        mlfi_put_f32(b, 16, 0.0f);
        mlfi_put_f32(b, 17, 0.0f);
        mlfi_put_f32(b, 18, 1.0f);
        mlfi_put_f32(b, 19, 1.0f);
    }
    // Rows 1, 3, 4: CsAMDSplatting64Constants (64 B).  projAmount/flowIsUv are the captured
    // values: 1.0/0 for the full-scale splat, 0.5/1 and 0.5/0 for the half-scale splats.
    static const float proj_amount[MLFI_PREPARE_ROWS] = {0.0f, 1.0f, 0.0f, 0.5f, 0.5f};
    static const uint32_t flow_is_uv[MLFI_PREPARE_ROWS] = {0, 0, 0, 1, 0};
    for (int i = 1; i < MLFI_PREPARE_ROWS; ++i)
    {
        if (i == 2)
            continue;  // 144-byte forward-MV block, handled below
        uint8_t* b = arena + g_mlfi_rows[i].constants_offset;
        memset(b, 0, 64);
        mlfi_put_u32(b, 0, sw);
        mlfi_put_u32(b, 1, sh);
        mlfi_put_u32(b, 2, w);
        mlfi_put_u32(b, 3, h);
        mlfi_put_f32(b, 4, proj_amount[i]);
        mlfi_put_u32(b, 5, flow_is_uv[i]);
        mlfi_put_f32(b, 6, gen_min[0]);
        mlfi_put_f32(b, 7, gen_min[1]);
        mlfi_put_f32(b, 8, gen_max[0]);
        mlfi_put_f32(b, 9, gen_max[1]);
        mlfi_put_u32(b, 10, 0);
    }
    // Row 2: CsComputeFwdMVUsingMatConstants (144 B): previous projection space -> current, and
    // previous -> previous2, followed by displaySize at offset 128.
    {
        uint8_t* b = arena + g_mlfi_rows[2].constants_offset;
        memset(b, 0, 144);
        float vp[16];
        if (mlfi_camera_vp(ctx, desc, vp) != 0)
        {
            mlfi_error(ctx, "PrepareV2 camera parameters are not a usable projection; using identity");
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            memcpy(vp, identity, sizeof(identity));
        }
        float prev[16], prev2[16], m0[16], m1[16];
        if (ctx->have_prev_vp)
            memcpy(prev, ctx->prev_vp, sizeof(prev));
        else
        {
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            memcpy(prev, identity, sizeof(prev));
        }
        if (ctx->have_prev2_vp)
            memcpy(prev2, ctx->prev2_vp, sizeof(prev2));
        else
        {
            static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
            memcpy(prev2, identity, sizeof(prev2));
        }
        float inv_prev[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        (void)mlfi_mat4_inv(prev, inv_prev);
        mlfi_mat4_mul(inv_prev, vp, m0);
        float inv_prev2[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        (void)mlfi_mat4_inv(prev2, inv_prev2);
        mlfi_mat4_mul(inv_prev2, prev, m1);
        memcpy(b + 0, m0, sizeof(m0));
        memcpy(b + 64, m1, sizeof(m1));
        mlfi_put_u32(b, 32, w);
        mlfi_put_u32(b, 33, h);

        memcpy(ctx->prev2_vp, prev, sizeof(prev));
        memcpy(ctx->prev_vp, vp, sizeof(vp));
        ctx->have_prev_vp = 1;
        ctx->have_prev2_vp = 1;
    }
}

static void mlfi_write_frame_constants(MlfiContext* ctx, const ffxDispatchDescFrameGeneration* desc)
{
    float gen_min[2] = {0.0f, 0.0f}, gen_max[2] = {1.0f, 1.0f};
    if (desc->generationRect.width > 0 && desc->generationRect.height > 0)
    {
        gen_min[0] = (float)desc->generationRect.left / (float)ctx->render_w;
        gen_min[1] = (float)desc->generationRect.top / (float)ctx->render_h;
        gen_max[0] = (float)(desc->generationRect.left + desc->generationRect.width) / (float)ctx->render_w;
        gen_max[1] = (float)(desc->generationRect.top + desc->generationRect.height) / (float)ctx->render_h;
    }
    uint8_t* arena = (uint8_t*)ctx->arena_map;
    int blocks = 0;
    for (uint32_t i = MLFI_PREPARE_ROWS; i < MLFI_GRAPH_ROW_COUNT; ++i)
    {
        const MlfiRow* row = &g_mlfi_rows[i];
        if (!row->constants_offset)
            continue;
        uint8_t* b = arena + row->constants_offset;
        memset(b, 0, 80);
        mlfi_put_u32(b, 0, ctx->render_w);
        mlfi_put_u32(b, 1, ctx->render_h);
        mlfi_put_u32(b, 2, ctx->display_w);
        mlfi_put_u32(b, 3, ctx->display_h);
        mlfi_put_u32(b, 4, (desc->reset || ctx->prepared_reset) ? 1u : 0u);
        mlfi_put_u32(b, 5, desc->backbufferTransferFunction);
        mlfi_put_u32(b, 6, 0);  // hudlessAttachedFactor: no hudless input in this milestone
        mlfi_put_u32(b, 7, 0);  // dispatchFlags: the 4.0.2 dispatch desc has no flags field
        mlfi_put_f32(b, 8, desc->minMaxLuminance[0]);
        mlfi_put_f32(b, 9, desc->minMaxLuminance[1]);
        mlfi_put_f32(b, 10, gen_min[0]);
        mlfi_put_f32(b, 11, gen_min[1]);
        mlfi_put_f32(b, 12, gen_max[0]);
        mlfi_put_f32(b, 13, gen_max[1]);
        mlfi_put_u32(b, 14, 0);  // _pad1
        mlfi_put_f32(b, 16, 0.0f);  // debugBarColor: cosmetic, constant (see doc)
        mlfi_put_f32(b, 17, 1.0f);
        mlfi_put_f32(b, 18, 1.0f);
        blocks++;
    }
    if (blocks != 3)
        mlfi_error(ctx, "expected three model constant blocks, wrote %d", blocks);
}

static void mlfi_write_tensor_tables(MlfiContext* ctx)
{
    uint8_t* arena = (uint8_t*)ctx->arena_map;
    int tables = 0;
    for (uint32_t i = 0; i < MLFI_GRAPH_ROW_COUNT; ++i)
    {
        const MlfiRow* row = &g_mlfi_rows[i];
        if (!row->tensor_offset)
            continue;
        memcpy(arena + row->tensor_offset, g_mlfi_tensor, sizeof(g_mlfi_tensor));
        tables++;
    }
    if (tables != 175)
        mlfi_error(ctx, "expected 175 tensor-table bindings, wrote %d", tables);
}

// ------------------------------------------------------------------ app resources and layouts -----

static VkFormat mlfi_vk_format(uint32_t ffx_format)
{
    switch (ffx_format)
    {
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case FFX_API_SURFACE_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case FFX_API_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R32G32B32A32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkImageLayout mlfi_layout_from_state(uint32_t state)
{
    switch (state)
    {
    case FFX_API_RESOURCE_STATE_COMMON:
    case FFX_API_RESOURCE_STATE_GENERIC_READ:
    case FFX_API_RESOURCE_STATE_UNORDERED_ACCESS:
        return VK_IMAGE_LAYOUT_GENERAL;
    case FFX_API_RESOURCE_STATE_COMPUTE_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_READ:
    case FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ:
        return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_SRC:
        return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case FFX_API_RESOURCE_STATE_COPY_DEST:
        return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case FFX_API_RESOURCE_STATE_PRESENT:
        return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    case FFX_API_RESOURCE_STATE_RENDER_TARGET:
        return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case FFX_API_RESOURCE_STATE_DEPTH_ATTACHMENT:
        return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    default:
        return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

static VkImageView mlfi_create_app_view(MlfiContext* ctx, VkImage image, VkFormat format,
                                        VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (ctx->vk.CreateImageView(ctx->dev, &vi, NULL, &view) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return view;
}

// Views of application images are cached until ffxDestroyContext.  The application owns the
// images and must destroy them after the context (the FFX contract already says the descriptors
// passed to ffxCreateContext outlive the context).
static VkImageView mlfi_app_view(MlfiContext* ctx, VkImage image, VkFormat primary,
                                 VkFormat fallback, VkImageAspectFlags aspect,
                                 VkImageAspectFlags fallback_aspect)
{
    if (!image || primary == VK_FORMAT_UNDEFINED)
        return VK_NULL_HANDLE;
    for (int i = 0; i < ctx->app_view_count; ++i)
        if (ctx->app_views[i].image == image && ctx->app_views[i].format == primary)
            return ctx->app_views[i].view;
    if (ctx->app_view_count >= MLFI_MAX_APPS)
    {
        mlfi_error(ctx, "application view cache is full");
        return VK_NULL_HANDLE;
    }
    VkImageView view = mlfi_create_app_view(ctx, image, primary, aspect);
    VkFormat used = primary;
    if (!view && fallback != VK_FORMAT_UNDEFINED && fallback != primary)
    {
        view = mlfi_create_app_view(ctx, image, fallback, fallback_aspect);
        used = fallback;
    }
    if (!view)
    {
        mlfi_error(ctx, "cannot create a view of an application image (formats %d/%d, aspect 0x%x)",
                   (int)primary, (int)fallback, aspect);
        return VK_NULL_HANDLE;
    }
    ctx->app_views[ctx->app_view_count].image = image;
    ctx->app_views[ctx->app_view_count].format = used;
    ctx->app_views[ctx->app_view_count].view = view;
    ctx->app_view_count++;
    return view;
}

static void mlfi_barrier(MlfiContext* ctx, VkCommandBuffer cmd, VkPipelineStageFlags src_stage,
                         VkPipelineStageFlags dst_stage, VkAccessFlags src_access, VkAccessFlags dst_access)
{
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = src_access;
    mb.dstAccessMask = dst_access;
    ctx->vk.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

// ------------------------------------------------------------------ copy / compose pipelines ------

static int mlfi_create_copy_pipelines(MlfiContext* ctx)
{
    struct
    {
        const uint32_t* code;
        size_t size;
        VkDescriptorType storage_type;
    } shaders[3] = {
        {mlfi_spv_rgba, mlfi_spv_rgba_size, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        {mlfi_spv_rg16f, mlfi_spv_rg16f_size, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
        {mlfi_spv_r32f, mlfi_spv_r32f_size, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
    };
    for (int i = 0; i < 3; ++i)
    {
        VkDescriptorSetLayoutBinding bindings[2];
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[0].pImmutableSamplers = NULL;
        bindings[1].binding = 1;
        bindings[1].descriptorType = shaders[i].storage_type;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].pImmutableSamplers = NULL;
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 2;
        sl.pBindings = bindings;
        VkResult r = ctx->vk.CreateDescriptorSetLayout(ctx->dev, &sl, NULL, &ctx->copy_set_layout[i]);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "copy %d: vkCreateDescriptorSetLayout failed: %s", i, mlfi_vk_result_name(r));
            return -1;
        }
        VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &ctx->copy_set_layout[i];
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        r = ctx->vk.CreatePipelineLayout(ctx->dev, &pl, NULL, &ctx->copy_pipe_layout[i]);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "copy %d: vkCreatePipelineLayout failed: %s", i, mlfi_vk_result_name(r));
            return -1;
        }
        VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = shaders[i].size;
        sm.pCode = shaders[i].code;
        VkShaderModule module = VK_NULL_HANDLE;
        r = ctx->vk.CreateShaderModule(ctx->dev, &sm, NULL, &module);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "copy %d: vkCreateShaderModule failed: %s", i, mlfi_vk_result_name(r));
            return -1;
        }
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = ctx->copy_pipe_layout[i];
        r = ctx->vk.CreateComputePipelines(ctx->dev, VK_NULL_HANDLE, 1, &ci, NULL, &ctx->copy_pipeline[i]);
        ctx->vk.DestroyShaderModule(ctx->dev, module, NULL);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "copy %d: vkCreateComputePipelines failed: %s", i, mlfi_vk_result_name(r));
            return -1;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ctx->descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ctx->copy_set_layout[i];
        r = ctx->vk.AllocateDescriptorSets(ctx->dev, &ai, &ctx->copy_set[i]);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "copy %d: vkAllocateDescriptorSets failed: %s", i, mlfi_vk_result_name(r));
            return -1;
        }
    }

    {
        VkDescriptorSetLayoutBinding binding = {0};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo sl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.bindingCount = 1;
        sl.pBindings = &binding;
        VkResult r = ctx->vk.CreateDescriptorSetLayout(ctx->dev, &sl, NULL, &ctx->compose_set_layout);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "compose: vkCreateDescriptorSetLayout failed: %s", mlfi_vk_result_name(r));
            return -1;
        }
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &ctx->compose_set_layout;
        r = ctx->vk.CreatePipelineLayout(ctx->dev, &pl, NULL, &ctx->compose_pipe_layout);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "compose: vkCreatePipelineLayout failed: %s", mlfi_vk_result_name(r));
            return -1;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ctx->descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ctx->compose_set_layout;
        r = ctx->vk.AllocateDescriptorSets(ctx->dev, &ai, &ctx->compose_set);
        if (r != VK_SUCCESS)
        {
            mlfi_error(ctx, "compose: vkAllocateDescriptorSets failed: %s", mlfi_vk_result_name(r));
            return -1;
        }
        VkDescriptorImageInfo image = {ctx->sampler_nearest, ctx->prediction_sampled_view,
                                       VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = ctx->compose_set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image;
        ctx->vk.UpdateDescriptorSets(ctx->dev, 1, &write, 0, NULL);
    }
    return 0;
}

static MlfiComposePass* mlfi_compose_pass(MlfiContext* ctx, VkFormat format)
{
    for (int i = 0; i < ctx->compose_pass_count; ++i)
        if (ctx->compose_passes[i].format == format)
            return &ctx->compose_passes[i];
    if (ctx->compose_pass_count >= 8)
        return NULL;
    MlfiComposePass* pass = &ctx->compose_passes[ctx->compose_pass_count++];
    memset(pass, 0, sizeof(*pass));
    pass->format = format;

    VkAttachmentDescription attachment = {0};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_GENERAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkAttachmentReference reference = {0, VK_IMAGE_LAYOUT_GENERAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    VkSubpassDependency dependency = {0};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dependency;
    VkResult r = ctx->vk.CreateRenderPass(ctx->dev, &rp, NULL, &pass->pass);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "compose: vkCreateRenderPass fmt %d failed: %s", (int)format,
                   mlfi_vk_result_name(r));
        return NULL;
    }

    VkShaderModuleCreateInfo vsm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    vsm.codeSize = mlfi_spv_compose_vert_size;
    vsm.pCode = mlfi_spv_compose_vert;
    VkShaderModule vert = VK_NULL_HANDLE;
    if (ctx->vk.CreateShaderModule(ctx->dev, &vsm, NULL, &vert) != VK_SUCCESS)
    {
        mlfi_error(ctx, "compose: vertex shader module failed");
        return NULL;
    }
    VkShaderModuleCreateInfo fsm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    fsm.codeSize = mlfi_spv_compose_frag_size;
    fsm.pCode = mlfi_spv_compose_frag;
    VkShaderModule frag = VK_NULL_HANDLE;
    if (ctx->vk.CreateShaderModule(ctx->dev, &fsm, NULL, &frag) != VK_SUCCESS)
    {
        mlfi_error(ctx, "compose: fragment shader module failed");
        ctx->vk.DestroyShaderModule(ctx->dev, vert, NULL);
        return NULL;
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT,
         vert, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
         frag, "main", NULL},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend = {0};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                           VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = ctx->compose_pipe_layout;
    ci.renderPass = pass->pass;
    ci.subpass = 0;
    r = ctx->vk.CreateGraphicsPipelines(ctx->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pass->pipeline);
    ctx->vk.DestroyShaderModule(ctx->dev, vert, NULL);
    ctx->vk.DestroyShaderModule(ctx->dev, frag, NULL);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "compose: vkCreateGraphicsPipelines fmt %d failed: %s", (int)format,
                   mlfi_vk_result_name(r));
        return NULL;
    }
    return pass;
}

static MlfiComposeTarget* mlfi_compose_target(MlfiContext* ctx, VkImageView view, uint32_t w, uint32_t h,
                                              VkRenderPass pass)
{
    for (int i = 0; i < ctx->compose_target_count; ++i)
        if (ctx->compose_targets[i].view == view && ctx->compose_targets[i].w == w &&
            ctx->compose_targets[i].h == h)
            return &ctx->compose_targets[i];
    if (ctx->compose_target_count >= 8)
        return NULL;
    MlfiComposeTarget* target = &ctx->compose_targets[ctx->compose_target_count];
    VkFramebufferCreateInfo fi = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VkResult r = ctx->vk.CreateFramebuffer(ctx->dev, &fi, NULL, &target->framebuffer);
    if (r != VK_SUCCESS)
    {
        mlfi_error(ctx, "compose: vkCreateFramebuffer failed: %s", mlfi_vk_result_name(r));
        return NULL;
    }
    target->view = view;
    target->w = w;
    target->h = h;
    ctx->compose_target_count++;
    return target;
}

// ------------------------------------------------------------------ frame recording ---------------

static void mlfi_reset_sequence(MlfiContext* ctx, VkCommandBuffer cmd)
{
    if (!ctx->images_initialized)
    {
        VkImageMemoryBarrier barriers[MLFI_MAX_IMAGES];
        uint32_t count = 0;
        for (int i = 0; i < ctx->image_count; ++i)
        {
            VkImageMemoryBarrier* b = &barriers[count++];
            memset(b, 0, sizeof(*b));
            b->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b->oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b->newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b->image = ctx->images[i].image;
            b->subresourceRange.aspectMask = ctx->images[i].aspect;
            b->subresourceRange.levelCount = 1;
            b->subresourceRange.layerCount = 1;
            b->dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        }
        ctx->vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL,
                                   count, barriers);
        ctx->images_initialized = 1;
    }
    // A reset frame starts from empty history, exactly like the captured first PrepareV2 after a
    // context reset.  Every provider image is a colour format (the depth image is R32_SFLOAT).
    VkClearColorValue zero;
    memset(&zero, 0, sizeof(zero));
    for (int i = 0; i < ctx->image_count; ++i)
    {
        VkImageSubresourceRange range = {ctx->images[i].aspect, 0, 1, 0, 1};
        ctx->vk.CmdClearColorImage(cmd, ctx->images[i].image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    // The 58 MB scratch buffer is workspace, not state: zero it once so two processes at the same
    // configuration produce the same frame (fresh device allocations are not guaranteed zeroed).
    ctx->vk.CmdFillBuffer(cmd, ctx->scratch, 0, VK_WHOLE_SIZE, 0);
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (!ctx->model_uploaded)
    {
        VkBufferCopy copy = {0, 0, MLFI_MODEL_SIZE};
        ctx->vk.CmdCopyBuffer(cmd, ctx->staging_model, ctx->model, 1, &copy);
        mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        ctx->model_uploaded = 1;
        mlfi_info(ctx, "model uploaded (10,657,152 bytes, fnv1a matches the capture)");
    }
}

static int mlfi_is_2d_size(const FfxApiResource* r, uint32_t w, uint32_t h)
{
    return r->resource && r->description.type == FFX_API_RESOURCE_TYPE_TEXTURE2D &&
           r->description.width == w && r->description.height == h;
}

static void mlfi_update_copy_set(MlfiContext* ctx, int which, VkImageView sampled,
                                 VkImageLayout layout, VkImageView storage)
{
    VkDescriptorImageInfo images[2];
    memset(images, 0, sizeof(images));
    images[0].sampler = ctx->sampler_nearest;
    images[0].imageView = sampled;
    images[0].imageLayout = layout;
    images[1].imageView = storage;
    images[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2];
    memset(writes, 0, sizeof(writes));
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ctx->copy_set[which];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &images[0];
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ctx->copy_set[which];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &images[1];
    ctx->vk.UpdateDescriptorSets(ctx->dev, 2, writes, 0, NULL);
}

static void mlfi_record_copy(MlfiContext* ctx, VkCommandBuffer cmd, int which, uint32_t w, uint32_t h)
{
    ctx->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->copy_pipeline[which]);
    ctx->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->copy_pipe_layout[which], 0,
                                  1, &ctx->copy_set[which], 0, NULL);
    uint32_t size[2] = {w, h};
    ctx->vk.CmdPushConstants(cmd, ctx->copy_pipe_layout[which], VK_SHADER_STAGE_COMPUTE_BIT, 0,
                             sizeof(size), size);
    ctx->vk.CmdDispatch(cmd, (w + 7) / 8, (h + 7) / 8, 1);
}

static ffxReturnCode_t mlfi_prepare(MlfiContext* ctx, const struct ffxDispatchDescFrameGenerationPrepareV2* desc)
{
    if (!desc->commandList)
    {
        mlfi_error(ctx, "PrepareV2: commandList is null");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->renderSize.width != ctx->render_w || desc->renderSize.height != ctx->render_h)
    {
        mlfi_error(ctx, "PrepareV2: renderSize %ux%u does not match the context %ux%u",
                   desc->renderSize.width, desc->renderSize.height, ctx->render_w, ctx->render_h);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (!mlfi_is_2d_size(&desc->depth, ctx->render_w, ctx->render_h) ||
        !mlfi_is_2d_size(&desc->motionVectors, ctx->render_w, ctx->render_h))
    {
        mlfi_error(ctx, "PrepareV2: depth/motion vectors must be %ux%u 2D textures",
                   ctx->render_w, ctx->render_h);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkImageLayout depth_layout = mlfi_layout_from_state(desc->depth.state);
    VkImageLayout mv_layout = mlfi_layout_from_state(desc->motionVectors.state);
    if (desc->depth.description.format != FFX_API_SURFACE_FORMAT_R32_FLOAT)
    {
        mlfi_error(ctx, "PrepareV2: depth format %u is not R32_FLOAT (the captured depth format)",
                   desc->depth.description.format);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->motionVectors.description.format != FFX_API_SURFACE_FORMAT_R16G16_FLOAT)
    {
        mlfi_error(ctx, "PrepareV2: motion-vector format %u is not R16G16_FLOAT",
                   desc->motionVectors.description.format);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (depth_layout == VK_IMAGE_LAYOUT_UNDEFINED || mv_layout == VK_IMAGE_LAYOUT_UNDEFINED)
    {
        mlfi_error(ctx, "PrepareV2: depth/motion vector resource state is not a known FFX state");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkImage depth_image = (VkImage)desc->depth.resource;
    VkImage mv_image = (VkImage)desc->motionVectors.resource;
    // The FFX surface format maps both R32_SFLOAT and D32_SFLOAT to R32_FLOAT, so the depth flag
    // in the description's usage is the only way to know whether the image is a depth format.
    int depth_is_depth_format =
        (desc->depth.description.usage & FFX_API_RESOURCE_USAGE_DEPTHTARGET) != 0;
    VkImageView depth_view = mlfi_app_view(
        ctx, depth_image,
        depth_is_depth_format ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT,
        depth_is_depth_format ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT,
        depth_is_depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT,
        VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView mv_view = mlfi_app_view(ctx, mv_image, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_UNDEFINED,
                                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!depth_view || !mv_view)
        return FFX_API_RETURN_ERROR_PARAMETER;

    VkCommandBuffer cmd = (VkCommandBuffer)desc->commandList;
    int reset = desc->reset || ctx->first_dispatch || ctx->reset_pending;
    if (reset)
    {
        mlfi_reset_sequence(ctx, cmd);
        ctx->resets++;
    }
    ctx->prepared_reset = reset;

    mlfi_write_prepare_constants(ctx, desc);
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT);
    // The application wrote depth, motion vectors and the presented colour before this recording.
    // The declared FFX state is their layout; a global memory barrier orders those writes before
    // the copy shaders without touching a layout the provider does not own.
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

    mlfi_update_copy_set(ctx, 2, depth_view, depth_layout, ctx->img_depth->view);
    mlfi_update_copy_set(ctx, 1, mv_view, mv_layout, ctx->img_mv->view);
    mlfi_record_copy(ctx, cmd, 2, ctx->render_w, ctx->render_h);
    mlfi_record_copy(ctx, cmd, 1, ctx->render_w, ctx->render_h);
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    for (int i = 0; i < MLFI_PREPARE_ROWS; ++i)
    {
        MlfiPipe* p = &ctx->prepare_dispatch[i];
        ctx->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
        ctx->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe_layout, 0, 1, &p->set,
                                      0, NULL);
        const MlfiRow* row = &g_mlfi_rows[i];
        ctx->vk.CmdDispatch(cmd, row->gx, row->gy, row->gz);
        mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    ctx->last_prepare_dispatches = MLFI_PREPARE_ROWS;
    ctx->last_copy_dispatches = 2;
    ctx->prepared = 1;
    ctx->prepared_frame_id = desc->frameID;
    ctx->first_dispatch = 0;
    ctx->reset_pending = 0;
    mlfi_verbose(ctx, "PrepareV2 frame %llu: 2 copies + 5 preprocess dispatches%s",
                 (unsigned long long)desc->frameID, reset ? " (reset)" : "");
    return FFX_API_RETURN_OK;
}

static ffxReturnCode_t mlfi_frame(MlfiContext* ctx, const ffxDispatchDescFrameGeneration* desc)
{
    if (!desc->commandList)
    {
        mlfi_error(ctx, "FrameGeneration: commandList is null");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (!ctx->prepared)
    {
        mlfi_error(ctx, "FrameGeneration: PrepareV2 has not run for this context");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (desc->numGeneratedFrames != 1)
    {
        mlfi_error(ctx, "FrameGeneration: numGeneratedFrames=%u is not implemented (only 1)",
                   desc->numGeneratedFrames);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    if (!mlfi_is_2d_size(&desc->presentColor, ctx->render_w, ctx->render_h) ||
        !mlfi_is_2d_size(&desc->outputs[0], ctx->render_w, ctx->render_h))
    {
        mlfi_error(ctx, "FrameGeneration: presentColor and outputs[0] must be %ux%u 2D textures",
                   ctx->render_w, ctx->render_h);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkFormat color_format = mlfi_vk_format(desc->presentColor.description.format);
    if (color_format != VK_FORMAT_R8G8B8A8_UNORM && color_format != VK_FORMAT_B8G8R8A8_UNORM)
    {
        mlfi_error(ctx, "FrameGeneration: presentColor format %u is not 8-bit UNORM (sRGB/HDR are "
                        "not implemented in this milestone)", desc->presentColor.description.format);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkFormat output_format = mlfi_vk_format(desc->outputs[0].description.format);
    if (output_format != VK_FORMAT_R8G8B8A8_UNORM && output_format != VK_FORMAT_B8G8R8A8_UNORM)
    {
        mlfi_error(ctx, "FrameGeneration: outputs[0] format %u is not 8-bit UNORM",
                   desc->outputs[0].description.format);
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkImageLayout present_layout = mlfi_layout_from_state(desc->presentColor.state);
    VkImageLayout output_layout = mlfi_layout_from_state(desc->outputs[0].state);
    if (present_layout == VK_IMAGE_LAYOUT_UNDEFINED || output_layout != VK_IMAGE_LAYOUT_GENERAL)
    {
        mlfi_error(ctx, "FrameGeneration: presentColor/outputs[0] state is not supported "
                        "(need a read state and UNORDERED_ACCESS/GENERAL)");
        return FFX_API_RETURN_ERROR_PARAMETER;
    }
    VkCommandBuffer cmd = (VkCommandBuffer)desc->commandList;

    int reset = desc->reset || ctx->prepared_reset;
    if (desc->reset && !ctx->prepared_reset && !ctx->warned_reset_after_prepare)
    {
        ctx->warned_reset_after_prepare = 1;
        mlfi_info(ctx, "FrameGeneration carries reset without PrepareV2; preprocess ran on the "
                       "previous history (set reset on PrepareV2 for a clean reset)");
    }
    mlfi_write_frame_constants(ctx, desc);
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_UNIFORM_READ_BIT);

    if (!reset)
    {
        // rgb0 = previous frame, rgb1 = current.  Both are provider images in GENERAL.
        VkImageCopy copy = {0};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent.width = ctx->render_w;
        copy.extent.height = ctx->render_h;
        copy.extent.depth = 1;
        ctx->vk.CmdCopyImage(cmd, ctx->img_rgb1->image, VK_IMAGE_LAYOUT_GENERAL, ctx->img_rgb0->image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    VkImageView present_view = mlfi_app_view(ctx, (VkImage)desc->presentColor.resource, color_format,
                                             VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT,
                                             VK_IMAGE_ASPECT_COLOR_BIT);
    if (!present_view)
        return FFX_API_RETURN_ERROR_PARAMETER;
    mlfi_update_copy_set(ctx, 0, present_view, present_layout, ctx->img_rgb1->view);
    mlfi_record_copy(ctx, cmd, 0, ctx->render_w, ctx->render_h);
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (reset)
    {
        // A reset has no previous present colour.  The capture's first frame binds both colour
        // images, so the provider duplicates the current frame into rgb0.
        VkImageCopy copy = {0};
        copy.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.srcSubresource.layerCount = 1;
        copy.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.dstSubresource.layerCount = 1;
        copy.extent.width = ctx->render_w;
        copy.extent.height = ctx->render_h;
        copy.extent.depth = 1;
        ctx->vk.CmdCopyImage(cmd, ctx->img_rgb1->image, VK_IMAGE_LAYOUT_GENERAL, ctx->img_rgb0->image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    }

    int dispatches = 0;
    for (uint32_t i = MLFI_PREPARE_ROWS; i < MLFI_GRAPH_ROW_COUNT; ++i)
    {
        MlfiPipe* p = &ctx->pipes[i - MLFI_PREPARE_ROWS];
        ctx->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
        ctx->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe_layout, 0, 1, &p->set,
                                      0, NULL);
        const MlfiRow* row = &g_mlfi_rows[i];
        ctx->vk.CmdDispatch(cmd, row->gx, row->gy, row->gz);
        mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                     VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                     VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        dispatches++;
    }
    ctx->last_model_dispatches = (uint32_t)dispatches;

    // Compose: `rw_prediction` -> outputs[0].  This pass is ours, not AMD's (the vendor's final
    // 8,980-byte `main` was never dispatched in the capture).
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                 VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    VkImageView output_view = mlfi_app_view(ctx, (VkImage)desc->outputs[0].resource, output_format,
                                            VK_FORMAT_UNDEFINED, VK_IMAGE_ASPECT_COLOR_BIT,
                                            VK_IMAGE_ASPECT_COLOR_BIT);
    if (!output_view)
        return FFX_API_RETURN_ERROR_PARAMETER;
    MlfiComposePass* pass = mlfi_compose_pass(ctx, output_format);
    if (!pass)
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    MlfiComposeTarget* target = mlfi_compose_target(ctx, output_view, ctx->render_w, ctx->render_h,
                                                    pass->pass);
    if (!target)
        return FFX_API_RETURN_ERROR_RUNTIME_ERROR;
    VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = pass->pass;
    rp.framebuffer = target->framebuffer;
    rp.renderArea.extent.width = ctx->render_w;
    rp.renderArea.extent.height = ctx->render_h;
    ctx->vk.CmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    ctx->vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass->pipeline);
    VkViewport viewport = {0.0f, 0.0f, (float)ctx->render_w, (float)ctx->render_h, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {ctx->render_w, ctx->render_h}};
    ctx->vk.CmdSetViewport(cmd, 0, 1, &viewport);
    ctx->vk.CmdSetScissor(cmd, 0, 1, &scissor);
    ctx->vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ctx->compose_pipe_layout, 0, 1,
                                  &ctx->compose_set, 0, NULL);
    ctx->vk.CmdDraw(cmd, 3, 1, 0, 0);
    ctx->vk.CmdEndRenderPass(cmd);
    // The application (or the swapchain provider) records its own barrier after this callback.
    // Publish the colour-attachment writes to all later commands so that a barrier which expects
    // shader writes (the 3.1.7 swapchain's post-interpolation barrier) still sees a completed frame.
    mlfi_barrier(ctx, cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    ctx->last_compose_dispatches = 1;
    ctx->frames++;
    mlfi_verbose(ctx, "FrameGeneration frame %llu: %d model dispatches + compose -> outputs[0]",
                 (unsigned long long)desc->frameID, dispatches);
    return FFX_API_RETURN_OK;
}

// ------------------------------------------------------------------ device requirements ----------

static int mlfi_has_device_extension(MlfiContext* ctx, const char* name)
{
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(ctx->phys, NULL, &count, NULL) != VK_SUCCESS)
        return 0;
    VkExtensionProperties* props = (VkExtensionProperties*)calloc(count ? count : 1, sizeof(*props));
    if (!props)
        return 0;
    vkEnumerateDeviceExtensionProperties(ctx->phys, NULL, &count, props);
    int found = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (!strcmp(props[i].extensionName, name))
            found = 1;
    free(props);
    return found;
}

static int mlfi_check_device(MlfiContext* ctx)
{
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx->phys, &props);
    mlfi_info(ctx, "device: %s (api %u.%u.%u, driver %u)", props.deviceName,
              VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
              VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion);
    if (props.apiVersion < MLFI_API_VERSION)
    {
        mlfi_error(ctx, "device API %u.%u is below Vulkan 1.3", VK_API_VERSION_MAJOR(props.apiVersion),
                   VK_API_VERSION_MINOR(props.apiVersion));
        return -1;
    }

    VkPhysicalDeviceFeatures core;
    vkGetPhysicalDeviceFeatures(ctx->phys, &core);
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cm = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
    VkPhysicalDeviceCooperativeMatrixMaintenance1FeaturesEXT cm2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_MAINTENANCE_1_FEATURES_EXT};
    VkPhysicalDeviceShaderFloat8FeaturesEXT f8 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT};
    void** tail = &f12.pNext;
    *tail = &cm;
    tail = &cm.pNext;
    *tail = &cm2;
    tail = &cm2.pNext;
    *tail = &f8;
    tail = &f8.pNext;
    *tail = &img64;
    tail = &img64.pNext;
    *tail = &f13;
    VkPhysicalDeviceFeatures2 query = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &f12;
    vkGetPhysicalDeviceFeatures2(ctx->phys, &query);

    if (!core.shaderInt64 || !f12.shaderFloat16 || !f12.shaderInt8)
    {
        mlfi_error(ctx, "device lacks shaderInt64/shaderFloat16/shaderInt8");
        return -1;
    }
    if (!core.shaderStorageImageReadWithoutFormat || !core.shaderStorageImageWriteWithoutFormat)
    {
        mlfi_error(ctx, "device lacks formatless storage-image read/write (pass151/152 need it)");
        return -1;
    }
    if (!cm.cooperativeMatrix || !cm2.cooperativeMatrixConversions ||
        !cm2.cooperativeMatrixPerElementOperations || !cm2.cooperativeMatrixReductions)
    {
        mlfi_error(ctx, "device lacks the cooperative-matrix maintenance1 feature set");
        return -1;
    }
    if (!f8.shaderFloat8CooperativeMatrix)
    {
        mlfi_error(ctx, "device lacks float8 cooperative matrices");
        return -1;
    }
    if (!img64.shaderImageInt64Atomics)
    {
        mlfi_error(ctx, "device lacks shaderImageInt64Atomics (the splat module packs R64 atomics)");
        return -1;
    }
    if (!f12.vulkanMemoryModel)
    {
        mlfi_error(ctx, "device lacks the Vulkan memory model");
        return -1;
    }
    if (!f13.subgroupSizeControl)
    {
        mlfi_error(ctx, "device lacks subgroupSizeControl (the cooperative-matrix modules need wave32)");
        return -1;
    }
    int has_cm2 = mlfi_has_device_extension(ctx, VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME);
    int has_cm1 = mlfi_has_device_extension(ctx, VK_EXT_COOPERATIVE_MATRIX_MAINTENANCE_1_EXTENSION_NAME);
    if (!mlfi_has_device_extension(ctx, VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME) ||
        !(has_cm2 || has_cm1) || !mlfi_has_device_extension(ctx, VK_EXT_SHADER_FLOAT8_EXTENSION_NAME))
    {
        mlfi_error(ctx, "device is missing cooperative_matrix / maintenance1 / shader_float8");
        return -1;
    }
    mlfi_verbose(ctx, "device features: cooperative_matrix, maintenance1, float8, memory model, wave32");
    return 0;
}

// ------------------------------------------------------------------ context lifecycle -------------

static void mlfi_default_path(char* out, size_t size, const char* env, const char* suffix)
{
    const char* explicit_path = getenv(env);
    if (explicit_path && *explicit_path)
    {
        snprintf(out, size, "%s", explicit_path);
        return;
    }
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg)
        snprintf(out, size, "%s/mk8online/%s", xdg, suffix);
    else
    {
        const char* home = getenv("HOME");
        snprintf(out, size, "%s/.cache/mk8online/%s", (home && *home) ? home : ".", suffix);
    }
}

static void mlfi_destroy_internal(MlfiContext* ctx)
{
    if (!ctx)
        return;
    if (ctx->dev)
    {
        if (ctx->vk.DestroyDescriptorPool && ctx->descriptor_pool)
            ctx->vk.DestroyDescriptorPool(ctx->dev, ctx->descriptor_pool, NULL);
        for (int i = 0; i < ctx->compose_target_count; ++i)
            if (ctx->vk.DestroyFramebuffer && ctx->compose_targets[i].framebuffer)
                ctx->vk.DestroyFramebuffer(ctx->dev, ctx->compose_targets[i].framebuffer, NULL);
        for (int i = 0; i < ctx->compose_pass_count; ++i)
        {
            if (ctx->vk.DestroyPipeline && ctx->compose_passes[i].pipeline)
                ctx->vk.DestroyPipeline(ctx->dev, ctx->compose_passes[i].pipeline, NULL);
            if (ctx->vk.DestroyRenderPass && ctx->compose_passes[i].pass)
                ctx->vk.DestroyRenderPass(ctx->dev, ctx->compose_passes[i].pass, NULL);
        }
        if (ctx->vk.DestroyPipelineLayout && ctx->compose_pipe_layout)
            ctx->vk.DestroyPipelineLayout(ctx->dev, ctx->compose_pipe_layout, NULL);
        if (ctx->vk.DestroyDescriptorSetLayout && ctx->compose_set_layout)
            ctx->vk.DestroyDescriptorSetLayout(ctx->dev, ctx->compose_set_layout, NULL);
        for (int i = 0; i < 3; ++i)
        {
            if (ctx->vk.DestroyPipeline && ctx->copy_pipeline[i])
                ctx->vk.DestroyPipeline(ctx->dev, ctx->copy_pipeline[i], NULL);
            if (ctx->vk.DestroyPipelineLayout && ctx->copy_pipe_layout[i])
                ctx->vk.DestroyPipelineLayout(ctx->dev, ctx->copy_pipe_layout[i], NULL);
            if (ctx->vk.DestroyDescriptorSetLayout && ctx->copy_set_layout[i])
                ctx->vk.DestroyDescriptorSetLayout(ctx->dev, ctx->copy_set_layout[i], NULL);
        }
        for (int i = 0; i < ctx->app_view_count; ++i)
            if (ctx->vk.DestroyImageView && ctx->app_views[i].view)
                ctx->vk.DestroyImageView(ctx->dev, ctx->app_views[i].view, NULL);
        for (int i = 0; i < ctx->pipe_count; ++i)
        {
            if (ctx->vk.DestroyPipeline && ctx->pipes[i].pipeline)
                ctx->vk.DestroyPipeline(ctx->dev, ctx->pipes[i].pipeline, NULL);
            if (ctx->vk.DestroyPipelineLayout && ctx->pipes[i].pipe_layout)
                ctx->vk.DestroyPipelineLayout(ctx->dev, ctx->pipes[i].pipe_layout, NULL);
            if (ctx->vk.DestroyDescriptorSetLayout && ctx->pipes[i].set_layout)
                ctx->vk.DestroyDescriptorSetLayout(ctx->dev, ctx->pipes[i].set_layout, NULL);
        }
        for (int i = 0; i < 2; ++i)
            if (ctx->vk.DestroyImageView && ctx->splat_storage_view[i])
                ctx->vk.DestroyImageView(ctx->dev, ctx->splat_storage_view[i], NULL);
        for (int i = 0; i < ctx->image_count; ++i)
        {
            if (ctx->vk.DestroyImageView && ctx->images[i].view)
                ctx->vk.DestroyImageView(ctx->dev, ctx->images[i].view, NULL);
            if (ctx->vk.DestroyImage && ctx->images[i].image)
                ctx->vk.DestroyImage(ctx->dev, ctx->images[i].image, NULL);
            if (ctx->vk.FreeMemory && ctx->images[i].memory)
                ctx->vk.FreeMemory(ctx->dev, ctx->images[i].memory, NULL);
        }
        if (ctx->vk.DestroySampler && ctx->sampler_linear)
            ctx->vk.DestroySampler(ctx->dev, ctx->sampler_linear, NULL);
        if (ctx->vk.DestroySampler && ctx->sampler_nearest)
            ctx->vk.DestroySampler(ctx->dev, ctx->sampler_nearest, NULL);
        if (ctx->vk.UnmapMemory && ctx->arena_map)
            ctx->vk.UnmapMemory(ctx->dev, ctx->arena_mem);
        if (ctx->vk.UnmapMemory && ctx->staging_map)
            ctx->vk.UnmapMemory(ctx->dev, ctx->staging_model_mem);
        VkBuffer buffers[4] = {ctx->model, ctx->scratch, ctx->arena, ctx->staging_model};
        VkDeviceMemory memories[4] = {ctx->model_mem, ctx->scratch_mem, ctx->arena_mem,
                                      ctx->staging_model_mem};
        for (int i = 0; i < 4; ++i)
        {
            if (ctx->vk.DestroyBuffer && buffers[i])
                ctx->vk.DestroyBuffer(ctx->dev, buffers[i], NULL);
            if (ctx->vk.FreeMemory && memories[i])
                ctx->vk.FreeMemory(ctx->dev, memories[i], NULL);
        }
    }
    for (int i = 0; i < ctx->module_count; ++i)
        mlfi_host_free(ctx, ctx->modules[i].code);
    if (g_scan)
    {
        for (int i = 0; i < 65536; ++i)
            free(g_scan->names[i]);
        free(g_scan);
        g_scan = NULL;
    }
    if (ctx->mem.dealloc)
        ctx->mem.dealloc(ctx->mem.pUserData, ctx);
    else
        free(ctx);
}

static ffxReturnCode_t mlfi_context_create(ffxContext* out_context, ffxCreateContextDescHeader* desc,
                                           const ffxAllocationCallbacks* mem_cb)
{
    if (!out_context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    *out_context = NULL;
    if (!desc || (desc->type & FFX_API_EFFECT_MASK) != FFX_API_EFFECT_ID_FRAMEGENERATION)
        return FFX_API_RETURN_NO_PROVIDER;

    ffxCreateContextDescFrameGeneration* fg = NULL;
    ffxCreateBackendVKDesc* backend = NULL;
    uint32_t api_version = 0;
    int have_api_version = 0;
    uint64_t override_id = 0;
    for (ffxCreateContextDescHeader* h = desc; h; h = h->pNext)
    {
        switch (h->type)
        {
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION:
            fg = (ffxCreateContextDescFrameGeneration*)h;
            break;
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK:
            backend = (ffxCreateBackendVKDesc*)h;
            break;
        case FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION:
        {
            ffxCreateContextDescFrameGenerationVersion* v = (ffxCreateContextDescFrameGenerationVersion*)h;
            api_version = v->version;
            have_api_version = 1;
            break;
        }
        case FFX_API_DESC_TYPE_OVERRIDE_VERSION:
            override_id = ((ffxOverrideVersion*)h)->versionId;
            break;
        default:
            break;
        }
    }

    MlfiContext* ctx = NULL;
    if (mem_cb && mem_cb->alloc)
        ctx = (MlfiContext*)mem_cb->alloc(mem_cb->pUserData, sizeof(MlfiContext));
    else
        ctx = (MlfiContext*)malloc(sizeof(MlfiContext));
    if (!ctx)
        return FFX_API_RETURN_ERROR_MEMORY;
    memset(ctx, 0, sizeof(*ctx));
    ctx->magic = 0x4d4c4649u;  // 'MLFI'
    if (mem_cb)
        ctx->mem = *mem_cb;
    ctx->first_dispatch = 1;
    ctx->debug_level = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_ERRORS;
    ctx->last_create_rc = FFX_API_RETURN_ERROR;

    if (!fg)
    {
        mlfi_error(ctx, "create: no ffxCreateContextDescFrameGeneration in the descriptor chain");
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (!backend || !backend->deviceContext.vkDevice || !backend->deviceContext.vkPhysicalDevice ||
        !backend->deviceContext.vkDeviceProcAddr)
    {
        mlfi_error(ctx, "create: ffxCreateBackendVKDesc (device, physical device, proc addr) is required");
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (have_api_version && api_version != FFX_FRAMEGENERATION_VERSION)
    {
        mlfi_error(ctx, "create: ffxCreateContextDescFrameGenerationVersion 0x%x is not 0x%x (4.0.2)",
                   api_version, FFX_FRAMEGENERATION_VERSION);
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (override_id && override_id != MLFI_PROVIDER_VERSION_ID)
    {
        mlfi_error(ctx, "create: ffxOverrideVersion 0x%016llx is not the MLFI 4.0.2 provider",
                   (unsigned long long)override_id);
        ctx->last_create_rc = FFX_API_RETURN_NO_PROVIDER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (fg->displaySize.width != MLFI_SUPPORTED_WIDTH || fg->displaySize.height != MLFI_SUPPORTED_HEIGHT ||
        fg->maxRenderSize.width != MLFI_SUPPORTED_WIDTH ||
        fg->maxRenderSize.height != MLFI_SUPPORTED_HEIGHT)
    {
        mlfi_error(ctx, "create: this milestone implements the captured 1280x720 layout, got "
                        "display %ux%u render max %ux%u",
                   fg->displaySize.width, fg->displaySize.height, fg->maxRenderSize.width,
                   fg->maxRenderSize.height);
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (fg->flags != 0)
    {
        mlfi_error(ctx, "create: flags 0x%x are not implemented (only 0: SDR, finite far, "
                        "render-resolution MVs)", fg->flags);
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (fg->backBufferFormat != FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM &&
        fg->backBufferFormat != FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM)
    {
        mlfi_error(ctx, "create: backBufferFormat %u is not 8-bit UNORM", fg->backBufferFormat);
        ctx->last_create_rc = FFX_API_RETURN_ERROR_PARAMETER;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }

    ctx->dev = backend->deviceContext.vkDevice;
    ctx->phys = backend->deviceContext.vkPhysicalDevice;
    ctx->gdpa = backend->deviceContext.vkDeviceProcAddr;
    ctx->display_w = fg->displaySize.width;
    ctx->display_h = fg->displaySize.height;
    ctx->render_w = fg->maxRenderSize.width;
    ctx->render_h = fg->maxRenderSize.height;
    ctx->backbuffer_format = fg->backBufferFormat;
    ctx->create_flags = fg->flags;
    mlfi_default_path(ctx->modules_dir, sizeof(ctx->modules_dir), "MK8_MLFI_MODULES",
                      "mlfi-replay/modules");
    mlfi_default_path(ctx->model_path, sizeof(ctx->model_path), "MK8_MLFI_MODEL",
                      "mlfi/model/model_blob.bin");

    if (mlfi_load_vk(ctx) != 0 || mlfi_check_device(ctx) != 0)
    {
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    if (!g_scan)
        g_scan = (MlfiSpirvScan*)calloc(1, sizeof(*g_scan));
    if (!g_scan || mlfi_load_modules(ctx) != 0 || mlfi_create_resources(ctx) != 0)
    {
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }

    // Model bank: the exact uploaded range model_blob[72 : 72+10,657,152], checked before use.
    {
        size_t total = 0;
        uint8_t* blob = mlfi_read_file(ctx, ctx->model_path, &total);
        if (!blob || total < MLFI_MODEL_BANK_OFFSET + MLFI_MODEL_SIZE)
        {
            mlfi_error(ctx, "cannot read %s or it is shorter than the captured model range",
                       ctx->model_path);
            mlfi_host_free(ctx, blob);
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
        uint64_t hash = fnv1a64(blob + MLFI_MODEL_BANK_OFFSET, (size_t)MLFI_MODEL_SIZE);
        if (hash != MLFI_MODEL_FNV1A)
        {
            mlfi_error(ctx, "%s model range fnv1a=%016llx, capture has %016llx; refusing to run",
                       ctx->model_path, (unsigned long long)hash,
                       (unsigned long long)MLFI_MODEL_FNV1A);
            mlfi_host_free(ctx, blob);
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
        memcpy(ctx->staging_map, blob + MLFI_MODEL_BANK_OFFSET, (size_t)MLFI_MODEL_SIZE);
        mlfi_host_free(ctx, blob);
        mlfi_info(ctx, "model: model_blob[72 : 10657224] fnv1a=%016llx (matches capture)",
                  (unsigned long long)hash);
    }

    // Identify the four preprocess pipelines the 4K context creates.  The fifth prepare row reuses
    // the splat module; the 8,980-byte module is created but never dispatched (as captured).
    MlfiModule* dilate = mlfi_module_by_ubo(ctx, "CsDilate");
    MlfiModule* splat = mlfi_module_by_ubo(ctx, "CsAMDSplatting64Constants");
    MlfiModule* fwdmv = mlfi_module_by_ubo(ctx, "CsComputeFwdMVUsingMatConstants");
    MlfiModule* watermark = NULL;
    for (int i = 0; i < ctx->module_count; ++i)
    {
        MlfiModule* m = &ctx->modules[i];
        if (strcmp(m->entry, "main"))
            continue;
        if (m == dilate || m == splat || m == fwdmv)
            continue;
        if (watermark)
        {
            mlfi_error(ctx, "more than one undispatched preprocess module");
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
        watermark = m;
    }
    if (!dilate || !splat || !fwdmv || !watermark)
    {
        mlfi_error(ctx, "cannot identify the four preprocess modules by their uniform block names");
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    ctx->prepare_modules[0] = dilate;
    ctx->prepare_modules[1] = splat;
    ctx->prepare_modules[2] = fwdmv;
    ctx->prepare_modules[3] = splat;
    ctx->prepare_modules[4] = splat;

    // Pipelines: the 191 model passes in captured order, then the four preprocess modules.
    for (uint32_t i = MLFI_PREPARE_ROWS; i < MLFI_GRAPH_ROW_COUNT; ++i)
    {
        MlfiModule* m = mlfi_module_by_entry(ctx, g_mlfi_rows[i].entry);
        if (!m)
        {
            mlfi_error(ctx, "no module for %s in %s", g_mlfi_rows[i].entry, ctx->modules_dir);
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
        if (!mlfi_create_pipe(ctx, m))
        {
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
    }
    MlfiPipe* prep_pipes[4];
    prep_pipes[0] = mlfi_create_pipe(ctx, dilate);
    prep_pipes[1] = mlfi_create_pipe(ctx, splat);
    prep_pipes[2] = mlfi_create_pipe(ctx, fwdmv);
    prep_pipes[3] = mlfi_create_pipe(ctx, watermark);
    if (!prep_pipes[0] || !prep_pipes[1] || !prep_pipes[2] || !prep_pipes[3])
    {
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    // The capture's 78/175/3/175 counts cover the 191 model passes; the four preprocess modules
    // are not part of that table (they bind their own small blocks).
    for (int i = 0; i < (int)MLFI_GRAPH_MODEL_ROWS; ++i)
        for (uint32_t b = 0; b < ctx->pipes[i].module->nbind; ++b)
            if (ctx->pipes[i].module->binds[b].binding < 512)
                ctx->bind_counts[ctx->pipes[i].module->binds[b].binding]++;
    if (ctx->bind_counts[0] != 78 || ctx->bind_counts[128] != 175 || ctx->bind_counts[256] != 3 ||
        ctx->bind_counts[257] != 175)
    {
        mlfi_error(ctx, "binding counts differ from the capture: model0=%d scratch=%d constants=%d "
                        "tensors=%d (expected 78/175/3/175)",
                   ctx->bind_counts[0], ctx->bind_counts[128], ctx->bind_counts[256],
                   ctx->bind_counts[257]);
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    mlfi_info(ctx, "pipelines: %d created (191 model + 4 preprocess); bindings match capture",
              ctx->pipe_count);

    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLER, 64},
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1024},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64},
    };
    VkDescriptorPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 256;
    pool_info.poolSizeCount = sizeof(pool_sizes) / sizeof(pool_sizes[0]);
    pool_info.pPoolSizes = pool_sizes;
    VkResult vr = ctx->vk.CreateDescriptorPool(ctx->dev, &pool_info, NULL, &ctx->descriptor_pool);
    if (vr != VK_SUCCESS)
    {
        mlfi_error(ctx, "vkCreateDescriptorPool failed: %s", mlfi_vk_result_name(vr));
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }

    // One descriptor set per dispatch: the captured arena offsets are baked in once.
    {
        VkDescriptorSetLayout layouts[MLFI_PREPARE_ROWS];
        for (int i = 0; i < MLFI_PREPARE_ROWS; ++i)
        {
            int source = (i == 0) ? 0 : (i == 2 ? 2 : 1);
            ctx->prepare_dispatch[i] = *prep_pipes[source];
            layouts[i] = prep_pipes[source]->set_layout;
        }
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ctx->descriptor_pool;
        ai.descriptorSetCount = MLFI_PREPARE_ROWS;
        ai.pSetLayouts = layouts;
        VkDescriptorSet sets[MLFI_PREPARE_ROWS];
        vr = ctx->vk.AllocateDescriptorSets(ctx->dev, &ai, sets);
        if (vr != VK_SUCCESS)
        {
            mlfi_error(ctx, "prepare descriptor sets failed: %s", mlfi_vk_result_name(vr));
            ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
            {
                ffxReturnCode_t failure = ctx->last_create_rc;
                mlfi_destroy_internal(ctx);
                return failure;
            }
        }
        for (int i = 0; i < MLFI_PREPARE_ROWS; ++i)
        {
            ctx->prepare_dispatch[i].set = sets[i];
            if (mlfi_write_pass_descriptors(ctx, &ctx->prepare_dispatch[i], &g_mlfi_rows[i], i) != 0)
            {
                ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
                {
                    ffxReturnCode_t failure = ctx->last_create_rc;
                    mlfi_destroy_internal(ctx);
                    return failure;
                }
            }
        }
        for (uint32_t i = MLFI_PREPARE_ROWS; i < MLFI_GRAPH_ROW_COUNT; ++i)
        {
            MlfiPipe* p = &ctx->pipes[i - MLFI_PREPARE_ROWS];
            VkDescriptorSetAllocateInfo dai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = ctx->descriptor_pool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &p->set_layout;
            vr = ctx->vk.AllocateDescriptorSets(ctx->dev, &dai, &p->set);
            if (vr != VK_SUCCESS)
            {
                mlfi_error(ctx, "descriptor set for %s failed: %s", p->module->entry,
                           mlfi_vk_result_name(vr));
                ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
                {
                    ffxReturnCode_t failure = ctx->last_create_rc;
                    mlfi_destroy_internal(ctx);
                    return failure;
                }
            }
            if (mlfi_write_pass_descriptors(ctx, p, &g_mlfi_rows[i], -1) != 0)
            {
                ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
                {
                    ffxReturnCode_t failure = ctx->last_create_rc;
                    mlfi_destroy_internal(ctx);
                    return failure;
                }
            }
        }
        mlfi_info(ctx, "descriptor sets: %u written (5 prepare + 191 model)", MLFI_GRAPH_ROW_COUNT);
    }

    mlfi_write_tensor_tables(ctx);
    mlfi_write_frame_constants(ctx, &(ffxDispatchDescFrameGeneration){0});  // deterministic initial state

    if (mlfi_create_copy_pipelines(ctx) != 0)
    {
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }
    // Create the composition pipeline for the context's back-buffer format now, so
    // `mk8MlfiGetStats`/the context summary describe pipelines that exist.  A different
    // outputs[0] format creates a second pass lazily.
    if (!mlfi_compose_pass(ctx, mlfi_vk_format(ctx->backbuffer_format)))
    {
        ctx->last_create_rc = FFX_API_RETURN_ERROR_RUNTIME_ERROR;
        {
            ffxReturnCode_t failure = ctx->last_create_rc;
            mlfi_destroy_internal(ctx);
            return failure;
        }
    }

    ctx->last_create_rc = FFX_API_RETURN_OK;
    mlfi_info(ctx, "context: MLFI 4.0.2 id=0x%016llx display=%ux%u render=%ux%u", 
              (unsigned long long)MLFI_PROVIDER_VERSION_ID, ctx->display_w, ctx->display_h,
              ctx->render_w, ctx->render_h);
    *out_context = ctx;
    return FFX_API_RETURN_OK;
}

// ------------------------------------------------------------------ FFX exports -------------------

FFX_API_ENTRY ffxReturnCode_t ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                               const ffxAllocationCallbacks* memCb)
{
    return mlfi_context_create(context, desc, memCb);
}

FFX_API_ENTRY ffxReturnCode_t ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb)
{
    (void)memCb;
    if (!context || !*context)
        return FFX_API_RETURN_ERROR_PARAMETER;
    MlfiContext* ctx = (MlfiContext*)*context;
    if (ctx->magic != 0x4d4c4649u)
        return FFX_API_RETURN_ERROR_PARAMETER;
    ctx->magic = 0;
    mlfi_destroy_internal(ctx);
    *context = NULL;
    return FFX_API_RETURN_OK;
}

FFX_API_ENTRY ffxReturnCode_t ffxQuery(ffxContext* context, ffxQueryDescHeader* desc)
{
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)
    {
        ffxQueryDescGetVersions* versions = (ffxQueryDescGetVersions*)desc;
        if (!versions->outputCount)
            return FFX_API_RETURN_ERROR_PARAMETER;
        if ((versions->createDescType & FFX_API_EFFECT_MASK) != FFX_API_EFFECT_ID_FRAMEGENERATION)
        {
            *versions->outputCount = 0;
            return FFX_API_RETURN_NO_PROVIDER;
        }
        uint64_t capacity = *versions->outputCount;
        *versions->outputCount = 1;
        if (capacity >= 1)
        {
            if (versions->versionIds)
                versions->versionIds[0] = MLFI_PROVIDER_VERSION_ID;
            if (versions->versionNames)
                versions->versionNames[0] = MLFI_PROVIDER_VERSION_NAME;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION)
    {
        if (!context || !*context || ((MlfiContext*)*context)->magic != 0x4d4c4649u)
            return FFX_API_RETURN_ERROR_PARAMETER;
        ffxQueryGetProviderVersion* version = (ffxQueryGetProviderVersion*)desc;
        version->versionId = MLFI_PROVIDER_VERSION_ID;
        version->versionName = MLFI_PROVIDER_VERSION_NAME;
        return FFX_API_RETURN_OK;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

FFX_API_ENTRY ffxReturnCode_t ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc)
{
    if (!desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG)
    {
        MlfiContext* ctx = context && *context ? (MlfiContext*)*context : NULL;
        if (ctx && ctx->magic == 0x4d4c4649u)
        {
            const ffxConfigureDescGlobalDebug* debug = (const ffxConfigureDescGlobalDebug*)desc;
            ctx->message = debug->fpMessage;
            ctx->debug_level = debug->debugLevel;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1)
    {
        // The deprecated structure has no effectId member between the header and the callback.
        MlfiContext* ctx = context && *context ? (MlfiContext*)*context : NULL;
        if (ctx && ctx->magic == 0x4d4c4649u)
        {
            const ffxConfigureDescGlobalDebug1* debug = (const ffxConfigureDescGlobalDebug1*)desc;
            ctx->message = debug->fpMessage;
            ctx->debug_level = debug->debugLevel;
        }
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION)
    {
        if (!context || !*context)
            return FFX_API_RETURN_ERROR_PARAMETER;
        MlfiContext* ctx = (MlfiContext*)*context;
        if (ctx->magic != 0x4d4c4649u)
            return FFX_API_RETURN_ERROR_PARAMETER;
        const ffxConfigureDescFrameGeneration* config = (const ffxConfigureDescFrameGeneration*)desc;
        ctx->swapchain = config->swapChain;
        ctx->present_callback = config->presentCallback;
        ctx->present_callback_user = config->presentCallbackUserContext;
        ctx->frame_callback = config->frameGenerationCallback;
        ctx->frame_callback_user = config->frameGenerationCallbackUserContext;
        ctx->frame_gen_enabled = config->frameGenerationEnabled ? 1 : 0;
        ctx->allow_async_workloads = config->allowAsyncWorkloads ? 1 : 0;
        ctx->only_present_generated = config->onlyPresentGenerated ? 1 : 0;
        ctx->configure_flags = config->flags;
        ctx->hudless_color = config->HUDLessColor;
        ctx->generation_rect = config->generationRect;
        if (ctx->have_configure_frame_id && config->frameID != ctx->last_configure_frame_id + 1 &&
            config->frameGenerationEnabled)
            ctx->reset_pending = 1;
        ctx->last_configure_frame_id = config->frameID;
        ctx->have_configure_frame_id = 1;
        // The swapchain/callback fields are stored, not used: this milestone does not replace a
        // swapchain.  The application calls ffxDispatch itself, as the capture harness did.
        return FFX_API_RETURN_OK;
    }
    if (desc->type == FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION_KEYVALUE)
        return FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE;
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

FFX_API_ENTRY ffxReturnCode_t ffxDispatch(ffxContext* context, const ffxDispatchDescHeader* desc)
{
    if (!context || !*context || !desc)
        return FFX_API_RETURN_ERROR_PARAMETER;
    MlfiContext* ctx = (MlfiContext*)*context;
    if (ctx->magic != 0x4d4c4649u)
        return FFX_API_RETURN_ERROR_PARAMETER;
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2)
    {
        ffxReturnCode_t rc = mlfi_prepare(ctx, (const ffxDispatchDescFrameGenerationPrepareV2*)desc);
        ctx->last_prepare_rc = rc;
        return rc;
    }
    if (desc->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION)
    {
        ffxReturnCode_t rc = mlfi_frame(ctx, (const ffxDispatchDescFrameGeneration*)desc);
        ctx->last_frame_rc = rc;
        return rc;
    }
    return FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE;
}

__attribute__((visibility("default"))) uint32_t mk8MlfiGetStats(void* context, Mk8MlfiStats* stats)
{
    if (!context || !stats || stats->struct_size != sizeof(Mk8MlfiStats))
        return FFX_API_RETURN_ERROR_PARAMETER;
    MlfiContext* ctx = (MlfiContext*)context;
    if (ctx->magic != 0x4d4c4649u)
        return FFX_API_RETURN_ERROR_PARAMETER;
    memset(stats, 0, sizeof(*stats));
    stats->struct_size = sizeof(*stats);
    stats->version = MK8_MLFI_STATS_VERSION;
    stats->graph_pipelines = (uint32_t)ctx->pipe_count;
    stats->copy_pipelines = 3;
    stats->compose_pipelines = (uint32_t)ctx->compose_pass_count;
    stats->prepare_dispatches = ctx->last_prepare_dispatches;
    stats->copy_dispatches = ctx->last_copy_dispatches;
    stats->model_dispatches = ctx->last_model_dispatches;
    stats->compose_dispatches = ctx->last_compose_dispatches;
    stats->frames = ctx->frames;
    stats->resets = ctx->resets;
    stats->first_frame = ctx->first_dispatch ? 1 : 0;
    stats->last_create_rc = (uint32_t)ctx->last_create_rc;
    stats->last_prepare_rc = (uint32_t)ctx->last_prepare_rc;
    stats->last_frame_rc = (uint32_t)ctx->last_frame_rc;
    stats->display_width = ctx->display_w;
    stats->display_height = ctx->display_h;
    stats->render_width = ctx->render_w;
    stats->render_height = ctx->render_h;
    stats->backbuffer_format = ctx->backbuffer_format;
    return FFX_API_RETURN_OK;
}
