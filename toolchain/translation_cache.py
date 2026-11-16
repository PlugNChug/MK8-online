"""Keep Maxwell -> GLSL translations on disk between sessions (runtime/mk8/graphics/translation_cache.c).

Only SPIR-V and pipelines were kept, so each session translated every shader again the first time it showed. The
prefetch scan waits for that translation on the frame path: with a warm cache, a race still had 16-19 frames with
over 5 ms of prefetch and 7 over 10 ms (14/11/2026). The four translations of the Vulkan backend (the three stages of
a program and compute shaders) go through mk8_translate_cached, which reads a stored translation from
cache/xlate.pack (one file, opened once, indexed in memory: a file per shader cost 0.2-0.6 ms to open) or translates and
appends it. translate.c is unchanged (tests compile it alone); the compile-ahead (ahead.c, off for MK8) is unchanged.
MK8_TRANSLATION_CACHE=0 turns it off.
"""

CALL = '= maxwell_translate('
BOUNDARY = 'static pthread_mutex_t translate_lock = PTHREAD_MUTEX_INITIALIZER;'
DECLARATION = ('// MK8 (toolchain/translation_cache.py): maxwell_translate, read from cache/xlate when this runtime stored it\n'
               'ShaderInfo *mk8_translate_cached(const uint8_t *blob, size_t max_size, int stage, const ShaderKey *key);\n')


def prepare(runtime):
    path = runtime/'nvn/vulkan/exec.c'
    text = path.read_text(encoding='utf-8')
    if text.count(CALL) != 4 or text.count(BOUNDARY) != 1 or 'mk8_translate_cached' in text:
        raise ValueError('Translation cache boundary changed: nvn/vulkan/exec.c')
    text = text.replace(CALL, '= mk8_translate_cached(').replace(BOUNDARY, DECLARATION + BOUNDARY)
    path.write_text(text, encoding='utf-8')
