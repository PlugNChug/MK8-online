// Copas do MK8 4.0.1 numa área maior (toolchain/cup_relocation.py; fase B do CTGP, docs/STATUS.md).
//
// O jogo guarda as copas em entradas de 0x80 bytes no começo de um objeto com lugar para 25 (12 do jogo base, 12 do
// DLC e a de batalha; f_70003f96b0 as monta). Ao fim dessa montagem, mk8_cups_built copia as 25 entradas para cá, onde
// há lugar para MK8_CUPS_CAPACITY; os pontos que leem as copas (índice limitado a 25) passam por mk8_cup_entry, que
// devolve esta área para esse objeto. O objeto original fica intacto (o mapa reverso em +0xC80 e a contagem em
// +0x1430 continuam lá). MK8_CUPS_RELOCATE=0: desligado, com o limite original de 25.
//
// Copas acrescentadas (MK8_CUPS_EXTRA) reusam os IDs de pistas que já estão numa copa de corrida: as tabelas por pista
// do jogo só existem para elas (posição = ID - 16 ou ID - 27; os IDs reservados dariam posições negativas). Cada pista
// acrescentada tem a sua pasta: quando a corrida sai de uma copa acrescentada (os leitores que copiam a pista escolhida
// para a seleção avisam por mk8_cup_select), os montadores de caminho Course/%s e Course/%s/%s (f_700089ed04,
// f_700089ec04) pedem o nome a mk8_course_folder, que devolve a pasta dela no lugar do nome da pista original.
#include "rt.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MK8_CUPS_ORIGINAL 25
#define MK8_CUPS_CAPACITY 49  // the game's 25 + two pages of 12 (CTGP Deluxe's 16 cups and 8 empty places)
#define MK8_CUPS_ADDED (MK8_CUPS_CAPACITY - MK8_CUPS_ORIGINAL)
#define MK8_CUP_BYTES 0x80
#define MK8_CUPS_REVERSE 0xC80  // 123 x {u32 cup, u32 slot} by course ID; -1 when no cup has the course
#define MK8_RACE_CUPS 24        // (the 25th entry is the battle cup)
#define MK8_COURSE_IDS 123
#define MK8_SETTINGS_COURSE 0x1a0  // course ID in the race settings the path builders take
#define MK8_FOLDER_MAX 32

static _Alignas(64) uint8_t g_cups[MK8_CUPS_CAPACITY * MK8_CUP_BYTES];
static uint64_t g_cups_object;
static int g_cups_on = -1, g_cups_added, g_cups_preview = -1, g_cups_selected = -1;
static int16_t g_ids[MK8_CUPS_ADDED][4];
static int32_t g_cup_labels[MK8_CUPS_ADDED];  // (-1: none given)
static char g_folders[MK8_CUPS_ADDED][4][MK8_FOLDER_MAX];

static int cups_enabled(void) {
    if (g_cups_on < 0) {
        const char *e = getenv("MK8_CUPS_RELOCATE");
        g_cups_on = !(e && e[0] == '0' && !e[1]);
    }
    return g_cups_on;
}

// An empty cup: 8 course records {-1, -1, -1, 0}, as the game's own unused records.
static void cups_clear(uint8_t *entry) {
    for (int k = 0; k < 8; k++) {
        memset(entry + 16 * k, 0xff, 12);
        memset(entry + 16 * k + 12, 0, 4);
    }
}

