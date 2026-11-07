#include "hle.h"
#include <stdio.h>
#include <string.h>

void mk8_heap_diagnostic(Cpu *c) {
    if (c && c->x[30] == 0x700058714cull && c->x[21] >= GUEST_LO && c->x[21] < GUEST_HI - 0x1940) {
        uint64_t manager = c->x[21], font;
        memcpy(&font, (void *)(uintptr_t)(manager + 0x10), 8);
        if (font >= GUEST_LO && font < GUEST_HI - 0xb8) {
            uint32_t error;
            memcpy(&error, (void *)(uintptr_t)(font + 0x48), 4);
            fprintf(stderr, "[mk8:font-fault] manager=%llx font=%llx FS_error=%x\n", (unsigned long long)manager, (unsigned long long)font, error);
        }
    }
    if (!c || c->x[30] != 0x7000605fd8ull || c->x[20] < GUEST_LO || c->x[20] > GUEST_HI - 0x100)
        return;
    const unsigned char *heap = (const unsigned char *)(uintptr_t)c->x[20];
    fprintf(stderr, "[mk8:heap] allocator=%llx request=%llx alignment=%llx\n", c->x[20], c->x[1], c->x[2]);
    for (unsigned offset = 0; offset < 0x100; offset += 16) {
        uint64_t a, b;
        memcpy(&a, heap + offset, 8); memcpy(&b, heap + offset + 8, 8);
        fprintf(stderr, "[mk8:heap] +%02x %016llx %016llx\n", offset, (unsigned long long)a, (unsigned long long)b);
    }
    uint64_t node;
    memcpy(&node, heap + 0xe0, 8);
    if (node >= GUEST_LO + 0x30 && node < GUEST_HI - 0x50) {
        for (int offset = -0x30; offset < 0x50; offset += 16) {
            uint64_t a, b;
            memcpy(&a, (void *)(uintptr_t)(node + offset), 8);
            memcpy(&b, (void *)(uintptr_t)(node + offset + 8), 8);
            fprintf(stderr, "[mk8:free-node] %+d %016llx %016llx\n", offset, (unsigned long long)a, (unsigned long long)b);
        }
    }
}
