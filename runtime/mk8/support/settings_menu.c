#include "rt.h"
#include "hle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic bool visible, focused, pending;

RT_ON_RESET(settings_menu_reset) { visible = false; focused = false; pending = false; }

static bool enabled(void) {
    const char *value = getenv("MK8_SETTINGS_EXPERIMENT");
    return value && !strcmp(value, "1");
}

// The verified top-menu callback reports focus (0/1) and confirmation (2).
// shortcut: reuse the Booster Course Pass tile for the first experiment; add a new native slot after navigation is proven.
void mk8_settings_top_event(uint64_t page, uint64_t event) {
    if (!enabled()) return;
    if (page < HEAP_BASE || page >= HEAP_BASE + HEAP_MAX || 0x700 > HEAP_BASE + HEAP_MAX - page ||
        event < GUEST_LO || event >= GUEST_HI || 0x38 > GUEST_HI - event)
        FATAL("[mk8:settings] unexpected top-menu object/event\n");
    uint32_t id = LD32(event + 8), kind = LD32(event + 0x28);
    if (kind == 0 && (id <= 40 || (id >= 100 && id <= 106))) {
        visible = true;
        focused = id == 105;
    }
    if (kind == 1 && id == 105) focused = false;
    if (kind == 2) { visible = false; focused = false; }
}

uint64_t mk8_settings_filter_buttons(uint64_t buttons, bool captured) {
    if (!enabled() || !atomic_load(&focused) || !(buttons & 1)) return buttons;
    // Keep the native tile out of its confirmed state; renderer-thread UI opens from the queued request.
    if (!captured && !atomic_exchange(&pending, true))
        LOG("[mk8:settings] focused Settings entry confirmed\n");
    return buttons & ~1ull;
}

bool mk8_settings_take_open(void) { return enabled() && atomic_exchange(&pending, false); }
bool mk8_settings_entry_visible(void) { return enabled() && atomic_load(&visible); }
bool mk8_settings_entry_focused(void) { return mk8_settings_entry_visible() && atomic_load(&focused); }
