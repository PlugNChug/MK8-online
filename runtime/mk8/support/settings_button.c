#include "rt.h"
#include <string.h>

static void heap(uint64_t p, uint64_t size) {
    if (p < HEAP_BASE || p >= HEAP_BASE + HEAP_MAX || size > HEAP_BASE + HEAP_MAX - p)
        FATAL("[mk8:settings] invalid native widget\n");
}

static uint64_t invoke(Cpu *c, uint64_t address, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f) {
    c->x[0]=a; c->x[1]=b; c->x[2]=d; c->x[3]=e; c->x[4]=f;
    guest_call(c, address);
    return c->x[0];
}

void mk8_settings_button_hover(uint64_t page, unsigned instant) {
    heap(page,0x700);
    Cpu *c=kernel_current_cpu();
    if(!c || c->sp<GUEST_LO+32 || c->sp>=GUEST_HI || instant>1)
        FATAL("[mk8:settings] invalid background context\n");
    Cpu saved=*c; c->sp-=32;
    uint64_t title=invoke(c,0x7000421dfcull,0,0,0,0,0); heap(title,0x300);
    // Native Other background (B20), without the stock Record foreground illustration.
    ST32(c->sp,6);
    invoke(c,0x70004c6360ull,title,c->sp,instant,0,0);
    uint64_t illustration=LD64(title+0x2a0); heap(illustration,0x60);
    invoke(c,0x70008ade74ull,illustration,0,0,0,0);
    invoke(c,0x70004da65cull,page,0xffffffffu,instant,0,0);
    *c=saved;
}

// Construct and attach through the game's own routines; the page owns the widget's lifetime.
void mk8_settings_button_build(uint64_t page) {
    heap(page, 0x700);
    Cpu *c = kernel_current_cpu();
    if (!c || c->sp < GUEST_LO+256 || c->sp >= GUEST_HI)
        FATAL("[mk8:settings] invalid widget construction context\n");
    Cpu saved = *c;
    c->sp -= 256;
    uint64_t scratch = c->sp, parent = LD64(page+0x448), example = LD64(page+0x4d0);
    heap(parent, 0x38); heap(example, 0x3d8);
    memset((void *)(uintptr_t)scratch, 0, 256);
    uint64_t string_vtable = LD64(0x70012f9e28ull)+0x10;
    ST64(scratch, string_vtable); ST64(scratch+8, scratch+32);
    memcpy((void *)(uintptr_t)(scratch+32), "L_SubBtn_07", 12);
    uint64_t widget = invoke(c, 0x70005902d0ull, 0x3d8, 4, 0, 0, 0);
    heap(widget, 0x3d8);
    invoke(c, 0x7000434d90ull, widget, 0, 0, 0, 0);
    ST64(widget, LD64(example)); ST64(widget+0x30, LD64(example+0x30));
    ST64(widget+0xb8, LD64(example+0xb8));
    ST8(widget+0x3cb, 0); ST64(widget+0x3d0, 0);
    uint64_t navigation = LD64(page+0x1b0);
    heap(navigation, 0xb8);
    uint32_t count = LD32(navigation+0x10), capacity = LD32(navigation+0x14);
    if (count != 11 || count > capacity || capacity > 124) FATAL("[mk8:settings] unexpected navigation capacity\n");
    uint64_t old = LD64(navigation+0x18);
    heap(old, capacity*8);
    if (count == capacity) {
        // Use the same guest buffer allocator/deallocator as the native navigation owner.
        invoke(c, 0x70005fb2a4ull, scratch+96, capacity+1, 0, 8, 0);
        uint64_t grown = LD64(scratch+104);
        heap(grown, (capacity+1)*8);
        memcpy((void *)(uintptr_t)grown, (void *)(uintptr_t)old, count*8);
        ST64(navigation+0x18, grown); ST32(navigation+0x14, capacity+1);
        invoke(c, 0x70005fae48ull, old, 0, 0, 0, 0);
    }
    invoke(c, 0x70008bc0c4ull, page, widget+0x30, parent, scratch, scratch+16);
    if (LD32(navigation+0x10) != count+1 || LD64(LD64(navigation+0x18)+count*8) != widget+0x30)
        FATAL("[mk8:settings] native button was not registered for input\n");
    if (!LD64(widget+0x38)) FATAL("[mk8:settings] Settings layout pane missing\n");
    ST32(widget+0x48, 107);
    memcpy((void *)(uintptr_t)(scratch+32), "T_Btn_00", 9);
    uint64_t text = invoke(c, 0x70008ad8e8ull, widget+0x30, scratch, 0, 0, 0);
    if (!text) FATAL("[mk8:settings] Settings label missing\n");
    static const uint16_t label[] = {'S','e','t','t','i','n','g','s',0};
    memcpy((void *)(uintptr_t)(scratch+64), label, sizeof label);
    invoke(c, 0x70008adc54ull, text, scratch+64, 0, 0, 0);
    LOG("[mk8:settings] native widget created: id=107 page=%llx widget=%llx\n", page, widget);
    *c = saved;
}

// Return on the guest UI thread using the native button/navigation reset routines.
void mk8_settings_button_restore(uint64_t page) {
    heap(page, 0x700);
    Cpu *c = kernel_current_cpu();
    if (!c) FATAL("[mk8:settings] invalid return context\n");
    Cpu saved = *c;
    uint64_t navigation = LD64(page+0x1b0);
    heap(navigation, 0xb8);
    if (LD32(navigation+0x10) != 12) FATAL("[mk8:settings] navigation changed before return\n");
    uint64_t items = LD64(navigation+0x18);
    heap(items, 12*8);
    uint64_t component = LD64(items+11*8);
    if (component < HEAP_BASE+0x30) FATAL("[mk8:settings] invalid return widget\n");
    uint64_t widget = component-0x30;
    heap(widget, 0x3d8);
    if (LD32(widget+0x48) != 107) FATAL("[mk8:settings] Settings return widget changed\n");
    if (LD32(page+0x120) != 4) FATAL("[mk8:settings] top menu left before pop-up return\n");
    uint8_t sound = LD8(widget+0x3ca);
    if (sound > 1) FATAL("[mk8:settings] native button sound flag changed\n");
    // Back/No já emitiu feedback; o reset nativo não deve repetir o som de cancelamento.
    ST8(widget+0x3ca, 0);
    invoke(c, 0x7000435d2cull, widget, 0xffffffffu, 0, 0, 0);
    ST8(widget+0x3ca, sound);
    // Match the top menu's return-from-dialog path: re-enable input and resume its idle state.
    ST32(page+0x1a8, LD32(page+0x1a8) | 0x80000000u);
    invoke(c, 0x70008bbee0ull, page, 0, 0, 0, 0);
    invoke(c, 0x70008afc54ull, navigation, 0, 0, 0, 0);
    invoke(c, 0x70008afdd0ull, navigation, 0, 0, 0, 0);
    if (LD32(navigation+0x30) != 11 || LD32(navigation+0x28) < 2 || LD32(navigation+0x28) >= 5 || LD16(widget+0x160))
        FATAL("[mk8:settings] could not restore native focus: state=%u index=%u latch=%u input=%x\n",
              LD32(navigation+0x28), LD32(navigation+0x30), LD16(widget+0x160), LD32(page+0x1a8));
    LOG("[mk8:settings] native focus restored after Settings\n");
    *c = saved;
}
