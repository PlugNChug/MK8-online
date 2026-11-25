#include "rt.h"
#include "hle.h"
#include "settings.h"
#include "input_bind.h"
#include "nvn/shader_ahead.h"
#include "settings_values.h"
#include "mk8_upscaler_choices.h"
#include <math.h>
#include "../ui/menu_language.h"
#include <stdatomic.h>
#include <SDL3/SDL.h>
#include <string.h>

#include "settings_catalog.h"

enum { VOLUME=MK8_MENU_VOLUME, CAMERA_DISTANCE=MK8_MENU_CAMERA_DISTANCE, SHADERS=MK8_MENU_SHADERS, DEVICE=MK8_MENU_DEVICE, RESET=MK8_MENU_RESET,
       BINDING=MK8_MENU_FIELD_COUNT, MAX_ROWS=BINDING+BIND_KEY_COUNT };
static int mapping_device;
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
void renderer_get_scale(float *,float *);
double output_scale_dynamic_factor(void);
static _Atomic bool native_theme;
bool mk8_settings_style_active(void) { return native_theme; }
bool mk8_settings_input_native(void) { return g_settings.theme==THEME_NATIVE; }
void menu_notice(const char *,double);

// Sample on the renderer thread, including after a controller binding finishes.
// Coalesce held buttons and slider drags before replacing the launcher's INI.
void mk8_publish_settings(bool overlay, uint64_t now) {
    static bool watching,pending;
    static uint64_t next_write;
    static Settings seen;
    static int16_t pad[BIND_PAD_COUNT];
    static int16_t key[BIND_KEY_COUNT];
    if (!opened && !overlay) { watching=pending=false; return; }
    if (!watching || memcmp(&seen,&g_settings,sizeof seen) ||
        memcmp(pad,g_bind_pad,sizeof pad) || memcmp(key,g_bind_key,sizeof key)) {
        pending=watching;
        watching=true;
        next_write=now+350;
        memcpy(&seen,&g_settings,sizeof seen);
        memcpy(pad,g_bind_pad,sizeof pad);
        memcpy(key,g_bind_key,sizeof key);
    } else if (pending && now>=next_write) {
        pending=!mk8_settings_save_checked();
        if (pending) next_write=now+2000;
    }
}

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
bool mk8_settings_values_request(int operation,int id,int value) {
    if(operation<SETTINGS_STEP || operation>SETTINGS_RESTART ||
       ((operation==SETTINGS_STEP || operation==SETTINGS_SET || operation==SETTINGS_ACTIVATE) && (id<0 || id>=BINDING+2*BIND_KEY_COUNT))) return false;
    if(operation==SETTINGS_SET && !(id==VOLUME && mk8_settings_volume_valid(value)) &&
       !(id==CAMERA_DISTANCE && value>=MK8_CAMERA_DISTANCE_MIN && value<=MK8_CAMERA_DISTANCE_MAX)) return false;
    if(operation==SETTINGS_STEP && (id>=BINDING || id==RESET || id==SHADERS || (value!=-1 && value!=1))) return false;
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
        if(saved==2) { dirty=false; opened=false; if(r.value==1 && main_menu && mk8_menu_restart_needed(&boot_settings)) saved=3; }
        else if(r.value==SETTINGS_DETACHED)
            menu_notice(mk8_menu_save_error(),8.0);
        return;
    }
    if(r.operation==SETTINGS_RESTART) {
        if(saved!=3) return;
        if(r.value && main_menu && mk8_menu_restart_needed(&boot_settings)) { saved=4; rt_request_reboot(RT_REBOOT_GAME); }
        else saved=2;
        return;
    }
    if(!opened) return;
    if(r.id<BINDING && !mk8_menu_option_enabled(r.id,main_menu)) return;
    if(r.id>=BINDING) {
        int dev=(r.id-BINDING)/BIND_KEY_COUNT, slot=(r.id-BINDING)%BIND_KEY_COUNT;
        if(r.operation==SETTINGS_ACTIVATE && slot<(dev==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT)) bind_capture_begin(dev,slot);
        return;
    }
    if(r.id==RESET) { if(r.operation==SETTINGS_ACTIVATE) { bind_reset(mapping_device); dirty=true; } return; }
    if(r.id==MK8_MENU_UPSCALER) {
        Mk8Upscalers list=mk8_upscaler_choices(g_settings.upscaler);
        int next=list.selected+r.value;
        if(next>=0 && next<list.count) { g_settings.upscaler=list.values[next]; dirty=true; }
        return;
    }
    if(r.id==SHADERS) return;
    if(r.id==DEVICE) { mapping_device=r.value<0 ? BIND_DEV_PAD : BIND_DEV_KEY; return; }
    if(r.id==VOLUME) {
        int pct=mk8_settings_volume_percent();
        int next=r.operation==SETTINGS_SET ? r.value : pct+MK8_VOLUME_STEP*r.value;
        if(next<MK8_VOLUME_MIN) next=MK8_VOLUME_MIN;
        if(next>MK8_VOLUME_MAX) next=MK8_VOLUME_MAX;
        mk8_settings_set_volume_percent(next); dirty|=next!=pct; return;
    }
    if(r.id==CAMERA_DISTANCE) {
        int current=mk8_settings_camera_distance();
        int next=r.operation==SETTINGS_SET ? r.value : current+10*r.value;
        if(next<MK8_CAMERA_DISTANCE_MIN) next=MK8_CAMERA_DISTANCE_MIN;
        if(next>MK8_CAMERA_DISTANCE_MAX) next=MK8_CAMERA_DISTANCE_MAX;
        mk8_settings_set_camera_distance(next);
        dirty|=next!=current;
        return;
    }
    const Mk8MenuOption *o=mk8_menu_option(r.id);
    if(r.id==MK8_MENU_CONTROLLER_VIBRATION) {
        mk8_rumble_set_enabled(!g_settings.controller_vibration);
        dirty=true;
    } else if(o->toggle) { *o->toggle=!*o->toggle; dirty=true; }
    else if(o->choice) {
        int index=mk8_menu_choice_index(r.id);
        if(index<0) index=o->default_index;
        int next=index+(r.value<0 ? -1 : 1);
        next=next<0 ? 0 : next>=o->count ? o->count-1 : next;
        if(next!=index) { *o->choice=o->values ? o->values[next] : next; dirty=true; }
    }
    if(r.id==MK8_MENU_FULLSCREEN) mk8_settings_window_changed();
}

