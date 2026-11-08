"""Video frames of MK8 4.0.1's menus (the mode previews): the decoded NV12 frame goes into the game's own buffer.

The game passes a buffer of its own to the decoder's GetOutputBuffer for every frame and reads the frame from it;
upstream pointed the Buffer at the decoder's internal one, so the game's stayed zeroed and the previews were solid green
(08/11/2026: decoded frames had mean Y 123 / UV 127, the game's buffers zeros). MK8_TRACE_MOVIE=1 also logs those
buffers and the mean of each decoded plane.
"""

TRACE = ('    if (d->video && b->base && b->capacity == 8) {\n', '''    if (getenv("MK8_TRACE_MOVIE") && d->video) {  // MK8 (toolchain/movie_output.py)
        static int n;
        if (n++ < 12) {
            uint64_t ysum = 0, uvsum = 0, ysz = (uint64_t)d->stride * d->height, uvoff = (uint64_t)d->stride * ((d->height + 15) & ~15);
            for (uint64_t k = 0; k < ysz && k < d->out_cap; k += 16) ysum += d->out_mem[i][k];
            for (uint64_t k = uvoff; k < uvoff + ysz / 2 && k < d->out_cap; k += 16) uvsum += d->out_mem[i][k];
            LOG("[mk8:movie] output buffer %d from the game: base %llx capacity %llu offset %d size %d (decoded %u bytes at %p, "
                "mean Y %llu UV %llu)\\n", i, (unsigned long long)b->base, (unsigned long long)b->capacity, b->offset, b->size,
                d->out_size[i], (void *)d->out_mem[i], (unsigned long long)(ysum * 16 / (ysz ? ysz : 1)),
                (unsigned long long)(uvsum * 32 / (ysz ? ysz : 1)));
        }
    }
    if (d->video && b->base && b->capacity == 8) {
''')


OWN = ('''    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->out_mem[i];
    b->capacity = d->out_cap;
    b->size = (int32_t)d->out_size[i];
''', '''    // MK8 (toolchain/movie_output.py): MK8 4.0.1 hands the decoder its own buffer for each frame and reads the frame
    // from there (its menu previews copy it into their Y / UV textures); pointing the Buffer at ours left that memory
    // zeroed, and zeroed NV12 is solid green. MK8_MOVIE_OWN_BUFFER=1: upstream (our buffer).
    if (d->video && b->base && b->capacity != 8 && b->capacity >= d->out_size[i] && !getenv("MK8_MOVIE_OWN_BUFFER")) {
        memcpy((void *)(uintptr_t)b->base, d->out_mem[i], d->out_size[i]);
        b->offset = 0;
        b->size = (int32_t)d->out_size[i];
        c->x[0] = 0;
        return;
    }
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)d->out_mem[i];
    b->capacity = d->out_cap;
    b->size = (int32_t)d->out_size[i];
''')


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Movie output boundary changed: {old.strip()[:80]}')
    return text.replace(old, new)


def prepare(runtime):
    movie = runtime/'hle/movie.c'
    text = replace(movie.read_text(encoding='utf-8'), *TRACE)
    movie.write_text(replace(text, *OWN), encoding='utf-8')
