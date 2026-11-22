#include "rt.h"
#include "kernel.h"
#include "auto_lan_menu.h"
#include <stdlib.h>
#include <string.h>

extern bool mk8_launcher_mii_matches(const uint8_t *record);
extern int mk8_lan_input_active(bool room_ready);
extern void mk8_lan_claim_notice(int state);
extern int mk8_lan_enroll_p2(bool active);
extern bool mk8_reserve_lan_registration(void);
extern bool mk8_lan_registration_alive(bool ready);
extern bool mk8_finish_lan_registration(bool applied);
static uint64_t opening_guest_room;
static uint64_t two_player_page, two_player_room, two_player_state;
static bool choosing_p1, picker_seen, enrolling_p2, two_registered;

// The MK8 4.0.1 UI manager tick is already hash-checked by toolchain/settings_menu.py.
static bool heap_span(uint64_t address, size_t size) {
    uint64_t end = HEAP_BASE + kernel_heap_size();
    return !(address & 7) && address >= HEAP_BASE && address < end && size <= end - address;
}

static uint64_t menu_call(uint64_t address, uint64_t object, uint64_t argument, uint64_t other) {
    Cpu *cpu = kernel_current_cpu();
    Cpu saved = *cpu;
    cpu->x[0] = object;
    cpu->x[1] = argument;
    cpu->x[2] = other;
    guest_call(cpu, address);
    uint64_t result = cpu->x[0];
    *cpu = saved;
    return result;
}

static uint64_t boot_call(Cpu *cpu, uint64_t fn, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    cpu->x[0] = a; cpu->x[1] = b; cpu->x[2] = c; cpu->x[3] = d;
    guest_call(cpu, fn);
    return cpu->x[0];
}

static uint64_t heap_pointer(uint64_t object, unsigned offset) {
    return heap_span(object, offset + sizeof(uint64_t)) ? LD64(object + offset) : 0;
}

static bool valid_page_nav(uint64_t page) {
    uint64_t nav = LD64(page + 0x1b0);
    return !nav || (heap_span(nav, 0xb8) && LD64(nav + 8) == page);
}

static bool valid_mii(Cpu *cpu, uint64_t info) {
    boot_call(cpu, MAIN_BASE + 0xb53100, cpu->sp, info, 0, 0);  // nn::mii::CharInfoAccessor
    return (boot_call(cpu, MAIN_BASE + 0xb53110, cpu->sp, 0, 0, 0) & 1) != 0;
}

