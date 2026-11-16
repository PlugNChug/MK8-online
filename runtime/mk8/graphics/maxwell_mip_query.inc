// TMML.B dos efeitos MK8: resolver somente handles estáticos da tabela NVN.
static bool mk8_tmml_linear(Ctx *c) {
    if (!c->branchy) return true;
    // O tradutor também marca como branch o padding inalcançável depois de EXIT.
    for (size_t i = 1; i < c->ninsts; i++) {
        if (i % 4 == 0) continue;
        uint64_t w = ((const uint64_t *)c->code)[i];
        uint64_t op = w & 0xfff0000000000000ull;
        if (op == 0xe300000000000000ull && bits(w, 16, 4) == PT && bits(w, 0, 5) == 15) return true;
        if (op == 0xe240000000000000ull || op == 0xe250000000000000ull ||
            op == 0xe290000000000000ull || op == 0xe2a0000000000000ull ||
            op == 0xe2b0000000000000ull || op == 0xe340000000000000ull ||
            op == 0xe350000000000000ull ||
            (w & 0xfff8000000000000ull) == 0xf0f8000000000000ull) return false;
    }
    return false;
}

static void mk8_emit_tmml_bindless(Ctx *c, uint64_t w, uint32_t pc) {
    int handle = (int)bits(w, 20, 8);
    if (bits(w, 0, 8) == RZ) return;
    // shortcut: apenas 2D/clamped, ampliar quando houver outro caso reproduzível.
    if (c->stage != STAGE_FRAGMENT || !mk8_tmml_linear(c) || bits(w, 28, 3) != 2 ||
        bits(w, 31, 4) != 2 || bits(w, 35, 14) || bits(w, 50, 1) || bits(w, 8, 8) >= 254 || handle == RZ) {
        fail(c, "tmml.b variant unsupported at %#x", pc); return;
    }
    for (uint32_t at = pc; at >= 8;) {
        at -= 8;
        if (at % 32 == 0) continue;  // palavra de escalonamento
        uint64_t prev = ((const uint64_t *)c->code)[at / 8];
        // FSETP escreve apenas predicados; KIL encerra a lane sem alterar GPRs.
        if ((prev & 0xfff0000000000000ull) == 0x4bb0000000000000ull ||
            (prev & 0xfef0000000000000ull) == 0x36b0000000000000ull ||
            (prev & 0xfff0000000000000ull) == 0xe330000000000000ull) continue;
        // TEXS.F32 2D/LOD/RGBA escreve dois pares; não basta conferir o primeiro destino.
        if ((prev & 0xf600000000000000ull) == 0xd000000000000000ull &&
            bits(prev, 59, 1) && bits(prev, 53, 4) == 3 && bits(prev, 50, 3) == 4) {
            int d0 = (int)bits(prev, 0, 8), d1 = (int)bits(prev, 28, 8);
            if (d1 == RZ || (d0 != RZ && handle >= d0 && handle <= d0 + 1) ||
                (handle >= d1 && handle <= d1 + 1)) break;
            continue;
        }
        bool mov_cb = (prev & 0xfff8000000000000ull) == 0x4c98000000000000ull;
        bool scalar = mov_cb ||
            (prev & 0xfff0000000000000ull) == 0x0100000000000000ull ||  // MOV32I
            (prev & 0xfff8000000000000ull) == 0x5080000000000000ull ||  // MUFU
            (prev & 0xfff8000000000000ull) == 0x5c68000000000000ull ||  // FMUL reg
            (prev & 0xfef8000000000000ull) == 0x3868000000000000ull ||  // FMUL imediato
            ((prev & 0xfff8000000000000ull) == 0x5cb8000000000000ull && bits(prev, 8, 4) == 10) ||  // I2F 32
            ((prev & 0xfff8000000000000ull) == 0xdf60000000000000ull && bits(prev, 31, 4) == 2) ||  // TMML.B clamped
            (prev & 0xff0000400000ff00ull) == 0xe00000000000ff00ull;   // IPA direto
        if (!scalar) break;  // não inferir ausência de escrita para instruções desconhecidas
        if ((int)bits(prev, 0, 8) != handle) continue;
        uint32_t offset = (uint32_t)bits(prev, 20, 14) * 4;
        if (!mov_cb || bits(prev, 16, 4) != PT || bits(prev, 39, 4) != 15 ||
            bits(prev, 34, 5) != 2 || offset < 0x20 || offset > 0x118 || offset % 8) break;
        // Mesmo slot de BindTextures; nenhuma leitura dinâmica ou novo contrato de ABI.
        uint64_t bound = (w & 0x0002000fffffffffull) | 0xdf58000000000000ull | ((uint64_t)(offset / 4) << 36);
        emit_tmml(c, bound);
        return;
    }
    fail(c, "tmml.b handle provenance unsupported at %#x", pc);
}
