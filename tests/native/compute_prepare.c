#ifdef NDEBUG
#error Assertions must be enabled
#endif
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "nvn/nvn.h"
#include "nvn/maxwell/shader.h"
#undef LOG
#define LOG(...) ((void)0)
#undef FATAL
#define FATAL(...) abort()
#include "buildsync.c"
#include "mk8_compute_prepare.h"

typedef struct {
    ShaderInfo *vi;
    uint64_t code[3];
    _Atomic int state;
    BuildJob cjob;
    bool async;
    uintptr_t layout, set_layout[3];
} Prog;
typedef struct CsProg {
    Prog p;
    ShaderInfo *si;
    uintptr_t pipe;
    uint32_t local[3];
    struct CsProg *mk8_retired_next;
} CsProg;
static struct { int dev; } g_vk;
static int g_cs_n;
static uint64_t g_cs_creates;
static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t maxwell_gmem_base, maxwell_gmem_size;
static JobQueue *build_queue;
static _Atomic bool build_gate, present_gate, reset_done;
static unsigned translations, destroyed_infos, destroyed_pipelines;
static unsigned seen[512];
static pthread_t translator;
static bool check_owner;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal_trace(void) { abort(); }
uint64_t vk_frame_counter(void) { return 20; }
uint64_t renderer_present_count(void) { return 10; }
bool mk8_wait_trace_verbose(void) { return false; }
ShaderInfo *maxwell_translate(const uint8_t *code, size_t size, int stage, const ShaderKey *key) {
    assert(size >= 8 && stage == STAGE_COMPUTE && key->glsl);
    assert((uintptr_t)code % 8 == 0);
    if (check_owner) assert(pthread_equal(pthread_self(), translator));
    seen[translations++] = code[0];
    ShaderInfo *si = calloc(1, sizeof *si);
    if (code[0] != 0xff) { si->msl = malloc(2); si->msl[0] = (char)code[0]; si->msl[1] = 0; }
    return si;
}
ShaderInfo *mk8_translate_cached(const uint8_t *code, size_t size, int stage, const ShaderKey *key) {
    return maxwell_translate(code, size, stage, key);
}
void maxwell_free(ShaderInfo *si) { destroyed_infos++; free(si->msl); free(si); }
static void check_destroy(uintptr_t value) {
    CsProg *cp = (CsProg *)value;
    assert(!cp->p.async || atomic_load(&cp->p.cjob.state) == JOB_DONE);
}
static void vkDestroyPipeline(int dev, uintptr_t value, void *unused) {
    (void)dev; (void)unused; check_destroy(value); destroyed_pipelines++;
}
static void vkDestroyPipelineLayout(int dev, uintptr_t value, void *unused) {
    (void)dev; (void)unused; check_destroy(value);
}
static void vkDestroyDescriptorSetLayout(int dev, uintptr_t value, void *unused) {
    (void)dev; (void)unused; check_destroy(value);
}
static void save_glsl(const char *source, uint64_t addr, const char *stage) {
    assert(source && addr && !strcmp(stage, "cs"));
}
static void build_compute(void *arg) {
    CsProg *cp = arg;
    while (!atomic_load(&build_gate)) Sleep(1);
    cp->pipe = (uintptr_t)cp;
    cp->p.layout = (uintptr_t)cp;
    cp->p.set_layout[0] = (uintptr_t)cp;
    int state = (unsigned char)cp->si->msl[0] == 0xfe ? BUILD_FAILED : BUILD_READY;
    if (cp->p.async) build_finish(&cp->p.state, state);
    else atomic_store(&cp->p.state, state);
}
static void run_build(bool translate, BuildJob *job, void (*fn)(void *), void *arg) {
    assert(!translate);
    if (shader_mode() == SHADERS_SYNC) { fn(arg); return; }
    if (!build_queue) build_queue = jobq_create(1);
    jobq_submit(build_queue, job, fn, arg);
}
#include "compute.inc"

