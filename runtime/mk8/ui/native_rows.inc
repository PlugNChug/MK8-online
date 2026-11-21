#include "native_rows_host.h"
#include "menu_row_labels.h"
#include <algorithm>
#include <mutex>
#include <vector>
extern "C" void mk8_native_menu_exit(void);
extern "C" bool mk8_settings_shared_race(void);

// Host option bodies own their data and actions. The guest sees only copies.
struct Mk8CapturedRow {
    Mk8NativeRow display{};
    unsigned id=0;
    int text_limit=0;
    bool operator==(const Mk8CapturedRow &other) const {
        return id==other.id && text_limit==other.text_limit &&
               memcmp(&display,&other.display,sizeof display)==0;
    }
};
struct Mk8RowAction {
    int index=-1, step=0;
    Mk8CapturedRow expected{};
    char text[192]{};
};
static std::mutex rows_lock;
static int rows_group=-1;
static unsigned rows_epoch,rows_revision,rows_depth;
static bool rows_back_requested;
static Mk8RowAction rows_action;
static std::vector<Mk8CapturedRow> rows_published;
static thread_local bool rows_collecting;
// Only the render thread visits these.
static unsigned render_epoch;
static int render_group=-1;
static Mk8RowAction render_action;
static std::vector<Mk8CapturedRow> render_rows;
static bool render_exit;

bool mk8_collecting_rows() { return rows_collecting; }
static void reset_rows_locked(int group) {
    ++rows_epoch; ++rows_revision; rows_group=group; rows_depth=0; rows_back_requested=false;
    rows_action={}; rows_published.clear();
}
extern "C" void mk8_native_rows_open(int group) {
    if (group!=MENU_ACCOUNT && group!=MENU_MODS && group!=MENU_QUIT) group=-1;
    std::lock_guard<std::mutex> lock(rows_lock);
    if (rows_group!=group) reset_rows_locked(group);
}
extern "C" void mk8_native_rows_close(void) { mk8_native_rows_open(-1); }
extern "C" int mk8_native_rows_count(unsigned *revision,unsigned *depth) {
    std::lock_guard<std::mutex> lock(rows_lock);
    if (revision) *revision=rows_revision;
    if (depth) *depth=rows_depth;
    return (int)rows_published.size();
}
extern "C" bool mk8_native_rows_get(int i,unsigned revision,Mk8NativeRow *out) {
    std::lock_guard<std::mutex> lock(rows_lock);
    if (!out || revision!=rows_revision || i<0 || (size_t)i>=rows_published.size()) return false;
    *out=rows_published[i].display;
    return true;
}
extern "C" bool mk8_native_rows_back(void) {
    std::lock_guard<std::mutex> lock(rows_lock);
    if (!rows_depth) return false;
    rows_action={}; rows_back_requested=true;
    return true;
}
extern "C" bool mk8_native_rows_activate(int i,unsigned revision,int step) {
    std::unique_lock<std::mutex> lock(rows_lock);
    if (revision!=rows_revision || i<0 || (size_t)i>=rows_published.size() ||
        rows_action.index>=0 || rows_back_requested || step < -1 || step > 1) return false;
    Mk8RowAction action{};
    action.expected=rows_published[i]; action.index=i; action.step=step ? step : 2;
    const Mk8NativeRow &row=action.expected.display;
    if (!row.enabled || (step && !row.adjustable)) return false;
    if (action.expected.text_limit) {
        lock.unlock();
        menu_keyboard_title(row.label);
        if (!mk8_native_text_keyboard(row.value,action.expected.text_limit,action.text,sizeof action.text)) return false;
        lock.lock();
        if (rows_revision!=revision || rows_action.index>=0 || rows_back_requested) return false;
    }
    rows_action=action;
    return true;
}

