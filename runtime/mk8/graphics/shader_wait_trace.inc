
// Mede apenas o caminho original; não antecipa nem descarta trabalho de GPU.
#include <math.h>
#include <pthread.h>
static bool mk8_job_ready(JobQueue *q, BuildJob *j);
static bool mk8_targets_redrawn(void);
uint64_t vk_frame_counter(void);
static pthread_once_t mk8_wait_config_once = PTHREAD_ONCE_INIT;
static double mk8_wait_threshold = 50;
static bool mk8_wait_verbose;
static void mk8_wait_config_init(void) {
    const char *s = getenv("MK8_TRACE_SHADER_WAITS");
    if (s) {
        char *end;
        double value = strtod(s, &end);
        mk8_wait_threshold = end != s && !*end && isfinite(value) && value > 0 ? value : 0;
        mk8_wait_verbose = mk8_wait_threshold > 0;
    }
}
static double mk8_wait_trace_ms(void) {
    pthread_once(&mk8_wait_config_once, mk8_wait_config_init);
    return mk8_wait_threshold;
}
bool mk8_wait_trace_verbose(void) {
    mk8_wait_trace_ms();
    return mk8_wait_verbose;
}
static void mk8_shader_build_time(const Prog *p, const BuildJob *job, const char *phase, uint64_t start) {
    if (!start) return;
    double ms = (SDL_GetTicksNS() - start) / 1e6;
    if (ms >= mk8_wait_trace_ms())
        LOG("[mk8:shader-build] phase=%s ms=%.3f program=%p job=%p code=%llx/%llx/%llx\n", phase, ms,
            (const void *)p, (const void *)job, (unsigned long long)p->code[0], (unsigned long long)p->code[1],
            (unsigned long long)p->code[2]);
}
static bool mk8_traced_job(JobQueue *q, BuildJob *j, const Prog *p,
                           const char *phase, uint64_t pipeline, bool budgeted) {
    double threshold = mk8_wait_trace_ms();
    uint64_t start = threshold > 0 ? SDL_GetTicksNS() : 0;
    bool ready = true;
    if (budgeted) ready = mk8_job_ready(q, j);
    else job_run_or_wait(q, j);
    double ms = threshold > 0 ? (SDL_GetTicksNS() - start) / 1e6 : 0;
    if (threshold > 0 && ms >= threshold)
        LOG("[mk8:shader-wait] frame=%llu phase=%s ms=%.3f program=%p job=%p code=%llx/%llx/%llx pipeline=%llx budgeted=%d redrawn=%d ready=%d\n",
            (unsigned long long)vk_frame_counter(), phase, ms, (const void *)p, (const void *)j,
            (unsigned long long)p->code[0], (unsigned long long)p->code[1], (unsigned long long)p->code[2],
            (unsigned long long)pipeline, budgeted, budgeted ? mk8_targets_redrawn() : 0, ready);
    return ready;
}
