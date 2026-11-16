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
#include <stdlib.h>
#include <string.h>

#define MK8_CUPS_ORIGINAL 25
#define MK8_CUPS_CAPACITY 41  // the game's 25 + 16 (CTGP Deluxe's cups)
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
// (e.g. "27:1401:CTGP01,28:1402:CTGP02,19:1403:CTGP03,17:1404:CTGP04"). The course ID must be in one of the game's
// race cups (its tables exist) and appear once per added cup; the folder is Course/<folder> in the RomFS overlay.
// Returns the number of cups, or -1 (and nothing usable) for anything else.
int mk8_cups_parse(const char *spec, uint8_t *entries, int capacity, const uint8_t *reverse, int16_t ids[][4],
                   char folders[][4][MK8_FOLDER_MAX]) {
    int cups = 0, slot = 0;
    const char *p = spec;
    if (!p || !*p) return 0;
    cups_clear(entries);
    while (*p) {
        char *end;
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
                               (const uint8_t *)(uintptr_t)(object + MK8_CUPS_REVERSE), g_ids, g_folders);
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

// The readers that copy the chosen course into the selection (f_70004980a0, f_7000498e90, f_70004a3940) or a Grand
// Prix's 4 courses into its list (f_70004fb290): the same
// entry, and the cup it came from is the one the next race is from.
uint64_t mk8_cup_select(uint64_t base, uint64_t index) {
    uint64_t at = cup_index(base, index);
    if (at != UINT64_MAX && (int)at != g_cups_selected) {
        g_cups_selected = (int)at;
        if (at >= MK8_CUPS_ORIGINAL) LOG("[mk8:cups] copa acrescentada %d escolhida\n", (int)at - MK8_CUPS_ORIGINAL + 1);
    }
    return mk8_cup_entry(base, index);
}

// From the path builders, with the name they chose for the race's course: the course's folder when the race is from
// an added cup that has it, the name unchanged otherwise.
uint64_t mk8_course_folder(uint64_t settings, uint64_t name) {
    int cup = g_cups_selected - MK8_CUPS_ORIGINAL;
    if (cup < 0 || cup >= g_cups_added || !settings) return name;
    uint32_t id;
    memcpy(&id, (const void *)(uintptr_t)(settings + MK8_SETTINGS_COURSE), 4);
    for (int s = 0; s < 4; s++) {
        if (g_ids[cup][s] != (int32_t)id) continue;
        static int logged[MK8_CUPS_ADDED][4];
        if (!logged[cup][s]) {
            logged[cup][s] = 1;
            LOG("[mk8:cups] pista %u da copa acrescentada %d: Course/%s\n", id, cup + 1, g_folders[cup][s]);
        }
        return (uint64_t)(uintptr_t)g_folders[cup][s];
    }
    return name;
}
