#include "rt.h"
#include "hle.h"
#include "settings.h"
#include "input_bind.h"
#include "nvn/shader_ahead.h"
#include "settings_values.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>

typedef struct {
    const char *label, *help;
    bool *toggle;
    int *choice;
    const char *const *names;
    const int *values;
    int count;
} Option;
static const char *const aspects[]={"Stretch","Original 16:9","Ultrawide (experimental)"};
static const char *const resolutions[]={"25%","50%","75%","Match display","Dynamic","1x (1080p)","2x","3x","4x"};
static const int resolution_values[]={RES_Q25,RES_Q50,RES_Q75,RES_MATCH,RES_DYNAMIC,1,2,3,4};
static const char *const upscalers[]={"Bilinear",
#ifdef __APPLE__
    "MetalFX"
#else
    "AMD FSR 1"
#endif
};
static const int upscaler_values[]={UPSCALE_BILINEAR,UPSCALE_SHARP};
static const char *const antialias[]={"Off","SMAA"}, *const framegen[]={"Off","120 FPS"};
static const char *const prompts[]={"Auto","Nintendo","Xbox","PlayStation"};
static const char *const devices[]={"Controller","Keyboard"};
static int mapping_device;
#define TOGGLE(label,field,help) {label,help,&g_settings.field,NULL,NULL,NULL,2}
#define CHOICE(label,field,names,values,n,help) {label,help,NULL,&g_settings.field,names,values,n}
static const Option options[]={
    CHOICE("Aspect ratio",aspect,aspects,NULL,3,"Solo 16:9-32:9; FG/other views: 16:9."),
    TOGGLE("Fullscreen",fullscreen,"F11 also toggles fullscreen."),
    TOGGLE("Show FPS",show_fps,"Show FPS in the screen corner."),
    TOGGLE("Show memory use",show_memory,"Show runtime memory usage."),
    CHOICE("Internal resolution",res_mode,resolutions,resolution_values,9,"Higher values use more GPU memory."),
    CHOICE("Upscaler",upscaler,upscalers,upscaler_values,2,"Used below the display resolution."),
    CHOICE("Anti-aliasing",antialias,antialias,NULL,2,"Smooth jagged edges with SMAA."),
    CHOICE("Frame generation",frame_gen,framegen,NULL,2,"High refresh; game stays at 60."),
    TOGGLE("Skip intro",skip_intro,"Skip logos on the next launch."),
    {"Shader preparation","Prepares shaders in background."},
    TOGGLE("Full-speed shaders",ahead_fast,"All CPU cores; next launch."),
    {"Master Volume","Adjust the game audio level."},
    TOGGLE("Reverb",reverb,"Enable the game's room reverb."),
    TOGGLE("Movie audio",movie_audio,"Used by the next movie."),
    TOGGLE("Mute in background",mute_in_background,"Mute while another app has focus."),
    CHOICE("Game language",language,g_language_names,NULL,N_LANGUAGES,"Applies after restarting the game."),
    CHOICE("Button icons",prompts,prompts,NULL,4,"Choose controller button icons."),
    TOGGLE("Controllers start at P2",pads_from_p2,"Keyboard P1; controllers from P2."),
    CHOICE("Joy-Con mode",joycon_mode,mk8_joycon_modes,NULL,2,"One pair or two sideways controllers."),
    {"Mapping device","Choose bindings to edit.",NULL,&mapping_device,devices,NULL,2},
    {"Reset these bindings","Restore this device's defaults."}
};
#undef TOGGLE
#undef CHOICE
enum { VOLUME=11, SHADERS=9, JOYCONS=18, DEVICE=19, RESET=20, BINDING=21, OPTIONS=21, MAX_ROWS=40 };
static const int bind_order[]={BIND_A,BIND_B,BIND_X,BIND_Y,BIND_L,BIND_R,BIND_ZL,BIND_ZR,BIND_PLUS,BIND_MINUS,
    BIND_UP,BIND_DOWN,BIND_LEFT,BIND_RIGHT,BIND_LSTICK,BIND_RSTICK,BIND_LS_UP,BIND_LS_DOWN,BIND_LS_LEFT,BIND_LS_RIGHT,
    BIND_RS_UP,BIND_RS_DOWN,BIND_RS_LEFT,BIND_RS_RIGHT};
typedef struct { int operation,id,value; } Request;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
enum { REQUEST_LIMIT=32 };
static Request requests[REQUEST_LIMIT+1];  // One terminal close must fit even when all ordinary slots are full.
static unsigned requested;
static Mk8Setting cached[SETTINGS_CATEGORIES][MAX_ROWS];
static int counts[SETTINGS_CATEGORIES];
static _Atomic uint64_t revision;
static _Atomic int saved;
static _Atomic bool capturing;
static _Atomic bool main_menu;
static bool opened,dirty;
static Settings baseline;
static Settings boot_settings;
static bool boot_set;
static int seen_binding=-1;
bool mk8_settings_save_checked(void);
void mk8_settings_window_changed(void);
void mk8_settings_pads_changed(void);
bool mk8_settings_style_active(void);
void menu_notice(const char *,double);