static NvnSync fence;
static NvnSync *sync_of(uint64_t guest) { (void)guest; return &fence; }
static void mk8_present_now(NvnTexture *t) { (void)t; while (!atomic_load(&present_gate)) Sleep(1); }
void mk8_texture_translator_thread(void) { translator = pthread_self(); }
void mk8_textures_drain(void) {}
void mk8_descriptors_drain(void) {}
NvnTexture *mk8_texture_retain(NvnTexture *t) { return t; }
void mk8_texture_release(NvnTexture *t) { (void)t; }
int mk8_textures_release_range(uint64_t lo, uint64_t hi, uint64_t *bytes) {
    (void)lo; (void)hi; *bytes = 0; return 0;
}
void mk8_texture_readback(NvnTexture *t, const NvnTextureView *v, const int32_t *r,
                          uint64_t d, int64_t row, int64_t image) {
    (void)t; (void)v; (void)r; (void)d; (void)row; (void)image;
}
static NvnProgram *submitted;
static CsProg *observed_cp;
void backend_prefetch(const uint64_t *handles, int n) {
    (void)handles; assert(n == 1); observed_cp = compute_program(submitted);
}
void backend_execute(uint64_t handle) { (void)handle; }
void renderer_prefetch_verify(void) {}
uint64_t renderer_fence_seq(void) { return 0; }
void renderer_submit(void) {}
#include "queue.inc"