static bool guest_mii_ready(Cpu *cpu, uint64_t scene, uint64_t record, uint64_t input) {
    static const uint8_t empty[0x5c];
    if (LD8(record + 0x7c) & 0x10) return valid_mii(cpu, record + 0x18);
    if (!(LD8(record + 0x7c) & 0x80) || memcmp((void *)(uintptr_t)(record + 0x18), empty, sizeof empty) ||
        !heap_span(scene, 0x69b) || LD8(scene + 0x698) || LD8(scene + 0x69a)) return false;

    uint64_t save = boot_call(cpu, MAIN_BASE + 0x8aceb4, 0, 0, 0, 0);
    uint64_t model = heap_pointer(heap_pointer(save, 0x158), 0x188);
    uint64_t queue = heap_pointer(save, 0x4c8);
    if (!heap_span(save, 0x4ed) || LD8(save + 0x4c0) || !LD8(save + 0x4ec) ||
        !heap_span(model, 0x84) || memcmp((void *)(uintptr_t)(model + 0x28), empty, sizeof empty) ||
        !heap_span(queue, 0x20) || LD32(queue + 0x18) > 1 ||
        !heap_span(LD64(queue + 8 + (1 - LD32(queue + 0x18)) * 8), 0x800)) return false;
    uint64_t system = boot_call(cpu, MAIN_BASE + 0x8108dc, 0, 0, 0, 0);
    uint64_t catalog = heap_pointer(heap_pointer(system, 0x1f8), 0x58);
    uint64_t entry = heap_pointer(catalog, 8);
    uint64_t pad = heap_pointer(heap_pointer(input, 0x58), 0);
    if (!LD32(input + 0x50) || !heap_span(pad, 0x2a0) || !heap_span(catalog, 0x98) ||
        LD32(catalog) != 100 || LD32(catalog + 0x10) != 1 || !heap_span(entry, 100 * 0x5c) ||
        LD32(catalog + 0x30) != 6 || !heap_span(LD64(catalog + 0x38), 6 * 0x5c) ||
        LD32(entry + 0x58) || !mk8_launcher_mii_matches((const uint8_t *)(uintptr_t)entry) ||
        !valid_mii(cpu, entry)) return false;

    uint8_t *copy = (uint8_t *)(uintptr_t)(cpu->sp + 0x60);
    memcpy(copy, (const void *)(uintptr_t)entry, sizeof empty);
    // UpdateLatest must report NotUpdated for the exact selected database Mii.
    if ((uint32_t)boot_call(cpu, MAIN_BASE + 0xb54380, catalog + 0x58, (uintptr_t)copy, 1, 0) != 0x67e ||
        memcmp(copy, (const void *)(uintptr_t)entry, sizeof empty)) return false;
    uint8_t *appearance = (uint8_t *)(uintptr_t)(model + 0x28);
    memcpy(appearance, copy, sizeof empty);
    *copy = 0xff;  // The stock picker uses the active account for this save request.
    if (!(boot_call(cpu, MAIN_BASE + 0x8a4dd0, save, (uintptr_t)copy, 0, 0) & 1)) {
        memset(appearance, 0, sizeof empty);
        return false;
    }
    // Native reload validates the saved appearance, fills the selected record and sets its presence bit.
    boot_call(cpu, MAIN_BASE + 0x80e1d8, catalog, 0, 0, 0);
    if (!(LD8(record + 0x7c) & 0x10) || memcmp(appearance, (const void *)(uintptr_t)(record + 0x18), sizeof empty))
        return false;
    boot_call(cpu, MAIN_BASE + 0x8654b0, pad, 0xff, 1, 0);
    return true;
}

static bool second_mii_list_ready(uint64_t scene) {
    if (!heap_span(scene,0x69b) || LD32(scene+0x49c)!=0x72 ||
        !heap_span(LD64(scene+0x4a0),0x72*8)) return false;
    unsigned n=LD32(scene+0x498);
    if (!LD8(scene+0x698) || !n || n>0x72) return false;
    uint64_t entries=LD64(scene+0x4a0);
    for (unsigned i=0;i<n;++i)
        if (!heap_span(LD64(entries+i*8),16)) return false;
    return true;
}