// MK8_CUPS_EXTRA: added cups, `;` between cups and `,` between their 4 courses, each `<course ID>:<name label>:<folder>`
// (e.g. "30000=27:31000:CTGP01,28:31001:CTGP02,19:31002:CTGP03,17:31003:CTGP04"); a cup may start with its own name
// label and `=`. The course ID must be in one of the game's race cups (its tables exist) and appear once per added
// cup; the folder is Course/<folder> in the RomFS overlay; a course's name label n has its race-intro text at n + 100,
// as the game's. Returns the number of cups, or -1 (and nothing usable) for anything else.
int mk8_cups_parse(const char *spec, uint8_t *entries, int capacity, const uint8_t *reverse, int16_t ids[][4],
                   char folders[][4][MK8_FOLDER_MAX], int32_t labels[]) {
    int cups = 0, slot = 0;
    const char *p = spec;
    if (!p || !*p) return 0;
    cups_clear(entries);
    while (*p) {
        char *end;
        if (slot == 0) {
            labels[cups] = -1;
            unsigned long cup = strtoul(p, &end, 10);
            if (end != p && *end == '=') {
                if (cup == 0 || cup > 0x7fffffff) return -1;
                labels[cups] = (int32_t)cup;
                p = end + 1;
            }
        }
        unsigned long id = strtoul(p, &end, 10);
        if (end == p || *end != ':' || id >= MK8_COURSE_IDS || slot >= 4) return -1;
        p = end + 1;
        unsigned long label = strtoul(p, &end, 10);
        if (end == p || *end != ':' || label == 0 || label > 0x7fffffff) return -1;
        p = end + 1;
        int n = 0;
        while ((p[n] >= 'A' && p[n] <= 'Z') || (p[n] >= 'a' && p[n] <= 'z') || (p[n] >= '0' && p[n] <= '9') || p[n] == '_')
            n++;
        if (n == 0 || n >= MK8_FOLDER_MAX) return -1;
        uint32_t cup;
        memcpy(&cup, reverse + id * 8, 4);
        if (cup >= MK8_RACE_CUPS) return -1;  // (no cup, or the battle cup's)
        for (int s = 0; s < slot; s++)
            if (ids[cups][s] == (int16_t)id) return -1;
        uint8_t *record = entries + cups * MK8_CUP_BYTES + 16 * slot;
        uint32_t words[3] = {(uint32_t)id, 0xffffffffu, (uint32_t)label};
        memcpy(record, words, 12);
        record[12] = 0;
        memcpy(folders[cups][slot], p, n);
        folders[cups][slot][n] = 0;
        ids[cups][slot++] = (int16_t)id;
        p += n;
        if (*p == ',') {
            p++;
        } else if (*p == ';' || !*p) {
            if (slot != 4) return -1;
            cups++;
            slot = 0;
            if (*p) {
                p++;
                if (cups >= capacity) return -1;
                cups_clear(entries + cups * MK8_CUP_BYTES);
            }
        } else {
            return -1;
        }
    }
    return cups;
}

// At the end of the cup builder (f_70003f96b0): its object's 25 entries, the added cups after them and empty ones.
// The game's reverse map (course ID -> cup, position) and its count stay as they are: each course keeps its own cup.
void mk8_cups_built(uint64_t object) {
    if (!cups_enabled() || !object) return;
    memcpy(g_cups, (const void *)(uintptr_t)object, MK8_CUPS_ORIGINAL * MK8_CUP_BYTES);
    for (int i = MK8_CUPS_ORIGINAL; i < MK8_CUPS_CAPACITY; i++) cups_clear(g_cups + i * MK8_CUP_BYTES);
    const char *spec = getenv("MK8_CUPS_EXTRA");
    int added = mk8_cups_parse(spec, g_cups + MK8_CUPS_ORIGINAL * MK8_CUP_BYTES, MK8_CUPS_ADDED,
                               (const uint8_t *)(uintptr_t)(object + MK8_CUPS_REVERSE), g_ids, g_folders, g_cup_labels);
    if (added < 0) FATAL("[mk8:cups] MK8_CUPS_EXTRA inválido: %s\n", spec);
    if (g_cups_object != object || g_cups_added != added) {
        LOG("[mk8:cups] %d entradas + %d copa(s) acrescentada(s) em área de %d (objeto %llx)\n", MK8_CUPS_ORIGINAL,
            added, MK8_CUPS_CAPACITY, (unsigned long long)object);
    }
    g_cups_object = object;
    g_cups_added = added;
    const char *preview = getenv("MK8_CUPS_PREVIEW");
    g_cups_preview = (preview && added > 0) ? atoi(preview) : -1;
    if (g_cups_preview >= MK8_RACE_CUPS) g_cups_preview = -1;
}

