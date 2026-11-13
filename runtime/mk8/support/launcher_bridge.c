// The Rust launcher owns import, builds and updates.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

bool launcher_needed(const char *root, bool asked) { (void)root; return asked; }
void launcher_run(const char *root) {
    (void)root;
    fprintf(stderr, "[launcher] Returning to MK8-online.\n");
    exit(0);
}
void launcher_boot_env(void) {}
void launcher_update_check_start(void) {}
const char *launcher_update_tag(void) { return NULL; }
bool launcher_settings_rows(void) { return false; }
bool launcher_has_settings(void) { return false; }

bool mk8_room_menu_available(void) { return getenv("MK8_ROOM_MENU_SIGNAL") != NULL; }
bool mk8_mii_menu_available(void) { return getenv("MK8_MII_MENU_SIGNAL") != NULL; }
void mk8_open_mii_menu(void) {
#ifdef _WIN32
    const wchar_t *path = _wgetenv(L"MK8_MII_MENU_SIGNAL");
    FILE *signal = path && *path ? _wfopen(path, L"wb") : NULL;
#else
    const char *path = getenv("MK8_MII_MENU_SIGNAL");
    FILE *signal = path && *path ? fopen(path, "wb") : NULL;
#endif
    if (signal) fclose(signal);
    else fprintf(stderr, "[launcher] Could not open Mii editor.\n");
}
void mk8_open_room_menu(void) {
#ifdef _WIN32
    const wchar_t *path = _wgetenv(L"MK8_ROOM_MENU_SIGNAL");
    if (!path || !*path) return;
    FILE *signal = _wfopen(path, L"wb");
#else
    const char *path = getenv("MK8_ROOM_MENU_SIGNAL");
    if (!path || !*path) return;
    FILE *signal = fopen(path, "wb");
#endif
    if (signal) fclose(signal);
    else fprintf(stderr, "[launcher] Could not open room menu.\n");
}
