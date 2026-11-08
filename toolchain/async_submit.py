"""Command translation on its own thread, so the game's submitting thread does not wait for it.

nvnQueueSubmitCommands translated the submitted lists to Vulkan before returning, on the game's own thread. An MK8
race frame there is about 10 ms of game work plus the translation (median 5.5 ms, 9-15 ms in slow frames), so
every slower translation became a late frame (docs/STATUS.md, 07/11/2026). On the console the GPU reads the
commands later, while the CPU goes on. Now submissions, presents and queue fences go into one ordered queue
served by a single translation thread (MK8_ASYNC_SUBMIT=0 restores the synchronous path):

- the game continues at once; a present waits only while more than MK8_ASYNC_AHEAD (default 1) presents
  are still untranslated, about the lag a real GPU has;
- every queued item has a ticket. A queue fence takes the ticket of its own item; a fence recorded in a
  command list takes the ticket of the submission that holds it (the lists are scanned when submitted).
  SyncWait first waits for the translation to pass that ticket, then for the GPU, as before. QueueFinish
  waits for everything queued;
- the translation reads guest memory later than before. The game protects memory still in use by the GPU
  with these fences, as it must on the console.

Grand Prix, frames 3600-7990, 07/11/2026: 57.89 FPS / p95 21.2 ms at the start of the day, 59.28 / 17.4 ms with
the 32-slot ring, 59.99 / 17.35 ms (p99 17.64, worst 30 ms) with this. Time Trial game clock: 50.003 s in 50.164 s.
A queue fence first submits the open work on this thread, so SyncWait never flushes from the game's thread: an
early version did and two threads recorded into one command pool (vkBeginCommandBuffer failed, boot logo).

The translation never calls guest code or the emulated kernel (checked in exec.c, textures.c, device.c).
Command lists stay alive for 20000 newer lists (cmdbuf.c GRAVE_DELAY) and their textures are retained
(texture_lifetime.py), so a queued list is never freed under the translator.
"""

SYNC_OLD = '''typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;'''
SYNC_NEW = '''typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
    volatile uint64_t mk8_ticket;  // (toolchain/async_submit.py) the queued item that sets seq; 0 = none
} NvnSync;'''