static uint64_t cup_index(uint64_t base, uint64_t index) {
    if (g_cups_object && base == g_cups_object && index < MK8_CUPS_CAPACITY && cups_enabled())
        return (int64_t)index == g_cups_preview ? MK8_CUPS_ORIGINAL : index;
    return UINT64_MAX;
}

// What the readers' `object + (index << 7)` became (their limit is MK8_CUPS_CAPACITY now): the relocated entry for the
// builder's object; for anything else, or with the relocation off, the original arithmetic with its limit of 25.
// MK8_CUPS_PREVIEW=<cup> (diagnostic, until the cup screen shows the added cups): that cup's entry is the first added.
uint64_t mk8_cup_entry(uint64_t base, uint64_t index) {
    uint64_t at = cup_index(base, index);
    if (at != UINT64_MAX) return (uint64_t)(uintptr_t)(g_cups + at * MK8_CUP_BYTES);
    if (index >= MK8_CUPS_ORIGINAL) index = 0;
    return base + (index << 7);
}

// Which races take an added cup's course folders ("armed"): only those set up by choosing that cup. A choice from
// the cup screen (f_70004980a0, f_7000498e90) arms the next race, whichever of the cup's courses it is, and its
// restarts (the same course again); a Grand Prix from it (f_70004fb290) arms its four races in the cup's order. A race
// of any other course disarms: one whose course comes from elsewhere (VS with random courses calls none of these
// readers, traced 16/11/2026) plays the game's own, even on a course ID an added cup borrows. Menu courses (IDs below
// 16, Gu_Menu) are not races and change nothing.
static int g_armed_cup = -1, g_armed_gp, g_armed_slot = -1;

static void cup_chosen(uint64_t at, int gp) {
    if (at == UINT64_MAX) return;
    if ((int)at != g_cups_selected) {
        if (at >= MK8_CUPS_ORIGINAL) LOG("[mk8:cups] copa acrescentada %d escolhida\n", (int)at - MK8_CUPS_ORIGINAL + 1);
        else if (g_cups_selected >= MK8_CUPS_ORIGINAL && g_cups_added > 0) LOG("[mk8:cups] copa %d escolhida\n", (int)at);
        g_cups_selected = (int)at;
    }
    int cup = (int)at - MK8_CUPS_ORIGINAL;
    g_armed_cup = cup >= 0 && cup < g_cups_added ? cup : -1;
    g_armed_gp = gp;
    g_armed_slot = -1;
}

// The readers that copy the chosen course into the selection (f_70004980a0, f_7000498e90): the same entry, and the
// cup it came from is the one the next race is from.
uint64_t mk8_cup_select(uint64_t base, uint64_t index) {
    cup_chosen(cup_index(base, index), 0);
    return mk8_cup_entry(base, index);
}

// The Grand Prix's reader (f_70004fb290: the cup's 4 courses into its list): its four races are from the cup.
uint64_t mk8_cup_select_gp(uint64_t base, uint64_t index) {
    cup_chosen(cup_index(base, index), 1);
    return mk8_cup_entry(base, index);
}

// ---- the cup select screen's pages. The game has two: 12 buttons with cups 0-11 (f_7000494b44) or 12-23
// (f_7000495564), and turning (f_70004966c8, bit 11 or 12 of the input) switches its page byte (+0x5a3) between them.
// With added cups the runtime keeps more: the page shown is base (0), DLC (1) or added (2, and 3 for cups 13-16),
// turning moves through all of them, and the button setups ask mk8_cup_page_id for each button's cup. Without added
// cups nothing changes.
static int g_cup_page = -1, g_cup_turn;

