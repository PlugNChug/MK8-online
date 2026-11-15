// Copas do MK8 4.0.1 numa área maior (toolchain/cup_relocation.py; fase B do CTGP, docs/STATUS.md).
//
// O jogo guarda as copas em entradas de 0x80 bytes no começo de um objeto com lugar para 25 (12 do jogo base, 12 do
// DLC e a de batalha; f_70003f96b0 as monta). Ao fim dessa montagem, mk8_cups_built copia as 25 entradas para cá, onde
// há lugar para MK8_CUPS_CAPACITY; os pontos que leem as copas (índice limitado a 25) passam por mk8_cup_entry, que
// devolve esta área para esse objeto. As entradas novas começam vazias (registros -1, como as vagas sem pista do
// jogo): nada muda no jogo até que copas sejam acrescentadas. O objeto original fica intacto (o mapa reverso em +0xC80
// e a contagem em +0x1430 continuam lá). MK8_CUPS_RELOCATE=0: desligado, com o limite original de 25.
#include "rt.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MK8_CUPS_ORIGINAL 25
#define MK8_CUPS_CAPACITY 41  // the game's 25 + 16 (CTGP Deluxe's cups)
#define MK8_CUP_BYTES 0x80

static _Alignas(64) uint8_t g_cups[MK8_CUPS_CAPACITY * MK8_CUP_BYTES];
static uint64_t g_cups_object;
static int g_cups_on = -1;

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

// At the end of the cup builder (f_70003f96b0): its object's 25 entries, and empty ones after them.
void mk8_cups_built(uint64_t object) {
    if (!cups_enabled() || !object) return;
    memcpy(g_cups, (const void *)(uintptr_t)object, MK8_CUPS_ORIGINAL * MK8_CUP_BYTES);
    for (int i = MK8_CUPS_ORIGINAL; i < MK8_CUPS_CAPACITY; i++) cups_clear(g_cups + i * MK8_CUP_BYTES);
    if (g_cups_object != object) LOG("[mk8:cups] %d entradas em área de %d (objeto %llx)\n", MK8_CUPS_ORIGINAL,
                                     MK8_CUPS_CAPACITY, (unsigned long long)object);
    g_cups_object = object;
}

// What the readers' `object + (index << 7)` became (their limit is MK8_CUPS_CAPACITY now): the relocated entry for the
// builder's object; for anything else, or with the relocation off, the original arithmetic with its limit of 25.
uint64_t mk8_cup_entry(uint64_t base, uint64_t index) {
    if (g_cups_object && base == g_cups_object && index < MK8_CUPS_CAPACITY && cups_enabled())
        return (uint64_t)(uintptr_t)(g_cups + index * MK8_CUP_BYTES);
    if (index >= MK8_CUPS_ORIGINAL) index = 0;
    return base + (index << 7);
}
