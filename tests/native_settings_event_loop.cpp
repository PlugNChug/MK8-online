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
static unsigned events, polls, transitions;
static uint64_t previous;
static SDL_JoystickID id;
static SDL_Joystick *stick;
static SDL_Gamepad *pad;

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
    // Keep the queue nonempty even when the host ignores an event.
    assert(++events<20000 && "window requests/controller sampling starved by the event loop");
    noise();
    return physical_blocked;
}
static bool menu_handle_event(const SDL_Event *event) { return mk8_settings_event(event,false); }
static void input_handle_event(const SDL_Event *) {}
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
}

// The actual prepared Vulkan UI loop, supplied by the Python test.
#include "settings_ui_loop.inc"

int main() {
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
    assert(polls==80 && transitions==19);
    SDL_CloseGamepad(pad);SDL_CloseJoystick(stick);SDL_DetachVirtualJoystick(id);SDL_Quit();
}
