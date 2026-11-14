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
static int mk8_retire_image(TexBackend *b, uint64_t now, uint64_t idle, bool *ended, uint64_t *bytes, int *targets) {
    extern bool mk8_copy_alias_uses(const TexBackend *b);
    extern void mk8_snapshots_forget(VkImage image);
    if (!b || !b->image || !b->mem || b->presented || b->pending_rgba || b->mk8_used > now || now - b->mk8_used < idle) return 0;
    bool superseded = b->mk8_superseded && b->mk8_superseded >= b->mk8_used;
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

void mk8_textures_retire_tick(void) {
    static int idle_s = -1;
    static uint64_t next, total_n, total_bytes;
    if (idle_s < 0) { const char *e = getenv("MK8_TEXTURE_IDLE_S"); idle_s = e ? atoi(e) : 60; if (idle_s < 0) idle_s = 0; }
    uint64_t now = renderer_present_count(), idle = (uint64_t)idle_s * 60;
    if (!idle_s || now < next) return;
    next = now + 600;
    if (now < idle) return;
    bool ended = false;
    uint64_t bytes = 0;
    int n = 0, targets = 0;
    for (int i = 0; i < g_nlive; i++) n += mk8_retire_image(g_live[i]->backend, now, idle, &ended, &bytes, &targets);
    for (unsigned k = 0; k < ALIAS_BUCKETS; k++)  // (shared images whose first owner is gone: not in the live list)
        for (Alias *a = g_alias[k]; a; a = a->next)
            if (!a->b->reg) n += mk8_retire_image(a->b, now, idle, &ended, &bytes, &targets);
    if (!n) return;
    total_n += (uint64_t)n, total_bytes += bytes;
    LOG("[mk8:textures] %d idle images retired (%.1f MB, %d of them superseded targets); %llu (%.0f MB) so far\\n", n,
        bytes / 1048576.0, targets, (unsigned long long)total_n, total_bytes / 1048576.0);
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
