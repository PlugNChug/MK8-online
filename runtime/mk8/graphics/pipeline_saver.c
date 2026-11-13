// MK8 (toolchain/pipeline_cache_save.py): the Vulkan pipeline cache (cache/vk_pipelines.bin) was written only by the
// exit handler. A session that ended any other way (a crash, the process killed) lost every pipeline the driver built,
// and the next session compiled them again in the middle of races (hundreds per load screen, then tens per second).
// It is now also saved while the game runs: once new pipelines exist and none was built for a while (so the save does
// not compete with a compile burst), at most once a minute, on this thread. MK8_NO_PIPELINE_SAVE=1: exit only.
#include "rt.h"
#include "platform.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

void vk_pipeline_cache_save(void);

#define QUIET_NS (10ull * 1000000000ull)
#define INTERVAL_NS (60ull * 1000000000ull)
#define POLL_NS (5ull * 1000000000ull)

static _Atomic uint64_t g_built, g_last_built_ns;

void mk8_pipeline_built(void) {
    atomic_store(&g_last_built_ns, plat_now_ns());
    atomic_fetch_add(&g_built, 1);
}

static void *saver(void *arg) {
    (void)arg;
    // Builds completed before this thread started have not been persisted yet.
    uint64_t saved = 0, last_save = plat_now_ns();
    for (;;) {
        plat_sleep_until_ns(plat_now_ns() + POLL_NS);
        uint64_t built = atomic_load(&g_built), now = plat_now_ns();
        if (built == saved || now - atomic_load(&g_last_built_ns) < QUIET_NS || now - last_save < INTERVAL_NS) continue;
        uint64_t t0 = plat_now_ns();
        vk_pipeline_cache_save();
        LOG("[mk8:pipelines] cache saved: %llu new pipelines (%.0f ms)\n", (unsigned long long)(built - saved),
            (double)(plat_now_ns() - t0) / 1e6);
        saved = built;
        last_save = plat_now_ns();
    }
    return NULL;
}

void mk8_pipeline_saver_start(void) {
    static atomic_flag started = ATOMIC_FLAG_INIT;
    if (atomic_flag_test_and_set(&started) || getenv("MK8_NO_PIPELINE_SAVE")) return;
    pthread_t thread;
    if (pthread_create(&thread, NULL, saver, NULL) == 0) pthread_detach(thread);
}
