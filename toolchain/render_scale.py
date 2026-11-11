"""Mantém alvos 2D e 2D-array na mesma escala de renderização."""


def prepare(runtime):
    textures = runtime/'nvn/vulkan/textures.c'
    text = textures.read_text(encoding='utf-8')
    old = '    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;'
    # Arrays e depth linear R16 participam dos mesmos passes que o depth 2D reduzido.
    new = '''    if ((t->target != 1 && t->target != 4) || t->samples > 1 || t->width < 32 || t->height < 16) return false;
    // MK8: o intermediário de depth linear R16 não usa a flag 8.
    if (!(t->flags & 8) && !(t->flags == 0 && t->target == 1 && t->format == 0x05)) return false;'''
    if text.count(old) != 1:
        raise ValueError('Render scale eligibility boundary changed')
    textures.write_text(text.replace(old, new), encoding='utf-8')