// From the navigation, after it switches the game's page: which way (bit 12 forward, bit 11 back).
void mk8_cup_page_turn(uint64_t input) {
    if (g_cups_added > 0) g_cup_turn = ((input >> 12) & 1) ? 1 : ((input >> 11) & 1) ? -1 : 1;
}

// From the button setups, for each button's cup (0-11 on the first page, 12-23 on the second): the page decided at
// the first button (after a turn, the next page that way; on opening, the page of the chosen cup if it is added).
uint64_t mk8_cup_page_id(uint64_t id) {
    if (g_cups_added <= 0 || id >= MK8_RACE_CUPS) return id;
    int slot = (int)(id % 12), game = (int)(id / 12), pages = 2 + (g_cups_added + 11) / 12;
    if (slot == 0) {
        if (g_cup_turn) {
            int from = g_cup_page >= 0 ? g_cup_page : !game;
            g_cup_page = (from + g_cup_turn + pages) % pages;
            g_cup_turn = 0;
        } else {
            g_cup_page = g_cups_selected >= MK8_CUPS_ORIGINAL ? 2 + (g_cups_selected - MK8_CUPS_ORIGINAL) / 12 : game;
        }
    }
    int page = g_cup_page >= 0 ? g_cup_page : game;
    return page >= 2 ? (uint64_t)(MK8_CUPS_ORIGINAL + (page - 2) * 12 + slot) : (uint64_t)(page * 12 + slot);
}

// From the per-cup DLC check (f_7000863428), which the cup state query (f_7000494e44) asks for the cups past the
// first 12: 0 is no restriction (what an owned DLC cup answers: the cup shows and can be chosen); anything else makes
// the query ask again, and its cup is shown with "???" (state 0 for 3, 2 otherwise). An added cup with courses has no
// restriction, an empty one has (1); -1 for the game's cups, which it answers itself (traced 15/11/2026).
int64_t mk8_cup_owned(uint64_t id) {
    if (g_cups_added <= 0 || id < MK8_CUPS_ORIGINAL || id >= MK8_CUPS_CAPACITY) return -1;
    return (int64_t)id - MK8_CUPS_ORIGINAL < g_cups_added ? 0 : 1;
}

// The reader that sets the selection from a course ID (f_70004a3940): it found the cup and position in the reverse
// map, always a game cup's, and will read the record at entry + position x 16. When the added cup chosen before has
// that course, it stays chosen and the record read is the added cup's (its name label, for one): the entry given back
// is offset so that the game's position lands on it. Otherwise the race is from the game cup (a random course, say).
uint64_t mk8_cup_by_course(uint64_t base, uint64_t index, uint64_t course) {
    uint64_t at = cup_index(base, index);
    int cup = g_cups_selected - MK8_CUPS_ORIGINAL;
    if (at != UINT64_MAX && at < MK8_CUPS_ORIGINAL && cup >= 0 && cup < g_cups_added && course < MK8_COURSE_IDS) {
        uint32_t position;
        memcpy(&position, (const void *)(uintptr_t)(base + MK8_CUPS_REVERSE + course * 8 + 4), 4);
        for (int s = 0; s < 4 && position < 8; s++)
            if (g_ids[cup][s] == (int32_t)course)
                return (uint64_t)(uintptr_t)(g_cups + (MK8_CUPS_ORIGINAL + cup) * MK8_CUP_BYTES + 16 * s) - 16 * position;
    }
    return mk8_cup_select(base, index);
}

// From the cup name lookup (f_700041fd6c: the game's table of 24 cup name labels, read without a limit): an added
// cup's own label; -1 for the game's cups and for an added cup with none, which the game answers itself.
int64_t mk8_cup_label(uint64_t id) {
    if (g_cups_added <= 0 || id < MK8_CUPS_ORIGINAL || id >= (uint64_t)(MK8_CUPS_ORIGINAL + g_cups_added)) return -1;
    return g_cup_labels[id - MK8_CUPS_ORIGINAL];
}

