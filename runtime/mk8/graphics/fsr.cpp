// FSR 4.1.2 / 3.1.6 upscaling bridge (host side, no guest changes).
//
// Loads an FFX upscaler provider, creates a context for the game image and the
// window size, and records one upscale into the current command buffer. The
// provider owns its history and model assets; this file only supplies the
// colour, depth and motion-vector images and the output image.
//
// The first integration runs with a neutral depth image and zero motion
// vectors. Real MK8 depth/motion vectors are a separate qualification step;
// the callers log that state so a prototype is never mistaken for qualified
// output.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <math.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if !defined(_WIN32)
#define __declspec(x)
#endif

#include "ffx/ffx_api.h"
#include "ffx/ffx_upscale.h"
#include "ffx/vk/ffx_api_vk.h"

#include "vulkan/vk.h"
#include "fsr_fg.h"
#include "dlss.h"
#include "xess.h"

extern "C" {
VkDescriptorSet fsr_vk_alloc_set(VkDescriptorSetLayout layout);
VkSampler fsr_vk_default_sampler(bool linear);
VkImageView fsr_vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv,
                              uint32_t ly0, uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect);
VkShaderModule fsr_vk_compile_glsl(const char *src, int stage, const char *name);
}



typedef struct {
    bool tried;
    bool available;
    void *library;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    ffxContext context;
    uint32_t rw, rh, ow, oh;
    VkImage output;
    VmaAllocation output_mem;
    VkImageView output_view;
    VkImage depth;
    VmaAllocation depth_mem;
    VkImageView depth_view;
    VkImage motion;
    VmaAllocation motion_mem;
    VkImageView motion_view;
    bool primed;
    bool create_failed;
    VkDescriptorSetLayout mv_layout;
    VkPipelineLayout mv_pipeline_layout;
    VkPipeline mv_pipeline;
    VkBuffer mv_ubo;
    VmaAllocation mv_ubo_mem;
    void *mv_ubo_ptr;
    bool have_prev;
    float prev_vp[16];
    // Frame-generation inputs (fsr_fg.cpp): independent of the upscaler's set.
    VkImage neutral_depth;
    VmaAllocation neutral_depth_mem;
    VkImageView neutral_depth_view;
    uint32_t neutral_w, neutral_h;
    bool neutral_primed;
    uint32_t motion_w, motion_h;
    bool motion_primed;
} Fsr4;

static Fsr4 fsr;

extern "C" {
extern float g_fsr_view[12];
extern float g_fsr_proj[16];
extern int g_fsr_vp_valid;
extern float g_fsr_jitter_x;
extern float g_fsr_jitter_y;
extern TexBackend *g_fsr_captured;  // exec.c: the scene-depth copy at the pass boundary
}

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

static VkImageView image_view(VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

static VkImage make_image(VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                          VkImageAspectFlags aspect, VmaAllocation *allocation) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (vmaCreateImage(g_vk.vma, &ci, &ai, &image, allocation, NULL) != VK_SUCCESS) return VK_NULL_HANDLE;
    (void)aspect;
    return image;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect,
                    VkImageLayout old_layout, VkImageLayout new_layout,
                    VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = old_layout;
    b.newLayout = new_layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void mul4(const float a[16], const float b[16], float out[16]) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                             a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
}

static bool invert4(const float m[16], float out[16]) {
    float a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) a[r][c] = m[r * 4 + c];
        for (int c = 0; c < 4; c++) a[r][4 + c] = (r == c) ? 1.0f : 0.0f;
    }
    for (int c = 0; c < 4; c++) {
        int pivot = c;
        for (int r = c + 1; r < 4; r++) if (fabsf(a[r][c]) > fabsf(a[pivot][c])) pivot = r;
        if (fabsf(a[pivot][c]) < 1e-9f) return false;
        if (pivot != c) for (int k = 0; k < 8; k++) { float t = a[c][k]; a[c][k] = a[pivot][k]; a[pivot][k] = t; }
        float d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++) if (r != c) {
            float f = a[r][c];
            if (f == 0.0f) continue;
            for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][4 + c];
    return true;
}

