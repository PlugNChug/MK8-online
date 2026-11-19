"""Bound the VK_EXT_external_memory_host guest-heap import (multi-second stalls in Vulkan calls).

The runtime imports the whole guest heap as one host-pointer allocation. On the tested host (RADV, RX 9070 XT) that
pins 3,100 MB, and the resulting memory pressure shows up as 0.4-3.5 s stalls inside vkQueueSubmit2 / present /
texture work; with LARECOMP_VK_NO_GMEM (no import) they disappear entirely, and the stall grows with the imported
size (3,100 MB: 7 frames over 200 ms in the first 900 frames, worst 1,379 ms; 1,024/512/256 MB: none).

MK8_GMEM_MB=N imports only the first N MB. Vertex/index reads above it fall back to copies (correct, logged), shader
global reads above it are out of bounds and read zero (gmem_desc passes the imported size to the shader). MK8_GMEM_MB=0
imports the whole heap (the previous behaviour).
"""
from pathlib import Path

# Measured: above 1024 MB the pinned heap drives 0.5-18 s stalls. Below it, the above-cap vertex/index work grows:
# with toolchain/vcache_above.py one 512 MB run was clean, but two repeats showed 220-537 ms exec spikes at 753-860
# draws, so 1024 MB stays the default (the cache still helps the >1 GB ranges there).
DEFAULT_MB = 1024

HELPER = '''static uint64_t mk8_gmem_cap(uint64_t size) {  // MK8 (toolchain/gmem_cap.py)
    static uint64_t cap;
    static int read;
    if (!read) {
        read = 1;
        const char *e = getenv("MK8_GMEM_MB");
        cap = e ? (uint64_t)atoll(e) << 20 : (''' + str(DEFAULT_MB) + '''ull << 20);
    }
    return cap && size > cap ? cap : size;
}
static uint64_t gmem_want;  // MK8: heap size last imported for (pre-cap)
'''

ANCHOR = 'static bool gmem_import(uint64_t size) {\n'
PROLOGUE = HELPER + ANCHOR + '''    uint64_t t_imp0 = SDL_GetTicksNS();  // MK8 (toolchain/gmem_cap.py)
    uint64_t want = size;
    size = mk8_gmem_cap(size);
    if (size != want)
        LOG("[vk] heap import capped to %llu MB of %llu MB (MK8_GMEM_MB)\\n", (unsigned long long)(size >> 20),
            (unsigned long long)(want >> 20));
'''

TAIL_OLD = '''    LOG("[vk] guest heap imported for shader global memory: %llu MB\\n", (unsigned long long)(size >> 20));
'''
TAIL_NEW = '''    gmem_want = want;  // MK8 (toolchain/gmem_cap.py): a capped import is still "the import" for gmem_info
    LOG("[vk] guest heap imported for shader global memory: %llu MB (requested %llu MB) in %.1f ms\\n",
        (unsigned long long)(size >> 20), (unsigned long long)(want >> 20), (SDL_GetTicksNS() - t_imp0) / 1e6);
'''

INFO_OLD = '''    if (!off && want > gmem_size && !gmem_import(want)) {
'''
INFO_NEW = '''    if (!off && want > gmem_want && !gmem_import(want)) {  // MK8 (toolchain/gmem_cap.py): not gmem_size
'''

DIRECT_OLD = '''    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) return false;
'''
DIRECT_NEW = '''    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) {
        static uint64_t hi;  // MK8 (toolchain/gmem_cap.py): how far past the import vertex/index data goes
        if (gmem_buf && addr + size - HEAP_BASE > hi) {
            hi = addr + size - HEAP_BASE;
            LOG("[gmem] vertex/index above the import: end offset %llu MB (import %llu MB)\\n",
                (unsigned long long)(hi >> 20), (unsigned long long)(gmem_size >> 20));
        }
        return false;
    }
'''


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'toolchain/gmem_cap.py' in text:
        return
    for old, new in ((ANCHOR, PROLOGUE), (TAIL_OLD, TAIL_NEW), (INFO_OLD, INFO_NEW), (DIRECT_OLD, DIRECT_NEW)):
        if text.count(old) != 1:
            raise ValueError('gmem boundary changed: ' + old.strip()[:60])
        text = text.replace(old, new, 1)
    ex.write_text(text, encoding='utf-8')
