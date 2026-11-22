"""Bound the swapchain acquire (MK8_ACQUIRE_MS, default 100; 0 keeps UINT64_MAX).

vkAcquireNextImageKHR was the only WSI wait with no timeout: a compositor that stops handing images back could block
a frame indefinitely. On VK_TIMEOUT vk_frame_begin returns false through its existing failure path and the next frame
retries. Applied after toolchain/submit_profile.py, whose instrumentation text it adjusts (that file also logs lock vs
call for present and acquire, which is how the WSI path is attributed).
"""
from pathlib import Path

HELPER_OLD = 'bool vk_frame_begin(const float clear[4]) {\n'
HELPER_NEW = '''static uint64_t mk8_acquire_timeout_ns(void) {  // MK8 (toolchain/acquire_bound.py)
    static uint64_t v = ~0ull;
    if (v == ~0ull) {
        const char *e = getenv("MK8_ACQUIRE_MS");
        uint64_t ms = e ? (uint64_t)atoll(e) : 100;
        v = ms ? ms * 1000000ull : UINT64_MAX;
    }
    return v;
}
bool vk_frame_begin(const float clear[4]) {
'''

FIRST_OLD = '''    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    uint64_t t_a3 = SDL_GetTicksNS();
'''
FIRST_NEW = '''    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, mk8_acquire_timeout_ns(), acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    uint64_t t_a3 = SDL_GetTicksNS();
'''

RETRY_OLD = '''        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
'''
RETRY_NEW = '''        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, mk8_acquire_timeout_ns(), acquire_sem[slot], VK_NULL_HANDLE,
                                  &cur_frame.image);  // MK8 (toolchain/acquire_bound.py)
'''


def prepare(runtime: Path):
    dev = runtime/'nvn/vulkan/device.c'
    text = dev.read_text(encoding='utf-8')
    if 'toolchain/acquire_bound.py' in text:
        return
    for old, new in ((HELPER_OLD, HELPER_NEW), (FIRST_OLD, FIRST_NEW), (RETRY_OLD, RETRY_NEW)):
        if text.count(old) != 1:
            raise ValueError('acquire bound boundary changed: ' + old.strip()[:50])
        text = text.replace(old, new, 1)
    dev.write_text(text, encoding='utf-8')
