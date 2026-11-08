"""Sampler state stored inside the guest NVNsampler, so copies of the object stay valid.

Upstream keeps a host NvnSamplerDesc and stores its pointer in the guest object; nvnSamplerFinalize frees it.
MK8's wrapper classes copy NVN objects with memcpy (the texture wrapper copies 0xC0 bytes, see
texture_pool.py), so several guest objects share one host pointer. Finalizing one of them freed the state the
others still named. The game's sampler cache then compared a new sampler with a registered copy and the runtime
read freed memory: c0000005 in hle_nvnSamplerCompare (objects.c), Water Park Time Trial frame 2316 of 07/11/2026,
guest f_7000629da8.

On the console the sampler's state lives in the object itself (NVNsampler is 96 bytes), so a copy is complete.
Here the descriptor (72 bytes) is kept at offset 8 of the guest object, as upstream already does for the
NVNsamplerBuilder. Initialize writes it, the getters, Compare and RegisterSampler read it, and Finalize frees
nothing.
"""

SAMPLER_SIZE = 96  # NVNsampler (reserved bytes of the NVN object)

CHANGES = [
    ('''NVN(nvnSamplerInitialize) {
    NvnSamplerDesc *d = malloc(sizeof *d);
    *d = *(NvnSamplerDesc *)(AP(1) + 8);
    SET_HOST(A(0), d);
    RET(1);
}
NVN(nvnSamplerFinalize) { free(HOST(NvnSamplerDesc, A(0))); SET_HOST(A(0), 0); }''',
     '''// MK8 (toolchain/sampler_inline.py): the state at offset 8 of the guest NVNsampler (96 bytes), as in the builder
_Static_assert(8 + sizeof(NvnSamplerDesc) <= 96, "NvnSamplerDesc must fit in the guest NVNsampler");
#define MK8_SAMPLER(guest) ((NvnSamplerDesc *)(uintptr_t)((guest) + 8))
NVN(nvnSamplerInitialize) {
    *MK8_SAMPLER(A(0)) = *(NvnSamplerDesc *)(AP(1) + 8);
    SET_HOST(A(0), 0);
    RET(1);
}
NVN(nvnSamplerFinalize) { (void)c; }  /* (copies of the object may still be registered or compared) */'''),
    ('#define SMP HOST(NvnSamplerDesc, A(0))', '#define SMP MK8_SAMPLER(A(0))'),
    ('NVN(nvnSamplerCompare) { RET(memcmp(HOST(NvnSamplerDesc, A(0)), HOST(NvnSamplerDesc, A(1)), sizeof(NvnSamplerDesc)) == 0); }',
     'NVN(nvnSamplerCompare) { RET(memcmp(MK8_SAMPLER(A(0)), MK8_SAMPLER(A(1)), sizeof(NvnSamplerDesc)) == 0); }'),
    ('((NvnSamplerDesc *)d->entries)[id] = *HOST(NvnSamplerDesc, A(2));',
     '((NvnSamplerDesc *)d->entries)[id] = *MK8_SAMPLER(A(2));'),
]


def prepare(runtime):
    objects = runtime/'nvn/objects.c'
    text = objects.read_text(encoding='utf-8')
    for old, new in CHANGES:
        if text.count(old) != 1:
            raise ValueError(f'Sampler boundary changed: {old.strip()[:80]}')
        text = text.replace(old, new)
    if 'HOST(NvnSamplerDesc' in text:
        raise ValueError('Unadapted sampler host pointer remains')
    objects.write_text(text, encoding='utf-8')