// The readers that find a course's cup and position in the reverse map to show it (the race's course name, for one):
// while an added cup is chosen, the game cup they read comes with that added cup's records (their name labels) in
// the places of the courses it borrows from it; any other cup, or no added cup chosen, as it is.
static _Alignas(16) uint8_t g_cup_view[MK8_CUPS_ORIGINAL][MK8_CUP_BYTES];

uint64_t mk8_cup_by_slot(uint64_t base, uint64_t index) {
    uint64_t at = cup_index(base, index);
    int cup = g_cups_selected - MK8_CUPS_ORIGINAL;
    if (at != UINT64_MAX && at < MK8_CUPS_ORIGINAL && cup >= 0 && cup < g_cups_added) {
        const uint8_t *game = g_cups + at * MK8_CUP_BYTES, *added = g_cups + (MK8_CUPS_ORIGINAL + cup) * MK8_CUP_BYTES;
        int changed = 0;
        memcpy(g_cup_view[at], game, MK8_CUP_BYTES);
        for (int k = 0; k < 8; k++) {
            int32_t id;
            memcpy(&id, game + 16 * k, 4);
            for (int s = 0; s < 4 && id >= 0; s++)
                if (g_ids[cup][s] == id) {
                    memcpy(g_cup_view[at] + 16 * k, added + 16 * s, 16);
                    changed = 1;
                }
        }
        if (changed) return (uint64_t)(uintptr_t)g_cup_view[at];
    }
    return mk8_cup_entry(base, index);
}

static const char *g_race_folder;  // (the race's course folder when it is an added cup's course)

// From the path builders, with the name they chose for the race's course: the course's folder when the race is from
// an added cup that has it, the name unchanged otherwise.
uint64_t mk8_course_folder(uint64_t settings, uint64_t name) {
    g_race_folder = NULL;
    if (!settings) return name;
    uint32_t id;
    memcpy(&id, (const void *)(uintptr_t)(settings + MK8_SETTINGS_COURSE), 4);
    if (id < 16 || id >= MK8_COURSE_IDS) return name;  // (the menu's course: not a race)
    int cup = g_armed_cup;
    if (cup < 0 || cup >= g_cups_added) return name;
    for (int s = 0; s < 4; s++) {
        if (g_ids[cup][s] != (int32_t)id) continue;
        // (the armed race and its restarts; in a Grand Prix the next of its courses as well)
        if (g_armed_slot >= 0 && s != g_armed_slot && !(g_armed_gp && s == g_armed_slot + 1)) break;
        g_armed_slot = s;
        static int logged[MK8_CUPS_ADDED][4];
        if (!logged[cup][s]) {
            logged[cup][s] = 1;
            LOG("[mk8:cups] pista %u da copa acrescentada %d: Course/%s\n", id, cup + 1, g_folders[cup][s]);
        }
        g_race_folder = g_folders[cup][s];
        return (uint64_t)(uintptr_t)g_folders[cup][s];
    }
    if (g_armed_cup >= 0) LOG("[mk8:cups] corrida da pista %u fora da copa acrescentada %d: pasta do jogo\n", id, cup + 1);
    g_armed_cup = -1;  // (any other course: the races after it are not the cup's)
    return name;
}

// From nn::fs::OpenFile (sdk f_7004050080), with the path the game opens: during a race of an added cup's course, its
// course music (the sound system's "content://Audio/Resource/Stream/pBGM_<course>_N.bfstm", and _F for the last lap)
// is the course folder's own, <folder>_N.bfstm / _F.bfstm in the overlay's Audio/Resource/Stream, when it is there;
// any other path, or no such file, unchanged. (The overlay is in the RomFS index from the start; the guest's RomFS
// reads the file with its own size.)
#define MK8_STREAM_DIR "content://Audio/Resource/Stream/"

