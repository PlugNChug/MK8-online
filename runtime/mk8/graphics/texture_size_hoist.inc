// MK8 (toolchain/texture_size_hoist.py): level-0 texture sizes of a branching GLSL shader are read once, before
// the `switch (pc)` loop. NVIDIA's Pascal compiler (GTX 1070, driver 580) mistranslates a textureSize inside that
// loop after an SSY reconvergence: the invocations never leave the loop and run it until the 100000 guard, with wrong
// results. The race's shadow mask (array layer clamp `tsizeN(0).z`) took 686 ms per frame (#7).
static void mk8_hoist_texture_sizes(Str *s, Str *body, int ntex) {
    if (!body->buf) return;
    for (int i = 0; i < ntex; i++) {
        char call[24], name[16];
        snprintf(call, sizeof call, "tsize%d(0)", i);
        snprintf(name, sizeof name, "tsz%d", i);
        size_t cl = strlen(call), nl = strlen(name);
        char *at = strstr(body->buf, call);
        if (!at) continue;
        sp(s, "  ivec3 %s = %s;\n", name, call);
        char *w = at;  // (the name is shorter than the call: rewritten in place)
        for (const char *r = at; *r;) {
            if (!strncmp(r, call, cl)) { memcpy(w, name, nl); w += nl; r += cl; }
            else *w++ = *r++;
        }
        *w = 0;
        body->len = (size_t)(w - body->buf);
    }
}
