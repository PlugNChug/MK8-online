#include <assert.h>
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "hle.h"
#include "kernel.h"
#include "settings.h"
#include "input_bind.h"
#include "padscript.h"
#include <windows.h>
#undef IN
#undef OUT
#undef LOG
#define LOG(...) ((void)0)
Settings g_settings;
static unsigned style_notifications;
static KEvent style_event;
pthread_mutex_t g_klock=PTHREAD_MUTEX_INITIALIZER;
void kernel_signal(KObject *event) {
    assert(event==&style_event && pthread_mutex_trylock(&g_klock)==EBUSY);
    ++style_notifications;
}
volatile int g_rt_unwind;
static SDL_AtomicInt core_released;
static char applet_notice[192];
static bool assignment_layout=true;
bool mk8_controller_layout_available(void) { return assignment_layout; }
bool mk8_controller_join_visible(void) { return assignment_layout; }
void kernel_host_wait_begin(void) { SDL_SetAtomicInt(&core_released,1); }
void kernel_host_wait_end(void) { SDL_SetAtomicInt(&core_released,0); }
void menu_notice(const char *text,double seconds) { SDL_strlcpy(applet_notice,text,sizeof applet_notice); }
void mk8_settings_pads_changed(void) { assert(0 && "No settings changes during a controller assignment"); }
static bool host_captures;
bool menu_input_captured(void) { return host_captures; }
bool menu_is_open(void) { return false; }
bool mk8_settings_page_active(void) { return false; }
static bool shared_race;
bool mk8_settings_shared_race(void) { return shared_race; }
bool game_overlay_captured(void) { return false; }
static bool scripted_input;
bool padscript_active(void) { return scripted_input; }
bool touchpad_handle_event(const SDL_Event *e) { return false; }
bool touch_handle_event(const SDL_Event *e) { return false; }
bool game_pc_key(int sc, bool cmd, bool shift, bool down, bool repeat) { return false; }
bool game_pc_mouse(int button, int act, float x, float y) { return false; }
bool game_pc_wheel(float dx, float dy, bool shift) { return false; }

enum Device { PRO, PAIR_L, PAIR_R, SOLO_L, SOLO_R, DEVICE_COUNT };
static SDL_JoystickID ids[DEVICE_COUNT];
static SDL_JoystickID counted_id;
static unsigned cold_opens, failures_remaining;
static SDL_Gamepad *count_gamepad_open(SDL_JoystickID id) {
    if (id == counted_id && failures_remaining) {
        --failures_remaining;
        return NULL;
    }
    bool cold = id == counted_id && !SDL_GetJoystickFromID(id);
    SDL_Gamepad *g = SDL_OpenGamepad(id);
    if (cold && g) ++cold_opens;
    return g;
}
static bool fixture_serials, returning_serial_ready, spare_serial_ready;
static const char *fixture_serial(SDL_Gamepad *gamepad) {
    if (!fixture_serials) return SDL_GetGamepadSerial(gamepad);
    SDL_JoystickID id = SDL_GetGamepadID(gamepad);
    if (id == ids[PAIR_L]) return "pair-left";
    if (id == ids[PAIR_R]) return returning_serial_ready ? "pair-right" : NULL;
    if (id == ids[SOLO_R]) return spare_serial_ready ? "other-right" : NULL;
    return NULL;
}
// SDL virtual joysticks have no serial field. Only these Bluetooth metadata reads are simulated.
#define SDL_GetGamepadSerial fixture_serial
#define SDL_OpenGamepad count_gamepad_open
#include "mixed.inc"
#undef SDL_OpenGamepad
#undef SDL_GetGamepadSerial
#include "keyboard_routing.inc"

