"""Bounded shader waits: a draw whose shader or pipeline is still building waits at most a per-frame budget.

Prefetch mode (the default) makes a draw wait for its program and pipeline, running a still-queued build inline.
The first sight of a new effect then stops the whole frame while the driver compiles: 244-375 ms frames in the
Grand Prix and Water Park runs of 06-07/11/2026 (docs/STATUS.md). The draws of one presented frame now share
MK8_SHADER_WAIT_MS of waiting (default 4 ms). A build that is not done within it keeps going on the worker threads,
and its draws are skipped until it is ready, as in skip mode (LARECOMP_ASYNC_SHADERS), normally for a few frames.
Compute dispatches still wait in full, because the game may read their results. A negative value keeps the
upstream behaviour. Full Grand Prix with async translation (07/11/2026): one draw skipped in 11,400 frames, no
stall over 60 ms, against 158-368 ms stalls without the budget in the run before.

Only draws whose targets are all screen-sized (textures.c `scalable`) or the validated two-player scene pair
may be skipped. The game draws some targets once: at race start, while the track's shaders still compile, it renders the lighting and
reflection probes (cubes, the BC3 cube array) a single time. Skipping those left them empty for the whole race,
and the track and the karts were drawn black (Sweet Sweet Canyon and Thwomp Ruins on a cold shader cache,
08/11/2026: mean luma 5 against 106 with the full wait). Draws into any other target wait in full, as upstream.
"""

WAIT_FOR = '''
// MK8 (toolchain/shader_budget.py): wait for a queued or running job at most max_ms; 1 when it is done.
// (never runs it inline: a build left queued is taken by a worker thread)
#include "platform.h"
int mk8_job_wait_for(BuildJob *j, double max_ms) {
    if (atomic_load(&j->state) == JOB_DONE) return 1;
    if (max_ms <= 0) return 0;
    double t0 = now_ms();
    uint64_t end = plat_now_ns() + (uint64_t)(max_ms * 1e6);
    // Winpthreads' sub-millisecond timed waits can sleep a full 15.6 ms tick. Use the platform's monotonic,
    // high-resolution sleep in short slices, without holding the workers' completion lock.
    while (atomic_load(&j->state) != JOB_DONE) {
        uint64_t now = plat_now_ns();
        if (now >= end) break;
        plat_sleep_until_ns(end - now > 500000 ? now + 500000 : end);
    }
    int ok = atomic_load(&j->state) == JOB_DONE;
    pthread_mutex_lock(&lock);
    count_wait(now_ms() - t0, j, "job (bounded)");
    pthread_mutex_unlock(&lock);
    return ok;
}
'''

