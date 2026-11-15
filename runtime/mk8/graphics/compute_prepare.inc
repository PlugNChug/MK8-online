/* Só o tradutor acessa estas entradas. Não ocupam a tabela de programas usados. */
static struct { CsProg *cp; Mk8ComputeSnapshot *snapshot; } mk8_compute_prepared[MK8_COMPUTE_PREPARE_LIMIT];
static unsigned mk8_compute_prepared_count;
static CsProg *mk8_compute_retired;

static void mk8_compute_release(CsProg *cp) {
    if (cp->pipe) vkDestroyPipeline(g_vk.dev, cp->pipe, NULL);
    if (cp->p.layout) vkDestroyPipelineLayout(g_vk.dev, cp->p.layout, NULL);
    for (int i = 0; i < 3; i++)
        if (cp->p.set_layout[i]) vkDestroyDescriptorSetLayout(g_vk.dev, cp->p.set_layout[i], NULL);
    if (cp->si) maxwell_free(cp->si);
    free(cp);
}

static void mk8_compute_collect(bool drained) {
    CsProg **link = &mk8_compute_retired;
    while (*link) {
        CsProg *cp = *link;
        if (!drained && cp->p.async && atomic_load(&cp->p.cjob.state) != JOB_DONE) {
            link = &cp->mk8_retired_next;
            continue;
        }
        *link = cp->mk8_retired_next;
        mk8_compute_release(cp);
    }
}

static void mk8_compute_prepare_reset(void) {
    /* build_drain já terminou. Só liberamos programas extras que nunca chegaram à GPU. */
    for (unsigned i = 0; i < mk8_compute_prepared_count; i++) {
        free(mk8_compute_prepared[i].snapshot);
        mk8_compute_release(mk8_compute_prepared[i].cp);
    }
    mk8_compute_prepared_count = 0;
    mk8_compute_collect(true);
}

void mk8_compute_prepare(Mk8ComputeSnapshot *s) {
    mk8_compute_collect(false);
    for (int i = 0; i < g_cs_n; i++)
        if (g_cs_tab[i].addr == s->addr) { free(s); return; }
    for (unsigned i = 0; i < mk8_compute_prepared_count; i++)
        if (mk8_compute_prepared[i].snapshot->addr == s->addr) { free(s); return; }
    if (mk8_compute_prepared_count == MK8_COMPUTE_PREPARE_LIMIT || g_cs_n == 256) { free(s); return; }
    NvnShaderStage ss = {.code_addr = s->addr, .const_offset = (uint32_t)s->size};
    CsProg *cp = mk8_compute_build(&ss, s->code, s->local);
    unsigned i = mk8_compute_prepared_count++;
    mk8_compute_prepared[i].cp = cp;
    mk8_compute_prepared[i].snapshot = s;
    LOG("[mk8:compute-prepare] event=start frame=%llu set_frame=%llu code=%llx fingerprint=%llx local=%u/%u/%u program=%p job=%p\n",
        (unsigned long long)vk_frame_counter(), (unsigned long long)s->frame, (unsigned long long)s->addr,
        (unsigned long long)s->fingerprint, s->local[0], s->local[1], s->local[2], (void *)cp, (void *)&cp->p.cjob);
}

static CsProg *compute_program(NvnProgram *np) {
    NvnShaderStage *ss = &np->stages[5];
    if (!ss->code_addr) return NULL;
    for (int i = 0; i < g_cs_n; i++) if (g_cs_tab[i].addr == ss->code_addr) return g_cs_tab[i].cp;
    mk8_compute_collect(false);
    CsProg *cp = NULL;
    for (unsigned i = 0; i < mk8_compute_prepared_count; i++) {
        Mk8ComputeSnapshot *s = mk8_compute_prepared[i].snapshot;
        if (s->addr != ss->code_addr) continue;
        bool valid = mk8_compute_matches(s, ss);
        cp = valid ? mk8_compute_prepared[i].cp : NULL;
        if (!valid) {
            CsProg *retired = mk8_compute_prepared[i].cp;
            retired->mk8_retired_next = mk8_compute_retired;
            mk8_compute_retired = retired;
        }
        LOG("[mk8:compute-prepare] event=first-use frame=%llu set_frame=%llu code=%llx fingerprint=%llx valid=%d state=%d\n",
            (unsigned long long)vk_frame_counter(), (unsigned long long)s->frame, (unsigned long long)s->addr,
            (unsigned long long)s->fingerprint, valid, atomic_load(&mk8_compute_prepared[i].cp->p.state));
        free(s);
        mk8_compute_prepared[i] = mk8_compute_prepared[--mk8_compute_prepared_count];
        break;
    }
    if (!cp) cp = mk8_compute_build(ss, NULL, NULL);
    int n = g_cs_n;
    if (n < 256) { g_cs_tab[n].addr = ss->code_addr; g_cs_tab[n].cp = cp; g_cs_n = n + 1; }
    return cp;
}