RT_ON_RESET(settings_values_reset) {
    pthread_mutex_lock(&lock);
    requested=0; opened=dirty=boot_set=false; saved=0; capturing=main_menu=false; seen_binding=-1;
    pthread_mutex_unlock(&lock);
}

int mk8_settings_values_count(unsigned category) {
    if(category>=SETTINGS_CATEGORIES) return 0;
    pthread_mutex_lock(&lock); int n=counts[category]; pthread_mutex_unlock(&lock); return n;
}
bool mk8_settings_values_row(unsigned category,unsigned row,Mk8Setting *out) {
    if(category>=SETTINGS_CATEGORIES || !out) return false;
    pthread_mutex_lock(&lock);
    bool valid=row<(unsigned)counts[category];
    if(valid) *out=cached[category][row];
    pthread_mutex_unlock(&lock); return valid;
}
uint64_t mk8_settings_values_revision(void) { return revision; }
int mk8_settings_values_saved(void) { return saved; }
bool mk8_settings_values_capturing(void) { return capturing; }
void mk8_settings_values_context(bool value) { main_menu=value; }
bool mk8_settings_values_main_menu(void) { return main_menu; }
static bool needs_main_menu(int id) { return id==8 || id==10 || id==15; }
static bool needs_restart(void) {
    return boot_settings.language!=g_settings.language || boot_settings.skip_intro!=g_settings.skip_intro || boot_settings.ahead_fast!=g_settings.ahead_fast;
}
bool mk8_settings_values_request(int operation,int id,int value) {
    if(operation<SETTINGS_STEP || operation>SETTINGS_RESTART ||
       ((operation==SETTINGS_STEP || operation==SETTINGS_SET || operation==SETTINGS_ACTIVATE) && (id<0 || id>=BINDING+2*BIND_KEY_COUNT))) return false;
    if(operation==SETTINGS_SET && (id!=VOLUME || value<0 || value>200)) return false;
    if(operation==SETTINGS_STEP && (id>=OPTIONS || id==RESET || id==SHADERS || (value!=-1 && value!=1))) return false;
    if(operation==SETTINGS_ACTIVATE && id!=RESET && id<BINDING) return false;
    bool detached=operation==SETTINGS_FINISH && value==SETTINGS_DETACHED;
    if((operation==SETTINGS_FINISH || operation==SETTINGS_RESTART) && (id || (value!=0 && value!=1 && !detached))) return false;
    if(operation==SETTINGS_ACTIVATE && id>=BINDING && (id-BINDING)/BIND_KEY_COUNT==BIND_DEV_PAD &&
       (id-BINDING)%BIND_KEY_COUNT>=BIND_PAD_COUNT) return false;
    pthread_mutex_lock(&lock);
    bool room=requested<REQUEST_LIMIT+(unsigned)detached;
    if(room) { requests[requested++]=(Request){operation,id,value}; if(operation==SETTINGS_FINISH) saved=1; }
    pthread_mutex_unlock(&lock); return room || detached;
}

static void apply(Request r) {
    if(r.operation==SETTINGS_BEGIN) {
        if(!opened) { dirty=false; baseline=g_settings; }
        opened=true; if(saved!=-1) saved=0; return;
    }
    if(r.operation==SETTINGS_FINISH) {
        bind_capture_cancel(); capturing=false;
        saved=!dirty || mk8_settings_save_checked() ? 2 : -1;
        if(saved==2) { dirty=false; opened=false; if(r.value==1 && main_menu && needs_restart()) saved=3; }
        else if(r.value==SETTINGS_DETACHED)
            menu_notice("Settings could not be saved. Check the settings folder, then reopen Settings and go Back to retry.",8.0);
        return;
    }
    if(r.operation==SETTINGS_RESTART) {
        if(saved!=3) return;
        if(r.value && main_menu && needs_restart()) { saved=4; rt_request_reboot(RT_REBOOT_GAME); }
        else saved=2;
        return;
    }
    if(!opened) return;
    if(needs_main_menu(r.id) && !main_menu) return;
    if(r.id>=BINDING) {
        int dev=(r.id-BINDING)/BIND_KEY_COUNT, slot=(r.id-BINDING)%BIND_KEY_COUNT;
        if(r.operation==SETTINGS_ACTIVATE && slot<(dev==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)) bind_capture_begin(dev,slot);
        return;
    }
    if(r.id==RESET) { if(r.operation==SETTINGS_ACTIVATE) { bind_reset(mapping_device); dirty=true; } return; }
    if(r.id==SHADERS || (r.id==5 && g_settings.res_mode==RES_MATCH)) return;
    if(r.id==VOLUME) {
        int pct=(int)lroundf(g_settings.volume*100);
        int next=r.operation==SETTINGS_SET ? r.value : pct+(r.value<0 ? -5 : 5);
        if(next<0 || next>200) return;
        g_settings.volume=next/100.0f; dirty|=next!=pct; return;
    }
    const Option *o=&options[r.id];
    if(o->toggle) { *o->toggle=!*o->toggle; dirty=true; }
    else if(o->choice) {
        int index=0;
        for(int i=0;i<o->count;i++) if(*o->choice==(o->values ? o->values[i] : i)) index=i;
        int next=index+(r.value<0 ? -1 : 1);
        next=next<0 ? 0 : next>=o->count ? o->count-1 : next;
        if(r.id==JOYCONS && next!=index && !mk8_set_joycon_mode(next)) return;
        *o->choice=o->values ? o->values[next] : next;
        dirty|=r.id!=DEVICE && next!=index;
    }
    if(r.id==1) mk8_settings_window_changed();
    if(r.id==17) mk8_settings_pads_changed();
}

