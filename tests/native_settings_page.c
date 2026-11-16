#include "rt.h"
#include "settings_values.h"
#include <assert.h>
#include <string.h>
unsigned char memory[32768] __attribute__((aligned(16)));
static Cpu cpu;
static int returns, text_calls, save_state, resets, restart_choice=-1;
static bool overlay, finger, cursor_shown=true, decided, capturing;
static bool back_done=true, button_done=true, cursor_done=true;
static int back_pulses, button_pulses, cursor_pulses, select_sounds, decide_sounds, cancel_sounds;
static float cursor_x;
static int32_t tx,ty;
static uint64_t now, revision;
static char rendered[SETTINGS_VISIBLE+1][80];
static Mk8Setting catalogue[3][12];
static int counts[]={11,4,2};
static bool in_main=true;
bool mk8_settings_style_active(void) { return true; }
bool mk8_settings_values_main_menu(void) { return in_main; }
void mk8_settings_values_context(bool value) { in_main=value; }
uint64_t plat_now_ns(void) { return now; }
Cpu *kernel_current_cpu(void) { return &cpu; }
uint64_t test_read(uint64_t p) {
    if(p==0x70012f9e28ull) return 0x7000001000ull;
    assert(p>=HEAP_BASE && p+8<=GUEST_HI); return *(u64u *)(uintptr_t)p;
}
bool menu_input_captured(void) { return overlay; }
bool touch_first(int32_t *x,int32_t *y) { *x=tx; *y=ty; return finger; }
void input_reset(void) {}
void mk8_settings_top_forget(void) {}
int mk8_settings_values_count(unsigned group) { assert(group<3); return counts[group]; }
bool mk8_settings_values_row(unsigned group,unsigned row,Mk8Setting *out) {
    if(group>=3 || row>=(unsigned)counts[group]) return false; *out=catalogue[group][row]; return true;
}
uint64_t mk8_settings_values_revision(void) { return revision; }
int mk8_settings_values_saved(void) { return save_state; }
bool mk8_settings_values_capturing(void) { return capturing; }
bool mk8_settings_values_request(int op,int id,int value) {
    if(op==SETTINGS_BEGIN) { save_state=0; return true; }
    if(op==SETTINGS_FINISH) { save_state=1; return true; }
    if(op==SETTINGS_RESTART) { restart_choice=value; save_state=value ? 4 : 2; return true; }
    if(op==SETTINGS_ACTIVATE) { if(id==20)capturing=true; else if(id==19)resets++; else assert(0); return true; }
    for(unsigned g=0;g<3;g++) for(int i=0;i<counts[g];i++) if(catalogue[g][i].id==id) {
        Mk8Setting *r=&catalogue[g][i];
        r->current=op==SETTINGS_SET ? value : !r->current;
        snprintf(r->value,sizeof r->value,"%d",r->current); revision++; return true;
    }
    assert(0); return false;
}
void guest_call(Cpu *c,uint64_t address) {
    if(address==0x70008ad8e8ull) {
        const char *name=(void *)(uintptr_t)LD64(c->x[1]+8); unsigned row;
        if(!strcmp(name,"N_Settings")) c->x[0]=HEAP_BASE+12200;
        else if(!strcmp(name,"N_Btn_00")) c->x[0]=HEAP_BASE+12000;
        else if(!strcmp(name,"N_SettingsPopup")) c->x[0]=HEAP_BASE+15100;
        else if(sscanf(name,"L_SettingsRow%u",&row)==1) c->x[0]=HEAP_BASE+12400+row*128;
        else if(!strcmp(name,"L_Btn_LR")) c->x[0]=HEAP_BASE+13200+(c->x[0]-HEAP_BASE-4000-0x30)/512*128;
        else assert(0);
    } else if(address==0x70008ade74ull) {
        assert(c->x[0]>=HEAP_BASE+12000 && c->x[0]<HEAP_BASE+16000);
        ST8(c->x[0]+0x58,(LD8(c->x[0]+0x58)&~1)|c->x[1]);
    } else if(address==0x70008ade34ull) {
        assert(c->x[0]==HEAP_BASE+12200);
        memcpy((void *)(uintptr_t)(c->x[0]+0x30),(void *)(uintptr_t)c->x[1],12);
    } else if(address==0x70008affa4ull) {
        assert(c->x[0]==HEAP_BASE+9000 && !c->x[1]); ST32(c->x[0]+0x28,6);
    } else if(address==0x7000435d2cull) {
        assert(c->x[0]==HEAP_BASE+14000 && c->x[1]==0xffffffffu);
        // O reset nativo toca cancelamento quando o flag do widget está ligado.
        if(LD8(c->x[0]+0x3ca)) cancel_sounds++;
        ST16(c->x[0]+0x160,0); returns++;
    } else if(address==0x70008bbee0ull) {
        assert((c->x[0]==HEAP_BASE || c->x[0]==HEAP_BASE+21000) && !c->x[1]);
        assert(LD32(c->x[0]+0x1a8)&0x80000000u);
    } else if(address==0x70008afc54ull) {
        assert(c->x[0]==HEAP_BASE+9000);
    } else if(address==0x70008afdd0ull) {
        assert(c->x[0]==HEAP_BASE+9000); ST32(c->x[0]+0x28,3); ST32(c->x[0]+0x30,11);
    } else if(address==0x7000435cc0ull) { assert(c->x[0]==HEAP_BASE+14000); c->x[0]=decided; }
    else if(address==0x70008bf228ull) c->x[0]=HEAP_BASE+10000;
    else if(address==0x70008adf84ull) { assert(c->x[0]==HEAP_BASE+11000+0x30); c->x[0]=cursor_shown; }
    else if(address==0x70008ae098ull) { assert(c->x[0]==HEAP_BASE+11000+0x30); cursor_shown=c->x[1]!=0; }
    else if(address==0x70008ad3c4ull) {
        assert(c->x[0]==HEAP_BASE+15200 || c->x[0]==HEAP_BASE+15400 || c->x[0]==HEAP_BASE+15500);
        float frame; memcpy(&frame,&c->v[0],sizeof frame); assert(frame==0 || frame==2);
    }
    else if(address==0x70008ad6acull) {
        float speed; memcpy(&speed,&c->v[0],sizeof speed);
        assert(c->x[1]<=2 && c->x[2]==0 && c->x[3]==1);
        assert(speed==(c->x[1]==1 ? -1 : 1));
        if(c->x[0]==HEAP_BASE+15200) { assert(c->x[1]==2); back_pulses++; }
        else { assert(c->x[0]==HEAP_BASE+15400 || c->x[0]==HEAP_BASE+15500); if(c->x[1]==2)button_pulses++; }
    } else if(address==0x70008ad5d4ull) {
        assert(c->x[0]==HEAP_BASE+15200); c->x[0]=back_done;
    } else if(address==0x70008ad59cull) {
        if(c->x[0]==HEAP_BASE+15800)c->x[0]=cursor_done;
        else { assert(c->x[0]==HEAP_BASE+15400 || c->x[0]==HEAP_BASE+15500); c->x[0]=button_done; }
    } else if(address==0x70008adff0ull) {
        assert(c->x[0]==HEAP_BASE+16000+0x30);
        const float *p=(void *)(uintptr_t)c->x[1]; cursor_x=p[0]; assert((p[0]==-200 || p[0]==200) && p[1]==-195 && p[2]==0);
    } else if(address==0x7000442f5cull || address==0x7000443320ull) {
        assert(c->x[0]==HEAP_BASE+16000); if(address==0x7000443320ull)cursor_pulses++;
    } else if(address==0x70008ad300ull) {
        assert(c->x[0]==HEAP_BASE+15800 && c->x[1]==1);
    } else if(address==0x700008c608ull) {
        assert(c->x[1]==0x7000ed8620ull);
        if(c->x[0]==0x7000ed87a1ull)select_sounds++;
        else if(c->x[0]==0x7000ed87bfull)decide_sounds++;
        else if(c->x[0]==0x7000ed87cfull)cancel_sounds++;
        else assert(c->x[0]==0x7000ee9492ull || c->x[0]==0x7000ee94acull);
    }
    else if(address==0x700044ed20ull || address==0x700044ee50ull) {}
    else if(address==0x70008adc54ull) {
        text_calls++;
        if(c->x[0]==HEAP_BASE+3500) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1]; unsigned n=0;
            while(s[n]) n++;
            assert(n<=40); // Native title text buffer: 82 bytes including the UTF-16 terminator.
        }
        for(unsigned i=0;i<=SETTINGS_VISIBLE;i++) if(c->x[0]==HEAP_BASE+2000+i*256) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1]; unsigned n=0;
            for(;s[n] && n<79;n++) rendered[i][n]=(char)s[n]; rendered[i][n]=0;
        }
    } else assert(0);
    c->pc=address; c->nzcv=0x12345678; c->v[1]=0x1234;
}
#include "settings_page.c"
static void sample(uint64_t b) {
    int32_t st[4]={12000,-1000,600,700};
    uint64_t result=mk8_settings_page_input(b,st,b);
    if(active) assert(!result && !st[0] && !st[1] && !st[2] && !st[3]);
    Cpu saved=cpu; mk8_settings_page_tick(HEAP_BASE,false); assert(!memcmp(&saved,&cpu,sizeof cpu));
}
static void press(uint64_t b) { sample(0); sample(b); sample(0); }
static void open_page(void) { decided=true; mk8_settings_page_tick(HEAP_BASE,true); now+=160000000; sample(0); assert(active && !entering); }
static void apply_number(const char *s) { for(;*s;s++) assert(mk8_settings_page_key(*s)); mk8_settings_page_key(13); sample(0); }
int main(void) {
    for(unsigned g=0;g<3;g++) for(int i=0;i<counts[g];i++) {
        Mk8Setting *r=&catalogue[g][i]; r->id=g==0 ? i : g==1 ? 11+i : i==0 ? 20 : 19;
        r->enabled=true; r->maximum=1;
        snprintf(r->label,sizeof r->label,"Option %d",i); strcpy(r->value,"0"); strcpy(r->help,"Changes a setting.");
    }
    catalogue[1][0].kind=SETTING_NUMBER; catalogue[1][0].maximum=200; catalogue[1][0].current=100; strcpy(catalogue[1][0].value,"100%");
    catalogue[2][0].kind=SETTING_BINDING; catalogue[2][1].kind=SETTING_RESET;
    memset(&cpu,0x17,sizeof cpu); cpu.sp=GUEST_HI-16;
    top=owner=root=HEAP_BASE; hint=HEAP_BASE+3500; from_button=true;
    ST32(top+0x120,4); ST8(HEAP_BASE+14000+0x3ca,1);
    back_animation=HEAP_BASE+15200;
    ST64(owner+0x1b0,HEAP_BASE+9000); ST32(HEAP_BASE+9000+0x10,12); ST32(HEAP_BASE+9000+0x28,5);
    ST64(HEAP_BASE+9000+0x18,HEAP_BASE+9200); ST64(HEAP_BASE+9200+88,HEAP_BASE+14000+0x30);
    ST32(HEAP_BASE+14000+0x48,107); ST8(HEAP_BASE+12000+0x58,1); ST64(HEAP_BASE+10000+0x1d0,HEAP_BASE+11000);
    ST64(owner+0x1d0,HEAP_BASE+11000); ST64(top+0x448,root);
    for(unsigned i=0;i<=SETTINGS_VISIBLE;i++) { labels[i]=HEAP_BASE+1000+i*256; values[i]=HEAP_BASE+2000+i*256; rows[i]=HEAP_BASE+4000+i*512; }
    Cpu saved=cpu; mk8_settings_page_tick(HEAP_BASE,true);
    assert(active && confirming && cursor_shown && !memcmp(&saved,&cpu,sizeof cpu));
    now=2000000000; sample(1ull<<14); assert(confirming && !category && (LD8(HEAP_BASE+12000+0x58)&1));
    decided=true; sample(1ull<<14); assert(!confirming && !cursor_shown && !LD8(HEAP_BASE+12200+0x59));
    now+=80000000; sample(1ull<<14); assert(entering && LD8(HEAP_BASE+12200+0x59)==127);
    now+=80000000; sample(1ull<<14); assert(!entering && !category && LD8(HEAP_BASE+12200+0x59)==255); sample(0);
    mk8_settings_page_fixed(1ull<<15); sample(0); assert(focus==1);
    mk8_settings_page_fixed(0); now+=600000000;
    mk8_settings_page_tick(owner,false); assert(focus==1); // P2 reassignment can stop all P1 HID polling.
    mk8_settings_page_fixed(1); mk8_settings_page_tick(owner,false); assert(catalogue[0][0].current==1);
    mk8_settings_page_fixed(0); mk8_settings_page_tick(owner,false);
    mk8_settings_page_fixed(1); mk8_settings_page_tick(owner,false); assert(catalogue[0][0].current==0);
    mk8_settings_page_fixed(0); mk8_settings_page_tick(owner,false); focus=offset=0;
    for(int i=0;i<6;i++) press(1ull<<15);
    assert(focus==6 && offset==1);
    mk8_settings_page_wheel(3); sample(0); assert(focus==9 && offset==4);
    focus=0; offset=0; finger=true; tx=1050; ty=420; sample(0); finger=false; sample(0);
    assert(focus==3 && catalogue[0][2].current==1);
    focus=0; press(1ull<<14); assert(category==1);
    press(1ull<<15); press(1); assert(editing);
    apply_number("80"); assert(!editing && catalogue[1][0].current==80);
    press(1); apply_number("201"); assert(editing && invalid && catalogue[1][0].current==80);
    press(2); assert(!editing && active);
    press(1); apply_number("1000"); assert(editing && invalid); press(2);
    press(1); mk8_settings_page_key(8); mk8_settings_page_key(13); sample(0); assert(editing && invalid && !*number);
    apply_number("0"); assert(!editing && !catalogue[1][0].current);
    press(1); press(1ull<<14); press(1); assert(catalogue[1][0].current==10);
    overlay=true; unsigned before=focus; press(1ull<<15); assert(focus==before); overlay=false;
    focus=0; press(1ull<<14); assert(category==2); press(1ull<<15); press(1); assert(capturing);
    press(1ull<<15); assert(focus==1); capturing=false; revision++; sample(0);
    press(1ull<<15); press(1); assert(reset_confirm); press(2); assert(!reset_confirm && !resets && active);
    press(1); press(1); assert(resets==1 && !reset_confirm);
    press(2); assert(exiting && active && !cursor_shown && !returns);
    now+=80000000; sample(0); assert(active && LD8(HEAP_BASE+12200+0x59)==127);
    now+=80000000; sample(0); assert(active && exiting && !returns); // Await save acknowledgment.
    save_state=2; sample(0); assert(!active && cursor_shown && returns==1);
    open_page(); press(2); save_state=-1; now+=80000000; sample(0); assert(active && entering && !exiting);
    now+=160000000; sample(0); assert(active && !entering && !cursor_shown);
    press(2); save_state=2; now+=160000000; sample(0); assert(!active && returns==2);
    decided=false; mk8_settings_page_tick(HEAP_BASE,true); press(2); assert(exiting && exit_from==0);
    save_state=2; sample(0); assert(!active && cursor_shown && returns==3);
    open_page(); mk8_settings_page_fixed(1ull<<15); sample(0); assert(focus==0); // fixed input wraps from reset row.
    mk8_settings_page_fixed(0); sample(0); finger=true; tx=300; ty=640; sample(0); finger=false; sample(0);
    assert(exiting); save_state=2; now+=160000000; sample(0);
    assert(!active && cursor_shown && returns==4 && text_calls>0);
    uint64_t manager=HEAP_BASE+16000;
    ST32(manager+0xb50,1); ST64(manager+0xb58,HEAP_BASE+20000); ST64(HEAP_BASE+20000,owner);
    ST32(top+0x120,4); mk8_settings_page_frame(manager); assert(in_main && available);
    top=HEAP_BASE+21000; mk8_settings_page_frame(manager); assert(!in_main && available);
    assert(mk8_settings_page_toggle() && mk8_settings_page_active());
    mk8_settings_page_tick(owner,atomic_exchange(&requested,false));
    now+=160000000; sample(0); assert(active && !confirming && !restore_nav && !top_root);
    int32_t sticks[4]={20000,20000,20000,20000};
    assert(!mk8_settings_page_input(1,sticks,1) && !sticks[0] && !sticks[1] && !sticks[2] && !sticks[3]);
    assert(mk8_settings_page_toggle()); sample(0); save_state=2; sample(0); now+=160000000; sample(0);
    assert(!active && cursor_shown && returns==4);
    ST32(manager+0xb50,0); ST64(manager+0xb58,0); mk8_settings_page_frame(manager);
    assert(!available && !mk8_settings_page_toggle());
    top=owner; in_main=true; available=true; from_button=true;
    dialog=HEAP_BASE+15600; dialog_animations[0]=HEAP_BASE+15400; dialog_animations[1]=HEAP_BASE+15500;
    dialog_cursor=HEAP_BASE+16000; cursor_animation=HEAP_BASE+15800;
    open_page(); back_done=false; int cancels_before=cancel_sounds, pulses_before=back_pulses;
    press(2); save_state=3; now+=400000000; sample(2);
    assert(exiting && exit_pressed && !restart_prompt && reveal==1);
    assert(cancel_sounds==cancels_before+1 && back_pulses==pulses_before+1);
    back_done=true; sample(0); now+=160000000; sample(0);
    assert(restart_prompt && !restart_yes && active && !exiting && cursor_x==200);
    press(1); sample(0); assert(restart_choice==0 && !active && returns==5); // No saves without rebooting.
    assert(decide_sounds==1 && button_pulses==1 && cursor_pulses==1);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    press(1ull<<12); assert(restart_yes && cursor_x==-200 && select_sounds==1);
    press(1ull<<12); assert(select_sounds==1); // No extra sound at the same selection.
    cancels_before=cancel_sounds; press(2); sample(0);
    assert(!active && restart_choice==0 && cancel_sounds==cancels_before+1 && decide_sounds==1);
    assert(button_pulses==2 && cursor_pulses==2 && cursor_x==200 && returns==6);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    button_done=cursor_done=false; press(1);
    assert(restart_choice==0 && restart_pressed && !restart_wait && active && !restart_yes);
    button_done=cursor_done=true; sample(0); sample(0); assert(!active && restart_choice==0 && returns==7);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    button_done=cursor_done=false; finger=true; tx=440; ty=555; sample(0); sample(0); finger=false; sample(0);
    assert(restart_choice==0 && restart_pressed && !restart_wait && active && restart_yes);
    sample(1); sample(1ull<<14); assert(restart_yes && decide_sounds==3 && button_pulses==4);
    button_done=true; sample(0); assert(!restart_wait); // The cursor also has a native Decide pulse.
    cursor_done=true; sample(0); assert(restart_choice==1 && restart_wait && active);
    sample(1); assert(decide_sounds==3 && cursor_pulses==4); // Wait for the real reboot.
    settings_page_reset(); assert(!owner && !active && !suppressed);
    owner=root=HEAP_BASE; top=HEAP_BASE+21000; available=in_main=true;
    back_animation=HEAP_BASE+15200; dialog=HEAP_BASE+15600;
    dialog_cursor=HEAP_BASE+16000; cursor_animation=HEAP_BASE+15800;
    ST64(top+0x1b0,HEAP_BASE+9000); ST64(top+0x448,root); ST32(top+0x120,4);
    for(unsigned icon=0;icon<2;icon++) for(unsigned prompt=0;prompt<2;prompt++) {
        int cancels=cancel_sounds, decides=decide_sounds, restored=returns;
        sample(0); decided=back_done=button_done=cursor_done=true;
        if(icon) mk8_settings_page_tick(top,true);
        else assert(mk8_settings_page_toggle());
        mk8_settings_page_tick(owner,atomic_exchange(&requested,false));
        now+=160000000; sample(0); assert(active && !entering && from_button==(bool)icon);
        press(2); assert(cancel_sounds==cancels+1); // Feedback imediato, antes do retorno.
        save_state=prompt ? 3 : 2; now+=160000000; sample(0);
        if(prompt) {
            assert(restart_prompt && !restart_yes);
            press(1); sample(0); assert(restart_choice==0);
        }
        assert(!active && cancel_sounds==cancels+1 && decide_sounds==decides+(int)prompt);
        assert(returns==restored+(int)icon && LD8(HEAP_BASE+14000+0x3ca)==1);
    }
    settings_page_reset();
}
