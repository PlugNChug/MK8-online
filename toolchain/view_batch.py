"""Create a pass's attachment views in parallel (runtime only; no guest code or ABI change).

The first race frame creates 2,221 image views; at ~0.95 ms each that is 1.19 s of the course-entry hitch (19/11/2026,
LARECOMP_VK_PROF: frame 2153, 1,372 ms exec, 223 pipeline compiles but only 85 ms of draw waits). `begin_pass` resolved
and created the views one by one on the render thread.

`mk8_views_prefetch` resolves the colour and depth attachments of the pass and sends their view descriptions to four
worker threads, which call `vk_image_view` in parallel; `begin_pass` uses the results. The view table takes a mutex
only around lookup/insert (creation is outside); a duplicate created by two threads is destroyed and the cached one
returned, and a worker that finds the table full returns its view without caching instead of evicting. The old path is
kept: an unresolved attachment, a failed spec, or `MK8_VIEW_BATCH=0` calls `vk_view` as before.

Texture uploads are not moved: they are tied to the submission's staging commands (upload_texture/vk_transient) and
stay on the render thread; the first-visit profile attributes 1.19 s to views and ~49-63 ms to texture binding.
"""
from pathlib import Path

VIEW_OLD = '''static void views_forget(TexBackend *b) {
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) return e->view;
        if (!e->b) break;
    }
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    ViewEnt *e = &view_tab[slot];
    if (e->b) { defer((Dead){.view = e->view}); vk_view_gen++; }  // evict (16 probes taken)
    *e = (ViewEnt){b, k1, k2, v};
    return v;
}
'''

VIEW_NEW = '''static pthread_mutex_t view_lock = PTHREAD_MUTEX_INITIALIZER;  // MK8 (toolchain/view_batch.py): view_tab
static _Thread_local int vb_worker;  // (a pool worker: no eviction, create without caching if the table is full)

static void views_forget(TexBackend *b) {
    pthread_mutex_lock(&view_lock);
    vk_view_gen++;
    for (int i = 0; i < VIEW_TAB; i++)
        if (view_tab[i].b == b) { if (view_tab[i].view) defer((Dead){.view = view_tab[i].view}); view_tab[i].b = NULL; view_tab[i].view = VK_NULL_HANDLE; }
    pthread_mutex_unlock(&view_lock);
}

VkImageView vk_image_view(TexBackend *b, VkImageViewType type, VkFormat fmt, uint32_t lv0, uint32_t nlv, uint32_t ly0,
                          uint32_t nly, VkComponentMapping sw, VkImageAspectFlags aspect) {
    uint64_t k1 = (uint64_t)type | (uint64_t)fmt << 8 | (uint64_t)lv0 << 40 | (uint64_t)nlv << 48 | (uint64_t)aspect << 56;
    uint64_t k2 = (uint64_t)ly0 | (uint64_t)nly << 16 | (uint64_t)sw.r << 32 | (uint64_t)sw.g << 40 | (uint64_t)sw.b << 48 | (uint64_t)sw.a << 56;
    uint64_t h = ((uint64_t)(uintptr_t)b * 0x9E3779B97F4A7C15ull) ^ (k1 * 0xC2B2AE3D27D4EB4Full) ^ (k2 * 0x165667B19E3779F9ull);
    uint32_t slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    pthread_mutex_lock(&view_lock);
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) { VkImageView r = e->view; pthread_mutex_unlock(&view_lock); return r; }
        if (!e->b) break;
    }
    pthread_mutex_unlock(&view_lock);
    extern uint64_t prof_views_created;
    prof_views_created++;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = b->image, .viewType = type,
                                .format = fmt, .components = sw, .subresourceRange = {aspect, lv0, nlv, ly0, nly}};
    VkImageView v;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) {
        static int nfail;
        if (nfail++ < 20) LOG("[vk] view failed: type %d fmt %d (image fmt %d) levels %u+%u layers %u+%u\\n", type, fmt, b->format, lv0, nlv, ly0, nly);
        return VK_NULL_HANDLE;
    }
    pthread_mutex_lock(&view_lock);
    slot = (uint32_t)(h ^ (h >> 32)) & (VIEW_TAB - 1);
    ViewEnt *free_e = NULL;
    for (int probe = 0; probe < 16; probe++, slot = (slot + 1) & (VIEW_TAB - 1)) {
        ViewEnt *e = &view_tab[slot];
        if (e->b == b && e->k1 == k1 && e->k2 == k2) {  // (another thread created it meanwhile)
            VkImageView r = e->view;
            pthread_mutex_unlock(&view_lock);
            vkDestroyImageView(g_vk.dev, v, NULL);
            return r;
        }
        if (!e->b) { free_e = e; break; }
        free_e = e;
    }
    if (!free_e) { pthread_mutex_unlock(&view_lock); return v; }
    if (free_e->b) {
        if (vb_worker) { pthread_mutex_unlock(&view_lock); return v; }  // (no eviction off the render thread)
        if (free_e->view) defer((Dead){.view = free_e->view});
        vk_view_gen++;
    }
    *free_e = (ViewEnt){b, k1, k2, v};
    pthread_mutex_unlock(&view_lock);
    return v;
}
'''

