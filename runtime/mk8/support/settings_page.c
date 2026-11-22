#include "rt.h"
#include "hle.h"
#include "settings_values.h"
#include "../ui/settings_children.h"
#include "../ui/menu_play.h"
#include "../ui/menu_nextendo.h"
#include "../net/social.h"
#include "../ui/menu_launcher_actions.h"
#include "controller_edit.h"
#include "controller_tips.h"
bool mk8_auto_lan_requested(void);
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const char *const symbol_names[]={"A","B","X","Y","L","R","ZL","ZR","SL","SR","Plus","Minus","DL","DR","DT","DB"};
enum { SYMBOL_COUNT=sizeof symbol_names/sizeof *symbol_names };
typedef struct {
    uint64_t page, mount, root, labels[SETTINGS_VISIBLE+1], values[SETTINGS_VISIBLE+1], rows[SETTINGS_VISIBLE+1];
    uint64_t hint, hint_second, hint_plate, hint_single, hint_double, keycaps[4], key_labels[4], action_labels[4];
    uint64_t join_buttons[4], join_plus[2], symbol_help, symbol_dialog, symbol_parts[5][SYMBOL_COUNT], shown_symbols[5];
    uint64_t title_label, scrollbar, dialog, dialog_buttons[2], dialog_animations[2], dialog_labels[3];
    uint64_t dialog_cursor, cursor_animation, back_animation, back_icon;
    uint64_t held_pulse;
    bool menu_cursor_active, play_layout, nextendo_layout, visible, reset_motion;
} MenuPanes;
// Only guest presentation objects are per page. Input, current section, edits and pending saves stay in one session.
static MenuPanes selector_panes, fader_panes, *panes=&selector_panes;
static uint64_t owner,top,top_root,restore_nav;
static struct Mk8ControlTips tips;
static bool selector_absent, restore_menu_cursors;

static bool restart_prompt, restart_yes, restart_wait, restart_pressed, exit_pressed;
static uint64_t restart_fade_at, controller_fade_at;
static int controller_press=-1;
static _Atomic bool from_button;
static _Atomic bool available, requested;
static _Atomic unsigned shared_race;
static bool overlay_wrench;
static unsigned category, focus, offset;
enum MenuLevel { MENU_HOME, MENU_OPTIONS, MENU_PLAY_ACTIONS, MENU_ROOMS, MENU_ROOM_DETAILS, MENU_FRIENDS };
enum PlayAction { PLAY_RESUME, PLAY_NEXTENDO, PLAY_MII, PLAY_ROOMS, PLAY_ENTER, PLAY_LEAVE, PLAY_IDLE };
static enum PlayAction play_actions[2+MENU_LAUNCHER_ACTION_COUNT], play_pending=PLAY_IDLE;
static unsigned play_count;
static uint64_t play_pressed_at, play_checked_at;
static Mk8PlaySession play_session;

static enum MenuLevel menu_level;
static _Atomic bool exit_menu;

static void menu_cursor(Cpu *c,bool show);
static const enum Mk8MenuGroup category_groups[]={MENU_VIDEO,MENU_AUDIO,MENU_CONTROLS,MENU_QUICK,MENU_ACCOUNT,MENU_MODS,MENU_QUIT};
static bool settings_from_service;
static int painted_depth=-1, slide_direction;
static uint64_t slide_at,pulse_at,pulse_pane;
static bool incoming;
static unsigned rich_revision,rich_depth,rich_count;
static unsigned parent_focus[4],parent_offset[4];
static bool rich_category(void) { return menu_level==MENU_OPTIONS && category>=SETTINGS_CATEGORIES; }
static uint64_t previous;
static bool touched;
static bool entering, confirming, exiting, reset_confirm;
static uint64_t entered_at, exited_at, last_revision, repeat_at;
static float reveal, exit_from;
static uint64_t cursor_owner, button;
static unsigned cursor_visibility;
static _Atomic bool controller_preview, controller_visible;
static _Atomic unsigned controller_applet_players, controller_entry_players;
static unsigned controller_rows;
static _Atomic bool controller_required;
static _Atomic bool controller_ready_button;
static _Atomic bool controller_detached;
static _Atomic bool controller_focused=true, controller_resync;
static _Atomic bool controller_mouse_down, controller_character_shortcut;
static _Atomic int controller_hover=-2, controller_click=-1, controller_armed=-1;
static int controller_focus;
enum ControllerPanel { PLAYERS, ROW_ACTIONS, SWAP_TARGET };
static enum ControllerPanel controller_panel;
static int action_player;
static bool controller_cancelled;
static uint64_t controller_previous;

uint64_t mk8_controller_p1_navigation(void);
void mk8_controller_hold_extra_inputs(void);
static void controller_release(void);
static _Atomic bool active, back;
static _Atomic uint64_t buttons, suppressed, fixed_buttons, raw_buttons;
static _Atomic int wheel;
static _Atomic bool editing;
static int editing_id, edit_min, edit_max;
static char number[4];
static bool replace_number, invalid;
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static int keys[32], key_count;
static bool overflow;
bool menu_input_captured(void);
bool touch_first(int32_t *, int32_t *);
void mk8_settings_button_restore(uint64_t);
bool mk8_settings_button_decided(uint64_t);
bool mk8_settings_layout_available(void);
bool mk8_settings_layout_unavailable(void);
void mk8_menu_request_overlay(void);
uint64_t mk8_settings_fader_root(uint64_t page);
bool mk8_settings_fader_covers(uint64_t page);
void mk8_settings_top_forget(void);
void input_reset(void);
void mk8_input_race_players(unsigned players);

RT_ON_RESET(settings_page_reset) {
    memset(&selector_panes,0,sizeof selector_panes); memset(&fader_panes,0,sizeof fader_panes);
    panes=&selector_panes; selector_absent=restore_menu_cursors=false;
    mk8_native_rows_close();
    panes->menu_cursor_active=exit_menu=false;
    if(controller_visible) controller_release();
    painted_depth=-1; slide_direction=0; pulse_pane=0;
    owner = panes->root = cursor_owner = button = 0; active = back = editing = entering = confirming = exiting = false;
    controller_preview=controller_visible=false;
    controller_applet_players=controller_entry_players=0;
    controller_mouse_down=controller_character_shortcut=false;
    controller_armed=controller_click=-1; controller_hover=-2;
    top=top_root=restore_nav=panes->scrollbar=0; available=requested=from_button=shared_race=overlay_wrench=false;
    panes->dialog=panes->dialog_cursor=panes->cursor_animation=panes->back_animation=0;
    restart_prompt=restart_wait=restart_pressed=exit_pressed=false;
    buttons = suppressed = fixed_buttons = raw_buttons = 0; wheel=0;
}

static void heap(uint64_t p, size_t n) {
    if (p < HEAP_BASE || p >= HEAP_BASE+HEAP_MAX || n > HEAP_BASE+HEAP_MAX-p)
        FATAL("[mk8:settings] invalid selector object: %llx size=%zu\n",p,n);
}

uint64_t kernel_heap_size(void);
static bool race_heap(uint64_t p,size_t size,uint64_t end) {
    return !(p&7) && p>=HEAP_BASE && p<end && size<=end-p;
}

static bool registered_page(uint64_t manager,uint64_t page,uint64_t vtable,unsigned id,uint64_t end) {
    uint64_t registry=LD64(manager+0x30);
    return id<135 && race_heap(registry,(id+1)*8,end) && LD64(registry+id*8)==page &&
           LD64(page)==vtable && LD64(page+0x20)==manager && LD32(page+0xe8)==id;
}

// Evaluated on the hash-checked 4.0.1 UI tick. Other threads see only the published decision.
static unsigned shared_race_scene(uint64_t manager) {
    uint64_t app=LD64(MAIN_BASE+0x1358c70);
    if(app!=MAIN_BASE+0x133ebd0) return false;
    uint64_t end=HEAP_BASE+kernel_heap_size(), node=LD64(app+0x20);
    if(!race_heap(node,0x10,end)) return false;
    uint64_t scene=LD64(node+8);
    if(!race_heap(scene,0x188,end) || LD32(scene+0x184)!=4) return false;
    uint64_t context=LD64(app+0x28);
    if(!race_heap(context,0x248,end)) return false;
    uint64_t config=LD64(context+0x240);
    if(!race_heap(config,0x18,end)) return false;
    uint64_t rules=LD64(config+0x10);
    if(!race_heap(rules,0x1a0,end)) return false;
    // MK8's local-player accessor (MAIN+0x86f154), including its alternate count for kind 4.
    uint32_t players=LD32(rules+(LD32(rules+8)==4 ? 0x19c : 0x188));
    if(players<2 || players>4) return false;
    // The race scene stays loaded under the standings and the following Next Round menu.
    if(race_heap(manager,0xb60,end)) {
        unsigned n=LD32(manager+0xb50);
        uint64_t pages=LD64(manager+0xb58);
        if(n<=32 && race_heap(pages,n*8,end)) for(unsigned i=0;i<n;i++) {
            uint64_t p=LD64(pages+i*8);
            if(!race_heap(p,0x128,end) || LD32(p+0x120)!=4) continue;
            uint64_t type=LD64(p);
            uint32_t id=LD32(p+0xe8);
            if((id==0x70 && type==MAIN_BASE+0x1288c38) ||
               (id==0x72 && type==MAIN_BASE+0x1288038)) return false;
        }
    }
    return players;
}

bool mk8_settings_shared_race(void) { return atomic_load(&shared_race); }

static uint64_t call(Cpu *c, uint64_t address, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f) {
    c->x[0]=a; c->x[1]=b; c->x[2]=d; c->x[3]=e; c->x[4]=f;
    guest_call(c, address);
    return c->x[0];
}

enum CursorAction { SAVE_CURSORS, HIDE_CURSORS, RESTORE_CURSORS };
static void stock_cursors(Cpu *c,enum CursorAction action) {
    if(!owner || selector_absent) return;
    if(action==SAVE_CURSORS) { cursor_owner=owner; cursor_visibility=0; }
    else if(cursor_owner!=owner) return;
    // The shared page creates one cursor per player even in single-player menus.
    heap(owner,0x1f0);
    for(unsigned i=0;i<4;i++) {
        uint64_t cursor=LD64(owner+0x1d0+i*8); heap(cursor,0xc8);
        if(action==SAVE_CURSORS) {
            if(call(c,0x70008adf84ull,cursor+0x30,0,0,0,0)) cursor_visibility|=1u<<i;
        } else call(c,0x70008ae098ull,cursor+0x30,action==RESTORE_CURSORS && (cursor_visibility&(1u<<i)),0,0,0);
    }
    if(action==RESTORE_CURSORS) cursor_owner=0;
}

static uint64_t string(Cpu *c, const char *text) {
    size_t n = strlen(text)+1;
    if (n > 80) FATAL("[mk8:settings] selector string too long\n");
    ST64(c->sp, LD64(0x70012f9e28ull)+0x10); ST64(c->sp+8, c->sp+32);
    memcpy((void *)(uintptr_t)(c->sp+32), text, n);
    return c->sp;
}

static uint64_t find(Cpu *c, uint64_t component, const char *name) {
    uint64_t p = call(c, 0x70008ad8e8ull, component, string(c,name), 0, 0, 0);
    heap(p, 0x18);
    return p;
}

static void visible(Cpu *c, uint64_t component, const char *name, bool show) {
    uint64_t pane = find(c,component,name); heap(pane,0x60);
    call(c,0x70008ade74ull,pane,show,0,0,0);
}

static void menu_sound(Cpu *c, uint64_t event) {
    call(c,0x700008c608ull,event,0x7000ed8620ull,0,0,0);
}

static void dialog_open_sound(Cpu *c) { menu_sound(c,0x7000ee9367ull); } // SE_SYS_DIALOG_OPEN

static void reveal_pane(Cpu *c,const char *name,float progress) {
    uint64_t pane=find(c,panes->root,name); heap(pane,0x60);
    float eased=progress*progress*(3-2*progress), translation[3]={0,-16*(1-eased),0};
    ST8(pane+0x59,(uint8_t)(255*eased));
    ST8(pane+0x58,LD8(pane+0x58)|2);
    memcpy((void *)(uintptr_t)(c->sp+320),translation,sizeof translation);
    call(c,0x70008ade34ull,pane,c->sp+320,0,0,0);
}

static void appear(Cpu *c, float progress) {
    reveal=progress;
    reveal_pane(c,"N_Settings",progress);
    if(panes->menu_cursor_active) reveal_pane(c,"N_SettingsPopup",progress);
}

// Both the assignment footer and restart dialog use the stock Decide animation before fading.
static bool popup_fade(Cpu *c,uint64_t *started,bool with_roster) {
    uint64_t now=plat_now_ns();
    if(!*started) *started=now;
    uint64_t elapsed=now-*started;
    float progress=elapsed>=160000000ull ? 0 : 1-elapsed/160000000.f;
    reveal_pane(c,"N_SettingsPopup",progress);
    if(with_roster) appear(c,progress);
    return progress==0;
}

static size_t utf16_line(uint16_t *dest,size_t limit,const char *value,bool shoulders) {
    size_t out=0;
    while(*value && out<limit) {
        unsigned ch=(unsigned char)*value++;
        if(shoulders && ch=='[' && (*value=='L' || *value=='R') && value[1]==']') {
            ch=*value=='L' ? 0xe0a4 : 0xe0a5; value+=2;
        } else if(ch>=0x80) {
            unsigned count=ch>=0xf0 && ch<=0xf4 ? 3 : ch>=0xe0 && ch<=0xef ? 2 : ch>=0xc2 && ch<=0xdf ? 1 : 0;
            unsigned cp=ch & (0x7f >> (count+1)),j=0;
            for(;j<count && ((unsigned char)*value&0xc0)==0x80;j++) cp=(cp<<6)|((unsigned char)*value++&63);
            ch=count && j==count && cp>=(count==1 ? 0x80u : count==2 ? 0x800u : 0x10000u) &&
               cp<=0x10ffff && (cp<0xd800 || cp>0xdfff) ? cp : 0xfffd;
        }
        if(ch>=0x10000) {
            if(out+2>limit) break;
            ch-=0x10000; dest[out++]=0xd800+(ch>>10); ch=0xdc00+(ch&1023);
        }
        dest[out++]=ch;
    }
    dest[out]=0;
    return out;
}
static void text(Cpu *c, uint64_t pane, const char *value) {
    size_t n = strlen(value);
    if (n > 79) FATAL("[mk8:settings] selector value too long\n");
    utf16_line((uint16_t *)(uintptr_t)(c->sp+128),79,value,false);
    call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
}

