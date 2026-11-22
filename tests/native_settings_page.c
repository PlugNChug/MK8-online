#include "rt.h"
#include "settings_values.h"
#include "../ui/settings_children.h"
#include "controller_edit.h"
#include "controller_tips.h"
#include "../ui/menu_play.h"
#include "../net/social.h"
#include <assert.h>
#include <string.h>
#include <wchar.h>
unsigned char memory[32768] __attribute__((aligned(16)));
static Cpu cpu;
static int returns, text_calls, save_state, resets, restart_choice=-1;
static bool finish_queue_full;
static int finish_mode;
static unsigned begin_requests,finish_requests;
static bool overlay, finger, decided, capturing;
static unsigned cursor_mask=11, cursor_writes;  // P1, P2 and P4 are visible before entering Settings.
#define cursor_shown (cursor_mask&1)
static bool back_done=true, button_done=true, cursor_done=true;
static float footer_frame[2], footer_drawn[2];
static int back_pulses, button_pulses, cursor_pulses, open_sounds, select_sounds, decide_sounds, cancel_sounds;
static float cursor_x, cursor_y, cursor_width, cursor_height;
static uint64_t popup_buttons;
uint64_t mk8_controller_p1_navigation(void) { return popup_buttons; }
static uint16_t popup_names[4][80]={{'P','r','o'}, {'L',' ','+',' ','R'}, {'L'}, {'R'}};
void mk8_controller_names(uint16_t out[4][80]) { memcpy(out,popup_names,sizeof popup_names); }
static enum Mk8EditCommand last_edit;
static enum Mk8EditState editor_state;
static int edit_row,edit_other;
static unsigned join_players;
static bool join_keyboard,join_pad_back;
unsigned mk8_controller_join_players(void) { return join_players; }
bool mk8_controller_join_keyboard(void) { return join_keyboard; }
bool mk8_controller_join_back_pressed(void) { return join_pad_back; }
bool mk8_controller_layout_available(void) { return true; }
static bool wait_apply,bad_apply;
bool mk8_edit_command(enum Mk8EditCommand cmd,int row,int other) {
    last_edit=cmd; edit_row=row; edit_other=other;
    switch(cmd) {
    case MK8_EDIT_CANCEL: editor_state=MK8_EDIT_CLOSED; break;
    case MK8_EDIT_APPLY: editor_state=wait_apply ? MK8_EDIT_PENDING : bad_apply ? MK8_EDIT_FAILED : MK8_EDIT_APPLIED; break;
    case MK8_EDIT_ASSIGN: case MK8_EDIT_CLEAR_ALL: editor_state=MK8_EDIT_LISTEN; break;
    default: editor_state=MK8_EDIT_READY; break;
    }
    return true;
}
enum Mk8EditState mk8_edit_view(uint16_t names[4][80],int *row) {
    if(names) memcpy(names,popup_names,sizeof popup_names);
    if(row) *row=edit_row;
    return editor_state;
}
bool mk8_edit_has_combined(void) { return false; }
static uint16_t last_name[80],last_footer_unit[80];
static char last_hint[41];
static uint16_t shown_tips[2][41];
static wchar_t shown_title[41], shown_cancel[41];
static char key_strings[4][41],action_strings[4][41];
#define TEST_KEYCAP(i) (HEAP_BASE+16400+(i)*400)
static struct Mk8ControlTips fixed_tips={MK8_TIP_KEYBOARD,{"Enter","Esc","","","","Up","Down","Left","Right"}},
    popup_tips={MK8_TIP_NINTENDO,{"A","B","X","Y","Minus"}};
