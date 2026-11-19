"""Warm the driver's graphics-pipeline cache from a recorded catalog (runtime only; no guest code or ABI change).

The first draw that uses a pipeline builds it: a course entry measured several seconds on the render/submit path while
later visits to the same course were smooth. The driver's VkPipelineCache keeps what it has built, but only for
pipelines the game has already reached.

Every successful vkCreateGraphicsPipelines records its full key and the shader bytes it was built from
(cache/pipeline_recipes2.bin). The guest moves its shader blobs between runs, so the bytes travel with the recipe and
the replay translates from them. At the first presented frame a low-priority thread replays those keys, creating and
destroying the pipelines: the driver cache is filled before the race needs them, so the first draw finds a cache hit
instead of compiling on the frame path. The replay is bounded (4096 recipes, 120 s, 8 KiB per stage), deduplicated
across runs, and skips recipes that no longer translate.

The replay never calls get_program(): the program table belongs to the game thread, and a second writer poisoned it
(failed programs made the game skip every draw that later asked for them). Each recipe builds a private Prog, uses it
for one vkCreateGraphicsPipelines, and frees it, so a stale recipe can only waste its own work. It also translates and
compiles on its own thread instead of the game's build queues: a queue-based replay once made the prefetch wait 79.8 s.

A missing or tiny cache/vk_pipelines.bin means a cold driver cache, where the game's own on-demand compiles are
unavoidable and the replay could only compete with them; the replay is skipped then. MK8_PIPELINE_WARM=0 disables it.
"""
from pathlib import Path

DECLARE = 'typedef struct { PipeKey k; uint64_t hash; VkPipeline pipe; _Atomic int state; BuildJob job; bool async; } Pipe;  // state: BUILD_*\n'
DECLARED = DECLARE + 'static void mk8_pipe_record(const PipeKey *k);  // MK8 (toolchain/pipeline_warm.py)\n'

OFFLINE_DECL = 'static uint64_t g_prog_creates, g_cs_creates;  // programs made (game thread): the scan\'s and the draws\' share\n'
OFFLINE_DECLARED = OFFLINE_DECL + 'static _Thread_local int pw_offline;  // MK8 (toolchain/pipeline_warm.py): replay builds outside the game queues\n'
TRANSLATE_TAIL = '    run_build(false, &p->cjob, compile_program, p);\n'
TRANSLATE_OFFLINE = '    if (pw_offline) compile_program(p); else run_build(false, &p->cjob, compile_program, p);  // MK8 (toolchain/pipeline_warm.py)\n'

TAIL = '''    int v = r == VK_SUCCESS ? BUILD_READY : BUILD_FAILED;
    if (pp->async) build_finish(&pp->state, v);
    else atomic_store(&pp->state, v);
}
'''