static SDL_Joystick *sticks[DEVICE_COUNT];
static bool buzzing[DEVICE_COUNT];
static bool fixed_navigation;
static bool SDLCALL rumble(void *data, Uint16 low, Uint16 high) {
    *(bool *)data = low || high;
    return true;
}
static bool SDLCALL sensors(void *data, bool enabled) { return true; }
static void attach_as(enum Device device,enum Device model) {
    const char *names[] = {"Contrôle 🎮", "GrIp 左", "GrIp 右", "Nintendo Switch Joy-Con (L)", "Nintendo Switch Joy-Con (R)"};
    const Uint16 products[] = {0x2009, 0x2006, 0x2007, 0x2006, 0x2007};
    SDL_VirtualJoystickSensorDesc motion[] = {{SDL_SENSOR_ACCEL, 200}, {SDL_SENSOR_GYRO, 200}};
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = 0x057e;
    d.product_id = products[model];
    d.name = names[model];
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.sensors = motion;
    d.nsensors = SDL_arraysize(motion);
    d.SetSensorsEnabled = sensors;
    d.userdata = &buzzing[device];
    d.Rumble = rumble;
    ids[device] = SDL_AttachVirtualJoystick(&d);
    assert(ids[device] && SDL_IsJoystickVirtual(ids[device]));
    sticks[device] = SDL_OpenJoystick(ids[device]);
    assert(sticks[device]);
}
static void attach(enum Device device) { attach_as(device,device); }
static void pump(void) {
    mk8_title_pump();
    SDL_UpdateJoysticks();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        mk8_tip_event(&event,fixed_navigation);
        if(mk8_title_event(&event,host_captures)) continue;
        extern bool settings_test_chord(const SDL_Event *,bool);
        if(settings_test_chord(&event,host_captures) && shared_race) {
            mk8_suppress_settings_buttons(event.gbutton.which,(1u<<SDL_GAMEPAD_BUTTON_BACK)|(1u<<SDL_GAMEPAD_BUTTON_START));
            continue; // The menu consumes the chord's second press.
        }
        input_handle_event(&event);
    }
    mk8_edit_pump();
    mk8_tip_sync();
}
static void set_extra_inputs(bool down) {
    // Guest P2 uses the right Joy-Con upright; its physical A arrives as sideways South.
    assert(SDL_SetJoystickVirtualButton(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_SOUTH, down));
    assert(SDL_SetJoystickVirtualButton(sticks[SOLO_L], SDL_GAMEPAD_BUTTON_EAST, down));
    assert(SDL_SetJoystickVirtualButton(sticks[SOLO_R], SDL_GAMEPAD_BUTTON_DPAD_LEFT, down));
    assert(SDL_SetJoystickVirtualAxis(sticks[PAIR_R], SDL_GAMEPAD_AXIS_LEFTY, down ? 24000 : 0));
    assert(SDL_SetJoystickVirtualAxis(sticks[SOLO_L], SDL_GAMEPAD_AXIS_LEFTX, down ? -20000 : 0));
    assert(SDL_SetJoystickVirtualAxis(sticks[SOLO_R], SDL_GAMEPAD_AXIS_LEFTY, down ? 32000 : 0));
    pump();
}
static void check_extra_inputs(bool physically_held, bool visible) {
    const int32_t expected[3][4]={{0,0,24000,0},{-20000,0,0,0},{0,-32000,0,0}};
    const uint64_t buttons[]={1ull<<BIND_A,1ull<<BIND_A,1ull<<BIND_LEFT};
    for(int i=0;i<EXTRA_PADS;i++) {
        uint64_t b=extra_buttons[i];
        int32_t axes[]={extra_lx[i],extra_ly[i],extra_rx[i],extra_ry[i]};
        assert(b==(physically_held ? buttons[i] : 0));
        for(int j=0;j<4;j++) assert(axes[j]==(physically_held ? expected[i][j] : 0));
        mk8_controller_filter_extra(i,&b,&axes[0],&axes[1],&axes[2],&axes[3]);
        assert(b==(visible ? buttons[i] : 0));
        for(int j=0;j<4;j++) assert(axes[j]==(visible ? expected[i][j] : 0));
    }
}
static void check_popup_owner(void) {
    const uint64_t plus = 1ull << BTN_PLUS;
    assert(SDL_SetJoystickVirtualButton(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_START, true));
    pump();
    assert((extra_buttons[0] & plus) && !mk8_controller_p1_navigation());
    assert(SDL_SetJoystickVirtualButton(sticks[PRO], SDL_GAMEPAD_BUTTON_START, true));
    pump();
    assert(mk8_controller_p1_navigation() == plus);
    assert(SDL_SetJoystickVirtualButton(sticks[PAIR_R], SDL_GAMEPAD_BUTTON_START, false));
    assert(SDL_SetJoystickVirtualButton(sticks[PRO], SDL_GAMEPAD_BUTTON_START, false));
    pump();
    assert(!(extra_buttons[0] & plus) && !mk8_controller_p1_navigation());
    SDL_Event key = {.type = SDL_EVENT_KEY_DOWN};
    key.key.scancode = SDL_SCANCODE_RETURN;
    input_handle_event(&key);
    assert(mk8_controller_p1_navigation() == plus);
    key.type = SDL_EVENT_KEY_UP;
    input_handle_event(&key);
    assert(!mk8_controller_p1_navigation());
    set_extra_inputs(true);
    assert(extra_buttons[0] && extra_buttons[1] && extra_buttons[2]);
    assert(!mk8_controller_p1_navigation());
    assert(SDL_SetJoystickVirtualButton(sticks[PRO], SDL_GAMEPAD_BUTTON_EAST, true));
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO], SDL_GAMEPAD_AXIS_LEFTX, 32000));
    pump();
    assert(mk8_controller_p1_navigation() == ((1u << BIND_A) | (1u << BIND_RIGHT)));
    assert(SDL_SetJoystickVirtualButton(sticks[PRO], SDL_GAMEPAD_BUTTON_EAST, false));
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO], SDL_GAMEPAD_AXIS_LEFTX, 0));
    pump();
    assert(!mk8_controller_p1_navigation());
    mk8_controller_hold_extra_inputs();
    check_extra_inputs(true,false);
    check_extra_inputs(true,false);
    set_extra_inputs(false);
    check_extra_inputs(false,false);
    set_extra_inputs(true);
    check_extra_inputs(true,true);
    set_extra_inputs(false);
}
static void check_popup_names(void) {
    uint16_t rows[4][80];
    mk8_controller_names(rows);
    assert(!memcmp(rows[0], u"Contrôle 🎮", sizeof(u"Contrôle 🎮")));
    assert(!memcmp(rows[1], u"GrIp 左 + GrIp 右", sizeof(u"GrIp 左 + GrIp 右")));
    assert(!memcmp(rows[2], u"Nintendo Switch Joy-Con (L)", sizeof(u"Nintendo Switch Joy-Con (L)")));
    assert(!memcmp(rows[3], u"Nintendo Switch Joy-Con (R)", sizeof(u"Nintendo Switch Joy-Con (R)")));
    mk8_label_device(rows[2], extra_pad[1], extra_pad[2], false);
    assert(!memcmp(rows[2], u"Paired Joy-Cons", sizeof(u"Paired Joy-Cons")));
    uint16_t scratch[81], limited[80] = {0};
    assert(mk8_device_name(NULL, scratch) == 18);
    assert(!memcmp(scratch, u"Unnamed controller", 18 * sizeof(uint16_t)));
    char longname[100];
    memset(longname, 'A', 77);
    strcpy(longname + 77, "🎮🎮🎮🎮");
    size_t n = mk8_device_name(longname, scratch);
    assert(mk8_copy_name(limited, scratch, n, 79) == 78);
    assert(limited[76] == 'A' && limited[77] == 0x2026 && !limited[78]);
}
static void pulse(unsigned player, unsigned side, float amplitude) {
    struct { uint32_t handle; Mk8Vibration value; } payload = {
        3u | (player << 8) | (side << 16), {amplitude, 160, amplitude, 320}
    };
    IpcReq req = {.cmd=201, .data=(const uint8_t *)&payload, .data_size=sizeof payload};
    IpcResp resp = {0};
    mk8_hid_rumble(&req, &resp);
    mk8_rumble_pump();
    assert(!resp.result);
}
static void expect_buzz(enum Device device) {
    for (int i = 0; i < DEVICE_COUNT; ++i) assert(buzzing[i] == (i == device));
}
static void check_pulse(unsigned player, unsigned side, enum Device device) {
    pulse(player, side, 1.f);
    expect_buzz(device);
    pulse(player, side, 0.f);
    expect_buzz(DEVICE_COUNT);
}
static void check_batched_pair(void) {
    uint8_t *guest = VirtualAlloc((void *)(GUEST_LO + 0x100000), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(guest);
    uint32_t handles[] = {3u | 1u << 8, 3u | 1u << 8 | 1u << 16};
    Mk8Vibration values[] = {{.25f, 120, .5f, 220}, {.5f, 240, .75f, 440}};
    memcpy(guest, handles, sizeof handles);
    memcpy(guest + 16, values, sizeof values);
    IpcReq batch = {.cmd=206, .nx=2};
    batch.x[0] = (IpcBuf){(uintptr_t)guest, sizeof handles};
    batch.x[1] = (IpcBuf){(uintptr_t)(guest + 16), sizeof values};
    IpcResp resp = {0};
    mk8_hid_rumble(&batch, &resp);
    mk8_rumble_pump();
    assert(!resp.result && buzzing[PAIR_L] && buzzing[PAIR_R]);
    assert(!buzzing[PRO] && !buzzing[SOLO_L] && !buzzing[SOLO_R]);
    for (unsigned side = 0; side < 2; ++side) {
        IpcReq get = {.cmd=202, .data=(const uint8_t *)&handles[side], .data_size=sizeof(uint32_t)};
        IpcResp state = {0};
        mk8_hid_rumble(&get, &state);
        assert(!state.result && !memcmp(state.data, values + side, sizeof values[0]));
        pulse(1, side, 0.f);
        if (!side) expect_buzz(PAIR_R);
    }
    expect_buzz(DEVICE_COUNT);
    assert(VirtualFree(guest, 0, MEM_RELEASE));
}
static void check_motion(unsigned player, enum Device source, bool upright_pair) {
    const float value[] = {source + 1.f, 10.f, 12.f};
    const uint64_t ns = SDL_GetTicksNS();
    assert(SDL_SendJoystickVirtualSensorData(sticks[source], SDL_SENSOR_ACCEL, ns, value, 3));
    assert(SDL_SendJoystickVirtualSensorData(sticks[source], SDL_SENSOR_GYRO, ns, value, 3));
    pump();
    float accel[3], gyro[3];
    uint64_t generation, stamp;
    assert(mk8_motion_read(player, &generation, &stamp, accel, gyro) && stamp == ns);
    assert(accel[0] == (upright_pair ? value[2] : value[0]));
    assert(accel[1] == value[1] && accel[2] == (upright_pair ? -value[0] : value[2]));
    assert(!memcmp(accel, gyro, sizeof accel));
}

static enum Mk8EditState edit(enum Mk8EditCommand cmd, int row, int other) {
    assert(mk8_edit_command(cmd,row,other));
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_PENDING);
    mk8_edit_pump();
    return mk8_edit_view(NULL,NULL);
}

