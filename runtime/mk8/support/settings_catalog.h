#pragma once
#include <stddef.h>
#include "input_bind.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "settings.h"
#ifdef __cplusplus
}
#endif

static const int mk8_menu_bind_order[]={
    BIND_A,BIND_B,BIND_X,BIND_Y,BIND_L,BIND_R,BIND_ZL,BIND_ZR,BIND_PLUS,BIND_MINUS,BIND_UP,BIND_DOWN,
    BIND_LEFT,BIND_RIGHT,BIND_LSTICK,BIND_RSTICK,BIND_LS_UP,BIND_LS_DOWN,BIND_LS_LEFT,BIND_LS_RIGHT,
    BIND_RS_UP,BIND_RS_DOWN,BIND_RS_LEFT,BIND_RS_RIGHT
};

enum { MK8_MENU_VIDEO, MK8_MENU_AUDIO, MK8_MENU_CONTROLS };
enum { MK8_MENU_ALWAYS, MK8_MENU_ON_RESTART };
enum { MK8_MENU_TOGGLE, MK8_MENU_CHOICE, MK8_MENU_NUMBER, MK8_MENU_INFO, MK8_MENU_ACTION };

// Ordered like the Native pages. Other renderers can group the same options differently.
#define MK8_MENU_FIELDS(T,C,S) \
    C(ASPECT, VIDEO, "Aspect ratio", aspect, aspects, NULL, 0, ALWAYS, "Solo 16:9-32:9; other views: 16:9.") \
    S(CAMERA_DISTANCE, VIDEO, "Driving camera distance", NUMBER, "Solo forward and rear. 100% is stock.") \
    T(SPLIT_SCREEN_60FPS, VIDEO, "Split-screen 60 FPS", split_screen_60fps, false, ALWAYS, "Target 60 FPS per view with 3-4 players.") \
    T(BLOOM, VIDEO, "Bloom", bloom, true, ALWAYS, "Add a glow around bright objects.") \
    T(FULLSCREEN, VIDEO, "Fullscreen", fullscreen, false, ALWAYS, "F11 also toggles fullscreen.") \
    T(AUTO_HIDE_CURSOR, VIDEO, "Auto-hide cursor", auto_hide_cursor, true, ALWAYS, "Hide pointer when idle in fullscreen.") \
    T(SHOW_FPS, VIDEO, "Show FPS", show_fps, false, ALWAYS, "Show FPS in the screen corner.") \
    T(SHOW_MEMORY, VIDEO, "Show memory use", show_memory, false, ALWAYS, "Show runtime memory usage.") \
    C(RESOLUTION, VIDEO, "Internal resolution", res_mode, resolutions, resolution_values, 3, ALWAYS, "Higher values use more GPU memory.") \
    S(UPSCALER, VIDEO, "Upscaler", CHOICE, "Used below the display resolution.") \
    C(ANTIALIAS, VIDEO, "Anti-aliasing", antialias, antialias, NULL, 0, ALWAYS, "Smooth jagged edges with SMAA.") \
    C(FRAME_GEN, VIDEO, "Frame generation", frame_gen, framegen, NULL, 0, ALWAYS, "High refresh; game stays at 60.") \
    T(SKIP_INTRO, VIDEO, "Skip intro", skip_intro, false, ON_RESTART, "Skip logos on the next launch.") \
    S(SHADERS, VIDEO, "Shader preparation", INFO, "Prepares shaders in background.") \
    T(AHEAD_FAST, VIDEO, "Full-speed shaders", ahead_fast, false, ON_RESTART, "All CPU cores; next launch.") \
    C(THEME, VIDEO, "Menu theme", theme, themes, theme_values, 0, ALWAYS, "Used the next time you open the Menu.") \
    S(VOLUME, AUDIO, "Master Volume", NUMBER, "Adjust the game audio level.") \
    T(REVERB, AUDIO, "Reverb", reverb, true, ALWAYS, "Enable the game's room reverb.") \
    T(MOVIE_AUDIO, AUDIO, "Movie audio", movie_audio, true, ALWAYS, "Used by the next movie.") \
    T(MUTE_IN_BACKGROUND, AUDIO, "Mute in background", mute_in_background, false, ALWAYS, "Mute while another app has focus.") \
    C(LANGUAGE, CONTROLS, "Game language", language, g_language_names, NULL, LANG_AUTO, ON_RESTART, "Applies after restarting the game.") \
    C(PROMPTS, CONTROLS, "Button icons", prompts, prompts, NULL, PROMPTS_AUTO, ALWAYS, "Choose controller button icons.") \
    S(RUMBLE_INTENSITY, CONTROLS, "Rumble intensity", NUMBER, "0% turns rumble off for all players.") \
    S(DEVICE, CONTROLS, "Mapping device", CHOICE, "Choose bindings to edit.") \
    S(RESET, CONTROLS, "Reset these bindings", ACTION, "Restore this device's defaults.")

