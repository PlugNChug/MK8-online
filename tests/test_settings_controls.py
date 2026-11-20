"""Execute the fixed native-menu event handler, including release and capture routing."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'toolchain'))
from native import DEFAULT_COMPILER,compiler_env
from host import DEPS,prepare_host_sources


@unittest.skipUnless(DEFAULT_COMPILER.is_file(),'Native compiler required')
class SettingsControlsTests(unittest.TestCase):
    def test_hub_prototype_keeps_keyboard_and_controller_shortcuts(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',root/'host')
            source=(runtime/'ui/menu.mm').read_text(encoding='utf-8')
            start=source.index('bool menu_handle_event(const SDL_Event *e) {')
            event=source[start:source.index('\n}',start)+2]
            declaration='static bool mk8_hub_enabled();\n'
            if declaration in source[:start]: event=declaration+event
            hub=(runtime/'ui/mk8_hub.h').read_text(encoding='utf-8')
            start=hub.index('static bool mk8_hub_enabled() {')
            predicate=hub[start:hub.index('\n}',start)+2]
            (root/'shortcuts.cpp').write_text(r'''
#include <SDL3/SDL.h>
#include "imgui.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#define CHECK(condition) do { if(!(condition)) { std::fprintf(stderr,"shortcut check failed: %s\n",#condition); std::exit(1); } } while(0)
static std::atomic<bool> g_open,g_last_pad,g_text_busy,g_want_fullscreen_apply;
static std::atomic<int> g_kbd;
static float g_pixel_density=1;
static struct { bool fullscreen; } g_settings;
static int native_requests;
struct QEvent { enum { KEY,ANALOG,MOUSE_POS,MOUSE_BUTTON,WHEEL,TEXT } kind; ImGuiKey key; bool down; float value,x,y; int button; bool touch; char text[32]; };
static void push(const QEvent &) {}
static bool capturing() { return g_open || g_kbd==1; }
static void set_open(bool value) { g_open=value; }
static bool bind_capture_event(const SDL_Event *) { return false; }
static bool mk8_settings_event(const SDL_Event *,bool) { return false; }
static bool mk8_settings_page_toggle() {
    if(std::strcmp(std::getenv("MK8_SETTINGS_EXPERIMENT"),"1")) return false;
    native_requests++; return true;
}
static void settings_save() {}
static void mk8_gfx_fullscreen_request(int,int) {}
static ImGuiKey key_from_scancode(SDL_Scancode) { return ImGuiKey_None; }
static ImGuiKey key_from_pad(int) { return ImGuiKey_None; }
static size_t strlcpy(char *out,const char *in,size_t count) { size_t n=std::strlen(in); if(count)std::snprintf(out,count,"%s",in);return n; }
''' + event + '\n' + predicate + r'''
static void key(SDL_Keymod modifiers=0) {
    SDL_Event event={};event.type=SDL_EVENT_KEY_DOWN;event.key.scancode=SDL_SCANCODE_F1;event.key.mod=modifiers;
    CHECK(menu_handle_event(&event));
}
static bool button(Uint32 type,Uint8 value) {
    SDL_Event event={};event.type=type;event.gbutton.button=value;return menu_handle_event(&event);
}
int main() {
    for(const char *native : {"0","1"}) for(const char *hub : {"0","1"}) {
        CHECK(!_putenv_s("MK8_SETTINGS_EXPERIMENT",native));CHECK(!_putenv_s("MK8_MENU_PROTOTYPE",hub));
        bool intercept=*native=='1' && *hub=='0';
        g_open=false;native_requests=0;key();
        CHECK(g_open==!intercept && native_requests==(int)intercept);
        if(g_open) { key();CHECK(!g_open); }
        g_open=false;native_requests=0;key(SDL_KMOD_SHIFT);CHECK(g_open && !native_requests);
        g_open=false;native_requests=0;
        button(SDL_EVENT_GAMEPAD_BUTTON_DOWN,SDL_GAMEPAD_BUTTON_BACK);
        CHECK(button(SDL_EVENT_GAMEPAD_BUTTON_DOWN,SDL_GAMEPAD_BUTTON_START));
        CHECK(g_open==!intercept && native_requests==(int)intercept);
        button(SDL_EVENT_GAMEPAD_BUTTON_UP,SDL_GAMEPAD_BUTTON_BACK);
        button(SDL_EVENT_GAMEPAD_BUTTON_UP,SDL_GAMEPAD_BUTTON_START);
        g_open=false;native_requests=0;
        CHECK(button(SDL_EVENT_GAMEPAD_BUTTON_DOWN,SDL_GAMEPAD_BUTTON_GUIDE));
        CHECK(g_open==!intercept && native_requests==(int)intercept);
        button(SDL_EVENT_GAMEPAD_BUTTON_UP,SDL_GAMEPAD_BUTTON_GUIDE);
    }
}
''',encoding='utf-8')
            exe=root/'shortcuts.exe'
            result=subprocess.run([str(DEFAULT_COMPILER),'c++','-target','x86_64-windows-gnu','-O2',
                '-I'+str(DEPS/'include'),'-I'+str(ROOT/'upstream/smm2recomp/recomp/third_party/imgui'),
                str(root/'shortcuts.cpp'),'-o',str(exe)],env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])

    def test_real_ui_loop_services_controller_state_under_continuous_events(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            runtime=prepare_host_sources(ROOT/'upstream/smm2recomp/recomp',root/'host')
            source=(runtime/'nvn/vulkan/device.c').read_text(encoding='utf-8')
            start=source.index('static void mk8_ultrawide_update_drawable(void) {')
            loop=source.index('int ui_run(void) {',start)
            (root/'settings_ui_loop.inc').write_text(source[start:source.index('\n}',loop)+2],encoding='utf-8')
            exe=root/'loop.exe'
            deps=DEPS
            result=subprocess.run([str(DEFAULT_COMPILER),'c++','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
                '-I'+str(root),'-I'+str(deps/'include'),'-I'+str(ROOT/'runtime/mk8/ui'),
                str(ROOT/'tests/native_settings_event_loop.cpp'),str(deps/'lib/libSDL3.dll.a'),'-o',str(exe)],
                env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])
            result=subprocess.run([str(exe)],env=dict(os.environ,PATH=str(deps/'bin')+os.pathsep+os.environ.get('PATH','')),
                                  capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])

    def test_navigation_survives_remapping_and_capture_takes_priority(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder)
            (root/'test.cpp').write_text(r'''
#include <SDL3/SDL.h>
#include <cassert>
#include <cstdint>
static bool active,editing,capture,swapped,blocked;
static uint64_t held;
static int wheel,typed;
extern "C" bool mk8_settings_page_active(void) { return active; }
extern "C" void mk8_settings_page_fixed(uint64_t b) { held=b; }
extern "C" void mk8_settings_page_wheel(int n) { wheel+=n; }
extern "C" void mk8_settings_values_sync(void) {}
extern "C" bool mk8_settings_page_key(int key) { if(!editing)return false; typed=key; return true; }
extern "C" bool input_user_blocked(const SDL_Event *) { return blocked; }
extern "C" bool bind_capture_active(int *,int *,int *) { return capture; }
static bool menu_ab_swapped(void) { return swapped; }
static bool bind_capture_event(const SDL_Event *e) { if(!capture)return false; if(e->type==SDL_EVENT_KEY_DOWN && e->key.scancode==SDL_SCANCODE_ESCAPE)capture=false; return true; }
#include "settings_controls.inc"
static SDL_Event key(Uint32 type,SDL_Scancode scan,SDL_Keycode value=0) { SDL_Event e={}; e.type=type; e.key.scancode=scan; e.key.key=value; return e; }
static void frame() {
    SDL_Event e;
    SDL_UpdateJoysticks();
    while(SDL_PollEvent(&e)) mk8_settings_event(&e,false);
    mk8_settings_poll(false);
}
int main() {
    SDL_Event e=key(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_RETURN);
    assert(!mk8_settings_event(&e,false) && !held);
    active=true; assert(mk8_settings_event(&e,false) && held==1);
    e.type=SDL_EVENT_KEY_UP; assert(!mk8_settings_event(&e,false) && !held);
    e=key(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_DOWN); assert(mk8_settings_event(&e,false) && held==(1ull<<15));
    assert(!mk8_settings_event(&e,true) && !held);
    e={};e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;e.gbutton.which=0x7E57u;e.gbutton.button=SDL_GAMEPAD_BUTTON_EAST;
    assert(mk8_settings_event(&e,false) && held==1);
    e.type=SDL_EVENT_GAMEPAD_BUTTON_UP; assert(!mk8_settings_event(&e,false) && !held);
    swapped=true;e.type=SDL_EVENT_GAMEPAD_BUTTON_DOWN;assert(mk8_settings_event(&e,false) && held==2);
    e.type=SDL_EVENT_GAMEPAD_BUTTON_UP;mk8_settings_event(&e,false);
    e={};e.type=SDL_EVENT_GAMEPAD_AXIS_MOTION;e.gaxis.which=0x7E57u;e.gaxis.axis=SDL_GAMEPAD_AXIS_LEFTY;e.gaxis.value=30000;
    assert(!mk8_settings_event(&e,false) && held==(1ull<<15));
    e.gaxis.value=0;assert(!mk8_settings_event(&e,false) && !held);
    editing=true;e=key(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_8,SDLK_8);
    assert(mk8_settings_event(&e,false) && typed=='8' && !held);
    e=key(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_ESCAPE);assert(mk8_settings_event(&e,false) && typed==27 && !held);
    editing=false;capture=true;assert(mk8_settings_event(&e,false) && !capture && !held);
    assert(mk8_settings_event(&e,false) && held==2);
    e.type=SDL_EVENT_KEY_UP;mk8_settings_event(&e,false);
    e=key(SDL_EVENT_KEY_DOWN,SDL_SCANCODE_F1);assert(!mk8_settings_event(&e,false));
    e={};e.type=SDL_EVENT_MOUSE_WHEEL;e.wheel.y=1;assert(mk8_settings_event(&e,false) && wheel==-1);

    assert(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_VirtualJoystickDesc d; SDL_INIT_INTERFACE(&d);
    d.type=SDL_JOYSTICK_TYPE_GAMEPAD;d.naxes=SDL_GAMEPAD_AXIS_COUNT;d.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    d.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;d.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    d.name="Settings live state test";
    SDL_JoystickID ids[2];SDL_Joystick *sticks[2];SDL_Gamepad *pads[2];
    for(int i=0;i<2;i++) {
        ids[i]=SDL_AttachVirtualJoystick(&d);assert(ids[i]);
        sticks[i]=SDL_OpenJoystick(ids[i]);assert(sticks[i]);
        pads[i]=SDL_OpenGamepad(ids[i]);assert(pads[i]);
    }
    swapped=false;frame();
    // No guest P1 poll; releases can be absent while SDL handles are reassigned.
    for(int i=0;i<24;i++) {
        assert(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_DPAD_RIGHT,true));
        assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTY,30000));frame();
        assert(held==((1ull<<14)|(1ull<<15)));
        SDL_CloseGamepad(pads[0]);pads[0]=SDL_OpenGamepad(ids[0]);assert(pads[0]);frame();
        SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_BUTTON_UP,false);
        SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_AXIS_MOTION,false);
        assert(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_DPAD_RIGHT,false));
        assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTY,0));frame();
        assert(!held);
        SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_BUTTON_UP,true);
        SDL_SetEventEnabled(SDL_EVENT_GAMEPAD_AXIS_MOTION,true);
        assert(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_EAST,true));frame();assert(held==1);
        assert(SDL_SetJoystickVirtualButton(sticks[0],SDL_GAMEPAD_BUTTON_EAST,false));frame();assert(!held);
    }
    assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTX,30000));frame();assert(held==(1ull<<14));
    assert(SDL_SetJoystickVirtualAxis(sticks[1],SDL_GAMEPAD_AXIS_LEFTY,-30000));frame();
    assert(held==((1ull<<14)|(1ull<<13)));
    assert(SDL_SetJoystickVirtualAxis(sticks[1],SDL_GAMEPAD_AXIS_LEFTY,0));frame();assert(held==(1ull<<14));
    capture=true;frame();assert(!held);capture=false;frame();assert(!held);
    assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTX,0));frame();assert(!held);
    assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTX,30000));frame();assert(held==(1ull<<14));
    blocked=true;frame();assert(!held);blocked=false;frame();assert(held==(1ull<<14));
    e={};e.type=SDL_EVENT_WINDOW_FOCUS_LOST;mk8_settings_event(&e,false);mk8_settings_poll(false);assert(!held);
    e.type=SDL_EVENT_WINDOW_FOCUS_GAINED;mk8_settings_event(&e,false);mk8_settings_poll(false);assert(!held);
    assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTX,0));frame();
    assert(SDL_SetJoystickVirtualAxis(sticks[0],SDL_GAMEPAD_AXIS_LEFTX,30000));frame();assert(held==(1ull<<14));
    SDL_CloseGamepad(pads[0]);SDL_CloseJoystick(sticks[0]);SDL_DetachVirtualJoystick(ids[0]);
    mk8_settings_poll(false);assert(!held); // Removal need not deliver a final centered-axis event.
    SDL_CloseGamepad(pads[1]);SDL_CloseJoystick(sticks[1]);SDL_DetachVirtualJoystick(ids[1]);SDL_Quit();
}
''',encoding='utf-8')
            exe=root/'test.exe'
            build=subprocess.run([str(DEFAULT_COMPILER),'c++','-target','x86_64-windows-gnu','-O2','-UNDEBUG',
                '-I'+str(DEPS/'include'),'-I'+str(ROOT/'runtime/mk8/ui'),
                str(root/'test.cpp'),str(DEPS/'lib/libSDL3.dll.a'),
                '-o',str(exe)],env=compiler_env(),capture_output=True,text=True)
            self.assertEqual(build.returncode,0,build.stderr[-3000:])
            env=dict(os.environ,PATH=str(DEPS/'bin')+os.pathsep+os.environ.get('PATH',''))
            result=subprocess.run([str(exe)],env=env,capture_output=True,text=True,timeout=15)
            self.assertEqual(result.returncode,0,result.stderr[-3000:])


if __name__=='__main__': unittest.main()
