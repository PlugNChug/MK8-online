#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <strings.h>
#include <utility>
#include <vector>
#include "usermods.h"
#include "gamebanana.h"
#include "net/netcfg.h"
#include "net/nextendo.h"

#define GAME_TITLE "fixture"
static bool saved=true, back, changed, connection, disabled;
static int reboots, exits, closes, messages, signins, signouts, removals;
static std::string action, observed;
static bool pressed(const char *label) {
    observed += std::string(label)+"\n";
    bool match=action==label && !disabled; disabled=false;
    if (match) action.clear();
    return match;
}
static bool saw(const char *label) {return observed.find(label)!=std::string::npos;}
enum { ImGuiKey_GamepadFaceRight, ImGuiKey_Escape };
namespace ImGui {
static bool IsKeyPressed(int key,bool) {return back && key==ImGuiKey_Escape;}
static bool IsItemClicked() { return false; }
static unsigned scope;
static void PushID(const char *id) {scope=(unsigned)std::hash<std::string>{}(id);}
static void PopID() {scope=0;}
static void TextWrapped(const char *, const char *msg) {assert(*msg); ++messages;}
}
#include "ui/menu_exit.h"
extern "C" bool mk8_settings_save_checked(void) {return saved;}
static void opt_begin(const char *,float) {} static void opt_end() {}
static void opt_section(const char *l) {
    if (mk8_collecting_rows()) mk8_collect_section(l);
    else observed+=std::string(l)+"\n";
}
static void opt_heading(const char *l) { opt_section(l); }
static void opt_disable_next() { disabled=true; }
static int step(const char *l,const char *v,const char *desc,bool adjust=false,bool named=true) {
    if (!mk8_collecting_rows()) return pressed(l) ? 2 : 0;
    bool enabled=!disabled; disabled=false;
    return mk8_collect_row(ImGui::scope,l,v,desc,enabled,adjust,named);
}
static bool opt_action(const char *l,const char *v,const char *d,bool named=true) {return step(l,v,d,false,named)==2;}
static bool opt_button(const char *l,const char *d) {return step(l,"",d)==2;}
static bool opt_toggle(const char *l,bool *v,bool,const char *d) {if(!step(l,*v?"On":"Off",d)) return false; *v=!*v; return true;}
static bool opt_choice(const char *l,int *v,const char *const *names,int count,int,const char *d) {
    int s=step(l,names[*v],d,true); if (!s) return false;
    *v=(*v+(s==2 ? 1:s)+count)%count; return true;
}
static void opt_text(const char *l,char *buf,size_t cap,const char *,const char *d) {
    if (mk8_collecting_rows()) mk8_collect_text(0,l,buf,cap,d);
}
static void rt_request_reboot(int mode) {reboots=mode;}
static void set_open(bool open) {assert(!open); ++closes;}
static void menu_close() {set_open(false);}
static void menu_toggle() {++closes;}
struct SDL_Event {int type;};
enum {SDL_EVENT_QUIT=1};
static void SDL_PushEvent(SDL_Event *e) {assert(e->type==SDL_EVENT_QUIT);++exits;}
static void SDL_OpenURL(const char *) {abort();}

static std::vector<UserMod> entries;
int usermods_count() {return (int)entries.size();}
bool usermods_get(int i,UserMod *out) {
    if ((unsigned)i>=entries.size()) return false;
    *out=entries[i]; return true;
}
const char *usermods_crash_note() {return "";}
bool usermods_changed_since_start() {return changed;}
int usermods_conflicts(int,char *out,size_t) { *out=0; return 0; }
void usermods_set_enabled(int i,bool on,char *off,size_t) {entries.at(i).enabled=on; *off=0;}
void usermods_move(int i,int dir) {std::swap(entries.at(i),entries.at(i+dir));}
bool usermods_remove(int i) {entries.erase(entries.begin()+i);++removals;return true;}

static NxStatus account;
static NetBackend backend=NET_BACKEND_OURS;
static bool mk8_nextendo_enabled() {return connection;}
NetBackend net_backend_setting() {return backend;}
bool net_backend_restart_needed() {return backend!=NET_BACKEND_OURS;}
bool net_backend_select(NetBackend n) {backend=n; return true;}
int net_backend_options(const NetBackendOption **out) {
    static const NetBackendOption choices[]={{NET_BACKEND_OURS,"off","Offline","Local play",true},
                                     {NET_BACKEND_NEXTENDO,"nx","Nextendo","Online play",true}};
    *out=choices; return 2;
}
void nextendo_status(NxStatus *out) { *out=account; }
static void sign_in() {++signins;account.state=NX_WAITING_BROWSER;}
void nextendo_sign_out() {++signouts;account.state=NX_SIGNED_OUT;}
static bool show_provider;
static GbState download=GB_IDLE;
static int download_cancels;
GbState gb_install_state(int *id,float *progress,char *msg,int cap) {
    *id=3; *progress=.5f; snprintf(msg,cap,"Provider status"); return download;
}
void gb_install_cancel() {++download_cancels; download=GB_IDLE;}

