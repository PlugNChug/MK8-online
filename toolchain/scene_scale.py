"""Scale the two MK8 scene buffers excluded by the upstream 2D/compressible heuristic.

Boo Lake uses a 1920x1080 R16F depth copy without flags, and a two-layer
B10G11R11 light array. Leaving them at native size while depth/normals scale
clips the lighting pass to the top-left part of its attachments. Keep these
observed, single-level buffers on the scene's scale; do not scale arbitrary
arrays, shadow cubes, multisample resources or CPU-uploaded colour textures.
"""

OLD = '''    if (getenv("LARECOMP_NO_SCALE")) return false;
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;'''

NEW = '''    if (getenv("LARECOMP_NO_SCALE")) return false;
    // MK8 4.0.1 scene depth copy / two-layer light accumulation (toolchain/scene_scale.py).
    if (t->width == 1920 && t->height == 1080 && t->levels == 1 && t->samples <= 1) {
        if (t->target == 1 && t->depth == 1 && t->flags == 0 && t->format == 0x05)
            return true;
        if (t->target == 4 && t->depth == 2 && t->flags == 8 && t->format == 0x3f)
            return true;
    }
    if (t->target != 1 || t->samples > 1 || !(t->flags & 8) || t->width < 32 || t->height < 16) return false;'''


def prepare(runtime):
    path = runtime / 'nvn/vulkan/textures.c'
    text = path.read_text(encoding='utf-8')
    if text.count(OLD) != 1:
        raise ValueError('Scene texture scaling boundary changed upstream')
    path.write_text(text.replace(OLD, NEW), encoding='utf-8')
