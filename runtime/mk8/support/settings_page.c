#include "rt.h"
#include "hle.h"
#include "settings_values.h"
#include "controller_edit.h"
#include "controller_tips.h"
bool mk8_auto_lan_requested(void);
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint64_t owner, labels[SETTINGS_VISIBLE+1], values[SETTINGS_VISIBLE+1], rows[SETTINGS_VISIBLE+1], root, hint, hint_second;
static struct Mk8ControlTips tips;
static uint64_t hint_plate, hint_single, hint_double, keycaps[4], key_labels[4], action_labels[4];
static uint64_t join_buttons[4], join_plus[2];
static const char *const symbol_names[]={"A","B","X","Y","L","R","ZL","ZR","SL","SR","Plus","Minus","DL","DR","DT","DB"};
enum { SYMBOL_COUNT=sizeof symbol_names/sizeof *symbol_names };
static uint64_t symbol_help, symbol_dialog, symbol_parts[5][SYMBOL_COUNT], shown_symbols[5];
static uint64_t top, top_root, restore_nav, scrollbar;
static uint64_t dialog, dialog_buttons[2], dialog_animations[2];
static uint64_t dialog_labels[3];
static uint64_t dialog_cursor, cursor_animation, back_animation, back_icon;
static bool restart_prompt, restart_yes, restart_wait, restart_pressed, exit_pressed;
static uint64_t restart_fade_at, controller_fade_at;
static int controller_press=-1;
static _Atomic bool from_button;
static _Atomic bool available, requested;
static _Atomic unsigned shared_race;
static bool hide_on_return;
static unsigned category, focus, offset;
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
static uint64_t title_label;
uint64_t mk8_controller_p1_navigation(void);
void mk8_controller_hold_extra_inputs(void);
static void controller_release(void);
static _Atomic bool active, back;
static _Atomic uint64_t buttons, suppressed, fixed_buttons, raw_buttons;
static _Atomic int wheel;
static _Atomic bool editing;
static int editing_id, edit_max;
static char number[4];
static bool replace_number, invalid;
static pthread_mutex_t key_lock=PTHREAD_MUTEX_INITIALIZER;
static int keys[32], key_count;
static bool overflow;
bool menu_input_captured(void);
bool touch_first(int32_t *, int32_t *);
void mk8_settings_button_restore(uint64_t);
void mk8_settings_top_forget(void);
void input_reset(void);
void mk8_input_race_players(unsigned players);

RT_ON_RESET(settings_page_reset) {
    if(controller_visible) controller_release();
    owner = root = cursor_owner = button = 0; active = back = editing = entering = confirming = exiting = false;
    controller_preview=controller_visible=false;
    controller_applet_players=controller_entry_players=0;
    controller_mouse_down=controller_character_shortcut=false;
    controller_armed=controller_click=-1; controller_hover=-2;
    top=top_root=restore_nav=scrollbar=0; available=requested=from_button=shared_race=hide_on_return=false;
    dialog=dialog_cursor=cursor_animation=back_animation=0;
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
    uint64_t pane=find(c,root,name); heap(pane,0x60);
    float eased=progress*progress*(3-2*progress), translation[3]={0,-16*(1-eased),0};
    ST8(pane+0x59,(uint8_t)(255*eased));
    ST8(pane+0x58,LD8(pane+0x58)|2);
    memcpy((void *)(uintptr_t)(c->sp+320),translation,sizeof translation);
    call(c,0x70008ade34ull,pane,c->sp+320,0,0,0);
}

