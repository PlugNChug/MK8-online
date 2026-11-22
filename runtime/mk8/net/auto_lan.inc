// Automatic host room creation for Steam rooms. Guests open the native room browser directly.
#include <stdatomic.h>
#include <stdbool.h>

static _Atomic int mk8_auto_menu, mk8_auto_vlan;

void mk8_auto_lan_movie(const char *path) {
    if (path && strstr(path, "MovieMenu")) mk8_auto_menu = 1;
}
void mk8_auto_lan_vlan_up(void) { mk8_auto_vlan = 1; }
#define AUTO_BEACON_PORT 47999
enum { AUTO_NONE, AUTO_MENU, AUTO_VLAN };
#define AUTO_A (1ull << 0)
#define AUTO_LSTICK (1ull << 4)
#define AUTO_L (1ull << 6)
#define AUTO_R (1ull << 7)
#define AUTO_DOWN (1ull << 15)

typedef struct {
    int wait;          // event that must have happened (AUTO_NONE: only the delay)
    uint32_t delay;    // frames after the event / the previous step
    uint32_t timeout;  // frames to wait for the event before giving up
    uint64_t buttons;  // 0: nothing pressed (a marker step)
    uint32_t hold;
    const char *what;
} AutoStep;

static const AutoStep auto_common[] = {
    {AUTO_MENU, 330, 36000, AUTO_A, 6, "title"},
    // The title opens the Mii picker, whose first item is the active local profile.
    // DOWN here selected the preset below it ("Player"), not a main-menu option.
    {AUTO_NONE, 156, 0, 0, 0, "Mii: keep active profile"},
    {AUTO_NONE, 90, 0, AUTO_A, 6, "Mii: confirm active profile"},
    {AUTO_NONE, 300, 0, AUTO_L | AUTO_R, 20, "LAN: hold L+R"},
    {AUTO_NONE, 0, 0, AUTO_L | AUTO_R | AUTO_LSTICK, 6, "LAN: left stick"},
    {AUTO_NONE, 0, 0, AUTO_L | AUTO_R, 64, "LAN: release"},
    {AUTO_NONE, 200, 0, AUTO_DOWN, 6, "LAN menu: down"},
    {AUTO_NONE, 60, 0, AUTO_DOWN, 6, "LAN menu: down"},
    {AUTO_NONE, 60, 0, AUTO_DOWN, 6, "LAN menu: down"},
    {AUTO_NONE, 90, 0, AUTO_A, 6, "LAN menu: lobby"},
    {AUTO_VLAN, 0, 600, 0, 0, "virtual LAN up"},
};
static const AutoStep auto_host[] = {
    {AUTO_NONE, 240, 0, AUTO_A, 6, "lobby: create room"},
};
static _Atomic int mk8_auto_lan_owns_title = -1;

static int mk8_auto_lan_role(void) {
    const char *mode = getenv("MK8_AUTO_LAN"), *slot = getenv("LARECOMP_LAN_SLOT");
    if (!mode || !getenv("LARECOMP_LAN_RELAY")) return 0;
    if (!strcmp(mode, "host")) return 1;
    if (!strcmp(mode, "guest")) return 2;
    if (!strcmp(mode, "1") && slot && *slot && !slot[1]) {
        if (*slot=='0') return 1;
        if (*slot>='1' && *slot<='3') return 2;
    }
    return 0;
}

bool mk8_guest_lan_requested(void) {
    if (mk8_auto_lan_role() != 2) return false;
    const char *slot=getenv("LARECOMP_LAN_SLOT"), *room=getenv("LARECOMP_LAN_ROOM");
    const char *relay=getenv("LARECOMP_LAN_RELAY"), *players=getenv("MK8_LAN_PLAYERS");
    const char *port=strrchr(relay, ':');
    return slot && *slot>='1' && *slot<='3' && !slot[1] && room && *room && strlen(room)<=16 &&
           port && port!=relay && port[1] && (!players || !strcmp(players,"1") || !strcmp(players,"2"));
}

int mk8_guest_lan_hide_boot_logo(void) {
    static bool visited;
    if (visited) return 0;
    visited = true;
    return mk8_guest_lan_requested();
}

// The stock LAN sequence supplies its own title confirm, including before its first input frame.
bool mk8_auto_lan_requested(void) {
    int active = mk8_auto_lan_owns_title;
    return active < 0 ? mk8_auto_lan_role() == 1 : active != 0;
}

static void mk8_auto_lan_frame(uint64_t frame) {
    static int role = -1;  // Only hosts run the input sequence.
    static size_t step;
    static uint64_t since, busy_until;
    if (role < 0) {
        role = mk8_auto_lan_role() == 1;
        mk8_auto_lan_owns_title = role != 0;
        if (role) LOG("[mk8:auto-lan] entering the LAN room as host (experimental)\n");
        since = frame;
    }
    if (role == 1 && step == (size_t)-1) {  // (host done: announce the room to the guests' runtimes once a second)
        extern int vlan_send(uint16_t, uint32_t, uint16_t, const void *, size_t);
        if (frame % 60 == 0) vlan_send(AUTO_BEACON_PORT, 0xFFFFFFFFu, AUTO_BEACON_PORT, "MK8-ROOM", 8);
        return;
    }
    if (!role || frame < busy_until) return;
    size_t common = sizeof auto_common / sizeof *auto_common;
    size_t count = common + sizeof auto_host / sizeof *auto_host;
    if (step >= count) return;
    const AutoStep *s = step < common ? &auto_common[step] : &auto_host[step - common];
    int happened = s->wait == AUTO_NONE || (s->wait == AUTO_MENU && mk8_auto_menu) ||
                   (s->wait == AUTO_VLAN && mk8_auto_vlan);
    if (!happened) {
        if (frame - since > s->timeout) {
            LOG("[mk8:auto-lan] stopped at \"%s\" (frame %llu): continue by hand\n", s->what, (unsigned long long)frame);
            role = 0;
            mk8_auto_lan_owns_title = 0;
        }
        return;
    }
    if (s->wait != AUTO_NONE && s->delay) {  // (the delay counts from the event, seen this frame)
        static size_t seen = (size_t)-1;
        if (seen != step) { seen = step; since = frame; }
    }
    if (frame - since < s->delay) return;
    if (s->buttons) {
        extern uint64_t g_test_buttons, g_test_buttons_until;
        g_test_buttons = s->buttons;
        g_test_buttons_until = frame + s->hold;
    }
    LOG("[mk8:auto-lan] %s (frame %llu)\n", s->what, (unsigned long long)frame);
    busy_until = frame + s->hold;
    since = frame + s->hold;
    if (++step == count) {
        mk8_auto_lan_owns_title = 0;
        LOG("[mk8:auto-lan] done: choose your character\n");
        if (role == 1) step = (size_t)-1;
    }
}