// The title font supplies the outline shoulders but has no sideways rail glyphs.
static size_t instruction_line(uint16_t *out,size_t limit,const char *s) {
    return utf16_line(out,limit,s,true);
}

static void instruction(Cpu *c,uint64_t pane,size_t limit,const char *s) {
    instruction_line((uint16_t *)(uintptr_t)(c->sp+128),limit,s);
    call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
}

static bool read_tips(bool settings) {
    struct Mk8ControlTips current;
    mk8_control_tips(settings,&current);
    if(!memcmp(&tips,&current,sizeof tips)) return false;
    tips=current;
    return true;
}

// turbo_messageFontOutline_50 already supplies these symbols to both stock help text boxes.
static uint16_t button_glyph(const char *label) {
    static const struct { const char *name; uint16_t code; } pad[]={
        {"A",0xe000},{"B",0xe001},{"X",0xe002},{"Y",0xe003},{"L",0xe004},{"R",0xe005},
        {"Plus",0xe045},{"Minus",0xe046},{"ZL",0xe054},{"ZR",0xe055},
        {"D-pad Up",0xe079},{"D-pad Down",0xe07a},{"D-pad Left",0xe07b},{"D-pad Right",0xe07c}
    }, ps[]={{"Cross",0x00d7},{"Circle",0x25cb},{"Square",0x25a1},{"Triangle",0x25b3}};
    if(tips.source==MK8_TIP_PLAYSTATION)
        for(unsigned i=0;i<sizeof ps/sizeof *ps;i++) if(!strcmp(label,ps[i].name)) return ps[i].code;
    unsigned count=tips.source==MK8_TIP_NINTENDO ? sizeof pad/sizeof *pad : tips.source==MK8_TIP_XBOX ? 4 : 0;
    for(unsigned i=0;i<count;i++) if(!strcmp(label,pad[i].name)) return pad[i].code;
    return 0;
}

struct TipAction { enum Mk8TipAction action; const char *verb; };

static void hint_box(Cpu *c,uint64_t pane,float x,float y,float width,float height) {
    float geometry[5]={x,y,0,width,height};
    memcpy((void *)(uintptr_t)(c->sp+320),geometry,sizeof geometry);
    call(c,0x70008ade34ull,pane,c->sp+320,0,0,0);
    call(c,0x70008ade58ull,pane,c->sp+332,0,0,0);
}

static int symbol_index(const char *label) {
    bool xbox=tips.source==MK8_TIP_XBOX;
    if(!xbox && tips.source!=MK8_TIP_NINTENDO) return -1;
    for(unsigned i=0;i<(xbox ? 4 : SYMBOL_COUNT);i++)
        if(!strcmp(label,symbol_names[i])) return (int)i;
    if(!xbox) {
        static const char *const directions[]={"Left","Right","Up","Down"};
        if(!strncmp(label,"D-pad ",6)) label+=6;
        for(unsigned i=0;i<4;i++) if(!strcmp(label,directions[i])) return 12+(int)i;
    }
    return -1;
}

static void hide_symbol(Cpu *c,unsigned slot) {
    if(panes->shown_symbols[slot]) call(c,0x70008ade74ull,panes->shown_symbols[slot],false,0,0,0);
    panes->shown_symbols[slot]=0;
}

static float symbol_width(int icon) {
    if(icon==10 || icon==11) return 24;
    return icon>=4 && icon<=9 ? 56 : 40;
}

static void show_symbol(Cpu *c,unsigned slot,int icon,float x,float y) {
    uint64_t parent=slot==4 ? panes->symbol_dialog : panes->symbol_help;
    uint64_t *part=&panes->symbol_parts[slot][icon];
    if(!*part) {
        char name[16];
        snprintf(name,sizeof name,"P_Map%u%s",slot==4 ? 0 : slot,symbol_names[icon]);
        *part=find(c,parent,name);
    }
    panes->shown_symbols[slot]=*part;
    call(c,0x70008ade74ull,*part,true,0,0,0);
    hint_box(c,*part,x,y-5.5f,symbol_width(icon),40);
}

static float hint_text_y(bool two_lines,bool lower) {
    // The font baseline places the 36-unit keycap ten units inside each 56-unit row.
    if(panes->hint_double && two_lines) return lower ? -57 : -1;
    return (two_lines ? 10 : 0)-(lower ? 44 : 5);
}

static void hint_background(Cpu *c,bool two_lines,float width) {
    if(!panes->keycaps[0]) return;
    bool legacy_double=two_lines && !panes->hint_double;
    hint_box(c,panes->hint_plate,0,legacy_double ? -8 : 2,width,legacy_double ? 96 : 56);
    if(panes->hint_double) {
        call(c,0x70008ade74ull,panes->hint_single,!two_lines,0,0,0);
        call(c,0x70008ade74ull,panes->hint_double,two_lines,0,0,0);
        hint_box(c,two_lines ? panes->hint_double : panes->hint_single,0,2,width,two_lines ? 112 : 56);
    }
    hint_box(c,panes->hint,0,hint_text_y(two_lines,false),width,50);
    hint_box(c,panes->hint_second,0,hint_text_y(two_lines,true),width,50);
}

static bool keycap_tips(Cpu *c,const char *description,const char *const keys_text[],const char *const verbs_text[],
                        unsigned n,const char *mouse_hint,bool show_chords,bool keyboard_joins) {
    if(!panes->keycaps[0]) return false; // Previously prepared archives retain their stock text-only hints.
    float keys[4], verbs[4], widths[4], columns[4]={0}, total=0;
    int icons[4]={-1,-1,-1,-1};
    bool plain[4]={0};
    for(unsigned i=0;i<4;i++) {
        hide_symbol(c,i);
        bool pad=i<n && tips.source!=MK8_TIP_KEYBOARD && !(keyboard_joins && i+1==n);
        if(pad && (controller_required || controller_preview) && panes->symbol_help && !(show_chords && i<2)) {
            icons[i]=symbol_index(keys_text[i]);
            plain[i]=icons[i]<0;
        }
        bool cap=i<n && icons[i]<0 && !plain[i] && !(show_chords && i<2);
        call(c,0x70008ade74ull,panes->keycaps[i],cap,0,0,0);
        call(c,0x70008ade74ull,panes->key_labels[i],cap,0,0,0);
        call(c,0x70008ade74ull,panes->action_labels[i],i<n,0,0,0);
        if(panes->join_buttons[0]) {
            call(c,0x70008ade74ull,panes->join_buttons[i],show_chords,0,0,0);
            if(i<2) call(c,0x70008ade74ull,panes->join_plus[i],show_chords,0,0,0);
        }
    }
    if(!n) return false;
    unsigned per_row=n==4 ? 2 : n;
    bool roster=n==4 && (controller_required || controller_preview);
    float button_columns[2]={0};
    bool two_lines=show_chords || description || mouse_hint || n>per_row;
    for(unsigned i=0;i<n;i++) {
        keys[i]=show_chords && i<2 ? 136 : icons[i]>=0 ? symbol_width(icons[i]) : 22+17*strlen(keys_text[i]);
        verbs[i]=12+17*strlen(verbs_text[i]);
        if(roster) {
            // Optical widths of Assign, Swap, Clear and Clear all in the stock tip font.
            static const float caption_widths[]={96,78,76,116};
            verbs[i]=i==2 && tips.source==MK8_TIP_KEYBOARD ? 138 : caption_widths[i];
            if(keys[i]>button_columns[i%2]) button_columns[i%2]=keys[i];
        }
        // Use the two join captions' rendered widths so both words start in one column.
        if(show_chords && i==0) verbs[i]=440;
    }
    for(unsigned i=0;i<n;i++) {
        widths[i]=(roster ? button_columns[i%2] : keys[i])+8+verbs[i];
        if(widths[i]>columns[i%per_row]) columns[i%per_row]=widths[i];
    }
    for(unsigned i=0;i<per_row;i++) total+=columns[i]+(i ? 40 : 0);
    float lower_width=0;
    if(show_chords) {
        lower_width=widths[1]+(n==3 ? 88+widths[2] : 0);
        total=widths[0]>lower_width ? widths[0] : lower_width;
    }
    hint_background(c,two_lines,total+44>880 ? total+44 : 880);
    instruction(c,panes->hint,40,description ? description : "");
    call(c,0x70008ade74ull,panes->hint_second,mouse_hint!=NULL,0,0,0);
    if(mouse_hint) text(c,panes->hint_second,mouse_hint);
    float x=0;
    for(unsigned i=0;i<n;i++) {
        if(i%per_row==0) x=-total/2;
        if(show_chords && i<2) x=-total/2;
        float start=roster ? x : x+(columns[i%per_row]-widths[i])/2;
        float y=hint_text_y(two_lines,show_chords ? i!=0 : description || i>=per_row);
        float button_width=roster ? button_columns[i%2] : keys[i];
        float key_center=start+button_width/2;
        if(plain[i]) {
            char label[64];
            snprintf(label,sizeof label,"%s %s",keys_text[i],verbs_text[i]);
            hint_box(c,panes->action_labels[i],start+widths[i]/2,y,widths[i],50);
            text(c,panes->action_labels[i],label);
            x+=columns[i%per_row]+(show_chords ? 88 : 40);
            continue;
        }
        // The stock title font's baseline is nine layout units below its pane center.
        if(show_chords && i<2) {
            // Center the chord on the label's capital letters, above the font's plus baseline.
            float plus_y=y+3.5f;
            // The stock picture sits half its height above the part origin.
            float icon_y=plus_y-9-20;
            // The SL artwork reaches farther toward the plus than the SR artwork does.
            float left_offset=i==1 ? 48 : 40;
            hint_box(c,panes->join_buttons[2*i],key_center-left_offset,icon_y,56,40);
            hint_box(c,panes->join_buttons[2*i+1],key_center+40,icon_y,56,40);
            hint_box(c,panes->join_plus[i],key_center,plus_y,24,50);
        } else if(icons[i]>=0) {
            show_symbol(c,i,icons[i],key_center,y);
        } else {
            hint_box(c,panes->keycaps[i],key_center,y-9,keys[i],36);
            hint_box(c,panes->key_labels[i],key_center,y,keys[i],50);
            text(c,panes->key_labels[i],keys_text[i]);
        }
        hint_box(c,panes->action_labels[i],start+button_width+8+verbs[i]/2,y,verbs[i],50);
        text(c,panes->action_labels[i],verbs_text[i]);
        x+=columns[i%per_row]+(show_chords ? 88 : 40);
    }
    return true;
}

static void action_tips(Cpu *c,const char *description,const struct TipAction *actions,unsigned count,const char *fallback) {
    if(!panes->hint) return;
    const char *key[4],*verb[4];
    unsigned n=0;
    if(tips.source==MK8_TIP_KEYBOARD || ((controller_required || controller_preview) && panes->symbol_help)) for(unsigned i=0;i<count;i++) {
        const char *label=tips.label[actions[i].action];
        if(*label && strcmp(label,"(none)")) { key[n]=label; verb[n++]=actions[i].verb; }
    }
    if(keycap_tips(c,description,key,verb,n,n<count && !description && *fallback ? fallback : NULL,false,false)) return;
    uint16_t lines[2][41]={{0}};
    size_t used[2]={0};
    unsigned line=0;
    if(description && *description) {
        used[0]=instruction_line(lines[0],40,description);
        line=1;
    }
    bool overflow=false, missing=false;
    for(unsigned i=0;i<count;i++) {
        const char *label=tips.label[actions[i].action], *verb=actions[i].verb;
        if(!*label || !strcmp(label,"(none)")) { missing=true; continue; }
        uint16_t glyph=button_glyph(label);
        size_t length=(glyph ? 1 : strlen(label))+1+strlen(verb);
        if(used[line] && used[line]+2+length>40 && line==0) line++;
        if(used[line]+(used[line] ? 2 : 0)+length>40) { overflow=true; break; }
        uint16_t *out=lines[line]+used[line];
        if(used[line]) { *out++=' '; *out++=' '; }
        if(glyph) *out++=glyph;
        else while(*label) *out++=(unsigned char)*label++;
        *out++=' ';
        while(*verb) *out++=(unsigned char)*verb++;
        used[line]=out-lines[line];
    }
    if(!description && missing && used[0] && !used[1] && *fallback) {
        while(*fallback && used[1]<40) lines[1][used[1]++]=(unsigned char)*fallback++;
    } else if(overflow || (!used[0] && !used[1]) || (description && count && !used[1])) {
        if(!description) { used[0]=used[1]=0; line=0; }
        used[line]=0;
        while(*fallback && used[line]<40) lines[line][used[line]++]=(unsigned char)*fallback++;
    }
    hint_background(c,used[1]!=0,880);
    for(unsigned i=0;i<2;i++) {
        uint64_t pane=i ? panes->hint_second : panes->hint;
        if(i) call(c,0x70008ade74ull,pane,used[i]!=0,0,0,0);
        lines[i][used[i]]=0;
        memcpy((void *)(uintptr_t)(c->sp+128),lines[i],(used[i]+1)*2);
        call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
    }
}

static uint64_t attach(Cpu *c, uint64_t page, uint64_t parent, const char *name) {
    uint64_t obj = call(c,0x70005902d0ull,0xb8,4,0,0,0); heap(obj,0xb8);
    call(c,0x7000723cf4ull,obj,0,0,0,0);
    call(c,0x70008ad888ull,obj+0x30,0,0,0,0);
    uint64_t vt = LD64(0x70013083e0ull);
    ST64(obj,vt+0x10); ST64(obj+0x28,obj+0x30); ST64(obj+0x30,vt+0x60); ST64(obj+0x38,obj);
    ST32(c->sp+112,0);
    if(parent) call(c,0x70008bc0c4ull,page,obj+0x30,parent,string(c,name),c->sp+112);
    else call(c,0x70008bbf04ull,page,obj+0x30,string(c,name),c->sp+112,0);
    if (!LD64(obj+0x40)) FATAL("[mk8:settings] selector layout missing\n");
    return obj+0x30;
}

// The stock text box limit is in UTF-16 units. Service text can be much longer.
static void list_text(Cpu *c,uint64_t pane,const char *value,size_t limit) {
    uint16_t *line=(uint16_t *)(uintptr_t)(c->sp+128);
    size_t n=utf16_line(line,limit+2,value,false);
    for(size_t i=0;i<n;i++) if(line[i]<32) line[i]=' ';
    if(n>limit) {
        size_t cut=limit-1;
        if(line[cut]>=0xdc00 && line[cut]<=0xdfff && cut) --cut;
        line[cut]=0x2026; line[cut+1]=0;
    }
    call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
}