void mk8_control_tips(bool settings,struct Mk8ControlTips *out) { *out=settings ? fixed_tips : popup_tips; }
static unsigned extra_holds;
void mk8_controller_hold_extra_inputs(void) { ++extra_holds; }
static int32_t tx,ty;
static uint64_t now, revision;
static bool publish_during_row;
static char rendered[SETTINGS_VISIBLE+1][80];
static char rendered_labels[SETTINGS_VISIBLE+1][80];
static Mk8Setting catalogue[SETTINGS_CATEGORIES][12];
static int counts[]={12,4,2,5};
static bool in_main=true;
static bool native_style=true, auto_lan, title_active;
bool mk8_auto_lan_requested(void) { return auto_lan; }
void mk8_title_input_active(bool active) { title_active=active; }
static unsigned race_input_players;
void mk8_input_race_players(unsigned players) { race_input_players=players; }
static uint64_t game_root, root_node, root_context;
uint64_t kernel_heap_size(void) { return HEAP_MAX; }
static bool probe_handoff, probe_reopen;
static void observe_request_handoff(void);
static void request_reopen(void);
static int rich_group=-1;
static unsigned rich_version,rich_level;
void mk8_native_rows_open(int group) { rich_group=group; rich_level=0; rich_version++; }
void mk8_native_rows_close(void) { rich_group=-1; rich_level=0; rich_version++; }
int mk8_native_rows_count(unsigned *revision,unsigned *depth) {
    *revision=rich_version; *depth=rich_level; return rich_group<0 ? 0 : 2;
}
bool mk8_native_rows_get(int i,unsigned revision,Mk8NativeRow *out) {
    if(i<0 || i>=2 || rich_group<0 || revision!=rich_version) return false;
    *out=(Mk8NativeRow){.label="Yes, quit to Start Screen",.enabled=i==1}; return true;
}
bool mk8_native_rows_activate(int i,unsigned revision,int step) {
    if(i!=1 || revision!=rich_version || step) return false;
    ++rich_level; ++rich_version; return true;
}
bool mk8_native_rows_back(void) {
    if(!rich_level) return false;
    --rich_level; ++rich_version; return true;
}
static bool mii_available, rooms_available;
static int mii_requests, rooms_requests;
static Mk8PlaySession test_session;
static Mk8Social test_rooms;
static bool rooms_connected, room_account_available;
void mk8_menu_nextendo_snapshot(Mk8Social *out,bool *connected,bool *account_available) {
    *out=test_rooms; *connected=rooms_connected; *account_available=room_account_available;
}
void mk8_menu_play_session(Mk8PlaySession *out) { *out=test_session; }
bool mk8_mii_menu_available(void) { return mii_available; }
bool mk8_room_menu_available(void) { return rooms_available; }
void mk8_open_mii_menu(void) { assert(mii_available); ++mii_requests; }
void mk8_open_room_menu(void) { assert(rooms_available); ++rooms_requests; }
bool mk8_settings_style_active(void) { return native_style; }
static bool missing_layout,loading_layout;
bool mk8_settings_input_native(void) { return native_style; }
bool mk8_settings_layout_available(void) { return !missing_layout && !loading_layout; }
bool mk8_settings_layout_unavailable(void) { return missing_layout; }
static unsigned overlay_opens;
void mk8_menu_request_overlay(void) { ++overlay_opens; }
static uint64_t fader_mount;
uint64_t mk8_settings_fader_root(uint64_t page) { (void)page; return fader_mount; }
static bool loading_pane;
bool mk8_settings_fader_covers(uint64_t page) { (void)page; return loading_pane; }
bool mk8_settings_values_main_menu(void) { return in_main; }
void mk8_settings_values_context(bool value) { in_main=value; }
uint64_t plat_now_ns(void) { return now; }
Cpu *kernel_current_cpu(void) { return &cpu; }
uint64_t test_read(uint64_t p) {
    if(p==MAIN_BASE+0x1358c70) return game_root;
    if(p==MAIN_BASE+0x133ebd0+0x20) return root_node;
    if(p==MAIN_BASE+0x133ebd0+0x28) return root_context;
    if(p==0x70012f9e28ull) return 0x7000001000ull;
    assert(p>=HEAP_BASE && p+8<=GUEST_HI); return *(u64u *)(uintptr_t)p;
}
bool menu_input_captured(void) { return overlay; }
bool touch_first(int32_t *x,int32_t *y) {
    if(probe_handoff) { probe_handoff=false; observe_request_handoff(); }
    *x=tx; *y=ty; return finger;
}
void input_reset(void) { if(probe_reopen) { probe_reopen=false; request_reopen(); } }
void mk8_settings_top_forget(void) {}
int mk8_settings_values_count(unsigned group) { assert(group<SETTINGS_CATEGORIES); return counts[group]; }
bool mk8_settings_values_row(unsigned group,unsigned row,Mk8Setting *out) {
    if(publish_during_row && row==1) { publish_during_row=false; revision++; }
    if(group>=SETTINGS_CATEGORIES || row>=(unsigned)counts[group]) return false; *out=catalogue[group][row]; return true;
}
uint64_t mk8_settings_values_revision(void) { return revision; }
int mk8_settings_values_saved(void) { return save_state; }
bool mk8_settings_values_capturing(void) { return capturing; }
bool mk8_settings_values_request(int op,int id,int value) {
    if(op==SETTINGS_BEGIN) { ++begin_requests; save_state=0; return true; }
    if(op==SETTINGS_FINISH) {
        if(finish_queue_full && value!=SETTINGS_DETACHED) return false;
        ++finish_requests; finish_mode=value; save_state=1; return true;
    }
    if(op==SETTINGS_RESTART) { restart_choice=value; save_state=value ? 4 : 2; return true; }
    if(op==SETTINGS_ACTIVATE) { if(id==20)capturing=true; else if(id==19)resets++; else assert(0); return true; }
    for(unsigned g=0;g<SETTINGS_CATEGORIES;g++) for(int i=0;i<counts[g];i++) if(catalogue[g][i].id==id) {
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
        else if(!strcmp(name,"L_SettingsBG")) c->x[0]=HEAP_BASE+11700;
        else if(!strcmp(name,"L_SettingsBack")) c->x[0]=HEAP_BASE+12064;
        else if(!strcmp(name,"L_SettingsDialog")) c->x[0]=HEAP_BASE+14100;
        else if(!strcmp(name,"L_SettingsQuestion")) c->x[0]=HEAP_BASE+14200;
        else if(!strcmp(name,"L_SettingsScroll")) c->x[0]=HEAP_BASE+14300;
        else if(!strcmp(name,"L_SettingsHint")) c->x[0]=HEAP_BASE+14400;
        else if(!strcmp(name,"L_SettingsYes")) c->x[0]=HEAP_BASE+14500;
        else if(!strcmp(name,"L_SettingsCursor")) c->x[0]=HEAP_BASE+14800;
        else if(!strcmp(name,"L_SettingsNo")) c->x[0]=HEAP_BASE+14600;
        else if(sscanf(name,"L_SettingsRow%u",&row)==1) c->x[0]=HEAP_BASE+12400+row*128;
        else if(!strcmp(name,"L_Btn_LR")) c->x[0]=HEAP_BASE+13200+(c->x[0]-HEAP_BASE-4000-0x30)/512*128;
        else assert(0);
    } else if(address==0x70008ade74ull) {
        assert((c->x[0]>=HEAP_BASE+12000 && c->x[0]<HEAP_BASE+19100) ||
               (c->x[0]>=HEAP_BASE+26000 && c->x[0]<HEAP_BASE+27000));
        ST8(c->x[0]+0x58,(LD8(c->x[0]+0x58)&~1)|c->x[1]);
    } else if(address==0x70008ade34ull) {
        assert(c->x[0]==HEAP_BASE+11700 || c->x[0]==HEAP_BASE+12064 || c->x[0]==HEAP_BASE+12200 || c->x[0]==HEAP_BASE+15100 || c->x[0]==HEAP_BASE+3500 || c->x[0]==HEAP_BASE+3700 ||
               (c->x[0]>=HEAP_BASE+26000 && c->x[0]<HEAP_BASE+27000) || c->x[0]==HEAP_BASE+14680 ||
               (c->x[0]>=HEAP_BASE+1256 && c->x[0]<=HEAP_BASE+2280 && (c->x[0]-HEAP_BASE-1000)%256==0) ||
               (c->x[0]>=HEAP_BASE+2256 && c->x[0]<=HEAP_BASE+3280 && (c->x[0]-HEAP_BASE-2000)%256==0) ||
               (c->x[0]>=HEAP_BASE+16256 && c->x[0]<HEAP_BASE+19100) || (c->x[0]>=HEAP_BASE+14400 && c->x[0]<=HEAP_BASE+14600) ||
               (c->x[0]>=HEAP_BASE+12400 && c->x[0]<=HEAP_BASE+12400+SETTINGS_VISIBLE*128));
        memcpy((void *)(uintptr_t)(c->x[0]+0x30),(void *)(uintptr_t)c->x[1],12);
    } else if(address==0x70008ade58ull) {
        memcpy((void *)(uintptr_t)(c->x[0]+0x50),(void *)(uintptr_t)c->x[1],8);
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
    else if(address==0x70008adf84ull || address==0x70008ae098ull) {
        uint64_t offset=c->x[0]-(HEAP_BASE+11000+0x30);
        assert(!(offset%224) && offset/224<4);
        unsigned bit=1u<<(offset/224);
        if(address==0x70008adf84ull) c->x[0]=!!(cursor_mask&bit);
        else { cursor_writes++; cursor_mask=(cursor_mask&~bit)|(c->x[1] ? bit : 0); }
    }
    else if(address==0x70008ad3c4ull) {
        assert(c->x[0]==HEAP_BASE+15200 || c->x[0]==HEAP_BASE+15400 || c->x[0]==HEAP_BASE+15500);
        float frame; memcpy(&frame,&c->v[0],sizeof frame);
        assert(c->x[1]==2 ? frame>=1 && frame<=8 : frame==0 || frame==2);
        if(c->x[0]!=HEAP_BASE+15200) footer_frame[c->x[0]==HEAP_BASE+15500]=frame;
    }
    else if(address==0x70008ae0b8ull) {
        assert(c->x[0]==HEAP_BASE+14500 || c->x[0]==HEAP_BASE+14600);
        int index=c->x[0]==HEAP_BASE+14600;
        footer_drawn[index]=footer_frame[index];
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
        const float *p=(void *)(uintptr_t)c->x[1]; cursor_x=p[0]; cursor_y=p[1]; assert(p[2]==0);
    } else if(address==0x7000443404ull) {
        assert(c->x[0]==HEAP_BASE+16000);
        const float *p=(void *)(uintptr_t)c->x[1]; cursor_width=p[0]; cursor_height=p[1];
    } else if(address==0x7000442f5cull || address==0x7000443320ull) {
        assert(c->x[0]==HEAP_BASE+16000); if(address==0x7000443320ull)cursor_pulses++;
    } else if(address==0x70008ad300ull) {
        assert(c->x[0]==HEAP_BASE+15800 && c->x[1]==1);
    } else if(address==0x700008c608ull) {
        assert(c->x[1]==0x7000ed8620ull);
        if(c->x[0]==0x7000ee9367ull)open_sounds++;
        else if(c->x[0]==0x7000ed87a1ull)select_sounds++;
        else if(c->x[0]==0x7000ed87bfull)decide_sounds++;
        else if(c->x[0]==0x7000ed87cfull)cancel_sounds++;
        else assert(c->x[0]==0x7000ee9492ull || c->x[0]==0x7000ee94acull);
    }
    else if(address==0x700044ed20ull || address==0x700044ee50ull) {}
    else if(address==0x70008adc54ull) {
        text_calls++;
        if(c->x[0]==HEAP_BASE+3600 || c->x[0]==HEAP_BASE+3700) {
            const wchar_t *label=(void *)(uintptr_t)c->x[1];
            assert(wcslen(label)<sizeof shown_title/sizeof *shown_title);
            wcscpy(c->x[0]==HEAP_BASE+3600 ? shown_title : shown_cancel,label);
        }
        for(unsigned i=0;i<4;i++) if(c->x[0]==TEST_KEYCAP(i)+128 || c->x[0]==TEST_KEYCAP(i)+256) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1];
            char *out=c->x[0]==TEST_KEYCAP(i)+128 ? key_strings[i] : action_strings[i];
            unsigned j=0;
            for(;s[j];j++) { assert(j<40); out[j]=(char)s[j]; }
            out[j]=0;
        }
        if(c->x[0]==HEAP_BASE+3500 || c->x[0]==HEAP_BASE+14680) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1]; unsigned n=0;
            while(s[n]) n++;
            assert(n<=40); // Native title text buffer: 82 bytes including the UTF-16 terminator.
            unsigned line=c->x[0]!=HEAP_BASE+3500;
            memcpy(shown_tips[line],s,(n+1)*sizeof *s);
            if(!line) for(unsigned i=0;i<=n;i++) last_hint[i]=(char)s[i];
        }
        for(unsigned i=0;i<=SETTINGS_VISIBLE;i++) if(c->x[0]==HEAP_BASE+2000+i*256) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1]; unsigned n=0;
            if(i==2) memcpy(last_name,s,sizeof last_name);
            if(i==5) {
                size_t j=0;
                do { assert(j<80); last_footer_unit[j]=s[j]; } while(s[j++]);
            }
            for(;s[n] && n<79;n++) rendered[i][n]=(char)s[n]; rendered[i][n]=0;
        }
        for(unsigned i=1;i<=SETTINGS_VISIBLE;i++) if(c->x[0]==HEAP_BASE+1000+i*256) {
            const uint16_t *s=(void *)(uintptr_t)c->x[1]; unsigned n=0;
            for(;s[n] && n<79;n++) rendered_labels[i][n]=(char)s[n]; rendered_labels[i][n]=0;
        }
    } else assert(0);
    c->pc=address; c->nzcv=0x12345678; c->v[1]=0x1234;
}
#include "settings_page.c"
static float pane_float(uint64_t pane,unsigned offset) {
    float value; memcpy(&value,(void *)(uintptr_t)(pane+offset),sizeof value); return value;
}
static void check_keycap(unsigned i,const char *key,const char *action) {
    assert(!strcmp(key_strings[i],key) && !strcmp(action_strings[i],action));
    assert((LD8(panes->keycaps[i]+0x58)&1) && (LD8(panes->key_labels[i]+0x58)&1) && (LD8(panes->action_labels[i]+0x58)&1));
    assert(pane_float(panes->keycaps[i],0x30)==pane_float(panes->key_labels[i],0x30));
    assert(pane_float(panes->keycaps[i],0x34)+9==pane_float(panes->action_labels[i],0x34));
    assert(pane_float(panes->keycaps[i],0x30)+pane_float(panes->keycaps[i],0x50)/2 <
           pane_float(panes->action_labels[i],0x30)-pane_float(panes->action_labels[i],0x50)/2);
}
static void *handoff_reader(void *argument) {
    *(bool *)argument=mk8_settings_page_active();
    return NULL;
}
static void observe_request_handoff(void) {
    // Observe the frame's open-request handoff before page_tick activates the page.
    bool captured=false;
    pthread_t reader;
    if(pthread_create(&reader,NULL,handoff_reader,&captured) || pthread_join(reader,NULL)) {
        fputs("Settings handoff reader failed\n",stderr); exit(1);
    }
    if(!captured) {
        fputs("Settings request disappeared before page activation\n",stderr); exit(1);
    }
}
static void *reopen_writer(void *argument) {
    *(bool *)argument=mk8_settings_page_toggle();
    return NULL;
}
static void request_reopen(void) {
    bool accepted=false;
    pthread_t writer;
    if(pthread_create(&writer,NULL,reopen_writer,&accepted) || pthread_join(writer,NULL) || !accepted) {
        fputs("Settings reopen request failed\n",stderr); exit(1);
    }
}
static void *capture_reader(void *argument) {
    bool expected=*(bool *)argument;
    for(unsigned i=0;i<10000;i++) assert(mk8_settings_page_active()==expected);
    return NULL;
}
static void check_capture_state(bool expected) {
    // Cross-thread reads of stable lifecycle states; frame handoff ordering is tested separately.
    pthread_t reader; assert(!pthread_create(&reader,NULL,capture_reader,&expected));
    assert(!pthread_join(reader,NULL));
}
static void sample(uint64_t b) {
    int32_t st[4]={12000,-1000,600,700};
    uint64_t result=mk8_settings_page_input(b,st,b);
    if(active) assert(!result && !st[0] && !st[1] && !st[2] && !st[3]);
    Cpu saved=cpu; mk8_settings_page_tick(HEAP_BASE,false); assert(!memcmp(&saved,&cpu,sizeof cpu));
}
static void settle_slide(void) { if(slide_direction) { now+=160000000; sample(0); } }
static void press(uint64_t b) { settle_slide(); sample(0); sample(b); sample(0); settle_slide(); }
static void finish_play_pulse(void) { now+=160000000; sample(0); }
static void refresh_room_frame(void) { now+=1000000000; sample(0); settle_slide(); }
static void check_native_rooms(void) {
    rooms_connected=room_account_available=true;
    test_rooms=(Mk8Social){.lobby_count=7,.online_players=28,.listings_ok=true,.lobby_ok=true};
    for(int i=0;i<7;i++) {
        Mk8Lobby *room=&test_rooms.lobbies[i];
        snprintf(room->id,sizeof room->id,"room-%d",i);
        snprintf(room->mode,sizeof room->mode,"Test mode %d",i);
        strcpy(room->state,"Waiting"); room->count=i+1; room->capacity=12;
    }
    test_rooms.listings_ok=false; test_rooms.busy=true;
    press(1); finish_play_pulse(); assert(!strcmp(rendered_labels[1],"Refresh"));
    test_rooms.listings_ok=true; test_rooms.busy=false; refresh_room_frame();
    assert(menu_level==MENU_ROOMS && !focus && !offset && !strcmp(rendered_labels[1],"Test mode 0"));
    assert(!strcmp(rendered[1],"1 / 12") && strstr(rendered[0],"Players online: 28") && strstr(rendered[0],"Rooms: 7"));
    for(int i=0;i<6;i++) press(1ull<<15);
    assert(focus==6 && offset && !strcmp(rendered_labels[focus-offset+1],"Test mode 6"));
    press(1); finish_play_pulse();
    assert(menu_level==MENU_ROOM_DETAILS);
    assert(!strcmp(rendered[1],"Test mode 6") && !strcmp(rendered[2],"Waiting") &&
           !strcmp(rendered[3],"7 / 12") && !strcmp(rendered[4],"room-6"));
    Mk8Lobby saved_room=test_rooms.lobbies[6];
    test_rooms.lobbies[6]=test_rooms.lobbies[0]; test_rooms.lobbies[0]=saved_room;
    test_rooms.lobbies[0].count=11; refresh_room_frame();
    assert(!strcmp(rendered[1],"Test mode 6") && !strcmp(rendered[3],"11 / 12"));
    press(2); finish_play_pulse();
    assert(menu_level==MENU_ROOMS && focus==0 && !strcmp(rendered_labels[1],"Test mode 6"));
    // A pointer tap opens the row even when keyboard/controller focus is elsewhere.
    finger=true; tx=620; ty=320; sample(0); finger=false; sample(0); settle_slide(); finish_play_pulse();
    assert(menu_level==MENU_ROOM_DETAILS);
    test_rooms.listings_ok=false; strcpy(test_rooms.listings_error,"Room list unavailable."); refresh_room_frame();
    assert(strstr(rendered[0],"unavailable") && !strstr(rendered[0],"no longer"));
    test_rooms.listings_ok=true; refresh_room_frame();
    assert(!strcmp(rendered[1],"Test mode 1"));
    strcpy(test_rooms.lobbies[1].id,"replacement"); refresh_room_frame();
    assert(strstr(rendered[0],"no longer listed") && !strcmp(rendered[1],"room-1"));
    press(2); finish_play_pulse();
    assert(menu_level==MENU_ROOMS && !strcmp(rendered_labels[focus-offset+1],"Refresh"));
    test_rooms.lobby_count=0; refresh_room_frame();
    assert(strstr(rendered[0],"No rooms"));
    test_rooms.busy=true; refresh_room_frame(); assert(strstr(rendered[0],"Updating"));
    test_rooms.busy=false; test_rooms.in_lobby=true; strcpy(test_rooms.lobby_id,"private-current");
    strcpy(test_rooms.lobby_mode,"Battle"); test_rooms.players=2; test_rooms.capacity=6; refresh_room_frame();
    assert(!strcmp(rendered_labels[1],"Your current room") && !strcmp(rendered[1],"2 / 6"));
    finger=true; tx=620; ty=260; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_ROOM_DETAILS && !strcmp(rendered[1],"Battle") && !strcmp(rendered[4],"private-current"));
    test_rooms.lobby_count=1;
    test_rooms.lobbies[0]=(Mk8Lobby){.id="private-current",.mode="Public battle",.state="Racing",.count=5,.capacity=6};
    strcpy(test_rooms.lobby_id,"new-current"); refresh_room_frame();
    assert(!strstr(rendered[0],"Your current") && !strcmp(rendered[1],"Public battle") && !strcmp(rendered[3],"5 / 6"));
    press(2); assert(menu_level==MENU_ROOMS && focus==0 && !strcmp(rendered_labels[1],"Public battle"));
    press(1); assert(menu_level==MENU_ROOM_DETAILS);
    rooms_connected=false; refresh_room_frame();
    assert(menu_level==MENU_ROOMS && !strcmp(rendered_labels[1],"Nextendo Account"));
    press(1); assert(menu_level==MENU_OPTIONS && category==MENU_ACCOUNT);
    press(2); assert(menu_level==MENU_ROOMS && !strcmp(rendered_labels[focus-offset+1],"Nextendo Account"));
    rooms_connected=true; test_rooms.in_lobby=false; refresh_room_frame();
    finger=true; tx=620; ty=260; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_ROOM_DETAILS);
    press(2); assert(focus==0);
    // The visible row changes under the pointer on the very frame of a confirmation.
    strcpy(test_rooms.lobbies[0].id,"different-room");
    now+=1000000000; finger=true; sample(1); sample(1); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_ROOMS && !strcmp(rendered_labels[focus-offset+1],"Refresh"));
    // The public status response can omit the ID while still supplying all of the display fields.
    test_rooms.lobby_count=4;
    test_rooms.lobbies[0]=(Mk8Lobby){.mode="Worldwide race",.kind="mk8-world-race",.state="waiting",.count=2,.capacity=12};
    test_rooms.lobbies[1]=test_rooms.lobbies[0];
    test_rooms.lobbies[2]=(Mk8Lobby){.mode="Friends",.state="playing",.count=6,.capacity=8};
    test_rooms.lobbies[3]=test_rooms.lobbies[2];
    refresh_room_frame();
    finger=true; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_ROOM_DETAILS && room_detail_row_count()==3 && strstr(rendered[0],"at selection"));
    assert(!strcmp(rendered[1],"Worldwide race") && !strcmp(rendered[2],"waiting") && !strcmp(rendered[3],"2 / 12"));
    press(2); assert(menu_level==MENU_ROOMS && focus==0);
    for(int i=1;i<4;i++) {
        press(1ull<<15); press(1); assert(menu_level==MENU_ROOM_DETAILS && !strcmp(rendered[1],test_rooms.lobbies[i].mode));
        if(i<3) { press(2); assert(focus==(unsigned)i); }
    }
    test_rooms.lobbies[3]=test_rooms.lobbies[0]; refresh_room_frame();
    assert(!strcmp(rendered[1],"Friends") && !strcmp(rendered[3],"6 / 8"));
    press(2); assert(service_rows[focus].kind==SERVICE_REFRESH);
    strcpy(test_rooms.lobbies[0].id,"duplicated"); strcpy(test_rooms.lobbies[1].id,"duplicated");
    refresh_room_frame(); mk8_settings_page_wheel(-3); sample(0); press(1);
    assert(menu_level==MENU_ROOM_DETAILS && room_detail_row_count()==3);
    rooms_connected=false; refresh_room_frame(); assert(menu_level==MENU_ROOMS);
    rooms_connected=true; refresh_room_frame();
    mk8_settings_page_wheel(-10); sample(0); assert(service_header && !offset);
    mk8_settings_page_wheel(10); sample(0); assert(!service_header && !strcmp(rendered_labels[focus-offset+1],"Back to game"));
    int confirms=decide_sounds; finish_queue_full=true; press(1); finish_queue_full=false;
    assert(!exiting && menu_level==MENU_ROOMS && decide_sounds==confirms);
    test_rooms.lobby_count=128; test_rooms.in_lobby=true;
    for(int i=0;i<128;i++) snprintf(test_rooms.lobbies[i].id,sizeof test_rooms.lobbies[i].id,"room-%d",i);
    Mk8Lobby *last=&test_rooms.lobbies[127];
    memset(last->id,'x',sizeof last->id-1); last->id[sizeof last->id-1]=0;
    refresh_room_frame(); mk8_settings_page_wheel(-4); sample(0);
    assert(focus==127 && offset>0);
    press(1);
    assert(menu_level==MENU_ROOM_DETAILS && !strcmp(rendered_labels[4],"ID"));
    assert(strlen(rendered[4])+strlen(rendered[5])==strlen(last->id) &&
           !strncmp(rendered[4],last->id,strlen(rendered[4])) && !strcmp(rendered[5],last->id+strlen(rendered[4])));
    press(2); assert(menu_level==MENU_ROOMS && focus==127);
    sample(1ull<<13);
    for(int i=0;i<140;i++) { now+=100000000; sample(1ull<<13); }
    sample(0); assert(service_header && !offset && !strcmp(rendered_labels[1],test_rooms.lobbies[0].mode));
    press(1ull<<15); assert(!service_header && !focus && !offset);
    press(2); finish_play_pulse();
    assert(menu_level==MENU_HOME && focus==MENU_NEXTENDO);
    rooms_connected=room_account_available=false;
    memset(&test_rooms,0,sizeof test_rooms);
}
static void check_native_friends(void) {
    rooms_connected=room_account_available=true;
    test_rooms=(Mk8Social){.friends_ok=true,.count=8,.requests=2,.sent=3};
    for(int i=0;i<test_rooms.count;i++) {
        Mk8Friend *person=&test_rooms.friends[i];
        person->pid=500+i; snprintf(person->name,sizeof person->name,"Racer %d",i);
        person->presence=i%4-1;
    }
    press(1); assert(menu_level==MENU_ROOMS && !offset);
    press(1ull<<13); assert(service_header && !offset);
    press(1); assert(service_header && menu_level==MENU_FRIENDS && !panes->menu_cursor_active);
    press(1ull<<15);
    assert(!service_header && menu_level==MENU_FRIENDS && focus==0 && panes->menu_cursor_active);
    assert(LD8(panes->labels[1]+0x59)<255 && LD8(panes->values[1]+0x59)==LD8(panes->labels[1]+0x59));
    assert(strstr(rendered[0],"Friends: 8") && strstr(rendered[0],"2 in / 3 sent"));
    assert(!strcmp(rendered[1],"Unknown") && !strcmp(rendered[2],"Offline") &&
           !strcmp(rendered[3],"Online") && !strcmp(rendered[4],"Playing"));
    // A friend is informational. No confirmation sound, invite, or navigation.
    int confirms=decide_sounds;
    press(1); assert(decide_sounds==confirms && !exiting && menu_level==MENU_FRIENDS);
    mk8_settings_page_wheel(6); sample(0);
    assert(focus==6 && offset && !strcmp(rendered_labels[focus-offset+1],"Racer 6"));
    unsigned old_offset=offset;
    press(1ull<<12); assert(menu_level==MENU_ROOMS && focus==0 && !offset);
    finger=true; tx=270; ty=180; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_FRIENDS && focus==6 && offset==old_offset);
    Mk8Friend old=test_rooms.friends[6];
    test_rooms.friends[6]=test_rooms.friends[1]; test_rooms.friends[1]=old;
    strcpy(test_rooms.friends[1].name,"Renamed racer"); test_rooms.friends[1].presence=0;
    refresh_room_frame(); assert(focus==1 && !strcmp(rendered_labels[focus-offset+1],"Renamed racer") &&
                                 !strcmp(rendered[focus-offset+1],"Offline"));
    test_rooms.friends[2].pid=test_rooms.friends[1].pid;
    now+=1000000000; finger=true; tx=620; ty=260; sample(1); finger=false; sample(0); settle_slide();
    assert(focus==8 && !strcmp(rendered_labels[focus-offset+1],"Refresh"));
    mk8_settings_page_wheel(-6); sample(0); assert(focus==2);
    test_rooms.friends[1].pid=42;
    refresh_room_frame(); assert(focus==8); // Previously ambiguous PID cannot identify the survivor.
    test_rooms.busy=true; refresh_room_frame(); assert(strstr(nextendo_tip(MK8_MENU_EN),"Updating"));
    test_rooms.count=0; test_rooms.busy=false; refresh_room_frame();
    assert(strstr(rendered[0],"Friends: 0") && strstr(rendered[0],"2 in / 3 sent") && strstr(nextendo_tip(MK8_MENU_EN),"empty"));
    test_rooms.friends_ok=false; strcpy(test_rooms.friends_error,"Session expired. Sign in again."); refresh_room_frame();
    assert(strstr(rendered[0],"unavailable") && !strcmp(nextendo_tip(MK8_MENU_EN),test_rooms.friends_error));
    rooms_connected=false; now+=1000000000; sample(1); sample(0);
    assert(!exiting && strstr(rendered[0],"Local session") && !strstr(nextendo_tip(MK8_MENU_EN),"expired"));
    finger=true; tx=620; ty=260; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_OPTIONS && category==MENU_ACCOUNT);
    int before=back_pulses; press(2);
    assert(back_pulses==before+1 && menu_level==MENU_FRIENDS && focus==0 && panes->menu_cursor_active);
    rooms_connected=true; test_rooms=(Mk8Social){.friends_ok=true,.count=300,.sent=2147483647,.requests=2147483647};
    for(int i=0;i<300;i++) { test_rooms.friends[i].pid=1000+i; snprintf(test_rooms.friends[i].name,128,"Friend %d",i); }
    refresh_room_frame();
    assert(strstr(rendered[0],"Friends: 300") && strstr(nextendo_tip(MK8_MENU_EN),"2147483647"));
    mk8_settings_page_wheel(-1); sample(0); // Account â†’ Refresh â†’ last friend.
    press(1ull<<13); assert(focus==299 && !strcmp(rendered_labels[focus-offset+1],"Friend 299"));
    press(2); assert(menu_level==MENU_HOME && focus==HOME_NEXTENDO && cursor_y==145-HOME_NEXTENDO*70);
    rooms_connected=room_account_available=false; memset(&test_rooms,0,sizeof test_rooms);
}
static void check_play_routes(void) {
    int opens=open_sounds, confirms=decide_sounds, cancels=cancel_sounds;
    press(1); now+=80000000; sample(0);
    assert(reveal==1 && LD8(HEAP_BASE+12200+0x59)==255);
    finish_play_pulse();
    assert(menu_level==MENU_PLAY_ACTIONS && focus==0);
    assert(open_sounds==opens && decide_sounds==confirms+1);
    assert(!strcmp(rendered_labels[1],"Back to game") && !strcmp(rendered_labels[2],"Open Nextendo"));
    assert(strstr(rendered[0],"Local session") && strstr(rendered[0],"4.0.1"));
    assert(!strcmp(rendered[5],"Account: signed out of Nextendo") && strstr(rendered_labels[5],"base game"));
    finger=true; tx=640; ty=526; sample(0); finger=false; sample(0);
    assert(focus==0 && decide_sounds==confirms+1);
    test_session=(Mk8PlaySession){.connected=true,.signed_in=true,.modded=true,.username="Player"};
    now+=1000000000; sample(0);
    assert(strstr(rendered[0],"Nextendo active") && !strcmp(rendered[5],"Account: Player") && strstr(rendered_labels[5],"CTGP"));
    strcpy(test_session.username,"1234567890123456789\xf0\x9f\x8f\x81" "racer");
    now+=1000000000; sample(0);
    assert(last_footer_unit[27]=='9' && last_footer_unit[28]==0x2026 && !last_footer_unit[29]);
    strcpy(test_session.username,"Kart\nRacer");
    now+=1000000000; sample(0);
    assert(!strcmp(rendered[5],"Account: Kart Racer"));
    memset(&test_session,0,sizeof test_session);
    press(1ull<<13); assert(focus==1);
    press(1ull<<15); assert(focus==0);
    int pulses=cursor_pulses;
    cursor_done=false;
    finger=true; tx=640; ty=320; sample(0); finger=false; sample(0);
    assert(cursor_pulses==pulses+1 && decide_sounds==confirms+2);
    assert(slide_direction==1 && pulse_pane && pane_float(pulse_pane,0x48)>1);
    now+=36000000; sample(0);
    float x=pane_float(HEAP_BASE+12200,0x30);
    assert(x<0 && x+pane_float(HEAP_BASE+11700,0x30)==0 && x+pane_float(HEAP_BASE+12064,0x30)==-460);
    assert(!strcmp(rendered_labels[2],"Open Nextendo"));
    now+=50000000; sample(0);
    assert(pane_float(HEAP_BASE+12200,0x30)>0 && LD8(HEAP_BASE+12200+0x59)==255);
    cursor_done=true; finish_play_pulse();
    assert(!slide_direction && pane_float(HEAP_BASE+12200,0x30)==0);
    assert(menu_level==MENU_ROOMS);
    int room_backs=back_pulses;
    press(2); finish_play_pulse();
    assert(back_pulses==room_backs+1);
    assert(menu_level==MENU_PLAY_ACTIONS && focus==1);
    press(2); now+=80000000; sample(0);
    assert(reveal==1 && LD8(HEAP_BASE+12200+0x59)==255 && back_pulses==room_backs+2);
    finish_play_pulse(); assert(menu_level==MENU_HOME && focus==MENU_PLAY && cancel_sounds==cancels+2);

    rooms_available=true;
    press(1); finish_play_pulse(); press(1ull<<13); assert(focus==2);
    press(1); finish_play_pulse(); assert(rooms_requests==1 && !mii_requests && focus==2);
    press(2); finish_play_pulse();
    mii_available=true;
    press(1); finish_play_pulse(); press(1ull<<13); assert(focus==3);
    press(1ull<<13); assert(focus==2);
    press(1); finish_play_pulse(); assert(mii_requests==1 && rooms_requests==1 && focus==2);
    press(1ull<<13); press(1ull<<13); assert(focus==0);
    confirms=decide_sounds; cancels=cancel_sounds;
    press(1); finish_play_pulse();
    assert(exiting && decide_sounds==confirms+1 && cancel_sounds==cancels);
    mii_available=rooms_available=false;
}
static void open_page(void) { decided=true; mk8_settings_page_tick(HEAP_BASE,true); now+=160000000; sample(0); assert(active && !entering); }
static void apply_number(const char *s) { for(;*s;s++) assert(mk8_settings_page_key(*s)); mk8_settings_page_key(13); sample(0); }
static void popup_key(uint64_t manager,uint64_t key) {
    popup_buttons=key; mk8_settings_page_frame(manager);
    popup_buttons=0; mk8_settings_page_frame(manager);
    if(controller_press>=0) {
        mk8_settings_page_frame(manager);
        now+=160000000; mk8_settings_page_frame(manager);
    }
}
static void check_shared_race(uint64_t manager) {
    const uint64_t node=HEAP_BASE+29000, scene=HEAP_BASE+29120, context=HEAP_BASE+29760;
    const uint64_t config=HEAP_BASE+30400, rules=HEAP_BASE+30600;
    game_root=MAIN_BASE+0x133ebd0;
    root_node=node; ST64(node+8,scene);
    root_context=context; ST64(context+0x240,config); ST64(config+0x10,rules);
    // A lobby can advance while its settings page is open and the render queue is full.
    assert(mk8_settings_page_active());
    ST32(scene+0x184,4); ST32(rules+8,1); ST32(rules+0x188,2);
    int cancels=cancel_sounds;
    finish_queue_full=true; save_state=0; mk8_settings_page_frame(manager);
    assert(!mk8_settings_page_active() && save_state==1 && finish_mode==SETTINGS_DETACHED);
    assert(cancel_sounds==cancels);
    finish_queue_full=false;
    ST32(manager+0xb50,0);
    // The fallback overlay also needs the guest's latest scene and local-seat count.
    native_style=false;
    static const struct { unsigned scene_id, kind, normal_count, alternate_count; bool shared; } cases[]={
        {3,1,4,0,false}, {4,1,1,0,false}, {4,1,2,0,true}, {4,1,4,0,true},
        {4,2,1,0,false}, {4,3,2,0,true}, {4,4,4,1,false}, {4,4,1,2,true},
        {3,4,1,2,false}, {4,1,0,0,false}, {4,1,5,0,false}
    };
    for(size_t i=0;i<sizeof cases/sizeof cases[0];i++) {
        ST32(scene+0x184,cases[i].scene_id); ST32(rules+8,cases[i].kind);
        ST32(rules+0x180,12); // AI and remote racers can also occupy the grid.
        ST32(rules+0x188,cases[i].normal_count); ST32(rules+0x19c,cases[i].alternate_count);
        mk8_settings_page_frame(manager);
        assert(mk8_settings_shared_race()==cases[i].shared);
        assert(race_input_players==(cases[i].shared ? (cases[i].kind==4 ? cases[i].alternate_count : cases[i].normal_count) : 0));
    }
    ST32(scene+0x184,3); mk8_settings_page_frame(manager);
    native_style=true; available=true;
    assert(mk8_settings_page_toggle() && mk8_settings_page_active());
    ST32(scene+0x184,4); ST32(rules+0x188,2); native_style=false;
    mk8_settings_page_frame(manager);
    assert(mk8_settings_shared_race() && !mk8_settings_page_active());
    native_style=true;
    assert(!mk8_settings_page_toggle() && !mk8_settings_page_active());
    assert(cancel_sounds==cancels);
    native_style=false;
    ST64(config+0x10,GUEST_HI-8);
    mk8_settings_page_frame(manager); assert(!mk8_settings_shared_race());
    ST64(config+0x10,rules); mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    // Split play stays active until the shared standings actually take over, even after an individual finish.
    uint64_t pages=LD64(manager+0xb58), first_page=LD64(pages), hud=HEAP_BASE+28160, standings=HEAP_BASE+28576;
    ST64(manager+0xb58,pages); ST64(pages,hud); ST64(pages+8,standings);
    ST32(rules+8,3); ST32(manager+0xb50,1);
    ST32(hud+0x120,4); ST32(hud+0xe8,0x70);
    mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST32(manager+0xb50,2); ST64(standings,MAIN_BASE+0x1288c38);
    ST32(standings+0x120,3); ST32(standings+0xe8,0x70);
    mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST32(standings+0x120,4); ST32(standings+0xe8,0x6f);
    mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST32(standings+0xe8,0x70);
    for(unsigned substate=0;substate<=3;substate++) {
        ST32(standings+0x124,substate); mk8_settings_page_frame(manager); assert(!mk8_settings_shared_race());
    }
    // Next Round / View Highlight Reel / Quit replaces the standings within the same race scene.
    ST64(standings,MAIN_BASE+0x1288038); mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST32(standings+0xe8,0x72); ST32(standings+0x120,3);
    mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST32(standings+0x120,4); mk8_settings_page_frame(manager); assert(!mk8_settings_shared_race());
    ST32(manager+0xb50,1); mk8_settings_page_frame(manager); assert(mk8_settings_shared_race());
    ST64(pages,first_page); native_style=true;
    for(unsigned prompt=0;prompt<4;prompt++) {
        ST32(manager+0xb50,1); ST32(scene+0x184,3); mk8_settings_page_frame(manager);
        cursor_mask=9;
        assert(mk8_settings_page_toggle());
        mk8_settings_page_frame(manager); now+=160000000; sample(0);
        assert(active && !editing && !restart_prompt);
        if(prompt==3) {
            press(1); finish_play_pulse(); assert(menu_level==MENU_PLAY_ACTIONS);
            press(1ull<<15); press(1); assert(menu_level==MENU_ROOMS);
        } else if(prompt==2) {
            room_account_available=true;
            press(1ull<<15); press(1); press(1); assert(menu_level==MENU_OPTIONS && category==MENU_ACCOUNT);
            room_account_available=false;
        } else if(prompt) {
            press(2); save_state=3; now+=160000000; sample(0); assert(restart_prompt);
        } else {
            begin_number(catalogue[1][0]); assert(mk8_settings_page_key('8'));
        }
        int cancels_before=cancel_sounds, writes_before=cursor_writes;
        ST32(manager+0xb50,0); ST32(scene+0x184,4); mk8_settings_page_frame(manager);
        assert(!mk8_settings_page_active() && !mk8_settings_page_key('9') && !restart_prompt);
        assert(!restore_nav && !top_root && cancel_sounds==cancels_before && cursor_writes==writes_before);
        ST32(manager+0xb50,1); mk8_settings_page_frame(manager);
        assert(!mk8_settings_page_active() && cursor_mask==9);
        assert(!(LD8(HEAP_BASE+12200+0x58)&1) && !(LD8(HEAP_BASE+15100+0x58)&1));
    }
    settings_page_reset(); assert(!mk8_settings_shared_race());
    game_root=0; native_style=true;
    mk8_settings_page_frame(manager); assert(!mk8_settings_shared_race());
}
static void popup_open(uint64_t manager) {
    int32_t st[4]={0};
    mk8_settings_page_input(0,st,0);
    popup_buttons=1u<<10;
    assert(!mk8_settings_page_input(popup_buttons,st,0));
    mk8_settings_page_frame(manager);
    assert(controller_visible);
    popup_buttons=0;
    mk8_settings_page_input(0,st,0);
}
static void register_page(uint64_t manager,uint64_t page,uint64_t vtable,unsigned id) {
    const uint64_t registry=HEAP_BASE+31520;
    ST64(manager+0x30,registry); ST64(registry+8*id,page);
    ST64(page,MAIN_BASE+vtable); ST64(page+0x20,manager); ST32(page+0xe8,id);
}
static void check_controller_footer(uint64_t manager) {
    int opens=open_sounds, moves=select_sounds, confirms=decide_sounds, cancels=cancel_sounds;
    selector_absent=false; top=owner=panes->root=HEAP_BASE;
    register_page(manager,owner,0x1271df0,13);
    ST64(owner+0x1d0,HEAP_BASE+11000);
    panes->dialog_cursor=HEAP_BASE+16000;
    panes->cursor_animation=HEAP_BASE+15800;
    panes->dialog_animations[0]=HEAP_BASE+15400; panes->dialog_animations[1]=HEAP_BASE+15500;
    panes->dialog_buttons[0]=HEAP_BASE+14500; panes->dialog_buttons[1]=HEAP_BASE+14600;
    panes->title_label=HEAP_BASE+3600;
    panes->dialog_labels[1]=HEAP_BASE+3700; panes->dialog_labels[2]=HEAP_BASE+3800;
    const uint64_t list=LD64(manager+0xb58), layout=HEAP_BASE+20200, drivers=HEAP_BASE+21200;
    ST32(manager+0xb50,3); ST64(list+8,layout); ST64(list+16,drivers);
    ST64(layout,0x7001279040ull); ST32(layout+0xe8,0x14); ST32(layout+0x120,4);
    ST64(drivers,0x7001278940ull); ST32(drivers+0xe8,0x2e); ST32(drivers+0x120,4);
    mk8_controller_applet_result(4,4,4);
    int32_t st[4]={0}; const uint64_t plus=1u<<10;
    // Held Plus crossing from solo into multiplayer cannot open until released and pressed again.
    mk8_settings_page_frame(manager); assert(mk8_settings_page_input(plus,st,0)==plus);
    ST32(layout+0xe8,0x16); mk8_settings_page_frame(manager);
    assert(!controller_visible && mk8_settings_page_input(plus,st,0)==plus);
    mk8_settings_page_input(0,st,0);
    ST32(manager+0xb50,2); mk8_settings_page_frame(manager);
    assert(mk8_settings_page_input(plus,st,0)==plus && !mk8_controller_layout_active());
    ST32(manager+0xb50,3); mk8_settings_page_frame(manager);
    active=true; mk8_settings_page_input(0,st,0);
    assert(!mk8_settings_page_input(plus,st,0) && !mk8_controller_layout_active());
    active=false; assert(mk8_settings_page_input(plus,st,0)==plus && !mk8_controller_layout_active());
    mk8_settings_page_input(0,st,0);
    // The initial controller applet and a transitioning player selection page cannot open the popup.
    ST32(drivers+0x120,3); mk8_settings_page_frame(manager);
    assert(mk8_settings_page_input(plus,st,0)==plus);
    assert(!mk8_controller_pointer(640,643,1) && !mk8_controller_pointer(640,643,2));
    ST32(drivers+0x120,4); mk8_settings_page_frame(manager);
    cursor_mask=13;
    assert(!mk8_controller_pointer(640,670,1) && !mk8_controller_pointer(640,643,2));
    assert(mk8_controller_pointer(640,643,1) && mk8_controller_pointer(640,670,0) && mk8_controller_pointer(640,643,2));
    assert(!mk8_controller_layout_active());
    assert(mk8_controller_pointer(640,643,1) && mk8_controller_pointer(640,643,2));
    mk8_settings_page_frame(manager);
    assert(controller_visible);
    mk8_settings_page_input(0,st,0);
    assert(!memcmp(shown_tips[0],u"\ue000 Assign  \ue002 Swap  \ue003 Clear  \ue046 Clear all",sizeof u"\ue000 Assign  \ue002 Swap  \ue003 Clear  \ue046 Clear all"));
    popup_tips=(struct Mk8ControlTips){MK8_TIP_KEYBOARD,{"Return","J","Keyboard X","Delete","Backspace"}};
    // A previously prepared title control has only the original two text panes.
    const uint64_t prepared_keycap=panes->keycaps[0];
    panes->keycaps[0]=0;
    mk8_settings_page_frame(manager);
    assert(!memcmp(shown_tips[0],u"Return Assign  Keyboard X Swap",sizeof u"Return Assign  Keyboard X Swap"));
    assert(!memcmp(shown_tips[1],u"Delete Clear row  Backspace Clear all",sizeof u"Delete Clear row  Backspace Clear all"));
    panes->keycaps[0]=prepared_keycap;
    popup_tips.label[MK8_TIP_UP][0]='W'; mk8_settings_page_frame(manager);
    check_keycap(0,"Return","Assign"); check_keycap(1,"Keyboard X","Swap");
    check_keycap(2,"Delete","Clear row"); check_keycap(3,"Backspace","Clear all");
    strcpy(popup_tips.label[MK8_TIP_CONFIRM],"MediaTrackPrevious");
    strcpy(popup_tips.label[MK8_TIP_SWAP],"Keypad Hexadecimal");
    mk8_settings_page_frame(manager);
    check_keycap(0,"MediaTrackPrevious","Assign"); check_keycap(1,"Keypad Hexadecimal","Swap");
    for(unsigned i=0;i<2;i++) {
        assert(pane_float(panes->keycaps[i],0x30)==pane_float(panes->keycaps[i+2],0x30));
        assert(pane_float(panes->action_labels[i],0x30)-pane_float(panes->action_labels[i],0x50)/2==
               pane_float(panes->action_labels[i+2],0x30)-pane_float(panes->action_labels[i+2],0x50)/2);
    }
    assert(pane_float(panes->keycaps[0],0x34)==pane_float(panes->keycaps[1],0x34));
    assert(pane_float(panes->keycaps[2],0x34)==pane_float(panes->keycaps[3],0x34));
    assert(pane_float(panes->keycaps[0],0x34)>pane_float(panes->keycaps[2],0x34));
    assert((LD8(panes->hint_double+0x58)&1) && !(LD8(panes->hint_single+0x58)&1) && pane_float(panes->hint_plate,0x50)>880);
    assert(pane_float(panes->hint_double,0x54)==112);
    assert(pane_float(panes->hint_double,0x50)==pane_float(panes->hint_plate,0x50));
    assert(pane_float(panes->hint_double,0x34)==pane_float(panes->hint_plate,0x34));
    assert(pane_float(panes->keycaps[0],0x34)==-10 && pane_float(panes->keycaps[2],0x34)==-66);
    strcpy(popup_tips.label[MK8_TIP_CLEAR_ALL],"(none)");
    mk8_settings_page_frame(manager);
    check_keycap(0,"MediaTrackPrevious","Assign"); check_keycap(1,"Keypad Hexadecimal","Swap"); check_keycap(2,"Delete","Clear row");
    assert(!(LD8(panes->keycaps[3]+0x58)&1) && LD8(panes->hint_second+0x58)&1);
    assert(!memcmp(shown_tips[1],u"Click a player for assignment options.",sizeof u"Click a player for assignment options."));
    assert(pane_float(panes->hint_plate,0x50)*0.55 < 1280);
    const uint64_t prepared_double=panes->hint_double;
    panes->hint_double=0;
    popup_tips.label[MK8_TIP_UP][0]='S'; mk8_settings_page_frame(manager);
    assert(pane_float(panes->hint_plate,0x54)==96);
    panes->hint_double=prepared_double;
    popup_tips=(struct Mk8ControlTips){MK8_TIP_PLAYSTATION,{"Cross","Circle","Square","Triangle","Create"}};
    mk8_settings_page_frame(manager);
    assert(shown_tips[0][0]==0x00d7);
    assert(!(LD8(panes->keycaps[0]+0x58)&1));
    popup_tips=(struct Mk8ControlTips){MK8_TIP_NINTENDO,{"A","B","X","Y",""}};
    mk8_settings_page_frame(manager);
    assert(!memcmp(shown_tips[1],u"Click a player for assignment options.",sizeof u"Click a player for assignment options."));
    assert(last_edit==MK8_EDIT_BEGIN && edit_row==4);
    assert(!cursor_mask);
    assert(mk8_settings_page_toggle() && !requested && !active);
    assert(!memcmp(last_name,popup_names[1],sizeof last_name));
    int before_names=text_calls;
    mk8_settings_page_frame(manager); assert(text_calls==before_names);
    popup_names[1][0]=0x5de6;
    mk8_settings_page_frame(manager);
    assert(text_calls==before_names+1 && !memcmp(last_name,popup_names[1],sizeof last_name));
    assert(controller_visible && cursor_x==0 && cursor_y==145 && cursor_width==1700 && cursor_height==158 && *(const float *)(memory+14800+0x48)==.5f);
    const float *cancel=(const float *)(memory+14500+0x30), *apply=(const float *)(memory+14600+0x30);
    popup_key(manager,1ull<<13); // Already at the first row.
    assert(open_sounds==opens+1 && select_sounds==moves);
    assert(cancel[0]==-200 && cancel[1]==-246 && apply[0]==200 && apply[1]==-246);
    for(int i=0;i<4;i++) popup_key(manager,1ull<<15);
    assert(controller_visible && cursor_x==-200 && cursor_y==-246 && cursor_width==370 && cursor_height==158 && *(const float *)(memory+14800+0x48)==1.f);
    popup_key(manager,1ull<<14);
    assert(controller_visible && cursor_x==200 && cursor_y==-246);
    popup_key(manager,1);
    assert(!controller_visible && !controller_preview && controller_focus==5 && cursor_mask==13);
    assert(select_sounds==moves+5 && decide_sounds==confirms+1 && cancel_sounds==cancels);
    popup_open(manager);
    popup_key(manager,1ull<<12);
    assert(cursor_x==-200 && cursor_y==-246);
    assert(mk8_controller_pointer(440,606,1));
    popup_key(manager,1);
    assert(!controller_visible && !controller_preview && controller_focus==4);
    assert(open_sounds==opens+2 && cancel_sounds==cancels+1);
    assert(mk8_controller_pointer(440,606,2));
    assert(!mk8_controller_pointer(440,606,2));

    popup_open(manager);
    // The original game can own a press that began before the popup was opened.
    assert(!mk8_controller_pointer(840,606,2));
    assert(mk8_controller_pointer(440,606,0)); mk8_settings_page_frame(manager);
    assert(controller_visible && cursor_x==-200 && cursor_y==-246);
    assert(mk8_controller_pointer(840,606,0)); mk8_settings_page_frame(manager);
    assert(controller_visible && cursor_x==200 && cursor_y==-246);
    assert(mk8_controller_pointer(640,606,0)); mk8_settings_page_frame(manager);
    assert(cursor_x==0 && cursor_y==145 && cursor_width==1700 && cursor_height==158 && *(const float *)(memory+14800+0x48)==.5f);
    assert(mk8_controller_pointer(440,606,1));
    assert(mk8_controller_pointer(640,606,2)); mk8_settings_page_frame(manager);
    assert(controller_visible); // Releasing between buttons cancels the click.
    assert(mk8_controller_pointer(840,606,1));
    mk8_controller_pointer(-1,-1,3); mk8_controller_pointer(-1,-1,4);
    mk8_controller_pointer(840,606,2); mk8_settings_page_frame(manager);
    assert(controller_visible); // Losing focus also cancels a held click.
    assert(mk8_controller_pointer(840,606,1));
    assert(mk8_controller_pointer(840,606,2)); mk8_settings_page_frame(manager);
    assert(controller_visible && last_edit==MK8_EDIT_APPLY);
    mk8_settings_page_frame(manager); // Wait for the host's acknowledgement.
    assert(controller_visible && controller_press==1);
    mk8_settings_page_frame(manager);
    now+=80000000; mk8_settings_page_frame(manager);
    assert(controller_visible && reveal>.45f && reveal<.55f && !cursor_mask);
    now+=80000000; mk8_settings_page_frame(manager);
    assert(!controller_visible && !controller_preview && controller_focus==5);
    assert(extra_holds==3);
    popup_open(manager);
    assert(mk8_controller_pointer(440,606,1));
    unsigned old_writes=cursor_writes;
    ST32(manager+0xb50,0); mk8_settings_page_frame(manager);
    assert(cursor_writes==old_writes);
    cursor_mask=6; // The next scene owns a different combination of visible cursors.
    assert(!mk8_controller_layout_active());
    assert(mk8_controller_pointer(440,606,2));  // The popup owned this press.
    ST32(manager+0xb50,3);
    for (int count=2;count<=3;count++) {
        mk8_controller_applet_result(count,count,count); mk8_settings_page_frame(manager);
        popup_open(manager);
        assert(controller_rows==(unsigned)count && !(LD8(HEAP_BASE+12400+(count+1)*128+0x58)&1));
        for(int j=0;j<count;j++) popup_key(manager,1ull<<15);
        assert(controller_focus==4);
        popup_key(manager,1ull<<13); assert(controller_focus==count-1);
        popup_key(manager,2); assert(!mk8_controller_layout_active() && cursor_mask==6);
    }
    mk8_controller_applet_result(4,4,4); mk8_settings_page_frame(manager);
    popup_open(manager);
    popup_key(manager,1ull<<15); popup_key(manager,8);
    assert(last_edit==MK8_EDIT_CLEAR && edit_row==1 && controller_visible);
    popup_key(manager,4); popup_key(manager,1ull<<15); popup_key(manager,1);
    assert(last_edit==MK8_EDIT_SWAP && edit_row==1 && edit_other==2);
    popup_key(manager,1);
    assert(last_edit==MK8_EDIT_ASSIGN && edit_row==2);
    editor_state=MK8_EDIT_AMBIGUOUS; mk8_settings_page_frame(manager);
    assert(!wcscmp((const wchar_t *)shown_tips[0],L"Release all. One player holds \ue0a4 + \ue0a5."));
    editor_state=MK8_EDIT_NEED_LEFT; mk8_settings_page_frame(manager);
    assert(!wcscmp((const wchar_t *)shown_tips[0],L"Hold \ue0a5. Press \ue0a4 on the left Joy-Con."));
    editor_state=MK8_EDIT_NEED_RIGHT; mk8_settings_page_frame(manager);
    assert(!wcscmp((const wchar_t *)shown_tips[0],L"Hold \ue0a4. Press \ue0a5 on the right Joy-Con."));
    editor_state=MK8_EDIT_PAIR_HELD; mk8_settings_page_frame(manager);
    assert(!strcmp(last_hint,"Release both shoulder buttons to join."));
    popup_key(manager,1ull<<14); popup_key(manager,1);
    assert(last_edit==MK8_EDIT_KEYBOARD && edit_row==2);
    popup_key(manager,1ull<<11);
    assert(last_edit==MK8_EDIT_CLEAR_ALL);
    popup_key(manager,2);
    assert(last_edit==MK8_EDIT_STOP_LISTEN && controller_visible);
    // Mouse-only assignment, including when the player controlling the menu has disconnected.
    assert(mk8_controller_pointer(640,285,1) && mk8_controller_pointer(640,285,2));
    mk8_settings_page_frame(manager); assert(controller_panel==ROW_ACTIONS && action_player==1);
    for(unsigned i=1;i<=4;i++) assert(pane_float(panes->labels[i],0x30)==0);
    assert(mk8_controller_pointer(640,285,1) && mk8_controller_pointer(640,285,2));
    mk8_settings_page_frame(manager); assert(last_edit==MK8_EDIT_KEYBOARD && edit_row==1);
    for(unsigned i=1;i<=4;i++) assert(pane_float(panes->labels[i],0x30)==-245);
    wait_apply=true;
    popup_key(manager,1ull<<14); popup_key(manager,1);
    assert(last_edit==MK8_EDIT_APPLY && controller_visible && !cursor_mask);
    mk8_settings_page_frame(manager); assert(controller_visible);
    editor_state=MK8_EDIT_FAILED; wait_apply=false;
    mk8_settings_page_frame(manager); assert(controller_visible);
    assert(mk8_controller_pointer(440,606,0)); // Hover Cancel in the old editor.
    popup_key(manager,2); assert(!controller_visible && last_edit==MK8_EDIT_CANCEL && cursor_mask==6);
    mk8_controller_applet_result(3,3,2); mk8_settings_page_frame(manager);
    assert(!controller_entry_players && mk8_settings_page_input(plus,st,0)==plus);

    // With just the standalone controller layout, P1 can reopen a completed two-player assignment.
    native_style=false;
    mk8_controller_applet_result(2,2,2); mk8_settings_page_frame(manager);
    assert(controller_entry_players==2);
    popup_open(manager);
    assert(controller_visible && controller_rows==2);
    popup_key(manager,2); assert(!controller_visible);

    // WiFi's two-driver character layout shares neither the local page IDs nor the four-driver vtable.
    ST32(layout+0xe8,0x15); ST64(drivers,MAIN_BASE+0x1278648); ST32(drivers+0xe8,0x2d);
    mk8_controller_applet_result(1,1,1); // A previous applet cannot supply the count for this page.
    ST32(drivers+0xe8,0x2c); mk8_settings_page_frame(manager); assert(!controller_entry_players);
    ST32(drivers+0xe8,0x2d); mk8_settings_page_frame(manager);
    popup_open(manager); assert(controller_visible && controller_rows==2);
    popup_key(manager,2); assert(!controller_visible);
    assert(!mk8_controller_pointer(640,643,1)); // The LAN layout hides its on-screen shortcut.
    ST32(layout+0xe8,0x16); ST64(drivers,MAIN_BASE+0x1278940); ST32(drivers+0xe8,0x2e);
    mk8_controller_applet_result(2,2,2); mk8_settings_page_frame(manager);
    assert(mk8_controller_pointer(640,643,1));
    ST32(drivers+0xe8,0x2d); mk8_settings_page_frame(manager); assert(!controller_entry_players);
    // A press on the local shortcut must not open the editor if released on the LAN layout.
    ST32(layout+0xe8,0x15); ST64(drivers,MAIN_BASE+0x1278648);
    mk8_settings_page_frame(manager); assert(controller_entry_players==2);
    assert(mk8_controller_pointer(640,643,2) && !mk8_controller_layout_active());
    // The mandatory join opens before character select, even with the optional F1 menu disabled.
    ST32(manager+0xb50,1);
    native_style=false;
    opens=open_sounds;
    join_players=4; join_keyboard=true; editor_state=MK8_EDIT_LISTEN; edit_row=1;
    mk8_settings_page_frame(manager);
    assert(controller_visible && controller_rows==4 && controller_focus==1);
    assert(!wcscmp(shown_title,L"Player 2: \ue0a4 + \ue0a5 to join"));
    assert(!wcscmp(shown_cancel,L"\ue001 Cancel"));
    check_keycap(0,"SL","+"); check_keycap(1,"SR","Join sideways");
    check_keycap(2,"Enter","Join keyboard");
    assert((LD8(HEAP_BASE+12200+0x58)&1) && open_sounds==opens+1);
    assert(!strcmp(rendered[1],"Pro") && (LD8(HEAP_BASE+12400+4*128+0x58)&1));
    assert(!(LD8(HEAP_BASE+14600+0x58)&1)); // Enter joins the keyboard; there is only one footer.
    assert(pane_float(HEAP_BASE+14500,0x30)==0);
    assert(mk8_controller_pointer(640,606,0)); mk8_settings_page_frame(manager);
    assert(controller_focus==1); // P1 is a pad. Hovering cannot highlight Cancel.
    for(int visit=0;visit<2;visit++) {
        int old_pulses=button_pulses, old_cancels=cancel_sounds;
        join_pad_back=true; editor_state=MK8_EDIT_PENDING; button_done=false;
        mk8_settings_page_frame(manager);
        assert(controller_focus==4 && footer_drawn[0]==2 && button_pulses==old_pulses+1);
        editor_state=MK8_EDIT_CLOSED; join_players=0;
        now+=80000000; mk8_settings_page_frame(manager);
        assert(controller_visible && reveal>0 && reveal<1);
        now+=80000000; mk8_settings_page_frame(manager);
        assert(!controller_visible && cancel_sounds==old_cancels);
        join_pad_back=false; editor_state=MK8_EDIT_LISTEN; join_players=4; button_done=true;
        mk8_settings_page_frame(manager);
        assert(controller_visible && controller_focus==1 && reveal==1 && footer_drawn[0]==0);
    }
    opens+=2;
    for(unsigned i=0;i<4;i++) panes->join_buttons[i]=HEAP_BASE+18304+128*i;
    for(unsigned i=0;i<2;i++) panes->join_plus[i]=HEAP_BASE+18816+128*i;
    edit_row=3; join_keyboard=false;
    mk8_settings_page_frame(manager);
    assert(controller_focus==3 && open_sounds==opens+1);
    assert(!(LD8(HEAP_BASE+14600+0x58)&1));
    assert(!wcscmp(shown_title,L"Player 4: choose how to join"));
    assert(!shown_tips[0][0]);
    assert(!strcmp(action_strings[0],"Controller or paired Joy-Cons") && !strcmp(action_strings[1],"Sideways Joy-Con"));
    assert(pane_float(panes->action_labels[0],0x34)>pane_float(panes->action_labels[1],0x34));
    assert(pane_float(panes->join_plus[0],0x30)==pane_float(panes->join_plus[1],0x30));
    for(unsigned i=0;i<2;i++) {
        assert(!(LD8(panes->keycaps[i]+0x58)&1) && !(LD8(panes->key_labels[i]+0x58)&1));
        assert((LD8(panes->join_buttons[2*i]+0x58)&1) && (LD8(panes->join_buttons[2*i+1]+0x58)&1) && (LD8(panes->join_plus[i]+0x58)&1));
        assert(pane_float(panes->join_buttons[2*i],0x30)+(i==1 ? 48 : 40)==pane_float(panes->join_plus[i],0x30));
        assert(pane_float(panes->join_buttons[2*i+1],0x30)-40==pane_float(panes->join_plus[i],0x30));
        assert(pane_float(panes->join_buttons[2*i],0x34)+20==pane_float(panes->join_plus[i],0x34)-9);
        assert(pane_float(panes->join_plus[i],0x34)==pane_float(panes->action_labels[i],0x34)+3.5f);
    }
    assert(!(LD8(panes->keycaps[2]+0x58)&1));
    popup_tips=(struct Mk8ControlTips){MK8_TIP_KEYBOARD,{"Return","J"}};
    mk8_settings_page_frame(manager);
    check_keycap(2,"J","Cancel");
    assert(mk8_controller_pointer(640,606,0)); mk8_settings_page_frame(manager);
    assert(controller_focus==4 && cursor_x==0 && cursor_y==-246);
    assert(mk8_controller_pointer(440,606,0)); mk8_settings_page_frame(manager);
    assert(controller_focus==3); // Previous left button position.
    assert(mk8_controller_pointer(640,606,1));
    join_keyboard=true;
    mk8_settings_page_frame(manager);
    check_keycap(2,"Enter","Join keyboard");
    popup_tips=(struct Mk8ControlTips){MK8_TIP_NINTENDO,{"A","B","X","Y","Minus"}};
    editor_state=MK8_EDIT_READY; edit_row=-1;
    mk8_settings_page_frame(manager);
    assert(controller_focus==4 && !(LD8(HEAP_BASE+14600+0x58)&1) && cursor_x==0 && cursor_y==-246);
    assert(!wcscmp(shown_cancel,L"\ue000 OK"));
    assert(mk8_controller_pointer(640,606,2)); mk8_settings_page_frame(manager);
    assert(editor_state==MK8_EDIT_READY); // Releasing the earlier Cancel press cannot click OK.
    for(unsigned i=0;i<4;i++) assert(!(LD8(panes->join_buttons[i]+0x58)&1));
    for(unsigned i=0;i<2;i++) assert(!(LD8(panes->join_plus[i]+0x58)&1));
    assert(!wcscmp((const wchar_t *)shown_tips[1],L"\ue000 OK"));
    panes->symbol_help=panes->symbol_dialog=HEAP_BASE+14400;
    panes->symbol_parts[0][0]=HEAP_BASE+26000; panes->symbol_parts[1][1]=HEAP_BASE+26128;
    panes->symbol_parts[2][1]=HEAP_BASE+26256; panes->symbol_parts[4][1]=HEAP_BASE+26384;
    panes->symbol_parts[4][0]=HEAP_BASE+26896;
    panes->symbol_parts[3][4]=HEAP_BASE+26512; panes->symbol_parts[3][5]=HEAP_BASE+26640;
    panes->symbol_parts[0][1]=HEAP_BASE+26768;
    editor_state=MK8_EDIT_LISTEN; edit_row=2; join_keyboard=false;
    mk8_settings_page_frame(manager);
    assert(panes->shown_symbols[2]==panes->symbol_parts[2][1] && panes->shown_symbols[4]==panes->symbol_parts[4][1]);
    assert(!wcscmp(shown_cancel,L"Cancel"));
    popup_tips=(struct Mk8ControlTips){MK8_TIP_PLAYSTATION,{"Cross","Circle"}};
    mk8_settings_page_frame(manager);
    assert(!panes->shown_symbols[2] && !panes->shown_symbols[4] && !(LD8(panes->keycaps[2]+0x58)&1));
    assert(!strcmp(action_strings[2],"Circle Cancel") && !wcscmp(shown_cancel,L"Circle Cancel"));
    join_keyboard=true;
    mk8_settings_page_frame(manager);
    check_keycap(2,"Enter","Join keyboard");
    join_keyboard=false;
    popup_tips=(struct Mk8ControlTips){MK8_TIP_NINTENDO,{"A","B","X","Y","Minus"}};
    for(unsigned i=0;i<2;i++) {
        editor_state=i ? MK8_EDIT_NEED_RIGHT : MK8_EDIT_NEED_LEFT;
        mk8_settings_page_frame(manager);
        assert(!wcscmp(shown_title,L"Player 3: pair Joy-Cons"));
        assert(!strcmp(action_strings[3],"Press") && !strcmp(last_hint,"on other Joy-Con to pair."));
        assert(panes->shown_symbols[3]==panes->symbol_parts[3][4+i]);
        assert(pane_float(panes->action_labels[3],0x30)<pane_float(panes->shown_symbols[3],0x30));
        assert(pane_float(panes->shown_symbols[3],0x30)<pane_float(panes->hint,0x30));
    }
    editor_state=MK8_EDIT_READY; edit_row=-1;
    mk8_settings_page_frame(manager);
    assert(panes->shown_symbols[0]==panes->symbol_parts[0][0] && panes->shown_symbols[4]==panes->symbol_parts[4][0]);
    assert(!panes->shown_symbols[1] && !panes->shown_symbols[2] && !panes->shown_symbols[3]);
    assert(!shown_tips[0][0] || !strcmp(last_hint,"All players are ready."));
    panes->symbol_help=panes->symbol_dialog=0;
    popup_tips=(struct Mk8ControlTips){MK8_TIP_KEYBOARD,{"Return","J"}};
    mk8_settings_page_frame(manager);
    check_keycap(0,"Return","OK"); assert(!(LD8(panes->keycaps[1]+0x58)&1));
    // The guest applet can hold the stock animation clock while waiting for the popup to close.
    int ok_sounds=decide_sounds;
    assert(mk8_controller_pointer(640,606,1) && mk8_controller_pointer(640,606,2));
    mk8_settings_page_frame(manager);
    assert(last_edit==MK8_EDIT_APPLY);
    button_done=false; join_players=0;
    mk8_settings_page_frame(manager);
    assert(controller_visible && decide_sounds==ok_sounds+1 && footer_drawn[0]==1);
    now+=80000000; mk8_settings_page_frame(manager);
    assert(reveal>.45f && reveal<.55f && footer_drawn[0]>5 && footer_drawn[0]<7);
    now+=80000000; mk8_settings_page_frame(manager);
    assert(!controller_visible && decide_sounds==ok_sounds+1);
    button_done=true; join_players=4; edit_row=3; editor_state=MK8_EDIT_LISTEN;
    mk8_settings_page_frame(manager);
    // A menu transition can temporarily remove the shared owner while the SDL applet still runs.
    ST32(manager+0xb50,0);
    mk8_settings_page_frame(manager);
    assert(!mk8_controller_join_visible() && !mk8_controller_layout_active());
    editor_state=MK8_EDIT_CLOSED; join_players=0;
    ST32(manager+0xb50,1);
    mk8_settings_page_frame(manager);
    assert(!controller_visible && !(LD8(HEAP_BASE+12200+0x58)&1) && cursor_mask==6);
    native_style=true;
}
int main(void) {
    uint64_t title_manager=HEAP_BASE+20000,title_page=HEAP_BASE+25000;
    ST64(title_manager+0xc68,title_page);
    ST64(title_page,MAIN_BASE+0x127cb38);
    native_style=false;
    // The stock title can buffer an advance in its entrance animation. Returning
    // from the main menu can add subphase 3 before the ordinary interactive phase.
    ST32(title_page+0x120,4); ST32(title_page+0x124,0);
    mk8_settings_page_frame(title_manager); assert(title_active);
    auto_lan=true; mk8_settings_page_frame(title_manager); assert(!title_active);
    auto_lan=false; ST32(title_page+0x124,1);
    mk8_settings_page_frame(title_manager); assert(!title_active);
    ST32(title_page+0x124,0); ST32(title_page+0x120,3);
    mk8_settings_page_frame(title_manager); assert(!title_active);
    ST32(title_page+0x120,4); ST64(title_page,MAIN_BASE+0x127c840);
    mk8_settings_page_frame(title_manager); assert(!title_active);
    assert(!in_main);
    mk8_settings_page_build(title_page);
    ST32(title_manager+0xb50,1); ST64(title_manager+0xb58,HEAP_BASE+24000);
    ST64(HEAP_BASE+24000,title_page);
    mk8_settings_page_frame(title_manager); assert(in_main);
    ST32(title_page+0x120,3); mk8_settings_page_frame(title_manager); assert(!in_main);
    ST32(title_page+0x120,4); mk8_settings_page_frame(title_manager); assert(in_main);
    native_style=true;

    for(unsigned g=0;g<SETTINGS_CATEGORIES;g++) for(int i=0;i<counts[g];i++) {
        Mk8Setting *r=&catalogue[g][i]; r->id=g==0 ? i : g==1 ? 11+i : i==0 ? 20 : 19;
        r->enabled=true; r->maximum=1;
        snprintf(r->label,sizeof r->label,"Option %d",i); strcpy(r->value,"0"); strcpy(r->help,"Changes a setting.");
    }
    catalogue[0][11].id=68;
    catalogue[1][0].kind=SETTING_NUMBER; catalogue[1][0].maximum=200; catalogue[1][0].current=100; strcpy(catalogue[1][0].value,"100%");
    catalogue[2][0].kind=SETTING_BINDING; catalogue[2][1].kind=SETTING_RESET;
    memset(&cpu,0x17,sizeof cpu); cpu.sp=GUEST_HI-16;
    selector_absent=false; top=owner=panes->root=HEAP_BASE; panes->hint=HEAP_BASE+3500; panes->hint_second=HEAP_BASE+14680; panes->back_icon=HEAP_BASE+14780; from_button=true;
    panes->hint_plate=HEAP_BASE+16256; panes->hint_single=HEAP_BASE+18032; panes->hint_double=HEAP_BASE+18160;
    for(unsigned i=0;i<4;i++) { panes->keycaps[i]=TEST_KEYCAP(i); panes->key_labels[i]=panes->keycaps[i]+128; panes->action_labels[i]=panes->keycaps[i]+256; }
    ST32(top+0x120,4); ST8(HEAP_BASE+14000+0x3ca,1);
    panes->back_animation=HEAP_BASE+15200;
    ST64(owner+0x1b0,HEAP_BASE+9000); ST32(HEAP_BASE+9000+0x10,12); ST32(HEAP_BASE+9000+0x28,5);
    ST64(HEAP_BASE+9000+0x18,HEAP_BASE+9200); ST64(HEAP_BASE+9200+88,HEAP_BASE+14000+0x30);
    ST32(HEAP_BASE+14000+0x48,107); ST8(HEAP_BASE+12000+0x58,1); ST64(HEAP_BASE+10000+0x1d0,HEAP_BASE+11000);
    for(unsigned i=0;i<4;i++) ST64(owner+0x1d0+8*i,HEAP_BASE+11000+i*224);
    ST64(top+0x448,panes->root);
    for(unsigned i=0;i<=SETTINGS_VISIBLE;i++) { panes->labels[i]=HEAP_BASE+1000+i*256; panes->values[i]=HEAP_BASE+2000+i*256; panes->rows[i]=HEAP_BASE+4000+i*512; }
    Cpu saved=cpu; mk8_settings_page_tick(HEAP_BASE,true);
    check_keycap(0,"Enter","Select"); check_keycap(1,"Esc","Close");
    assert(pane_float(panes->hint_single,0x54)==56 && (LD8(panes->hint_single+0x58)&1) && !(LD8(panes->hint_double+0x58)&1));
    assert(!(LD8(panes->back_icon+0x58)&1));
    assert(active && confirming && cursor_shown && !memcmp(&saved,&cpu,sizeof cpu));
    assert(open_sounds==0);
    now=2000000000; sample(1ull<<14); assert(confirming && !category && (LD8(HEAP_BASE+12000+0x58)&1));
    decided=true; sample(1ull<<14); assert(!confirming && !cursor_mask && !LD8(HEAP_BASE+12200+0x59));
    now+=80000000; sample(1ull<<14); assert(entering && LD8(HEAP_BASE+12200+0x59)==127);
    now+=80000000; sample(1ull<<14); assert(!entering && !category && LD8(HEAP_BASE+12200+0x59)==255); sample(0);
    assert(open_sounds==1 && menu_level==MENU_HOME && focus==MENU_PLAY);
    ST8(HEAP_BASE+12064+0x58,1);
    panes->dialog_cursor=HEAP_BASE+16000; panes->cursor_animation=HEAP_BASE+15800;
    revision++; sample(0);
    assert(panes->menu_cursor_active && cursor_x==0 && cursor_y==145 && cursor_width==1700);
    assert((LD8(HEAP_BASE+15100+0x58)&1) && !(LD8(HEAP_BASE+14200+0x58)&1));
    press(1); now+=80000000; sample(0);
    assert(LD8(HEAP_BASE+12200+0x59)==255);
    finish_play_pulse(); assert(menu_level==MENU_PLAY_ACTIONS && focus==0);
    press(2); finish_play_pulse(); assert(menu_level==MENU_HOME && focus==MENU_PLAY);
    press(1ull<<15);
    check_native_rooms();
    check_native_friends(); press(1ull<<15);
    press(1); assert(menu_level==MENU_OPTIONS && category==SETTINGS_QUICK && !panes->menu_cursor_active);
    assert(!(LD8(HEAP_BASE+15100+0x58)&1) && (LD8(HEAP_BASE+14200+0x58)&1));
    for(int group=MENU_QUIT;group>=MENU_ACCOUNT;group--) {
        press(1ull<<12); assert(rich_group==group && focus==0);
        assert(!rendered_labels[1][0] && !strcmp(rendered[1],"Yes, quit to Start Screen"));
        assert(pane_float(panes->values[1],0x30)==-195);
        assert(LD8(panes->values[1]+0x59)<LD8(panes->values[2]+0x59));
        if(group==MENU_QUIT) {
            press(1ull<<15); press(1ull<<15);
            sample(1); assert(rich_level==1 && !slide_direction && focus==1);
            sample(0); sample(2); assert(!rich_level && !slide_direction && focus==2);
            sample(0); press(1ull<<15); assert(!focus);
        }
    }
    press(1ull<<15); press(1ull<<15); press(1);
    assert(rich_level==1 && focus==1);
    press(2); assert(rich_level==0 && focus==2);
    finger=true; tx=420; ty=355; sample(0); finger=false; sample(0); settle_slide();
    assert(rich_level==1);
    press(2); assert(rich_level==0 && focus==2);
    press(2); assert(menu_level==MENU_HOME && focus==HOME_SETTINGS && rich_group<0);
    finger=true; tx=640; ty=355; sample(0); finger=false; sample(0); settle_slide();
    assert(menu_level==MENU_OPTIONS && category==SETTINGS_QUICK);
    press(1ull<<14); assert(menu_level==MENU_OPTIONS && category==0 && pane_float(panes->values[1],0x30)==0);
    finger=true; tx=640; ty=205; sample(0); finger=false; sample(0); assert(category==1);
    finger=true; tx=240; ty=205; sample(0); finger=false; sample(0); assert(category==0);
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
    assert(!strcmp(last_hint,"Type a value or use the arrow keys."));
    struct Mk8ControlTips keyboard=fixed_tips;
    fixed_tips=(struct Mk8ControlTips){MK8_TIP_NINTENDO,{"B","A"}};
    sample(0);
    assert(!(LD8(panes->back_icon+0x58)&1));
    assert(!strcmp(last_hint,"Up/down: 1   Left/right: 10"));
    assert(!memcmp(shown_tips[1],u"\ue001 OK  \ue000 Cancel",sizeof u"\ue001 OK  \ue000 Cancel"));
    fixed_tips=(struct Mk8ControlTips){MK8_TIP_NINTENDO,{"A","B"}};
    sample(0); assert(LD8(panes->back_icon+0x58)&1);
    fixed_tips=keyboard; sample(0);
    assert(!(LD8(panes->back_icon+0x58)&1));
    check_keycap(0,"Enter","OK"); check_keycap(1,"Esc","Cancel");
    apply_number("80"); assert(!editing && catalogue[1][0].current==80);
    press(1); apply_number("201"); assert(editing && invalid && catalogue[1][0].current==80);
    int edit_pulses=back_pulses;
    press(2); assert(!editing && active && back_pulses==edit_pulses+1);
    press(1); apply_number("1000"); assert(editing && invalid); press(2);
    press(1); mk8_settings_page_key(8); mk8_settings_page_key(13); sample(0); assert(editing && invalid && !*number);
    apply_number("0"); assert(!editing && !catalogue[1][0].current);
    press(1); press(1ull<<14); press(1); assert(catalogue[1][0].current==10);
    overlay=true; unsigned before=focus; press(1ull<<15); assert(focus==before); overlay=false;
    focus=0; press(1ull<<14); assert(category==2); press(1ull<<15); press(1); assert(capturing);
    press(1ull<<15); assert(focus==1); capturing=false; revision++; sample(0);
    publish_during_row=true; revision++; sample(0);
    assert(!publish_during_row && last_revision!=revision);
    sample(0); assert(last_revision==revision);
    press(1ull<<15); press(1); assert(reset_confirm); press(2); assert(!reset_confirm && !resets && active);
    press(1); press(1); assert(resets==1 && !reset_confirm);
    int menu_back_pulses=back_pulses;
    press(2); assert(menu_level==MENU_HOME && focus==HOME_SETTINGS);
    assert(back_pulses==menu_back_pulses+1);
    press(2); assert(exiting && active && !cursor_shown && !returns);
    now+=80000000; sample(0); assert(active && LD8(HEAP_BASE+12200+0x59)==127);
    now+=80000000; sample(0); assert(active && exiting && !returns); // Await save acknowledgment.
    save_state=2; sample(0); assert(!active && cursor_mask==11 && returns==1);
    open_page(); press(1ull<<13); press(1); press(1ull<<12); press(1ull<<12);
    assert(rich_group==MENU_MODS);
    assert(mk8_settings_page_toggle()); sample(0); sample(0);
    save_state=-1; now+=80000000; sample(0);
    assert(active && entering && !exiting && rich_group==MENU_MODS && rich_count==2);
    now+=160000000; sample(0); assert(active && !entering && !cursor_shown);
    press(2); save_state=2; now+=160000000; sample(0); assert(!active && returns==2);
    decided=false; mk8_settings_page_tick(HEAP_BASE,true); press(2); assert(exiting && exit_from==0);
    save_state=2; sample(0); assert(!active && cursor_shown && returns==3);
    open_page(); mk8_settings_page_fixed(1ull<<15); sample(0); assert(focus==MENU_NEXTENDO); // Each opening starts at Play.
    mk8_settings_page_fixed(0); sample(0); finger=true; tx=300; ty=640; sample(0); finger=false; sample(0);
    assert(exiting); save_state=2; now+=160000000; sample(0);
    assert(!active && cursor_shown && returns==4 && text_calls>0);
    uint64_t manager=HEAP_BASE+16000;
    register_page(manager,owner,0x1271df0,13);
    ST32(manager+0xb50,1); ST64(manager+0xb58,HEAP_BASE+20000); ST64(HEAP_BASE+20000,owner);
    ST32(top+0x120,4); mk8_settings_page_frame(manager); assert(in_main && available);
    top=HEAP_BASE+21000; mk8_settings_page_frame(manager); assert(!in_main && available);
    assert(mk8_settings_page_toggle() && mk8_settings_page_active());
    mk8_settings_page_tick(owner,atomic_exchange(&requested,false));
    now+=160000000; sample(0); assert(active && !confirming && !restore_nav && !top_root);
    int32_t sticks[4]={20000,20000,20000,20000};
    assert(!mk8_settings_page_input(1,sticks,1) && !sticks[0] && !sticks[1] && !sticks[2] && !sticks[3]);
    native_style=false; // Changing theme in Native must still close and save this same open page.
    assert(mk8_settings_page_toggle()); sample(0); save_state=2; sample(0); now+=160000000; sample(0);
    assert(!mk8_settings_page_toggle()); native_style=true;
    assert(!active && cursor_shown && returns==4);
    ST32(manager+0xb50,0); ST64(manager+0xb58,0); mk8_settings_page_frame(manager);
    assert(!available && mk8_settings_page_toggle() && requested); requested=false; selector_absent=false;
    top=owner; in_main=true; available=true; from_button=true;
    panes->dialog=HEAP_BASE+15600; panes->dialog_animations[0]=HEAP_BASE+15400; panes->dialog_animations[1]=HEAP_BASE+15500;
    panes->dialog_cursor=HEAP_BASE+16000; panes->cursor_animation=HEAP_BASE+15800;
    const int nav_decides=decide_sounds;
    open_page(); back_done=false; int cancels_before=cancel_sounds, pulses_before=back_pulses, previous_cursor_pulses=cursor_pulses;
    press(2); save_state=3; now+=400000000; sample(2);
    assert(exiting && exit_pressed && !restart_prompt && reveal==1);
    assert(cancel_sounds==cancels_before+1 && back_pulses==pulses_before+1);
    back_done=true; sample(0); now+=160000000; sample(0);
    assert(restart_prompt && !restart_yes && active && !exiting && cursor_x==200);
    press(1); now+=160000000; sample(0); sample(0); assert(restart_choice==0 && !active && returns==5); // No saves without rebooting.
    assert(decide_sounds==nav_decides+1 && button_pulses==1 && cursor_pulses==previous_cursor_pulses+1);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    press(1ull<<12); assert(restart_yes && cursor_x==-200 && cursor_y==-195 && select_sounds==1);
    press(1ull<<12); assert(select_sounds==1); // No extra sound at the same selection.
    cancels_before=cancel_sounds; press(2); now+=160000000; sample(0); sample(0);
    assert(!active && restart_choice==0 && cancel_sounds==cancels_before+1 && decide_sounds==nav_decides+1);
    assert(button_pulses==2 && cursor_pulses==previous_cursor_pulses+2 && cursor_x==200 && cursor_y==-195 && returns==6);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    button_done=cursor_done=false; press(1);
    assert(restart_choice==0 && restart_pressed && !restart_wait && active && !restart_yes);
    button_done=cursor_done=true; sample(0); now+=160000000; sample(0); sample(0); assert(!active && restart_choice==0 && returns==7);
    open_page(); press(2); save_state=3; now+=160000000; sample(0);
    button_done=cursor_done=false; finger=true; tx=440; ty=555; sample(0); sample(0); finger=false; sample(0);
    assert(restart_choice==0 && restart_pressed && !restart_wait && active && restart_yes);
    sample(1); sample(1ull<<14); assert(restart_yes && decide_sounds==nav_decides+3 && button_pulses==4);
    button_done=true; sample(0); assert(!restart_wait); // The cursor also has a native Decide pulse.
    cursor_done=true; sample(0); assert(!restart_wait); now+=160000000; sample(0); assert(restart_choice==1 && restart_wait && active);
    sample(1); assert(decide_sounds==nav_decides+3 && cursor_pulses==previous_cursor_pulses+4); // Wait for the real reboot.
    MenuPanes synthetic=*panes; settings_page_reset(); assert(!owner && !active && !suppressed); check_capture_state(false);
    *panes=synthetic;
    owner=panes->root=HEAP_BASE; top=HEAP_BASE+21000; available=in_main=true;
    panes->back_animation=HEAP_BASE+15200; panes->dialog=HEAP_BASE+15600;
    panes->dialog_cursor=HEAP_BASE+16000; panes->cursor_animation=HEAP_BASE+15800;
    ST64(top+0x1b0,HEAP_BASE+9000); ST64(top+0x448,panes->root); ST32(top+0x120,4);
    for(unsigned icon=0;icon<2;icon++) for(unsigned prompt=0;prompt<2;prompt++) {
        int opened=open_sounds;
        int cancels=cancel_sounds, decides=decide_sounds, restored=returns;
        sample(0); decided=back_done=button_done=cursor_done=true;
        if(icon) mk8_settings_page_tick(top,true);
        else { assert(mk8_settings_page_toggle()); check_capture_state(true); }
        mk8_settings_page_tick(owner,atomic_load(&requested));
        now+=160000000; sample(0); assert(active && !entering && from_button==(bool)icon); check_capture_state(true);
        assert(open_sounds==opened+1);
        press(2); assert(cancel_sounds==cancels+1); check_capture_state(true); // Feedback imediato, antes do retorno.
        save_state=prompt ? 3 : 2; now+=160000000; sample(0);
        if(prompt) {
            assert(restart_prompt && !restart_yes);
            press(1); now+=160000000; sample(0); sample(0); assert(restart_choice==0);
        }
        assert(!active && cancel_sounds==cancels+1 && decide_sounds==decides+(int)prompt); check_capture_state(false);
        assert(returns==restored+(int)icon && LD8(HEAP_BASE+14000+0x3ca)==1);
    }
    // Default/NES use the same wrench. The stock confirmation finishes before opening once.
    native_style=false; decided=false; int restored=returns;
    mk8_settings_page_tick(top,true); assert(!requested && !overlay_opens);
    decided=true; mk8_settings_page_tick(top,false); mk8_settings_page_tick(top,false);
    assert(overlay_opens==1 && returns==restored+1 && !mk8_settings_page_toggle());
    native_style=true; available=false; missing_layout=true; mk8_settings_page_tick(top,true);
    assert(overlay_opens==2 && !requested && !mk8_settings_page_toggle()); missing_layout=false;
    available=true;
    // Use the real frame hook and public request API, not a synthetic atomic exchange.
    manager=HEAP_BASE+24000;
    register_page(manager,owner,0x1271df0,13);
    ST32(manager+0xb50,1); ST64(manager+0xb58,HEAP_BASE+28000); ST64(HEAP_BASE+28000,owner);
    fixed_buttons=buttons=raw_buttons=0;
    assert(!mk8_settings_page_active() && mk8_settings_page_toggle());
    assert(mk8_settings_page_toggle()); // Repeated opens while pending coalesce.
    probe_handoff=true;
    mk8_settings_page_frame(manager);
    assert(!probe_handoff && active && !requested && mk8_settings_page_active());
    assert(mk8_settings_page_toggle()); // Once active, toggle requests Back instead.
    mk8_settings_page_frame(manager); assert(active && exiting);
    // A delayed open publisher can leave a redundant request while the page is active.
    requested=true; save_state=2; probe_reopen=true;
    mk8_settings_page_frame(manager);
    assert(!probe_reopen && !active && requested && mk8_settings_page_active());
    mk8_settings_page_frame(manager); assert(active && !requested);
    now+=160000000; sample(0); check_play_routes();
    save_state=2; now+=160000000; sample(0); assert(!active);
    assert(mk8_settings_page_toggle()); mk8_settings_page_frame(manager);
    now+=160000000; sample(0); room_account_available=true;
    press(1ull<<15); press(1); press(1); assert(menu_level==MENU_OPTIONS && category==MENU_ACCOUNT);
    room_account_available=false;
    mk8_native_menu_exit(); sample(0); assert(exiting); sample(0);
    save_state=2; now+=160000000; sample(0); assert(!active);
    assert(mk8_settings_page_toggle()); mk8_settings_page_frame(manager); assert(active);
    // The two separate guest layout owners share one settings session, even through an empty active list.
    const uint64_t fader=HEAP_BASE+28500;
    register_page(manager,fader,0x12733d8,4);
    ST64(HEAP_BASE+28008,fader); ST32(manager+0xb50,2);
    fader_mount=HEAP_BASE+28576;
    fader_panes=selector_panes; fader_panes.page=fader; fader_panes.mount=fader_mount; fader_panes.visible=false;
    loading_pane=false; now+=160000000; mk8_settings_page_frame(manager); assert(active && !entering && panes==&selector_panes);
    Cpu paint_cpu=cpu; paint_cpu.sp-=512;
    menu_level=MENU_OPTIONS; category=1; focus=1; offset=0; draw(&paint_cpu);
    unsigned opened_before=begin_requests,saved_before=finish_requests;
    press(1); assert(editing); assert(mk8_settings_page_key('8')); sample(0);
    loading_pane=true; mk8_settings_page_frame(manager);
    assert(active && panes==&fader_panes && fader_panes.visible && !selector_panes.visible && focus==1 && category==1);
    assert(editing && !strcmp(number,"8")); assert(mk8_settings_page_key('0') && mk8_settings_page_key(13));
    mk8_settings_page_frame(manager); assert(!editing && catalogue[1][0].current==80);
    ST32(manager+0xb50,1); ST64(HEAP_BASE+28000,fader);
    mk8_settings_page_frame(manager); assert(active && selector_absent && panes==&fader_panes);
    mk8_settings_page_fixed(1ull<<15); mk8_settings_page_frame(manager);
    mk8_settings_page_fixed(0); mk8_settings_page_frame(manager); assert(focus==2);
    pulse_slot(&paint_cpu,1); uint64_t animated_pane=pulse_pane;
    ST32(manager+0xb50,0); mk8_settings_page_frame(manager); mk8_settings_page_frame(manager);
    assert(active && menu_level==MENU_OPTIONS && focus==2);
    assert(fader_panes.held_pulse==animated_pane && animated_pane!=0);
    ST64(HEAP_BASE+28000,owner); ST32(manager+0xb50,2); loading_pane=false; mk8_settings_page_frame(manager);
    assert(active && panes==&selector_panes && !fader_panes.visible && selector_panes.visible && focus==2 && category==1);
    assert(!fader_panes.held_pulse && pane_float(animated_pane,0x48)==1);
    assert(begin_requests==opened_before && finish_requests==saved_before);
    // Reused former addresses must not receive guest calls under the wrong class or registry slot.
    ST32(manager+0xb50,1); ST64(owner,MAIN_BASE+0x12723e0); mk8_settings_page_frame(manager);
    assert(active && selector_absent && !available);
    ST64(owner,MAIN_BASE+0x1271df0); ST64(LD64(manager+0x30)+13*8,0); mk8_settings_page_frame(manager);
    assert(active && selector_absent && !available);
    register_page(manager,owner,0x1271df0,13); ST32(manager+0xb50,2); mk8_settings_page_frame(manager);
    mk8_native_menu_exit(); sample(0); sample(0); save_state=3; now+=160000000; sample(0); assert(restart_prompt);
    int open_feedback=open_sounds;
    loading_pane=true; mk8_settings_page_frame(manager);
    assert(panes==&fader_panes && restart_prompt && !panes->menu_cursor_active);
    assert(LD8(HEAP_BASE+15100+0x59)==255 && (LD8(HEAP_BASE+15100+0x58)&1));
    loading_pane=false; mk8_settings_page_frame(manager);
    assert(panes==&selector_panes && open_feedback==open_sounds && restart_prompt);
    // Close while only the fader exists. Restore the stock selector's exact old cursor mask on its return.
    unsigned previous_mask=cursor_visibility;
    ST32(manager+0xb50,1); ST64(HEAP_BASE+28000,fader); mk8_settings_page_frame(manager);
    mk8_settings_page_fixed(1); mk8_settings_page_frame(manager);
    mk8_settings_page_fixed(0); mk8_settings_page_frame(manager);
    now+=160000000; mk8_settings_page_frame(manager); mk8_settings_page_frame(manager);
    assert(!active && restore_menu_cursors);
    ST32(manager+0xb50,0); mk8_settings_page_frame(manager);
    ST32(manager+0xb50,1); ST64(HEAP_BASE+28000,owner); mk8_settings_page_frame(manager);
    assert(!restore_menu_cursors && !cursor_owner && cursor_mask==previous_mask);
    assert(mk8_settings_page_toggle()); mk8_settings_page_frame(manager);
    check_shared_race(manager);
    settings_page_reset();
    *panes=synthetic; check_controller_footer(manager);
    settings_page_reset();
    // An early native request stays pending until lookup completes. A missing layout falls back once.
    native_style=true; loading_layout=true; unsigned overlays=overlay_opens;
    assert(mk8_settings_page_toggle()); mk8_settings_page_frame(manager);
    assert(requested && overlay_opens==overlays);
    loading_layout=false; missing_layout=true; mk8_settings_page_frame(manager); mk8_settings_page_frame(manager);
    assert(!requested && overlay_opens==overlays+1);
}
