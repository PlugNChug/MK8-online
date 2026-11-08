"""Diagnostics for VRAM and save-file errors seen in long sessions.

VRAM: the dedicated GPU memory Windows reports rose by ~256 MB steps per new track (668, 932, 1189, 1453 MB in one
Grand Prix), the size of a Vulkan Memory Allocator block, so it counts reserved blocks, not what is in use.
MK8_TRACE_VRAM=1 logs, every 10 s: per device-local heap the bytes in blocks and in allocations, the driver's
usage/budget, the vertex/index cache (exec.c VC_BUDGET) and the live game textures.

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
        LOG("[mk8:vram] heap %u: %.0f MB in %u blocks, %.0f MB used by %u allocations; driver usage %.0f of %.0f MB; "
            "vertex cache %.0f MB; %d live textures\\n", i, s->blockBytes / 1048576.0, s->blockCount,
            s->allocationBytes / 1048576.0, s->allocationCount, budget[i].usage / 1048576.0, budget[i].budget / 1048576.0,
            vc_bytes / 1048576.0, mk8_live_textures());
    }
}
'''
LIVE = '''static void live_remove(TexBackend *b) {'''
LIVE_COUNT = '''int mk8_live_textures(void) { return g_nlive; }  // MK8 (toolchain/vram_trace.py)
''' + LIVE

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
    if text.count(LIVE) != 1:
        raise ValueError('Live texture list boundary changed upstream')
    textures.write_text(text.replace(LIVE, LIVE_COUNT), encoding='utf-8')
    fs = runtime/'services/fs.c'
    text = fs.read_text(encoding='utf-8')
    if text.count(FS_END) != 1:
        raise ValueError('Filesystem service boundary changed upstream')
    fs.write_text(text.replace(FS_END, FS_LOGGED), encoding='utf-8')
