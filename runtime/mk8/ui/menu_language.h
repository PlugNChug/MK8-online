#pragma once
#include <stdlib.h>
#include <string.h>

typedef enum { MK8_MENU_EN, MK8_MENU_PT, MK8_MENU_ES } Mk8MenuLanguage;

static inline Mk8MenuLanguage mk8_menu_language(void) {
    const char *code = getenv("MK8_SETTINGS_LANGUAGE");
    if (code && !strcmp(code, "pt-BR")) return MK8_MENU_PT;
    if (code && !strcmp(code, "es")) return MK8_MENU_ES;
    return MK8_MENU_EN;
}

static inline const char *mk8_menu_text(Mk8MenuLanguage language, const char *en, const char *pt, const char *es) {
    return language == MK8_MENU_PT ? pt : language == MK8_MENU_ES ? es : en;
}

typedef struct { const char *en, *pt, *es; } Mk8MenuLabel;
static inline const char *mk8_menu_label(Mk8MenuLanguage language, Mk8MenuLabel label) {
    return mk8_menu_text(language, label.en, label.pt, label.es);
}

enum Mk8MenuPage { MENU_PLAY, MENU_NEXTENDO, MENU_SOCIAL, MENU_SETTINGS, MENU_PAGE_COUNT };
static const Mk8MenuLabel mk8_menu_pages[MENU_PAGE_COUNT] = {
    {"Play","Jogar","Jugar"}, {"Nextendo","Nextendo","Nextendo"}, {"Social","Social","Social"},
    {"Settings","Ajustes","Ajustes"}
};
enum { HOME_PLAY, HOME_NEXTENDO, HOME_SETTINGS, HOME_COUNT };
static const enum Mk8MenuPage mk8_home_pages[HOME_COUNT]={MENU_PLAY,MENU_NEXTENDO,MENU_SETTINGS};
static inline unsigned mk8_home_row(int page) {
    return page==MENU_SETTINGS ? HOME_SETTINGS : page==MENU_PLAY ? HOME_PLAY : HOME_NEXTENDO;
}
static const Mk8MenuLabel mk8_nx_views[]={{"Rooms","Salas","Salas"},{"Friends","Amigos","Amigos"}};

enum Mk8MenuGroup { MENU_QUICK, MENU_VIDEO, MENU_AUDIO, MENU_CONTROLS, MENU_ACCOUNT, MENU_MODS, MENU_QUIT, MENU_GROUP_COUNT };
static const Mk8MenuLabel mk8_menu_groups[MENU_GROUP_COUNT] = {
    {"Quick settings","Rápidos","Ajustes rápidos"}, {"Video","Vídeo","Vídeo"}, {"Audio","Áudio","Audio"},
    {"Controls","Controle","Controles"}, {"Account","Conta","Cuenta"}, {"Mods","Mods","Mods"}, {"Quit","Sair","Salir"}
};

static inline const char *mk8_menu_save_error(void) {
    return mk8_menu_text(mk8_menu_language(),
        "Settings could not be saved. Check the settings folder, then reopen the Menu and close it to retry.",
        "Não foi possível salvar os ajustes. Verifique a pasta de configurações e abra e feche o Menu para tentar novamente.",
        "No se pudieron guardar los ajustes. Comprueba la carpeta de configuración y vuelve a abrir y cerrar el Menú para reintentar.");
}