static Mk8Setting selected(void) {
    Mk8Setting row;
    if(!focus || !mk8_settings_values_row(category,focus-1,&row)) FATAL("[mk8:settings] invalid settings selection\n");
    return row;
}

static bool nextendo_page(void) { return menu_level==MENU_ROOMS || menu_level==MENU_ROOM_DETAILS || menu_level==MENU_FRIENDS; }
static const char *nextendo_tip(Mk8MenuLanguage lang);
static bool friend_row_focused(void);
static bool service_header;
static void enter_settings_category(Cpu *c,unsigned group);
static void settings_tips(Cpu *c) {
    bool stock_back=tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_BACK],"B");
    call(c,0x70008ade74ull,panes->back_icon,stock_back,0,0,0);
    static const struct TipAction category_tips[]={{MK8_TIP_CONFIRM,"Category"},{MK8_TIP_DOWN,"Options"},{MK8_TIP_BACK,"Back"}};
    static const struct TipAction save[]={{MK8_TIP_CONFIRM,"OK"},{MK8_TIP_BACK,"Cancel"}};
    static const struct TipAction reset[]={{MK8_TIP_CONFIRM,"Reset"},{MK8_TIP_BACK,"Cancel"}};
    struct TipAction row_tips[]={{MK8_TIP_CONFIRM,"Change"},{MK8_TIP_BACK,"Back"}};
    const char *help=NULL;
    char range[40];
    if(exiting) help="Saving settings...";
    else if(mk8_settings_values_saved()<0) help="Save failed. Check folder; Back retries.";
    else if(mk8_settings_values_capturing()) help="Waiting for a new binding...";
    if(help) { action_tips(c,help,NULL,0,""); return; }
    if(menu_level!=MENU_OPTIONS) {
        Mk8MenuLanguage lang=mk8_menu_language();
        struct TipAction actions[]={{MK8_TIP_CONFIRM,mk8_menu_text(lang,"Select","Selecionar","Elegir")},
            {MK8_TIP_BACK,menu_level==MENU_HOME ? mk8_menu_text(lang,"Close","Fechar","Cerrar") : mk8_menu_text(lang,"Back","Voltar","Volver")}};
        const char *description=menu_level==MENU_PLAY_ACTIONS ? mk8_menu_play_text(lang,&play_session,PLAY_INSTRUCTION) : NULL;
        bool detail=menu_level==MENU_ROOM_DETAILS;
        if(nextendo_page()) description=nextendo_tip(lang);
        if(nextendo_page() && !detail && service_header) actions[0]=(struct TipAction){MK8_TIP_CONFIRM,
            menu_level==MENU_ROOMS ? mk8_menu_text(lang,"View friends","Ver amigos","Ver amigos") : mk8_menu_text(lang,"View rooms","Ver salas","Ver salas")};
        else if(friend_row_focused()) actions[0]=(struct TipAction){MK8_TIP_DOWN,mk8_menu_text(lang,"Scroll","Rolar","Recorrer")};
        action_tips(c,description,actions+(detail ? 1 : 0),detail ? 1 : 2,""); return;
    }
    if(editing) {
        snprintf(range,sizeof range,"Enter a value from %d to %d.",edit_min,edit_max);
        help=invalid ? range : tips.source==MK8_TIP_KEYBOARD ? "Type a value or use the arrow keys." : "Up/down: 1   Left/right: 10";
        action_tips(c,help,save,2,""); return;
    }
    if(reset_confirm) { action_tips(c,"Reset these bindings?",reset,2,""); return; }
    if(!focus) { action_tips(c,NULL,category_tips,3,"Select a category or setting."); return; }
    if(rich_category()) {
        Mk8NativeRow row={0};
        mk8_native_rows_get(focus-1,rich_revision,&row);
        row_tips[0].verb=mk8_menu_text(mk8_menu_language(),"Select","Selecionar","Elegir");
        action_tips(c,row.help[0] ? row.help : NULL,row_tips+(row.enabled ? 0 : 1),row.enabled ? 2 : 1,"");
        return;
    }
    Mk8Setting row=selected();
    if(row.kind==SETTING_NUMBER) row_tips[0].verb="Edit";
    if(row.kind==SETTING_BINDING) row_tips[0].verb="Remap";
    if(row.kind==SETTING_RESET) row_tips[0].verb="Reset";
    unsigned first=row.enabled && row.kind!=SETTING_INFO ? 0 : 1;
    action_tips(c,row.help,row_tips+first,2-first,"");
}

static void controller_place(Cpu *c,uint64_t pane,float x,float y);
static void pane_scale(uint64_t pane,float scale) {
    heap(pane,0x60);
    memcpy((void *)(uintptr_t)(pane+0x48),&scale,sizeof scale);
    memcpy((void *)(uintptr_t)(pane+0x4c),&scale,sizeof scale);
    ST8(pane+0x58,LD8(pane+0x58)|0x10);
}

static void pulse_slot(Cpu *c,unsigned slot) {
    if(slot>SETTINGS_VISIBLE) return;
    if(pulse_pane) pane_scale(pulse_pane,1);
    char name[24]; snprintf(name,sizeof name,"L_SettingsRow%u",slot);
    pulse_pane=find(c,panes->root,name); pulse_at=plat_now_ns();
    pane_scale(pulse_pane,1.025f);
}

static void row_ink(uint64_t pane,bool actionable) {
    heap(pane,0x60);
    ST8(pane+0x59,actionable ? 255 : 155);
    ST8(pane+0x58,LD8(pane+0x58)|2);
}

static void draw_play_info(Cpu *c,Mk8MenuLanguage lang) {
    char line[80];
    snprintf(line,sizeof line,"%s  ·  %s",mk8_menu_play_text(lang,&play_session,PLAY_SESSION),mk8_menu_play_text(lang,&play_session,PLAY_VERSION));
    text(c,panes->values[0],line);
    visible(c,panes->rows[0]+0x30,"L_Btn_LR",false);
    visible(c,panes->root,"L_SettingsRow5",true);
    controller_place(c,find(c,panes->root,"L_SettingsRow5"),0,-155);
    call(c,0x700044ee50ull,panes->rows[5],1,0,0,0);
    row_ink(panes->labels[5],false); row_ink(panes->values[5],false);
    visible(c,panes->rows[5]+0x30,"L_Btn_LR",false);
    snprintf(line,sizeof line,"%s: %s",mk8_menu_label(lang,mk8_play_content_label),mk8_menu_play_text(lang,&play_session,PLAY_CONTENT));
    text(c,panes->labels[5],line);
    snprintf(line,sizeof line,"%s: %s",mk8_menu_label(lang,mk8_play_account_label),mk8_menu_play_text(lang,&play_session,PLAY_ACCOUNT));
    // The status column has room for 30 units. Keep an ellipsis clear of UTF-16 surrogate pairs.
    uint16_t *label=(uint16_t *)(uintptr_t)(c->sp+128);
    size_t length=utf16_line(label,79,line,false);
    if(play_session.signed_in) for(size_t i=0;i<length;i++)
        if(label[i]=='\r' || label[i]=='\n' || label[i]=='\t') label[i]=' ';
    if(play_session.signed_in && length>30) {
        size_t cut=29;
        if(label[cut]>=0xdc00 && label[cut]<=0xdfff) cut--;
        label[cut]=0x2026; label[cut+1]=0;
    }
    call(c,0x70008adc54ull,panes->values[5],c->sp+128,0,0,0);
}

#include "settings_nextendo.h"

static void enter_settings_category(Cpu *c,unsigned group) {
    (void)c;
    if(menu_level!=MENU_OPTIONS) settings_from_service=nextendo_page();
    if(rich_category()) mk8_native_rows_close();
    menu_level=MENU_OPTIONS;
    category=group==MENU_QUICK ? SETTINGS_QUICK : group<=MENU_CONTROLS ? group-MENU_VIDEO : group;
    focus=offset=rich_depth=rich_revision=rich_count=0;
    if(rich_category()) mk8_native_rows_open(group);
}

static void refresh_rich_rows(void) {
    unsigned depth;
    int count=mk8_native_rows_count(&rich_revision,&depth);
    if(depth!=rich_depth) {
        if(depth>rich_depth) {
            if(rich_depth<sizeof parent_focus/sizeof *parent_focus) {
                parent_focus[rich_depth]=focus; parent_offset[rich_depth]=offset;
            }
            focus=count ? 1 : 0; offset=0;
        } else {
            focus=depth<sizeof parent_focus/sizeof *parent_focus ? parent_focus[depth] : 0;
            offset=depth<sizeof parent_offset/sizeof *parent_offset ? parent_offset[depth] : 0;
        }
        rich_depth=depth;
    }
    rich_count=(unsigned)count;
}

static unsigned option_count(void) {
    return rich_category() ? rich_count : (unsigned)mk8_settings_values_count(category);
}

static unsigned menu_action_count(void) {
    if(nextendo_page()) return menu_level==MENU_ROOM_DETAILS ? room_detail_row_count() : service_count;
    return menu_level==MENU_HOME ? HOME_COUNT : play_count;
}
static void paint(Cpu *c) {
    bool options=menu_level==MENU_OPTIONS,play=menu_level==MENU_PLAY_ACTIONS;
    bool rooms=nextendo_page();
    if(panes->play_layout!=play || panes->nextendo_layout!=rooms) {
        // Play borrows the category heading and last row for read-only session details.
        controller_place(c,find(c,panes->root,"L_SettingsRow0"),0,play || rooms ? 174 : 150);
        visible(c,panes->rows[0]+0x30,"L_Btn_LR",!play && !rooms);
        pane_scale(panes->values[0],play || rooms ? .70f : 1.f);
        pane_scale(panes->labels[5],play ? .66f : 1.f);
        pane_scale(panes->values[5],play ? .66f : 1.f);
        controller_place(c,panes->values[5],play ? -418 : 0,0);
        if(play) controller_place(c,panes->labels[5],220,0);
        panes->play_layout=play; panes->nextendo_layout=rooms;
    }
    const char *category_name=mk8_menu_label(mk8_menu_language(),mk8_menu_groups[category_groups[category]]);
    const char *heading=menu_level==MENU_HOME ? mk8_menu_text(mk8_menu_language(),"Menu","Menu","Menú") :
        mk8_menu_label(mk8_menu_language(),mk8_menu_pages[play ? MENU_PLAY : MENU_SETTINGS]);
    if(rooms) heading=mk8_menu_label(mk8_menu_language(),menu_level==MENU_ROOM_DETAILS ? mk8_nx_room : mk8_nx_views[menu_level==MENU_FRIENDS]);
    if(panes->title_label) text(c,panes->title_label,heading);
    uint64_t shown_revision=mk8_settings_values_revision();
    unsigned count=options ? option_count() :
        menu_action_count();
    unsigned first=options ? 1 : 0;
    if(!count && !rich_category()) FATAL("[mk8:settings] settings catalogue unavailable\n");
    if(focus>=count+first) focus=count+first-1;
    if(focus>=first && focus-first<offset) offset=focus-first;
    if(focus>=offset+SETTINGS_VISIBLE+first) offset=focus-SETTINGS_VISIBLE-first+1;
    if(offset+SETTINGS_VISIBLE>count) offset=count>SETTINGS_VISIBLE ? count-SETTINGS_VISIBLE : 0;
    if(rooms && service_header) offset=0;
    visible(c,panes->root,"L_SettingsHint",true);
    visible(c,panes->root,"L_SettingsRow0",options || play || rooms);
    if(options) text(c,panes->values[0],category_name);
    call(c,((!play && !rooms && focus==0) || (rooms && service_header)) ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[0],1,0,0,0);
    for (unsigned i=1;i<=SETTINGS_VISIBLE;i++) {
        char name[32]; snprintf(name,sizeof name,"L_SettingsRow%u",i);
        bool shown=offset+i<=count; visible(c,panes->root,name,shown);
        if(!shown) continue;
        controller_place(c,find(c,panes->root,name),0,options ? 60-(int)(i-1)*60 : play || rooms ? 100-(int)(i-1)*60 : 145-(int)(i-1)*70);
        // The value pane sits 195 units right of center and holds up to 49 UTF-16 units.
        controller_place(c,panes->values[i],0,0);
        if(rooms) { draw_nextendo_row(c,i,offset+i-1); continue; }
        controller_place(c,panes->labels[i],options ? -245 : 0,0);
        if(!options) {
            unsigned index=offset+i-1;
            Mk8MenuLabel label;
            if(play) {
                enum PlayAction action=play_actions[index];
                label=action==PLAY_RESUME ? mk8_play_back : action==PLAY_NEXTENDO ? mk8_play_open_nextendo :
                    mk8_menu_launcher_actions[action-PLAY_MII].label;
            } else label=mk8_menu_pages[mk8_home_pages[index]];
            text(c,panes->labels[i],mk8_menu_label(mk8_menu_language(),label)); text(c,panes->values[i],"");
            row_ink(panes->labels[i],true);
            call(c,index==focus ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[i],1,0,0,0);
            visible(c,panes->rows[i]+0x30,"L_Btn_LR",false);
            continue;
        }
        if(rich_category()) {
            Mk8NativeRow row={0};
            mk8_native_rows_get(offset+i-1,rich_revision,&row);
            bool paired=row.value[0]!=0;
            if(!paired) controller_place(c,panes->values[i],-195,0);
            list_text(c,panes->labels[i],paired ? row.label : "",24);
            list_text(c,panes->values[i],paired ? row.value : row.label,paired ? 26 : 49);
            call(c,offset+i==focus ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[i],1,0,0,0);
            row_ink(panes->labels[i],row.enabled); row_ink(panes->values[i],row.enabled);
            visible(c,panes->rows[i]+0x30,"L_Btn_LR",row.enabled && row.adjustable);
            continue;
        }
        Mk8Setting row;
        if(!mk8_settings_values_row(category,offset+i-1,&row)) FATAL("[mk8:settings] settings row changed\n");
        text(c,panes->labels[i],row.label);
        if(editing && row.id==editing_id) {
            char value[32];
            snprintf(value,sizeof value,replace_number ? "[%s]" : "%s_",number);
            text(c,panes->values[i],value);
        } else text(c,panes->values[i],reset_confirm && offset+i==focus ? "Confirm reset?" : row.value);
        call(c,offset+i==focus ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[i],1,0,0,0);
        bool actionable=row.enabled && row.kind!=SETTING_INFO;
        row_ink(panes->labels[i],actionable); row_ink(panes->values[i],actionable);
        visible(c,panes->rows[i]+0x30,"L_Btn_LR",row.enabled && (row.kind==SETTING_CHOICE || row.kind==SETTING_NUMBER));
    }
    if(play) draw_play_info(c,mk8_menu_language());
    if(rooms) draw_nextendo_header(c);
    if(panes->scrollbar) {
        visible(c,panes->root,"L_SettingsScroll",count>SETTINGS_VISIBLE);
        float args[5]={0,(float)offset,(float)(count>SETTINGS_VISIBLE ? count-SETTINGS_VISIBLE : 0),SETTINGS_VISIBLE,(float)count};
        for(unsigned i=0;i<5;i++) memcpy(&c->v[i],&args[i],sizeof(float));
        call(c,0x7000451ff4ull,panes->scrollbar,0,0,0,0);
    }
    menu_cursor(c,!options && menu_level!=MENU_ROOM_DETAILS && !(rooms && service_header));
    settings_tips(c);
    last_revision=shown_revision;
    LOG("[mk8:settings] category=%s focus=%u offset=%u\n",options ? mk8_menu_groups[category_groups[category]].en : heading,focus,offset);
}