#define MK8_FIELD_ID(id, ...) MK8_MENU_##id,
enum Mk8MenuField { MK8_MENU_FIELDS(MK8_FIELD_ID,MK8_FIELD_ID,MK8_FIELD_ID) MK8_MENU_FIELD_COUNT };
#undef MK8_FIELD_ID

static const struct { int id; const char *pt, *es; } mk8_menu_quick[]={
    {MK8_MENU_SHOW_FPS,"Mostrar FPS","Mostrar FPS"},
    {MK8_MENU_SHOW_MEMORY,"Mostrar memória utilizada","Mostrar uso de memoria"},
    {MK8_MENU_FULLSCREEN,"Tela cheia","Pantalla completa"},
    {MK8_MENU_MUTE_IN_BACKGROUND,"Silenciar em segundo plano","Silenciar en segundo plano"},
    {MK8_MENU_VOLUME,"Volume","Volumen"}
};

typedef struct {
    const char *label, *help;
    int group, kind, availability;
    bool *toggle;
    int *choice;
    const char *const *names;
    const int *values;
    int count, default_index;
} Mk8MenuOption;

static inline const Mk8MenuOption *mk8_menu_option(int id) {
    static const char *const themes[]={"Native","Simple Dark","Simple Light","NES Dark","NES Light"};
    static const int theme_values[]={THEME_NATIVE,THEME_DEFAULT,THEME_SIMPLE_LIGHT,THEME_NES,THEME_NES_LIGHT};
    static const char *const aspects[]={"Stretch","Original 16:9","Ultrawide (experimental)"};
    static const char *const resolutions[]={"25%","50%","75%","Match display","Dynamic","1x (1080p)","2x","3x","4x"};
    static const int resolution_values[]={RES_Q25,RES_Q50,RES_Q75,RES_MATCH,RES_DYNAMIC,1,2,3,4};
    static const char *const antialias[]={"Off","SMAA"};
    static const char *const framegen[]={"Off","120 FPS"
#ifdef __APPLE__
        ,"120 FPS (replay)"
#endif
    };
    static const char *const prompts[]={"Auto","Nintendo","Xbox","PlayStation"};
#define MK8_TOGGLE(id,group,label,field,def,rule,help) \
    {label,help,MK8_MENU_##group,MK8_MENU_TOGGLE,MK8_MENU_##rule,&g_settings.field,NULL,NULL,NULL,2,def},
#define MK8_CHOICE(id,group,label,field,names,values,def,rule,help) \
    {label,help,MK8_MENU_##group,MK8_MENU_CHOICE,MK8_MENU_##rule,NULL,&g_settings.field,names,values,(int)(sizeof(names)/sizeof(*(names))),def},
#define MK8_SPECIAL(id,group,label,kind,help) \
    {label,help,MK8_MENU_##group,MK8_MENU_##kind,MK8_MENU_ALWAYS,NULL,NULL,NULL,NULL,0,0},
    static const Mk8MenuOption options[]={MK8_MENU_FIELDS(MK8_TOGGLE,MK8_CHOICE,MK8_SPECIAL)};
#undef MK8_TOGGLE
#undef MK8_CHOICE
#undef MK8_SPECIAL
    return (unsigned)id<MK8_MENU_FIELD_COUNT ? &options[id] : NULL;
}

static inline int mk8_menu_choice_index(int id) {
    const Mk8MenuOption *option=mk8_menu_option(id);
    int value=*option->choice;
#ifndef __APPLE__
    // Vulkan uses its normal 120 FPS path for an INI copied from Metal in replay mode.
    if(id==MK8_MENU_FRAME_GEN && value==FRAMEGEN_REPLAY) value=FRAMEGEN_120;
#endif
    for(int i=0;i<option->count;i++)
        if(value==(option->values ? option->values[i] : i)) return i;
    return -1;
}

static inline bool mk8_menu_option_enabled(int id, bool main_menu) {
    const Mk8MenuOption *o=mk8_menu_option(id);
    return o && o->kind!=MK8_MENU_INFO &&
        (o->availability!=MK8_MENU_ON_RESTART || main_menu);
}

static inline bool mk8_menu_restart_needed(const Settings *boot) {
#define MK8_COMPARE(id,group,label,field,def,rule,help) if (MK8_MENU_##rule==MK8_MENU_ON_RESTART && g_settings.field!=boot->field) return true;
#define MK8_COMPARE_CHOICE(id,group,label,field,names,values,def,rule,help) MK8_COMPARE(id,group,label,field,def,rule,help)
#define MK8_IGNORE(...)
    MK8_MENU_FIELDS(MK8_COMPARE,MK8_COMPARE_CHOICE,MK8_IGNORE)
#undef MK8_IGNORE
#undef MK8_COMPARE_CHOICE
#undef MK8_COMPARE
    return false;
}
#undef MK8_MENU_FIELDS
