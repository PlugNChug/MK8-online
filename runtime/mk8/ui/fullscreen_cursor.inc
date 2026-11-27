typedef struct {
    Uint64 last_activity;
    bool hidden;
} Mk8FullscreenCursor;

static void mk8_cursor_event(Mk8FullscreenCursor *cursor, SDL_WindowID window, const SDL_Event *event) {
    SDL_WindowID target;
    SDL_MouseID mouse;
    switch (event->type) {
    case SDL_EVENT_MOUSE_MOTION:
        target = event->motion.windowID;
        mouse = event->motion.which;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        target = event->button.windowID;
        mouse = event->button.which;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        target = event->wheel.windowID;
        mouse = event->wheel.which;
        break;
    default:
        return;
    }
    if (target == window && mouse != SDL_TOUCH_MOUSEID)
        cursor->last_activity = SDL_GetTicks();
}

static void mk8_cursor_update(Mk8FullscreenCursor *cursor, SDL_Window *window, bool enabled) {
    const SDL_WindowFlags required = SDL_WINDOW_FULLSCREEN | SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    const Uint64 now = SDL_GetTicks();
    const bool active = enabled && (SDL_GetWindowFlags(window) & required) == required;
    if (!active) cursor->last_activity = now;
    const bool hide = active && now - cursor->last_activity >= 2000;
    if (hide != cursor->hidden && (hide ? SDL_HideCursor() : SDL_ShowCursor()))
        cursor->hidden = hide;
}
