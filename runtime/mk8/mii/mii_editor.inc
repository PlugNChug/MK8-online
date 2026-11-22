// Minimal single-profile editor, without system Mii models. Applet ABI: libnx mii_la.h.
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <SDL3/SDL_stdinc.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

bool mk8_random(void *buffer, size_t size);

static bool mk8_mii_save_name(const char *name, uint8_t info[0x58]) {
    const char *file = getenv("MK8_MII_PROFILE_PATH");
    if (!file || !*file) return false;
#ifdef _WIN32
    wchar_t wide[12], path[2048], pending[2060];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, 12)) return false;
    size_t n = wcslen(wide);
    if (!n || n > 10) return false;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, file, -1, path, 2048)) return false;
    if (swprintf(pending, 2060, L"%ls.pending", path) < 0) return false;
#else
    uint32_t wide[10];
    size_t n = 0;
    const char *next = name;
    while (*next) {
        if (n == sizeof wide / sizeof wide[0]) return false;
        const char *before = next;
        uint32_t c = SDL_StepUTF8(&next, NULL);
        if (c == 0xfffd && next - before != 3) return false;
        wide[n++] = c;
    }
    char pending[4096];
    int written = snprintf(pending, sizeof pending, "%s.pending", file);
    if (written < 0 || (size_t)written >= sizeof pending) return false;
#endif
    bool nonspace = false;
    for (size_t i = 0; i < n; i++) {
        uint32_t c = wide[i];
        if (c < 0x20 || (c >= 0x7f && c <= 0x9f) || c > 0xffff ||
            (c >= 0xd800 && c <= 0xdfff)) return false;
        if (!iswspace((wint_t)c)) nonspace = true;
    }
    if (!nonspace) return false;
    uint8_t old[0x58];
    uint8_t face[9];
    if (!mk8_face_parameters(face)) return false;
    bool existing = mk8_profile_charinfo(old);
    mii_default_charinfo(info, name);
    if (existing) {
        memcpy(info, old, 16);
        memcpy(info + 0x26, old + 0x26, 0x58 - 0x26);
    } else {
        if (!mk8_random(info, 16)) return false;
        info[8] = (info[8] & 0x3f) | 0x80;
        mk8_face_charinfo(info, face);
    }
#ifdef _WIN32
    FILE *f = _wfopen(pending, L"wb");
#else
    FILE *f = fopen(pending, "wb");
#endif
    if (!f) return false;
    bool ok = fprintf(f, "{\"schema\":1,\"id\":[") > 0;
    for (int i = 0; i < 16; i++) ok &= fprintf(f, "%s%u", i ? "," : "", info[i]) > 0;
    ok &= fprintf(f, "],\"appearance\":{\"name\":\"") > 0;
    for (size_t i = 0; i < n; i++) ok &= fprintf(f, "\\u%04x", (unsigned)wide[i]) > 0;
    ok &= fprintf(f, "\",\"color\":%u,\"gender\":%u,\"face\":{\"skin\":%u,\"shape\":%u,\"hair\":%u,\"hair_color\":%u,\"eyes\":%u,\"eye_color\":%u,\"glasses\":%s,\"height\":%u,\"build\":%u}}}\n",
        info[0x27],info[0x28],face[0],face[1],face[2],face[3],face[4],face[5],face[6]?"true":"false",face[7],face[8]) > 0;
#ifdef _WIN32
    if (fflush(f) || _commit(_fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    if (ok) ok = MoveFileExW(pending, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (fflush(f) || fsync(fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    if (ok) ok = rename(pending, file) == 0;
#endif
    if (!ok) return false; // Preserve the prior profile on any write/replace failure.
    memcpy(mk8_edited_mii, info, sizeof mk8_edited_mii);
    mk8_edited_mii_valid = mk8_mii_updated = true;
    return true;
}

size_t mk8_mii_editor(uint32_t mode, uint8_t out[0x80]) {
    extern int menu_keyboard_run(const char *, int, char *, int);
    extern void menu_keyboard_title(const char *);
    extern void menu_notice(const char *, double);
    memset(out, 0, 0x80);
    out[0] = 1; // Cancel unless a valid profile is durably saved.
    size_t size = mode == 4 || mode == 5 ? 0x80 : 0x20;
    // The launcher owns complete profiles. Never overwrite imported CharInfo with this legacy name-only editor.
    if (getenv("MK8_MII_MENU_SIGNAL")) {
        extern void mk8_open_mii_menu(void);
        mk8_open_mii_menu();
        return size; // Cancel this applet; profile changes take effect on next launch.
    }
    if (mode != 0 && mode != 1 && mode != 4 && mode != 5) return size;
    char name[64] = "";
    const char *initial = getenv("MK8_MII_NAME");
    menu_keyboard_title("Nome do Mii (aparencia salva no launcher sera preservada)");
    if (!menu_keyboard_run(initial ? initial : "Player", 10, name, sizeof name)) return size;
    uint8_t info[0x58];
    if (!mk8_mii_save_name(name, info)) {
        menu_notice("Perfil nao salvo. Use 1 a 10 caracteres e verifique a pasta do launcher.", 10);
        return size;
    }
    if (mode == 4 || mode == 5) memcpy(out + 4, info, sizeof info);
    out[0] = 0;
    LOG("[mk8:mii-editor] profile saved; mode=%u output=%zu\n", mode, size);
    return size;
}