BUDGET = '''
// MK8 (toolchain/shader_budget.py): MK8_SHADER_WAIT_MS (default 4, negative: no limit), the waiting the draws of one presented frame may do for
// their shaders and pipelines; past it a build goes on in the background and its draws are skipped meanwhile.
int mk8_job_wait_for(BuildJob *j, double max_ms);
uint64_t vk_frame_counter(void);
int build_inflight(void);
static uint64_t mk8_sb_skips;
static double mk8_shader_budget_ms(void) {
    static double v = -2, burst = -2;
    static uint64_t burst_until;
    if (v < -1) { const char *e = getenv("MK8_SHADER_WAIT_MS"); v = e && *e ? atof(e) : 4; }
    // MK8 (toolchain/shader_budget.py): optional burst grace for diagnosis. A course entry queues hundreds of builds
    // at once; skipping the scene draws then punched holes in the loading frame (836 skipped in the first entry frame,
    // 7,118 in the next, 19/11/2026) but did shorten the hitch (2.36 s with full waits against 1.20 s with the 4 ms
    // budget; the full cost is 2,221 image views, ~1.19 s). MK8_SHADER_BURST_WAIT_MS (0: off, the default) waits that
    // much during a >64-build burst and MK8_SHADER_BURST_FRAMES (120) frames after it; a negative value waits fully.
    if (burst < -1) { const char *e = getenv("MK8_SHADER_BURST_WAIT_MS"); burst = e && *e ? atof(e) : 0; }
    if (burst != 0) {
        static long frames = -1;
        if (frames < 0) { const char *e = getenv("MK8_SHADER_BURST_FRAMES"); frames = e && *e ? atol(e) : 120; }
        uint64_t f = vk_frame_counter();
        if (build_inflight() > 64) burst_until = f + (uint64_t)frames;
        if (burst_until && f < burst_until) return burst;
    }
    return v;
}
// MK8 4.0.1's two-player scene has a color/depth pair at 8:9, including dynamic resolution. It is redrawn each
// frame but is not `scalable` (that flag only accepts 16:9). Do not extend this to arbitrary offscreen targets.
static bool mk8_split_scene(void) {
    if (st.rt.ncolor != 1) return false;
    const NvnTexture *c = st.rt.color[0], *d = st.rt.depth;
    if (!c || !d || !c->backend || !d->backend) return false;
    if (c->target != 1 || d->target != 1 || c->samples > 1 || d->samples > 1 ||
        c->flags != 8 || d->flags != 8 || c->format != 63 || d->format != 52) return false;
    if (c->width != d->width || c->height != d->height ||
        c->width < 640 || c->width > 960 || c->height < 720 || c->height > 1080) return false;
    // Rounded dynamic dimensions can differ by one pixel from the exact 8:9 ratio.
    int delta = (int)c->width * 9 - (int)c->height * 8;
    return delta >= -8 && delta <= 8;
}
// Every target of the draw is screen-sized (redrawn each frame): skipping it costs a frame of one object. Targets
// drawn once (probes, cube maps, baked maps) or not created yet are never skipped.
static bool mk8_targets_redrawn(void) {
    if (mk8_split_scene()) return true;
    int n = 0;
    for (int i = 0; i < st.rt.ncolor; i++) {
        const NvnTexture *t = st.rt.color[i];
        if (!t) continue;
        const TexBackend *b = t->backend;
        if (!b || !b->scalable) return false;
        n++;
    }
    if (st.rt.depth) {
        const TexBackend *b = st.rt.depth->backend;
        if (!b || !b->scalable) return false;
        n++;
    }
    return n > 0;
}
static bool mk8_job_ready(JobQueue *q, BuildJob *j) {
    double budget = mk8_shader_budget_ms();
    if (budget < 0 || !mk8_targets_redrawn()) {
        uint64_t t0 = SDL_GetTicksNS();
        job_run_or_wait(q, j);
        double ms = (SDL_GetTicksNS() - t0) / 1e6;
        if (ms > 50 && getenv("MK8_TRACE_SHADER_TARGETS")) {
            LOG("[mk8:shader-wait] frame %llu full wait %.1f ms\\n", (unsigned long long)vk_frame_counter(), ms);
            for (int i = 0; i <= st.rt.ncolor; i++) {
                const NvnTexture *t = i < st.rt.ncolor ? st.rt.color[i] : st.rt.depth;
                if (!t) continue;
                const TexBackend *b = t->backend;
                LOG("[mk8:shader-wait] %s %d: %ux%u target %d samples %d flags %x format %d scalable %d\\n",
                    i < st.rt.ncolor ? "color" : "depth", i, t->width, t->height, t->target, t->samples,
                    t->flags, t->format, b ? b->scalable : 0);
            }
        }
        return true;
    }
    static uint64_t frame = ~0ull;
    static double spent;
    uint64_t f = vk_frame_counter();
    if (f != frame) { frame = f; spent = 0; }
    uint64_t t0 = SDL_GetTicksNS();
    int ok = mk8_job_wait_for(j, budget - spent);
    spent += (SDL_GetTicksNS() - t0) / 1e6;
    if (!ok && (mk8_sb_skips++ == 0 || mk8_sb_skips % 500 == 0))
        LOG("[shader] budget %.1f ms per frame: a draw skipped while its build finishes (%llu so far)\\n", budget,
            (unsigned long long)mk8_sb_skips);
    return ok;
}
'''

SITES = [
    ('''        job_run_or_wait(g_tq, &p->tjob);
        if (atomic_load(&p->state) == BUILD_PENDING) job_run_or_wait(g_wq, &p->cjob);  // (queued by the translation)
''', '''        if (mk8_job_ready(g_tq, &p->tjob) && atomic_load(&p->state) == BUILD_PENDING)
            mk8_job_ready(g_wq, &p->cjob);  // (queued by the translation; toolchain/shader_budget.py)
'''),
    ('''        while (atomic_load(&pp->job.state) == JOB_IDLE) sched_yield();  // (put in the table just before it's queued)
        job_run_or_wait(g_wq, &pp->job);
''', '''        while (atomic_load(&pp->job.state) == JOB_IDLE) sched_yield();  // (put in the table just before it's queued)
        mk8_job_ready(g_wq, &pp->job);  // (toolchain/shader_budget.py: the draw is skipped below if it isn't ready)
'''),
]
ANCHOR = 'static bool prog_ready(Prog *p) {\n'


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Shader budget boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    buildsync = runtime/'nvn/buildsync.c'
    text = buildsync.read_text(encoding='utf-8')
    execute = runtime/'nvn/vulkan/exec.c'
    current = execute.read_text(encoding='utf-8')
    if 'mk8_job_wait_for' in text and 'mk8_job_ready' in current:
        return  # already prepared
    buildsync.write_text(replace(text, 'void build_test_delay(void) {\n', WAIT_FOR + '\nvoid build_test_delay(void) {\n'),
                         encoding='utf-8')
    text = replace(current, ANCHOR, BUDGET + ANCHOR)
    for old, new in SITES:
        text = replace(text, old, new)
    execute.write_text(text, encoding='utf-8')
