#include "rt.h"
#include "hle.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic bool pending;
static _Atomic uint64_t policy;
void mk8_settings_top_forget(void) { pending = false; policy = 0; }
RT_ON_RESET(settings_menu_reset) { mk8_settings_top_forget(); }

bool mk8_settings_style_active(void) {
    const char *value = getenv("MK8_SETTINGS_EXPERIMENT");
    return value && !strcmp(value, "1");
}

const char *mk8_settings_romfs_overlay(void) {
    static char path[4096];
    if (!mk8_settings_style_active()) return NULL;
    if (!plat_exe_path(path, sizeof path)) FATAL("[mk8:settings] executable path unavailable\n");
    char *end = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (!end || (back && back > end)) end = back;
    if (!end || sizeof path - (size_t)(end-path) < 32) FATAL("[mk8:settings] layout path too long\n");
    strcpy(end, "/settings-menu/romfs");
    const char *combined = getenv("MK8_SETTINGS_ROMFS");
    if (combined && *combined) {
        if (strlen(combined) >= sizeof path || strchr(combined, '\n'))
            FATAL("[mk8:settings] combined layout path invalid\n");
        strcpy(path, combined);
    }
    char check[4200];
    snprintf(check, sizeof check, "%s/UI/cmn/menu.sarc", path);
    FILE *file = fopen(check, "rb");
    if (!file) FATAL("[mk8:settings] build the private Settings layout before enabling the experiment\n");
    fclose(file);
    return path;
}

void mk8_settings_button_build(uint64_t page);
void mk8_settings_button_hover(uint64_t page, unsigned instant);
void mk8_settings_page_build(uint64_t page);
void mk8_settings_page_tick(uint64_t page, bool open);

// Called only by hash-verified MK8 4.0.1 construction and top-menu event boundaries.
bool mk8_settings_top_event(uint64_t page, uint64_t event) {
    if (!mk8_settings_style_active()) return false;
    if (page < HEAP_BASE || page >= HEAP_BASE + HEAP_MAX || 0x700 > HEAP_BASE + HEAP_MAX - page)
        FATAL("[mk8:settings] unexpected top-menu object\n");
    if (!event) { mk8_settings_button_build(page); mk8_settings_page_build(page); policy = page+0x420; return false; }
    if (event == 1) {
        if (policy == page+0x420) mk8_settings_page_tick(page, atomic_exchange(&pending,false));
        return false;
    }
    if (event < GUEST_LO || event >= GUEST_HI || 0x38 > GUEST_HI - event)
        FATAL("[mk8:settings] unexpected top-menu event\n");
    if (LD32(event + 8) != 107) return false;
    uint32_t kind = LD32(event + 0x28);
    if (kind > 2) FATAL("[mk8:settings] unexpected Settings event\n");
    if (kind == 0) mk8_settings_button_hover(page, LD8(event+0x34));
    if (kind == 2 && !atomic_exchange(&pending, true))
        LOG("[mk8:settings] native button confirmed\n");
    return true;
}

// Extend the native row's fixed wrap points; all other moves use the original policy.
uint64_t mk8_settings_navigation(uint64_t owner, uint64_t index, uint64_t direction) {
    if (!mk8_settings_style_active() || !policy || owner != policy) return 0;
    uint32_t next;
    if (index == 11) {
        if (direction & 0x20) next=3;
        else if (direction & 0x40) next=0;
        else if (direction & 0x80) next=9;
        else if (direction & 0x100) next=10;
        else next=11;
    } else if (index == 9 && !(direction & 0xe0) && (direction & 0x100)) next=11;
    else if (index == 10 && !(direction & 0x60) && (direction & 0x80)) next=11;
    else return 0;
    return (1ull<<32) | next;
}
