"""Serve vertex/index data above the capped heap import from the device-local vertex cache.

At a cap below the data (e.g. 512 MB) guest_direct fell back to a transient copy per draw, which turned course loads
into 0.5-1.1 s bursts. The existing vcache already keeps a device-local buffer per range and refreshes only CPU-written
pages; it only refused ranges past gmem_size because vc_flush copies from the imported heap. Above the cap, copy from
guest memory through the upload ring instead: one sequential copy per range at first use, reused by every later draw.
MK8_GMEM_MB=0 keeps the previous behaviour for the ranges the import covers.
"""
from pathlib import Path

GUEST_OLD = '''static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    if (!gmem_buf || addr + size > HEAP_BASE + gmem_size) {
        static uint64_t hi;  // MK8 (toolchain/gmem_cap.py): how far past the import vertex/index data goes
        if (gmem_buf && addr + size - HEAP_BASE > hi) {
            hi = addr + size - HEAP_BASE;
            LOG("[gmem] vertex/index above the import: end offset %llu MB (import %llu MB)\\n",
                (unsigned long long)(hi >> 20), (unsigned long long)(gmem_size >> 20));
        }
        return false;
    }
    if (addr - HEAP_BASE + size > gmem_direct_limit()) return false;
    gpu_read_mark(addr, size);  // (read when the submission runs: see gpu_read_mark)
    if (vcache_lookup(addr, size, buf, off)) return true;
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}
'''

GUEST_NEW = '''static bool guest_direct(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off) {
    static int copy = -1;
    if (copy < 0) copy = getenv("LARECOMP_VK_COPY_VERTICES") != NULL;
    if (copy || addr < HEAP_BASE) return false;
    if (addr + size > HEAP_BASE + gmem_size && kernel_heap_size() != gmem_tried) { gmem_tried = kernel_heap_size(); gmem_info(); }
    bool above = !gmem_buf || addr + size > HEAP_BASE + gmem_size;  // MK8 (toolchain/vcache_above.py)
    if (above) {
        if (!gmem_buf || addr + size > HEAP_BASE + HEAP_MAX) return false;
    } else if (addr - HEAP_BASE + size > gmem_direct_limit()) {
        return false;
    }
    gpu_read_mark(addr, size);  // (read when the submission runs: see gpu_read_mark)
    if (vcache_lookup(addr, size, buf, off)) return true;  // (now also above the import: vc_flush copies from guest memory)
    if (above) {
        static uint64_t hi;  // MK8 (toolchain/gmem_cap.py): how far past the import vertex/index data goes
        if (addr + size - HEAP_BASE > hi) {
            hi = addr + size - HEAP_BASE;
            LOG("[gmem] vertex/index above the import and the cache: end offset %llu MB (import %llu MB)\\n",
                (unsigned long long)(hi >> 20), (unsigned long long)(gmem_size >> 20));
        }
        return false;
    }
    *buf = gmem_buf;
    *off = addr - HEAP_BASE;
    return true;
}
'''

LOOKUP_OLD = '    if (hi > gmem_size) return false;\n'
LOOKUP_NEW = '    if (hi > HEAP_MAX) return false;  // MK8 (toolchain/vcache_above.py): the cache serves above the import\n'

FLUSH_OLD = '''        if (nc) {
            vkCmdCopyBuffer(cb, gmem_buf, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
'''
FLUSH_NEW = '''        if (nc) {
            VkBuffer src = gmem_buf;
            VkDeviceSize adj = 0;
            if (gmem_size && e->hi > gmem_size) {  // MK8 (toolchain/vcache_above.py): above the import, from guest memory
                VkSpan sp = vk_transient_copy((const void *)(uintptr_t)(HEAP_BASE + e->lo), e->hi - e->lo, 256);
                src = sp.buf;
                adj = sp.off - e->lo;
            }
            for (int k = 0; k < nc; k++) cp[k].srcOffset += adj;
            vkCmdCopyBuffer(cb, src, e->buf, (uint32_t)nc, cp);
            for (int k = 0; k < nc; k++) prof_vc_copied += cp[k].size;
        }
'''


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'toolchain/vcache_above.py' in text:
        return
    for old, new in ((GUEST_OLD, GUEST_NEW), (LOOKUP_OLD, LOOKUP_NEW), (FLUSH_OLD, FLUSH_NEW)):
        if text.count(old) != 1:
            raise ValueError('vcache above boundary changed: ' + old.strip()[:50])
        text = text.replace(old, new, 1)
    ex.write_text(text, encoding='utf-8')