BATCH = r'''
// MK8 (toolchain/view_batch.py): the pass's attachment views, created on four workers. The render thread is blocked in
// mk8_views_prefetch while they run, so nothing else touches the view table except the workers (the mutex above still
// covers the lookup/insert, and a duplicate create is destroyed).
typedef struct {
    TexBackend *b;
    VkImageViewType type;
    VkFormat fmt;
    uint32_t lv0, nlv, ly0, nly;
    VkComponentMapping sw;
    VkImageAspectFlags aspect;
} ViewSpec;
static pthread_mutex_t vb_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vb_work = PTHREAD_COND_INITIALIZER, vb_done = PTHREAD_COND_INITIALIZER;
static ViewSpec *vb_specs;
static VkImageView *vb_views;
static int vb_n, vb_next, vb_done_n, vb_started;

static void *vb_worker_fn(void *arg) {
    (void)arg;
    vb_worker = 1;
    for (;;) {
        pthread_mutex_lock(&vb_lock);
        while (!vb_n) pthread_cond_wait(&vb_work, &vb_lock);
        int i = vb_next++, n = vb_n;
        if (i >= n) { pthread_mutex_unlock(&vb_lock); continue; }
        ViewSpec s = vb_specs[i];
        pthread_mutex_unlock(&vb_lock);
        VkImageView v = vk_image_view(s.b, s.type, s.fmt, s.lv0, s.nlv, s.ly0, s.nly, s.sw, s.aspect);
        vb_views[i] = v;
        pthread_mutex_lock(&vb_lock);
        if (++vb_done_n == n) { vb_n = 0; pthread_cond_broadcast(&vb_done); }
        pthread_mutex_unlock(&vb_lock);
    }
}

static void vb_batch(ViewSpec *specs, const int *idx, int n, VkImageView *out) {
    VkImageView views[9] = {0};
    if (n <= 1) {
        for (int i = 0; i < n; i++) out[idx[i]] = vk_image_view(specs[i].b, specs[i].type, specs[i].fmt, specs[i].lv0,
                                                                 specs[i].nlv, specs[i].ly0, specs[i].nly, specs[i].sw, specs[i].aspect);
        return;
    }
    pthread_mutex_lock(&vb_lock);
    if (!vb_started) {
        vb_started = 1;
        for (int i = 0; i < 4; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, vb_worker_fn, NULL) == 0) pthread_detach(t);
        }
    }
    vb_specs = specs;
    vb_views = views;
    vb_n = n;
    vb_next = 0;
    vb_done_n = 0;
    pthread_cond_broadcast(&vb_work);
    while (vb_n) pthread_cond_wait(&vb_done, &vb_lock);
    pthread_mutex_unlock(&vb_lock);
    for (int i = 0; i < n; i++) out[idx[i]] = views[i];
}

// A colour/depth attachment's render view (one level, one layer), without creating it: vk_view's render branch.
static bool vk_render_spec(NvnTexture *t, const NvnTextureView *v, bool has, ViewSpec *s) {
    TexBackend *b = t ? t->backend : NULL;
    if (!b || !b->image) return false;
    if (!has) v = NULL;
    VkFormat fmt = b->format;
    if (v && (v->setmask & 4) && !(b->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        VkFormat vf = fmt_of(v->format).vf;
        if (vk_format(t->format) == b->format) fmt = vf;
    }
    uint32_t lv0 = v && (v->setmask & 1) ? v->base_level : 0;
    uint32_t nlv = v && (v->setmask & 1) && v->num_levels ? v->num_levels : b->levels - (lv0 < b->levels ? lv0 : 0);
    uint32_t ly0 = v && (v->setmask & 2) ? v->min_layer : 0;
    uint32_t nly = v && (v->setmask & 2) && v->num_layers ? v->num_layers : b->layers - (ly0 < b->layers ? ly0 : 0);
    if (lv0 >= b->levels) lv0 = b->levels - 1;
    if (lv0 + nlv > b->levels) nlv = b->levels - lv0;
    if (ly0 >= b->layers) ly0 = b->layers - 1;
    if (ly0 + nly > b->layers) nly = b->layers - ly0;
    uint32_t ly = ly0;
    if (b->base_type == VK_IMAGE_VIEW_TYPE_3D) {
        uint32_t d = b->depth >> lv0 ? b->depth >> lv0 : 1;
        uint32_t slice = v && (v->setmask & 2) ? v->min_layer : 0;
        if (slice >= d) slice = d - 1;
        ly = slice;
    }
    s->b = b;
    s->type = VK_IMAGE_VIEW_TYPE_2D;
    s->fmt = fmt;
    s->lv0 = lv0;
    s->nlv = 1;
    s->ly0 = ly;
    s->nly = 1;
    s->sw = IDENTITY;
    s->aspect = b->aspect;
    return true;
}

void mk8_views_prefetch(NvnTexture **color, int ncolor, const NvnTextureView *cview, const uint8_t *has,
                        NvnTexture *depth, const NvnTextureView *dview, uint8_t dhas, VkImageView *out) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("MK8_VIEW_BATCH"); off = e && atoi(e) == 0; }
    if (off) return;
    ViewSpec specs[9];
    int idx[9], n = 0;
    for (int i = 0; i < ncolor && i < 8; i++) {
        if (!color[i]) continue;
        tex_backend(color[i]);
        if (vk_render_spec(color[i], &cview[i], has[i] != 0, &specs[n])) { idx[n] = i; n++; }
    }
    if (depth) {
        tex_backend(depth);
        if (vk_render_spec(depth, dview, dhas != 0, &specs[n])) { idx[n] = 8; n++; }
    }
    if (n) vb_batch(specs, idx, n, out);
}
'''

