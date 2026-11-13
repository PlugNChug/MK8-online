"""Native ownership tests against the prepared runtime; no game files or GPU required."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'toolchain'))
from host import DEPS, prepare_host_sources
from native import DEFAULT_COMPILER


def between(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


@unittest.skipUnless(DEFAULT_COMPILER.is_file(), 'Native compiler required')
class DescriptorLifetimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.out = Path(cls.folder.name)
        cls.runtime = prepare_host_sources(ROOT/'upstream/smm2recomp/recomp', cls.out/'prepared')
        objects = (cls.runtime/'nvn/objects.c').read_text(encoding='utf-8')
        commands = (cls.runtime/'nvn/cmdbuf.c').read_text(encoding='utf-8')
        pools = between(objects, 'NvnDescPool *g_texture_pools[64]', '// ================================================================== programs')
        refs = between(commands, 'struct Mk8TextureRefs {', 'static void mk8_retire_command_buffer')
        tracking = between(commands, '// Track unfinished recordings', 'static NvnCmdBuf *cb_of(')
        lists = between(commands, 'typedef struct ListInfo {', 'static uint64_t est_used(')
        buffers = between(commands, 'static NvnCmdBuf *cb_of(', 'static CmdRec *raw_rec(')
        reset = commands[commands.index('// Reboot only: no guest threads, no translation'):]
        queue = between(objects, '// MK8 (toolchain/async_submit.py): submissions, presents', 'void renderer_wait_seq(uint64_t seq);')
        sync_hook = between(objects, 'bool mk8_async_submit_hook(int n, const uint64_t *handles) {', 'void mk8_async_drain_hook')
        execute = (cls.runtime/'nvn/vulkan/exec.c').read_text(encoding='utf-8')
        state_helpers = between(execute, 'static void mk8_rt_retain(', 'static void st_defaults(')
        state_cases = between(execute, '        case CMD_SET_TEXTURE_POOL:', '        case CMD_SET_RENDER_TARGETS:')
        device = (cls.runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
        present_owner = between(device, '    NvnTexture *previous_presented', '    TexBackend *b = vk_tex(t, true);')
        cls.source = r'''
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "nvn/nvn.h"
#undef NVN
#define NVN(name) void name(Cpu *c)
#define MK8_SAMPLER(guest) ((NvnSamplerDesc *)(uintptr_t)((guest) + 8))
static _Atomic unsigned destroyed;
static _Atomic bool require_translator;
static pthread_t translator;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal_trace(void) { _Exit(77); }
static int fail_calloc_after = -1;
static void *test_calloc(size_t n, size_t size) {
    if (fail_calloc_after == 0) return NULL;
    if (fail_calloc_after > 0) fail_calloc_after--;
    return calloc(n, size);
}
#define calloc test_calloc
void backend_texture_destroyed(NvnTexture *t) {
    assert(t->width == 123);
    if (atomic_load(&require_translator)) assert(pthread_equal(pthread_self(), translator));
    atomic_fetch_add(&destroyed, 1);
}
uint64_t renderer_present_count(void) { return 0; }
''' + (ROOT/'runtime/mk8/graphics/texture_refs.inc').read_text(encoding='utf-8') + pools + refs + tracking + lists + buffers + reset + r'''
static _Atomic bool gate, entered;
static NvnSync test_sync;
static NvnSync *sync_of(uint64_t guest) { (void)guest; return &test_sync; }
static void mk8_present_now(NvnTexture *t) {
    (void)t; translator = pthread_self(); atomic_store(&entered, true);
    while (!atomic_load(&gate)) Sleep(1);
}
int mk8_textures_release_range(uint64_t lo, uint64_t hi, uint64_t *bytes) {
    (void)lo; (void)hi; *bytes = 0; return 0;
}
void backend_prefetch(const uint64_t *h, int n) { (void)h; (void)n; }
void backend_execute(uint64_t handle) {
    CmdRec *r = (CmdRec *)(uintptr_t)handle;
    NvnDescPool *p = r->mk8_textures->pool;
    assert(p->mk8_finalized && p->count == 16);
    assert(((NvnTexEntry *)p->entries)[5].tex->width == 123);
    assert(!atomic_load(&destroyed));
    mk8_free_list((uint8_t *)r);
}
void renderer_prefetch_verify(void) {}
uint64_t renderer_fence_seq(void) { return 1; }
void renderer_submit(void) {}
''' + queue + sync_hook + r'''
typedef struct { int ncolor; NvnTexture *color[8], *depth; } RtState;
typedef struct { NvnDescPool *tpool, *spool; RtState rt; } ExecState;
static ExecState st;
''' + state_helpers + r'''
static bool apply_pool(ExecState *s, const CmdRec *r) {
    switch (r->op) {
''' + state_cases + r'''
    default: return false;
    }
}
static NvnTexture *g_last_presented;
static void hold_presented(NvnTexture *t) {
''' + present_owner + r'''
}
static NvnTexture *texture(void) {
    NvnTexture *t = calloc(1, sizeof *t);
    t->mk8_refs = 1; t->width = 123; t->target = 8;
    return t;
}
static void create_pool(uint64_t guest[4], int count, bool sampler) {
    NvnPool memory = {.storage = 0x10000, .size = 0x100000};
    uint64_t mem_guest = (uintptr_t)&memory;
    Cpu c = {0}; c.x[0] = (uintptr_t)guest; c.x[1] = (uintptr_t)&mem_guest; c.x[3] = count;
    if (sampler) nvnSamplerPoolInitialize(&c); else nvnTexturePoolInitialize(&c);
    assert(c.x[0] == 1);
}
static void finalize(uint64_t guest[4], bool sampler) {
    Cpu c = {0}; c.x[0] = (uintptr_t)guest;
    if (sampler) nvnSamplerPoolFinalize(&c); else nvnTexturePoolFinalize(&c);
    assert(!guest[0]);
}
static void reg(uint64_t guest[4], int id, NvnTexture *t) {
    uint64_t tex_guest = (uintptr_t)t;
    Cpu c = {0}; c.x[0] = (uintptr_t)guest; c.x[1] = id; c.x[2] = (uintptr_t)&tex_guest;
    nvnTexturePoolRegisterTexture(&c);
}
static CmdRec *record_pool(NvnDescPool *p) {
    CmdRec *r = calloc(2, sizeof *r);
    r[0].op = CMD_SET_TEXTURE_POOL; r[0].size = sizeof *r; r[0].x[0] = (uintptr_t)p;
    r[1].op = CMD_END; r[1].size = sizeof *r;
    mk8_command_pool(r, p);
    return r;
}
static void collect(void) { mk8_descriptors_drain(); mk8_textures_drain(); }
static void *retire_owner(void *arg) { finalize(arg, false); return NULL; }
static void *references(void *arg) {
    for (int i = 0; i < 10000; i++) { mk8_descriptor_retain(arg); mk8_descriptor_release(arg); }
    return NULL;
}
static void ownership(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnDescPool *p = (void *)(uintptr_t)guest[0];
    NvnTexture *t = texture(); reg(guest, 5, t); reg(guest, 6, t);
    mk8_texture_release(t); // two descriptors plus two history entries own this texture
    assert(t->mk8_refs == 4);
    CmdRec *r = record_pool(p), copy[2]; memcpy(copy, r, sizeof copy);
    mk8_copy_command_textures(copy, r);
    mk8_descriptor_retain(p); // persistent executor binding
    pthread_t worker; assert(!pthread_create(&worker, NULL, retire_owner, guest)); pthread_join(worker, NULL);
    finalize(guest, false); // repeated finalization must be harmless
    collect(); assert(!destroyed && p->mk8_pool_refs == 3);
    create_pool(guest, 32, false); // same guest object, old recorded pool still in use
    assert(guest[0] != (uintptr_t)p && !mk8_texpool_cube((void *)(uintptr_t)guest[0], 5));
    finalize(guest, false); collect(); assert(!destroyed && p->count == 16);
    mk8_free_list((uint8_t *)r); collect(); assert(!destroyed);
    mk8_release_records((uint8_t *)copy, sizeof copy); collect(); assert(!destroyed);
    mk8_descriptor_release(p); collect(); assert(destroyed == 1 && !mk8_desc_live && !mk8_cube_ids);
}
static void cycles(void) {
    uint64_t guests[130][4] = {{0}};
    for (int i = 0; i < 130; i++) {
        create_pool(guests[i], 16, false);
        NvnTexture *t = texture(); reg(guests[i], 5, t); mk8_texture_release(t);
        NvnDescPool *p = (void *)(uintptr_t)guests[i][0];
        assert(mk8_texpool_cube(p, 5)->tex == t);
    }
    // Remove a middle entry first: all other histories must remain discoverable.
    finalize(guests[35], false); collect();
    assert(mk8_texpool_cube((void *)(uintptr_t)guests[129][0], 5));
    for (int i = 0; i < 130; i++) finalize(guests[i], false);
    collect(); assert(destroyed == 130 && !mk8_cube_ids && !mk8_desc_live);
    for (int i = 0; i < 200; i++) {
        create_pool(guests[0], 16, false);
        NvnDescPool *p = (void *)(uintptr_t)guests[0][0];
        assert(!mk8_texpool_cube(p, 5)); // guest address reuse must not inherit old cubes
        finalize(guests[0], false); collect();
        create_pool(guests[0], 4, true); finalize(guests[0], true); collect();
    }
    for (int i = 0; i < 64; i++) assert(!g_texture_pools[i] && !g_sampler_pools[i]);
}
static void reset_owners(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnDescPool *p = (void *)(uintptr_t)guest[0];
    NvnTexture *t = texture(); reg(guest, 5, t); mk8_texture_release(t);
    uint64_t cbguest[4] = {0}; Cpu c = {0}; c.x[0] = (uintptr_t)cbguest;
    nvnCommandBufferInitialize(&c);
    NvnCmdBuf *cb = (void *)(uintptr_t)cbguest[0];
    cb->buf = (void *)record_pool(p); cb->buf_used = 2 * sizeof(CmdRec);
    cb->calls_only[0] = (void *)record_pool(p);
    ListInfo *l = calloc(1, sizeof *l); l->mem = (void *)record_pool(p); list_buckets[0] = l;
    bury((void *)record_pool(p));
    mk8_commands_reset_owned(); mk8_descriptors_reset(); collect();
    assert(destroyed == 1 && !mk8_desc_live && !mk8_desc_dead && !mk8_cube_ids && !mk8_cmd_owners);
    assert(!grave_head && !grave_tail && !list_clock);
    for (unsigned i = 0; i < LIST_BUCKETS; i++) assert(!list_buckets[i]);
    mk8_commands_reset_owned(); mk8_descriptors_reset(); collect(); // idempotent empty reset
}
static void concurrent(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnDescPool *p = (void *)(uintptr_t)guest[0];
    pthread_t threads[8];
    for (int i = 0; i < 8; i++) assert(!pthread_create(&threads[i], NULL, references, p));
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    assert(p->mk8_pool_refs == 1); finalize(guest, false); collect(); assert(!mk8_desc_live);
}
static void queued(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnDescPool *p = (void *)(uintptr_t)guest[0];
    NvnTexture *t = texture(); reg(guest, 5, t); mk8_texture_release(t);
    CmdRec *r = record_pool(p);
    Mk8QItem *pr = calloc(1, sizeof *pr); pr->kind = MK8_Q_PRESENT; mk8_q_put(pr, NULL, 0);
    while (!atomic_load(&entered)) Sleep(1);
    uint64_t h = (uintptr_t)r; assert(mk8_async_submit(1, &h));
    finalize(guest, false);
    assert(p->mk8_pool_refs == 1 && !destroyed); // finalized while submission is queued
    atomic_store(&require_translator, true);
    atomic_store(&gate, true);
    mk8_q_drain();
    // A following item drains the retired pool on the translator and releases the image exactly once.
    pr = calloc(1, sizeof *pr); pr->kind = MK8_Q_PRESENT; mk8_q_put(pr, NULL, 0); mk8_q_drain();
    assert(destroyed == 1 && !mk8_desc_live && !mk8_desc_dead);
}
static void state_bindings(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnDescPool *p = (void *)(uintptr_t)guest[0];
    NvnTexture *t = texture(); reg(guest, 5, t); mk8_texture_release(t);
    CmdRec *r = record_pool(p);
    ExecState shadow = {0}; apply_pool(&shadow, r); assert(p->mk8_pool_refs == 2);
    apply_pool(&st, r); apply_pool(&st, r); assert(p->mk8_pool_refs == 3);
    finalize(guest, false); mk8_free_list((void *)r); collect(); assert(!destroyed);
    mk8_executor_owners_reset(); collect(); assert(destroyed == 1 && !mk8_desc_live);
    create_pool(guest, 16, true); p = (void *)(uintptr_t)guest[0];
    r = record_pool(p); r->op = CMD_SET_SAMPLER_POOL; apply_pool(&st, r);
    finalize(guest, true); mk8_free_list((void *)r); collect(); assert(st.spool == p);
    mk8_executor_owners_reset(); collect(); assert(!mk8_desc_live);
}
static void present_queue(void) {
    NvnTexture *t = texture();
    Mk8QItem *pr = calloc(1, sizeof *pr); pr->kind = MK8_Q_PRESENT; pr->tex = t;
    mk8_q_put(pr, NULL, 0);
    while (!atomic_load(&entered)) Sleep(1);
    mk8_texture_release(t); assert(t->mk8_refs == 1 && !destroyed);
    atomic_store(&require_translator, true); atomic_store(&gate, true); mk8_q_drain();
    assert(destroyed == 1);
}
static void last_presented(void) {
    NvnTexture *a = texture(), *b = texture();
    hold_presented(a); mk8_texture_release(a);
    hold_presented(a); assert(a->mk8_refs == 1 && !destroyed);
    hold_presented(b); assert(destroyed == 1);
    mk8_texture_release(b); assert(b->mk8_refs == 1);
    mk8_texture_release(g_last_presented); g_last_presented = NULL; assert(destroyed == 2);
}
static void synchronous(void) {
    uint64_t guest[4] = {0}; create_pool(guest, 16, false);
    NvnTexture *t = texture(); reg(guest, 5, t); mk8_texture_release(t);
    finalize(guest, false); assert(!destroyed);
    assert(!mk8_async_submit_hook(0, NULL)); assert(destroyed == 1 && !mk8_desc_live && !mk8_desc_dead);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "ownership")) ownership();
    else if (!strcmp(argv[1], "cycles")) cycles();
    else if (!strcmp(argv[1], "reset")) reset_owners();
    else if (!strcmp(argv[1], "concurrent")) concurrent();
    else if (!strcmp(argv[1], "queued")) queued();
    else if (!strcmp(argv[1], "bindings")) state_bindings();
    else if (!strcmp(argv[1], "present")) present_queue();
    else if (!strcmp(argv[1], "last-presented")) last_presented();
    else if (!strcmp(argv[1], "synchronous")) synchronous();
    else if (!strcmp(argv[1], "oom-pool")) { uint64_t g[4] = {0}; fail_calloc_after = 0; create_pool(g, 16, false); return 99; }
    else if (!strcmp(argv[1], "oom-entries")) { uint64_t g[4] = {0}; fail_calloc_after = 1; create_pool(g, 16, false); return 99; }
    else if (!strcmp(argv[1], "oom-submit")) { uint64_t h = 0; fail_calloc_after = 0; mk8_async_submit(1, &h); return 99; }
    else if (!strcmp(argv[1], "invalid")) { uint64_t g[4] = {0}; create_pool(g, -1, false); return 99; }
    else return 98;
    return 0;
}
'''
        source = cls.out/'descriptors.c'
        source.write_text(cls.source, encoding='utf-8')
        cls.exe = cls.out/'descriptors.exe'
        cls.env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=str(ROOT/'local/native-tools/cache'))
        cls.env.pop('MK8_ASYNC_SUBMIT', None)
        cls.env.pop('MK8_TEXPOOL_CUBE_HISTORY', None)
        result = subprocess.run([str(DEFAULT_COMPILER), 'cc', '-target', 'x86_64-windows-gnu', '-O1', '-UNDEBUG',
                                 '-I'+str(cls.runtime), '-I'+str(DEPS/'include'), str(source), '-o', str(cls.exe)],
                                env=cls.env, capture_output=True, text=True, timeout=300)
        if result.returncode:
            raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.folder.cleanup()

    def run_case(self, case):
        result = subprocess.run([str(self.exe), case], env=self.env, capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_finalization_preserves_recorded_copied_and_bound_references(self):
        self.run_case('ownership')

    def test_more_than_64_pools_and_reused_guest_address(self):
        self.run_case('cycles')

    def test_reset_releases_recording_active_and_retired_lists(self):
        self.run_case('reset')

    def test_reference_count_across_eight_threads(self):
        self.run_case('concurrent')

    def test_finalization_behind_blocked_translation(self):
        self.run_case('queued')

    def test_executor_bindings_own_but_prefetch_borrows(self):
        self.run_case('bindings')

    def test_queued_presentation_keeps_texture_alive(self):
        self.run_case('present')

    def test_repeated_presentation_and_replacement(self):
        self.run_case('last-presented')

    def test_synchronous_translation_drains_retired_pools(self):
        result = subprocess.run([str(self.exe), 'synchronous'], env=dict(self.env, MK8_ASYNC_SUBMIT='0'),
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_invalid_size_stops_instead_of_allocating(self):
        result = subprocess.run([str(self.exe), 'invalid'], env=self.env, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 77)

    def test_allocation_failure_reports_fatal_without_null_dereference(self):
        for case in ('oom-pool', 'oom-entries', 'oom-submit'):
            with self.subTest(case=case):
                result = subprocess.run([str(self.exe), case], env=self.env, capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 77)


if __name__ == '__main__':
    unittest.main()
