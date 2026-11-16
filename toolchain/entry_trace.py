"""Read-only entry tracing for reverse engineering (runtime/mk8/support/entry_trace.c, MK8_TRACE_ENTRY).

The prepared fnprof.c calls mk8_trace_entry first in fnprof_hit, the function every generated FN_ENTER reaches while
the hook is on. Without MK8_TRACE_ENTRY the hook stays off (g_fnprof 0) and nothing changes; with it, listed entries
are logged and, unless the profiler itself was asked for, nothing is counted.
"""

HIT = 'void fnprof_hit(uint64_t a, uint64_t lr, uint64_t x0) {\n'
HIT_NEW = HIT + '''    extern int mk8_trace_entry(uint64_t addr, uint64_t lr);  // MK8 (toolchain/entry_trace.py): read only
    if (mk8_trace_entry(a, lr)) return;
'''


def prepare(runtime):
    path = runtime/'fnprof.c'
    text = path.read_text(encoding='utf-8')
    if text.count(HIT) != 1 or 'mk8_trace_entry' in text:
        raise ValueError('Function profiler boundary changed: fnprof.c')
    path.write_text(text.replace(HIT, HIT_NEW), encoding='utf-8')