static void copy_row_text(char *dst,size_t cap,const char *text) {
    if (!text) text="";
    size_t n=strlen(text);
    if (n>=cap) {
        n=cap-1;
        while (n && ((unsigned char)text[n]&0xc0)==0x80) --n;
    }
    memcpy(dst,text,n); dst[n]=0;
}
static int record_row(unsigned id,const char *label,const char *value,const char *help,bool enabled,bool adjustable,int text_limit,bool named_option) {
    // The installed-mod service caps its list at 128, GameBanana at 100.
    if (render_rows.size()>=256) return 0;
    Mk8CapturedRow row{};
    if (named_option) {
        const auto lang=mk8_menu_language();
        for (const auto &entry : mk8_stock_labels) {
            if (entry.group!=render_group || strcmp(label,entry.label.en)) continue;
            label=mk8_menu_label(lang,entry.label);
            if (entry.tip.en) help=mk8_menu_label(lang,entry.tip);
            break;
        }
    }
    row.id=id; row.text_limit=text_limit; row.display.enabled=enabled; row.display.adjustable=adjustable;
    copy_row_text(row.display.label,sizeof row.display.label,label);
    copy_row_text(row.display.value,sizeof row.display.value,value);
    copy_row_text(row.display.help,sizeof row.display.help,help);
    int index=(int)render_rows.size();
    render_rows.push_back(row);
    if (!enabled || render_action.index!=index || !(row==render_action.expected)) return 0;
    std::lock_guard<std::mutex> lock(rows_lock);
    return render_epoch==rows_epoch ? render_action.step : 0;
}
int mk8_collect_row(unsigned id,const char *label,const char *value,const char *help,bool enabled,bool adjustable,bool named_option) {
    return record_row(id,label,value,help,enabled,adjustable,0,named_option);
}
void mk8_collect_section(const char *label) {
    if (*label && strcmp(label,"Mods") && strcmp(label,"Quit")) mk8_collect_row(0,label,"","",false,false,false);
}
bool mk8_collect_text(unsigned id,const char *label,char *buf,size_t cap,const char *help) {
    if (!cap) return false;
    if (record_row(id,label,buf,help,true,false,(int)std::min(cap-1,(size_t)95),true)!=2) return false;
    copy_row_text(buf,cap,render_action.text);
    return true;
}
void mk8_collect_exit_feedback(bool failed,const char *message) {
    if (failed) {
        const char *label=mk8_menu_text(mk8_menu_language(),"Save failed","Falha ao salvar","Error al guardar");
        mk8_collect_row(0,label,"",message,false,false);
    } else render_exit=true;
}
extern "C" unsigned mk8_mods_depth(void);
extern "C" bool mk8_mods_detail_back(void);

static bool mk8_native_rows_active() {
    std::lock_guard<std::mutex> lock(rows_lock);
    return rows_group>=0 || render_group>=0;
}
static void mk8_native_rows_pump(float scale) {
    int group;
    unsigned epoch;
    bool back;
    if (mk8_settings_shared_race()) mk8_native_rows_close();
    {
        std::lock_guard<std::mutex> lock(rows_lock);
        group=rows_group; epoch=rows_epoch;
        back=rows_back_requested; rows_back_requested=false;
        render_action=rows_action; rows_action={};
    }
    if (render_epoch!=epoch) {
        if (group==MENU_MODS || render_group==MENU_MODS) while (mk8_mods_detail_back()) {}
        if (group==MENU_QUIT || render_group==MENU_QUIT) mk8_quit_detail_back();
        render_epoch=epoch; render_group=group;
    }
    if (group<0) return;
    if (back) {
        if (group==MENU_MODS) mk8_mods_detail_back();
        if (group==MENU_QUIT) mk8_quit_detail_back();
    }
    rows_collecting=true; render_exit=false;
    auto visit=[&]() {
        render_rows.clear();
        if (group==MENU_MODS) tab_mods();
        else if (group==MENU_QUIT) tab_quit(scale,true);
        else if (online_rows_available()) tab_online();
        else {
            auto lang=mk8_menu_language();
            const Mk8PlaySession local{};
            mk8_collect_row(0,"Nextendo",mk8_menu_text(lang,"Offline","Offline","Sin conexión"),
                mk8_menu_play_text(lang,&local,PLAY_INSTRUCTION),false,false);
        }
    };
    visit();
    if (render_action.index>=0 && !render_exit) { render_action={}; visit(); }
    unsigned depth=group==MENU_MODS ? mk8_mods_depth() : group==MENU_QUIT && mk8_quit_choice!=0;
    rows_collecting=false;
    {
        std::lock_guard<std::mutex> lock(rows_lock);
        if (rows_epoch!=epoch) return;
        if (render_exit) reset_rows_locked(-1);
        else if (rows_depth!=depth || rows_published!=render_rows) {
            rows_published.swap(render_rows); rows_depth=depth; ++rows_revision;
        }
    }
    if (render_exit) mk8_native_menu_exit();
}
