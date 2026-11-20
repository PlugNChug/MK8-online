extern "C" bool mk8_settings_page_active(void);
extern "C" void mk8_settings_page_back(void);
extern "C" bool mk8_settings_page_key(int);
extern "C" bool mk8_settings_page_toggle(void);

static float mk8_settings_phase(bool open) {
    static bool was_open;
    static Uint64 began;
    Uint64 now = SDL_GetTicks();
    if (open && !was_open) began = now;
    was_open = open;
    float t = fminf(1, (now - began) / 160.0f);
    return 1 - (1-t)*(1-t)*(1-t);
}
