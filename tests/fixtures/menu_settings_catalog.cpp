#include "imgui.h"
#include "imgui_internal.h"
#include "settings_catalog.h"
#include "mk8_upscaler_choices.h"
#include <SDL3/SDL.h>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

extern "C" {
const char *const g_tunic_colours[N_TUNIC_COLOURS]={};
void input_pads_reassign(void) {}
void mk8_settings_pads_changed(void) {}
const char *const g_heart_colours[N_HEART_COLOURS]={};
void rt_log(const char *,...) {}
int input_pad_family(void) { return 0; }
}
static bool main_menu,disabled,reset_buttons[2];
static int seen[MK8_MENU_FIELD_COUNT],cells[2][BIND_KEY_COUNT];
static const char *click;
extern "C" bool mk8_settings_values_main_menu(void) { return main_menu; }
static bool mk8_settings_overlay_style_active() { return false; }
static std::atomic<bool> g_want_fullscreen_apply;
static void mk8_gfx_fullscreen_request(int,bool) {}
static void renderer_get_scale(float *x,float *y) { *x=*y=1; }
static float output_scale_dynamic_factor() { return 1; }
struct AheadStatus { int state, items; };
static AheadStatus shader_ahead_status() { return {2,10}; }
static void opt_heading(const char *) {}
static void hint(const char *) {}
static void opt_begin(const char *,float) {}
static void opt_end() {}
static void opt_disable_next() { disabled=true; }

static int record(const char *label) {
    for(int id=0;id<MK8_MENU_FIELD_COUNT;id++) if(!strcmp(mk8_menu_option(id)->label,label)) {
        ++seen[id];
        bool active=!disabled && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
        if(id!=MK8_MENU_SHADERS) assert(active==mk8_menu_option_enabled(id,main_menu));
        disabled=false;
        return id;
    }
    assert(false && "Unexpected field outside the shared catalog");
    return -1;
}
static bool opt_choice(const char *label,int *,const char *const *,int,int,const char *,bool=true) { record(label); return false; }
static bool opt_toggle(const char *label,bool *field,bool,const char *) {
    int id=record(label);
    assert(id==MK8_MENU_FULLSCREEN || field==mk8_menu_option(id)->toggle);
    return false;
}
static bool opt_action(const char *label,const char *,const char *) { record(label); return false; }
static bool choice(const char *label,int *value,const char *const *,int n) {
    const Mk8MenuOption *o=mk8_menu_option(record(label));
    assert(value==o->choice && n==o->count); return false;
}

// Observe actual remap buttons and emulate a click without polling connected hardware.
namespace ControlsGui {
using namespace ImGui;
static bool Button(const char *label,const ImVec2 &size=ImVec2(0,0)) {
    const char *ids=strstr(label,"##");
    if(ids) {
        int dev,slot;
        assert(sscanf(ids,"##%d_%d",&dev,&slot)==2 && (dev==BIND_DEV_PAD || dev==BIND_DEV_KEY));
        assert(slot>=0 && slot<(dev==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT));
        ++cells[dev][slot];
    } else if(!strcmp(label,"Reset keyboard to defaults")) reset_buttons[BIND_DEV_KEY]=true;
    else if(!strcmp(label,"Reset controller to defaults")) reset_buttons[BIND_DEV_PAD]=true;
    ImGui::Button(label,size);
    return click && !strcmp(ids ? ids : label,click) && !(ImGui::GetCurrentContext()->CurrentItemFlags & ImGuiItemFlags_Disabled);
}
}
#define ImGui ControlsGui
#include "ui/controls_ui.mm"
#undef ImGui
#include "catalog_pages.inc"

static int frame(const char *button) {
    memset(seen,0,sizeof seen); memset(cells,0,sizeof cells); memset(reset_buttons,0,sizeof reset_buttons); click=button;
    ImGui::NewFrame();
    ImGui::SetNextWindowSize(ImVec2(1200,2000));
    ImGui::Begin("catalog");
    tab_display(); tab_about();
    ImGui::End(); ImGui::Render();
    for(int dev : {BIND_DEV_PAD,BIND_DEV_KEY}) {
        assert(reset_buttons[dev]);
        int count=dev==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT;
        for(int slot=0;slot<count;slot++) assert(cells[dev][slot]==1);
    }
    // Advanced Controls presents both device columns and both reset actions at once.
    seen[MK8_MENU_DEVICE]=seen[MK8_MENU_RESET]=1;
    for(int id=0;id<MK8_MENU_FIELD_COUNT;id++) if(mk8_menu_option(id)->group!=MK8_MENU_AUDIO && seen[id]!=1) {
        fprintf(stderr,"missing/duplicate field: %s\n",mk8_menu_option(id)->label); return 1;
    }
    return 0;
}
int main() {
    settings_load();
    ImGui::CreateContext();
    ImGuiIO &io=ImGui::GetIO(); io.IniFilename=nullptr; io.DisplaySize=ImVec2(1280,2160);
    unsigned char *pixels; int w,h; io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    if(frame(nullptr)) return 1;
    main_menu=true; g_settings.res_mode=RES_Q75;
    for(int dev : {BIND_DEV_PAD,BIND_DEV_KEY}) {
        char button[20]; snprintf(button,sizeof button,"##%d_%d",dev,BIND_B);
        if(frame(button)) return 1;
        int cd,cs,ms; assert(bind_capture_active(&cd,&cs,&ms) && cd==dev && cs==BIND_B);
        bind_capture_cancel();
        if(dev==BIND_DEV_PAD) g_bind_pad[BIND_B]=SDL_GAMEPAD_BUTTON_GUIDE; else g_bind_key[BIND_B]=SDL_SCANCODE_F10;
        assert(!bind_is_default(dev));
        if(frame(dev==BIND_DEV_PAD ? "Reset controller to defaults" : "Reset keyboard to defaults")) return 1;
        assert(bind_is_default(dev));
    }
    ImGui::DestroyContext();
}