uint64_t mk8_stream_path(uint64_t path) {
    const char *folder = g_race_folder, *p = (const char *)(uintptr_t)path;
    if (!folder || !p) return path;
    size_t n = strnlen(p, 256), dir = sizeof MK8_STREAM_DIR - 1;
    if (n >= 256 || n < dir + 5 + 8 || strncmp(p, MK8_STREAM_DIR, dir) || strncmp(p + dir, "pBGM_", 5)) return path;
    const char *tail = p + n - 8;  // "_N.bfstm" or "_F.bfstm"
    if (strcmp(tail, "_N.bfstm") && strcmp(tail, "_F.bfstm")) return path;
    static char out[2][128];
    char *buf = out[tail[1] == 'F'];
    const char *overlay = getenv("MK8_ROMFS_OVERLAY");
    char host[1100];
    struct stat st;
    if (!overlay || snprintf(host, sizeof host, "%s/Audio/Resource/Stream/%s%s", overlay, folder, tail) >= (int)sizeof host
        || stat(host, &st) || !S_ISREG(st.st_mode))
        return path;
    snprintf(buf, sizeof out[0], "%s%s%s", MK8_STREAM_DIR, folder, tail);
    static const char *logged[2];
    if (logged[tail[1] == 'F'] != folder) {
        logged[tail[1] == 'F'] = folder;
        LOG("[mk8:cups] música %s -> %s\n", p + dir, buf + dir);
    }
    return (uint64_t)(uintptr_t)buf;
}

// ---- the course pictures (UI/cmn/menu/ym_CoursePict_<suffix>^u.bntx). f_7000418ed0(names, &record, load) builds the
// file name from a course ID (the suffix: names + 0x80, index ID + 1): with load = 1 it loads the picture (the menu
// loads every course's at once, f_7000418e3c: IDs 16-63, 67-122 and -1, Random), with 0 it finds the loaded one to
// show (given a copy of the course ID). The runtime adds the added courses' own pictures, <folder>_00, those the
// overlay has: the menu loads them after its own under IDs from 123 on, and the screens that show an added course
// ask for its picture's ID instead (the cup panel by the record it shows, the time trial screen by the chosen cup).
#define MK8_PICTURE_FIRST 0x7b
static int g_pictures = -1;
static int16_t g_picture_course[MK8_CUPS_ADDED * 4];  // (cup * 4 + slot)
static char g_picture_names[MK8_CUPS_ADDED * 4][MK8_FOLDER_MAX + 4];

static void pictures_find(void) {
    const char *overlay = getenv("MK8_ROMFS_OVERLAY");
    char host[1200];
    struct stat st;
    g_pictures = 0;
    for (int cup = 0; overlay && cup < g_cups_added; cup++)
        for (int s = 0; s < 4; s++) {
            if (snprintf(host, sizeof host, "%s/UI/cmn/menu/ym_CoursePict_%s_00^u.bntx", overlay, g_folders[cup][s])
                    >= (int)sizeof host || stat(host, &st) || !S_ISREG(st.st_mode))
                continue;
            g_picture_course[g_pictures] = (int16_t)(cup * 4 + s);
            snprintf(g_picture_names[g_pictures], sizeof g_picture_names[0], "%s_00", g_folders[cup][s]);
            g_pictures++;
        }
    if (g_pictures) LOG("[mk8:cups] %d imagem(ns) de pista acrescentada(s)\n", g_pictures);
}

// From the menu's picture loading, after the game's own: how many more to load (IDs MK8_PICTURE_FIRST on).
int64_t mk8_course_pictures(void) {
    if (g_pictures < 0) pictures_find();
    return g_pictures;
}

static int picture_of(int course) {  // (cup * 4 + slot -> the picture's index, -1 without one)
    for (int k = 0; k < g_pictures; k++)
        if (g_picture_course[k] == course) return k;
    return -1;
}

