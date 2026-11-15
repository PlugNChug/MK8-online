// Diagnóstico somente leitura para a engenharia reversa do CTGP (fase B, docs/STATUS.md): registra a entrada em
// funções guest escolhidas, com x0..x3, o chamador e, se pedido, bytes da memória apontada por um registrador.
// Não altera registradores, memória nem fluxo do jogo.
//
// MK8_TRACE_ENTRY=<endereço hex>[:x<reg>:<bytes>][,...]   ex.: 700043c2e8:x1:48,7000418ed0
// MK8_TRACE_ENTRY_MAX=<n>: entradas registradas por função (padrão 64).
// Liga o gancho FN_ENTER de toda função (como o perfilador LARECOMP_FNPROF), então o jogo fica mais lento.
#include "rt.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MK8_TRACE_MAX 16

extern volatile int g_fnprof;

static struct {
    uint64_t addr;
    int reg, bytes;
    _Atomic int seen;
} g_trace[MK8_TRACE_MAX];
static int g_trace_n = -1, g_trace_limit = 64, g_trace_profiling;

// Parses the list; returns how many functions it names (0 for none or an invalid list, which traces nothing).
int mk8_trace_entry_parse(const char *spec) {
    g_trace_n = 0;
    for (const char *p = spec; p && *p && g_trace_n < MK8_TRACE_MAX;) {
        char *end;
        uint64_t addr = strtoull(p, &end, 16);
        if (end == p || addr < GUEST_LO || addr >= GUEST_HI) return g_trace_n = 0;
        int reg = -1, bytes = 0;
        if (*end == ':') {
            if (end[1] != 'x') return g_trace_n = 0;
            reg = (int)strtol(end + 2, &end, 10);
            if (reg < 0 || reg > 30 || *end != ':') return g_trace_n = 0;
            bytes = (int)strtol(end + 1, &end, 10);
            if (bytes <= 0 || bytes > 256) return g_trace_n = 0;
        }
        g_trace[g_trace_n].addr = addr, g_trace[g_trace_n].reg = reg, g_trace[g_trace_n].bytes = bytes;
        atomic_store(&g_trace[g_trace_n].seen, 0);
        g_trace_n++;
        p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    return g_trace_n;
}

__attribute__((constructor)) static void mk8_trace_entry_init(void) {
    const char *spec = getenv("MK8_TRACE_ENTRY"), *max = getenv("MK8_TRACE_ENTRY_MAX");
    if (!spec || !*spec) { g_trace_n = 0; return; }
    if (max) g_trace_limit = atoi(max) > 0 ? atoi(max) : g_trace_limit;
    g_trace_profiling = getenv("LARECOMP_FNPROF") || getenv("RECOMP_CALL_RING");
    if (mk8_trace_entry_parse(spec)) g_fnprof = 1;
    else fprintf(stderr, "[mk8:entry] MK8_TRACE_ENTRY inválido: %s\n", spec);
}

// Formats one entry (testable without a running game); returns the length written.
int mk8_trace_entry_line(char *out, size_t size, uint64_t addr, uint64_t lr, const uint64_t x[4], int reg, uint64_t at,
                         const uint8_t *mem, int bytes) {
    int n = snprintf(out, size, "[mk8:entry] f_%llx from %llx x0=%llx x1=%llx x2=%llx x3=%llx",
                     (unsigned long long)addr, (unsigned long long)lr, (unsigned long long)x[0],
                     (unsigned long long)x[1], (unsigned long long)x[2], (unsigned long long)x[3]);
    if (reg >= 0 && n > 0 && (size_t)n < size) {
        if (!mem) n += snprintf(out + n, size - n, " [x%d=%llx fora da memória guest]", reg, (unsigned long long)at);
        else {
            n += snprintf(out + n, size - n, " [x%d]", reg);
            for (int i = 0; i < bytes && n > 0 && (size_t)n < size; i++)
                n += snprintf(out + n, size - n, "%s%02x", i % 4 ? "" : " ", mem[i]);
        }
    }
    return n;
}

// From fnprof_hit (toolchain/entry_trace.py): 1 when the profiler proper is off and has nothing to count.
int mk8_trace_entry(uint64_t addr, uint64_t lr) {
    if (g_trace_n <= 0) return 0;
    for (int i = 0; i < g_trace_n; i++) {
        if (g_trace[i].addr != addr) continue;
        if (atomic_fetch_add(&g_trace[i].seen, 1) >= g_trace_limit) break;
        Cpu *c = kernel_current_cpu();
        if (!c) break;
        uint64_t x[4] = {c->x[0], c->x[1], c->x[2], c->x[3]}, at = 0;
        const uint8_t *mem = NULL;
        if (g_trace[i].reg >= 0) {
            at = c->x[g_trace[i].reg];
            if (at >= GUEST_LO && at + (uint64_t)g_trace[i].bytes <= GUEST_HI) mem = (const uint8_t *)(uintptr_t)at;
        }
        char line[1024];
        mk8_trace_entry_line(line, sizeof line, addr, lr, x, g_trace[i].reg, at, mem, g_trace[i].bytes);
        LOG("%s\n", line);
        break;
    }
    return !g_trace_profiling;
}
