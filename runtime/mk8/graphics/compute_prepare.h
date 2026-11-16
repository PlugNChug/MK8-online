#ifndef MK8_COMPUTE_PREPARE_H
#define MK8_COMPUTE_PREPARE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* A antecipação nunca conserva ponteiros para o programa/controle do guest. */
enum { MK8_COMPUTE_PREPARE_LIMIT = 64, MK8_COMPUTE_PREPARE_BYTES = 0x10000 };
typedef struct Mk8ComputeSnapshot {
    uint64_t addr, frame, fingerprint;
    size_t size;
    uint32_t local[3];
    _Alignas(8) uint8_t code[];
} Mk8ComputeSnapshot;

static bool mk8_compute_guest_range(uint64_t addr, size_t size) {
    return addr >= GUEST_LO && addr < GUEST_HI && size <= GUEST_HI - addr;
}

static bool mk8_compute_inputs(const NvnShaderStage *ss, size_t *size, uint32_t local[3]) {
    /* O fallback do tradutor é limite de busca, não garantia de memória legível. */
    if (!ss->const_offset) return false;
    *size = ss->const_offset;
    if (*size < 8 || *size > MK8_COMPUTE_PREPARE_BYTES ||
        !mk8_compute_guest_range(ss->code_addr, *size)) return false;
    if (ss->control && !mk8_compute_guest_range((uintptr_t)ss->control, 0x754)) return false;
    for (int i = 0; i < 3; i++) {
        local[i] = 1;
        if (ss->control) memcpy(&local[i], ss->control + 0x748 + 4 * i, 4);
        if (!local[i]) local[i] = 1;
    }
    return true;
}

static Mk8ComputeSnapshot *mk8_compute_snapshot(const NvnShaderStage *ss, uint64_t frame) {
    size_t size;
    uint32_t local[3];
    if (!mk8_compute_inputs(ss, &size, local)) return NULL;
    Mk8ComputeSnapshot *s = malloc(sizeof *s + size);
    if (!s) return NULL;  /* Preparação opcional: o primeiro uso mantém o caminho normal. */
    s->addr = ss->code_addr;
    s->frame = frame;
    s->size = size;
    memcpy(s->local, local, sizeof local);
    memcpy(s->code, (const void *)(uintptr_t)s->addr, size);
    s->fingerprint = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < size; i++) s->fingerprint = (s->fingerprint ^ s->code[i]) * 0x100000001b3ull;
    return s;
}

static bool mk8_compute_matches(const Mk8ComputeSnapshot *s, const NvnShaderStage *ss) {
    size_t size;
    uint32_t local[3];
    return ss->code_addr == s->addr && mk8_compute_inputs(ss, &size, local) && size == s->size &&
           !memcmp(local, s->local, sizeof local) && !memcmp(s->code, (const void *)(uintptr_t)s->addr, size);
}

void mk8_compute_prepare(Mk8ComputeSnapshot *s);
#endif
