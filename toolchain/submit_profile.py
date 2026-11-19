"""Per-submit composition of vkQueueSubmit2 (diagnostic only; no behaviour change).

Logs, when LARECOMP_VK_PROF=1, one line per submission: duration, whether the upload buffer was included, texture
copies, image-layout barriers, full texture uploads, main draws and passes. The 16.4 s entry event has 428 first-use
textures/111 MB; this separates the upload-buffer cost from the main draws.
"""
from pathlib import Path

GLOBALS_OLD = 'static uint64_t stat_draws, stat_skipped, stat_passes, stat_dispatches, stat_wait_ns, stat_flushes, stat_feedback_copies;\n'
GLOBALS_NEW = GLOBALS_OLD + '''uint64_t mk8_submit_copies, mk8_submit_layouts;  // MK8 (toolchain/submit_profile.py): per submission
uint64_t mk8_lock_ns, mk8_call_ns;                // MK8: queue-lock wait and vkQueueSubmit2 call, last submit
'''

SUBMIT_OLD = '''        PROF_ADD(prof_submit);
    }
'''
SUBMIT_NEW = '''        PROF_ADD(prof_submit);
    }
    if (prof) {  // MK8 (toolchain/submit_profile.py)
        static uint64_t last_draws, last_passes, last_full;
        extern uint64_t prof_tex_full;
        uint64_t full = prof_tex_full;
        double ms = (SDL_GetTicksNS() - st0) / 1e6;
        LOG("[submit] %.1f ms lock %.1f call %.1f upload %d copies %llu layouts %llu full %llu draws %llu passes %llu\\n",
            ms, mk8_lock_ns / 1e6, mk8_call_ns / 1e6,
            (int)s->upload_begun, (unsigned long long)mk8_submit_copies,
            (unsigned long long)mk8_submit_layouts, (unsigned long long)(full >= last_full ? full - last_full : 0),
            (unsigned long long)(stat_draws >= last_draws ? stat_draws - last_draws : 0),
            (unsigned long long)(stat_passes >= last_passes ? stat_passes - last_passes : 0));
        last_draws = stat_draws;
        last_passes = stat_passes;
        last_full = full;
        mk8_submit_copies = mk8_submit_layouts = 0;
    }
'''
SUBMIT_ST0_OLD = '''    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
'''
SUBMIT_ST0_NEW = '''    uint64_t st0 = SDL_GetTicksNS();  // MK8 (toolchain/submit_profile.py)
    {
        PROF_T0();
        dl_note("submit %llu", (unsigned long long)submit_seq + 1);
'''

TEX_COPY = '''    { extern uint64_t mk8_submit_copies; mk8_submit_copies++; }  // MK8 (toolchain/submit_profile.py)
'''
TEX_LAYOUT_OLD = '''void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
'''
TEX_LAYOUT_NEW = '''void vk_image_init_layout(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect) {
    extern uint64_t mk8_submit_layouts;
    mk8_submit_layouts++;  // MK8 (toolchain/submit_profile.py)
'''


SUBMIT_SPLIT_OLD = '''        vk_queue_lock();  // (shared with device.c's present thread)
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        vk_queue_unlock();
'''
SUBMIT_SPLIT_NEW = '''        uint64_t t_l0 = SDL_GetTicksNS();  // MK8 (toolchain/submit_profile.py): lock wait vs the submit call
        vk_queue_lock();  // (shared with device.c's present thread)
        mk8_lock_ns = SDL_GetTicksNS() - t_l0;
        VkResult sr = vkQueueSubmit2(g_vk.queue, 1, &si, VK_NULL_HANDLE);
        mk8_call_ns = SDL_GetTicksNS() - t_l0 - mk8_lock_ns;
        vk_queue_unlock();
'''

DEV_PRESENT_OLD = '''        vk_queue_lock();
        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
        vk_queue_unlock();
'''
DEV_PRESENT_NEW = '''        uint64_t t_p0 = SDL_GetTicksNS();  // MK8 (toolchain/submit_profile.py)
        vk_queue_lock();
        uint64_t t_p1 = SDL_GetTicksNS();
        VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
        uint64_t t_p2 = SDL_GetTicksNS();
        vk_queue_unlock();
        if (t_p2 - t_p0 > 5000000ull) LOG("[present] lock %.1f call %.1f\\n", (t_p1 - t_p0) / 1e6, (t_p2 - t_p1) / 1e6);
'''
DEV_ACQUIRE_OLD = '''    vk_present_flush();  // (the swapchain is the present thread's until its present is issued)
    vk_wait_seq(slot_seq[slot]);
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
'''
DEV_ACQUIRE_NEW = '''    uint64_t t_a0 = SDL_GetTicksNS();  // MK8 (toolchain/submit_profile.py)
    vk_present_flush();  // (the swapchain is the present thread's until its present is issued)
    uint64_t t_a1 = SDL_GetTicksNS();
    vk_wait_seq(slot_seq[slot]);
    uint64_t t_a2 = SDL_GetTicksNS();
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    uint64_t t_a3 = SDL_GetTicksNS();
    if (t_a3 - t_a0 > 5000000ull)
        LOG("[acquire] flush %.1f seq %.1f acq %.1f\\n", (t_a1 - t_a0) / 1e6, (t_a2 - t_a1) / 1e6, (t_a3 - t_a2) / 1e6);
'''