// The backdrop and Back button stay fixed while the content and its cursor travel together.
static void slide_content(Cpu *c,float x) {
    controller_place(c,find(c,panes->root,"N_Settings"),x,0);
    controller_place(c,find(c,panes->root,"L_SettingsBG"),-x,0);
    controller_place(c,find(c,panes->root,"L_SettingsBack"),-460-x,-280);
    if(panes->menu_cursor_active) controller_place(c,find(c,panes->root,"N_SettingsPopup"),x,0);
}

static void end_motion(Cpu *c) {
    if(pulse_pane) { pane_scale(pulse_pane,1); pulse_pane=0; }
    if(slide_direction) { slide_content(c,0); slide_direction=0; }
}

static void draw(Cpu *c) {
    int depth=menu_level==MENU_HOME ? 0 : 1;
    if(nextendo_page() || (menu_level==MENU_OPTIONS && settings_from_service)) {
        if(service_parent==MENU_PLAY_ACTIONS) ++depth;
        if(menu_level==MENU_ROOM_DETAILS || menu_level==MENU_OPTIONS) ++depth;
    }
    if(rich_category()) {
        refresh_rich_rows();
        if(category_groups[category]!=MENU_QUIT) depth+=(int)rich_depth;
    }
    if(painted_depth>=0 && painted_depth!=depth && active && !entering && !exiting && !restart_prompt) {
        slide_direction=depth>painted_depth ? 1 : -1;
        slide_at=plat_now_ns(); incoming=false; painted_depth=depth;
        return; // Keep the outgoing rows until they have left the viewport.
    }
    painted_depth=depth;
    if(!slide_direction) paint(c);
}

static bool animate_menu(Cpu *c) {
    uint64_t now=plat_now_ns();
    if(pulse_pane) {
        float t=(float)(now-pulse_at)/120000000.f;
        if(t>=1) { pane_scale(pulse_pane,1); pulse_pane=0; }
        else pane_scale(pulse_pane,1+.025f*(1-t)*(1+2*t));
    }
    if(!slide_direction) return false;
    uint64_t age=now-slide_at;
    if(age>=160000000ull) {
        if(!incoming) paint(c);
        end_motion(c);
        return true;
    }
    bool arriving=age>=72000000ull;
    if(arriving && !incoming) {
        if(pulse_pane) { pane_scale(pulse_pane,1); pulse_pane=0; }
        paint(c); incoming=true;
    }
    float t=arriving ? (age-72000000ull)/88000000.f : age/72000000.f;
    float eased=t*t*(3-2*t);
    slide_content(c,1280*slide_direction*(arriving ? 1-eased : -eased));
    return true;
}

static bool post(int operation,int id,int value) {
    return mk8_settings_values_request(operation,id,value);
}

static void animation_frame(Cpu *c,uint64_t animation,unsigned index,float frame) {
    memcpy(&c->v[0],&frame,sizeof frame);
    call(c,0x70008ad3c4ull,animation,index,0,0,0);
}

static void animation_play(Cpu *c,uint64_t animation,unsigned index,float speed) {
    memcpy(&c->v[0],&speed,sizeof speed);
    call(c,0x70008ad6acull,animation,index,0,1,0);
}

static void menu_back_feedback(Cpu *c) {
    animation_play(c,panes->back_animation,2,1);
    menu_sound(c,0x7000ed87cfull);
}

static uint64_t button_animations(Cpu *c,uint64_t component) {
    call(c,0x70008ae7b4ull,component,1,0,0,0);
    uint64_t animation=call(c,0x70008ae888ull,component,0,3,0,0);
    heap(animation,0x30);
    // OnOff toca nos dois sentidos, como nos controles originais.
    for(unsigned i=0;i<3;i++) call(c,0x70008ad158ull,animation,i,string(c,i==2 ? "Decide" : "OnOff"),0,0);
    animation_frame(c,animation,0,0);
    return animation;
}

// The stock automatic applet establishes the player count before character selection.
void mk8_controller_applet_result(int min,int max,int players) {
    controller_applet_players=min>=2 && min<=4 && min==max && min==players ? (unsigned)min : 0;
}

bool mk8_controller_layout_active(void) { return !controller_detached && (controller_preview || controller_visible); }
bool mk8_controller_join_visible(void) { return controller_required && controller_visible && !controller_detached; }
bool mk8_controller_pointer_available(void) { return controller_entry_players || controller_mouse_down || mk8_controller_layout_active(); }

static bool controller_can_open(void) {
    return controller_entry_players && controller_focused && !requested && !active &&
           !mk8_controller_layout_active() && !menu_input_captured();
}

static void controller_request_editor(void) {
    controller_armed=controller_click=-1; controller_hover=-2;
    controller_preview=true;
}

bool mk8_controller_pointer(int x,int y,int action) {
    if (action==3 || action==4) {
        controller_focused=action==4; controller_resync=true;
        controller_armed=controller_click=-1; controller_hover=-1;
        return false;
    }
    if (!mk8_controller_layout_active()) {
        // B_MP_00 in the local CharaParts layout: 260 x 64 at 0.8 scale, centered at (640,643).
        bool hit=x>=536 && x<744 && y>=618 && y<=668;
        enum { SHORTCUT=6 };
        if (action==1) {
            bool capture=hit && controller_character_shortcut && controller_can_open();
            controller_mouse_down=capture; controller_armed=capture ? SHORTCUT : -1;
            return capture;
        }
        if (!controller_mouse_down) return false;
        if (action==0 && !hit) controller_armed=-1;
        if (action==2) {
            controller_mouse_down=false;
            if (atomic_exchange(&controller_armed,-1)==SHORTCUT && hit && controller_character_shortcut && controller_can_open()) controller_request_editor();
        }
        return true;
    }
    if (action==1) controller_mouse_down=true;
    if (action==2 && !atomic_exchange(&controller_mouse_down,false)) return false;
    if (!controller_focused) return true;
    int hit=-1;
    if(x>=200 && x<=1080 && y>=185 && y<=455 && (y-185)%70<=60) hit=(y-185)/70;
    if (controller_required && y>=561 && y<=651) {
        // Keep a press on Cancel from confirming OK if the last player joins before release.
        if (x>=482 && x<=798) hit=controller_ready_button ? 5 : 4;
    } else if (y>=561 && y<=651) {
        if (x>=282 && x<=598) hit=4;
        if (x>=682 && x<=998) hit=5;
    }
    controller_hover=hit;
    if (action==1) controller_armed=hit;
    if (action==0 && hit!=controller_armed) controller_armed=-1;
    if (action==2 && atomic_exchange(&controller_armed,-1)==hit && hit>=0) controller_click=hit;
    return true;
}

static void controller_place(Cpu *c, uint64_t pane, float x,float y) {
    float translation[3]={x,y,0};
    memcpy((void *)(uintptr_t)(c->sp+320),translation,sizeof translation);
    call(c,0x70008ade34ull,pane,c->sp+320,0,0,0);
}

static void controller_cursor_scale(Cpu *c,float scale) {
    pane_scale(find(c,panes->root,"L_SettingsCursor"),scale);
}

static void position_cursor(Cpu *c,float x,float y,bool row) {
    // Keep the four native corners apart; shrinking the cursor bounds alone makes them cross.
    float scale=row ? .5f : 1.f;
    controller_cursor_scale(c,scale);
    float position[3]={x,y,0};
    float size[2]={row ? 1700 : 370,158};
    memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
    call(c,0x7000443404ull,panes->dialog_cursor,c->sp+320,0,0,0);
    memcpy((void *)(uintptr_t)(c->sp+320),position,sizeof position);
    call(c,0x70008adff0ull,panes->dialog_cursor+0x30,c->sp+320,0,0,0);
}

static void menu_cursor(Cpu *c,bool show) {
    if(!panes->dialog_cursor) return;
    if(panes->menu_cursor_active!=show) {
        static const char *const dialog_parts[]={"L_SettingsDialog","L_SettingsQuestion","L_SettingsYes","L_SettingsNo"};
        for(unsigned i=0;i<sizeof dialog_parts/sizeof *dialog_parts;i++) visible(c,panes->root,dialog_parts[i],!show);
        visible(c,panes->root,"N_SettingsPopup",show);
        panes->menu_cursor_active=show;
        if(show) {
            reveal_pane(c,"N_SettingsPopup",reveal);
            call(c,0x7000442f5cull,panes->dialog_cursor,1,0,0,0);
            float speed=1; memcpy(&c->v[0],&speed,sizeof speed);
            call(c,0x70008ad300ull,panes->cursor_animation,1,0,0,0);
        } else position_cursor(c,0,-195,false);
    }
    if(show) position_cursor(c,0,(menu_level==MENU_PLAY_ACTIONS || nextendo_page()) ? 100-(int)(focus-offset)*60 : 145-(int)(focus-offset)*70,true);
}

static void controller_highlight(Cpu *c) {
    bool footer=controller_focus>=4;
    for(unsigned i=0;i<4;i++)
        call(c,controller_focus==(int)i ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[i+1],1,0,0,0);
    for(unsigned i=0;i<2;i++) {
        animation_frame(c,panes->dialog_animations[i],0,controller_focus==(int)i+4 ? 2 : 0);
        // Apply the OnOff pose now, including the neutral colors after the previous Decide pulse.
        call(c,0x70008ae0b8ull,panes->dialog_buttons[i],0,0,0,0);
    }
    position_cursor(c,footer && !controller_required ? (controller_focus==4 ? -200 : 200) : 0,
                    footer ? -246 : 145-controller_focus*70,!footer);
}

static bool controller_listening(enum Mk8EditState state) {
    return state==MK8_EDIT_LISTEN || state==MK8_EDIT_NEED_LEFT || state==MK8_EDIT_NEED_RIGHT ||
           state==MK8_EDIT_AMBIGUOUS || state==MK8_EDIT_PAIR_HELD;
}

static void controller_step(Cpu *c,enum Mk8EditState state,int listening) {
    uint64_t held=mk8_controller_p1_navigation(), edge=held&~controller_previous;
    controller_previous=held;
    if (atomic_exchange(&controller_resync,false) || !controller_focused) return;
    bool enrolling=controller_listening(state);
    int old=controller_focus, count=controller_panel==ROW_ACTIONS ? 4 : (int)controller_rows;
    int hover=atomic_exchange(&controller_hover,-2), click=atomic_exchange(&controller_click,-1);
    if(hover>=0 && hover<count && !enrolling) controller_focus=hover;
    if(hover==4 || (hover==5 && controller_panel!=SWAP_TARGET)) controller_focus=hover;
    if (hover==-1 && controller_focus>=4) controller_focus=0;
    if (edge&(1ull<<13)) controller_focus=controller_focus>=4 ? count-1 : controller_focus>0 ? controller_focus-1 : 0;
    if ((edge&(1ull<<15)) && controller_focus<4)
        controller_focus=controller_focus+1<count ? controller_focus+1 : 4;
    if (edge&(1ull<<12)) controller_focus=4;
    if ((edge&(1ull<<14)) && controller_panel!=SWAP_TARGET) controller_focus=5;
    if(enrolling && controller_focus<4) controller_focus=listening>=0 && listening<count ? listening : 0;
    if(click>=0 && click<count && !enrolling) controller_focus=click;
    if(click==5 && controller_panel==SWAP_TARGET) click=-1;
    if(click>=4) controller_focus=click;
    bool confirm=(edge&1) || (click>=0 && (click<count || click>=4));
    bool back=(edge&2) || (confirm && controller_focus==4);
    bool staged=false;
    if(back) {
        if(enrolling) {
            staged=mk8_edit_command(MK8_EDIT_STOP_LISTEN,0,0);
            controller_focus=listening>=0 ? listening : 0;
        } else if(controller_panel!=PLAYERS) {
            controller_panel=PLAYERS; controller_focus=action_player;
        } else {
            controller_focus=4; controller_cancelled=mk8_edit_command(MK8_EDIT_CANCEL,0,0);
        }
    } else if(state!=MK8_EDIT_PENDING && !controller_cancelled) {
        if(enrolling) {
            if(confirm && controller_focus==5 && listening>=0) {
                staged=mk8_edit_command(MK8_EDIT_KEYBOARD,listening,0); controller_focus=listening;
            }
        } else if(controller_panel==ROW_ACTIONS && confirm) {
            static const enum Mk8EditCommand actions[]={MK8_EDIT_ASSIGN,MK8_EDIT_KEYBOARD,MK8_EDIT_SWAP,MK8_EDIT_CLEAR};
            enum Mk8EditCommand cmd=controller_focus==5 ? MK8_EDIT_CLEAR_ALL : actions[controller_focus];
            controller_panel=cmd==MK8_EDIT_SWAP ? SWAP_TARGET : PLAYERS;
            controller_focus=action_player;
            if(cmd!=MK8_EDIT_SWAP) staged=mk8_edit_command(cmd,action_player,0);
        } else if(controller_panel==SWAP_TARGET && confirm && controller_focus<count) {
            if(controller_focus!=action_player) {
                staged=mk8_edit_command(MK8_EDIT_SWAP,action_player,controller_focus);
                if(staged) controller_panel=PLAYERS;
            }
        } else if(controller_panel==PLAYERS) {
            if(edge&(1ull<<11)) staged=mk8_edit_command(MK8_EDIT_CLEAR_ALL,0,0);
            else if(confirm && controller_focus==5) mk8_edit_command(MK8_EDIT_APPLY,0,0);
            else if(controller_focus<count) {
                action_player=controller_focus;
                if(click>=0) { controller_panel=ROW_ACTIONS; controller_focus=0; }
                else if(confirm) staged=mk8_edit_command(MK8_EDIT_ASSIGN,action_player,0);
                else if(edge&4) controller_panel=SWAP_TARGET;
                else if(edge&8) staged=mk8_edit_command(MK8_EDIT_CLEAR,action_player,0);
            }
        }
    }
    if(staged) menu_sound(c,0x7000ed87bfull);
    if((click>=4 || back) && (staged || (back && !controller_cancelled && controller_panel==PLAYERS))) {
        animation_play(c,panes->dialog_animations[click==5],2,1);
    }
    if(old!=controller_focus) {
        controller_highlight(c);
        menu_sound(c,0x7000ed87a1ull);
    }
}