static void button(SDL_Joystick *j, SDL_GamepadButton button, bool pressed) {
    assert(SDL_SetJoystickVirtualButton(j,button,pressed));
    pump();
}

static uint64_t guest_buttons(unsigned player) {
    Cpu cpu={0};
    uint64_t state[5];
    fill_state(&cpu,(uint8_t *)state,player,false);
    return state[1];
}

static void check_blocked_settings_chord(void) {
    const uint64_t plus = 1ull << BTN_PLUS, minus = 1ull << BTN_MINUS;
    Cpu cpu = {0};
    uint64_t p1[5], p2[5];
    shared_race=true;
    for(unsigned order=0;order<2;order++) {
        SDL_GamepadButton first=order ? SDL_GAMEPAD_BUTTON_START : SDL_GAMEPAD_BUTTON_BACK;
        SDL_GamepadButton second=order ? SDL_GAMEPAD_BUTTON_BACK : SDL_GAMEPAD_BUTTON_START;
        button(sticks[PRO],first,true);
        assert(!(guest_buttons(0)&(plus|minus)));
        button(sticks[PRO],second,true);
        button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,true);
        assert(SDL_SetJoystickVirtualAxis(sticks[PRO],SDL_GAMEPAD_AXIS_LEFTX,21000));
        pump();
        fill_state(&cpu,(uint8_t *)p1,0,false);
        fill_state(&cpu,(uint8_t *)p2,1,false);
        assert(!(p1[1]&(plus|minus)) && (int32_t)p1[2]==21000);
        assert(p2[1]&(1ull<<BIND_A));
        button(sticks[PRO],first,false);
        assert(!(guest_buttons(0)&(plus|minus)));
        button(sticks[PRO],second,false);
        assert(!(guest_buttons(0)&(plus|minus)));
        button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_SOUTH,false);
    }
    // Both physical events can arrive between game reads. Preserve a single, released guest tap.
    const SDL_GamepadButton physical[]={SDL_GAMEPAD_BUTTON_BACK,SDL_GAMEPAD_BUTTON_START};
    const uint64_t mapped[]={minus,plus};
    for(unsigned i=0;i<2;i++) {
        assert(SDL_SetJoystickVirtualButton(sticks[PRO],physical[i],true)); SDL_UpdateJoysticks();
        assert(SDL_SetJoystickVirtualButton(sticks[PRO],physical[i],false)); pump();
        assert(!(guest_buttons(1)&(plus|minus)));
        assert((guest_buttons(0)&(plus|minus))==mapped[i]);
        assert((guest_buttons(0)&(plus|minus))==mapped[i]); // MK8 polls repeatedly within a frame.
        ++presented_frames; assert((guest_buttons(0)&(plus|minus))==mapped[i]);
        ++presented_frames;
        assert(!(guest_buttons(0)&(plus|minus)));
    }
    // P2's two physical grips have independent stock Minus/Plus buttons.
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_BACK,true);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_START,true);
    assert(!(guest_buttons(0)&(plus|minus)) && (guest_buttons(1)&(plus|minus))==plus);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_START,true);
    assert((guest_buttons(1)&(plus|minus))==(plus|minus));
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_START,false);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_START,false);
    assert(!(guest_buttons(1)&(plus|minus)));
    // P4's standalone tap cannot complete P1's shortcut or deliver a pulse to the wrong seat.
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_START,true);
    assert(!(guest_buttons(0)&(plus|minus)) && !(guest_buttons(3)&(plus|minus)));
    button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_START,false);
    assert((guest_buttons(3)&(plus|minus))==plus);
    presented_frames+=2;
    assert(!(guest_buttons(3)&(plus|minus)));
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_BACK,false);
    assert((guest_buttons(0)&(plus|minus))==minus);
    presented_frames+=2;
    assert(!(guest_buttons(0)&(plus|minus)));
    // A non-pause action bound to Back must still act like a held button.
    int normal_a=g_bind_pad[BIND_A];
    g_bind_pad[BIND_A]=SDL_GAMEPAD_BUTTON_BACK;
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_BACK,true);
    assert((guest_buttons(0)&((1ull<<BIND_A)|minus))==(1ull<<BIND_A));
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_BACK,false);
    assert((guest_buttons(0)&((1ull<<BIND_A)|minus))==minus);
    presented_frames+=2; assert(!(guest_buttons(0)&minus));
    g_bind_pad[BIND_A]=normal_a;
    // Host UI may take or release focus while the physical Start button is held.
    for(unsigned captured_on_down=0;captured_on_down<2;captured_on_down++) {
        host_captures=captured_on_down; button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,true);
        host_captures=!captured_on_down; button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,false);
        host_captures=false; assert(!(guest_buttons(0)&(plus|minus)));
    }
    // Only Start/Back need to wait. A different physical button bound to Pause is immediate.
    int normal_plus=g_bind_pad[BIND_PLUS]; g_bind_pad[BIND_PLUS]=SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true);
    assert(guest_buttons(0)&plus);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
    assert(!(guest_buttons(0)&plus)); g_bind_pad[BIND_PLUS]=normal_plus;
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO], SDL_GAMEPAD_AXIS_LEFTX, 0));
    pump();
    shared_race=false;
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,true); assert(guest_buttons(0)&plus);
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_START,false); assert(!(guest_buttons(0)&plus));
}

