"""Give back the GPU memory of texture images no draw used for a minute, when their contents can be had again.

The game abandons texture objects without finalizing them: render targets it makes again over the same storage at
another size (38 sizes in a 50-minute session) and textures of earlier screens. On the console an abandoned NVN texture
costs nothing; here each keeps its Vulkan image, 340-740 MB of images unused for over a minute in long sessions
(14/11/2026, race-grand-prix-ov14d8nd / -vunwgxoi).

Every ~10 s (600 presented frames), on the translation thread right after a frame is submitted, an image goes when no
lookup used it for MK8_TEXTURE_IDLE_S (60 by default; 0: off) and
- its contents come from guest memory (not GPU-written): a later use re-creates and uploads it, as the console reads
  that memory, or
- it is a GPU-written target whose storage start got a texture of another shape after its last use: the console's
  memory there holds the newer texture's contents, the old target's are gone there too.
Never a presented image, one with video pixels pending, or one a deferred depth copy (exec.c copy aliases) involves.

A superseded target goes sooner: MK8_TARGET_GRACE_FRAMES (30 by default, half a second) after its last use, checked
as often. When the GPU falls behind, the game's dynamic resolution makes its render targets again at a new size every
few frames (1499x843, 1546x870, 720x405...); with the minute-long wait, ~700 such images a second piled up during a
course intro until 7.3 GB of VRAM and 7.8 GB of RAM were in use, and the game ran out of device memory (CTGP Deluxe
courses in a Grand Prix, 15/11/2026, race-grand-prix-oz51459d / -70aahhh4). Their contents are gone on the console too.
Same first race since (race-grand-prix-cgxrwi9u): 2.9 GB reserved at most, 1.7 GB used, 57.8 FPS; 6043 such images
(17 GB) came and went in it. With a 2 s grace the heap still grew to 5.6 GB (race-grand-prix-7dpwm7t1).
The backend record stays: every path that uses an image goes through tex_backend first (vk_view, vk_tex, the
per-frame memo's vk_tex), whose existing "no image" branch (toolchain/pool_release.py) re-creates it; the direct
backend reads elsewhere take metadata only, or validate the image handle (copy aliases). Retiring forgets the image's
views (bumping vk_view_gen, so the per-frame view memo is redone), invalidates feedback snapshots of it and defers the
destruction past the submissions that may still read it. Uses are now stamped always, not only with MK8_TRACE_VRAM.
"""

USED_FIELD = '    uint64_t mk8_used;  // MK8 (toolchain/vram_trace.py): frame of the last lookup, with MK8_TRACE_VRAM\n'
USED_FIELD_NEW = ('    uint64_t mk8_used;  // MK8 (toolchain/vram_trace.py, texture_retire.py): presented frame of the last lookup\n'
                  '    uint64_t mk8_superseded;  // MK8 (toolchain/texture_retire.py): when a texture of another shape took its storage\n')
