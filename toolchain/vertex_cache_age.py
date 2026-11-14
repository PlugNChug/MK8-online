"""Drop vertex cache entries that no draw used for a minute (runtime/nvn/vulkan/exec.c's device-local copies).

The cache keeps a copy of each guest range drawn from and lets entries go only past its 768 MB budget. Every load
places the game's buffers elsewhere, so ranges of earlier tracks stayed: in a 50-minute session of three Grand Prix
over the same cup it grew 31 -> 130 MB and never shrank (14/11/2026, race-grand-prix-ov14d8nd), on its way to the
768 MB budget. Every ~10 s (600 presented frames) vc_flush now records the pending submission and drops the entries
not drawn from since the record made about MK8_VCACHE_IDLE_S (60 by default; 0: off) earlier, through vc_drop, which
frees a buffer only once the submissions that may read it are done. It stops before the dead list would fill (no GPU
wait from here). An entry dropped and drawn from again is copied again, as a new one.
"""

BOUNDARY = 'void vc_flush(void) {\n'
SWEEP = '''// MK8 (toolchain/vertex_cache_age.py): entries no draw used for about MK8_VCACHE_IDLE_S seconds go (translation thread).
#define MK8_VC_MARKS 64
static void mk8_vc_sweep(void) {
    extern uint64_t vk_frame_counter(void);
    static int idle_s = -1;
    static uint64_t next_frame, marks[MK8_VC_MARKS];
    static unsigned nmarks;
    if (idle_s < 0) { const char *e = getenv("MK8_VCACHE_IDLE_S"); idle_s = e ? atoi(e) : 60; if (idle_s < 0) idle_s = 0; }
    uint64_t frame = vk_frame_counter();
    if (!idle_s || frame < next_frame) return;
    next_frame = frame + 600;
    marks[nmarks++ % MK8_VC_MARKS] = vk_pending_seq();
    unsigned back = (unsigned)(idle_s + 9) / 10;  // (marks are ~10 s apart)
    if (back >= MK8_VC_MARKS) back = MK8_VC_MARKS - 1;
    if (nmarks <= back) return;
    uint64_t before = marks[(nmarks - 1 - back) % MK8_VC_MARKS];
    if (vc_ndead) vc_free_dead(false);
    int dropped = 0;
    uint64_t bytes = 0;
    for (int i = 0; i < VC_TAB && vc_ndead < VC_DEAD - 1; i++) {
        VcEnt *e = &vc_tab[i];
        if (!e->buf || e->used_seq >= before) continue;
        uint64_t size = e->hi - e->lo;
        if (vc_drop(e)) dropped++, bytes += size;
    }
    static uint64_t total_n, total_bytes;
    total_n += (uint64_t)dropped, total_bytes += bytes;
    if (dropped)
        LOG("[mk8:vcache] %d idle ranges dropped (%.1f MB); %.0f MB kept; %llu (%.0f MB) so far\\n", dropped, bytes / 1048576.0,
            vc_bytes / 1048576.0, (unsigned long long)total_n, total_bytes / 1048576.0);
}

'''


def prepare(runtime):
    path = runtime/'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    if text.count(BOUNDARY) != 1 or 'mk8_vc_sweep' in text or text.count('static bool vc_drop(VcEnt *e) {') != 1:
        raise ValueError('Vertex cache age boundary changed')
    text = text.replace(BOUNDARY, SWEEP + BOUNDARY + '    mk8_vc_sweep();\n')
    path.write_text(text, encoding='utf-8')