static void shoulders(SDL_Joystick *j) {
    button(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);
    button(j,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true);
    button(j,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);
    button(j,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
}

static void check_tips(SDL_Joystick *generic, SDL_JoystickID spare) {
    struct Mk8ControlTips tips;
    mk8_control_tips(false,&tips);
    assert(tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_CONFIRM],"A"));
    SDL_Event key={0};
    key.type=SDL_EVENT_KEY_DOWN; key.key.scancode=SDL_SCANCODE_L;
    mk8_tip_event(&key,false); mk8_tip_sync(); mk8_control_tips(false,&tips);
    assert(tips.source==MK8_TIP_KEYBOARD && !strcmp(tips.label[MK8_TIP_CONFIRM],"K"));
    g_bind_key[BIND_A]=SDL_SCANCODE_TAB;
    mk8_tip_sync(); mk8_control_tips(false,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"Tab"));
    mk8_control_tips(true,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"Enter"));
    g_bind_pad[BIND_A]=PADB_LTRIGGER; mk8_tip_sync();
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO],SDL_GAMEPAD_AXIS_LEFT_TRIGGER,32767)); pump();
    mk8_control_tips(false,&tips);
    assert((mk8_controller_p1_navigation()&1) && tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_CONFIRM],"ZL"));
    mk8_tip_event(&key,false); mk8_tip_sync();
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO],SDL_GAMEPAD_AXIS_LEFT_TRIGGER,29000)); pump();
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_KEYBOARD); // Held trigger is not a new input.
    assert(SDL_SetJoystickVirtualAxis(sticks[PRO],SDL_GAMEPAD_AXIS_LEFT_TRIGGER,-32768)); pump();
    g_bind_pad[BIND_A]=SDL_GAMEPAD_BUTTON_EAST;
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,true); button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,false);
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_KEYBOARD); // P2 has no ownership here.
    button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,true); button(sticks[PRO],SDL_GAMEPAD_BUTTON_EAST,false);
    g_bind_pad[BIND_A]=SDL_GAMEPAD_BUTTON_WEST;
    mk8_tip_sync(); mk8_control_tips(false,&tips);
    assert(tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_CONFIRM],"Y"));
    mk8_control_tips(true,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"A"));
    g_bind_pad[BIND_A]=SDL_GAMEPAD_BUTTON_SOUTH; g_bind_pad[BIND_B]=SDL_GAMEPAD_BUTTON_EAST;
    mk8_tip_sync(); mk8_control_tips(true,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"B"));
    fixed_navigation=true;
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,true); button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_EAST,false);
    mk8_control_tips(true,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"B")); // P2 fallback doesn't own P1's hints.
    mk8_control_tips(false,&tips); assert(!strcmp(tips.label[MK8_TIP_CONFIRM],"B")); // Still the P1 Pro.
    fixed_navigation=false;
    g_bind_key[BIND_A]=SDL_SCANCODE_K; g_bind_pad[BIND_A]=SDL_GAMEPAD_BUTTON_EAST; g_bind_pad[BIND_B]=SDL_GAMEPAD_BUTTON_SOUTH;
    SDL_JoystickID layout[4][2]={{spare,0},{ids[PRO],0},{ids[PAIR_L],ids[PAIR_R]},{ids[SOLO_L],0}};
    assert(mk8_assign_layout(layout,1,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_GENERIC && !strcmp(tips.label[MK8_TIP_CONFIRM],"East"));
    mk8_tip_icon_choice(PROMPTS_XBOX);
    mk8_tip_sync(); mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_XBOX && !strcmp(tips.label[MK8_TIP_CONFIRM],"B"));
    mk8_tip_icon_choice(PROMPTS_PLAYSTATION);
    mk8_tip_sync(); mk8_control_tips(true,&tips); assert(tips.source==MK8_TIP_PLAYSTATION && !strcmp(tips.label[MK8_TIP_CONFIRM],"Circle"));
    mk8_tip_icon_choice(PROMPTS_AUTO);
    mk8_tip_event(&key,false); mk8_tip_sync(); mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_GENERIC);
    // SDL virtual remapping changes a device's published label family without changing its attachment ID.
    const SDL_GamepadType types[]={SDL_GAMEPAD_TYPE_XBOXONE,SDL_GAMEPAD_TYPE_PS5};
    const char *names[]={"B","Circle"};
    char *original=SDL_GetGamepadMappingForID(spare); assert(original);
    for (int i=0;i<2;i++) {
        char mapping[256],guid[33];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(spare),guid,sizeof guid);
        snprintf(mapping,sizeof mapping,"%s,Family test,a:b0,b:b1,x:b2,y:b3,type:%s,",guid,SDL_GetGamepadStringForType(types[i]));
        assert(SDL_SetGamepadMapping(spare,mapping)); pump();
        button(generic,SDL_GAMEPAD_BUTTON_EAST,true); button(generic,SDL_GAMEPAD_BUTTON_EAST,false);
        mk8_control_tips(false,&tips);
        assert(tips.source==(i ? MK8_TIP_PLAYSTATION : MK8_TIP_XBOX) && !strcmp(tips.label[MK8_TIP_CONFIRM],names[i]));
    }
    assert(SDL_SetGamepadMapping(spare,original)); SDL_free(original); pump();
    key.key.scancode=SDL_SCANCODE_F1; mk8_tip_event(&key,false); mk8_tip_sync();
    mk8_control_tips(true,&tips); assert(tips.source==MK8_TIP_GENERIC);
    layout[0][0]=0;
    assert(mk8_assign_layout(layout,1,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_NONE && !*tips.label[MK8_TIP_BACK]);
    mk8_control_tips(true,&tips); assert(tips.source==MK8_TIP_KEYBOARD && !strcmp(tips.label[MK8_TIP_BACK],"Esc"));
    assert(mk8_assign_layout(layout,0,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_KEYBOARD);
    mk8_tip_icon_choice(PROMPTS_XBOX);
    mk8_tip_sync(); mk8_control_tips(true,&tips); assert(tips.source==MK8_TIP_KEYBOARD && !strcmp(tips.label[MK8_TIP_CONFIRM],"Enter"));
    layout[0][0]=ids[SOLO_R];
    assert(mk8_assign_layout(layout,1,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_CONFIRM],"X"));
    assert(!*tips.label[MK8_TIP_CLEAR_ALL] && !strcmp(tips.label[MK8_TIP_PLUS],"Plus"));
    mk8_control_tips(true,&tips); assert(!strcmp(tips.label[MK8_TIP_DOWN],"Stick down"));
    layout[0][0]=ids[PAIR_L]; layout[2][0]=layout[2][1]=0;
    assert(mk8_assign_layout(layout,1,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(!*tips.label[MK8_TIP_CLEAR_ALL] && !strcmp(tips.label[MK8_TIP_PLUS],"Minus"));
    g_bind_pad[BIND_MINUS]=SDL_GAMEPAD_BUTTON_LEFT_PADDLE2; mk8_tip_sync(); mk8_control_tips(false,&tips);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,true);
    assert((mk8_controller_p1_navigation()&(1ull<<BIND_MINUS)) && !strcmp(tips.label[MK8_TIP_CLEAR_ALL],"ZL"));
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,false); g_bind_pad[BIND_MINUS]=SDL_GAMEPAD_BUTTON_BACK;
    mk8_tip_icon_choice(PROMPTS_AUTO);
    layout[0][1]=ids[PAIR_R];
    assert(mk8_assign_layout(layout,1,false)); mk8_tip_sync();
    mk8_control_tips(false,&tips); assert(!strcmp(tips.label[MK8_TIP_CLEAR_ALL],"Minus"));
    fixed_navigation=true;
    for(int side=0;side<2;side++) {
        SDL_Joystick *stick=sticks[side ? PAIR_R : PAIR_L];
        assert(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTY,26000)); pump();
        mk8_control_tips(true,&tips);
        assert(!strcmp(tips.label[MK8_TIP_DOWN],side ? "R stick right" : "L stick left"));
        // F1 samples raw +Y (down). In the upright pair this is physical left on L or physical right on R.
        assert((side ? pad_rx : pad_lx)==(side ? 26000 : -26000));
        button(stick,SDL_GAMEPAD_BUTTON_EAST,true);
        assert(SDL_GetGamepadButton(SDL_GetGamepadFromID(SDL_GetJoystickID(stick)),SDL_GAMEPAD_BUTTON_EAST));
        assert(!strcmp(tips.label[MK8_TIP_CONFIRM],side ? "X" : "Down"));
        button(stick,SDL_GAMEPAD_BUTTON_EAST,false);
        assert(SDL_SetJoystickVirtualAxis(stick,SDL_GAMEPAD_AXIS_LEFTY,0)); pump();
    }
    fixed_navigation=false;
    assert(mk8_assign_gamepads(NULL)); mk8_tip_sync();
}

