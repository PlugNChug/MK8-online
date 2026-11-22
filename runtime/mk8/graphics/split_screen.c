#include "kernel.h"
#include "settings.h"

static bool heap_object(uint64_t address, uint64_t size, uint64_t end) {
    return !(address & 7) && address >= HEAP_BASE && address < end && size <= end - address;
}

bool mk8_split_race_full_speed(void) {
    if (!mk8_split_screen_60fps_enabled()) return false;
    uint64_t root = LD64(MAIN_BASE + 0x1358c70);
    if (root != MAIN_BASE + 0x133ebd0) return false;
    uint64_t end = HEAP_BASE + kernel_heap_size();
    uint64_t node = LD64(root + 0x20);
    if (!heap_object(node, 0x10, end)) return false;
    uint64_t scene = LD64(node + 8);
    if (!heap_object(scene, 0x188, end) || LD32(scene + 0x184) != 4) return false;
    uint64_t context = LD64(root + 0x28);
    if (!heap_object(context, 0x248, end)) return false;
    uint64_t config = LD64(context + 0x240);
    if (!heap_object(config, 0x18, end)) return false;
    uint64_t rules = LD64(config + 0x10);
    if (!heap_object(rules, 0x1a0, end)) return false;
    // Same two count fields as MK8's race-rules accessor at main+0x86f154.
    uint32_t players = LD32(rules + (LD32(rules + 8) == 4 ? 0x19c : 0x188));
    return players == 3 || players == 4;
}
