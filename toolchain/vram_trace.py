"""Diagnostics for VRAM and save-file errors seen in long sessions.

VRAM: the dedicated GPU memory Windows reports rose by ~256 MB steps per new track (668, 932, 1189, 1453 MB in one
Grand Prix), the size of a Vulkan Memory Allocator block, so it counts reserved blocks, not what is in use.
MK8_TRACE_VRAM=1 logs, every 10 s: per device-local heap the bytes in blocks and in allocations, VMA's estimated
usage/budget (the allocator does not enable EXT_memory_budget), the vertex/index cache and the live game images; then the listed textures by the
frame of their last use (this second, this minute, older) with their memory, and the shapes of the older ones. Only
with the variable set is a texture's last use recorded (one store per lookup).

Save files: an automated Grand Prix ended with the game's own abort right after nn::fs::CreateDirectory; the
filesystem service did not log failures. Every failed IFileSystem command is now logged with its path and errno.
"""

STATS = '''    stat_feedback_copies = stat_upload_bytes = 0;
    {
        extern void vk_alias_stats(void);
        vk_alias_stats();
    }
'''
STATS_TRACED = STATS + '''    if (getenv("MK8_TRACE_VRAM")) {  // MK8 (toolchain/vram_trace.py): every fifth stats line (10 s)
        static int every;
        extern void mk8_vram_report(void);
        if (every++ % 5 == 0) mk8_vram_report();
    }
'''
REPORT = '''
// MK8 (toolchain/vram_trace.py)
void mk8_vram_report(void) {
    VmaTotalStatistics st;
    vmaCalculateStatistics(g_vk.vma, &st);
    VmaBudget budget[VK_MAX_MEMORY_HEAPS];
    memset(budget, 0, sizeof budget);
    vmaGetHeapBudgets(g_vk.vma, budget);
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    extern int mk8_live_textures(void);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++) {
        if (!(mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
        const VmaStatistics *s = &st.memoryHeap[i].statistics;
        LOG("[mk8:vram] heap %u: %.0f MB in %u blocks, %.0f MB used by %u allocations; VMA estimate %.0f of %.0f MB; "
            "vertex cache %.0f MB; %d live textures\\n", i, s->blockBytes / 1048576.0, s->blockCount,
            s->allocationBytes / 1048576.0, s->allocationCount, budget[i].usage / 1048576.0, budget[i].budget / 1048576.0,
            vc_bytes / 1048576.0, mk8_live_textures());
    }
    extern void mk8_texture_age_report(void);
    static int every;
    if (every++ % 3 == 0) mk8_texture_age_report();  // (every 30 s)
}
'''
LIVE = '''static void live_remove(TexBackend *b) {'''
LIVE_COUNT = '''int mk8_unlisted_images(uint64_t *bytes);
int mk8_live_textures(void) {  // MK8 (toolchain/vram_trace.py): allocated images, not retired backend records
    int n = 0;
    for (int i = 0; i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (b && b->mem) n++;
    }
    uint64_t bytes = 0;
    return n + mk8_unlisted_images(&bytes);
}
static int mk8_vram_traced(void) {
    static int on = -1;
    if (on < 0) on = getenv("MK8_TRACE_VRAM") != NULL;
    return on;
}
// Live textures by their last use; the older ones by shape (MK8_TRACE_VRAM, translation thread).
void mk8_texture_age_report(void) {
    uint64_t now = renderer_present_count();
    uint64_t count[3] = {0}, bytes[3] = {0};
    struct { uint32_t w, h, fmt, layers; uint64_t n, bytes; } shapes[256];
    int nshapes = 0;
    int listed = 0;
    for (int i = 0; i < g_nlive; i++) {
        TexBackend *b = g_live[i]->backend;
        if (!b || !b->mem) continue;
        VmaAllocationInfo info;
        vmaGetAllocationInfo(g_vk.vma, b->mem, &info);
        uint64_t age = now > b->mk8_used ? now - b->mk8_used : 0;
        int k = age <= 60 ? 0 : age <= 3600 ? 1 : 2;
        count[k]++;
        bytes[k] += info.size;
        if (k != 2) continue;
        {
            if (listed < 16 && info.size >= (4u << 20)) {
                listed++;
                LOG("[mk8:vram]   older texture storage %llx %ux%u, %llu bytes\\n", (unsigned long long)g_live[i]->addr,
                    b->width, b->height, (unsigned long long)info.size);
            }
        }
        int s = 0;
        while (s < nshapes && !(shapes[s].w == b->width && shapes[s].h == b->height && shapes[s].fmt == (uint32_t)b->format &&
                                shapes[s].layers == b->layers)) s++;
        if (s == nshapes && nshapes < (int)(sizeof shapes / sizeof shapes[0]))
            shapes[nshapes++] = (typeof(shapes[0])){b->width, b->height, (uint32_t)b->format, b->layers, 0, 0};
        if (s < nshapes) { shapes[s].n++; shapes[s].bytes += info.size; }
    }
    LOG("[mk8:vram] listed textures used this second: %llu (%.0f MB); this minute: %llu (%.0f MB); older: %llu (%.0f MB)\\n",
        (unsigned long long)count[0], bytes[0] / 1048576.0, (unsigned long long)count[1], bytes[1] / 1048576.0,
        (unsigned long long)count[2], bytes[2] / 1048576.0);
    uint64_t unlisted_bytes = 0;
    int unlisted = mk8_unlisted_images(&unlisted_bytes);
    LOG("[mk8:vram] shared images without original owner: %d (%.0f MB)\\n", unlisted, unlisted_bytes / 1048576.0);
    for (int s = 0; s < nshapes; s++)
        if (shapes[s].bytes >= (4u << 20))
            LOG("[mk8:vram]   older: %ux%u vk format %u x%u layers: %llu textures, %.0f MB\\n", shapes[s].w, shapes[s].h,
                shapes[s].fmt, shapes[s].layers, (unsigned long long)shapes[s].n, shapes[s].bytes / 1048576.0);
}
''' + LIVE
UNLISTED = '''
// The first owner can be finalized while another texture still references the backend (no g_live entry).
int mk8_unlisted_images(uint64_t *bytes) {
    int n = 0;
    for (unsigned k = 0; k < ALIAS_BUCKETS; k++) {
        for (Alias *a = g_alias[k]; a; a = a->next) {
            TexBackend *b = a->b;
            if (b->reg || !b->mem) continue;
            VmaAllocationInfo info;
            vmaGetAllocationInfo(g_vk.vma, b->mem, &info);
            *bytes += info.size;
            n++;
        }
    }
    return n;
}
'''
USE = '''static TexBackend *tex_backend(NvnTexture *t) {
    TexBackend *b = t->backend;
    if (b) {
'''
USED = USE + '''        if (mk8_vram_traced()) b->mk8_used = renderer_present_count();  // MK8 (toolchain/vram_trace.py)
'''
FIELD = '''    int refs;  // texture objects sharing it (aliases of the same storage: textures.c tex_backend)
'''
FIELDED = FIELD + '''    uint64_t mk8_used;  // MK8 (toolchain/vram_trace.py): frame of the last lookup, with MK8_TRACE_VRAM
'''

