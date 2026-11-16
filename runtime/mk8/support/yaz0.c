// MK8 (toolchain/yaz0_native.py): the decompression loop of the game's Yaz0 decoder f_7000a88a44 (main), run natively.
// The game decompresses the next screen's archives (two of ~92 MB) on a worker thread of priority 52 at the end of
// every race. As recompiled code (registers in memory, loads through the guest macros) this byte loop took ~0.5 s, and
// with no preemption inside guest code that worker held its emulated core, core 2, the whole time: threads of better
// priority there waited ~260 ms for it (14/11/2026, MK8_SAMPLE: all of the worker's CPU in this function). Natively the
// two archives take ~0.2 s each. (The main thread's own wait, on core 0, is toolchain/core_preempt.py.) The guest keeps
// the header: the magic and size reads through its own byte-order helpers, the -1 / -2
// returns and the `size < 1` exit; this runs from where its loop starts, with the same reads and writes in the same
// order and the registers the loop leaves (x8 size, x9 dst - 1, x10 bytes written, x11 mask, x12 flag byte, x13
// source offset, and the flags of the last compare); x14-x18 are scratch registers the caller cannot read (AAPCS64).
// MK8_YAZ0_GUEST=1: the guest loop instead.
#include "rt.h"
#include "platform.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static _Atomic uint64_t n_calls, n_bytes, n_ns;

int mk8_yaz0_loop(Cpu *c) {
    static int guest = -1;
    if (guest < 0) guest = getenv("MK8_YAZ0_GUEST") != NULL;
    if (guest) return 0;
    uint64_t t0 = plat_now_ns();
    const uint8_t *src = (const uint8_t *)(uintptr_t)c->x[19];
    uint8_t *dst = (uint8_t *)(uintptr_t)c->x[20];
    const int32_t size = (int32_t)(uint32_t)c->x[8];
    uint32_t mask = 0, code = (uint32_t)c->x[12], pos = 0x10, out = 0;
    do {
        if (!(mask & 0xff)) {
            code = src[(int32_t)pos];
            pos++;
            mask = 0x80;
        }
        if (code & mask & 0xff) {
            dst[(int32_t)out] = src[(int32_t)pos];
            pos++;
            out++;
        } else {
            const uint8_t *at = src + (int32_t)pos;
            uint32_t b0 = at[0], b1 = at[1];
            uint32_t back = out - (b1 | (b0 & 0xf) << 8);
            uint32_t n = b0 >> 4;
            if (n) {
                n += 2;
                pos += 2;
            } else {
                n = (uint32_t)at[2] + 0x12;
                pos += 3;
            }
            const uint8_t *from = dst - 1 + (int32_t)back;
            uint8_t *to = dst + (int32_t)out;
            uint32_t i = 0;
            do {  // (byte by byte: the copy may overlap what it writes)
                to[i] = from[i];
                i++;
            } while ((int32_t)i < (int32_t)n);
            out += i;
        }
        mask = (mask >> 1) & 0x7f;
    } while ((int32_t)out < size);
    c->x[9] = c->x[20] - 1;
    c->x[10] = out;
    c->x[11] = mask;
    c->x[12] = code;
    c->x[13] = pos;
    (void)subs32(c, out, (uint32_t)size);
    uint64_t calls = atomic_fetch_add(&n_calls, 1) + 1, bytes = atomic_fetch_add(&n_bytes, (uint32_t)size) + (uint32_t)size;
    uint64_t ns = atomic_fetch_add(&n_ns, plat_now_ns() - t0) + (plat_now_ns() - t0);
    if (size >= (1 << 20) || (calls & (calls - 1)) == 0)  // (large archives, and every power of two)
        LOG("[mk8:yaz0] %d KB in %.1f ms; %llu archives, %llu MB in %.0f ms so far\n", size >> 10,
            (double)(plat_now_ns() - t0) / 1e6, (unsigned long long)calls, (unsigned long long)(bytes >> 20), ns / 1e6);
    return 1;
}
