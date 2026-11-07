"""Debug switches read once instead of on every draw, clear or dispatch.

getenv in the Windows CRT locks the environment and scans it; pipe_key called it for every draw (sampler, boot 139:
~16% of the translating thread's samples were in ucrtbase called from pipe_key). Each switch keeps its meaning; it is
read at its first use, like the runtime's other LARECOMP_* switches.
"""

SWITCHES = [
    ('    if (getenv("LARECOMP_VK_NO_BLEND")) k->blend_on = 0;  // (debug: no blending at all)\n',
     '    static int no_blend = -1;\n    if (no_blend < 0) no_blend = getenv("LARECOMP_VK_NO_BLEND") != NULL;\n'
     '    if (no_blend) k->blend_on = 0;  // (debug: no blending at all)\n'),
    ('            if (getenv("LARECOMP_LOG_CLEARS") && index < st.rt.ncolor && st.rt.color[index])\n',
     '            static int log_clears = -1;\n            if (log_clears < 0) log_clears = getenv("LARECOMP_LOG_CLEARS") != NULL;\n'
     '            if (log_clears && index < st.rt.ncolor && st.rt.color[index])\n'),
    ('    if (cp->si->uses_gmem && getenv("LARECOMP_LOG_CS_SBUF")) {',
     '    static int log_cs_sbuf = -1;\n    if (log_cs_sbuf < 0) log_cs_sbuf = getenv("LARECOMP_LOG_CS_SBUF") != NULL;\n'
     '    if (cp->si->uses_gmem && log_cs_sbuf) {'),
]


TEXTURE_SWITCHES = [  # (textures.c: the format of a signed BC5 texture, looked up per use)
    ('    case 0x4C: return (VkFmt){getenv("LARECOMP_BC5_UNORM") ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};\n',
     '    case 0x4C: {\n        static int bc5_unorm = -1;\n        if (bc5_unorm < 0) bc5_unorm = getenv("LARECOMP_BC5_UNORM") != NULL;\n'
     '        return (VkFmt){bc5_unorm ? VK_FORMAT_BC5_UNORM_BLOCK : VK_FORMAT_BC5_SNORM_BLOCK};\n    }\n'),
]

FMZ_MODE = '''static int mk8_fmz_mode(void) {
    static __thread int mode;
    if (!mode) mode = getenv("LARECOMP_NO_FMZ") ? -1 : 2;
    return mode;
}
'''

TRANSLATOR_SWITCHES = [
    ('#define FMZ (getenv("LARECOMP_NO_FMZ") ? -1 : 2)', FMZ_MODE + '#define FMZ mk8_fmz_mode()'),
]

STRING_APPEND = '''static void sp(Str *s, const char *fmt, ...) {
    va_list ap, retry;
    va_start(ap, fmt);
    va_copy(retry, ap);
    size_t available = s->cap - s->len;
    int n = vsnprintf(available ? s->buf + s->len : NULL, available, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n > SIZE_MAX - s->len - 1) abort();
    if ((size_t)n >= available) {
        size_t needed = s->len + (size_t)n + 1;
        size_t capacity = needed <= SIZE_MAX / 2 ? needed * 2 : needed;
        char *buf = realloc(s->buf, capacity);
        if (!buf) abort();
        s->buf = buf;
        s->cap = capacity;
        if (vsnprintf(s->buf + s->len, s->cap - s->len, fmt, retry) != n) abort();
    }
    va_end(retry);
    s->len += (size_t)n;
}
'''


def apply(path, switches):
    text = path.read_text(encoding='utf-8')
    for old, new in switches:
        if text.count(old) != 1:
            raise ValueError(f'Hot path boundary changed: {old.strip()[:80]}')
        text = text.replace(old, new)
    path.write_text(text, encoding='utf-8')


def prepare(runtime):
    apply(runtime/'nvn/vulkan/exec.c', SWITCHES)
    apply(runtime/'nvn/vulkan/textures.c', TEXTURE_SWITCHES)
    apply(runtime/'nvn/maxwell/translate.c', TRANSLATOR_SWITCHES)
    path = runtime/'nvn/maxwell/translate.c'
    text = path.read_text(encoding='utf-8')
    start = text.index('static void sp(Str *s, const char *fmt, ...) {')
    end = text.index('\n}\n', start) + 3
    original = text[start:end]
    if original.count('vsnprintf') != 2 or 'vsnprintf(NULL, 0, fmt, ap)' not in original:
        raise ValueError('Shader string append boundary changed')
    # Usually the allocation has room: format once, retry only when it needs to grow.
    path.write_text(text[:start] + STRING_APPEND + text[end:], encoding='utf-8')
