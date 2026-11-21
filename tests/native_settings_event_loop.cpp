#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>

static std::atomic<bool> ui_ready;
static std::atomic<int> g_display_hz;
static SDL_Window *window;
static bool active=true, physical_blocked;
static uint64_t held;
static unsigned events, polls, transitions, editor_polls, tip_polls, tip_events, title_polls, title_events;
static unsigned steam_polls, rumble_polls;
void mk8_poll_steam_gamepads(void) { ++steam_polls; }
bool mk8_steam_gamepad_event(const SDL_Event *) { return false; }
void mk8_rumble_pump(void) { ++rumble_polls; }
static uint64_t previous;
static SDL_JoystickID id;
static SDL_Joystick *stick;
static SDL_Gamepad *pad;
static unsigned drawable_updates, resize_events, before_poll;
static int drawable_width, drawable_height;
static bool minimized, query_ok=true;
void mk8_ultrawide_drawable(int width,int height) {
    if(!drawable_updates) assert(!ui_ready.load());
    drawable_width=width;drawable_height=height;drawable_updates++;
}
static SDL_WindowFlags test_window_flags(SDL_Window *) { return minimized ? SDL_WINDOW_MINIMIZED : 0; }
static bool test_window_size(SDL_Window *,int *width,int *height) {
    *width=2560;*height=720;return query_ok;
}
#define SDL_GetWindowFlags test_window_flags
#define SDL_GetWindowSizeInPixels test_window_size

extern "C" bool mk8_settings_page_active(void) { return active; }
extern "C" void mk8_settings_page_fixed(uint64_t value) { held=value; }
extern "C" void mk8_settings_page_wheel(int) {}
extern "C" bool mk8_settings_page_key(int) { return false; }
extern "C" bool bind_capture_active(int *,int *,int *) { return false; }
static bool bind_capture_event(const SDL_Event *) { return false; }
static bool menu_ab_swapped(void) { return false; }
#include "settings_controls.inc"

static void noise() {
    SDL_Event event={}; event.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.which=id; event.gaxis.axis=SDL_GAMEPAD_AXIS_RIGHTX;
    assert(SDL_PushEvent(&event));
}
extern "C" bool input_user_blocked(const SDL_Event *event) {
    if(!event) return physical_blocked;
    if(event->type==SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        // The actual publisher must run before an input filter consumes this event.
        assert(drawable_updates==before_poll+2 && drawable_width==2560 && drawable_height==720);
        resize_events++; return true;
    }
    // Keep the queue nonempty even when the host ignores an event.
    assert(++events<20000 && "window requests/controller sampling starved by the event loop");
    noise();
    return physical_blocked;
}
static bool menu_handle_event(const SDL_Event *event) { return mk8_settings_event(event,false); }
static void input_handle_event(const SDL_Event *) {}
void mk8_edit_pump() { editor_polls++; }
void mk8_title_pump() { ++title_polls; }
bool menu_input_captured() { return false; }
bool mk8_title_event(const SDL_Event *,bool captured) { assert(captured && !physical_blocked); ++title_events; return false; }
bool mk8_controller_layout_active() { return false; }
void mk8_tip_sync() { tip_polls++; }
void mk8_tip_event(const SDL_Event *, bool settings) {
    assert(settings && !physical_blocked);
    tip_events++;
}
static void ui_window_init() {}
static int framegen_display_hz(SDL_Window *) { return 60; }
static int rt_reboot_requested() { return polls==80 ? 1 : 0; }
#define LOG(...) ((void)0)
#define atomic_store(p,v) ((p)->store(v))

static void set_input(bool down) {
    assert(SDL_SetJoystickVirtualButton(stick,SDL_GAMEPAD_BUTTON_EAST,down));
    assert(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTY,down ? 30000 : 0));
    SDL_UpdateJoysticks();
}
static void menu_apply_window_requests(void *) {
    assert(steam_polls==polls+1 && rumble_polls==polls+1);
    before_poll=drawable_updates;
    assert(events<=128); events=0;
    // Apply the same close/reopen operation as reassignment while input is held.
    if(polls%4==2) { SDL_CloseGamepad(pad);pad=SDL_OpenGamepad(id);assert(pad); }
    mk8_settings_poll(false);
    uint64_t expected=(!physical_blocked && (polls%4==1 || polls%4==2)) ? 1|(1ull<<15) : 0;
    assert(held==expected);
    if(held&~previous) transitions++;
    previous=held;
    polls++;
    physical_blocked=polls>=40 && polls<44;
    set_input(polls%4==1 || polls%4==2);
    if(polls==2) { SDL_Event event={};event.type=SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;assert(SDL_PushEvent(&event)); }
}

// The actual prepared Vulkan UI loop, supplied by the Python test.
#include "settings_ui_loop.inc"

int main() {
    // Use the real prepared publisher without requiring a display/GPU in this fixture.
    mk8_ultrawide_update_drawable();assert(drawable_width==2560 && drawable_height==720);
    minimized=true;mk8_ultrawide_update_drawable();assert(!drawable_width && !drawable_height);
    minimized=false;query_ok=false;mk8_ultrawide_update_drawable();assert(!drawable_width && !drawable_height);
    query_ok=true;drawable_updates=0;
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc desc;SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;desc.naxes=SDL_GAMEPAD_AXIS_COUNT;desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="Settings event flood regression";
    id=SDL_AttachVirtualJoystick(&desc);assert(id);
    stick=SDL_OpenJoystick(id);pad=SDL_OpenGamepad(id);assert(stick && pad);
    // Lost releases and neutral noise must not starve polling or preserve old held bits.
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_BUTTON_UP,false);
    SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_AXIS_MOTION,false);
    SDL_UpdateJoysticks();mk8_settings_poll(false);
    noise();assert(ui_run()==1);
    assert(polls==80 && transitions==19 && resize_events==1 && drawable_updates>=82);
    assert(steam_polls==polls && rumble_polls==polls);
    assert(editor_polls==polls && title_polls==polls && tip_polls==polls && tip_events==title_events && title_events);
    SDL_CloseGamepad(pad);SDL_CloseJoystick(stick);SDL_DetachVirtualJoystick(id);SDL_Quit();
}