static bool create_mv_resources(void) {
    if (fsr.mv_pipeline) return true;
    VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = b;
    if (vkCreateDescriptorSetLayout(g_vk.dev, &li, NULL, &fsr.mv_layout) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &fsr.mv_layout;
    if (vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &fsr.mv_pipeline_layout) != VK_SUCCESS) return false;
    static const char *src =
        "#version 450\n"
        "layout(local_size_x = 16, local_size_y = 16) in;\n"
        "layout(set = 0, binding = 0) uniform U { mat4 invVP; mat4 prevVP; vec2 size; } u;\n"
        "layout(set = 0, binding = 1) uniform sampler2D depthTex;\n"
        "layout(set = 0, binding = 2, rg16f) uniform image2D mvOut;\n"
        "void main() {\n"
        "  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n"
        "  if (p.x >= int(u.size.x) || p.y >= int(u.size.y)) return;\n"
        "  vec2 uv = (vec2(p) + 0.5) / u.size;\n"
        "  float d = texture(depthTex, uv).r;\n"
        "  if (d >= 0.9999) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  vec4 w = u.invVP * vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
        "  if (abs(w.w) < 1e-9) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  w /= w.w;\n"
        "  vec4 pc = u.prevVP * w;\n"
        "  if (pc.w <= 0.0) { imageStore(mvOut, p, vec4(0.0)); return; }\n"
        "  vec2 puv = pc.xy / pc.w * 0.5 + 0.5;\n"
        "  imageStore(mvOut, p, vec4((puv - uv) * u.size, 0.0, 0.0));\n"
        "}\n";
    VkShaderModule module = fsr_vk_compile_glsl(src, VK_SHADER_STAGE_COMPUTE_BIT, "fsr_mv");
    if (!module) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = fsr.mv_pipeline_layout;
    VkResult r = vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &fsr.mv_pipeline);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    if (r != VK_SUCCESS) return false;
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 144;
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo ai = {};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info = {};
    if (vmaCreateBuffer(g_vk.vma, &bci, &ai, &fsr.mv_ubo, &fsr.mv_ubo_mem, &info) != VK_SUCCESS) return false;
    fsr.mv_ubo_ptr = info.pMappedData;
    return fsr.mv_ubo_ptr != NULL;
}

static bool run_mv(VkCommandBuffer cb, TexBackend *depth, uint32_t rw, uint32_t rh) {
    if (!g_fsr_vp_valid || !depth || !depth->image || !fsr.mv_pipeline || !fsr.mv_ubo_ptr) {
        static int once;
        if (!once++) fprintf(stderr, "[fsr] motion skipped (vp %d depth %p)\n", g_fsr_vp_valid, depth ? (void *)depth->image : NULL);
        return false;
    }
    float v[16] = {g_fsr_view[0], g_fsr_view[1], g_fsr_view[2], g_fsr_view[3],
                   g_fsr_view[4], g_fsr_view[5], g_fsr_view[6], g_fsr_view[7],
                   g_fsr_view[8], g_fsr_view[9], g_fsr_view[10], g_fsr_view[11],
                   0.0f, 0.0f, 0.0f, 1.0f};
    float vp[16], inv[16], prev[16];
    mul4(g_fsr_proj, v, vp);
    if (!invert4(vp, inv)) return false;
    if (fsr.have_prev) memcpy(prev, fsr.prev_vp, sizeof prev);
    else memcpy(prev, vp, sizeof prev);
    float *u = (float *)fsr.mv_ubo_ptr;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            u[c * 4 + r] = inv[r * 4 + c];
            u[16 + c * 4 + r] = prev[r * 4 + c];
        }
    u[32] = (float)rw;
    u[33] = (float)rh;
    barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkDescriptorSet set = fsr_vk_alloc_set(fsr.mv_layout);
    VkDescriptorBufferInfo bi = {fsr.mv_ubo, 0, 144};
    VkDescriptorImageInfo di = {fsr_vk_default_sampler(true),
                                fsr_vk_image_view(depth, VK_IMAGE_VIEW_TYPE_2D, depth->format, 0, 1, 0, 1,
                                              VkComponentMapping{}, (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, fsr.motion_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[3] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &di, NULL, NULL},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &oi, NULL, NULL}};
    vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.mv_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, fsr.mv_pipeline_layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (rw + 15) / 16, (rh + 15) / 16, 1);
    barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    memcpy(fsr.prev_vp, vp, sizeof fsr.prev_vp);
    fsr.have_prev = true;
    return true;
}

