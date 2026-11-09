"""Retire finalized pools' images in command order, including shared storage backends.

An image can outlive its original NvnTexture: aliases still reference its backend, which then disappears from
g_live but remains in g_alias. Release the image, not the backend or those references. A subsequent use recreates
and uploads it. GPU destruction uses the existing submission fence. MK8_RELEASE_POOL_TEXTURES=1 enables this
experiment; MK8_KEEP_POOL_TEXTURES overrides it. It remains off: a four-track comparison reduced some retained
allocations but increased the peak reservation and showed a possible FPS regression on Thwomp Ruins.
"""

POOL = '''NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
'''
POOL_RELEASED = POOL + '''    if (p) mk8_pool_released(p->storage, p->size);  // MK8 (toolchain/pool_release.py)
'''
QUEUE_RELEASE = '''
static void mk8_release_pool_now(uint64_t lo, uint64_t hi) {
    extern int mk8_textures_release_range(uint64_t lo, uint64_t hi, uint64_t *bytes);
    static uint64_t total_images, total_bytes;
    static unsigned logged;
    uint64_t bytes = 0;
    int n = mk8_textures_release_range(lo, hi, &bytes);
    if (!n) return;
    total_images += (uint64_t)n;
    total_bytes += bytes;
    if (logged < 30 || logged % 200 == 0)
        LOG("[mk8:pools] finalized pool %llx+%llx: %d images retired (%.1f MB); %llu (%.0f MB) so far\\n",
            (unsigned long long)lo, (unsigned long long)(hi - lo), n, bytes / 1048576.0,
            (unsigned long long)total_images, total_bytes / 1048576.0);
    logged++;
}

static void mk8_pool_released(uint64_t storage, uint64_t size) {
    const char *enabled = getenv("MK8_RELEASE_POOL_TEXTURES");
    if (!enabled || atoi(enabled) == 0 || getenv("MK8_KEEP_POOL_TEXTURES") || !size) return;
    if (size > UINT64_MAX - storage) FATAL("Memory pool range overflow\\n");
    if (!mk8_async_on()) { mk8_release_pool_now(storage, storage + size); return; }
    Mk8QItem *it = calloc(1, sizeof *it);
    if (!it) FATAL("Memory pool retirement allocation failed\\n");
    it->kind = MK8_Q_POOL_RELEASE;
    it->pool_lo = storage;
    it->pool_hi = storage + size;
    mk8_q_put(it, NULL, 0);
}
'''

RESET = 'void vk_textures_reset(void) {\n'
RANGE = '''// MK8 (toolchain/pool_release.py): keep shared backend pointers valid; only their GPU allocations retire.
static int mk8_pool_image_retire(TexBackend *b, uint64_t *bytes) {
    if (!b || !b->image) return 0;
    if (b->mem) {
        VmaAllocationInfo info;
        vmaGetAllocationInfo(g_vk.vma, b->mem, &info);
        *bytes += info.size;
    }
    vk_end_pass();
    destroy_image(b);  // views and image survive until the pending GPU submission completes
    b->uploaded = b->gpu_owned = false;
    return 1;
}

int mk8_textures_release_range(uint64_t lo, uint64_t hi, uint64_t *bytes) {
    if (hi <= lo) return 0;
    extern void mk8_pool_copy_aliases(uint64_t lo, uint64_t hi);
    mk8_pool_copy_aliases(lo, hi);
    int n = 0;
    // Includes backends whose first owner was finalized while another alias kept the image alive.
    for (unsigned k = 0; k < ALIAS_BUCKETS; k++)
        for (Alias *a = g_alias[k]; a; a = a->next)
            if (a->addr >= lo && a->addr < hi) n += mk8_pool_image_retire(a->b, bytes);
    // Non-aliased textures have no alias-table entry. Shared entries already have image == NULL.
    for (int i = 0; i < g_nlive; i++) {
        NvnTexture *t = g_live[i];
        if (t->addr >= lo && t->addr < hi) n += mk8_pool_image_retire(t->backend, bytes);
    }
    return n;
}

''' + RESET

BACKEND = '''static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
'''
RECREATE = BACKEND + '''        if (!b->image) {  // MK8 (toolchain/pool_release.py): reuse after pool retirement
            b->uploaded = b->gpu_owned = b->dynamic = b->presented = false;
            b->data_hash = b->sample_hash = b->checked_frame = b->full_frame = 0;
            b->wgen = b->level_gen = 0;
            b->touched_frame = b->frame_cleared = 0;
            create_image(t, b);
        }
'''

COPY_ALIASES = '''// Resolve deferred depth copies before either allocation is retired, including destinations in another pool.
void mk8_pool_copy_aliases(uint64_t lo, uint64_t hi) {
    for (int i = naliases - 1; i >= 0; i--) {
        CopyAlias *a = &aliases[i];
        if ((a->src->addr >= lo && a->src->addr < hi) || (a->dst->addr >= lo && a->dst->addr < hi)) {
            if (alias_valid(a)) alias_copy(a, ((TexBackend *)a->dst->backend)->aspect, "pool finalized");
            alias_drop(i);
        }
    }
}
'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Pool release boundary changed: {old[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    for old, new in [
        ('MK8_Q_SUBMIT, MK8_Q_PRESENT, MK8_Q_FENCE', 'MK8_Q_SUBMIT, MK8_Q_PRESENT, MK8_Q_FENCE, MK8_Q_POOL_RELEASE'),
        ('    uint64_t ticket, *handles;', '    uint64_t ticket, *handles;\n    uint64_t pool_lo, pool_hi;'),
        ('static void *mk8_q_worker(void *arg) {', 'static void mk8_release_pool_now(uint64_t lo, uint64_t hi);\nstatic void *mk8_q_worker(void *arg) {'),
        ('        } else {\n            // (the open submission',
         '        } else if (it->kind == MK8_Q_POOL_RELEASE) {\n            mk8_release_pool_now(it->pool_lo, it->pool_hi);\n        } else {\n            // (the open submission'),
        ('void renderer_wait_seq(uint64_t seq);', QUEUE_RELEASE + '\nvoid renderer_wait_seq(uint64_t seq);'),
        (POOL, POOL_RELEASED),
    ]:
        text = replace(text, old, new)
    objects.write_text(text, encoding='utf-8')
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    text = replace(text, RESET, RANGE)
    text = replace(text, BACKEND, RECREATE)
    textures.write_text(text, encoding='utf-8')
    execute = runtime/'nvn/vulkan/exec.c'
    text = execute.read_text(encoding='utf-8')
    boundary = 'void vk_alias_reset(void) { naliases = 0; }'
    execute.write_text(replace(text, boundary, COPY_ALIASES + boundary), encoding='utf-8')