def unprepare(runtime: Path):
    """Undo every patch above (restore the prepared tree before host.py re-applies them in its own order)."""
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    for old, new in ((SUBMIT_NEW, SUBMIT_OLD), (SUBMIT_ST0_NEW, SUBMIT_ST0_OLD),
                     (SUBMIT_SPLIT_NEW, SUBMIT_SPLIT_OLD), (GLOBALS_NEW, GLOBALS_OLD)):
        if text.count(old) != 1:
            raise ValueError('cannot revert submit profile: ' + new.strip()[:50])
        text = text.replace(old, new, 1)
    ex.write_text(text, encoding='utf-8')
    tex = runtime/'nvn/vulkan/textures.c'
    text = tex.read_text(encoding='utf-8')
    text = text.replace(TEX_LAYOUT_NEW, TEX_LAYOUT_OLD, 1)
    for call in ('vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);',
                 'vkCmdCopyBufferToImage(cb, buf.buf, tab[k].d.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);'):
        text = text.replace(call + '\n' + TEX_COPY, call + '\n')
    tex.write_text(text, encoding='utf-8')
    img = runtime/'nvn/vulkan/imgui_glue.cpp'
    text = img.read_text(encoding='utf-8')
    call = 'vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);'
    img.write_text(text.replace(call + '\n' + TEX_COPY, call + '\n'), encoding='utf-8')
    dev = runtime/'nvn/vulkan/device.c'
    text = dev.read_text(encoding='utf-8')
    text = text.replace(DEV_ACQUIRE_NEW, DEV_ACQUIRE_OLD, 1)
    text = text.replace(DEV_PRESENT_NEW, DEV_PRESENT_OLD)
    dev.write_text(text, encoding='utf-8')


def prepare(runtime: Path):
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'mk8_lock_ns' not in text:
        if text.count(SUBMIT_SPLIT_OLD) != 1:
            raise ValueError('submit split boundary changed')
        text = text.replace(SUBMIT_SPLIT_OLD, SUBMIT_SPLIT_NEW, 1)
        ex.write_text(text, encoding='utf-8')
    dev = runtime/'nvn/vulkan/device.c'
    text = dev.read_text(encoding='utf-8')
    if 'MK8 (toolchain/submit_profile.py)' not in text:
        if text.count(DEV_PRESENT_OLD) != 2 or text.count(DEV_ACQUIRE_OLD) != 1:
            raise ValueError('present/acquire boundary changed')
        text = text.replace(DEV_PRESENT_OLD, DEV_PRESENT_NEW)
        text = text.replace(DEV_ACQUIRE_OLD, DEV_ACQUIRE_NEW, 1)
        dev.write_text(text, encoding='utf-8')
    ex = runtime/'nvn/vulkan/exec.c'
    text = ex.read_text(encoding='utf-8')
    if 'mk8_submit_copies' not in text:
        for old, new in ((GLOBALS_OLD, GLOBALS_NEW), (SUBMIT_ST0_OLD, SUBMIT_ST0_NEW), (SUBMIT_OLD, SUBMIT_NEW)):
            if text.count(old) != 1:
                raise ValueError('submit profile boundary changed: ' + old.strip()[:50])
            text = text.replace(old, new, 1)
        ex.write_text(text, encoding='utf-8')
    tex = runtime/'nvn/vulkan/textures.c'
    text = tex.read_text(encoding='utf-8')
    if 'mk8_submit_layouts' not in text:
        if text.count(TEX_LAYOUT_OLD) != 1:
            raise ValueError('layout boundary changed')
        text = text.replace(TEX_LAYOUT_OLD, TEX_LAYOUT_NEW, 1)
        # Count every upload copy in the upload path (the same lines as the audit) and the buffer copies.
        text = text.replace('vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);',
                            'vkCmdCopyBufferToImage(cb, s.buf, b->image, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);\n' + TEX_COPY)
        text = text.replace('vkCmdCopyBufferToImage(cb, buf.buf, tab[k].d.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);',
                            'vkCmdCopyBufferToImage(cb, buf.buf, tab[k].d.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);\n' + TEX_COPY)
        tex.write_text(text, encoding='utf-8')
    img = runtime/'nvn/vulkan/imgui_glue.cpp'
    text = img.read_text(encoding='utf-8')
    if 'mk8_submit_copies' not in text:
        text = text.replace('vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);',
                            'vkCmdCopyBufferToImage(cb, s.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &rg);\n' + TEX_COPY)
        img.write_text(text, encoding='utf-8')
