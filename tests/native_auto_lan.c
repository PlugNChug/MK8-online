#include "rt.h"
#include "net/auto_lan.inc"
#include "net/auto_lan_menu.h"

unsigned char memory[65536] __attribute__((aligned(16)));
static Cpu cpu;
uint64_t g_test_buttons, g_test_buttons_until;
static unsigned beacons;
#define MANAGER HEAP_BASE
#define PAGE (HEAP_BASE+4096)
#define NAV (HEAP_BASE+6144)
#define PRESENT (HEAP_BASE+12544)
#define DESCRIPTOR (HEAP_BASE+16000)
#define SERVICE (HEAP_BASE+16384)
#define ROOM_STATE (HEAP_BASE+17408)
#define PROFILE (HEAP_BASE+26560)
#define RECORD (HEAP_BASE+27904)
#define INPUT (HEAP_BASE+28928)
#define SYSTEMS (HEAP_BASE+30976)
#define RACE (HEAP_BASE+32000)
#define PLAYERS (HEAP_BASE+33024)
#define APP (HEAP_BASE+34048)
#define SAVE (HEAP_BASE+36864)
#define ACCOUNT_DATA (HEAP_BASE+38912)
#define MODEL (HEAP_BASE+39424)
#define QUEUE (HEAP_BASE+39936)
#define BUFFER (HEAP_BASE+40960)
#define MII_SYSTEM (HEAP_BASE+43520)
#define MII_MANAGER (HEAP_BASE+44032)
#define CATALOG (HEAP_BASE+44544)
#define ENTRY (HEAP_BASE+45056)
#define PRESETS (HEAP_BASE+55296)
#define PADS (HEAP_BASE+56320)
#define PAD (HEAP_BASE+56832)
#define DEVICE (HEAP_BASE+58368)
#define CHOOSER (HEAP_BASE+6400)
#define SECOND (HEAP_BASE+7680)
#define LOGO (HEAP_BASE+8960)
static bool boot_case;
static unsigned prepared;
static bool matches, refuse;
static unsigned save_calls, restored;
static unsigned handoffs;

static unsigned claim_ready, claim_lost;
static int local_players=1, enroll_result;
static bool stall_catalog;
static bool claimed, backed, worker, released;
bool mk8_reserve_lan_registration(void) { return true; }
bool mk8_lan_registration_alive(bool ready) { (void)ready; return !released; }
bool mk8_finish_lan_registration(bool applied) { assert(applied==(enroll_result>0)); released=true; return worker; }
int mk8_lan_input_active(bool ready) {
    if (ready) ++claim_ready;
    else ++claim_lost;
    return ready ? claimed ? 2 : 1 : 0;
}
void mk8_lan_claim_notice(int state) { assert(state>=0 && state<=2); }
int mk8_lan_enroll_p2(bool active) { assert(active && claimed && local_players==2); return enroll_result; }

bool mk8_launcher_mii_matches(const uint8_t *info) {
    assert(info==(const uint8_t *)(uintptr_t)ENTRY);
    return matches;
}

