// Appended to pl.c to reuse its local font loader. MK8 SDK SharedFontData ABI.
HLE_FUNC("_ZN2nn2pl13GetSharedFontEPiPNS0_14SharedFontDataEiNS_8settings12LanguageCodeE", mk8_shared_fonts) {
    uint64_t count_ptr = c->x[0], array = c->x[1];
    int32_t capacity = (int32_t)c->x[2];
    c->x[0] = 0;
    if (count_ptr < GUEST_LO || count_ptr > GUEST_HI - 4 || capacity < 0) return;
    unsigned count = capacity < NFONTS ? (unsigned)capacity : NFONTS;
    if (count && (array < GUEST_LO || array > GUEST_HI - count * 24)) return;
    for (unsigned i = 0; i < count; i++) {
        const Font *f = font_for(i);
        uint64_t entry[3] = {i, f->addr, f->size};
        memcpy((void *)(uintptr_t)(array + i * 24), entry, sizeof entry);
    }
    int32_t available = NFONTS;
    memcpy((void *)(uintptr_t)count_ptr, &available, sizeof available);
    c->x[0] = 1;
    LOG("[mk8:fonts] shared fonts available=%u written=%u (local substitutes permitted)\n", NFONTS, count);
}