static void controller_release(void) {
    controller_press=-1;
    controller_fade_at=0;
    enum Mk8EditState state=mk8_edit_view(NULL,NULL);
    if(!controller_required && state!=MK8_EDIT_CLOSED && state!=MK8_EDIT_APPLIED) mk8_edit_command(MK8_EDIT_CANCEL,0,0);
    uint64_t held=mk8_controller_p1_navigation();
    suppressed|=raw_buttons|held|((held&0xf000u)<<4);
    mk8_controller_hold_extra_inputs();
    controller_armed=controller_click=-1;
    controller_preview=controller_visible=false;
    controller_required=controller_detached=false;
}

static void controller_labels(Cpu *c, const uint16_t names[4][80],bool refresh) {
    static uint16_t shown[4][80];
    for (unsigned i=0;i<controller_rows;i++) {
        if (!refresh && !memcmp(shown[i],names[i],sizeof shown[i])) continue;
        memcpy(shown[i],names[i],sizeof shown[i]);
        memcpy((void *)(uintptr_t)(c->sp+128),names[i],sizeof names[i]);
        call(c,0x70008adc54ull,panes->values[i+1],c->sp+128,0,0,0);
    }
}

static void controller_draw(Cpu *c,enum Mk8EditState state,int listening,const uint16_t names[4][80],bool refresh) {
    static enum ControllerPanel last_panel;
    static enum Mk8EditState last_state;
    static int previous_player;
    bool enrolling=controller_listening(state), combined=enrolling && mk8_edit_has_combined();
    bool keyboard=controller_required && mk8_controller_join_keyboard();
    static bool last_combined,last_keyboard;
    int player=enrolling ? listening : controller_panel==PLAYERS ? -1 : action_player;
    bool source_changed=read_tips(false);
    bool changed=refresh || source_changed || last_panel!=controller_panel || last_state!=state || previous_player!=player ||
        combined!=last_combined || keyboard!=last_keyboard;
    if(changed) {
        const char *heading="Controller assignment", *help=NULL;
        const char *left="Cancel", *right="Apply";
        char title[40];
        if(controller_panel==ROW_ACTIONS) {
            snprintf(title,sizeof title,"Player %d options",action_player+1);
            heading=title; help="Select an action for this player."; left="Back"; right="Clear all";
        } else if(controller_panel==SWAP_TARGET) {
            snprintf(title,sizeof title,"Swap Player %d with...",action_player+1);
            heading=title; help="Select another player to swap inputs."; left="Back";
        } else if(enrolling) {
            const char *format=panes->symbol_help && (state==MK8_EDIT_NEED_LEFT || state==MK8_EDIT_NEED_RIGHT) ? "Player %d: pair Joy-Cons" :
                state==MK8_EDIT_LISTEN && panes->join_buttons[0] ? "Player %d: choose how to join" :
                panes->keycaps[0] ? "Player %d: [L] + [R] to join" : "Player %d: [L] + [R] or SL + SR";
            snprintf(title,sizeof title,format,listening+1);
            heading=title; left="Back"; right="Use keyboard";
            switch(state) {
            case MK8_EDIT_AMBIGUOUS:
                snprintf(title,sizeof title,"Player %d: multiple Joy-Cons",listening+1);
                help=panes->symbol_help ? "Release both. Pair one set at a time." : "Release all. One player holds [L] + [R]."; break;
            case MK8_EDIT_PAIR_HELD:
                snprintf(title,sizeof title,"Player %d: Joy-Con pair ready",listening+1);
                help="Release both shoulder buttons to join."; break;
            case MK8_EDIT_NEED_LEFT: help="Hold [R]. Press [L] on the left Joy-Con."; break;
            case MK8_EDIT_NEED_RIGHT: help="Hold [L]. Press [R] on the right Joy-Con."; break;
            default: help=combined && !controller_required ? "For separate halves, use F1: Individual." :
                "To pair two Joy-Cons, hold L and R."; break;
            }
        } else if(state==MK8_EDIT_FAILED) help="Assign every player a connected input.";
        else if(state==MK8_EDIT_PENDING) help="Updating controller assignments...";
        if(controller_required) {
            controller_ready_button=state==MK8_EDIT_READY;
            left=controller_ready_button ? "OK" : "Cancel";
            if(state==MK8_EDIT_READY) help="All players are ready.";
        }
        instruction(c,panes->title_label,40,heading);
        const struct TipAction roster[]={{MK8_TIP_CONFIRM,"Assign"},{MK8_TIP_SWAP,"Swap"},
            {MK8_TIP_CLEAR,tips.source==MK8_TIP_KEYBOARD ? "Clear row" : "Clear"},{MK8_TIP_CLEAR_ALL,"Clear all"}};
        static const struct TipAction submenu[]={{MK8_TIP_CONFIRM,"Select"},{MK8_TIP_BACK,"Back"}};
        static const struct TipAction ready={MK8_TIP_CONFIRM,"OK"};
        const struct TipAction back_tip={MK8_TIP_BACK,controller_required ? "Cancel" : "Back"};
        if(!help) action_tips(c,NULL,roster,4,"Click a player for assignment options.");
        else if(controller_panel!=PLAYERS) action_tips(c,help,submenu,2,"Click an action or Back.");
        else if(controller_required && state==MK8_EDIT_READY) action_tips(c,help,&ready,1,"Click OK.");
        else if(state==MK8_EDIT_LISTEN && panes->keycaps[0]) {
            const char *key[]={"SL","SR",keyboard ? "Enter" : tips.label[MK8_TIP_BACK]};
            bool chords=panes->join_buttons[0]!=0;
            const char *verb[]={chords ? "Controller or paired Joy-Cons" : "+",chords ? "Sideways Joy-Con" : "Join sideways",
                                keyboard ? "Join keyboard" : back_tip.verb};
            bool third=keyboard || ((panes->symbol_help || tips.source==MK8_TIP_KEYBOARD) && *key[2] && strcmp(key[2],"(none)"));
            keycap_tips(c,chords ? NULL : help,key,verb,third ? 3 : 2,NULL,chords,keyboard);
        }
        else if(panes->symbol_help && (state==MK8_EDIT_NEED_LEFT || state==MK8_EDIT_NEED_RIGHT)) {
            const char *keys[]={keyboard ? "Enter" : tips.label[MK8_TIP_BACK]};
            const char *verbs[]={keyboard ? "Join keyboard" : back_tip.verb};
            bool bound=*keys[0] && strcmp(keys[0],"(none)");
            keycap_tips(c,"",keys,verbs,bound,NULL,false,keyboard);
            hint_background(c,true,880);
            call(c,0x70008ade74ull,panes->hint_second,false,0,0,0);
            float y=hint_text_y(true,false);
            call(c,0x70008ade74ull,panes->action_labels[3],true,0,0,0);
            hint_box(c,panes->action_labels[3],-205,y,96,50);
            text(c,panes->action_labels[3],"Press");
            show_symbol(c,3,state==MK8_EDIT_NEED_LEFT ? 4 : 5,-134,y);
            hint_box(c,panes->hint,77,y,350,50);
            text(c,panes->hint,"on other Joy-Con to pair.");
        }
        else if(enrolling && keyboard) {
            const char *key[]={"Enter"}, *verb[]={"Join keyboard"};
            if(!keycap_tips(c,help,key,verb,1,NULL,false,true))
                action_tips(c,"Enter joins with the keyboard.",&back_tip,1,"Esc cancels.");
        }
        else if(enrolling) action_tips(c,help,&back_tip,1,"Click Back.");
        else action_tips(c,help,NULL,0,"");
        const char *footer_key=tips.label[controller_required && state==MK8_EDIT_READY ? MK8_TIP_CONFIRM : MK8_TIP_BACK];
        bool footer_binding=enrolling || (controller_required && state==MK8_EDIT_READY);
        uint16_t footer_glyph=footer_binding ? button_glyph(footer_key) : 0;
        int mapped_back=footer_binding && panes->symbol_dialog ? symbol_index(footer_key) : -1;
        if(panes->symbol_dialog) {
            hide_symbol(c,4);
            hint_box(c,panes->dialog_labels[1],mapped_back>=0 ? 20 : 0,-5,900,160);
        }
        if(mapped_back>=0) {
            show_symbol(c,4,mapped_back,-10-8*(float)strlen(left),0);
            text(c,panes->dialog_labels[1],left);
        } else if(panes->symbol_dialog && footer_binding && tips.source!=MK8_TIP_KEYBOARD &&
                  *footer_key && strcmp(footer_key,"(none)")) {
            char button[64];
            snprintf(button,sizeof button,"%s %s",footer_key,left);
            text(c,panes->dialog_labels[1],button);
        } else if(footer_glyph && !panes->symbol_dialog) {
            uint16_t *line=(uint16_t *)(uintptr_t)(c->sp+128);
            line[0]=footer_glyph; line[1]=' '; instruction_line(line+2,38,left);
            call(c,0x70008adc54ull,panes->dialog_labels[1],c->sp+128,0,0,0);
        } else text(c,panes->dialog_labels[1],left);
        text(c,panes->dialog_labels[2],right);
        visible(c,panes->root,"L_SettingsNo",!controller_required && controller_panel!=SWAP_TARGET);
        static const char *actions[]={"Assign controller","Use keyboard","Swap with player","Clear player"};
        for(unsigned i=0;i<4;i++) {
            char pane[20],label[16];
            snprintf(pane,sizeof pane,"L_SettingsRow%u",i+1);
            bool used=controller_panel==ROW_ACTIONS || i<controller_rows;
            visible(c,panes->root,pane,used);
            if(!used) continue;
            snprintf(label,sizeof label,"Player %u",i+1);
            controller_place(c,panes->labels[i+1],controller_panel==ROW_ACTIONS ? 0 : -245,0);
            text(c,panes->labels[i+1],controller_panel==ROW_ACTIONS ? actions[i] : label);
            if(controller_panel==ROW_ACTIONS) text(c,panes->values[i+1],"");
        }
        last_panel=controller_panel; last_state=state; previous_player=player; last_combined=combined; last_keyboard=keyboard;
        controller_highlight(c);
    }
    if(controller_panel!=ROW_ACTIONS) controller_labels(c,names,changed);
}

static void controller_layout(Cpu *c,bool required) {
    bool show=controller_preview || required;
    if(show && !controller_visible) {
        controller_focus=action_player=0; controller_panel=PLAYERS; controller_cancelled=false;
        controller_required=required;
        if(required) { controller_armed=controller_click=-1; controller_hover=-2; }
        if(!required && !mk8_edit_command(MK8_EDIT_BEGIN,(int)controller_rows,0)) { controller_release(); return; }
    }
    uint16_t names[4][80]; int listening=-1;
    enum Mk8EditState state=mk8_edit_view(names,&listening);
    bool accepted=state==MK8_EDIT_APPLIED;
    bool pad_back=controller_required && mk8_controller_join_back_pressed();
    bool closing=controller_visible && !controller_detached && (accepted || pad_back ||
        (state==MK8_EDIT_CLOSED && (controller_cancelled || (controller_required && !required))));
    if(closing) {
        bool required_cancel=controller_required && !accepted;
        if(controller_press<0) {
            controller_press=controller_required || !accepted ? 0 : 1;
            if(!controller_required || accepted || pad_back)
                controller_focus=4+controller_press;
            controller_highlight(c);
            animation_play(c,panes->dialog_animations[controller_press],2,1);
            if(controller_focus>=4) call(c,0x7000443320ull,panes->dialog_cursor,0,0,0,0);
            // Character selection plays Back when the mandatory applet returns ControllerSupportCanceled.
            if(!required_cancel) menu_sound(c,accepted ? 0x7000ed87bfull : 0x7000ed87cfull);
            if(!controller_required) return;
        }
        if(!controller_required && !call(c,0x70008ad59cull,panes->dialog_animations[controller_press],0,0,0,0)) return;
        bool faded=popup_fade(c,&controller_fade_at,true);
        if(controller_required && accepted) {
            // The applet waits for us. Sample the stock eight-frame Decide pulse on the fade clock.
            float frame=1.f+(1.f-reveal)*9.6f;
            animation_frame(c,panes->dialog_animations[0],2,frame<8.f ? frame : 8.f);
            call(c,0x70008ae0b8ull,panes->dialog_buttons[0],0,0,0,0);
        }
        if(!faded || (controller_required && required)) return;
        show=controller_preview=false;
    } else if(controller_visible && required && (state==MK8_EDIT_PENDING || state==MK8_EDIT_CLOSED)) {
        return;
    } else if(show && required) {
        // SDL owns enrollment, Apply and Back while the applet's guest worker waits.
        int hover=atomic_load(&controller_hover), click=atomic_exchange(&controller_click,-1);
        bool ready=state==MK8_EDIT_READY;
        int target=ready || (hover==4 && tips.source==MK8_TIP_KEYBOARD) ? 4 : listening>=0 ? listening : 0;
        if(controller_visible && controller_focused && state!=MK8_EDIT_PENDING) {
            if(click==4 && !ready) mk8_edit_command(MK8_EDIT_CANCEL,0,0);
            if(click==5 && ready) mk8_edit_command(MK8_EDIT_APPLY,0,0);
        }
        if(target!=controller_focus) {
            controller_focus=target;
            if(controller_visible) { controller_highlight(c); menu_sound(c,0x7000ed87a1ull); }
        }
    } else if(show && controller_visible) controller_step(c,state,listening);
    if(show==controller_visible) {
        if(show) controller_draw(c,state,listening,names,false);
        return;
    }
    if(show) stock_cursors(c,SAVE_CURSORS);
    stock_cursors(c,show ? HIDE_CURSORS : RESTORE_CURSORS);
    if(!show) text(c,panes->title_label,"Settings");
    visible(c,panes->root,"L_SettingsRow0",!show);
    visible(c,panes->root,"L_SettingsBack",!show);
    visible(c,panes->root,"L_SettingsDialog",!show);
    visible(c,panes->root,"L_SettingsQuestion",!show);
    visible(c,panes->root,"L_SettingsScroll",false);
    for(unsigned i=1;i<=SETTINGS_VISIBLE;i++) {
        char name[24];
        snprintf(name,sizeof name,"L_SettingsRow%u",i);
        visible(c,panes->root,name,!show || i<=controller_rows);
        controller_place(c,find(c,panes->root,name),0,show ? 145-(int)(i-1)*70 : 60-(int)(i-1)*60);
        if(!show) controller_place(c,panes->labels[i],-245,0);
        if(!show || i>4) continue;
        visible(c,panes->rows[i]+0x30,"L_Btn_LR",false);
        call(c,i==1 ? 0x700044ed20ull : 0x700044ee50ull,panes->rows[i],1,0,0,0);
    }
    controller_place(c,find(c,panes->root,"L_SettingsHint"),0,show ? -115 : -235);
    controller_place(c,find(c,panes->root,"L_SettingsYes"),show && required ? 0 : -200,show ? -246 : -195);
    controller_place(c,find(c,panes->root,"L_SettingsNo"),200,show ? -246 : -195);
    if(show) {
        appear(c,1);
        reveal_pane(c,"N_SettingsPopup",1);
        visible(c,panes->root,"N_Settings",true);
        visible(c,panes->root,"N_SettingsPopup",true);
        dialog_open_sound(c);
        controller_previous=mk8_controller_p1_navigation();
        controller_draw(c,state,listening,names,true);
        call(c,0x7000442f5cull,panes->dialog_cursor,1,0,0,0);
        float speed=1; memcpy(&c->v[0],&speed,sizeof speed);
        call(c,0x70008ad300ull,panes->cursor_animation,1,0,0,0);
    } else {
        visible(c,panes->root,"N_Settings",false);
        visible(c,panes->root,"N_SettingsPopup",false);
        controller_release();
        visible(c,panes->root,"L_SettingsNo",true);
        controller_cursor_scale(c,1.f);
        float size[2]={370,158};
        memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
        call(c,0x7000443404ull,panes->dialog_cursor,c->sp+320,0,0,0);
    }
    controller_visible=show;
}

