// Experimental: enter the LAN room by itself, for rooms opened through the launcher (MK8_AUTO_LAN=host|guest, or 1:
// slot 0 hosts). Included into hle/input.c by toolchain/auto_lan.py.
//
// It presses what a player would, through the game's own input (g_test_buttons, OR'd with the player's input), and
// waits for what it can observe instead of fixed times: the menu movie opening (the title screen), the virtual LAN
// starting (the game entered the LAN lobby) and, for a guest, the host's room beacon: once the host's sequence has
// created the room, its runtime broadcasts a datagram on a port the game never uses (AUTO_BEACON_PORT, kept out of the
// game by net/vlan.c). Without the beacon a guest never presses A: on an empty list A would create a room. Any wait
// that runs out stops the sequence; the player goes on by hand. Character and rules stay the player's.
#include <stdatomic.h>

static _Atomic int mk8_auto_menu, mk8_auto_vlan, mk8_auto_room;

void mk8_auto_lan_movie(const char *path) {
    if (path && strstr(path, "MovieMenu")) mk8_auto_menu = 1;
}
void mk8_auto_lan_vlan_up(void) { mk8_auto_vlan = 1; }
#define AUTO_BEACON_PORT 47999
void mk8_auto_lan_beacon(uint32_t from, const uint8_t *data, size_t len) {
    if (len != 8 || memcmp(data, "MK8-ROOM", 8)) return;
    if (!mk8_auto_room) LOG("[mk8:auto-lan] room beacon from 10.13.37.%u\n", (unsigned)(from & 0xff));
    mk8_auto_room = 1;
}

enum { AUTO_NONE, AUTO_MENU, AUTO_VLAN, AUTO_ROOM };
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
    {AUTO_NONE, 150, 0, AUTO_DOWN, 6, "menu: down"},
    {AUTO_NONE, 90, 0, AUTO_A, 6, "menu: choose"},
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
static const AutoStep auto_guest[] = {
    {AUTO_ROOM, 180, 18000, AUTO_DOWN, 6, "lobby: the host's room"},
    {AUTO_NONE, 150, 0, AUTO_A, 6, "lobby: join"},
};

static void mk8_auto_lan_frame(uint64_t frame) {
    static int role = -1;  // 0 off, 1 host, 2 guest
    static size_t step;
    static uint64_t since, busy_until;
    if (role < 0) {
        const char *mode = getenv("MK8_AUTO_LAN"), *slot = getenv("LARECOMP_LAN_SLOT");
        role = 0;
        if (mode && getenv("LARECOMP_LAN_RELAY")) {
            if (!strcmp(mode, "host")) role = 1;
            else if (!strcmp(mode, "guest")) role = 2;
            else if (!strcmp(mode, "1")) role = slot && atoi(slot) == 0 ? 1 : 2;
        }
        if (role) LOG("[mk8:auto-lan] entering the LAN room as %s (experimental)\n", role == 1 ? "host" : "guest");
        since = frame;
    }
    if (role == 1 && step == (size_t)-1) {  // (host done: announce the room to the guests' runtimes once a second)
        extern int vlan_send(uint16_t, uint32_t, uint16_t, const void *, size_t);
        if (frame % 60 == 0) vlan_send(AUTO_BEACON_PORT, 0xFFFFFFFFu, AUTO_BEACON_PORT, "MK8-ROOM", 8);
        return;
    }
    if (!role || frame < busy_until) return;
    size_t common = sizeof auto_common / sizeof *auto_common;
    const AutoStep *steps = role == 1 ? auto_host : auto_guest;
    size_t count = common + (role == 1 ? sizeof auto_host / sizeof *auto_host : sizeof auto_guest / sizeof *auto_guest);
    if (step >= count) return;
    const AutoStep *s = step < common ? &auto_common[step] : &steps[step - common];
    int happened = s->wait == AUTO_NONE || (s->wait == AUTO_MENU && mk8_auto_menu) ||
                   (s->wait == AUTO_VLAN && mk8_auto_vlan) || (s->wait == AUTO_ROOM && mk8_auto_room);
    if (!happened) {
        if (frame - since > s->timeout) {
            LOG("[mk8:auto-lan] stopped at \"%s\" (frame %llu): continue by hand\n", s->what, (unsigned long long)frame);
            role = 0;
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
        LOG("[mk8:auto-lan] done: choose your character\n");
        if (role == 1) step = (size_t)-1;
    }
}