PROTO = 'extern void mk8_views_prefetch(NvnTexture **, int, const NvnTextureView *, const uint8_t *, NvnTexture *, const NvnTextureView *, uint8_t, VkImageView *);  // MK8 (toolchain/view_batch.py)\n'

PASS_ANCHOR = '    g_pass_depth = st.rt.depth;\n'
PASS_NEW = PASS_ANCHOR + '''    VkImageView mk8_batch_views[9] = {0};  // MK8 (toolchain/view_batch.py)
    mk8_views_prefetch(st.rt.color, st.rt.ncolor, st.rt.cview, st.rt.has_cview, st.rt.depth, &st.rt.dview, st.rt.has_dview, mk8_batch_views);
'''

COLOR_OLD = '''        renderer_mark_gpu_owned(t);
        VkImageView v = vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);
'''
COLOR_NEW = '''        renderer_mark_gpu_owned(t);
        VkImageView v = mk8_batch_views[i] ? mk8_batch_views[i] : vk_view(t, &st.rt.cview[i], st.rt.has_cview[i], true, TEXTYPE_2D);  // MK8 (toolchain/view_batch.py)
'''

DEPTH_OLD = '''        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);
'''
DEPTH_NEW = '''        renderer_mark_gpu_owned(st.rt.depth);
        VkImageView v = mk8_batch_views[8] ? mk8_batch_views[8] : vk_view(st.rt.depth, &st.rt.dview, st.rt.has_dview, true, TEXTYPE_2D);  // MK8 (toolchain/view_batch.py)
'''


def prepare(runtime: Path):
    textures = runtime / 'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    if 'mk8_views_prefetch' not in text:
        if text.count(VIEW_OLD) != 1:
            raise ValueError('View cache boundary changed upstream')
        text = text.replace(VIEW_OLD, VIEW_NEW, 1)
        tail = '// A colour texture sampled with depth comparison'
        if text.count(tail) != 1:
            raise ValueError('vk_view boundary changed upstream')
        text = text.replace(tail, BATCH + '\n' + tail, 1)
        textures.write_text(text, encoding='utf-8')
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if 'mk8_views_prefetch' in text:
        return
    if text.count('static void begin_pass(void) {\n') != 1:
        raise ValueError('begin_pass boundary changed upstream')
    text = text.replace('static void begin_pass(void) {\n', PROTO + 'static void begin_pass(void) {\n', 1)
    for old, new in ((PASS_ANCHOR, PASS_NEW), (COLOR_OLD, COLOR_NEW), (DEPTH_OLD, DEPTH_NEW)):
        if text.count(old) != 1:
            raise ValueError('begin_pass attachment boundary changed upstream: ' + old.strip()[:60])
        text = text.replace(old, new, 1)
    exec_c.write_text(text, encoding='utf-8')
