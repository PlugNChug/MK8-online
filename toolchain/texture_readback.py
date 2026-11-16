"""Implement the pinned NVN CPU-read entrypoints using the existing Vulkan readback path."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def replace(text, old, new):
    if text.count(old) != 1:
        raise ValueError('Texture readback boundary changed: ' + old[:90])
    return text.replace(old, new)


def prepare(runtime):
    path = runtime / 'nvn/objects.c'
    text = path.read_text(encoding='utf-8')
    declaration = '''typedef struct Mk8Readback {
    NvnTexture *texture;
    const NvnTextureView *view;
    const int32_t *region;
    uint64_t destination;
    int64_t row, image;
} Mk8Readback;
extern void mk8_texture_readback(NvnTexture *, const NvnTextureView *, const int32_t *, uint64_t, int64_t, int64_t);
'''
    boundary = 'typedef struct Mk8QItem {'
    text = replace(text, boundary, declaration + boundary)
    boundary = '    int kind, n;'
    text = replace(text, boundary, boundary + '\n    const Mk8Readback *readback;')
    boundary = 'MK8_Q_SUBMIT, MK8_Q_PRESENT, MK8_Q_FENCE'
    text = replace(text, boundary, boundary + ', MK8_Q_READBACK')
    boundary = '        if (it->kind == MK8_Q_SUBMIT) {'
    text = replace(text, boundary, '''        if (it->kind == MK8_Q_READBACK) {
            const Mk8Readback *r = it->readback;
            mk8_texture_readback(r->texture, r->view, r->region, r->destination, r->row, r->image);
        } else if (it->kind == MK8_Q_SUBMIT) {''')
    old = ('NVN(nvnTextureReadTexels) { LOG("[nvn] ReadTexels not implemented\\n"); (void)c; }\n'
           'NVN(nvnTextureReadTexelsStrided) { (void)c; }')
    new = '''static void mk8_readback_sync(NvnTexture *texture, const NvnTextureView *view, const int32_t *region,
                              uint64_t destination, int64_t row, int64_t image) {
    if (!mk8_async_on()) { mk8_texture_readback(texture, view, region, destination, row, image); return; }
    // The request and guest arguments remain live until the worker finishes the CPU copy.
    Mk8Readback request = {mk8_texture_retain(texture), view, region, destination, row, image};
    Mk8QItem *it = calloc(1, sizeof *it);
    if (!it) FATAL("Texture readback allocation failed\\n");
    it->kind = MK8_Q_READBACK;
    it->readback = &request;
    mk8_q_wait(mk8_q_put(it, NULL, 0));
    mk8_texture_release(request.texture);
}
NVN(nvnTextureReadTexels) {
    mk8_readback_sync(A(0) ? HOST(NvnTexture,A(0)) : NULL, AP(1), AP(2), A(3), 0, 0);
}
NVN(nvnTextureReadTexelsStrided) {
    mk8_readback_sync(A(0) ? HOST(NvnTexture,A(0)) : NULL, AP(1), AP(2), A(3), (int64_t)A(4), (int64_t)A(5));
}'''
    path.write_text(replace(text, old, new), encoding='utf-8')
    path = runtime / 'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    boundary = 'static void copy_texture(const TexRefRec *src, const TexRefRec *dst, uint64_t flags) {'
    own = (ROOT / 'runtime/mk8/graphics/texture_readback.inc').read_text(encoding='utf-8')
    path.write_text(replace(text, boundary, own + '\n' + boundary), encoding='utf-8')