// Called by the existing menu renderer: all real settings writes share its thread.
void mk8_settings_values_sync(void) {
    if(!mk8_settings_style_active()) return;
    if(!boot_set) { boot_settings=g_settings; boot_set=true; }
    Request batch[REQUEST_LIMIT+1]; unsigned n;
    pthread_mutex_lock(&lock); n=requested; memcpy(batch,requests,n*sizeof *batch); requested=0; pthread_mutex_unlock(&lock);
    // Keep the initial snapshot for menu availability; rebuild it on open or queued work.
    if (!opened && !n && revision) return;
    int serial,kind=bind_last_result(NULL,NULL,NULL,&serial);
    if(opened && memcmp(&baseline,&g_settings,sizeof baseline)) dirty=true;
    if(opened && seen_binding>=0 && serial!=seen_binding && (kind==1 || kind==2 || kind==5)) dirty=true;
    seen_binding=serial;
    for(unsigned i=0;i<n;i++) apply(batch[i]);
    int cd,cs,ms;
    capturing=bind_capture_active(&cd,&cs,&ms);
    Mk8Setting next[SETTINGS_CATEGORIES][MAX_ROWS]={0};
    int sizes[]={11,4,6+(mapping_device==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)};
    int family=mapping_device==BIND_DEV_PAD ? bind_label_family() : 0;
    for(unsigned g=0;g<SETTINGS_CATEGORIES;g++) for(int row=0;row<sizes[g];row++) {
        Mk8Setting *v=&next[g][row];
        v->enabled=true;
        v->id=g==0 ? row : g==1 ? VOLUME+row : 15+row;
        if(g==2 && row>=6) {
            int slot=bind_order[row-6]; v->id=BINDING+mapping_device*BIND_KEY_COUNT+slot; v->kind=SETTING_BINDING;
            snprintf(v->label,sizeof v->label,"%s",bind_input_name(slot));
            if(capturing && cd==mapping_device && cs==slot) snprintf(v->value,sizeof v->value,"Press %s... %d",mapping_device==BIND_DEV_PAD ? "a button" : "a key",(ms+999)/1000);
            else snprintf(v->value,sizeof v->value,"%s",mapping_device==BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot],family) : bind_key_label(g_bind_key[slot]));
            snprintf(v->help,sizeof v->help,"Conflicting bindings swap positions.");
            continue;
        }
        const Option *o=&options[v->id];
        snprintf(v->label,sizeof v->label,"%s",o->label); snprintf(v->help,sizeof v->help,"%s",o->help);
        if(v->id==SHADERS) {
            AheadStatus a=shader_ahead_status(); v->kind=SETTING_INFO; v->enabled=false;
            snprintf(v->value,sizeof v->value,a.state==2 ? "Prepared" : a.state==1 ? "Preparing (%d)" : "Starts with the game",a.items);
        } else if(v->id==VOLUME) {
            v->kind=SETTING_NUMBER; v->current=(int)lroundf(g_settings.volume*100); v->maximum=200;
            snprintf(v->value,sizeof v->value,"%d%%",v->current);
        } else if(v->id==RESET) {
            v->kind=SETTING_RESET; snprintf(v->value,sizeof v->value,"Restore defaults");
        } else if(o->toggle) {
            v->maximum=1; v->current=*o->toggle; snprintf(v->value,sizeof v->value,*o->toggle ? "On" : "Off");
        } else {
            v->maximum=o->count-1; bool found=false;
            for(int i=0;i<o->count;i++) if(*o->choice==(o->values ? o->values[i] : i)) { v->current=i; snprintf(v->value,sizeof v->value,"%s",o->names[i]); found=true; }
            if(!found) snprintf(v->value,sizeof v->value,"Custom (%d)",*o->choice);
            if(v->id==5 && g_settings.res_mode==RES_MATCH) { v->enabled=false; snprintf(v->value,sizeof v->value,"Not used at Match"); }
        }
        if(needs_main_menu(v->id) && !main_menu) {
            v->enabled=false; snprintf(v->help,sizeof v->help,"Exit the race to modify.");
        }
    }
    pthread_mutex_lock(&lock);
    if(memcmp(cached,next,sizeof cached) || memcmp(counts,sizes,sizeof counts)) { memcpy(cached,next,sizeof cached); memcpy(counts,sizes,sizeof counts); revision++; }
    pthread_mutex_unlock(&lock);
}