static void dialog_draw(Cpu *c,bool instant) {
    static const char *messages[]={"Some settings require a restart.\nRestart now?","Yes","No"};
    if(panes->symbol_dialog) {
        hide_symbol(c,4);
        hint_box(c,panes->dialog_labels[1],0,-5,900,160);
    }
    for(unsigned i=0;i<3;i++) if(panes->dialog_labels[i]) text(c,panes->dialog_labels[i],messages[i]);
    for(unsigned i=0;i<2;i++) {
        bool selected=(i==0)==restart_yes;
        if(instant) animation_frame(c,panes->dialog_animations[i],0,selected ? 2 : 0);
        else animation_play(c,panes->dialog_animations[i],selected ? 0 : 1,selected ? 1 : -1);
    }
    float position[3]={restart_yes ? -200 : 200,-195,0};
    memcpy((void *)(uintptr_t)(c->sp+320),position,sizeof position);
    call(c,0x70008adff0ull,panes->dialog_cursor+0x30,c->sp+320,0,0,0);
}

static void finish_return(Cpu *c,uint64_t page,uint64_t b) {
    (void)page;
    end_motion(c);
    mk8_native_rows_close();
    menu_cursor(c,false);
    visible(c,panes->root,"N_Settings",false);
    panes->visible=false;
    if(panes->dialog) visible(c,panes->root,"N_SettingsPopup",false);
    restart_prompt=restart_wait=restart_pressed=exit_pressed=false;
    if(top_root) visible(c,top_root,"N_Btn_00",true);
    if(selector_absent) restore_menu_cursors=cursor_owner!=0;
    stock_cursors(c,RESTORE_CURSORS);
    if(from_button) mk8_settings_button_restore(top);
    else if(restore_nav) {
        call(c,0x70008afc54ull,restore_nav,0,0,0,0);
        call(c,0x70008afdd0ull,restore_nav,0,0,0,0);
    }
    top_root=restore_nav=0; suppressed=raw_buttons|b; active=false; exiting=false;
    input_reset();
    if(mk8_settings_values_saved()==2) LOG("[mk8:settings] native selector closed and saved\n");
    else LOG("[mk8:settings] native selector closed\n");
}

static void start_exit(Cpu *c,bool resume) {
    if(post(SETTINGS_FINISH,0,1)) {
        end_motion(c);
        exiting=true; exited_at=plat_now_ns(); exit_from=reveal;
        exit_pressed=!resume && reveal>0;
        if(exit_pressed) menu_back_feedback(c);
        else if(!resume) menu_sound(c,0x7000ed87cfull);
        entering=confirming=false; draw(c);
    }
}

static void finish_play_action(Cpu *c);
static void begin_play_action(Cpu *c,enum PlayAction action) {
    play_pending=action; play_pressed_at=plat_now_ns();
    if(action!=PLAY_LEAVE) pulse_slot(c,focus-offset+1);
    if(action==PLAY_LEAVE) menu_back_feedback(c);
    else {
        if(panes->dialog_cursor) call(c,0x7000443320ull,panes->dialog_cursor,0,0,0,0);
        menu_sound(c,0x7000ed87bfull);
    }
    if(action==PLAY_ENTER || action==PLAY_LEAVE || action==PLAY_NEXTENDO) finish_play_action(c);
}

static void finish_play_action(Cpu *c) {
    enum PlayAction action=play_pending;
    uint64_t elapsed=plat_now_ns()-play_pressed_at;
    if(action!=PLAY_ENTER && action!=PLAY_NEXTENDO && action!=PLAY_LEAVE && elapsed<160000000ull) {
        if(action==PLAY_RESUME) appear(c,1.f-(float)elapsed/160000000.f);
        return;
    }
    play_pending=PLAY_IDLE;
    if(action==PLAY_RESUME) {
        start_exit(c,true);
        if(exiting) { exit_from=0; return; }
    } else if(action==PLAY_ENTER) {
        menu_level=MENU_PLAY_ACTIONS; focus=offset=0;
        play_actions[0]=PLAY_RESUME; play_actions[1]=PLAY_NEXTENDO; play_count=2;
        for(int i=0;i<MENU_LAUNCHER_ACTION_COUNT;i++)
            if(mk8_menu_launcher_actions[i].available()) play_actions[play_count++]=(enum PlayAction)(PLAY_MII+i);
        mk8_menu_play_session(&play_session); play_checked_at=plat_now_ns();
    } else if(action==PLAY_LEAVE) {
        menu_level=MENU_HOME; focus=MENU_PLAY; offset=0;
    } else if(action==PLAY_NEXTENDO) {
        enter_service(MENU_PLAY_ACTIONS,MENU_ROOMS);
    } else {
        const Mk8MenuLauncherAction *launcher=&mk8_menu_launcher_actions[action-PLAY_MII];
        if(launcher->available()) launcher->open();
    }
    // The Decide animation collapses the chevrons. Re-arm them even when staying on Play.
    menu_cursor(c,false); appear(c,1); draw(c);
}

void mk8_settings_page_build(uint64_t page) { top=page; overlay_wrench=false; }

static void build_widgets(Cpu *c,uint64_t page) {
    Cpu saved=*c; c->sp-=512;
    panes->page=page;
    panes->root=attach(c,page,0,"L_SettingsRoot"); heap(panes->root,0x88);
    uint64_t title=attach(c,page,panes->root,"L_SettingsTitle");
    panes->title_label=find(c,title,"T_Title_00"); text(c,panes->title_label,"Settings");
    uint64_t help=attach(c,page,panes->root,"L_SettingsHint");
    panes->symbol_help=call(c,0x70008ad8e8ull,help,string(c,"P_Map0A"),0,0,0) ? help : 0;
    panes->symbol_dialog=0;
    memset(panes->symbol_parts,0,sizeof panes->symbol_parts); memset(panes->shown_symbols,0,sizeof panes->shown_symbols);
    panes->hint=find(c,help,"T_Title_00"); panes->hint_second=find(c,help,"T_Title_01");
    panes->hint_plate=find(c,help,"W_Title_00");
    panes->hint_double=call(c,0x70008ad8e8ull,help,string(c,"W_HintDouble"),0,0,0);
    if(panes->hint_double) panes->hint_single=find(c,help,"W_HintSingle");
    panes->keycaps[0]=call(c,0x70008ad8e8ull,help,string(c,"P_Key0"),0,0,0);
    if(panes->keycaps[0]) for(unsigned i=0;i<4;i++) {
        char name[16];
        snprintf(name,sizeof name,"P_Key%u",i); panes->keycaps[i]=find(c,help,name);
        snprintf(name,sizeof name,"T_Key%u",i); panes->key_labels[i]=find(c,help,name);
        snprintf(name,sizeof name,"T_Verb%u",i); panes->action_labels[i]=find(c,help,name);
    }
    panes->join_buttons[0]=call(c,0x70008ad8e8ull,help,string(c,"P_JoinL"),0,0,0);
    if(panes->join_buttons[0]) {
        static const char *const parts[]={"P_JoinL","P_JoinR","P_JoinSL","P_JoinSR"};
        for(unsigned i=0;i<4;i++) {
            panes->join_buttons[i]=find(c,help,parts[i]);
            attach(c,page,help,parts[i]);
        }
        for(unsigned i=0;i<2;i++) {
            panes->join_plus[i]=find(c,help,i ? "T_JoinPlus1" : "T_JoinPlus0");
            text(c,panes->join_plus[i],"+");
        }
    }
    float second_line[3]={0,-44,0};
    memcpy((void *)(uintptr_t)(c->sp+320),second_line,sizeof second_line);
    call(c,0x70008ade34ull,panes->hint_second,c->sp+320,0,0,0);
    attach(c,page,panes->root,"L_SettingsBG");
    uint64_t back_button=attach(c,page,panes->root,"L_SettingsBack");
    panes->back_animation=button_animations(c,back_button);
    panes->back_icon=find(c,back_button,"L_BtnIconB_00");
    for (unsigned i=0;i<=SETTINGS_VISIBLE;i++) {
        char name[32]; snprintf(name,sizeof name,"L_SettingsRow%u",i);
        uint64_t obj=call(c,0x70005902d0ull,0x2c8,4,0,0,0); heap(obj,0x2c8);
        memset((void *)(uintptr_t)obj,0,0x2c8);
        call(c,0x700044dee4ull,obj,0,0,0,0);
        uint64_t row=obj+0x30, vt=LD64(row);
        // Attach the native row without registering it in the main menu's navigation list.
        ST64(row,LD64(0x70013083e0ull)+0x60); ST32(c->sp+112,0);
        call(c,0x70008bc0c4ull,page,row,panes->root,string(c,name),c->sp+112);
        ST64(row,vt);
        call(c,0x70008ae9bcull,row,4,0,0,0);
        call(c,0x70008ae7b4ull,row,4,0,0,0);
        call(c,0x700044e100ull,obj,i ? 0x7000ee8eccull : 0,0x7000ee93f8ull,0x7000ee9402ull,0x7000ee940cull);
        if (i) heap(LD64(obj+0x128),0x18);
        heap(LD64(obj+0x130),0x18); heap(LD64(obj+0x190),0x88);
        panes->rows[i]=obj;
        panes->labels[i]=i ? find(c,row,"T_Text_00") : 0; panes->values[i]=find(c,row,"T_Text_01");
    }
    panes->scrollbar=call(c,0x70005902d0ull,0x1b0,4,0,0,0); heap(panes->scrollbar,0x1b0);
    call(c,0x7000451ed0ull,panes->scrollbar,0,0,0,0);
    // Use the native scrollbar as a display; its stock input callback requires a Nintendo list owner.
    ST64(panes->scrollbar+0x30,LD64(0x70013083e0ull)+0x60);
    ST32(c->sp+112,0);
    call(c,0x70008bc0c4ull,page,panes->scrollbar+0x30,panes->root,string(c,"L_SettingsScroll"),c->sp+112);
    call(c,0x70004521d4ull,panes->scrollbar,0,0,0,0);
    panes->dialog=attach(c,page,panes->root,"L_SettingsDialog");
    panes->dialog_labels[0]=find(c,attach(c,page,panes->root,"L_SettingsQuestion"),"T_Title_00");
    visible(c,panes->dialog,"L_WinBtn_C",false); visible(c,panes->dialog,"N_Load_00",false);
    for(unsigned i=0;i<2;i++) {
        visible(c,panes->dialog,i ? "L_WinBtn_R" : "L_WinBtn_L",false);
        panes->dialog_buttons[i]=attach(c,page,panes->root,i ? "L_SettingsNo" : "L_SettingsYes");
        uint64_t label=attach(c,page,panes->dialog_buttons[i],"L_SettingsLabel");
        panes->dialog_labels[i+1]=find(c,label,"T_Title_00");
        if(!i && panes->symbol_help) panes->symbol_dialog=label;
        panes->dialog_animations[i]=button_animations(c,panes->dialog_buttons[i]);
    }
    // Um cursor próprio preserva a posição e a animação do retículo da cena.
    panes->dialog_cursor=call(c,0x70005902d0ull,0xc8,4,0,0,0); heap(panes->dialog_cursor,0xc8);
    call(c,0x7000442a84ull,panes->dialog_cursor,0,0,0,0);
    ST32(c->sp+112,0);
    call(c,0x70008bc0c4ull,page,panes->dialog_cursor+0x30,panes->root,string(c,"L_SettingsCursor"),c->sp+112);
    if(LD32(panes->dialog_cursor+0x68)!=4) FATAL("[mk8:settings] cursor animation layout changed\n");
    uint64_t cursor_groups=LD64(panes->dialog_cursor+0x70); heap(cursor_groups,4*8);
    panes->cursor_animation=LD64(cursor_groups+8); heap(panes->cursor_animation,0x30);
    call(c,0x7000442ca0ull,panes->dialog_cursor,1,0,0,0);
    float size[2]={370,158}; memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
    heap(LD64(panes->dialog_cursor+0xb8),0x60);
    call(c,0x7000443404ull,panes->dialog_cursor,c->sp+320,0,0,0);
    visible(c,panes->root,"N_SettingsPopup",false);
    *c=saved;
}

