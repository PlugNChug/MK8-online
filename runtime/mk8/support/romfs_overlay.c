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
static _Atomic enum { LAYOUT_PENDING, LAYOUT_PRESENT, LAYOUT_MISSING } g_settings_layout;
bool mk8_settings_layout_available(void) { return g_settings_layout==LAYOUT_PRESENT; }
bool mk8_settings_layout_unavailable(void) { return g_settings_layout==LAYOUT_MISSING; }
bool mk8_controller_layout_available(void) { return g_controller_layout; }

static bool has_layout(const char *folder, const char *prefix, const char *file) {
    if (!folder || !*folder) return false;
    char path[8192];
    int n=snprintf(path,sizeof path,"%s%s/UI/cmn/%s.sarc",folder,prefix,file);
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

const char *game_romfs_overlay(void) {
    // (called once, from runtime/mods.c overlay_once under pthread_once)
    const char *pack = mk8_romfs_overlay_from(getenv("MK8_ROMFS_OVERLAY"));
    if (pack && !*pack) FATAL("[mk8:pack] MK8_ROMFS_OVERLAY não é uma pasta válida\n");
    if (pack) LOG("[mk8:pack] %s\n", pack);
    extern const char *mk8_settings_romfs_overlay(void);
    const char *mod=getenv("RECOMP_MOD_DIR"), *combined=getenv("MK8_SETTINGS_ROMFS");
    bool custom_common=has_layout(mod,"/romfs","common") || has_layout(pack,"","common");
    bool custom_menu=has_layout(mod,"/romfs","menu") || has_layout(pack,"","menu");
    bool conflict=has_layout(mod,"/romfs","common") || has_layout(mod,"/romfs","menu") ||
                  ((custom_common || custom_menu) && !(combined && *combined));
    const char *settings = conflict ? NULL : mk8_settings_romfs_overlay();
    g_settings_layout=settings ? LAYOUT_PRESENT : LAYOUT_MISSING;
    if (!settings) {
        // Keep a mod's own common.sarc intact. Older builds may lack the standalone layout.
        g_controller_layout=false;
        if (custom_common) return pack;
        static char roster[4096];
        extern bool plat_exe_path(char *,size_t);
        if (!plat_exe_path(roster,sizeof roster)) return pack;
        char *end=strrchr(roster,'/'), *back=strrchr(roster,'\\');
        if (!end || (back && back>end)) end=back;
        const char suffix[]="/controller-menu/romfs";
        if (!end || sizeof roster-(size_t)(end-roster)<sizeof suffix) return pack;
        memcpy(end,suffix,sizeof suffix);
        if (!has_layout(roster,"","common")) return pack;
        g_controller_layout=true;
        if (!pack) return roster;
        static char with_pack[sizeof roster+sizeof g_pack+1];
        snprintf(with_pack,sizeof with_pack,"%s\n%s",roster,pack);
        return with_pack;
    }
    g_controller_layout=true;
    if (!pack) return settings;

    static char overlays[8192];
    int length = snprintf(overlays, sizeof overlays, "%s\n%s", pack, settings);
    if (length < 0 || (size_t)length >= sizeof overlays)
        FATAL("[mk8:settings] combined overlays too long\n");
    return overlays;
}
