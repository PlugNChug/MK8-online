#include "imgui.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static bool no() { return false; }
static void nothing() {}
static void menu_notice(const char *, double) {}
static bool online, mii=true, rooms=true, closed;
static int account_frames;
static bool nes;
static bool nes_theme() { return nes; }
static auto ui_text_busy=no, mods_page_used_back=no;
static bool online_rows_available() { return online; }
extern "C" bool mk8_room_menu_available(void) { return rooms; }
extern "C" bool mk8_mii_menu_available(void) { return mii; }
extern "C" void mk8_open_room_menu(void) {}
extern "C" void mk8_open_mii_menu(void) {}
static auto tab_display=nothing, tab_audio=nothing, tab_about=nothing, tab_mods=nothing;
static void tab_online() { ++account_frames; }
static ImFont *g_font, *g_font_big;
static ImFont *menu_font() { return g_font; }
static ImFont *menu_font_big() { return g_font_big; }
static void set_open(bool value) { closed=!value; }
static void osk_draw(ImFont*,float) {}
static struct Palette { ImVec4 accent,muted,child; } palette;
static Palette &theme_palette() { return palette; }
#include "settings.h"
Settings g_settings;
const char *const mk8_joycon_modes[]={"Combined","Individual (horizontal)"};
const char *const g_language_names[N_LANGUAGES]={};
const char *const g_theme_names[N_THEMES]={"Default","NES","Native","SimpleLight","NESLight"};
static bool g_want_fullscreen_apply;

static const char *target;
static ImVec2 position;
namespace ObservedGui {
using namespace ImGui;
static void record(const char *label) {
    if (target && !strcmp(label,target)) {
        const ImVec2 a=GetItemRectMin(), b=GetItemRectMax();
        position=ImVec2((a.x+b.x)*.5f,(a.y+b.y)*.5f);
    }
}
static bool Button(const char *label, const ImVec2 &size={}) {
    bool pressed=ImGui::Button(label,size); record(label); return pressed;
}
static bool InvisibleButton(const char *label,const ImVec2 &size,ImGuiButtonFlags flags) {
    bool pressed=ImGui::InvisibleButton(label,size,flags); record(label); return pressed;
}
static bool Selectable(const char *label, bool selected, ImGuiSelectableFlags flags, const ImVec2 &size) {
    bool pressed=ImGui::Selectable(label,selected,flags,size); record(label); return pressed;
}
}
#define ImGui ObservedGui
#define GAME_TITLE "Test"
#include "ui/menu_exit.h"
bool mk8_collecting_rows() {return false;}
void mk8_collect_exit_feedback(bool,const char *) {abort();}
extern "C" bool mk8_settings_save_checked(void) { return true; }
static void opt_begin(const char *,float) {} static void opt_end() {}
static void opt_section(const char *) {} static void opt_disable_next() {}
static bool opt_action(const char *label,const char *,const char *,bool active=true) { return ImGui::Button(label) && active; }
static void rt_request_reboot(int) { abort(); }
union SDL_Event {int type;};
enum {SDL_EVENT_QUIT=1};
static void SDL_PushEvent(SDL_Event *) { abort(); }
#include "fixture_quit.inc"
#include "ui/mk8_hub.h"
#undef ImGui
static NxStatus state;
static Mk8Social social;
void nextendo_status(NxStatus *out) { *out=state; }
uint64_t nextendo_pid() { return 0; }
bool nextendo_username(char *,size_t) { return false; }
NetBackend net_backend() { return online ? NET_BACKEND_NEXTENDO : NET_BACKEND_OURS; }
static int refreshes;
void mk8_social_refresh() { ++refreshes; }
void mk8_social_snapshot(Mk8Social *out) { *out=social; }

#include "native_content_checks.inc"
#include "ui/menu_nes_row.h"

int main(int argc,char **) {
    ImGui::CreateContext();
    ImGuiIO &io=ImGui::GetIO();
    io.DisplaySize=ImVec2(1280,720);
    io.IniFilename=nullptr;
    g_font=g_font_big=io.Fonts->AddFontDefault();
    unsigned char *pixels; int width,height;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    if (argc>1) { check_native_pages(); check_quit_back(); ImGui::DestroyContext(); return 0; }
    ImGui::NewFrame();
    const char *aspect="ASPECT RATIO", *wide="Ultrawide (experimental)";
    const float fs=ImGui::GetFontSize();
    Mk8NesRow one=mk8_nes_row(aspect,wide,true,60*fs,1,3*fs);
    assert(one.label_top==one.value_top && one.height==3*fs);
    Mk8NesRow two=mk8_nes_row(aspect,wide,true,15*fs,1,3*fs);
    assert(two.value_top>=two.label_top+ImGui::CalcTextSize(aspect,nullptr,false,two.label_wrap).y);
    assert(two.value_top+ImGui::CalcTextSize(wide,nullptr,false,two.value_wrap).y<=two.height);
    ImGui::LogToTTY();
    mk8_hub_draw(1);
    ImGui::LogFinish();
    ImGui::Render();
    ImGui::DestroyContext();
}
