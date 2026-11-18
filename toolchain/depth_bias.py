"""Conserva o afastamento por inclinação ao escalar uniformemente o alvo Vulkan."""


def prepare(runtime):
    path = runtime/'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    old = '''        if (on) memcpy(f, st.poly_offset, sizeof f);
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)'''
    if text.count(old) != 1:
        raise ValueError('Depth-bias scale boundary changed')
    new = '''        if (on) memcpy(f, st.poly_offset, sizeof f);
        // Uniform target scaling shrinks raster depth gradients by the same factor.
        // shortcut: unequal axis scales need both primitive gradients before compensation.
        if (pass_sx > 0 && pass_sx == pass_sy && isfinite(pass_sx)) {
            float slope = f[0] * pass_sx;
            if (isfinite(slope)) f[0] = slope;
        }
        if (equal_tol) { f[1] -= 8; on = true; }  // (see the EQUAL comment above: a few depth ulps nearer)'''
    path.write_text(text.replace(old, new), encoding='utf-8')
