// The presenter's drawable rectangle and SDL window coordinates can have different pixel densities.
bool mk8_controller_pointer(int x, int y, int action);
bool mk8_controller_layout_active(void);

bool mk8_controller_pointer_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST || e->type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
        e->type == SDL_EVENT_WINDOW_MOUSE_LEAVE) {
        int action = e->type == SDL_EVENT_WINDOW_MOUSE_LEAVE ? 0 : e->type == SDL_EVENT_WINDOW_FOCUS_GAINED ? 4 : 3;
        mk8_controller_pointer(-1, -1, action);
        return false;  // Other input consumers still need the window event.
    }
    bool motion = e->type == SDL_EVENT_MOUSE_MOTION;
    if (!motion && e->type != SDL_EVENT_MOUSE_BUTTON_DOWN && e->type != SDL_EVENT_MOUSE_BUTTON_UP) return false;
    if ((!motion && e->button.button != SDL_BUTTON_LEFT) ||
        (motion ? e->motion.which : e->button.which) == SDL_TOUCH_MOUSEID) return false;
    if (!mk8_controller_layout_active())
        return !motion && e->type == SDL_EVENT_MOUSE_BUTTON_UP && mk8_controller_pointer(-1, -1, 2);

    SDL_Window *win = SDL_GetWindowFromID(motion ? e->motion.windowID : e->button.windowID);
    int w, h;
    int32_t x = -1, y = -1;
    if (win && SDL_GetWindowSize(win, &w, &h) && w > 0 && h > 0) {
        float fx = (motion ? e->motion.x : e->button.x) / w;
        float fy = (motion ? e->motion.y : e->button.y) / h;
        pthread_mutex_lock(&lock);
        double px = fx * rect[4], py = fy * rect[5];
        if (px >= rect[0] && px < rect[0] + rect[2] && py >= rect[1] && py < rect[1] + rect[3])
            to_game(fx, fy, &x, &y);
        pthread_mutex_unlock(&lock);
    }
    return mk8_controller_pointer(x, y, motion ? 0 : e->type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 1 : 2);
}