#include "prepared_actions.inc"
#include "ui/menu_play.h"
static int native_exits;
static std::string typed;
static void menu_keyboard_title(const char *) {}
static int mk8_native_text_keyboard(const char *,int,char *out,int cap) { snprintf(out,cap,"%s",typed.c_str()); return 1; }
extern "C" void mk8_native_menu_exit() {++native_exits;}
static bool online_rows_available() {return connection;}
static void tab_mods() {if (show_provider) install_status_row(); else page_installed();}
static void tab_online() {online_rows();}
extern "C" bool mk8_settings_shared_race(void) {return false;}
#include "ui/native_rows.inc"

static int find_row(const char *label,unsigned &rev,unsigned &depth) {
    int n=mk8_native_rows_count(&rev,&depth);
    for (int i=0;i<n;++i) {
        Mk8NativeRow row;
        assert(mk8_native_rows_get(i,rev,&row));
        if (!strcmp(row.label,label)) return i;
    }
    return -1;
}
static void select_row(const char *label,int step=0) {
    unsigned rev,depth; int i=find_row(label,rev,depth);
    assert(i>=0 && mk8_native_rows_activate(i,rev,step));
    mk8_native_rows_pump(1);
}
static Mk8NativeRow stock_row(const char *label) {
    unsigned rev,depth; int i=find_row(label,rev,depth); Mk8NativeRow row;
    assert(i>=0 && mk8_native_rows_get(i,rev,&row));
    return row;
}
static void read_only(const char *label) {
    unsigned rev,depth; int i=find_row(label,rev,depth);
    assert(i>=0 && !stock_row(label).enabled && !mk8_native_rows_activate(i,rev,0));
}
static void stock_rows() {
    for (const auto &entry : mk8_stock_labels) if (entry.tip.en) {
        for (auto lang : {MK8_MENU_EN,MK8_MENU_PT,MK8_MENU_ES}) {
            const char *tip=mk8_menu_label(lang,entry.tip);
            int units=0;
            for (const unsigned char *p=(const unsigned char *)tip;*p;++p) if ((*p&0xc0)!=0x80) units+=*p>=0xf0?2:1;
            assert(units<=40);
        }
    }
    mk8_native_rows_open(MENU_QUIT); mk8_native_rows_pump(1);
    assert(mk8_native_rows_count(nullptr,nullptr)==3);
    unsigned rev,depth; int i=find_row("Quit to Desktop",rev,depth); assert(i>=0 && !depth);
    select_row("Quit to Desktop"); unsigned before; assert(find_row("Cancel",before,depth)>=0 && depth==1);
    assert(mk8_native_rows_count(nullptr,nullptr)==2);
    Mk8NativeRow warning; int yes=find_row("Yes, quit to Desktop",before,depth);
    assert(yes>=0 && mk8_native_rows_get(yes,before,&warning) && !strcmp(warning.help,mk8_progress_warning.en));
    assert(!mk8_native_rows_activate(i,rev,0) && mk8_native_rows_back());
    mk8_native_rows_pump(1); assert(find_row("Quit to Desktop",rev,depth)>=0 && !depth);
    select_row("Quit to Desktop"); saved=false; select_row("Yes, quit to Desktop");
    assert(!native_exits && find_row("Save failed",rev,depth)>=0);
    saved=true; select_row("Yes, quit to Desktop"); assert(native_exits==1);
    mk8_native_rows_open(MENU_ACCOUNT); connection=false; mk8_native_rows_pump(1);
    i=find_row("Nextendo",rev,depth); assert(i>=0 && !mk8_native_rows_activate(i,rev,0));
    connection=true; mk8_native_rows_pump(1);
    read_only("Nextendo");
    assert(!strcmp(stock_row("Nextendo account").help,"Sign in through your browser"));
    select_row("Nextendo account");
    assert(!strcmp(stock_row("Nextendo account").help,"Finish signing in through your browser"));
#ifdef NX_SHEET
    read_only("Nextendo account");
#else
    int first_attempt=signins;
    select_row("Nextendo account"); assert(signins==first_attempt+1);
#endif
    select_row("Cancel sign-in");
    account.state=NX_SIGNED_IN; strcpy(account.username,"Local racer"); mk8_native_rows_pump(1);
    auto signed_in=stock_row("Nextendo account");
    assert(!strcmp(signed_in.value,account.username) && !strcmp(signed_in.help,"Your Nextendo username"));
    read_only("Nextendo account"); assert(stock_row("Sign out").enabled);
    account.state=NX_ERROR; strcpy(account.message,"Fixture error"); mk8_native_rows_pump(1);
    assert(!strcmp(stock_row("Nextendo account").help,account.message));
    mk8_native_rows_open(MENU_MODS); g_page=1; mk8_native_rows_pump(1);
    select_row("A mod");
    for (auto row : {"Mod","By","Category","Licence","Game files","Code patches"}) read_only(row);
    assert(stock_row("On").enabled && stock_row("Load later").enabled);
    select_row("Remove");
    i=find_row("Yes, remove it",rev,depth); Mk8NativeRow mod;
    assert(i>=0 && depth==2 && mk8_native_rows_get(i,rev,&mod) && !strcmp(mod.value,"A mod"));
    assert(mk8_native_rows_activate(i,rev,0)); strcpy(entries[g_mod].id,"replacement-with-same-display-name");
    mk8_native_rows_pump(1); assert(!removals);
    assert(mk8_native_rows_back()); mk8_native_rows_pump(1); assert(!g_ask_remove && !removals);
    assert(mk8_native_rows_back()); mk8_native_rows_pump(1);
    typed="A mod"; select_row("Search installed"); assert(find_row("B mod",rev,depth)<0);
    typed.clear(); select_row("Search installed"); assert(find_row("B mod",rev,depth)>=0);
    i=find_row("A mod",rev,depth); assert(mk8_native_rows_activate(i,rev,0));
    mk8_native_rows_close(); mk8_native_rows_pump(1);
    assert(g_mod==-1 && mk8_native_rows_count(nullptr,nullptr)==0);
    entries.clear(); mk8_native_rows_open(MENU_MODS); mk8_native_rows_pump(1); read_only("None yet");
    show_provider=true; mk8_native_rows_pump(1); read_only("Provider status");
    download=GB_BUSY; mk8_native_rows_pump(1); select_row("Provider status");
    assert(download_cancels==1); read_only("Provider status");
}