static void destroy_neutral_depth(void) {
    if (fsr.neutral_depth_view) vkDestroyImageView(g_vk.dev, fsr.neutral_depth_view, NULL);
    if (fsr.neutral_depth) { vkDestroyImage(g_vk.dev, fsr.neutral_depth, NULL); vmaFreeMemory(g_vk.vma, fsr.neutral_depth_mem); }
    fsr.neutral_depth = VK_NULL_HANDLE;
    fsr.neutral_depth_mem = VK_NULL_HANDLE;
    fsr.neutral_depth_view = VK_NULL_HANDLE;
    fsr.neutral_primed = false;
}

// A depth image of the presented size, cleared to 1.0 once. It is the fallback when the
// scene depth copy is not available; leaving it SHADER_READ_ONLY suits every consumer.
static bool ensure_neutral_depth(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (fsr.neutral_depth && fsr.neutral_w == w && fsr.neutral_h == h) return fsr.neutral_depth_view != VK_NULL_HANDLE;
    destroy_neutral_depth();
    fsr.neutral_depth = make_image(VK_FORMAT_D32_SFLOAT, w, h,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                   VK_IMAGE_ASPECT_DEPTH_BIT, &fsr.neutral_depth_mem);
    if (!fsr.neutral_depth) return false;
    fsr.neutral_depth_view = image_view(fsr.neutral_depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!fsr.neutral_depth_view) return false;
    fsr.neutral_w = w;
    fsr.neutral_h = h;
    return true;
}

static bool ensure_motion(VkCommandBuffer cb, uint32_t w, uint32_t h) {
    if (!fsr.motion || fsr.motion_w != w || fsr.motion_h != h) {
        if (fsr.motion_view) vkDestroyImageView(g_vk.dev, fsr.motion_view, NULL);
        if (fsr.motion) { vkDestroyImage(g_vk.dev, fsr.motion, NULL); vmaFreeMemory(g_vk.vma, fsr.motion_mem); }
        fsr.motion = make_image(VK_FORMAT_R16G16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                                VK_IMAGE_ASPECT_COLOR_BIT, &fsr.motion_mem);
        fsr.motion_view = fsr.motion ? image_view(fsr.motion, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT) : VK_NULL_HANDLE;
        fsr.motion_w = w;
        fsr.motion_h = h;
        fsr.motion_primed = false;
    }
    if (!fsr.motion || !fsr.motion_view) return false;
    if (!create_mv_resources()) return false;
    if (!fsr.motion_primed) {
        barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, fsr.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        fsr.motion_primed = true;
    }
    return true;
}

