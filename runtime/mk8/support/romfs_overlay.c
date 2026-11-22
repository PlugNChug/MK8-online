// Pacote de pistas opcional (toolchain/ctgp_pack.py): a pasta romfs/ que o launcher monta, servida por cima do RomFS
// do jogo como a sobreposição do próprio jogo (runtime/mods.c: depois de RECOMP_MOD_DIR, que segue livre para a cabeça
// Mii). MK8_ROMFS_OVERLAY=<pasta>; vazio ou ausente: nenhum pacote. Uma pasta inválida encerra o jogo em vez de abrir
// o jogo base calado, que pareceria o pacote ativo.
#include "rt.h"
#include <stdio.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_pack[1024];
static _Atomic bool g_controller_layout;
bool mk8_controller_layout_available(void) { return g_controller_layout; }
static _Atomic bool g_custom_ui;
bool mk8_manual_ui_layout(void) { return g_custom_ui; }

static bool has_common(const char *folder, const char *prefix) {
    if (!folder || !*folder) return false;
    char path[8192];
    int n=snprintf(path,sizeof path,"%s%s/UI/cmn/common.sarc",folder,prefix);
    struct stat st;
    return n<0 || (size_t)n>=sizeof path || (!stat(path,&st) && S_ISREG(st.st_mode));
}

const char *mk8_romfs_overlay_from(const char *value) {
    if (!value || !*value) return NULL;
    struct stat st;
    if (strlen(value) >= sizeof g_pack || strchr(value, '\n') || stat(value, &st) || !S_ISDIR(st.st_mode)) return "";
    snprintf(g_pack, sizeof g_pack, "%s", value);
    return g_pack;
}

static const char *game_layouts(bool custom_common) {
    const char *pack = mk8_romfs_overlay_from(getenv("MK8_ROMFS_OVERLAY"));
    if (pack && !*pack) FATAL("[mk8:pack] MK8_ROMFS_OVERLAY não é uma pasta válida\n");
    if (pack) LOG("[mk8:pack] %s\n", pack);
    extern const char *mk8_settings_romfs_overlay(void);
    const char *settings = mk8_settings_romfs_overlay();
    if (!settings) {
        // Keep a mod's own common.sarc intact. Older builds may lack the standalone layout.
        g_controller_layout=false;
        if (custom_common || has_common(pack,"") || has_common(getenv("RECOMP_MOD_DIR"),"/romfs")) return pack;
        static char roster[4096];
        extern bool plat_exe_path(char *,size_t);
        if (!plat_exe_path(roster,sizeof roster)) return pack;
        char *end=strrchr(roster,'/'), *back=strrchr(roster,'\\');
        if (!end || (back && back>end)) end=back;
        const char suffix[]="/controller-menu/romfs";
        if (!end || sizeof roster-(size_t)(end-roster)<sizeof suffix) return pack;
        memcpy(end,suffix,sizeof suffix);
        if (!has_common(roster,"")) return pack;
        g_controller_layout=true;
        if (!pack) return roster;
        static char with_pack[sizeof roster+sizeof g_pack+1];
        snprintf(with_pack,sizeof with_pack,"%s\n%s",roster,pack);
        return with_pack;
    }
    g_controller_layout=true;
    if (!pack) return settings;
    const char *combined = getenv("MK8_SETTINGS_ROMFS");
    if (!combined || !*combined)
        FATAL("[mk8:settings] start through the launcher to combine CTGP and Settings layouts\n");
    static char overlays[8192];
    int length = snprintf(overlays, sizeof overlays, "%s\n%s", pack, settings);
    if (length < 0 || (size_t)length >= sizeof overlays)
        FATAL("[mk8:settings] combined overlays too long\n");
    return overlays;
}

const char *game_romfs_overlay(void) {
    extern size_t usermods_overlay(char *,size_t);
    extern bool usermods_find(const char *,char *,size_t);
    extern bool mk8_nextendo_enabled(void);
    static char layers[16384 + 1100];  // Same capacity as the pinned mods_overlay and filesystem reader.
    const char *explicit_mod=getenv("RECOMP_MOD_DIR");
    size_t used=0;
    layers[0]=0;
    if ((!explicit_mod || !*explicit_mod) && !mk8_nextendo_enabled())
        used=usermods_overlay(layers,sizeof layers);
    if (used>=sizeof layers || layers[used]) FATAL("[mk8:mods] installed overlay paths too long\n");
    char file[1024];
    bool common=used && usermods_find("UI/cmn/common.sarc",file,sizeof file);
    g_custom_ui=common || (used && usermods_find("UI/cmn/menu.sarc",file,sizeof file));
    const char *game=game_layouts(common);
    if (!used) return game;
    if (game && *game) {
        size_t n=strlen(game);
        if (n+1>=sizeof layers-used) FATAL("[mk8:mods] combined overlay paths too long\n");
        memmove(layers+n+1,layers,used+1);
        memcpy(layers,game,n);
        layers[n]='\n';
    }
    return layers;
}