int mk8_guest_lan_hold_boot(uint64_t page) {
    static unsigned waiting_frames;
    const char *count=getenv("MK8_LAN_PLAYERS");
    if (!mk8_guest_lan_requested() || !count || strcmp(count,"2") || waiting_frames>=600 ||
        !heap_span(page,0x388) || LD64(page)!=MAIN_BASE+0x1270290 || LD32(page+0xe8)!=3 ||
        LD32(page+0x124)!=1 || LD32(page+0x130)<2 || LD8(page+0x380)) return 0;
    Cpu *cpu=kernel_current_cpu();
    if (!cpu || (cpu->sp&15) || cpu->sp<0xc0) return 0;
    uint64_t scratch=cpu->sp-0xc0;
    if (!heap_span(scratch,0xc0) && !(scratch>=STACK_BASE && cpu->sp<=STACK_BASE+STACK_SIZE)) return 0;
    Cpu saved=*cpu;
    cpu->sp=scratch;
    ST32(scratch,3);
    uint64_t scene=boot_call(cpu,MAIN_BASE+0x8bf0b0,scratch,0,0,0);
    bool hold=false;
    if (heap_span(scene,0x69b) && LD64(scene)==MAIN_BASE+0x1265cf0 && LD32(scene+0x49c)==0x72 &&
        heap_span(LD64(scene+0x4a0),0x72*8) && !LD8(scene+0x698)) {
        // As on the title, let the real Mii catalog worker and portrait renderer run between frames.
        if (!LD8(scene+0x69a) && heap_span(LD64(scene+0x4a8),16)) {
            uint64_t profile=boot_call(cpu,MAIN_BASE+0x8ace98,0,0,0,0);
            uint64_t record=heap_pointer(profile,0x488);
            uint64_t input=boot_call(cpu,MAIN_BASE+0x8ace28,0,0,0,0);
            if (heap_span(record,0x80) && heap_span(input,0x200) && guest_mii_ready(cpu,scene,record,input))
                boot_call(cpu,MAIN_BASE+0x400908,0,0,0,0);
        }
        hold=LD8(scene+0x69a)!=0;
    }
    *cpu=saved;
    if (hold) ++waiting_frames;
    return hold;
}

void mk8_guest_lan_start(uint64_t descriptor, uint64_t manager) {
    static bool consumed;
    if (consumed || !mk8_guest_lan_requested() || !heap_span(descriptor, 0x30) ||
        LD64(descriptor) != MAIN_BASE+0x1265b00 || LD32(descriptor+8) != 3 || LD32(descriptor+0xc) != 0x20 ||
        LD32(descriptor+0x2c) != 3 || !heap_span(manager, 0xc70)) return;
    consumed = true;
    Cpu *cpu = kernel_current_cpu();
    if (!cpu || (cpu->sp & 15) || cpu->sp < 0xc0) return;
    uint64_t scratch = cpu->sp - 0xc0;
    if (!heap_span(scratch, 0xc0) && !(scratch >= STACK_BASE && cpu->sp <= STACK_BASE + STACK_SIZE)) return;
    Cpu saved = *cpu;
    cpu->sp = scratch;
    ST32(cpu->sp, 3);
    uint64_t service = boot_call(cpu,0x70008bf0b0ull,cpu->sp,0,0,0);
    uint64_t state = heap_span(service, 0x38) && LD64(service)==MAIN_BASE+0x1265cf0 ? LD64(service+0x30) : 0;
    const char *option = getenv("MK8_LAN_PLAYERS");
    unsigned count = option && !strcmp(option, "2") ? 2 : 1;
    ST32(cpu->sp, 0x61);
    uint64_t room = boot_call(cpu,0x70008bf0ecull,cpu->sp,0,0,0);
    uint64_t chooser = 0;
    if (count == 2) {
        // Stock Top -> LAN (2 Players) opens the second Mii picker, whose OK opens the room list.
        ST32(cpu->sp, 0x3f);
        chooser = boot_call(cpu,0x70008bf0ecull,cpu->sp,0,0,0);
    }
    uint64_t profile = boot_call(cpu,0x70008ace98ull,0,0,0,0);
    uint64_t record = heap_span(profile,0x490) ? LD64(profile+0x488) : 0;
    uint64_t input = boot_call(cpu,0x70008ace28ull,0,0,0,0);
    uint64_t pad = heap_pointer(heap_pointer(input, 0x58), 0);
    uint64_t device = heap_pointer(pad, 0x200);
    uint64_t second = count == 2 ? heap_pointer(heap_pointer(input, 0x58), 8) : 0;
    uint64_t systems = boot_call(cpu,0x70007e7434ull,0,0,0,0);
    uint64_t race = heap_span(systems,0x1b8) ? LD64(systems+0x1b0) : 0;
    uint64_t players = heap_span(race,0x240) ? LD64(race+0x238) : 0;
    uint64_t app = boot_call(cpu,0x70008acdccull,0,0,0,0);
    bool ready = heap_span(room,8) && LD64(room)==MAIN_BASE+0x1276e58 &&
        heap_span(state,0x1fac) && heap_span(record,0x80) && heap_span(input,0x200) &&
        LD32(input + 0x50) == 16 && heap_span(pad, 0x2b0) && LD64(pad) == MAIN_BASE + 0x12cf828 &&
        LD64(pad + 0x38) == input && LD32(pad + 0x48) == 0 &&
        heap_span(device, 0x7b8) && LD64(device) == MAIN_BASE + 0x11b8c50 &&
        (count == 1 || (heap_span(chooser,0x3a8) && LD64(chooser)==MAIN_BASE+0x127d7a8 &&
            LD32(chooser+0xe8)==0x3f && heap_span(second,0x2b0) && second!=pad &&
            LD64(second)==MAIN_BASE+0x12cf828 && LD64(second+0x38)==input)) &&
        heap_span(players,0xf4) && heap_span(app,0x7f0) && guest_mii_ready(cpu, service, record, input) &&
        (count==1 || second_mii_list_ready(service));
    if (ready) {
        boot_call(cpu,0x7000425904ull,0,0,0,0);
        ST8(input+0x1fd,1);
        boot_call(cpu,0x700086aac8ull,input,0,0,0);
        boot_call(cpu,0x70008ac63cull,app,0,0,0);
        ST8(state+0x1f99,1);
        ST32(state+0x1fa8,count==2 ? 7 : 6);
        ST32(state+0x1fa4,count);
        boot_call(cpu,0x7000869180ull,input,count,1,0);
        ST32(cpu->sp,0);
        boot_call(cpu,0x700013d56cull,players,count,cpu->sp,0);
        boot_call(cpu,0x70008544c8ull,count,0,0,0);
        // The title normally starts the native controller assignment before taking input.
        boot_call(cpu, MAIN_BASE + 0x86a930, input, 0, 0, 0);
        ST32(descriptor+0xc,count==2 ? 0x3f : 0x61);
        if (count==2) {
            two_player_page=chooser;
            two_player_room=room;
            two_player_state=state;
            choosing_p1=mk8_reserve_lan_registration();
        } else opening_guest_room = room;
    }
    LOG("[mk8:lan-start] %s\n", ready ? "opening native guest LAN room browser" : "continuing at the normal title screen");
    *cpu = saved;
}