static void quit(const char *which) {
    observed.clear(); action=which; tab_quit(.65f,true);
    assert(!saw("About") && !saw("Version"));
}
static void mods(const char *which) {action=which;page_installed();}
static void online(const char *which) {action=which;online_rows();}
int main() {
    quit("Quit to Desktop"); assert(mk8_quit_choice==2);
    quit("Cancel"); assert(mk8_quit_consumed_back && !mk8_quit_choice && !exits);
    assert(mk8_quit_detail_back() && !exits);
    quit("Restart the Game"); back=true; quit(""); back=false;
    assert(mk8_quit_detail_back() && !reboots);
    for (int i=1;i<=3;++i) {
        quit(i==1 ? "Restart the Game" : i==2 ? "Quit to Start Screen" : "Quit to Desktop");
        const char *yes=i==1 ? "Yes, restart the game" : i==2 ? "Yes, quit to Start Screen" : "Yes, quit to Desktop";
        reboots=0; saved=false; quit(yes); assert(!reboots && !exits && messages);
        saved=true; quit(yes); assert(i==3 ? exits==1 : reboots==i);
    }

    g_page=1; mods(""); assert(saw("None yet") && !mk8_mods_detail_back());
    UserMod one={},two={}; strcpy(one.name,"A mod"); strcpy(two.name,"B mod");
    strcpy(one.license,"MIT"); strcpy(one.category,"Racing"); one.has_exefs=true;
    strcpy(one.id,"mod-a"); strcpy(two.id,"mod-b"); one.has_romfs=two.has_romfs=true;
    entries={one,two}; mods("A mod"); assert(g_mod==0);
    mods("On"); assert(entries[0].enabled);
    mods("Load later"); assert(g_mod==1 && !strcmp(entries[1].name,"A mod"));
    mods("Remove"); assert(g_ask_remove && !removals);
    back=true; mods(""); back=false; assert(!g_ask_remove && g_mod==1 && mods_page_used_back());
    mods("< Back to installed mods"); assert(mods_page_used_back() && g_mod==-1 && entries.size()==2);
    changed=true; saved=false; reboots=0; mods("Restart the game to apply"); assert(!reboots);
    saved=true; mods("Restart the game to apply"); assert(reboots==2);
    connection=false; online("Nextendo account"); assert(!signins);
    connection=true; online("Nextendo account"); assert(signins==1 && account.state==NX_WAITING_BROWSER);
    online("Cancel sign-in"); assert(signouts==1);
    account.state=NX_SIGNED_IN; online("Sign out"); assert(signouts==2);
    online("Online backend"); assert(backend==NET_BACKEND_NEXTENDO);
    saved=false; reboots=0; online("Restart the game now"); assert(!reboots);
    saved=true; online("Restart the game now"); assert(reboots==1);
    stock_rows();
}