void mk8_settings_shared_build(uint64_t page) {
    if(!mk8_controller_layout_available()) { top=0; return; }
    heap(page,0x1f8);
    if(controller_visible) controller_release();
    mk8_settings_top_forget();
    owner=page; cursor_owner=top=top_root=restore_nav=0; restore_menu_cursors=false;
    from_button=overlay_wrench=controller_visible=false;
    selector_absent=true;
    Cpu *c=kernel_current_cpu();
    if(!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid selector construction context\n");
    MenuPanes *previous_panes=panes;
    memset(&selector_panes,0,sizeof selector_panes);
    panes=&selector_panes; build_widgets(c,page); panes=previous_panes;
    if(panes==&selector_panes) { pulse_pane=0; slide_direction=0; }
    LOG("[mk8:settings] native selector page built\n");
}

// The allocator callback is installed only while the game builds each page.
// Its manager tick may update the bound widgets, but must never allocate them.
void mk8_settings_fader_build(uint64_t page) {
    if(panes==&fader_panes) { pulse_pane=0; slide_direction=0; }
    memset(&fader_panes,0,sizeof fader_panes);
    if(!mk8_settings_layout_available()) return;
    uint64_t mount=mk8_settings_fader_root(page);
    if(!mount) return; // Older/custom archives can still use the ordinary selector.
    Cpu *c=kernel_current_cpu();
    if(!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid fader construction context\n");
    MenuPanes *previous_panes=panes;
    panes=&fader_panes; build_widgets(c,page); panes->mount=mount; panes=previous_panes;
    LOG("[mk8:settings] bound genuine loading Menu on page %llx\n",page);
}

bool mk8_settings_page_active(void) { return requested || active || mk8_controller_layout_active(); }
bool mk8_settings_page_toggle(void) {
    if(shared_race) return false;
    if(mk8_controller_layout_active()) return true;
    if(active) { exit_menu=true; return true; }
    if(!mk8_settings_input_native() || mk8_settings_layout_unavailable()) return false;
    from_button=false; requested=true;
    return true;
}
void mk8_native_menu_exit(void) { if(active) exit_menu=true; }
void mk8_settings_page_back(void) { if (active) back=true; }

bool mk8_settings_page_key(int key) {
    if (!editing || !((key>='0' && key<='9') || key==8 || key==13 || key==27)) return false;
    pthread_mutex_lock(&key_lock);
    if (key_count<32) keys[key_count++]=key; else overflow=true;
    pthread_mutex_unlock(&key_lock);
    return true;
}

static void begin_number(Mk8Setting row) {
    pthread_mutex_lock(&key_lock); key_count=0; overflow=false; pthread_mutex_unlock(&key_lock);
    editing_id=row.id; edit_min=row.minimum; edit_max=row.maximum;
    snprintf(number,sizeof number,"%d",row.current); replace_number=true; invalid=false; editing=true;
}

static bool edit_number(Cpu *c,uint64_t edge, bool cancel) {
    int batch[32], count; bool lost;
    pthread_mutex_lock(&key_lock);
    count=key_count; memcpy(batch,keys,count*sizeof *keys); key_count=0; lost=overflow; overflow=false;
    pthread_mutex_unlock(&key_lock);
    if (lost && !cancel) { invalid=true; return true; }
    bool commit=(edge&1)!=0;
    for (int i=0;i<count;i++) {
        int key=batch[i];
        if (key==27) { cancel=true; break; }
        if (key==13) { commit=true; break; }
        size_t n=strlen(number);
        if (key==8) { if (replace_number) n=0; else if (n) n--; number[n]=0; replace_number=false; }
        else if (key>='0' && key<='9') {
            if (replace_number) n=0;
            if (n==3) { invalid=true; continue; }
            number[n]=(char)key; number[n+1]=0;
            replace_number=false;
        }
        invalid=false;
    }
    int step=0;
    if (edge&((1ull<<13)|(1ull<<17))) step=1;
    if (edge&((1ull<<15)|(1ull<<19))) step=-1;
    if (edge&((1ull<<14)|(1ull<<18))) step=10;
    if (edge&((1ull<<12)|(1ull<<16))) step=-10;
    if (step) {
        int value=atoi(number)+step; value=value<edit_min ? edit_min : value>edit_max ? edit_max : value;
        snprintf(number,sizeof number,"%d",value); replace_number=true; invalid=false;
    }
    if (cancel) { editing=false; menu_back_feedback(c); LOG("[mk8:settings] numeric edit cancelled\n"); }
    else if (commit) {
        if (!*number || atoi(number)<edit_min || atoi(number)>edit_max || invalid) invalid=true;
        else if(post(SETTINGS_SET,editing_id,atoi(number))) { editing=false; LOG("[mk8:settings] numeric value applied: %s\n",number); }
    }
    return count || edge || cancel;
}

void mk8_settings_page_fixed(uint64_t b) { fixed_buttons=b; }
void mk8_settings_page_wheel(int steps) { atomic_fetch_add(&wheel,steps); }
uint64_t mk8_settings_page_input(uint64_t b, int32_t st[4],uint64_t scripted) {
    uint64_t edge=b&~atomic_exchange(&raw_buttons,b); buttons=scripted;
    if ((edge&(1ull<<10)) && controller_can_open()) controller_request_editor();
    if (mk8_settings_page_active()) { memset(st,0,4*sizeof *st); return 0; }
    uint64_t held=atomic_load(&suppressed)&b;
    suppressed=held;
    if(held&(0xfull<<16)) st[0]=st[1]=0;
    if(held&(0xfull<<20)) st[2]=st[3]=0;
    return b&~held;
}

// All engine calls stay on the guest's UI thread; render/input threads only publish requests.
void mk8_settings_page_tick(uint64_t page, bool open) {
    if(page==top && page!=owner) {
        if(open) {
            if(mk8_settings_style_active() && !mk8_settings_layout_unavailable()) { from_button=true; requested=true; }
            else overlay_wrench=true;
        }
        if(overlay_wrench && mk8_settings_button_decided(page)) {
            overlay_wrench=false;
            mk8_settings_button_restore(page);
            mk8_menu_request_overlay();
        }
        return;
    }
    if ((page!=owner && page!=panes->page) || (!active && !open)) return;
    Cpu *c=kernel_current_cpu();
    if (!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid selector update context\n");
    Cpu saved=*c; c->sp-=512;
    int32_t x=0,y=0; bool touch=touch_first(&x,&y);
    // A P2 controller can stop the game's P1 polls; read menu events directly on every UI tick.
    uint64_t b=fixed_buttons|buttons;
    bool source_changed=read_tips(true);
    if(source_changed && active && !restart_prompt) settings_tips(c);
    if (open && !active) {
        if(!post(SETTINGS_BEGIN,0,0)) FATAL("[mk8:settings] settings request queue full before opening\n");
        end_motion(c); painted_depth=-1; settings_from_service=false;
        menu_level=MENU_HOME; category=focus=offset=0; exit_menu=false; play_pending=PLAY_IDLE;
        entering=true; confirming=from_button; back=false; previous=b; touched=touch; active=true;
        panes->visible=true;
        input_reset(); reset_confirm=false; atomic_store(&wheel,0); repeat_at=0;
        animation_frame(c,panes->back_animation,0,0);
        if(mk8_settings_values_main_menu()) {
        uint64_t nav=LD64(top+0x1b0); heap(nav,0xb8);
        if (LD32(nav+0x10)!=12) FATAL("[mk8:settings] navigation changed before opening\n");
        uint64_t items=LD64(nav+0x18); heap(items,12*8);
        uint64_t component=LD64(items+11*8);
        if (component<HEAP_BASE+0x30) FATAL("[mk8:settings] invalid confirmation widget\n");
        button=component-0x30; heap(button,0x3d8);
        if (LD32(button+0x48)!=107) FATAL("[mk8:settings] confirmation widget changed\n");
        top_root=LD64(top+0x448); heap(top_root,0x88); restore_nav=nav;
        }
        stock_cursors(c,SAVE_CURSORS);
        if(!confirming) {
            entered_at=plat_now_ns();
            if(restore_nav) call(c,0x70008affa4ull,restore_nav,0,0,0,0);
            stock_cursors(c,HIDE_CURSORS);
            if(top_root) visible(c,top_root,"N_Btn_00",false);
        }
        visible(c,panes->root,"N_Settings",false); appear(c,0); draw(c);
        if(!confirming) { visible(c,panes->root,"N_Settings",true); dialog_open_sound(c); }
        LOG("[mk8:settings] native selector entering\n");
    }
    // Publish active before consuming the open request; do not erase a later reopen during exit.
    if(open) requested=false;
    if(exiting) {
        if(exit_pressed) {
            if(!call(c,0x70008ad5d4ull,panes->back_animation,0,0,0,0)) { previous=b; touched=touch; *c=saved; return; }
            exit_pressed=false; exited_at=plat_now_ns();
        }
        uint64_t elapsed=plat_now_ns()-exited_at;
        float progress=elapsed>=160000000ull ? 0 : exit_from*(1-elapsed/160000000.0f);
        appear(c,progress);
        int result=mk8_settings_values_saved();
        if(result<0) { exiting=false; entering=true; confirming=false; entered_at=plat_now_ns()-(uint64_t)(reveal*160000000ull); draw(c); }
        else if(elapsed>=160000000ull || exit_from==0) {
            if(result==2) finish_return(c,page,b);
            else if(result==3) {
                menu_cursor(c,false);
                exiting=false; restart_prompt=true; restart_yes=false; back=false; restart_pressed=false; restart_fade_at=0;
                reveal_pane(c,"N_SettingsPopup",1);
                visible(c,panes->root,"N_SettingsPopup",true); dialog_draw(c,true);
                call(c,0x7000442f5cull,panes->dialog_cursor,1,0,0,0);
                float speed=1; memcpy(&c->v[0],&speed,sizeof speed);
                call(c,0x70008ad300ull,panes->cursor_animation,1,0,0,0);
                LOG("[mk8:settings] saved; restart confirmation opened\n");
            }
        }
        previous=b; touched=touch; *c=saved; return;
    }
    if (entering) {
        bool cancel=atomic_exchange(&exit_menu,false) || back || ((b&~previous)&2);
        if (!cancel) {
            // Keep the original button visible until its native Decide pulse finishes.
            if (confirming) {
                if (!call(c,0x7000435cc0ull,button,0,0,0,0)) { previous=b; touched=touch; *c=saved; return; }
                confirming=false; entered_at=plat_now_ns();
                call(c,0x70008affa4ull,restore_nav,0,0,0,0);
                stock_cursors(c,HIDE_CURSORS);
                visible(c,top_root,"N_Btn_00",false); visible(c,panes->root,"N_Settings",true);
                dialog_open_sound(c);
            }
            uint64_t elapsed=plat_now_ns()-entered_at;
            float progress=elapsed>=160000000ull ? 1 : elapsed/160000000.0f;
            appear(c,progress);
            previous=b; touched=touch;
            if (elapsed<160000000ull) { *c=saved; return; }
            LOG("[mk8:settings] native selector opened\n");
        } else { start_exit(c,false); previous=b; touched=touch; *c=saved; return; }
        entering=confirming=false;
    }
    if(!exiting && !restart_prompt && atomic_exchange(&exit_menu,false)) {
        play_pending=PLAY_IDLE;
        editing=reset_confirm=false; start_exit(c,false); previous=b; touched=touch; *c=saved; return;
    }
    if(animate_menu(c)) {
        atomic_store(&wheel,0); previous=b; touched=touch; *c=saved; return;
    }
    if(play_pending!=PLAY_IDLE) {
        finish_play_action(c); previous=b; touched=touch; *c=saved; return;
    }
    if(nextendo_page() && plat_now_ns()-service_checked_at>=1000000000ull) {
        if(update_service()) {
            // Consume edges sampled against the old rows before a refreshed target can receive them.
            draw(c); previous=b; touched=touch; *c=saved; return;
        }
    }
    if(menu_level==MENU_PLAY_ACTIONS && plat_now_ns()-play_checked_at>=1000000000ull) {
        Mk8PlaySession next={0}; mk8_menu_play_session(&next); play_checked_at=plat_now_ns();
        if(memcmp(&play_session,&next,sizeof next)) { play_session=next; draw(c); }
    }
    if (!menu_input_captured()) {
        uint64_t edge=b&~previous;
        if(restart_prompt) {
            if(restart_wait) {
                if(mk8_settings_values_saved()==2) finish_return(c,page,b);
            } else if(restart_pressed) {
                // Os pulsos duram 8 e 16 quadros; consultar o quadro final, não o evento transitório de término.
                if(call(c,0x70008ad59cull,panes->dialog_animations[restart_yes ? 0 : 1],0,0,0,0) &&
                   call(c,0x70008ad59cull,panes->cursor_animation,0,0,0,0) && popup_fade(c,&restart_fade_at,false))
                    restart_wait=post(SETTINGS_RESTART,0,restart_yes);
            } else {
                bool close=atomic_exchange(&back,false) || (edge&2), confirm=(edge&1)!=0;
                bool was_yes=restart_yes;
                if(edge&((1ull<<12)|(1ull<<16))) restart_yes=true;
                if(edge&((1ull<<14)|(1ull<<18))) restart_yes=false;
                if(edge&((1ull<<13)|(1ull<<15)|(1ull<<17)|(1ull<<19))) restart_yes=!restart_yes;
                if(touch && !touched && y>=476 && y<=634) {
                    if(x>=255 && x<=625) { restart_yes=true; confirm=true; }
                    if(x>=655 && x<=1025) { restart_yes=false; confirm=true; }
                }
                if(close) { restart_yes=false; confirm=true; }
                if(was_yes!=restart_yes) {
                    dialog_draw(c,false);
                    if(!confirm) menu_sound(c,0x7000ed87a1ull);
                }
                if(confirm) {
                    restart_pressed=true;
                    animation_play(c,panes->dialog_animations[restart_yes ? 0 : 1],2,1);
                    call(c,0x7000443320ull,panes->dialog_cursor,0,0,0,0);
                    menu_sound(c,close ? 0x7000ed87cfull : 0x7000ed87bfull);
                }
            }
            previous=b; touched=touch; *c=saved; return;
        }
        uint64_t direction_bits=b&((0xfull<<12)|(0xfull<<16));
        if(direction_bits) {
            if(direction_bits!=(previous&((0xfull<<12)|(0xfull<<16)))) repeat_at=plat_now_ns()+400000000ull;
            else if(repeat_at && plat_now_ns()>=repeat_at) { edge|=direction_bits; repeat_at=plat_now_ns()+90000000ull; }
        } else repeat_at=0;
        bool close=atomic_exchange(&back,false) || (edge&2);
        unsigned old_focus=focus;
        bool change=false, confirm=(edge&1)!=0; int direction=0;
        if(menu_level!=MENU_OPTIONS) {
            if(nextendo_page()) {
                nextendo_input(c,edge,close,confirm,touch && !touched,x,y);
                previous=b; touched=touch; *c=saved; return;
            }
            bool play=menu_level==MENU_PLAY_ACTIONS;
            unsigned count=menu_action_count();
            int scroll=atomic_exchange(&wheel,0);
            if(scroll) { int next=(int)focus+scroll; focus=next<0 ? 0 : next>=(int)count ? count-1 : (unsigned)next; }
            if(edge&((1ull<<13)|(1ull<<17))) focus=(focus+count-1)%count;
            if(edge&((1ull<<15)|(1ull<<19))) focus=(focus+1)%count;
            if(touch && !touched) {
                if(x>=20 && x<=340 && y>=600 && y<=680) close=true;
                int first_y=play ? 230 : 180, height=play ? 60 : 70;
                if(x>=200 && x<=1080 && y>=first_y && y<first_y+height*SETTINGS_VISIBLE) {
                    unsigned index=offset+(unsigned)(y-first_y)/height;
                    if(index<count) { focus=index; confirm=true; }
                }
            }
            if(close && menu_level==MENU_HOME) start_exit(c,false);
            else if((play && (close || confirm)) || (menu_level==MENU_HOME && focus==MENU_PLAY && confirm)) {
                // Move the pointer selection before its confirmation pulse.
                if(focus!=old_focus) draw(c);
                begin_play_action(c,play ? (close ? PLAY_LEAVE : play_actions[focus]) : PLAY_ENTER);
            } else {
                if(close) { menu_level=MENU_HOME; focus=HOME_SETTINGS; offset=0; }
                else if(confirm) {
                    pulse_slot(c,focus-offset+1);
                    if(focus==HOME_SETTINGS) enter_settings_category(c,MENU_QUICK);
                    else if(focus==HOME_NEXTENDO) enter_service(MENU_HOME,MENU_ROOMS);
                }
                if(close || confirm || old_focus!=focus) {
                    if(close) menu_back_feedback(c);
                    else menu_sound(c,confirm ? 0x7000ed87bfull : 0x7000ee9492ull);
                    draw(c);
                } else if(last_revision!=mk8_settings_values_revision()) draw(c);
            }
            previous=b; touched=touch; *c=saved; return;
        }
        if(rich_category()) {
            unsigned revision,depth;
            mk8_native_rows_count(&revision,&depth);
            if(revision!=rich_revision || depth!=rich_depth) {
                draw(c); previous=b; touched=touch; *c=saved; return;
            }
        }
        unsigned count=option_count();
        if(mk8_settings_values_capturing()) { previous=b; touched=touch; if(last_revision!=mk8_settings_values_revision()) draw(c); *c=saved; return; }
        int scroll=atomic_exchange(&wheel,0);
        if(!editing && !reset_confirm && scroll) { int next=(int)focus+scroll; focus=next<0 ? 0 : next>(int)count ? count : (unsigned)next; change=true; }
        if (touch && !touched) {
            if (x>=20 && x<=340 && y>=600 && y<=680) close=true;
            if(reset_confirm && focus>offset && focus<=offset+SETTINGS_VISIBLE && x>650 && x<1010 &&
               y>=270+(int)(focus-offset-1)*60 && y<=330+(int)(focus-offset-1)*60) confirm=true;
            if(!editing && !reset_confirm && x>=200 && x<=1080 && y>=175 && y<=245) {
                focus=0; change=true;
                if(x<=270) direction=-1; else if(x>=1010) direction=1; else confirm=true;
            }
            for (unsigned i=0;i<SETTINGS_VISIBLE;i++) if (!editing && !reset_confirm && offset+i<count && x>=200 && x<=1080 && y>=270+(int)i*60 && y<=330+(int)i*60) {
                focus=offset+i+1; change=true;
                Mk8NativeRow row;
                if(rich_category() && mk8_native_rows_get(focus-1,rich_revision,&row) && !row.adjustable) confirm=true;
                else if (x>=590 && x<=650) direction=-1;
                else if (x>=1010) direction=1;
                else if (x>650) confirm=true;
            }
        }
        if (editing) { change=edit_number(c,edge,close); close=false; }
        else if(reset_confirm) {
            if(close) { menu_back_feedback(c); reset_confirm=false; close=false; change=true; }
            else if(confirm) { Mk8Setting row=selected(); change=post(SETTINGS_ACTIVATE,row.id,0); reset_confirm=false; }
        }
        else {
            if (edge&((1ull<<13)|(1ull<<17))) { focus=focus ? focus-1 : count; change=true; }
            if (edge&((1ull<<15)|(1ull<<19))) { focus=focus<count ? focus+1 : 0; change=true; }
            if (edge&((1ull<<12)|(1ull<<16))) direction=-1;
            if (edge&((1ull<<14)|(1ull<<18))) direction=1;
            if(focus && rich_category()) {
                if((confirm || direction) && mk8_native_rows_activate(focus-1,rich_revision,direction)) {
                    if(confirm) pulse_slot(c,focus-offset);
                    change=true;
                }
            } else if(focus) {
                Mk8Setting row=selected();
                if(row.enabled && confirm && row.kind!=SETTING_INFO) pulse_slot(c,focus-offset);
                if(row.enabled && confirm && row.kind==SETTING_NUMBER) { begin_number(row); change=true; }
                else if(row.enabled && confirm && row.kind==SETTING_RESET) { reset_confirm=true; change=true; }
                else if(row.enabled && confirm && row.kind==SETTING_BINDING) change|=post(SETTINGS_ACTIVATE,row.id,0);
                else if(row.enabled && (direction || confirm) && row.kind==SETTING_CHOICE) change|=post(SETTINGS_STEP,row.id,direction ? direction : 1);
                else if(row.enabled && direction && row.kind==SETTING_NUMBER) change|=post(SETTINGS_STEP,row.id,direction);
            } else if(direction || confirm) {
                if(confirm) pulse_slot(c,0);
                unsigned group=(category_groups[category]+(direction<0 ? MENU_GROUP_COUNT-1 : 1))%MENU_GROUP_COUNT;
                enter_settings_category(c,group); change=true;
            }
        }
        if (close) {
            if(mk8_settings_values_saved()<0) start_exit(c,false);
            else {
                if(!rich_category() || !mk8_native_rows_back()) {
                    if(rich_category()) mk8_native_rows_close();
                    if(settings_from_service) return_to_service();
                    else { menu_level=MENU_HOME; focus=HOME_SETTINGS; offset=0; }
                }
                menu_back_feedback(c); draw(c);
            }
        } else if (change || last_revision!=mk8_settings_values_revision()) {
            draw(c);
            if (direction || confirm || old_focus!=focus)
                menu_sound(c,old_focus!=focus ? 0x7000ee9492ull : 0x7000ee94acull);
        }
        previous=b; touched=touch;
    }
    *c=saved;
}

// The shared cursor page is owned by each UI scene, including the race scene.
static bool select_panes(MenuPanes *next,uint64_t fader,uint64_t mount,bool selector_present) {
    bool fader_present=fader && mount && fader_panes.page==fader && fader_panes.mount==mount;
    MenuPanes *old=panes;
    bool repaint=next && active && (next!=old || !next->visible || next->reset_motion);
    if(!repaint && !((old!=next && old->visible && !old->reset_motion) ||
       (selector_present && selector_panes.visible && (!active || next!=&selector_panes)) ||
       (fader_present && fader_panes.visible && (!active || next!=&fader_panes)))) {
        if(next) panes=next;
        return next!=NULL;
    }
    Cpu *c=kernel_current_cpu();
    if(!c || c->sp<GUEST_LO+1024 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid Menu transfer context\n");
    Cpu saved=*c; c->sp-=512;
    if(old!=next && old->visible && !old->reset_motion) {
        // A page can temporarily leave the active stack without being destroyed. Restore
        // its transforms only when its own page/mount is live again.
        old->held_pulse=pulse_pane; old->reset_motion=true;
        pulse_pane=0; slide_direction=0;
    }
    MenuPanes *sets[]={&selector_panes,&fader_panes};
    for(unsigned i=0;i<2;i++) {
        panes=sets[i];
        if(!(i ? fader_present : selector_present) || !panes->root) continue;
        if(panes->reset_motion) {
            if(panes->held_pulse) pane_scale(panes->held_pulse,1);
            slide_content(c,0);
            panes->held_pulse=0; panes->reset_motion=false;
        }
        if(panes->visible && (!active || panes!=next)) {
            menu_cursor(c,false);
            visible(c,panes->root,"N_Settings",false);
            if(panes->dialog) visible(c,panes->root,"N_SettingsPopup",false);
            panes->visible=false;
        }
    }
    panes=next ? next : old;
    if(repaint) {
        painted_depth=-1;
        draw(c); appear(c,reveal);
        visible(c,panes->root,"N_Settings",true);
        if(restart_prompt) {
            menu_cursor(c,false); reveal_pane(c,"N_SettingsPopup",1);
            visible(c,panes->root,"N_SettingsPopup",true); dialog_draw(c,true);
        }
        panes->visible=true;
    }
    *c=saved;
    return next!=NULL;
}

void mk8_settings_page_frame(uint64_t manager) {
    uint64_t end=HEAP_BASE+kernel_heap_size();
    uint64_t page=race_heap(manager,0xc70,end) ? LD64(manager+0xc68) : 0;
    mk8_title_input_active(race_heap(page,0x128,end) && LD64(page)==MAIN_BASE+0x127cb38 &&
                           LD32(page+0x120)==4 && LD32(page+0x124)==0 && !mk8_auto_lan_requested());
    unsigned players=shared_race_scene(manager);
    if (players != shared_race) {
        mk8_input_race_players(players);
        shared_race=players;
    }
    if(shared_race) { requested=false; if(active) exit_menu=false; }
    bool layout=mk8_settings_layout_available();
    if(!layout && mk8_settings_layout_unavailable() && atomic_exchange(&requested,false))
        mk8_menu_request_overlay();
    unsigned joining=mk8_controller_join_players();
    if(!layout && !owner && !top) { mk8_settings_values_context(false); return; }
    heap(manager,0xb60);
    unsigned count=LD32(manager+0xb50);
    if(count>32) FATAL("[mk8:settings] unexpected active page count\n");
    uint64_t pages=LD64(manager+0xb58);
    if(count) heap(pages,count*8);
    bool present=false,main_menu=false,covered=false;
    uint64_t fader=0,mount=0;
    unsigned parts=0,drivers=0;
    for(unsigned i=0;i<count;i++) {
        uint64_t p=LD64(pages+i*8); heap(p,0x1d0);
        if(p==owner && registered_page(manager,p,MAIN_BASE+0x1271df0,13,end)) present=true;
        if((requested || active || panes==&fader_panes || fader_panes.visible) &&
           p==fader_panes.page && registered_page(manager,p,MAIN_BASE+0x12733d8,4,end)) {
            uint64_t found=mk8_settings_fader_root(p);
            if(found && found==fader_panes.mount) { fader=p; mount=found; covered=mk8_settings_fader_covers(p); }
        }
        if(p==top && LD32(p+0x120)==4) main_menu=true;
        // In MK8 4.0.1 the CharaParts layout is shared by solo (0x14), WiFi (0x15) and local (0x16).
        if(LD32(p+0x120)==4 && !LD32(p+0x124)) {
            uint64_t type=LD64(p);
            unsigned id=LD32(p+0xe8);
            if(type==MAIN_BASE+0x1279040 && (id==0x15 || id==0x16)) parts=id;
            if(type==MAIN_BASE+0x1278648 && id==0x2d) drivers=2;
            if(type==MAIN_BASE+0x1278940 && id==0x2e) drivers=4;
        }
    }
    mk8_settings_values_context(main_menu);
    if(!layout && (!owner || !mk8_controller_layout_available())) {
        controller_entry_players=0;
        if(mk8_controller_layout_active()) controller_release();
        return;
    }
    selector_absent=!present;
    available=layout && (present || mount) && mk8_settings_values_count(0)>0;
    unsigned edit_players=0;
    if(present && !shared_race) {
        if(parts==0x16 && drivers==4) edit_players=atomic_load(&controller_applet_players);
        if(parts==0x15 && drivers==2) edit_players=2;
    }
    // The LAN layout hides the on-screen shortcut. Plus still opens the editor.
    controller_character_shortcut=parts==0x16 && edit_players!=0;
    controller_entry_players=edit_players;
    if(!main_menu) { top_root=restore_nav=0; from_button=confirming=false; }
    if(!present) {
        if(controller_visible && controller_required) { controller_detached=true; return; }
        if(!active && !restore_menu_cursors) cursor_owner=0;
        if(mk8_controller_layout_active()) controller_release();
    }
    if(joining) controller_detached=false;
    MenuPanes *next=present ? &selector_panes : NULL;
    if(!shared_race && (requested || active) && !controller_visible && mount && (covered || !present)) next=&fader_panes;
    if(!next && mount && active) next=&fader_panes; // Permit the silent close on race entry.
    if(!select_panes(next,fader,mount,present)) {
        if(active && (shared_race || exit_menu)) {
            restore_menu_cursors=cursor_owner!=0;
            post(SETTINGS_FINISH,0,SETTINGS_DETACHED);
            mk8_native_rows_close();
            active=editing=entering=confirming=exiting=reset_confirm=exit_menu=back=false;
            restart_prompt=restart_wait=restart_pressed=exit_pressed=from_button=false;
            suppressed=raw_buttons|fixed_buttons|buttons; input_reset();
        }
        return;
    }
    if(present && (active ? !shared_race && !cursor_owner && !confirming : restore_menu_cursors)) {
        Cpu *c=kernel_current_cpu(); Cpu saved=*c;
        if(active) { stock_cursors(c,SAVE_CURSORS); stock_cursors(c,HIDE_CURSORS); }
        else { stock_cursors(c,RESTORE_CURSORS); restore_menu_cursors=false; }
        *c=saved;
    }
    if(shared_race && active) {
        // LAN can leave the lobby while its settings are open. Save without a restart prompt.
        post(SETTINGS_FINISH,0,SETTINGS_DETACHED);
        Cpu *c=kernel_current_cpu();
        if(!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid close context\n");
        Cpu saved=*c; c->sp-=512;
        if(!main_menu) { top_root=restore_nav=0; from_button=false; }
        finish_return(c,owner,raw_buttons);
        editing=entering=confirming=reset_confirm=back=false;
        *c=saved;
        return;
    }
    if(joining || controller_preview || controller_visible) {
        unsigned players=joining ? joining : controller_entry_players;
        if(!players && !controller_visible) controller_release();
        if(!players) controller_preview=false;
        else if(!controller_visible) controller_rows=players;
        Cpu *c=kernel_current_cpu();
        if(!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:controllers] invalid popup context\n");
        Cpu saved=*c; c->sp-=512;
        controller_layout(c,joining!=0);
        *c=saved;
        if(controller_visible) return;
    }
    if(active || (layout && available)) mk8_settings_page_tick(panes->page ? panes->page : owner,atomic_load(&requested));
}
