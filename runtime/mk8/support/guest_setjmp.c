#include "rt.h"
#include <setjmp.h>
#include <stdint.h>

// Guest setjmp/longjmp across recompiled frames (MK8 4.0.1 sdk, nn::image JPEG paths).
//
// Guest longjmp restores x19-x30, sp and d8-d15 from the jmp_buf and branches to x30: the instruction after the
// guest `bl setjmp`. That address is inside a recompiled C function, not an entry. toolchain/experimental.py wraps
// each verified call site in a host __builtin_setjmp inside that function's own frame and calls mk8_setjmp_arm; the
// guest return addresses below are dispatch overrides that jump back to that host frame. Guest registers are
// already those longjmp restored. Each guest call is a host C call and a guest tail call replaces the host frame
// too, so a guest jmp_buf that is still valid (its caller active) has its host frame live. A resume with no slot
// for this site and guest sp, or with a slot not above the current host frame, stops: no fictitious return.

#define MK8_SETJMP_SLOTS 16

typedef struct {
    uint64_t resume, sp;
    void *frame;     // host frame of the recompiled function that called setjmp
    jmp_buf buf;     // CRT setjmp (it unwinds through the frames in between, as kernel.c's thread exit does)
} Mk8Jump;

static __thread Mk8Jump slots[MK8_SETJMP_SLOTS];
static __thread unsigned next_slot;

// Return addresses of the four guest `bl setjmp` sites verified by toolchain/experimental.py (SETJMP_SITES).
static const uint64_t resumes[] = {0x700402e874ull, 0x700402efe0ull, 0x70041f8928ull, 0x70043e7188ull};

jmp_buf *mk8_setjmp_arm(Cpu *c, uint64_t resume, void *frame) {
    Mk8Jump *pick = NULL;
    for (unsigned i = 0; i < MK8_SETJMP_SLOTS && !pick; i++)
        if (slots[i].resume == resume && slots[i].sp == c->sp) pick = &slots[i];
    for (unsigned i = 0; i < MK8_SETJMP_SLOTS && !pick; i++)
        if (!slots[i].frame || (uintptr_t)slots[i].frame <= (uintptr_t)frame) pick = &slots[i];  // empty, or not above us: dead
    if (!pick) pick = &slots[next_slot++ % MK8_SETJMP_SLOTS];
    pick->resume = resume;
    pick->sp = c->sp;
    pick->frame = frame;
    return &pick->buf;
}

static void mk8_setjmp_resume(Cpu *c) {
    void *here = __builtin_frame_address(0);
    for (unsigned i = 0; i < MK8_SETJMP_SLOTS; i++) {
        Mk8Jump *s = &slots[i];
        if (s->frame && s->resume == c->pc && s->sp == c->sp && (uintptr_t)s->frame > (uintptr_t)here) {
            static int logged;
            if (!logged++) LOG("[mk8:setjmp] guest longjmp to %llx resumed in its recompiled frame\n", (unsigned long long)c->pc);
            longjmp(s->buf, 1);
        }
    }
    FATAL("guest longjmp to %llx (sp %llx) without a live setjmp frame\n", (unsigned long long)c->pc,
          (unsigned long long)c->sp);
}

__attribute__((constructor)) static void mk8_setjmp_register(void) {
    for (unsigned i = 0; i < sizeof resumes / sizeof *resumes; i++) hle_override(resumes[i], mk8_setjmp_resume, "mk8 setjmp resume");
}
