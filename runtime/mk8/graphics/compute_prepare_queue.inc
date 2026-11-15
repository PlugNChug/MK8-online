/* Experimento opt-in; a fila existente preserva ownership, ordenação e drenagem no reboot. */
static _Atomic unsigned mk8_compute_queued;
static pthread_once_t mk8_compute_once = PTHREAD_ONCE_INIT;
static bool mk8_compute_on;
static void mk8_compute_option(void) {
    const char *e = getenv("MK8_COMPUTE_PREPARE");
    mk8_compute_on = e && !strcmp(e, "1");
}
static void mk8_compute_set(const NvnShaderStage *ss) {
    pthread_once(&mk8_compute_once, mk8_compute_option);
    if (!mk8_compute_on || !mk8_async_on() || shader_mode() != SHADERS_PREFETCH) return;
    if (atomic_fetch_add(&mk8_compute_queued, 1) >= MK8_COMPUTE_PREPARE_LIMIT) {
        atomic_fetch_sub(&mk8_compute_queued, 1);
        return;
    }
    extern uint64_t renderer_present_count(void);
    Mk8ComputeSnapshot *s = mk8_compute_snapshot(ss, renderer_present_count());
    Mk8QItem *it = s ? calloc(1, sizeof *it) : NULL;
    if (!it) {
        free(s);
        atomic_fetch_sub(&mk8_compute_queued, 1);
        return;
    }
    it->kind = MK8_Q_COMPUTE_PREPARE;
    it->compute = s;
    mk8_q_put(it, NULL, 0);
}