// From f_7000418ed0, with the course ID it was given (load or find) and the suffix it chose: an added picture's own
// for its ID; the game's otherwise.
uint64_t mk8_course_picture(uint64_t course, uint64_t load, uint64_t name) {
    (void)load;
    if (g_pictures < 0) pictures_find();
    if (g_pictures <= 0 || !course) return name;
    uint32_t id;
    memcpy(&id, (const void *)(uintptr_t)course, 4);
    if (id >= MK8_PICTURE_FIRST && id < (uint32_t)(MK8_PICTURE_FIRST + g_pictures)) {
        static uint8_t logged[MK8_CUPS_ADDED * 4][2];
        if (!logged[id - MK8_PICTURE_FIRST][load & 1]++)
            LOG("[mk8:cups] imagem %u (%s): %s\n", id, (load & 1) ? "carregar" : "achar", g_picture_names[id - MK8_PICTURE_FIRST]);
        return (uint64_t)(uintptr_t)g_picture_names[id - MK8_PICTURE_FIRST];
    }
    return name;
}

// The cup panel (f_700043c2e8: panel, record): at its entry the record it is set to; where it asks for the picture,
// the added picture's ID when that record is an added cup's course that has one.
static struct { uint64_t panel; int32_t picture; } g_panels[16];

void mk8_panel_record(uint64_t panel, uint64_t record) {
    if (g_pictures < 0) pictures_find();
    int32_t picture = -1;
    const uint8_t *added = g_cups + MK8_CUPS_ORIGINAL * MK8_CUP_BYTES, *r = (const uint8_t *)(uintptr_t)record;
    if (g_pictures > 0 && r >= added && r < added + g_cups_added * MK8_CUP_BYTES) {
        size_t at = (size_t)(r - added);
        int k = at % 16 == 0 && (at % MK8_CUP_BYTES) / 16 < 4
                    ? picture_of((int)(at / MK8_CUP_BYTES) * 4 + (int)(at % MK8_CUP_BYTES) / 16) : -1;
        if (k >= 0) picture = MK8_PICTURE_FIRST + k;
    }
    int free = -1;
    for (int i = 0; i < 16; i++) {
        if (g_panels[i].panel == panel) {
            g_panels[i].picture = picture;
            return;
        }
        if (!g_panels[i].panel && free < 0) free = i;
    }
    if (picture < 0) return;
    if (free < 0) free = (int)((panel >> 4) & 15);
    g_panels[free].panel = panel;
    g_panels[free].picture = picture;
}

uint64_t mk8_panel_picture(uint64_t panel, uint64_t id) {
    for (int i = 0; i < 16; i++)
        if (g_panels[i].panel == panel) return g_panels[i].picture >= 0 ? (uint64_t)g_panels[i].picture : id;
    return id;
}

// The time trial screen (f_7000446068) asks for the picture of the race's course: the added course's own when the
// chosen cup is added and has it.
uint64_t mk8_course_picture_id(uint64_t id) {
    if (g_pictures < 0) pictures_find();
    int cup = g_cups_selected - MK8_CUPS_ORIGINAL;
    for (int s = 0; s < 4 && cup >= 0 && cup < g_cups_added; s++)
        if (g_ids[cup][s] == (int32_t)(uint32_t)id) {
            int k = picture_of(cup * 4 + s);
            return k >= 0 ? (uint64_t)(MK8_PICTURE_FIRST + k) : id;
        }
    return id;
}

// The menu's texture manager (f_7000405f74 makes it with room for 150 textures: the game's 105 course pictures and
// the rest of the menu's) gets room for the added courses' pictures as well: with 150, CTGP Deluxe's last 16 did not
// fit and the panel kept showing the ones before (traced 16/11/2026: its count reached 150 of 150). It is made before
// the cups are built, so the places come from MK8_CUPS_EXTRA itself: one per course listed, none without added cups.
int64_t mk8_course_picture_places(void) {
    const char *spec = getenv("MK8_CUPS_EXTRA");
    int64_t colons = 0;
    if (!cups_enabled() || !spec || !*spec) return 0;
    for (const char *p = spec; *p; p++) colons += *p == ':';
    int64_t courses = colons / 2;  // (two ':' per course)
    return courses > MK8_CUPS_ADDED * 4 ? MK8_CUPS_ADDED * 4 : courses;
}