POOL = '''NVN(nvnMemoryPoolFinalize) {
    NvnPool *p = HOST(NvnPool, A(0));
'''
POOL_TRACED = POOL + '''    if (p && getenv("MK8_TRACE_VRAM"))  // MK8 (toolchain/vram_trace.py)
        LOG("[mk8:vram] pool finalized: storage %llx size %llx\\n", (unsigned long long)p->storage, (unsigned long long)p->size);
'''

FS_END = '''    default: LOG("[fs] IFileSystem cmd %u (ok)\\n", req->cmd);
    }
}

static Service *open_host_fs('''
FS_LOGGED = '''    default: LOG("[fs] IFileSystem cmd %u (ok)\\n", req->cmd);
    }
    if (resp->result) {  // MK8 (toolchain/vram_trace.py): a failure the game may abort on
        static int logged;
        if (logged++ < 200) LOG("[fs] IFileSystem cmd %u %s -> result %#x (errno %d)\\n", req->cmd, p, resp->result, errno);
    }
}

static Service *open_host_fs('''


def prepare(runtime):
    exec_c = runtime/'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if text.count(STATS) != 1 or text.count('static uint64_t vc_bytes;') != 1:
        raise ValueError('Renderer statistics boundary changed upstream')
    exec_c.write_text(text.replace(STATS, STATS_TRACED) + REPORT, encoding='utf-8')
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    if text.count(LIVE) != 1 or text.count(USE) != 1:
        raise ValueError('Live texture list boundary changed upstream')
    textures.write_text(text.replace(LIVE, LIVE_COUNT).replace(USE, USED) + UNLISTED, encoding='utf-8')
    header = runtime/'nvn/vulkan/vk.h'
    text = header.read_text(encoding='utf-8')
    if text.count(FIELD) != 1:
        raise ValueError('Texture backend boundary changed upstream')
    header.write_text(text.replace(FIELD, FIELDED), encoding='utf-8')
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    if text.count(POOL) != 1:
        raise ValueError('Memory pool boundary changed upstream')
    objects.write_text(text.replace(POOL, POOL_TRACED), encoding='utf-8')
    fs = runtime/'services/fs.c'
    text = fs.read_text(encoding='utf-8')
    if text.count(FS_END) != 1:
        raise ValueError('Filesystem service boundary changed upstream')
    fs.write_text(text.replace(FS_END, FS_LOGGED), encoding='utf-8')
