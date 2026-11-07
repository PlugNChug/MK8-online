NvnTexture *mk8_texture_retain(NvnTexture *t) {
    if (t && !__atomic_fetch_add(&t->mk8_refs, 1, __ATOMIC_RELAXED))
        FATAL("Retaining a retired texture\n");
    return t;
}

void mk8_texture_release(NvnTexture *t) {
    if (!t) return;
    unsigned previous = __atomic_fetch_sub(&t->mk8_refs, 1, __ATOMIC_ACQ_REL);
    if (!previous) FATAL("Texture reference underflow\n");
    if (previous == 1) {
        backend_texture_destroyed(t);
        free(t);
    }
}