WARM = r'''
#ifdef __linux__
#include <unistd.h>
#endif
// MK8 (toolchain/pipeline_warm.py): replay the pipelines a previous run recorded, so the driver compiles them before
// the race's first draw needs them. The keys and shader bytes are host-side only; guest code, render state and the ABI
// are unchanged.
#define PW_MAGIC 0x4d4b3850u  /* "MK8P" */
#define PW_VERSION 2u
#define PW_TAB 16384
#define PW_MAX 4096
#define PW_SECONDS 120
#define PW_CODE_MAX 8192u
typedef struct {
    uint32_t magic, version, stage_mask;
    uint64_t cmax[3];
    uint32_t len[3];
    ShaderKey vkey, fkey;
    PipeKey k;  // (the program pointer is not persisted)
} PwHeader;
static pthread_mutex_t pw_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t pw_seen[PW_TAB];
static int pw_n, pw_total, pw_loaded, pw_fp_ok, pw_off = -1;
static FILE *pw_fp;
static _Thread_local int pw_quiet;

// The file may hold earlier runs: its keys are loaded once so a run appends only what is new.
static void pw_load(void) {
    if (pw_loaded) return;
    pw_loaded = 1;
    FILE *f = fopen("cache/pipeline_recipes2.bin", "rb");
    if (!f) return;
    PwHeader h;
    while (pw_total < PW_MAX && fread(&h, sizeof h, 1, f) == 1) {
        if (h.magic != PW_MAGIC || h.version != PW_VERSION || h.len[0] > h.cmax[0] || h.len[1] > h.cmax[1] ||
            h.len[2] > h.cmax[2] || h.cmax[0] > 0x10000 || h.cmax[1] > 0x10000 || h.cmax[2] > 0x10000)
            break;
        uint64_t hash = hash_words(0xcbf29ce484222325ull, &h, sizeof h);
        bool ok = true;
        for (int i = 0; i < 3 && ok; i++) {
            if (!h.len[i]) continue;
            uint8_t *b = malloc(h.len[i]);
            if (!b) { ok = false; break; }
            if (fread(b, 1, h.len[i], f) != h.len[i]) ok = false;
            else hash = hash_words(hash, b, h.len[i]);
            free(b);
        }
        if (!ok) break;
        if (!hash) hash = 1;
        uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (PW_TAB - 1);
        while (pw_seen[slot] && pw_seen[slot] != hash) slot = (slot + 1) & (PW_TAB - 1);
        if (!pw_seen[slot]) { pw_seen[slot] = hash; pw_total++; }
    }
    fclose(f);
}

static void mk8_pipe_record(const PipeKey *k) {
    if (pw_off < 0) {
        const char *e = getenv("MK8_PIPELINE_WARM");
        pw_off = !e || strcmp(e, "1") != 0;  /* opt-in: recording and replay together */
    }
    if (pw_off || pw_quiet || !k || !k->prog) return;
    PwHeader h;
    memset(&h, 0, sizeof h);
    h.magic = PW_MAGIC;
    h.version = PW_VERSION;
    h.stage_mask = 1u | 2u | (k->prog->code[2] ? 4u : 0u);
    h.vkey = k->prog->vkey;
    h.fkey = k->prog->fkey;
    h.k = *k;
    h.k.prog = NULL;
    uint8_t *code[3] = {NULL, NULL, NULL};
    for (int i = 0; i < 3; i++) {
        if (!k->prog->code[i]) continue;
        uint64_t cm = k->prog->cmax[i] ? k->prog->cmax[i] : 0x10000;
        uint32_t len = cm > PW_CODE_MAX ? PW_CODE_MAX : (uint32_t)cm;
        code[i] = malloc(len ? len : 1);
        if (!code[i]) goto done;
        memcpy(code[i], (const void *)(uintptr_t)k->prog->code[i], len);
        h.cmax[i] = cm;
        h.len[i] = len;
    }
    uint64_t hash = hash_words(0xcbf29ce484222325ull, &h, sizeof h);
    for (int i = 0; i < 3; i++) hash = hash_words(hash, code[i] ? code[i] : (const uint8_t *)"", h.len[i]);
    if (!hash) hash = 1;
    pthread_mutex_lock(&pw_lock);
    pw_load();
    uint32_t slot = (uint32_t)(hash ^ (hash >> 29)) & (PW_TAB - 1);
    while (pw_seen[slot] && pw_seen[slot] != hash) slot = (slot + 1) & (PW_TAB - 1);
    if (pw_seen[slot] != hash && pw_total < PW_MAX) {
        pw_seen[slot] = hash;
        pw_total++;
        pw_n++;
        if (!pw_fp && !pw_fp_ok) {
            mkdir("cache", 0755);
            pw_fp = fopen("cache/pipeline_recipes2.bin", "ab");
            pw_fp_ok = pw_fp != NULL;
        }
        if (pw_fp) {
            fwrite(&h, sizeof h, 1, pw_fp);
            for (int i = 0; i < 3; i++)
                if (h.len[i]) fwrite(code[i], 1, h.len[i], pw_fp);
        }
    }
    pthread_mutex_unlock(&pw_lock);
done:
    for (int i = 0; i < 3; i++) free(code[i]);
}

static volatile int pw_stop;

// One recipe: a private program, translated and compiled from the recorded bytes, then one pipeline. Nothing enters
// the game's program or pipeline tables, and no game queue is used; a recipe that no longer matches only fails here.
static void pw_replay(const PwHeader *h, uint8_t *code[3], int *built, int *skipped) {
    if ((h->stage_mask & 4u) && !g_vk.geometry) { (*skipped)++; return; }
    Prog *p = calloc(1, sizeof *p);
    if (!p) { (*skipped)++; return; }
    for (int i = 0; i < 3; i++) {
        p->code[i] = (uint64_t)(uintptr_t)code[i];
        p->cmax[i] = h->cmax[i] ? h->cmax[i] : 0x10000;
    }
    p->vkey = h->vkey;
    p->fkey = h->fkey;
    p->async = false;
    pw_offline = 1;  // (translate_program compiles inline: no game queue and no in-flight slot)
    translate_program(p);
    pw_offline = 0;
    if (atomic_load(&p->state) != BUILD_READY) {
        if (p->vi) maxwell_free(p->vi);
        if (p->fi) maxwell_free(p->fi);
        if (p->gi) maxwell_free(p->gi);
        free(p);
        (*skipped)++;
        return;
    }
    Pipe tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.k = h->k;
    tmp.k.prog = p;
    tmp.async = false;
    build_pipeline(&tmp);
    if (atomic_load(&tmp.state) == BUILD_READY && tmp.pipe) {
        vkDestroyPipeline(g_vk.dev, tmp.pipe, NULL);  // (the driver cache keeps the compiled pipeline)
        (*built)++;
    } else {
        (*skipped)++;
    }
    if (p->layout) vkDestroyPipelineLayout(g_vk.dev, p->layout, NULL);
    for (int i = 0; i < 3; i++)
        if (p->set_layout[i]) vkDestroyDescriptorSetLayout(g_vk.dev, p->set_layout[i], NULL);
    if (p->vs) vkDestroyShaderModule(g_vk.dev, p->vs, NULL);
    if (p->fs) vkDestroyShaderModule(g_vk.dev, p->fs, NULL);
    if (p->gs) vkDestroyShaderModule(g_vk.dev, p->gs, NULL);
    if (p->vi) maxwell_free(p->vi);
    if (p->fi) maxwell_free(p->fi);
    if (p->gi) maxwell_free(p->gi);
    free(p);
}

static void *pw_thread(void *arg) {
    (void)arg;
    pw_quiet = 1;
#ifdef __linux__
    nice(10);  // (the game's own builds and the frame thread come first)
#endif
    FILE *f = fopen("cache/pipeline_recipes2.bin", "rb");
    if (!f) return NULL;
    uint64_t t0 = SDL_GetTicksNS();
    int n = 0, built = 0, skipped = 0;
    PwHeader h;
    while (!pw_stop && fread(&h, sizeof h, 1, f) == 1) {
        n++;
        if (built >= PW_MAX || SDL_GetTicksNS() - t0 > (uint64_t)PW_SECONDS * 1000000000ull) break;
        if (h.magic != PW_MAGIC || h.version != PW_VERSION || h.len[0] > h.cmax[0] || h.len[1] > h.cmax[1] ||
            h.len[2] > h.cmax[2] || h.cmax[0] > 0x10000 || h.cmax[1] > 0x10000 || h.cmax[2] > 0x10000) {
            skipped++;
            continue;
        }
        uint8_t *code[3] = {NULL, NULL, NULL};
        bool ok = true;
        for (int i = 0; i < 3; i++) {
            if (!h.len[i]) continue;
            code[i] = calloc(1, h.cmax[i] ? h.cmax[i] : 1);
            if (!code[i] || fread(code[i], 1, h.len[i], f) != h.len[i]) { ok = false; break; }
        }
        if (ok) pw_replay(&h, code, &built, &skipped);
        else skipped++;
        for (int i = 0; i < 3; i++) free(code[i]);
        if (!ok) break;
        if (n % 256 == 0) LOG("[mk8:pipelines] warm: %d recipes, %d built, %d skipped, %.0f s\n", n, built, skipped,
                              (double)(SDL_GetTicksNS() - t0) / 1e9);
    }
    fclose(f);
    LOG("[mk8:pipelines] warm done: %d recipes, %d built, %d skipped, %.0f s\n", n, built, skipped,
        (double)(SDL_GetTicksNS() - t0) / 1e9);
    return NULL;
}

void mk8_pipeline_warm_start(void) {
    static atomic_flag started = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&started)) return;
    const char *e = getenv("MK8_PIPELINE_WARM");
    if (!e || strcmp(e, "1")) return;  /* opt-in: no measured benefit on a warm cache */
    struct stat st;
    if (stat("cache/vk_pipelines.bin", &st) != 0 || st.st_size < (1L << 20)) {
        LOG("[mk8:pipelines] warm skipped: cold driver cache follows the game's own path\n");
        return;
    }
    pthread_t t;
    if (pthread_create(&t, NULL, pw_thread, NULL) == 0) pthread_detach(t);
}
'''

