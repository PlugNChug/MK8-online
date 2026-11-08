"""Bounded shader waits: a draw whose shader or pipeline is still building waits at most a per-frame budget.

Prefetch mode (the default) makes a draw wait for its program and pipeline, running a still-queued build inline.
The first sight of a new effect then stops the whole frame while the driver compiles: 244-375 ms frames in the
Grand Prix and Water Park runs of 06-07/11/2026 (docs/STATUS.md). The draws of one presented frame now share
MK8_SHADER_WAIT_MS of waiting (default 4 ms). A build that is not done within it keeps going on the worker threads,
and its draws are skipped until it is ready, as in skip mode (LARECOMP_ASYNC_SHADERS), normally for a few frames.
Compute dispatches still wait in full, because the game may read their results. A negative value keeps the
upstream behaviour. Full Grand Prix with async translation (07/11/2026): one draw skipped in 11,400 frames, no
stall over 60 ms, against 158-368 ms stalls without the budget in the run before.

Only draws whose targets are all screen-sized (textures.c `scalable`: redrawn every frame) may be skipped. The
game draws some targets once: at race start, while the track's shaders still compile, it renders the lighting and
reflection probes (cubes, the BC3 cube array) a single time. Skipping those left them empty for the whole race,
and the track and the karts were drawn black (Sweet Sweet Canyon and Thwomp Ruins on a cold shader cache,
08/11/2026: mean luma 5 against 106 with the full wait). Draws into any other target wait in full, as upstream.
"""

WAIT_FOR = '''
// MK8 (toolchain/shader_budget.py): wait for a queued or running job at most max_ms; 1 when it is done.
// (never runs it inline: a build left queued is taken by a worker thread)
#include <errno.h>
int mk8_job_wait_for(BuildJob *j, double max_ms) {
    if (atomic_load(&j->state) == JOB_DONE) return 1;
    if (max_ms <= 0) return 0;
    double t0 = now_ms();
    pthread_mutex_lock(&lock);
    // (short slices against the monotonic clock: a timed wait here can overshoot a whole timer tick)
    while (atomic_load(&j->state) != JOB_DONE && now_ms() - t0 < max_ms) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 500000;
        if (until.tv_nsec >= 1000000000) until.tv_sec++, until.tv_nsec -= 1000000000;
        pthread_cond_timedwait(&done, &lock, &until);
    }
    int ok = atomic_load(&j->state) == JOB_DONE;
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
static uint64_t mk8_sb_skips;
static double mk8_shader_budget_ms(void) {
    static double v = -2;
    if (v < -1) { const char *e = getenv("MK8_SHADER_WAIT_MS"); v = e && *e ? atof(e) : 4; }
    return v;
}
// Every target of the draw is screen-sized (redrawn each frame): skipping it costs a frame of one object. Targets
// drawn once (probes, cube maps, baked maps) or not created yet are never skipped.
static bool mk8_targets_redrawn(void) {
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
    if (budget < 0 || !mk8_targets_redrawn()) { job_run_or_wait(q, j); return true; }
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
    buildsync.write_text(replace(text, 'void build_test_delay(void) {\n', WAIT_FOR + '\nvoid build_test_delay(void) {\n'),
                         encoding='utf-8')
    execute = runtime/'nvn/vulkan/exec.c'
    text = replace(execute.read_text(encoding='utf-8'), ANCHOR, BUDGET + ANCHOR)
    for old, new in SITES:
        text = replace(text, old, new)
    execute.write_text(text, encoding='utf-8')