// Called by the existing menu renderer: all real settings writes share its thread.
void mk8_settings_values_sync(void) {
    native_theme=g_settings.theme==THEME_NATIVE;
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
    if(n) native_theme=g_settings.theme==THEME_NATIVE;
    int cd,cs,ms;
    capturing=bind_capture_active(&cd,&cs,&ms);
    Mk8Setting next[SETTINGS_CATEGORIES][MAX_ROWS]={0};
    Mk8Setting *fields[MK8_MENU_FIELD_COUNT];
    // Native Video reserves its first row for the theme choice; the shared field IDs stay stable.
    int sizes[SETTINGS_CATEGORIES]={[MK8_MENU_VIDEO]=1};
    int binding_count=mapping_device==BIND_DEV_PAD ? BIND_PAD_COUNT : BIND_KEY_COUNT;
    int family=mapping_device==BIND_DEV_PAD ? bind_label_family() : 0;
    static char device[41];
    static uint64_t refresh_device;
    uint64_t now=SDL_GetTicks();
    if (opened && (!refresh_device || now>=refresh_device)) {
        int count=0;
        SDL_JoystickID *ids=SDL_GetGamepads(&count);
        const char *name=ids && count ? SDL_GetGamepadNameForID(ids[0]) : NULL;
        SDL_utf8strlcpy(device,name ? name : "No controller connected",sizeof device);
        SDL_free(ids);
        refresh_device=now+1000;
    }
    for(int i=0;i<BINDING+binding_count;i++) {
        const Mk8MenuOption *o=mk8_menu_option(i);
        int group=o ? o->group : MK8_MENU_CONTROLS;
        Mk8Setting *v=i==MK8_MENU_THEME ? &next[group][0] : &next[group][sizes[group]++];
        if(o) fields[i]=v;
        v->id=i;
        v->enabled=!o || mk8_menu_option_enabled(i,main_menu);
        if(!o) {
            int slot=mk8_menu_bind_order[i-BINDING]; v->id=BINDING+mapping_device*BIND_KEY_COUNT+slot; v->kind=SETTING_BINDING;
            snprintf(v->label,sizeof v->label,"%s",bind_input_name(slot));
            if(capturing && cd==mapping_device && cs==slot) snprintf(v->value,sizeof v->value,"Press %s... %d",mapping_device==BIND_DEV_PAD ? "a button" : "a key",(ms+999)/1000);
            else snprintf(v->value,sizeof v->value,"%s",mapping_device==BIND_DEV_PAD ? bind_pad_label(g_bind_pad[slot],family) : bind_key_label(g_bind_key[slot]));
            snprintf(v->help,sizeof v->help,"%s binding. Conflicts swap.",
                     bind_is_default_slot(mapping_device,slot) ? "Default" : "Custom");
            continue;
        }
        snprintf(v->label,sizeof v->label,"%s",o->label); snprintf(v->help,sizeof v->help,"%s",o->help);
        if(v->id==SHADERS) {
            AheadStatus a=shader_ahead_status(); v->kind=SETTING_INFO; v->enabled=false;
            snprintf(v->value,sizeof v->value,a.state==2 ? "Prepared" : a.state==1 ? "Preparing (%d)" : "Starts with the game",a.items);
        } else if(v->id==MK8_MENU_UPSCALER) {
            Mk8Upscalers list=mk8_upscaler_choices(g_settings.upscaler);
            v->maximum=list.count-1; v->current=list.selected;
            snprintf(v->value,sizeof v->value,"%s",list.names[list.selected]);
            if (list.count>2) snprintf(v->help,sizeof v->help,"Reopen game to load a new provider.");
        } else if(v->id==VOLUME) {
            v->kind=SETTING_NUMBER; v->current=mk8_settings_volume_percent(); v->minimum=MK8_VOLUME_MIN; v->maximum=MK8_VOLUME_MAX;
            snprintf(v->value,sizeof v->value,"%d%%",v->current);
        } else if(v->id==CAMERA_DISTANCE) {
            v->kind=SETTING_NUMBER; v->current=mk8_settings_camera_distance();
            v->minimum=MK8_CAMERA_DISTANCE_MIN; v->maximum=MK8_CAMERA_DISTANCE_MAX;
            snprintf(v->value,sizeof v->value,"%d%%",v->current);
        } else if(v->id==DEVICE) {
            v->current=mapping_device; v->maximum=1;
            if (*device) snprintf(v->help,sizeof v->help,"%s",device);
            snprintf(v->value,sizeof v->value,"%s",mapping_device==BIND_DEV_PAD ? "Controller" : "Keyboard");
        } else if(v->id==RESET) {
            v->kind=SETTING_RESET; snprintf(v->value,sizeof v->value,"Restore defaults");
        } else if(o->toggle) {
            v->maximum=1; v->current=*o->toggle; snprintf(v->value,sizeof v->value,*o->toggle ? "On" : "Off");
        } else {
            int index=mk8_menu_choice_index(v->id);
            v->maximum=o->count-1;
            if(index>=0) { v->current=index; snprintf(v->value,sizeof v->value,"%s",o->names[index]); }
            else snprintf(v->value,sizeof v->value,"Custom (%d)",*o->choice);
        }
        if (v->id==MK8_MENU_RESOLUTION && opened) {
            float sx,sy; renderer_get_scale(&sx,&sy);
            if (g_settings.res_mode==RES_DYNAMIC)
                snprintf(v->help,sizeof v->help,"Rendering %.0fx%.0f (%.0f%%).",1920.f*sx,1080.f*sy,100.0*output_scale_dynamic_factor());
            else snprintf(v->help,sizeof v->help,"Rendering %.0fx%.0f.",1920.f*sx,1080.f*sy);
        }
        if(!v->enabled && o->availability==MK8_MENU_ON_RESTART) {
            snprintf(v->help,sizeof v->help,"Exit the race to modify.");
        }
    }
    for (unsigned i=0;i<sizeof mk8_menu_quick/sizeof *mk8_menu_quick;i++)
        next[SETTINGS_QUICK][sizes[SETTINGS_QUICK]++]=*fields[mk8_menu_quick[i].id];
    pthread_mutex_lock(&lock);
    if(memcmp(cached,next,sizeof cached) || memcmp(counts,sizes,sizeof counts)) { memcpy(cached,next,sizeof cached); memcpy(counts,sizes,sizeof counts); revision++; }
    pthread_mutex_unlock(&lock);
}
