#include "rt.h"
#include "hle.h"
#include "settings_values.h"
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static uint64_t owner, labels[SETTINGS_VISIBLE+1], values[SETTINGS_VISIBLE+1], rows[SETTINGS_VISIBLE+1], root, hint;
static uint64_t top, top_root, restore_nav, scrollbar;
static uint64_t dialog, dialog_buttons[2], dialog_animations[2];
static uint64_t dialog_labels[3];
static uint64_t dialog_cursor, cursor_animation, back_animation;
static bool restart_prompt, restart_yes, restart_wait, restart_pressed, exit_pressed;
static _Atomic bool from_button;
static _Atomic bool available, requested;
static unsigned category, focus, offset;
static uint64_t previous;
static bool touched;
static bool entering, confirming, exiting, reset_confirm;
static uint64_t entered_at, exited_at, last_revision, repeat_at;
static float reveal, exit_from;
static uint64_t reticle, button;
static bool reticle_visible;
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

RT_ON_RESET(settings_page_reset) {
    owner = root = reticle = button = 0; active = back = editing = entering = confirming = exiting = false;
    top=top_root=restore_nav=scrollbar=0; available=requested=from_button=false;
    dialog=dialog_cursor=cursor_animation=back_animation=0;
    restart_prompt=restart_wait=restart_pressed=exit_pressed=false;
    buttons = suppressed = fixed_buttons = raw_buttons = 0; wheel=0;
}

static void heap(uint64_t p, size_t n) {
    if (p < HEAP_BASE || p >= HEAP_BASE+HEAP_MAX || n > HEAP_BASE+HEAP_MAX-p)
        FATAL("[mk8:settings] invalid selector object: %llx size=%zu\n",p,n);
}

