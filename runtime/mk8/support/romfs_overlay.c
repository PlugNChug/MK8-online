// Pacote de pistas opcional (toolchain/ctgp_pack.py): a pasta romfs/ que o launcher monta, servida por cima do RomFS
// do jogo como a sobreposição do próprio jogo (runtime/mods.c: depois de RECOMP_MOD_DIR, que segue livre para a cabeça
// Mii). MK8_ROMFS_OVERLAY=<pasta>; vazio ou ausente: nenhum pacote. Uma pasta inválida encerra o jogo em vez de abrir
// o jogo base calado, que pareceria o pacote ativo.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_pack[1024];

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
    const char *settings = mk8_settings_romfs_overlay();
    if (!settings) return pack;
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