// Frame-generation inputs. The captured scene depth is the copy exec.c makes at the pass end
// (MK8_FSR_FG enables that capture); the motion vectors are the camera-derived ones the upscaler
// uses. A missing view/projection leaves the zero motion image and says so loudly.
extern "C" bool mk8_fsr_fg_inputs(VkCommandBuffer cb, uint32_t w, uint32_t h,
                                  Mk8FsrFgInput *depth, Mk8FsrFgInput *motion) {
    if (!cb || !depth || !motion || !w || !h) return false;
    memset(depth, 0, sizeof *depth);
    memset(motion, 0, sizeof *motion);
    TexBackend *cap = g_fsr_captured;
    bool real = cap && cap->image && cap->width == w && cap->height == h;
    if (real) {
        VkImageView view = fsr_vk_image_view(cap, VK_IMAGE_VIEW_TYPE_2D, cap->format, 0, 1, 0, 1,
                                             VkComponentMapping{}, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!view) return false;
        barrier(cb, cap->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        depth->image = cap->image;
        depth->view = view;
        depth->format = cap->format;
        depth->real = true;
    } else {
        if (!ensure_neutral_depth(cb, w, h)) return false;
        if (!fsr.neutral_primed) {
            barrier(cb, fsr.neutral_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkClearDepthStencilValue one = {1.0f, 0};
            VkImageSubresourceRange range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdClearDepthStencilImage(cb, fsr.neutral_depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &range);
            barrier(cb, fsr.neutral_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            fsr.neutral_primed = true;
        }
        depth->image = fsr.neutral_depth;
        depth->view = fsr.neutral_depth_view;
        depth->format = VK_FORMAT_D32_SFLOAT;
        depth->real = false;
    }
    depth->width = w;
    depth->height = h;
    static int depth_state = -1;
    if ((int)depth->real != depth_state) {
        depth_state = (int)depth->real;
        fprintf(stderr, "[fsr_fg] depth source: %s %ux%u fmt %d\n",
                depth->real ? "captured scene depth" : "neutral (no captured scene depth)", w, h, depth->format);
    }
    if (!ensure_motion(cb, w, h)) return false;
    bool camera = depth->real && run_mv(cb, cap, w, h);
    motion->image = fsr.motion;
    motion->view = fsr.motion_view;
    motion->format = VK_FORMAT_R16G16_SFLOAT;
    motion->width = w;
    motion->height = h;
    motion->real = camera;
    static int motion_state = -1;
    if ((int)camera != motion_state) {
        motion_state = (int)camera;
        if (camera) fprintf(stderr, "[fsr_fg] motion vectors: camera-derived\n");
        else fprintf(stderr, "[fsr_fg] WARNING: motion vectors are zeros (no captured view/projection); "
                             "interpolation relies on optical flow only\n");
    }
    return true;
}

static void destroy_images(void) {
    if (fsr.output_view) vkDestroyImageView(g_vk.dev, fsr.output_view, NULL);
    if (fsr.output) { vkDestroyImage(g_vk.dev, fsr.output, NULL); vmaFreeMemory(g_vk.vma, fsr.output_mem); }
    if (fsr.depth_view) vkDestroyImageView(g_vk.dev, fsr.depth_view, NULL);
    if (fsr.depth) { vkDestroyImage(g_vk.dev, fsr.depth, NULL); vmaFreeMemory(g_vk.vma, fsr.depth_mem); }
    if (fsr.motion_view) vkDestroyImageView(g_vk.dev, fsr.motion_view, NULL);
    if (fsr.motion) { vkDestroyImage(g_vk.dev, fsr.motion, NULL); vmaFreeMemory(g_vk.vma, fsr.motion_mem); }
    fsr.output = VK_NULL_HANDLE;
    fsr.output_view = VK_NULL_HANDLE;
    fsr.depth = VK_NULL_HANDLE;
    fsr.depth_view = VK_NULL_HANDLE;
    fsr.motion = VK_NULL_HANDLE;
    fsr.motion_view = VK_NULL_HANDLE;
    fsr.primed = false;
}

static bool create_images(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    VkImageUsageFlags sampled = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    fsr.output = make_image(VK_FORMAT_R8G8B8A8_UNORM, ow, oh,
                            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, &fsr.output_mem);
    fsr.depth = make_image(VK_FORMAT_D32_SFLOAT, rw, rh, sampled, VK_IMAGE_ASPECT_DEPTH_BIT, &fsr.depth_mem);
    fsr.motion = make_image(VK_FORMAT_R16G16_SFLOAT, rw, rh,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT, &fsr.motion_mem);
    if (!fsr.output || !fsr.depth || !fsr.motion) return false;
    fsr.output_view = image_view(fsr.output, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    fsr.depth_view = image_view(fsr.depth, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    fsr.motion_view = image_view(fsr.motion, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    return fsr.output_view && fsr.depth_view && fsr.motion_view;
}

static bool create_context(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (!fsr.create) return false;
    ffxCreateBackendVKDesc backend = {0};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    // The backend descriptor layout follows the SDK ABI the provider was built
    // against, not the upscaler version it implements. A 3.1.6 provider built
    // from the 2.4 SDK takes the current descriptor; only providers built
    // against the 1.x ABI need the deprecated one.
    if (getenv("MK8_FSR_LEGACY_ABI")) backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK_DEPRECATED;
    backend.deviceContext.vkDevice = g_vk.dev;
    backend.deviceContext.vkPhysicalDevice = g_vk.phys;
    backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;

    // The version descriptor carries the API version this call site was built
    // against. Asking for another provider version is done with the 64-bit
    // ffxOverrideVersion id returned by ffxQuery; the signed 2.4 upscaler
    // advertises 4.1.2, 3.1.6 and 2.3.4 through that list.
    ffxCreateContextDescUpscaleVersion version = {0};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    version.version = FFX_UPSCALER_VERSION;
    const char *want = getenv("MK8_FSR_VERSION");
    ffxOverrideVersion override_version = {0};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (want && *want && fsr.query) {
        uint64_t count = 0;
        ffxQueryDescGetVersions versions = {0};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        versions.device = &backend.deviceContext;
        versions.outputCount = &count;
        if (fsr.query(NULL, &versions.header) == FFX_API_RETURN_OK && count > 0 && count <= 16) {
            uint64_t ids[16] = {0};
            const char *names[16] = {0};
            uint64_t capacity = count;
            versions.outputCount = &capacity;
            versions.versionIds = ids;
            versions.versionNames = names;
            if (fsr.query(NULL, &versions.header) == FFX_API_RETURN_OK) {
                for (uint64_t i = 0; i < capacity && i < count; i++) {
                    if (names[i] && strcmp(names[i], want) == 0) {
                        override_version.versionId = ids[i];
                        break;
                    }
                }
            }
            if (override_version.versionId)
                fprintf(stderr, "[fsr] selecting version %s (0x%llx)\n", want,
                        (unsigned long long)override_version.versionId);
            else
                fprintf(stderr, "[fsr] provider does not offer version %s\n", want);
        }
    }

    ffxCreateContextDescUpscale upscale = {0};
    upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    upscale.maxRenderSize = {rw, rh};
    upscale.maxUpscaleSize = {ow, oh};
    upscale.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;  // the game image is perceptual sRGB
    upscale.header.pNext = &version.header;
    if (override_version.versionId) {
        version.header.pNext = &override_version.header;
        override_version.header.pNext = &backend.header;
    } else {
        version.header.pNext = &backend.header;
    }

    ffxContext context = NULL;
    ffxReturnCode_t rc = fsr.create(&context, &upscale.header, NULL);
    if (rc != FFX_API_RETURN_OK || !context) {
        fprintf(stderr, "[fsr] create failed 0x%x\n", (unsigned)rc);
        fsr.create_failed = true;
        return false;
    }
    fsr.context = context;
    ffxQueryGetProviderVersion provider = {0};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (fsr.query && fsr.query(&context, &provider.header) == FFX_API_RETURN_OK)
        fprintf(stderr, "[fsr] provider %s\n", provider.versionName ? provider.versionName : "(unknown)");
    return true;
}

static bool init(void) {
    if (fsr.tried) return fsr.available;
    fsr.tried = true;
    const char *path = getenv("MK8_FSR_LIB");
    if (!path || !*path) path = "libamd_fidelityfx_upscaler_vk.so";
    fsr.library = load_library(path);
    if (!fsr.library) {
        fprintf(stderr, "[fsr] provider not loaded: %s\n", path);
        return false;
    }
    fsr.create = (PfnFfxCreateContext)load_symbol(fsr.library, "ffxCreateContext");
    fsr.destroy = (PfnFfxDestroyContext)load_symbol(fsr.library, "ffxDestroyContext");
    fsr.query = (PfnFfxQuery)load_symbol(fsr.library, "ffxQuery");
    fsr.dispatch = (PfnFfxDispatch)load_symbol(fsr.library, "ffxDispatch");
    fsr.available = fsr.create && fsr.destroy && fsr.query && fsr.dispatch;
    if (!fsr.available) fprintf(stderr, "[fsr] provider %s lacks FFX exports\n", path);
    return fsr.available;
}

// Returns the upscaled view, or VK_NULL_HANDLE so the caller keeps FSR 1/bilinear.
extern "C" VkImageView mk8_fsr4_upscale(VkCommandBuffer cb, TexBackend *color, TexBackend *depth, uint32_t rw, uint32_t rh,
                             uint32_t ow, uint32_t oh, float frame_ms) {
    // MK8_DLSS selects the optional NGX bridge. It returns VK_NULL_HANDLE with one reasoned
    // log when it cannot run, and the path below then applies unchanged.
    if (getenv("MK8_DLSS")) {
        VkImageView dlss_view = mk8_dlss_upscale(cb, color, depth, rw, rh, ow, oh, frame_ms);
        if (dlss_view) return dlss_view;
    }
    // MK8_XESS is the Windows XeSS bridge (Deliberately a stub on Linux, where Intel ships no library).
    if (getenv("MK8_XESS")) {
        VkImageView xess_view = mk8_xess_upscale(cb, color, depth, rw, rh, ow, oh, frame_ms);
        if (xess_view) return xess_view;
    }
    if (!getenv("MK8_FSR4")) return VK_NULL_HANDLE;  // diagnostic switch until the settings/UI land
    if (getenv("MK8_FSR_FG")) {  // native frame generation takes the final image; combining is not finished
        static int once;
        if (!once++) fprintf(stderr, "[fsr] MK8_FSR_FG is set: the FSR upscaler stays off (the frame-generation path uses the native image)\n");
        return VK_NULL_HANDLE;
    }
    static int seen;
    if (!seen++) fprintf(stderr, "[fsr] present %ux%u -> %ux%u color %dx%d\n", rw, rh, ow, oh,
                         color ? (int)color->width : -1, color ? (int)color->height : -1);
    if (!init()) return VK_NULL_HANDLE;
    if (fsr.create_failed) return VK_NULL_HANDLE;
    if (!color || !color->image) return VK_NULL_HANDLE;
    if (ow <= rw + 1 || oh <= rh + 1) return VK_NULL_HANDLE;  // an upscale only
    if (fsr.context && (fsr.rw != rw || fsr.rh != rh || fsr.ow != ow || fsr.oh != oh)) {
        fsr.destroy(&fsr.context, NULL);
        fsr.context = NULL;
        destroy_images();
    }
    if (!fsr.context) {
        if (!create_images(rw, rh, ow, oh) || !create_context(rw, rh, ow, oh)) return VK_NULL_HANDLE;
        create_mv_resources();
        fsr.rw = rw, fsr.rh = rh, fsr.ow = ow, fsr.oh = oh;
    }
    if (!fsr.primed) {
        // One-time initialisation of the neutral inputs and the output layout.
        barrier(cb, fsr.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearDepthStencilValue clear_depth = {1.0f, 0};
        VkImageSubresourceRange depth_range = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(cb, fsr.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_depth, 1, &depth_range);
        barrier(cb, fsr.depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue clear_motion = {{0, 0, 0, 0}};
        VkImageSubresourceRange motion_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, fsr.motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear_motion, 1, &motion_range);
        barrier(cb, fsr.motion, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        barrier(cb, fsr.output, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        fsr.primed = true;
    }
    // The provider expects the colour/depth/motion inputs in SHADER_READ_ONLY and the output in GENERAL.
    barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkImageCreateInfo color_ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    color_ci.imageType = VK_IMAGE_TYPE_2D;
    color_ci.format = color->format;
    color_ci.extent = (VkExtent3D){rw, rh, 1};
    color_ci.mipLevels = 1;
    color_ci.arrayLayers = 1;
    color_ci.samples = VK_SAMPLE_COUNT_1_BIT;
    color_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    color_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    color_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkImage depth_image = fsr.depth;
    VkFormat depth_format = VK_FORMAT_D32_SFLOAT;
    VkImageAspectFlags depth_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    bool real_depth = depth && depth->image && depth->width == rw && depth->height == rh;
    static int depth_logged;
    if (real_depth) {
        depth_image = depth->image;
        depth_format = depth->format;
        depth_aspect = (depth->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
        barrier(cb, depth_image, depth_aspect, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    }
    static int depth_state = -1;
    if ((int)real_depth != depth_state) {
        depth_state = real_depth;
        fprintf(stderr, "[fsr] depth now %s (%ux%u fmt %d)\n", real_depth ? "game" : "neutral",
                real_depth ? depth->width : rw, real_depth ? depth->height : rh,
                real_depth ? depth_format : VK_FORMAT_D32_SFLOAT);
    }
    if (!depth_logged++) fprintf(stderr, "[fsr] depth source %s %ux%u fmt %d\n",
                                 real_depth ? "game" : "neutral", real_depth ? depth->width : rw,
                                 real_depth ? depth->height : rh, real_depth ? depth_format : VK_FORMAT_D32_SFLOAT);
    VkImageCreateInfo depth_ci = color_ci;
    depth_ci.format = depth_format;
    depth_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo motion_ci = color_ci;
    motion_ci.format = VK_FORMAT_R16G16_SFLOAT;
    motion_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo output_ci = color_ci;
    output_ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    output_ci.extent = (VkExtent3D){ow, oh, 1};
    output_ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    ffxDispatchDescUpscale desc = {0};
    desc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    desc.commandList = cb;
    desc.color = ffxApiGetResourceVK(color->image,
        ffxApiGetImageResourceDescriptionVK(color->image, color_ci, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.depth = ffxApiGetResourceVK(depth_image,
        ffxApiGetImageResourceDescriptionVK(depth_image, depth_ci, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.output = ffxApiGetResourceVK(fsr.output,
        ffxApiGetImageResourceDescriptionVK(fsr.output, output_ci, FFX_API_RESOURCE_USAGE_UAV),
        FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.renderSize = {rw, rh};
    desc.upscaleSize = {ow, oh};
    bool mv = real_depth && fsr.mv_pipeline && run_mv(cb, depth, rw, rh);
    static int motion_state = -1;
    if ((int)mv != motion_state) {
        motion_state = mv;
        fprintf(stderr, "[fsr] motion now %s\n", mv ? "camera" : "neutral");
    }
    desc.motionVectors = ffxApiGetResourceVK(fsr.motion,
        ffxApiGetImageResourceDescriptionVK(fsr.motion, motion_ci, FFX_API_RESOURCE_USAGE_READ_ONLY),
        FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    desc.motionVectorScale = mv ? (struct FfxApiFloatCoords2D){1.0f, 1.0f}
                                : (struct FfxApiFloatCoords2D){(float)rw, (float)rh};
    desc.jitterOffset = {g_fsr_jitter_x, g_fsr_jitter_y};
    desc.enableSharpening = true;
    desc.sharpness = 0.2f;
    desc.frameTimeDelta = frame_ms > 0 ? frame_ms : 16.7f;
    desc.preExposure = 1.0f;
    desc.reset = !mv;  // real camera motion keeps history; the neutral image resets every frame
    desc.cameraNear = 0.1f;
    desc.cameraFar = 1000.0f;
    desc.cameraFovAngleVertical = 1.0472f;
    desc.viewSpaceToMetersFactor = 1.0f;
    desc.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    if (fsr.dispatch(&fsr.context, &desc.header) != FFX_API_RETURN_OK) {
        static int warned;
        if (warned++ < 3) fprintf(stderr, "[fsr] dispatch failed\n");
        return VK_NULL_HANDLE;
    }
    static int logged;
    if (!logged++) fprintf(stderr, "[fsr] upscale active %ux%u -> %ux%u (depth %s, motion %s) color %p output %p\n",
                           rw, rh, ow, oh, real_depth ? "game" : "neutral", mv ? "camera" : "neutral",
                           (void *)color->image, (void *)fsr.output);
    static int timings;
    if (timings++ % 120 == 0) {
        static uint64_t last;
        uint64_t now = (uint64_t)(clock() * 1000000ull / CLOCKS_PER_SEC);
        if (last) fprintf(stderr, "[fsr] cpu dispatch %.2f ms\n", (now - last) / 1e6 / 120.0);
        last = now;
    }
    // The provider samples the colour and depth inputs and leaves them in a read layout; the game's
    // next pass uses the colour backbuffer (and, with real depth, the captured depth image) as
    // render targets in GENERAL. Restore them after the dispatch so the next pass starts cleanly.
    barrier(cb, color->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    if (real_depth)
        barrier(cb, depth_image, depth_aspect, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    return fsr.output_view;
}