STAMP = '        if (mk8_vram_traced()) b->mk8_used = renderer_present_count();  // MK8 (toolchain/vram_trace.py)\n'
STAMP_NEW = '        b->mk8_used = renderer_present_count();  // MK8 (toolchain/vram_trace.py, texture_retire.py)\n'
CREATE = '    vk_collect_dead();\n    b = calloc(1, sizeof *b);\n    b->refs = 1;\n'
CREATE_NEW = '''    if (alias_on(t)) {  // MK8 (toolchain/texture_retire.py): another shape over the same storage supersedes the old one
        for (Alias *a = g_alias[alias_bucket(t->addr)]; a; a = a->next)
            if (a->addr == t->addr && !alias_match(a, t)) a->b->mk8_superseded = renderer_present_count();
    }
    vk_collect_dead();
    b = calloc(1, sizeof *b);
    b->refs = 1;
    b->mk8_used = renderer_present_count();  // MK8 (toolchain/texture_retire.py): made now, not idle
'''
SWEEP = '''
// MK8 (toolchain/texture_retire.py): images no lookup used for MK8_TEXTURE_IDLE_S seconds, whose contents can be had
// again (from guest memory, or gone there too: a superseded target), give their memory back. The record stays.
static int mk8_retire_image(TexBackend *b, uint64_t now, uint64_t idle, uint64_t grace, bool *ended, uint64_t *bytes,
                            int *targets) {
    extern bool mk8_copy_alias_uses(const TexBackend *b);
    extern void mk8_snapshots_forget(VkImage image);
    if (!b || !b->image || !b->mem || b->presented || b->pending_rgba || b->mk8_used > now) return 0;
    bool superseded = b->mk8_superseded && b->mk8_superseded >= b->mk8_used;
    if (now - b->mk8_used < (superseded ? grace : idle)) return 0;  // (superseded: its contents are gone already)
    if (b->gpu_owned && !superseded) return 0;
    if (mk8_copy_alias_uses(b)) return 0;
    if (!*ended) { vk_end_pass(); *ended = true; }
    VmaAllocationInfo info;
    vmaGetAllocationInfo(g_vk.vma, b->mem, &info);
    *bytes += info.size;
    if (b->gpu_owned) (*targets)++;
    mk8_snapshots_forget(b->image);
    destroy_image(b);  // (views forgotten, image destroyed once the submissions that may read it are done)
    b->uploaded = b->gpu_owned = false;
    b->mk8_superseded = 0;
    return 1;
}

// A device-local heap past 60% of its budget. At 2x the game's dynamic resolution made ~100 target sizes during a
// course intro (Twisted Mansion, 22/11/2026): the heap went from 0.75 to 7.4 GB between two ten-second reports, past
// the 6.3 GB budget, before the 30-frame grace retired 1908 superseded targets (24.8 GB in all); some runs ran out of
// device memory first. Under that pressure superseded targets go every frame, one frame after their last use.
static bool mk8_vram_pressure(void) {
    VmaBudget budget[VK_MAX_MEMORY_HEAPS];
    memset(budget, 0, sizeof budget);
    vmaGetHeapBudgets(g_vk.vma, budget);
    const VkPhysicalDeviceMemoryProperties *mp;
    vmaGetMemoryProperties(g_vk.vma, &mp);
    for (uint32_t i = 0; i < mp->memoryHeapCount; i++)
        if ((mp->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && budget[i].budget &&
            budget[i].usage > budget[i].budget / 5 * 3)
            return true;
    return false;
}

void mk8_textures_retire_tick(void) {
    static int idle_s = -1, grace = -1;
    static uint64_t next, next_log, total_n, total_bytes, logged_n, logged_bytes, logged_targets;
    if (idle_s < 0) { const char *e = getenv("MK8_TEXTURE_IDLE_S"); idle_s = e ? atoi(e) : 60; if (idle_s < 0) idle_s = 0; }
    if (grace < 0) { const char *e = getenv("MK8_TARGET_GRACE_FRAMES"); grace = e ? atoi(e) : 30; if (grace < 1) grace = 1; }
    uint64_t now = renderer_present_count(), idle = (uint64_t)idle_s * 60;
    if (!idle_s) return;
    bool pressure = mk8_vram_pressure();
    if (now < next && !pressure) return;
    uint64_t target_grace = pressure ? 1 : (uint64_t)grace;
    next = now + (grace < 60 ? (uint64_t)grace : 60);  // (often: superseded targets pile up by the hundred per second)
    if (now < target_grace) return;
    bool ended = false;
    uint64_t bytes = 0;
    int n = 0, targets = 0;
    for (int i = 0; i < g_nlive; i++)
        n += mk8_retire_image(g_live[i]->backend, now, idle, target_grace, &ended, &bytes, &targets);
    for (unsigned k = 0; k < ALIAS_BUCKETS; k++)  // (shared images whose first owner is gone: not in the live list)
        for (Alias *a = g_alias[k]; a; a = a->next)
            if (!a->b->reg) n += mk8_retire_image(a->b, now, idle, target_grace, &ended, &bytes, &targets);
    total_n += (uint64_t)n, total_bytes += bytes;
    logged_n += (uint64_t)n, logged_bytes += bytes, logged_targets += (uint64_t)targets;
    if (!logged_n || now < next_log) return;  // (one line per 10 s at most)
    next_log = now + 600;
    LOG("[mk8:textures] %llu images retired (%.1f MB, %llu of them superseded targets); %llu (%.0f MB) so far\\n",
        (unsigned long long)logged_n, logged_bytes / 1048576.0, (unsigned long long)logged_targets,
        (unsigned long long)total_n, total_bytes / 1048576.0);
    logged_n = logged_bytes = logged_targets = 0;
}
'''
EXEC_HELPERS = '''
// MK8 (toolchain/texture_retire.py): a deferred depth copy involves this image (it stays); feedback snapshots of an
// image about to be retired (its handle may come back for another image).
bool mk8_copy_alias_uses(const TexBackend *b) {
    for (int i = 0; i < naliases; i++)
        if (aliases[i].src->backend == b || aliases[i].dst->backend == b) return true;
    return false;
}
void mk8_snapshots_forget(VkImage image) {
    for (int i = 0; i < 8; i++)
        if (snap_ok[i] && mk8_snap_sources[i] == image) { snap_ok[i] = false; snap_why[i] = "texture retired"; }
    for (int i = 0; i < nsnaps; i++)
        if (snaps[i].source == image) snaps[i].source = VK_NULL_HANDLE;
}
'''
TICK = '    prefetch_stats_tick(total_frames);\n'
TICK_NEW = TICK + '    extern void mk8_textures_retire_tick(void);  // MK8 (toolchain/texture_retire.py)\n    mk8_textures_retire_tick();\n'


def prepare(runtime):
    vulkan = runtime/'nvn/vulkan'
    edits = [
        ('vk.h', USED_FIELD, USED_FIELD_NEW),
        ('textures.c', STAMP, STAMP_NEW),
        ('textures.c', CREATE, CREATE_NEW),
        ('device.c', TICK, TICK_NEW),
    ]
    texts = {}
    for name, old, new in edits:
        text = texts.get(name) or (vulkan/name).read_text(encoding='utf-8')
        if text.count(old) != 1 or 'mk8_textures_retire_tick' in text or 'mk8_superseded' in text:
            raise ValueError(f'Texture retirement boundary changed: {name}: {old[:60]}')
        texts[name] = text.replace(old, new)
    execute = (vulkan/'exec.c').read_text(encoding='utf-8')
    for needed in ('static CopyAlias aliases[8];', 'static bool snap_ok[8];', 'static VkImage mk8_snap_sources[8];',
                   'static Snap snaps[16];', 'static const char *snap_why[8];'):
        if execute.count(needed) != 1:
            raise ValueError(f'Texture retirement boundary changed: exec.c: {needed}')
    if 'mk8_copy_alias_uses' in execute:
        raise ValueError('Texture retirement already prepared')
    texts['textures.c'] += SWEEP
    texts['exec.c'] = execute + EXEC_HELPERS
    for name, text in texts.items():
        (vulkan/name).write_text(text, encoding='utf-8')