static void appear(Cpu *c, float progress) {
    reveal=progress;
    reveal_pane(c,"N_Settings",progress);
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

static void text(Cpu *c, uint64_t pane, const char *value) {
    size_t n = strlen(value);
    if (n > 79) FATAL("[mk8:settings] selector value too long\n");
    for (size_t i=0; i<=n; i++) ST16(c->sp+128+i*2,(unsigned char)value[i]);
    call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
}

// The title font supplies the outline shoulders but has no sideways rail glyphs.
static size_t instruction_line(uint16_t *out,size_t limit,const char *s) {
    size_t n=0;
    while(*s && n<limit) {
        uint16_t ch=(unsigned char)*s++;
        if(ch=='[' && (*s=='L' || *s=='R') && s[1]==']') {
            ch=*s=='L' ? 0xe0a4 : 0xe0a5; s+=2;
        }
        out[n++]=ch;
    }
    out[n]=0;
    return n;
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
    if(shown_symbols[slot]) call(c,0x70008ade74ull,shown_symbols[slot],false,0,0,0);
    shown_symbols[slot]=0;
}

static float symbol_width(int icon) {
    if(icon==10 || icon==11) return 24;
    return icon>=4 && icon<=9 ? 56 : 40;
}

static void show_symbol(Cpu *c,unsigned slot,int icon,float x,float y) {
    uint64_t parent=slot==4 ? symbol_dialog : symbol_help;
    uint64_t *part=&symbol_parts[slot][icon];
    if(!*part) {
        char name[16];
        snprintf(name,sizeof name,"P_Map%u%s",slot==4 ? 0 : slot,symbol_names[icon]);
        *part=find(c,parent,name);
    }
    shown_symbols[slot]=*part;
    call(c,0x70008ade74ull,*part,true,0,0,0);
    hint_box(c,*part,x,y-5.5f,symbol_width(icon),40);
}

static float hint_text_y(bool two_lines,bool lower) {
    // The font baseline places the 36-unit keycap ten units inside each 56-unit row.
    if(hint_double && two_lines) return lower ? -57 : -1;
    return (two_lines ? 10 : 0)-(lower ? 44 : 5);
}

static void hint_background(Cpu *c,bool two_lines,float width) {
    if(!keycaps[0]) return;
    bool legacy_double=two_lines && !hint_double;
    hint_box(c,hint_plate,0,legacy_double ? -8 : 2,width,legacy_double ? 96 : 56);
    if(hint_double) {
        call(c,0x70008ade74ull,hint_single,!two_lines,0,0,0);
        call(c,0x70008ade74ull,hint_double,two_lines,0,0,0);
        hint_box(c,two_lines ? hint_double : hint_single,0,2,width,two_lines ? 112 : 56);
    }
    hint_box(c,hint,0,hint_text_y(two_lines,false),width,50);
    hint_box(c,hint_second,0,hint_text_y(two_lines,true),width,50);
}

static bool keycap_tips(Cpu *c,const char *description,const char *const keys_text[],const char *const verbs_text[],
                        unsigned n,const char *mouse_hint,bool show_chords,bool keyboard_joins) {
    if(!keycaps[0]) return false; // Previously prepared archives retain their stock text-only hints.
    float keys[4], verbs[4], widths[4], columns[4]={0}, total=0;
    int icons[4]={-1,-1,-1,-1};
    bool plain[4]={0};
    for(unsigned i=0;i<4;i++) {
        hide_symbol(c,i);
        bool pad=i<n && tips.source!=MK8_TIP_KEYBOARD && !(keyboard_joins && i+1==n);
        if(pad && (controller_required || controller_preview) && symbol_help && !(show_chords && i<2)) {
            icons[i]=symbol_index(keys_text[i]);
            plain[i]=icons[i]<0;
        }
        bool cap=i<n && icons[i]<0 && !plain[i] && !(show_chords && i<2);
        call(c,0x70008ade74ull,keycaps[i],cap,0,0,0);
        call(c,0x70008ade74ull,key_labels[i],cap,0,0,0);
        call(c,0x70008ade74ull,action_labels[i],i<n,0,0,0);
        if(join_buttons[0]) {
            call(c,0x70008ade74ull,join_buttons[i],show_chords,0,0,0);
            if(i<2) call(c,0x70008ade74ull,join_plus[i],show_chords,0,0,0);
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
    instruction(c,hint,79,description ? description : "");
    call(c,0x70008ade74ull,hint_second,mouse_hint!=NULL,0,0,0);
    if(mouse_hint) text(c,hint_second,mouse_hint);
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
            hint_box(c,action_labels[i],start+widths[i]/2,y,widths[i],50);
            text(c,action_labels[i],label);
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
            hint_box(c,join_buttons[2*i],key_center-left_offset,icon_y,56,40);
            hint_box(c,join_buttons[2*i+1],key_center+40,icon_y,56,40);
            hint_box(c,join_plus[i],key_center,plus_y,24,50);
        } else if(icons[i]>=0) {
            show_symbol(c,i,icons[i],key_center,y);
        } else {
            hint_box(c,keycaps[i],key_center,y-9,keys[i],36);
            hint_box(c,key_labels[i],key_center,y,keys[i],50);
            text(c,key_labels[i],keys_text[i]);
        }
        hint_box(c,action_labels[i],start+button_width+8+verbs[i]/2,y,verbs[i],50);
        text(c,action_labels[i],verbs_text[i]);
        x+=columns[i%per_row]+(show_chords ? 88 : 40);
    }
    return true;
}

static void action_tips(Cpu *c,const char *description,const struct TipAction *actions,unsigned count,const char *fallback) {
    if(!hint) return;
    const char *key[4],*verb[4];
    unsigned n=0;
    if(tips.source==MK8_TIP_KEYBOARD || ((controller_required || controller_preview) && symbol_help)) for(unsigned i=0;i<count;i++) {
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
        uint64_t pane=i ? hint_second : hint;
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

static Mk8Setting selected(void) {
    Mk8Setting row;
    if(!focus || !mk8_settings_values_row(category,focus-1,&row)) FATAL("[mk8:settings] invalid settings selection\n");
    return row;
}

static void settings_tips(Cpu *c) {
    bool stock_back=tips.source==MK8_TIP_NINTENDO && !strcmp(tips.label[MK8_TIP_BACK],"B");
    call(c,0x70008ade74ull,back_icon,stock_back,0,0,0);
    static const struct TipAction category_tips[]={{MK8_TIP_CONFIRM,"Category"},{MK8_TIP_DOWN,"Options"},{MK8_TIP_BACK,"Close"}};
    static const struct TipAction save[]={{MK8_TIP_CONFIRM,"OK"},{MK8_TIP_BACK,"Cancel"}};
    static const struct TipAction reset[]={{MK8_TIP_CONFIRM,"Reset"},{MK8_TIP_BACK,"Cancel"}};
    struct TipAction row_tips[]={{MK8_TIP_CONFIRM,"Change"},{MK8_TIP_BACK,"Back"}};
    const char *help=NULL;
    char range[40];
    if(exiting) help="Saving settings...";
    else if(mk8_settings_values_saved()<0) help="Save failed. Check folder; Back retries.";
    else if(mk8_settings_values_capturing()) help="Waiting for a new binding...";
    if(help) { action_tips(c,help,NULL,0,""); return; }
    if(editing) {
        snprintf(range,sizeof range,"Enter a value from 0 to %d.",edit_max);
        help=invalid ? range : tips.source==MK8_TIP_KEYBOARD ? "Type a value or use the arrow keys." : "Up/down: 1   Left/right: 10";
        action_tips(c,help,save,2,""); return;
    }
    if(reset_confirm) { action_tips(c,"Reset these bindings?",reset,2,""); return; }
    if(!focus) { action_tips(c,NULL,category_tips,3,"Select a category or setting."); return; }
    Mk8Setting row=selected();
    if(row.kind==SETTING_NUMBER) row_tips[0].verb="Edit";
    if(row.kind==SETTING_BINDING) row_tips[0].verb="Remap";
    if(row.kind==SETTING_RESET) row_tips[0].verb="Reset";
    unsigned first=row.enabled && row.kind!=SETTING_INFO ? 0 : 1;
    action_tips(c,row.help,row_tips+first,2-first,"");
}

static void draw(Cpu *c) {
    static const char *categories[] = {"Video", "Audio", "Controls"};
    uint64_t shown_revision=mk8_settings_values_revision();
    unsigned count=(unsigned)mk8_settings_values_count(category);
    if(!count) FATAL("[mk8:settings] settings catalogue unavailable\n");
    if(focus>count) focus=count;
    if(focus && focus<=offset) offset=focus-1;
    if(focus>offset+SETTINGS_VISIBLE) offset=focus-SETTINGS_VISIBLE;
    if(offset+SETTINGS_VISIBLE>count) offset=count>SETTINGS_VISIBLE ? count-SETTINGS_VISIBLE : 0;
    text(c,values[0],categories[category]);
    call(c,focus==0 ? 0x700044ed20ull : 0x700044ee50ull,rows[0],1,0,0,0);
    for (unsigned i=1;i<=SETTINGS_VISIBLE;i++) {
        char name[32]; snprintf(name,sizeof name,"L_SettingsRow%u",i);
        bool shown=offset+i<=count; visible(c,root,name,shown);
        if(!shown) continue;
        Mk8Setting row;
        if(!mk8_settings_values_row(category,offset+i-1,&row)) FATAL("[mk8:settings] settings row changed\n");
        text(c,labels[i],row.label);
        if(editing && row.id==editing_id) {
            char value[32];
            snprintf(value,sizeof value,replace_number ? "[%s]" : "%s_",number);
            text(c,values[i],value);
        } else text(c,values[i],reset_confirm && offset+i==focus ? "Confirm reset?" : row.value);
        call(c,offset+i==focus ? 0x700044ed20ull : 0x700044ee50ull,rows[i],1,0,0,0);
        for(unsigned t=0;t<2;t++) {
            uint64_t pane=t ? values[i] : labels[i]; heap(pane,0x60);
            ST8(pane+0x59,row.enabled ? 255 : 100); ST8(pane+0x58,LD8(pane+0x58)|2);
        }
        visible(c,rows[i]+0x30,"L_Btn_LR",row.enabled && (row.kind==SETTING_CHOICE || row.kind==SETTING_NUMBER));
    }
    if(scrollbar) {
        visible(c,root,"L_SettingsScroll",count>SETTINGS_VISIBLE);
        float args[5]={0,(float)offset,(float)(count>SETTINGS_VISIBLE ? count-SETTINGS_VISIBLE : 0),SETTINGS_VISIBLE,(float)count};
        for(unsigned i=0;i<5;i++) memcpy(&c->v[i],&args[i],sizeof(float));
        call(c,0x7000451ff4ull,scrollbar,0,0,0,0);
    }
    settings_tips(c);
    last_revision=shown_revision;
    LOG("[mk8:settings] category=%s focus=%u offset=%u\n",categories[category],focus,offset);
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
    uint64_t pane=find(c,root,"L_SettingsCursor"); heap(pane,0x60);
    memcpy((void *)(uintptr_t)(pane+0x48),&scale,sizeof scale);
    memcpy((void *)(uintptr_t)(pane+0x4c),&scale,sizeof scale);
    ST8(pane+0x58,LD8(pane+0x58)|0x10);
}

static void controller_highlight(Cpu *c) {
    bool footer=controller_focus>=4;
    for(unsigned i=0;i<4;i++)
        call(c,controller_focus==(int)i ? 0x700044ed20ull : 0x700044ee50ull,rows[i+1],1,0,0,0);
    for(unsigned i=0;i<2;i++) {
        animation_frame(c,dialog_animations[i],0,controller_focus==(int)i+4 ? 2 : 0);
        // Apply the OnOff pose now, including the neutral colors after the previous Decide pulse.
        call(c,0x70008ae0b8ull,dialog_buttons[i],0,0,0,0);
    }
    // Keep the four native corners apart; shrinking the cursor bounds alone makes them cross.
    float scale=footer ? 1.f : .5f;
    controller_cursor_scale(c,scale);
    float position[3]={footer && !controller_required ? (controller_focus==4 ? -200 : 200) : 0,footer ? -246 : 145-controller_focus*70,0};
    float size[2]={footer ? 370 : 1700,158};
    memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
    call(c,0x7000443404ull,dialog_cursor,c->sp+320,0,0,0);
    memcpy((void *)(uintptr_t)(c->sp+320),position,sizeof position);
    call(c,0x70008adff0ull,dialog_cursor+0x30,c->sp+320,0,0,0);
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
        animation_play(c,dialog_animations[click==5],2,1);
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
        call(c,0x70008adc54ull,values[i+1],c->sp+128,0,0,0);
    }
}

static void controller_draw(Cpu *c,enum Mk8EditState state,int listening,const uint16_t names[4][80],bool refresh) {
    static enum ControllerPanel last_panel;
    static enum Mk8EditState last_state;
    static int previous_player;
    bool enrolling=controller_listening(state), paired=enrolling && mk8_edit_has_pair();
    bool keyboard=controller_required && mk8_controller_join_keyboard();
    static bool last_paired,last_keyboard;
    int player=enrolling ? listening : controller_panel==PLAYERS ? -1 : action_player;
    bool source_changed=read_tips(false);
    bool changed=refresh || source_changed || last_panel!=controller_panel || last_state!=state || previous_player!=player ||
        paired!=last_paired || keyboard!=last_keyboard;
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
            const char *format=symbol_help && (state==MK8_EDIT_NEED_LEFT || state==MK8_EDIT_NEED_RIGHT) ? "Player %d: pair Joy-Cons" :
                state==MK8_EDIT_LISTEN && join_buttons[0] ? "Player %d: choose how to join" :
                keycaps[0] ? "Player %d: [L] + [R] to join" : "Player %d: [L] + [R] or SL + SR";
            snprintf(title,sizeof title,format,listening+1);
            heading=title; left="Back"; right="Use keyboard";
            switch(state) {
            case MK8_EDIT_AMBIGUOUS:
                snprintf(title,sizeof title,"Player %d: multiple Joy-Cons",listening+1);
                help=symbol_help ? "Release both. Pair one set at a time." : "Release all. One player holds [L] + [R]."; break;
            case MK8_EDIT_PAIR_HELD:
                snprintf(title,sizeof title,"Player %d: Joy-Con pair ready",listening+1);
                help="Release both shoulder buttons to join."; break;
            case MK8_EDIT_NEED_LEFT: help="Hold [R]. Press [L] on the left Joy-Con."; break;
            case MK8_EDIT_NEED_RIGHT: help="Hold [L]. Press [R] on the right Joy-Con."; break;
            default: help=paired ? "SL + SR on one Joy-Con splits the pair." :
                "To pair two Joy-Cons, hold L and R."; break;
            }
        } else if(state==MK8_EDIT_FAILED) help="Assign every player a connected input.";
        else if(state==MK8_EDIT_PENDING) help="Updating controller assignments...";
        if(controller_required) {
            controller_ready_button=state==MK8_EDIT_READY;
            left=controller_ready_button ? "OK" : "Cancel";
            if(state==MK8_EDIT_READY) help="All players are ready.";
        }
        instruction(c,title_label,40,heading);
        const struct TipAction roster[]={{MK8_TIP_CONFIRM,"Assign"},{MK8_TIP_SWAP,"Swap"},
            {MK8_TIP_CLEAR,tips.source==MK8_TIP_KEYBOARD ? "Clear row" : "Clear"},{MK8_TIP_CLEAR_ALL,"Clear all"}};
        static const struct TipAction submenu[]={{MK8_TIP_CONFIRM,"Select"},{MK8_TIP_BACK,"Back"}};
        static const struct TipAction ready={MK8_TIP_CONFIRM,"OK"};
        const struct TipAction back_tip={MK8_TIP_BACK,controller_required ? "Cancel" : "Back"};
        if(!help) action_tips(c,NULL,roster,4,"Click a player for assignment options.");
        else if(controller_panel!=PLAYERS) action_tips(c,help,submenu,2,"Click an action or Back.");
        else if(controller_required && state==MK8_EDIT_READY) action_tips(c,help,&ready,1,"Click OK.");
        else if(state==MK8_EDIT_LISTEN && keycaps[0]) {
            const char *key[]={"SL","SR",keyboard ? "Enter" : tips.label[MK8_TIP_BACK]};
            bool chords=join_buttons[0]!=0;
            const char *sideways=paired ? "Join sideways or split pair" : "Sideways Joy-Con";
            const char *verb[]={chords ? "Controller or paired Joy-Cons" : "+",chords ? sideways : "Join sideways",
                                keyboard ? "Join keyboard" : back_tip.verb};
            bool third=keyboard || ((symbol_help || tips.source==MK8_TIP_KEYBOARD) && *key[2] && strcmp(key[2],"(none)"));
            keycap_tips(c,chords ? NULL : help,key,verb,third ? 3 : 2,NULL,chords,keyboard);
        }
        else if(symbol_help && (state==MK8_EDIT_NEED_LEFT || state==MK8_EDIT_NEED_RIGHT)) {
            const char *keys[]={keyboard ? "Enter" : tips.label[MK8_TIP_BACK]};
            const char *verbs[]={keyboard ? "Join keyboard" : back_tip.verb};
            bool bound=*keys[0] && strcmp(keys[0],"(none)");
            keycap_tips(c,"",keys,verbs,bound,NULL,false,keyboard);
            hint_background(c,true,880);
            call(c,0x70008ade74ull,hint_second,false,0,0,0);
            float y=hint_text_y(true,false);
            call(c,0x70008ade74ull,action_labels[3],true,0,0,0);
            hint_box(c,action_labels[3],-205,y,96,50);
            text(c,action_labels[3],"Press");
            show_symbol(c,3,state==MK8_EDIT_NEED_LEFT ? 4 : 5,-134,y);
            hint_box(c,hint,77,y,350,50);
            text(c,hint,"on other Joy-Con to pair.");
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
        uint16_t back_icon=footer_binding ? button_glyph(footer_key) : 0;
        int mapped_back=footer_binding && symbol_dialog ? symbol_index(footer_key) : -1;
        if(symbol_dialog) {
            hide_symbol(c,4);
            hint_box(c,dialog_labels[1],mapped_back>=0 ? 20 : 0,-5,900,160);
        }
        if(mapped_back>=0) {
            show_symbol(c,4,mapped_back,-10-8*(float)strlen(left),0);
            text(c,dialog_labels[1],left);
        } else if(symbol_dialog && footer_binding && tips.source!=MK8_TIP_KEYBOARD &&
                  *footer_key && strcmp(footer_key,"(none)")) {
            char button[64];
            snprintf(button,sizeof button,"%s %s",footer_key,left);
            text(c,dialog_labels[1],button);
        } else if(back_icon && !symbol_dialog) {
            uint16_t *line=(uint16_t *)(uintptr_t)(c->sp+128);
            line[0]=back_icon; line[1]=' '; instruction_line(line+2,38,left);
            call(c,0x70008adc54ull,dialog_labels[1],c->sp+128,0,0,0);
        } else text(c,dialog_labels[1],left);
        text(c,dialog_labels[2],right);
        visible(c,root,"L_SettingsNo",!controller_required && controller_panel!=SWAP_TARGET);
        static const char *actions[]={"Assign controller","Use keyboard","Swap with player","Clear player"};
        for(unsigned i=0;i<4;i++) {
            char pane[20],label[16];
            snprintf(pane,sizeof pane,"L_SettingsRow%u",i+1);
            bool used=controller_panel==ROW_ACTIONS || i<controller_rows;
            visible(c,root,pane,used);
            if(!used) continue;
            snprintf(label,sizeof label,"Player %u",i+1);
            controller_place(c,labels[i+1],controller_panel==ROW_ACTIONS ? 0 : -245,0);
            text(c,labels[i+1],controller_panel==ROW_ACTIONS ? actions[i] : label);
            if(controller_panel==ROW_ACTIONS) text(c,values[i+1],"");
        }
        last_panel=controller_panel; last_state=state; previous_player=player; last_paired=paired; last_keyboard=keyboard;
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
            animation_play(c,dialog_animations[controller_press],2,1);
            if(controller_focus>=4) call(c,0x7000443320ull,dialog_cursor,0,0,0,0);
            // Character selection plays Back when the mandatory applet returns ControllerSupportCanceled.
            if(!required_cancel) menu_sound(c,accepted ? 0x7000ed87bfull : 0x7000ed87cfull);
            if(!controller_required) return;
        }
        if(!controller_required && !call(c,0x70008ad59cull,dialog_animations[controller_press],0,0,0,0)) return;
        bool faded=popup_fade(c,&controller_fade_at,true);
        if(controller_required && accepted) {
            // The applet waits for us. Sample the stock eight-frame Decide pulse on the fade clock.
            float frame=1.f+(1.f-reveal)*9.6f;
            animation_frame(c,dialog_animations[0],2,frame<8.f ? frame : 8.f);
            call(c,0x70008ae0b8ull,dialog_buttons[0],0,0,0,0);
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
    if(!show) text(c,title_label,"Settings");
    visible(c,root,"L_SettingsRow0",!show);
    visible(c,root,"L_SettingsBack",!show);
    visible(c,root,"L_SettingsDialog",!show);
    visible(c,root,"L_SettingsQuestion",!show);
    visible(c,root,"L_SettingsScroll",false);
    for(unsigned i=1;i<=SETTINGS_VISIBLE;i++) {
        char name[24];
        snprintf(name,sizeof name,"L_SettingsRow%u",i);
        visible(c,root,name,!show || i<=controller_rows);
        controller_place(c,find(c,root,name),0,show ? 145-(int)(i-1)*70 : 60-(int)(i-1)*60);
        if(!show) controller_place(c,labels[i],-245,0);
        if(!show || i>4) continue;
        visible(c,rows[i]+0x30,"L_Btn_LR",false);
        call(c,i==1 ? 0x700044ed20ull : 0x700044ee50ull,rows[i],1,0,0,0);
    }
    controller_place(c,find(c,root,"L_SettingsHint"),0,show ? -115 : -235);
    controller_place(c,find(c,root,"L_SettingsYes"),show && required ? 0 : -200,show ? -246 : -195);
    controller_place(c,find(c,root,"L_SettingsNo"),200,show ? -246 : -195);
    if(show) {
        appear(c,1);
        reveal_pane(c,"N_SettingsPopup",1);
        visible(c,root,"N_Settings",true);
        visible(c,root,"N_SettingsPopup",true);
        dialog_open_sound(c);
        controller_previous=mk8_controller_p1_navigation();
        controller_draw(c,state,listening,names,true);
        call(c,0x7000442f5cull,dialog_cursor,1,0,0,0);
        float speed=1; memcpy(&c->v[0],&speed,sizeof speed);
        call(c,0x70008ad300ull,cursor_animation,1,0,0,0);
    } else {
        visible(c,root,"N_Settings",false);
        visible(c,root,"N_SettingsPopup",false);
        controller_release();
        visible(c,root,"L_SettingsNo",true);
        controller_cursor_scale(c,1.f);
        float size[2]={370,158};
        memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
        call(c,0x7000443404ull,dialog_cursor,c->sp+320,0,0,0);
    }
    controller_visible=show;
}

static void dialog_draw(Cpu *c,bool instant) {
    static const char *messages[]={"Some settings require a restart.\nRestart now?","Yes","No"};
    if(symbol_dialog) {
        hide_symbol(c,4);
        hint_box(c,dialog_labels[1],0,-5,900,160);
    }
    for(unsigned i=0;i<3;i++) if(dialog_labels[i]) text(c,dialog_labels[i],messages[i]);
    for(unsigned i=0;i<2;i++) {
        bool selected=(i==0)==restart_yes;
        if(instant) animation_frame(c,dialog_animations[i],0,selected ? 2 : 0);
        else animation_play(c,dialog_animations[i],selected ? 0 : 1,selected ? 1 : -1);
    }
    float position[3]={restart_yes ? -200 : 200,-195,0};
    memcpy((void *)(uintptr_t)(c->sp+320),position,sizeof position);
    call(c,0x70008adff0ull,dialog_cursor+0x30,c->sp+320,0,0,0);
}

static void finish_return(Cpu *c,uint64_t page,uint64_t b) {
    (void)page;
    visible(c,root,"N_Settings",false);
    if(dialog) visible(c,root,"N_SettingsPopup",false);
    restart_prompt=restart_wait=restart_pressed=exit_pressed=false;
    if(top_root) visible(c,top_root,"N_Btn_00",true);
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

static void start_exit(Cpu *c) {
    if(post(SETTINGS_FINISH,0,1)) {
        exiting=true; exited_at=plat_now_ns(); exit_from=reveal;
        exit_pressed=reveal>0;
        if(exit_pressed) animation_play(c,back_animation,2,1);
        menu_sound(c,0x7000ed87cfull);
        entering=confirming=false; draw(c);
    }
}

void mk8_settings_page_build(uint64_t page) { top=page; }

void mk8_settings_shared_build(uint64_t page) {
    if(!mk8_controller_layout_available() && !mk8_settings_style_active()) return;
    heap(page,0x1f8);
    if(controller_visible) controller_release();
    if(active) (void)post(SETTINGS_FINISH,0,SETTINGS_DETACHED);
    mk8_settings_top_forget();
    Cpu *c=kernel_current_cpu();
    if (!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid selector construction context\n");
    Cpu saved=*c; c->sp-=512;
    owner=page; cursor_owner=0; hide_on_return=false;
    active=back=editing=entering=confirming=exiting=reset_confirm=restart_prompt=restart_wait=restart_pressed=exit_pressed=false; category=focus=offset=0;
    root=0; available=false; requested=false; top_root=restore_nav=0;
    controller_visible=false;
    top=0;
    root=attach(c,page,0,"L_SettingsRoot"); heap(root,0x88);
    uint64_t title=attach(c,page,root,"L_SettingsTitle");
    title_label=find(c,title,"T_Title_00"); text(c,title_label,"Settings");
    uint64_t help=attach(c,page,root,"L_SettingsHint");
    symbol_help=call(c,0x70008ad8e8ull,help,string(c,"P_Map0A"),0,0,0) ? help : 0;
    symbol_dialog=0;
    memset(symbol_parts,0,sizeof symbol_parts); memset(shown_symbols,0,sizeof shown_symbols);
    hint=find(c,help,"T_Title_00"); hint_second=find(c,help,"T_Title_01");
    hint_plate=find(c,help,"W_Title_00");
    hint_double=call(c,0x70008ad8e8ull,help,string(c,"W_HintDouble"),0,0,0);
    if(hint_double) hint_single=find(c,help,"W_HintSingle");
    keycaps[0]=call(c,0x70008ad8e8ull,help,string(c,"P_Key0"),0,0,0);
    if(keycaps[0]) for(unsigned i=0;i<4;i++) {
        char name[16];
        snprintf(name,sizeof name,"P_Key%u",i); keycaps[i]=find(c,help,name);
        snprintf(name,sizeof name,"T_Key%u",i); key_labels[i]=find(c,help,name);
        snprintf(name,sizeof name,"T_Verb%u",i); action_labels[i]=find(c,help,name);
    }
    join_buttons[0]=call(c,0x70008ad8e8ull,help,string(c,"P_JoinL"),0,0,0);
    if(join_buttons[0]) {
        static const char *const parts[]={"P_JoinL","P_JoinR","P_JoinSL","P_JoinSR"};
        for(unsigned i=0;i<4;i++) {
            join_buttons[i]=find(c,help,parts[i]);
            attach(c,page,help,parts[i]);
        }
        for(unsigned i=0;i<2;i++) {
            join_plus[i]=find(c,help,i ? "T_JoinPlus1" : "T_JoinPlus0");
            text(c,join_plus[i],"+");
        }
    }
    float second_line[3]={0,-44,0};
    memcpy((void *)(uintptr_t)(c->sp+320),second_line,sizeof second_line);
    call(c,0x70008ade34ull,hint_second,c->sp+320,0,0,0);
    attach(c,page,root,"L_SettingsBG");
    uint64_t back_button=attach(c,page,root,"L_SettingsBack");
    back_animation=button_animations(c,back_button);
    back_icon=find(c,back_button,"L_BtnIconB_00");
    for (unsigned i=0;i<=SETTINGS_VISIBLE;i++) {
        char name[32]; snprintf(name,sizeof name,"L_SettingsRow%u",i);
        uint64_t obj=call(c,0x70005902d0ull,0x2c8,4,0,0,0); heap(obj,0x2c8);
        memset((void *)(uintptr_t)obj,0,0x2c8);
        call(c,0x700044dee4ull,obj,0,0,0,0);
        uint64_t row=obj+0x30, vt=LD64(row);
        // Attach the native row without registering it in the main menu's navigation list.
        ST64(row,LD64(0x70013083e0ull)+0x60); ST32(c->sp+112,0);
        call(c,0x70008bc0c4ull,page,row,root,string(c,name),c->sp+112);
        ST64(row,vt);
        call(c,0x70008ae9bcull,row,4,0,0,0);
        call(c,0x70008ae7b4ull,row,4,0,0,0);
        call(c,0x700044e100ull,obj,i ? 0x7000ee8eccull : 0,0x7000ee93f8ull,0x7000ee9402ull,0x7000ee940cull);
        if (i) heap(LD64(obj+0x128),0x18);
        heap(LD64(obj+0x130),0x18); heap(LD64(obj+0x190),0x88);
        rows[i]=obj;
        labels[i]=i ? find(c,row,"T_Text_00") : 0; values[i]=find(c,row,"T_Text_01");
    }
    scrollbar=call(c,0x70005902d0ull,0x1b0,4,0,0,0); heap(scrollbar,0x1b0);
    call(c,0x7000451ed0ull,scrollbar,0,0,0,0);
    // Use the native scrollbar as a display; its stock input callback requires a Nintendo list owner.
    ST64(scrollbar+0x30,LD64(0x70013083e0ull)+0x60);
    ST32(c->sp+112,0);
    call(c,0x70008bc0c4ull,page,scrollbar+0x30,root,string(c,"L_SettingsScroll"),c->sp+112);
    call(c,0x70004521d4ull,scrollbar,0,0,0,0);
    dialog=attach(c,page,root,"L_SettingsDialog");
    dialog_labels[0]=find(c,attach(c,page,root,"L_SettingsQuestion"),"T_Title_00");
    visible(c,dialog,"L_WinBtn_C",false); visible(c,dialog,"N_Load_00",false);
    for(unsigned i=0;i<2;i++) {
        visible(c,dialog,i ? "L_WinBtn_R" : "L_WinBtn_L",false);
        dialog_buttons[i]=attach(c,page,root,i ? "L_SettingsNo" : "L_SettingsYes");
        uint64_t label=attach(c,page,dialog_buttons[i],"L_SettingsLabel");
        dialog_labels[i+1]=find(c,label,"T_Title_00");
        if(!i && symbol_help) symbol_dialog=label;
        dialog_animations[i]=button_animations(c,dialog_buttons[i]);
    }
    // Um cursor próprio preserva a posição e a animação do retículo da cena.
    dialog_cursor=call(c,0x70005902d0ull,0xc8,4,0,0,0); heap(dialog_cursor,0xc8);
    call(c,0x7000442a84ull,dialog_cursor,0,0,0,0);
    ST32(c->sp+112,0);
    call(c,0x70008bc0c4ull,page,dialog_cursor+0x30,root,string(c,"L_SettingsCursor"),c->sp+112);
    if(LD32(dialog_cursor+0x68)!=4) FATAL("[mk8:settings] cursor animation layout changed\n");
    uint64_t cursor_groups=LD64(dialog_cursor+0x70); heap(cursor_groups,4*8);
    cursor_animation=LD64(cursor_groups+8); heap(cursor_animation,0x30);
    call(c,0x7000442ca0ull,dialog_cursor,1,0,0,0);
    float size[2]={370,158}; memcpy((void *)(uintptr_t)(c->sp+320),size,sizeof size);
    heap(LD64(dialog_cursor+0xb8),0x60);
    call(c,0x7000443404ull,dialog_cursor,c->sp+320,0,0,0);
    visible(c,root,"N_SettingsPopup",false);
    *c=saved;
    LOG("[mk8:settings] native selector page built\n");
}

bool mk8_settings_page_active(void) { return requested || active || mk8_controller_layout_active(); }
bool mk8_settings_page_toggle(void) {
    if(shared_race) return false;
    if(!mk8_settings_style_active() || !available) return false;
    if(mk8_controller_layout_active()) return true;
    if(active) back=true;
    else { from_button=false; requested=true; }
    return true;
}
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
    editing_id=row.id; edit_max=row.maximum;
    snprintf(number,sizeof number,"%d",row.current); replace_number=true; invalid=false; editing=true;
}

static bool edit_number(uint64_t edge, bool cancel) {
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
        int value=atoi(number)+step; value=value<0 ? 0 : value>edit_max ? edit_max : value;
        snprintf(number,sizeof number,"%d",value); replace_number=true; invalid=false;
    }
    if (cancel) { editing=false; LOG("[mk8:settings] numeric edit cancelled\n"); }
    else if (commit) {
        if (!*number || atoi(number)>edit_max || invalid) invalid=true;
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
    if(page==top && page!=owner) { if(open) { from_button=true; requested=true; } return; }
    if (page!=owner || (!active && !open)) return;
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
        entering=true; confirming=from_button; back=false; previous=b; touched=touch; active=true;
        input_reset(); reset_confirm=false; atomic_store(&wheel,0); repeat_at=0;
        animation_frame(c,back_animation,0,0);
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
        visible(c,root,"N_Settings",false); appear(c,0); draw(c);
        if(!confirming) { visible(c,root,"N_Settings",true); dialog_open_sound(c); }
        LOG("[mk8:settings] native selector entering\n");
    }
    // Publish active before consuming the open request; do not erase a later reopen during exit.
    if(open) requested=false;
    if(exiting) {
        if(exit_pressed) {
            if(!call(c,0x70008ad5d4ull,back_animation,0,0,0,0)) { previous=b; touched=touch; *c=saved; return; }
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
                exiting=false; restart_prompt=true; restart_yes=false; back=false; restart_pressed=false; restart_fade_at=0;
                reveal_pane(c,"N_SettingsPopup",1);
                visible(c,root,"N_SettingsPopup",true); dialog_draw(c,true);
                call(c,0x7000442f5cull,dialog_cursor,1,0,0,0);
                float speed=1; memcpy(&c->v[0],&speed,sizeof speed);
                call(c,0x70008ad300ull,cursor_animation,1,0,0,0);
                LOG("[mk8:settings] saved; restart confirmation opened\n");
            }
        }
        previous=b; touched=touch; *c=saved; return;
    }
    if (entering) {
        bool cancel=back || ((b&~previous)&2);
        if (!cancel) {
            // Keep the original button visible until its native Decide pulse finishes.
            if (confirming) {
                if (!call(c,0x7000435cc0ull,button,0,0,0,0)) { previous=b; touched=touch; *c=saved; return; }
                confirming=false; entered_at=plat_now_ns();
                call(c,0x70008affa4ull,restore_nav,0,0,0,0);
                stock_cursors(c,HIDE_CURSORS);
                visible(c,top_root,"N_Btn_00",false); visible(c,root,"N_Settings",true);
                dialog_open_sound(c);
            }
            uint64_t elapsed=plat_now_ns()-entered_at;
            float progress=elapsed>=160000000ull ? 1 : elapsed/160000000.0f;
            appear(c,progress);
            previous=b; touched=touch;
            if (elapsed<160000000ull) { *c=saved; return; }
            LOG("[mk8:settings] native selector opened\n");
        } else { start_exit(c); previous=b; touched=touch; *c=saved; return; }
        entering=confirming=false;
    }
    if (!menu_input_captured()) {
        uint64_t edge=b&~previous;
        if(restart_prompt) {
            if(restart_wait) {
                if(mk8_settings_values_saved()==2) finish_return(c,page,b);
            } else if(restart_pressed) {
                // Os pulsos duram 8 e 16 quadros; consultar o quadro final, não o evento transitório de término.
                if(call(c,0x70008ad59cull,dialog_animations[restart_yes ? 0 : 1],0,0,0,0) &&
                   call(c,0x70008ad59cull,cursor_animation,0,0,0,0) && popup_fade(c,&restart_fade_at,false))
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
                    animation_play(c,dialog_animations[restart_yes ? 0 : 1],2,1);
                    call(c,0x7000443320ull,dialog_cursor,0,0,0,0);
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
        unsigned count=(unsigned)mk8_settings_values_count(category);
        if(mk8_settings_values_capturing()) { previous=b; touched=touch; if(last_revision!=mk8_settings_values_revision()) draw(c); *c=saved; return; }
        int scroll=atomic_exchange(&wheel,0);
        if(!editing && !reset_confirm && scroll) { int next=(int)focus+scroll; focus=next<0 ? 0 : next>(int)count ? count : (unsigned)next; change=true; }
        if (touch && !touched) {
            if (x>=20 && x<=340 && y>=600 && y<=680) close=true;
            if(reset_confirm && focus>offset && focus<=offset+SETTINGS_VISIBLE && x>650 && x<1010 &&
               y>=270+(int)(focus-offset-1)*60 && y<=330+(int)(focus-offset-1)*60) confirm=true;
            if(!editing && !reset_confirm && x>=200 && x<=1080 && y>=175 && y<=245) {
                focus=0; change=true;
                if(x<=270) direction=-1; else if(x>=1010) direction=1;
            }
            for (unsigned i=0;i<SETTINGS_VISIBLE;i++) if (!editing && !reset_confirm && offset+i<count && x>=200 && x<=1080 && y>=270+(int)i*60 && y<=330+(int)i*60) {
                focus=offset+i+1; change=true;
                if (x>=590 && x<=650) direction=-1;
                else if (x>=1010) direction=1;
                else if (x>650) confirm=true;
            }
        }
        if (editing) { change=edit_number(edge,close); close=false; }
        else if(reset_confirm) {
            if(close) { reset_confirm=false; close=false; change=true; }
            else if(confirm) { Mk8Setting row=selected(); change=post(SETTINGS_ACTIVATE,row.id,0); reset_confirm=false; }
        }
        else {
            if (edge&((1ull<<13)|(1ull<<17))) { focus=focus ? focus-1 : count; change=true; }
            if (edge&((1ull<<15)|(1ull<<19))) { focus=focus<count ? focus+1 : 0; change=true; }
            if (edge&((1ull<<12)|(1ull<<16))) direction=-1;
            if (edge&((1ull<<14)|(1ull<<18))) direction=1;
            if(focus) {
                Mk8Setting row=selected();
                if(row.enabled && confirm && row.kind==SETTING_NUMBER) { begin_number(row); change=true; }
                else if(row.enabled && confirm && row.kind==SETTING_RESET) { reset_confirm=true; change=true; }
                else if(row.enabled && confirm && row.kind==SETTING_BINDING) change|=post(SETTINGS_ACTIVATE,row.id,0);
                else if(row.enabled && (direction || confirm) && row.kind==SETTING_CHOICE) change|=post(SETTINGS_STEP,row.id,direction ? direction : 1);
                else if(row.enabled && direction && row.kind==SETTING_NUMBER) change|=post(SETTINGS_STEP,row.id,direction);
            } else if(direction || confirm) {
                category=(category+(direction<0 ? SETTINGS_CATEGORIES-1 : 1))%SETTINGS_CATEGORIES; offset=0; change=true;
            }
        }
        if (close) {
            start_exit(c);
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
void mk8_settings_page_frame(uint64_t manager) {
    uint64_t end=HEAP_BASE+kernel_heap_size();
    uint64_t page=race_heap(manager,0xc70,end) ? LD64(manager+0xc68) : 0;
    enum Mk8TitlePhase title_input=MK8_TITLE_INACTIVE;
    if(race_heap(page,0x128,end) && LD64(page)==MAIN_BASE+0x127cb38 && !mk8_auto_lan_requested()) {
        unsigned phase=LD32(page+0x120),step=LD32(page+0x124);
        if (phase==3 && step==0) title_input=MK8_TITLE_PRELUDE;
        else if (phase==3 && step>=1 && step<=3) title_input=MK8_TITLE_ENTRANCE;
        else if (phase==4 && step==0) title_input=MK8_TITLE_PROMPT;
    }
    mk8_title_input_phase(title_input);
    unsigned players=shared_race_scene(manager);
    if (players != shared_race) {
        mk8_input_race_players(players);
        shared_race=players;
    }
    if(shared_race) requested=false;
    bool style=mk8_settings_style_active();
    unsigned joining=mk8_controller_join_players();
    if(!style && (!owner || !mk8_controller_layout_available())) {
        controller_entry_players=0;
        if(mk8_controller_layout_active()) controller_release();
        return;
    }
    heap(manager,0xb60);
    unsigned count=LD32(manager+0xb50);
    if(count>32) FATAL("[mk8:settings] unexpected active page count\n");
    uint64_t pages=LD64(manager+0xb58);
    if(count) heap(pages,count*8);
    bool present=false,main_menu=false;
    unsigned parts=0,drivers=0;
    for(unsigned i=0;i<count;i++) {
        uint64_t p=LD64(pages+i*8); heap(p,0x1d0);
        if(p==owner) present=true;
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
    available=style && present && mk8_settings_values_count(0)>0;
    unsigned edit_players=0;
    if(present && !shared_race) {
        if(parts==0x16 && drivers==4) edit_players=atomic_load(&controller_applet_players);
        if(parts==0x15 && drivers==2) edit_players=2;
    }
    // The LAN layout hides the on-screen shortcut. Plus still opens the editor.
    controller_character_shortcut=parts==0x16 && edit_players!=0;
    controller_entry_players=edit_players;
    if(!present) {
        // This same page can return after the worker has finished in another scene.
        // Keep its saved cursors until we can safely close the roster on the owner.
        if(controller_visible && controller_required) { controller_detached=true; return; }
        if(!active && !hide_on_return) cursor_owner=0;
        if(mk8_controller_layout_active()) controller_release();
        if(active) {
            post(SETTINGS_FINISH,0,SETTINGS_DETACHED);
            hide_on_return=true;
            // This owner can reappear. Its edit state and previous scene's navigation cannot.
            editing=entering=confirming=exiting=reset_confirm=back=false;
            restart_prompt=restart_wait=restart_pressed=exit_pressed=from_button=false;
            top_root=restore_nav=0;
        }
        active=requested=false;
        return;
    }
    if(joining) controller_detached=false;
    if(hide_on_return) {
        // The same page is live again; a rebuilt page clears this flag in its constructor.
        Cpu *c=kernel_current_cpu();
        if(!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid return context\n");
        Cpu saved=*c; c->sp-=512;
        visible(c,root,"N_Settings",false);
        if(dialog) visible(c,root,"N_SettingsPopup",false);
        stock_cursors(c,RESTORE_CURSORS);
        *c=saved;
        hide_on_return=false;
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
    mk8_settings_page_tick(owner,atomic_load(&requested));
}