QUEUE = '''
// MK8 (toolchain/async_submit.py): submissions, presents and queue fences translated in order on one thread
static bool mk8_async_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("MK8_ASYNC_SUBMIT"); on = !e || atoi(e) != 0; if (on) LOG("[mk8] async command translation on\\n"); }
    return on;
}
enum { MK8_Q_SUBMIT, MK8_Q_PRESENT, MK8_Q_FENCE };
typedef struct Mk8QItem {
    int kind, n;
    uint64_t ticket, *handles;
    NvnTexture *tex;
    NvnSync *sync;
    struct Mk8QItem *next;
} Mk8QItem;
static pthread_mutex_t mk8_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mk8_q_cond = PTHREAD_COND_INITIALIZER;  // (an item queued or done)
static Mk8QItem *mk8_q_head, *mk8_q_tail;
static uint64_t mk8_q_issued, mk8_q_done;
static int mk8_q_presents;  // queued presents not yet done
static void mk8_present_now(NvnTexture *t);  // (below, with nvnQueuePresentTexture)
uint64_t renderer_fence_seq(void);
void renderer_submit(void);
static void *mk8_q_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&mk8_q_lock);
        while (!mk8_q_head) pthread_cond_wait(&mk8_q_cond, &mk8_q_lock);
        Mk8QItem *it = mk8_q_head;
        mk8_q_head = it->next;
        if (!mk8_q_head) mk8_q_tail = NULL;
        pthread_mutex_unlock(&mk8_q_lock);
        if (it->kind == MK8_Q_SUBMIT) {
            backend_prefetch(it->handles, it->n);
            for (int i = 0; i < it->n; i++) backend_execute(it->handles[i]);
            renderer_prefetch_verify();
        } else if (it->kind == MK8_Q_PRESENT) {
            mk8_present_now(it->tex);
        } else {
            // (the open submission goes out here, on this thread: a SyncWait then only waits for the GPU and never
            // flushes from the game's thread, which would record into this thread's command buffers)
            renderer_submit();
            it->sync->seq = renderer_fence_seq();
        }
        pthread_mutex_lock(&mk8_q_lock);
        mk8_q_done = it->ticket;
        if (it->kind == MK8_Q_PRESENT) mk8_q_presents--;
        pthread_cond_broadcast(&mk8_q_cond);
        pthread_mutex_unlock(&mk8_q_lock);
        free(it->handles);
        free(it);
    }
    return NULL;
}
// Queues an item (syncs: tagged with its ticket in the same step) and returns its ticket.
static uint64_t mk8_q_put(Mk8QItem *it, NvnSync **syncs, int nsyncs) {
    static bool started;
    pthread_mutex_lock(&mk8_q_lock);
    if (!started) {
        pthread_t t;
        if (pthread_create(&t, NULL, mk8_q_worker, NULL) != 0) FATAL("async translation thread\\n");
        pthread_detach(t);
        started = true;
    }
    it->ticket = ++mk8_q_issued;
    for (int i = 0; i < nsyncs; i++) syncs[i]->mk8_ticket = it->ticket;
    if (it->kind == MK8_Q_PRESENT) mk8_q_presents++;
    it->next = NULL;
    if (mk8_q_tail) mk8_q_tail->next = it; else mk8_q_head = it;
    mk8_q_tail = it;
    pthread_cond_broadcast(&mk8_q_cond);
    pthread_mutex_unlock(&mk8_q_lock);
    return it->ticket;
}
static void mk8_q_wait(uint64_t ticket) {
    pthread_mutex_lock(&mk8_q_lock);
    while (mk8_q_done < ticket) pthread_cond_wait(&mk8_q_cond, &mk8_q_lock);
    pthread_mutex_unlock(&mk8_q_lock);
}
static void mk8_q_drain(void) {
    pthread_mutex_lock(&mk8_q_lock);
    uint64_t t = mk8_q_issued;
    pthread_mutex_unlock(&mk8_q_lock);
    mk8_q_wait(t);
}
// The fences recorded in submitted lists (CMD_FENCE_SYNC), following jumps and calls as exec_list does.
static int mk8_scan_fences(uint64_t p, int depth, NvnSync **out, int n, int cap) {
    for (int guard = 0; guard < 10000000; guard++) {
        CmdRec *r = (CmdRec *)(uintptr_t)p;
        switch (r->op) {
        case CMD_END: return n;
        case CMD_JUMP: p = r->x[0]; continue;
        case CMD_CALL: {
            const uint64_t *h = (const uint64_t *)r->extra;
            for (uint64_t i = 0; i < r->x[0] && depth < 8; i++) n = mk8_scan_fences(h[i], depth + 1, out, n, cap);
            break;
        }
        case CMD_FENCE_SYNC:
            if (r->x[1]) { if (n < cap) out[n] = sync_of(r->x[1]); n++; }
            break;
        default: break;
        }
        p += r->size;
    }
    return n;
}
static bool mk8_async_submit(int n, const uint64_t *handles) {
    if (!mk8_async_on() || n <= 0) return false;
    Mk8QItem *it = calloc(1, sizeof *it);
    it->handles = malloc((size_t)n * sizeof *it->handles);
    if (!it || !it->handles) FATAL("async translation: out of memory\\n");
    memcpy(it->handles, handles, (size_t)n * sizeof *it->handles);
    it->kind = MK8_Q_SUBMIT;
    it->n = n;
    NvnSync *syncs[256];
    int nsyncs = 0;
    for (int i = 0; i < n; i++) if (handles[i]) nsyncs = mk8_scan_fences(handles[i], 0, syncs, nsyncs, 256);
    uint64_t ticket = mk8_q_put(it, syncs, nsyncs < 256 ? nsyncs : 256);
    if (nsyncs > 256) mk8_q_wait(ticket);  // (more fences than tagged: this submission completes in order first)
    return true;
}
'''

SUBMIT_OLD = '''    backend_prefetch(handles, n);
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
    renderer_prefetch_verify();
}'''
SUBMIT_NEW = '''    extern bool mk8_async_submit_hook(int n, const uint64_t *handles);
    if (mk8_async_submit_hook(n, handles)) return;  // (toolchain/async_submit.py)
    backend_prefetch(handles, n);
    for (int i = 0; i < n; i++) backend_execute(handles[i]);
    renderer_prefetch_verify();
}'''

