// Pools own their registered descriptors. Recorded commands and the executor may outlive the guest object.
// Zero-reference pools retire on the translator, just like textures. No GPU object is freed on a guest thread.
static pthread_mutex_t mk8_desc_lock = PTHREAD_MUTEX_INITIALIZER;
static NvnDescPool *mk8_desc_live, *mk8_desc_dead;
void mk8_cube_forget(NvnDescPool *d);

NvnDescPool *mk8_descriptor_retain(NvnDescPool *d) {
    if (d && !__atomic_fetch_add(&d->mk8_pool_refs, 1, __ATOMIC_RELAXED))
        FATAL("Retaining a retired descriptor pool\n");
    return d;
}

void mk8_descriptor_release(NvnDescPool *d) {
    if (!d) return;
    unsigned previous = __atomic_fetch_sub(&d->mk8_pool_refs, 1, __ATOMIC_ACQ_REL);
    if (!previous) FATAL("Descriptor reference underflow\n");
    if (previous != 1) return;
    pthread_mutex_lock(&mk8_desc_lock);
    NvnDescPool **p = &mk8_desc_live;
    while (*p && *p != d) p = &(*p)->mk8_next;
    if (!*p) FATAL("Untracked descriptor pool\n");
    *p = d->mk8_next;
    d->mk8_next = mk8_desc_dead;
    mk8_desc_dead = d;
    pthread_mutex_unlock(&mk8_desc_lock);
}

static void mk8_descriptor_created(NvnDescPool *d, bool texture) {
    d->mk8_pool_refs = 1;
    d->mk8_texture = texture;
    pthread_mutex_lock(&mk8_desc_lock);
    d->mk8_next = mk8_desc_live;
    mk8_desc_live = d;
    pthread_mutex_unlock(&mk8_desc_lock);
}

static void mk8_descriptor_finalized(NvnDescPool *d) {
    if (!d) return;
    pthread_mutex_lock(&mk8_desc_lock);
    bool first = !d->mk8_finalized;
    d->mk8_finalized = true;
    NvnDescPool **reg = d->mk8_texture ? g_texture_pools : g_sampler_pools;
    for (int i = 0; i < 64; i++) if (reg[i] == d) reg[i] = NULL;
    pthread_mutex_unlock(&mk8_desc_lock);
    if (first) mk8_descriptor_release(d);
}

void mk8_descriptors_drain(void) {
    pthread_mutex_lock(&mk8_desc_lock);
    NvnDescPool *d = mk8_desc_dead;
    mk8_desc_dead = NULL;
    pthread_mutex_unlock(&mk8_desc_lock);
    while (d) {
        NvnDescPool *next = d->mk8_next;
        if (d->mk8_texture) {
            mk8_cube_forget(d);
            NvnTexEntry *entries = d->entries;
            for (int i = 0; i < d->count; i++) mk8_texture_release(entries[i].tex);
        }
        free(d->entries);
        free(d);
        d = next;
    }
}

// Reboot only: guest threads stopped, translator drained, command and executor references already released.
void mk8_descriptors_reset(void) {
    NvnDescPool *d = mk8_desc_live;
    while (d) {
        NvnDescPool *next = d->mk8_next;
        mk8_descriptor_finalized(d);
        d = next;
    }
    if (mk8_desc_live) FATAL("Descriptor references survived graphics reset\n");
    mk8_descriptors_drain();
}

void mk8_descriptors_report(void) {
    uint64_t live = 0, finalized = 0, refs = 0, pending = 0, entries = 0;
    pthread_mutex_lock(&mk8_desc_lock);
    for (NvnDescPool *d = mk8_desc_live; d; d = d->mk8_next) {
        live++;
        finalized += d->mk8_finalized;
        refs += __atomic_load_n(&d->mk8_pool_refs, __ATOMIC_RELAXED);
        entries += (uint64_t)d->count * (d->mk8_texture ? sizeof(NvnTexEntry) : sizeof(NvnSamplerDesc));
    }
    for (NvnDescPool *d = mk8_desc_dead; d; d = d->mk8_next) pending++;
    pthread_mutex_unlock(&mk8_desc_lock);
    LOG("[mk8:vram] descriptor pools: %llu live, %llu finalized retained, %llu references, %llu pending; %.2f MB host entries\n",
        (unsigned long long)live, (unsigned long long)finalized, (unsigned long long)refs,
        (unsigned long long)pending, entries / 1048576.0);
}
