// Plain TLD4S F32. Attribution and license are recorded in docs/THIRD_PARTY.md.
// Depth-compare, offsets and F16 variants are deliberately not claimed by this adapter.
static void mk8_emit_tld4s(Ctx *c, uint64_t w) {
    int d0 = (int)bits(w, 0, 8), a = (int)bits(w, 8, 8);
    int b = (int)bits(w, 20, 8), d1 = (int)bits(w, 28, 8);
    uint32_t index = (uint32_t)bits(w, 36, 13);
    int comp = (int)bits(w, 52, 2);
    if (bits(w, 50, 1) || bits(w, 51, 1)) {
        fail(c, "tld4s depth-compare/offset unsupported"); return;
    }
    int slot = tex_slot(c, index, TEXTYPE_2D, false, false);
    char call[512];
    if (c->glsl)
        snprintf(call, sizeof call, "textureGather(t%d, vec2(%s, %s), %d)", slot, F(R(a)), F(R(b)), comp);
    else
        snprintf(call, sizeof call, "t%d.gather(s%d, float2(%s, %s), int2(0), component::%c)", slot, slot, F(R(a)), F(R(b)), "xyzw"[comp]);
    write_tex_result(c, slot, call, 0xf, d0, d1, true, false);
}