static void check_live_editor(void) {
    const SDL_JoystickID old_p4=SDL_GetGamepadID(extra_pad[2]);
    assert(!mk8_explicit_slots && mk8_assigned_keyboard()==0);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED && !mk8_explicit_slots);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_SWAP,0,1)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    assert(mk8_assigned_keyboard()==0 && SDL_GetGamepadID(pad)==ids[PAIR_L]);
    assert(mk8_assign_gamepads(NULL));
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD; d.vendor_id=0x1209; d.product_id=1; d.name="Spare USB pad";
    d.naxes=SDL_GAMEPAD_AXIS_COUNT; d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    SDL_JoystickID spare=SDL_AttachVirtualJoystick(&d);
    SDL_Joystick *usb=SDL_OpenJoystick(spare); assert(usb); pump();
    assert(!SDL_GetGamepadFromID(spare));
    check_tips(usb,spare);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(SDL_GetGamepadFromID(spare));
    assert(edit(MK8_EDIT_CLEAR,1,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_FAILED);
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L]);
    button(usb,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);
    assert(edit(MK8_EDIT_ASSIGN,1,0)==MK8_EDIT_LISTEN);
    button(usb,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true);
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_LISTEN);
    button(usb,SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);
    button(usb,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
    shoulders(sticks[SOLO_L]); // Already belongs to P4, which isn't in this two-row popup.
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_LISTEN);
    shoulders(usb);
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY);
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L]);
    assert(edit(MK8_EDIT_CANCEL,0,0)==MK8_EDIT_CLOSED);
    assert(!SDL_GetGamepadFromID(spare) && SDL_GetGamepadID(extra_pad[2])==old_p4);

    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_ASSIGN,0,0)==MK8_EDIT_LISTEN);
    shoulders(usb);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED && SDL_GetGamepadID(pad)==spare);
    assert(mk8_assigned_keyboard()==-1);
    assert(mk8_assign_gamepads(NULL));
    assert(edit(MK8_EDIT_BEGIN,4,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_CLEAR_ALL,0,0)==MK8_EDIT_LISTEN);
    shoulders(usb); // First to join takes P1, regardless of USB/SDL enumeration order.
    int row=-1;
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==1);
    // An interleaved second player must not complete another player's mixed pair.
    for (int reverse=0; reverse<2; reverse++) {
        enum Device devices[4]={PAIR_L,SOLO_R,SOLO_L,PAIR_R};
        for (int i=0;i<4;i++) {
            enum Device dev=devices[reverse ? 3-i : i];
            button(sticks[dev],dev==PAIR_L || dev==SOLO_L ? SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 : SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true);
        }
        assert(mk8_edit_view(NULL,&row)==MK8_EDIT_AMBIGUOUS && row==1);
        for (int i=0;i<4;i++)
            button(sticks[devices[i]],i%2==0 ? SDL_GAMEPAD_BUTTON_LEFT_PADDLE1 : SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false);
    }
    // A released first grip cannot pair with a much later press on another person's grip.
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_NEED_LEFT && row==1);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==1);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,true);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_NEED_RIGHT && row==1);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,true);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_PAIR_HELD && row==1);
    button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,false);
    button(sticks[PAIR_R],SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,false);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==2);
    shoulders(sticks[SOLO_R]);
    assert(mk8_edit_view(NULL,&row)==MK8_EDIT_LISTEN && row==3);
    shoulders(sticks[SOLO_L]);
    assert(mk8_edit_view(NULL,NULL)==MK8_EDIT_READY);
    assert(SDL_GetGamepadID(pad)==ids[PRO]);
    struct Mk8ControlTips tips;
    mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_NINTENDO);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    assert(SDL_GetGamepadID(pad)==spare && mk8_assigned_keyboard()==-1);
    mk8_tip_sync(); mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_GENERIC);
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[1])==ids[PAIR_R]);
    assert(SDL_GetGamepadID(extra_pad[1])==ids[SOLO_R] && SDL_GetGamepadID(extra_pad[2])==ids[SOLO_L]);
    check_pulse(1,0,PAIR_L); check_pulse(1,1,PAIR_R); check_motion(1,PAIR_R,true);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_KEYBOARD,1,0)==MK8_EDIT_READY && mk8_assigned_keyboard()==-1);
    assert(edit(MK8_EDIT_SWAP,0,1)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    assert(!pad && mk8_assigned_keyboard()==0 && SDL_GetGamepadID(extra_pad[0])==spare);
    mk8_tip_sync(); mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_KEYBOARD);
    assert(SDL_GetGamepadID(extra_pad[1])==ids[SOLO_R] && SDL_GetGamepadID(extra_pad[2])==ids[SOLO_L]);
    // A two-row edit cannot take the physical keyboard away from a hidden player.
    assert(edit(MK8_EDIT_BEGIN,4,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_KEYBOARD,3,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_ASSIGN,0,0)==MK8_EDIT_LISTEN);
    shoulders(sticks[PRO]);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED && mk8_assigned_keyboard()==3);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(edit(MK8_EDIT_KEYBOARD,1,0)==MK8_EDIT_FAILED);
    assert(edit(MK8_EDIT_CLEAR_ALL,0,0)==MK8_EDIT_LISTEN);
    shoulders(sticks[PRO]);
    shoulders(usb);
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED);
    assert(mk8_assigned_keyboard()==3 && !extra_pad[2] && SDL_GetGamepadID(extra_pad[1])==ids[SOLO_R]);
    assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
    assert(SDL_DetachVirtualJoystick(spare)); pump();
    assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_FAILED);
    assert(edit(MK8_EDIT_CANCEL,0,0)==MK8_EDIT_CLOSED && !extra_pad[0]);
    SDL_CloseJoystick(usb);
    assert(mk8_assign_gamepads(NULL));
}