FRAME_END = 'void vk_frame_end(const char *shot_path) { frame_end(shot_path, 0); }\n'
FRAME_END_AT = 'void vk_frame_end_at(const char *shot_path, uint64_t present_at_ns) { frame_end(shot_path, present_at_ns ? present_at_ns : 1); }\n'
WARM_DECL = 'extern void mk8_pipeline_warm_start(void);  // MK8 (toolchain/pipeline_warm.py)\n'
FRAME_END_WARM = WARM_DECL + 'void vk_frame_end(const char *shot_path) { mk8_pipeline_warm_start(); frame_end(shot_path, 0); }\n'
FRAME_END_AT_WARM = (WARM_DECL
                     + 'void vk_frame_end_at(const char *shot_path, uint64_t present_at_ns) { mk8_pipeline_warm_start(); '
                       'frame_end(shot_path, present_at_ns ? present_at_ns : 1); }\n')


def prepare(runtime: Path):
    exec_c = runtime / 'nvn/vulkan/exec.c'
    text = exec_c.read_text(encoding='utf-8')
    if 'mk8_pipe_record' not in text:
        if text.count(DECLARE) != 1:
            raise ValueError('Pipeline record declaration boundary changed upstream')
        if text.count(TAIL) != 1:
            raise ValueError('Pipeline build tail boundary changed upstream')
        if text.count(OFFLINE_DECL) != 1 or text.count(TRANSLATE_TAIL) != 1:
            raise ValueError('Program translation boundary changed upstream')
        text = text.replace(OFFLINE_DECL, OFFLINE_DECLARED, 1).replace(TRANSLATE_TAIL, TRANSLATE_OFFLINE, 1)
        text = text.replace(DECLARE, DECLARED, 1).replace(
            TAIL, '    if (r == VK_SUCCESS) mk8_pipe_record(&pp->k);  // MK8 (toolchain/pipeline_warm.py)\n' + TAIL + WARM, 1)
        exec_c.write_text(text, encoding='utf-8')
    device_c = runtime / 'nvn/vulkan/device.c'
    text = device_c.read_text(encoding='utf-8')
    if 'mk8_pipeline_warm_start' in text:
        return
    if text.count(FRAME_END) != 1 or text.count(FRAME_END_AT) != 1:
        raise ValueError('Present boundary changed upstream')
    text = text.replace(FRAME_END, FRAME_END_WARM, 1).replace(FRAME_END_AT, FRAME_END_AT_WARM, 1)
    device_c.write_text(text, encoding='utf-8')