static bool picker_overlay(uint64_t owner) {
    return owner==LD64(two_player_page+0xf8) && heap_span(owner,0x1d0) &&
           LD64(owner)==MAIN_BASE+0x126c6e8;
}

static bool picker_active(uint64_t manager) {
    if (!two_player_page || !heap_span(two_player_page,0x3a8) ||
        LD64(two_player_page)!=MAIN_BASE+0x127d7a8) return false;
    unsigned phase=LD32(two_player_page+0x120);
    uint64_t owner=heap_pointer(manager,0xc68);
    // The stock two-pad applet suspends the picker and gives its overlay the cursor.
    if (phase==5 && picker_overlay(owner)) return true;
    if (phase!=4) return false;
    // The picker gives P2 its cursor. Shared input can belong to a child of the page.
    uint64_t pages=heap_pointer(manager,0xb58);
    unsigned n=heap_span(manager,0xb60) ? LD32(manager+0xb50) : 0;
    if (!n || n>32 || !heap_span(pages,8ull*n)) return false;
    for (unsigned i=0;i<n;i++) if (LD64(pages+8ull*i)==two_player_page) return true;
    return false;
}

static bool two_player_mode(void) {
    return heap_span(two_player_state,0x1fac) && LD8(two_player_state+0x1f99)==1 &&
           LD32(two_player_state+0x1fa4)==2 && LD32(two_player_state+0x1fa8)==7;
}

