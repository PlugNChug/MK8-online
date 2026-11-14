"""Let a thread of better priority take its emulated core at the running thread's next indirect call.

The pinned kernel hands a core over only at kernel calls (core_check in rt_svc). Horizon preempts at once: a thread
of better priority that becomes ready takes the core from the running one. At the end of every race a worker of
priority 45 builds the next screen for ~0.5 s without a kernel call while holding core 0, and the main thread (44)
waited for it: frames of 475-545 ms on crossing the line (14/11/2026; the guest's Yaz0 loop, now native, held core 2
the same way). Guest code passes through rt_dispatch at every indirect call (virtual calls, function pointers): it
reads one global flag, set only while some core has a waiter of better priority than its owner, and then does what
rt_svc does, core_check under g_klock (trylock: never waits for the lock there). One thread per core still runs at a
time; a thread of better priority taking the core at an indirect call is an interleaving Horizon allows at any
instruction, and a guest call could already give the core up at its next kernel call. MK8_NO_PREEMPT=1: off.
"""

FLAGS_BOUNDARY = '// Give the core up; it goes to the best waiter (handed over, so nobody else slips in)\n'
FLAGS = '''// MK8 (toolchain/core_preempt.py): which cores have a waiter of better priority than their owner (caller holds g_klock).
_Atomic int mk8_preempt_any;
static _Atomic int mk8_core_contended[4];
static void mk8_core_flags(void) {
    static int off = -1;
    if (off < 0) off = getenv("MK8_NO_PREEMPT") != NULL;
    int any = 0;
    for (int i = 0; i < 4; i++) {
        KThread *owner = g_cores[i].owner, *w = owner && !off ? core_best_waiter(i) : NULL;
        int contended = w && w->prio < owner->prio;
        atomic_store(&mk8_core_contended[i], contended);
        any |= contended;
    }
    atomic_store(&mk8_preempt_any, any);
}

'''
RELEASE = ('    g_cores[t->core].owner = w;\n'
           '    if (w) w->want_core = false, pthread_cond_signal(&w->cond);\n')
WAIT = '    t->want_core = true;\n    uint64_t t0 = plat_now_ns();\n'
ACQUIRED = ('    t->want_core = false;\n'
            '    if (t->core >= 0 && g_cores[t->core].owner != t) g_cores[t->core].owner = t;  // (reboot unwinding)\n')
MOVED = '    t->core = core;\n    if (held && t == t_cur) core_acquire(t);\n'
POINT_BOUNDARY = '// A thread moved to another core (SetThreadCoreMask)\n'
POINT = '''// MK8 (toolchain/core_preempt.py): rt_dispatch calls this while mk8_preempt_any is set. The running thread gives its
// core to a waiter of better priority, as at a kernel call (rt_svc), and waits for it back.
void mk8_preempt_point(void) {
    KThread *t = t_cur;
    if (!t || t->core < 0 || cores_off > 0 || !atomic_load(&mk8_core_contended[t->core])) return;
    if (pthread_mutex_trylock(&g_klock) != 0) return;  // (taken: the next indirect call looks again)
    if (g_cores[t->core].owner == t) core_check(t);
    pthread_mutex_unlock(&g_klock);
}

'''
DISPATCH = ('void rt_dispatch(Cpu *c) {\n'
            '    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);\n')
DISPATCH_NEW = ('extern _Atomic int mk8_preempt_any;  // MK8 (toolchain/core_preempt.py): runtime kernel.c\n'
                'void mk8_preempt_point(void);\n'
                'void rt_dispatch(Cpu *c) {\n'
                '    if (__builtin_expect(atomic_load_explicit(&mk8_preempt_any, memory_order_relaxed), 0)) mk8_preempt_point();\n'
                '    unsigned h = (unsigned)(c->pc >> 2) & (DCACHE - 1);\n')


def prepare(runtime):
    kernel = runtime/'kernel.c'
    text = kernel.read_text(encoding='utf-8')
    for boundary in (FLAGS_BOUNDARY, RELEASE, WAIT, ACQUIRED, MOVED, POINT_BOUNDARY):
        if text.count(boundary) != 1:
            raise ValueError(f'Core preemption boundary changed: {boundary[:60]}')
    if 'mk8_preempt_point' in text:
        raise ValueError('Core preemption already prepared')
    text = text.replace(FLAGS_BOUNDARY, FLAGS + FLAGS_BOUNDARY)
    text = text.replace(RELEASE, RELEASE + '    mk8_core_flags();\n')
    text = text.replace(WAIT, '    t->want_core = true;\n    mk8_core_flags();\n    uint64_t t0 = plat_now_ns();\n')
    text = text.replace(ACQUIRED, ACQUIRED + '    mk8_core_flags();\n')
    text = text.replace(MOVED, '    t->core = core;\n    mk8_core_flags();\n    if (held && t == t_cur) core_acquire(t);\n')
    text = text.replace(POINT_BOUNDARY, POINT + POINT_BOUNDARY)
    if '#include <stdatomic.h>' not in text:
        text = '#include <stdatomic.h>\n' + text
    kernel.write_text(text, encoding='utf-8')
    dispatch = runtime/'dispatch.c'
    text = dispatch.read_text(encoding='utf-8')
    if text.count(DISPATCH) != 1 or 'mk8_preempt_point' in text:
        raise ValueError('Core preemption boundary changed: dispatch.c')
    text = text.replace(DISPATCH, DISPATCH_NEW)
    if '#include <stdatomic.h>' not in text:
        text = '#include <stdatomic.h>\n' + text
    dispatch.write_text(text, encoding='utf-8')
