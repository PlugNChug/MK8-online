#include <map>

extern "C" void mk8_suppress_settings_buttons(SDL_JoystickID, uint32_t);
extern "C" int mk8_pause_owner(SDL_JoystickID);
extern "C" void mk8_release_pause_button(SDL_JoystickID, unsigned);

// Two players pressing one button each must never form a shortcut together.
static bool mk8_settings_chord(const SDL_Event *event, bool captured) {
    struct Press { unsigned down; bool chord; int owner; };
    static std::map<SDL_JoystickID, Press> held;
    if (event->type == SDL_EVENT_GAMEPAD_REMOVED) held.erase(event->gdevice.which);
    if (event->type == SDL_EVENT_WINDOW_FOCUS_LOST) held.clear();
    if (captured) for (auto &entry : held) entry.second.owner = -1;
    if (event->type != SDL_EVENT_GAMEPAD_BUTTON_DOWN && event->type != SDL_EVENT_GAMEPAD_BUTTON_UP) return false;
    unsigned button = event->gbutton.button;
    if (button != SDL_GAMEPAD_BUTTON_BACK && button != SDL_GAMEPAD_BUTTON_START) return false;
    SDL_JoystickID id = event->gbutton.which;
    unsigned bit = 1u << button;
    if (event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        Press &press = held[id];
        if (press.down & bit) return false;
        if (!press.down) press.owner = !captured && mk8_settings_shared_race() ? mk8_pause_owner(id) : -1;
        press.down |= bit;
        if (press.down == ((1u << SDL_GAMEPAD_BUTTON_BACK) | (1u << SDL_GAMEPAD_BUTTON_START))) {
            press.chord = true;
            return true;
        }
        return false;
    }
    auto it = held.find(id);
    if (it == held.end() || !(it->second.down & bit)) return false;
    Press &press = it->second;
    if (!press.chord && press.owner >= 0 && press.owner == mk8_pause_owner(id))
        mk8_release_pause_button(id, button);
    press.down &= ~bit;
    if (!press.down) held.erase(it);
    return false;
}

// Readable for three full seconds between two 200 ms fades. Other game notices retain their own timing.
static float mk8_race_notice_alpha(Uint64 elapsed_ms) {
    if (elapsed_ms<200) return elapsed_ms/200.f;
    if (elapsed_ms<=3200) return 1.f;
    return elapsed_ms<3400 ? (3400-elapsed_ms)/200.f : 0.f;
}

extern "C" void mk8_menu_tip(const char *text) {
    pthread_mutex_lock(&notice_lock);
    Uint64 now=SDL_GetTicks();
    // Renew the reading time on another press, starting the new fade at the current opacity.
    float alpha=notice_fades ? mk8_race_notice_alpha(now-notice_started) : 0.f;
    notice_started=now-(Uint64)(200.f*alpha);
    snprintf(notice_text,sizeof notice_text,"%s",text);
    notice_fades=true;
    notice_until=(notice_started+3400)/1000.0;
    pthread_mutex_unlock(&notice_lock);
}

static std::atomic<bool> mk8_lan_claim_pending;
static const char *const mk8_lan_claim_hint = "Choose Player 1. Press a controller button or a key.";
extern "C" void mk8_controller_names(uint16_t names[4][80]);

extern "C" void mk8_lan_claim_notice(int state) {
    mk8_lan_claim_pending.store(state == 1);
    if (state != 2) return;
    uint16_t names[4][80];
    mk8_controller_names(names);
    char *device = SDL_iconv_string("UTF-8", "UTF-16LE", (const char *)names[0], sizeof names[0]);
    char label[384];
    snprintf(label, sizeof label, "Player 1 · %s", device ? device : "Controller");
    SDL_free(device);
    mk8_menu_tip(label);
}

static bool mk8_block_shared_settings(void) {
    if (!mk8_settings_shared_race()) return false;
    mk8_menu_tip("Settings cannot be accessed during a multiplayer race.");
    return true;
}