static uint64_t call(Cpu *c, uint64_t address, uint64_t a, uint64_t b, uint64_t d, uint64_t e, uint64_t f) {
    c->x[0]=a; c->x[1]=b; c->x[2]=d; c->x[3]=e; c->x[4]=f;
    guest_call(c, address);
    return c->x[0];
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

static void appear(Cpu *c, float progress) {
    reveal=progress;
    uint64_t pane=find(c,root,"N_Settings"); heap(pane,0x60);
    float eased=progress*progress*(3-2*progress), translation[3]={0,-16*(1-eased),0};
    ST8(pane+0x59,(uint8_t)(255*eased));
    ST8(pane+0x58,LD8(pane+0x58)|2);
    memcpy((void *)(uintptr_t)(c->sp+320),translation,sizeof translation);
    call(c,0x70008ade34ull,pane,c->sp+320,0,0,0);
}

static void text(Cpu *c, uint64_t pane, const char *value) {
    size_t n = strlen(value);
    if (n > 79) FATAL("[mk8:settings] selector value too long\n");
    for (size_t i=0; i<=n; i++) ST16(c->sp+128+i*2,(unsigned char)value[i]);
    call(c,0x70008adc54ull,pane,c->sp+128,0,0,0);
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

static void draw(Cpu *c) {
    static const char *categories[] = {"Video", "Audio", "Controls"};
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
    char help[80];
    if(exiting) snprintf(help,sizeof help,"Saving settings...");
    else if(mk8_settings_values_saved()<0) snprintf(help,sizeof help,"Save failed. Check folder; Back retries.");
    else if(editing) snprintf(help,sizeof help,invalid ? "Use 1-3 digits, from 0 to %d" : "Type/arrows | A: OK | B/Esc: cancel",edit_max);
    else if(reset_confirm) snprintf(help,sizeof help,"Reset bindings? A: reset | B/Esc: cancel");
    else if(focus) {
        Mk8Setting row=selected(); snprintf(help,sizeof help,"%s",row.help);
    } else snprintf(help,sizeof help,"Select settings to modify");
    if(hint) text(c,hint,help);
    last_revision=mk8_settings_values_revision();
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

static void dialog_draw(Cpu *c,bool instant) {
    static const char *messages[]={"Some settings require a restart.\nRestart now?","Yes","No"};
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
    call(c,0x70008ae098ull,reticle,reticle_visible,0,0,0); reticle=0;
    if(from_button) mk8_settings_button_restore(top);
    else if(restore_nav) {
        call(c,0x70008afc54ull,restore_nav,0,0,0,0);
        call(c,0x70008afdd0ull,restore_nav,0,0,0,0);
    }
    top_root=restore_nav=0; suppressed=raw_buttons|b; active=false; exiting=false;
    input_reset();
    LOG("[mk8:settings] native selector closed and saved\n");
}

static void start_exit(Cpu *c) {
    if(post(SETTINGS_FINISH,0,1)) {
        exiting=true; exited_at=plat_now_ns(); exit_from=reveal;
        exit_pressed=reveal>0;
        if(exit_pressed) animation_play(c,back_animation,2,1);
        call(c,0x700008c608ull,0x7000ed87cfull,0x7000ed8620ull,0,0,0);
        entering=confirming=false; draw(c);
    }
}

void mk8_settings_page_build(uint64_t page) { top=page; }

void mk8_settings_shared_build(uint64_t page) {
    if(!mk8_settings_style_active()) return;
    heap(page,0x1f8);
    if(active) (void)post(SETTINGS_FINISH,0,0);
    mk8_settings_top_forget();
    Cpu *c=kernel_current_cpu();
    if (!c || c->sp<GUEST_LO+512 || c->sp>=GUEST_HI) FATAL("[mk8:settings] invalid selector construction context\n");
    Cpu saved=*c; c->sp-=512;
    owner=page; active=back=editing=entering=confirming=exiting=reset_confirm=restart_prompt=restart_wait=restart_pressed=exit_pressed=false; category=focus=offset=0;
    root=0; available=false; requested=false; top_root=restore_nav=0;
    top=0;
    root=attach(c,page,0,"L_SettingsRoot"); heap(root,0x88);
    uint64_t title=attach(c,page,root,"L_SettingsTitle");
    text(c,find(c,title,"T_Title_00"),"Settings");
    hint=find(c,attach(c,page,root,"L_SettingsHint"),"T_Title_00");
    attach(c,page,root,"L_SettingsBG");
    back_animation=button_animations(c,attach(c,page,root,"L_SettingsBack"));
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
        dialog_labels[i+1]=find(c,attach(c,page,dialog_buttons[i],"L_SettingsLabel"),"T_Title_00");
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

bool mk8_settings_page_active(void) { return active || requested; }
bool mk8_settings_page_toggle(void) {
    if(!mk8_settings_style_active() || !available) return false;
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
    raw_buttons=b; buttons=scripted;
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
    if (open && !active) {
        if(!post(SETTINGS_BEGIN,0,0)) FATAL("[mk8:settings] settings request queue full before opening\n");
        active=true; entering=true; confirming=from_button; back=false; previous=b; touched=touch;
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
        uint64_t cursor=LD64(owner+0x1d0); heap(cursor,0xc8);
        reticle=cursor+0x30;
        reticle_visible=call(c,0x70008adf84ull,reticle,0,0,0,0)!=0;
        if(!confirming) {
            entered_at=plat_now_ns();
            if(restore_nav) call(c,0x70008affa4ull,restore_nav,0,0,0,0);
            call(c,0x70008ae098ull,reticle,0,0,0,0);
            if(top_root) visible(c,top_root,"N_Btn_00",false);
        }
        visible(c,root,"N_Settings",false); appear(c,0); draw(c);
        if(!confirming) visible(c,root,"N_Settings",true);
        LOG("[mk8:settings] native selector entering\n");
    }
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
                exiting=false; restart_prompt=true; restart_yes=false; back=false; restart_pressed=false;
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
                call(c,0x70008ae098ull,reticle,0,0,0,0);
                visible(c,top_root,"N_Btn_00",false); visible(c,root,"N_Settings",true);
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
                   call(c,0x70008ad59cull,cursor_animation,0,0,0,0))
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
                    if(!confirm) call(c,0x700008c608ull,0x7000ed87a1ull,0x7000ed8620ull,0,0,0);
                }
                if(confirm) {
                    restart_pressed=true;
                    animation_play(c,dialog_animations[restart_yes ? 0 : 1],2,1);
                    call(c,0x7000443320ull,dialog_cursor,0,0,0,0);
                    call(c,0x700008c608ull,close ? 0x7000ed87cfull : 0x7000ed87bfull,0x7000ed8620ull,0,0,0);
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
                call(c,0x700008c608ull,old_focus!=focus ? 0x7000ee9492ull : 0x7000ee94acull,0x7000ed8620ull,0,0,0);
        }
        previous=b; touched=touch;
    }
    *c=saved;
}

// The shared cursor page is owned by each UI scene, including the race scene.
void mk8_settings_page_frame(uint64_t manager) {
    if(!mk8_settings_style_active()) return;
    heap(manager,0xb60);
    unsigned count=LD32(manager+0xb50);
    if(count>32) FATAL("[mk8:settings] unexpected active page count\n");
    uint64_t pages=LD64(manager+0xb58);
    if(count) heap(pages,count*8);
    bool present=false,main_menu=false;
    for(unsigned i=0;i<count;i++) {
        uint64_t p=LD64(pages+i*8); heap(p,0x1d0);
        if(p==owner) present=true;
        if(p==top && LD32(p+0x120)==4) main_menu=true;
    }
    mk8_settings_values_context(main_menu);
    available=present && mk8_settings_values_count(0)>0;
    if(!present) { if(active) post(SETTINGS_FINISH,0,0); active=requested=false; return; }
    mk8_settings_page_tick(owner,atomic_exchange(&requested,false));
}
