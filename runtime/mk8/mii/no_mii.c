#include "hle.h"
#include <string.h>
#include <stdatomic.h>

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);

// Diagnostic only: identify the actual guest consumers reached by a Mii race.
// Never dereference a guest model or change the existing no-model policy here.
static void mk8_trace_mii_consumer(uint64_t caller) {
    if (!getenv("MK8_TRACE_MII_CONSUMERS")) return;
    static atomic_flag lock = ATOMIC_FLAG_INIT;
    static uint64_t seen[64];
    static unsigned count;
    // Tracing must not stall rendering when another thread is logging.
    if (atomic_flag_test_and_set(&lock)) return;
    for (unsigned i = 0; i < count; i++) {
        if (seen[i] == caller) {
            atomic_flag_clear(&lock);
            return;
        }
    }
    if (count < sizeof seen / sizeof *seen) {
        seen[count++] = caller;
        uint64_t offset = 0;
        const char *name = rt_host_fn_name(caller, &offset);
        LOG("[mk8:mii-consumer] disabled caller=%s host_offset=%llx\n",
            name ? name : "unknown", (unsigned long long)offset);
    }
    atomic_flag_clear(&lock);
}

// shortcut: bypass verified Mii preparation and model policy paths. Other Mii
// consumers still need qualification before this can be called a playable mode.
int mk8_no_mii_renderer(void) {
    const char *mode = getenv("MK8_EXPERIMENTAL_NO_MII");
    if (!mode || strcmp(mode, "1")) return 0;
    mk8_trace_mii_consumer((uint64_t)(uintptr_t)__builtin_return_address(0));
    static atomic_flag announced = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&announced))
        LOG("[mk8:no-mii] Mii manager preparation/update disabled; gameplay unqualified\n");
    return 1;
}