#include "title_input.inc"

// The production Steam bridge exposes both simulated GameInput devices through SDL.
void check_gameinput_title(SDL_JoystickID first, SDL_JoystickID second, uint32_t *buttons) {
    pump();
    title_ready(true);
    *buttons=4; pump(); // GameInput A
    *buttons=0; pump();
    title_accepted();
    assert(title_pad_seat(second)==0 && title_pad_seat(first)>0);
    title_ready(true); title_key(SDL_SCANCODE_K,false); title_accepted();
    assert(mk8_assigned_keyboard()==0 && title_pad_seat(second)>0);
}

int steam_gamepads_title_test(void);
int main(int argc, char **argv) {
    style_events[nstyle_events++]=&style_event;
    assert(argc == 3);
    assert(SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, "0"));
    assert(SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS, "0"));
    assert(SDL_Init(SDL_INIT_GAMEPAD));
    int count;
    SDL_JoystickID *physical = SDL_GetJoysticks(&count);
    assert(physical && !count);
    SDL_free(physical);
    if(!strncmp(argv[2],"all-reconnect",13)) {
        check_all_reconnect(argv[2]);
        SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"race-disconnect")) {
        check_race_disconnect(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"hotplug-open-failure")) {
        check_hotplug_open_failure(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"prerace-applet")) {
        check_prerace_applet(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"prerace-p1-only")) {
        check_prerace_p1_only(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"keyboard-prerace-applet")) {
        check_keyboard_prerace(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"prerace-sparse-seats")) {
        check_prerace_sparse_seats(); SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2],"prerace-p1-replacement")) {
        check_prerace_p1_replacement(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"steam-title")) return steam_gamepads_title_test();
    if(!strcmp(argv[2],"title")) {
        check_title_input(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"title-hotplug")) {
        check_title_hotplug(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"title-reconnect")) {
        check_title_reconnect(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"right-reconnect")) {
        check_right_reconnect(!strcmp(argv[1],"early-add")); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"title-identical")) {
        check_title_identical(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"title-manual")) {
        check_title_manual_layout(); SDL_Quit(); return 0;
    }
    if(!strcmp(argv[2],"title-blocked") || !strcmp(argv[2],"title-script")) {
        scripted_input=!strcmp(argv[2],"title-script");
        attach(PRO); attach(PAIR_L); pump();
        SDL_Gamepad *initial=pad; int keyboard=mk8_assigned_keyboard();
        title_ready(true); title_key(SDL_SCANCODE_K,false);
        button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,true);
        button(sticks[PAIR_L],SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,false);
        assert(pad==initial); assert(mk8_assigned_keyboard()==keyboard); assert(!(guest_buttons(0)&(1ull<<BIND_A)));
        title_ready(false);
        SDL_CloseJoystick(sticks[PAIR_L]); assert(SDL_DetachVirtualJoystick(ids[PAIR_L]));
        SDL_CloseJoystick(sticks[PRO]); assert(SDL_DetachVirtualJoystick(ids[PRO])); pump(); SDL_Quit(); return 0;
    }
    uint16_t empty[4][80];
    mk8_controller_names(empty);
    assert(!memcmp(empty[0], u"Keyboard", sizeof(u"Keyboard")));
    for (unsigned i=1; i<4; i++) assert(!memcmp(empty[i], u"Unassigned", sizeof(u"Unassigned")));
    for (int i = 0; i < DEVICE_COUNT; ++i) {
        if (i == 2 && !strcmp(argv[2],"editor")) {
            pump();
            assert(edit(MK8_EDIT_BEGIN,2,0)==MK8_EDIT_READY);
        }
        attach(!strcmp(argv[1], "reverse") ? DEVICE_COUNT - 1 - i : i);
        if (i == 2 && !strcmp(argv[2],"editor")) {
            pump();
            assert(!extra_pad[1]); // Observer opened it, but a live player cannot change mid-dialog.
            assert(edit(MK8_EDIT_APPLY,0,0)==MK8_EDIT_APPLIED && !mk8_explicit_slots);
            assert(SDL_GetGamepadID(extra_pad[1])==ids[PAIR_R]); // No-op Apply resumes normal discovery.
        }
    }
    pump();
    SDL_JoystickID layout[4][2] = {
        {ids[PRO], 0}, {ids[PAIR_L], ids[PAIR_R]}, {ids[SOLO_L], 0}, {ids[SOLO_R], 0}
    };
    if (!strcmp(argv[2],"editor")) {
        assert(!SDL_getenv("MK8_GAMEPAD_LAYOUT"));
        check_live_editor();
        for (int i=0;i<DEVICE_COUNT;i++) {
            SDL_CloseJoystick(sticks[i]);
            assert(SDL_DetachVirtualJoystick(ids[i]));
        }
        pump();
        struct Mk8ControlTips tips;
        mk8_control_tips(false,&tips); assert(tips.source==MK8_TIP_KEYBOARD);
        SDL_Quit(); return 0;
    }
    if (!strcmp(argv[2], "automatic")) {
        assert(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, false));
        assert(SDL_GetGamepadID(pad) == ids[PRO]);
        assert(SDL_GetGamepadID(extra_pad[2]) == ids[SOLO_L]);
        assert(!SDL_GetGamepadFromID(ids[SOLO_R]));
        assert(mk8_assign_gamepads(layout));
    }
    assert(extra_connected(0) && extra_connected(1) && extra_connected(2));
    check_popup_names();
    check_popup_owner();
    check_blocked_settings_chord();
    if(!strcmp(argv[2],"mixed")) {
        mk8_title_input_active(true); pump();
        button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true);
        assert(SDL_GetGamepadID(pad)==ids[PRO] && mk8_partner[1] && !(guest_buttons(0)&(1ull<<BIND_A)));
        button(sticks[SOLO_R],SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,false);
        mk8_title_input_active(false); pump();
    }
    const int buttons[] = {SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST,
                           SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST};
    for (int i = 0; i < DEVICE_COUNT; ++i) {
        assert(SDL_SetJoystickVirtualButton(sticks[i], buttons[i], true));
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_LEFTX, (i + 1) * 1000));
        assert(SDL_SetJoystickVirtualAxis(sticks[i], SDL_GAMEPAD_AXIS_LEFTY, (i + 1) * 100));
    }
    pump();
    assert(pad_buttons == (1u << BIND_X) && pad_lx == 1000 && pad_ly == -100);
    assert(extra_buttons[0] == ((1u << BIND_UP) | (1u << BIND_A)));
    assert(extra_lx[0] == -200 && extra_ly[0] == -2000);
    assert(extra_rx[0] == 300 && extra_ry[0] == 3000);
    assert(extra_buttons[1] == (1u << BIND_A) && extra_lx[1] == 4000 && extra_ly[1] == -400);
    assert(extra_buttons[2] == (1u << BIND_Y) && extra_lx[2] == 5000 && extra_ly[2] == -500);
    check_motion(0, PRO, false);
    check_motion(1, PAIR_R, true);
    check_motion(2, SOLO_L, false);
    check_motion(3, SOLO_R, false);
    check_pulse(0, 1, PRO);
    check_pulse(1, 0, PAIR_L);
    check_pulse(1, 1, PAIR_R);
    check_pulse(2, 1, SOLO_L);
    check_pulse(3, 0, SOLO_R);
    check_batched_pair();

    layout[2][0] = ids[PAIR_R];
    assert(!mk8_assign_gamepads(layout));
    assert(extra_buttons[1] == (1u << BIND_A));
    check_pulse(1, 1, PAIR_R);
    layout[2][0] = ids[SOLO_R];
    layout[3][0] = ids[SOLO_L];
    pulse(2, 0, 1.f);
    expect_buzz(SOLO_L);
    pulse(0, 1, 1.f);
    unsigned before_swap = style_notifications;
    assert(mk8_assign_gamepads(layout));
    assert(style_notifications == before_swap + 1);
    uint16_t names[4][80];
    mk8_controller_names(names);
    assert(!memcmp(names[2], u"Nintendo Switch Joy-Con (R)", sizeof(u"Nintendo Switch Joy-Con (R)")));
    assert(!memcmp(names[3], u"Nintendo Switch Joy-Con (L)", sizeof(u"Nintendo Switch Joy-Con (L)")));
    assert(extra_connected(0) && extra_connected(1) && extra_connected(2));
    expect_buzz(PRO);
    pulse(0, 1, 0.f);
    expect_buzz(DEVICE_COUNT);
    assert(extra_buttons[1] == (1u << BIND_Y) && extra_lx[1] == 5000);
    assert(extra_buttons[2] == (1u << BIND_A) && extra_lx[2] == 4000);
    check_motion(2, SOLO_R, false);
    check_pulse(2, 0, SOLO_R);

    pulse(1, 1, 1.f);
    expect_buzz(PAIR_R);
    assert(SDL_DetachVirtualJoystick(ids[PAIR_L]));
    pump();
    assert(!extra_pad[0] && !extra_buttons[0] && !extra_lx[0]);
    mk8_controller_names(names);
    assert(!memcmp(names[1], u"Unassigned", sizeof(u"Unassigned")));
    assert(!extra_connected(0) && extra_connected(1) && extra_connected(2));
    expect_buzz(DEVICE_COUNT);
    SDL_CloseJoystick(sticks[PAIR_L]);
    attach(PAIR_L);
    pump();
    assert(SDL_GetGamepadID(extra_pad[0])==ids[PAIR_L] && SDL_GetGamepadID(mk8_partner[1])==ids[PAIR_R]);
    check_motion(1, PAIR_R, true);
    check_pulse(1, 0, PAIR_L); check_pulse(1, 1, PAIR_R);
    check_pulse(3, 0, SOLO_L);
    assert(mk8_assign_gamepads(NULL));
    assert(SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_COMBINE_JOY_CONS, false));
    for (int i = 0; i < DEVICE_COUNT; ++i) {
        SDL_CloseJoystick(sticks[i]);
        assert(SDL_DetachVirtualJoystick(ids[i]));
    }
    pump();
    mk8_controller_names(names);
    assert(!memcmp(names, empty, sizeof names));
    // Explicitly resetting the control layout releases the disconnected players' reservations.
    input_pads_reassign();
    // In automatic mode SDL can expose the two Joy-Cons as one named device.
    SDL_VirtualJoystickDesc pair;
    SDL_INIT_INTERFACE(&pair);
    pair.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    pair.vendor_id = 0x057e;
    pair.product_id = 0x2008;
    pair.name = "Joy-Con (L/R)";
    pair.naxes = SDL_GAMEPAD_AXIS_COUNT;
    pair.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    SDL_JoystickID combined = SDL_AttachVirtualJoystick(&pair);
    assert(combined);
    pump();
    mk8_controller_names(names);
    assert(!memcmp(names[0], u"Joy-Con (L/R)", sizeof(u"Joy-Con (L/R)")));
    assert(SDL_DetachVirtualJoystick(combined));
    pump();
    mk8_controller_names(names);
    assert(!memcmp(names, empty, sizeof names));
    check_keyboard_routing();
    SDL_Quit();
}
