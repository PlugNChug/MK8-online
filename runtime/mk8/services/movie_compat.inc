// MK8's three-argument constructor must use the same host objects as MediaData/Extractor.
HLE_FUNC("_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE", mk8_movie_decoder_ctor) {
    movie_decoder_ctor(c);
}

HLE_FUNC("_ZNK5movie9MediaData8FindRectEPKcPiS3_S3_S3_", mk8_movie_find_rect) {
    MediaData *m=md(c->x[0]);
    const char *key=(const char *)(uintptr_t)c->x[1];
    if (getenv("MK8_TRACE_MOVIE")) LOG("[mk8:movie] rectangle key=%s\n",key);
    if (strcmp(key,"crop")) { c->x[0]=0; return; }
    int32_t width=0,height=0;
    for(int i=0;i<m->n;i++) {
        if(!m->e[i].is_str && !strcmp(m->e[i].key,"width")) width=(int32_t)m->e[i].ival;
        if(!m->e[i].is_str && !strcmp(m->e[i].key,"height")) height=(int32_t)m->e[i].ival;
    }
    if(width<=0 || height<=0) { c->x[0]=0; return; }
    *(int32_t *)(uintptr_t)c->x[2]=0; *(int32_t *)(uintptr_t)c->x[3]=0;
    *(int32_t *)(uintptr_t)c->x[4]=width-1; *(int32_t *)(uintptr_t)c->x[5]=height-1;
    c->x[0]=1;
}
