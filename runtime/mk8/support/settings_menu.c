#include "rt.h"
#include "hle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic bool visible, focused, pending, selected;
static _Atomic uint64_t previous, suppressed;

RT_ON_RESET(settings_menu_reset) { visible = false; focused = false; pending = false; selected = false; previous = 0; suppressed = 0; }

static bool enabled(void) {
    const char *value = getenv("MK8_SETTINGS_EXPERIMENT");
    return value && !strcmp(value, "1");
}

// The verified top-menu callback reports focus (0/1) and confirmation (2).
// The runtime-drawn slot follows the final native tile; the original tile retains its action.
void mk8_settings_top_event(uint64_t page, uint64_t event) {
    if (!enabled()) return;
    if (page < HEAP_BASE || page >= HEAP_BASE + HEAP_MAX || 0x700 > HEAP_BASE + HEAP_MAX - page ||
        event < GUEST_LO || event >= GUEST_HI || 0x38 > GUEST_HI - event)
        FATAL("[mk8:settings] unexpected top-menu object/event\n");
    uint32_t id = LD32(event + 8), kind = LD32(event + 0x28);
    if (kind == 0 && (id <= 40 || (id >= 100 && id <= 106))) {
        visible = true;
        focused = id == 105;
        if (id != 105) selected = false;
    }
    if (kind == 1 && id == 105) { focused = false; selected = false; }
    if (kind == 2) { visible = false; focused = false; selected = false; }
}

uint64_t mk8_settings_filter_buttons(uint64_t buttons, bool captured) {
    if (!enabled()) return buttons;
    uint64_t edge = buttons & ~atomic_exchange(&previous, buttons);
    uint64_t held = atomic_load(&suppressed) & buttons;
    suppressed = held;
    uint64_t out = buttons & ~held;
    if (captured) return selected ? out & ~1ull : out;
    if (!atomic_load(&focused)) return out;
    const uint64_t right = (1ull << 14) | (1ull << 18), left = (1ull << 12) | (1ull << 16);
    if (!selected && (edge & right)) {
        selected = true; suppressed = held | (buttons & right);
        return out & ~right;
    }
    if (!selected) return out;
    if (edge & (left | 2)) {
        selected = false; suppressed = held | (buttons & (left | 2));
        return out & ~(left | 2);
    }
    if (edge & ((1ull << 13) | (1ull << 17))) { selected = false; return out; }
    if (buttons & 1) {
        if (!atomic_exchange(&pending, true)) LOG("[mk8:settings] dedicated Settings entry confirmed\n");
        out &= ~1ull;
    }
    return out & ~right;
}

bool mk8_settings_take_open(void) { return enabled() && atomic_exchange(&pending, false); }
bool mk8_settings_entry_visible(void) { return enabled() && atomic_load(&visible); }
bool mk8_settings_entry_focused(void) { return mk8_settings_entry_visible() && atomic_load(&selected); }

bool mk8_settings_style_active(void) { return enabled(); }