static NvnProgram programs[100];
static uint8_t *memory;
static NvnProgram *program(unsigned id, uint8_t value) {
    NvnProgram *p = &programs[id];
    uint8_t *code = memory + 0x1000 * id;
    uint8_t *control = code + 0x800;
    memset(code, value, 0x100);
    uint32_t local[3] = {4, 0, 2};
    memcpy(control + 0x748, local, sizeof local);
    p->stages[5] = (NvnShaderStage){.code_addr = (uintptr_t)code, .control = control, .const_offset = 0x100};
    return p;
}
static void submit(NvnProgram *p) {
    static _Alignas(8) CmdRec end;
    end.op = CMD_END; end.size = sizeof end;
    uint64_t handle = (uintptr_t)&end;
    submitted = p;
    assert(mk8_async_submit(1, &handle));
    mk8_q_drain();
}
static void prepare(NvnProgram *p) { mk8_compute_set(&p->stages[5]); mk8_q_drain(); }
static void finish_jobs(void) { atomic_store(&build_gate, true); build_drain(); }
static void cleanup_demand(void) {
    for (int i = 0; i < g_cs_n; i++) mk8_compute_release(g_cs_tab[i].cp);
    g_cs_n = 0;
}
static void *reset_thread(void *arg) {
    (void)arg;
    mk8_q_drain();
    build_drain();
    mk8_compute_prepare_reset();
    atomic_store(&reset_done, true);
    return NULL;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    memory = VirtualAlloc((void *)(uintptr_t)HEAP_BASE, 0x100000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(memory == (void *)(uintptr_t)HEAP_BASE);
    atomic_store(&present_gate, true);
    NvnProgram *p = program(0, 0x2a);
    if (!strcmp(argv[1], "off")) {
        mk8_compute_set(&p->stages[5]);
        assert(!mk8_q_issued && !translations && !atomic_load(&mk8_compute_queued));
        atomic_store(&build_gate, true);
        CsProg *cp = compute_program(p);
        build_drain();
        assert(cp && atomic_load(&cp->p.state) == BUILD_READY && translations == 1);
        cleanup_demand(); return 0;
    }
    check_owner = true;
    if (!strcmp(argv[1], "queue")) {
        atomic_store(&present_gate, false);
        Mk8QItem *item = calloc(1, sizeof *item); item->kind = MK8_Q_PRESENT; mk8_q_put(item, NULL, 0);
        for (int i = 0; i < 1000; i++) mk8_compute_set(&p->stages[5]);
        assert(atomic_load(&mk8_compute_queued) == MK8_COMPUTE_PREPARE_LIMIT && !translations);
        memory[0] = 0x2b;
        atomic_store(&present_gate, true); mk8_q_drain();
        assert(translations == 1 && seen[0] == 0x2a && !g_cs_n && mk8_compute_prepared_count == 1);
        assert(!atomic_load(&mk8_compute_queued));
        memory[0] = 0x2a;
        submit(p);
        assert(translations == 1 && g_cs_n == 1 && !mk8_compute_prepared_count);
        assert(observed_cp->local[0] == 4 && observed_cp->local[1] == 1 && observed_cp->local[2] == 2);
        assert(atomic_load(&observed_cp->p.state) == BUILD_PENDING);
        finish_jobs();
        assert(atomic_load(&observed_cp->p.state) == BUILD_READY);
        for (unsigned i = 1; i <= 70; i++) prepare(program(i, 0x2a));
        assert(mk8_compute_prepared_count == 64 && g_cs_n == 1 && translations == 65);
        finish_jobs(); mk8_compute_prepare_reset();
        assert(destroyed_infos == 64 && g_cs_n == 1);
        cleanup_demand();
        assert(destroyed_infos == 65 && destroyed_pipelines == 65);
        NvnShaderStage bad = p->stages[5];
        bad.code_addr = GUEST_HI - 4; assert(!mk8_compute_snapshot(&bad, 0));
        bad = p->stages[5]; bad.const_offset = 0x10001; assert(!mk8_compute_snapshot(&bad, 0));
        bad = p->stages[5]; bad.const_offset = 0; assert(!mk8_compute_snapshot(&bad, 0));
        bad = p->stages[5]; bad.control = (void *)(uintptr_t)(GUEST_HI - 4); assert(!mk8_compute_snapshot(&bad, 0));
        DWORD old;
        uint8_t *guard = memory + 90 * 0x1000;
        assert(VirtualProtect(guard, 0x1000, PAGE_NOACCESS, &old));
        bad = (NvnShaderStage){.code_addr = (uintptr_t)(guard - 8), .const_offset = 8};
        Mk8ComputeSnapshot *bounded = mk8_compute_snapshot(&bad, 0);
        assert(bounded && !((uintptr_t)bounded->code % 8)); free(bounded);
        bad.const_offset = 0; assert(!mk8_compute_snapshot(&bad, 0));
        assert(VirtualProtect(guard, 0x1000, old, &old));
        g_cs_n = 256;
        prepare(program(99, 0x2a));
        assert(!mk8_compute_prepared_count && translations == 65);
        g_cs_n = 0;
    } else if (!strcmp(argv[1], "invalidate")) {
        for (unsigned i = 0; i < 3; i++) {
            p = program(i, 0x2a); prepare(p);
            if (!i) *(uint8_t *)(uintptr_t)p->stages[5].code_addr = 0x2b;
            else if (i == 1) { uint32_t local = 8; memcpy((void *)(p->stages[5].control + 0x748), &local, 4); }
            else p->stages[5].const_offset = 0x80;
            submit(p);
            assert(translations == (i + 1) * 2 && mk8_compute_retired && !destroyed_infos);
        }
        finish_jobs(); mk8_compute_collect(false);
        assert(!mk8_compute_retired && destroyed_infos == 3 && g_cs_n == 3);
        cleanup_demand();
    } else if (!strcmp(argv[1], "states")) {
        prepare(program(0, 0xff)); submit(&programs[0]);
        assert(atomic_load(&observed_cp->p.state) == BUILD_FAILED && !observed_cp->p.async);
        prepare(program(1, 0xfe)); submit(&programs[1]);
        assert(atomic_load(&observed_cp->p.state) == BUILD_PENDING);
        finish_jobs();
        job_run_or_wait(build_queue, &observed_cp->p.cjob);
        assert(atomic_load(&observed_cp->p.state) == BUILD_FAILED && translations == 2);
        prepare(program(2, 0x2a)); finish_jobs(); submit(&programs[2]);
        assert(atomic_load(&observed_cp->p.state) == BUILD_READY && translations == 3);
        cleanup_demand();
    } else if (!strcmp(argv[1], "reset")) {
        for (unsigned i = 0; i < 3; i++) prepare(program(i, 0x2a));
        assert(mk8_compute_prepared_count == 3 && !g_cs_n);
        pthread_t thread; assert(!pthread_create(&thread, NULL, reset_thread, NULL));
        Sleep(30); assert(!atomic_load(&reset_done) && !destroyed_infos);
        atomic_store(&build_gate, true); pthread_join(thread, NULL);
        assert(atomic_load(&reset_done) && destroyed_infos == 3 && !mk8_compute_prepared_count);
        for (unsigned i = 0; i < 5; i++) {
            prepare(p); finish_jobs(); mk8_compute_prepare_reset();
            assert(destroyed_infos == 4 + i && !mk8_compute_prepared_count);
        }
    } else abort();
    VirtualFree(memory, 0, MEM_RELEASE);
    return 0;
}