uint64_t kernel_heap_size(void) { return sizeof memory; }
Cpu *kernel_current_cpu(void) { return &cpu; }
void guest_call(Cpu *c, uint64_t address) {
    if (boot_case) {
        switch (address) {
        case 0x70008bf0b0ull: assert(LD32(c->x[0])==3); c->x[0]=SERVICE; return;
        case 0x7000400908ull:
            assert(!LD8(SERVICE+0x69a)); LD8(SERVICE+0x69a)=1; return;
        case 0x70008bf0ecull:
            if (LD32(c->x[0])==0x3f) { assert(local_players==2); c->x[0]=CHOOSER; }
            else { assert(LD32(c->x[0])==0x61); c->x[0]=PAGE; }
            return;
        case 0x70008ace98ull: c->x[0]=PROFILE; return;
        case 0x70008ace28ull: c->x[0]=INPUT; return;
        case 0x70007e7434ull: c->x[0]=SYSTEMS; return;
        case 0x70008acdccull: c->x[0]=APP; return;
        case 0x70008aceb4ull: c->x[0]=SAVE; return;
        case 0x70008108dcull: c->x[0]=MII_SYSTEM; return;
        case 0x7000b53100ull:
            assert(c->x[0]==c->sp); LD64(c->sp)=c->x[1]; return;
        case 0x7000b53110ull:
            assert(c->x[0]==c->sp); c->x[0]=LD8(LD64(c->sp))==0x31; return;
        case 0x7000b54380ull:
            assert(c->x[0]==CATALOG+0x58 && c->x[2]==1);
            assert(!memcmp((void *)(uintptr_t)c->x[1],(void *)(uintptr_t)ENTRY,0x5c));
            c->x[0]=0x67e; return;
        case 0x70008a4dd0ull:
            assert(c->x[0]==SAVE && LD8(c->x[1])==0xff);
            assert(!memcmp((void *)(uintptr_t)(MODEL+0x28),(void *)(uintptr_t)ENTRY,0x5c));
            assert(!restored && !(LD8(RECORD+0x7c)&16)); ++save_calls;
            c->x[0]=!refuse; return;
        case 0x700080e1d8ull:
            assert(c->x[0]==CATALOG && save_calls==1 && !refuse);
            memcpy((void *)(uintptr_t)(RECORD+0x18),(void *)(uintptr_t)(MODEL+0x28),0x5c);
            LD8(RECORD+0x7c)|=16; ++restored; return;
        case 0x70008654b0ull:
            assert(c->x[0]==PAD && c->x[1]==0xff && c->x[2]==1 && restored==1); return;
        case 0x7000425904ull: assert(c->x[0]==0); break;
        case 0x700086aac8ull: assert(c->x[0]==INPUT && LD8(INPUT+0x1fd)==1); break;
        case 0x70008ac63cull: assert(c->x[0]==APP && c->x[1]==0); break;
        case 0x7000869180ull: assert(c->x[0]==INPUT && c->x[1]==local_players && c->x[2]==1 && !c->x[3]); break;
        case 0x700013d56cull: assert(c->x[0]==PLAYERS && c->x[1]==local_players && LD32(c->x[2])==0); break;
        case 0x70008544c8ull: assert(c->x[0]==local_players); break;
        case 0x70004dc27cull:
            assert(c->x[0]==CHOOSER && c->x[1]==CHOOSER+0x248); backed=true; return;
        case 0x700086a930ull: assert(c->x[0]==INPUT && !c->x[1]); break;
        case 0x70008bd5f8ull:
            assert(c->x[0]==MANAGER && !c->x[1] && c->x[2]==PAGE);
            LD64(MANAGER+0xc68)=PAGE; ++handoffs; return;
        default: assert(0);
        }
        ++prepared;
        return;
    }
    assert(0);
}
int vlan_send(uint16_t a, uint32_t b, uint16_t c, const void *d, size_t n) {
    assert(a == 47999 && c == a && b == 0xffffffffu && n == 8 && !memcmp(d,"MK8-ROOM",8));
    beacons++;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    bool first=!strncmp(argv[1],"first-",6) || !strcmp(argv[1],"boot-2p-first");
    bool boot=!strncmp(argv[1],"boot",4);
    if (!strncmp(argv[1],"boot-2p",7)) local_players=2;
    if (first || boot || !strcmp(argv[1],"wrong-room") || !strcmp(argv[1],"invalid-save") ||
        !strcmp(argv[1],"missing-controller") || !strcmp(argv[1],"foreign-controller")) {
        boot_case = true;
        cpu.sp = HEAP_BASE+sizeof memory;
        Cpu saved = cpu;
        LD64(DESCRIPTOR)=MAIN_BASE+0x1265b00;
        LD32(DESCRIPTOR+8)=3; LD32(DESCRIPTOR+0x2c)=3;
        LD64(SERVICE)=MAIN_BASE+0x1265cf0; LD64(SERVICE+0x30)=ROOM_STATE;
        bool supported = boot || !strcmp(argv[1],"first-ok");
        LD64(PAGE)=MAIN_BASE+(!strcmp(argv[1],"wrong-room") ? 0x127c840 : 0x1276e58);
        LD64(PROFILE+0x488)=RECORD;
        LD8(RECORD+0x7c)=first ? 0x80 : 0x90;
        if (!first && strcmp(argv[1],"invalid-save")) LD8(RECORD+0x18)=0x31;
        LD32(INPUT+0x50)=16; LD64(INPUT+0x58)=PADS; LD64(PADS)=PAD;
        LD64(PAD)=MAIN_BASE+0x12cf828; LD64(PAD+0x38)=INPUT;
        if (strcmp(argv[1],"missing-controller")) LD64(PAD+0x200)=DEVICE;
        if (strcmp(argv[1],"foreign-controller")) LD64(DEVICE)=MAIN_BASE+0x11b8c50;
        if (local_players==2) {
            LD64(CHOOSER)=MAIN_BASE+0x127d7a8; LD32(CHOOSER+0xe8)=0x3f;
            LD64(PADS+8)=SECOND; LD64(SECOND)=MAIN_BASE+0x12cf828; LD64(SECOND+0x38)=INPUT;
            LD32(SERVICE+0x49c)=0x72; LD64(SERVICE+0x4a0)=QUEUE; LD64(SERVICE+0x4a8)=ENTRY;
            if (!strcmp(argv[1],"boot-2p-stalled")) { stall_catalog=true; supported=false; }
            if (!strcmp(argv[1],"boot-2p-invalid")) { LD64(SECOND+0x38)=0; supported=false; }
        }
        LD64(SAVE+0x158)=ACCOUNT_DATA; LD64(ACCOUNT_DATA+0x188)=MODEL;
        LD8(SAVE+0x4ec)=1; LD64(SAVE+0x4c8)=QUEUE; LD64(QUEUE+16)=BUFFER;
        LD64(MII_SYSTEM+0x1f8)=MII_MANAGER; LD64(MII_MANAGER+0x58)=CATALOG;
        LD32(CATALOG)=100; LD32(CATALOG+0x10)=1; LD64(CATALOG+8)=ENTRY;
        LD32(CATALOG+0x30)=6; LD64(CATALOG+0x38)=PRESETS;
        memset((void *)(uintptr_t)ENTRY,0x31,0x58);
        if (!strcmp(argv[1],"first-busy")) LD8(SAVE+0x4c0)=1;
        if (!strcmp(argv[1],"first-preset")) LD32(ENTRY+0x58)=1;
        if (!strcmp(argv[1],"first-invalid")) LD8(ENTRY)=0;
        if (!strcmp(argv[1],"first-partial")) LD8(MODEL+0x30)=1;
        refuse=!strcmp(argv[1],"first-refused");
        matches=first && strcmp(argv[1],"first-unselected");
        uint8_t original_model[0x5c];
        memcpy(original_model,(void *)(uintptr_t)(MODEL+0x28),sizeof original_model);
        LD64(SYSTEMS+0x1b0)=RACE; LD64(RACE+0x238)=PLAYERS;
        LD32(DESCRIPTOR+0xc)=0x21;
        mk8_guest_lan_start(DESCRIPTOR,MANAGER);
        assert(LD32(DESCRIPTOR+0xc)==0x21 && !prepared);
        LD32(DESCRIPTOR+8)=0x21; LD32(DESCRIPTOR+0xc)=0x20;
        mk8_guest_lan_start(DESCRIPTOR,MANAGER);
        assert(LD32(DESCRIPTOR+0xc)==0x20 && !prepared && !save_calls);
        LD32(DESCRIPTOR+8)=3;
        LD32(DESCRIPTOR+0xc)=0x20;
        if (local_players==2) {
            LD64(LOGO)=MAIN_BASE+0x1270290; LD32(LOGO+0xe8)=3;
            LD32(LOGO+0x130)=2; assert(!mk8_guest_lan_hold_boot(LOGO));
            LD32(LOGO+0x124)=1;
            assert(mk8_guest_lan_hold_boot(LOGO) && LD8(SERVICE+0x69a) && !LD8(SERVICE+0x698));
            assert(mk8_guest_lan_hold_boot(LOGO) && !memcmp(&cpu,&saved,sizeof cpu));
            if (stall_catalog) {
                for (int i=2;i<600;++i) assert(mk8_guest_lan_hold_boot(LOGO));
            } else {
                LD64(QUEUE)=ENTRY; LD32(SERVICE+0x498)=1;
                LD8(SERVICE+0x698)=1; LD8(SERVICE+0x69a)=0;
            }
            assert(!mk8_guest_lan_hold_boot(LOGO));
        }
        mk8_guest_lan_start(DESCRIPTOR,MANAGER);
        assert(!memcmp(&cpu,&saved,sizeof cpu) && !g_test_buttons && !g_test_buttons_until);
        unsigned first_page=local_players==1 ? 0x61 : 0x3f;
        assert(LD32(DESCRIPTOR+0xc)==(supported ? first_page : 0x20));
        assert(prepared==(supported ? 7 : 0));
        assert(LD8(ROOM_STATE+0x1f99)==supported && LD32(ROOM_STATE+0x1fa4)==(supported ? local_players : 0));
        assert(LD32(ROOM_STATE+0x1fa8)==(supported ? 5+local_players : 0));
        assert(save_calls==(unsigned)(refuse || (first && supported)) && restored==(unsigned)(first && supported));
        if (!(first && supported)) assert(!memcmp(original_model,(void *)(uintptr_t)(MODEL+0x28),sizeof original_model));
        if (first && !supported) assert(LD8(RECORD+0x7c)==0x80 && !LD64(RECORD+0x18));
        if (local_players==2) {
            LD64(MANAGER+0xc68)=CHOOSER; LD32(MANAGER+0xb50)=1;
            LD64(MANAGER+0xb58)=NAV; LD64(NAV)=CHOOSER;
            LD32(CHOOSER+0x120)=3;
            mk8_auto_lan_menu_tick(MANAGER);
            assert(!claim_ready && !mk8_guest_lan_editor_players(MANAGER));
            if (!strcmp(argv[1],"boot-2p-left-early")) {
                LD64(MANAGER+0xc68)=PRESENT; LD32(PRESENT+0x120)=4; LD32(PRESENT+0xe8)=0x21;
                mk8_auto_lan_menu_tick(MANAGER); assert(released && !claim_ready);
                LD64(MANAGER+0xc68)=CHOOSER; LD32(CHOOSER+0x120)=4;
                mk8_auto_lan_menu_tick(MANAGER); assert(!claim_ready);
                return 0;
            }
            worker=!strcmp(argv[1],"boot-2p-worker-cancel");
            LD32(CHOOSER+0x120)=worker ? 5 : 4;
            if (worker) {
                LD64(MANAGER+0xc68)=PRESENT; LD64(CHOOSER+0xf8)=PRESENT;
                LD64(PRESENT)=MAIN_BASE+0x126c6e8;
            }
            mk8_auto_lan_menu_tick(MANAGER);
            assert(claim_ready==supported);
            if (!supported) return 0;
            claimed=true; mk8_auto_lan_menu_tick(MANAGER);
            assert(!mk8_guest_lan_editor_players(MANAGER));
            enroll_result=worker || !strcmp(argv[1],"boot-2p-cancel") ? -1 : 1;
            mk8_auto_lan_menu_tick(MANAGER);
            assert(released && backed==(enroll_result<0 && !worker));
            unsigned seats=enroll_result<0 ? 0 : 2;
            assert(mk8_guest_lan_editor_players(MANAGER)==seats);
            LD64(MANAGER+0xc68)=PRESENT; LD64(CHOOSER+0xf8)=PRESENT;
            LD64(PRESENT)=MAIN_BASE+0x126c6e8;
            assert(mk8_guest_lan_editor_players(MANAGER)==seats);
            LD32(CHOOSER+0x120)=2;
            LD64(MANAGER+0xc68)=PAGE; LD32(PAGE+0x120)=4; LD32(PAGE+0x124)=11;
            assert(mk8_guest_lan_editor_players(MANAGER)==seats);
            LD64(MANAGER+0xc68)=PRESENT;
            assert(!mk8_guest_lan_editor_players(MANAGER));
            LD64(MANAGER+0xc68)=PAGE; LD32(ROOM_STATE+0x1fa4)=1; LD32(ROOM_STATE+0x1fa8)=6;
            assert(!mk8_guest_lan_editor_players(MANAGER));
            LD64(MANAGER+0xc68)=PRESENT; LD32(PRESENT+0x120)=4; LD32(PRESENT+0xe8)=0x21;
            mk8_auto_lan_menu_tick(MANAGER);
            LD64(MANAGER+0xc68)=PAGE; LD32(ROOM_STATE+0x1fa4)=2; LD32(ROOM_STATE+0x1fa8)=7;
            assert(!mk8_guest_lan_editor_players(MANAGER));
            return 0;
        }
        LD32(PAGE+0x120)=3; LD32(PAGE+0x124)=11; LD32(PAGE+0x190)=1;
        LD64(PAGE+0xf8)=PRESENT; LD64(PRESENT)=MAIN_BASE+0x126c6e8;
        if (!strcmp(argv[1],"boot-invalid-nav")) LD64(PRESENT+0x1b0)=HEAP_BASE+sizeof memory-8;
        LD64(MANAGER+0xc68)=!strcmp(argv[1],"boot-foreign-owner") ? NAV : PRESENT;
        LD64(NAV)=MAIN_BASE+0x126c6e8;
        mk8_auto_lan_menu_tick(MANAGER);
        assert(!handoffs && !claim_ready);
        LD32(PAGE+0x120)=!strcmp(argv[1],"boot-left") ? 6 : 4;
        mk8_auto_lan_menu_tick(MANAGER);
        unsigned expected=(!strcmp(argv[1],"boot") || !strcmp(argv[1],"first-ok"));
        assert(claim_ready==expected && !claim_lost);
        assert(handoffs==expected && !memcmp(&cpu,&saved,sizeof cpu));
        if (expected) assert(LD64(MANAGER+0xc68)==PAGE);
        LD32(PAGE+0x120)=4; LD64(MANAGER+0xc68)=PRESENT;
        mk8_auto_lan_menu_tick(MANAGER);
        assert(claim_ready==expected && claim_lost==expected);
        assert(handoffs==expected && !g_test_buttons && !g_test_buttons_until);
        LD32(DESCRIPTOR+0xc)=0x20;
        mk8_guest_lan_start(DESCRIPTOR,MANAGER);
        assert(LD32(DESCRIPTOR+0xc)==0x20);
        return 0;
    }
    assert(!strcmp(argv[1],"direct"));
    assert(mk8_guest_lan_requested() && !mk8_auto_lan_requested());
    for (uint64_t frame=0; frame<2500; ++frame) mk8_auto_lan_frame(frame);
    assert(!g_test_buttons && !g_test_buttons_until && !beacons);
    return 0;
}
