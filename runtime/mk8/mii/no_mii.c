#include "hle.h"
#include <string.h>
#include <stdatomic.h>

// shortcut: bypass verified Mii preparation and model policy paths. Other Mii
// consumers still need qualification before this can be called a playable mode.
int mk8_no_mii_renderer(void) {
    const char *mode = getenv("MK8_EXPERIMENTAL_NO_MII");
    if (!mode || strcmp(mode, "1")) return 0;
    static atomic_flag announced = ATOMIC_FLAG_INIT;
    if (!atomic_flag_test_and_set(&announced))
        LOG("[mk8:no-mii] Mii manager preparation/update disabled; gameplay unqualified\n");
    return 1;
}