unsigned mk8_guest_lan_editor_players(uint64_t manager) {
    if (!two_registered || !two_player_mode()) return 0;
    uint64_t owner=heap_pointer(manager,0xc68);
    if (picker_active(manager) && LD32(two_player_page+0x120)==4 &&
        (owner==two_player_page || picker_overlay(owner))) return 2;
    return owner==two_player_room && heap_span(owner,0x1b8) && LD64(owner)==MAIN_BASE+0x1276e58 &&
           LD32(owner+0x120)==4 && LD32(owner+0x124)==11 ? 2 : 0;
}

void mk8_auto_lan_menu_tick(uint64_t manager) {
    static uint64_t claim_room;
    uint64_t owner=heap_pointer(manager,0xc68);
    if ((two_registered || choosing_p1 || enrolling_p2) && (!two_player_mode() ||
        (heap_span(owner,0x128) && LD32(owner+0x120)==4 && (LD32(owner+0xe8)==0x20 || LD32(owner+0xe8)==0x21)))) {
        if (choosing_p1 || enrolling_p2) mk8_finish_lan_registration(false);
        two_registered=false;
    }
    if (choosing_p1 || enrolling_p2) {
        bool ready=picker_active(manager);
        if (!mk8_lan_registration_alive(ready)) {
            if (choosing_p1) mk8_lan_claim_notice(mk8_lan_input_active(false));
            if (enrolling_p2) mk8_lan_enroll_p2(false);
            choosing_p1=enrolling_p2=false;
            return;
        }
        if (choosing_p1 && (ready || picker_seen)) {
            picker_seen=true;
            int state=mk8_lan_input_active(ready);
            mk8_lan_claim_notice(state);
            if (state!=1) {
                choosing_p1=false;
                enrolling_p2=state==2;
                if (!enrolling_p2) mk8_finish_lan_registration(false);
            }
        }
        if (enrolling_p2) {
            int result=mk8_lan_enroll_p2(ready);
            if (result) {
                enrolling_p2=false;
                two_registered=result>0;
                bool worker=mk8_finish_lan_registration(two_registered);
                // The picker's own Back handler closes the 2P flow on cancellation.
                if (!worker && ready && result<0)
                    menu_call(MAIN_BASE+0x4dc27c,two_player_page,two_player_page+0x248,0);
            }
        }
    }
    if (opening_guest_room) {
        uint64_t room = opening_guest_room;
        if (heap_span(room, 0x1b8) && LD64(room) == MAIN_BASE + 0x1276e58) {
            uint32_t phase = LD32(room + 0x120);
            if (phase < 4) return;
            uint64_t owner = heap_pointer(manager, 0xc68);
            // When the boot scene closes, MK8 can give P1 to the browser's button overlay.
            // Ordinary Top -> LAN leaves the room itself in charge of Back and joining.
            if (phase == 4 && LD32(room + 0x124) == 11 && (LD32(room + 0x190) & 1) &&
                owner == LD64(room + 0xf8) && heap_span(owner, 0x1b8) && LD64(owner) == MAIN_BASE + 0x126c6e8 &&
                valid_page_nav(owner) && valid_page_nav(room))
                menu_call(MAIN_BASE + 0x8bd5f8, manager, 0, room);
            if (phase == 4 && LD32(room + 0x124) == 11 && (LD32(room + 0x190) & 1) &&
                heap_pointer(manager, 0xc68) == room && valid_page_nav(room))
                claim_room = room;
        }
        opening_guest_room = 0;
    }
    if (claim_room) {
        bool ready = heap_pointer(manager, 0xc68) == claim_room && heap_span(claim_room, 0x1b8) &&
            LD64(claim_room) == MAIN_BASE + 0x1276e58 && LD32(claim_room + 0x120) == 4 &&
            LD32(claim_room + 0x124) == 11;
        int state = mk8_lan_input_active(ready);
        mk8_lan_claim_notice(state);
        if (state != 1) claim_room = 0;
    }
}