FINISH_OLD = 'NVN(nvnQueueFinish) { (void)c; renderer_sync(); }'
FINISH_NEW = 'NVN(nvnQueueFinish) { (void)c; extern void mk8_async_drain_hook(void); mk8_async_drain_hook(); renderer_sync(); }'

FENCE_OLD = 'NVN(nvnQueueFenceSync) { NvnSync *s = sync_of(A(1)); s->signaled = 1; s->seq = renderer_fence_seq(); }'
FENCE_NEW = '''NVN(nvnQueueFenceSync) {
    NvnSync *s = sync_of(A(1));
    s->signaled = 1;
    if (mk8_async_on()) {  // (toolchain/async_submit.py) its submission number once the work before it is translated
        Mk8QItem *it = calloc(1, sizeof *it);
        if (!it) FATAL("async translation: out of memory\\n");
        it->kind = MK8_Q_FENCE;
        it->sync = s;
        mk8_q_put(it, &s, 1);
        return;
    }
    s->seq = renderer_fence_seq();
}
bool mk8_async_submit_hook(int n, const uint64_t *handles) { return mk8_async_submit(n, handles); }
void mk8_async_drain_hook(void) { if (mk8_async_on()) mk8_q_drain(); }'''

WAIT_OLD = '''    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else renderer_wait_seq(sync_of(A(0))->seq);  // only the submission that holds this fence'''
WAIT_NEW = '''    if (mk8_async_on()) {  // (toolchain/async_submit.py) the translation reaches the fence first
        if (flush || whole) mk8_q_drain();
        else mk8_q_wait(sync_of(A(0))->mk8_ticket);
    }
    if (flush) renderer_flush();
    else if (whole) renderer_sync();
    else renderer_wait_seq(sync_of(A(0))->seq);  // only the submission that holds this fence'''

PRESENT_OLD = '''    if (i >= 0 && i < w->num) {
        pthread_mutex_lock(&g_present_mx);
        backend_present(HOST(NvnTexture, w->textures[i]));
        atomic_store(&g_game_present_ns, plat_now_ns());
        pthread_mutex_unlock(&g_present_mx);
    }
    kernel_host_wait_end();
}'''
PRESENT_NEW = '''    if (i >= 0 && i < w->num) {
        NvnTexture *t = HOST(NvnTexture, w->textures[i]);
        if (mk8_async_on()) {  // (toolchain/async_submit.py) queued after this frame's work; at most AHEAD frames ahead
            static int ahead = -1;
            if (ahead < 0) { const char *e = getenv("MK8_ASYNC_AHEAD"); ahead = e && atoi(e) > 0 ? atoi(e) : 1; }
            Mk8QItem *it = calloc(1, sizeof *it);
            if (!it) FATAL("async translation: out of memory\\n");
            it->kind = MK8_Q_PRESENT;
            it->tex = t;
            mk8_q_put(it, NULL, 0);
            pthread_mutex_lock(&mk8_q_lock);
            while (mk8_q_presents > ahead) pthread_cond_wait(&mk8_q_cond, &mk8_q_lock);
            pthread_mutex_unlock(&mk8_q_lock);
        } else {
            mk8_present_now(t);
        }
    }
    kernel_host_wait_end();
}
static void mk8_present_now(NvnTexture *t) {
    pthread_mutex_lock(&g_present_mx);
    backend_present(t);
    atomic_store(&g_game_present_ns, plat_now_ns());
    pthread_mutex_unlock(&g_present_mx);
}'''

SYNC_OF = '''static NvnSync *sync_of(uint64_t guest) {
    NvnSync *s = HOST(NvnSync, guest);
    if (!s) { s = calloc(1, sizeof *s); SET_HOST(guest, s); }
    return s;
}
'''


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Async translation boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    header = runtime/'nvn/nvn.h'
    header.write_text(replace(header.read_text(encoding='utf-8'), SYNC_OLD, SYNC_NEW), encoding='utf-8')
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    text = replace(text, SYNC_OF, SYNC_OF + QUEUE)
    for old, new in [(SUBMIT_OLD, SUBMIT_NEW), (FINISH_OLD, FINISH_NEW), (FENCE_OLD, FENCE_NEW),
                     (WAIT_OLD, WAIT_NEW), (PRESENT_OLD, PRESENT_NEW)]:
        text = replace(text, old, new)
    objects.write_text(text, encoding='utf-8')
