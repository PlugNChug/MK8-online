// Keep the real HID state writer; the guest frame services are idle in this input test.
static uint64_t script_buttons, g_pad_obj;
static int32_t script_lx, script_ly, script_rx, script_ry;
volatile int g_inject_plus;
uint64_t g_watch_lo, g_watch_hi, g_stick_freeze_until;
int g_stick_lx, g_stick_ly;
uint64_t padscript_buttons(void) { return 0; }
static uint64_t presented_frames=1;
uint64_t renderer_present_count(void) { return presented_frames; }
uint64_t game_input_mask(void) { return UINT64_MAX; }
void touchpad_state(uint64_t *buttons, int32_t *lx, int32_t *ly) { *buttons = 0; *lx = *ly = 0; }
uint64_t touch_gesture_buttons(void) { return 0; }
bool mirror_input(void) { return false; }
void warp_game_tick(Cpu *c) {}
void give_item_tick(Cpu *c, uint64_t frame) {}
void chamber_game_tick(Cpu *c, uint64_t frame) {}
void digs_game_tick(Cpu *c, uint64_t frame) {}
void pets_game_tick(Cpu *c) {}
void qol_tick(void) {}
uint64_t pets_menu_input(uint64_t buttons, int32_t sticks[4]) { return buttons; }
uint64_t mk8_settings_page_input(uint64_t buttons, int32_t sticks[4], uint64_t scripted) { return buttons; }
void input_note_rstick(int32_t ry) {}
#include "guest_pad.inc"

static void check_keyboard_routing(void) {
    Cpu cpu = {0};
    const SDL_Scancode held[] = {SDL_SCANCODE_RETURN, SDL_SCANCODE_D, SDL_SCANCODE_KP_8};
    for (unsigned k = 0; k < SDL_arraysize(held); ++k) {
        SDL_Event key = {.type = SDL_EVENT_KEY_DOWN};
        key.key.scancode = held[k];
        input_handle_event(&key);
    }
    const int owners[] = {0, 2, -1};
    for (unsigned i = 0; i < SDL_arraysize(owners); ++i) {
        int owner = owners[i];
        mk8_set_assigned_keyboard(owner);
        mk8_publish_connections();
        for (int player = 0; player < 4; ++player) {
            uint64_t state[5];
            fill_state(&cpu, (uint8_t *)state, player, false);
            assert((uint32_t)state[4] == (player == 0 || player == owner));
            assert(state[1] == (player == owner ? (1ull << BTN_PLUS | 1ull << 18 | 1ull << 21) : 0));
            assert((int32_t)state[2] == (player == owner ? 32767 : 0));
            assert((int32_t)(state[3] >> 32) == (player == owner ? 32767 : 0));
        }
        assert((input_player_buttons() != 0) == (owner == 0));
        assert((mk8_controller_p1_navigation() != 0) == (owner == 0));
    }
    input_reset();
    mk8_set_assigned_keyboard(0);
    mk8_publish_connections();
}
